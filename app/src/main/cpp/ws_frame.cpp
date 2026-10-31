#include "ws_frame.h"

#include <chrono>
#include <cstring>
#include <random>

#include "base64.h"

namespace vcore {
namespace {

const char* kAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

uint32_t rotateLeft(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::mt19937& rng() {
    static std::mt19937 engine([] {
        std::random_device device;
        const auto now = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::seed_seq seq{device(), device(), static_cast<uint32_t>(now),
                          static_cast<uint32_t>(now >> 32)};
        return std::mt19937(seq);
    }());
    return engine;
}

uint32_t randomWord() {
    return static_cast<uint32_t>(rng()());
}

}  // namespace

std::string sha1(const std::string& data) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    std::string message = data;
    const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) {
        message.push_back(static_cast<char>(0x00));
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<char>((bitLength >> (i * 8)) & 0xFF));
    }

    for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const size_t offset = chunk + static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(static_cast<uint8_t>(message[offset])) << 24) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(message[offset + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(message[offset + 2])) << 8) |
                   static_cast<uint32_t>(static_cast<uint8_t>(message[offset + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t temp = rotateLeft(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::string digest;
    digest.reserve(20);
    const uint32_t words[5] = {h0, h1, h2, h3, h4};
    for (uint32_t word : words) {
        digest.push_back(static_cast<char>((word >> 24) & 0xFF));
        digest.push_back(static_cast<char>((word >> 16) & 0xFF));
        digest.push_back(static_cast<char>((word >> 8) & 0xFF));
        digest.push_back(static_cast<char>(word & 0xFF));
    }
    return digest;
}

std::string generateWebSocketKey() {
    std::string raw;
    raw.reserve(16);
    for (int i = 0; i < 4; ++i) {
        const uint32_t word = randomWord();
        raw.push_back(static_cast<char>((word >> 24) & 0xFF));
        raw.push_back(static_cast<char>((word >> 16) & 0xFF));
        raw.push_back(static_cast<char>((word >> 8) & 0xFF));
        raw.push_back(static_cast<char>(word & 0xFF));
    }
    return base64Encode(raw);
}

std::string buildClientFrame(WsOpcode opcode, const std::string& payload, bool fin) {
    std::string frame;
    frame.reserve(payload.size() + 14);

    const uint8_t firstByte = static_cast<uint8_t>((fin ? 0x80 : 0x00) |
                                                   static_cast<uint8_t>(opcode));
    frame.push_back(static_cast<char>(firstByte));

    const size_t length = payload.size();
    const uint8_t maskBit = 0x80;
    if (length < 126) {
        frame.push_back(static_cast<char>(maskBit | static_cast<uint8_t>(length)));
    } else if (length <= 0xFFFF) {
        frame.push_back(static_cast<char>(maskBit | 126));
        frame.push_back(static_cast<char>((length >> 8) & 0xFF));
        frame.push_back(static_cast<char>(length & 0xFF));
    } else {
        frame.push_back(static_cast<char>(maskBit | 127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(
                static_cast<char>((static_cast<uint64_t>(length) >> (i * 8)) & 0xFF));
        }
    }

    const uint32_t seed = randomWord();
    const uint8_t mask[4] = {
        static_cast<uint8_t>(seed & 0xFF),
        static_cast<uint8_t>((seed >> 8) & 0xFF),
        static_cast<uint8_t>((seed >> 16) & 0xFF),
        static_cast<uint8_t>((seed >> 24) & 0xFF),
    };
    frame.append(reinterpret_cast<const char*>(mask), 4);

    for (size_t i = 0; i < length; ++i) {
        frame.push_back(
            static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]));
    }
    return frame;
}

std::string buildServerFrame(WsOpcode opcode, const std::string& payload, bool fin) {
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) |
                                      static_cast<uint8_t>(opcode)));
    const size_t length = payload.size();
    if (length < 126) {
        frame.push_back(static_cast<char>(length));
    } else if (length <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((length >> 8) & 0xFF));
        frame.push_back(static_cast<char>(length & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(
                static_cast<char>((static_cast<uint64_t>(length) >> (i * 8)) & 0xFF));
        }
    }
    frame.append(payload);
    return frame;
}

bool WsFrameParser::feed(const char* data, size_t length) {
    if (failed_ || data == nullptr || length == 0) {
        return !failed_;
    }
    buffer_.append(data, length);
    return true;
}

bool WsFrameParser::takeFrame(WsFrame& frame) {
    if (failed_ || buffer_.size() < 2) {
        return false;
    }
    const uint8_t b0 = static_cast<uint8_t>(buffer_[0]);
    const uint8_t b1 = static_cast<uint8_t>(buffer_[1]);
    const bool fin = (b0 & 0x80) != 0;
    const uint8_t opcode = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t payloadLength = b1 & 0x7F;
    size_t offset = 2;

    if (payloadLength == 126) {
        if (buffer_.size() < 4) return false;
        payloadLength = (static_cast<uint64_t>(static_cast<uint8_t>(buffer_[2])) << 8) |
                        static_cast<uint64_t>(static_cast<uint8_t>(buffer_[3]));
        offset = 4;
    } else if (payloadLength == 127) {
        if (buffer_.size() < 10) return false;
        payloadLength = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLength = (payloadLength << 8) |
                            static_cast<uint64_t>(
                                static_cast<uint8_t>(buffer_[2 + static_cast<size_t>(i)]));
        }
        offset = 10;
    }

    if (payloadLength > (16ull * 1024ull * 1024ull)) {
        failed_ = true;
        error_ = "websocket frame too large";
        return false;
    }

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (buffer_.size() < offset + 4) return false;
        std::memcpy(mask, buffer_.data() + offset, 4);
        offset += 4;
    }

    if (buffer_.size() < offset + payloadLength) {
        return false;
    }

    std::string payload = buffer_.substr(offset, static_cast<size_t>(payloadLength));
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
        }
    }
    buffer_.erase(0, offset + static_cast<size_t>(payloadLength));

    frame.fin = fin;
    frame.opcode = static_cast<WsOpcode>(opcode);
    frame.payload = std::move(payload);
    if (frame.opcode == WsOpcode::Close) {
        closeReceived_ = true;
    }
    return true;
}

std::string buildHandshakeRequest(const std::string& host, const std::string& path,
                                  const std::string& apiKey, const std::string& origin,
                                  const std::string& webSocketKey) {
    std::string request;
    request += "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + webSocketKey + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    if (!origin.empty()) {
        request += "Origin: " + origin + "\r\n";
    }
    request += "Authorization: Bearer " + apiKey + "\r\n";
    request += "User-Agent: VoiceLatencyLab/1.0\r\n";
    request += "\r\n";
    return request;
}

bool validateHandshakeResponse(const std::string& response, const std::string& webSocketKey,
                               std::string& error) {
    const size_t lineEnd = response.find("\r\n");
    if (lineEnd == std::string::npos) {
        error = "incomplete handshake response";
        return false;
    }
    const std::string statusLine = response.substr(0, lineEnd);
    if (statusLine.find(" 101") == std::string::npos) {
        error = "handshake rejected: " + statusLine;
        return false;
    }
    const std::string expected = base64Encode(sha1(webSocketKey + kAcceptGuid));

    // Header names are case-insensitive but the accept value is not: compare the
    // value against the response text as sent.
    std::string lower = response;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    std::string expectedLower = expected;
    for (char& c : expectedLower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (lower.find("sec-websocket-accept: " + expectedLower) == std::string::npos &&
        lower.find("sec-websocket-accept:" + expectedLower) == std::string::npos) {
        error = "Sec-WebSocket-Accept mismatch";
        return false;
    }
    return true;
}

}  // namespace vcore
