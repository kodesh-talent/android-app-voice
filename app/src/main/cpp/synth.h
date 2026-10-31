// Local PCM synthesis used by the offline (no-network) calibration path.
//
// The gateway probe requires a working API key; this module lets the app still
// exercise the full capture -> process -> playback pipeline and establish a
// device-local latency baseline for comparison.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vcore {

// Synthesises speech-like PCM16 mono audio for |text| at |sampleRate| Hz.
// The output is deterministic for a given text so repeated runs are comparable.
std::vector<int16_t> synthesizeSpeech(const std::string& text, int sampleRate);

// Converts mono PCM16 to WAV bytes (delegates to wav_codec).
std::string synthesizeWav(const std::string& text, int sampleRate);

// Linearly resamples mono PCM16 from one rate to another.
std::vector<int16_t> resamplePcm16(const std::vector<int16_t>& input, int fromRate,
                                   int toRate);

// Peak amplitude in the range [0, 1]; used for simple VAD decisions.
double peakAmplitude(const int16_t* samples, size_t count);

// Root-mean-square level in the range [0, 1].
double rmsLevel(const int16_t* samples, size_t count);

}  // namespace vcore
