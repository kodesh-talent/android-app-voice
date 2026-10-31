package com.xeno.voicelatency;

/** Probe modes offered by the main test screen. */
public enum ProbeMode {
    TTS("tts", 0, "TTS 合成", "文本 → 音频，测首音延迟"),
    STT("stt", 1, "STT 识别", "录音 → 文本，测转写延迟"),
    REALTIME("realtime", 2, "实时语音", "连续对话：停顿自动提交，点击停止结束"),
    SIMULATED("simulated", 3, "模拟链路", "本地合成 + 仿真网关延迟");

    public final String id;
    public final int nativeValue;
    public final String title;
    public final String subtitle;

    ProbeMode(String id, int nativeValue, String title, String subtitle) {
        this.id = id;
        this.nativeValue = nativeValue;
        this.title = title;
        this.subtitle = subtitle;
    }

    public static ProbeMode fromId(String id) {
        for (ProbeMode mode : values()) {
            if (mode.id.equalsIgnoreCase(id)) {
                return mode;
            }
        }
        return TTS;
    }

    /** True when the mode contacts the gateway over the network. */
    public boolean usesNetwork() {
        return this != SIMULATED;
    }

    /** True when the mode needs microphone access. */
    public boolean needsMicrophone() {
        return this == STT || this == REALTIME;
    }
}
