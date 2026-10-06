#include "engine.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace deckcalc {
namespace {

constexpr int kDims = 3;
constexpr size_t kNumpyPairwiseBlock = 128;
constexpr const char *kAxisNames[kDims] = {"performance", "technique", "visual"};

std::string quote(const std::string &text) {
    return "'" + text + "'";
}

int64_t native_floor32(int64_t product) {
    const float scaled = static_cast<float>(product) / 10000.0f;
    return static_cast<int64_t>(std::floor(scaled));
}

int64_t native_floor64(int64_t product) {
    return static_cast<int64_t>(std::floor(static_cast<double>(product) / 10000.0));
}

int64_t power_floor(int64_t product, bool float32_mode) {
    return float32_mode ? native_floor32(product) : native_floor64(product);
}

bool power_uses_float32(const Settings &settings) {
    return settings.power.rounding == "float32_floor";
}

double numpy_sum(const double *data, size_t count) {
    if (count < 8) {
        double result = 0.0;
        for (size_t i = 0; i < count; ++i) {
            result += data[i];
        }
        return result;
    }
    if (count <= kNumpyPairwiseBlock) {
        double lanes[8] = {data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]};
        size_t i = 8;
        const size_t limit = count - (count % 8);
        for (; i < limit; i += 8) {
            for (size_t lane = 0; lane < 8; ++lane) {
                lanes[lane] += data[i + lane];
            }
        }
        double result = ((lanes[0] + lanes[1]) + (lanes[2] + lanes[3])) +
                        ((lanes[4] + lanes[5]) + (lanes[6] + lanes[7]));
        for (; i < count; ++i) {
            result += data[i];
        }
        return result;
    }
    size_t half = count / 2;
    half -= half % 8;
    return numpy_sum(data, half) + numpy_sum(data + half, count - half);
}

bool contains_int(const std::vector<int> &values, int wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

bool intersects(const std::vector<int> &left, const std::vector<int> &right) {
    for (int value : left) {
        if (contains_int(right, value)) {
            return true;
        }
    }
    return false;
}

uint64_t pack_ids(int64_t first, int64_t second) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(first)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(second));
}

bool axis_index(const std::string &axis, int &out) {
    for (int i = 0; i < kDims; ++i) {
        if (axis == kAxisNames[i]) {
            out = i;
            return true;
        }
    }
    return false;
}

std::string format_double(double value) {
    char buffer[64];
    const std::to_chars_result result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (result.ec == std::errc()) {
        return std::string(buffer, result.ptr);
    }
    return std::to_string(value);
}

std::string python_repr(const Json &value) {
    switch (value.type()) {
    case Json::Type::Null:
        return "None";
    case Json::Type::Bool:
        return value.as_bool() ? "True" : "False";
    case Json::Type::Number: {
        const double number = value.as_double();
        if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 1e15) {
            return std::to_string(static_cast<long long>(number));
        }
        return format_double(number);
    }
    case Json::Type::String:
        return quote(value.as_string());
    case Json::Type::Array: {
        const std::vector<Json> &items = value.items();
        std::string out = "[";
        for (size_t i = 0; i < items.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += python_repr(items[i]);
        }
        out += "]";
        return out;
    }
    case Json::Type::Object: {
        const std::vector<std::pair<std::string, Json>> &fields = value.fields();
        std::string out = "{";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += quote(fields[i].first) + ": " + python_repr(fields[i].second);
        }
        out += "}";
        return out;
    }
    }
    return std::string();
}

std::string json_label(const Json &value) {
    if (value.is_string()) {
        return value.as_string();
    }
    return python_repr(value);
}

bool parse_number_literal(const std::string &text, double &out) {
    size_t first = 0;
    size_t last = text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
        ++first;
    }
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
        --last;
    }
    if (first == last) {
        return false;
    }
    std::string cleaned;
    cleaned.reserve(last - first);
    for (size_t i = first; i < last; ++i) {
        if (text[i] != '_') {
            cleaned.push_back(text[i]);
        }
    }
    const char *begin = cleaned.c_str();
    char *end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin || end != begin + cleaned.size()) {
        return false;
    }
    out = value;
    return true;
}

template <typename T>
void index_by_id(std::unordered_map<int64_t, const T *> &table, const std::vector<T> &rows,
                 const char *label) {
    table.clear();
    table.reserve(rows.size());
    for (const T &row : rows) {
        if (!table.emplace(row.id, &row).second) {
            throw SpecError("catalog." + std::string(label) + ": ID 重复 " +
                            std::to_string(row.id));
        }
    }
}

const Target &require_target(const CatalogIndex &index, int64_t target_id) {
    const auto found = index.target_by_id.find(target_id);
    if (found == index.target_by_id.end()) {
        throw SpecError("未知技能目标 ID " + std::to_string(target_id));
    }
    return *found->second;
}

bool target_matches_impl(const CatalogIndex &index, const Target &row, const Member &member) {
    const bool checks[4] = {
        row.character == 0 || row.character == member.character,
        row.band == 0 || row.band == member.band,
        row.card_type == 0 || row.card_type == member.card_type,
        row.tag == 0 || contains_int(member.tags, row.tag),
    };
    if (row.gekisou_mission_type != 0 || !row.gekisou_skill_categories.empty()) {
        const auto found = index.gekisou_skill_by_id.find(member.gekisou_skill);
        if (found == index.gekisou_skill_by_id.end()) {
            return false;
        }
        const Skill &gekisou = *found->second;
        if (row.gekisou_mission_type != 0 && row.gekisou_mission_type != gekisou.mission_type) {
            return false;
        }
        if (!row.gekisou_skill_categories.empty() &&
            !intersects(row.gekisou_skill_categories, gekisou.categories)) {
            return false;
        }
    }
    if (!row.live_skill_categories.empty()) {
        static const std::vector<int> kNoCategories;
        const auto found = index.live_skill_by_id.find(member.live_skill);
        const std::vector<int> &categories =
            found == index.live_skill_by_id.end() ? kNoCategories : found->second->categories;
        if (!intersects(row.live_skill_categories, categories)) {
            return false;
        }
    }
    return checks[0] && checks[1] && checks[2] && checks[3];
}

bool condition_matches_impl(const Rules &rules, const CatalogIndex &index, int64_t condition_id,
                            const Member &member) {
    const auto found = index.condition_by_id.find(condition_id);
    if (found == index.condition_by_id.end()) {
        throw SpecError("未知技能条件 ID " + std::to_string(condition_id));
    }
    const Condition &row = *found->second;
    if (row.type != 5000) {
        throw SpecError("技能条件 " + std::to_string(row.id) + " 的 type=" +
                        std::to_string(row.type) + " 未支持（v1 只实现 5000：目标匹配）");
    }
    bool matched = false;
    for (int64_t target_id : row.targets) {
        if (rules.target_matches(require_target(index, target_id), member)) {
            matched = true;
            break;
        }
    }
    return row.positive ? matched : !matched;
}

bool effect_selected(const SkillEffect &effect, int level, const std::optional<int> &effect_type) {
    if (effect.level != level) {
        return false;
    }
    return !effect_type.has_value() || effect.effect_type == *effect_type;
}

std::vector<const SkillEffect *> select_effects(const Skill &skill, int level,
                                                const std::optional<int> &effect_type) {
    std::vector<const SkillEffect *> out;
    out.reserve(skill.effects.size());
    for (const SkillEffect &effect : skill.effects) {
        if (effect_selected(effect, level, effect_type)) {
            out.push_back(&effect);
        }
    }
    return out;
}

int64_t pick_effect_value(const std::vector<const SkillEffect *> &effects,
                          const std::string &branch) {
    if (effects.empty()) {
        return 0;
    }
    if (branch == "max") {
        int64_t best = effects[0]->value;
        for (const SkillEffect *effect : effects) {
            best = std::max(best, effect->value);
        }
        return best;
    }
    if (branch == "min") {
        int64_t best = effects[0]->value;
        for (const SkillEffect *effect : effects) {
            best = std::min(best, effect->value);
        }
        return best;
    }
    if (branch == "first") {
        return effects[0]->value;
    }
    if (branch.compare(0, 5, "index") == 0) {
        size_t index = 0;
        const size_t colon = branch.find(':');
        if (colon != std::string::npos) {
            const std::string text = branch.substr(colon + 1);
            if (text.empty()) {
                throw SpecError("live_effect_select.branch = " + quote(branch) + " 越界（共 " +
                                std::to_string(effects.size()) + " 条效果）");
            }
            try {
                index = static_cast<size_t>(std::stoll(text));
            } catch (const std::exception &) {
                throw SpecError("未知的 live_effect_select.branch: " + quote(branch));
            }
        }
        if (index >= effects.size()) {
            throw SpecError("live_effect_select.branch = " + quote(branch) + " 越界（共 " +
                            std::to_string(effects.size()) + " 条效果）");
        }
        return effects[index]->value;
    }
    throw SpecError("未知的 live_effect_select.branch: " + quote(branch));
}

std::vector<const SkillEffect *>
filter_live_conditions(const Problem &problem, const std::vector<const SkillEffect *> &effects) {
    std::vector<const SkillEffect *> out;
    for (const SkillEffect *effect : effects) {
        if (effect->condition_group == 0) {
            out.push_back(effect);
            continue;
        }
        if (problem.settings.life.mode != "constant")
            throw SpecError("生命条件 LIVE 的动态执行时刻尚未验证；请使用恒定生命模型");
        const auto group = std::find_if(
            problem.catalog.condition_groups.begin(), problem.catalog.condition_groups.end(),
            [&](const ConditionGroup &g) { return g.group == effect->condition_group; });
        if (group == problem.catalog.condition_groups.end())
            throw SpecError("缺少 LIVE 条件组");
        bool any = false;
        for (const auto &row : group->rows) {
            bool all = true;
            for (int64_t id : row) {
                const auto c = std::find_if(problem.catalog.conditions.begin(),
                                            problem.catalog.conditions.end(),
                                            [&](const Condition &c) { return c.id == id; });
                if (c == problem.catalog.conditions.end() || c->values.size() != 1 ||
                    c->type < 2000 || c->type > 2003)
                    throw SpecError("此 LIVE 条件尚未支持: " + std::to_string(id));
                const double life = problem.settings.life.initial;
                const int64_t threshold = c->values[0];
                bool matches = c->type == 2000   ? life > threshold
                               : c->type == 2001 ? life >= threshold
                               : c->type == 2002 ? life < threshold
                                                 : life <= threshold;
                if (!c->positive)
                    matches = !matches;
                if (!matches) {
                    all = false;
                    break;
                }
            }
            if (all) {
                any = true;
                break;
            }
        }
        if (any)
            out.push_back(effect);
    }
    return out;
}

const Triple &band_item_rate(const Fix &fix, const Member &member) {
    const auto found = fix.band_item_bonus_bp_by_band.find(member.band);
    return found == fix.band_item_bonus_bp_by_band.end() ? fix.band_item_bonus_bp : found->second;
}

Triple flat_bonus(const Fix &fix, const Member &member) {
    const auto found = fix.character_rank_bonus_by_character.find(member.character);
    const Triple &character_rank = found == fix.character_rank_bonus_by_character.end()
                                       ? fix.character_rank_bonus
                                       : found->second;
    Triple flat{};
    for (int d = 0; d < kDims; ++d) {
        flat[d] = character_rank[d] + fix.character_total_rank_bonus[d];
    }
    return flat;
}

Triple member_common(const Member &member, const Fix &fix, bool float32_mode) {
    const Triple flat = flat_bonus(fix, member);
    Triple common{};
    for (int d = 0; d < kDims; ++d) {
        const int64_t trained = member.trained[d];
        common[d] = trained + power_floor(trained * member.event_bonus_bp, float32_mode) + flat[d];
    }
    return common;
}

bool extra_applies(const Json &when, const Member &member, const Snapshot *snapshot) {
    if (!when.is_object() || when.fields().empty()) {
        return true;
    }
    static const char *const kKeys[5] = {"member_tags_any", "member_bands", "member_characters",
                                         "card_types", "snapshot_card_types"};
    const Json *found[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < 5; ++i) {
        found[i] = when.find(kKeys[i]);
    }
    auto as_list = [](const Json &value, const char *key) {
        if (!value.is_array()) {
            throw SpecError(std::string("power_model.extra_sources.when.") + key + " 需要数组");
        }
        return value.items();
    };
    if (found[0] != nullptr) {
        bool hit = false;
        for (const Json &item : as_list(*found[0], kKeys[0])) {
            if (contains_int(member.tags, static_cast<int>(item.as_int64()))) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            return false;
        }
    }
    if (found[1] != nullptr) {
        bool hit = false;
        for (const Json &item : as_list(*found[1], kKeys[1])) {
            if (item.as_int64() == member.band) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            return false;
        }
    }
    if (found[2] != nullptr) {
        bool hit = false;
        for (const Json &item : as_list(*found[2], kKeys[2])) {
            if (item.as_int64() == member.character) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            return false;
        }
    }
    if (found[3] != nullptr) {
        bool hit = false;
        for (const Json &item : as_list(*found[3], kKeys[3])) {
            if (item.as_int64() == member.card_type) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            return false;
        }
    }
    if (found[4] != nullptr) {
        if (snapshot == nullptr) {
            return false;
        }
        bool hit = false;
        for (const Json &item : as_list(*found[4], kKeys[4])) {
            if (item.as_int64() == snapshot->card_type) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            return false;
        }
    }
    return true;
}

bool song_tag_matches(const Song &song, const Member &member) {
    if (song.tags.empty()) {
        return false;
    }
    for (int tag : song.tags) {
        if (contains_int(member.tags, tag)) {
            return true;
        }
    }
    return false;
}

struct JudgementLabels {
    std::vector<std::string> labels;
    std::vector<double> factors;
};

JudgementLabels resolve_judgement(const Judgement &judgement, size_t count) {
    JudgementLabels out;
    out.labels.reserve(count);
    const std::string &mode = judgement.mode;
    if (mode == "all_perfect") {
        out.labels.assign(count, "perfect");
    } else if (mode == "fixed") {
        out.labels.assign(count, judgement.fixed_label);
    } else if (mode == "sequence") {
        if (judgement.sequence.empty()) {
            throw SpecError("settings.judgement.mode = 'sequence' 但 sequence 为空");
        }
        for (size_t i = 0; i < count; ++i) {
            out.labels.push_back(json_label(judgement.sequence[i % judgement.sequence.size()]));
        }
    } else if (mode == "random") {
        std::vector<std::pair<std::string, double>> distribution;
        if (judgement.has_distribution && !judgement.distribution.empty()) {
            distribution.assign(judgement.distribution.begin(), judgement.distribution.end());
        } else {
            distribution.emplace_back("perfect", 1.0);
        }
        double total = 0.0;
        for (const auto &entry : distribution) {
            total += entry.second;
        }
        if (!(total > 0.0)) {
            throw SpecError("settings.judgement.distribution 权重之和必须 > 0");
        }
        std::mt19937_64 rng(static_cast<uint64_t>(judgement.seed));
        for (size_t i = 0; i < count; ++i) {
            const double unit = static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0);
            const double target = unit * total;
            double accumulated = 0.0;
            size_t chosen = distribution.size() - 1;
            for (size_t j = 0; j < distribution.size(); ++j) {
                accumulated += distribution[j].second;
                if (target < accumulated) {
                    chosen = j;
                    break;
                }
            }
            out.labels.push_back(distribution[chosen].first);
        }
    } else {
        throw SpecError("未知 judgement.mode: " + quote(mode));
    }
    out.factors.reserve(out.labels.size());
    for (const std::string &label : out.labels) {
        const auto found = judgement.factors.find(label);
        if (found != judgement.factors.end()) {
            out.factors.push_back(found->second);
            continue;
        }
        double number = 0.0;
        if (parse_number_literal(label, number)) {
            out.factors.push_back(number);
        } else {
            out.factors.push_back(judgement.default_factor);
        }
    }
    return out;
}

struct LifeResult {
    std::vector<double> factors;
    Json trace;
};

LifeResult resolve_life(const Life &life, const std::vector<std::string> &labels) {
    const double initial = life.initial;
    const double onus = life.onus_factor;
    const double floor_value = life.floor;
    LifeResult out;
    out.trace = Json::object();
    out.trace.set("mode", Json(life.mode));
    out.trace.set("initial", Json(initial));
    out.trace.set("onus_factor", Json(onus));
    if (initial < 0.0) {
        throw SpecError("settings.life.initial 必须 >= 0");
    }
    if (life.mode == "constant") {
        const double factor = initial > 0.0 ? 1.0 : onus;
        out.trace.set("minimum", Json(initial));
        out.factors.assign(labels.size(), factor);
        return out;
    }
    double current = initial;
    double minimum = initial;
    out.factors.reserve(labels.size());
    for (const std::string &label : labels) {
        double loss = 0.0;
        const auto found = life.damage.find(label);
        if (found != life.damage.end()) {
            loss = found->second;
        } else {
            const auto fallback = life.damage.find("default");
            loss = fallback == life.damage.end() ? 0.0 : fallback->second;
        }
        current = std::max(floor_value, current - loss);
        minimum = std::min(minimum, current);
        out.factors.push_back(current > 0.0 ? 1.0 : onus);
    }
    out.trace.set("minimum", Json(minimum));
    out.trace.set("final", Json(current));
    Json damage = Json::object();
    for (const auto &entry : life.damage) {
        damage.set(entry.first, Json(entry.second));
    }
    out.trace.set("damage", damage);
    return out;
}

Json absolute_score_block(const Problem &problem, const ChartData &chart,
                          const std::vector<double> &boosts,
                          const std::unordered_map<int64_t, int> &member_pos,
                          const std::vector<SlotEval> &slots, int64_t power, double weight_factor) {
    const Settings &settings = problem.settings;
    const ScoreModel &model = settings.score;
    if (!model.level_alpha.has_value()) {
        throw SpecError("settings.score_model.level_alpha 未提供，无法计算绝对分数");
    }
    const double alpha = *model.level_alpha;
    const double level = problem.chart.level;
    const int64_t converted = chart.converted_note_count;
    if (converted <= 0) {
        throw SpecError("chart.converted_note_count 为 0，无法计算绝对分数");
    }
    const double base_note_power = static_cast<double>(power) * model.adjustment_factor *
                                   (1.0 + (level - model.level_base) * alpha) /
                                   static_cast<double>(converted);

    const size_t note_count = chart.times.size();
    const JudgementLabels judged = resolve_judgement(settings.judgement, note_count);

    std::vector<double> skill_factor(note_count, 1.0);
    std::vector<float> native_live_boost(note_count, 0.0f);
    for (const SlotEval &slot : slots) {
        const auto found = member_pos.find(slot.member);
        if (found == member_pos.end()) {
            throw SpecError("未知成员 ID " + std::to_string(slot.member));
        }
        const size_t member_index = static_cast<size_t>(found->second);
        const auto member =
            std::find_if(problem.catalog.members.begin(), problem.catalog.members.end(),
                         [&](const Member &m) { return m.id == slot.member; });
        bool perfect_only = false;
        if (member != problem.catalog.members.end()) {
            const auto skill =
                std::find_if(problem.catalog.live_skills.begin(), problem.catalog.live_skills.end(),
                             [&](const Skill &x) { return x.id == member->live_skill; });
            if (skill != problem.catalog.live_skills.end()) {
                const auto effects = filter_live_conditions(
                    problem,
                    select_effects(*skill, member->live_skill_level.value_or(settings.live_level),
                                   settings.skill.live_effect_type));
                const int64_t selected =
                    pick_effect_value(effects, settings.skill.live_effect_branch);
                for (const SkillEffect *e : effects) {
                    if (e->value != selected)
                        continue;
                    if (e->effect_type != 2000 && e->effect_type != 2004)
                        throw SpecError("绝对分数尚不支持此 LIVE 效果类型: " +
                                        std::to_string(e->effect_type));
                    perfect_only = e->effect_type == 2004;
                    if (perfect_only && e->targets != std::vector<int64_t>{41, 46})
                        throw SpecError("指定判定 LIVE 目标尚未验证；当前仅支持 Master 的 "
                                        "PERFECT/JUST 目标 41、46");
                    break;
                }
            }
        }
        const double boost = member_index < boosts.size() ? boosts[member_index] : 0.0;
        if (boost == 0.0) {
            continue;
        }
        const size_t trigger = static_cast<size_t>(slot.trigger - 1);
        if (trigger >= problem.chart.skill_times_ms.size()) {
            throw SpecError("formation.slots trigger 越界: " + std::to_string(slot.trigger));
        }
        const int64_t start = problem.chart.skill_times_ms[trigger];
        const int64_t end = start + slot.duration_ms;
        for (size_t i = 0; i < note_count; ++i) {
            if (chart.times[i] >= start && chart.times[i] < end &&
                (!perfect_only || judged.labels[i] == "perfect" || judged.labels[i] == "just")) {
                skill_factor[i] += boost;
                native_live_boost[i] += static_cast<float>(boost);
            }
        }
    }

    const LifeResult life = resolve_life(settings.life, judged.labels);
    const double assist_factor =
        settings.assist.enabled ? settings.assist.score_percent / 100.0 : 1.0;

    std::vector<double> raw(note_count, 0.0);
    for (size_t i = 0; i < note_count; ++i) {
        const double boost = (1.0 + chart.combo_bonuses[i]) * skill_factor[i];
        double value = base_note_power * chart.note_weights[i] * boost;
        value *= judged.factors[i];
        value *= life.factors[i];
        value *= assist_factor;
        raw[i] = value;
    }

    const std::string &rounding = model.rounding;
    std::vector<int64_t> rounded;
    bool keep_float = false;
    if (rounding == "float32_floor") {
        rounded.resize(note_count);
        for (size_t i = 0; i < note_count; ++i) {

            float difficulty = static_cast<float>(level - model.level_base);
            difficulty *= static_cast<float>(alpha);
            difficulty += 1.0f;
            float base = static_cast<float>(model.adjustment_factor) * static_cast<float>(power);
            base *= difficulty;
            float value = static_cast<float>(chart.note_weights[i]) * base;
            value *= static_cast<float>(judged.factors[i]);
            float score_boost = chart.native_combo_bonuses[i] + 1.0f;
            value *= score_boost;
            value *= 1.0f + native_live_boost[i];
            value /= static_cast<float>(converted);
            value = std::floor(value);
            value *= static_cast<float>(life.factors[i]);
            value *= static_cast<float>(assist_factor);
            rounded[i] = static_cast<int64_t>(std::floor(value));
        }
    } else if (rounding == "float64_floor") {
        rounded.resize(note_count);
        for (size_t i = 0; i < note_count; ++i) {
            rounded[i] = static_cast<int64_t>(std::floor(raw[i]));
        }
    } else if (rounding == "none") {
        keep_float = true;
    } else {
        throw SpecError("未知 rounding: " + quote(rounding));
    }

    struct Section {
        std::string label;
        int64_t start_ms = 0;
        int64_t end_ms = 0;
        int64_t bonus_bp = 10000;
    };
    const int64_t last_ms = chart.times.empty() ? 0 : chart.times.back();
    std::vector<Section> sections;
    if (settings.gekisou.enabled && !settings.gekisou.sections.empty()) {
        for (size_t i = 0; i < settings.gekisou.sections.size(); ++i) {
            const GekisouSection &item = settings.gekisou.sections[i];
            const int64_t start = item.start_ms;
            const int64_t end = item.end_ms.has_value() ? *item.end_ms : last_ms + 1;
            if (end <= start) {
                throw SpecError("settings.gekisou.sections[" + std::to_string(i) +
                                "]: end_ms 必须大于 start_ms");
            }
            Section section;
            section.label = item.label.empty() ? "section-" + std::to_string(i + 1) : item.label;
            section.start_ms = start;
            section.end_ms = end;
            section.bonus_bp = item.bonus_bp;
            sections.push_back(std::move(section));
        }
    } else {
        sections.push_back(Section{"full", 0, last_ms + 1, 10000});
    }

    for (size_t i = 0; i < sections.size(); ++i) {
        if (sections[i].bonus_bp < 10000)
            throw SpecError("击奏 bonus_bp 表示总倍率，必须 >= 10000；追加奖励 250% 应填 35000");
        for (size_t j = 0; j < i; ++j)
            if (sections[i].start_ms < sections[j].end_ms &&
                sections[j].start_ms < sections[i].end_ms)
                throw SpecError("击奏区间不能重叠");
    }

    double section_total = keep_float ? numpy_sum(raw.data(), raw.size()) : 0.0;
    if (!keep_float)
        for (int64_t value : rounded)
            section_total += value;
    Json gekisou_detail = Json::array();
    std::vector<double> scratch;
    for (const Section &section : sections) {
        const size_t lo = static_cast<size_t>(
            std::lower_bound(chart.times.begin(), chart.times.end(), section.start_ms) -
            chart.times.begin());
        const size_t hi = static_cast<size_t>(
            std::lower_bound(chart.times.begin(), chart.times.end(), section.end_ms) -
            chart.times.begin());
        const double bonus = static_cast<double>(section.bonus_bp) / 10000.0;
        double subtotal = 0.0;
        double section_base = 0.0;
        if (keep_float) {
            section_base = numpy_sum(raw.data() + lo, hi - lo);
            subtotal = section_base * bonus;
        } else {
            scratch.resize(hi - lo);
            for (size_t i = lo; i < hi; ++i) {
                scratch[i - lo] = static_cast<double>(rounded[i]);
            }
            section_base = numpy_sum(scratch.data(), scratch.size());

            subtotal = section_base + std::floor(section_base * (bonus - 1.0));
        }
        section_total += subtotal - section_base;
        Json item = Json::object();
        item.set("label", Json(section.label));
        item.set("start_ms", Json(section.start_ms));
        item.set("end_ms", Json(section.end_ms));
        item.set("bonus_bp", Json(section.bonus_bp));
        item.set("notes", Json(static_cast<int64_t>(hi - lo)));
        item.set("base_score", Json(section_base));
        item.set("additional_reward", Json(subtotal - section_base));
        item.set("subtotal", Json(subtotal));
        gekisou_detail.push_back(std::move(item));
    }

    Json counts = Json::object();
    std::vector<std::pair<std::string, int64_t>> ordered_counts;
    for (const std::string &label : judged.labels) {
        bool seen = false;
        for (auto &entry : ordered_counts) {
            if (entry.first == label) {
                entry.second += 1;
                seen = true;
                break;
            }
        }
        if (!seen) {
            ordered_counts.emplace_back(label, 1);
        }
    }
    for (const auto &entry : ordered_counts) {
        counts.set(entry.first, Json(entry.second));
    }
    Json judgement_block = Json::object();
    judgement_block.set("mode", Json(settings.judgement.mode));
    judgement_block.set("counts", std::move(counts));

    Json out = Json::object();
    out.set("calibrated", Json(false));
    out.set("total", Json(section_total));
    out.set("unrounded_total", Json(numpy_sum(raw.data(), raw.size())));
    out.set("base_note_power", Json(base_note_power));
    out.set("converted_note_count", Json(converted));
    out.set("adjustment_factor", Json(model.adjustment_factor));
    out.set("level", Json(level));
    out.set("level_alpha", Json(alpha));
    out.set("rounding", Json(rounding));
    out.set("combo_live_rule", Json("multiplicative_note_score"));
    out.set("note_pipeline",
            Json(rounding == "float32_floor" ? "float32_two_floors" : "analytical"));
    out.set("assist_factor", Json(assist_factor));
    out.set("judgement", std::move(judgement_block));
    out.set("life", life.trace);
    Json gekisou = Json::object();
    gekisou.set("enabled", Json(settings.gekisou.enabled));
    gekisou.set("sections", std::move(gekisou_detail));
    out.set("gekisou", std::move(gekisou));
    out.set("linearized_total", Json(base_note_power * chart.base * weight_factor));
    return out;
}

}

void CatalogIndex::build(const Catalog &catalog) {
    index_by_id(member_by_id, catalog.members, "members");
    index_by_id(snapshot_by_id, catalog.snapshots, "snapshots");
    index_by_id(live_skill_by_id, catalog.live_skills, "live_skills");
    index_by_id(leader_skill_by_id, catalog.leader_skills, "leader_skills");
    index_by_id(support_skill_by_id, catalog.support_skills, "support_skills");
    index_by_id(gekisou_skill_by_id, catalog.gekisou_skills, "gekisou_skills");
    index_by_id(target_by_id, catalog.targets, "targets");
    index_by_id(condition_by_id, catalog.conditions, "conditions");

    condition_group_by_group.clear();
    for (const ConditionGroup &group : catalog.condition_groups) {
        condition_group_by_group[group.group] = &group;
    }
    note_parameter_by_op.clear();
    for (const NoteParameter &parameter : catalog.note_parameters) {
        note_parameter_by_op[parameter.op] = &parameter;
    }
    combo_by_type.clear();
    for (const ComboBonus &bonus : catalog.combo_bonuses) {
        combo_by_type[bonus.type].push_back(&bonus);
    }
    for (auto &entry : combo_by_type) {
        std::stable_sort(entry.second.begin(), entry.second.end(),
                         [](const ComboBonus *left, const ComboBonus *right) {
                             return left->required_combo_count < right->required_combo_count;
                         });
    }
}

Rules::Rules(const Problem &problem, const CatalogIndex &index)
    : problem_(problem), index_(index), live_level_(problem.settings.live_level),
      leader_level_(problem.settings.leader_level), support_level_(problem.settings.support_level),
      base_duration_ms_(problem.settings.base_duration_ms),
      duration_effect_type_(static_cast<int>(problem.settings.duration_extension_effect_type)),
      live_effect_type_(problem.settings.skill.live_effect_type),
      live_effect_branch_(problem.settings.skill.live_effect_branch),
      unmapped_is_error_(problem.settings.leader_unmapped_effect_types == "error") {
    if (problem.settings.leader_effect_axes.empty()) {

        leader_axes_ = {{1000, "all"},
                        {1001, "technique"},
                        {1002, "visual"},
                        {1003, "performance"},
                        {3000, "all"}};
    }
    for (const auto &entry : problem.settings.leader_effect_axes) {
        const std::string &axis = entry.second;
        if (axis != "all" && axis != "performance" && axis != "technique" && axis != "visual") {
            throw SpecError("settings.leader_effect_axes." + std::to_string(entry.first) +
                            " 只支持 'all' / ('performance', 'technique', 'visual')，收到 " +
                            quote(axis));
        }
        leader_axes_[entry.first] = axis;
    }
}

bool Rules::target_matches(const Target &target, const Member &member) const {
    const uint64_t key = pack_ids(member.id, target.id);
    const auto cached = target_cache_.find(key);
    if (cached != target_cache_.end()) {
        return cached->second;
    }
    const bool value = target_matches_impl(index_, target, member);
    target_cache_.emplace(key, value);
    return value;
}

bool Rules::condition_group_matches(int group, const Member &member) const {
    if (group == 0) {
        return true;
    }
    const uint64_t key = pack_ids(static_cast<int64_t>(group), member.id);
    const auto cached = group_cache_.find(key);
    if (cached != group_cache_.end()) {
        return cached->second;
    }
    const auto found = index_.condition_group_by_group.find(group);
    if (found == index_.condition_group_by_group.end()) {
        throw SpecError("未知技能条件组 " + std::to_string(group));
    }
    bool value = false;
    for (const std::vector<int64_t> &row : found->second->rows) {
        bool all = true;
        for (int64_t condition_id : row) {
            if (!condition_matches_impl(*this, index_, condition_id, member)) {
                all = false;
                break;
            }
        }
        if (all) {
            value = true;
            break;
        }
    }
    group_cache_.emplace(key, value);
    return value;
}

double Rules::live_boost(const Member &member) const {
    const auto cached = live_cache_.find(member.id);
    if (cached != live_cache_.end()) {
        return cached->second;
    }
    double value = 0.0;
    const auto found = index_.live_skill_by_id.find(member.live_skill);
    if (found != index_.live_skill_by_id.end()) {
        const std::vector<const SkillEffect *> effects = filter_live_conditions(
            problem_, select_effects(*found->second, member.live_skill_level.value_or(live_level_),
                                     live_effect_type_));
        const int64_t chosen = pick_effect_value(effects, live_effect_branch_);
        for (const SkillEffect *effect : effects) {
            if (effect->value != chosen)
                continue;
            if (effect->effect_type != 2000 && effect->effect_type != 2004)
                throw SpecError("LIVE 分数模型尚不支持效果类型 " +
                                std::to_string(effect->effect_type));
            break;
        }
        value = static_cast<double>(chosen) / 10000.0;
    }
    live_cache_.emplace(member.id, value);
    return value;
}

Triple Rules::leader_bonus_bp(const Member &leader, const Member &member) const {
    const uint64_t cache_key = pack_ids(leader.id, member.id);
    const auto cached = leader_cache_.find(cache_key);
    if (cached != leader_cache_.end()) {
        return cached->second;
    }
    Triple out{0, 0, 0};
    const auto found = index_.leader_skill_by_id.find(leader.leader_skill);
    if (found != index_.leader_skill_by_id.end()) {
        const Skill &skill = *found->second;
        for (const SkillEffect *effect : select_effects(
                 skill, leader.leader_skill_level.value_or(leader_level_), std::nullopt)) {
            if (!effect->targets.empty()) {
                bool any = false;
                for (int64_t target_id : effect->targets) {
                    if (target_matches(require_target(index_, target_id), member)) {
                        any = true;
                        break;
                    }
                }
                if (!any) {
                    continue;
                }
            }
            const auto axis = leader_axes_.find(effect->effect_type);
            if (axis == leader_axes_.end()) {
                if (unmapped_is_error_) {
                    throw SpecError("队长技能 " + std::to_string(skill.id) + " 的效果类型 " +
                                    std::to_string(effect->effect_type) +
                                    " 未在 settings.leader_effect_axes 中定义");
                }
                const std::pair<int64_t, int> key = {skill.id, effect->effect_type};
                const auto known = unmapped_.find(key);
                if (known == unmapped_.end()) {
                    unmapped_.emplace(key, std::make_pair(1, effect->value));
                } else {
                    known->second.first += 1;
                }
                continue;
            }
            if (axis->second == "all") {
                for (int d = 0; d < kDims; ++d) {
                    out[d] += effect->value;
                }
            } else {
                int index = 0;
                if (!axis_index(axis->second, index)) {
                    throw SpecError("settings.leader_effect_axes." +
                                    std::to_string(effect->effect_type) + " 未知属性轴 " +
                                    quote(axis->second));
                }
                out[index] += effect->value;
            }
        }
    }
    leader_cache_.emplace(cache_key, out);
    return out;
}

int64_t Rules::duration_ms(const Member &member, const Snapshot &snapshot) const {
    int64_t extension = 0;
    for (int64_t skill_id : snapshot.support_skills) {
        if (skill_id == 0) {
            continue;
        }
        const auto found = index_.support_skill_by_id.find(skill_id);
        if (found == index_.support_skill_by_id.end()) {
            throw SpecError("Snapshot " + std::to_string(snapshot.id) + " 引用了未知支援技能 " +
                            std::to_string(skill_id));
        }
        const auto level = snapshot.support_skill_levels.find(skill_id);
        const int skill_level =
            level == snapshot.support_skill_levels.end() ? support_level_ : level->second;
        for (const SkillEffect *effect : select_effects(
                 *found->second, skill_level, std::optional<int>(duration_effect_type_))) {
            if (effect->condition_group != 0 &&
                !condition_group_matches(effect->condition_group, member)) {
                continue;
            }
            extension += effect->value;
        }
    }
    return base_duration_ms_ + extension;
}

double ChartData::coverage(int64_t start_ms, int64_t duration_ms) const {
    if (duration_ms <= 0) {
        throw SpecError("技能持续时间必须 > 0，收到 " + std::to_string(duration_ms));
    }
    const int64_t end_ms = start_ms + duration_ms;

    const size_t lo =
        static_cast<size_t>(std::lower_bound(times.begin(), times.end(), start_ms) - times.begin());
    const size_t hi =
        static_cast<size_t>(std::lower_bound(times.begin(), times.end(), end_ms) - times.begin());
    return (prefix[hi] - prefix[lo]) / base;
}

std::vector<int64_t> Evaluation::member_ids() const {
    std::vector<int64_t> ids;
    ids.reserve(slots.size());
    for (const SlotEval &slot : slots) {
        ids.push_back(slot.member);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

Json Evaluation::to_json(bool detail, bool with_rank, int rank) const {
    std::vector<SlotEval> ordered = slots;
    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const SlotEval &left, const SlotEval &right) { return left.trigger < right.trigger; });

    Json out = Json::object();
    if (with_rank) {
        out.set("rank", Json(static_cast<int64_t>(rank)));
    }
    out.set("leader", Json(leader));
    Json members = Json::array();
    for (int64_t member_id : member_ids()) {
        members.push_back(Json(member_id));
    }
    out.set("members", std::move(members));

    Json assignments = Json::array();
    for (const SlotEval &slot : ordered) {
        Json item = Json::object();
        item.set("trigger", Json(static_cast<int64_t>(slot.trigger)));
        item.set("member", Json(slot.member));
        item.set("snapshot", Json(slot.snapshot));
        item.set("duration_ms", Json(slot.duration_ms));
        item.set("power", Json(slot.power));
        item.set("live_boost", Json(slot.live_boost));
        item.set("weighted_skill_gain", Json(slot.weighted_skill_gain));
        if (detail && slot.has_breakdown) {
            item.set("power_breakdown", slot.breakdown);
        }
        assignments.push_back(std::move(item));
    }
    out.set("assignments", std::move(assignments));
    out.set("power", Json(power));
    out.set("weight_factor", Json(weight_factor));
    out.set("index", Json(index));
    out.set("ranking_objective", Json(ranking_objective));
    out.set("ranking_score", ranking_score ? Json(*ranking_score) : Json());
    out.set("estimated_score", has_estimated ? estimated_score : Json());
    if (has_order) {
        out.set("order_analysis", order_analysis);
    }
    return out;
}

namespace {

CatalogIndex build_catalog_index(const Catalog &catalog) {
    CatalogIndex index;
    index.build(catalog);
    return index;
}

void require_index(size_t position, size_t limit, const char *label) {
    if (position >= limit) {
        throw SpecError(std::string(label) + "序号越界: " + std::to_string(position) + "（共 " +
                        std::to_string(limit) + "）");
    }
}

}

Engine::Engine(const Problem &problem)
    : problem_(problem), index_(build_catalog_index(problem.catalog)), rules_(problem, index_),
      members_(problem.available_members()), snapshots_(problem.available_snapshots()) {
    if (problem_.settings.power.rounding == "none") {
        throw SpecError("settings.power_model.rounding 不支持 'none'（综合力必须是整数）");
    }
    member_pos_.reserve(members_.size() * 2);
    for (size_t i = 0; i < members_.size(); ++i) {
        member_pos_[members_[i]->id] = static_cast<int>(i);
    }
    snapshot_pos_.reserve(snapshots_.size() * 2);
    for (size_t i = 0; i < snapshots_.size(); ++i) {
        snapshot_pos_[snapshots_[i]->id] = static_cast<int>(i);
    }
    build_chart();
}

void Engine::build_chart() {
    const ChartSpec &spec = problem_.chart;
    std::vector<const Note *> judged;
    judged.reserve(spec.notes.size());
    for (const Note &note : spec.notes) {
        if (note.scoring) {
            judged.push_back(&note);
        }
    }
    if (judged.empty()) {
        throw SpecError("chart.notes: 没有任何计分音符");
    }

    const size_t count = judged.size();
    chart_.times.resize(count);
    chart_.ops.resize(count);
    for (size_t i = 0; i < count; ++i) {
        chart_.times[i] = judged[i]->t;
        chart_.ops[i] = judged[i]->op;
    }
    for (size_t i = 1; i < count; ++i) {
        if (chart_.times[i] < chart_.times[i - 1]) {
            throw SpecError("chart.notes: 音符时间必须按非递减顺序排列");
        }
    }

    std::vector<int> missing;
    for (int op : chart_.ops) {
        if (index_.note_parameter_by_op.find(op) == index_.note_parameter_by_op.end() &&
            std::find(missing.begin(), missing.end(), op) == missing.end()) {
            missing.push_back(op);
        }
    }
    if (!missing.empty()) {
        std::sort(missing.begin(), missing.end());
        std::string text = "[";
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += std::to_string(missing[i]);
        }
        text += "]";
        throw SpecError("谱面使用的 op " + text + " 缺少 score_percent 定义");
    }

    const auto combo = index_.combo_by_type.find(problem_.settings.combo_type);
    const std::vector<const ComboBonus *> *combo_table =
        combo == index_.combo_by_type.end() ? nullptr : &combo->second;

    chart_.weights.assign(count, 0.0);
    chart_.note_weights.assign(count, 0.0);
    chart_.combo_bonuses.assign(count, 0.0);
    chart_.native_combo_bonuses.assign(count, 0.0f);
    for (size_t i = 0; i < count; ++i) {
        const auto parameter = index_.note_parameter_by_op.find(chart_.ops[i]);
        if (parameter == index_.note_parameter_by_op.end()) {
            throw SpecError("谱面音符 op=" + std::to_string(chart_.ops[i]) +
                            " 没有对应的 score_percent 定义");
        }
        const int64_t prior = static_cast<int64_t>(
            std::lower_bound(chart_.times.begin(), chart_.times.end(), chart_.times[i]) -
            chart_.times.begin());
        double combo_bonus = 0.0;
        float native_combo_bonus = 0.0f;
        if (combo_table != nullptr) {
            for (const ComboBonus *bonus : *combo_table) {
                if (prior >= bonus->required_combo_count) {
                    combo_bonus += bonus->factor;
                    native_combo_bonus += static_cast<float>(bonus->factor);
                }
            }
        }
        chart_.combo_bonuses[i] = std::min(combo_bonus, 1.0);
        chart_.native_combo_bonuses[i] = std::min(native_combo_bonus, 1.0f);
        chart_.note_weights[i] = parameter->second->score_percent / 100.0;
        chart_.weights[i] = chart_.note_weights[i] * (1.0 + chart_.combo_bonuses[i]);
    }

    double score_percent_sum = 0.0;
    for (const Note &note : spec.notes) {
        const auto parameter = index_.note_parameter_by_op.find(note.op);
        if (parameter != index_.note_parameter_by_op.end()) {
            score_percent_sum += parameter->second->score_percent;
        }
    }

    const float converted_weight = static_cast<float>(score_percent_sum) / 100.0f;
    chart_.all_note_weight_sum = score_percent_sum / 100.0;
    chart_.converted_note_count = static_cast<int64_t>(std::ceil(converted_weight));
    chart_.judged_notes = static_cast<int>(count);
    chart_.base = numpy_sum(chart_.weights.data(), chart_.weights.size());
    chart_.prefix.assign(count + 1, 0.0);
    for (size_t i = 0; i < count; ++i) {
        chart_.prefix[i + 1] = chart_.prefix[i] + chart_.weights[i];
    }
}

int Engine::member_pos(int64_t id) const {
    const auto found = member_pos_.find(id);
    return found == member_pos_.end() ? -1 : found->second;
}

int Engine::snapshot_pos(int64_t id) const {
    const auto found = snapshot_pos_.find(id);
    return found == snapshot_pos_.end() ? -1 : found->second;
}

const std::vector<std::vector<int64_t>> &Engine::durations() const {
    if (!durations_ready_) {
        durations_.assign(members_.size(), std::vector<int64_t>(snapshots_.size(), 0));
        for (size_t m = 0; m < members_.size(); ++m) {
            for (size_t s = 0; s < snapshots_.size(); ++s) {
                durations_[m][s] = rules_.duration_ms(*members_[m], *snapshots_[s]);
            }
        }
        durations_ready_ = true;
    }
    return durations_;
}

const std::vector<double> &Engine::boosts() const {
    if (!boosts_ready_) {
        boosts_.clear();
        boosts_.reserve(members_.size());
        for (const Member *member : members_) {
            boosts_.push_back(rules_.live_boost(*member));
        }
        boosts_ready_ = true;
    }
    return boosts_;
}

const std::vector<std::vector<std::vector<double>>> &Engine::gains() const {
    if (!gains_ready_) {
        const std::vector<std::vector<int64_t>> &duration_matrix = durations();
        const std::vector<double> &boost_values = boosts();
        const std::vector<int64_t> &starts = problem_.chart.skill_times_ms;
        for (const std::vector<int64_t> &row : duration_matrix) {
            for (int64_t duration : row) {
                if (duration <= 0) {
                    throw SpecError("技能持续时间必须 > 0");
                }
            }
        }
        gains_.assign(members_.size(),
                      std::vector<std::vector<double>>(snapshots_.size(),
                                                       std::vector<double>(starts.size(), 0.0)));
        for (size_t m = 0; m < members_.size(); ++m) {
            for (size_t s = 0; s < snapshots_.size(); ++s) {
                for (size_t k = 0; k < starts.size(); ++k) {
                    gains_[m][s][k] =
                        chart_.coverage(starts[k], duration_matrix[m][s]) * boost_values[m];
                }
            }
        }
        gains_ready_ = true;
    }
    return gains_;
}

int64_t Engine::duration_ms(int member_index, int snapshot_index) const {
    require_index(static_cast<size_t>(member_index), members_.size(), "成员");
    require_index(static_cast<size_t>(snapshot_index), snapshots_.size(), "Snapshot");
    return durations()[static_cast<size_t>(member_index)][static_cast<size_t>(snapshot_index)];
}

double Engine::live_boost(int member_index) const {
    require_index(static_cast<size_t>(member_index), members_.size(), "成员");
    return boosts()[static_cast<size_t>(member_index)];
}

int64_t Engine::slot_power(size_t leader_index, size_t member_index, size_t snapshot_index) const {
    require_index(leader_index, members_.size(), "队长");
    require_index(member_index, members_.size(), "成员");
    require_index(snapshot_index, snapshots_.size(), "Snapshot");

    const Settings &settings = problem_.settings;
    const Fix &fix = problem_.catalog.fix;
    const bool float32_mode = power_uses_float32(settings);
    const Member &leader = *members_[leader_index];
    const Member &member = *members_[member_index];
    const Snapshot &snapshot = *snapshots_[snapshot_index];
    const Triple common = member_common(member, fix, float32_mode);

    int64_t total = 0;
    for (int d = 0; d < kDims; ++d) {
        total += common[d];
    }
    if (settings.power.snapshot) {
        for (int d = 0; d < kDims; ++d) {
            const int64_t rate = snapshot.trained[d] + snapshot.event_bonus_bp;
            total += power_floor(common[d] * rate, float32_mode);
        }
    }
    if (settings.power.type_link) {
        int64_t rate = 0;
        if (member.card_type == snapshot.card_type) {
            const int64_t link = settings.type_link_bonus_source == "snapshot"
                                     ? snapshot.card_rank_bonus_bp.type_link
                                     : member.card_rank_bonus_bp.type_link;
            rate = fix.type_link_base_bp + link;
        }
        for (int d = 0; d < kDims; ++d) {
            total += power_floor(common[d] * rate, float32_mode);
        }
    }
    if (settings.power.band_item) {
        for (int d = 0; d < kDims; ++d) {
            total += power_floor(common[d] * band_item_rate(fix, member)[d], float32_mode);
        }
    }
    if (settings.power.music_type && problem_.song.type == member.card_type) {
        const int64_t rate = fix.music_type_base_bp + member.card_rank_bonus_bp.music_type;
        for (int d = 0; d < kDims; ++d) {
            total += power_floor(common[d] * rate, float32_mode);
        }
    }
    if (settings.power.music_tag && song_tag_matches(problem_.song, member)) {
        const int64_t rate = fix.music_tag_base_bp + member.card_rank_bonus_bp.music_tag;
        for (int d = 0; d < kDims; ++d) {
            total += power_floor(common[d] * rate, float32_mode);
        }
    }
    if (settings.power.leader) {
        const Triple rate = rules_.leader_bonus_bp(leader, member);
        for (int d = 0; d < kDims; ++d) {
            total += power_floor(common[d] * rate[d], float32_mode);
        }
    }
    if (settings.power.vip) {
        for (int d = 0; d < kDims; ++d) {
            total += power_floor(common[d] * fix.vip_bonus_bp[d], float32_mode);
        }
    }
    for (const ExtraSource &source : settings.power.extra_sources) {
        if (source.kind == "flat") {
            continue;
        }
        if (!extra_applies(source.when, member, &snapshot)) {
            continue;
        }
        for (int d = 0; d < kDims; ++d) {
            total += power_floor(common[d] * source.rate_bp[d], float32_mode);
        }
    }
    for (const ExtraSource &source : settings.power.extra_sources) {
        if (source.kind != "flat") {
            continue;
        }
        if (!extra_applies(source.when, member, &snapshot)) {
            continue;
        }
        for (int d = 0; d < kDims; ++d) {
            total += source.rate_bp[d];
        }
    }
    return total;
}

Json Engine::slot_breakdown(size_t leader_index, size_t member_index, size_t snapshot_index) const {
    require_index(leader_index, members_.size(), "队长");
    require_index(member_index, members_.size(), "成员");
    require_index(snapshot_index, snapshots_.size(), "Snapshot");

    const Settings &settings = problem_.settings;
    const Fix &fix = problem_.catalog.fix;
    const bool float32_mode = power_uses_float32(settings);
    const Member &leader = *members_[leader_index];
    const Member &member = *members_[member_index];
    const Snapshot &snapshot = *snapshots_[snapshot_index];
    const Triple common = member_common(member, fix, float32_mode);

    std::vector<std::pair<std::string, Triple>> parts;
    auto add_part = [&](const std::string &name, const Triple &rate) {
        Triple part{};
        for (int d = 0; d < kDims; ++d) {
            part[d] = power_floor(common[d] * rate[d], float32_mode);
        }
        parts.emplace_back(name, part);
    };
    auto add_scalar = [&](const std::string &name, int64_t rate) {
        Triple part{};
        for (int d = 0; d < kDims; ++d) {
            part[d] = power_floor(common[d] * rate, float32_mode);
        }
        parts.emplace_back(name, part);
    };

    if (settings.power.snapshot) {
        Triple rate{};
        for (int d = 0; d < kDims; ++d) {
            rate[d] = snapshot.trained[d] + snapshot.event_bonus_bp;
        }
        add_part("snapshot", rate);
    }
    if (settings.power.type_link) {
        int64_t rate = 0;
        if (member.card_type == snapshot.card_type) {
            const int64_t link = settings.type_link_bonus_source == "snapshot"
                                     ? snapshot.card_rank_bonus_bp.type_link
                                     : member.card_rank_bonus_bp.type_link;
            rate = fix.type_link_base_bp + link;
        }
        add_scalar("type_link", rate);
    }
    if (settings.power.band_item) {
        add_part("band_item", band_item_rate(fix, member));
    }
    if (settings.power.music_type) {
        const int64_t rate = problem_.song.type == member.card_type
                                 ? fix.music_type_base_bp + member.card_rank_bonus_bp.music_type
                                 : 0;
        add_scalar("music_type", rate);
    }
    if (settings.power.music_tag) {
        const int64_t rate = song_tag_matches(problem_.song, member)
                                 ? fix.music_tag_base_bp + member.card_rank_bonus_bp.music_tag
                                 : 0;
        add_scalar("music_tag", rate);
    }
    if (settings.power.leader) {
        add_part("leader", rules_.leader_bonus_bp(leader, member));
    }
    if (settings.power.vip) {
        add_part("vip", fix.vip_bonus_bp);
    }
    for (const ExtraSource &source : settings.power.extra_sources) {
        if (source.kind == "flat") {
            continue;
        }
        if (!extra_applies(source.when, member, &snapshot)) {
            continue;
        }
        add_part("extra:" + source.id, source.rate_bp);
    }
    for (const ExtraSource &source : settings.power.extra_sources) {
        if (source.kind != "flat") {
            continue;
        }
        if (!extra_applies(source.when, member, &snapshot)) {
            continue;
        }
        parts.emplace_back("extra:" + source.id, source.rate_bp);
    }

    Triple total = common;
    for (const auto &part : parts) {
        for (int d = 0; d < kDims; ++d) {
            total[d] += part.second[d];
        }
    }
    const int64_t power = total[0] + total[1] + total[2];

    Json out = Json::object();
    out.set("member", Json(member.id));
    out.set("snapshot", Json(snapshot.id));
    Json common_json = Json::array();
    for (int d = 0; d < kDims; ++d) {
        common_json.push_back(Json(common[d]));
    }
    out.set("common", std::move(common_json));
    Json parts_json = Json::object();
    for (const auto &part : parts) {
        Json values = Json::array();
        for (int d = 0; d < kDims; ++d) {
            values.push_back(Json(part.second[d]));
        }
        parts_json.set(part.first, std::move(values));
    }
    out.set("parts", std::move(parts_json));
    Json total_json = Json::array();
    for (int d = 0; d < kDims; ++d) {
        total_json.push_back(Json(total[d]));
    }
    out.set("total", std::move(total_json));
    out.set("power", Json(power));
    return out;
}

const std::vector<std::vector<int64_t>> &Engine::power_matrix(size_t leader_index) const {
    const auto cached = power_matrices_.find(leader_index);
    if (cached != power_matrices_.end()) {
        return cached->second;
    }
    require_index(leader_index, members_.size(), "队长");

    const size_t member_count = members_.size();
    const size_t snapshot_count = snapshots_.size();
    std::vector<std::vector<int64_t>> matrix;
    matrix.resize(member_count);
    for (std::vector<int64_t> &row : matrix) {
        row.assign(snapshot_count, 0);
    }
    if (member_count == 0 || snapshot_count == 0) {
        return power_matrices_.emplace(leader_index, std::move(matrix)).first->second;
    }

    const Settings &settings = problem_.settings;
    const Fix &fix = problem_.catalog.fix;
    const bool float32_mode = power_uses_float32(settings);
    const Member &leader = *members_[leader_index];
    std::vector<Triple> common(member_count);
    std::vector<int64_t> music_type_rate(member_count, 0);
    std::vector<int64_t> music_tag_rate(member_count, 0);
    std::vector<Triple> leader_rate(member_count);
    for (size_t m = 0; m < member_count; ++m) {
        const Member &member = *members_[m];
        common[m] = member_common(member, fix, float32_mode);
        if (problem_.song.type == member.card_type) {
            music_type_rate[m] = fix.music_type_base_bp + member.card_rank_bonus_bp.music_type;
        }
        if (song_tag_matches(problem_.song, member)) {
            music_tag_rate[m] = fix.music_tag_base_bp + member.card_rank_bonus_bp.music_tag;
        }
        if (settings.power.leader) {
            leader_rate[m] = rules_.leader_bonus_bp(leader, member);
        }
    }

    std::vector<Triple> snapshot_rate(snapshot_count);
    std::vector<int64_t> snapshot_link(snapshot_count, 0);
    for (size_t s = 0; s < snapshot_count; ++s) {
        const Snapshot &snapshot = *snapshots_[s];
        for (int d = 0; d < kDims; ++d) {
            snapshot_rate[s][d] = snapshot.trained[d] + snapshot.event_bonus_bp;
        }
        snapshot_link[s] = snapshot.card_rank_bonus_bp.type_link;
    }

    struct ExtraPlan {
        bool flat = false;
        Triple rate{};
        std::vector<unsigned char> applies;
    };
    std::vector<ExtraPlan> extra_plans;
    for (const ExtraSource &source : settings.power.extra_sources) {
        ExtraPlan plan;
        plan.flat = source.kind == "flat";
        plan.rate = source.rate_bp;
        plan.applies.assign(member_count * snapshot_count, 0);
        for (size_t m = 0; m < member_count; ++m) {
            for (size_t s = 0; s < snapshot_count; ++s) {
                plan.applies[m * snapshot_count + s] =
                    extra_applies(source.when, *members_[m], snapshots_[s]) ? 1 : 0;
            }
        }
        extra_plans.push_back(std::move(plan));
    }

    for (size_t m = 0; m < member_count; ++m) {
        const Member &member = *members_[m];
        int64_t base_sum = 0;
        for (int d = 0; d < kDims; ++d) {
            base_sum += common[m][d];
        }
        for (size_t s = 0; s < snapshot_count; ++s) {
            int64_t total = base_sum;
            if (settings.power.snapshot) {
                for (int d = 0; d < kDims; ++d) {
                    total += power_floor(common[m][d] * snapshot_rate[s][d], float32_mode);
                }
            }
            if (settings.power.type_link) {
                int64_t rate = 0;
                if (member.card_type == snapshots_[s]->card_type) {
                    const int64_t link = settings.type_link_bonus_source == "snapshot"
                                             ? snapshot_link[s]
                                             : member.card_rank_bonus_bp.type_link;
                    rate = fix.type_link_base_bp + link;
                }
                for (int d = 0; d < kDims; ++d) {
                    total += power_floor(common[m][d] * rate, float32_mode);
                }
            }
            if (settings.power.band_item) {
                for (int d = 0; d < kDims; ++d) {
                    total +=
                        power_floor(common[m][d] * band_item_rate(fix, member)[d], float32_mode);
                }
            }
            if (settings.power.music_type) {
                for (int d = 0; d < kDims; ++d) {
                    total += power_floor(common[m][d] * music_type_rate[m], float32_mode);
                }
            }
            if (settings.power.music_tag) {
                for (int d = 0; d < kDims; ++d) {
                    total += power_floor(common[m][d] * music_tag_rate[m], float32_mode);
                }
            }
            if (settings.power.leader) {
                for (int d = 0; d < kDims; ++d) {
                    total += power_floor(common[m][d] * leader_rate[m][d], float32_mode);
                }
            }
            if (settings.power.vip) {
                for (int d = 0; d < kDims; ++d) {
                    total += power_floor(common[m][d] * fix.vip_bonus_bp[d], float32_mode);
                }
            }
            for (const ExtraPlan &plan : extra_plans) {
                if (plan.applies[m * snapshot_count + s] == 0) {
                    continue;
                }
                if (plan.flat) {
                    for (int d = 0; d < kDims; ++d) {
                        total += plan.rate[d];
                    }
                } else {
                    for (int d = 0; d < kDims; ++d) {
                        total += power_floor(common[m][d] * plan.rate[d], float32_mode);
                    }
                }
            }
            matrix[m][s] = total;
        }
    }
    return power_matrices_.emplace(leader_index, std::move(matrix)).first->second;
}

ChartInfo Engine::chart_info() const {
    ChartInfo info;
    info.judged_notes = chart_.judged_notes;
    info.converted_note_count = chart_.converted_note_count;
    info.note_weight_sum = chart_.base;
    info.all_note_weight_sum = chart_.all_note_weight_sum;
    info.skill_times = problem_.chart.skill_times_ms;
    return info;
}

Json Engine::chart_analysis() const {
    Json out = Json::object();
    out.set("difficulty", Json(problem_.chart.difficulty));
    out.set("level", Json(problem_.chart.level));
    out.set("display_level", problem_.chart.display_level.has_value()
                                 ? Json(*problem_.chart.display_level)
                                 : Json());
    out.set("full_combo_count", problem_.chart.full_combo_count.has_value()
                                    ? Json(*problem_.chart.full_combo_count)
                                    : Json());
    out.set("notes", Json(static_cast<int64_t>(problem_.chart.notes.size())));
    out.set("judged_notes", Json(static_cast<int64_t>(chart_.judged_notes)));
    out.set("converted_note_count", Json(chart_.converted_note_count));
    out.set("note_weight_sum", Json(chart_.base));
    out.set("all_note_weight_sum", Json(chart_.all_note_weight_sum));
    out.set("prior_combo_rule", Json("strictly_before"));
    Json times = Json::array();
    for (int64_t start : problem_.chart.skill_times_ms) {
        times.push_back(Json(start));
    }
    out.set("skill_times_ms", std::move(times));
    return out;
}

Json Engine::model_block() const {
    Json assumptions = Json::array();
    assumptions.push_back(
        Json("index = 综合力 × (1 + Σ "
             "技能收益)；技能收益按原始音符权重覆盖计算，再除以含连击加成的基线权重总和"));
    assumptions.push_back(Json("combo 采用严格早于本音符毫秒的判定数（同毫秒多押共享之前连击）"));
    assumptions.push_back(Json("综合力每条加成来源单独 float32 取整"));
    assumptions.push_back(Json("技能在谱面计划时刻触发，窗口左闭右开"));
    assumptions.push_back(Json("普通连击与 LIVE 分数倍率相乘；重叠分数 LIVE 加成相加；普通 Master "
                               "连击表在 500 连击达到 +30%；搜索按 AP"));
    if (!problem_.settings.score.level_alpha.has_value()) {
        assumptions.push_back(Json("绝对分数未计算：level_alpha 未知"));
    } else {
        assumptions.push_back(
            Json("float32_floor 使用客户端运算顺序与两次取整；level_alpha=0.005 与 BV1Fpa66aEGu 的 "
                 "Lv27×1.11 交叉吻合，可覆盖；完整分数仍待实测校准"));
    }
    Json out = Json::object();
    out.set("id", Json(kModelId));
    out.set("calibrated", Json(false));
    out.set("assumptions", std::move(assumptions));
    return out;
}

void Engine::populate_estimated_score(Evaluation &evaluation) const {
    if (!problem_.settings.score.level_alpha.has_value() || evaluation.has_estimated)
        return;
    evaluation.estimated_score =
        absolute_score_block(problem_, chart_, boosts(), member_pos_, evaluation.slots,
                             evaluation.power, evaluation.weight_factor);
    evaluation.has_estimated = true;
}

Evaluation Engine::evaluate(const Formation &formation, const EvalOptions &options) const {
    if (options.validate) {
        formation.validate(problem_);
    }
    const std::string search =
        options.order_search.empty() ? problem_.search.order_search : options.order_search;
    if (search != "exact" && search != "given") {
        throw SpecError("未知的 order_search: " + quote(search));
    }

    std::vector<Slot> ordered = formation.slots;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Slot &left, const Slot &right) {
        return left.trigger < right.trigger;
    });

    const int team_size = problem_.settings.team_size;
    if (ordered.size() != static_cast<size_t>(team_size)) {
        throw SpecError("收益矩阵形状 (" + std::to_string(ordered.size()) +
                        ",) 与 team_size=" + std::to_string(team_size) + " 不符");
    }

    std::vector<std::pair<int, int>> pairs;
    pairs.reserve(ordered.size());
    for (const Slot &slot : ordered) {
        const int member_index = member_pos(slot.member);
        if (member_index < 0) {
            throw SpecError("成员 " + std::to_string(slot.member) + " 不在可用成员中");
        }
        const int snapshot_index = snapshot_pos(slot.snapshot);
        if (snapshot_index < 0) {
            throw SpecError("Snapshot " + std::to_string(slot.snapshot) + " 不在可用卡池中");
        }
        pairs.emplace_back(member_index, snapshot_index);
    }

    const std::vector<std::vector<std::vector<double>>> &gain_grid = gains();
    std::vector<double> per_pair(static_cast<size_t>(team_size), 0.0);
    std::vector<int> trigger_for_pair(static_cast<size_t>(team_size));
    for (int i = 0; i < team_size; ++i)
        trigger_for_pair[i] = i;
    Json order_analysis;
    bool has_order = false;
    if (search == "exact" && team_size > 1) {
        std::vector<int> perm(static_cast<size_t>(team_size));
        for (int i = 0; i < team_size; ++i) {
            perm[static_cast<size_t>(i)] = i;
        }
        std::vector<int> best_perm = perm;
        double best_total = std::numeric_limits<double>::infinity();
        double worst_total = std::numeric_limits<double>::infinity();
        double gain_total = 0.0;
        int64_t gain_count = 0;
        bool first = true;
        do {
            double total = 0.0;
            for (int i = 0; i < team_size; ++i) {
                const auto &pair = pairs[static_cast<size_t>(i)];
                total +=
                    gain_grid[static_cast<size_t>(pair.first)][static_cast<size_t>(pair.second)]
                             [static_cast<size_t>(perm[static_cast<size_t>(i)])];
            }
            gain_total += total;
            ++gain_count;
            if (first || total > best_total) {
                best_total = total;
                best_perm = perm;
            }
            if (first || total < worst_total) {
                worst_total = total;
            }
            first = false;
        } while (std::next_permutation(perm.begin(), perm.end()));
        for (int i = 0; i < team_size; ++i) {
            const auto &pair = pairs[static_cast<size_t>(i)];
            per_pair[static_cast<size_t>(i)] =
                gain_grid[static_cast<size_t>(pair.first)][static_cast<size_t>(pair.second)]
                         [static_cast<size_t>(best_perm[static_cast<size_t>(i)])];
        }
        trigger_for_pair = best_perm;
        std::vector<int64_t> best_order(static_cast<size_t>(team_size), 0);
        for (int pair_index = 0; pair_index < team_size; ++pair_index) {
            const int trigger_index = best_perm[static_cast<size_t>(pair_index)];
            best_order[static_cast<size_t>(trigger_index)] =
                members_[static_cast<size_t>(pairs[static_cast<size_t>(pair_index)].first)]->id;
        }
        int64_t permutations = 1;
        for (int i = 2; i <= team_size; ++i) {
            permutations *= i;
        }
        Json order_array = Json::array();
        for (int64_t member_id : best_order) {
            order_array.push_back(Json(member_id));
        }
        order_analysis = Json::object();
        order_analysis.set("evaluated_permutations", Json(permutations));
        order_analysis.set("best_order", std::move(order_array));
        order_analysis.set("best_gain", Json(best_total));
        order_analysis.set("mean_gain", Json(gain_total / gain_count));
        order_analysis.set("activation_order", Json("random_permutation"));
        order_analysis.set("best_order_is_controllable", Json(false));
        order_analysis.set("worst_gain", Json(worst_total));
        order_analysis.set(
            "worst_loss_percent",
            Json(best_total == 0.0 ? 0.0 : (best_total - worst_total) / best_total * 100.0));
        has_order = true;
    } else {
        for (int i = 0; i < team_size; ++i) {
            const auto &pair = pairs[static_cast<size_t>(i)];
            per_pair[static_cast<size_t>(i)] =
                gain_grid[static_cast<size_t>(pair.first)][static_cast<size_t>(pair.second)]
                         [static_cast<size_t>(i)];
        }
    }

    const int leader_pos = member_pos(formation.leader);
    if (leader_pos < 0) {
        throw SpecError("队长 " + std::to_string(formation.leader) + " 不在可用成员中");
    }
    const std::vector<std::vector<int64_t>> &matrix = power_matrix(static_cast<size_t>(leader_pos));
    const std::vector<std::vector<int64_t>> &duration_matrix = durations();
    const std::vector<double> &boost_values = boosts();

    Evaluation evaluation;
    evaluation.leader = formation.leader;
    evaluation.slots.reserve(static_cast<size_t>(team_size));
    int64_t total_power = 0;
    for (int i = 0; i < team_size; ++i) {
        const size_t member_index = static_cast<size_t>(pairs[static_cast<size_t>(i)].first);
        const size_t snapshot_index = static_cast<size_t>(pairs[static_cast<size_t>(i)].second);
        const Member &member = *members_[member_index];
        const Snapshot &snapshot = *snapshots_[snapshot_index];
        const int64_t slot_power_value = matrix[member_index][snapshot_index];
        total_power += slot_power_value;

        SlotEval slot;
        slot.trigger = trigger_for_pair[static_cast<size_t>(i)] + 1;
        slot.member = member.id;
        slot.snapshot = snapshot.id;
        slot.duration_ms = duration_matrix[member_index][snapshot_index];
        slot.power = slot_power_value;
        slot.live_boost = boost_values[member_index];
        slot.weighted_skill_gain = per_pair[static_cast<size_t>(i)];
        if (options.detail) {
            slot.breakdown =
                slot_breakdown(static_cast<size_t>(leader_pos), member_index, snapshot_index);
            slot.has_breakdown = true;
        }
        evaluation.slots.push_back(std::move(slot));
    }
    const double gain_sum = numpy_sum(per_pair.data(), per_pair.size());
    evaluation.power = total_power;
    evaluation.weight_factor = 1.0 + gain_sum;
    evaluation.index = static_cast<double>(total_power) * evaluation.weight_factor;
    if (has_order) {
        order_analysis.set("best_index", Json(evaluation.index));
        order_analysis.set("mean_index",
                           Json(total_power * (1 + num_field(order_analysis, "mean_gain"))));
        order_analysis.set("worst_index",
                           Json(total_power * (1 + num_field(order_analysis, "worst_gain"))));
        order_analysis.set("score_scope",
                           Json("AP / maintained holds / no Luck / specified section rewards"));
    }

    std::vector<std::string> warnings;
    if (!problem_.settings.score.level_alpha.has_value()) {
        warnings.push_back("settings.score_model.level_alpha 未提供：estimated_score 为 null，"
                           "index 只能用于同一 problem 内的排序。");
    } else if (options.calculate_score) {
        populate_estimated_score(evaluation);
        if (has_order && options.detail) {
            std::vector<int> permutation(static_cast<size_t>(team_size));
            std::iota(permutation.begin(), permutation.end(), 0);
            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -std::numeric_limits<double>::infinity();
            double sum = 0.0;
            int64_t count = 0;
            Json best_score_order;
            do {
                auto permuted_slots = evaluation.slots;
                for (int i = 0; i < team_size; ++i)
                    permuted_slots[static_cast<size_t>(i)].trigger =
                        permutation[static_cast<size_t>(i)] + 1;
                const Json score =
                    absolute_score_block(problem_, chart_, boost_values, member_pos_,
                                         permuted_slots, total_power, evaluation.weight_factor);
                const double value = num_field(score, "total");
                if (value > maximum) {
                    maximum = value;
                    best_score_order = Json::array();
                    for (int trigger = 1; trigger <= team_size; ++trigger)
                        for (const auto &slot : permuted_slots)
                            if (slot.trigger == trigger)
                                best_score_order.push_back(Json(slot.member));
                }
                minimum = std::min(minimum, value);
                sum += value;
                ++count;
            } while (std::next_permutation(permutation.begin(), permutation.end()));
            order_analysis.set("best_score", Json(maximum));
            order_analysis.set("mean_score", Json(sum / count));
            order_analysis.set("worst_score", Json(minimum));
            order_analysis.set("best_score_order", std::move(best_score_order));
            order_analysis.set("score_permutations", Json(count));
        }
    }

    if (problem_.settings.judgement.mode != "all_perfect" &&
        problem_.settings.score.level_alpha.has_value()) {
        warnings.push_back("judgement.mode = " + quote(problem_.settings.judgement.mode) +
                           "：判定分布是模型假设，不是实测数据。");
    }
    if (problem_.settings.life.mode != "constant") {
        warnings.push_back("life.mode 非 'constant'：生命损失与技能回血未建模，结果更不可靠。");
    }
    if (problem_.settings.gekisou.enabled) {
        warnings.push_back("gekisou.enabled：分段 Bonus 只是外部系数，真人排名未建模。");
    }
    if (problem_.settings.assist.enabled) {
        warnings.push_back("assist.enabled：Assist 模式按 Master 百分比直接乘算。");
    }
    const auto &unmapped = rules_.unmapped_leader_effects();
    if (!unmapped.empty()) {
        std::string details;
        size_t shown = 0;
        for (const auto &entry : unmapped) {
            if (shown == 5) {
                break;
            }
            if (shown != 0) {
                details += "，";
            }
            details += "技能 " + std::to_string(entry.first.first) + " 类型 " +
                       std::to_string(entry.first.second) + " ×" +
                       std::to_string(entry.second.first) + "（示例 " +
                       std::to_string(entry.second.second) + "）";
            ++shown;
        }
        warnings.push_back("队长技能效果类型未在 leader_effect_axes 中定义，已按规则忽略：" +
                           details);
    }
    evaluation.warnings = std::move(warnings);
    evaluation.order_analysis = std::move(order_analysis);
    evaluation.has_order = has_order;
    return evaluation;
}

void Engine::validate_theoretical_scope() const {
    const auto &settings = problem_.settings;
    if (!settings.score.level_alpha || settings.score.rounding != "float32_floor")
        throw SpecError(
            "理论最高分需要 level_alpha 和 float32_floor；指数搜索请用 --objective index");
    if (settings.judgement.mode != "all_perfect" || settings.life.mode != "constant" ||
        settings.gekisou.enabled)
        throw SpecError("理论最高分当前支持 AP、恒定生命、无竞技击奏的课题模型；其他模型请用 "
                        "--objective index");
    if (team_size() > 5)
        throw SpecError("理论最高分支持最多五人队伍");
    for (double boost : boosts())
        if (boost < 0 || !std::isfinite(boost))
            throw SpecError("理论最高分的安全上界要求 LIVE 分数加成非负且有限");
    const double factor =
        1 + (problem_.chart.level - settings.score.level_base) * (*settings.score.level_alpha);
    if (settings.score.adjustment_factor < 0 || factor < 0 ||
        settings.judgement.factors.at("perfect") < 0 || settings.life.onus_factor < 0 ||
        settings.assist.score_percent < 0)
        throw SpecError("理论最高分的安全上界要求非负计分因子");
}

bool Engine::for_each_score_order(const Formation &formation,
                                  const std::function<bool(const Evaluation &)> &visitor) const {
    EvalOptions options;
    options.order_search = "given";
    options.calculate_score = false;
    const Evaluation base = evaluate(formation, options);
    const auto &grid = gains();
    std::vector<int> order(base.slots.size());
    std::iota(order.begin(), order.end(), 1);
    std::vector<double> per_pair(base.slots.size());
    do {
        Evaluation result = base;
        for (size_t i = 0; i < order.size(); ++i)
            result.slots[i].trigger = order[i];
        std::sort(result.slots.begin(), result.slots.end(),
                  [](const SlotEval &a, const SlotEval &b) { return a.trigger < b.trigger; });
        for (size_t i = 0; i < result.slots.size(); ++i) {
            auto &slot = result.slots[i];
            slot.weighted_skill_gain =
                grid[member_pos(slot.member)][snapshot_pos(slot.snapshot)][i];
            per_pair[i] = slot.weighted_skill_gain;
        }
        result.weight_factor = 1 + numpy_sum(per_pair.data(), per_pair.size());
        result.index = static_cast<double>(result.power) * result.weight_factor;
        populate_estimated_score(result);
        if (!visitor(result))
            return false;
    } while (std::next_permutation(order.begin(), order.end()));
    return true;
}

Evaluation Engine::evaluate_theoretical(const Formation &formation, bool detail) const {
    validate_theoretical_scope();
    formation.validate(problem_);
    std::vector<int> permutation(static_cast<size_t>(team_size()));
    std::iota(permutation.begin(), permutation.end(), 0);
    EvalOptions base_options;
    base_options.order_search = "given";
    base_options.validate = false;
    base_options.calculate_score = false;
    const Evaluation base = evaluate(formation, base_options);
    std::vector<SlotEval> original_slots;
    for (const auto &slot : formation.slots) {
        const auto found =
            std::find_if(base.slots.begin(), base.slots.end(), [&](const SlotEval &candidate) {
                return candidate.member == slot.member;
            });
        original_slots.push_back(*found);
    }
    Formation selected = formation;
    double maximum = -std::numeric_limits<double>::infinity(),
           minimum = std::numeric_limits<double>::infinity(), sum = 0;
    int64_t count = 0;
    do {
        auto slots = original_slots;
        for (int i = 0; i < team_size(); ++i)
            slots[i].trigger = permutation[i] + 1;
        std::sort(slots.begin(), slots.end(), [](const SlotEval &left, const SlotEval &right) {
            return left.trigger < right.trigger;
        });
        const Json result = absolute_score_block(problem_, chart_, boosts(), member_pos_, slots,
                                                 base.power, base.weight_factor);
        const double score = num_field(result, "total");
        if (score > maximum) {
            maximum = score;
            for (int i = 0; i < team_size(); ++i)
                selected.slots[i].trigger = permutation[i] + 1;
        }
        minimum = std::min(minimum, score);
        sum += score;
        ++count;
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    EvalOptions selected_options;
    selected_options.order_search = "given";
    selected_options.validate = false;
    selected_options.detail = detail;
    Evaluation best = evaluate(selected, selected_options);
    Json order = Json::array();
    for (int trigger = 1; trigger <= team_size(); ++trigger)
        for (const auto &slot : best.slots)
            if (slot.trigger == trigger)
                order.push_back(Json(slot.member));
    best.ranking_objective = "theoretical_score";
    best.ranking_score = maximum;
    best.has_order = true;
    best.order_analysis = Json::object();
    best.order_analysis.set("activation_order", Json("random_permutation"));
    best.order_analysis.set("best_order_is_controllable", Json(false));
    best.order_analysis.set("best_score_order", std::move(order));
    best.order_analysis.set("best_score", Json(maximum));
    best.order_analysis.set("mean_score", Json(sum / count));
    best.order_analysis.set("worst_score", Json(minimum));
    best.order_analysis.set("score_permutations", Json(count));
    best.order_analysis.set("order_search_complete", Json(true));
    best.warnings.push_back(
        "ranking_score 是 AP 且随机技能顺序最有利时的模型最高分，不是每局保证得分。");
    return best;
}

NativeScoreBound Engine::linear_score_bound(const std::vector<float> &live_bound) const {
    validate_theoretical_scope();
    NativeScoreBound result;
    const auto &e = *this;
    const auto &chart = e.chart();
    if (live_bound.size() != chart.times.size())
        throw SpecError("理论分数上界形状错误");
    const auto &settings = e.problem().settings;
    float difficulty = static_cast<float>(e.problem().chart.level - settings.score.level_base);
    difficulty *= static_cast<float>(*settings.score.level_alpha);
    difficulty += 1.0f;
    const float adjustment = static_cast<float>(settings.score.adjustment_factor);
    const float judgement = static_cast<float>(settings.judgement.factors.at("perfect"));
    const float life =
        settings.life.initial > 0 ? 1.0f : static_cast<float>(settings.life.onus_factor);
    const float assist =
        settings.assist.enabled ? static_cast<float>(settings.assist.score_percent / 100) : 1.0f;
    const float denominator = static_cast<float>(chart.converted_note_count);
    if (!std::isnormal(adjustment) || adjustment <= 0 || !std::isnormal(difficulty) ||
        difficulty <= 0 || !std::isnormal(judgement) || judgement <= 0 || !std::isnormal(life) ||
        life <= 0 || !std::isnormal(assist) || assist <= 0 || !std::isnormal(denominator) ||
        denominator <= 0)
        return result;
    std::vector<long double> coefficients(chart.times.size());
    long double base = 0;
    for (size_t i = 0; i < chart.times.size(); ++i) {
        const float weight = static_cast<float>(chart.note_weights[i]);
        const float combo = chart.native_combo_bonuses[i] + 1.0f;
        if (weight == 0)
            continue;
        if (weight < 0 || !std::isnormal(weight) || combo <= 0 || !std::isnormal(combo))
            return result;
        float check = adjustment;
        for (float operand : {difficulty, weight, judgement, combo}) {
            check *= operand;
            if (!std::isnormal(check))
                return result;
        }
        check /= denominator;
        if (!std::isnormal(check))
            return result;

        check = life * assist;
        if (!std::isnormal(check))
            return result;
        const long double coefficient = static_cast<long double>(adjustment) * difficulty * weight *
                                        judgement * combo / denominator * life * assist;
        coefficients[i] = coefficient;
        base += coefficient;
    }
    const double inflation = 1 + (chart.times.size() + 32) * std::numeric_limits<double>::epsilon();
    auto up = [&](long double v) {
        return std::nextafter(static_cast<double>(v) * inflation,
                              std::numeric_limits<double>::infinity());
    };
    result.base = up(base);
    result.gains.assign(members().size() * snapshots().size(), std::vector<double>(team_size()));
    for (size_t p = 0; p < result.gains.size(); ++p) {
        const int member = p / snapshots().size(), snap = p % snapshots().size();
        const float boost = static_cast<float>(e.live_boost(member));
        if (boost < 0 || !std::isfinite(boost))
            return result;
        for (int k = 0; k < team_size(); ++k) {
            const auto start = e.problem().chart.skill_times_ms[k];
            const auto end = start + e.duration_ms(member, snap);
            long double sum = 0;
            for (size_t i = 0; i < chart.times.size(); ++i)
                if (chart.times[i] >= start && chart.times[i] < end)
                    sum += coefficients[i] * boost;
            result.gains[p][k] = up(sum);
        }
    }
    long double peak = 1;
    float max_weight = 0, max_combo = 1, max_live = 0;
    for (size_t i = 0; i < chart.times.size(); ++i) {
        max_weight = std::max(max_weight, static_cast<float>(chart.note_weights[i]));
        max_combo = std::max(max_combo, chart.native_combo_bonuses[i] + 1.0f);
        max_live = std::max(max_live, live_bound[i]);
    }
    if (!std::isfinite(max_live))
        return result;
    for (long double f :
         {static_cast<long double>(adjustment), static_cast<long double>(difficulty),
          static_cast<long double>(max_weight), static_cast<long double>(judgement),
          static_cast<long double>(max_combo), 1 + static_cast<long double>(max_live),
          1 / static_cast<long double>(denominator), static_cast<long double>(life),
          static_cast<long double>(assist)})
        peak *= std::max(1.0L, f);
    peak *= std::pow(1 + static_cast<long double>(std::numeric_limits<float>::epsilon()), 64);
    result.max_power =
        std::nextafter(static_cast<double>(std::numeric_limits<float>::max() / peak), 0.0);
    result.enabled = true;
    return result;
}

double NativeScoreBound::upper(double power, double gain, double base, double max_power,
                               int team_size, size_t note_count) {
    const double inflated =
        std::nextafter(power * (1 + (team_size + 2) * std::numeric_limits<double>::epsilon()),
                       std::numeric_limits<double>::infinity());
    const float native_power =
        std::nextafter(static_cast<float>(inflated), std::numeric_limits<float>::infinity());
    if (!std::isfinite(native_power) || native_power > max_power)
        return std::numeric_limits<double>::infinity();
    const double rounding = std::pow(1 + std::numeric_limits<float>::epsilon(), 32 + team_size);
    const double sums = 1 + (note_count + 32) * std::numeric_limits<double>::epsilon();
    return std::nextafter(static_cast<double>(native_power) * (base + gain) * rounding * sums,
                          std::numeric_limits<double>::infinity());
}

double Engine::theoretical_upper_bound(double power_bound,
                                       const std::vector<float> &live_bound) const {
    const auto &settings = problem_.settings;
    if (live_bound.size() != chart_.times.size())
        throw SpecError("理论分数上界形状错误");

    const double inflated = std::nextafter(
        power_bound * (1 + (team_size() + 1) * std::numeric_limits<double>::epsilon()),
        std::numeric_limits<double>::infinity());
    const float p =
        std::nextafter(static_cast<float>(inflated), std::numeric_limits<float>::infinity());
    float difficulty = static_cast<float>(problem_.chart.level - settings.score.level_base);
    difficulty *= static_cast<float>(*settings.score.level_alpha);
    difficulty += 1.0f;
    float base = static_cast<float>(settings.score.adjustment_factor) * p;
    base *= difficulty;
    const float judgement = static_cast<float>(settings.judgement.factors.at("perfect"));
    const float life =
        settings.life.initial > 0 ? 1.0f : static_cast<float>(settings.life.onus_factor);
    const float assist =
        settings.assist.enabled ? static_cast<float>(settings.assist.score_percent / 100) : 1.0f;
    long double sum = 0;
    for (size_t i = 0; i < chart_.times.size(); ++i) {
        float value = static_cast<float>(chart_.note_weights[i]) * base;
        value *= judgement;
        value *= chart_.native_combo_bonuses[i] + 1.0f;
        value *= 1.0f + live_bound[i];
        value /= static_cast<float>(chart_.converted_note_count);
        value = std::floor(value);
        value *= life;
        value *= assist;
        sum += std::floor(value);
    }

    return std::nextafter(
        static_cast<double>(sum) *
            (1 + (chart_.times.size() + 16) * std::numeric_limits<double>::epsilon()),
        std::numeric_limits<double>::infinity());
}

}
