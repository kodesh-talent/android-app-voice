# Voice Latency Lab — 语音能力延迟测试台

按 `对接文档.md` 实现的 Android 语音延迟测试应用。

- **UI**：Java + XML，全部组件为 Material 3（`com.google.android.material:material:1.14.0`，`Theme.Material3.*`）
- **语音模型调用**：C/C++（`libvoicecore.so`），负责全部协议与计时
- **主界面**：即延迟测试界面

---

## 1. 当前接口状态（重要）

2026-10-28 实测网关 `https://xenoai.eu.cc/v1`：

| 接口 | 实测结果 | 说明 |
| --- | --- | --- |
| `GET /v1/models` | 200 | 三个语音别名均可见 |
| `POST /v1/audio/speech` | 200，`text/html`（3411 B） | 返回 Sub2API 控制台页面，语音路由暂不可用 |
| `POST /v1/audio/transcriptions` | 200，`text/html`（3411 B） | 同样返回控制台页面，暂无法验证转写 |
| `WSS /v1/realtime` | **502 Bad Gateway** | `Grok realtime upstream unavailable` |

实时链路代码已升级为 1.1.0 连续对话：握手 → `session.update` → 本地 VAD 检测语音 → 只上传检测到的 16 kHz PCM16 → 停顿约 650 ms 后 `input_audio_buffer.commit` + `response.create` → `response.output_audio.delta` 边收边播 → `response.done` 后自动继续聆听。当前网关 Realtime 返回 502，尚无真实音频端到端验收结果。

模拟链路仍保留，用于离线或接口异常时验证完整测量流程。

---

## 2. 架构

```
Java（平台能力 + UI）                 C/C++（协议 + 计时 + 音频）
─────────────────────────            ──────────────────────────────
MainActivity         M3 界面          voice_core.cpp     探针编排、计时打点
ProbeEngine         调度/批量/统计     realtime_probe.cpp 连续实时会话状态机
TlsTransport        TLS 字节管道 ───► client_vad.cpp     本地 VAD（预卷/尾静音）
AudioCapture        麦克风 PCM16      http_client.cpp    HTTP/1.1 分帧/chunked
AudioPlayback       扬声器播放         ws_frame.cpp       RFC6455 帧、握手、SHA-1
ProbeSimulator      仿真链路          json_lite.cpp      JSON 解析/序列化
ProbeResult/Settings/StatSummary      base64.cpp         Base64
HistoryAdapter                        wav_codec.cpp      WAV 编解码
                                      mpeg_audio.cpp     MP3 帧解析
                                      synth.cpp          PCM 合成/重采样
                                      latency_stats.cpp  单调时钟、百分位统计
```

**职责边界**：Java 只做「平台服务」（TLS socket、麦克风、扬声器、UI）；所有 HTTP/WebSocket 分帧、chunked 解码、JSON、Base64、WAV、时间戳与统计都在 C++ 中完成。JNI 签名固定，见 `jni_bridge.cpp` 与 `VoiceNative.java`。

### 计时口径

| 指标 | 定义 | 打点位置 |
| --- | --- | --- |
| 响应头 `headMs` | 请求发出 → 响应头解析完成 | `HttpStream` 头解析完成时 |
| 首字节 `TTFB` | 请求发出 → 首个体字节到达 | `pump()` 首次 `bodySize() > 0` |
| 首音 `TTFA` | TTS：请求发出 → 首个音频字节；实时：`commit + response.create` → 首个 `response.output_audio.delta` | `pump()` / Realtime 事件处理 |
| 总耗时 `totalMs` | 请求发出 → 流结束 / `response.done` | `pump()` 返回后 / 实时循环结束 |

时钟使用 `clock_gettime(CLOCK_MONOTONIC)`，不受系统时间调整影响。

---

## 3. 构建与运行

```powershell
$env:JAVA_HOME="D:\MyDevelopKits\JDK\jdk-21"
D:\MyDevelopKits\Gradle\gradle-9.7.1\bin\gradle.bat -p . :app:assembleDebug
```

产物：`app/build/outputs/apk/debug/app-debug.apk`

```powershell
adb install -r app\build\outputs\apk\debug\app-debug.apk
# STT/实时需要录音权限；部分 OPPO/OPlus 系统会拒绝 pm grant，请在系统弹窗中授权
adb shell pm grant com.xeno.voicelatency android.permission.RECORD_AUDIO
```

| 项 | 值 |
| --- | --- |
| AGP / Gradle / JDK | 9.3.3 / 9.7.1 / 21 |
| compileSdk / minSdk / targetSdk | 36 / 26 / 34 |
| NDK / CMake | 28.2.13676358 / 3.22.1 |
| ABI | arm64-v8a、x86_64 |
| C++ 标准 | C++17（`c++_shared`） |

---

## 4. 界面

主界面即测试台，自上而下：

1. **结果卡** — 模式 chip、数据来源 chip（真实接口 / 模拟链路）、主指标大字（TTFA / TTFT）、失败原因、进度条
2. **延迟指标** — 响应头、TTFB、TTFA、总耗时、音频时长、音频大小、HTTP 状态、服务端消息
3. **统计** — 平均 / P95 / 抖动 / 样本数与区间
4. **测试模式** — TTS、STT、实时、模拟（`MaterialButtonToggleGroup`）
5. **最近记录** — 每次运行一条，失败项带原因
6. **设置**（右上角）— Base URL、API Key、合成文本、音色、语言、重复次数、模拟开关

默认值面向中文场景：语言 `zh`、合成文本为中文示例、语言下拉 `zh` 在前。老版本安装升级后会自动迁移（仅当用户没改过这两项时），自己在设置里改过的值不会被覆盖。

全部为 M3 组件：`MaterialToolbar`、`MaterialCardView`、`Chip`、`MaterialButtonToggleGroup`、`LinearProgressIndicator`、`ExtendedFloatingActionButton`、`Slider`、`MaterialSwitch`、`TextInputLayout`、`MaterialAutoCompleteTextView`、`MaterialAlertDialogBuilder`、`Snackbar`、`RecyclerView`。

历史截图（模拟器 API 34，x86_64；当前调试真机为 PDKT00，arm64-v8a）：

| 文件 | 内容 |
| --- | --- |
| [shots/01-idle.png](shots/01-idle.png) | 启动后的测试台 |
| [shots/02-simulated-result.png](shots/02-simulated-result.png) | 模拟链路：TTFA 379 ms、统计与历史 |
| [shots/03-settings.png](shots/03-settings.png) | 设置面板 |
| [shots/04-real-tts-failure.png](shots/04-real-tts-failure.png) | 真实 TTS：HTML 页面诊断 |
| [shots/05-real-stt.png](shots/05-real-stt.png) | 真实 STT：转写结果诊断 |

---

## 5. 失败诊断

`ProbeResult.diagnosis()` 将网关响应翻译为可执行结论：

| 现象 | 提示 |
| --- | --- |
| 200 + HTML | 接口返回 HTML 控制台页面而非音频：该分组未开通语音路由 |
| 403 + `INSUFFICIENT_BALANCE` | 账户欠费，需充值后才能进行真实链路测试 |
| 404 + `not available for this group` | 原样透出服务端消息 |
| 401 | API Key 无效或已失效 |
| 502（WebSocket 握手） | 网关上游不可用，Realtime 通道当前不可用 |
| 连接超时 | 无法连接网关（自动重试一次） |

失败样本**不计入统计**，避免把 0 ms 拉低均值。

---

## 6. 原生核心自测

`app/src/main/cpp/host_test.cpp` 是独立测试程序（不参与 APK 构建），覆盖 URL 解析、JSON 转义、统计、Base64、WAV 往返、WebSocket 帧与握手校验、chunked 增量解码与 VAD：

```powershell
$ndk="C:\Users\kodesh\AppData\Local\Android\Sdk\ndk\28.2.13676358"
$clang="$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe"
$sysroot="$ndk\toolchains\llvm\prebuilt\windows-x86_64\sysroot"
$cpp="app\src\main\cpp"
& $clang --target=aarch64-linux-android26 --sysroot=$sysroot -std=c++17 -O1 -I $cpp `
  -o hosttest\host_test "$cpp\host_test.cpp" "$cpp\http_client.cpp" "$cpp\json_lite.cpp" `
  "$cpp\base64.cpp" "$cpp\wav_codec.cpp" "$cpp\latency_stats.cpp" "$cpp\ws_frame.cpp" `
  "$cpp\client_vad.cpp" -static-libstdc++
adb push hosttest\host_test /data/local/tmp/host_test
adb shell chmod 755 /data/local/tmp/host_test
adb shell /data/local/tmp/host_test            # 可附加一份抓取的原始响应做 chunked 回放
```

当前结果：**PASS（0 failures）**。该测试曾发现并修复一个真实缺陷：`validateHandshakeResponse` 把整个响应转小写后与区分大小写的 `Sec-WebSocket-Accept` 比较，导致任何合法 101 握手都会被判为失败。

---

## 7. 接口恢复后的验证步骤

1. 打开应用 → 右上角设置 → 确认 Base URL 为 `https://xenoai.eu.cc/v1`、填入有效 Key
2. 关闭「使用模拟链路」，测试模式选 **TTS** → 开始测试
3. 预期：主指标显示首音延迟（通常 300–900 ms），下方显示响应头 / TTFB / TTFA / 总耗时 / 音频时长，并听到合成语音
4. **STT**：首次会申请录音权限，录 3 秒后上传，显示转写延迟与转写文本
5. **实时**：建立 WebSocket 会话，说一句后停顿约 0.7 秒；状态应依次为“检测到说话…”→“说话结束，等待回复…”→“助手正在回复…”→“继续聆听…”，随后可直接说第二句

若仍失败，主界面会直接给出对应原因，无需查看日志。

---

## 8. 已知限制

- TTS 返回的 `audio/mpeg` 通过 `MediaPlayer` 播放；WAV / PCM 走 `AudioTrack`。
- 实时输入固定为 16 kHz PCM16；本地 VAD 以约 650 ms 尾静音切分轮次，单段最长 15 s，回复音频边收边播。
- 失败结果卡会显示可长按选择复制的完整诊断（异常类型、消息、堆栈、HTTP 状态、完整服务端响应），并提供“复制全部异常信息”按钮。
- 模拟链路的网络段是建模值（握手 ≈68 ms、服务端 ≈185 ms、首音 ≈+140 ms），合成部分为**真实**原生调用，用于验证流程而非评估线上性能。
- 实时模式麦克风常开，单次 WebSocket 会话上限 120 s；`response.done` 后自动恢复上行，可连续多轮对话。
