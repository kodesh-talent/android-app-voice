package com.xeno.voicelatency;

import java.util.ArrayList;
import java.util.List;

/** One probe run, as reported by the native core or the simulator. */
public final class ProbeResult {

    public final boolean ok;
    public final int statusCode;
    public final String error;
    public final String contentType;
    public final long bodyBytes;
    public final double headMs;
    public final double ttfbMs;
    public final double firstAudioMs;
    public final double totalMs;
    public final double audioDurationMs;
    /** Realtime only: commit/request → first audio delta. */
    public final double responseStartMs;
    /** Realtime only: number of completed conversation turns. */
    public final int turns;
    public final String transcript;
    public final String bodyPreview;
    /** Complete server response body / protocol frame, retained for failures. */
    public final String responseBody;
    /** Native exception type, or empty when the failure was protocol-level. */
    public final String exceptionType;
    /** Java exception stack trace, or empty when no Java exception was caught. */
    public final String stackTrace;
    public final ProbeMode mode;
    public final long wallClockMs;

    public ProbeResult(boolean ok, int statusCode, String error, String contentType,
                       long bodyBytes, double headMs, double ttfbMs, double firstAudioMs,
                       double totalMs, double audioDurationMs, String transcript,
                       String bodyPreview, ProbeMode mode) {
        this(ok, statusCode, error, contentType, bodyBytes, headMs, ttfbMs, firstAudioMs,
                totalMs, audioDurationMs, 0, transcript, bodyPreview, "", "", "", mode);
    }

    public ProbeResult(boolean ok, int statusCode, String error, String contentType,
                       long bodyBytes, double headMs, double ttfbMs, double firstAudioMs,
                       double totalMs, double audioDurationMs, double responseStartMs,
                       String transcript, String bodyPreview, String responseBody,
                       String exceptionType, String stackTrace, ProbeMode mode) {
        this(ok, statusCode, error, contentType, bodyBytes, headMs, ttfbMs, firstAudioMs,
                totalMs, audioDurationMs, responseStartMs, transcript, bodyPreview,
                responseBody, exceptionType, stackTrace, 0, mode);
    }

    public ProbeResult(boolean ok, int statusCode, String error, String contentType,
                       long bodyBytes, double headMs, double ttfbMs, double firstAudioMs,
                       double totalMs, double audioDurationMs, double responseStartMs,
                       String transcript, String bodyPreview, String responseBody,
                       String exceptionType, String stackTrace, int turns, ProbeMode mode) {
        this.ok = ok;
        this.statusCode = statusCode;
        this.error = error == null ? "" : error;
        this.contentType = contentType == null ? "" : contentType;
        this.bodyBytes = bodyBytes;
        this.headMs = headMs;
        this.ttfbMs = ttfbMs;
        this.firstAudioMs = firstAudioMs;
        this.totalMs = totalMs;
        this.audioDurationMs = audioDurationMs;
        this.responseStartMs = responseStartMs;
        this.turns = Math.max(0, turns);
        this.transcript = transcript == null ? "" : transcript;
        this.bodyPreview = bodyPreview == null ? "" : bodyPreview;
        this.responseBody = responseBody == null ? "" : responseBody;
        this.exceptionType = exceptionType == null ? "" : exceptionType;
        this.stackTrace = stackTrace == null ? "" : stackTrace;
        this.mode = mode;
        this.wallClockMs = System.currentTimeMillis();
    }

    /** Builds a result from the JSON emitted by the native probe. */
    public static ProbeResult fromJson(String raw, ProbeMode mode) {
        var json = Json.parseObject(raw);
        return new ProbeResult(
                Json.bool(json, "ok", false),
                (int) Json.number(json, "statusCode", 0),
                Json.string(json, "error", ""),
                Json.string(json, "contentType", ""),
                (long) Json.number(json, "bodyBytes", 0),
                Json.number(json, "headMs", 0),
                Json.number(json, "ttfbMs", 0),
                Json.number(json, "firstAudioMs", 0),
                Json.number(json, "totalMs", 0),
                Json.number(json, "audioDurationMs", 0),
                Json.number(json, "responseStartMs", 0),
                Json.string(json, "transcript", ""),
                Json.string(json, "bodyPreview", ""),
                Json.string(json, "responseBody", ""),
                Json.string(json, "exceptionType", ""),
                "",
                (int) Json.number(json, "turns", 0),
                mode);
    }

    /**
     * True when the gateway rejected the call for billing reasons. The documented
     * failure mode is HTTP 403 with {@code INSUFFICIENT_BALANCE}.
     */
    public boolean isInsufficientBalance() {
        final String haystack = (bodyPreview + " " + error).toUpperCase();
        return haystack.contains("INSUFFICIENT_BALANCE")
                || haystack.contains("INSUFFICIENT ACCOUNT BALANCE")
                || (statusCode == 403 && haystack.contains("BALANCE"));
    }

    /** True when the gateway answered with its console HTML instead of audio. */
    public boolean isHtmlResponse() {
        final String type = contentType.toLowerCase(java.util.Locale.US);
        final String body = bodyPreview.trim().toLowerCase(java.util.Locale.US);
        return type.contains("text/html")
                || body.startsWith("<!doctype")
                || body.startsWith("<html");
    }

    /** Message field of the gateway's JSON error envelope, if present. */
    public String gatewayMessage() {
        final String body = bodyPreview.trim();
        if (body.isEmpty()) {
            return "";
        }
        final int marker = body.indexOf("\"message\"");
        if (marker < 0) {
            return "";
        }
        final int colon = body.indexOf(':', marker);
        if (colon < 0) {
            return "";
        }
        final int quote = body.indexOf('"', colon + 1);
        if (quote < 0) {
            return "";
        }
        StringBuilder builder = new StringBuilder();
        for (int i = quote + 1; i < body.length(); i++) {
            char c = body.charAt(i);
            if (c == '\\' && i + 1 < body.length()) {
                builder.append(body.charAt(++i));
                continue;
            }
            if (c == '"') {
                break;
            }
            builder.append(c);
        }
        return builder.toString();
    }

    /** Short diagnosis shown under the primary metric. */
    public String diagnosis() {
        if (ok) {
            return "";
        }
        final String message = gatewayMessage();
        if (isInsufficientBalance()) {
            return "接口返回 403：账户欠费，需充值后才能进行真实链路测试";
        }
        if (isHtmlResponse()) {
            final String expected = mode == ProbeMode.STT ? "转写结果" : "音频";
            return "接口返回 HTML 控制台页面而非" + expected
                    + "：该分组未开通语音路由，请核对 Base URL 与分组权限";
        }
        if (statusCode == 401) {
            return "接口返回 401：API Key 无效或已失效";
        }
        if (statusCode == 404) {
            final String lowered = message.toLowerCase(java.util.Locale.US);
            if (lowered.contains("not available for this group")) {
                final String channel = mode == ProbeMode.STT ? "语音识别（STT）" : "该语音模型";
                return "接口返回 404：" + channel + "未对当前 API Key 所属分组开通"
                        + "（" + message + "）。TTS 已可用，说明余额正常，需在网关后台为分组启用该模型";
            }
            return message.isEmpty()
                    ? "接口返回 404：Base URL、模型别名或分组权限不正确"
                    : "接口返回 404：" + message;
        }
        if (statusCode == 429) {
            return "接口返回 429：触发频率或额度限制";
        }
        if (error.contains("realtime server did not respond")) {
            return "实时会话已建立，但服务端未返回响应。请确认 Realtime 上游可用";
        }
        if (error.contains("realtime response timeout")) {
            return "实时会话已提交音频，但响应超时";
        }
        if (error.contains("realtime response cancelled")) {
            return "实时响应被取消";
        }
        if (error.contains("cancelled by user")) {
            return "测试已停止";
        }
        if (error.contains("realtime response incomplete")) {
            return "实时响应未完整生成";
        }
        if (error.contains("no speech detected")) {
            return "实时会话已建立，但未检测到有效说话。请靠近麦克风后重试";
        }
        if (error.contains("handshake rejected")) {
            if (statusCode == 502 || error.contains("502")) {
                return "实时会话握手被拒：网关上游不可用（502），Realtime 通道当前不可用";
            }
            if (statusCode == 401 || statusCode == 403) {
                return "实时会话握手被拒：鉴权失败，请检查 API Key";
            }
            return "实时会话握手被拒：" + error;
        }
        if (statusCode >= 500) {
            return "接口返回 " + statusCode + "：上游服务不可用"
                    + (message.isEmpty() ? "" : "（" + message + "）");
        }
        if (statusCode == 400 && !message.isEmpty()) {
            return "请求被拒绝：" + message;
        }
        if (error.contains("websocket handshake closed by peer")) {
            return "实时 WebSocket 在完成握手前被关闭。下方为完整响应，"
                    + "可据此判断是网关/代理还是网络中断";
        }
        if (error.contains("websocket closed before response completed")) {
            return "实时 WebSocket 在响应完成前断开。下方保留完整错误与响应内容";
        }
        return error.isEmpty() ? "测试未通过" : error;
    }

    /** Label of the headline metric for this mode. */
    public String primaryMetricLabel() {
        switch (mode) {
            case TTS:
            case REALTIME:
            case SIMULATED:
                return "首音延迟 TTFA";
            case STT:
                return "转写延迟 TTFT";
            default:
                return "合成耗时";
        }
    }

    /** Headline metric in milliseconds. */
    public double primaryMetricMs() {
        switch (mode) {
            case REALTIME:
                return responseStartMs > 0 ? responseStartMs : firstAudioMs;
            case TTS:
            case SIMULATED:
                return firstAudioMs > 0 ? firstAudioMs : ttfbMs;
            case STT:
                return ttfbMs;
            default:
                return totalMs;
        }
    }

    /** Ordered detail rows for the result card. */
    public List<String[]> detailRows() {
        List<String[]> rows = new ArrayList<>();
        if (ok && headMs > 0) {
            rows.add(new String[]{"响应头", format(headMs)});
        }
        if (ok && ttfbMs > 0) {
            rows.add(new String[]{"首字节 TTFB", format(ttfbMs)});
        }
        if (ok && firstAudioMs > 0 && mode != ProbeMode.STT) {
            rows.add(new String[]{
                    mode == ProbeMode.REALTIME ? "提交→首音" : "首音 TTFA",
                    format(mode == ProbeMode.REALTIME && responseStartMs > 0
                            ? responseStartMs : firstAudioMs)});
        }
        if (totalMs > 0) {
            rows.add(new String[]{"总耗时", format(totalMs)});
        }
        if (audioDurationMs > 0) {
            rows.add(new String[]{"音频时长", format(audioDurationMs)});
        }
        if (bodyBytes > 0) {
            rows.add(new String[]{
                    mode == ProbeMode.REALTIME ? "音频数据（流式）" : "音频数据",
                    bodyBytes + " B"});
        }
        if (mode == ProbeMode.REALTIME && turns > 0) {
            rows.add(new String[]{"完成轮次", String.valueOf(turns)});
        }
        if (statusCode > 0) {
            rows.add(new String[]{"HTTP 状态", String.valueOf(statusCode)});
        }
        if (!transcript.isEmpty()) {
            rows.add(new String[]{"转写结果", transcript});
        }
        final String message = gatewayMessage();
        if (!message.isEmpty()) {
            rows.add(new String[]{"服务端消息", message});
        }
        return rows;
    }

    /** Full, selectable diagnostic text. Never truncated or key-bearing. */
    public String diagnosticText() {
        StringBuilder builder = new StringBuilder();
        builder.append("模式: ").append(mode.title).append('\n');
        builder.append("结果: ").append(ok ? "成功" : "失败").append('\n');
        builder.append("时间: ").append(new java.text.SimpleDateFormat(
                "yyyy-MM-dd HH:mm:ss.SSS", java.util.Locale.US).format(
                new java.util.Date(wallClockMs))).append('\n');
        builder.append("HTTP 状态: ").append(statusCode == 0 ? "—" : statusCode).append('\n');
        builder.append("Content-Type: ")
                .append(contentType.isEmpty() ? "—" : contentType).append('\n');
        builder.append("响应字节: ").append(bodyBytes).append('\n');
        builder.append("响应头: ").append(format(headMs)).append('\n');
        builder.append("首字节 TTFB: ").append(format(ttfbMs)).append('\n');
        if (mode == ProbeMode.REALTIME) {
            builder.append("提交→首音: ").append(format(responseStartMs)).append('\n');
            builder.append("完成轮次: ").append(turns).append('\n');
        }
        builder.append("首音 TTFA: ").append(format(firstAudioMs)).append('\n');
        builder.append("总耗时: ").append(format(totalMs)).append('\n');
        builder.append("音频时长: ").append(format(audioDurationMs)).append('\n');
        builder.append("异常类型: ")
                .append(exceptionType.isEmpty() ? "—" : exceptionType).append('\n');
        builder.append("错误消息:\n").append(error.isEmpty() ? "—" : error).append('\n');
        if (!stackTrace.isEmpty()) {
            builder.append("\nJava 堆栈:\n").append(stackTrace).append('\n');
        }
        if (!transcript.isEmpty()) {
            builder.append("\n转写结果:\n").append(transcript).append('\n');
        }
        if (!responseBody.isEmpty()) {
            builder.append("\n服务端完整响应:\n").append(responseBody).append('\n');
        }
        return builder.toString();
    }

    public static String format(double ms) {
        if (ms <= 0) {
            return "—";
        }
        if (ms < 10) {
            return String.format(java.util.Locale.US, "%.2f ms", ms);
        }
        return String.format(java.util.Locale.US, "%.1f ms", ms);
    }

    /** Snapshot used by the history list and export. */
    public org.json.JSONObject toJson() {
        org.json.JSONObject object = new org.json.JSONObject();
        try {
            object.put("mode", mode.id);
            object.put("ok", ok);
            object.put("statusCode", statusCode);
            object.put("error", error);
            object.put("headMs", headMs);
            object.put("ttfbMs", ttfbMs);
            object.put("firstAudioMs", firstAudioMs);
            object.put("totalMs", totalMs);
            object.put("audioDurationMs", audioDurationMs);
            object.put("responseStartMs", responseStartMs);
            object.put("turns", turns);
            object.put("bodyBytes", bodyBytes);
            object.put("responseBody", responseBody);
            object.put("exceptionType", exceptionType);
            object.put("stackTrace", stackTrace);
            object.put("timestampMs", wallClockMs);
        } catch (org.json.JSONException ignored) {
            // Values are primitives; this cannot fail in practice.
        }
        return object;
    }
}
