package com.xeno.voicelatency;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.MediaPlayer;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * PCM playback for probe audio.
 *
 * <p>{@code onFirstSample} fires immediately before {@link AudioTrack#play()},
 * which is the point the UI attributes to audible output, so the reported
 * playback latency excludes only the hardware buffer itself.
 */
public final class AudioPlayback {

    private AudioTrack track;
    private MediaPlayer player;
    private File spoolFile;
    private volatile boolean playing;

    private AudioTrack streamTrack;
    private int streamSampleRate;
    private int streamChannels;

    /**
     * Plays a compressed payload (MP3 and anything else the platform decodes).
     *
     * <p>{@link AudioTrack} only accepts PCM, so compressed replies are spooled to
     * the cache directory and handed to {@link MediaPlayer}. {@code onFirstSample}
     * fires once preparation completes, just before playback starts.
     *
     * @return true when playback started
     */
    public boolean playEncoded(byte[] payload, File cacheDir, Runnable onFirstSample) {
        stop();
        if (payload == null || payload.length < 4 || cacheDir == null) {
            return false;
        }
        File file = new File(cacheDir, "probe-audio-" + System.nanoTime() + ".bin");
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(payload);
        } catch (IOException e) {
            file.delete();
            return false;
        }
        spoolFile = file;

        MediaPlayer created = new MediaPlayer();
        try {
            created.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build());
            created.setDataSource(file.getAbsolutePath());
            created.setOnCompletionListener(mp -> stop());
            created.setOnErrorListener((mp, what, extra) -> {
                stop();
                return true;
            });
            created.prepare();
            created.start();
        } catch (IOException | IllegalStateException | IllegalArgumentException e) {
            created.release();
            stop();
            return false;
        }
        player = created;
        playing = true;
        if (onFirstSample != null) {
            onFirstSample.run();
        }
        return true;
    }

    /**
     * Plays 16-bit PCM.
     *
     * @return true when playback started
     */
    public boolean play(byte[] pcm, int sampleRate, int channels, Runnable onFirstSample) {
        stop();
        if (pcm == null || pcm.length < 2) {
            return false;
        }
        final int channelMask = channels > 1
                ? AudioFormat.CHANNEL_OUT_STEREO
                : AudioFormat.CHANNEL_OUT_MONO;

        final int minBuffer = AudioTrack.getMinBufferSize(
                sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT);
        final int bufferSize = Math.max(Math.max(minBuffer, 4096), pcm.length);

        AudioTrack created;
        try {
            created = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                            .build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(sampleRate)
                            .setChannelMask(channelMask)
                            .build())
                    .setBufferSizeInBytes(bufferSize)
                    .setTransferMode(AudioTrack.MODE_STATIC)
                    .build();
        } catch (IllegalArgumentException | IllegalStateException e) {
            return false;
        }

        if (created.getState() != AudioTrack.STATE_INITIALIZED) {
            created.release();
            return false;
        }

        int written = created.write(pcm, 0, pcm.length);
        if (written <= 0) {
            created.release();
            return false;
        }

        track = created;
        if (onFirstSample != null) {
            onFirstSample.run();
        }
        playing = true;
        created.play();

        // Release once the buffer has drained; MODE_STATIC keeps memory until then.
        final long durationMs = durationMs(pcm.length, sampleRate, channels) + 500L;
        Thread releaser = new Thread(() -> {
            try {
                Thread.sleep(durationMs);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (track == created) {
                stop();
            }
        }, "audio-playback-release");
        releaser.setDaemon(true);
        releaser.start();
        return true;
    }

    /**
     * Starts a low-latency streaming PCM16 track for realtime audio deltas.
     * Chunks are queued in order; the caller must invoke {@link #stop()} when
     * the response ends.
     *
     * @return true when the track is ready for writes
     */
    public boolean startStream(int sampleRate, int channels) {
        stop();
        if (sampleRate <= 0 || channels <= 0) {
            return false;
        }
        final int channelMask = channels > 1
                ? AudioFormat.CHANNEL_OUT_STEREO
                : AudioFormat.CHANNEL_OUT_MONO;
        final int minBuffer = AudioTrack.getMinBufferSize(
                sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuffer <= 0) {
            return false;
        }
        final int bufferSize = Math.max(minBuffer * 2, 8192);
        AudioTrack created;
        try {
            created = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                            .build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(sampleRate)
                            .setChannelMask(channelMask)
                            .build())
                    .setBufferSizeInBytes(bufferSize)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build();
        } catch (IllegalArgumentException | IllegalStateException e) {
            return false;
        }
        if (created.getState() != AudioTrack.STATE_INITIALIZED) {
            created.release();
            return false;
        }
        streamTrack = created;
        streamSampleRate = sampleRate;
        streamChannels = channels;
        created.play();
        playing = true;
        return true;
    }

    /** Writes one PCM16 chunk to the active realtime stream. */
    public synchronized boolean writeStream(byte[] pcm) {
        final AudioTrack active = streamTrack;
        if (active == null || pcm == null || pcm.length < 2) {
            return false;
        }
        int offset = 0;
        while (offset < pcm.length) {
            final int written = active.write(pcm, offset, pcm.length - offset);
            if (written <= 0) {
                return false;
            }
            offset += written;
        }
        return true;
    }

    /** Stops and releases the current track. */
    public synchronized void stop() {
        playing = false;

        AudioTrack activeStream = streamTrack;
        streamTrack = null;
        if (activeStream != null) {
            try {
                activeStream.stop();
            } catch (IllegalStateException ignored) {
                // Not started.
            }
            activeStream.release();
        }

        MediaPlayer activePlayer = player;
        player = null;
        if (activePlayer != null) {
            try {
                activePlayer.stop();
            } catch (IllegalStateException ignored) {
                // Not started.
            }
            activePlayer.release();
        }
        File spool = spoolFile;
        spoolFile = null;
        if (spool != null) {
            spool.delete();
        }

        AudioTrack active = track;
        track = null;
        if (active == null) {
            return;
        }
        try {
            active.stop();
        } catch (IllegalStateException ignored) {
            // Not started.
        }
        try {
            active.release();
        } catch (IllegalStateException ignored) {
            // Already released.
        }
    }

    public boolean isPlaying() {
        return playing;
    }

    /** Approximate playback duration of a PCM16 payload. */
    public static long durationMs(int byteCount, int sampleRate, int channels) {
        if (sampleRate <= 0 || channels <= 0) {
            return 0L;
        }
        final double frames = byteCount / 2.0 / channels;
        return (long) (frames / sampleRate * 1000.0);
    }

    /** Concatenates realtime PCM chunks. */
    public static final class PcmAccumulator {
        private final ByteArrayOutputStream buffer = new ByteArrayOutputStream();

        public synchronized void append(byte[] chunk) {
            if (chunk != null && chunk.length > 0) {
                buffer.write(chunk, 0, chunk.length);
            }
        }

        public synchronized byte[] toByteArray() {
            return buffer.toByteArray();
        }

        public synchronized int size() {
            return buffer.size();
        }
    }
}
