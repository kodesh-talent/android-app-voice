#include "client_vad.h"

#include <algorithm>
#include <cmath>

namespace vcore {
namespace {

constexpr int kFrameMs = 20;

}  // namespace

ClientVad::ClientVad(int sampleRate, int minSpeechMs, int endSilenceMs, int preRollMs,
                     int maxSpeechMs)
    : sampleRate_(sampleRate > 0 ? sampleRate : 16000),
      minSpeechMs_(std::max(0, minSpeechMs)),
      endSilenceMs_(std::max(kFrameMs, endSilenceMs)),
      preRollMs_(std::max(0, preRollMs)),
      maxSpeechMs_(std::max(minSpeechMs_, maxSpeechMs)),
      frameBytes_(static_cast<size_t>(sampleRate_) * 2 * kFrameMs / 1000),
      preRollLimit_(static_cast<size_t>(sampleRate_) * 2 * preRollMs_ / 1000) {
    if (frameBytes_ < 2) {
        frameBytes_ = 2;
    }
}

double ClientVad::frameRms(const int16_t* samples, size_t sampleCount) const {
    if (samples == nullptr || sampleCount == 0) {
        return 0.0;
    }
    double sum = 0.0;
    for (size_t i = 0; i < sampleCount; ++i) {
        const double value = static_cast<double>(samples[i]) / 32768.0;
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(sampleCount));
}

void ClientVad::finishUtterance() {
    speechActive_ = false;
    voicedFrames_ = 0;
    speechMs_ = 0;
    silenceMs_ = 0;
    preRoll_.clear();
}

ClientVad::Result ClientVad::process(const char* pcm, size_t size) {
    Result result;
    if (pcm != nullptr && size > 0) {
        pending_.append(pcm, size);
    }

    while (pending_.size() >= frameBytes_ && !result.utteranceComplete) {
        const char* frame = pending_.data();
        const auto* samples = reinterpret_cast<const int16_t*>(frame);
        const size_t sampleCount = frameBytes_ / 2;
        const double rms = frameRms(samples, sampleCount);

        if (!speechActive_) {
            preRoll_.append(frame, frameBytes_);
            if (preRoll_.size() > preRollLimit_) {
                preRoll_.erase(0, preRoll_.size() - preRollLimit_);
            }

            // Adapt slowly to the room while keeping a useful floor for quiet
            // microphones. Speech has to clear the floor by a healthy margin.
            const double speechThreshold = std::max(0.010, noiseFloor_ * 2.8);
            if (rms >= speechThreshold) {
                ++voicedFrames_;
            } else {
                voicedFrames_ = 0;
                noiseFloor_ = noiseFloor_ * 0.95 + rms * 0.05;
            }

            if (voicedFrames_ >= 2) {
                speechActive_ = true;
                voicedFrames_ = 0;
                speechMs_ = kFrameMs;
                silenceMs_ = 0;
                result.speechStarted = true;
                result.audio.append(preRoll_);
                preRoll_.clear();
            }
        } else {
            result.audio.append(frame, frameBytes_);
            speechMs_ += kFrameMs;

            const double speechThreshold = std::max(0.010, noiseFloor_ * 2.8);
            const double silenceThreshold = std::max(0.006, speechThreshold * 0.60);
            if (rms < silenceThreshold) {
                silenceMs_ += kFrameMs;
            } else {
                silenceMs_ = 0;
            }

            const bool naturalEnd = speechMs_ >= minSpeechMs_ && silenceMs_ >= endSilenceMs_;
            const bool forcedEnd = speechMs_ >= maxSpeechMs_;
            if (naturalEnd || forcedEnd) {
                result.utteranceComplete = true;
                finishUtterance();
            }
        }

        pending_.erase(0, frameBytes_);
    }

    // Keep at most one incomplete PCM frame between socket reads.
    if (pending_.size() > frameBytes_) {
        pending_.erase(0, pending_.size() - frameBytes_);
    }
    return result;
}

void ClientVad::reset() {
    pending_.clear();
    preRoll_.clear();
    speechActive_ = false;
    voicedFrames_ = 0;
    speechMs_ = 0;
    silenceMs_ = 0;
}

}  // namespace vcore
