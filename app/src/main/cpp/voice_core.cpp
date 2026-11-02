#include "voice_core.h"

#include <android/log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "base64.h"
#include "json_lite.h"
#include "mpeg_audio.h"
#include "realtime_language.h"
#include "synth.h"
#include "wav_codec.h"
#include "ws_frame.h"

namespace vcore {
namespace {

constexpr size_t kReadBufferSize = 16 * 1024;
constexpr size_t kMaxBodyBytes = 32 * 1024 * 1024;
constexpr size_t kPreviewLimit = 512;

std::string shorten(const std::string& text, size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    return text.substr(0, limit) + "...";
}

bool isAudioContentType(const std::string& contentType) {
    const std::string lowered = [&contentType] {
        std::string copy = contentType;
        for (char& c : copy) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return copy;
    }();
    return lowered.rfind("audio/", 0) == 0 || lowered.find("octet-stream") != std::string::npos;
}

bool isHtmlResponse(const std::string& contentType, const std::string& body) {
    std::string lowered = contentType;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (lowered.find("text/html") != std::string::npos) {
        return true;
    }
    size_t index = 0;
    while (index < body.size() && index < 64 &&
           (body[index] == ' ' || body[index] == '\r' || body[index] == '\n' ||
            body[index] == '\t')) {
        ++index;
    }
    return body.compare(index, 9, "<!doctype") == 0 || body.compare(index, 5, "<html") == 0;
}

double estimateWavDurationMs(const std::string& bytes) {
    WavInfo info;
    std::string error;
    if (!parseWav(bytes, info, error) || info.sampleRate <= 0) {
        return 0.0;
    }
    const double frames = static_cast<double>(info.dataBytes) /
                          (static_cast<double>(info.channels) * 2.0);
    return frames / static_cast<double>(info.sampleRate) * 1000.0;
}

// Duration of a compressed payload. The gateway answers /audio/speech with MP3
// regardless of the requested response_format, so WAV is only one of two cases.
double estimateAudioDurationMs(const std::string& bytes, const std::string& contentType) {
    const double wavMs = estimateWavDurationMs(bytes);
    if (wavMs > 0.0) {
        return wavMs;
    }
    std::string lowered = contentType;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const bool maybeMpeg = lowered.find("mpeg") != std::string::npos ||
                           lowered.find("mp3") != std::string::npos ||
                           looksLikeMpegAudio(bytes);
    if (!maybeMpeg) {
        return 0.0;
    }
    MpegInfo mpeg;
    std::string error;
    if (!parseMpegAudio(bytes, mpeg, error)) {
        return 0.0;
    }
    return mpeg.durationMs;
}

// Reads until |stream| finishes, the peer closes, or the cap is hit.
void pump(HttpStream& stream, const TransportBinding& transport, ProbeReport& report,
          int64_t startUs, int64_t& firstBodyUs, std::string& rawBody,
          bool expectAudio, int readTimeoutMs) {
    std::vector<char> buffer(kReadBufferSize);
    const int64_t deadline = nowMicros() + static_cast<int64_t>(readTimeoutMs) * 1000;
    while (!stream.finished() && stream.error().empty()) {
        if (nowMicros() > deadline) {
            report.error = "read timeout";
            break;
        }
        const int count = transport.read(buffer.data(), static_cast<int>(buffer.size()));
        if (count < 0) {
            stream.notifyEof();
            break;
        }
        if (count == 0) {
            continue;
        }
        const bool headWasComplete = stream.headComplete();
        if (!stream.feed(buffer.data(), static_cast<size_t>(count))) {
            break;
        }
        if (!headWasComplete && stream.headComplete()) {
            report.headMs = static_cast<double>(nowMicros() - startUs) / 1000.0;
        }
        if (firstBodyUs == 0 && stream.bodySize() > 0) {
            firstBodyUs = nowMicros();
            report.ttfbMs = static_cast<double>(firstBodyUs - startUs) / 1000.0;
            if (expectAudio) {
                report.firstAudioMs = report.ttfbMs;
            }
        }
        if (stream.bodySize() > kMaxBodyBytes) {
            report.error = "response body too large";
            break;
        }
    }
    rawBody = stream.takeBody();
}

}  // namespace

int TransportBinding::read(char* buffer, int capacity) const {
    if (!valid()) {
        return -1;
    }
    jbyteArray array = env->NewByteArray(capacity);
    if (array == nullptr) {
        return -1;
    }
    const jint count = env->CallIntMethod(handle, readMethod, array);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        env->DeleteLocalRef(array);
        return -1;
    }
    if (count > 0) {
        env->GetByteArrayRegion(array, 0, count, reinterpret_cast<jbyte*>(buffer));
    }
    env->DeleteLocalRef(array);
    return static_cast<int>(count);
}

bool TransportBinding::writeAll(const char* data, size_t length) const {
    if (!valid() || data == nullptr) {
        return false;
    }
    jbyteArray array = env->NewByteArray(static_cast<jsize>(length));
    if (array == nullptr) {
        return false;
    }
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(length),
                            reinterpret_cast<const jbyte*>(data));
    env->CallVoidMethod(handle, writeMethod, array, static_cast<jint>(length));
    env->DeleteLocalRef(array);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    return true;
}

void TransportBinding::close() const {
    if (env != nullptr && handle != nullptr && closeMethod != nullptr) {
        env->CallVoidMethod(handle, closeMethod);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }
}

ProbeConfig ProbeConfig::fromJson(const std::string& json) {
    ProbeConfig config;
    JsonValue parsed;
    std::string error;
    if (json.empty() || !JsonValue::parse(json, parsed, error) || !parsed.isObject()) {
        return config;
    }
    if (parsed.has("baseUrl")) config.baseUrl = parsed["baseUrl"].asString(config.baseUrl);
    if (parsed.has("apiKey")) config.apiKey = parsed["apiKey"].asString();
    if (parsed.has("text")) config.text = parsed["text"].asString(config.text);
    if (parsed.has("voice")) config.voice = parsed["voice"].asString(config.voice);
    if (parsed.has("language")) config.language = parsed["language"].asString(config.language);
    if (parsed.has("outputLanguage")) {
        config.outputLanguage = parsed["outputLanguage"].asString(config.outputLanguage);
    }
    if (parsed.has("ttsModel")) config.ttsModel = parsed["ttsModel"].asString(config.ttsModel);
    if (parsed.has("sttModel")) config.sttModel = parsed["sttModel"].asString(config.sttModel);
    if (parsed.has("realtimeModel")) {
        config.realtimeModel = parsed["realtimeModel"].asString(config.realtimeModel);
    }
    if (parsed.has("realtimeUrl")) {
        config.realtimeUrl = parsed["realtimeUrl"].asString();
    }
    if (parsed.has("sampleRate")) {
        config.sampleRate = static_cast<int>(parsed["sampleRate"].asNumber(config.sampleRate));
    }
    if (parsed.has("connectTimeoutMs")) {
        config.connectTimeoutMs =
            static_cast<int>(parsed["connectTimeoutMs"].asNumber(config.connectTimeoutMs));
    }
    if (parsed.has("readTimeoutMs")) {
        config.readTimeoutMs =
            static_cast<int>(parsed["readTimeoutMs"].asNumber(config.readTimeoutMs));
    }
    if (config.realtimeUrl.empty()) {
        // Derive wss://host/v1/realtime from the base URL.
        ParsedUrl base;
        std::string urlError;
        if (parseUrl(config.baseUrl, base, urlError)) {
            const std::string scheme = base.scheme == "https" ? "wss" : "ws";
            config.realtimeUrl = scheme + "://" + base.hostHeader() + base.path + "/realtime";
        }
    }
    return config;
}

std::string ProbeReport::toJson() const {
    JsonValue root = JsonValue::makeObject();
    root.set("ok", JsonValue::makeBool(ok));
    root.set("statusCode", JsonValue::makeNumber(statusCode));
    root.set("error", JsonValue::makeString(error));
    root.set("contentType", JsonValue::makeString(contentType));
    root.set("bodyBytes", JsonValue::makeNumber(static_cast<double>(bodyBytes)));
    root.set("headMs", JsonValue::makeNumber(headMs));
    root.set("ttfbMs", JsonValue::makeNumber(ttfbMs));
    root.set("firstAudioMs", JsonValue::makeNumber(firstAudioMs));
    root.set("totalMs", JsonValue::makeNumber(totalMs));
    root.set("audioDurationMs", JsonValue::makeNumber(audioDurationMs));
    root.set("responseStartMs", JsonValue::makeNumber(responseStartMs));
    root.set("turns", JsonValue::makeNumber(turns));
    root.set("transcript", JsonValue::makeString(transcript));
    root.set("bodyPreview", JsonValue::makeString(bodyPreview));
    root.set("responseBody", JsonValue::makeString(responseBody));
    root.set("exceptionType", JsonValue::makeString(exceptionType));
    return root.dump();
}

std::string buildRealtimeSessionUpdate(const ProbeConfig& config) {
    const int pcmRate = config.sampleRate > 0 ? config.sampleRate : 24000;
    const std::string spoken = config.outputLanguage.empty()
        ? config.language : config.outputLanguage;
    JsonValue sessionUpdate = JsonValue::makeObject();
    sessionUpdate.set("type", JsonValue::makeString("session.update"));
    JsonValue session = JsonValue::makeObject();
    session.set("voice", JsonValue::makeString(config.voice.empty() ? "Ara" : config.voice));
    JsonValue modalities = JsonValue::makeArray();
    modalities.push(JsonValue::makeString("text"));
    modalities.push(JsonValue::makeString("audio"));
    session.set("modalities", std::move(modalities));
    session.set("instructions", JsonValue::makeString(realtimeOutputInstructions(spoken)));
    JsonValue turnDetection = JsonValue::makeObject();
    turnDetection.set("type", JsonValue::makeString("server_vad"));
    session.set("turn_detection", std::move(turnDetection));

    JsonValue audio = JsonValue::makeObject();
    JsonValue input = JsonValue::makeObject();
    JsonValue inputFormat = JsonValue::makeObject();
    inputFormat.set("type", JsonValue::makeString("audio/pcm"));
    inputFormat.set("rate", JsonValue::makeNumber(pcmRate));
    input.set("format", std::move(inputFormat));
    JsonValue transcription = JsonValue::makeObject();
    transcription.set("language_hint", JsonValue::makeString(realtimeLanguageHint(config.language)));
    input.set("transcription", std::move(transcription));
    audio.set("input", std::move(input));

    JsonValue output = JsonValue::makeObject();
    JsonValue outputFormat = JsonValue::makeObject();
    outputFormat.set("type", JsonValue::makeString("audio/pcm"));
    outputFormat.set("rate", JsonValue::makeNumber(pcmRate));
    output.set("format", std::move(outputFormat));
    audio.set("output", std::move(output));
    session.set("audio", std::move(audio));
    sessionUpdate.set("session", std::move(session));
    return sessionUpdate.dump();
}

std::string buildTtsBody(const ProbeConfig& config) {
    JsonValue body = JsonValue::makeObject();
    body.set("model", JsonValue::makeString(config.ttsModel));
    body.set("input", JsonValue::makeString(config.text));
    body.set("voice", JsonValue::makeString(config.voice));
    const std::string spoken = config.outputLanguage.empty()
        ? config.language : config.outputLanguage;
    body.set("language", JsonValue::makeString(spoken));
    return body.dump();
}

std::string buildSttMultipart(const ProbeConfig& config, const std::string& wavBytes,
                              std::string& boundary) {
    boundary = "----VoiceLatencyBoundary" + std::to_string(nowMicros());

    std::string body;
    body.reserve(wavBytes.size() + 512);

    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
    body += config.sttModel + "\r\n";

    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"language\"\r\n\r\n";
    body += config.language + "\r\n";

    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"probe.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body += wavBytes;
    body += "\r\n--" + boundary + "--\r\n";
    return body;
}

bool extractTranscript(const std::string& json, std::string& text, std::string& error) {
    JsonValue parsed;
    if (!JsonValue::parse(json, parsed, error)) {
        return false;
    }
    if (parsed.has("text")) {
        text = parsed["text"].asString();
        return true;
    }
    if (parsed.has("data") && parsed["data"].has("text")) {
        text = parsed["data"]["text"].asString();
        return true;
    }
    if (parsed.has("error")) {
        error = parsed["error"]["message"].asString("upstream error");
        return false;
    }
    error = "response has no text field";
    return false;
}

ProbeReport runTtsProbe(const TransportBinding& transport, const ProbeConfig& config,
                        std::string* audioOut) {
    ProbeReport report;
    ParsedUrl url;
    std::string urlError;
    if (!parseUrl(config.baseUrl + "/audio/speech", url, urlError)) {
        report.error = "invalid base URL: " + urlError;
        return report;
    }

    HttpRequestSpec spec;
    spec.method = "POST";
    spec.path = url.path;
    spec.hostHeader = url.hostHeader();
    spec.apiKey = config.apiKey;
    spec.contentType = "application/json";
    spec.accept = "audio/*";
    spec.body = buildTtsBody(config);

    const std::string request = buildHttpRequest(spec);
    HttpStream stream;
    int64_t firstBodyUs = 0;
    const int64_t startUs = nowMicros();

    if (!transport.writeAll(request.data(), request.size())) {
        report.error = "failed to send request";
        return report;
    }

    std::string rawBody;
    pump(stream, transport, report, startUs, firstBodyUs, rawBody, true, config.readTimeoutMs);

    const int64_t endUs = nowMicros();
    report.totalMs = static_cast<double>(endUs - startUs) / 1000.0;
    report.statusCode = stream.statusCode();
    report.contentType = stream.header("content-type");
    report.bodyBytes = rawBody.size();

    if (report.statusCode != 200) {
        report.ok = false;
        report.bodyPreview = shorten(rawBody, kPreviewLimit);
        report.responseBody = rawBody;
        if (report.error.empty()) {
            report.error = "HTTP " + std::to_string(report.statusCode) + " " +
                           stream.reason();
        }
        return report;
    }
    if (rawBody.empty()) {
        report.error = report.error.empty() ? "empty audio response" : report.error;
        return report;
    }

    // The gateway answers unknown or misrouted paths with its single-page console.
    // A 200 plus HTML is therefore a routing failure, not a successful synthesis.
    if (isHtmlResponse(report.contentType, rawBody)) {
        report.ok = false;
        report.bodyPreview = shorten(rawBody, kPreviewLimit);
        report.responseBody = rawBody;
        if (report.error.empty()) {
            report.error = "gateway returned an HTML page instead of audio";
        }
        return report;
    }

    if (isAudioContentType(report.contentType)) {
        report.audioDurationMs = estimateAudioDurationMs(rawBody, report.contentType);
    }
    if (audioOut != nullptr) {
        *audioOut = rawBody;
    }
    report.ok = report.error.empty();
    if (!report.ok) {
        report.bodyPreview = shorten(rawBody, kPreviewLimit);
    }
    return report;
}

ProbeReport runSttProbe(const TransportBinding& transport, const ProbeConfig& config,
                        const std::string& wavBytes) {
    ProbeReport report;
    ParsedUrl url;
    std::string urlError;
    if (!parseUrl(config.baseUrl + "/audio/transcriptions", url, urlError)) {
        report.error = "invalid base URL: " + urlError;
        return report;
    }

    std::string boundary;
    const std::string body = buildSttMultipart(config, wavBytes, boundary);

    HttpRequestSpec spec;
    spec.method = "POST";
    spec.path = url.path;
    spec.hostHeader = url.hostHeader();
    spec.apiKey = config.apiKey;
    spec.contentType = "multipart/form-data; boundary=" + boundary;
    spec.accept = "application/json";
    spec.body = body;

    const std::string request = buildHttpRequest(spec);
    HttpStream stream;
    int64_t firstBodyUs = 0;
    const int64_t startUs = nowMicros();

    if (!transport.writeAll(request.data(), request.size())) {
        report.error = "failed to send request";
        return report;
    }

    std::string rawBody;
    pump(stream, transport, report, startUs, firstBodyUs, rawBody, false, config.readTimeoutMs);

    const int64_t endUs = nowMicros();
    report.totalMs = static_cast<double>(endUs - startUs) / 1000.0;
    report.statusCode = stream.statusCode();
    report.contentType = stream.header("content-type");
    report.bodyBytes = rawBody.size();
    report.firstAudioMs = report.ttfbMs;  // first transcription byte

    if (report.statusCode != 200) {
        report.ok = false;
        report.bodyPreview = shorten(rawBody, kPreviewLimit);
        report.responseBody = rawBody;
        if (report.error.empty()) {
            report.error = "HTTP " + std::to_string(report.statusCode) + " " +
                           stream.reason();
        }
        return report;
    }

    std::string transcript;
    std::string parseError;
    if (extractTranscript(rawBody, transcript, parseError)) {
        report.transcript = transcript;
    } else if (!parseError.empty()) {
        report.error = parseError;
        report.bodyPreview = shorten(rawBody, kPreviewLimit);
        report.responseBody = rawBody;
        return report;
    }
    report.ok = report.error.empty();
    return report;
}



ProbeReport runLocalProbe(const ProbeConfig& config, std::string* audioOut) {
    ProbeReport report;
    const int64_t startUs = nowMicros();
    const std::string wav = synthesizeWav(config.text, config.sampleRate);
    const int64_t endUs = nowMicros();

    report.ok = !wav.empty();
    report.statusCode = 0;
    report.contentType = "audio/wav";
    report.bodyBytes = wav.size();
    report.ttfbMs = static_cast<double>(endUs - startUs) / 1000.0;
    report.firstAudioMs = report.ttfbMs;
    report.totalMs = report.ttfbMs;
    report.audioDurationMs = estimateWavDurationMs(wav);
    report.headMs = 0.0;
    if (!report.ok) {
        report.error = "local synthesis produced no audio";
    }
    if (audioOut != nullptr) {
        *audioOut = wav;
    }
    return report;
}

std::string computeStats(const std::vector<double>& samplesMs) {
    Rollup rollup;
    for (double value : samplesMs) {
        rollup.add(value);
    }
    return rollup.toJson();
}

}  // namespace vcore

