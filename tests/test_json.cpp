// json.cpp 的测试。
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using deckcalc::Json;
using deckcalc::JsonError;

static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

template <typename F>
static bool throws_json_error_type(F&& action) {
    try {
        action();
    } catch (const JsonError&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

static bool throws_json_error(const std::string& text) {
    try {
        Json::parse(text);
    } catch (const JsonError&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

static std::string error_message(const std::string& text) {
    try {
        Json::parse(text);
    } catch (const JsonError& e) {
        return e.what();
    } catch (...) {
        return "<not JsonError>";
    }
    return "<no throw>";
}

static bool message_has_offset(const std::string& text, size_t offset) {
    const std::string message = error_message(text);
    return message.find("byte " + std::to_string(offset)) != std::string::npos;
}

int main() {
    // 嵌套文档的紧凑往返
    const std::string nested =
        R"({"name":"deck","count":3,"ratio":0.5,"ok":true,"none":null,)"
        R"("tags":["a","b"],"nested":{"x":[1,2,{"y":[]}]},"empty":{}})";
    Json doc = Json::parse(nested);
    CHECK(doc.is_object());
    CHECK(doc.size() == 8);
    CHECK(doc.dump(0) == nested);
    CHECK(Json::parse(doc.dump(0)).dump(0) == nested);
    CHECK(Json::parse(doc.dump(2)).dump(0) == nested);
    CHECK(doc.at("name").as_string() == "deck");
    CHECK(doc.at("tags").at(1).as_string() == "b");
    CHECK(doc.at("nested").at("x").at(2).at("y").size() == 0);

    // dump(0) 与 dump(2)
    Json pretty_src = Json::parse(R"({"a":1,"b":[1,2],"c":{},"d":[]})");
    CHECK(pretty_src.dump(0) == R"({"a":1,"b":[1,2],"c":{},"d":[]})");
    CHECK(pretty_src.dump(-1) == pretty_src.dump(0));
    const std::string pretty = pretty_src.dump(2);
    const std::string pretty_expected =
        "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ],\n  \"c\": {},\n  \"d\": []\n}";
    CHECK(pretty == pretty_expected);
    CHECK(pretty.find("\n\n") == std::string::npos);
    CHECK(pretty.find(" \n") == std::string::npos);
    CHECK(pretty.back() == '}');
    CHECK(Json::object().dump(2) == "{}");
    CHECK(Json::array().dump(2) == "[]");
    CHECK(Json::parse("[1,[2]]").dump(4).find("\n    1,") != std::string::npos);
    CHECK(Json::parse("[1,[2]]").dump(4).find("\n        2") != std::string::npos);

    // 转义序列
    const std::string escaped = std::string("\"\\/\b\f\n\r\t");
    Json ev = Json::parse(R"("\"\\\/\b\f\n\r\t")");
    CHECK(ev.as_string() == escaped);
    CHECK(ev.dump(0) == "\"\\\"\\\\/\\b\\f\\n\\r\\t\"");
    CHECK(Json::parse(ev.dump(0)).as_string() == escaped);
    CHECK(Json(std::string("\x01\x1f")).dump(0) == "\"\\u0001\\u001f\"");
    CHECK(Json(std::string("\x7f")).dump(0) == "\"\x7f\"");

    // 含引号与反斜杠的字符串
    const std::string tricky = "he said \"hi\\\" then C:\\path\\file";
    Json tricky_json = Json::parse(Json(tricky).dump(0));
    CHECK(tricky_json.as_string() == tricky);
    CHECK(tricky_json.dump(0).find("\\\"hi\\\\\\\"") != std::string::npos);

    // 非 ASCII 字节原样保留
    Json utf8_raw = Json::parse("\"\xe4\xb8\xad\xe6\x96\x87\"");
    CHECK(utf8_raw.as_string() == "\xe4\xb8\xad\xe6\x96\x87");
    CHECK(utf8_raw.dump(0) == "\"\xe4\xb8\xad\xe6\x96\x87\"");
    CHECK(utf8_raw.dump(0).find("\\u") == std::string::npos);

    // \uXXXX 与代理对
    CHECK(Json::parse(R"("\u4e2d\u6587")").as_string() == "\xe4\xb8\xad\xe6\x96\x87");
    CHECK(Json::parse(R"("\ud83d\ude00")").as_string() == "\xf0\x9f\x98\x80");
    CHECK(Json::parse(R"("\uD834\uDD1E")").as_string() == "\xf0\x9d\x84\x9e");
    CHECK(Json::parse(R"("\u0041\u00e9\u20ac")").as_string() == "A\xc3\xa9\xe2\x82\xac");
    CHECK(Json::parse(R"("\u0000")").as_string().size() == 1);
    CHECK(Json::parse(R"("\u0000")").as_string()[0] == '\0');
    CHECK(Json::parse(R"("\uFFFD")").as_string() == "\xef\xbf\xbd");
    CHECK(throws_json_error(R"("\ud800")"));
    CHECK(throws_json_error(R"("\udc00")"));
    CHECK(throws_json_error(R"("\ud800\u0041")"));
    CHECK(throws_json_error(R"("\ud800x")"));
    CHECK(throws_json_error(R"("\u12")"));
    CHECK(throws_json_error(R"("\uZZZZ")"));
    CHECK(throws_json_error(R"("\u00")"));

    // 数字形式
    CHECK(Json::parse("0").as_double() == 0.0);
    CHECK(Json::parse("-0").as_double() == 0.0);
    CHECK(Json::parse("123").as_double() == 123.0);
    CHECK(Json::parse("-42").as_double() == -42.0);
    CHECK(Json::parse("1.5").as_double() == 1.5);
    CHECK(Json::parse("-1.5").as_double() == -1.5);
    CHECK(Json::parse("0.5").as_double() == 0.5);
    CHECK(Json::parse("1e3").as_double() == 1000.0);
    CHECK(Json::parse("1E+3").as_double() == 1000.0);
    CHECK(Json::parse("1e-3").as_double() == 1e-3);
    CHECK(Json::parse("-2.5e-2").as_double() == -0.025);
    CHECK(Json::parse("1.25e2").as_double() == 125.0);
    CHECK(Json::parse("123456789012345").as_double() == 123456789012345.0);
    CHECK(Json::parse("1e-400").as_double() == 0.0);
    CHECK(Json::parse("0.0000000001").dump(0) == "1e-10");
    CHECK(Json::parse("1.0").dump(0) == "1");
    CHECK(Json::parse("100.0").dump(0) == "100");
    CHECK(Json::parse("3.5").dump(0) == "3.5");
    CHECK(Json::parse("1e3").dump(0) == "1000");
    CHECK(Json::parse("1e15").dump(0) == "1e+15");
    CHECK(Json::parse("-7").dump(0) == "-7");
    CHECK(Json::parse("0.1").dump(0) == "0.1");

    // 非法数字
    CHECK(throws_json_error("01"));
    CHECK(throws_json_error("-01"));
    CHECK(throws_json_error("1."));
    CHECK(throws_json_error(".5"));
    CHECK(throws_json_error("+1"));
    CHECK(throws_json_error("1e"));
    CHECK(throws_json_error("1e+"));
    CHECK(throws_json_error("--1"));
    CHECK(throws_json_error("-"));
    CHECK(throws_json_error("0x10"));
    CHECK(throws_json_error("1e999"));
    CHECK(throws_json_error("-1e999"));
    CHECK(throws_json_error("NaN"));
    CHECK(throws_json_error("Infinity"));
    CHECK(throws_json_error("-Infinity"));
    CHECK(throws_json_error("00"));
    CHECK(throws_json_error("[01]"));
    CHECK((message_has_offset("01", 1)));

    // 字面量
    CHECK(Json::parse("true").as_bool() == true);
    CHECK(Json::parse("false").as_bool() == false);
    CHECK(Json::parse("null").is_null());
    CHECK(throws_json_error("True"));
    CHECK(throws_json_error("tru"));
    CHECK(throws_json_error("nul"));
    CHECK(throws_json_error("falsey"));
    CHECK(Json::parse(" \t\r\n true \t\r\n ").as_bool() == true);
    CHECK(Json::parse("\n[\r\n 1 ,\t2\n]\n").size() == 2);
    CHECK(throws_json_error("\v1"));
    CHECK(throws_json_error("\f1"));

    // 结构错误
    CHECK(throws_json_error(""));
    CHECK(throws_json_error("   "));
    CHECK(throws_json_error("[1] x"));
    CHECK(throws_json_error("[1]]"));
    CHECK(throws_json_error("{\"a\":1} {\"b\":2}"));
    CHECK(throws_json_error("[1,]"));
    CHECK(throws_json_error("{\"a\":1,}"));
    CHECK(throws_json_error("{,}"));
    CHECK(throws_json_error("[1 2]"));
    CHECK(throws_json_error("{\"a\" 1}"));
    CHECK(throws_json_error("{a:1}"));
    CHECK(throws_json_error("{'a':1}"));
    CHECK(throws_json_error("{\"a\":}"));
    CHECK(throws_json_error("["));
    CHECK(throws_json_error("]"));
    CHECK(throws_json_error("}"));
    CHECK(throws_json_error("[1"));
    CHECK(throws_json_error("{\"a\":1"));
    CHECK(throws_json_error("[,1]"));
    CHECK(throws_json_error("{\"a\":1,,\"b\":2}"));
    CHECK((message_has_offset("{\"a\":1,}", 7)));
    CHECK((message_has_offset("[1,]", 3)));
    CHECK((message_has_offset("[1] x", 4)));
    CHECK(error_message("{a:1}").find("byte 1") != std::string::npos);

    // 字符串错误
    CHECK(throws_json_error("\"abc"));
    CHECK(throws_json_error("{\"a\""));
    CHECK(throws_json_error(std::string("\"a\nb\"")));
    CHECK(throws_json_error(std::string("\"a\tb\"")));
    CHECK(throws_json_error(std::string("\"\x01\"")));
    CHECK(throws_json_error(std::string("\"\x1f\"")));
    CHECK(throws_json_error(R"("\q")"));
    CHECK(throws_json_error(R"("\")"));
    CHECK(throws_json_error("[1,\"a]"));

    // BOM
    CHECK(Json::parse("\xef\xbb\xbf{\"a\":1}").at("a").as_double() == 1.0);
    CHECK(Json::parse("\xef\xbb\xbf[1,2]").size() == 2);
    CHECK(Json::parse("\xef\xbb\xbf 42").as_double() == 42.0);
    CHECK(throws_json_error("\xef\xbb\xbf"));
    CHECK(throws_json_error("\xef\xbb\xbf\xef\xbb\xbf{}"));

    // 重复键：后值覆盖，位置保留
    Json dup = Json::parse(R"({"a":1,"b":2,"a":3})");
    CHECK(dup.size() == 2);
    CHECK(dup.at("a").as_double() == 3.0);
    CHECK(dup.dump(0) == R"({"a":3,"b":2})");

    // 深度保护
    std::string deep_ok;
    for (int i = 0; i < 512; ++i) deep_ok += '[';
    for (int i = 0; i < 512; ++i) deep_ok += ']';
    CHECK(Json::parse(deep_ok).is_array());
    std::string deep_bad;
    for (int i = 0; i < 600; ++i) deep_bad += '[';
    for (int i = 0; i < 600; ++i) deep_bad += ']';
    CHECK(throws_json_error(deep_bad));

    // find / at / set / push_back / size 语义
    Json obj = Json::object();
    CHECK(obj.is_object());
    CHECK(obj.size() == 0);
    obj.set("b", 1);
    obj.set("a", Json("x"));
    CHECK(obj.size() == 2);
    CHECK(obj.dump(0) == R"({"b":1,"a":"x"})");
    obj.set("b", 3);
    CHECK(obj.size() == 2);
    CHECK(obj.dump(0) == R"({"b":3,"a":"x"})");
    CHECK(obj.find("a") != nullptr);
    CHECK(obj.find("a")->as_string() == "x");
    CHECK(obj.find("zzz") == nullptr);
    CHECK(obj.at("b").as_double() == 3.0);
    CHECK(Json(5).find("b") == nullptr);
    CHECK(Json::array().find("b") == nullptr);
    CHECK(throws_json_error_type([&] { obj.at("zzz"); }));
    CHECK(throws_json_error_type([&] { Json::array().at("b"); }));
    CHECK(throws_json_error_type([&] { Json(1).fields(); }));
    CHECK(throws_json_error_type([&] { Json::array().fields(); }));
    CHECK(obj.fields().size() == 2);
    CHECK(obj.fields()[0].first == "b");
    CHECK(obj.fields()[0].second.as_double() == 3.0);
    CHECK(obj.fields()[1].first == "a");

    Json arr = Json::array();
    CHECK(arr.is_array());
    CHECK(arr.size() == 0);
    arr.push_back(1);
    arr.push_back(Json("two"));
    arr.push_back(Json::object());
    CHECK(arr.size() == 3);
    CHECK(arr.dump(0) == R"([1,"two",{}])");
    CHECK(arr.items().size() == 3);
    CHECK(arr.at(0).as_double() == 1.0);
    CHECK(arr.at(2).is_object());
    CHECK(throws_json_error_type([&] { arr.at(3); }));
    CHECK(throws_json_error_type([&] { Json(1).at(0); }));
    CHECK(throws_json_error_type([&] { Json(1).items(); }));
    CHECK(throws_json_error_type([&] { Json(1).at("a"); }));

    Json fresh;
    fresh.push_back(Json(true));
    CHECK(fresh.is_array());
    CHECK(fresh.size() == 1);
    Json fresh2;
    fresh2.set("k", Json(nullptr));
    CHECK(fresh2.is_object());
    CHECK(fresh2.at("k").is_null());

    // size 语义
    CHECK(Json().size() == 0);
    CHECK(Json(true).size() == 0);
    CHECK(Json(1.5).size() == 0);
    CHECK(Json("s").size() == 1);
    CHECK(Json(std::string("")).size() == 1);
    CHECK(Json::array().size() == 0);
    CHECK(Json::object().size() == 0);

    // as_* 类型检查
    CHECK(Json(true).as_bool() == true);
    CHECK(Json(false).as_bool() == false);
    CHECK(Json(3).as_int64() == 3);
    CHECK(Json(3.0).as_int64() == 3);
    CHECK(Json(static_cast<int64_t>(1) << 40).as_int64() == (static_cast<int64_t>(1) << 40));
    CHECK(Json::parse("42").as_int64() == 42);
    CHECK(Json::parse("42.0").as_int64() == 42);
    CHECK(Json::parse("-7").as_int64() == -7);
    CHECK(Json::parse("9223372036854775807").as_int64() == std::numeric_limits<int64_t>::max());
    CHECK(Json::parse("-9223372036854775808").as_int64() == std::numeric_limits<int64_t>::min());
    CHECK(Json("hello").as_string() == "hello");
    CHECK(Json("hello").as_string().size() == 5);
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(throws_json_error_type([&] { Json(true).as_double(); }));
    CHECK(throws_json_error_type([&] { Json(1).as_bool(); }));
    CHECK(throws_json_error_type([&] { Json("1").as_int64(); }));
    CHECK(throws_json_error_type([&] { Json(1).as_string(); }));
    CHECK(throws_json_error_type([&] { Json().as_bool(); }));
    CHECK(throws_json_error_type([&] { Json(1.5).as_int64(); }));
    CHECK(throws_json_error_type([&] { Json(inf).as_int64(); }));
    CHECK(throws_json_error_type([&] { Json(nan).as_int64(); }));
    CHECK(throws_json_error_type([&] { Json(1e300).as_int64(); }));
    CHECK(throws_json_error_type([&] { Json(-1e300).as_int64(); }));

    // 非有限数不输出 nan/inf
    CHECK(Json(inf).dump(0) == "null");
    CHECK(Json(-inf).dump(0) == "null");
    CHECK(Json(nan).dump(0) == "null");

    // 文件读取
    const std::string path = "test_json_tmp.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << "\xef\xbb\xbf{\"k\":[1,2,3],\"s\":\"a\\nb\"}";
    }
    Json from_file = Json::parse_file(path);
    CHECK(from_file.is_object());
    CHECK(from_file.at("k").size() == 3);
    CHECK(from_file.at("s").as_string() == "a\nb");
    CHECK(Json::read(path).dump(0) == R"({"k":[1,2,3],"s":"a\nb"})");
    std::remove(path.c_str());
    bool file_missing_threw = false;
    try {
        Json::parse_file("no_such_file_json_test.json");
    } catch (const JsonError&) {
        file_missing_threw = true;
    }
    CHECK(file_missing_threw);

    // "-" 从 stdin 读取
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"stdin":[1,2,3]})";
        out.close();
        if (std::freopen(path.c_str(), "rb", stdin) != nullptr) {
            Json from_stdin = Json::read("-");
            CHECK(from_stdin.at("stdin").size() == 3);
        }
        std::remove(path.c_str());
    }

    std::printf("test_json: %d checks passed\n", g_checks);
    return 0;
}
