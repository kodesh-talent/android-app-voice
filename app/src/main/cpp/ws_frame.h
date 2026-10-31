// RFC 6455 WebSocket framing used by the Realtime voice channel.
//
// The transport itself lives in Java (TLS + Authorization header), so this
// module only builds/parses frames and assembles the handshake request.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vcore {

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct WsFrame {
    WsOpcode opcode = WsOpcode::Text;
    bool fin = true;
    std::string payload;
};

// Builds a client frame. Client frames are always masked, as required by RFC 6455.
std::string buildClientFrame(WsOpcode opcode, const std::string& payload, bool fin = true);

// Builds an unmasked server-style frame for parser and integration tests.
std::string buildServerFrame(WsOpcode opcode, const std::string& payload, bool fin = true);

// Incremental parser: feed() raw socket bytes, then drain takeFrame().
class WsFrameParser {
public:
    bool feed(const char* data, size_t length);
    // Returns false when no complete frame is buffered.
    bool takeFrame(WsFrame& frame);
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }
    // True once a close frame was consumed.
    bool closeReceived() const { return closeReceived_; }

private:
    std::string buffer_;
    bool failed_ = false;
    bool closeReceived_ = false;
    std::string error_;
};

// Creates the Sec-WebSocket-Key value (16 random bytes, base64 encoded).
std::string generateWebSocketKey();

// Builds the HTTP Upgrade request. |host| includes the port when non-default.
std::string buildHandshakeRequest(const std::string& host, const std::string& path,
                                  const std::string& apiKey, const std::string& origin,
                                  const std::string& webSocketKey);

// Validates the 101 response and the Sec-WebSocket-Accept echo.
bool validateHandshakeResponse(const std::string& response, const std::string& webSocketKey,
                               std::string& error);

// SHA-1 (needed for Sec-WebSocket-Accept) and base64 helpers.
std::string sha1(const std::string& data);

}  // namespace vcore
