#include "base64.h"

namespace vcore {
namespace {

const char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeChar(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    if (data == nullptr || len == 0) {
        return out;
    }
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                               (static_cast<uint32_t>(data[i + 1]) << 8) |
                               static_cast<uint32_t>(data[i + 2]);
        out.push_back(kEncodeTable[(chunk >> 18) & 0x3F]);
        out.push_back(kEncodeTable[(chunk >> 12) & 0x3F]);
        out.push_back(kEncodeTable[(chunk >> 6) & 0x3F]);
        out.push_back(kEncodeTable[chunk & 0x3F]);
    }
    const size_t remaining = len - i;
    if (remaining == 1) {
        const uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kEncodeTable[(chunk >> 18) & 0x3F]);
        out.push_back(kEncodeTable[(chunk >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                               (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kEncodeTable[(chunk >> 18) & 0x3F]);
        out.push_back(kEncodeTable[(chunk >> 12) & 0x3F]);
        out.push_back(kEncodeTable[(chunk >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string base64Encode(const std::string& raw) {
    return base64Encode(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
}

bool base64Decode(const std::string& encoded, std::string& out) {
    out.clear();
    out.reserve((encoded.size() / 4) * 3);
    uint32_t accumulator = 0;
    int bits = 0;
    for (char rawChar : encoded) {
        const unsigned char c = static_cast<unsigned char>(rawChar);
        if (c == '=') {
            break;
        }
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        const int value = decodeChar(c);
        if (value < 0) {
            out.clear();
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
        }
    }
    return true;
}

}  // namespace vcore
