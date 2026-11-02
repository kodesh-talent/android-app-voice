#include "voice_core.h"

#include <android/log.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "base64.h"
#include "json_lite.h"
#include "ws_frame.h"

namespace vcore {
namespace {

constexpr size_t kReadBufferSize = 16 * 1024;
constexpr size_t kPreviewLimit = 512;
constexpr int64_t kSessionMaxMs = 120000;
constexpr int64_t kNoResponseTimeoutMs = 15000;
constexpr int64_t kResponseTimeoutMs = 60000;
constexpr int64_t kReadyFallbackMs = 2000;
constexpr int64_t kResumeDelayMs = 200;
// Gateway closes the socket with 1009 once a text frame exceeds 32769 bytes.
// 100 ms of 24 kHz PCM16 is 4800 bytes and stays far under that limit.
constexpr size_t kMaxAppendPcmBytes = 4800;

std::string shorten(const std::string& text, size_t limit) {
    return text.size() <= limit ? text : text.substr(0, limit) + "...";
}

int statusCodeFromHead(const std::string& head) {
    const size_t lineEnd = head.find("\r\n");
    const std::string statusLine = lineEnd == std::string::npos ? head : head.substr(0, lineEnd);
    const size_t firstSpace = statusLine.find(' ');
    return firstSpace == std::string::npos ? 0 : std::atoi(statusLine.c_str() + firstSpace + 1);
}

std::string closeReason(const std::string& payload) {
    if (payload.size() < 2) {
        return payload.empty() ? "websocket closed by server" : "websocket closed by server: " + payload;
    }
    const int code = (static_cast<unsigned char>(payload[0]) << 8) |
                     static_cast<unsigned char>(payload[1]);
    const std::string text = payload.substr(2);
    std::string message = "websocket closed by server: " + std::to_string(code);
    if (!text.empty()) {
        message += " " + text;
    }
    return message;
}

}  // namespace

ProbeReport runRealtimeProbe(const TransportBinding& transport, const RealtimeListener& listener,
                             const ProbeConfig& config, int64_t maxDurationMs) {
    ProbeReport report;
    ParsedUrl url;
    std::string urlError;
    if (!parseUrl(config.realtimeUrl, url, urlError)) {
        report.error = "invalid realtime URL: " + urlError;
        return report;
    }

    const std::string key = generateWebSocketKey();
    const std::string origin = url.scheme == "wss" ? "https://" + url.host : "http://" + url.host;
    std::string path = url.path;
    if (path.find('?') == std::string::npos) {
        path += "?model=" + config.realtimeModel;
    }
    const std::string handshake =
        buildHandshakeRequest(url.hostHeader(), path, config.apiKey, origin, key);

    const int64_t startUs = nowMicros();
    if (!transport.writeAll(handshake.data(), handshake.size())) {
        report.error = "failed to send websocket handshake";
        return report;
    }

    std::string headBuffer;
    std::vector<char> buffer(kReadBufferSize);
    const int64_t handshakeDeadline = nowMicros() + 15000LL * 1000;
    while (headBuffer.find("\r\n\r\n") == std::string::npos) {
        if (nowMicros() > handshakeDeadline) {
            report.error = "websocket handshake timeout";
            return report;
        }
        const int count = transport.read(buffer.data(), static_cast<int>(buffer.size()));
        if (count == 0) {
            continue;
        }
        if (count < 0) {
            report.error = "websocket handshake closed by peer";
            report.bodyPreview = shorten(headBuffer, kPreviewLimit);
            report.responseBody = headBuffer;
            return report;
        }
        headBuffer.append(buffer.data(), static_cast<size_t>(count));
        if (headBuffer.size() > 64 * 1024) {
            report.error = "websocket handshake response too large";
            return report;
        }
    }

    const size_t headEnd = headBuffer.find("\r\n\r\n") + 4;
    const std::string head = headBuffer.substr(0, headEnd);
    std::string acceptError;
    if (!validateHandshakeResponse(head, key, acceptError)) {
        std::string body = headBuffer.substr(headEnd);
        if (body.empty()) {
            const int extra = transport.read(buffer.data(), static_cast<int>(buffer.size()));
            if (extra > 0) {
                body.assign(buffer.data(), static_cast<size_t>(extra));
                headBuffer.append(body);
            }
        }
        report.error = acceptError;
        if (!body.empty()) {
            report.error += " " + shorten(body, 240);
        }
        report.bodyPreview = shorten(body.empty() ? head : body, kPreviewLimit);
        report.responseBody = headBuffer;
        report.bodyBytes = body.size();
        report.statusCode = statusCodeFromHead(head);
        report.headMs = static_cast<double>(nowMicros() - startUs) / 1000.0;
        return report;
    }

    report.headMs = static_cast<double>(nowMicros() - startUs) / 1000.0;
    report.statusCode = 101;
    __android_log_print(ANDROID_LOG_INFO, "VoiceLatencyNative",
                        "realtime handshake ok head=%.1fms leftover=%zu",
                        report.headMs, headBuffer.size() - headEnd);

    WsFrameParser parser;
    const std::string leftover = headBuffer.substr(headEnd);
    if (!leftover.empty()) {
        parser.feed(leftover.data(), leftover.size());
    }

    ProbeConfig sessionConfig = config;
    if (sessionConfig.sampleRate <= 0) {
        sessionConfig.sampleRate = 24000;
    }
    const int pcmRate = sessionConfig.sampleRate;
    JsonValue sessionUpdate;
    std::string sessionError;
    if (!JsonValue::parse(buildRealtimeSessionUpdate(sessionConfig), sessionUpdate, sessionError)
            || !sessionUpdate.isObject()) {
        report.error = "failed to build session.update";
        return report;
    }

    auto writeTextEvent = [&transport](const JsonValue& event) {
        const std::string payload = event.dump();
        const std::string frame = buildClientFrame(WsOpcode::Text, payload);
        return transport.writeAll(frame.data(), frame.size());
    };
    if (!writeTextEvent(sessionUpdate)) {
        report.error = "failed to send session.update";
        return report;
    }

    auto shouldStop = [&listener]() {
        if (!listener.valid() || listener.shouldStopMethod == nullptr) {
            return false;
        }
        const jboolean stop = listener.env->CallBooleanMethod(listener.handle,
                                                             listener.shouldStopMethod);
        if (listener.env->ExceptionCheck()) {
            listener.env->ExceptionClear();
            return false;
        }
        return stop == JNI_TRUE;
    };

    auto notifyEvent = [&listener](const std::string& type, const std::string& payload) {
        if (type.empty() || !listener.valid() || listener.onEventMethod == nullptr) {
            return;
        }
        jstring eventType = listener.env->NewStringUTF(type.c_str());
        jstring eventBody = listener.env->NewStringUTF(shorten(payload, 2048).c_str());
        listener.env->CallVoidMethod(listener.handle, listener.onEventMethod, eventType,
                                     eventBody);
        listener.env->DeleteLocalRef(eventType);
        listener.env->DeleteLocalRef(eventBody);
        if (listener.env->ExceptionCheck()) {
            listener.env->ExceptionClear();
        }
    };

    auto deliverAudio = [&listener](const std::string& pcm) {
        if (pcm.empty() || !listener.valid() || listener.onAudioMethod == nullptr) {
            return;
        }
        jbyteArray audioBytes = listener.env->NewByteArray(static_cast<jsize>(pcm.size()));
        if (audioBytes == nullptr) {
            return;
        }
        listener.env->SetByteArrayRegion(audioBytes, 0, static_cast<jsize>(pcm.size()),
                                         reinterpret_cast<const jbyte*>(pcm.data()));
        listener.env->CallVoidMethod(listener.handle, listener.onAudioMethod, audioBytes);
        listener.env->DeleteLocalRef(audioBytes);
        if (listener.env->ExceptionCheck()) {
            listener.env->ExceptionClear();
        }
    };

    auto setMicPaused = [&listener](bool paused) {
        if (!listener.valid()) {
            return;
        }
        jmethodID method = paused ? listener.onCaptureDoneMethod : listener.onResponseDoneMethod;
        if (method == nullptr) {
            return;
        }
        listener.env->CallVoidMethod(listener.handle, method);
        if (listener.env->ExceptionCheck()) {
            listener.env->ExceptionClear();
        }
    };

    auto appendAudio = [&writeTextEvent, pcmRate](const std::string& pcm) {
        size_t offset = 0;
        while (offset < pcm.size()) {
            const size_t hundredMs = static_cast<size_t>(std::max(pcmRate, 8000)) * 2 / 10;
            size_t count = std::min(std::min(kMaxAppendPcmBytes, hundredMs), pcm.size() - offset);
            if (count > 1 && (count % 2) != 0) {
                --count;
            }
            if (count == 0) {
                break;
            }
            JsonValue append = JsonValue::makeObject();
            append.set("type", JsonValue::makeString("input_audio_buffer.append"));
            append.set("audio", JsonValue::makeString(base64Encode(pcm.substr(offset, count))));
            if (!writeTextEvent(append)) {
                return false;
            }
            offset += count;
        }
        return true;
    };

    const int64_t requestedMs = maxDurationMs > 0
        ? std::min<int64_t>(maxDurationMs, kSessionMaxMs)
        : kSessionMaxMs;
    const int64_t overallDeadline =
        nowMicros() + std::max<int64_t>(10000, requestedMs) * 1000;
    const int64_t readyFallbackUs = startUs + kReadyFallbackMs * 1000;

    bool sawSessionUpdated = false;
    bool primedMic = false;
    bool responseActive = false;
    bool responseCreated = false;
    bool stoppedByUser = false;
    bool serverClosed = false;
    int64_t responseRequestedUs = 0;
    int64_t turnStartUs = 0;
    int64_t firstAudioUs = 0;
    int64_t resumeAtUs = 0;
    int64_t audioPayloadBytes = 0;
    int turns = 0;
    double firstTurnTtfaMs = 0.0;
    std::string currentTranscript;
    std::string streamedTranscript;

    while (!serverClosed && nowMicros() < overallDeadline) {
        if (shouldStop()) {
            stoppedByUser = true;
            break;
        }

        const int64_t now = nowMicros();
        const bool microphoneReady = sawSessionUpdated || now >= readyFallbackUs;
        if (!primedMic && microphoneReady) {
            primedMic = true;
            // Drop audio captured while the socket was still connecting. The
            // gateway uses server VAD and will not answer a client commit.
            setMicPaused(false);
            notifyEvent("local.listening", "{}");
        }
        if (!responseActive && microphoneReady && resumeAtUs > 0 && now >= resumeAtUs) {
            setMicPaused(false);
            notifyEvent("local.listening", "{}");
            resumeAtUs = 0;
        }
        if (!responseActive && microphoneReady && now >= resumeAtUs) {
            for (int i = 0; i < 8; ++i) {
                if (!listener.valid() || listener.readMicMethod == nullptr) {
                    break;
                }
                jbyteArray micChunk = static_cast<jbyteArray>(
                    listener.env->CallObjectMethod(listener.handle, listener.readMicMethod));
                if (listener.env->ExceptionCheck()) {
                    listener.env->ExceptionClear();
                    break;
                }
                if (micChunk == nullptr) {
                    break;
                }
                const jsize length = listener.env->GetArrayLength(micChunk);
                std::string pcm(static_cast<size_t>(length), '\0');
                if (length > 0) {
                    listener.env->GetByteArrayRegion(
                        micChunk, 0, length, reinterpret_cast<jbyte*>(&pcm[0]));
                }
                listener.env->DeleteLocalRef(micChunk);
                if (!appendAudio(pcm)) {
                    report.error = "failed to send microphone audio";
                    break;
                }
            }
            if (!report.error.empty()) {
                break;
            }
        }

        const int count = transport.read(buffer.data(), static_cast<int>(buffer.size()));
        if (count < 0) {
            if (shouldStop()) {
                stoppedByUser = true;
            } else if (responseActive) {
                report.error = "websocket closed before response completed";
            } else {
                report.error = "websocket closed during realtime session";
            }
            break;
        }
        if (count == 0) {
            if (responseRequestedUs > 0 && !responseCreated &&
                nowMicros() - responseRequestedUs > kNoResponseTimeoutMs * 1000) {
                report.error = "realtime server did not respond";
                break;
            }
            if (responseActive && responseCreated &&
                nowMicros() - turnStartUs > kResponseTimeoutMs * 1000) {
                report.error = "realtime response timeout";
                break;
            }
            continue;
        }

        parser.feed(buffer.data(), static_cast<size_t>(count));
        WsFrame frame;
        while (parser.takeFrame(frame)) {
            if (frame.opcode == WsOpcode::Ping) {
                const std::string pong = buildClientFrame(WsOpcode::Pong, frame.payload);
                transport.writeAll(pong.data(), pong.size());
                continue;
            }
            if (frame.opcode == WsOpcode::Pong) {
                continue;
            }
            if (frame.opcode == WsOpcode::Close) {
                if (!responseActive && turns > 0) {
                    serverClosed = true;
                    break;
                }
                report.error = closeReason(frame.payload);
                break;
            }
            if (frame.opcode != WsOpcode::Text) {
                continue;
            }

            JsonValue event;
            std::string parseError;
            if (!JsonValue::parse(frame.payload, event, parseError)) {
                continue;
            }
            const std::string eventType = event["type"].asString();
            if (eventType.empty()) {
                continue;
            }
            notifyEvent(eventType, frame.payload);
            __android_log_print(ANDROID_LOG_INFO, "VoiceLatencyNative",
                                "realtime event=%s", eventType.c_str());

            if (eventType == "session.updated") {
                sawSessionUpdated = true;
            } else if (eventType == "input_audio_buffer.speech_stopped" ||
                       eventType == "input_audio_buffer.committed") {
                if (responseRequestedUs == 0) {
                    responseRequestedUs = nowMicros();
                    turnStartUs = responseRequestedUs;
                }
            } else if (eventType == "response.created") {
                responseCreated = true;
                responseActive = true;
                if (responseRequestedUs == 0) {
                    responseRequestedUs = nowMicros();
                    turnStartUs = responseRequestedUs;
                }
                setMicPaused(true);
            } else if (eventType == "response.output_audio.delta" ||
                       eventType == "response.audio.delta") {
                std::string pcm;
                const std::string delta = event["delta"].asString();
                if (!delta.empty() && base64Decode(delta, pcm) && !pcm.empty()) {
                    if (firstAudioUs == 0) {
                        firstAudioUs = nowMicros();
                        if (turns == 0 && turnStartUs > 0) {
                            firstTurnTtfaMs =
                                static_cast<double>(firstAudioUs - turnStartUs) / 1000.0;
                        }
                    }
                    audioPayloadBytes += static_cast<int64_t>(pcm.size());
                    deliverAudio(pcm);
                }
            } else if (eventType == "response.output_audio_transcript.delta" ||
                       eventType == "response.audio_transcript.delta" ||
                       eventType == "response.text.delta" ||
                       eventType == "response.output_text.delta") {
                const std::string delta = event["delta"].asString();
                if (!delta.empty()) {
                    currentTranscript += delta;
                }
            } else if (eventType == "response.output_audio_transcript.done" ||
                       eventType == "response.audio_transcript.done") {
                const std::string transcript = event["transcript"].asString();
                if (!transcript.empty()) {
                    currentTranscript = transcript;
                }
            } else if (eventType == "response.done" || eventType == "response.completed") {
                const std::string status = event["response"]["status"].asString();
                // server_vad barge-in cancels the current reply and keeps the session open.
                const bool cancelled = status == "cancelled";
                if (!cancelled && (status == "incomplete" || status == "failed")) {
                    report.error = "realtime response " + status;
                }
                if (!currentTranscript.empty()) {
                    if (!streamedTranscript.empty()) {
                        streamedTranscript += "\n";
                    }
                    streamedTranscript += currentTranscript;
                }
                currentTranscript.clear();
                if (!cancelled || firstAudioUs > 0) {
                    ++turns;
                }
                responseActive = false;
                responseCreated = false;
                responseRequestedUs = 0;
                firstAudioUs = 0;
                // Keep the microphone paused through the short cooldown so the
                // tail of the assistant audio is not captured into the next turn.
                setMicPaused(true);
                resumeAtUs = nowMicros() + kResumeDelayMs * 1000;
            } else if (eventType == "error") {
                report.error = "realtime error: " + frame.payload;
                report.responseBody = frame.payload;
            }
            if (!report.error.empty()) {
                break;
            }
        }

        if (!report.error.empty()) {
            break;
        }
        if (serverClosed) {
            break;
        }
        if (responseRequestedUs > 0 && !responseCreated &&
            nowMicros() - responseRequestedUs > kNoResponseTimeoutMs * 1000) {
            report.error = "realtime server did not respond";
            break;
        }
        if (responseActive && responseCreated &&
            nowMicros() - turnStartUs > kResponseTimeoutMs * 1000) {
            report.error = "realtime response timeout";
            break;
        }
    }

    setMicPaused(true);
    if (responseActive) {
        JsonValue cancel = JsonValue::makeObject();
        cancel.set("type", JsonValue::makeString("response.cancel"));
        writeTextEvent(cancel);
    }
    const std::string closeFrame = buildClientFrame(WsOpcode::Close, std::string());
    transport.writeAll(closeFrame.data(), closeFrame.size());

    if (stoppedByUser && turns == 0) {
        report.error = "cancelled by user";
    } else if (turns == 0 && report.error.empty()) {
        report.error = "no speech detected before realtime session timeout";
    }
    report.totalMs = static_cast<double>(nowMicros() - startUs) / 1000.0;
    report.bodyBytes = static_cast<size_t>(audioPayloadBytes);
    report.contentType = "audio/pcm;rate=" + std::to_string(pcmRate);
    report.ttfbMs = report.headMs;
    report.firstAudioMs = firstTurnTtfaMs;
    report.responseStartMs = firstTurnTtfaMs;
    report.turns = turns;
    report.transcript = streamedTranscript;
    report.ok = report.error.empty();
    if (audioPayloadBytes > 0) {
        report.audioDurationMs =
            static_cast<double>(audioPayloadBytes) / (pcmRate * 2.0) * 1000.0;
    }
    return report;
}

}  // namespace vcore
