// JNI bridge: exposes the native voice probe core to the Java UI layer.
//
// Java owns only platform services (TLS socket, microphone, speaker, UI).
// Request shaping, protocol parsing, audio decoding and latency measurement all
// happen in C++ behind these entry points.
#include <jni.h>

#include <string>
#include <typeinfo>
#include <vector>

#include "base64.h"
#include "http_client.h"
#include "json_lite.h"
#include "latency_stats.h"
#include "synth.h"
#include "voice_core.h"
#include "wav_codec.h"
#include "ws_frame.h"

namespace {

std::string toStd(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return std::string();
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars != nullptr ? std::string(chars) : std::string();
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(value, chars);
    }
    return result;
}

jstring toJava(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

std::string toStd(JNIEnv* env, jbyteArray array) {
    if (array == nullptr) {
        return std::string();
    }
    const jsize length = env->GetArrayLength(array);
    std::string result(static_cast<size_t>(length), '\0');
    if (length > 0) {
        env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(&result[0]));
    }
    return result;
}

jbyteArray toJavaBytes(JNIEnv* env, const std::string& value) {
    jbyteArray array = env->NewByteArray(static_cast<jsize>(value.size()));
    if (array == nullptr) {
        return nullptr;
    }
    if (!value.empty()) {
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(value.size()),
                                reinterpret_cast<const jbyte*>(value.data()));
    }
    return array;
}

// The most recent TTS payload, handed to Java without a second round trip.
std::string& lastTtsAudio() {
    static std::string payload;
    return payload;
}

}  // namespace

extern "C" {

// ------------------------------------------------------------------ metadata

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeVersion(JNIEnv* env, jclass) {
    return toJava(env, "voicecore/1.1.0");
}

JNIEXPORT jlong JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeNowMicros(JNIEnv*, jclass) {
    return static_cast<jlong>(vcore::nowMicros());
}

// ------------------------------------------------------------- configuration

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeDescribeConfig(JNIEnv* env, jclass,
                                                            jstring configJson) {
    const vcore::ProbeConfig config = vcore::ProbeConfig::fromJson(toStd(env, configJson));
    vcore::JsonValue root = vcore::JsonValue::makeObject();
    root.set("baseUrl", vcore::JsonValue::makeString(config.baseUrl));
    root.set("realtimeUrl", vcore::JsonValue::makeString(config.realtimeUrl));
    root.set("ttsModel", vcore::JsonValue::makeString(config.ttsModel));
    root.set("sttModel", vcore::JsonValue::makeString(config.sttModel));
    root.set("realtimeModel", vcore::JsonValue::makeString(config.realtimeModel));
    root.set("voice", vcore::JsonValue::makeString(config.voice));
    root.set("language", vcore::JsonValue::makeString(config.language));
    root.set("sampleRate", vcore::JsonValue::makeNumber(config.sampleRate));
    return toJava(env, root.dump());
}

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeParseUrl(JNIEnv* env, jclass, jstring url) {
    vcore::ParsedUrl parsed;
    std::string error;
    const bool ok = vcore::parseUrl(toStd(env, url), parsed, error);
    vcore::JsonValue root = vcore::JsonValue::makeObject();
    root.set("ok", vcore::JsonValue::makeBool(ok));
    root.set("error", vcore::JsonValue::makeString(error));
    root.set("scheme", vcore::JsonValue::makeString(parsed.scheme));
    root.set("host", vcore::JsonValue::makeString(parsed.host));
    root.set("port", vcore::JsonValue::makeNumber(parsed.port));
    root.set("path", vcore::JsonValue::makeString(parsed.path));
    return toJava(env, root.dump());
}

// -------------------------------------------------------------------- probes

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeRunTtsProbe(JNIEnv* env, jclass, jobject transport,
                                                         jstring configJson) {
    vcore::TransportBinding binding;
    binding.env = env;
    binding.handle = transport;
    if (transport != nullptr) {
        jclass cls = env->GetObjectClass(transport);
        binding.readMethod = env->GetMethodID(cls, "read", "([B)I");
        binding.writeMethod = env->GetMethodID(cls, "write", "([BI)V");
        binding.closeMethod = env->GetMethodID(cls, "close", "()V");
    }
    const vcore::ProbeConfig config = vcore::ProbeConfig::fromJson(toStd(env, configJson));
    std::string audio;
    vcore::ProbeReport report;
    try {
        report = vcore::runTtsProbe(binding, config, &audio);
    } catch (const std::exception& e) {
        report.error = e.what();
        report.exceptionType = typeid(e).name();
    } catch (...) {
        report.error = "unknown native exception";
        report.exceptionType = "unknown";
    }
    lastTtsAudio() = audio;
    vcore::JsonValue root;
    std::string error;
    vcore::JsonValue::parse(report.toJson(), root, error);
    root.set("audioBytes", vcore::JsonValue::makeNumber(static_cast<double>(audio.size())));
    return toJava(env, root.dump());
}

JNIEXPORT jbyteArray JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeTakeTtsAudio(JNIEnv* env, jclass) {
    std::string payload;
    payload.swap(lastTtsAudio());
    return toJavaBytes(env, payload);
}

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeRunSttProbe(JNIEnv* env, jclass, jobject transport,
                                                         jstring configJson, jbyteArray wav) {
    vcore::TransportBinding binding;
    binding.env = env;
    binding.handle = transport;
    if (transport != nullptr) {
        jclass cls = env->GetObjectClass(transport);
        binding.readMethod = env->GetMethodID(cls, "read", "([B)I");
        binding.writeMethod = env->GetMethodID(cls, "write", "([BI)V");
        binding.closeMethod = env->GetMethodID(cls, "close", "()V");
    }
    const vcore::ProbeConfig config = vcore::ProbeConfig::fromJson(toStd(env, configJson));
    const std::string wavBytes = toStd(env, wav);
    vcore::ProbeReport report;
    try {
        report = vcore::runSttProbe(binding, config, wavBytes);
    } catch (const std::exception& e) {
        report.error = e.what();
        report.exceptionType = typeid(e).name();
    } catch (...) {
        report.error = "unknown native exception";
        report.exceptionType = "unknown";
    }
    return toJava(env, report.toJson());
}

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeRunRealtimeProbe(JNIEnv* env, jclass,
                                                              jobject transport, jobject listener,
                                                              jstring configJson,
                                                              jlong maxDurationMs) {
    vcore::TransportBinding binding;
    binding.env = env;
    binding.handle = transport;
    if (transport != nullptr) {
        jclass cls = env->GetObjectClass(transport);
        binding.readMethod = env->GetMethodID(cls, "read", "([B)I");
        binding.writeMethod = env->GetMethodID(cls, "write", "([BI)V");
        binding.closeMethod = env->GetMethodID(cls, "close", "()V");
    }

    vcore::RealtimeListener sink;
    sink.env = env;
    sink.handle = listener;
    if (listener != nullptr) {
        jclass cls = env->GetObjectClass(listener);
        sink.readMicMethod = env->GetMethodID(cls, "readMicAudio", "()[B");
        sink.onAudioMethod = env->GetMethodID(cls, "onAudio", "([B)V");
        sink.onEventMethod = env->GetMethodID(cls, "onEvent", "(Ljava/lang/String;Ljava/lang/String;)V");
        sink.onErrorMethod = env->GetMethodID(cls, "onError", "(Ljava/lang/String;)V");
        sink.onCaptureDoneMethod = env->GetMethodID(cls, "onCaptureDone", "()V");
        sink.onResponseDoneMethod = env->GetMethodID(cls, "onResponseDone", "()V");
        sink.shouldStopMethod = env->GetMethodID(cls, "shouldStop", "()Z");
    }

    const vcore::ProbeConfig config = vcore::ProbeConfig::fromJson(toStd(env, configJson));
    vcore::ProbeReport report;
    try {
        report = vcore::runRealtimeProbe(binding, sink, config,
                                         static_cast<int64_t>(maxDurationMs));
    } catch (const std::exception& e) {
        report.error = e.what();
        report.exceptionType = typeid(e).name();
    } catch (...) {
        report.error = "unknown native exception";
        report.exceptionType = "unknown";
    }
    return toJava(env, report.toJson());
}

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeRunLocalProbe(JNIEnv* env, jclass,
                                                           jstring configJson) {
    const vcore::ProbeConfig config = vcore::ProbeConfig::fromJson(toStd(env, configJson));
    return toJava(env, vcore::runLocalProbe(config, nullptr).toJson());
}

// ------------------------------------------------------------------- audio io

JNIEXPORT jbyteArray JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeSynthesizeWav(JNIEnv* env, jclass, jstring text,
                                                          jint sampleRate) {
    return toJavaBytes(env, vcore::synthesizeWav(toStd(env, text), sampleRate));
}

JNIEXPORT jbyteArray JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeEncodeWav(JNIEnv* env, jclass, jbyteArray pcm,
                                                       jint sampleRate, jint channels) {
    const std::string raw = toStd(env, pcm);
    return toJavaBytes(env, vcore::encodePcm16Wav(
                                    reinterpret_cast<const uint8_t*>(raw.data()), raw.size(),
                                    sampleRate, channels));
}

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeInspectWav(JNIEnv* env, jclass, jbyteArray wav) {
    const std::string bytes = toStd(env, wav);
    vcore::WavInfo info;
    std::string error;
    vcore::parseWav(bytes, info, error);
    vcore::JsonValue root = vcore::JsonValue::makeObject();
    root.set("ok", vcore::JsonValue::makeBool(error.empty()));
    root.set("error", vcore::JsonValue::makeString(error));
    root.set("sampleRate", vcore::JsonValue::makeNumber(info.sampleRate));
    root.set("channels", vcore::JsonValue::makeNumber(info.channels));
    root.set("bitsPerSample", vcore::JsonValue::makeNumber(info.bitsPerSample));
    root.set("dataOffset", vcore::JsonValue::makeNumber(static_cast<double>(info.dataOffset)));
    root.set("dataBytes", vcore::JsonValue::makeNumber(static_cast<double>(info.dataBytes)));
    return toJava(env, root.dump());
}

JNIEXPORT jdouble JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativePeakAmplitude(JNIEnv* env, jclass, jbyteArray pcm) {
    const std::string raw = toStd(env, pcm);
    return vcore::peakAmplitude(reinterpret_cast<const int16_t*>(raw.data()), raw.size() / 2);
}

JNIEXPORT jdouble JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeRmsLevel(JNIEnv* env, jclass, jbyteArray pcm) {
    const std::string raw = toStd(env, pcm);
    return vcore::rmsLevel(reinterpret_cast<const int16_t*>(raw.data()), raw.size() / 2);
}

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeBase64Encode(JNIEnv* env, jclass, jbyteArray data) {
    return toJava(env, vcore::base64Encode(toStd(env, data)));
}

JNIEXPORT jbyteArray JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeBase64Decode(JNIEnv* env, jclass, jstring text) {
    std::string decoded;
    if (!vcore::base64Decode(toStd(env, text), decoded)) {
        return nullptr;
    }
    return toJavaBytes(env, decoded);
}

// -------------------------------------------------------------- statistics

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeComputeStats(JNIEnv* env, jclass,
                                                          jdoubleArray samples) {
    std::vector<double> values;
    if (samples != nullptr) {
        const jsize length = env->GetArrayLength(samples);
        values.resize(static_cast<size_t>(length));
        if (length > 0) {
            env->GetDoubleArrayRegion(samples, 0, length, values.data());
        }
    }
    return toJava(env, vcore::computeStats(values));
}

// ------------------------------------------------------------ websocket frames

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeGenerateWebSocketKey(JNIEnv* env, jclass) {
    return toJava(env, vcore::generateWebSocketKey());
}

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeBuildHandshakeRequest(
        JNIEnv* env, jclass, jstring host, jstring path, jstring apiKey, jstring origin,
        jstring key) {
    return toJava(env, vcore::buildHandshakeRequest(toStd(env, host), toStd(env, path),
                                                    toStd(env, apiKey), toStd(env, origin),
                                                    toStd(env, key)));
}

JNIEXPORT jstring JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeValidateHandshake(JNIEnv* env, jclass,
                                                              jstring response,
                                                              jstring key) {
    std::string error;
    const bool ok =
        vcore::validateHandshakeResponse(toStd(env, response), toStd(env, key), error);
    return toJava(env, ok ? std::string() : error);
}

JNIEXPORT jbyteArray JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeBuildTextFrame(JNIEnv* env, jclass,
                                                           jstring payload) {
    return toJavaBytes(env, vcore::buildClientFrame(vcore::WsOpcode::Text, toStd(env, payload)));
}

JNIEXPORT jbyteArray JNICALL
Java_com_xeno_voicelatency_VoiceNative_nativeBuildCloseFrame(JNIEnv* env, jclass) {
    return toJavaBytes(env, vcore::buildClientFrame(vcore::WsOpcode::Close, std::string()));
}

}  // extern "C"
