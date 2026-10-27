#pragma once

#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace deckcalc {

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string &message) : std::runtime_error(message) {}
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using array_t = std::vector<Json>;
    using object_t = std::vector<std::pair<std::string, Json>>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    Json(double value) : value_(value) {}
    Json(int value) : value_(static_cast<double>(value)) {}
    Json(int64_t value) : value_(static_cast<double>(value)) {}
    Json(const char *value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}

    static Json object();
    static Json array();
    static Json parse(std::string_view text);
    static Json parse_file(const std::string &path);
    static Json read(const std::string &path);

    Type type() const {
        return static_cast<Type>(value_.index());
    }
    bool is_null() const {
        return type() == Type::Null;
    }
    bool is_bool() const {
        return type() == Type::Bool;
    }
    bool is_number() const {
        return type() == Type::Number;
    }
    bool is_integer() const;
    bool is_string() const {
        return type() == Type::String;
    }
    bool is_array() const {
        return type() == Type::Array;
    }
    bool is_object() const {
        return type() == Type::Object;
    }

    bool as_bool() const;
    double as_double() const;
    int64_t as_int64() const;
    const std::string &as_string() const;

    const std::vector<Json> &items() const;
    const array_t &as_array() const {
        return items();
    }
    const std::vector<std::pair<std::string, Json>> &fields() const;

    const Json *find(std::string_view key) const;
    bool contains(std::string_view key) const {
        return find(key) != nullptr;
    }
    const Json &at(std::string_view key) const;
    const Json &at(size_t index) const;

    size_t size() const;
    void set(std::string key, Json value);
    void push_back(Json value);

    std::string dump(int indent = 2) const;

private:
    using storage_t = std::variant<std::monostate, bool, double, std::string, array_t, object_t>;
    explicit Json(storage_t value) : value_(std::move(value)) {}
    void dump_to(std::ostream &out, int indent, int depth) const;

    storage_t value_;
};

}
