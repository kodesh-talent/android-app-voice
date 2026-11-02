package com.xeno.voicelatency;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Runs latency probes and reports results.
 *
 * <p>Live probes delegate to the native core ({@link VoiceNative}), which owns the
 * HTTP/WebSocket protocol and every timing mark. The simulated mode reuses the
 * native synthesis path and adds modelled network segments, so the UI and the
 * statistics code have a single code path whether or not the gateway is funded.
 */
public final class ProbeEngine {

    private static final String TAG = "VoiceLatency";

    /** Callbacks, always delivered on the main thread. */
    public interface Listener {
        void onProbeStarted(ProbeMode mode, int iteration, int total);

        void onProbeFinished(ProbeResult result, int completed, int total);

        void onBatchFinished(ProbeMode mode, List<ProbeResult> results, boolean cancelled);

        void onStatus(String message);

        /** Live state line for an active realtime conversation. */
        default void onRealtimeState(String state) {
        }
    }

    private final Context context;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService executor = Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "probe-engine");
        thread.setDaemon(true);
        return thread;
    });
    /**
     * Socket close does a TLS write. It must not run on the UI thread, and it
     * cannot be queued on {@link #executor} because that thread is blocked in
     * the live probe.
     */
    private final ExecutorService closer = Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "probe-close");
        thread.setDaemon(true);
        return thread;
    });
    private final AtomicBoolean cancelled = new AtomicBoolean(false);
    private final AtomicBoolean running = new AtomicBoolean(false);
    private final AudioCapture capture = new AudioCapture();
    private final AudioPlayback playback = new AudioPlayback();
    private volatile RealtimeBridge realtimeBridge;
    private volatile TlsTransport realtimeTransport;

    public ProbeEngine(Context context) {
        this.context = context.getApplicationContext();
    }

    public boolean isRunning() {
        return running.get();
    }

    /**
     * Requests cancellation of the running batch. A live realtime session is
     * interrupted immediately instead of waiting for its bounded turn timeout.
     */
    public void cancel() {
        cancelled.set(true);
        RealtimeBridge bridge = realtimeBridge;
        if (bridge != null) {
            bridge.requestStop();
        }
        TlsTransport transport = realtimeTransport;
        if (transport != null) {
            closer.execute(transport::close);
        }
    }

    /** Runs {@code iterations} probes of {@code mode}. */
    public void runBatch(ProbeMode mode, ProbeSettings settings, int iterations,
                         Listener listener) {
        cancelled.set(false);
        running.set(true);
        executor.execute(() -> {
            List<ProbeResult> results = new ArrayList<>();
            try {
                for (int i = 0; i < iterations; i++) {
                    if (cancelled.get()) {
                        break;
                    }
                    final int iteration = i + 1;
                    post(() -> listener.onProbeStarted(mode, iteration, iterations));

                    // The mode alone decides the path: SIMULATED never touches the
                    // network, everything else always does.
                    ProbeResult result = mode.usesNetwork()
                            ? runLiveProbe(mode, settings, listener)
                            : new ProbeSimulator(settings).run(mode);

                    results.add(result);
                    final ProbeResult delivered = result;
                    final int completed = results.size();
                    post(() -> listener.onProbeFinished(delivered, completed, iterations));
                }
            } catch (RuntimeException e) {
                ProbeResult failure = ProbeEngine.exceptionResult(mode, e);
                results.add(failure);
                post(() -> listener.onProbeFinished(failure, results.size(), iterations));
            } finally {
                running.set(false);
                final boolean wasCancelled = cancelled.get();
                final List<ProbeResult> snapshot = new ArrayList<>(results);
                post(() -> listener.onBatchFinished(mode, snapshot, wasCancelled));
            }
        });
    }

    private ProbeResult runLiveProbe(ProbeMode mode, ProbeSettings settings, Listener listener) {
        switch (mode) {
            case STT:
                return runStt(settings, listener);
            case REALTIME:
                return runRealtime(settings, listener);
            case TTS:
            default:
                return runTts(settings, listener);
        }
    }

    private ProbeResult runTts(ProbeSettings settings, Listener listener) {
        TlsTransport transport = null;
        final long connectStart = System.nanoTime();
        try {
            final String host = hostOf(settings.baseUrl);
            post(() -> listener.onStatus("正在连接 " + host + " …"));
            transport = connect(host, portOf(settings.baseUrl), 15000, 60000);
            android.util.Log.d(TAG, "TLS connected in "
                    + (System.nanoTime() - connectStart) / 1_000_000 + " ms");

            final String report =
                    VoiceNative.nativeRunTtsProbe(transport, settings.toNativeConfigJson());
            ProbeResult result = ProbeResult.fromJson(report, ProbeMode.TTS);
            android.util.Log.d(TAG, "TTS head=" + result.headMs + " ttfb=" + result.ttfbMs
                    + " total=" + result.totalMs + " status=" + result.statusCode
                    + " type=" + result.contentType + " bytes=" + result.bodyBytes);

            byte[] audio = VoiceNative.nativeTakeTtsAudio();
            if (audio != null && audio.length > 0) {
                playIfPlayable(audio, result.contentType);
            }
            return result;
        } catch (Exception e) {
            return failure(ProbeMode.TTS, e);
        } finally {
            if (transport != null) {
                transport.close();
            }
        }
    }

    private ProbeResult runStt(ProbeSettings settings, Listener listener) {
        TlsTransport transport = null;
        try {
            if (!AudioCapture.hasPermission(context)) {
                return error(ProbeMode.STT, "缺少录音权限，请在设置中授权麦克风");
            }
            post(() -> listener.onStatus("正在录音 3 秒 …"));
            byte[] pcm = capture.record(3000);
            android.util.Log.d(TAG, "captured " + pcm.length + " bytes in 3 s window");
            if (pcm.length < 1600) {
                String reason = capture.lastError();
                return error(ProbeMode.STT, reason != null ? reason : "录音数据不足，请检查麦克风");
            }
            byte[] wav = VoiceNative.nativeEncodeWav(pcm, AudioCapture.SAMPLE_RATE, 1);

            final String host = hostOf(settings.baseUrl);
            post(() -> listener.onStatus("正在上传并转写 …"));
            final long connectStart = System.nanoTime();
            transport = connect(host, portOf(settings.baseUrl), 15000, 90000);
            android.util.Log.d(TAG, "STT TLS connected in "
                    + (System.nanoTime() - connectStart) / 1_000_000 + " ms");

            String report =
                    VoiceNative.nativeRunSttProbe(transport, settings.toNativeConfigJson(), wav);
            ProbeResult result = ProbeResult.fromJson(report, ProbeMode.STT);
            android.util.Log.d(TAG, "STT head=" + result.headMs + " ttfb=" + result.ttfbMs
                    + " total=" + result.totalMs + " status=" + result.statusCode
                    + " type=" + result.contentType + " preview=" + result.bodyPreview);
            return result;
        } catch (Exception e) {
            return failure(ProbeMode.STT, e);
        } finally {
            if (transport != null) {
                transport.close();
            }
        }
    }

    private ProbeResult runRealtime(ProbeSettings settings, Listener listener) {
        ProbeResult result = runRealtimeOnce(settings, listener, null);
        if (result.ok || (result.statusCode != 502 && result.statusCode != 503)) {
            return result;
        }
        final String alternate = ProbeSettings.alternateBaseUrl(settings.baseUrl);
        if (alternate == null) {
            return result;
        }
        final int status = result.statusCode;
        post(() -> listener.onStatus(
                "主地址返回 " + status + "，正在改用文档中的备用地址…"));
        return runRealtimeOnce(settings, listener, alternate);
    }

    private ProbeResult runRealtimeOnce(ProbeSettings settings, Listener listener,
                                        String baseUrlOverride) {
        TlsTransport transport = null;
        RealtimeBridge bridge = null;
        try {
            if (!AudioCapture.hasPermission(context)) {
                return error(ProbeMode.REALTIME, "缺少录音权限，请在设置中授权麦克风");
            }
            final String seed = settings.toNativeConfigJson(
                    baseUrlOverride == null ? null : ProbeSettings.realtimeUrlFor(baseUrlOverride),
                    AudioCapture.REALTIME_SAMPLE_RATE);
            String realtimeUrl = Json.string(
                    Json.parseObject(VoiceNative.nativeDescribeConfig(seed)), "realtimeUrl", "");
            if (realtimeUrl.isEmpty()) {
                return error(ProbeMode.REALTIME, "无法解析 Realtime 地址");
            }

            final String host = hostOf(realtimeUrl);
            post(() -> listener.onStatus("正在建立实时会话 …"));
            // 250 ms reads make cancellation responsive without flooding the socket.
            transport = connect(host, portOf(realtimeUrl), 15000, 250);
            realtimeTransport = transport;

            bridge = new RealtimeBridge(context,
                    state -> post(() -> listener.onRealtimeState(state)));
            realtimeBridge = bridge;
            if (!bridge.startCapture(125000)) {
                String reason = bridge.lastError();
                return error(ProbeMode.REALTIME,
                        reason == null || reason.isEmpty() ? "无法启动麦克风" : reason);
            }
            bridge.setSampleRate(bridge.sampleRate());
            final String config = settings.toNativeConfigJson(realtimeUrl, bridge.sampleRate());

            String report = VoiceNative.nativeRunRealtimeProbe(
                    transport, bridge, config, 120000L);
            ProbeResult result = ProbeResult.fromJson(report, ProbeMode.REALTIME);
            if (result.ok) {
                post(() -> listener.onStatus(cancelled.get()
                        ? "实时会话已结束"
                        : "实时会话完成"));
            }
            return result;
        } catch (Exception e) {
            if (cancelled.get()) {
                return error(ProbeMode.REALTIME, "测试已停止");
            }
            return failure(ProbeMode.REALTIME, e);
        } finally {
            realtimeBridge = null;
            realtimeTransport = null;
            if (bridge != null) {
                bridge.stopCapture();
            }
            if (transport != null) {
                transport.close();
            }
        }
    }

    /** Plays WAV/PCM directly; compressed payloads go through the platform decoder. */
    private void playIfPlayable(byte[] payload, String contentType) {
        final String type = contentType == null ? "" : contentType.toLowerCase(Locale.US);
        final boolean isWav = payload.length > 12 && payload[0] == 'R' && payload[1] == 'I'
                && payload[2] == 'F' && payload[3] == 'F';
        if (isWav) {
            String info = VoiceNative.nativeInspectWav(payload);
            int offset = Json.intValue(info, "dataOffset", 0);
            int bytes = Json.intValue(info, "dataBytes", 0);
            int rate = Json.intValue(info, "sampleRate", AudioCapture.SAMPLE_RATE);
            int channels = Json.intValue(info, "channels", 1);
            if (offset > 0 && bytes > 0 && offset + bytes <= payload.length) {
                byte[] pcm = new byte[bytes];
                System.arraycopy(payload, offset, pcm, 0, bytes);
                post(() -> playback.play(pcm, rate, channels, null));
            }
            return;
        }
        if (type.contains("pcm") || type.contains("octet-stream")) {
            post(() -> playback.play(payload, AudioCapture.SAMPLE_RATE, 1, null));
            return;
        }
        // The gateway answers audio/mpeg regardless of the requested format; the
        // platform decoder handles it, so the synthesized speech is audible.
        final boolean compressed = type.contains("mpeg") || type.contains("mp3")
                || type.contains("aac") || type.contains("ogg")
                || looksLikeMpeg(payload);
        if (compressed) {
            final java.io.File cacheDir = context.getCacheDir();
            post(() -> playback.playEncoded(payload, cacheDir, null));
        }
    }

    /** MPEG audio frame sync or ID3v2 tag at the head of the payload. */
    private static boolean looksLikeMpeg(byte[] payload) {
        if (payload.length < 4) {
            return false;
        }
        if (payload[0] == 'I' && payload[1] == 'D' && payload[2] == '3') {
            return true;
        }
        return (payload[0] & 0xFF) == 0xFF && (payload[1] & 0xE0) == 0xE0;
    }

    /** Opens the TLS transport, retrying once on a transient connect failure. */
    private static TlsTransport connect(String host, int port, int connectTimeoutMs,
                                        int readTimeoutMs) throws java.io.IOException {
        try {
            return TlsTransport.connect(host, port, connectTimeoutMs, readTimeoutMs);
        } catch (java.io.IOException first) {
            try {
                Thread.sleep(400);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw first;
            }
            return TlsTransport.connect(host, port, connectTimeoutMs, readTimeoutMs);
        }
    }

    private static ProbeResult failure(ProbeMode mode, Exception e) {
        final ProbeResult base = exceptionResult(mode, e);
        final String message = e.getMessage() == null
                ? e.getClass().getSimpleName()
                : e.getMessage();
        final String lowered = message.toLowerCase(Locale.US);
        String reason;
        if (lowered.contains("failed to connect") || lowered.contains("connect timed out")
                || lowered.contains("timeout")) {
            reason = "无法连接网关：" + message;
        } else if (lowered.contains("handshake") || lowered.contains("certificate")) {
            reason = "TLS 握手失败：" + message;
        } else if (lowered.contains("unable to resolve host")
                || lowered.contains("unknownhost")) {
            reason = "无法解析域名，请检查网络：" + message;
        } else {
            reason = "网络错误：" + message;
        }
        return new ProbeResult(false, 0, reason, "", 0, 0, 0, 0, 0, 0, 0, "", "",
                "", base.exceptionType, base.stackTrace, mode);
    }

    private static ProbeResult error(ProbeMode mode, String message) {
        return new ProbeResult(false, 0, message, "", 0, 0, 0, 0, 0, 0, "", "", mode);
    }

    /** Converts an unexpected runtime failure into a selectable diagnostic result. */
    static ProbeResult exceptionResult(ProbeMode mode, Throwable e) {
        final java.io.StringWriter writer = new java.io.StringWriter();
        e.printStackTrace(new java.io.PrintWriter(writer));
        final String message = e.getMessage() == null ? "" : e.getMessage();
        return new ProbeResult(false, 0,
                e.getClass().getName() + (message.isEmpty() ? "" : ": " + message),
                "", 0, 0, 0, 0, 0, 0, 0, "", "", "", e.getClass().getName(),
                writer.toString(), mode);
    }

    /** Host part of a URL, resolved by the native parser. */
    private static String hostOf(String url) {
        if (url == null || url.isEmpty()) {
            return "";
        }
        String host = Json.string(Json.parseObject(VoiceNative.nativeParseUrl(url)), "host", "");
        if (!host.isEmpty()) {
            return host;
        }
        String rest = url.contains("://") ? url.substring(url.indexOf("://") + 3) : url;
        int slash = rest.indexOf('/');
        String authority = slash >= 0 ? rest.substring(0, slash) : rest;
        int colon = authority.lastIndexOf(':');
        return colon > 0 ? authority.substring(0, colon) : authority;
    }

    /** Port part of a URL, resolved by the native parser. */
    private static int portOf(String url) {
        if (url == null || url.isEmpty()) {
            return 443;
        }
        int port = (int) Json.number(Json.parseObject(VoiceNative.nativeParseUrl(url)), "port", 0);
        if (port > 0) {
            return port;
        }
        return url.startsWith("https") || url.startsWith("wss") ? 443 : 80;
    }

    private void post(Runnable action) {
        mainHandler.post(action);
    }

    public void shutdown() {
        cancel();
        playback.stop();
        capture.release();
        executor.shutdownNow();
        closer.shutdown();
    }
}
