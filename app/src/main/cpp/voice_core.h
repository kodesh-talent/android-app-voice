// Voice probe orchestration.
//
// The native core drives every network conversation end to end: it frames the
// HTTP request, parses the response (including chunked transfer), decodes audio
// payloads and records high-resolution timestamps. Java supplies raw byte
// transport over TLS plus microphone/speaker access.
#pragma once

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "http_client.h"
#include "latency_stats.h"

namespace vcore {

// Java object providing blocking byte transport:
//   int  read(byte[] buffer)          -> bytes read, -1 on EOF
//   void write(byte[] data, int len)
//   void close()
struct TransportBinding {
    JNIEnv* env = nullptr;
    jobject handle = nullptr;
    jmethodID readMethod = nullptr;
    jmethodID writeMethod = nullptr;
    jmethodID closeMethod = nullptr;

    bool valid() const {
        return env != nullptr && handle != nullptr && readMethod != nullptr &&
               writeMethod != nullptr;
    }
    int read(char* buffer, int capacity) const;
    bool writeAll(const char* data, size_t length) const;
    void close() const;
};

// Java object receiving realtime callbacks:
//   byte[]  readMicAudio()
//   void    onAudio(byte[] pcm16)
//   void    onEvent(String type, String payload)
//   void    onError(String message)
//   void    onCaptureDone()
//   void    onResponseDone()
//   boolean shouldStop()
struct RealtimeListener {
    JNIEnv* env = nullptr;
    jobject handle = nullptr;
    jmethodID readMicMethod = nullptr;
    jmethodID onAudioMethod = nullptr;
    jmethodID onEventMethod = nullptr;
    jmethodID onErrorMethod = nullptr;
    jmethodID onCaptureDoneMethod = nullptr;
    jmethodID onResponseDoneMethod = nullptr;
    jmethodID shouldStopMethod = nullptr;

    bool valid() const {
        return env != nullptr && handle != nullptr && readMicMethod != nullptr &&
               onAudioMethod != nullptr && onEventMethod != nullptr;
    }
};

struct ProbeConfig {
    std::string baseUrl = "https://xenoai.eu.cc/v1";
    std::string apiKey;
    std::string text = "这是一段用于测试语音延迟的中文示例。";
    std::string voice = "Ara";
    std::string language = "zh";
    std::string ttsModel = "grok-voice-tts";
    std::string sttModel = "grok-voice-stt";
    std::string realtimeModel = "grok-voice-realtime";
    std::string realtimeUrl;
    int sampleRate = 16000;
    int connectTimeoutMs = 15000;
    int readTimeoutMs = 60000;

    static ProbeConfig fromJson(const std::string& json);
};

struct ProbeReport {
    bool ok = false;
    int statusCode = 0;
    std::string error;
    std::string contentType;
    size_t bodyBytes = 0;
    double headMs = 0.0;
    double ttfbMs = 0.0;
    double firstAudioMs = 0.0;
    double totalMs = 0.0;
    double audioDurationMs = 0.0;
    // Time from commit/request to the first audio delta. Realtime TTFA uses
    // this instead of measuring from the start of microphone capture.
    double responseStartMs = 0.0;
    // Number of completed realtime turns in this WebSocket session.
    int turns = 0;
    std::string transcript;
    std::string bodyPreview;
    // Complete server response / protocol frame for user-visible diagnostics.
    // Unlike |bodyPreview| this is not truncated; it is only set on failures.
    std::string responseBody;
    // Exception type when the probe failed via a caught C++ exception.
    std::string exceptionType;

    std::string toJson() const;
};

// Builds the TTS JSON body (protocol shaping stays native).
std::string buildTtsBody(const ProbeConfig& config);
// Builds a multipart/form-data STT body; |boundary| receives the boundary.
std::string buildSttMultipart(const ProbeConfig& config, const std::string& wavBytes,
                              std::string& boundary);

// Extracts the "text" field from an STT JSON response.
bool extractTranscript(const std::string& json, std::string& text, std::string& error);

// Runs a synchronous TTS probe. |audioOut| receives the returned audio payload.
ProbeReport runTtsProbe(const TransportBinding& transport, const ProbeConfig& config,
                        std::string* audioOut);

// Runs a synchronous STT probe with the supplied WAV payload.
ProbeReport runSttProbe(const TransportBinding& transport, const ProbeConfig& config,
                        const std::string& wavBytes);

// Drives a Realtime session until the listener stops it, the turn completes, or
// |maxDurationMs| elapses.
ProbeReport runRealtimeProbe(const TransportBinding& transport, const RealtimeListener& listener,
                             const ProbeConfig& config, int64_t maxDurationMs);

// Offline synthesis probe: exercises the audio pipeline with no network.
ProbeReport runLocalProbe(const ProbeConfig& config, std::string* audioOut);

// Aggregates a series of millisecond samples into a statistics JSON object
// (count/avg/min/max/p50/p90/p95/stddev/jitter).
std::string computeStats(const std::vector<double>& samplesMs);

}  // namespace vcore
