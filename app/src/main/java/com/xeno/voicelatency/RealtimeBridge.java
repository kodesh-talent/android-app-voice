package com.xeno.voicelatency;

import android.content.Context;
import android.media.AudioManager;

import java.util.function.Consumer;

/**
 * Realtime session bridge invoked from native code.
 *
 * <p>Method names and signatures are fixed by {@code jni_bridge.cpp}
 * ({@code RealtimeListener}). Native code polls {@link #readMicAudio()} for the
 * next captured frame, pushes decoded audio into {@link #onAudio(byte[])}, and
 * forwards gateway events to {@link #onEvent(String, String)}.
 */
public final class RealtimeBridge {

    private final AudioCapture capture = new AudioCapture();
    private final AudioPlayback playback = new AudioPlayback();
    private final AudioPlayback.PcmAccumulator received = new AudioPlayback.PcmAccumulator();
    private final AudioManager audioManager;
    private final Consumer<String> statusSink;

    private volatile boolean stopRequested;
    private volatile boolean micStarted;
    private volatile boolean micPaused;
    private volatile String lastEvent = "";
    private volatile String lastError = "";
    private volatile String lastStatus = "";
    private volatile int sampleRate = AudioCapture.SAMPLE_RATE;
    private volatile boolean playbackStarted;
    private boolean communicationModeActive;
    private int previousAudioMode = AudioManager.MODE_NORMAL;
    private boolean previousSpeakerphone;

    RealtimeBridge(Context context, Consumer<String> statusSink) {
        this.audioManager = context == null
                ? null
                : (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        this.statusSink = statusSink;
    }

    boolean startCapture(int maxDurationMs) {
        stopRequested = false;
        micPaused = false;
        communicationModeActive = false;
        capture.release();
        micStarted = capture.startStream(maxDurationMs, AudioCapture.REALTIME_SAMPLE_RATE);
        sampleRate = capture.activeSampleRate();
        if (!micStarted) {
            String error = capture.lastError();
            onError(error == null || error.isEmpty() ? "无法启动麦克风" : error);
            return false;
        }
        enterCommunicationMode();
        notifyState("麦克风已启动，正在连接会话…");
        return true;
    }

    /** Non-blocking cancellation flag for the UI thread. */
    void requestStop() {
        stopRequested = true;
        micPaused = true;
    }

    void stopCapture() {
        stopRequested = true;
        micPaused = true;
        capture.release();
        playback.stop();
        playbackStarted = false;
        leaveCommunicationMode();
    }

    /** Called by native code: returns the next PCM frame, or null when paused. */
    public byte[] readMicAudio() {
        if (stopRequested || micPaused || !micStarted) {
            return null;
        }
        if (!capture.isRecording()) {
            return null;
        }
        byte[] frame = capture.drainChunk();
        if (frame == null) {
            try {
                Thread.sleep(10);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return null;
            }
        }
        return frame;
    }

    /**
     * Called by native code with decoded PCM16 audio from the session. Audio is
     * written to the speaker as it arrives and also retained for diagnostics.
     */
    public void onAudio(byte[] pcm) {
        if (pcm == null || pcm.length < 2 || stopRequested) {
            return;
        }
        received.append(pcm);
        synchronized (this) {
            if (!playbackStarted) {
                playbackStarted = playback.startStream(sampleRate, 1);
            }
            if (playbackStarted) {
                playback.writeStream(pcm);
            }
        }
    }

    /** Called by native code for every server or local session event. */
    public void onEvent(String type, String payload) {
        lastEvent = type == null ? "" : type;
        switch (lastEvent) {
            case "session.created":
                notifyState("实时会话已建立");
                break;
            case "session.updated":
                notifyState("会话已就绪，请开始说话");
                break;
            case "local.speech_started":
            case "input_audio_buffer.speech_started":
                playback.stop();
                playbackStarted = false;
                notifyState("检测到说话…");
                break;
            case "input_audio_buffer.speech_stopped":
                notifyState("说话结束，等待回复…");
                break;
            case "local.response_requested":
                notifyState("说话结束，等待回复…");
                break;
            case "response.created":
                notifyState("助手正在回复…");
                break;
            case "response.output_audio.delta":
            case "response.audio.delta":
                notifyState("正在播放回复…");
                break;
            case "response.done":
                notifyState("继续聆听…");
                break;
            case "error":
                notifyState("实时会话错误");
                break;
            case "local.listening":
                notifyState("继续聆听…");
                break;
            default:
                break;
        }
    }

    /** Called by native code when the session reports an error. */
    public void onError(String message) {
        lastError = message == null ? "" : message;
        lastEvent = "error";
        notifyState(lastError.isEmpty() ? "实时会话错误" : lastError);
    }

    /** Native reports the negotiated PCM output rate before the session starts. */
    public void setSampleRate(int rate) {
        if (rate > 0) {
            sampleRate = rate;
        }
    }

    int sampleRate() {
        return sampleRate;
    }

    /** Native calls this after commit; stop uploading the current turn. */
    public void onCaptureDone() {
        micPaused = true;
        capture.clearBuffered();
    }

    /** Native calls this after response.done; resume the next turn. */
    public void onResponseDone() {
        if (stopRequested) {
            return;
        }
        capture.clearBuffered();
        micPaused = false;
    }

    /** Called by native code to check whether the user cancelled the probe. */
    public boolean shouldStop() {
        return stopRequested;
    }

    byte[] collectedAudio() {
        return received.toByteArray();
    }

    String lastEventType() {
        return lastEvent;
    }

    String lastError() {
        return lastError;
    }

    String lastStatus() {
        return lastStatus;
    }

    private void notifyState(String message) {
        if (message == null || message.isEmpty()) {
            return;
        }
        if (message.equals(lastStatus)) {
            return;
        }
        lastStatus = message;
        Consumer<String> sink = statusSink;
        if (sink != null) {
            sink.accept(message);
        }
    }

    private void enterCommunicationMode() {
        if (audioManager == null || communicationModeActive) {
            return;
        }
        try {
            previousAudioMode = audioManager.getMode();
            previousSpeakerphone = audioManager.isSpeakerphoneOn();
            audioManager.setMode(AudioManager.MODE_IN_COMMUNICATION);
            audioManager.setSpeakerphoneOn(true);
            communicationModeActive = true;
        } catch (RuntimeException ignored) {
            communicationModeActive = false;
        }
    }

    private void leaveCommunicationMode() {
        if (audioManager == null || !communicationModeActive) {
            return;
        }
        try {
            audioManager.setSpeakerphoneOn(previousSpeakerphone);
            audioManager.setMode(previousAudioMode);
        } catch (RuntimeException ignored) {
            // The audio service may already have restored the route.
        }
        communicationModeActive = false;
    }
}
