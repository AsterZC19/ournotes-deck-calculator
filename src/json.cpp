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

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

[[noreturn]] void throw_error(size_t offset, const std::string &reason) {
    throw JsonError("json: byte " + std::to_string(offset) + ": " + reason);
}

[[noreturn]] void throw_type_error(const char *context, const char *expected) {
    throw JsonError(std::string("json: ") + context + " requires a " + expected + " value");
}

void append_utf8(std::string &out, uint32_t cp) {
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

void write_escaped(std::ostream &out, const std::string &text) {
    out.put('"');
    for (unsigned char c : text) {
        switch (c) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (c < 0x20) {
                out << "\\u00" << kHexDigits[c >> 4] << kHexDigits[c & 0x0F];
            } else {
                out.put(static_cast<char>(c));
            }
        }
    }
    out.put('"');
}

void write_number(std::ostream &out, double value) {
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
        result =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 17);
    }
    if (result.ec == std::errc()) {
        out.write(buffer, result.ptr - buffer);
    } else {
        out << "0";
    }
}

void write_indent(std::ostream &out, int indent, int depth) {
    for (int i = 0; i < indent * depth; ++i) {
        out.put(' ');
    }
}

double to_double(std::string_view literal, size_t offset) {
    double value = 0.0;
    const char *first = literal.data();
    const char *last = first + literal.size();
    std::from_chars_result result = std::from_chars(first, last, value, std::chars_format::general);
    if (result.ec == std::errc()) {
        return value;
    }
    if (result.ec != std::errc::result_out_of_range) {
        throw_error(offset, "invalid number");
    }
    errno = 0;
    const std::string copy(literal);
    double fallback = std::strtod(copy.c_str(), nullptr);
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
        case '{':
            return parse_object(depth);
        case '[':
            return parse_array(depth);
        case '"':
            return Json(parse_string());
        case 't':
            expect_literal("true");
            return Json(true);
        case 'f':
            expect_literal("false");
            return Json(false);
        case 'n':
            expect_literal("null");
            return Json(nullptr);
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

    void expect_literal(const char *literal) {
        const size_t start = pos_;
        for (size_t i = 0; literal[i] != '\0'; ++i) {
            if (pos_ >= text_.size() || text_[pos_] != literal[i]) {
                throw_error(start, std::string("invalid literal, expected '") + literal + "'");
            }
            ++pos_;
        }
    }

    Json parse_object(int depth) {
        ++pos_;
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
        ++pos_;
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
        ++pos_;
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

    void parse_escape(std::string &out) {
        if (pos_ >= text_.size()) {
            throw_error(pos_, "unterminated escape");
        }
        const size_t escape_start = pos_ - 1;
        const char e = text_[pos_++];
        switch (e) {
        case '"':
            out.push_back('"');
            break;
        case '\\':
            out.push_back('\\');
            break;
        case '/':
            out.push_back('/');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
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
        return Json(to_double(text_.substr(start, pos_ - start), start));
    }

    std::string_view text_;
    size_t pos_ = 0;
};

std::string read_whole(const std::string &path) {
    if (path == "-") {
        return std::string(std::istreambuf_iterator<char>(std::cin),
                           std::istreambuf_iterator<char>());
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

}

Json Json::object() {
    return Json(storage_t(std::in_place_type<object_t>));
}

Json Json::array() {
    return Json(storage_t(std::in_place_type<array_t>));
}

Json Json::parse(std::string_view text) {
    return Parser(text).parse_document();
}

Json Json::read(const std::string &path) {
    return parse(read_whole(path));
}

Json Json::parse_file(const std::string &path) {
    return parse(read_whole(path));
}

bool Json::as_bool() const {
    if (!is_bool()) {
        throw_type_error("as_bool()", "bool");
    }
    return std::get<bool>(value_);
}

bool Json::is_integer() const {
    if (!is_number()) {
        return false;
    }
    const double number = std::get<double>(value_);
    return std::isfinite(number) && number == std::trunc(number);
}

double Json::as_double() const {
    if (!is_number()) {
        throw_type_error("as_double()", "number");
    }
    return std::get<double>(value_);
}

int64_t Json::as_int64() const {
    if (!is_number()) {
        throw_type_error("as_int64()", "number");
    }
    const double number = std::get<double>(value_);
    if (!std::isfinite(number)) {
        throw JsonError("json: as_int64() cannot convert a non-finite number");
    }
    if (number != std::trunc(number)) {
        throw JsonError("json: as_int64() requires an integral value");
    }

    constexpr double kInt64Min = -9223372036854775808.0;
    constexpr double kInt64Max = 9223372036854775808.0;
    if (number < kInt64Min || number > kInt64Max) {
        throw JsonError("json: as_int64() value is out of int64 range");
    }
    if (number >= kInt64Max) {
        return std::numeric_limits<int64_t>::max();
    }
    if (number <= kInt64Min) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(number);
}

const std::string &Json::as_string() const {
    if (!is_string()) {
        throw_type_error("as_string()", "string");
    }
    return std::get<std::string>(value_);
}

const std::vector<Json> &Json::items() const {
    if (!is_array()) {
        throw_type_error("items()", "array");
    }
    return std::get<array_t>(value_);
}

const std::vector<std::pair<std::string, Json>> &Json::fields() const {
    if (!is_object()) {
        throw_type_error("fields()", "object");
    }
    return std::get<object_t>(value_);
}

const Json *Json::find(std::string_view key) const {
    if (!is_object()) {
        return nullptr;
    }
    for (const auto &field : std::get<object_t>(value_)) {
        if (field.first == key) {
            return &field.second;
        }
    }
    return nullptr;
}

const Json &Json::at(std::string_view key) const {
    if (!is_object()) {
        throw_type_error("at(key)", "object");
    }
    const Json *found = find(key);
    if (found == nullptr) {
        throw JsonError("json: key not found: " + std::string(key));
    }
    return *found;
}

const Json &Json::at(size_t index) const {
    if (!is_array()) {
        throw_type_error("at(index)", "array");
    }
    const auto &array = std::get<array_t>(value_);
    if (index >= array.size()) {
        throw JsonError("json: index out of range: " + std::to_string(index));
    }
    return array[index];
}

size_t Json::size() const {
    switch (type()) {
    case Type::String:
        return 1;
    case Type::Array:
        return std::get<array_t>(value_).size();
    case Type::Object:
        return std::get<object_t>(value_).size();
    default:
        return 0;
    }
}

void Json::set(std::string key, Json value) {
    if (!is_object()) {
        value_.emplace<object_t>();
    }
    auto &object = std::get<object_t>(value_);
    for (auto &field : object) {
        if (field.first == key) {
            field.second = std::move(value);
            return;
        }
    }
    object.emplace_back(std::move(key), std::move(value));
}

void Json::push_back(Json value) {
    if (!is_array()) {
        value_.emplace<array_t>();
    }
    std::get<array_t>(value_).push_back(std::move(value));
}

std::string Json::dump(int indent) const {
    std::ostringstream out;
    dump_to(out, indent, 0);
    return out.str();
}

void Json::dump_to(std::ostream &out, int indent, int depth) const {
    switch (type()) {
    case Type::Null:
        out << "null";
        return;
    case Type::Bool:
        out << (std::get<bool>(value_) ? "true" : "false");
        return;
    case Type::Number:
        write_number(out, std::get<double>(value_));
        return;
    case Type::String:
        write_escaped(out, std::get<std::string>(value_));
        return;
    case Type::Array: {
        const auto &array = std::get<array_t>(value_);
        if (array.empty()) {
            out << "[]";
            return;
        }
        out.put('[');
        for (size_t i = 0; i < array.size(); ++i) {
            if (i != 0) {
                out.put(',');
            }
            if (indent > 0) {
                out.put('\n');
                write_indent(out, indent, depth + 1);
            }
            array[i].dump_to(out, indent, depth + 1);
        }
        if (indent > 0) {
            out.put('\n');
            write_indent(out, indent, depth);
        }
        out.put(']');
        return;
    }
    case Type::Object: {
        const auto &object = std::get<object_t>(value_);
        if (object.empty()) {
            out << "{}";
            return;
        }
        out.put('{');
        for (size_t i = 0; i < object.size(); ++i) {
            if (i != 0) {
                out.put(',');
            }
            if (indent > 0) {
                out.put('\n');
                write_indent(out, indent, depth + 1);
            }
            write_escaped(out, object[i].first);
            out << (indent > 0 ? ": " : ":");
            object[i].second.dump_to(out, indent, depth + 1);
        }
        if (indent > 0) {
            out.put('\n');
            write_indent(out, indent, depth);
        }
        out.put('}');
        return;
    }
    }
}

}
