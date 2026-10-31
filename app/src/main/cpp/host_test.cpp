// Host-side verification for the native protocol core.
//
// Feeds a captured raw HTTP response through HttpStream and checks status line,
// chunked decoding, completion and body integrity. Built and run on the
// development host (not shipped in the APK).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "base64.h"
#include "client_vad.h"
#include "http_client.h"
#include "json_lite.h"
#include "latency_stats.h"
#include "wav_codec.h"
#include "ws_frame.h"

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    } else {
        std::printf("  ok    %s\n", what);
    }
}

std::string readFile(const char* path) {
    std::string out;
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return out;
    }
    char buffer[4096];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, count);
    }
    std::fclose(file);
    return out;
}

// Replays |raw| into |stream| in chunks of |chunk| bytes.
void replay(vcore::HttpStream& stream, const std::string& raw, size_t chunk) {
    size_t offset = 0;
    while (offset < raw.size()) {
        const size_t take = std::min(chunk, raw.size() - offset);
        if (!stream.feed(raw.data() + offset, take)) {
            return;
        }
        offset += take;
        if (stream.finished()) {
            return;
        }
    }
    stream.notifyEof();
}

void testUrlParsing() {
    std::printf("url parsing\n");
    vcore::ParsedUrl url;
    std::string error;

    check(vcore::parseUrl("https://xenoai.eu.cc/v1", url, error), "parses https base url");
    check(url.scheme == "https" && url.host == "xenoai.eu.cc" && url.port == 443, "scheme/host/port");
    check(url.path == "/v1", "keeps path");

    check(vcore::parseUrl("wss://xenoai.eu.cc/v1/realtime?model=grok-voice-realtime", url, error),
          "parses realtime url");
    check(url.path == "/v1/realtime?model=grok-voice-realtime", "keeps query string");

    check(vcore::parseUrl("https://example.com:8443/v1", url, error), "parses explicit port");
    check(url.port == 8443 && url.hostHeader() == "example.com:8443", "host header with port");

    check(!vcore::parseUrl("xenoai.eu.cc/v1", url, error), "rejects url without scheme");
}

void testChunkedResponse(const std::string& raw) {
    std::printf("chunked response decoding (%zu raw bytes)\n", raw.size());
    if (raw.empty()) {
        std::printf("  SKIP  capture file missing\n");
        return;
    }

    // Feed in small slices to exercise the incremental parser.
    for (size_t chunk : {size_t(7), size_t(64), size_t(1024), raw.size()}) {
        vcore::HttpStream stream;
        replay(stream, raw, chunk);
        const std::string body = stream.body();
        char label[96];
        std::snprintf(label, sizeof(label), "chunk=%zu finished=%d status=%d", chunk,
                      stream.finished() ? 1 : 0, stream.statusCode());
        check(stream.finished(), label);
        check(stream.statusCode() == 200, "status 200");
        check(!stream.header("content-type").empty(), "content-type present");
        check(body.size() > 0, "body non-empty");
        check(body.find("chunked") == std::string::npos || chunk != 7, "no framing leak");
        if (chunk == 1024) {
            std::printf("        body=%zu bytes, type=%s\n", body.size(),
                        stream.header("content-type").c_str());
        }
    }

    // A single feed must produce the same body as incremental feeds.
    vcore::HttpStream oneShot;
    oneShot.feed(raw.data(), raw.size());
    vcore::HttpStream sliced;
    replay(sliced, raw, 13);
    check(oneShot.body() == sliced.body(), "incremental body matches one-shot body");
}

void testWavRoundTrip() {
    std::printf("wav round trip\n");
    std::vector<int16_t> pcm;
    for (int i = 0; i < 800; ++i) {
        pcm.push_back(static_cast<int16_t>(i % 200 - 100));
    }
    const std::string wav = vcore::encodePcm16Wav(
            reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * 2, 16000, 1);
    check(!wav.empty(), "encodes wav");

    vcore::WavInfo info;
    std::string error;
    check(vcore::parseWav(wav, info, error), "parses wav");
    check(info.sampleRate == 16000, "sample rate");
    check(info.channels == 1, "channels");
    check(info.bitsPerSample == 16, "bit depth");
    check(info.dataBytes == pcm.size() * 2, "data bytes");
    check(info.dataOffset + info.dataBytes == wav.size(), "data window in range");
}

void testJson() {
    std::printf("json\n");
    vcore::JsonValue value;
    std::string error;
    const char* text = R"({"ok":true,"statusCode":200,"ttfbMs":12.5,"text":"a\"b"})";
    check(vcore::JsonValue::parse(text, value, error), "parses object");
    check(value["ok"].asBool(), "bool field");
    check(value["statusCode"].asNumber() == 200, "number field");
    check(value["ttfbMs"].asNumber() == 12.5, "fractional field");
    check(value["text"].asString() == "a\"b", "escaped string");

    vcore::JsonValue built = vcore::JsonValue::makeObject();
    built.set("message", vcore::JsonValue::makeString("quote\" and \\ slash"));
    vcore::JsonValue reparsed;
    check(vcore::JsonValue::parse(built.dump(), reparsed, error), "re-parses dump");
    check(reparsed["message"].asString() == "quote\" and \\ slash", "round trips escapes");
}

void testStats() {
    std::printf("statistics\n");
    vcore::Rollup rollup;
    for (double sample : {10.0, 20.0, 30.0, 40.0, 50.0}) {
        rollup.add(sample);
    }
    vcore::JsonValue stats;
    std::string error;
    check(vcore::JsonValue::parse(rollup.toJson(), stats, error), "stats json");
    check(stats["count"].asNumber() == 5, "count");
    check(stats["avg"].asNumber() == 30, "average");
    check(stats["min"].asNumber() == 10 && stats["max"].asNumber() == 50, "range");
    check(stats["p50"].asNumber() == 30, "median");
}

void testWebSocket() {
    std::printf("websocket framing\n");
    const std::string key = vcore::generateWebSocketKey();
    check(!key.empty(), "generates key");
    check(key.size() == 24, "key is 16 bytes base64");

    const std::string frame = vcore::buildClientFrame(vcore::WsOpcode::Text,
                                                     std::string("{\"type\":\"x\"}"));
    check(frame.size() >= 6, "frame has header");
    check((static_cast<unsigned char>(frame[0]) & 0x80) != 0, "fin bit set");
    check((static_cast<unsigned char>(frame[0]) & 0x0F) == 0x1, "text opcode");
    check((static_cast<unsigned char>(frame[1]) & 0x80) != 0, "mask bit set");

    // Round trip a server frame (unmasked) through the parser.
    std::string server;
    server.push_back(static_cast<char>(0x81));
    server.push_back(static_cast<char>(5));
    server.append("hello");
    vcore::WsFrameParser parser;
    check(parser.feed(server.data(), server.size()), "feeds server frame");
    vcore::WsFrame received;
    check(parser.takeFrame(received), "takes one frame");
    check(received.payload == "hello", "payload intact");

    // A server-style unmasked text frame must round-trip through the same
    // parser used by the realtime session, including a >125 byte payload.
    const std::string realtimeEvent =
        "{\"type\":\"response.output_audio.delta\",\"delta\":\"AAAA\"}";
    const std::string serverFrame =
        vcore::buildServerFrame(vcore::WsOpcode::Text, realtimeEvent);
    vcore::WsFrameParser eventParser;
    check(eventParser.feed(serverFrame.data(), serverFrame.size()), "feeds event frame");
    vcore::WsFrame eventFrame;
    check(eventParser.takeFrame(eventFrame), "takes event frame");
    check(eventFrame.payload == realtimeEvent, "event payload intact");
    const std::string longEvent(300, 'x');
    const std::string longFrame =
        vcore::buildServerFrame(vcore::WsOpcode::Text, longEvent);
    vcore::WsFrameParser longParser;
    longParser.feed(longFrame.data(), longFrame.size());
    vcore::WsFrame longReceived;
    check(longParser.takeFrame(longReceived), "takes extended-length frame");
    check(longReceived.payload == longEvent, "extended payload intact");

    const std::string handshake = vcore::buildHandshakeRequest(
            "xenoai.eu.cc", "/v1/realtime?model=grok-voice-realtime", "test-key",
            "https://xenoai.eu.cc", key);
    check(handshake.find("GET /v1/realtime?model=grok-voice-realtime HTTP/1.1") == 0,
          "handshake request line");
    check(handshake.find("Upgrade: websocket") != std::string::npos, "upgrade header");
    check(handshake.find("Sec-WebSocket-Key: " + key) != std::string::npos, "key header");
    check(handshake.find("Authorization: Bearer test-key") != std::string::npos, "auth header");

    // Sec-WebSocket-Accept for the RFC 6455 example key.
    std::string error;
    const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    check(vcore::validateHandshakeResponse(response, "dGhlIHNhbXBsZSBub25jZQ==", error),
          "validates RFC 6455 accept echo");
    check(!vcore::validateHandshakeResponse(response, "AAAAAAAAAAAAAAAAAAAAAA==", error),
          "rejects wrong accept echo");
}

void testBase64() {
    std::printf("base64\n");
    const std::string encoded = vcore::base64Encode(std::string("hello world"));
    check(encoded == "aGVsbG8gd29ybGQ=", "encodes");
    std::string decoded;
    check(vcore::base64Decode(encoded, decoded), "decodes");
    check(decoded == "hello world", "round trips");
}

void testRealtimeEventShapes() {
    std::printf("realtime event parsing\n");
    vcore::JsonValue event;
    std::string error;
    const std::string audioEvent =
        "{\"type\":\"response.output_audio.delta\",\"delta\":\"AAAA\"}";
    check(vcore::JsonValue::parse(audioEvent, event, error), "parses audio delta");
    check(event["type"].asString() == "response.output_audio.delta", "audio event type");
    check(event["delta"].asString() == "AAAA", "audio delta field");

    const std::string doneEvent =
        "{\"type\":\"response.done\",\"response\":{\"status\":\"completed\"}}";
    check(vcore::JsonValue::parse(doneEvent, event, error), "parses response.done");
    check(event["response"]["status"].asString() == "completed", "done status");

    const std::string textEvent =
        "{\"type\":\"response.text.delta\",\"delta\":\"hello\"}";
    check(vcore::JsonValue::parse(textEvent, event, error), "parses text delta");
    check(event["delta"].asString() == "hello", "text delta field");
}

void appendVadFrames(std::string& pcm, int16_t amplitude, int frames) {
    const int16_t sample = amplitude;
    const auto* bytes = reinterpret_cast<const char*>(&sample);
    for (int frame = 0; frame < frames; ++frame) {
        for (int i = 0; i < 320; ++i) {
            pcm.append(bytes, 2);
        }
    }
}

void testClientVad() {
    std::printf("client VAD\n");
    vcore::ClientVad vad(16000, 100, 100, 60, 500);
    std::string leadingSilence;
    appendVadFrames(leadingSilence, 0, 8);
    vcore::ClientVad::Result first = vad.process(leadingSilence.data(), leadingSilence.size());
    check(!first.speechStarted, "ignores leading silence");
    check(first.audio.empty(), "does not upload leading silence");

    std::string speech;
    appendVadFrames(speech, 3000, 10);
    vcore::ClientVad::Result started = vad.process(speech.data(), speech.size());
    check(started.speechStarted, "detects speech onset");
    check(!started.audio.empty(), "includes speech and pre-roll");

    std::string trailingSilence;
    appendVadFrames(trailingSilence, 0, 8);
    vcore::ClientVad::Result ended = vad.process(trailingSilence.data(), trailingSilence.size());
    check(ended.utteranceComplete, "commits after trailing silence");
    check(!vad.speechActive(), "returns to listening state");

    std::string afterReset;
    appendVadFrames(afterReset, 3000, 1);
    vad.reset();
    vcore::ClientVad::Result resetResult = vad.process(afterReset.data(), afterReset.size());
    check(!resetResult.speechStarted, "reset clears partial state");
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("voicecore host checks\n\n");
    testUrlParsing();
    testJson();
    testStats();
    testBase64();
    testRealtimeEventShapes();
    testClientVad();
    testWavRoundTrip();
    testWebSocket();
    if (argc > 1) {
        testChunkedResponse(readFile(argv[1]));
    } else {
        std::printf("chunked response decoding\n  SKIP  no capture file given\n");
    }

    std::printf("\n%s (%d failure%s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures,
                gFailures == 1 ? "" : "s");
    return gFailures == 0 ? 0 : 1;
}
