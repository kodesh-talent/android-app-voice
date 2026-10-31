// Minimal JSON value/parser used for gateway request bodies and responses.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace vcore {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }

    static JsonValue makeObject();
    static JsonValue makeArray();
    static JsonValue makeString(const std::string& value);
    static JsonValue makeNumber(double value);
    static JsonValue makeBool(bool value);

    // Object access. Missing keys return a shared null value.
    const JsonValue& operator[](const std::string& key) const;
    bool has(const std::string& key) const;

    // Array access.
    size_t size() const;
    const JsonValue& at(size_t index) const;

    std::string asString(const std::string& fallback = std::string()) const;
    double asNumber(double fallback = 0.0) const;
    bool asBool(bool fallback = false) const;

    void set(const std::string& key, JsonValue value);
    void push(JsonValue value);

    std::string dump() const;

    // Parses |text|. Returns false and fills |error| on malformed input.
    static bool parse(const std::string& text, JsonValue& out, std::string& error);

private:
    void dumpTo(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::vector<std::pair<std::string, JsonValue>> object_;
};

std::string jsonEscape(const std::string& input);
std::string jsonQuote(const std::string& value);

}  // namespace vcore
