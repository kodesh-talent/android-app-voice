#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vcore {

// Lightweight client-side voice activity detector for the manual-commit
// Realtime flow. It keeps a short pre-roll, emits speech only while an
// utterance is active, and reports when trailing silence makes the turn ready
// for input_audio_buffer.commit.
class ClientVad {
public:
    struct Result {
        bool speechStarted = false;
        bool utteranceComplete = false;
        // PCM16LE mono audio that should be appended to the input buffer.
        std::string audio;
    };

    explicit ClientVad(int sampleRate = 16000, int minSpeechMs = 250,
                       int endSilenceMs = 650, int preRollMs = 300,
                       int maxSpeechMs = 15000);

    Result process(const char* pcm, size_t size);

    // Drops pending samples, pre-roll and the current utterance state.
    void reset();

    bool speechActive() const { return speechActive_; }

private:
    void finishUtterance();
    double frameRms(const int16_t* samples, size_t sampleCount) const;

    int sampleRate_;
    int minSpeechMs_;
    int endSilenceMs_;
    int preRollMs_;
    int maxSpeechMs_;
    size_t frameBytes_;
    size_t preRollLimit_;

    std::string pending_;
    std::string preRoll_;
    bool speechActive_ = false;
    int voicedFrames_ = 0;
    int64_t speechMs_ = 0;
    int64_t silenceMs_ = 0;
    double noiseFloor_ = 0.0015;
};

}  // namespace vcore
