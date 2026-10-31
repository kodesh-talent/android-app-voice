// Minimal Base64 codec used for audio payloads and WebSocket handshake keys.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vcore {

std::string base64Encode(const uint8_t* data, size_t len);
std::string base64Encode(const std::string& raw);
bool base64Decode(const std::string& encoded, std::string& out);

}  // namespace vcore
