#include "http_client.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "latency_stats.h"

namespace vcore {
namespace {

std::string trimAscii(const std::string& input) {
    size_t begin = 0;
    size_t end = input.size();
    while (begin < end && (input[begin] == ' ' || input[begin] == '\t' ||
                           input[begin] == '\r' || input[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (input[end - 1] == ' ' || input[end - 1] == '\t' ||
                           input[end - 1] == '\r' || input[end - 1] == '\n')) {
        --end;
    }
    return input.substr(begin, end - begin);
}

std::string toLower(const std::string& input) {
    std::string out = input;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

}  // namespace

std::string ParsedUrl::hostHeader() const {
    const bool isDefaultPort = ((scheme == "https" || scheme == "wss") && port == 443) ||
                               ((scheme == "http" || scheme == "ws") && port == 80);
    return isDefaultPort ? host : host + ":" + std::to_string(port);
}

bool parseUrl(const std::string& url, ParsedUrl& out, std::string& error) {
    out = ParsedUrl();
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        error = "missing scheme";
        return false;
    }
    out.scheme = toLower(url.substr(0, schemeEnd));
    if (out.scheme != "https" && out.scheme != "http" && out.scheme != "wss" &&
        out.scheme != "ws") {
        error = "unsupported scheme: " + out.scheme;
        return false;
    }
    const size_t authorityStart = schemeEnd + 3;
    const size_t pathStart = url.find('/', authorityStart);
    const std::string authority = pathStart == std::string::npos
                                      ? url.substr(authorityStart)
                                      : url.substr(authorityStart, pathStart - authorityStart);
    out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    if (out.path.empty()) {
        out.path = "/";
    }

    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = std::atoi(authority.substr(colon + 1).c_str());
    } else {
        out.host = authority;
    }
    if (out.port <= 0) {
        out.port = (out.scheme == "https" || out.scheme == "wss") ? 443 : 80;
    }
    if (out.host.empty()) {
        error = "missing host";
        return false;
    }
    return true;
}

std::string buildHttpRequest(const HttpRequestSpec& spec) {
    std::string request;
    request.reserve(spec.body.size() + 512);
    request += spec.method + " " + spec.path + " HTTP/1.1\r\n";
    request += "Host: " + spec.hostHeader + "\r\n";
    request += "Authorization: Bearer " + spec.apiKey + "\r\n";
    request += "Accept: " + spec.accept + "\r\n";
    if (!spec.contentType.empty()) {
        request += "Content-Type: " + spec.contentType + "\r\n";
    }
    request += "User-Agent: " + spec.userAgent + "\r\n";
    request += "Accept-Encoding: identity\r\n";
    if (spec.closeConnection) {
        request += "Connection: close\r\n";
    }
    request += "Content-Length: " + std::to_string(spec.body.size()) + "\r\n";
    request += "\r\n";
    request += spec.body;
    return request;
}

void HttpStream::reset() {
    buffer_.clear();
    body_.clear();
    headers_.clear();
    headComplete_ = false;
    finished_ = false;
    eof_ = false;
    chunked_ = false;
    hasLength_ = false;
    contentLength_ = 0;
    statusCode_ = 0;
    reason_.clear();
    error_.clear();
    chunkState_ = ChunkState::Size;
    chunkRemaining_ = 0;
    startUs_ = 0;
    headUs_ = 0;
    firstByteUs_ = 0;
}

void HttpStream::startClock() {
    startUs_ = nowMicros();
}

double HttpStream::headLatencyMs() const {
    if (startUs_ == 0 || headUs_ == 0) return 0.0;
    return static_cast<double>(headUs_ - startUs_) / 1000.0;
}

double HttpStream::firstByteLatencyMs() const {
    if (startUs_ == 0 || firstByteUs_ == 0) return 0.0;
    return static_cast<double>(firstByteUs_ - startUs_) / 1000.0;
}

std::string HttpStream::header(const std::string& name) const {
    const auto it = headers_.find(toLower(name));
    return it == headers_.end() ? std::string() : it->second;
}

std::string HttpStream::takeBody() {
    std::string out;
    out.swap(body_);
    return out;
}

bool HttpStream::parseHead() {
    const size_t headEnd = buffer_.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        return false;
    }
    const std::string headText = buffer_.substr(0, headEnd);
    buffer_.erase(0, headEnd + 4);

    const size_t firstLineEnd = headText.find("\r\n");
    const std::string statusLine =
        firstLineEnd == std::string::npos ? headText : headText.substr(0, firstLineEnd);
    std::istringstream statusStream(statusLine);
    std::string version;
    statusStream >> version >> statusCode_;
    std::getline(statusStream, reason_);
    reason_ = trimAscii(reason_);
    if (statusCode_ <= 0) {
        error_ = "invalid HTTP status line: " + statusLine;
        return false;
    }

    size_t lineStart = firstLineEnd == std::string::npos ? headText.size() : firstLineEnd + 2;
    std::string currentName;
    while (lineStart < headText.size()) {
        size_t lineEnd = headText.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = headText.size();
        }
        const std::string line = headText.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 2;
        if (line.empty()) {
            continue;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            if (!currentName.empty()) {
                headers_[currentName] += " " + trimAscii(line);
            }
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        currentName = toLower(trimAscii(line.substr(0, colon)));
        headers_[currentName] = trimAscii(line.substr(colon + 1));
    }

    headComplete_ = true;
    headUs_ = nowMicros();
    chunked_ = toLower(header("transfer-encoding")).find("chunked") != std::string::npos;
    if (!chunked_) {
        const std::string lengthHeader = header("content-length");
        if (!lengthHeader.empty()) {
            hasLength_ = true;
            contentLength_ = static_cast<size_t>(std::strtoull(lengthHeader.c_str(), nullptr, 10));
        }
    }
    return true;
}

bool HttpStream::feed(const char* data, size_t length) {
    if (error_.size() > 0 || finished_) {
        return error_.empty();
    }
    if (data != nullptr && length > 0) {
        buffer_.append(data, length);
    }
    if (!headComplete_ && !parseHead()) {
        if (!error_.empty()) {
            return false;
        }
        if (buffer_.size() > 64 * 1024) {
            error_ = "HTTP head too large";
            return false;
        }
        return true;
    }
    drainBody();
    return error_.empty();
}

void HttpStream::drainBody() {
    if (finished_) {
        return;
    }
    if (!chunked_) {
        if (!buffer_.empty()) {
            if (firstByteUs_ == 0) {
                firstByteUs_ = nowMicros();
            }
            if (hasLength_) {
                const size_t remaining =
                    contentLength_ > body_.size() ? contentLength_ - body_.size() : 0;
                const size_t take = std::min(remaining, buffer_.size());
                body_.append(buffer_, 0, take);
                buffer_.erase(0, take);
                if (body_.size() >= contentLength_) {
                    finished_ = true;
                }
            } else {
                body_ += buffer_;
                buffer_.clear();
            }
        }
        if (eof_) {
            finished_ = true;
        }
        return;
    }

    while (true) {
        if (chunkState_ == ChunkState::Size) {
            const size_t lineEnd = buffer_.find("\r\n");
            if (lineEnd == std::string::npos) {
                return;
            }
            const std::string sizeLine = trimAscii(buffer_.substr(0, lineEnd));
            const std::string hexPart = sizeLine.substr(0, sizeLine.find(';'));
            char* parseEnd = nullptr;
            const unsigned long long value = std::strtoull(hexPart.c_str(), &parseEnd, 16);
            if (parseEnd == hexPart.c_str()) {
                error_ = "malformed chunk size: " + sizeLine;
                return;
            }
            buffer_.erase(0, lineEnd + 2);
            if (value == 0) {
                chunkState_ = ChunkState::Trailer;
            } else {
                chunkRemaining_ = static_cast<size_t>(value);
                chunkState_ = ChunkState::Data;
            }
        } else if (chunkState_ == ChunkState::Data) {
            if (buffer_.empty()) {
                return;
            }
            if (firstByteUs_ == 0) {
                firstByteUs_ = nowMicros();
            }
            const size_t take = std::min(chunkRemaining_, buffer_.size());
            body_.append(buffer_, 0, take);
            buffer_.erase(0, take);
            chunkRemaining_ -= take;
            if (chunkRemaining_ == 0) {
                chunkState_ = ChunkState::Trailer;
                // Consume the CRLF that terminates the chunk payload.
                if (buffer_.size() >= 2) {
                    buffer_.erase(0, 2);
                    chunkState_ = ChunkState::Size;
                } else {
                    chunkState_ = ChunkState::Trailer;
                }
            }
        } else if (chunkState_ == ChunkState::Trailer) {
            const size_t lineEnd = buffer_.find("\r\n");
            if (lineEnd == std::string::npos) {
                if (eof_) {
                    finished_ = true;
                }
                return;
            }
            if (lineEnd == 0) {
                buffer_.erase(0, 2);
                chunkState_ = ChunkState::Done;
                finished_ = true;
                return;
            }
            buffer_.erase(0, lineEnd + 2);
        } else {
            finished_ = true;
            return;
        }
    }
}

void HttpStream::notifyEof() {
    eof_ = true;
    if (!headComplete_) {
        if (buffer_.empty()) {
            finished_ = true;
            return;
        }
        if (!parseHead()) {
            finished_ = true;
            return;
        }
    }
    drainBody();
    if (!finished_) {
        if (chunked_) {
            // Tolerate a missing terminating chunk when the peer closes cleanly.
            chunkState_ = ChunkState::Done;
        }
        finished_ = true;
    }
}

}  // namespace vcore
