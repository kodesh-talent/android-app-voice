#include "wav_codec.h"

#include <cstring>

namespace vcore {
namespace {

void writeLe32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void writeLe16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

uint32_t readLe32(const std::string& bytes, size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + 3])) << 24);
}

uint16_t readLe16(const std::string& bytes, size_t offset) {
    if (offset + 2 > bytes.size()) {
        return 0;
    }
    return static_cast<uint16_t>(static_cast<uint8_t>(bytes[offset]) |
                                 (static_cast<uint8_t>(bytes[offset + 1]) << 8));
}

}  // namespace

std::string encodePcm16Wav(const uint8_t* pcm, size_t pcmBytes, int sampleRate,
                           int channels) {
    const int safeRate = sampleRate > 0 ? sampleRate : 16000;
    const int safeChannels = channels > 0 ? channels : 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = static_cast<uint32_t>(safeRate * safeChannels * 2);
    const uint16_t blockAlign = static_cast<uint16_t>(safeChannels * 2);
    const uint32_t dataSize = static_cast<uint32_t>(pcmBytes);

    std::string out;
    out.reserve(44 + pcmBytes);
    out.append("RIFF", 4);
    writeLe32(out, 36 + dataSize);
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    writeLe32(out, 16);
    writeLe16(out, 1);  // PCM
    writeLe16(out, static_cast<uint16_t>(safeChannels));
    writeLe32(out, static_cast<uint32_t>(safeRate));
    writeLe32(out, byteRate);
    writeLe16(out, blockAlign);
    writeLe16(out, bitsPerSample);
    out.append("data", 4);
    writeLe32(out, dataSize);
    if (pcm != nullptr && pcmBytes > 0) {
        out.append(reinterpret_cast<const char*>(pcm), pcmBytes);
    }
    return out;
}

bool parseWav(const std::string& bytes, WavInfo& info, std::string& error) {
    info = WavInfo();
    if (bytes.size() < 12) {
        error = "file too small for RIFF header";
        return false;
    }
    if (bytes.compare(0, 4, "RIFF") != 0 || bytes.compare(8, 4, "WAVE") != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    size_t offset = 12;
    bool sawFormat = false;
    while (offset + 8 <= bytes.size()) {
        const std::string chunkId = bytes.substr(offset, 4);
        const uint32_t chunkSize = readLe32(bytes, offset + 4);
        const size_t payload = offset + 8;
        if (chunkId == "fmt " && payload + 16 <= bytes.size()) {
            const uint16_t audioFormat = readLe16(bytes, payload);
            info.channels = static_cast<int>(readLe16(bytes, payload + 2));
            info.sampleRate = static_cast<int>(readLe32(bytes, payload + 4));
            info.bitsPerSample = static_cast<int>(readLe16(bytes, payload + 14));
            sawFormat = true;
            if (audioFormat != 1) {
                error = "unsupported WAV audio format " + std::to_string(audioFormat);
                return false;
            }
        } else if (chunkId == "data") {
            info.dataOffset = payload;
            info.dataBytes = chunkSize;
            if (info.dataOffset + info.dataBytes > bytes.size()) {
                info.dataBytes = bytes.size() - info.dataOffset;
            }
            if (sawFormat && info.valid()) {
                return true;
            }
        }
        offset = payload + chunkSize + (chunkSize & 1u);
    }
    if (!sawFormat) {
        error = "missing fmt chunk";
        return false;
    }
    error = "missing data chunk";
    return false;
}

std::vector<float> pcm16ToFloat(const uint8_t* pcm, size_t pcmBytes) {
    std::vector<float> samples;
    if (pcm == nullptr) {
        return samples;
    }
    const size_t count = pcmBytes / 2;
    samples.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const int16_t value = static_cast<int16_t>(
            static_cast<uint16_t>(pcm[i * 2]) |
            (static_cast<uint16_t>(pcm[i * 2 + 1]) << 8));
        samples[i] = static_cast<float>(value) / 32768.0f;
    }
    return samples;
}

}  // namespace vcore
