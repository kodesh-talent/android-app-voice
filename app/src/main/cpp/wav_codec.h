// PCM16 mono WAV encoding/decoding used by the latency probes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vcore {

struct WavInfo {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    size_t dataOffset = 0;
    size_t dataBytes = 0;

    bool valid() const { return sampleRate > 0 && channels > 0 && dataBytes > 0; }
};

// Wraps raw little-endian PCM16 samples in a RIFF/WAVE container.
std::string encodePcm16Wav(const uint8_t* pcm, size_t pcmBytes, int sampleRate,
                           int channels);

// Parses the RIFF header. Only PCM (format 1) and float (format 3) are described;
// |outPcm16| is produced only for PCM16 input.
bool parseWav(const std::string& bytes, WavInfo& info, std::string& error);

// Converts 16-bit mono PCM to normalised float samples.
std::vector<float> pcm16ToFloat(const uint8_t* pcm, size_t pcmBytes);

}  // namespace vcore
