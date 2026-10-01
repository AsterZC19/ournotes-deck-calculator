// JSON 解析与序列化，保留对象键顺序。
#pragma once

#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace deckcalc {

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string& message) : std::runtime_error(message) {}
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(int value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(int64_t value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(const char* value) : type_(Type::String), string_(value) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}

    static Json object();
    static Json array();
    static Json parse(std::string_view text);
    static Json parse_file(const std::string& path);
    static Json read(const std::string& path);  // path 为 "-" 时读 stdin

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const;
    double as_double() const;
    int64_t as_int64() const;
    const std::string& as_string() const;

    const std::vector<Json>& items() const;
    const std::vector<std::pair<std::string, Json>>& fields() const;

    const Json* find(std::string_view key) const;
    const Json& at(std::string_view key) const;
    const Json& at(size_t index) const;

    size_t size() const;
    void set(std::string key, Json value);
    void push_back(Json value);

    std::string dump(int indent = 2) const;

private:
    void dump_to(std::ostream& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> array_;
    std::vector<std::pair<std::string, Json>> object_;
};

}  // namespace deckcalc
