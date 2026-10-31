# Default ProGuard rules. The app keeps JNI entry points reachable.
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.xeno.voicelatency.VoiceNative { *; }
-keep class com.xeno.voicelatency.TlsTransport { *; }
