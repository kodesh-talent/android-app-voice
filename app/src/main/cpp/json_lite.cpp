#include "json_lite.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vcore {
namespace {

const JsonValue& nullValue() {
    static const JsonValue kNull;
    return kNull;
}

void appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool parseHex4(const std::string& text, size_t offset, unsigned int& out) {
    if (offset + 4 > text.size()) {
        return false;
    }
    unsigned int value = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = text[offset + i];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<unsigned int>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<unsigned int>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<unsigned int>(c - 'A' + 10);
        } else {
            return false;
        }
    }
    out = value;
    return true;
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool run(JsonValue& out, std::string& error) {
        skipWhitespace();
        if (!parseValue(out, 0)) {
            error = error_.empty() ? "malformed JSON" : error_;
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            error = "trailing data after JSON value";
            return false;
        }
        return true;
    }

private:
    bool fail(const std::string& message) {
        if (error_.empty()) {
            error_ = message + " at offset " + std::to_string(pos_);
        }
        return false;
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool expect(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return fail(std::string("expected '") + c + "'");
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > 64) {
            return fail("nesting too deep");
        }
        skipWhitespace();
        if (pos_ >= text_.size()) {
            return fail("unexpected end of input");
        }
        const char c = text_[pos_];
        if (c == '{') return parseObject(out, depth);
        if (c == '[') return parseArray(out, depth);
        if (c == '"') {
            std::string value;
            if (!parseString(value)) return false;
            out = JsonValue::makeString(value);
            return true;
        }
        if (c == 't') {
            if (text_.compare(pos_, 4, "true") == 0) {
                pos_ += 4;
                out = JsonValue::makeBool(true);
                return true;
            }
            return fail("invalid literal");
        }
        if (c == 'f') {
            if (text_.compare(pos_, 5, "false") == 0) {
                pos_ += 5;
                out = JsonValue::makeBool(false);
                return true;
            }
            return fail("invalid literal");
        }
        if (c == 'n') {
            if (text_.compare(pos_, 4, "null") == 0) {
                pos_ += 4;
                out = JsonValue();
                return true;
            }
            return fail("invalid literal");
        }
        return parseNumber(out);
    }

    bool parseNumber(JsonValue& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            ++pos_;
        }
        bool digits = false;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
            digits = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
                digits = true;
            }
        }
        if (digits && pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
                ++pos_;
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (!digits) {
            return fail("invalid number");
        }
        const std::string slice = text_.substr(start, pos_ - start);
        out = JsonValue::makeNumber(std::strtod(slice.c_str(), nullptr));
        return true;
    }

    bool parseString(std::string& out) {
        if (!expect('"')) {
            return false;
        }
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                return fail("unterminated escape");
            }
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!parseHex4(text_, pos_, cp)) {
                        return fail("bad unicode escape");
                    }
                    pos_ += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        unsigned int low = 0;
                        if (parseHex4(text_, pos_ + 2, low) && low >= 0xDC00 &&
                            low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            pos_ += 6;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("unknown escape sequence");
            }
        }
        return fail("unterminated string");
    }

    bool parseArray(JsonValue& out, int depth) {
        if (!expect('[')) {
            return false;
        }
        out = JsonValue::makeArray();
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue element;
            if (!parseValue(element, depth + 1)) {
                return false;
            }
            out.push(std::move(element));
            skipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            return expect(']');
        }
    }

    bool parseObject(JsonValue& out, int depth) {
        if (!expect('{')) {
            return false;
        }
        out = JsonValue::makeObject();
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (!expect(':')) {
                return false;
            }
            JsonValue value;
            if (!parseValue(value, depth + 1)) {
                return false;
            }
            out.set(key, std::move(value));
            skipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            return expect('}');
        }
    }

    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;
};

}  // namespace

JsonValue JsonValue::makeObject() {
    JsonValue value;
    value.type_ = Type::Object;
    return value;
}

JsonValue JsonValue::makeArray() {
    JsonValue value;
    value.type_ = Type::Array;
    return value;
}

JsonValue JsonValue::makeString(const std::string& text) {
    JsonValue value;
    value.type_ = Type::String;
    value.string_ = text;
    return value;
}

JsonValue JsonValue::makeNumber(double number) {
    JsonValue value;
    value.type_ = Type::Number;
    value.number_ = number;
    return value;
}

JsonValue JsonValue::makeBool(bool flag) {
    JsonValue value;
    value.type_ = Type::Bool;
    value.bool_ = flag;
    return value;
}


const JsonValue& JsonValue::operator[](const std::string& key) const {
    if (type_ != Type::Object) {
        return nullValue();
    }
    for (const auto& entry : object_) {
        if (entry.first == key) {
            return entry.second;
        }
    }
    return nullValue();
}

bool JsonValue::has(const std::string& key) const {
    if (type_ != Type::Object) {
        return false;
    }
    for (const auto& entry : object_) {
        if (entry.first == key) {
            return true;
        }
    }
    return false;
}

size_t JsonValue::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

const JsonValue& JsonValue::at(size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) {
        return nullValue();
    }
    return array_[index];
}

std::string JsonValue::asString(const std::string& fallback) const {
    return type_ == Type::String ? string_ : fallback;
}

double JsonValue::asNumber(double fallback) const {
    return type_ == Type::Number ? number_ : fallback;
}

bool JsonValue::asBool(bool fallback) const {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return number_ != 0.0;
    return fallback;
}

void JsonValue::set(const std::string& key, JsonValue value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
    }
    for (auto& entry : object_) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    object_.emplace_back(key, std::move(value));
}

void JsonValue::push(JsonValue value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
    }
    array_.push_back(std::move(value));
}

std::string JsonValue::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

void JsonValue::dumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number: {
            char buffer[48];
            if (std::fabs(number_ - std::llround(number_)) < 1e-9 &&
                std::fabs(number_) < 1e15) {
                std::snprintf(buffer, sizeof(buffer), "%lld",
                              static_cast<long long>(std::llround(number_)));
            } else {
                std::snprintf(buffer, sizeof(buffer), "%.6f", number_);
            }
            out += buffer;
            break;
        }
        case Type::String:
            out.push_back('"');
            out += jsonEscape(string_);
            out.push_back('"');
            break;
        case Type::Array: {
            out.push_back('[');
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i != 0) out.push_back(',');
                array_[i].dumpTo(out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& entry : object_) {
                if (!first) out.push_back(',');
                first = false;
                out.push_back('"');
                out += jsonEscape(entry.first);
                out += "\":";
                entry.second.dumpTo(out);
            }
            out.push_back('}');
            break;
        }
    }
}

bool JsonValue::parse(const std::string& text, JsonValue& out, std::string& error) {
    Parser parser(text);
    return parser.run(out, error);
}

std::string jsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string jsonQuote(const std::string& value) {
    return "\"" + jsonEscape(value) + "\"";
}

}  // namespace vcore

