#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace deckcalc {
namespace {

using Path = std::string;

[[noreturn]] void fail(const Path &path, const std::string &message) {
    throw SpecError(path + ": " + message);
}

std::string child(const Path &path, const std::string &key) {
    return path.empty() ? key : path + "." + key;
}

std::string indexed(const Path &path, std::size_t index) {
    return path + "[" + std::to_string(index) + "]";
}

const char *type_name(const Json &value) {
    switch (value.type()) {
    case Json::Type::Null:
        return "NoneType";
    case Json::Type::Bool:
        return "bool";
    case Json::Type::String:
        return "str";
    case Json::Type::Array:
        return "list";
    case Json::Type::Object:
        return "dict";
    case Json::Type::Number:
        break;
    }
    const double number = value.as_double();
    return number == std::floor(number) ? "int" : "float";
}

std::string json_repr(const Json &value) {
    switch (value.type()) {
    case Json::Type::Null:
        return "None";
    case Json::Type::Bool:
        return value.as_bool() ? "True" : "False";
    case Json::Type::Number: {
        const double number = value.as_double();
        if (number == std::floor(number) && std::fabs(number) < 1e15) {
            return std::to_string(static_cast<long long>(number));
        }
        std::ostringstream stream;
        stream << number;
        return stream.str();
    }
    case Json::Type::String: {
        std::string out = "'";
        for (char item : value.as_string()) {
            if (item == '\\' || item == '\'') {
                out += '\\';
            }
            out += item;
        }
        out += '\'';
        return out;
    }
    default:
        break;
    }
    return value.dump(0);
}

std::string int_list_repr(const std::vector<int64_t> &values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += std::to_string(values[i]);
    }
    return out + "]";
}

bool truthy(const Json &value) {
    switch (value.type()) {
    case Json::Type::Null:
        return false;
    case Json::Type::Bool:
        return value.as_bool();
    case Json::Type::Number:
        return value.as_double() != 0;
    case Json::Type::String:
        return !value.as_string().empty();
    case Json::Type::Array:
    case Json::Type::Object:
        return value.size() != 0;
    }
    return false;
}

bool is_string_in(const Json &value, std::initializer_list<const char *> options) {
    if (!value.is_string()) {
        return false;
    }
    const std::string &text = value.as_string();
    for (const char *option : options) {
        if (text == option) {
            return true;
        }
    }
    return false;
}

const Json *member_ptr(const Json &object, const char *key) {
    if (!object.is_object()) {
        return nullptr;
    }
    return object.find(key);
}

const Json &as_mapping(const Json &value, const Path &path) {
    if (!value.is_object()) {
        fail(path, std::string("需要对象，实际是 ") + type_name(value));
    }
    return value;
}

const Json &as_list(const Json &value, const Path &path) {
    if (!value.is_array()) {
        fail(path, std::string("需要数组，实际是 ") + type_name(value));
    }
    return value;
}

int64_t as_int(const Json &value, const Path &path) {
    if (value.is_bool() || !value.is_number()) {
        fail(path, "需要整数，实际是 " + json_repr(value));
    }
    const double number = value.as_double();
    if (number != std::floor(number)) {
        fail(path, "需要整数，实际是 " + json_repr(value));
    }
    return static_cast<int64_t>(number);
}

double as_num(const Json &value, const Path &path) {
    if (value.is_bool() || !value.is_number()) {
        fail(path, "需要数字，实际是 " + json_repr(value));
    }
    return value.as_double();
}

std::string as_str(const Json &value, const Path &path) {
    if (!value.is_string()) {
        fail(path, "需要字符串，实际是 " + json_repr(value));
    }
    return value.as_string();
}

int64_t int_default(const Json &object, const char *key, int64_t fallback, const Path &path) {
    const Json *value = member_ptr(object, key);
    if (value == nullptr) {
        return fallback;
    }
    return as_int(*value, path);
}

double num_default(const Json &object, const char *key, double fallback, const Path &path) {
    const Json *value = member_ptr(object, key);
    if (value == nullptr) {
        return fallback;
    }
    return as_num(*value, path);
}

std::string str_default(const Json &object, const char *key, const std::string &fallback,
                        const Path &path) {
    const Json *value = member_ptr(object, key);
    if (value == nullptr) {
        return fallback;
    }
    return as_str(*value, path);
}

bool bool_default(const Json &object, const char *key, bool fallback, const Path &path) {
    const Json *value = member_ptr(object, key);
    if (value == nullptr) {
        return fallback;
    }
    if (!value->is_bool()) {
        fail(path, "需要布尔值，实际是 " + json_repr(*value));
    }
    return value->as_bool();
}

bool truthy_default(const Json &object, const char *key, bool fallback) {
    const Json *value = member_ptr(object, key);
    if (value == nullptr) {
        return fallback;
    }
    return truthy(*value);
}

std::string str_or(const Json &object, const char *key, const std::string &fallback,
                   const Path &path) {
    const Json *value = member_ptr(object, key);
    if (value == nullptr || value->is_null()) {
        return fallback;
    }
    if (value->is_string()) {
        const std::string &text = value->as_string();
        return text.empty() ? fallback : text;
    }
    if (value->is_number() || value->is_bool()) {
        const std::string text = json_repr(*value);
        return text.empty() ? fallback : text;
    }
    fail(path, "需要字符串，实际是 " + json_repr(*value));
}

int64_t require_int(const Json &object, const char *key, const Path &path) {
    const Json *value = member_ptr(object, key);
    if (value == nullptr) {
        fail(path, "需要整数，实际是 None");
    }
    return as_int(*value, path);
}

const Json &list_or_empty(const Json &object, const char *key, const Path &path) {
    static const Json kEmpty = Json::array();
    const Json *value = member_ptr(object, key);
    if (value == nullptr || !truthy(*value)) {
        return kEmpty;
    }
    return as_list(*value, path);
}

const Json &object_or_empty(const Json *value) {
    static const Json kEmpty = Json::object();
    if (value == nullptr || !truthy(*value)) {
        return kEmpty;
    }
    return *value;
}

template <typename T> std::vector<T> int_list(const Json &value, const Path &path) {
    const Json &array = as_list(value, path);
    std::vector<T> out;
    out.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        out.push_back(static_cast<T>(as_int(array.at(i), indexed(path, i))));
    }
    return out;
}

template <typename T>
std::vector<T> int_array_or_empty(const Json &object, const char *key, const Path &path) {
    return int_list<T>(list_or_empty(object, key, path), path);
}

Triple as_triple(const Json *value, const Path &path, const Triple &fallback, bool has_default) {
    if (value == nullptr || value->is_null()) {
        if (!has_default) {
            fail(path, "缺少三维数值");
        }
        return fallback;
    }
    if (value->is_bool()) {
        fail(path, "需要 3 个数字，实际是 " + json_repr(*value));
    }
    if (value->is_number()) {
        const int64_t broadcast = as_int(*value, path);
        return Triple{broadcast, broadcast, broadcast};
    }
    const std::vector<int64_t> items = int_list<int64_t>(*value, path);
    if (items.size() != 3) {
        fail(path, "需要 3 个数字，实际有 " + std::to_string(items.size()) + " 个");
    }
    return Triple{items[0], items[1], items[2]};
}

const Json &settings_defaults() {
    static const Json kDefaults = [] {
        Json axes = Json::object();
        axes.set("1000", Json("all"));
        axes.set("1001", Json("technique"));
        axes.set("1002", Json("visual"));
        axes.set("1003", Json("performance"));

        Json select = Json::object();
        select.set("effect_type", Json(nullptr));
        select.set("branch", Json("max"));

        Json skill = Json::object();
        skill.set("overlap", Json("additive"));
        skill.set("activation", Json("scheduled"));
        skill.set("live_effect_select", select);

        Json sources = Json::object();
        for (const char *key :
             {"snapshot", "type_link", "band_item", "music_type", "music_tag", "leader", "vip"}) {
            sources.set(key, Json(true));
        }
        Json power = Json::object();
        power.set("rounding", Json("float32_floor"));
        power.set("sources", sources);
        power.set("extra_sources", Json::array());

        Json score = Json::object();
        score.set("adjustment_factor", Json(3.0));
        score.set("level_base", Json(5.0));
        score.set("level_alpha", Json(0.005));
        score.set("rounding", Json("float32_floor"));

        Json factors = Json::object();
        factors.set("perfect", Json(1.0));
        factors.set("great", Json(0.8));
        factors.set("good", Json(0.5));
        factors.set("bad", Json(0.0));
        factors.set("miss", Json(0.0));
        factors.set("just", Json(2.3));
        factors.set("secret", Json(3.0));
        Json judgement = Json::object();
        judgement.set("mode", Json("all_perfect"));
        judgement.set("factors", factors);
        judgement.set("default_factor", Json(0.0));
        judgement.set("fixed_label", Json("perfect"));
        judgement.set("sequence", Json::array());
        judgement.set("distribution", Json(nullptr));
        judgement.set("seed", Json(0));

        Json life = Json::object();
        life.set("mode", Json("constant"));
        life.set("initial", Json(1000.0));
        life.set("onus_factor", Json(0.3));
        life.set("damage", Json(nullptr));
        life.set("floor", Json(0.0));

        Json assist = Json::object();
        assist.set("enabled", Json(false));
        assist.set("score_percent", Json(90.0));

        Json gekisou = Json::object();
        gekisou.set("enabled", Json(false));
        gekisou.set("sections", Json::array());

        Json out = Json::object();
        out.set("team_size", Json(5));
        out.set("combo_type", Json(0));
        out.set("base_duration_ms", Json(static_cast<int64_t>(5000)));
        out.set("live_level", Json(5));
        out.set("leader_level", Json(5));
        out.set("support_level", Json(5));
        out.set("duration_extension_effect_type", Json(static_cast<int64_t>(15000)));
        out.set("type_link_bonus_source", Json("snapshot"));
        out.set("leader_unmapped_effect_types", Json("ignore"));
        out.set("leader_effect_axes", axes);
        out.set("skill", skill);
        out.set("power_model", power);
        out.set("score_model", score);
        out.set("judgement", judgement);
        out.set("life", life);
        out.set("assist", assist);
        out.set("gekisou", gekisou);
        return out;
    }();
    return kDefaults;
}

Json deep_defaults(const Json *value, const Json &defaults, const Path &path, bool allow_extra) {
    if (defaults.is_object()) {
        if (value == nullptr || value->is_null()) {
            return defaults;
        }
        const Path spot = path.empty() ? "<root>" : path;
        const Json &data = as_mapping(*value, spot);
        Json out = Json::object();
        for (const auto &entry : defaults.fields()) {
            const Json *raw = member_ptr(data, entry.first.c_str());
            if (raw != nullptr && raw->is_null()) {
                raw = nullptr;
            }
            out.set(entry.first,
                    deep_defaults(raw, entry.second, child(path, entry.first), allow_extra));
        }
        if (!allow_extra) {
            for (const auto &entry : data.fields()) {
                if (defaults.find(entry.first) == nullptr) {
                    fail(child(path, entry.first), "未知字段");
                }
            }
        } else {
            for (const auto &entry : data.fields()) {
                if (defaults.find(entry.first) == nullptr) {
                    out.set(entry.first, entry.second);
                }
            }
        }
        return out;
    }
    if (value == nullptr || value->is_null()) {
        return defaults;
    }
    return *value;
}

Settings parse_settings(const Json *data, const Path &path) {
    const Json merged = deep_defaults(data, settings_defaults(), path, false);
    Settings out;

    const Json &skill = merged.at("skill");
    const Json &overlap = skill.at("overlap");
    if (!is_string_in(overlap, {"additive"})) {
        fail(child(path, "skill.overlap"), "v1 只支持 'additive'，收到 " + json_repr(overlap));
    }
    const Json &activation = skill.at("activation");
    if (!is_string_in(activation, {"scheduled", "chart"})) {
        fail(child(path, "skill.activation"),
             "只支持 'scheduled' / 'chart'，收到 " + json_repr(activation));
    }
    const Json &select = skill.at("live_effect_select");
    const Json &branch = select.at("branch");
    if (!is_string_in(branch, {"max", "min", "first", "index"})) {
        fail(child(path, "skill.live_effect_select.branch"),
             "只支持 'max' / 'min' / 'first' / 'index'");
    }
    out.skill.overlap = overlap.as_string();
    out.skill.activation = activation.as_string();
    out.skill.live_effect_branch = branch.as_string();
    const Json *effect_type = member_ptr(select, "effect_type");
    if (effect_type != nullptr && !effect_type->is_null()) {
        out.skill.live_effect_type = static_cast<int>(
            as_int(*effect_type, child(path, "skill.live_effect_select.effect_type")));
    }

    const Json &power = merged.at("power_model");
    const Json &power_rounding = power.at("rounding");
    if (!is_string_in(power_rounding, {"float32_floor", "float64_floor"})) {
        fail(child(path, "power_model.rounding"),
             "综合力只支持 float32_floor / float64_floor；整数属性模型不支持 none");
    }
    out.power.rounding = power_rounding.as_string();
    const Json &sources = power.at("sources");
    for (const auto &entry : sources.fields()) {
        if (!entry.second.is_bool()) {
            fail(child(child(path, "power_model.sources"), entry.first), "需要布尔值");
        }
    }
    out.power.snapshot = sources.at("snapshot").as_bool();
    out.power.type_link = sources.at("type_link").as_bool();
    out.power.band_item = sources.at("band_item").as_bool();
    out.power.music_type = sources.at("music_type").as_bool();
    out.power.music_tag = sources.at("music_tag").as_bool();
    out.power.leader = sources.at("leader").as_bool();
    out.power.vip = sources.at("vip").as_bool();

    const Path extra_path = child(path, "power_model.extra_sources");
    const Json &extra_sources = as_list(power.at("extra_sources"), extra_path);
    for (std::size_t i = 0; i < extra_sources.size(); ++i) {
        const Path spot = indexed(extra_path, i);
        const Json &source = as_mapping(extra_sources.at(i), spot);
        const Json *id = member_ptr(source, "id");
        if (id == nullptr) {
            fail(child(spot, "id"), "缺少 id");
        }
        const Json *rate = member_ptr(source, "rate_bp");
        if (rate != nullptr && rate->is_null()) {
            rate = nullptr;
        }
        ExtraSource entry;
        entry.id = id->is_string() ? id->as_string() : json_repr(*id);
        entry.rate_bp = as_triple(rate, child(spot, "rate_bp"), Triple{0, 0, 0}, true);
        const Json *scope = member_ptr(source, "scope");
        if (scope != nullptr && !scope->is_null()) {
            if (!is_string_in(*scope, {"slot", "all"})) {
                fail(child(spot, "scope"), "只支持 'slot' / 'all'");
            }
            entry.scope = scope->as_string();
        }
        const Json *kind = member_ptr(source, "kind");
        if (kind != nullptr && !kind->is_null()) {
            if (!is_string_in(*kind, {"percent", "flat"})) {
                fail(child(spot, "kind"), "只支持 'percent' / 'flat'");
            }
            entry.kind = kind->as_string();
        }
        entry.when = Json::object();
        const Json *when = member_ptr(source, "when");
        if (when != nullptr && !when->is_null()) {
            const Path when_path = child(spot, "when");
            const Json &when_map = as_mapping(*when, when_path);
            for (const auto &filter : when_map.fields()) {
                if (filter.first != "member_tags_any" && filter.first != "member_bands" &&
                    filter.first != "member_characters" && filter.first != "card_types" &&
                    filter.first != "snapshot_card_types") {
                    fail(child(child(spot, "when"), filter.first), "未知过滤条件");
                }
            }
            entry.when = when_map;
        }
        out.power.extra_sources.push_back(entry);
    }

    const Json &score = merged.at("score_model");
    const Json &axes = merged.at("leader_effect_axes");
    for (const auto &entry : axes.fields()) {
        if (!is_string_in(entry.second, {"all", "performance", "technique", "visual"})) {
            fail(child(child(path, "leader_effect_axes"), entry.first),
                 "只支持 'all' / 'performance' / 'technique' / 'visual'");
        }
        out.leader_effect_axes[std::stoi(entry.first)] = entry.second.as_string();
    }
    const Json &link_source = merged.at("type_link_bonus_source");
    if (!is_string_in(link_source, {"snapshot", "member"})) {
        fail(child(path, "type_link_bonus_source"), "只支持 'snapshot' / 'member'");
    }
    out.type_link_bonus_source = link_source.as_string();
    const Json &unmapped = merged.at("leader_unmapped_effect_types");
    if (!is_string_in(unmapped, {"ignore", "error"})) {
        fail(child(path, "leader_unmapped_effect_types"), "只支持 'ignore' / 'error'");
    }
    out.leader_unmapped_effect_types = unmapped.as_string();
    const Json &score_rounding = score.at("rounding");
    if (!is_string_in(score_rounding, {"float32_floor", "float64_floor", "none"})) {
        fail(child(path, "score_model.rounding"),
             "只支持 ('float32_floor', 'float64_floor', 'none')");
    }
    out.score.rounding = score_rounding.as_string();
    out.score.adjustment_factor =
        as_num(score.at("adjustment_factor"), child(path, "score_model.adjustment_factor"));
    out.score.level_base = as_num(score.at("level_base"), child(path, "score_model.level_base"));
    const Json &level_alpha = score.at("level_alpha");
    if (!level_alpha.is_null()) {
        out.score.level_alpha = as_num(level_alpha, child(path, "score_model.level_alpha"));
    } else {
        out.score.level_alpha.reset();
    }

    if (data != nullptr && data->is_object()) {
        const Json *requested_score = data->find("score_model");
        if (requested_score != nullptr && requested_score->is_object()) {
            const Json *requested_alpha = requested_score->find("level_alpha");
            if (requested_alpha != nullptr && requested_alpha->is_null())
                out.score.level_alpha.reset();
        }
    }

    const Json &judgement = merged.at("judgement");
    const Json &mode = judgement.at("mode");
    if (!is_string_in(mode, {"all_perfect", "fixed", "sequence", "random"})) {
        fail(child(path, "judgement.mode"),
             "只支持 'all_perfect' / 'fixed' / 'sequence' / 'random'");
    }
    out.judgement.mode = mode.as_string();
    const Path factors_path = child(path, "judgement.factors");
    for (const auto &entry : judgement.at("factors").fields()) {
        out.judgement.factors[entry.first] = as_num(entry.second, child(factors_path, entry.first));
    }
    out.judgement.default_factor =
        as_num(judgement.at("default_factor"), child(path, "judgement.default_factor"));
    out.judgement.fixed_label =
        as_str(judgement.at("fixed_label"), child(path, "judgement.fixed_label"));
    out.judgement.seed = as_int(judgement.at("seed"), child(path, "judgement.seed"));
    const Path sequence_path = child(path, "judgement.sequence");
    const Json &sequence = as_list(judgement.at("sequence"), sequence_path);
    out.judgement.sequence = sequence.items();
    const Json &distribution = judgement.at("distribution");
    if (!distribution.is_null()) {
        const Path distribution_path = child(path, "judgement.distribution");
        const Json &distribution_map = as_mapping(distribution, distribution_path);
        for (const auto &entry : distribution_map.fields()) {
            out.judgement.distribution[entry.first] =
                as_num(entry.second, child(distribution_path, entry.first));
        }
        out.judgement.has_distribution = true;
    }

    const Json &life = merged.at("life");
    const Json &life_mode = life.at("mode");
    if (!is_string_in(life_mode, {"constant", "per_note"})) {
        fail(child(path, "life.mode"), "只支持 'constant' / 'per_note'");
    }
    out.life.mode = life_mode.as_string();
    out.life.initial = as_num(life.at("initial"), child(path, "life.initial"));
    out.life.onus_factor = as_num(life.at("onus_factor"), child(path, "life.onus_factor"));
    out.life.floor = as_num(life.at("floor"), child(path, "life.floor"));
    const Json &damage = life.at("damage");
    if (!damage.is_null()) {
        const Path damage_path = child(path, "life.damage");
        const Json &damage_map = as_mapping(damage, damage_path);
        for (const auto &entry : damage_map.fields()) {
            out.life.damage[entry.first] = as_num(entry.second, child(damage_path, entry.first));
        }
    }

    const Json &assist = merged.at("assist");
    out.assist.enabled = truthy(assist.at("enabled"));
    out.assist.score_percent =
        as_num(assist.at("score_percent"), child(path, "assist.score_percent"));

    const Json &gekisou = merged.at("gekisou");
    out.gekisou.enabled = truthy(gekisou.at("enabled"));
    const Path sections_path = child(path, "gekisou.sections");
    const Json &sections = as_list(gekisou.at("sections"), sections_path);
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const Path spot = indexed(sections_path, i);
        const Json &section = as_mapping(sections.at(i), spot);
        GekisouSection parsed;
        parsed.start_ms = int_default(section, "start_ms", 0, child(spot, "start_ms"));
        const Json *end_ms = member_ptr(section, "end_ms");
        if (end_ms != nullptr) {
            parsed.end_ms = as_int(*end_ms, child(spot, "end_ms"));
        }
        parsed.bonus_bp = int_default(section, "bonus_bp", 10000, child(spot, "bonus_bp"));
        parsed.label =
            str_or(section, "label", "section-" + std::to_string(i + 1), child(spot, "label"));
        out.gekisou.sections.push_back(parsed);
    }

    const int64_t team_size = as_int(merged.at("team_size"), child(path, "team_size"));
    if (team_size < 1) {
        fail(child(path, "team_size"), "必须 >= 1");
    }
    if (team_size > 8)
        fail(child(path, "team_size"), "必须 <= 8");
    out.team_size = static_cast<int>(team_size);
    out.combo_type = static_cast<int>(as_int(merged.at("combo_type"), child(path, "combo_type")));
    out.base_duration_ms = as_int(merged.at("base_duration_ms"), child(path, "base_duration_ms"));
    out.live_level = static_cast<int>(as_int(merged.at("live_level"), child(path, "live_level")));
    out.leader_level =
        static_cast<int>(as_int(merged.at("leader_level"), child(path, "leader_level")));
    out.support_level =
        static_cast<int>(as_int(merged.at("support_level"), child(path, "support_level")));
    out.duration_extension_effect_type =
        static_cast<int>(as_int(merged.at("duration_extension_effect_type"),
                                child(path, "duration_extension_effect_type")));
    if (out.base_duration_ms < 0 || out.live_level < 1 || out.leader_level < 1 ||
        out.support_level < 1)
        fail(path, "技能等级必须为正数，基础持续时间必须非负");
    return out;
}

Song parse_song(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    Song song;
    song.id = require_int(object, "id", child(path, "id"));
    song.title = str_or(object, "title", "", child(path, "title"));
    song.type = static_cast<int>(int_default(object, "type", 0, child(path, "type")));
    song.bands = int_array_or_empty<int>(object, "bands", child(path, "bands"));
    song.tags = int_array_or_empty<int>(object, "tags", child(path, "tags"));
    return song;
}

Note parse_note(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "t") == nullptr) {
        fail(child(path, "t"), "缺少音符时间（毫秒）");
    }
    Note note;
    note.t = as_int(*member_ptr(object, "t"), child(path, "t"));
    note.op = static_cast<int>(int_default(object, "op", 0, child(path, "op")));
    note.scoring = truthy_default(object, "scoring", true);
    return note;
}

ChartSpec parse_chart(const Json *data, const Path &path) {
    const Json &object = as_mapping(object_or_empty(data), path);
    ChartSpec chart;
    chart.difficulty = str_default(object, "difficulty", "expert", child(path, "difficulty"));
    chart.level = num_default(object, "level", 0, child(path, "level"));
    const Path notes_path = child(path, "notes");
    const Json &notes = list_or_empty(object, "notes", notes_path);
    for (std::size_t i = 0; i < notes.size(); ++i) {
        chart.notes.push_back(parse_note(notes.at(i), indexed(notes_path, i)));
    }
    const Path times_path = child(path, "skill_times_ms");
    chart.skill_times_ms =
        int_list<int64_t>(list_or_empty(object, "skill_times_ms", times_path), times_path);
    const Json *display = member_ptr(object, "display_level");
    if (display != nullptr && !display->is_null()) {
        chart.display_level = as_num(*display, child(path, "display_level"));
    }
    const Json *full_combo = member_ptr(object, "full_combo_count");
    if (full_combo != nullptr && !full_combo->is_null()) {
        chart.full_combo_count = as_int(*full_combo, child(path, "full_combo_count"));
    }
    return chart;
}

CardRankBonus parse_card_rank_bonus(const Json *data, const Path &path) {
    CardRankBonus out;
    if (data == nullptr || data->is_null()) {
        return out;
    }
    const Json &object = as_mapping(*data, path);
    out.type_link = int_default(object, "type_link", 0, child(path, "type_link"));
    out.music_type = int_default(object, "music_type", 0, child(path, "music_type"));
    out.music_tag = int_default(object, "music_tag", 0, child(path, "music_tag"));
    return out;
}

Member parse_member(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "id") == nullptr) {
        fail(child(path, "id"), "缺少成员卡 ID");
    }
    if (member_ptr(object, "trained") == nullptr) {
        fail(child(path, "trained"), "缺少 trained 三维属性");
    }
    Member member;
    member.id = as_int(*member_ptr(object, "id"), child(path, "id"));
    member.name = str_or(object, "name", "", child(path, "name"));
    member.title = str_or(object, "title", "", child(path, "title"));
    member.character =
        static_cast<int>(int_default(object, "character", 0, child(path, "character")));
    member.band = static_cast<int>(int_default(object, "band", 0, child(path, "band")));
    const Json *card_type = member_ptr(object, "card_type");
    if (card_type == nullptr) {
        card_type = member_ptr(object, "type");
    }
    member.card_type =
        card_type == nullptr ? 0 : static_cast<int>(as_int(*card_type, child(path, "card_type")));
    member.rarity = static_cast<int>(int_default(object, "rarity", 0, child(path, "rarity")));
    member.trained =
        as_triple(member_ptr(object, "trained"), child(path, "trained"), Triple{0, 0, 0}, false);
    member.event_bonus_bp = int_default(object, "event_bonus_bp", 0, child(path, "event_bonus_bp"));
    member.tags = int_array_or_empty<int>(object, "tags", child(path, "tags"));
    member.live_skill = int_default(object, "live_skill", 0, child(path, "live_skill"));
    member.leader_skill = int_default(object, "leader_skill", 0, child(path, "leader_skill"));
    for (const char *field : {"live_skill_level", "leader_skill_level"}) {
        if (const Json *entry = member_ptr(object, field)) {
            int64_t level = as_int(*entry, child(path, field));
            if (level < 1 || level > std::numeric_limits<int>::max())
                fail(child(path, field), "技能等级必须为正整数");
            if (std::string(field) == "live_skill_level")
                member.live_skill_level = static_cast<int>(level);
            else
                member.leader_skill_level = static_cast<int>(level);
        }
    }
    member.gekisou_skill = int_default(object, "gekisou_skill", 0, child(path, "gekisou_skill"));
    member.card_rank_bonus_bp = parse_card_rank_bonus(member_ptr(object, "card_rank_bonus_bp"),
                                                      child(path, "card_rank_bonus_bp"));
    member.level = static_cast<int>(int_default(object, "level", 0, child(path, "level")));
    member.rank = static_cast<int>(int_default(object, "rank", 0, child(path, "rank")));
    member.awake = static_cast<int>(int_default(object, "awake", 0, child(path, "awake")));
    return member;
}

Snapshot parse_snapshot(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "id") == nullptr) {
        fail(child(path, "id"), "缺少 Snapshot ID");
    }
    if (member_ptr(object, "trained") == nullptr) {
        fail(child(path, "trained"), "缺少 trained 三维百分比（bp）");
    }
    Snapshot snapshot;
    snapshot.id = as_int(*member_ptr(object, "id"), child(path, "id"));
    snapshot.name = str_or(object, "name", "", child(path, "name"));
    snapshot.title = str_or(object, "title", "", child(path, "title"));
    snapshot.characters = int_array_or_empty<int>(object, "characters", child(path, "characters"));
    snapshot.bands = int_array_or_empty<int>(object, "bands", child(path, "bands"));
    const Json *card_type = member_ptr(object, "card_type");
    if (card_type == nullptr) {
        card_type = member_ptr(object, "type");
    }
    snapshot.card_type =
        card_type == nullptr ? 0 : static_cast<int>(as_int(*card_type, child(path, "card_type")));
    snapshot.rarity = static_cast<int>(int_default(object, "rarity", 0, child(path, "rarity")));
    snapshot.trained =
        as_triple(member_ptr(object, "trained"), child(path, "trained"), Triple{0, 0, 0}, false);
    snapshot.event_bonus_bp =
        int_default(object, "event_bonus_bp", 0, child(path, "event_bonus_bp"));
    snapshot.rank = static_cast<int>(int_default(object, "rank", 0, child(path, "rank")));
    snapshot.level = static_cast<int>(int_default(object, "level", 0, child(path, "level")));
    snapshot.support_skills =
        int_array_or_empty<int64_t>(object, "support_skills", child(path, "support_skills"));
    if (const Json *levels = member_ptr(object, "support_skill_levels")) {
        const Json mapping = as_mapping(*levels, child(path, "support_skill_levels"));
        for (const auto &entry : mapping.fields()) {
            int64_t id = 0;
            try {
                size_t used = 0;
                id = std::stoll(entry.first, &used);
                if (used != entry.first.size() || id < 1)
                    throw std::invalid_argument("skill ID");
            } catch (const std::exception &) {
                fail(child(path, "support_skill_levels"), "技能 ID 必须为正整数");
            }
            int64_t level =
                as_int(entry.second, child(path, "support_skill_levels." + entry.first));
            if (level < 1 || level > std::numeric_limits<int>::max())
                fail(child(path, "support_skill_levels." + entry.first), "技能等级必须为正整数");
            if (std::find(snapshot.support_skills.begin(), snapshot.support_skills.end(), id) ==
                snapshot.support_skills.end())
                fail(child(path, "support_skill_levels"), "等级配置引用了 Snapshot 未装备的技能");
            snapshot.support_skill_levels[id] = static_cast<int>(level);
        }
    }
    snapshot.card_rank_bonus_bp = parse_card_rank_bonus(member_ptr(object, "card_rank_bonus_bp"),
                                                        child(path, "card_rank_bonus_bp"));
    return snapshot;
}

Fix parse_fix(const Json *data, const Path &path) {
    Fix fix;
    if (data == nullptr || data->is_null()) {
        return fix;
    }
    const Json &object = as_mapping(*data, path);
    const Json *character_rank = member_ptr(object, "character_rank_bonus");
    const Json *total_rank = member_ptr(object, "character_total_rank_bonus");
    const Json *band_item = member_ptr(object, "band_item_bonus_bp");
    const Json *vip = member_ptr(object, "vip_bonus_bp");
    fix.character_rank_bonus =
        as_triple(character_rank, child(path, "character_rank_bonus"), Triple{0, 0, 0}, true);
    fix.character_total_rank_bonus =
        as_triple(total_rank, child(path, "character_total_rank_bonus"), Triple{0, 0, 0}, true);
    fix.band_item_bonus_bp =
        as_triple(band_item, child(path, "band_item_bonus_bp"), Triple{0, 0, 0}, true);
    if (const Json *by_character = member_ptr(object, "character_rank_bonus_by_character")) {
        const Json character_map =
            as_mapping(*by_character, child(path, "character_rank_bonus_by_character"));
        for (const auto &entry : character_map.fields()) {
            int character = 0;
            try {
                size_t used = 0;
                character = std::stoi(entry.first, &used);
                if (used != entry.first.size() || character < 1)
                    throw std::invalid_argument("character");
            } catch (const std::exception &) {
                fail(child(path, "character_rank_bonus_by_character"), "角色 ID 必须为正整数");
            }
            fix.character_rank_bonus_by_character[character] = as_triple(
                &entry.second, child(path, "character_rank_bonus_by_character." + entry.first),
                Triple{0, 0, 0}, true);
        }
    }
    if (const Json *by_band = member_ptr(object, "band_item_bonus_bp_by_band")) {
        const Json band_map = as_mapping(*by_band, child(path, "band_item_bonus_bp_by_band"));
        for (const auto &entry : band_map.fields()) {
            int band = 0;
            try {
                size_t used = 0;
                band = std::stoi(entry.first, &used);
                if (used != entry.first.size() || band < 1)
                    throw std::invalid_argument("band");
            } catch (const std::exception &) {
                fail(child(path, "band_item_bonus_bp_by_band"), "乐队 ID 必须为正整数");
            }
            fix.band_item_bonus_bp_by_band[band] =
                as_triple(&entry.second, child(path, "band_item_bonus_bp_by_band." + entry.first),
                          Triple{0, 0, 0}, true);
        }
    }
    fix.vip_bonus_bp = as_triple(vip, child(path, "vip_bonus_bp"), Triple{0, 0, 0}, true);
    fix.type_link_base_bp =
        int_default(object, "type_link_base_bp", 0, child(path, "type_link_base_bp"));
    fix.music_type_base_bp =
        int_default(object, "music_type_base_bp", 0, child(path, "music_type_base_bp"));
    fix.music_tag_base_bp =
        int_default(object, "music_tag_base_bp", 0, child(path, "music_tag_base_bp"));
    return fix;
}

SkillEffect parse_skill_effect(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "value") == nullptr) {
        fail(child(path, "value"), "缺少效果数值");
    }
    SkillEffect effect;
    effect.value = as_int(*member_ptr(object, "value"), child(path, "value"));
    effect.level = static_cast<int>(int_default(object, "level", 0, child(path, "level")));
    effect.effect_type =
        static_cast<int>(int_default(object, "effect_type", 0, child(path, "effect_type")));
    effect.targets = int_array_or_empty<int64_t>(object, "targets", child(path, "targets"));
    effect.condition_group =
        static_cast<int>(int_default(object, "condition_group", 0, child(path, "condition_group")));
    return effect;
}

Skill parse_skill(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "id") == nullptr) {
        fail(child(path, "id"), "缺少技能 ID");
    }
    Skill skill;
    skill.id = as_int(*member_ptr(object, "id"), child(path, "id"));
    const Path effects_path = child(path, "effects");
    const Json &effects = list_or_empty(object, "effects", effects_path);
    for (std::size_t i = 0; i < effects.size(); ++i) {
        skill.effects.push_back(parse_skill_effect(effects.at(i), indexed(effects_path, i)));
    }
    skill.categories = int_array_or_empty<int>(object, "categories", child(path, "categories"));
    skill.mission_type =
        static_cast<int>(int_default(object, "mission_type", 0, child(path, "mission_type")));
    return skill;
}

Target parse_target(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "id") == nullptr) {
        fail(child(path, "id"), "缺少目标 ID");
    }
    Target target;
    target.id = as_int(*member_ptr(object, "id"), child(path, "id"));
    target.character =
        static_cast<int>(int_default(object, "character", 0, child(path, "character")));
    target.band = static_cast<int>(int_default(object, "band", 0, child(path, "band")));
    target.card_type =
        static_cast<int>(int_default(object, "card_type", 0, child(path, "card_type")));
    target.tag = static_cast<int>(int_default(object, "tag", 0, child(path, "tag")));
    target.gekisou_mission_type = static_cast<int>(
        int_default(object, "gekisou_mission_type", 0, child(path, "gekisou_mission_type")));
    target.live_skill_categories = int_array_or_empty<int>(object, "live_skill_categories",
                                                           child(path, "live_skill_categories"));
    target.gekisou_skill_categories = int_array_or_empty<int>(
        object, "gekisou_skill_categories", child(path, "gekisou_skill_categories"));
    return target;
}

Condition parse_condition(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "id") == nullptr) {
        fail(child(path, "id"), "缺少条件 ID");
    }
    Condition condition;
    condition.id = as_int(*member_ptr(object, "id"), child(path, "id"));
    condition.type = static_cast<int>(int_default(object, "type", 0, child(path, "type")));
    condition.positive = truthy_default(object, "positive", true);
    condition.targets = int_array_or_empty<int64_t>(object, "targets", child(path, "targets"));
    condition.values = int_array_or_empty<int64_t>(object, "values", child(path, "values"));
    return condition;
}

ConditionGroup parse_condition_group(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "group") == nullptr) {
        fail(child(path, "group"), "缺少条件组 ID");
    }
    ConditionGroup group;
    group.group = static_cast<int>(as_int(*member_ptr(object, "group"), child(path, "group")));
    const Path rows_path = child(path, "rows");
    const Json &rows = list_or_empty(object, "rows", rows_path);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        group.rows.push_back(int_list<int64_t>(rows.at(i), indexed(rows_path, i)));
    }
    return group;
}

NoteParameter parse_note_parameter(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "op") == nullptr) {
        fail(child(path, "op"), "缺少音符操作类型");
    }
    NoteParameter parameter;
    parameter.op = static_cast<int>(as_int(*member_ptr(object, "op"), child(path, "op")));
    parameter.score_percent = num_default(object, "score_percent", 0, child(path, "score_percent"));
    return parameter;
}

ComboBonus parse_combo_bonus(const Json &data, const Path &path) {
    const Json &object = as_mapping(data, path);
    if (member_ptr(object, "required_combo_count") == nullptr) {
        fail(child(path, "required_combo_count"), "缺少连击门槛");
    }
    ComboBonus bonus;
    bonus.required_combo_count = static_cast<int>(
        as_int(*member_ptr(object, "required_combo_count"), child(path, "required_combo_count")));
    bonus.factor = num_default(object, "factor", 0, child(path, "factor"));
    bonus.type = static_cast<int>(int_default(object, "type", 0, child(path, "type")));
    return bonus;
}

template <typename T, typename Parse>
std::vector<T> parse_list_field(const Json &object, const char *key, const Path &path,
                                Parse parse) {
    const Path field_path = child(path, key);
    const Json &array = list_or_empty(object, key, field_path);
    std::vector<T> out;
    out.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        out.push_back(parse(array.at(i), indexed(field_path, i)));
    }
    return out;
}

Catalog parse_catalog(const Json *data, const Path &path) {
    const Json &object = as_mapping(object_or_empty(data), path);
    Catalog catalog;
    catalog.members = parse_list_field<Member>(object, "members", path, parse_member);
    catalog.snapshots = parse_list_field<Snapshot>(object, "snapshots", path, parse_snapshot);
    catalog.live_skills = parse_list_field<Skill>(object, "live_skills", path, parse_skill);
    catalog.leader_skills = parse_list_field<Skill>(object, "leader_skills", path, parse_skill);
    catalog.support_skills = parse_list_field<Skill>(object, "support_skills", path, parse_skill);
    catalog.gekisou_skills = parse_list_field<Skill>(object, "gekisou_skills", path, parse_skill);
    catalog.targets = parse_list_field<Target>(object, "targets", path, parse_target);
    catalog.conditions = parse_list_field<Condition>(object, "conditions", path, parse_condition);
    catalog.condition_groups =
        parse_list_field<ConditionGroup>(object, "condition_groups", path, parse_condition_group);
    catalog.note_parameters =
        parse_list_field<NoteParameter>(object, "note_parameters", path, parse_note_parameter);
    catalog.combo_bonuses =
        parse_list_field<ComboBonus>(object, "combo_bonuses", path, parse_combo_bonus);
    catalog.fix = parse_fix(member_ptr(object, "fix"), child(path, "fix"));
    return catalog;
}

Constraints parse_constraints(const Json *data, const Path &path) {
    const Json &object = as_mapping(object_or_empty(data), path);
    Constraints out;
    out.distinct_characters = truthy_default(object, "distinct_characters", true);
    out.distinct_snapshots = truthy_default(object, "distinct_snapshots", true);
    const Json *member_pool = member_ptr(object, "member_pool");
    if (member_pool != nullptr && !member_pool->is_null()) {
        out.member_pool = int_list<int64_t>(*member_pool, child(path, "member_pool"));
        out.has_member_pool = true;
    }
    const Json *snapshot_pool = member_ptr(object, "snapshot_pool");
    if (snapshot_pool != nullptr && !snapshot_pool->is_null()) {
        out.snapshot_pool = int_list<int64_t>(*snapshot_pool, child(path, "snapshot_pool"));
        out.has_snapshot_pool = true;
    }
    const Json *leader_pool = member_ptr(object, "leader_pool");
    if (leader_pool != nullptr && !leader_pool->is_null()) {
        out.leader_pool = int_list<int64_t>(*leader_pool, child(path, "leader_pool"));
        out.has_leader_pool = true;
    }
    out.banned_members =
        int_array_or_empty<int64_t>(object, "banned_members", child(path, "banned_members"));
    out.banned_snapshots =
        int_array_or_empty<int64_t>(object, "banned_snapshots", child(path, "banned_snapshots"));
    out.required_members =
        int_array_or_empty<int64_t>(object, "required_members", child(path, "required_members"));
    out.required_snapshots = int_array_or_empty<int64_t>(object, "required_snapshots",
                                                         child(path, "required_snapshots"));
    return out;
}

SearchSettings parse_search(const Json *data, const Path &path) {
    const Json &object = as_mapping(object_or_empty(data), path);
    SearchSettings out;
    const Json *mode = member_ptr(object, "mode");
    if (mode != nullptr) {
        if (!is_string_in(*mode, {"rank", "score"})) {
            fail(child(path, "mode"), "只支持 'rank' / 'score'");
        }
        out.mode = mode->as_string();
    }
    out.top = static_cast<int>(int_default(object, "top", 20, child(path, "top")));
    if (out.top < 1) {
        fail(child(path, "top"), "必须 >= 1");
    }
    const Json *order = member_ptr(object, "order_search");
    if (order != nullptr) {
        if (!is_string_in(*order, {"exact", "given"})) {
            fail(child(path, "order_search"), "只支持 'exact' / 'given'");
        }
        out.order_search = order->as_string();
    }
    out.time_limit_s = num_default(object, "time_limit_s", 60, child(path, "time_limit_s"));
    out.prescreen = truthy_default(object, "prescreen", true);
    out.seed = int_default(object, "seed", 0, child(path, "seed"));
    out.verbose = truthy_default(object, "verbose", false);
    return out;
}

Json triple_json(const Triple &value) {
    Json array = Json::array();
    for (int64_t item : value) {
        array.push_back(Json(item));
    }
    return array;
}

Json int64_array_json(const std::vector<int64_t> &values) {
    Json array = Json::array();
    for (int64_t item : values) {
        array.push_back(Json(item));
    }
    return array;
}

Json int_array_json(const std::vector<int> &values) {
    Json array = Json::array();
    for (int item : values) {
        array.push_back(Json(item));
    }
    return array;
}

Json settings_json(const Settings &settings) {
    Json axes = Json::object();
    for (const auto &entry : settings.leader_effect_axes) {
        axes.set(std::to_string(entry.first), Json(entry.second));
    }
    Json select = Json::object();
    select.set("effect_type", settings.skill.live_effect_type.has_value()
                                  ? Json(static_cast<int64_t>(*settings.skill.live_effect_type))
                                  : Json(nullptr));
    select.set("branch", Json(settings.skill.live_effect_branch));
    Json skill = Json::object();
    skill.set("overlap", Json(settings.skill.overlap));
    skill.set("activation", Json(settings.skill.activation));
    skill.set("live_effect_select", select);

    Json sources = Json::object();
    sources.set("snapshot", Json(settings.power.snapshot));
    sources.set("type_link", Json(settings.power.type_link));
    sources.set("band_item", Json(settings.power.band_item));
    sources.set("music_type", Json(settings.power.music_type));
    sources.set("music_tag", Json(settings.power.music_tag));
    sources.set("leader", Json(settings.power.leader));
    sources.set("vip", Json(settings.power.vip));
    Json extra = Json::array();
    for (const ExtraSource &source : settings.power.extra_sources) {
        Json entry = Json::object();
        entry.set("id", Json(source.id));
        entry.set("rate_bp", triple_json(source.rate_bp));
        entry.set("scope", Json(source.scope));
        if (source.kind != "percent") {
            entry.set("kind", Json(source.kind));
        }
        entry.set("when", source.when.is_object() ? source.when : Json::object());
        extra.push_back(entry);
    }
    Json power = Json::object();
    power.set("rounding", Json(settings.power.rounding));
    power.set("sources", sources);
    power.set("extra_sources", extra);

    Json score = Json::object();
    score.set("adjustment_factor", Json(settings.score.adjustment_factor));
    score.set("level_base", Json(settings.score.level_base));
    score.set("level_alpha", settings.score.level_alpha.has_value()
                                 ? Json(*settings.score.level_alpha)
                                 : Json(nullptr));
    score.set("rounding", Json(settings.score.rounding));

    Json factors = Json::object();
    for (const auto &entry : settings.judgement.factors) {
        factors.set(entry.first, Json(entry.second));
    }
    Json distribution = Json(nullptr);
    if (settings.judgement.has_distribution) {
        distribution = Json::object();
        for (const auto &entry : settings.judgement.distribution) {
            distribution.set(entry.first, Json(entry.second));
        }
    }
    Json judgement = Json::object();
    judgement.set("mode", Json(settings.judgement.mode));
    judgement.set("factors", factors);
    judgement.set("default_factor", Json(settings.judgement.default_factor));
    judgement.set("fixed_label", Json(settings.judgement.fixed_label));
    Json sequence = Json::array();
    for (const Json &item : settings.judgement.sequence) {
        sequence.push_back(item);
    }
    judgement.set("sequence", sequence);
    judgement.set("distribution", distribution);
    judgement.set("seed", Json(settings.judgement.seed));

    Json damage = Json(nullptr);
    if (!settings.life.damage.empty()) {
        damage = Json::object();
        for (const auto &entry : settings.life.damage) {
            damage.set(entry.first, Json(entry.second));
        }
    }
    Json life = Json::object();
    life.set("mode", Json(settings.life.mode));
    life.set("initial", Json(settings.life.initial));
    life.set("onus_factor", Json(settings.life.onus_factor));
    life.set("damage", damage);
    life.set("floor", Json(settings.life.floor));

    Json assist = Json::object();
    assist.set("enabled", Json(settings.assist.enabled));
    assist.set("score_percent", Json(settings.assist.score_percent));

    Json sections = Json::array();
    for (const GekisouSection &section : settings.gekisou.sections) {
        Json item = Json::object();
        item.set("start_ms", Json(section.start_ms));
        item.set("end_ms", section.end_ms.has_value() ? Json(*section.end_ms) : Json(nullptr));
        item.set("bonus_bp", Json(section.bonus_bp));
        item.set("label", Json(section.label));
        sections.push_back(item);
    }
    Json gekisou = Json::object();
    gekisou.set("enabled", Json(settings.gekisou.enabled));
    gekisou.set("sections", sections);

    Json out = Json::object();
    out.set("team_size", Json(settings.team_size));
    out.set("combo_type", Json(settings.combo_type));
    out.set("base_duration_ms", Json(settings.base_duration_ms));
    out.set("live_level", Json(settings.live_level));
    out.set("leader_level", Json(settings.leader_level));
    out.set("support_level", Json(settings.support_level));
    out.set("duration_extension_effect_type", Json(settings.duration_extension_effect_type));
    out.set("type_link_bonus_source", Json(settings.type_link_bonus_source));
    out.set("leader_unmapped_effect_types", Json(settings.leader_unmapped_effect_types));
    out.set("leader_effect_axes", axes);
    out.set("skill", skill);
    out.set("power_model", power);
    out.set("score_model", score);
    out.set("judgement", judgement);
    out.set("life", life);
    out.set("assist", assist);
    out.set("gekisou", gekisou);
    return out;
}

const Member *find_member(const Problem &problem, int64_t id) {
    for (const Member &member : problem.catalog.members) {
        if (member.id == id) {
            return &member;
        }
    }
    return nullptr;
}

}

const Json *opt(const Json &object, const char *key) {
    const Json *value = member_ptr(object, key);
    if (value == nullptr || value->is_null()) {
        return nullptr;
    }
    return value;
}

int64_t int_field(const Json &object, const char *key, int64_t fallback) {
    return int_default(object, key, fallback, key);
}

double num_field(const Json &object, const char *key, double fallback) {
    return num_default(object, key, fallback, key);
}

bool bool_field(const Json &object, const char *key, bool fallback) {
    return bool_default(object, key, fallback, key);
}

std::string str_field(const Json &object, const char *key, const std::string &fallback) {
    return str_default(object, key, fallback, key);
}

std::vector<int> int_array_field(const Json &object, const char *key) {
    return int_array_or_empty<int>(object, key, key);
}

std::vector<int64_t> int64_array_field(const Json &object, const char *key) {
    return int_array_or_empty<int64_t>(object, key, key);
}

Triple triple_field(const Json &object, const char *key, Triple fallback) {
    return as_triple(member_ptr(object, key), key, fallback, true);
}

Problem parse_problem(const Json &document) {
    const Path root = "problem";
    const Json &data = as_mapping(document, root);
    const Json *schema = member_ptr(data, "schema");
    if (schema != nullptr && !is_string_in(*schema, {kSchemaProblem})) {
        fail("problem.schema",
             "不支持的版本 " + json_repr(*schema) + "，需要 " + std::string(kSchemaProblem) + "");
    }
    Problem problem;
    problem.song = parse_song(object_or_empty(member_ptr(data, "song")), "song");
    problem.chart = parse_chart(member_ptr(data, "chart"), "chart");
    problem.catalog = parse_catalog(member_ptr(data, "catalog"), "catalog");
    problem.settings = parse_settings(member_ptr(data, "settings"), "settings");
    problem.constraints = parse_constraints(member_ptr(data, "constraints"), "constraints");
    problem.search = parse_search(member_ptr(data, "search"), "search");
    problem.raw = data;
    problem.validate();
    return problem;
}

void Problem::validate() const {
    const int team_size = settings.team_size;
    if (static_cast<int>(chart.skill_times_ms.size()) != team_size) {
        fail("chart.skill_times_ms", "长度必须是 team_size=" + std::to_string(team_size) +
                                         "，实际是 " + std::to_string(chart.skill_times_ms.size()));
    }
    std::vector<int64_t> member_ids;
    member_ids.reserve(catalog.members.size());
    for (const Member &member : catalog.members) {
        member_ids.push_back(member.id);
    }
    std::vector<int64_t> snapshot_ids;
    snapshot_ids.reserve(catalog.snapshots.size());
    for (const Snapshot &snapshot : catalog.snapshots) {
        snapshot_ids.push_back(snapshot.id);
    }
    const std::pair<const char *, const std::vector<int64_t> *> id_lists[] = {
        {"members", &member_ids},
        {"snapshots", &snapshot_ids},
    };
    for (const auto &entry : id_lists) {
        std::set<int64_t> seen;
        for (int64_t value : *entry.second) {
            if (!seen.insert(value).second) {
                fail(std::string("catalog.") + entry.first, "ID 重复: " + std::to_string(value));
            }
        }
    }
    if (member_ids.empty()) {
        fail("catalog.members", "卡池为空");
    }
    if (snapshot_ids.empty()) {
        fail("catalog.snapshots", "Snapshot 卡池为空");
    }

    std::set<int> params;
    for (const NoteParameter &parameter : catalog.note_parameters) {
        params.insert(parameter.op);
    }
    std::set<int> missing;
    for (const Note &note : chart.notes) {
        if (note.scoring && params.find(note.op) == params.end()) {
            missing.insert(note.op);
        }
    }
    if (!missing.empty()) {
        std::vector<int64_t> missing_list(missing.begin(), missing.end());
        fail("catalog.note_parameters",
             "谱面使用的 op " + int_list_repr(missing_list) + " 缺少 score_percent 定义");
    }

    const std::set<int64_t> known_members(member_ids.begin(), member_ids.end());
    const std::set<int64_t> known_snapshots(snapshot_ids.begin(), snapshot_ids.end());
    const std::pair<const char *, const std::vector<int64_t> *> member_refs[] = {
        {"member_pool", &constraints.member_pool},
        {"banned_members", &constraints.banned_members},
        {"required_members", &constraints.required_members},
        {"leader_pool", &constraints.leader_pool},
    };
    for (const auto &entry : member_refs) {
        for (int64_t value : *entry.second) {
            if (known_members.find(value) == known_members.end()) {
                fail(std::string("constraints.") + entry.first,
                     "未知成员 ID " + std::to_string(value));
            }
        }
    }
    const std::pair<const char *, const std::vector<int64_t> *> snapshot_refs[] = {
        {"snapshot_pool", &constraints.snapshot_pool},
        {"banned_snapshots", &constraints.banned_snapshots},
        {"required_snapshots", &constraints.required_snapshots},
    };
    for (const auto &entry : snapshot_refs) {
        for (int64_t value : *entry.second) {
            if (known_snapshots.find(value) == known_snapshots.end()) {
                fail(std::string("constraints.") + entry.first,
                     "未知 Snapshot ID " + std::to_string(value));
            }
        }
    }

    const std::size_t available_member_count = available_members().size();
    if (static_cast<int>(available_member_count) < team_size) {
        fail("constraints", "可用成员 " + std::to_string(available_member_count) +
                                " 少于 team_size=" + std::to_string(team_size));
    }
    const std::size_t available_snapshot_count = available_snapshots().size();
    if (static_cast<int>(available_snapshot_count) < team_size) {
        fail("constraints", "可用 Snapshot " + std::to_string(available_snapshot_count) +
                                " 少于 team_size=" + std::to_string(team_size));
    }

    std::set<int64_t> targets;
    for (const Target &target : catalog.targets) {
        targets.insert(target.id);
    }
    std::vector<const Skill *> referenced = {};
    for (const Skill &skill : catalog.leader_skills) {
        referenced.push_back(&skill);
    }
    for (const Skill &skill : catalog.live_skills) {
        referenced.push_back(&skill);
    }
    for (const Skill &skill : catalog.support_skills) {
        referenced.push_back(&skill);
    }
    for (const Skill *skill : referenced) {
        for (const SkillEffect &effect : skill->effects) {
            for (int64_t target_id : effect.targets) {
                if (targets.find(target_id) == targets.end()) {
                    fail("catalog.targets", "技能 " + std::to_string(skill->id) +
                                                " 引用了未知目标 " + std::to_string(target_id));
                }
            }
        }
    }

    std::set<int> groups;
    for (const ConditionGroup &group : catalog.condition_groups) {
        groups.insert(group.group);
    }
    std::set<int64_t> conditions;
    for (const Condition &condition : catalog.conditions) {
        conditions.insert(condition.id);
    }
    for (const ConditionGroup &group : catalog.condition_groups) {
        for (const std::vector<int64_t> &row : group.rows) {
            for (int64_t condition_id : row) {
                if (conditions.find(condition_id) == conditions.end()) {
                    fail("catalog.conditions", "条件组 " + std::to_string(group.group) +
                                                   " 引用了未知条件 " +
                                                   std::to_string(condition_id));
                }
            }
        }
    }
    for (const Skill &skill : catalog.support_skills) {
        for (const SkillEffect &effect : skill.effects) {
            if (effect.condition_group != 0 &&
                groups.find(effect.condition_group) == groups.end()) {
                fail("catalog.condition_groups", "支援技能 " + std::to_string(skill.id) +
                                                     " 引用了未知条件组 " +
                                                     std::to_string(effect.condition_group));
            }
        }
    }

    std::set<int64_t> live_skill_ids;
    for (const Skill &skill : catalog.live_skills) {
        live_skill_ids.insert(skill.id);
    }
    std::set<int64_t> leader_skill_ids;
    for (const Skill &skill : catalog.leader_skills) {
        leader_skill_ids.insert(skill.id);
    }
    std::set<int64_t> gekisou_skill_ids;
    for (const Skill &skill : catalog.gekisou_skills) {
        gekisou_skill_ids.insert(skill.id);
    }
    std::set<int64_t> support_skill_ids;
    for (const Skill &skill : catalog.support_skills) {
        support_skill_ids.insert(skill.id);
    }
    for (const Member &member : catalog.members) {
        if (member.live_skill != 0 &&
            live_skill_ids.find(member.live_skill) == live_skill_ids.end()) {
            fail("catalog.live_skills", "成员 " + std::to_string(member.id) +
                                            " 引用了未知 LIVE 技能 " +
                                            std::to_string(member.live_skill));
        }
        if (member.leader_skill != 0 &&
            leader_skill_ids.find(member.leader_skill) == leader_skill_ids.end()) {
            fail("catalog.leader_skills", "成员 " + std::to_string(member.id) +
                                              " 引用了未知队长技能 " +
                                              std::to_string(member.leader_skill));
        }
        if (member.gekisou_skill != 0 &&
            gekisou_skill_ids.find(member.gekisou_skill) == gekisou_skill_ids.end()) {
            fail("catalog.gekisou_skills", "成员 " + std::to_string(member.id) +
                                               " 引用了未知撃奏技能 " +
                                               std::to_string(member.gekisou_skill));
        }
    }
    for (const Snapshot &snapshot : catalog.snapshots) {
        for (int64_t skill_id : snapshot.support_skills) {
            if (skill_id != 0 && support_skill_ids.find(skill_id) == support_skill_ids.end()) {
                fail("catalog.support_skills", "Snapshot " + std::to_string(snapshot.id) +
                                                   " 引用了未知支援技能 " +
                                                   std::to_string(skill_id));
            }
        }
    }
}

std::vector<const Member *> Problem::available_members() const {
    const std::set<int64_t> banned(constraints.banned_members.begin(),
                                   constraints.banned_members.end());
    const std::set<int64_t> allowed(constraints.member_pool.begin(), constraints.member_pool.end());
    std::vector<const Member *> out;
    out.reserve(catalog.members.size());
    for (const Member &member : catalog.members) {
        if (banned.find(member.id) != banned.end()) {
            continue;
        }
        if (constraints.has_member_pool && allowed.find(member.id) == allowed.end()) {
            continue;
        }
        out.push_back(&member);
    }
    return out;
}

std::vector<const Snapshot *> Problem::available_snapshots() const {
    const std::set<int64_t> banned(constraints.banned_snapshots.begin(),
                                   constraints.banned_snapshots.end());
    const std::set<int64_t> allowed(constraints.snapshot_pool.begin(),
                                    constraints.snapshot_pool.end());
    std::vector<const Snapshot *> out;
    out.reserve(catalog.snapshots.size());
    for (const Snapshot &snapshot : catalog.snapshots) {
        if (banned.find(snapshot.id) != banned.end()) {
            continue;
        }
        if (constraints.has_snapshot_pool && allowed.find(snapshot.id) == allowed.end()) {
            continue;
        }
        out.push_back(&snapshot);
    }
    return out;
}

std::vector<const Member *> Problem::leader_candidates() const {
    const std::set<int64_t> allowed(constraints.leader_pool.begin(), constraints.leader_pool.end());
    std::vector<const Member *> out;
    for (const Member *member : available_members()) {
        if (constraints.has_leader_pool && allowed.find(member->id) == allowed.end()) {
            continue;
        }
        out.push_back(member);
    }
    return out;
}

Json Problem::describe() const {
    Json song_json = Json::object();
    song_json.set("id", Json(song.id));
    song_json.set("title", Json(song.title));
    song_json.set("type", Json(song.type));
    song_json.set("bands", int_array_json(song.bands));
    song_json.set("tags", int_array_json(song.tags));

    std::set<int> ops;
    int scoring_notes = 0;
    for (const Note &note : chart.notes) {
        if (note.scoring) {
            ++scoring_notes;
            ops.insert(note.op);
        }
    }
    Json chart_json = Json::object();
    chart_json.set("difficulty", Json(chart.difficulty));
    chart_json.set("level", Json(chart.level));
    chart_json.set("notes", Json(static_cast<int64_t>(chart.notes.size())));
    chart_json.set("scoring_notes", Json(static_cast<int64_t>(scoring_notes)));
    Json ops_json = Json::array();
    for (int op : ops) {
        ops_json.push_back(Json(op));
    }
    chart_json.set("ops", ops_json);
    chart_json.set("skill_times_ms", int64_array_json(chart.skill_times_ms));

    Json catalog_json = Json::object();
    catalog_json.set("members", Json(static_cast<int64_t>(catalog.members.size())));
    catalog_json.set("snapshots", Json(static_cast<int64_t>(catalog.snapshots.size())));
    catalog_json.set("live_skills", Json(static_cast<int64_t>(catalog.live_skills.size())));
    catalog_json.set("leader_skills", Json(static_cast<int64_t>(catalog.leader_skills.size())));
    catalog_json.set("support_skills", Json(static_cast<int64_t>(catalog.support_skills.size())));
    catalog_json.set("targets", Json(static_cast<int64_t>(catalog.targets.size())));
    catalog_json.set("condition_groups",
                     Json(static_cast<int64_t>(catalog.condition_groups.size())));

    Json available = Json::object();
    available.set("members", Json(static_cast<int64_t>(available_members().size())));
    available.set("snapshots", Json(static_cast<int64_t>(available_snapshots().size())));
    available.set("leaders", Json(static_cast<int64_t>(leader_candidates().size())));

    Json constraints_json = Json::object();
    constraints_json.set("distinct_characters", Json(constraints.distinct_characters));
    constraints_json.set("distinct_snapshots", Json(constraints.distinct_snapshots));
    constraints_json.set("member_pool", constraints.has_member_pool
                                            ? int64_array_json(constraints.member_pool)
                                            : Json(nullptr));
    constraints_json.set("snapshot_pool", constraints.has_snapshot_pool
                                              ? int64_array_json(constraints.snapshot_pool)
                                              : Json(nullptr));
    constraints_json.set("leader_pool", constraints.has_leader_pool
                                            ? int64_array_json(constraints.leader_pool)
                                            : Json(nullptr));
    constraints_json.set("banned_members", int64_array_json(constraints.banned_members));
    constraints_json.set("banned_snapshots", int64_array_json(constraints.banned_snapshots));
    constraints_json.set("required_members", int64_array_json(constraints.required_members));
    constraints_json.set("required_snapshots", int64_array_json(constraints.required_snapshots));

    Json search = Json::object();
    search.set("mode", Json(this->search.mode));
    search.set("top", Json(this->search.top));
    search.set("time_limit_s", Json(this->search.time_limit_s));
    search.set("order_search", Json(this->search.order_search));
    search.set("prescreen", Json(this->search.prescreen));
    search.set("seed", Json(this->search.seed));
    search.set("verbose", Json(this->search.verbose));

    Json out = Json::object();
    out.set("song", song_json);
    out.set("chart", chart_json);
    out.set("catalog", catalog_json);
    out.set("available", available);
    out.set("settings", settings_json(settings));
    out.set("constraints", constraints_json);
    out.set("search", search);
    return out;
}

Formation parse_formation(const Json &document) {
    const Path root = "formation";
    const Json &data = as_mapping(document, root);
    const Json *schema = member_ptr(data, "schema");
    if (schema != nullptr && !is_string_in(*schema, {kSchemaFormation})) {
        fail("formation.schema",
             "不支持的版本 " + json_repr(*schema) + "，需要 " + std::string(kSchemaFormation) + "");
    }
    if (member_ptr(data, "leader") == nullptr) {
        fail("formation.leader", "缺少队长成员 ID");
    }
    const Path slots_path = "formation.slots";
    const Json &slots = list_or_empty(data, "slots", slots_path);
    Formation formation;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const Path spot = indexed(slots_path, i);
        const Json &slot = as_mapping(slots.at(i), spot);
        if (member_ptr(slot, "member") == nullptr || member_ptr(slot, "snapshot") == nullptr) {
            fail(spot, "槽位需要 member 与 snapshot");
        }
        Slot parsed;
        parsed.member = as_int(*member_ptr(slot, "member"), child(spot, "member"));
        parsed.snapshot = as_int(*member_ptr(slot, "snapshot"), child(spot, "snapshot"));
        const int64_t trigger = int_default(slot, "trigger", 0, child(spot, "trigger"));
        parsed.trigger = static_cast<int>(trigger != 0 ? trigger : static_cast<int64_t>(i) + 1);
        formation.slots.push_back(parsed);
    }
    if (formation.slots.empty()) {
        fail(slots_path, "至少需要一个槽位");
    }
    formation.leader = as_int(*member_ptr(data, "leader"), "formation.leader");
    std::stable_sort(
        formation.slots.begin(), formation.slots.end(),
        [](const Slot &left, const Slot &right) { return left.trigger < right.trigger; });
    return formation;
}

Json dump_formation(const Formation &formation) {
    Json slots = Json::array();
    for (const Slot &slot : formation.slots) {
        Json item = Json::object();
        item.set("member", Json(slot.member));
        item.set("snapshot", Json(slot.snapshot));
        item.set("trigger", Json(slot.trigger));
        slots.push_back(item);
    }
    Json out = Json::object();
    out.set("schema", Json(kSchemaFormation));
    out.set("leader", Json(formation.leader));
    out.set("slots", slots);
    return out;
}

void Formation::validate(const Problem &problem) const {
    const int team_size = problem.settings.team_size;
    if (static_cast<int>(slots.size()) != team_size) {
        fail("formation.slots", "槽位数量必须是 " + std::to_string(team_size) + "，实际 " +
                                    std::to_string(slots.size()));
    }
    std::vector<int64_t> triggers;
    triggers.reserve(slots.size());
    for (const Slot &slot : slots) {
        triggers.push_back(slot.trigger);
    }
    std::sort(triggers.begin(), triggers.end());
    bool trigger_ok = static_cast<int>(triggers.size()) == team_size;
    for (int i = 0; trigger_ok && i < team_size; ++i) {
        trigger_ok = triggers[static_cast<std::size_t>(i)] == static_cast<int64_t>(i) + 1;
    }
    if (!trigger_ok) {
        fail("formation.slots", "trigger 必须是 1.." + std::to_string(team_size) +
                                    " 各一次，实际 " + int_list_repr(triggers));
    }
    std::set<int64_t> members;
    for (const Slot &slot : slots) {
        members.insert(slot.member);
    }
    if (members.find(leader) == members.end()) {
        fail("formation.leader", "队长 " + std::to_string(leader) + " 不在编成中");
    }

    std::set<int64_t> available_members;
    for (const Member *member : problem.available_members()) {
        available_members.insert(member->id);
    }
    for (const Slot &slot : slots) {
        if (available_members.find(slot.member) == available_members.end()) {
            fail("formation.slots", "成员 " + std::to_string(slot.member) + " 不在可用卡池中");
        }
    }
    std::set<int64_t> available_snapshots;
    for (const Snapshot *snapshot : problem.available_snapshots()) {
        available_snapshots.insert(snapshot->id);
    }
    for (const Slot &slot : slots) {
        if (available_snapshots.find(slot.snapshot) == available_snapshots.end()) {
            fail("formation.slots",
                 "Snapshot " + std::to_string(slot.snapshot) + " 不在可用卡池中");
        }
    }

    if (problem.constraints.distinct_characters) {
        std::set<int> characters;
        bool distinct = true;
        for (const Slot &slot : slots) {
            const Member *member = find_member(problem, slot.member);
            if (member == nullptr) {
                fail("catalog.members", "未知成员 " + std::to_string(slot.member));
            }
            if (!characters.insert(member->character).second) {
                distinct = false;
            }
        }
        if (!distinct) {
            fail("formation.slots", "编成中出现重复角色（distinct_characters = true）");
        }
    }
    if (problem.constraints.distinct_snapshots) {
        std::set<int64_t> seen;
        bool distinct = true;
        for (const Slot &slot : slots) {
            if (!seen.insert(slot.snapshot).second) {
                distinct = false;
            }
        }
        if (!distinct) {
            fail("formation.slots", "编成中出现重复 Snapshot（distinct_snapshots = true）");
        }
    }
    for (int64_t member_id : problem.constraints.required_members) {
        if (members.find(member_id) == members.end()) {
            fail("formation.slots", "缺少必需成员 " + std::to_string(member_id));
        }
    }
    std::set<int64_t> slot_snapshots;
    for (const Slot &slot : slots) {
        slot_snapshots.insert(slot.snapshot);
    }
    for (int64_t snapshot_id : problem.constraints.required_snapshots) {
        if (slot_snapshots.find(snapshot_id) == slot_snapshots.end()) {
            fail("formation.slots", "缺少必需 Snapshot " + std::to_string(snapshot_id));
        }
    }
}

std::vector<int64_t> Formation::members() const {
    std::vector<int64_t> out;
    out.reserve(slots.size());
    for (const Slot &slot : slots) {
        out.push_back(slot.member);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}
