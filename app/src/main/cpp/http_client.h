// HTTP/1.1 client-side protocol state: request framing, response head parsing
// and chunked/content-length body decoding.
//
// Java supplies raw TLS socket bytes; this module owns the protocol. That keeps
// all model-call wire handling in C++ while Java stays a byte pump.
#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace vcore {

struct ParsedUrl {
    std::string scheme;  // https / http / wss / ws
    std::string host;
    int port = 0;
    std::string path;    // begins with '/'

    bool valid() const { return !host.empty() && port > 0; }
    // Host header value (adds ":port" only for non-default ports).
    std::string hostHeader() const;
};

bool parseUrl(const std::string& url, ParsedUrl& out, std::string& error);

struct HttpRequestSpec {
    std::string method = "POST";
    std::string path;
    std::string hostHeader;
    std::string apiKey;
    std::string contentType;
    std::string accept = "*/*";
    std::string body;
    std::string userAgent = "VoiceLatencyLab/1.1 (Android; voicecore)";
    bool closeConnection = true;
};

std::string buildHttpRequest(const HttpRequestSpec& spec);

// Incremental HTTP response reader.
//
// Usage: feed() socket chunks until finished() is true (or the socket EOFs, in
// which case call notifyEof()). body() returns the accumulated payload.
class HttpStream {
public:
    void reset();

    // Returns false when the stream is malformed; see error().
    bool feed(const char* data, size_t length);
    void notifyEof();

    bool headComplete() const { return headComplete_; }
    bool finished() const { return finished_; }
    const std::string& error() const { return error_; }

    int statusCode() const { return statusCode_; }
    const std::string& reason() const { return reason_; }
    std::string header(const std::string& name) const;

    size_t bodySize() const { return body_.size(); }
    const std::string& body() const { return body_; }
    std::string takeBody();

    // Milliseconds from start to the moment the head finished parsing.
    double headLatencyMs() const;
    // Milliseconds from start to the first body byte.
    double firstByteLatencyMs() const;
    void startClock();

private:
    bool parseHead();
    void drainBody();

    std::string buffer_;
    std::string body_;
    std::map<std::string, std::string> headers_;

    bool headComplete_ = false;
    bool finished_ = false;
    bool eof_ = false;
    bool chunked_ = false;
    bool hasLength_ = false;
    size_t contentLength_ = 0;

    int statusCode_ = 0;
    std::string reason_;
    std::string error_;

    // Chunked decoding state.
    enum class ChunkState { Size, Data, Trailer, Done };
    ChunkState chunkState_ = ChunkState::Size;
    size_t chunkRemaining_ = 0;

    long long startUs_ = 0;
    long long headUs_ = 0;
    long long firstByteUs_ = 0;
};

}  // namespace vcore
