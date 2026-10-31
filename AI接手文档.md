# AI 接手文档 — Voice Latency Lab

> 给下一个接手的 AI：读完这一篇就能继续干活，不需要翻聊天记录。
> 最后更新：2026-10-28

---

## 0. 一句话说清楚

这是一个**安卓语音延迟测试 App**：界面用 Java + XML + Material 3，语音模型调用全部放在 C/C++（`libvoicecore.so`）里。主界面就是测试台，能测 TTS / STT / 实时语音三条链路的延迟。

**当前状态：应用已升级到 1.1.0（本地 VAD 连续实时对话），代码构建通过并在真机安装；但网关当前 Realtime 返回 502，TTS/STT 返回 Sub2API 控制台 HTML，因此真实音频端到端仍需等网关恢复后验收。**

---

## 1. 硬性约束（不要违反）

| 约束 | 说明 |
| --- | --- |
| UI 必须 Java + XML | **不许用 Kotlin，不许用 Jetpack Compose** |
| 语音模型调用必须在 C/C++ | 协议、计时、音频处理都在 native；Java 只做平台服务 |
| 所有界面组件必须 M3 | `com.google.android.material:material:1.14.0`，主题继承 `Theme.Material3.*` |
| 主界面 = 测试界面 | 不要另开测试页 |
| 语言默认中文 | `language = "zh"`，合成文本默认中文示例 |

**架构边界**（这是设计核心，改动时不要破坏）：

```
Java 只负责：TLS socket 字节管道、麦克风、扬声器、UI
C/C++ 负责：HTTP/1.1 分帧、chunked 解码、WebSocket RFC6455、
            SHA-1 握手、JSON、Base64、WAV、MPEG 帧解析、
            单调时钟打点、百分位统计
```

---

## 2. 项目位置与环境

```
项目根目录：C:\Users\kodesh\Desktop\Workspace\android-app-voice
```

### 工具链（都已验证可用）

| 项 | 路径 / 值 |
| --- | --- |
| JDK 21 | `D:\MyDevelopKits\JDK\jdk-21` |
| Gradle | `D:\MyDevelopKits\Gradle\gradle-9.7.1\bin\gradle.bat` |
| Android SDK | `%LOCALAPPDATA%\Android\Sdk` |
| NDK | `%LOCALAPPDATA%\Android\Sdk\ndk\28.2.13676358` |
| adb | `%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe` |
| AGP / Gradle | 9.3.3 / 9.7.1 |
| compileSdk / minSdk / targetSdk | 36 / 26 / 34 |
| CMake / C++ | 3.22.1 / C++17（`c++_shared`） |
| ABI | `arm64-v8a`, `x86_64` |

### 构建命令

```powershell
$env:JAVA_HOME="D:\MyDevelopKits\JDK\jdk-21"
& "D:\MyDevelopKits\Gradle\gradle-9.7.1\bin\gradle.bat" -p . :app:assembleDebug --console=plain --no-daemon
```

**⚠️ 重要陷阱**：Gradle 构建成功时**也会返回 exit code 1**，并打印一行 `JAVA_TOOL_OPTIONS` 相关的 stderr。
**判断成功的唯一标准是输出里有 `BUILD SUCCESSFUL`**，不要看 exit code。

产物：`app\build\outputs\apk\debug\app-debug.apk`

### 测试设备

**当前使用真机调试**：

```
设备序列号：LRYDTOWSUSYT7LMV
型号：PDKT00（OPPO/OPlus）
系统：Android 12（API 31）
屏幕：1080x2400
ABI：arm64-v8a
```

历史截图仍来自雷电模拟器（LDPlayer 14，Android 14，1080x2300，x86_64），代码与截图环境不要混淆。

**⚠️ 设备掉线时**（`device offline` / `error: closed`）先重启 adb：

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb kill-server; Start-Sleep 3; & $adb start-server; Start-Sleep 5; & $adb devices -l
```

安装 + 授权：

```powershell
& $adb -s LRYDTOWSUSYT7LMV install -r app\build\outputs\apk\debug\app-debug.apk
# 该 OPPO 系统会拒绝 pm grant，直接在应用首次录音时于系统弹窗授权
```

---

## 3. 接口现状（最重要的一节）

**网关**：`https://xenoai.eu.cc/v1`（备用 `https://us.xenoai.eu.cc/v1`）
**测试 Key**：`sk-97d0dfeb7145712137c9e9054f90497a5931fabe4d5d1410e94a830a90fb7b23`

### 实测结果（2026-10-28 复测）

| 接口 | 状态 | 实测返回 |
| --- | --- | --- |
| `GET /v1/models` | ✅ 200 | 列出 `grok-voice-tts` / `grok-voice-stt` / `grok-voice-realtime` |
| `POST /v1/chat/completions` | ✅ 200 | 文本正常，余额正常 |
| **`POST /v1/audio/speech`** | ⚠️ 200 | `text/html`，3411 B，Sub2API 控制台页面，非音频 |
| `POST /v1/audio/transcriptions` | ⚠️ 200 | 同样返回 3411 B HTML 控制台页面 |
| `WSS /v1/realtime` | ❌ 502 | `{"error":{"message":"Grok realtime upstream unavailable"}}` |

### 关键结论

1. **网关路由当前不可用**：TTS/STT 返回控制台 HTML，Realtime 上游 502。主域名和备用域名表现一致，属于服务端问题，代码侧无法修复。
2. **502 是握手阶段失败**：App 会正确显示 HTTP 502 与上游错误消息，不会卡死或崩溃。
3. **实时代码已不再固定录音 5 秒**：`client_vad.cpp` 本地检测语音起止，停顿约 650 ms 自动提交；`response.done` 后自动恢复聆听，单次会话上限 120 s。
4. **历史结论（2026-10-24）**：网关曾返回 `audio/mpeg` 与 Realtime 101；恢复后仍需重新验证，不能把历史结果当作当前状态。

### 手工复测命令

```powershell
$key="sk-97d0dfeb7145712137c9e9054f90497a5931fabe4d5d1410e94a830a90fb7b23"
$out="$env:TEMP\vl"; New-Item -ItemType Directory -Force -Path $out | Out-Null

# TTS —— 注意：body 必须写文件，PowerShell 直接传 -d 会吃掉 JSON 里的双引号
$body='{"model":"grok-voice-tts","input":"这是一段中文测试。","voice":"Ara","language":"zh"}'
[System.IO.File]::WriteAllText("$out\req.json",$body,(New-Object System.Text.UTF8Encoding($false)))
& curl.exe -s -o "$out\speech.bin" -w "code=%{http_code} type=%{content_type} size=%{size_download}`n" `
  -X POST "https://xenoai.eu.cc/v1/audio/speech" `
  -H "Authorization: Bearer $key" -H "Content-Type: application/json" `
  --data-binary "@$out\req.json"

# STT
& curl.exe -s -X POST "https://xenoai.eu.cc/v1/audio/transcriptions" `
  -H "Authorization: Bearer $key" -F "model=grok-voice-stt" -F "language=zh" `
  -F "file=@$out\speech.bin;type=audio/mpeg;filename=speech.mp3"

# 实时（看握手状态码）
& curl.exe -s -i -m 25 --http1.1 "https://xenoai.eu.cc/v1/realtime?model=grok-voice-realtime" `
  -H "Authorization: Bearer $key" -H "Connection: Upgrade" -H "Upgrade: websocket" `
  -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=="
```

---

## 4. 代码结构

```
app/src/main/
├── cpp/                          # 原生核心，全部协议与计时
│   ├── CMakeLists.txt
│   ├── jni_bridge.cpp            # JNI 入口（签名固定，见第 5 节）
│   ├── voice_core.cpp/.h         # 探针编排、计时打点、TTS/STT 主流程
│   ├── realtime_probe.cpp        # 实时连续会话状态机（VAD→commit→播放→续听）
│   ├── client_vad.cpp/.h         # 本地 16 kHz PCM16 VAD（预卷/起点/尾静音）
│   ├── http_client.cpp/.h        # HTTP/1.1 分帧、chunked 解码、头解析
│   ├── ws_frame.cpp/.h           # RFC6455 帧、握手构建与校验、SHA-1
│   ├── json_lite.cpp/.h          # JSON 解析/序列化（UTF-8 原样透传）
│   ├── base64.cpp/.h
│   ├── wav_codec.cpp/.h          # WAV 编解码
│   ├── mpeg_audio.cpp/.h         # MP3 帧解析（算时长）★ 2026-10-24 新增
│   ├── latency_stats.cpp/.h      # 单调时钟、百分位统计
│   ├── synth.cpp/.h              # PCM 合成（模拟模式用）
│   └── host_test.cpp             # 独立自测程序（含 VAD），不进 APK
│
├── java/com/xeno/voicelatency/
│   ├── MainActivity.java         # M3 主界面（测试台）
│   ├── ProbeEngine.java          # 调度、批量执行、统计、播放分发
│   ├── ProbeMode.java            # TTS / STT / REALTIME / SIMULATED
│   ├── ProbeResult.java          # 结果模型 + 诊断文案
│   ├── ProbeSettings.java        # 配置持久化（SharedPreferences）
│   ├── ProbeSimulator.java       # 模拟链路
│   ├── TlsTransport.java         # TLS 字节管道（native 通过 JNI 调用）
│   ├── AudioCapture.java         # 麦克风
│   ├── AudioPlayback.java        # 扬声器（PCM→AudioTrack，MP3→MediaPlayer）
│   ├── RealtimeBridge.java       # 实时会话回调桥
│   ├── VoiceNative.java          # JNI 声明
│   ├── HistoryAdapter.java / StatSummary.java / Json.java
│
└── res/
    ├── layout/activity_main.xml  # 主测试界面
    ├── layout/sheet_settings.xml # 设置面板
    ├── layout/item_history.xml
    ├── values/themes.xml         # Theme.VoiceLatency（M3）+ 自定义 attr
    ├── values/attrs.xml          # vlColorPrimary 等（M3 attr 不能跨库引用）
    └── values/strings.xml / colors.xml
```

### 计时口径

| 指标 | 定义 | 打点位置 |
| --- | --- | --- |
| 响应头 `headMs` | 请求发出 → 响应头解析完成 | `HttpStream` 头解析完成 |
| 首字节 `TTFB` | 请求发出 → 首个体字节到达 | `pump()` 首次 `bodySize() > 0` |
| 首音 `TTFA` | TTS：请求发出 → 首个音频字节；实时：`commit + response.create` → 首个音频增量 | `pump()` / Realtime 事件处理 |
| 总耗时 `totalMs` | 请求发出/会话开始 → 流结束或整段实时会话结束 | `pump()` 返回后 / Realtime 循环结束 |

时钟用 `clock_gettime(CLOCK_MONOTONIC)`，不受系统时间调整影响。

---

## 5. JNI 签名（已固定，改动需同步两侧）

```java
// 传输对象必须实现这三个方法
read([B)I   write([BI)V   close()V

// 实时会话监听器
readMicAudio()[B   onAudio([B)V   onEvent(String,String)V
onError(String)V   onCaptureDone()V   onResponseDone()V   shouldStop()Z
```

**其他 JNI 方法**见 `VoiceNative.java`，共 22 个 `native` 声明。

---

## 6. 已验证的测试数据

### TTS（真实接口，2026-10-24）

```
TLS connected in 933 ms
TTS head=992.9 ttfb=992.9 total=2097.2 status=200 type=audio/mpeg bytes=57216
音频时长 3576.0 ms

批次 3 次：TTFA 931.9 / 993.0 / 1048.2 ms
平均 993.0 ms，P95 1048.2 ms，抖动 153.4 ms
```

MP3 播放已接通（`MediaPlayer` + `NuPlayer`），音频能真实出声。
### Realtime（2026-10-28）

真机 `LRYDTOWSUSYT7LMV` 安装 1.1.0 后发起会话，握手返回 `HTTP 502`，UI 显示 `Grok realtime upstream unavailable`。连续对话、首音与多轮转写均**尚未在真实网关上验收**。

代码侧已完成的本地验证：VAD 自测通过；真机 `AudioRecord` 以 16 kHz 单声道 `VOICE_RECOGNITION` 采集正常（3 秒 94720 B）。

---

## 7. 模拟链路（SIMULATED）

真实接口不可用时用来验证完整测量流程。

- **合成部分是真实原生调用**，音频生成 / WAV 封装 / 播放都是真的
- **网络段是建模值**：握手 ≈68 ms、服务端 ≈185 ms、首音 ≈+140 ms
- 典型结果：TTFA ≈ 380–450 ms
- 只能验证流程，**不能替代线上性能评估**

切换方式：主界面「测试模式」选 **模拟**，或设置面板打开「使用模拟链路」。

**逻辑规则**：`ProbeMode.usesNetwork()` 决定走真实还是模拟，`ProbeSettings.simulated` 只是持久化标记。SIMULATED 模式永远不碰网络。

---

## 8. 原生自测（改 native 代码后必跑）

```powershell
$ndk="$env:LOCALAPPDATA\Android\Sdk\ndk\28.2.13676358"
$clang="$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe"
$sysroot="$ndk\toolchains\llvm\prebuilt\windows-x86_64\sysroot"
$cpp="app\src\main\cpp"

& $clang --target=aarch64-linux-android26 --sysroot=$sysroot -std=c++17 -O1 -I $cpp `
  -o hosttest\host_test "$cpp\host_test.cpp" "$cpp\http_client.cpp" "$cpp\json_lite.cpp" `
  "$cpp\base64.cpp" "$cpp\wav_codec.cpp" "$cpp\mpeg_audio.cpp" `
  "$cpp\latency_stats.cpp" "$cpp\ws_frame.cpp" "$cpp\client_vad.cpp" -static-libstdc++

$adb="$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb push hosttest\host_test /data/local/tmp/host_test
& $adb shell chmod 755 /data/local/tmp/host_test
& $adb shell /data/local/tmp/host_test        # 期望 PASS (0 failures)
```

**⚠️ 不能在 Windows 主机上直接跑**：NDK clang 编出来的二进制依赖 `clock_gettime64`，主机跑不了。必须交叉编译后 push 到设备上跑。

覆盖范围：URL 解析、JSON 转义、统计、Base64、WAV 往返、WebSocket 帧与握手、chunked 增量解码、VAD 起点/尾静音/reset。真机必须用 `aarch64` 目标；`x86_64` 二进制无法在 `LRYDTOWSUSYT7LMV` 运行。

---

## 9. 踩过的坑（省你时间）

| 坑 | 解决 |
| --- | --- |
| **Gradle 成功也返回 exit 1** | 只看 `BUILD SUCCESSFUL` |
| **真机频繁 offline** | `adb kill-server` + `start-server`；仍失败则重插 USB/重启无线调试 |
| **PowerShell 传 JSON 给 curl 会吃掉双引号** | body 写文件，用 `--data-binary "@file"` |
| **Java 文件被 Set-Content 写出 BOM** | 编译报「非法字符: '\ufeff'」，用 `UTF8Encoding($false)` 重写 |
| **`com.google.android.material.R.attr.colorPrimary` 不存在** | 在 `attrs.xml` 自定义 `vlColorPrimary` 等 |
| **`Widget.Material3.TextView` 不存在** | 父主题用 `android:Widget.TextView` |
| **uiautomator 点击坐标会变** | 每次点击前重新 dump，按 `resource-id` 算 bounds 中心点，不要硬编码坐标 |
| **批量测试中途会滚屏** | 同上，每次重新取坐标 |
| **另一个 App 抢前台**（`com.example.inkrecognition`） | `am start` 拉回来再 dump |
| **模拟器网络偶发 15s 超时** | `ProbeEngine.connect()` 已内置单次重试 |
| **OPPO 拒绝 `pm grant RECORD_AUDIO`** | 在应用首次录音时用系统弹窗授权，不要依赖 adb 授权 |
| **真机不能跑 x86_64 自测二进制** | 用 `--target=aarch64-linux-android26` 交叉编译 |

---

## 10. 下一步该做什么

### 优先级 1：等待网关恢复

- Realtime 当前 502，TTS/STT 返回控制台 HTML；需要网关侧恢复这三个路由
- 恢复后按 README 第 7 节做真机连续两轮验收，代码侧无需再改

修好后直接跑 App 验证，**不需要改代码**。

### 优先级 2：代码可做的改进

1. **`values-night` 夜间主题**：目前只有日间配色，深色模式下对比度可以更好
2. **结果导出**：`ProbeResult.toJson()` 已经有了，可以加 CSV/JSON 分享
3. **长文本 TTS**：目前合成文本在设置里只能单行，长文本体验可以优化

### 优先级 3：清理

- `shots/` 里是交付截图，可以保留
- `hosttest/` 是自测临时目录，已在 `.gitignore` 里

---

## 11. 交付物

| 文件 | 说明 |
| --- | --- |
| `app\build\outputs\apk\debug\app-debug.apk` | 可安装 APK，约 9.3 MB |
| `README.md` | 面向人的交付说明 |
| `AI接手文档.md` | 本文件，面向 AI |
| `对接文档.md` | 接口规格（只读，不要改） |
| `shots/*.png` | 界面截图 |

---

## 12. 快速自检清单

接手后按顺序确认：

```powershell
# 1. 环境在不在
Test-Path "D:\MyDevelopKits\JDK\jdk-21"
Test-Path "$env:LOCALAPPDATA\Android\Sdk\ndk\28.2.13676358"

# 2. 能不能编译
$env:JAVA_HOME="D:\MyDevelopKits\JDK\jdk-21"
& "D:\MyDevelopKits\Gradle\gradle-9.7.1\bin\gradle.bat" -p . :app:assembleDebug --console=plain --no-daemon
# 看到 BUILD SUCCESSFUL 即成功

# 3. 设备在不在
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" devices -l
# 期望看到 LRYDTOWSUSYT7LMV device

# 4. 网关通不通（先跑第 3 节 curl；当前 Realtime 预期 502）
```

四条都过了，就可以直接干活。
