package com.xeno.voicelatency;

/**
 * Simulated probe used when the gateway cannot serve voice requests.
 *
 * <p>As of the last live check the test key authenticates fine (text chat works)
 * but the account's group has no voice routing: {@code /audio/speech} and
 * {@code /audio/transcriptions} answer 200 with the gateway's HTML console page,
 * and the realtime upgrade is rejected with 502. The simulated mode keeps the
 * full measurement pipeline exercisable: it runs the native synthesis path for
 * real (so audio generation, WAV framing and playback are genuine) and adds a
 * modelled network segment on top, producing the same {@link ProbeResult} shape
 * as a live probe.
 *
 * <p>The modelled numbers are intentionally conservative and stable enough to
 * compare against a real run once the account is funded.
 */
public final class ProbeSimulator {

    /** One-way modelling constants, in milliseconds. */
    private static final double TTS_HANDSHAKE_MS = 68.0;
    private static final double TTS_SERVER_MS = 185.0;
    private static final double STT_UPLOAD_MS = 240.0;
    private static final double STT_SERVER_MS = 520.0;
    private static final double RT_HANDSHAKE_MS = 96.0;
    private static final double RT_SERVER_MS = 340.0;

    private final ProbeSettings settings;

    public ProbeSimulator(ProbeSettings settings) {
        this.settings = settings;
    }

    /** Runs one simulated probe. */
    public ProbeResult run(ProbeMode mode) {
        switch (mode) {
            case TTS:
                return runTts();
            case STT:
                return runStt();
            case REALTIME:
                return runRealtime();
            case SIMULATED:
            default:
                return runLocal();
        }
    }

    private ProbeResult runTts() {
        // The synthesis itself is real: it exercises the native audio pipeline.
        long localStart = VoiceNative.nativeNowMicros();
        byte[] wav = VoiceNative.nativeSynthesizeWav(settings.text, AudioCapture.SAMPLE_RATE);
        double localMs = (VoiceNative.nativeNowMicros() - localStart) / 1000.0;

        double connect = jitter(TTS_HANDSHAKE_MS, 12);
        double head = connect + jitter(TTS_SERVER_MS, 35);
        double firstAudio = head + jitter(140, 40);
        double total = firstAudio + jitter(localMs, localMs * 0.15) + jitter(60, 15);
        long bytes = wav == null ? 0 : wav.length;
        double durationMs = AudioPlayback.durationMs((int) bytes, AudioCapture.SAMPLE_RATE, 1);

        playLocally(wav);
        return new ProbeResult(true, 200, "", "audio/wav", bytes, round(head),
                round(firstAudio), round(firstAudio), round(total), round(durationMs), "",
                "simulated", ProbeMode.TTS);
    }

    private ProbeResult runStt() {
        double head = jitter(72, 14);
        double upload = head + jitter(STT_UPLOAD_MS, 40);
        double transcript = upload + jitter(STT_SERVER_MS, 80);
        double total = transcript + jitter(45, 12);
        return new ProbeResult(true, 200, "", "application/json", 286, round(head),
                round(transcript), round(transcript), round(total), 0,
                "这是一段语音延迟测试的示例转写文本。", "simulated", ProbeMode.STT);
    }

    private ProbeResult runRealtime() {
        double handshake = jitter(RT_HANDSHAKE_MS, 16);
        double firstAudio = handshake + jitter(RT_SERVER_MS, 70);
        double total = firstAudio + jitter(180, 45);

        // Reuse the native synthesis so the returned audio is a real waveform.
        byte[] wav = VoiceNative.nativeSynthesizeWav(settings.text, AudioCapture.SAMPLE_RATE);
        long bytes = wav == null ? 0 : wav.length;
        playLocally(wav);

        return new ProbeResult(true, 101, "", "audio/wav", bytes, round(handshake),
                round(firstAudio), round(firstAudio), round(total),
                round(AudioPlayback.durationMs((int) bytes, AudioCapture.SAMPLE_RATE, 1)), "",
                "simulated", ProbeMode.REALTIME);
    }

    /**
     * Simulated link: synthesis is real (the native core generates the waveform),
     * the network segments are modelled. The report has the same shape as a live
     * TTS probe so the UI, the statistics and the history behave identically.
     */
    private ProbeResult runLocal() {
        long startUs = VoiceNative.nativeNowMicros();
        String report = VoiceNative.nativeRunLocalProbe(settings.toNativeConfigJson());
        ProbeResult local = ProbeResult.fromJson(report, ProbeMode.SIMULATED);
        double synthMs = (VoiceNative.nativeNowMicros() - startUs) / 1000.0;

        byte[] wav = VoiceNative.nativeSynthesizeWav(settings.text, AudioCapture.SAMPLE_RATE);
        playLocally(wav);
        long bytes = wav == null ? 0 : wav.length;

        double connect = jitter(TTS_HANDSHAKE_MS, 12);
        double head = connect + jitter(TTS_SERVER_MS, 35);
        double firstAudio = head + jitter(140, 40);
        double total = firstAudio + synthMs + jitter(60, 15);

        return new ProbeResult(local.ok, 200, local.ok ? "" : local.error, "audio/wav", bytes,
                round(head), round(firstAudio), round(firstAudio), round(total),
                round(AudioPlayback.durationMs((int) bytes, AudioCapture.SAMPLE_RATE, 1)),
                local.transcript, "simulated", ProbeMode.SIMULATED);
    }

    private void playLocally(byte[] wav) {
        if (wav == null || wav.length <= 44) {
            return;
        }
        String info = VoiceNative.nativeInspectWav(wav);
        int offset = Json.intValue(info, "dataOffset", 0);
        int bytes = Json.intValue(info, "dataBytes", 0);
        int rate = Json.intValue(info, "sampleRate", AudioCapture.SAMPLE_RATE);
        int channels = Json.intValue(info, "channels", 1);
        if (offset <= 0 || bytes <= 0 || offset + bytes > wav.length) {
            return;
        }
        byte[] pcm = new byte[bytes];
        System.arraycopy(wav, offset, pcm, 0, bytes);
        new AudioPlayback().play(pcm, rate, channels, null);
    }

    private static double jitter(double mean, double spread) {
        return Math.max(1.0, mean + (Math.random() * 2 - 1) * spread);
    }

    private static double round(double value) {
        return Math.round(value * 100.0) / 100.0;
    }
}
