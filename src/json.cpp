// JSON 解析与序列化。
#include "json.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <system_error>

namespace deckcalc {
namespace {

constexpr int kMaxDepth = 512;
constexpr char kHexDigits[] = "0123456789abcdef";

bool is_digit(char c) { return c >= '0' && c <= '9'; }

[[noreturn]] void throw_error(size_t offset, const std::string& reason) {
    throw JsonError("json: byte " + std::to_string(offset) + ": " + reason);
}

[[noreturn]] void throw_type_error(const char* context, const char* expected) {
    throw JsonError(std::string("json: ") + context + " requires a " + expected + " value");
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
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

void write_escaped(std::ostream& out, const std::string& text) {
    out.put('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u00" << kHexDigits[c >> 4] << kHexDigits[c & 0x0F];
                } else {
                    out.put(static_cast<char>(c));  // 非 ASCII 字节原样输出
                }
        }
    }
    out.put('"');
}

void write_number(std::ostream& out, double value) {
    if (!std::isfinite(value)) {
        out << "null";
        return;
    }
    if (value == std::trunc(value) && std::fabs(value) < 1e15) {
        out << static_cast<long long>(value);
        return;
    }
    char buffer[64];
    std::to_chars_result result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (result.ec != std::errc()) {
        result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 17);
    }
    if (result.ec == std::errc()) {
        out.write(buffer, result.ptr - buffer);
    } else {
        out << "0";
    }
}

void write_indent(std::ostream& out, int indent, int depth) {
    for (int i = 0; i < indent * depth; ++i) {
        out.put(' ');
    }
}

// from_chars 在上溢/下溢时是否改写 value 由实现决定，故用 strtod 复核。
double to_double(const std::string& literal, size_t offset) {
    double value = 0.0;
    const char* first = literal.data();
    const char* last = first + literal.size();
    std::from_chars_result result =
        std::from_chars(first, last, value, std::chars_format::general);
    if (result.ec == std::errc()) {
        return value;
    }
    if (result.ec != std::errc::result_out_of_range) {
        throw_error(offset, "invalid number");
    }
    errno = 0;
    double fallback = std::strtod(literal.c_str(), nullptr);
    if (!std::isfinite(fallback)) {
        throw_error(offset, "number out of range");
    }
    return fallback;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Json parse_document() {
        skip_bom();
        skip_ws();
        if (pos_ >= text_.size()) {
            throw_error(pos_, "empty input");
        }
        Json value = parse_value(0);
        skip_ws();
        if (pos_ != text_.size()) {
            throw_error(pos_, "trailing garbage");
        }
        return value;
    }

private:
    void skip_bom() {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    Json parse_value(int depth) {
        if (depth > kMaxDepth) {
            throw_error(pos_, "maximum depth exceeded");
        }
        if (pos_ >= text_.size()) {
            throw_error(pos_, "unexpected end of input");
        }
        const char c = text_[pos_];
        switch (c) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': return Json(parse_string());
            case 't': expect_literal("true"); return Json(true);
            case 'f': expect_literal("false"); return Json(false);
            case 'n': expect_literal("null"); return Json(nullptr);
            default:
                if (c == '-' || is_digit(c)) {
                    return parse_number();
                }
                throw_error(pos_, std::string("unexpected character '") + printable(c) + "'");
        }
    }

    static std::string printable(char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7F) {
            return std::string(1, c);
        }
        return "\\x" + std::string(1, kHexDigits[u >> 4]) + std::string(1, kHexDigits[u & 0x0F]);
    }

    void expect_literal(const char* literal) {
        const size_t start = pos_;
        for (size_t i = 0; literal[i] != '\0'; ++i) {
            if (pos_ >= text_.size() || text_[pos_] != literal[i]) {
                throw_error(start, std::string("invalid literal, expected '") + literal + "'");
            }
            ++pos_;
        }
    }

    Json parse_object(int depth) {
        ++pos_;  // '{'
        Json object = Json::object();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return object;
        }
        for (;;) {
            skip_ws();
            if (pos_ >= text_.size()) {
                throw_error(pos_, "unexpected end of input in object");
            }
            if (text_[pos_] != '"') {
                throw_error(pos_, "expected quoted key");
            }
            std::string key = parse_string();
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                throw_error(pos_, "expected ':' after key");
            }
            ++pos_;
            skip_ws();
            object.set(std::move(key), parse_value(depth + 1));
            skip_ws();
            if (pos_ >= text_.size()) {
                throw_error(pos_, "unexpected end of input in object");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                skip_ws();
                if (pos_ < text_.size() && text_[pos_] == '}') {
                    throw_error(pos_, "trailing comma in object");
                }
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return object;
            }
            throw_error(pos_, "expected ',' or '}' in object");
        }
    }

    Json parse_array(int depth) {
        ++pos_;  // '['
        Json array = Json::array();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return array;
        }
        for (;;) {
            skip_ws();
            array.push_back(parse_value(depth + 1));
            skip_ws();
            if (pos_ >= text_.size()) {
                throw_error(pos_, "unexpected end of input in array");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                skip_ws();
                if (pos_ < text_.size() && text_[pos_] == ']') {
                    throw_error(pos_, "trailing comma in array");
                }
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return array;
            }
            throw_error(pos_, "expected ',' or ']' in array");
        }
    }

    std::string parse_string() {
        const size_t open = pos_;
        ++pos_;  // '"'
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) {
                throw_error(open, "unterminated string");
            }
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                ++pos_;
                parse_escape(out);
                continue;
            }
            if (c < 0x20) {
                throw_error(pos_, "control character in string");
            }
            out.push_back(static_cast<char>(c));
            ++pos_;
        }
    }

    void parse_escape(std::string& out) {
        if (pos_ >= text_.size()) {
            throw_error(pos_, "unterminated escape");
        }
        const size_t escape_start = pos_ - 1;
        const char e = text_[pos_++];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = parse_hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        const uint32_t low = parse_hex4();
                        if (low < 0xDC00 || low > 0xDFFF) {
                            throw_error(escape_start, "invalid surrogate pair");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        throw_error(escape_start, "unpaired high surrogate");
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    throw_error(escape_start, "unpaired low surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default:
                throw_error(escape_start, std::string("invalid escape '\\") + printable(e) + "'");
        }
    }

    uint32_t parse_hex4() {
        if (pos_ + 4 > text_.size()) {
            throw_error(pos_, "incomplete \\u escape");
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            value <<= 4;
            if (h >= '0' && h <= '9') {
                value |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                value |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                value |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
                throw_error(pos_ - 1, "invalid hex digit in \\u escape");
            }
        }
        return value;
    }

    Json parse_number() {
        const size_t start = pos_;
        if (text_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= text_.size()) {
            throw_error(start, "incomplete number");
        }
        if (text_[pos_] == '0') {
            ++pos_;
            if (pos_ < text_.size() && is_digit(text_[pos_])) {
                throw_error(pos_, "leading zero in number");
            }
        } else if (is_digit(text_[pos_])) {
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        } else {
            throw_error(pos_, "invalid number");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                throw_error(pos_, "expected digit after '.'");
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                throw_error(pos_, "expected digit in exponent");
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) {
                ++pos_;
            }
        }
        return Json(to_double(std::string(text_.substr(start, pos_ - start)), start));
    }

    std::string_view text_;
    size_t pos_ = 0;
};

std::string read_whole(const std::string& path) {
    if (path == "-") {
        return std::string(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw JsonError("json: cannot open file: " + path);
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw JsonError("json: read failure: " + path);
    }
    return text;
}

}  // namespace

Json Json::object() {
    Json value;
    value.type_ = Type::Object;
    return value;
}

Json Json::array() {
    Json value;
    value.type_ = Type::Array;
    return value;
}

Json Json::parse(std::string_view text) { return Parser(text).parse_document(); }

Json Json::read(const std::string& path) { return parse(read_whole(path)); }

Json Json::parse_file(const std::string& path) { return parse(read_whole(path)); }

bool Json::as_bool() const {
    if (type_ != Type::Bool) {
        throw_type_error("as_bool()", "bool");
    }
    return bool_;
}

double Json::as_double() const {
    if (type_ != Type::Number) {
        throw_type_error("as_double()", "number");
    }
    return number_;
}

int64_t Json::as_int64() const {
    if (type_ != Type::Number) {
        throw_type_error("as_int64()", "number");
    }
    if (!std::isfinite(number_)) {
        throw JsonError("json: as_int64() cannot convert a non-finite number");
    }
    if (number_ != std::trunc(number_)) {
        throw JsonError("json: as_int64() requires an integral value");
    }
    // 双精度存不下 int64 端点，端点值夹紧而不是返回错误结果
    constexpr double kInt64Min = -9223372036854775808.0;  // -2^63
    constexpr double kInt64Max = 9223372036854775808.0;   // 2^63
    if (number_ < kInt64Min || number_ > kInt64Max) {
        throw JsonError("json: as_int64() value is out of int64 range");
    }
    if (number_ >= kInt64Max) {
        return std::numeric_limits<int64_t>::max();
    }
    if (number_ <= kInt64Min) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(number_);
}

const std::string& Json::as_string() const {
    if (type_ != Type::String) {
        throw_type_error("as_string()", "string");
    }
    return string_;
}

const std::vector<Json>& Json::items() const {
    if (type_ != Type::Array) {
        throw_type_error("items()", "array");
    }
    return array_;
}

const std::vector<std::pair<std::string, Json>>& Json::fields() const {
    if (type_ != Type::Object) {
        throw_type_error("fields()", "object");
    }
    return object_;
}

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const auto& field : object_) {
        if (field.first == key) {
            return &field.second;
        }
    }
    return nullptr;
}

const Json& Json::at(std::string_view key) const {
    if (type_ != Type::Object) {
        throw_type_error("at(key)", "object");
    }
    const Json* found = find(key);
    if (found == nullptr) {
        throw JsonError("json: key not found: " + std::string(key));
    }
    return *found;
}

const Json& Json::at(size_t index) const {
    if (type_ != Type::Array) {
        throw_type_error("at(index)", "array");
    }
    if (index >= array_.size()) {
        throw JsonError("json: index out of range: " + std::to_string(index));
    }
    return array_[index];
}

size_t Json::size() const {
    switch (type_) {
        case Type::String: return 1;
        case Type::Array: return array_.size();
        case Type::Object: return object_.size();
        default: return 0;
    }
}

void Json::set(std::string key, Json value) {
    if (type_ != Type::Object) {
        object_.clear();
        type_ = Type::Object;
    }
    for (auto& field : object_) {
        if (field.first == key) {
            field.second = std::move(value);
            return;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
}

void Json::push_back(Json value) {
    if (type_ != Type::Array) {
        array_.clear();
        type_ = Type::Array;
    }
    array_.push_back(std::move(value));
}

std::string Json::dump(int indent) const {
    std::ostringstream out;
    dump_to(out, indent, 0);
    return out.str();
}

void Json::dump_to(std::ostream& out, int indent, int depth) const {
    switch (type_) {
        case Type::Null:
            out << "null";
            return;
        case Type::Bool:
            out << (bool_ ? "true" : "false");
            return;
        case Type::Number:
            write_number(out, number_);
            return;
        case Type::String:
            write_escaped(out, string_);
            return;
        case Type::Array:
            if (array_.empty()) {
                out << "[]";
                return;
            }
            out.put('[');
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i != 0) {
                    out.put(',');
                }
                if (indent > 0) {
                    out.put('\n');
                    write_indent(out, indent, depth + 1);
                }
                array_[i].dump_to(out, indent, depth + 1);
            }
            if (indent > 0) {
                out.put('\n');
                write_indent(out, indent, depth);
            }
            out.put(']');
            return;
        case Type::Object:
            if (object_.empty()) {
                out << "{}";
                return;
            }
            out.put('{');
            for (size_t i = 0; i < object_.size(); ++i) {
                if (i != 0) {
                    out.put(',');
                }
                if (indent > 0) {
                    out.put('\n');
                    write_indent(out, indent, depth + 1);
                }
                write_escaped(out, object_[i].first);
                out << (indent > 0 ? ": " : ":");
                object_[i].second.dump_to(out, indent, depth + 1);
            }
            if (indent > 0) {
                out.put('\n');
                write_indent(out, indent, depth);
            }
            out.put('}');
            return;
    }
}

}  // namespace deckcalc
