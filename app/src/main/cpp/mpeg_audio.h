// MPEG audio (MP3) frame parsing: duration and format probing for TTS replies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vcore {

struct MpegInfo {
    int version = 0;        // 1, 2, or 25 (MPEG 2.5)
    int layer = 0;          // 1, 2, or 3
    int sampleRate = 0;     // Hz
    int channels = 0;       // 1 or 2
    int bitrateKbps = 0;    // first frame's bitrate
    size_t frameCount = 0;  // frames successfully walked
    size_t audioOffset = 0; // byte offset of the first frame
    double durationMs = 0.0;

    bool valid() const { return frameCount > 0 && sampleRate > 0; }
};

// True when |bytes| starts with an ID3v2 tag or an MPEG audio frame sync.
bool looksLikeMpegAudio(const std::string& bytes);

// Walks the frame headers and sums frame durations. Handles a leading ID3v2 tag.
// Returns false when no frame could be decoded.
bool parseMpegAudio(const std::string& bytes, MpegInfo& info, std::string& error);

}  // namespace vcore
