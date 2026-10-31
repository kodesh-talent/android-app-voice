package com.xeno.voicelatency;

/**
 * Java surface of the native voice core.
 *
 * <p>Every method maps 1:1 onto a JNI entry point in {@code jni_bridge.cpp}. The
 * native layer owns request shaping, protocol parsing, audio decoding and
 * latency measurement; Java only supplies platform services (TLS socket,
 * microphone, speaker) and the Material 3 UI.
 */
public final class VoiceNative {

    static {
        System.loadLibrary("voicecore");
    }

    private VoiceNative() {
    }

    /** Native library build identifier, e.g. {@code voicecore/1.1.0}. */
    public static native String nativeVersion();

    /** Monotonic clock in microseconds, shared with the native timestamps. */
    public static native long nativeNowMicros();

    // ---------------------------------------------------------- configuration

    /** Resolves the effective configuration (URLs, models, derived realtime URL). */
    public static native String nativeDescribeConfig(String configJson);

    /** Parses a URL into scheme/host/port/path; returns JSON. */
    public static native String nativeParseUrl(String url);

    // ----------------------------------------------------------------- probes

    /** Runs a TTS probe over {@code transport}; returns a JSON report. */
    public static native String nativeRunTtsProbe(Object transport, String configJson);

    /** Hands over the audio payload captured by the last successful TTS probe. */
    public static native byte[] nativeTakeTtsAudio();

    /** Runs an STT probe over {@code transport} using {@code wav} bytes. */
    public static native String nativeRunSttProbe(Object transport, String configJson, byte[] wav);

    /** Runs a Realtime (WebSocket) probe driven by {@code listener}. */
    public static native String nativeRunRealtimeProbe(
            Object transport, Object listener, String configJson, long maxDurationMs);

    /** Runs the offline synthesis probe (no network dependency). */
    public static native String nativeRunLocalProbe(String configJson);

    // --------------------------------------------------------------- audio io

    public static native byte[] nativeSynthesizeWav(String text, int sampleRate);

    public static native byte[] nativeEncodeWav(byte[] pcm, int sampleRate, int channels);

    /** Describes a WAV payload (rate, channels, data offset); returns JSON. */
    public static native String nativeInspectWav(byte[] wav);

    public static native double nativePeakAmplitude(byte[] pcm);

    public static native double nativeRmsLevel(byte[] pcm);

    public static native String nativeBase64Encode(byte[] data);

    public static native byte[] nativeBase64Decode(String text);

    // -------------------------------------------------------------- statistics

    /** Computes count/avg/min/max/p50/p90/p95/stddev/jitter over millisecond samples. */
    public static native String nativeComputeStats(double[] samplesMs);

    // ------------------------------------------------------- websocket frames

    public static native String nativeGenerateWebSocketKey();

    public static native String nativeBuildHandshakeRequest(
            String host, String path, String apiKey, String origin, String key);

    /** Returns an empty string on success, otherwise the failure reason. */
    public static native String nativeValidateHandshake(String response, String key);

    public static native byte[] nativeBuildTextFrame(String payload);

    public static native byte[] nativeBuildCloseFrame();
}
