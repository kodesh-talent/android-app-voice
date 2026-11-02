package com.xeno.voicelatency;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONObject;

/** Persisted probe configuration (endpoint, key, voice, text). */
public final class ProbeSettings {

    private static final String PREFS = "voice_latency_prefs";

    private static final String KEY_BASE_URL = "base_url";
    private static final String KEY_API_KEY = "api_key";
    private static final String KEY_TEXT = "text";
    private static final String KEY_VOICE = "voice";
    private static final String KEY_LANGUAGE = "language";
    private static final String KEY_OUTPUT_LANGUAGE = "output_language";
    private static final String KEY_ITERATIONS = "iterations";
    private static final String KEY_SIMULATED = "simulated_mode";

    public static final String DEFAULT_BASE_URL = "https://xenoai.eu.cc/v1";
    public static final String ALTERNATE_BASE_URL = "https://us.xenoai.eu.cc/v1";
    public static final String DEFAULT_TEXT = "这是一段用于测试语音延迟的中文示例。";
    public static final String DEFAULT_VOICE = "Ara";
    public static final String DEFAULT_LANGUAGE = "zh";

    /** Previous English defaults, migrated forward on first launch after upgrade. */
    private static final String LEGACY_TEXT = "This is a short voice test.";
    private static final String LEGACY_LANGUAGE = "en";
    /** Bump when the shipped defaults change so stored values can be migrated. */
    private static final int DEFAULTS_VERSION = 2;
    private static final String KEY_DEFAULTS_VERSION = "defaults_version";

    private static final String DEFAULT_KEY =
            "sk-97d0dfeb7145712137c9e9054f90497a5931fabe4d5d1410e94a830a90fb7b23";

    public String baseUrl = DEFAULT_BASE_URL;
    public String apiKey = DEFAULT_KEY;
    public String text = DEFAULT_TEXT;
    public String voice = DEFAULT_VOICE;
    public String language = DEFAULT_LANGUAGE;
    public String outputLanguage = DEFAULT_LANGUAGE;
    public int iterations = 3;
    /** When true, probes run against the local simulator instead of the network. */
    public boolean simulated = false;

    public static ProbeSettings load(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        ProbeSettings settings = new ProbeSettings();
        settings.baseUrl = prefs.getString(KEY_BASE_URL, DEFAULT_BASE_URL);
        settings.apiKey = prefs.getString(KEY_API_KEY, DEFAULT_KEY);
        settings.text = prefs.getString(KEY_TEXT, DEFAULT_TEXT);
        settings.voice = prefs.getString(KEY_VOICE, DEFAULT_VOICE);
        settings.language = prefs.getString(KEY_LANGUAGE, DEFAULT_LANGUAGE);
        settings.outputLanguage = prefs.getString(KEY_OUTPUT_LANGUAGE, DEFAULT_LANGUAGE);
        settings.iterations = prefs.getInt(KEY_ITERATIONS, 3);
        settings.simulated = prefs.getBoolean(KEY_SIMULATED, false);
        migrateDefaults(prefs, settings);
        return settings;
    }

    /**
     * Moves installs that still hold the shipped English defaults onto the Chinese
     * ones. Anything the user typed themselves is left untouched.
     */
    private static void migrateDefaults(SharedPreferences prefs, ProbeSettings settings) {
        if (prefs.getInt(KEY_DEFAULTS_VERSION, 1) >= DEFAULTS_VERSION) {
            return;
        }
        boolean changed = false;
        if (LEGACY_LANGUAGE.equals(settings.language)) {
            settings.language = DEFAULT_LANGUAGE;
            changed = true;
        }
        if (LEGACY_TEXT.equals(settings.text)) {
            settings.text = DEFAULT_TEXT;
            changed = true;
        }
        SharedPreferences.Editor editor = prefs.edit().putInt(KEY_DEFAULTS_VERSION, DEFAULTS_VERSION);
        if (changed) {
            editor.putString(KEY_LANGUAGE, settings.language)
                    .putString(KEY_TEXT, settings.text);
        }
        editor.apply();
    }

    public void save(Context context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit()
                .putString(KEY_BASE_URL, baseUrl)
                .putString(KEY_API_KEY, apiKey)
                .putString(KEY_TEXT, text)
                .putString(KEY_VOICE, voice)
                .putString(KEY_LANGUAGE, language)
                .putString(KEY_OUTPUT_LANGUAGE, outputLanguage)
                .putInt(KEY_ITERATIONS, iterations)
                .putBoolean(KEY_SIMULATED, simulated)
                .apply();
    }

    /** Config handed to the native core. Never logged. */
    public String toNativeConfigJson() {
        return toNativeConfigJson(null, AudioCapture.SAMPLE_RATE);
    }

    /**
     * @param realtimeUrl full websocket URL, or null to derive it from {@link #baseUrl}
     * @param sampleRate PCM rate advertised in session.update
     */
    public String toNativeConfigJson(String realtimeUrl, int sampleRate) {
        JSONObject object = new JSONObject();
        try {
            object.put("baseUrl", baseUrl);
            object.put("apiKey", apiKey);
            object.put("text", text);
            object.put("voice", voice);
            object.put("language", language);
            object.put("outputLanguage", outputLanguage);
            object.put("sampleRate", sampleRate > 0 ? sampleRate : AudioCapture.SAMPLE_RATE);
            object.put("connectTimeoutMs", 15000);
            object.put("readTimeoutMs", 60000);
            if (realtimeUrl != null && !realtimeUrl.isEmpty()) {
                object.put("realtimeUrl", realtimeUrl);
            }
        } catch (org.json.JSONException ignored) {
            // All values are primitives.
        }
        return object.toString();
    }

    /** The other Base URL listed in the gateway document, or null when this host is unknown. */
    public static String alternateBaseUrl(String baseUrl) {
        if (baseUrl == null) {
            return null;
        }
        if (baseUrl.contains("://us.xenoai.eu.cc")) {
            return baseUrl.replace("://us.xenoai.eu.cc", "://xenoai.eu.cc");
        }
        if (baseUrl.contains("://xenoai.eu.cc")) {
            return baseUrl.replace("://xenoai.eu.cc", "://us.xenoai.eu.cc");
        }
        return null;
    }

    /** wss URL for {@code POST}-style Base URL, without the model query. */
    public static String realtimeUrlFor(String baseUrl) {
        String trimmed = baseUrl == null ? "" : baseUrl.trim();
        while (trimmed.endsWith("/")) {
            trimmed = trimmed.substring(0, trimmed.length() - 1);
        }
        if (trimmed.startsWith("https://")) {
            return "wss://" + trimmed.substring("https://".length()) + "/realtime";
        }
        if (trimmed.startsWith("http://")) {
            return "ws://" + trimmed.substring("http://".length()) + "/realtime";
        }
        if (trimmed.startsWith("wss://") || trimmed.startsWith("ws://")) {
            return trimmed.endsWith("/realtime") ? trimmed : trimmed + "/realtime";
        }
        return "wss://" + trimmed + "/realtime";
    }

    /** Masks the key for display. */
    public String maskedKey() {
        if (apiKey == null || apiKey.length() < 12) {
            return apiKey == null || apiKey.isEmpty() ? "未设置" : "••••";
        }
        return apiKey.substring(0, 6) + "…" + apiKey.substring(apiKey.length() - 4);
    }

    public String hostLabel() {
        String url = baseUrl == null ? "" : baseUrl;
        int schemeEnd = url.indexOf("://");
        String rest = schemeEnd >= 0 ? url.substring(schemeEnd + 3) : url;
        int slash = rest.indexOf('/');
        return slash >= 0 ? rest.substring(0, slash) : rest;
    }
}
