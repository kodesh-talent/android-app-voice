cmake_minimum_required(VERSION 3.22.1)
project(voicelatency LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The native core owns everything protocol-shaped and measurement-related.
# Java supplies only platform services: TLS socket, microphone, speaker.
add_library(voicecore SHARED
        jni_bridge.cpp
        voice_core.cpp
        http_client.cpp
        json_lite.cpp
        base64.cpp
        wav_codec.cpp
        mpeg_audio.cpp
        latency_stats.cpp
        ws_frame.cpp
        client_vad.cpp
        realtime_probe.cpp
        synth.cpp
)

target_include_directories(voicecore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(voicecore
        ${log-lib}
        ${android-lib}
        m
)

target_compile_options(voicecore PRIVATE -Wall -Wextra -Wno-unused-parameter)
