package com.xeno.voicelatency;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Small, null-tolerant helpers over org.json for the native JSON reports. */
public final class Json {

    private Json() {
    }

    public static JSONObject parseObject(String raw) {
        if (raw == null || raw.trim().isEmpty()) {
            return new JSONObject();
        }
        try {
            return new JSONObject(raw);
        } catch (JSONException e) {
            return new JSONObject();
        }
    }

    public static JSONArray parseArray(String raw) {
        if (raw == null || raw.trim().isEmpty()) {
            return new JSONArray();
        }
        try {
            return new JSONArray(raw);
        } catch (JSONException e) {
            return new JSONArray();
        }
    }

    public static String string(JSONObject object, String key, String fallback) {
        if (object == null || !object.has(key) || object.isNull(key)) {
            return fallback;
        }
        return object.optString(key, fallback);
    }

    public static double number(JSONObject object, String key, double fallback) {
        if (object == null || !object.has(key) || object.isNull(key)) {
            return fallback;
        }
        return object.optDouble(key, fallback);
    }

    public static boolean bool(JSONObject object, String key, boolean fallback) {
        if (object == null || !object.has(key) || object.isNull(key)) {
            return fallback;
        }
        return object.optBoolean(key, fallback);
    }

    public static int intValue(String raw, String key, int fallback) {
        JSONObject object = parseObject(raw);
        if (!object.has(key)) {
            return fallback;
        }
        return object.optInt(key, fallback);
    }
}
