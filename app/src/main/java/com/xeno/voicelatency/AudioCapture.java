package com.xeno.voicelatency;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.Build;

import androidx.core.content.ContextCompat;

import java.io.ByteArrayOutputStream;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Microphone capture as 16-bit mono PCM at 16 kHz.
 *
 * <p>Two consumption styles share one capture engine:
 * <ul>
 *   <li>{@link #record(int)} buffers everything and returns it once (STT probe).</li>
 *   <li>{@link #startStream()} plus {@link #drainChunk()} exposes frames as they
 *       arrive (Realtime probe).</li>
 * </ul>
 * Capture always ends after a bounded window so a probe cannot hang on silence.
 */
public final class AudioCapture {

    public static final int SAMPLE_RATE = 16000;
    /** 20 ms of 16 kHz PCM16. */
    public static final int FRAME_BYTES = 640;
    private static final int MAX_QUEUED_FRAMES = 400;

    private final AtomicBoolean recording = new AtomicBoolean(false);
    private final AtomicInteger levelMilli = new AtomicInteger(0);
    private final ConcurrentLinkedQueue<byte[]> queue = new ConcurrentLinkedQueue<>();

    private volatile AudioRecord record;
    private volatile Thread worker;
    private volatile ByteArrayOutputStream sink;
    private volatile String error;

    /** True when RECORD_AUDIO is already granted. */
    public static boolean hasPermission(Context context) {
        return ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED;
    }

    public static String permission() {
        return Manifest.permission.RECORD_AUDIO;
    }

    /** Records {@code durationMs} and returns the raw PCM16 payload. */
    public byte[] record(int durationMs) {
        queue.clear();
        if (!startInternal(durationMs, true)) {
            return new byte[0];
        }
        joinWorker(4000);
        ByteArrayOutputStream buffer = sink;
        sink = null;
        return buffer != null ? buffer.toByteArray() : new byte[0];
    }

    /** Starts streaming capture; frames are taken with {@link #drainChunk()}. */
    public boolean startStream() {
        queue.clear();
        return startInternal(15000, false);
    }

    /**
     * Starts streaming capture for a bounded realtime turn. The timeout is only a
     * safety bound; the session stops capture explicitly after it is committed.
     */
    public boolean startStream(int maxDurationMs) {
        queue.clear();
        return startInternal(Math.max(1000, maxDurationMs), false);
    }

    /** Returns the next captured frame, or null when none is buffered yet. */
    public byte[] drainChunk() {
        return queue.poll();
    }

    /** Drops frames captured while the realtime turn was paused. */
    public void clearBuffered() {
        queue.clear();
    }

    /** Latest peak level in the range 0..1, for the UI level meter. */
    public double level() {
        return levelMilli.get() / 1000.0;
    }

    private boolean startInternal(int maxDurationMs, boolean bufferAll) {
        if (recording.get()) {
            return true;
        }
        error = null;

        final int minBuffer = AudioRecord.getMinBufferSize(
                SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuffer <= 0) {
            error = "设备不支持 16 kHz 单声道 PCM 采集";
            return false;
        }
        final int bufferSize = Math.max(minBuffer * 2, FRAME_BYTES * 4);

        AudioRecord created;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                created = new AudioRecord.Builder()
                        .setAudioSource(MediaRecorder.AudioSource.VOICE_RECOGNITION)
                        .setAudioFormat(new AudioFormat.Builder()
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setSampleRate(SAMPLE_RATE)
                                .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                                .build())
                        .setBufferSizeInBytes(bufferSize)
                        .build();
            } else {
                created = new AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION,
                        SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                        AudioFormat.ENCODING_PCM_16BIT, bufferSize);
            }
        } catch (IllegalArgumentException | SecurityException | UnsupportedOperationException e) {
            error = "无法打开麦克风：" + e.getMessage();
            return false;
        }

        if (created.getState() != AudioRecord.STATE_INITIALIZED) {
            error = "麦克风不可用";
            created.release();
            return false;
        }

        record = created;
        sink = bufferAll
                ? new ByteArrayOutputStream(SAMPLE_RATE * 2 * Math.max(1, maxDurationMs / 1000))
                : null;
        recording.set(true);

        final long deadline = System.currentTimeMillis() + Math.max(250, maxDurationMs);
        Thread thread = new Thread(() -> {
            byte[] chunk = new byte[Math.max(FRAME_BYTES, bufferSize)];
            try {
                created.startRecording();
                while (recording.get() && System.currentTimeMillis() < deadline) {
                    int read = created.read(chunk, 0, chunk.length);
                    if (read > 0) {
                        ByteArrayOutputStream target = sink;
                        if (target != null) {
                            target.write(chunk, 0, read);
                        }
                        if (queue.size() < MAX_QUEUED_FRAMES) {
                            byte[] frame = new byte[read];
                            System.arraycopy(chunk, 0, frame, 0, read);
                            queue.offer(frame);
                        }
                        updateLevel(chunk, read);
                    } else if (read < 0) {
                        error = "麦克风读取失败（" + read + "）";
                        break;
                    }
                }
            } catch (IllegalStateException e) {
                error = "麦克风状态异常：" + e.getMessage();
            } finally {
                try {
                    created.stop();
                } catch (IllegalStateException ignored) {
                    // Already stopped.
                }
                created.release();
                record = null;
                recording.set(false);
            }
        }, "audio-capture");
        thread.setDaemon(true);
        worker = thread;
        thread.start();
        return true;
    }

    private void updateLevel(byte[] pcm, int length) {
        int peak = 0;
        for (int i = 0; i + 1 < length; i += 2) {
            final int sample = (short) ((pcm[i] & 0xFF) | (pcm[i + 1] << 8));
            final int magnitude = Math.abs(sample);
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        levelMilli.set((int) (peak / 32768.0 * 1000.0));
    }

    /** Requests capture to stop and waits briefly for the worker. */
    public void stop() {
        recording.set(false);
        joinWorker(2000);
    }

    public boolean isRecording() {
        return recording.get();
    }

    public String lastError() {
        return error;
    }

    /** Stops capture and drops buffered audio. */
    public void release() {
        stop();
        queue.clear();
        sink = null;
        AudioRecord active = record;
        if (active != null) {
            try {
                active.release();
            } catch (IllegalStateException ignored) {
                // Already released.
            }
            record = null;
        }
    }

    private void joinWorker(long timeoutMs) {
        Thread thread = worker;
        worker = null;
        if (thread == null) {
            return;
        }
        try {
            thread.join(timeoutMs);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
