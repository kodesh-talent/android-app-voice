#include "mpeg_audio.h"

#include <cstdio>

namespace vcore {
namespace {

// Bitrate tables in kbps, indexed by the 4-bit header field.
const int kBitratesV1L1[16] = {0, 32, 64, 96, 128, 160, 192, 224,
                               256, 288, 320, 352, 384, 416, 448, -1};
const int kBitratesV1L2[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                               128, 160, 192, 224, 256, 320, 384, -1};
const int kBitratesV1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96,
                               112, 128, 160, 192, 224, 256, 320, -1};
const int kBitratesV2L1[16] = {0, 32, 48, 56, 64, 80, 96, 112,
                               128, 144, 160, 176, 192, 224, 256, -1};
const int kBitratesV2L23[16] = {0, 8, 16, 24, 32, 40, 48, 56,
                                64, 80, 96, 112, 128, 144, 160, -1};

const int kSampleRatesV1[3] = {44100, 48000, 32000};
const int kSampleRatesV2[3] = {22050, 24000, 16000};
const int kSampleRatesV25[3] = {11025, 12000, 8000};

struct FrameHeader {
    int version = 0;
    int layer = 0;
    int sampleRate = 0;
    int bitrateKbps = 0;
    int channels = 0;
    int samplesPerFrame = 0;
    size_t frameBytes = 0;
};

// Decodes a 4-byte frame header. Returns false for sync/reserved-field failures.
bool decodeHeader(const uint8_t* p, FrameHeader& out) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return false;
    }
    const int versionBits = (p[1] >> 3) & 0x03;
    const int layerBits = (p[1] >> 1) & 0x03;
    if (versionBits == 1 || layerBits == 0) {
        return false;  // reserved
    }
    switch (versionBits) {
        case 3: out.version = 1; break;
        case 2: out.version = 2; break;
        default: out.version = 25; break;
    }
    // layerBits 3 = Layer I, 2 = Layer II, 1 = Layer III.
    out.layer = 4 - layerBits;

    const int bitrateIndex = (p[2] >> 4) & 0x0F;
    const int sampleRateIndex = (p[2] >> 2) & 0x03;
    const int padding = (p[2] >> 1) & 0x01;
    if (bitrateIndex == 0 || bitrateIndex == 15 || sampleRateIndex == 3) {
        return false;  // free-form / invalid / reserved
    }

    const int* table;
    if (out.version == 1) {
        table = out.layer == 1 ? kBitratesV1L1
                               : (out.layer == 2 ? kBitratesV1L2 : kBitratesV1L3);
    } else {
        table = out.layer == 1 ? kBitratesV2L1 : kBitratesV2L23;
    }
    out.bitrateKbps = table[bitrateIndex];
    if (out.bitrateKbps <= 0) {
        return false;
    }

    const int* rates = out.version == 1 ? kSampleRatesV1
                                        : (out.version == 2 ? kSampleRatesV2 : kSampleRatesV25);
    out.sampleRate = rates[sampleRateIndex];
    if (out.sampleRate <= 0) {
        return false;
    }

    const int channelMode = (p[3] >> 6) & 0x03;
    out.channels = channelMode == 3 ? 1 : 2;

    if (out.layer == 1) {
        out.samplesPerFrame = 384;
        out.frameBytes = static_cast<size_t>(
            (12 * out.bitrateKbps * 1000 / out.sampleRate + padding) * 4);
    } else if (out.layer == 2) {
        out.samplesPerFrame = 1152;
        out.frameBytes = static_cast<size_t>(
            144 * out.bitrateKbps * 1000 / out.sampleRate + padding);
    } else {
        out.samplesPerFrame = out.version == 1 ? 1152 : 576;
        out.frameBytes = static_cast<size_t>(
            (out.samplesPerFrame / 8) * out.bitrateKbps * 1000 / out.sampleRate + padding);
    }
    return out.frameBytes >= 4;
}

// Skips a leading ID3v2 tag, returning the offset of the first audio byte.
size_t skipId3v2(const std::string& bytes) {
    if (bytes.size() < 10 || bytes.compare(0, 3, "ID3") != 0) {
        return 0;
    }
    // Syncsafe 28-bit size in bytes 6..9, plus the 10-byte header, plus an
    // optional 10-byte footer when the footer flag (bit 4) is set.
    const size_t size = (static_cast<size_t>(bytes[6] & 0x7F) << 21) |
                        (static_cast<size_t>(bytes[7] & 0x7F) << 14) |
                        (static_cast<size_t>(bytes[8] & 0x7F) << 7) |
                        static_cast<size_t>(bytes[9] & 0x7F);
    const bool hasFooter = (bytes[5] & 0x10) != 0;
    const size_t total = 10 + size + (hasFooter ? 10 : 0);
    return total <= bytes.size() ? total : 0;
}

}  // namespace

bool looksLikeMpegAudio(const std::string& bytes) {
    if (bytes.size() < 4) {
        return false;
    }
    if (bytes.compare(0, 3, "ID3") == 0) {
        return true;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
    FrameHeader header;
    return decodeHeader(p, header);
}

bool parseMpegAudio(const std::string& bytes, MpegInfo& info, std::string& error) {
    info = MpegInfo();
    const size_t start = skipId3v2(bytes);
    if (start + 4 > bytes.size()) {
        error = "no MPEG audio frames after ID3 tag";
        return false;
    }

    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    size_t offset = start;
    double durationMs = 0.0;

    while (offset + 4 <= bytes.size()) {
        FrameHeader header;
        if (!decodeHeader(data + offset, header)) {
            // A single corrupt byte should not discard the frames already read.
            break;
        }
        if (info.frameCount == 0) {
            info.version = header.version;
            info.layer = header.layer;
            info.sampleRate = header.sampleRate;
            info.channels = header.channels;
            info.bitrateKbps = header.bitrateKbps;
            info.audioOffset = offset;
        }
        if (offset + header.frameBytes > bytes.size()) {
            break;  // truncated tail
        }
        durationMs += 1000.0 * static_cast<double>(header.samplesPerFrame) /
                      static_cast<double>(header.sampleRate);
        ++info.frameCount;
        offset += header.frameBytes;
    }

    if (info.frameCount == 0) {
        error = "no decodable MPEG audio frame";
        return false;
    }
    info.durationMs = durationMs;
    return true;
}

}  // namespace vcore
