#include "gekisou.hpp"

#include "model.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace deckcalc {
namespace {

// 激奏（Gekisou / Fever Mission）算分与组队搜索核心实现。
// 致谢：算法与逐帧状态机移植自 haneoka-gakuen/haneoka (https://github.com/haneoka-gakuen/haneoka)。

inline float f32(double x) {
    return static_cast<float>(x);
}
inline float f32(float x) {
    return x;
}
inline float f32(int32_t x) {
    return static_cast<float>(x);
}
inline float f32(int64_t x) {
    return static_cast<float>(x);
}

constexpr int32_t kI32Min = std::numeric_limits<int32_t>::min();
constexpr int32_t kI32Max = std::numeric_limits<int32_t>::max();

inline int32_t wrap_add(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t wrap_sub(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline int32_t wrap_mul(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

inline int32_t sat_i32(double x) {
    if (std::isnan(x))
        return 0;
    if (x >= static_cast<double>(kI32Max))
        return kI32Max;
    if (x <= static_cast<double>(kI32Min))
        return kI32Min;
    return static_cast<int32_t>(std::trunc(x));
}

inline int32_t floor_to_i32(float x) {
    if (x == std::numeric_limits<float>::infinity())
        return kI32Min;
    int32_t t = sat_i32(static_cast<double>(x));
    return (t != kI32Min && f32(t) > x) ? (t - 1) : t;
}

inline int32_t floor_to_i32_f64(double x) {
    if (x == std::numeric_limits<double>::infinity())
        return kI32Min;
    int32_t t = sat_i32(x);
    return (t != kI32Min && static_cast<double>(t) > x) ? (t - 1) : t;
}

inline int32_t ceil_to_i32(float x) {
    if (std::isinf(x))
        return kI32Min;
    int32_t t = sat_i32(static_cast<double>(x));
    return (t != kI32Max && f32(t) < x) ? (t + 1) : t;
}

inline double round_ties_even(double x) {
    double fl = std::floor(x);
    double diff = x - fl;
    if (diff < 0.5)
        return fl;
    if (diff > 0.5)
        return fl + 1.0;
    int64_t i = static_cast<int64_t>(fl);
    return (i % 2 == 0) ? fl : (fl + 1.0);
}

inline float min_ignoring_nan(float a, float b) {
    if (std::isnan(a))
        return b;
    if (std::isnan(b))
        return a;
    if (a == b)
        return std::signbit(a) ? a : b;
    return a < b ? a : b;
}

inline int32_t sdiv(int32_t a, int32_t b) {
    if (b == 0)
        return 0;
    if (a == kI32Min && b == -1)
        return kI32Min;
    return a / b;
}

inline int32_t srem(int32_t a, int32_t b) {
    return wrap_sub(a, wrap_mul(sdiv(a, b), b));
}

inline int32_t get_frame(int32_t ms) {
    return ms < 0 ? 0 : ceil_to_i32(f32(f32(ms) / 40.0f));
}

// .NET System.Random 减法随机数发生器，对齐客户端抽签与概率判定流
constexpr int32_t kMBig = 2147483647;
constexpr int32_t kMSeed = 161803398;
constexpr int kStreamSkill = 0;
constexpr int kStreamLuck = 1;

struct NetRandom {
    std::array<int32_t, 56> seeds{};
    int inext = 0;
    int inextp = 21;

    explicit NetRandom(int32_t seed = 0) {
        int32_t subtraction = (seed == kI32Min) ? kMBig : std::abs(seed);
        int32_t mj = wrap_sub(kMSeed, subtraction);
        seeds[55] = mj;
        int32_t mk = 1;
        int ii = 0;
        for (int i = 1; i < 55; ++i) {
            ii += 21;
            if (ii >= 55)
                ii -= 55;
            seeds[ii] = mk;
            mk = wrap_sub(mj, mk);
            if (mk < 0)
                mk = wrap_add(mk, kMBig);
            mj = seeds[ii];
        }
        for (int k = 1; k < 5; ++k) {
            for (int i = 1; i < 56; ++i) {
                int n = i + 30;
                if (n >= 55)
                    n -= 55;
                seeds[i] = wrap_sub(seeds[i], seeds[1 + n]);
                if (seeds[i] < 0)
                    seeds[i] = wrap_add(seeds[i], kMBig);
            }
        }
    }

    int32_t internal_sample() {
        int a = inext + 1;
        int b = inextp + 1;
        if (a >= 56)
            a = 1;
        if (b >= 56)
            b = 1;
        int32_t r = wrap_sub(seeds[a], seeds[b]);
        if (r == kMBig)
            --r;
        if (r < 0)
            r = wrap_add(r, kMBig);
        seeds[a] = r;
        inext = a;
        inextp = b;
        return r;
    }

    double next_double() {
        return static_cast<double>(internal_sample()) * (1.0 / kMBig);
    }

    int32_t next_int() {
        int32_t result = internal_sample();
        if (internal_sample() % 2 == 0)
            result = -result;
        double d = static_cast<double>(result) + static_cast<double>(kMBig - 1);
        d /= static_cast<double>(2LL * kMBig - 1);
        int64_t tr = static_cast<int64_t>(std::trunc(d * 4294967295.0)) - 2147483648LL;
        return static_cast<int32_t>(tr);
    }
};

struct LiveRandom {
    static constexpr int kSkill = 0;
    static constexpr int kLuck = 1;
    std::array<NetRandom, 4> streams;
    int64_t draws = 0;

    explicit LiveRandom(int32_t base_seed = 0)
        : streams{
              NetRandom(wrap_mul(0, -0x61c88647) ^ base_seed),
              NetRandom(wrap_mul(1, -0x61c88647) ^ base_seed),
              NetRandom(wrap_mul(2, -0x61c88647) ^ base_seed),
              NetRandom(wrap_mul(3, -0x61c88647) ^ base_seed),
          } {}

    float value(int stream) {
        ++draws;
        return f32(streams[stream].next_double());
    }

    int32_t next_int(int stream) {
        ++draws;
        return streams[stream].next_int();
    }
};

using Stat3 = std::array<int32_t, 3>;
using BP3 = std::array<int64_t, 3>;

struct TargetRow {
    int32_t id = 0;
    int32_t band_id = 0;
    int32_t card_type = 0;
    int32_t character_id = 0;
    int32_t tag_id = 0;
    std::vector<int32_t> live_skill_categories;
    std::vector<int32_t> gekisou_skill_categories;
    int32_t gekisou_mission_type = 0;
    int32_t live_music_type = 0;
    int32_t judgement = -1;
    int32_t skill_target_type = 0;
};

struct ConditionRow {
    int32_t id = 0;
    int32_t type = 0;
    std::vector<int32_t> target_ids;
    std::vector<int32_t> values;
    bool positive = true;
};

struct CumulativeRow {
    int32_t id = 0;
    int32_t type = 0;
    std::vector<int32_t> target_ids;
    std::vector<int32_t> values;
    int32_t cap = 0;
};

struct SkillEffectRow {
    int32_t id = 0;
    int32_t level = 0;
    int32_t trigger_type = 0;
    int32_t execute_limit = 0;
    int32_t reset_group = 0;
    int32_t type = 0;
    int32_t value = 0;
    double seconds = 0.0;
    int32_t condition_group = 0;
    int32_t trigger_group = 0;
    int32_t release_group = 0;
    int32_t cumulative_id = 0;
    std::vector<int32_t> target_ids;
    int32_t limit_count = 0;
    int32_t max_value = 0;
};

struct GkMemberCard {
    int32_t id = 0;
    int32_t character_id = 0;
    int32_t band_id = 0;
    int32_t rarity = 0;
    int32_t card_type = 0;
    std::vector<int32_t> best_tags;
    Stat3 stat_max{0, 0, 0};
    int32_t level_group = 0;
    int32_t awake_group = 0;
    int32_t rank_group = 0;
    int32_t live_skill_id = 0;
    int32_t gekiso_skill_id = 0;
    int32_t leader_skill_id = 0;
};

struct GkSnapCard {
    int32_t id = 0;
    std::vector<int32_t> character_ids;
    std::vector<int32_t> band_ids;
    int32_t rarity = 0;
    int32_t card_type = 0;
    Stat3 stat_max{0, 0, 0};
    int32_t level_group = 0;
    int32_t rank_group = 0;
    std::array<int32_t, 2> support_skill_ids{0, 0};
    std::array<int32_t, 2> gekiso_support_skill_ids{0, 0};
};

struct MemberRankRow {
    Stat3 rates{0, 0, 0};
    int32_t leader_skill_level = 1;
    int32_t music_type_rate = 0;
    int32_t music_tag_rate = 0;
};

struct SnapRankRow {
    int32_t type_link_rate = 0;
    int32_t limit_level = 0;
    std::array<int32_t, 2> skill_levels{0, 0};
    std::array<int32_t, 2> gekiso_skill_levels{0, 0};
};

struct LuckBasePointRow {
    int32_t category = 0;
    int32_t judgement = 0;
    int32_t weight = 0;
    int32_t point = 0;
};

struct LuckBonusLotRow {
    int32_t lot_type = 0;
    int32_t result = 0;
    int32_t weight = 0;
};

struct RankingBonusRow {
    int32_t pattern = 0;
    int32_t count = 0;
    int32_t rank = 0;
    int32_t percent = 0;
};

struct GekisouTables {
    std::vector<LuckBasePointRow> luck_base_points;
    std::vector<LuckBonusLotRow> luck_bonus_lots;
    std::vector<RankingBonusRow> ranking_bonuses;
    int32_t gauge_max = 100;
    int32_t gauge_max_rush = 50;
    int32_t rush_percent = 0;
};

struct GekisoSkillEntry {
    int32_t mission_type = 0;
    std::vector<SkillEffectRow> effects;
};

struct BandItemEntry {
    int32_t band_id = 0;
    std::unordered_map<int32_t, std::vector<SkillEffectRow>> levels;
};

struct EngineMaster {
    struct Params {
        int32_t music_type_base = 500;
        int32_t music_tag_base = 500;
        int32_t type_link_base = 500;
    } params;

    struct Live {
        float adjustment = 3.0f;
        float life_onus = 0.3f;
        int32_t life_base = 1000;
        std::unordered_map<int32_t, int32_t> note_percent;
        std::unordered_map<int32_t, int32_t> judge_percent;
        std::unordered_map<int32_t, std::vector<std::pair<int32_t, float>>> combo;
        int32_t assist_percent = 90;
        std::array<int32_t, 7> damage{0, 0, 0, 0, 0, 0, 0};
        std::unordered_set<int32_t> just_types;
        std::unordered_set<int32_t> timing_types;
        int32_t after_ms = 0;
        std::unordered_map<int32_t, int32_t> phases;
        GekisouTables gekiso;
    } live;

    std::unordered_map<int32_t, int32_t> character_band;
    std::unordered_map<int32_t, GkMemberCard> members;
    std::unordered_map<int32_t, GkSnapCard> snaps;
    std::unordered_map<int32_t, std::unordered_map<int32_t, Stat3>> member_levels;
    std::unordered_map<int32_t, std::unordered_map<int32_t, Stat3>> member_awake;
    std::unordered_map<int32_t, std::unordered_map<int32_t, MemberRankRow>> member_ranks;
    std::unordered_map<int64_t, int32_t> member_level_limits;
    std::unordered_map<int32_t, std::unordered_map<int32_t, Stat3>> snap_levels;
    std::unordered_map<int32_t, std::unordered_map<int32_t, SnapRankRow>> snap_ranks;
    std::vector<std::pair<int32_t, int32_t>> character_rank_bonus;
    std::vector<std::pair<int32_t, int32_t>> total_rank_bonus;
    std::unordered_map<int32_t, int32_t> vip_bonus;
    std::unordered_map<int32_t, TargetRow> targets;
    std::unordered_map<int32_t, ConditionRow> conditions;
    std::unordered_map<int32_t, std::vector<std::vector<int32_t>>> condition_sets;
    std::unordered_map<int32_t, CumulativeRow> cumulative;
    std::unordered_map<int32_t, std::vector<SkillEffectRow>> leader_skills;
    std::unordered_map<int32_t, std::vector<SkillEffectRow>> live_skills;
    std::unordered_map<int32_t, std::vector<SkillEffectRow>> support_skills;
    std::unordered_map<int32_t, GekisoSkillEntry> gekiso_skills;
    std::unordered_map<int32_t, GekisoSkillEntry> gekiso_support_skills;
    std::unordered_map<int32_t, std::vector<int32_t>> live_skill_categories;
    std::unordered_map<int32_t, std::vector<int32_t>> gekiso_skill_categories;
    std::unordered_map<int32_t, BandItemEntry> band_items;
};

const std::array<const char *, 36> kAll36Tables = {
    "MasterCharacter",
    "MasterLiveMusic",
    "MasterLiveMusicScore",
    "MasterMemberCard",
    "MasterMemberCardAwake",
    "MasterMemberCardLevel",
    "MasterMemberCardRank",
    "MasterSupportCard",
    "MasterSupportCardLevel",
    "MasterSupportCardRank",
    "MasterCharacterRank",
    "MasterCharacterTotalRank",
    "MasterVipRankBonus",
    "MasterLiveSkill",
    "MasterLiveSkillEffect",
    "MasterGekisouSkill",
    "MasterGekisouSkillEffect",
    "MasterLeaderSkill",
    "MasterLeaderSkillEffect",
    "MasterSupportSkill",
    "MasterSupportSkillEffect",
    "MasterGekisouSupportSkill",
    "MasterGekisouSupportSkillEffect",
    "MasterSkillTarget",
    "MasterSkillCondition",
    "MasterSkillConditionSet",
    "MasterSkillCumulativeCondition",
    "MasterLiveNoteParameter",
    "MasterLiveComboScoreBonus",
    "MasterLiveGekisouRankingScoreBonus",
    "MasterLiveGekisouLuckBasePoint",
    "MasterLiveGekisouLuckBonusLot",
    "MasterParameter",
    "MasterLiveSettings",
    "MasterLiveJudgementParameter",
    "MasterLiveJudgementTiming",
};

const std::unordered_set<int32_t> kSupportedEffectTypes = {
    0,     1000,  1001,  1002,  1003,  1500,  1501,  1502,  1503,  2000,  2001,  2002,  2003,
    2004,  2005,  3000,  3001,  3002,  3003,  3004,  4004,  11000, 11001, 11002, 11003, 11004,
    11005, 12000, 12002, 12003, 12004, 12006, 13000, 13002, 13003, 13004, 13005, 15000,
};

const Json *row_field(const Json &row, const std::string &name, const std::string &alt = "") {
    if (!row.is_object())
        return nullptr;
    if (const Json *p = row.find(name))
        return p;
    if (const Json *p = row.find("_" + name))
        return p;
    if (!alt.empty()) {
        if (const Json *p = row.find(alt))
            return p;
        if (const Json *p = row.find("_" + alt))
            return p;
    }
    if (const Json *raw = row.find("raw"); raw && raw->is_object()) {
        if (const Json *p = raw->find(name))
            return p;
        if (const Json *p = raw->find("_" + name))
            return p;
        if (!alt.empty()) {
            if (const Json *p = raw->find(alt))
                return p;
            if (const Json *p = raw->find("_" + alt))
                return p;
        }
    }
    return nullptr;
}

double json_num(const Json *val, double fallback = 0.0) {
    if (!val || val->is_null())
        return fallback;
    if (val->is_number()) {
        double v = val->as_double();
        return std::isfinite(v) ? v : fallback;
    }
    if (val->is_string()) {
        const std::string &s = val->as_string();
        if (s.empty())
            return fallback;
        try {
            size_t idx = 0;
            double v = std::stod(s, &idx);
            return std::isfinite(v) ? v : fallback;
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

int32_t json_i32(const Json *val, int32_t fallback = 0) {
    return static_cast<int32_t>(json_num(val, static_cast<double>(fallback)));
}

int32_t row_i32(const Json &row, const std::string &name, int32_t fallback = 0,
                const std::string &alt = "") {
    return json_i32(row_field(row, name, alt), fallback);
}

double row_f64(const Json &row, const std::string &name, double fallback = 0.0,
               const std::string &alt = "") {
    return json_num(row_field(row, name, alt), fallback);
}

std::string row_str(const Json &row, const std::string &name, const std::string &fallback = "") {
    const Json *v = row_field(row, name);
    if (!v || v->is_null())
        return fallback;
    if (v->is_string())
        return v->as_string();
    if (v->is_integer())
        return std::to_string(v->as_int64());
    if (v->is_number())
        return std::to_string(v->as_double());
    return fallback;
}

std::vector<int32_t> row_ids(const Json &row, const std::string &name,
                             const std::string &alt = "") {
    std::vector<int32_t> out;
    const Json *v = row_field(row, name, alt);
    if (!v || !v->is_array())
        return out;
    for (const Json &item : v->as_array()) {
        int32_t id = json_i32(&item, 0);
        if (id > 0)
            out.push_back(id);
    }
    return out;
}

std::vector<int32_t> row_list(const Json &row, const std::string &name,
                              const std::string &alt = "") {
    std::vector<int32_t> out;
    const Json *v = row_field(row, name, alt);
    if (!v || !v->is_array())
        return out;
    for (const Json &item : v->as_array()) {
        out.push_back(json_i32(&item, 0));
    }
    return out;
}

Stat3 row_stat(const Json &row, const std::string &p, const std::string &t, const std::string &v) {
    return {row_i32(row, p), row_i32(row, t), row_i32(row, v)};
}

SkillEffectRow parse_skill_effect(const Json &row) {
    SkillEffectRow out;
    out.id = row_i32(row, "id");
    out.level = row_i32(row, "level");
    out.trigger_type = row_i32(row, "skillTriggerType");
    out.execute_limit = row_i32(row, "effectExecuteLimitCount");
    out.reset_group = row_i32(row, "effectExecuteLimitResetConditionGroup");
    out.type = row_i32(row, "skillEffectType", 0, "effectType");
    out.value = row_i32(row, "effectValue");
    out.seconds = row_f64(row, "activationTimeSecond");
    out.condition_group = row_i32(row, "skillConditionGroup", 0, "conditionGroup");
    out.trigger_group = row_i32(row, "skillTriggerConditionGroup", 0, "triggerConditionGroup");
    out.release_group = row_i32(row, "skillReleaseConditionGroup", 0, "releaseConditionGroup");
    out.cumulative_id = row_i32(row, "skillCumulativeConditionID", 0, "cumulativeConditionId");
    out.target_ids = row_ids(row, "skillTargetIDs", "targetIds");
    out.limit_count = row_i32(row, "effectLimitCount");
    out.max_value = row_i32(row, "maxEffectValue");
    return out;
}

TargetRow parse_target_row(const Json &row) {
    TargetRow out;
    out.id = row_i32(row, "id");
    out.band_id = row_i32(row, "bandID", 0, "bandId");
    out.card_type = row_i32(row, "cardType");
    out.character_id = row_i32(row, "characterID", 0, "characterId");
    out.tag_id = row_i32(row, "tagID", 0, "tagId");
    out.live_skill_categories = row_list(row, "liveSkillCategories");
    out.gekisou_skill_categories = row_list(row, "gekisouSkillCategories");
    out.gekisou_mission_type = row_i32(row, "gekisouMissionType");
    out.live_music_type = row_i32(row, "liveMusicType");
    out.judgement = row_i32(row, "judgement", -1);
    out.skill_target_type = row_i32(row, "skillTargetType");
    return out;
}

EngineMaster load_master(const std::string &master_dir, const Json *tables_map) {
    std::vector<std::string> missing;
    for (const char *t : kAll36Tables) {
        if (tables_map && tables_map->is_object() && tables_map->contains(t))
            continue;
        if (!master_dir.empty() &&
            std::filesystem::exists(std::filesystem::path(master_dir) / (std::string(t) + ".json")))
            continue;
        missing.push_back(t);
    }
    if (!missing.empty()) {
        std::string msg =
            "缺少必需的 Master 数据表 (" + std::to_string(missing.size()) + "/36 张缺失): ";
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i > 0)
                msg += ", ";
            msg += missing[i];
        }
        throw SpecError(msg);
    }

    std::unordered_map<std::string, Json> loaded_files;
    auto read_table = [&](const std::string &name) -> const Json::array_t * {
        const Json *src = nullptr;
        if (tables_map && tables_map->is_object())
            src = tables_map->find(name);
        if (!src && !master_dir.empty()) {
            auto p = std::filesystem::path(master_dir) / (name + ".json");
            if (std::filesystem::exists(p)) {
                auto [it, _] = loaded_files.emplace(name, Json::read(p.string()));
                src = &it->second;
            }
        }
        if (!src)
            return nullptr;
        if (src->is_array())
            return &src->as_array();
        if (src->is_object()) {
            if (const Json *a = src->find("_allData"); a && a->is_array())
                return &a->as_array();
            if (const Json *a = src->find("entries"); a && a->is_array())
                return &a->as_array();
        }
        return nullptr;
    };

    const Json::array_t empty_arr;
    auto rows = [&](const std::string &name) -> const Json::array_t & {
        const Json::array_t *p = read_table(name);
        return p ? *p : empty_arr;
    };

    EngineMaster master;
    const auto &parameters = rows("MasterParameter");
    auto get_param = [&](const std::string &key, int32_t fallback) -> int32_t {
        for (const Json &r : parameters) {
            if (row_str(r, "id") == key)
                return row_i32(r, "value", fallback);
        }
        return fallback;
    };
    master.params.music_type_base = get_param("music_type_base_bonus_rate", 500);
    master.params.music_tag_base = get_param("music_tag_base_bonus_rate", 500);
    master.params.type_link_base = get_param("type_link_base_bonus_rate", 500);

    const auto &live_settings = rows("MasterLiveSettings");
    auto get_setting = [&](const std::string &key, double fallback) -> double {
        for (const Json &r : live_settings) {
            if (row_str(r, "key") == key)
                return row_f64(r, "value", fallback);
        }
        return fallback;
    };
    master.live.adjustment = f32(get_setting("note_score_adjustment_factor", 3.0));
    master.live.life_onus = f32(get_setting("note_score_life_onus_factor", 0.3));
    master.live.life_base = static_cast<int32_t>(get_setting("life_base", 1000.0));
    master.live.assist_percent = static_cast<int32_t>(get_setting("assist_score_percent", 90.0));
    master.live.gekiso.gauge_max =
        static_cast<int32_t>(get_setting("gekisou_luck_gauge_max", 100.0));
    master.live.gekiso.gauge_max_rush =
        static_cast<int32_t>(get_setting("gekisou_luck_gauge_max_rush", 50.0));
    master.live.gekiso.rush_percent =
        static_cast<int32_t>(get_setting("gekisou_luck_rush_score_bonus_percent", 0.0));

    for (const Json &r : rows("MasterLiveNoteParameter")) {
        master.live.note_percent[row_i32(r, "noteOperateType")] = row_i32(r, "scorePercent");
    }
    for (const Json &r : rows("MasterLiveJudgementParameter")) {
        int32_t sim = row_i32(r, "noteSimulateJudgement", -1);
        if (sim >= 1 && sim <= 6) {
            master.live.judge_percent[7 - sim] = row_i32(r, "scorePercent");
            master.live.damage[sim] = row_i32(r, "damage");
        }
    }

    std::unordered_map<int32_t, std::vector<std::pair<int32_t, float>>> raw_combo;
    for (const Json &r : rows("MasterLiveComboScoreBonus")) {
        raw_combo[row_i32(r, "comboBonusType")].emplace_back(row_i32(r, "requiredComboCount"),
                                                             f32(row_f64(r, "bonusFactor")));
    }
    for (auto &[type, items] : raw_combo) {
        std::stable_sort(items.begin(), items.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        float sum = 0.0f;
        for (auto &it : items) {
            sum = f32(sum + it.second);
            it.second = sum;
        }
        master.live.combo[type] = std::move(items);
    }

    int32_t max_after = 0;
    for (const Json &r : rows("MasterLiveJudgementTiming")) {
        int32_t jt = row_i32(r, "noteJudgementType");
        master.live.timing_types.insert(jt);
        if (row_i32(r, "noteSimulateJudgement") == 6)
            master.live.just_types.insert(jt);
        int32_t after = row_i32(r, "afterMs", 0, "after");
        if (after > max_after)
            max_after = after;
    }
    master.live.after_ms = max_after;

    for (const Json &r : rows("MasterSkillEffectSetting")) {
        master.live.phases[row_i32(r, "skillEffectType")] = row_i32(r, "phase", 1);
    }

    for (const Json &r : rows("MasterLiveGekisouLuckBasePoint")) {
        master.live.gekiso.luck_base_points.push_back({
            row_i32(r, "noteCategory"),
            row_i32(r, "noteSimulateJudgement"),
            row_i32(r, "weight"),
            row_i32(r, "basePoint"),
        });
    }
    for (const Json &r : rows("MasterLiveGekisouLuckBonusLot")) {
        master.live.gekiso.luck_bonus_lots.push_back({
            row_i32(r, "chanceLotType"),
            row_i32(r, "lotResult"),
            row_i32(r, "weight"),
        });
    }
    for (const Json &r : rows("MasterLiveGekisouRankingScoreBonus")) {
        master.live.gekiso.ranking_bonuses.push_back({
            row_i32(r, "missionPattern"),
            row_i32(r, "count"),
            row_i32(r, "rank"),
            row_i32(r, "scoreBonusPercent"),
        });
    }

    for (const Json &r : rows("MasterCharacter")) {
        int32_t cid = row_i32(r, "id", 0, "characterId");
        master.character_band[cid] = row_i32(r, "bandID", 0, "bandId");
    }

    for (const Json &r : rows("MasterMemberCard")) {
        GkMemberCard card;
        card.id = row_i32(r, "id");
        card.character_id = row_i32(r, "characterID", 0, "characterId");
        auto bit = master.character_band.find(card.character_id);
        card.band_id = (bit != master.character_band.end()) ? bit->second : 0;
        card.rarity = row_i32(r, "rarity");
        card.card_type = row_i32(r, "cardType");
        card.best_tags = row_ids(r, "bestMusicTagIDs", "bestMusicTagIds");
        card.stat_max = row_stat(r, "performancePowerMax", "technicPowerMax", "visualPowerMax");
        card.level_group = row_i32(r, "memberCardLevelGroup");
        card.awake_group = row_i32(r, "memberCardAwakeGroup");
        card.rank_group = row_i32(r, "memberCardRankGroup");
        card.live_skill_id = row_i32(r, "liveSkillID", 0, "liveSkillId");
        card.gekiso_skill_id = row_i32(r, "gekisouSkillID", 0, "gekisouSkillId");
        card.leader_skill_id = row_i32(r, "leaderSkillID", 0, "leaderSkillId");
        master.members[card.id] = std::move(card);
    }

    std::unordered_set<int32_t> gk_sup_ids;
    for (const Json &r : rows("MasterGekisouSupportSkill")) {
        gk_sup_ids.insert(row_i32(r, "id"));
    }
    auto norm_gk = [&](int32_t id) -> int32_t {
        if (gk_sup_ids.count(id))
            return id;
        if (id > 1000 && gk_sup_ids.count(id / 1000))
            return id / 1000;
        return id;
    };
    for (const Json &r : rows("MasterSupportCard")) {
        GkSnapCard card;
        card.id = row_i32(r, "id");
        card.character_ids = row_ids(r, "characterIDs", "characterIds");
        for (int32_t cid : card.character_ids) {
            int32_t bid = master.character_band[cid];
            if (bid != 0 &&
                std::find(card.band_ids.begin(), card.band_ids.end(), bid) == card.band_ids.end()) {
                card.band_ids.push_back(bid);
            }
        }
        card.rarity = row_i32(r, "rarity");
        card.card_type = row_i32(r, "cardType");
        card.stat_max = row_stat(r, "performancePowerMax", "technicPowerMax", "visualPowerMax");
        card.level_group = row_i32(r, "supportCardLevelGroup");
        card.rank_group = row_i32(r, "supportCardRankGroup");
        card.support_skill_ids = {row_i32(r, "supportSkillId01"), row_i32(r, "supportSkillId02")};
        card.gekiso_support_skill_ids = {
            norm_gk(row_i32(r, "gekisouSupportSkillId01")),
            norm_gk(row_i32(r, "gekisouSupportSkillId02")),
        };
        master.snaps[card.id] = std::move(card);
    }

    for (const Json &r : rows("MasterMemberCardLevel")) {
        master.member_levels[row_i32(r, "group")][row_i32(r, "level")] =
            row_stat(r, "performanceRate", "technicRate", "visualRate");
    }
    for (const Json &r : rows("MasterMemberCardAwake")) {
        master.member_awake[row_i32(r, "group")][row_i32(r, "awakeCount")] =
            row_stat(r, "performanceRate", "technicRate", "visualRate");
    }
    for (const Json &r : rows("MasterMemberCardRank")) {
        master.member_ranks[row_i32(r, "group")][row_i32(r, "rank")] = {
            row_stat(r, "performanceRate", "technicRate", "visualRate"),
            row_i32(r, "leaderSkillLevel", 1),
            row_i32(r, "musicTypeBonusRate"),
            row_i32(r, "musicTagBonusRate"),
        };
    }
    for (const Json &r : rows("MasterMemberCardLevelLimit")) {
        int64_t key = (static_cast<int64_t>(row_i32(r, "rarity")) << 32) |
                      static_cast<uint32_t>(row_i32(r, "awakeCount"));
        master.member_level_limits[key] = row_i32(r, "limitLevel");
    }
    for (const Json &r : rows("MasterSupportCardLevel")) {
        master.snap_levels[row_i32(r, "group")][row_i32(r, "level")] =
            row_stat(r, "performanceRate", "technicRate", "visualRate");
    }
    for (const Json &r : rows("MasterSupportCardRank")) {
        master.snap_ranks[row_i32(r, "group")][row_i32(r, "rank")] = {
            row_i32(r, "cardTypeLinkBonusRate"),
            row_i32(r, "limitLevel"),
            {row_i32(r, "supportSkill01Level"), row_i32(r, "supportSkill02Level")},
            {row_i32(r, "gekisouSupportSkill01Level"), row_i32(r, "gekisouSupportSkill02Level")},
        };
    }
    for (const Json &r : rows("MasterCharacterRank")) {
        master.character_rank_bonus.emplace_back(row_i32(r, "rank"), row_i32(r, "bonus"));
    }
    std::sort(master.character_rank_bonus.begin(), master.character_rank_bonus.end());
    for (const Json &r : rows("MasterCharacterTotalRank")) {
        master.total_rank_bonus.emplace_back(row_i32(r, "totalRank"), row_i32(r, "bonus"));
    }
    std::sort(master.total_rank_bonus.begin(), master.total_rank_bonus.end());
    for (const Json &r : rows("MasterVipRankBonus")) {
        if (row_i32(r, "vipBonusType") == 7)
            master.vip_bonus[row_i32(r, "vipRank")] = row_i32(r, "value");
    }

    for (const Json &r : rows("MasterSkillTarget")) {
        TargetRow tr = parse_target_row(r);
        master.targets[tr.id] = std::move(tr);
    }
    for (const Json &r : rows("MasterSkillCondition")) {
        ConditionRow cr;
        cr.id = row_i32(r, "id");
        cr.type = row_i32(r, "conditionType");
        cr.target_ids = row_ids(r, "conditionTargetIDs");
        cr.values = row_list(r, "conditionValues");
        const Json *pos = row_field(r, "isPositive");
        cr.positive = !(pos && pos->is_bool() && !pos->as_bool());
        master.conditions[cr.id] = std::move(cr);
    }
    for (const Json &r : rows("MasterSkillConditionSet")) {
        master.condition_sets[row_i32(r, "group")].push_back(row_ids(r, "conditionIds"));
    }
    for (const Json &r : rows("MasterSkillCumulativeCondition")) {
        CumulativeRow cr;
        cr.id = row_i32(r, "id");
        cr.type = row_i32(r, "skillCumulativeConditionType", 0, "conditionType");
        cr.target_ids = row_ids(r, "conditionTargetIDs");
        cr.values = row_list(r, "conditionValues");
        cr.cap = row_i32(r, "maxCumulativeCount");
        master.cumulative[cr.id] = std::move(cr);
    }

    auto load_skill_map = [&](const std::string &skill_table, const std::string &effect_table,
                              const std::string &fk) {
        std::unordered_map<int32_t, std::vector<SkillEffectRow>> grouped;
        for (const Json &r : rows(effect_table)) {
            grouped[row_i32(r, fk)].push_back(parse_skill_effect(r));
        }
        std::unordered_map<int32_t, std::vector<SkillEffectRow>> out;
        for (const Json &r : rows(skill_table)) {
            int32_t sid = row_i32(r, "id");
            out[sid] = grouped[sid];
        }
        return out;
    };
    master.leader_skills =
        load_skill_map("MasterLeaderSkill", "MasterLeaderSkillEffect", "leaderSkillID");
    master.live_skills = load_skill_map("MasterLiveSkill", "MasterLiveSkillEffect", "liveSkillID");
    master.support_skills =
        load_skill_map("MasterSupportSkill", "MasterSupportSkillEffect", "supportSkillID");

    auto load_gekiso_map = [&](const std::string &skill_table, const std::string &effect_table,
                               const std::string &fk) {
        std::unordered_map<int32_t, std::vector<SkillEffectRow>> grouped;
        for (const Json &r : rows(effect_table)) {
            grouped[row_i32(r, fk)].push_back(parse_skill_effect(r));
        }
        std::unordered_map<int32_t, GekisoSkillEntry> out;
        for (const Json &r : rows(skill_table)) {
            int32_t sid = row_i32(r, "id");
            out[sid] = {row_i32(r, "gekisouMissionType"), std::move(grouped[sid])};
        }
        return out;
    };
    master.gekiso_skills =
        load_gekiso_map("MasterGekisouSkill", "MasterGekisouSkillEffect", "gekisouSkillID");
    master.gekiso_support_skills = load_gekiso_map(
        "MasterGekisouSupportSkill", "MasterGekisouSupportSkillEffect", "gekisouSupportSkillID");

    for (const Json &r : rows("MasterLiveSkill")) {
        master.live_skill_categories[row_i32(r, "id")] = row_list(r, "skillCategories");
    }
    for (const Json &r : rows("MasterGekisouSkill")) {
        master.gekiso_skill_categories[row_i32(r, "id")] = row_list(r, "skillCategories");
    }
    for (const Json &r : rows("MasterBandItem")) {
        BandItemEntry item;
        item.band_id = row_i32(r, "bandID", 0, "bandId");
        master.band_items[row_i32(r, "id")] = std::move(item);
    }
    for (const Json &r : rows("MasterBandItemSkillEffect")) {
        int32_t id = row_i32(r, "bandItemID", 0, "bandItemId");
        auto it = master.band_items.find(id);
        if (it == master.band_items.end())
            continue;
        auto effect = parse_skill_effect(r);
        it->second.levels[effect.level].push_back(std::move(effect));
    }

    return master;
}

// 卡盒养成解析与综合力计算
struct MemberGrowth {
    int32_t level = 1;
    int32_t awake = 1;
    int32_t rank = 1;
    int32_t live_skill_level = 1;
    int32_t gekiso_skill_level = 1;
};

struct SnapGrowth {
    int32_t level = 1;
    int32_t rank = 1;
};

struct MemberState {
    std::string key;
    GkMemberCard card;
    MemberGrowth growth;
    int32_t leader_skill_level = 1;
    int32_t music_type_rate = 0;
    int32_t music_tag_rate = 0;
    BP3 own{0, 0, 0};
    std::vector<int32_t> live_skill_categories;
    std::vector<int32_t> gekiso_skill_categories;
    int32_t gekiso_mission_type = 0;
};

struct SnapState {
    std::string key;
    GkSnapCard card;
    SnapGrowth growth;
    BP3 percent{0, 0, 0};
    int32_t type_link_rate = 0;
    std::array<int32_t, 2> skill_levels{0, 0};
    std::array<int32_t, 2> gekiso_skill_levels{0, 0};
};

struct MemberInput {
    std::string key;
    int32_t card_id = 0;
    std::optional<int32_t> level;
    std::optional<int32_t> awake;
    std::optional<int32_t> rank;
    std::optional<int32_t> live_skill_level;
    std::optional<int32_t> gekiso_skill_level;
};

struct SnapInput {
    std::string key;
    int32_t card_id = 0;
    std::optional<int32_t> level;
    std::optional<int32_t> rank;
};

inline int64_t floor_f32_div_10000(int64_t numerator) {
    return static_cast<int64_t>(std::floor(f32(f32(numerator) / 10000.0f)));
}

int32_t member_level_cap(const EngineMaster &master, const GkMemberCard &card, int32_t awake) {
    int64_t k = (static_cast<int64_t>(card.rarity) << 32) | static_cast<uint32_t>(awake);
    auto it = master.member_level_limits.find(k);
    if (it != master.member_level_limits.end())
        return it->second;
    int32_t mx = 1;
    auto git = master.member_levels.find(card.level_group);
    if (git != master.member_levels.end()) {
        for (const auto &[lv, _] : git->second)
            mx = std::max(mx, lv);
    }
    return mx;
}

int32_t snap_level_cap(const EngineMaster &master, const GkSnapCard &card, int32_t rank) {
    auto rgit = master.snap_ranks.find(card.rank_group);
    if (rgit != master.snap_ranks.end()) {
        auto rit = rgit->second.find(rank);
        if (rit != rgit->second.end())
            return rit->second.limit_level;
    }
    int32_t mx = 1;
    auto lgit = master.snap_levels.find(card.level_group);
    if (lgit != master.snap_levels.end()) {
        for (const auto &[lv, _] : lgit->second)
            mx = std::max(mx, lv);
    }
    return mx;
}

int32_t max_awake(const EngineMaster &master, const GkMemberCard &card) {
    int32_t mx = 1;
    auto it = master.member_awake.find(card.awake_group);
    if (it != master.member_awake.end()) {
        for (const auto &[aw, _] : it->second)
            mx = std::max(mx, aw);
    }
    return mx;
}

int32_t max_member_rank(const EngineMaster &master, const GkMemberCard &card) {
    int32_t mx = 1;
    auto it = master.member_ranks.find(card.rank_group);
    if (it != master.member_ranks.end()) {
        for (const auto &[rk, _] : it->second)
            mx = std::max(mx, rk);
    }
    return mx;
}

int32_t max_snap_rank(const EngineMaster &master, const GkSnapCard &card) {
    int32_t mx = 1;
    auto it = master.snap_ranks.find(card.rank_group);
    if (it != master.snap_ranks.end()) {
        for (const auto &[rk, _] : it->second)
            mx = std::max(mx, rk);
    }
    return mx;
}

int32_t max_skill_level(const std::vector<SkillEffectRow> *rows) {
    int32_t mx = 1;
    if (rows) {
        for (const auto &r : *rows)
            mx = std::max(mx, r.level);
    }
    return mx;
}

std::optional<MemberState> resolve_member(const EngineMaster &master, const std::string &key,
                                          const GkMemberCard &card, const MemberGrowth &growth) {
    auto lgit = master.member_levels.find(card.level_group);
    if (lgit == master.member_levels.end())
        return std::nullopt;
    auto lit = lgit->second.find(growth.level);
    if (lit == lgit->second.end())
        return std::nullopt;

    Stat3 awake{0, 0, 0};
    auto agit = master.member_awake.find(card.awake_group);
    if (agit != master.member_awake.end() && agit->second.count(growth.awake)) {
        awake = agit->second.at(growth.awake);
    } else if (growth.awake > 1) {
        return std::nullopt;
    }

    auto rgit = master.member_ranks.find(card.rank_group);
    if (rgit == master.member_ranks.end())
        return std::nullopt;
    auto rit = rgit->second.find(growth.rank);
    if (rit == rgit->second.end())
        return std::nullopt;

    MemberState st;
    st.key = key;
    st.card = card;
    st.growth = growth;
    st.leader_skill_level = rit->second.leader_skill_level;
    st.music_type_rate = rit->second.music_type_rate;
    st.music_tag_rate = rit->second.music_tag_rate;
    for (int i = 0; i < 3; ++i) {
        int64_t lp = floor_f32_div_10000(static_cast<int64_t>(lit->second[i]) * card.stat_max[i]);
        int64_t ap = static_cast<int64_t>(
            std::floor(f32(f32(static_cast<double>(awake[i]) / 10000.0) * f32(card.stat_max[i]))));
        int64_t rp = floor_f32_div_10000(
            static_cast<int64_t>(wrap_mul(rit->second.rates[i], card.stat_max[i])));
        st.own[i] = (lp + ap + rp) * 10000;
    }
    if (auto it = master.live_skill_categories.find(card.live_skill_id);
        it != master.live_skill_categories.end())
        st.live_skill_categories = it->second;
    if (auto it = master.gekiso_skill_categories.find(card.gekiso_skill_id);
        it != master.gekiso_skill_categories.end())
        st.gekiso_skill_categories = it->second;
    if (auto it = master.gekiso_skills.find(card.gekiso_skill_id); it != master.gekiso_skills.end())
        st.gekiso_mission_type = it->second.mission_type;
    return st;
}

std::optional<SnapState> resolve_snap(const EngineMaster &master, const std::string &key,
                                      const GkSnapCard &card, const SnapGrowth &growth) {
    auto lgit = master.snap_levels.find(card.level_group);
    if (lgit == master.snap_levels.end())
        return std::nullopt;
    auto lit = lgit->second.find(growth.level);
    if (lit == lgit->second.end())
        return std::nullopt;

    auto rgit = master.snap_ranks.find(card.rank_group);
    if (rgit == master.snap_ranks.end())
        return std::nullopt;
    auto rit = rgit->second.find(growth.rank);
    if (rit == rgit->second.end())
        return std::nullopt;

    SnapState st;
    st.key = key;
    st.card = card;
    st.growth = growth;
    for (int i = 0; i < 3; ++i) {
        st.percent[i] =
            floor_f32_div_10000(static_cast<int64_t>(wrap_mul(lit->second[i], card.stat_max[i])));
    }
    st.type_link_rate = rit->second.type_link_rate;
    st.skill_levels = rit->second.skill_levels;
    st.gekiso_skill_levels = rit->second.gekiso_skill_levels;
    return st;
}

struct ResolvedBox {
    std::vector<MemberState> members;
    std::vector<SnapState> snaps;
    std::vector<std::string> unknown;
};

ResolvedBox resolve_box(const EngineMaster &master, const std::vector<MemberInput> &member_inputs,
                        const std::vector<SnapInput> &snap_inputs) {
    ResolvedBox out;
    for (const auto &inp : member_inputs) {
        auto cit = master.members.find(inp.card_id);
        if (cit == master.members.end()) {
            out.unknown.push_back(inp.key);
            continue;
        }
        const GkMemberCard &card = cit->second;
        int32_t max_aw = max_awake(master, card);
        int32_t max_rk = max_member_rank(master, card);
        const auto *ls = master.live_skills.count(card.live_skill_id)
                             ? &master.live_skills.at(card.live_skill_id)
                             : nullptr;
        const auto *gs = master.gekiso_skills.count(card.gekiso_skill_id)
                             ? &master.gekiso_skills.at(card.gekiso_skill_id).effects
                             : nullptr;
        int32_t max_lv_sk = max_skill_level(ls);
        int32_t max_gk_sk = max_skill_level(gs);
        int32_t awake = std::min(inp.awake.value_or(max_aw), max_aw);
        int32_t cap = member_level_cap(master, card, awake);
        MemberGrowth g;
        g.level = std::max(1, std::min(inp.level.value_or(cap), cap));
        g.awake = awake;
        g.rank = std::min(inp.rank.value_or(max_rk), max_rk);
        g.live_skill_level = std::min(inp.live_skill_level.value_or(max_lv_sk), max_lv_sk);
        g.gekiso_skill_level = std::min(inp.gekiso_skill_level.value_or(max_gk_sk), max_gk_sk);
        auto st = resolve_member(master, inp.key, card, g);
        if (st)
            out.members.push_back(std::move(*st));
        else
            out.unknown.push_back(inp.key);
    }
    for (const auto &inp : snap_inputs) {
        auto cit = master.snaps.find(inp.card_id);
        if (cit == master.snaps.end()) {
            out.unknown.push_back(inp.key);
            continue;
        }
        const GkSnapCard &card = cit->second;
        int32_t max_rk = max_snap_rank(master, card);
        int32_t rank = std::min(inp.rank.value_or(max_rk), max_rk);
        int32_t cap = snap_level_cap(master, card, rank);
        SnapGrowth g;
        g.level = std::max(1, std::min(inp.level.value_or(cap), cap));
        g.rank = rank;
        auto st = resolve_snap(master, inp.key, card, g);
        if (st)
            out.snaps.push_back(std::move(*st));
        else
            out.unknown.push_back(inp.key);
    }
    return out;
}

bool is_target_member(const MemberState &member, const TargetRow &target) {
    const auto &card = member.card;
    if (target.band_id > 0 && target.band_id == card.band_id)
        return true;
    if (target.card_type != 0 && target.card_type == card.card_type)
        return true;
    if (target.character_id > 0 && target.character_id == card.character_id)
        return true;
    if (target.tag_id > 0 && std::find(card.best_tags.begin(), card.best_tags.end(),
                                       target.tag_id) != card.best_tags.end())
        return true;
    for (int32_t c : target.live_skill_categories) {
        if (std::find(member.live_skill_categories.begin(), member.live_skill_categories.end(),
                      c) != member.live_skill_categories.end())
            return true;
    }
    for (int32_t c : target.gekisou_skill_categories) {
        if (std::find(member.gekiso_skill_categories.begin(), member.gekiso_skill_categories.end(),
                      c) != member.gekiso_skill_categories.end())
            return true;
    }
    return target.gekisou_mission_type != 0 &&
           target.gekisou_mission_type == member.gekiso_mission_type;
}

bool matches_targets(const EngineMaster &master, const MemberState &member,
                     const std::vector<int32_t> &target_ids) {
    for (int32_t id : target_ids) {
        auto it = master.targets.find(id);
        if (it != master.targets.end() && is_target_member(member, it->second))
            return true;
    }
    return false;
}

// 与 C++ Engine 严格对齐的队伍综合力计算（支持全部 power_model 开关、舍入模式及 extra_sources）
struct TeamPowerResult {
    double total = 0.0;
    std::vector<double> slots;
};

TeamPowerResult calc_deckcalc_team_power(const Json *prob, const EngineMaster &master,
                                         const std::vector<MemberState> &members,
                                         const std::vector<const SnapState *> &snaps,
                                         size_t leader_idx) {
    (void)master;
    const Json *catalog = (prob && prob->is_object()) ? prob->find("catalog") : nullptr;
    const Json *settings = (prob && prob->is_object()) ? prob->find("settings") : nullptr;
    const Json *song = (prob && prob->is_object()) ? prob->find("song") : nullptr;
    const Json *fix = (catalog && catalog->is_object()) ? catalog->find("fix") : nullptr;
    const Json *pm = (settings && settings->is_object()) ? settings->find("power_model") : nullptr;
    const Json *sources = (pm && pm->is_object()) ? pm->find("sources") : nullptr;

    auto src_enabled = [&](const std::string &name) -> bool {
        if (!sources || !sources->is_object())
            return true;
        const Json *v = sources->find(name);
        return !(v && v->is_bool() && !v->as_bool());
    };
    bool en_trained = src_enabled("trained");
    bool en_char_rank = src_enabled("character_rank");
    bool en_char_total_rank = src_enabled("character_total_rank");
    bool en_snapshot = src_enabled("snapshot");
    bool en_type_link = src_enabled("type_link");
    bool en_leader = src_enabled("leader");
    bool en_band_item = src_enabled("band_item");
    bool en_music_type = src_enabled("music_type");
    bool en_music_tag = src_enabled("music_tag");
    bool en_vip = src_enabled("vip");

    std::string rounding = "float32_floor";
    if (pm && pm->is_object()) {
        if (const Json *r = pm->find("rounding"); r && r->is_string())
            rounding = r->as_string();
    }
    std::string type_link_source = "snapshot";
    if (settings && settings->is_object()) {
        if (const Json *tls = settings->find("type_link_bonus_source"); tls && tls->is_string())
            type_link_source = tls->as_string();
    }
    bool apply_event_power = true;
    if (settings && settings->is_object()) {
        if (const Json *ev = settings->find("event"); ev && ev->is_object()) {
            if (const Json *aep = ev->find("apply_event_power"); aep && aep->is_bool())
                apply_event_power = aep->as_bool();
        }
    }

    auto round_val = [&](double value) -> double {
        if (rounding == "float32_floor") {
            int64_t scaled = static_cast<int64_t>(std::trunc(value * 10000.0));
            return std::floor(f32(f32(scaled) / 10000.0f));
        }
        if (rounding == "float64_floor")
            return std::floor(value);
        if (rounding == "round")
            return std::round(value);
        return value;
    };
    auto apply_rate = [&](double base_val, int64_t rate_bp) -> double {
        if (rate_bp == 0)
            return 0.0;
        if (rounding == "float32_floor") {
            int64_t base_scaled = static_cast<int64_t>(std::llround(base_val * 10000.0));
            int64_t mul = static_cast<int64_t>(
                std::trunc(static_cast<double>(base_scaled * rate_bp) / 10000.0));
            return std::floor(f32(f32(mul) / 10000.0f));
        }
        return round_val((base_val * static_cast<double>(rate_bp)) / 10000.0);
    };

    auto get_vec3 = [](const Json *obj, const std::string &key,
                       int64_t def = 0) -> std::array<int64_t, 3> {
        std::array<int64_t, 3> res{def, def, def};
        if (!obj || !obj->is_object())
            return res;
        const Json *a = obj->find(key);
        if (!a || !a->is_array())
            return res;
        const auto &arr = a->as_array();
        for (size_t i = 0; i < 3 && i < arr.size(); ++i)
            res[i] = json_i32(&arr[i], static_cast<int32_t>(def));
        return res;
    };
    auto get_map_vec3 = [&](const Json *obj, const std::string &map_key, int32_t id,
                            const std::array<int64_t, 3> &fallback) -> std::array<int64_t, 3> {
        if (!obj || !obj->is_object())
            return fallback;
        const Json *mp = obj->find(map_key);
        if (!mp || !mp->is_object())
            return fallback;
        const Json *entry = mp->find(std::to_string(id));
        if (!entry || !entry->is_array())
            return fallback;
        std::array<int64_t, 3> res = fallback;
        const auto &arr = entry->as_array();
        for (size_t i = 0; i < 3 && i < arr.size(); ++i)
            res[i] = json_i32(&arr[i], static_cast<int32_t>(fallback[i]));
        return res;
    };

    std::unordered_map<int32_t, const Json *> cat_members;
    std::unordered_map<int32_t, const Json *> cat_snaps;
    if (catalog && catalog->is_object()) {
        if (const Json *ms = catalog->find("members"); ms && ms->is_array()) {
            for (const Json &m : ms->as_array())
                cat_members[row_i32(m, "id")] = &m;
        }
        if (const Json *ss = catalog->find("snapshots"); ss && ss->is_array()) {
            for (const Json &s : ss->as_array())
                cat_snaps[row_i32(s, "id")] = &s;
        }
    }

    int32_t song_type = song ? row_i32(*song, "type") : 0;
    std::vector<int32_t> song_tags = song ? row_list(*song, "tags") : std::vector<int32_t>{};

    // 计算队长技能每个槽位的三维加成 BP
    std::vector<std::array<int64_t, 3>> leader_rates(members.size(), {0, 0, 0});
    if (en_leader && leader_idx < members.size() && catalog && catalog->is_object()) {
        const MemberState &leader_m = members[leader_idx];
        const Json *cat_l =
            cat_members.count(leader_m.card.id) ? cat_members.at(leader_m.card.id) : nullptr;
        int32_t l_skill_id = cat_l ? row_i32(*cat_l, "leader_skill", leader_m.card.leader_skill_id)
                                   : leader_m.card.leader_skill_id;
        int32_t l_level = leader_m.leader_skill_level > 0 ? leader_m.leader_skill_level : 5;

        const Json *l_skill = nullptr;
        if (const Json *lskills = catalog->find("leader_skills"); lskills && lskills->is_array()) {
            for (const Json &sk : lskills->as_array()) {
                if (row_i32(sk, "id") == l_skill_id) {
                    l_skill = &sk;
                    break;
                }
            }
        }
        if (l_skill) {
            std::unordered_map<int32_t, const Json *> target_map;
            if (const Json *ts = catalog->find("targets"); ts && ts->is_array()) {
                for (const Json &t : ts->as_array())
                    target_map[row_i32(t, "id")] = &t;
            }
            std::unordered_map<int32_t, const Json *> cond_map;
            if (const Json *cs = catalog->find("conditions"); cs && cs->is_array()) {
                for (const Json &c : cs->as_array())
                    cond_map[row_i32(c, "id")] = &c;
            }
            std::unordered_map<int32_t, const Json *> cg_map;
            if (const Json *cgs = catalog->find("condition_groups"); cgs && cgs->is_array()) {
                for (const Json &cg : cgs->as_array())
                    cg_map[row_i32(cg, "id")] = &cg;
            }
            std::unordered_map<int32_t, int32_t> gk_mission_by_skill;
            if (const Json *gks = catalog->find("gekisou_skills"); gks && gks->is_array()) {
                for (const Json &gk : gks->as_array())
                    gk_mission_by_skill[row_i32(gk, "id")] = row_i32(gk, "mission_type");
            }

            auto match_cat_targets = [&](const MemberState &m,
                                         const std::vector<int32_t> &tids) -> bool {
                if (tids.empty())
                    return true;
                for (int32_t tid : tids) {
                    auto tit = target_map.find(tid);
                    if (tit == target_map.end())
                        continue;
                    const Json &t = *tit->second;
                    bool matched = false;
                    int32_t tb = row_i32(t, "band");
                    if (tb > 0 && tb == m.card.band_id)
                        matched = true;
                    int32_t tc = row_i32(t, "character");
                    if (!matched && tc > 0 && tc == m.card.character_id)
                        matched = true;
                    int32_t tct = row_i32(t, "card_type");
                    if (!matched && tct > 0 && tct == m.card.card_type)
                        matched = true;
                    int32_t ttag = row_i32(t, "tag");
                    if (!matched && ttag > 0 &&
                        std::find(m.card.best_tags.begin(), m.card.best_tags.end(), ttag) !=
                            m.card.best_tags.end())
                        matched = true;
                    int32_t tgm = row_i32(t, "gekisou_mission_type");
                    if (!matched && tgm > 0) {
                        int32_t m_gm = m.gekiso_mission_type;
                        if (m_gm == 0 && gk_mission_by_skill.count(m.card.gekiso_skill_id))
                            m_gm = gk_mission_by_skill.at(m.card.gekiso_skill_id);
                        if (m_gm == tgm)
                            matched = true;
                    }
                    if (!matched) {
                        for (int32_t cat : row_list(t, "live_skill_categories")) {
                            if (cat != 0 && std::find(m.live_skill_categories.begin(),
                                                      m.live_skill_categories.end(),
                                                      cat) != m.live_skill_categories.end()) {
                                matched = true;
                                break;
                            }
                        }
                    }
                    if (!matched) {
                        for (int32_t cat : row_list(t, "gekisou_skill_categories")) {
                            if (cat != 0 && std::find(m.gekiso_skill_categories.begin(),
                                                      m.gekiso_skill_categories.end(),
                                                      cat) != m.gekiso_skill_categories.end()) {
                                matched = true;
                                break;
                            }
                        }
                    }
                    if (matched)
                        return true;
                }
                return false;
            };

            auto check_cond_group = [&](int32_t group_id) -> bool {
                if (group_id <= 0)
                    return true;
                auto git = cg_map.find(group_id);
                if (git == cg_map.end())
                    return false;
                const Json *sets = git->second->find("sets");
                if (!sets || !sets->is_array())
                    return false;
                for (const Json &set_item : sets->as_array()) {
                    const Json *cids_json =
                        set_item.is_object() ? set_item.find("conditions") : &set_item;
                    if (!cids_json || !cids_json->is_array())
                        continue;
                    bool set_ok = true;
                    for (const Json &cid_val : cids_json->as_array()) {
                        int32_t cid = json_i32(&cid_val);
                        auto cit = cond_map.find(cid);
                        if (cit == cond_map.end()) {
                            set_ok = false;
                            break;
                        }
                        const Json &cond = *cit->second;
                        int32_t ctype = row_i32(cond, "condition_type");
                        auto tids = row_list(cond, "targets");
                        bool raw_ok = false;
                        if (ctype == 0) {
                            raw_ok = true;
                        } else if (ctype == 3000) {
                            for (const auto &m : members) {
                                if (match_cat_targets(m, tids)) {
                                    raw_ok = true;
                                    break;
                                }
                            }
                        } else if (ctype == 3001) {
                            raw_ok = !members.empty();
                            for (const auto &m : members) {
                                if (!match_cat_targets(m, tids)) {
                                    raw_ok = false;
                                    break;
                                }
                            }
                        } else if (ctype == 4012) {
                            for (int32_t tid : tids) {
                                if (target_map.count(tid) &&
                                    row_i32(*target_map.at(tid), "music_type") == song_type) {
                                    raw_ok = true;
                                    break;
                                }
                            }
                        } else if (ctype == 5000) {
                            raw_ok = match_cat_targets(leader_m, tids);
                        }
                        const Json *pos = cond.find("positive");
                        bool is_pos = !(pos && pos->is_bool() && !pos->as_bool());
                        if ((is_pos ? raw_ok : !raw_ok) == false) {
                            set_ok = false;
                            break;
                        }
                    }
                    if (set_ok)
                        return true;
                }
                return false;
            };

            if (const Json *effs = l_skill->find("effects"); effs && effs->is_array()) {
                for (const Json &eff : effs->as_array()) {
                    int32_t elv = row_i32(eff, "level");
                    if (elv != 0 && elv != l_level)
                        continue;
                    if (!check_cond_group(row_i32(eff, "condition_group")))
                        continue;
                    int32_t et = row_i32(eff, "effect_type");
                    int64_t mult = 1;
                    if (et >= 1500 && et <= 1503) {
                        int32_t cum_type = row_i32(eff, "cumulative_type");
                        auto cum_tids = row_list(eff, "cumulative_targets");
                        int64_t count = 0;
                        if (cum_type == 3000) {
                            for (const auto &m : members)
                                if (match_cat_targets(m, cum_tids))
                                    ++count;
                        } else if (cum_type == 3001) {
                            for (size_t i = 0; i < members.size(); ++i)
                                if (i != leader_idx && match_cat_targets(members[i], cum_tids))
                                    ++count;
                        } else if (cum_type == 3002) {
                            for (const auto &m : members)
                                if (m.card.band_id == leader_m.card.band_id)
                                    ++count;
                        } else if (cum_type == 3003) {
                            std::unordered_set<int32_t> bands;
                            for (const auto &m : members)
                                if (m.card.band_id != leader_m.card.band_id)
                                    bands.insert(m.card.band_id);
                            count = static_cast<int64_t>(bands.size());
                        } else if (cum_type == 3004) {
                            std::unordered_set<int32_t> bands;
                            for (const auto &m : members)
                                bands.insert(m.card.band_id);
                            count = static_cast<int64_t>(bands.size());
                        } else if (cum_type == 3005) {
                            std::unordered_set<int32_t> cts;
                            for (const auto &m : members)
                                cts.insert(m.card.card_type);
                            count = static_cast<int64_t>(cts.size());
                        }
                        int32_t max_c = row_i32(eff, "max_cumulative_count");
                        if (max_c > 0 && count > max_c)
                            count = max_c;
                        mult = count;
                    }
                    int64_t val = static_cast<int64_t>(row_i32(eff, "value")) * mult;
                    int32_t max_v = row_i32(eff, "max_value");
                    if (max_v > 0 && val > max_v)
                        val = max_v;
                    auto eff_tids = row_list(eff, "targets");
                    for (size_t i = 0; i < members.size(); ++i) {
                        if (!match_cat_targets(members[i], eff_tids))
                            continue;
                        if (et == 1000 || et == 1500) {
                            leader_rates[i][0] += val;
                            leader_rates[i][1] += val;
                            leader_rates[i][2] += val;
                        } else if (et == 1003 || et == 1503) {
                            leader_rates[i][0] += val;
                        } else if (et == 1001 || et == 1501) {
                            leader_rates[i][1] += val;
                        } else if (et == 1002 || et == 1502) {
                            leader_rates[i][2] += val;
                        }
                    }
                }
            }
        }
    }

    auto def_cr = get_vec3(fix, "character_rank_bonus");
    auto def_ctr = get_vec3(fix, "character_total_rank_bonus");
    auto def_bi = get_vec3(fix, "band_item_bonus_bp");
    auto def_vip = get_vec3(fix, "vip_bonus_bp");
    int64_t mt_base_bp = fix ? row_i32(*fix, "music_type_base_bp") : 0;
    int64_t mtag_base_bp = fix ? row_i32(*fix, "music_tag_base_bp") : 0;
    int64_t tl_base_bp = fix ? row_i32(*fix, "type_link_base_bp") : 0;

    TeamPowerResult out;
    out.slots.resize(members.size(), 0.0);
    for (size_t idx = 0; idx < members.size(); ++idx) {
        const MemberState &m = members[idx];
        const SnapState *s = idx < snaps.size() ? snaps[idx] : nullptr;
        const Json *cat_m = cat_members.count(m.card.id) ? cat_members.at(m.card.id) : nullptr;
        const Json *cat_s = (s && cat_snaps.count(s->card.id)) ? cat_snaps.at(s->card.id) : nullptr;

        int64_t m_ev_bp = (apply_event_power && cat_m) ? row_i32(*cat_m, "event_bonus_bp") : 0;
        int64_t s_ev_bp = (apply_event_power && cat_s) ? row_i32(*cat_s, "event_bonus_bp") : 0;

        const Json *m_crb =
            (cat_m && cat_m->is_object()) ? cat_m->find("card_rank_bonus_bp") : nullptr;
        const Json *s_crb =
            (cat_s && cat_s->is_object()) ? cat_s->find("card_rank_bonus_bp") : nullptr;
        int64_t m_mt_bp = m_crb ? row_i32(*m_crb, "music_type", m.music_type_rate)
                                : static_cast<int64_t>(m.music_type_rate);
        int64_t m_mtag_bp = m_crb ? row_i32(*m_crb, "music_tag", m.music_tag_rate)
                                  : static_cast<int64_t>(m.music_tag_rate);
        int64_t m_tl_bp = m_crb ? row_i32(*m_crb, "type_link", 0) : 0;
        int64_t s_tl_bp = s ? static_cast<int64_t>(s->type_link_rate) : 0;
        if (s && s_crb && s->growth.rank == row_i32(*cat_s, "rank", s->growth.rank)) {
            s_tl_bp = row_i32(*s_crb, "type_link", s->type_link_rate);
        }

        auto cr_vec =
            get_map_vec3(fix, "character_rank_bonus_by_character", m.card.character_id, def_cr);
        auto bi_char =
            get_map_vec3(fix, "band_item_bonus_bp_by_character", m.card.character_id, {0, 0, 0});
        auto bi_band = get_map_vec3(fix, "band_item_bonus_bp_by_band", m.card.band_id, def_bi);
        auto bi_ct =
            get_map_vec3(fix, "band_item_bonus_bp_by_card_type", m.card.card_type, {0, 0, 0});

        bool tag_matched = false;
        for (int32_t tg : m.card.best_tags) {
            if (std::find(song_tags.begin(), song_tags.end(), tg) != song_tags.end()) {
                tag_matched = true;
                break;
            }
        }

        double slot_total = 0.0;
        for (int d = 0; d < 3; ++d) {
            double trained_d = std::round(static_cast<double>(m.own[d]) / 10000.0);
            double mem_val = en_trained ? trained_d : 0.0;
            if (en_trained && m_ev_bp != 0)
                mem_val += apply_rate(mem_val, m_ev_bp);
            double cr_val = en_char_rank ? static_cast<double>(cr_vec[d]) : 0.0;
            double ctr_val = en_char_total_rank ? static_cast<double>(def_ctr[d]) : 0.0;
            double base_val = round_val(mem_val + cr_val + ctr_val);

            double snap_val = 0.0;
            if (en_snapshot && s) {
                int64_t s_rate = s->percent[d] + s_ev_bp;
                snap_val = apply_rate(base_val, s_rate);
            }

            double tl_val = 0.0;
            if (en_type_link && s && s->card.card_type == m.card.card_type) {
                int64_t r = tl_base_bp;
                if (type_link_source == "member")
                    r += m_tl_bp;
                else if (type_link_source == "snapshot")
                    r += s_tl_bp;
                else if (type_link_source == "both")
                    r += m_tl_bp + s_tl_bp;
                tl_val = apply_rate(base_val, r);
            }

            double ld_val = en_leader ? apply_rate(base_val, leader_rates[idx][d]) : 0.0;
            double bi_val =
                en_band_item ? apply_rate(base_val, bi_char[d] + bi_band[d] + bi_ct[d]) : 0.0;

            double mt_val = 0.0;
            if (en_music_type &&
                (song_type == 99 || m.card.card_type == 99 || m.card.card_type == song_type)) {
                mt_val = apply_rate(base_val, mt_base_bp + m_mt_bp);
            }

            double mtag_val = 0.0;
            if (en_music_tag && tag_matched) {
                mtag_val = apply_rate(base_val, mtag_base_bp + m_mtag_bp);
            }

            double vip_val = en_vip ? apply_rate(base_val, def_vip[d]) : 0.0;

            double extra_val = 0.0;
            if (pm && pm->is_object()) {
                if (const Json *exs = pm->find("extra_sources"); exs && exs->is_array()) {
                    for (const Json &src : exs->as_array()) {
                        std::string kind = row_str(src, "kind", "rate_bp");
                        auto vals = get_vec3(&src, "rate_bp");
                        if (vals[0] == 0 && vals[1] == 0 && vals[2] == 0)
                            vals = get_vec3(&src, "flat_power");
                        if (kind == "flat")
                            extra_val += round_val(static_cast<double>(vals[d]));
                        else
                            extra_val += apply_rate(base_val, vals[d]);
                    }
                }
            }

            slot_total += base_val + snap_val + tl_val + ld_val + bi_val + mt_val + mtag_val +
                          vip_val + extra_val;
        }
        out.slots[idx] = slot_total;
        out.total += slot_total;
    }
    return out;
}
// 整曲逐帧状态机：Combo、判定改判、体力、分数差分与激奏控制器
class ComboCounter {
public:
    void add_judgement(int32_t time_ms, int32_t judgement) {
        if (judgement == 0 || judgement == 7)
            return;
        int32_t order = add_order_++;
        size_t i = times_.size();
        while (i >= 1 &&
               (time_ms < times_[i - 1] || (time_ms == times_[i - 1] && order < orders_[i - 1]))) {
            --i;
        }
        times_.insert(times_.begin() + static_cast<ptrdiff_t>(i), time_ms);
        orders_.insert(orders_.begin() + static_cast<ptrdiff_t>(i), order);
        judgements_.insert(judgements_.begin() + static_cast<ptrdiff_t>(i), judgement);
        combos_.insert(combos_.begin() + static_cast<ptrdiff_t>(i), 0);
        int32_t combo = (i == 0) ? 0 : combos_[i - 1];
        for (size_t k = i; k < times_.size(); ++k) {
            int32_t j = judgements_[k];
            combo = (j == 1 || j == 2) ? 0 : (combo + 1);
            combos_[k] = combo;
        }
    }

    int32_t timing_combo(int32_t t) const {
        size_t n = times_.size();
        if (n == 0 || times_[0] >= t)
            return 0;
        size_t lo = 0;
        size_t hi = n - 1;
        while (lo < hi) {
            size_t mid = lo + (hi - lo + 1) / 2;
            if (times_[mid] < t)
                lo = mid;
            else
                hi = mid - 1;
        }
        return combos_[lo];
    }

private:
    std::vector<int32_t> times_;
    std::vector<int32_t> orders_;
    std::vector<int32_t> judgements_;
    std::vector<int32_t> combos_;
    int32_t add_order_ = 0;
};

constexpr int32_t kStay = 0;
constexpr int32_t kExecuteFrame = 2;
constexpr int32_t kExecuting = 3;
constexpr int32_t kEndFrame = 4;
constexpr int32_t kOneShot = 1;
constexpr int32_t kSustained = 2;

struct ConvertEffect {
    int32_t type = 0;
    int32_t value = 0;
    int32_t limit_count = 0;
    std::vector<int32_t> targets;
    int32_t effect_id = 0;
};

class Conversion {
public:
    explicit Conversion(std::unordered_set<int32_t> no_just) : no_just_(std::move(no_just)) {}

    int32_t convert(int32_t judgement, int32_t judgement_type, int32_t time_ms) {
        for (size_t i = 0; i < order_.size(); ++i) {
            int32_t key = order_[i];
            if (!registered_.count(key))
                continue;
            int32_t out = try_convert(key, judgement, judgement_type, time_ms);
            if (out != judgement)
                return out;
            if (!(i < order_.size() && order_[i] == key)) {
                if (i > 0)
                    --i;
                else {
                    // order_[0] was removed; restart at 0 on next iteration
                    i = static_cast<size_t>(-1);
                }
            }
        }
        return judgement;
    }

    std::optional<int32_t> update(const std::string &key, int32_t state,
                                  const ConvertEffect &effect) {
        std::optional<int32_t> finished;
        auto fit = limit_finished_.find(key);
        if (fit != limit_finished_.end()) {
            finished = fit->second;
            limit_finished_.erase(fit);
            state = kEndFrame;
        }
        if (state == kEndFrame) {
            auto it = id_map_.find(key);
            if (it != id_map_.end()) {
                int32_t id = it->second;
                id_map_.erase(it);
                unregister(id);
                params_.erase(id);
            }
        } else if (state == kExecuteFrame) {
            auto cit = context_.find(effect.effect_id);
            if (cit == context_.end()) {
                cit = context_.emplace(effect.effect_id, effect.targets).first;
            }
            int32_t to = (effect.type == 13005)
                             ? 6
                             : (effect.value >= 1 && effect.value <= 6 ? effect.value : -1);
            int32_t id = reg();
            id_map_[key] = id;
            params_[id] = {key, 0, effect.limit_count, cit->second, to};
        }
        return finished;
    }

private:
    struct ConvertParam {
        std::string key;
        int32_t count = 0;
        int32_t max_count = 0;
        std::vector<int32_t> targets;
        int32_t to = -1;
    };

    int32_t reg() {
        int32_t k = ++func_id_;
        registered_.insert(k);
        order_.insert(order_.begin(), k);
        return k;
    }

    void unregister(int32_t id) {
        registered_.erase(id);
        auto it = std::find(order_.begin(), order_.end(), id);
        if (it != order_.end())
            order_.erase(it);
    }

    int32_t try_convert(int32_t id, int32_t cur, int32_t judgement_type, int32_t time_ms) {
        auto pit = params_.find(id);
        if (pit == params_.end())
            return cur;
        auto &p = pit->second;
        if (p.to == -1 || cur == p.to ||
            std::find(p.targets.begin(), p.targets.end(), cur) == p.targets.end())
            return cur;
        if (p.to == 6 && no_just_.count(judgement_type))
            return cur;
        p.count++;
        if (p.max_count > 0 && p.max_count <= p.count) {
            std::string k = p.key;
            unregister(id);
            id_map_.erase(k);
            params_.erase(id);
            limit_finished_[k] = time_ms;
        }
        return p.to;
    }

    std::unordered_set<int32_t> no_just_;
    std::vector<int32_t> order_;
    std::unordered_set<int32_t> registered_;
    int32_t func_id_ = 0;
    std::unordered_map<std::string, int32_t> id_map_;
    std::unordered_map<int32_t, ConvertParam> params_;
    std::unordered_map<std::string, int32_t> limit_finished_;
    std::unordered_map<int32_t, std::vector<int32_t>> context_;
};

class LifeController {
public:
    LifeController(int32_t initial_life, std::array<int32_t, 7> damage, int32_t music_length_ms)
        : current_life(initial_life), initial_life_(initial_life), damage_(damage),
          max_life_(initial_life), max_frame_(wrap_add(get_frame(music_length_ms), 2)),
          commands_(std::max(1, max_frame_)) {}

    int32_t maximum() const {
        return max_life_;
    }

    void note_damage(int32_t time_ms, int32_t judgement) {
        if (judgement <= 0 || judgement >= 7)
            return;
        int32_t d = damage_[judgement];
        if (d > 0)
            add({time_ms, 0, d, false, false});
    }

    void recovery(int32_t time_ms, int32_t amount, bool over_heal) {
        if (amount > 0)
            add({time_ms, 2, amount, over_heal, false});
    }

    void skill_damage(int32_t time_ms, int32_t amount, bool safety) {
        if (amount > 0)
            add({time_ms, 1, amount, false, safety});
    }

    int32_t enable_reduction(int32_t time_ms, int32_t bp) {
        int32_t id = ++reduction_counter_;
        reductions_[id] = bp;
        add({time_ms, 5, bp, false, false});
        return id;
    }

    void disable_reduction(int32_t time_ms, int32_t id) {
        auto it = reductions_.find(id);
        if (it == reductions_.end())
            return;
        int32_t bp = it->second;
        reductions_.erase(it);
        add({time_ms, 6, bp, false, false});
    }

    int32_t add_limit(int32_t amount) {
        int32_t id = ++limit_counter_;
        limits_[id] = amount;
        update_max();
        return id;
    }

    void subtract_limit(int32_t id) {
        if (limits_.erase(id))
            update_max();
    }

    int32_t enable_guard(int32_t time_ms) {
        int32_t id = ++guard_counter_;
        guards_.push_back(id);
        add({time_ms, 3, 0, false, false});
        return id;
    }

    void disable_guard(int32_t time_ms, int32_t id) {
        auto it = std::find(guards_.begin(), guards_.end(), id);
        if (it == guards_.end())
            return;
        guards_.erase(it);
        add({time_ms, 4, 0, false, false});
    }

    int32_t life_at(int32_t ms) {
        int32_t frame = frame_of(ms);
        int32_t c = cached_frame_;
        bool full = false;
        int32_t start = 0;
        std::array<int32_t, 3> state{initial_life_, 0, 0};
        if (c < 0 || frame <= c) {
            full = (c >= 0 && frame <= c);
            start = 0;
        } else {
            full = false;
            state = {cached_life_, cached_guard_, cached_reduction_};
            start = c + 1;
        }
        for (int32_t k = start; k < std::max(frame, start); ++k) {
            for (const auto &cmd : commands_[k])
                step(state, cmd);
        }
        if (frame < 0)
            return state[0];
        if (!full) {
            cached_frame_ = frame - 1;
            cached_life_ = state[0];
            cached_guard_ = state[1];
            cached_reduction_ = state[2];
        }
        if (frame >= max_frame_)
            return state[0];
        for (const auto &cmd : commands_[frame]) {
            if (cmd.time_ms > ms)
                break;
            step(state, cmd);
        }
        return state[0];
    }

    void sync_current(int32_t t) {
        current_life = life_at(t);
    }

    int32_t current_life = 0;

private:
    struct LifeCommand {
        int32_t time_ms = 0;
        int32_t kind = 0;
        int32_t value = 0;
        bool over_heal = false;
        bool safety = false;
    };

    static int32_t reduce_dmg(int32_t damage, int32_t bp) {
        if (bp < 1)
            return damage;
        if ((static_cast<uint32_t>(bp) >> 4) > 0x270u)
            return 0;
        int32_t r = wrap_mul(wrap_sub(10000, bp), damage) / 10000;
        return std::max(0, r);
    }

    int32_t frame_of(int32_t ms) const {
        int32_t frame = get_frame(ms);
        return max_frame_ <= frame ? (max_frame_ - 1) : frame;
    }

    void add(const LifeCommand &cmd) {
        int32_t frame = frame_of(cmd.time_ms);
        if (frame < 0)
            frame = 0;
        auto &list = commands_[frame];
        size_t i = list.size();
        while (i >= 1 && cmd.time_ms < list[i - 1].time_ms)
            --i;
        list.insert(list.begin() + static_cast<ptrdiff_t>(i), cmd);
        if (frame <= cached_frame_) {
            cached_frame_ = -1;
            cached_life_ = cached_guard_ = cached_reduction_ = 0;
        }
    }

    void update_max() {
        int32_t sum = 0;
        for (const auto &[_, v] : limits_)
            sum = wrap_add(sum, v);
        max_life_ = wrap_add(initial_life_, sum);
    }

    void step(std::array<int32_t, 3> &state, const LifeCommand &cmd) const {
        int32_t life = state[0];
        int32_t guard = state[1];
        int32_t reduction = state[2];
        switch (cmd.kind) {
        case 0: // NOTE_DAMAGE
            if (guard < 1)
                life = std::max(0, wrap_sub(life, reduce_dmg(cmd.value, reduction)));
            break;
        case 1: // SKILL_DAMAGE
            if (guard < 1 && (life > 0 || !cmd.safety)) {
                life = wrap_sub(life, reduce_dmg(cmd.value, reduction));
                life = std::max(life, cmd.safety ? 1 : 0);
            }
            break;
        case 2: // RECOVERY
            if (life > 0) {
                int32_t cap = cmd.over_heal ? (max_life_ << 1) : max_life_;
                int32_t s = wrap_add(cmd.value, life);
                life = std::min(s, cap);
            }
            break;
        case 3: // GUARD_START
            guard = wrap_add(guard, 1);
            break;
        case 4: // GUARD_END
            guard = std::max(0, wrap_sub(guard, 1));
            break;
        case 5: // REDUCTION_START
            reduction = wrap_add(reduction, cmd.value);
            break;
        case 6: // REDUCTION_END
            reduction = std::max(0, wrap_sub(reduction, cmd.value));
            break;
        }
        state = {life, guard, reduction};
    }

    int32_t initial_life_;
    std::array<int32_t, 7> damage_;
    int32_t max_life_;
    int32_t max_frame_;
    std::vector<std::vector<LifeCommand>> commands_;
    std::vector<int32_t> guards_;
    int32_t guard_counter_ = 0;
    std::unordered_map<int32_t, int32_t> reductions_;
    int32_t reduction_counter_ = 0;
    std::unordered_map<int32_t, int32_t> limits_;
    int32_t limit_counter_ = 0;
    int32_t cached_frame_ = -1;
    int32_t cached_life_ = 0;
    int32_t cached_guard_ = 0;
    int32_t cached_reduction_ = 0;
};

struct FactorCommand {
    int32_t time_ms = 0;
    int32_t owner = 0;
    int32_t note = 0;
    int32_t combo = 0;
    int32_t judgement = 0;
    int32_t judge = 0;
    int32_t luck = 0;
};

inline int32_t note_factor_mill(float factor) {
    return floor_to_i32(f32(factor * 100000.0f));
}

inline int32_t judgement_factor_mill(float factor) {
    float x = f32(factor * 100000.0f);
    return std::isfinite(x) ? floor_to_i32_f64(round_ties_even(static_cast<double>(x)))
                            : floor_to_i32(x);
}

inline float mill_to_f32(int32_t m) {
    return m != 0 ? f32(f32(m) / 100000.0f) : 0.0f;
}

struct FactorState {
    float combo = 0.0f;
    float note = 1.0f;
    float just = 0.0f;
    float perfect = 0.0f;
    float great = 0.0f;
    float good = 0.0f;
    int32_t luck = 0;

    float judgement_factor(int32_t score_type) const {
        switch (score_type) {
        case 1:
            return just;
        case 2:
            return perfect;
        case 3:
            return great;
        case 4:
            return good;
        default:
            return 0.0f;
        }
    }

    void apply(const FactorCommand &cmd) {
        float fc = mill_to_f32(cmd.combo);
        float fn = mill_to_f32(cmd.note);
        float fj = mill_to_f32(cmd.judge);
        if (fc != 0.0f)
            combo = f32(combo + fc);
        if (fn != 0.0f)
            note = f32(note + fn);
        switch (cmd.judgement) {
        case 6:
            just = f32(just + fj);
            break;
        case 5:
            perfect = f32(perfect + fj);
            break;
        case 4:
            great = f32(great + fj);
            break;
        case 3:
            good = f32(good + fj);
            break;
        }
        luck = wrap_add(luck, cmd.luck);
    }
};

struct FrameDiff {
    float combo = 0.0f;
    float note = 0.0f;
    float just = 0.0f;
    float perfect = 0.0f;
    float great = 0.0f;
    float good = 0.0f;
    int32_t luck = 0;

    void add(const FactorCommand &cmd) {
        luck = wrap_add(luck, cmd.luck);
        float fc = mill_to_f32(cmd.combo);
        float fn = mill_to_f32(cmd.note);
        float fj = mill_to_f32(cmd.judge);
        if (fc != 0.0f)
            combo = f32(combo + fc);
        if (fn != 0.0f)
            note = f32(note + fn);
        switch (cmd.judgement) {
        case 6:
            just = f32(just + fj);
            break;
        case 5:
            perfect = f32(perfect + fj);
            break;
        case 4:
            great = f32(great + fj);
            break;
        case 3:
            good = f32(good + fj);
            break;
        }
    }

    void undo(FactorState &s) {
        s.just = f32(s.just - just);
        s.perfect = f32(s.perfect - perfect);
        s.combo = f32(s.combo - combo);
        s.note = f32(s.note - note);
        s.luck = wrap_sub(s.luck, luck);
        s.great = f32(s.great - great);
        s.good = f32(s.good - good);
        combo = note = just = perfect = great = good = 0.0f;
        luck = 0;
    }
};

struct GekisouComboInfo {
    virtual ~GekisouComboInfo() = default;
    virtual std::optional<int32_t> gekisou_combo(int32_t time_ms) const = 0;
};

using LiveData = EngineMaster::Live;

class ScoreCalculator {
public:
    FactorState state;
    const LiveData &settings;
    double power;
    float level_factor;
    int32_t converted_count;
    float assist;
    std::function<void(int32_t, int32_t, float, float, float, float)> on_note;

    ScoreCalculator(const LiveData &settings_in, double power_in, int32_t level,
                    int32_t converted_count_in, float assist_in = 1.0f)
        : settings(settings_in), power(power_in),
          level_factor(f32(f32(f32(wrap_sub(level, 5)) * f32(0.005)) + 1.0f)),
          converted_count(converted_count_in), assist(assist_in) {}

    float table(int32_t type, int32_t combo) const {
        auto it = settings.combo.find(type);
        if (it == settings.combo.end() || it->second.empty() || it->second[0].first > combo)
            return 0.0f;
        const auto &rows = it->second;
        size_t lo = 0;
        size_t hi = rows.size() - 1;
        while (lo < hi) {
            size_t mid = lo + (hi - lo + 1) / 2;
            if (rows[mid].first > combo)
                hi = mid - 1;
            else
                lo = mid;
        }
        return rows[lo].second;
    }

    float gekisou_combo_factor(const GekisouComboInfo *info, int32_t time_ms) const {
        if (!info)
            return 1.0f;
        auto c = info->gekisou_combo(time_ms);
        if (!c.has_value())
            return 1.0f;
        return f32(min_ignoring_nan(table(1, *c), 1.0f) + 1.0f);
    }

    int32_t note_score(int32_t combo, int32_t life, int32_t time_ms, int32_t op, int32_t score_type,
                       const GekisouComboInfo *info, int32_t note_id = 0) {
        float cum = table(0, combo);
        float gk = gekisou_combo_factor(info, time_ms);
        float jf = state.judgement_factor(score_type);
        float score_up = f32(state.note + jf);
        int32_t luck_percent = std::min(wrap_add(state.luck, 100), 200);
        float luck = f32(f32(luck_percent) / 100.0f);
        float combo_factor = f32(gk * f32(state.combo + f32(min_ignoring_nan(cum, 1.0f) + 1.0f)));
        if (on_note)
            on_note(note_id, score_type, state.note, gk, luck, jf);
        return note_score_core(life, op, score_type, combo_factor, score_up, luck);
    }

    int32_t note_score_core(int32_t life, int32_t op, int32_t score_type, float combo_factor,
                            float score_up, float luck) const {
        int32_t note_pct = settings.note_percent.at(op);
        int32_t judge_pct = settings.judge_percent.at(score_type);
        float life_factor = life > 0 ? 1.0f : settings.life_onus;
        float t = f32(f32(settings.adjustment * f32(power)) * level_factor);
        float a = f32(f32(f32(note_pct) / 100.0f) * t);
        float b = f32(f32(f32(judge_pct) / 100.0f) * a);
        float c = f32(f32(b * combo_factor) * score_up);
        float d = f32(luck * c);
        float x = f32(d / f32(converted_count));
        double floored = std::floor(static_cast<double>(x));
        float y = (floored == std::numeric_limits<double>::infinity()) ? f32(kI32Min)
                                                                       : f32(sat_i32(floored));
        float z = f32(assist * f32(life_factor * y));
        return floor_to_i32(z);
    }
};

struct NoteCommand {
    int32_t time_ms = 0;
    int32_t life = 0;
    int32_t note_id = 0;
    int32_t op = 0;
    int32_t score_type = 0;
    int32_t added = 0;
};

class IncrementalScore {
public:
    ScoreCalculator calc;
    int32_t max_frame;
    int32_t score = 0;
    int32_t rank_bonus = 0;

    IncrementalScore(ScoreCalculator calc_in, int32_t music_length_ms)
        : calc(std::move(calc_in)), max_frame(std::max(1, get_frame(music_length_ms) + 50)),
          notes_(max_frame), factors_(max_frame), diffs_(max_frame) {}

    void add_note(int32_t time_ms, int32_t life, int32_t note_id, int32_t op, int32_t score_type) {
        int32_t frame = file(time_ms);
        notes_[frame].push_back({time_ms, life, note_id, op, score_type, 0});
    }

    void add_factor(const FactorCommand &cmd) {
        int32_t frame = file(cmd.time_ms);
        factors_[frame].push_back(cmd);
    }

    void add_fixed(int32_t time_ms, int32_t s) {
        pending_fixed_ = std::make_pair(time_ms, s);
    }

    int32_t calculate(int32_t t, const ComboCounter &combo, const GekisouComboInfo *info) {
        int32_t g = get_frame(t);
        int32_t to = g < 0 ? 0 : g;
        if (max_frame <= g)
            to = max_frame - 1;
        int32_t u = added_ < 0 ? to : std::min(to, added_ - 1);
        int32_t start = 0;
        if (u < prev_) {
            for (int32_t frame = prev_; frame > u; --frame)
                undo(frame);
            start = u + 1;
        } else {
            start = prev_ + 1;
        }
        for (int32_t frame = start; frame <= to; ++frame)
            execute(frame, combo, info);
        if (pending_fixed_.has_value()) {
            auto [ft, fs] = *pending_fixed_;
            pending_fixed_.reset();
            int32_t ff = get_frame(ft);
            fixed_[ff] = fs;
            score = wrap_add(score, fs);
        }
        prev_ = to;
        added_ = -1;
        return score;
    }

private:
    int32_t file(int32_t t) {
        int32_t frame = get_frame(t);
        if (frame < 0)
            frame = 0;
        if (max_frame <= frame)
            frame = max_frame - 1;
        added_ = (added_ < 0) ? frame : std::min(added_, frame);
        return frame;
    }

    void undo(int32_t frame) {
        for (const auto &n : notes_[frame])
            score = wrap_sub(score, n.added);
        if (auto it = fixed_.find(frame); it != fixed_.end())
            score = wrap_sub(score, it->second);
        diffs_[frame].undo(calc.state);
    }

    void execute(int32_t frame, const ComboCounter &combo, const GekisouComboInfo *info) {
        const auto &fl = factors_[frame];
        auto &nl = notes_[frame];
        if (fl.empty() && nl.empty()) {
            if (auto it = fixed_.find(frame); it != fixed_.end()) {
                score = wrap_add(score, it->second);
                rank_bonus = wrap_add(rank_bonus, it->second);
            }
            return;
        }
        std::vector<size_t> of_idx(fl.size());
        std::iota(of_idx.begin(), of_idx.end(), 0);
        if (fl.size() > 1) {
            std::sort(of_idx.begin(), of_idx.end(), [&](size_t a, size_t b) {
                if (fl[a].time_ms != fl[b].time_ms)
                    return fl[a].time_ms < fl[b].time_ms;
                if (fl[a].owner != fl[b].owner)
                    return fl[a].owner < fl[b].owner;
                return a < b;
            });
        }
        std::vector<size_t> on_idx(nl.size());
        std::iota(on_idx.begin(), on_idx.end(), 0);
        if (nl.size() > 1) {
            std::sort(on_idx.begin(), on_idx.end(), [&](size_t a, size_t b) {
                if (nl[a].time_ms != nl[b].time_ms)
                    return nl[a].time_ms < nl[b].time_ms;
                if (nl[a].note_id != nl[b].note_id)
                    return nl[a].note_id < nl[b].note_id;
                return a < b;
            });
        }
        size_t a = 0;
        size_t b = 0;
        while (a < fl.size() || b < nl.size()) {
            if (a < fl.size() &&
                (b >= nl.size() || nl[on_idx[b]].time_ms >= fl[of_idx[a]].time_ms)) {
                const auto &c = fl[of_idx[a]];
                calc.state.apply(c);
                diffs_[frame].add(c);
                ++a;
            } else {
                auto &n = nl[on_idx[b]];
                int32_t s = calc.note_score(combo.timing_combo(n.time_ms), n.life, n.time_ms, n.op,
                                            n.score_type, info, n.note_id);
                n.added = s;
                score = wrap_add(score, s);
                ++b;
            }
        }
        if (auto it = fixed_.find(frame); it != fixed_.end()) {
            score = wrap_add(score, it->second);
            rank_bonus = wrap_add(rank_bonus, it->second);
        }
    }

    std::vector<std::vector<NoteCommand>> notes_;
    std::vector<std::vector<FactorCommand>> factors_;
    std::vector<FrameDiff> diffs_;
    int32_t prev_ = -1;
    int32_t added_ = -1;
    std::unordered_map<int32_t, int32_t> fixed_;
    std::optional<std::pair<int32_t, int32_t>> pending_fixed_;
};

// 激奏（Gekisou）抽奖机、槽位与区间状态机
constexpr int32_t kMCombo = 1;
constexpr int32_t kMLuck = 2;
constexpr int32_t kMJust = 3;
constexpr int32_t kMAll = 4;

constexpr int32_t kSWait = 1;
constexpr int32_t kSStandby = 2;
constexpr int32_t kSStart = 3;
constexpr int32_t kSPlaying = 4;
constexpr int32_t kSEnd = 5;
constexpr int32_t kSDelay = 6;
constexpr int32_t kSComplete = 7;
constexpr int32_t kSFinish = 8;

constexpr int32_t kFeverWait = 1;
constexpr int32_t kFeverFever = 2;
constexpr int32_t kFeverEnd = 3;

inline int32_t buffed_weight(float buff, int32_t weight) {
    return floor_to_i32(f32(f32(buff + 1.0f) * f32(weight)));
}

inline bool is_sub_note(int32_t nt) {
    return nt == 21 || nt == 120 || (static_cast<uint32_t>(nt) & 0xfffffffcu) == 0x3cu;
}

inline bool is_non_luck_note(int32_t nt) {
    switch (nt) {
    case 0:
    case 80:
    case 82:
    case 100:
    case 101:
    case 102:
    case 103:
    case 104:
    case 105:
    case 121:
    case 122:
        return true;
    default:
        return false;
    }
}

using LotItem = std::pair<int32_t, int32_t>; // (weight, value)

std::optional<int32_t> lottery_table(const std::vector<LotItem> &items, int32_t r) {
    if (items.empty())
        return std::nullopt;
    int32_t total = 0;
    for (const auto &it : items)
        total = wrap_add(total, it.first);
    if (total < 1)
        return std::nullopt;
    int32_t abs_r = (r == kI32Min) ? kI32Min : std::abs(r);
    int32_t m = srem(abs_r, total);
    int32_t cum = 0;
    for (const auto &it : items) {
        cum = wrap_add(cum, it.first);
        if (m < cum)
            return it.second;
    }
    return std::nullopt;
}

struct LuckSkillTable {
    std::vector<LotItem> items;
    float buff = 0.0f;
    int32_t total = 0;

    LuckSkillTable() = default;
    explicit LuckSkillTable(std::vector<LotItem> in_items) : items(std::move(in_items)) {
        std::sort(items.begin(), items.end(),
                  [](const LotItem &a, const LotItem &b) { return b.second < a.second; });
        for (const auto &it : items)
            total = wrap_add(total, it.first);
    }

    std::optional<int32_t> lottery(int32_t r) const {
        if (items.empty())
            return std::nullopt;
        int32_t abs_r = (r == kI32Min) ? kI32Min : std::abs(r);
        int32_t m = srem(abs_r, total);
        int32_t cum = 0;
        for (const auto &it : items) {
            cum = wrap_add(cum, buffed_weight(buff, it.first));
            if (m < cum)
                return it.second;
        }
        return std::nullopt;
    }

    std::optional<int32_t> lottery_with_minimum(int32_t r, int32_t minimum) const {
        if (items.empty())
            return std::nullopt;
        int32_t n = 0;
        int32_t inc = 0;
        int32_t exc = 0;
        for (const auto &it : items) {
            int32_t w = buffed_weight(buff, it.first);
            if (minimum <= it.second) {
                ++n;
                inc = wrap_add(inc, w);
            } else {
                exc = wrap_add(exc, w);
            }
        }
        if (n == 0)
            return std::nullopt;
        int32_t per = sdiv(exc, n);
        int32_t tot = wrap_add(inc, wrap_mul(per, n));
        int32_t abs_r = (r == kI32Min) ? kI32Min : std::abs(r);
        int32_t m = srem(abs_r, tot);
        int32_t cum = 0;
        for (const auto &it : items) {
            if (minimum <= it.second) {
                cum = wrap_add(cum, wrap_add(per, buffed_weight(buff, it.first)));
                if (m < cum)
                    return it.second;
            }
        }
        return std::nullopt;
    }
};

class LotteryMachine {
public:
    explicit LotteryMachine(const GekisouTables &t) {
        for (const auto &r : t.luck_base_points) {
            if (r.weight <= 0)
                continue;
            LotItem it{r.weight, r.point};
            if (r.category == 0) {
                if (r.judgement == 3)
                    good_.push_back(it);
                else if (r.judgement == 4)
                    great_.push_back(it);
                else if (r.judgement == 5)
                    perfect_.push_back(it);
            } else if (r.category == 1 && r.judgement == 5) {
                hold_.push_back(it);
            }
        }
        std::array<std::vector<LotItem>, 5> bonus;
        for (const auto &r : t.luck_bonus_lots) {
            if (r.weight > 0 && r.lot_type >= 0 && r.lot_type < 5)
                bonus[r.lot_type].emplace_back(r.weight, r.result);
        }
        for (size_t i = 0; i < 5; ++i)
            tables_[i] = LuckSkillTable(std::move(bonus[i]));
    }

    void set_buff(int32_t percent) {
        float mul = f32(f32(percent) / 100.0f);
        for (auto &t : tables_)
            t.buff = mul;
    }

    int32_t enable_minimum(int32_t result, int32_t limit) {
        minimum_counter_ = wrap_add(minimum_counter_, 1);
        minimum_.push_back({minimum_counter_, result, limit >= 1 ? limit : -1});
        return minimum_counter_;
    }

    void disable_minimum(int32_t id) {
        minimum_.erase(std::remove_if(minimum_.begin(), minimum_.end(),
                                      [&](const std::array<int32_t, 3> &e) { return e[0] == id; }),
                       minimum_.end());
    }

    bool is_minimum_active(int32_t id) const {
        for (const auto &e : minimum_)
            if (e[0] == id)
                return true;
        return false;
    }

    int32_t take_last_consumed(int32_t id) {
        auto it = last_consumed_.find(id);
        if (it == last_consumed_.end())
            return -1;
        int32_t v = it->second;
        last_consumed_.erase(it);
        return v;
    }

    int32_t luck_bonus(int32_t lot_type, int32_t t, LiveRandom &random) {
        int32_t r = random.next_int(LiveRandom::kLuck);
        int32_t applied = max_minimum();
        const auto &table = tables_.at(lot_type);
        if (applied < 1) {
            return table.lottery(r).value_or(0);
        }
        int32_t v = table.lottery_with_minimum(r, applied).value_or(0);
        consume_minimum(applied, t);
        return v;
    }

    int32_t base_point(int32_t nt, int32_t j, LiveRandom &random) {
        if (is_non_luck_note(nt))
            return 0;
        uint32_t u = static_cast<uint32_t>(j + 1);
        if (u < 9 && ((0x107u >> (u & 31u)) & 1u) != 0)
            return 0;
        int32_t r = random.next_int(LiveRandom::kLuck);
        const std::vector<LotItem> *items = nullptr;
        if (is_sub_note(nt))
            items = &hold_;
        else if (static_cast<uint32_t>(j - 5) < 2u)
            items = &perfect_;
        else if (j == 4)
            items = &great_;
        else if (j == 3)
            items = &good_;
        else
            return 0;
        return lottery_table(*items, r).value_or(0);
    }

private:
    int32_t max_minimum() const {
        int32_t m = 0;
        for (const auto &e : minimum_)
            if (m <= e[1])
                m = e[1];
        return m;
    }

    void consume_minimum(int32_t applied, int32_t t) {
        std::vector<std::array<int32_t, 3>> next;
        for (auto e : minimum_) {
            if (e[1] > applied || e[2] < 0) {
                next.push_back(e);
                continue;
            }
            e[2] = wrap_sub(e[2], 1);
            if (e[2] != 0) {
                next.push_back(e);
                continue;
            }
            last_consumed_[e[0]] = t;
        }
        minimum_ = std::move(next);
    }

    std::vector<LotItem> good_;
    std::vector<LotItem> great_;
    std::vector<LotItem> perfect_;
    std::vector<LotItem> hold_;
    std::array<LuckSkillTable, 5> tables_;
    std::vector<std::array<int32_t, 3>> minimum_;
    int32_t minimum_counter_ = 0;
    std::unordered_map<int32_t, int32_t> last_consumed_;
};

struct LuckScore {
    int32_t total_bonus_point = 0;
    int32_t gauge = 0;
    int32_t lot_count = 0;
    int32_t rush_combo = 0;
    std::array<int32_t, 4> results{0, 0, 0, 0};
    int32_t next = -1;
    int32_t gauge_max = 100;
    int32_t gauge_max_default = 100;
    int32_t gauge_max_rush = 50;

    void add_gauge(int32_t v) {
        int32_t g = wrap_add(gauge, v);
        int32_t m = gauge_max;
        if (m <= g) {
            lot_count = wrap_add(lot_count, g / m);
            gauge = g % m;
        } else {
            gauge = g;
        }
    }

    void change_max(int32_t m) {
        if (m < 1 || gauge_max == m)
            return;
        gauge_max = m;
        int32_t g = gauge;
        if (g <= m)
            return;
        lot_count = wrap_add(lot_count, g / m);
        gauge = g % m;
    }

    void initialize(int32_t max_val, int32_t rush_max) {
        if (max_val > 0 && rush_max > 0) {
            gauge_max_default = max_val;
            gauge_max_rush = rush_max;
            change_max(max_val);
        }
    }

    int32_t current_lot_type() const {
        constexpr std::array<int32_t, 4> kLotByRush{0, 4, 3, 2};
        uint32_t rc = static_cast<uint32_t>(rush_combo);
        return rc < 4 ? kLotByRush[rc] : 1;
    }

    void add_score(int32_t r) {
        constexpr std::array<int32_t, 3> kBonusByResult{5, 10, 10};
        uint32_t i = static_cast<uint32_t>(r - 1);
        if (i < 3u)
            total_bonus_point = wrap_add(total_bonus_point, kBonusByResult[i]);
        if (r >= 0 && r <= 3)
            results[r]++;
        if (static_cast<uint32_t>(r) < 3u) {
            rush_combo = 0;
            change_max(gauge_max_default);
        } else if (r == 3) {
            rush_combo = wrap_add(rush_combo, 1);
            change_max(gauge_max_rush);
        }
    }
};

struct FactorStorage {
    std::vector<std::pair<int32_t, float>> lot;
    std::vector<std::pair<int32_t, float>> gauge;
    std::unordered_map<int32_t, float> lot_ids;
    std::unordered_map<int32_t, float> gauge_ids;
    int32_t current = 0;

    static float at(const std::vector<std::pair<int32_t, float>> &cmds, int32_t t) {
        float s = 0.0f;
        for (const auto &[ct, d] : cmds)
            if (ct <= t)
                s = f32(s + d);
        return s;
    }

    int32_t add(bool is_gauge, int32_t t, float value) {
        current = wrap_add(current, 1);
        if (is_gauge) {
            gauge_ids[current] = value;
            gauge.emplace_back(t, value);
        } else {
            lot_ids[current] = value;
            lot.emplace_back(t, value);
        }
        return current;
    }

    void subtract(bool is_gauge, int32_t t, int32_t id) {
        auto &ids = is_gauge ? gauge_ids : lot_ids;
        auto it = ids.find(id);
        if (it == ids.end())
            return;
        float v = it->second;
        ids.erase(it);
        if (is_gauge)
            gauge.emplace_back(t, -v);
        else
            lot.emplace_back(t, -v);
    }
};

struct GkRange {
    int32_t start_ms = 0;
    int32_t end_ms = 0;
    int32_t mission = 0;
    std::unordered_set<int32_t> targets;
};

struct RangeState {
    int32_t state = kSWait;
    int32_t time_to = 0;
    int32_t combo = 0;
    int32_t max_combo = 0;
    int32_t just = 0;
    int32_t raw_just = 0;
    int32_t cum_base = 0;
    int32_t start_score = 0;
    int32_t end_score = 0;
    int32_t last_combo_ms = -1;
    int32_t last_just_ms = -1;
    LuckScore luck;
    float sw_elapsed = 0.0f;
    bool sw_stopped = true;

    int32_t score() const {
        return wrap_sub(end_score, start_score);
    }
};

struct LuckHandle {
    virtual ~LuckHandle() = default;
    virtual int32_t add(int32_t t, int32_t percent) = 0;
    virtual void disable(int32_t t, int32_t id) = 0;
};

struct GkEnv {
    LiveRandom &random;
    LuckHandle &handle;
};

using GkNote = std::array<int32_t, 4>; // [id, type, time_ms, judgement]

class Controller : public GekisouComboInfo {
public:
    std::vector<GkRange> ranges;
    std::vector<RangeState> states;
    LotteryMachine machine;
    std::vector<int32_t> lot_results;
    std::vector<int32_t> state_updates;
    int32_t current_playing_index = -1;
    bool weighted = false;

    Controller(const std::vector<std::array<int32_t, 3>> &range_defs,
               const std::vector<std::pair<int32_t, int32_t>> &notes, const GekisouTables &t,
               int32_t rush_percent, int32_t last_note_delay_ms)
        : machine(t), rush_percent_(rush_percent), last_note_delay_ms_(last_note_delay_ms),
          luck_gauge_max_(t.gauge_max) {
        for (const auto &[s, e, m] : range_defs) {
            GkRange r{s, e, m, {}};
            for (const auto &[id, tm] : notes) {
                if (s <= tm && tm <= e)
                    r.targets.insert(id);
            }
            ranges.push_back(std::move(r));
            RangeState st;
            if (m == kMLuck)
                st.luck.initialize(t.gauge_max, t.gauge_max_rush);
            states.push_back(std::move(st));
        }
        size_t n = ranges.size();
        history_.resize(n);
        snapshot_.resize(n);
        resume_.resize(n);
        pending_.resize(n);
    }

    int32_t playing_range_index() const {
        return playing_.empty() ? -1 : playing_.back();
    }

    void add_just_count(int32_t time_ms, int32_t count) {
        add_just_stack_.push_back({time_ms, f32(count), playing_range_index()});
        flag();
    }

    void add_gekisou_combo(int32_t time_ms, int32_t count) {
        add_combo_stack_.push_back({time_ms, f32(count), playing_range_index()});
        flag();
    }

    void add_luck_point(int32_t p) {
        int32_t i = playing_range_index();
        if (i >= 0)
            states[i].luck.total_bonus_point = wrap_add(states[i].luck.total_bonus_point, p);
    }

    void add_luck_gauge_percent(int32_t g) {
        int32_t i = playing_range_index();
        if (i >= 0)
            states[i].luck.add_gauge(g);
    }

    int32_t current_gauge_max() const {
        int32_t i = playing_range_index();
        return i >= 0 ? states[i].luck.gauge_max : luck_gauge_max_;
    }

    int32_t add_combo_bonus(int32_t t, float bonus) {
        int32_t i = next_id();
        combo_bonus_ids_[i] = bonus;
        cb_stack_.push_back({t, bonus, wrap_sub(seq_, 1)});
        flag();
        return i;
    }

    void subtract_combo_bonus(int32_t t, int32_t id) {
        auto it = combo_bonus_ids_.find(id);
        if (it == combo_bonus_ids_.end())
            return;
        float v = it->second;
        combo_bonus_ids_.erase(it);
        cb_stack_.push_back({t, -v, wrap_sub(seq_, 1)});
        flag();
    }

    int32_t add_just_bonus(int32_t t, float bonus) {
        int32_t i = next_id();
        just_bonus_ids_[i] = bonus;
        jb_stack_.push_back({t, bonus, wrap_sub(seq_, 1)});
        flag();
        return i;
    }

    void subtract_just_bonus(int32_t t, int32_t id) {
        auto it = just_bonus_ids_.find(id);
        if (it == just_bonus_ids_.end())
            return;
        float v = it->second;
        just_bonus_ids_.erase(it);
        jb_stack_.push_back({t, -v, wrap_sub(seq_, 1)});
        flag();
    }

    int32_t add_cumulative_rule(int32_t unit, int32_t effect, int32_t max_cum, int32_t max_eff) {
        int32_t i = next_id();
        rules_[i] = {playing_range_index(), unit, effect, max_cum, max_eff};
        flag();
        return i;
    }

    void remove_cumulative_rule(int32_t id) {
        if (rules_.erase(id))
            flag();
    }

    int32_t enable_combo_protect(int32_t t, int32_t limit, int32_t mask) {
        int32_t i = next_id();
        if (limit < 1) {
            pr_stack_.push_back({t, 1, i, mask & 0xff});
        } else {
            limited_ids_.insert(i);
            pr_stack_.push_back({t, wrap_add(limit, 1), i, mask & 0xff});
        }
        flag();
        return i;
    }

    void disable_combo_protect(int32_t t, int32_t id) {
        int32_t d = limited_ids_.erase(id) ? -2 : -1;
        pr_stack_.push_back({t, d, id, 0});
        flag();
    }

    int32_t add_lot_probability_up(int32_t t, float value) {
        return storage_.add(false, t, value);
    }
    int32_t add_gauge_up(int32_t t, float value) {
        return storage_.add(true, t, value);
    }
    void subtract_lot_probability_up(int32_t t, int32_t id) {
        storage_.subtract(false, t, id);
    }
    void subtract_gauge_up(int32_t t, int32_t id) {
        storage_.subtract(true, t, id);
    }

    float gauge_speed_at(int32_t t) const {
        return FactorStorage::at(storage_.gauge, t);
    }
    int32_t lot_buff_at(int32_t t) const {
        return floor_to_i32(f32(FactorStorage::at(storage_.lot, t) * 100.0f));
    }

    void update(int32_t t, const std::vector<GkNote> &judged,
                const std::vector<std::pair<int32_t, int32_t>> &fever_updates,
                int32_t current_score, GkEnv &env) {
        for (size_t idx = 0; idx < states.size(); ++idx) {
            if (states[idx].state > kSStandby && !pending_[idx].empty()) {
                auto pend = std::move(pending_[idx]);
                pending_[idx].clear();
                for (const auto &p : pend)
                    update_luck(static_cast<int32_t>(idx), p[0], p[1], p[3], env);
            }
        }
        for (size_t idx = 0; idx < states.size(); ++idx) {
            for (const auto &jn : judged) {
                if (ranges[idx].targets.count(jn[0]))
                    judge(static_cast<int32_t>(idx), jn[1], jn[2], jn[3], env);
            }
        }
        if (needs_recalc_) {
            recalculate();
            needs_recalc_ = false;
        }
        for (const auto &[idx, fs] : fever_updates) {
            if (fs == kFeverEnd)
                states.at(idx).end_score = current_score;
        }
        pending_lots(t, env);
    }

    void before_update(float dt, int32_t t,
                       const std::vector<std::pair<int32_t, int32_t>> &fever_updates,
                       int32_t current_score, GkEnv &env) {
        state_updates.clear();
        lot_result_ = -1;
        lot_results.clear();
        current_playing_index = -1;
        for (size_t i = 0; i < states.size(); ++i)
            states[i].time_to = wrap_sub(ranges[i].start_ms, t);
        std::vector<int32_t> remove;
        for (int32_t idx : playing_) {
            auto &rs = states[idx];
            int32_t st = rs.state;
            if (st < kSEnd) {
                if (st == kSStart) {
                    rs.state = kSPlaying;
                    state_update(idx);
                    current_playing_index = idx;
                } else if (st == kSPlaying) {
                    current_playing_index = idx;
                }
            } else if (st == kSEnd) {
                current_playing_index = idx;
                if (!rs.sw_stopped)
                    rs.sw_elapsed = f32(rs.sw_elapsed + dt);
                if (last_note_delay_ms_ <= elapsed_ms(rs)) {
                    rs.sw_elapsed = 0.0f;
                    rs.sw_stopped = false;
                    rs.state = kSDelay;
                    state_update(idx);
                }
            } else if (st == kSDelay) {
                if (!rs.sw_stopped)
                    rs.sw_elapsed = f32(rs.sw_elapsed + dt);
                if (500 <= elapsed_ms(rs)) {
                    rs.state = kSComplete;
                    rs.sw_stopped = true;
                    state_update(idx);
                }
            } else if (st == kSComplete) {
                rs.state = kSFinish;
                state_update(idx);
                remove.push_back(idx);
                disable_rush(t, env);
            }
        }
        for (int32_t idx : remove) {
            auto it = std::find(playing_.begin(), playing_.end(), idx);
            if (it != playing_.end())
                playing_.erase(it);
        }
        for (const auto &[idx, fs] : fever_updates) {
            auto &rs = states.at(idx);
            if (fs == kFeverEnd) {
                rs.state = kSEnd;
                rs.sw_elapsed = 0.0f;
                rs.sw_stopped = false;
                state_update(idx);
            } else if (fs == kFeverFever) {
                rs.state = kSStart;
                rs.start_score = current_score;
                playing_.push_back(idx);
                state_update(idx);
                current_playing_index = idx;
            }
        }
        for (size_t idx = 0; idx < states.size(); ++idx) {
            if (states[idx].state == kSWait && states[idx].time_to < 4001) {
                states[idx].state = kSStandby;
                state_update(static_cast<int32_t>(idx));
            }
        }
    }

    std::optional<int32_t> gekisou_combo(int32_t time_ms) const override {
        for (size_t i = 0; i < ranges.size(); ++i) {
            const auto &r = ranges[i];
            if (r.mission == kMCombo && r.start_ms <= time_ms && time_ms <= r.end_ms)
                return timing_combo_in_range(static_cast<int32_t>(i), time_ms);
        }
        return std::nullopt;
    }

private:
    struct Resume {
        size_t processed = 0;
        int32_t combo = 0;
        int32_t max_combo = 0;
        int32_t just = 0;
        int32_t raw_just = 0;
        int32_t cum_base = 0;
        float combo_bonus = 1.0f;
        float just_bonus = 1.0f;
        size_t i_cb = 0;
        size_t i_jb = 0;
        size_t i_add_just = 0;
        size_t i_add_combo = 0;
    };

    struct Rule {
        int32_t range_index = -1;
        int32_t unit = 0;
        int32_t effect = 0;
        int32_t max_cum = 0;
        int32_t max_eff = 0;
    };

    struct FloatStackEntry {
        int32_t time_ms = 0;
        float val = 0.0f;
        int32_t aux = 0;
    };

    static int64_t elapsed_ms(const RangeState &rs) {
        float v = f32(rs.sw_elapsed * 1000.0f);
        if (v == std::numeric_limits<float>::infinity())
            return -9007199254740991LL;
        return static_cast<int64_t>(std::trunc(v));
    }

    int32_t next_id() {
        handle_id_ = wrap_add(handle_id_, 1);
        return handle_id_;
    }

    void flag() {
        needs_recalc_ = true;
        full_recalc_ = true;
    }

    void state_update(int32_t idx) {
        if (std::find(state_updates.begin(), state_updates.end(), idx) == state_updates.end())
            state_updates.push_back(idx);
    }

    void apply_fixed_count(int32_t idx, float delta, int32_t range, bool just) {
        if (range != -1 && range != idx)
            return;
        int32_t n = static_cast<int32_t>(std::trunc(delta));
        auto &rs = states[idx];
        if (just) {
            rs.just = wrap_add(rs.just, n);
            rs.cum_base = wrap_add(rs.cum_base, n);
        } else {
            rs.combo = wrap_add(rs.combo, n);
            rs.max_combo = std::max(rs.max_combo, rs.combo);
        }
    }

    int32_t cumulative_just_bonus(int32_t range_index, int32_t x) const {
        int32_t total = 0;
        for (const auto &[_, r] : rules_) {
            if (r.range_index != range_index || r.unit <= 0)
                continue;
            int32_t steps = x / r.unit;
            if (r.max_cum > 0 && steps > r.max_cum)
                steps = r.max_cum;
            int32_t v = wrap_mul(steps, r.effect);
            if (r.max_eff > 0 && r.max_eff <= v)
                v = r.max_eff;
            total = wrap_add(total, v);
        }
        return total;
    }

    void recalculate() {
        for (size_t idx = 0; idx < states.size(); ++idx) {
            if (states[idx].state >= kSDelay || history_[idx].empty())
                continue;
            auto &rr = resume_[idx];
            auto &rs = states[idx];
            const auto &h = history_[idx];
            float cb = 1.0f, jb = 1.0f;
            size_t i_cb = 0, i_jb = 0, i_pr = 0, k = 0, i_add_just = 0, i_add_combo = 0;
            if (!full_recalc_ && pr_stack_.empty() && 1 <= rr.processed &&
                rr.processed <= h.size()) {
                rs.combo = rr.combo;
                rs.max_combo = rr.max_combo;
                rs.just = rr.just;
                rs.raw_just = rr.raw_just;
                rs.cum_base = rr.cum_base;
                cb = rr.combo_bonus;
                jb = rr.just_bonus;
                i_cb = rr.i_cb;
                i_jb = rr.i_jb;
                k = rr.processed;
                i_add_just = rr.i_add_just;
                i_add_combo = rr.i_add_combo;
            } else {
                rs.combo = rs.max_combo = rs.just = rs.raw_just = rs.cum_base = 0;
                snapshot_[idx].clear();
                unlimited_.clear();
                limited_.clear();
            }
            while (k < h.size()) {
                auto [et, j, es] = h[k];
                while (i_cb < cb_stack_.size()) {
                    const auto &c = cb_stack_[i_cb];
                    if (et <= c.time_ms && (et < c.time_ms || es < c.aux))
                        break;
                    cb = f32(cb + c.val);
                    ++i_cb;
                }
                while (i_jb < jb_stack_.size()) {
                    const auto &c = jb_stack_[i_jb];
                    if (et <= c.time_ms && (et < c.time_ms || es < c.aux))
                        break;
                    jb = f32(jb + c.val);
                    ++i_jb;
                }
                while (i_pr < pr_stack_.size() && et >= pr_stack_[i_pr][0]) {
                    auto [_, d, pid, mask] = pr_stack_[i_pr];
                    if (d < 2) {
                        if (d == 1) {
                            unlimited_.emplace_back(pid, mask);
                        } else if (d < -1) {
                            auto it = std::find_if(limited_.begin(), limited_.end(),
                                                   [&](const auto &x) { return x[0] == pid; });
                            if (it != limited_.end())
                                limited_.erase(it);
                        } else {
                            auto it = std::find_if(unlimited_.begin(), unlimited_.end(),
                                                   [&](const auto &x) { return x.first == pid; });
                            if (it != unlimited_.end())
                                unlimited_.erase(it);
                        }
                    } else {
                        limited_.push_back({pid, wrap_sub(d, 1), mask});
                    }
                    ++i_pr;
                }
                while (i_add_just < add_just_stack_.size() &&
                       add_just_stack_[i_add_just].time_ms <= et) {
                    const auto &c = add_just_stack_[i_add_just];
                    apply_fixed_count(static_cast<int32_t>(idx), c.val, c.aux, true);
                    ++i_add_just;
                }
                while (i_add_combo < add_combo_stack_.size() &&
                       add_combo_stack_[i_add_combo].time_ms <= et) {
                    const auto &c = add_combo_stack_[i_add_combo];
                    apply_fixed_count(static_cast<int32_t>(idx), c.val, c.aux, false);
                    ++i_add_combo;
                }
                entry(static_cast<int32_t>(idx), j, floor_to_i32(cb), floor_to_i32(jb));
                snapshot_[idx].push_back(rs.combo);
                ++k;
            }
            resume_[idx] = {h.size(), rs.combo, rs.max_combo, rs.just, rs.raw_just, rs.cum_base,
                            cb,       jb,       i_cb,         i_jb,    i_add_just,  i_add_combo};
            for (size_t i = i_add_just; i < add_just_stack_.size(); ++i) {
                const auto &c = add_just_stack_[i];
                apply_fixed_count(static_cast<int32_t>(idx), c.val, c.aux, true);
            }
            for (size_t i = i_add_combo; i < add_combo_stack_.size(); ++i) {
                const auto &c = add_combo_stack_[i];
                apply_fixed_count(static_cast<int32_t>(idx), c.val, c.aux, false);
            }
        }
        full_recalc_ = false;
    }

    void entry(int32_t idx, int32_t j, int32_t i_combo, int32_t i_just) {
        auto &rs = states[idx];
        if (static_cast<uint32_t>(j - 3) < 0xfffffffeu) {
            if (static_cast<uint32_t>(j - 7) > 0xfffffffbu) {
                rs.combo = wrap_add(rs.combo, i_combo);
                rs.max_combo = std::max(rs.max_combo, rs.combo);
            } else {
                return;
            }
        } else {
            int32_t bit = 1 << (j & 31);
            auto eff = [](int32_t m) { return m != 0 ? m : 6; };
            bool prot = false;
            for (const auto &[_, m] : unlimited_) {
                if ((eff(m) & bit) != 0) {
                    prot = true;
                    break;
                }
            }
            for (ptrdiff_t i = static_cast<ptrdiff_t>(limited_.size()) - 1; i >= 0; --i) {
                auto [pid, rem, m] = limited_[static_cast<size_t>(i)];
                if ((eff(m) & bit) != 0) {
                    int32_t left = wrap_sub(rem, 1);
                    if (left < 1)
                        limited_.erase(limited_.begin() + i);
                    else
                        limited_[static_cast<size_t>(i)] = {pid, left, m};
                    prot = true;
                }
            }
            if (!prot) {
                rs.max_combo = std::max(rs.max_combo, rs.combo);
                rs.combo = 0;
            }
        }
        if (j == 6) {
            rs.raw_just = wrap_add(rs.raw_just, 1);
            rs.cum_base = wrap_add(rs.cum_base, i_just);
            int32_t c = cumulative_just_bonus(idx, rs.cum_base);
            rs.just = wrap_add(wrap_add(c, i_just), rs.just);
        }
    }

    int32_t timing_combo_in_range(int32_t idx, int32_t t) const {
        const auto &h = history_[idx];
        const auto &sn = snapshot_[idx];
        size_t n = std::min(h.size(), sn.size());
        if (n == 0)
            return 0;
        int32_t score_frame = t > 0 ? static_cast<int32_t>(std::ceil(f32(f32(t) / 40.0f))) : 0;
        int32_t prev_end = wrap_sub(wrap_mul(score_frame, 40), 40);
        if (prev_end < h[0][0])
            return 0;
        int32_t hi = static_cast<int32_t>(n) - 1;
        if (hi < 1)
            return sn[0];
        int32_t lo = 0;
        for (;;) {
            int32_t mid = lo + sdiv(hi - lo + 1, 2);
            if (h[static_cast<size_t>(mid)][0] <= prev_end)
                lo = mid;
            else
                hi = mid - 1;
            if (lo >= hi)
                return sn[static_cast<size_t>(lo)];
        }
    }

    void update_luck(int32_t idx, int32_t nt, int32_t tn, int32_t j, GkEnv &env) {
        if (weighted)
            return;
        int32_t buff = floor_to_i32(f32(FactorStorage::at(storage_.lot, tn) * 100.0f));
        machine.set_buff(buff);
        float gup = FactorStorage::at(storage_.gauge, tn);
        int32_t base = machine.base_point(nt, j, env.random);
        int32_t add = floor_to_i32(f32(f32(gup + 1.0f) * f32(base)));
        states[idx].luck.add_gauge(add);
        consume_lot(idx, tn, env);
    }

    void disable_rush(int32_t t, GkEnv &env) {
        if (rush_id_ != 0) {
            env.handle.disable(t, rush_id_);
            rush_id_ = 0;
        }
    }

    void consume_lot(int32_t idx, int32_t t, GkEnv &env) {
        auto &st = states[idx];
        if (st.state > kSEnd || st.luck.lot_count < 1)
            return;
        st.luck.lot_count--;
        if (st.luck.next == -1)
            st.luck.next = machine.luck_bonus(0, t, env.random);
        int32_t next = st.luck.next;
        int32_t rush = st.luck.rush_combo;
        if (rush == 0) {
            if (next == 3)
                rush_id_ = env.handle.add(t, rush_percent_);
            else
                disable_rush(t, env);
        } else if (next != 3) {
            disable_rush(t, env);
        }
        st.luck.add_score(next);
        if (next != -1) {
            lot_result_ = next;
            lot_results.push_back(next);
        }
        st.luck.next = machine.luck_bonus(st.luck.current_lot_type(), t, env.random);
    }

    void pending_lots(int32_t t, GkEnv &env) {
        if (weighted)
            return;
        auto pl = playing_;
        for (int32_t idx : pl) {
            auto &rs = states[idx];
            if (ranges[idx].mission == kMLuck && rs.state == kSPlaying && rs.luck.lot_count != 0 &&
                lot_result_ == -1) {
                machine.set_buff(floor_to_i32(f32(FactorStorage::at(storage_.lot, t) * 100.0f)));
                consume_lot(idx, t, env);
            }
        }
    }

    void judge(int32_t idx, int32_t nt, int32_t tn, int32_t j, GkEnv &env) {
        if (j == 7 || j == 0)
            return;
        history_[idx].push_back({tn, j, seq_});
        seq_ = wrap_add(seq_, 1);
        needs_recalc_ = true;
        if (static_cast<uint32_t>(j - 3) < 4u)
            states[idx].last_combo_ms = tn;
        if (j == 6)
            states[idx].last_just_ms = tn;
        if (ranges[idx].mission == kMLuck) {
            if (states[idx].state < kSStart)
                pending_[idx].push_back({nt, tn, 0, j});
            else
                update_luck(idx, nt, tn, j, env);
        }
    }

    FactorStorage storage_;
    int32_t rush_percent_;
    int32_t last_note_delay_ms_;
    int32_t luck_gauge_max_;
    int32_t rush_id_ = 0;
    std::vector<int32_t> playing_;
    std::unordered_map<int32_t, float> combo_bonus_ids_;
    std::unordered_map<int32_t, float> just_bonus_ids_;
    std::unordered_map<int32_t, Rule> rules_;
    int32_t handle_id_ = 0;
    bool needs_recalc_ = false;
    bool full_recalc_ = false;
    int32_t seq_ = 0;
    std::vector<std::vector<std::array<int32_t, 3>>> history_;
    std::vector<std::vector<int32_t>> snapshot_;
    std::vector<Resume> resume_;
    std::vector<FloatStackEntry> cb_stack_;
    std::vector<FloatStackEntry> jb_stack_;
    std::vector<FloatStackEntry> add_just_stack_;
    std::vector<FloatStackEntry> add_combo_stack_;
    std::vector<std::array<int32_t, 4>> pr_stack_;
    std::vector<std::pair<int32_t, int32_t>> unlimited_;
    std::vector<std::array<int32_t, 3>> limited_;
    std::unordered_set<int32_t> limited_ids_;
    std::vector<std::vector<std::array<int32_t, 4>>> pending_;
    int32_t lot_result_ = -1;
};

class FeverUpdater {
public:
    explicit FeverUpdater(std::vector<std::pair<int32_t, int32_t>> fevers)
        : fevers_(std::move(fevers)), state_(fevers_.size(), kFeverWait) {}

    const std::vector<std::pair<int32_t, int32_t>> &update(int32_t t) {
        out_.clear();
        for (size_t i = 0; i < fevers_.size(); ++i) {
            auto [s, e] = fevers_[i];
            int32_t st = state_[i];
            int32_t next = (st == kFeverWait && t >= s)
                               ? kFeverFever
                               : ((st == kFeverFever && t >= e) ? kFeverEnd : 0);
            if (!next)
                continue;
            state_[i] = next;
            out_.emplace_back(static_cast<int32_t>(i), next);
        }
        return out_;
    }

private:
    std::vector<std::pair<int32_t, int32_t>> fevers_;
    std::vector<int32_t> state_;
    std::vector<std::pair<int32_t, int32_t>> out_;
};

inline int32_t mission_pattern(int32_t t0, int32_t t1, int32_t t2) {
    if (!t0 || !t1 || !t2)
        return 0;
    if (t0 == t1)
        return t0 == t2 ? 1 : 3;
    if (t1 != t2 && t0 != t2)
        return 2;
    return 3;
}

inline std::array<std::array<int32_t, 5>, 3> ranking_factors(const GekisouTables &t,
                                                             int32_t pattern) {
    std::array<std::array<int32_t, 5>, 3> out{};
    for (const auto &r : t.ranking_bonuses) {
        int32_t c = r.count - 1;
        int32_t k = r.rank - 1;
        if (r.pattern == pattern && static_cast<uint32_t>(c) < 3u &&
            static_cast<uint32_t>(k) <= 4u) {
            out[c][k] = r.percent;
        }
    }
    return out;
}
// 技能条件检查器、累积条件与状态机
struct GkView {
    Controller *ctrl = nullptr;
    std::vector<int32_t> prev_lots;
    int32_t prev_lot_ms = 0;
};

struct CheckCtx {
    std::optional<int32_t> prev_confirmed_rank;
    LifeController *life = nullptr;
    LiveRandom *random = nullptr;
    int32_t frame_time = 0;
    int32_t current_combo = 0;
    const std::vector<std::array<int32_t, 3>> *judged = nullptr;
    const std::vector<std::pair<int32_t, int32_t>> *events = nullptr;
    std::optional<GkView> gk;
    bool record = false;
};

class Checker {
public:
    virtual ~Checker() = default;
    virtual std::pair<bool, int32_t> check(CheckCtx &ctx) = 0;
    virtual std::optional<int32_t> override_time() const {
        return std::nullopt;
    }
    virtual void reset(CheckCtx &) {}
    virtual bool count_resettable() const {
        return false;
    }
    virtual void reset_count(CheckCtx &) {}
    virtual std::optional<int32_t> idle_member() const {
        return std::nullopt;
    }
    virtual double chance(CheckCtx &ctx) {
        return check(ctx).first ? 1.0 : 0.0;
    }
};

class FixedChecker : public Checker {
public:
    explicit FixedChecker(bool ok) : ok_(ok) {}
    std::pair<bool, int32_t> check(CheckCtx &) override {
        return {ok_, ok_ ? 1 : 0};
    }
    std::optional<int32_t> idle_member() const override {
        return ok_ ? std::nullopt : std::optional<int32_t>(-1);
    }

private:
    bool ok_;
};

class AndChecker : public Checker {
public:
    explicit AndChecker(std::vector<std::shared_ptr<Checker>> items) : items_(std::move(items)) {
        for (const auto &it : items_)
            resettable_.push_back(it->count_resettable());
    }
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t hit = 0;
        for (size_t i = 0; i < items_.size(); ++i) {
            auto [ok, h] = items_[i]->check(ctx);
            hit = std::max(hit, h);
            if (!ok) {
                if (!resettable_[i]) {
                    for (size_t k = 0; k < items_.size(); ++k)
                        if (resettable_[k])
                            items_[k]->reset_count(ctx);
                }
                return {false, 0};
            }
        }
        return {true, hit};
    }
    void reset(CheckCtx &ctx) override {
        for (auto &it : items_)
            it->reset(ctx);
    }
    double chance(CheckCtx &ctx) override {
        double p = 1.0;
        for (auto &it : items_)
            p *= it->chance(ctx);
        return p;
    }

private:
    std::vector<std::shared_ptr<Checker>> items_;
    std::vector<bool> resettable_;
};

class OrChecker : public Checker {
public:
    explicit OrChecker(std::vector<std::shared_ptr<Checker>> items) : items_(std::move(items)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t hit = 0;
        for (auto &c : items_) {
            auto [ok, h] = c->check(ctx);
            hit = std::max(hit, h);
            if (ok)
                return {true, hit};
        }
        return {false, 0};
    }
    void reset(CheckCtx &ctx) override {
        for (auto &it : items_)
            it->reset(ctx);
    }
    double chance(CheckCtx &ctx) override {
        double prod = 1.0;
        for (auto &it : items_)
            prod *= (1.0 - it->chance(ctx));
        return 1.0 - prod;
    }

private:
    std::vector<std::shared_ptr<Checker>> items_;
};

class NotChecker : public Checker {
public:
    explicit NotChecker(std::shared_ptr<Checker> inner) : inner_(std::move(inner)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        auto [ok, h] = inner_->check(ctx);
        return {!ok, h};
    }
    std::optional<int32_t> override_time() const override {
        return inner_->override_time();
    }
    void reset(CheckCtx &ctx) override {
        inner_->reset(ctx);
    }
    bool count_resettable() const override {
        return true;
    }
    void reset_count(CheckCtx &ctx) override {
        if (inner_->count_resettable())
            inner_->reset_count(ctx);
    }
    double chance(CheckCtx &ctx) override {
        return 1.0 - inner_->chance(ctx);
    }

private:
    std::shared_ptr<Checker> inner_;
};

class LifeChecker : public Checker {
public:
    LifeChecker(int32_t type, int32_t value) : type_(type), value_(value) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t life = ctx.life->life_at(ctx.frame_time);
        bool ok = (type_ == 2000)   ? (value_ < life)
                  : (type_ == 2001) ? (value_ <= life)
                  : (type_ == 2002) ? (life < value_)
                                    : (life <= value_);
        return {ok, ok ? 1 : 0};
    }

private:
    int32_t type_;
    int32_t value_;
};

class LifeChangedChecker : public Checker {
public:
    explicit LifeChangedChecker(int32_t previous) : previous_(previous) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t life = ctx.life->life_at(ctx.frame_time);
        bool ok = (previous_ != life);
        previous_ = life;
        return {ok, 0};
    }

private:
    int32_t previous_;
};

class LifePercentChecker : public Checker {
public:
    explicit LifePercentChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t life = ctx.life->life_at(ctx.frame_time);
        bool ok = (previous_ > threshold_ && life <= threshold_);
        previous_ = life;
        return {ok, ok ? 1 : 0};
    }

private:
    int32_t threshold_;
    int32_t previous_ = 0;
};

class LifeDeltaChecker : public Checker {
public:
    LifeDeltaChecker(int32_t threshold, bool increase, int32_t previous)
        : threshold_(threshold), increase_(increase), previous_(previous) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t life = ctx.life->life_at(ctx.frame_time);
        int32_t change = increase_ ? wrap_sub(life, previous_) : wrap_sub(previous_, life);
        previous_ = life;
        if (change > 0)
            count_ = wrap_add(count_, change);
        int32_t hits = (threshold_ > 0 && count_ >= threshold_) ? (count_ / threshold_) : 0;
        if (hits > 0)
            count_ %= threshold_;
        return {hits > 0, hits};
    }
    void reset(CheckCtx &ctx) override {
        count_ = 0;
        previous_ = ctx.life->life_at(ctx.frame_time);
    }
    bool count_resettable() const override {
        return true;
    }
    void reset_count(CheckCtx &ctx) override {
        reset(ctx);
    }

private:
    int32_t threshold_;
    bool increase_;
    int32_t previous_;
    int32_t count_ = 0;
};

class LiveComboMultipleChecker : public Checker {
public:
    explicit LiveComboMultipleChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t combo = ctx.current_combo;
        if (combo < 1 || threshold_ < 1) {
            previous_ = 0;
            return {false, 0};
        }
        if (combo <= previous_) {
            previous_ = combo;
            return {false, 0};
        }
        int32_t hit = (combo / threshold_) - (previous_ / threshold_);
        previous_ = combo;
        return {hit > 0, hit};
    }
    void reset(CheckCtx &) override {
        previous_ = 0;
    }

private:
    int32_t threshold_;
    int32_t previous_ = 0;
};

class LiveComboAtLeastChecker : public Checker {
public:
    explicit LiveComboAtLeastChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        bool hit = threshold_ > 0 && ctx.current_combo >= threshold_;
        return {hit, hit ? 1 : 0};
    }

private:
    int32_t threshold_;
};

class ElapsedTimeChecker : public Checker {
public:
    ElapsedTimeChecker(int32_t period, int32_t previous) : period_(period), previous_(previous) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        elapsed_ = wrap_add(elapsed_, wrap_sub(ctx.frame_time, previous_));
        previous_ = ctx.frame_time;
        bool hit = elapsed_ >= period_;
        if (hit)
            elapsed_ = wrap_sub(elapsed_, period_);
        return {hit, hit ? 1 : 0};
    }
    void reset(CheckCtx &ctx) override {
        elapsed_ = 0;
        previous_ = ctx.frame_time;
    }
    bool count_resettable() const override {
        return true;
    }
    void reset_count(CheckCtx &ctx) override {
        reset(ctx);
    }

private:
    int32_t period_;
    int32_t previous_;
    int32_t elapsed_ = 0;
};

class SnapGekisouStartChecker : public Checker {
public:
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        if (ctx.gk) {
            for (int32_t idx : ctx.gk->ctrl->state_updates) {
                if (ctx.gk->ctrl->states[idx].state == kSStart && idx != previous_) {
                    previous_ = idx;
                    return {true, 1};
                }
            }
        }
        return {false, 0};
    }
    void reset(CheckCtx &) override {
        previous_ = -1;
    }

private:
    int32_t previous_ = -1;
};

class SameMemberLiveSkillChecker : public Checker {
public:
    explicit SameMemberLiveSkillChecker(int32_t member) : member_(member) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        bool ok = false;
        if (ctx.events) {
            for (const auto &[idx, _] : *ctx.events) {
                if (idx == member_) {
                    ok = true;
                    break;
                }
            }
        }
        return {ok, ok ? 1 : 0};
    }
    std::optional<int32_t> idle_member() const override {
        return member_;
    }

private:
    int32_t member_;
};

class ProbabilityChecker : public Checker {
public:
    explicit ProbabilityChecker(float rate) : rate_(rate) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        if (ctx.record)
            return {true, 0};
        return {ctx.random->value(LiveRandom::kSkill) < rate_, 0};
    }
    double chance(CheckCtx &) override {
        return static_cast<double>(rate_);
    }

private:
    float rate_;
};

class NoteJudgementMatchChecker : public Checker {
public:
    NoteJudgementMatchChecker(int32_t kind, std::vector<int32_t> targets)
        : kind_(kind), targets_(std::move(targets)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t hit = 0;
        if (ctx.judged) {
            for (const auto &[nid, j, tn] : *ctx.judged) {
                if (kind_ == 1020 && (j == -1 || j == 7))
                    continue;
                for (int32_t target : targets_) {
                    bool matches = (kind_ == 1000)   ? (target == j)
                                   : (kind_ == 1010) ? (j >= target)
                                                     : (j <= target);
                    if (matches) {
                        ++hit;
                        if (nid >= 0)
                            override_ = tn;
                        else
                            override_.reset();
                        break;
                    }
                }
            }
        }
        return {hit > 0, hit};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        override_.reset();
    }

private:
    int32_t kind_;
    std::vector<int32_t> targets_;
    std::optional<int32_t> override_;
};

class NoteJudgementCountChecker : public Checker {
public:
    NoteJudgementCountChecker(int32_t n, bool consecutive, std::vector<int32_t> targets)
        : n_(n), consecutive_(consecutive), targets_(std::move(targets)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t trig = -1;
        int32_t trig_ms = 0;
        bool ok = false;
        int32_t hit = 0;
        if (ctx.judged) {
            for (const auto &[nid, j, tn] : *ctx.judged) {
                for (int32_t tj : targets_) {
                    if (tj == j) {
                        count_ = wrap_add(count_, 1);
                        if (count_ >= n_) {
                            ok = true;
                            ++hit;
                            count_ = 0;
                            trig = nid;
                            trig_ms = tn;
                        }
                    } else if (consecutive_) {
                        count_ = 0;
                    }
                }
            }
        }
        if (ok && trig >= 0)
            override_ = trig_ms;
        return {ok, hit};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        count_ = 0;
    }
    bool count_resettable() const override {
        return true;
    }
    void reset_count(CheckCtx &ctx) override {
        reset(ctx);
    }

private:
    int32_t n_;
    bool consecutive_;
    std::vector<int32_t> targets_;
    int32_t count_ = 0;
    std::optional<int32_t> override_;
};

class LuckLotResultChecker : public Checker {
public:
    explicit LuckLotResultChecker(int32_t target) : target_(target) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t hit = 0;
        if (ctx.gk) {
            for (int32_t r : ctx.gk->prev_lots)
                if (r == target_)
                    ++hit;
            if (hit > 0) {
                override_ = ctx.gk->prev_lot_ms;
                return {true, hit};
            }
        }
        return {false, 0};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        override_.reset();
    }

private:
    int32_t target_;
    std::optional<int32_t> override_;
};

class ComboAtLeastChecker : public Checker {
public:
    explicit ComboAtLeastChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        if (!ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        int32_t idx = c->current_playing_index;
        if (idx < 0 || threshold_ <= 0)
            return {false, 0};
        const auto &rs = c->states[idx];
        if (rs.combo < threshold_)
            return {false, 0};
        if (rs.last_combo_ms >= 0)
            override_ = rs.last_combo_ms;
        return {true, 1};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        override_.reset();
    }

private:
    int32_t threshold_;
    std::optional<int32_t> override_;
};

class GkIntervalChecker : public Checker {
public:
    GkIntervalChecker(int32_t kind, int32_t n) : kind_(kind), n_(n) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t idx = ctx.gk ? ctx.gk->ctrl->current_playing_index : -1;
        const RangeState *rs = (ctx.gk && idx >= 0) ? &ctx.gk->ctrl->states[idx] : nullptr;
        int32_t hits = 0;
        if (n_ > 0 && rs) {
            int32_t current = (kind_ == 7001)   ? rs->combo
                              : (kind_ == 7002) ? rs->just
                                                : rs->raw_just;
            if (current <= 0) {
                previous_ = 0;
            } else {
                int32_t old = previous_;
                previous_ = current;
                if (current > old)
                    hits = std::max(0, (current / n_) - (old / n_));
            }
        }
        if (hits > 0 && rs) {
            int32_t t = (kind_ == 7001) ? rs->last_combo_ms : rs->last_just_ms;
            if (t >= 0)
                override_ = t;
        }
        return {hits > 0, hits};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        previous_ = 0;
        override_.reset();
    }

private:
    int32_t kind_;
    int32_t n_;
    int32_t previous_ = 0;
    std::optional<int32_t> override_;
};

class JustAtLeastChecker : public Checker {
public:
    explicit JustAtLeastChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t idx = ctx.gk ? ctx.gk->ctrl->current_playing_index : -1;
        const RangeState *rs = (ctx.gk && idx >= 0) ? &ctx.gk->ctrl->states[idx] : nullptr;
        bool hit = threshold_ > 0 && rs != nullptr && rs->just >= threshold_;
        if (hit && rs->last_just_ms >= 0)
            override_ = rs->last_just_ms;
        return {hit, hit ? 1 : 0};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        override_.reset();
    }

private:
    int32_t threshold_;
    std::optional<int32_t> override_;
};

class GkJustEdgeChecker : public Checker {
public:
    GkJustEdgeChecker(bool raw, int32_t threshold) : raw_(raw), threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t idx = ctx.gk ? ctx.gk->ctrl->current_playing_index : -1;
        const RangeState *rs = (ctx.gk && idx >= 0) ? &ctx.gk->ctrl->states[idx] : nullptr;
        if (threshold_ <= 0)
            return {false, 0};
        if (!rs) {
            previous_ = 0;
            last_range_ = -1;
            return {false, 0};
        }
        if (idx != last_range_) {
            previous_ = 0;
            last_range_ = idx;
        }
        int32_t count = raw_ ? rs->raw_just : rs->just;
        bool hit = previous_ < threshold_ && count >= threshold_;
        previous_ = count;
        if (hit && rs->last_just_ms >= 0)
            override_ = rs->last_just_ms;
        return {hit, hit ? 1 : 0};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        previous_ = 0;
        last_range_ = -1;
        override_.reset();
    }

private:
    bool raw_;
    int32_t threshold_;
    int32_t previous_ = 0;
    int32_t last_range_ = -1;
    std::optional<int32_t> override_;
};

class GkOnceChecker : public Checker {
public:
    explicit GkOnceChecker(std::function<bool(CheckCtx &)> eligible)
        : eligible_(std::move(eligible)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        bool hit = !triggered_ && eligible_(ctx);
        if (hit)
            triggered_ = true;
        return {hit, hit ? 1 : 0};
    }
    void reset(CheckCtx &) override {
        triggered_ = false;
    }

private:
    std::function<bool(CheckCtx &)> eligible_;
    bool triggered_ = false;
};

inline bool mission_ok(const std::vector<int32_t> &missions, int32_t mission) {
    if (missions.empty())
        return true;
    for (int32_t m : missions)
        if (m == kMAll || m == mission)
            return true;
    return false;
}

class RangeStartChecker : public Checker {
public:
    explicit RangeStartChecker(std::vector<int32_t> missions) : missions_(std::move(missions)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        if (!ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        for (int32_t idx : c->state_updates) {
            int32_t s = c->states[idx].state;
            if (s == kSFinish) {
                triggered_.erase(std::remove(triggered_.begin(), triggered_.end(), idx),
                                 triggered_.end());
            } else if (s == kSStart) {
                if (!mission_ok(missions_, c->ranges[idx].mission))
                    continue;
                if (std::find(triggered_.begin(), triggered_.end(), idx) == triggered_.end()) {
                    triggered_.push_back(idx);
                    override_ = c->ranges[idx].start_ms;
                    return {true, 1};
                }
            }
        }
        return {false, 0};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        triggered_.clear();
        override_.reset();
    }

private:
    std::vector<int32_t> missions_;
    std::vector<int32_t> triggered_;
    std::optional<int32_t> override_;
};

class RangeCompleteChecker : public Checker {
public:
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        if (!ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        for (int32_t i : c->state_updates) {
            if (c->states[i].state == kSComplete)
                return {true, 1};
        }
        return {false, 0};
    }
};

class RangePlayingChecker : public Checker {
public:
    explicit RangePlayingChecker(std::vector<int32_t> missions) : missions_(std::move(missions)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        if (!ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        int32_t first = -1;
        for (int32_t idx : c->state_updates) {
            int32_t s = c->states[idx].state;
            if (s == kSStart || s == kSPlaying) {
                bool exists = false;
                for (const auto &a : active_)
                    if (a.first == idx)
                        exists = true;
                if (!exists) {
                    active_.emplace_back(idx, c->ranges[idx].mission);
                    if (first < 0)
                        first = c->ranges[idx].start_ms;
                }
            } else if (s == kSComplete || s == kSFinish) {
                active_.erase(std::remove_if(active_.begin(), active_.end(),
                                             [&](const auto &a) { return a.first == idx; }),
                              active_.end());
            }
        }
        if (active_.empty())
            return {false, 0};
        if (!missions_.empty() &&
            std::find(missions_.begin(), missions_.end(), kMAll) == missions_.end()) {
            bool any_m = false;
            for (const auto &a : active_) {
                if (std::find(missions_.begin(), missions_.end(), a.second) != missions_.end()) {
                    any_m = true;
                    break;
                }
            }
            if (!any_m)
                return {false, 0};
        }
        if (first >= 0)
            override_ = first;
        return {true, 1};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        active_.clear();
        override_.reset();
    }

private:
    std::vector<int32_t> missions_;
    std::vector<std::pair<int32_t, int32_t>> active_;
    std::optional<int32_t> override_;
};

class LuckRushPlayingChecker : public Checker {
public:
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        if (ctx.record || !ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        int32_t idx = c->current_playing_index;
        if (idx >= 0 && c->ranges[idx].mission == kMLuck)
            rush_ = (c->states[idx].luck.rush_combo != 0);
        for (int32_t i : c->state_updates) {
            int32_t s = c->states[i].state;
            if (c->ranges[i].mission == kMLuck && (s == kSComplete || s == kSFinish))
                rush_ = false;
        }
        return {rush_, rush_ ? 1 : 0};
    }
    void reset(CheckCtx &) override {
        rush_ = false;
    }

private:
    bool rush_ = false;
};

inline double chance_of(const std::shared_ptr<Checker> &checker, CheckCtx &ctx) {
    return checker ? checker->chance(ctx) : 1.0;
}

class Cumulative {
public:
    virtual ~Cumulative() = default;
    virtual int32_t init_count() const {
        return 0;
    }
    virtual int32_t unit() const {
        return 0;
    }
    virtual int32_t max_val() const {
        return 0;
    }
    virtual int32_t update_count(CheckCtx &ctx) = 0;
    virtual void reset() {}
};

class CumulativeFixed : public Cumulative {
public:
    explicit CumulativeFixed(int32_t value) : value_(value) {}
    int32_t init_count() const override {
        return value_;
    }
    int32_t update_count(CheckCtx &) override {
        return value_;
    }

private:
    int32_t value_;
};

class ComboPerN : public Cumulative {
public:
    ComboPerN(int32_t n, int32_t maximum, bool just) : n_(n), maximum_(maximum), just_(just) {}
    int32_t unit() const override {
        return n_;
    }
    int32_t max_val() const override {
        return maximum_;
    }
    int32_t update_count(CheckCtx &ctx) override {
        if (!ctx.gk)
            return 0;
        int32_t idx = ctx.gk->ctrl->current_playing_index;
        int32_t v = 0;
        if (idx >= 0) {
            const auto &st = ctx.gk->ctrl->states[idx];
            v = floor_to_i32(f32(f32(just_ ? st.just : st.combo) / f32(n_)));
        }
        return std::min(v, maximum_);
    }

private:
    int32_t n_;
    int32_t maximum_;
    bool just_;
};

class LifePerN : public Cumulative {
public:
    LifePerN(int32_t init, int32_t n, int32_t maximum, bool limit)
        : init_(init), n_(n), maximum_(maximum), limit_(limit) {}
    int32_t init_count() const override {
        return init_;
    }
    int32_t update_count(CheckCtx &ctx) override {
        int32_t life = limit_ ? ctx.life->maximum() : ctx.life->life_at(ctx.frame_time);
        return std::min(floor_to_i32(f32(f32(life) / f32(n_))), maximum_);
    }

private:
    int32_t init_;
    int32_t n_;
    int32_t maximum_;
    bool limit_;
};

class JudgementPerN : public Cumulative {
public:
    JudgementPerN(int32_t n, std::vector<int32_t> targets, int32_t maximum, int32_t kind)
        : n_(n), targets_(std::move(targets)), maximum_(maximum), kind_(kind) {}
    int32_t unit() const override {
        return n_;
    }
    int32_t max_val() const override {
        return maximum_;
    }
    int32_t update_count(CheckCtx &ctx) override {
        if (ctx.judged) {
            for (const auto &jn : *ctx.judged) {
                int32_t j = jn[1];
                bool match = false;
                for (int32_t t : targets_) {
                    if ((kind_ == 1001) ? (j >= t) : (kind_ == 1002) ? (j <= t) : (j == t)) {
                        match = true;
                        break;
                    }
                }
                if (match)
                    count_ = wrap_add(count_, 1);
            }
        }
        return std::min(floor_to_i32(f32(f32(count_) / f32(n_))), maximum_);
    }
    void reset() override {
        count_ = 0;
    }

private:
    int32_t n_;
    std::vector<int32_t> targets_;
    int32_t maximum_;
    int32_t kind_;
    int32_t count_ = 0;
};

class ElapsedCount : public Cumulative {
public:
    ElapsedCount(int32_t period, int32_t previous, int32_t maximum)
        : period_(period), previous_(previous), maximum_(maximum) {}
    int32_t update_count(CheckCtx &ctx) override {
        elapsed_ = wrap_add(elapsed_, wrap_sub(ctx.frame_time, previous_));
        previous_ = ctx.frame_time;
        if (elapsed_ >= period_) {
            elapsed_ = wrap_sub(elapsed_, period_);
            count_ = wrap_add(count_, 1);
        }
        return std::min(count_, maximum_);
    }
    void reset() override {
        elapsed_ = 0;
        count_ = 0;
    }

private:
    int32_t period_;
    int32_t previous_;
    int32_t maximum_;
    int32_t elapsed_ = 0;
    int32_t count_ = 0;
};

struct Performer {
    std::optional<std::pair<int32_t, int32_t>> live_skill;
    std::vector<std::pair<int32_t, int32_t>> support_skills;
    int32_t band_id = 0;
    int32_t character_id = 0;
    int32_t card_type = 0;
    std::vector<int32_t> tag_ids;
    std::vector<int32_t> live_skill_categories;
    std::vector<int32_t> gekisou_skill_categories;
    int32_t gekisou_mission_type = 0;
    std::optional<std::pair<int32_t, int32_t>> gekisou_skill;
    std::vector<std::pair<int32_t, int32_t>> gekisou_support_skills;
    std::optional<int32_t> mission_only;
};

inline Performer make_performer(const EngineMaster &master, const MemberState &m,
                                const SnapState *s) {
    const auto &card = m.card;
    auto pairs = [](const std::array<int32_t, 2> &ids, const std::array<int32_t, 2> &levels) {
        std::vector<std::pair<int32_t, int32_t>> out;
        for (size_t i = 0; i < 2; ++i) {
            if (ids[i] != 0)
                out.emplace_back(ids[i], levels[i]);
        }
        return out;
    };
    Performer p;
    p.live_skill = std::make_pair(card.live_skill_id, m.growth.live_skill_level);
    if (s)
        p.support_skills = pairs(s->card.support_skill_ids, s->skill_levels);
    p.band_id = card.band_id;
    p.character_id = card.character_id;
    p.card_type = card.card_type;
    p.tag_ids = card.best_tags;
    p.live_skill_categories = m.live_skill_categories;
    p.gekisou_skill_categories = m.gekiso_skill_categories;
    p.gekisou_mission_type = m.gekiso_mission_type;
    if (card.gekiso_skill_id != 0 && master.gekiso_skills.count(card.gekiso_skill_id)) {
        p.gekisou_skill = std::make_pair(card.gekiso_skill_id, m.growth.gekiso_skill_level);
    }
    if (s)
        p.gekisou_support_skills = pairs(s->card.gekiso_support_skill_ids, s->gekiso_skill_levels);
    return p;
}

inline Performer gekiso_performer(Performer p) {
    p.live_skill.reset();
    p.support_skills.clear();
    return p;
}

inline bool performer_matches(const Performer &p, const TargetRow &t) {
    auto has_cat = [](const std::vector<int32_t> &targets, const std::vector<int32_t> &member) {
        for (int32_t c : targets) {
            if (c != 0 && std::find(member.begin(), member.end(), c) != member.end())
                return true;
        }
        return false;
    };
    return (t.band_id > 0 && t.band_id == p.band_id) ||
           (t.card_type != 0 && t.card_type == p.card_type) ||
           (t.character_id > 0 && t.character_id == p.character_id) ||
           (t.tag_id > 0 &&
            std::find(p.tag_ids.begin(), p.tag_ids.end(), t.tag_id) != p.tag_ids.end()) ||
           has_cat(t.live_skill_categories, p.live_skill_categories) ||
           has_cat(t.gekisou_skill_categories, p.gekisou_skill_categories) ||
           (t.gekisou_mission_type != 0 && t.gekisou_mission_type == p.gekisou_mission_type);
}

class Factory {
public:
    Factory(const EngineMaster &master, const std::vector<Performer> &deck, int32_t initial_life,
            int32_t skill_target_music_type, int32_t initial_time_ms = 0)
        : master_(master), deck_(deck), initial_life_(initial_life),
          skill_target_music_type_(skill_target_music_type), initial_time_ms_(initial_time_ms) {}

    std::shared_ptr<Checker> group(int32_t gid, size_t k) const {
        if (!gid)
            return nullptr;
        auto it = master_.condition_sets.find(gid);
        if (it == master_.condition_sets.end())
            return nullptr;
        std::vector<std::shared_ptr<Checker>> ors;
        for (const auto &set : it->second) {
            std::vector<std::shared_ptr<Checker>> items;
            for (int32_t cid : set) {
                auto ch = one(cid, k);
                if (ch)
                    items.push_back(std::move(ch));
            }
            if (items.size() == 1)
                ors.push_back(std::move(items[0]));
            else if (items.size() > 1)
                ors.push_back(std::make_shared<AndChecker>(std::move(items)));
        }
        if (ors.empty())
            return nullptr;
        if (ors.size() == 1)
            return ors[0];
        return std::make_shared<OrChecker>(std::move(ors));
    }

    std::shared_ptr<Cumulative> cumulative(int32_t cid, size_t k) const {
        if (!cid)
            return nullptr;
        const auto &c = master_.cumulative.at(cid);
        int32_t max_c = c.cap < 1 ? kI32Max : c.cap;
        int32_t n = c.values.empty() ? 0 : c.values[0];
        switch (c.type) {
        case 7000:
        case 7001:
            return std::make_shared<ComboPerN>(n, max_c, c.type == 7000);
        case 2000:
        case 2001: {
            int32_t init = (c.type == 2000) ? floor_to_i32(f32(f32(initial_life_) / f32(n))) : 0;
            return std::make_shared<LifePerN>(init, n, max_c, c.type == 2001);
        }
        case 1000:
        case 1001:
        case 1002: {
            std::vector<int32_t> targets;
            for (int32_t id : c.target_ids) {
                int32_t j = master_.targets.at(id).judgement;
                if (j != -1)
                    targets.push_back(j);
            }
            return std::make_shared<JudgementPerN>(n, std::move(targets), max_c, c.type);
        }
        case 3000:
        case 3001:
        case 3002:
        case 3003:
        case 3004:
        case 3005: {
            int32_t count = 0;
            if (c.type <= 3001) {
                for (size_t idx = 0; idx < deck_.size(); ++idx) {
                    if (c.type == 3001 && idx == k)
                        continue;
                    for (int32_t tid : c.target_ids) {
                        if (performer_matches(deck_[idx], master_.targets.at(tid))) {
                            ++count;
                            break;
                        }
                    }
                }
            } else if (c.type <= 3003) {
                int32_t band = deck_.at(k).band_id;
                if (c.type == 3002) {
                    for (const auto &p : deck_)
                        if (p.band_id == band)
                            ++count;
                } else {
                    std::unordered_set<int32_t> bs;
                    for (const auto &p : deck_)
                        if (p.band_id != band)
                            bs.insert(p.band_id);
                    count = static_cast<int32_t>(bs.size());
                }
            } else {
                std::unordered_set<int32_t> vals;
                for (const auto &p : deck_)
                    vals.insert(c.type == 3004 ? p.band_id : p.card_type);
                count = static_cast<int32_t>(vals.size());
            }
            return std::make_shared<CumulativeFixed>(std::min(count, max_c));
        }
        case 6000:
            return std::make_shared<ElapsedCount>(wrap_mul(n, 1000), initial_time_ms_, max_c);
        default:
            throw std::runtime_error("未知的累积条件类型: " + std::to_string(c.type));
        }
    }

private:
    std::shared_ptr<Checker> one(int32_t cid, size_t k) const {
        const auto &c = master_.conditions.at(cid);
        int32_t v0 = c.values.empty() ? 0 : c.values[0];
        auto missions = [&]() {
            std::vector<int32_t> ms;
            for (int32_t id : c.target_ids) {
                const auto &t = master_.targets.at(id);
                if (t.skill_target_type == 5 && t.gekisou_mission_type != 0)
                    ms.push_back(t.gekisou_mission_type);
            }
            return ms;
        };
        std::shared_ptr<Checker> ch;
        switch (c.type) {
        case 0:
            return nullptr;
        case 2000:
        case 2001:
        case 2002:
        case 2003:
            ch = std::make_shared<LifeChecker>(c.type, v0);
            break;
        case 2004:
            ch = std::make_shared<LifeChangedChecker>(initial_life_);
            break;
        case 4007:
            ch = std::make_shared<LifePercentChecker>(
                ceil_to_i32(f32(f32(f32(v0) / 10.0f) * f32(initial_life_))));
            break;
        case 4008:
        case 4009:
            ch = std::make_shared<LifeDeltaChecker>(v0, c.type == 4008, initial_life_);
            break;
        case 3000:
        case 3001: {
            size_t count = 0;
            for (const auto &member : deck_) {
                for (int32_t id : c.target_ids) {
                    if (performer_matches(member, master_.targets.at(id))) {
                        ++count;
                        break;
                    }
                }
            }
            ch = std::make_shared<FixedChecker>(c.type == 3000 ? (count > 0)
                                                               : (count == deck_.size()));
            break;
        }
        case 4001:
            ch = std::make_shared<LiveComboMultipleChecker>(v0);
            break;
        case 4002:
            ch = std::make_shared<LiveComboAtLeastChecker>(v0);
            break;
        case 4000:
            ch = std::make_shared<ElapsedTimeChecker>(wrap_mul(v0, 1000), initial_time_ms_);
            break;
        case 4010:
        case 5020:
            ch = std::make_shared<SameMemberLiveSkillChecker>(static_cast<int32_t>(k));
            break;
        case 5021:
            ch = std::make_shared<SnapGekisouStartChecker>();
            break;
        case 4012: {
            bool ok = false;
            for (int32_t id : c.target_ids) {
                if (master_.targets.at(id).live_music_type == skill_target_music_type_) {
                    ok = true;
                    break;
                }
            }
            ch = std::make_shared<FixedChecker>(ok);
            break;
        }
        case 4011:
            ch = std::make_shared<ProbabilityChecker>(f32(f32(v0) / 100.0f));
            break;
        case 5000: {
            bool ok = false;
            if (k < deck_.size()) {
                for (int32_t id : c.target_ids) {
                    if (performer_matches(deck_[k], master_.targets.at(id))) {
                        ok = true;
                        break;
                    }
                }
            }
            ch = std::make_shared<FixedChecker>(ok);
            break;
        }
        case 8000:
            ch = std::make_shared<FixedChecker>(false);
            break;
        case 1000:
        case 1010:
        case 1020:
        case 1030:
        case 1040: {
            std::vector<int32_t> judgements;
            for (int32_t id : c.target_ids) {
                int32_t j = master_.targets.at(id).judgement;
                if (j != -1)
                    judgements.push_back(j);
            }
            if (c.type == 1030 || c.type == 1040)
                ch = std::make_shared<NoteJudgementCountChecker>(v0, c.type == 1040,
                                                                 std::move(judgements));
            else
                ch = std::make_shared<NoteJudgementMatchChecker>(c.type, std::move(judgements));
            break;
        }
        case 7000:
            ch = std::make_shared<LuckLotResultChecker>(v0);
            break;
        case 7001:
        case 7002:
        case 7004:
            ch = std::make_shared<GkIntervalChecker>(c.type, v0);
            break;
        case 7003:
            ch = std::make_shared<JustAtLeastChecker>(v0);
            break;
        case 7006:
        case 7007:
            ch = std::make_shared<GkJustEdgeChecker>(c.type == 7007, v0);
            break;
        case 7011:
            ch = std::make_shared<GkOnceChecker>([](CheckCtx &ctx) { return ctx.gk.has_value(); });
            break;
        case 7012: {
            int32_t threshold = c.values.empty() ? 1 : v0;
            ch = std::make_shared<GkOnceChecker>([threshold](CheckCtx &ctx) {
                return ctx.prev_confirmed_rank.has_value() && *ctx.prev_confirmed_rank > 0 &&
                       *ctx.prev_confirmed_rank <= threshold;
            });
            break;
        }
        case 7005:
            ch = std::make_shared<ComboAtLeastChecker>(v0);
            break;
        case 7010:
            ch = std::make_shared<RangeStartChecker>(missions());
            break;
        case 7013:
            ch = std::make_shared<RangeCompleteChecker>();
            break;
        case 7020:
            ch = std::make_shared<RangePlayingChecker>(missions());
            break;
        case 7021:
            ch = std::make_shared<LuckRushPlayingChecker>();
            break;
        default:
            throw std::runtime_error("不支持的技能条件类型: " + std::to_string(c.type));
        }
        return c.positive ? ch : std::make_shared<NotChecker>(std::move(ch));
    }

    const EngineMaster &master_;
    const std::vector<Performer> &deck_;
    int32_t initial_life_;
    int32_t skill_target_music_type_;
    int32_t initial_time_ms_;
};

struct EffectState {
    int32_t state = kStay;
    int32_t execute_ms = -1;
    int32_t finish_ms = -1;
    float extended_ms = 0.0f;
    int32_t cumulative_count = 0;
    int32_t cumulative_unit = 0;
    int32_t cumulative_max = 0;
};

struct FrameInput {
    int32_t time_ms = 0;
    int32_t music_length_ms = 0;
    bool finished = false;
};

struct Trigger {
    bool hit = false;
    int32_t time_ms = 0;
};

bool effect_update(EffectState &st, float act, const FrameInput &inp, const Trigger &trigger,
                   Checker *condition, Checker *release, bool finish_frame, CheckCtx &ctx) {
    switch (st.state) {
    case kStay: {
        bool ok = condition ? condition->check(ctx).first : true;
        if (ok && trigger.hit) {
            st.state = kExecuteFrame;
            st.execute_ms = trigger.time_ms;
        }
        return ok && trigger.hit;
    }
    case kExecuteFrame: {
        int32_t t = inp.time_ms;
        if (finish_frame) {
            st.state = kEndFrame;
            st.finish_ms = t;
        } else if (release) {
            st.state = kExecuting;
        } else {
            float dur = f32(f32(act * 1000.0f) + st.extended_ms);
            float elapsed = f32(wrap_sub(t, st.execute_ms));
            if (dur > elapsed) {
                st.state = kExecuting;
            } else {
                st.state = kEndFrame;
                st.finish_ms = t;
            }
        }
        return true;
    }
    case kExecuting: {
        bool released = release ? release->check(ctx).first : false;
        float dur = f32(f32(act * 1000.0f) + st.extended_ms);
        float elapsed = f32(wrap_sub(inp.time_ms, st.execute_ms));
        bool by_time = (dur < elapsed && act > 0.0f);
        if (!(released || finish_frame || by_time))
            return false;
        st.state = kEndFrame;
        int32_t finish =
            (!released && !finish_frame) ? wrap_add(st.execute_ms, ceil_to_i32(dur)) : inp.time_ms;
        if (inp.music_length_ms > 0 && inp.music_length_ms <= finish)
            finish = inp.music_length_ms;
        st.finish_ms = finish;
        return true;
    }
    case kEndFrame:
        st.state = kStay;
        return true;
    default:
        return false;
    }
}

inline bool has_activation_time(float act) {
    if (std::isnan(act) || act <= 0.0f)
        return false;
    float big = f32(kI32Max);
    float m = std::max(std::abs(act), big);
    float tol = std::max(f32(m * f32(1e-6)), f32(1.401298464324817e-45 * 8.0));
    return tol <= std::abs(f32(big - act));
}

struct CondEffect {
    int32_t effect_id = 0;
    int32_t trigger_type = 0;
    float act = 0.0f;
    int32_t phase = 1;
    std::shared_ptr<Checker> trigger;
    std::shared_ptr<Checker> condition;
    int32_t execute_limit = 0;
    std::shared_ptr<Checker> reset;
    size_t row = 0;
    std::function<std::shared_ptr<Cumulative>()> cumulative;
};

struct EffectUpdater {
    EffectState state;
    size_t effect = 0;
    int32_t index = 0;
    std::shared_ptr<Checker> release;
    int32_t phase = 1;
    std::shared_ptr<Cumulative> cumulative;

    EffectUpdater(size_t ef, int32_t idx, std::shared_ptr<Checker> rel, int32_t ph,
                  std::shared_ptr<Cumulative> cum)
        : effect(ef), index(idx), release(std::move(rel)), phase(ph), cumulative(std::move(cum)) {
        if (cumulative) {
            state.cumulative_count = cumulative->init_count();
            state.cumulative_unit = cumulative->unit();
            state.cumulative_max = cumulative->max_val();
        }
    }

    bool update(float act, const FrameInput &inp, const Trigger &trigger, bool finish_frame,
                CheckCtx &ctx) {
        int32_t s0 = state.state;
        bool r = effect_update(state, act, inp, trigger, nullptr, release.get(), finish_frame, ctx);
        if (cumulative) {
            int32_t s1 = state.state;
            if ((s0 == kStay && s1 == kExecuteFrame) || s0 == kExecuteFrame ||
                (s0 == kExecuting && s1 == kExecuting))
                state.cumulative_count = cumulative->update_count(ctx);
            else if (s0 == kEndFrame)
                cumulative->reset();
        }
        return r;
    }
};

inline bool mission_gate_open(std::optional<int32_t> gate, const CheckCtx &ctx) {
    if (!gate.has_value() || *gate == kMAll)
        return true;
    if (!ctx.gk)
        return false;
    const auto *c = ctx.gk->ctrl;
    for (int32_t i : c->state_updates) {
        if (c->ranges[i].mission == *gate)
            return true;
    }
    int32_t i = c->current_playing_index;
    return i >= 0 && c->ranges[i].mission == *gate;
}

class ConditionSkillUpdater {
public:
    std::vector<CondEffect> effects;
    std::vector<EffectUpdater> updaters;
    std::optional<int32_t> gate;

    ConditionSkillUpdater(std::vector<CondEffect> effects_in,
                          const std::function<std::shared_ptr<Checker>(size_t)> &release_fn,
                          std::optional<int32_t> gate_in)
        : effects(std::move(effects_in)), gate(gate_in) {
        sorted_.resize(effects.size());
        std::iota(sorted_.begin(), sorted_.end(), 0);
        std::sort(sorted_.begin(), sorted_.end(),
                  [&](size_t a, size_t b) { return effects[a].effect_id < effects[b].effect_id; });
        stacks_.resize(effects.size());
        sustained_.resize(effects.size());
        for (size_t e = 0; e < effects.size(); ++e) {
            const auto &ef = effects[e];
            if (ef.trigger_type != kOneShot && ef.trigger_type != kSustained)
                continue;
            std::vector<size_t> pool;
            for (int32_t idx = 0; idx < 5; ++idx) {
                pool.push_back(updaters.size());
                updaters.emplace_back(e, idx, release_fn(e), ef.phase,
                                      ef.cumulative ? ef.cumulative() : nullptr);
            }
            if (ef.trigger_type == kOneShot) {
                stacks_[e] = std::move(pool);
            } else {
                Sustained s;
                s.present = true;
                s.queue = std::move(pool);
                s.timed = has_activation_time(ef.act);
                sustained_[e] = std::move(s);
            }
        }
        cache_.resize(effects.size());
        if (!effects.empty()) {
            std::vector<int32_t> idle;
            bool ok = true;
            for (const auto &ef : effects) {
                auto m = ef.trigger ? ef.trigger->idle_member() : std::nullopt;
                if (ef.trigger_type != kOneShot || ef.reset != nullptr || !m.has_value()) {
                    ok = false;
                    break;
                }
                if (*m >= 0 && std::find(idle.begin(), idle.end(), *m) == idle.end())
                    idle.push_back(*m);
            }
            if (ok)
                idle_members_ = std::move(idle);
        }
    }

    void begin_frame() {
        trigger_checked_ = false;
        quiet_frame_ = false;
    }

    std::vector<size_t> update(int32_t phase, const FrameInput &inp, CheckCtx &ctx) {
        if (!trigger_checked_ && executing_.empty()) {
            bool idle_skip = false;
            if (idle_members_.has_value()) {
                bool any_ev = false;
                if (ctx.events) {
                    for (const auto &[m, _] : *ctx.events) {
                        if (std::find(idle_members_->begin(), idle_members_->end(), m) !=
                            idle_members_->end()) {
                            any_ev = true;
                            break;
                        }
                    }
                }
                if (!any_ev)
                    idle_skip = true;
            }
            if (idle_skip || inp.finished || !mission_gate_open(gate, ctx)) {
                trigger_checked_ = true;
                quiet_frame_ = true;
            }
        }
        if (quiet_frame_)
            return {};
        std::vector<size_t> updated;
        std::vector<size_t> done;
        auto exec_copy = executing_;
        for (size_t u : exec_copy) {
            if (phase < 1 || updaters[u].phase == phase) {
                update_one(u, inp, {false, 0}, false, ctx);
                if (updaters[u].state.state == kStay)
                    done.push_back(u);
                else
                    updated.push_back(u);
            }
        }
        if (!trigger_checked_) {
            trigger_checked_ = true;
            std::fill(cache_.begin(), cache_.end(), std::nullopt);
            if (inp.finished || !mission_gate_open(gate, ctx)) {
                finish(done);
                return updated;
            }
            for (const auto &ef : effects) {
                if (ef.reset && ef.reset->check(ctx).first)
                    execute_count_.erase(ef.effect_id);
            }
            for (size_t e : sorted_) {
                auto &c = effects[e].trigger;
                if (!c) {
                    cache_[e] = Trigger{false, 0};
                } else {
                    bool hit = c->check(ctx).first;
                    cache_[e] = Trigger{hit, c->override_time().value_or(inp.time_ms)};
                }
            }
        }
        for (size_t e : sorted_) {
            if (!cache_[e].has_value())
                continue;
            Trigger tr = *cache_[e];
            auto &ef = effects[e];
            if (ef.trigger_type == kSustained) {
                if (phase < 1 || ef.phase == phase)
                    update_sustained(e, inp, tr, ctx, updated);
                continue;
            }
            if (!tr.hit)
                continue;
            auto &stack = stacks_[e];
            if (stack.empty())
                continue;
            size_t top = stack.back();
            if (phase >= 1 && updaters[top].phase != phase)
                continue;
            auto ec_it = execute_count_.find(ef.effect_id);
            if (ef.execute_limit > 0 && ec_it != execute_count_.end() &&
                ef.execute_limit <= ec_it->second)
                continue;
            if (ef.condition && !ef.condition->check(ctx).first)
                continue;
            size_t u = stack.back();
            stack.pop_back();
            update_one(u, inp, {true, tr.time_ms}, false, ctx);
            executing_.push_back(u);
            updated.push_back(u);
            if (ef.execute_limit > 0)
                execute_count_[ef.effect_id]++;
        }
        finish(done);
        return updated;
    }

private:
    struct Sustained {
        bool present = false;
        std::vector<size_t> queue;
        std::optional<size_t> current;
        bool enabled = false;
        bool timed = false;
        std::vector<size_t> executing;
    };

    bool update_one(size_t u, const FrameInput &inp, const Trigger &trigger, bool finish_frame,
                    CheckCtx &ctx) {
        auto &upd = updaters[u];
        return upd.update(effects[upd.effect].act, inp, trigger, finish_frame, ctx);
    }

    void finish(const std::vector<size_t> &done) {
        for (size_t u : done) {
            stacks_[updaters[u].effect].push_back(u);
            auto it = std::find(executing_.begin(), executing_.end(), u);
            if (it != executing_.end())
                executing_.erase(it);
        }
    }

    void update_sustained(size_t e, const FrameInput &inp, const Trigger &tr, CheckCtx &ctx,
                          std::vector<size_t> &updated) {
        auto &s = sustained_[e];
        if (s.timed) {
            timed_step(s, e, inp, tr, ctx, updated);
        } else {
            auto u = step(s, e, tr.hit ? tr.time_ms : inp.time_ms, inp, tr, ctx);
            if (u.has_value())
                updated.push_back(*u);
        }
    }

    void timed_step(Sustained &s, size_t e, const FrameInput &inp, const Trigger &tr, CheckCtx &ctx,
                    std::vector<size_t> &updated) {
        if (tr.hit) {
            auto &condition = effects[e].condition;
            if (!s.enabled) {
                if (condition)
                    condition->reset(ctx);
                s.enabled = true;
            }
            std::optional<size_t> fresh;
            if (!condition || condition->check(ctx).first) {
                if (condition)
                    condition->reset(ctx);
                size_t u = s.queue.front();
                s.queue.erase(s.queue.begin());
                s.executing.push_back(u);
                fresh = u;
            }
            std::vector<size_t> done;
            for (size_t u : s.executing) {
                bool is_fresh = (fresh.has_value() && *fresh == u);
                Trigger trigger{is_fresh, is_fresh ? tr.time_ms : inp.time_ms};
                if (update_one(u, inp, trigger, false, ctx)) {
                    updated.push_back(u);
                    if (updaters[u].state.state == kStay)
                        done.push_back(u);
                }
            }
            for (size_t u : done) {
                s.queue.push_back(u);
                s.executing.erase(std::remove(s.executing.begin(), s.executing.end(), u),
                                  s.executing.end());
            }
        } else {
            for (size_t u : s.executing) {
                update_one(u, inp, {false, 0}, true, ctx);
                updated.push_back(u);
                s.queue.push_back(u);
            }
            s.executing.clear();
            s.enabled = false;
        }
    }

    std::optional<size_t> step(Sustained &s, size_t e, int32_t execute_ms, const FrameInput &inp,
                               const Trigger &tr, CheckCtx &ctx) {
        auto cur = s.current;
        if (cur.has_value() && updaters[*cur].state.state == kEndFrame) {
            update_one(*cur, inp, {false, 0}, false, ctx);
            s.queue.push_back(*cur);
            s.current.reset();
            cur.reset();
        }
        if (cur.has_value() && updaters[*cur].state.state == kExecuteFrame)
            updaters[*cur].state.state = kExecuting;
        auto &condition = effects[e].condition;
        if (tr.hit) {
            if (!s.enabled) {
                if (condition)
                    condition->reset(ctx);
                s.enabled = true;
            }
            if (condition && !condition->check(ctx).first)
                return std::nullopt;
            if (!cur.has_value()) {
                size_t c = s.queue.front();
                s.queue.erase(s.queue.begin());
                s.current = c;
                if (update_one(c, inp, {true, execute_ms}, false, ctx))
                    return c;
            }
        } else {
            if (cur.has_value()) {
                update_one(*cur, inp, {false, 0}, true, ctx);
                s.enabled = false;
                return *cur;
            }
            s.enabled = false;
        }
        return std::nullopt;
    }

    std::vector<size_t> sorted_;
    std::vector<std::vector<size_t>> stacks_;
    std::vector<Sustained> sustained_;
    std::vector<size_t> executing_;
    std::unordered_map<int32_t, int32_t> execute_count_;
    bool trigger_checked_ = false;
    std::vector<std::optional<Trigger>> cache_;
    std::optional<std::vector<int32_t>> idle_members_;
    bool quiet_frame_ = false;
};
// 整曲逐帧仿真模型 (LiveModel) 与幸运期望递推 (luck_curve)
struct LiveNote {
    int32_t id = 0;
    int32_t time_ms = 0;
    int32_t op = 0;
    int32_t judgement_type = 0;
};

struct JudgedNote {
    int32_t note_id = 0;
    int32_t judgement = 0;
    int32_t time_ms = 0;
};

struct PlayFrame {
    int32_t time_ms = 0;
    std::vector<JudgedNote> judged;
};

struct LiveParams {
    int32_t skill_target_music_type = 0;
    double power = 1.0;
    int32_t level = 26;
    int32_t converted_count = 1;
    int32_t music_length_ms = 1000;
    float assist = 1.0f;
};

struct LuckAction {
    size_t frame = 0;
    int32_t range = 0;
    int32_t type = 0;
    int32_t value = 0;
    int32_t limit = 0;
    double chance = 0.0;
};

struct LuckFrame {
    std::vector<int32_t> states;
    std::vector<int32_t> updates;
    int32_t playing = -1;
};

struct LuckNoteRec {
    size_t frame = 0;
    int32_t range = 0;
    int32_t op = 0;
    int32_t judgement = 0;
    int32_t time_ms = 0;
    float speed = 0.0f;
    int32_t buff = 0;
};

struct LuckRecord {
    std::vector<LuckAction> actions;
    std::vector<LuckFrame> frames;
    std::vector<LuckNoteRec> notes;
};

struct GekisouSetup {
    std::vector<std::pair<int32_t, int32_t>> fevers;
    std::vector<int32_t> missions;
};

inline int32_t convert_score_type(int32_t j) {
    constexpr std::array<int32_t, 9> kMap{0, 0, 6, 5, 4, 3, 2, 1, 0};
    size_t idx = static_cast<size_t>(j + 1);
    return idx < kMap.size() ? kMap[idx] : 0;
}

inline bool approximately(float a, float b) {
    float tol = std::max(f32(f32(1e-6) * std::max(std::abs(a), std::abs(b))),
                         f32(1.401298464324817e-45 * 8.0));
    return std::abs(f32(b - a)) < tol;
}

inline int32_t minimum_result_of(int32_t v) {
    return (v >= 2 && v <= 4) ? (v - 1) : 0;
}

inline int32_t gekisou_mission_group(int32_t mission) {
    if (mission >= 1 && mission <= 3)
        return mission;
    if (mission == 4)
        return 1;
    throw std::runtime_error("未知的激奏任务类型: " + std::to_string(mission));
}

inline bool is_gekisou_applier_type(int32_t type) {
    switch (type) {
    case 11000:
    case 11001:
    case 11002:
    case 11003:
    case 11004:
    case 11005:
    case 12000:
    case 12002:
    case 12003:
    case 12004:
    case 13000:
    case 13002:
    case 13003:
    case 13004:
        return true;
    default:
        return false;
    }
}

class ScoreCtl : public LuckHandle {
public:
    explicit ScoreCtl(IncrementalScore &score) : score_(score) {}

    int32_t put(const FactorCommand &cmd) {
        counter_ = wrap_add(counter_, 1);
        cmds_[counter_] = cmd;
        score_.add_factor(cmd);
        return counter_;
    }

    int32_t add_note_score_up(int32_t owner, int32_t t, float factor) {
        return put({t, owner, note_factor_mill(factor), 0, 0, 0, 0});
    }

    void disable_note_score_up(int32_t t, int32_t id) {
        auto c = take(id);
        score_.add_factor({t, c.owner, wrap_sub(0, c.note), 0, 0, 0, 0});
    }

    int32_t add_combo_bonus(int32_t owner, int32_t t, float factor) {
        return put({t, owner, 0, judgement_factor_mill(factor), 0, 0, 0});
    }

    void disable_combo_bonus(int32_t t, int32_t id) {
        auto c = take(id);
        score_.add_factor({t, c.owner, 0, wrap_sub(0, c.combo), 0, 0, 0});
    }

    float combo_bonus_factor(int32_t id) const {
        auto it = cmds_.find(id);
        return (it != cmds_.end() && it->second.combo != 0) ? f32(f32(it->second.combo) / 100000.0f)
                                                            : 0.0f;
    }

    float score_up_factor(int32_t id) const {
        auto it = cmds_.find(id);
        return (it != cmds_.end() && it->second.note != 0) ? f32(f32(it->second.note) / 100000.0f)
                                                           : 0.0f;
    }

    int32_t add(int32_t t, int32_t percent) override {
        return put({t, -1, 0, 0, 0, 0, percent});
    }

    void disable(int32_t t, int32_t id) override {
        auto c = take(id);
        score_.add_factor({t, -1, 0, 0, 0, 0, wrap_sub(0, c.luck)});
    }

private:
    FactorCommand take(int32_t id) {
        auto it = cmds_.find(id);
        FactorCommand c = it->second;
        cmds_.erase(it);
        return c;
    }

    IncrementalScore &score_;
    int32_t counter_ = 0;
    std::unordered_map<int32_t, FactorCommand> cmds_;
};

struct GkAppliers {
    std::unordered_map<std::string, int32_t> ids;
    std::unordered_set<std::string> exhausted;
    std::unordered_map<std::string, int32_t> limit_finished;

    void add(const std::string &key, int32_t id) {
        ids[key] = id;
    }
    int32_t pop(const std::string &key) {
        auto it = ids.find(key);
        int32_t id = it->second;
        ids.erase(it);
        return id;
    }
    void limit(const std::string &key, EffectState &st) {
        auto it = limit_finished.find(key);
        if (it != limit_finished.end()) {
            int32_t t = it->second;
            limit_finished.erase(it);
            st.state = kEndFrame;
            st.finish_ms = t;
        }
    }
};

class LiveModel {
public:
    LifeController life;
    IncrementalScore score;

    LiveModel(const EngineMaster &master, const std::vector<Performer> &deck,
              const std::vector<LiveNote> &notes,
              const std::vector<std::pair<int32_t, int32_t>> &events, const LiveParams &params,
              const GekisouSetup *setup, int32_t seed = 0, LuckRecord *record = nullptr)
        : life(master.live.life_base, master.live.damage,
               notes.empty() ? 1000
                             : (std::max_element(notes.begin(), notes.end(),
                                                 [](const LiveNote &a, const LiveNote &b) {
                                                     return a.time_ms < b.time_ms;
                                                 })
                                    ->time_ms +
                                1000)),
          score(ScoreCalculator(master.live, params.power, params.level, params.converted_count,
                                params.assist),
                params.music_length_ms),
          events_(events), fired_(events.size(), false), random_(seed), conversion_([&]() {
              std::unordered_set<int32_t> nj;
              for (int32_t t : master.live.timing_types) {
                  if (!master.live.just_types.count(t))
                      nj.insert(t);
              }
              return nj;
          }()),
          params_(params), scorectl_(score), record_(record) {
        for (const auto &n : notes)
            notes_[n.id] = n;
        const auto &live = master.live;
        if (setup) {
            size_t n = std::min<size_t>(setup->fevers.size(), 3);
            std::vector<std::array<int32_t, 3>> range_defs;
            for (size_t i = 0; i < n; ++i) {
                range_defs.push_back(
                    {setup->fevers[i].first, setup->fevers[i].second, setup->missions.at(i)});
            }
            std::vector<std::pair<int32_t, int32_t>> note_pairs;
            note_pairs.reserve(notes.size());
            for (const auto &nt : notes)
                note_pairs.emplace_back(nt.id, nt.time_ms);
            gk_ = std::make_unique<GekisouLive>(
                Controller(range_defs, note_pairs, live.gekiso, live.gekiso.rush_percent,
                           live.after_ms),
                FeverUpdater(setup->fevers),
                ranking_factors(live.gekiso, mission_pattern(setup->missions[0], setup->missions[1],
                                                             setup->missions[2])));
            if (record_)
                gk_->ctrl.weighted = true;
        }

        ctx_.record = (record_ != nullptr);
        ctx_.life = &life;
        ctx_.random = &random_;
        ctx_.judged = &judged_;
        ctx_.events = &frame_events_;
        if (gk_)
            ctx_.gk = GkView{&gk_->ctrl, {}, 0};

        auto phase_of = [&](int32_t type) -> int32_t {
            auto it = live.phases.find(type);
            return it != live.phases.end() ? it->second : 1;
        };
        Factory factory(master, deck, live.life_base, params.skill_target_music_type);
        auto add_effect_row = [&](const SkillEffectRow &r) -> size_t {
            std::vector<int32_t> targets;
            for (int32_t t : r.target_ids)
                targets.push_back(master.targets.at(t).judgement);
            rows_.push_back(
                {r.id, r.type, r.value, r.max_value, r.limit_count, std::move(targets)});
            return rows_.size() - 1;
        };

        for (size_t k = 0; k < deck.size(); ++k) {
            const auto &p = deck[k];
            if (!p.live_skill.has_value())
                continue;
            auto [sid, lv] = *p.live_skill;
            std::vector<SkillEffectRow> rs;
            if (auto it = master.live_skills.find(sid); it != master.live_skills.end()) {
                for (const auto &r : it->second)
                    if (r.level == lv)
                        rs.push_back(r);
            }
            std::sort(rs.begin(), rs.end(),
                      [](const SkillEffectRow &a, const SkillEffectRow &b) { return a.id < b.id; });
            std::vector<size_t> row_indexes;
            for (const auto &r : rs)
                row_indexes.push_back(add_effect_row(r));
            int32_t key = sid * 1000 + 100 + static_cast<int32_t>(k);
            std::vector<size_t> pool;
            for (int32_t index = 0; index < 5; ++index) {
                pool.push_back(live_.size());
                std::vector<LiveEffect> effs;
                for (size_t i = 0; i < rs.size(); ++i) {
                    const auto &r = rs[i];
                    auto cum = factory.cumulative(r.cumulative_id, k);
                    EffectState st;
                    if (cum) {
                        st.cumulative_count = cum->init_count();
                        st.cumulative_unit = cum->unit();
                        st.cumulative_max = cum->max_val();
                    }
                    effs.push_back({row_indexes[i], f32(r.seconds), phase_of(r.type), st,
                                    factory.group(r.condition_group, k),
                                    factory.group(r.release_group, k), cum});
                }
                live_.push_back(
                    {key, static_cast<int32_t>(k), index, kStay, {false, 0}, std::move(effs)});
            }
            live_pools_.push_back({key, static_cast<int32_t>(k), std::move(pool)});
        }
        std::sort(live_pools_.begin(), live_pools_.end(),
                  [](const LivePool &a, const LivePool &b) { return a.key < b.key; });

        auto add_condition_skill = [&](std::vector<SkillEffectRow> rs, size_t k, int32_t skill_type,
                                       std::optional<int32_t> gate) {
            std::sort(rs.begin(), rs.end(),
                      [](const SkillEffectRow &a, const SkillEffectRow &b) { return a.id < b.id; });
            std::vector<CondEffect> effs;
            std::vector<int32_t> releases;
            for (const auto &r : rs) {
                int32_t cum_id = r.cumulative_id;
                effs.push_back({
                    r.id * 100 + skill_type * 10 + static_cast<int32_t>(k),
                    r.trigger_type,
                    f32(r.seconds),
                    phase_of(r.type),
                    factory.group(r.trigger_group, k),
                    factory.group(r.condition_group, k),
                    r.execute_limit,
                    r.execute_limit > 0 ? factory.group(r.reset_group, k) : nullptr,
                    add_effect_row(r),
                    cum_id ? std::function<std::shared_ptr<Cumulative>()>([&master, &deck, &live,
                                                                           &params, cum_id, k]() {
                        Factory f(master, deck, live.life_base, params.skill_target_music_type);
                        return f.cumulative(cum_id, k);
                    })
                           : nullptr,
                });
                releases.push_back(r.release_group);
            }
            ConditionSkillUpdater updater(
                std::move(effs), [&](size_t e) { return factory.group(releases[e], k); }, gate);
            cond_.push_back({static_cast<int32_t>(k), skill_type, std::move(updater)});
        };

        for (size_t k = 0; k < deck.size(); ++k) {
            for (const auto &[sid, lv] : deck[k].support_skills) {
                std::vector<SkillEffectRow> rs;
                if (auto it = master.support_skills.find(sid); it != master.support_skills.end()) {
                    for (const auto &r : it->second)
                        if (r.level == lv)
                            rs.push_back(r);
                }
                add_condition_skill(std::move(rs), k, 3, std::nullopt);
            }
        }
        if (gk_) {
            std::vector<std::optional<int32_t>> mission(deck.size());
            for (size_t k = 0; k < deck.size(); ++k) {
                const auto &p = deck[k];
                if (p.mission_only.has_value() && *p.mission_only != 0) {
                    mission[k] = *p.mission_only;
                } else if (p.gekisou_skill.has_value()) {
                    int32_t mt = master.gekiso_skills.at(p.gekisou_skill->first).mission_type;
                    gekisou_mission_group(mt);
                    mission[k] = mt;
                }
            }
            for (int32_t m = 1; m <= 3; ++m) {
                for (size_t k = 0; k < deck.size(); ++k) {
                    const auto &p = deck[k];
                    if (!p.gekisou_skill.has_value() || !mission[k].has_value() ||
                        gekisou_mission_group(*mission[k]) != m)
                        continue;
                    auto [sid, lv] = *p.gekisou_skill;
                    std::vector<SkillEffectRow> rs;
                    for (const auto &r : master.gekiso_skills.at(sid).effects)
                        if (r.level == lv)
                            rs.push_back(r);
                    add_condition_skill(std::move(rs), k, 4, *mission[k]);
                }
            }
            for (size_t k = 0; k < deck.size(); ++k) {
                if (!mission[k].has_value())
                    continue;
                for (const auto &[sid, lv] : deck[k].gekisou_support_skills) {
                    const auto &row = master.gekiso_support_skills.at(sid);
                    std::vector<SkillEffectRow> rs;
                    for (const auto &r : row.effects)
                        if (r.level == lv)
                            rs.push_back(r);
                    add_condition_skill(std::move(rs), k, 5, row.mission_type);
                }
            }
        }
    }

    void frame(int32_t t, const std::vector<JudgedNote> &judged, float dt) {
        frame_time_ = t;
        auto frame_rank = prev_confirmed_rank_;
        prev_confirmed_rank_.reset();
        if (gk_) {
            GkEnv env{random_, scorectl_};
            gk_->fever_updates = gk_->fever.update(t);
            gk_->ctrl.before_update(dt, t, gk_->fever_updates, score.score, env);
            if (record_) {
                std::vector<int32_t> sts;
                sts.reserve(gk_->ctrl.states.size());
                for (const auto &s : gk_->ctrl.states)
                    sts.push_back(s.state);
                record_->frames.push_back(
                    {std::move(sts), gk_->ctrl.state_updates, gk_->ctrl.current_playing_index});
            }
        }
        frame_events_.clear();
        if (fired_count_ < events_.size()) {
            for (size_t i = 0; i < events_.size(); ++i) {
                auto [index, ev_t] = events_[i];
                if (!fired_[i] && ev_t <= t) {
                    fired_[i] = true;
                    ++fired_count_;
                    frame_events_.emplace_back(index, ev_t);
                }
            }
        }
        judged_.clear();
        results_.clear();
        bool broken = false;
        for (const auto &j : judged) {
            const auto &n = notes_.at(j.note_id);
            int32_t conv = conversion_.convert(j.judgement, n.judgement_type, j.time_ms);
            combo_.add_judgement(n.time_ms, conv);
            results_.emplace_back(n, conv);
            judged_.push_back({n.id, conv, n.time_ms});
            if (conv == 1 || conv == 2)
                broken = true;
        }
        int32_t simulator_combo = combo_.timing_combo(t);
        int32_t added_combo = std::max(0, simulator_combo - simulator_previous_combo_);
        simulator_previous_combo_ = simulator_combo;
        if (broken)
            current_combo_ = 0;
        current_combo_ = wrap_add(current_combo_, added_combo);

        const auto &note_percent = score.calc.settings.note_percent;
        for (const auto &[n, conv] : results_) {
            life.note_damage(n.time_ms, conv);
            int32_t lf = life.life_at(n.time_ms);
            if (note_percent.count(n.op))
                score.add_note(n.time_ms, lf, n.id, n.op, convert_score_type(conv));
        }
        score.calculate(t, combo_, info());

        FrameInput inp{t, params_.music_length_ms, finished_};
        for (size_t si : enabled_live_)
            live_[si].trigger = {false, 0};
        bool started = false;
        if (!frame_events_.empty()) {
            for (auto &pool : live_pools_) {
                std::optional<std::pair<int32_t, int32_t>> ev;
                for (ptrdiff_t i = static_cast<ptrdiff_t>(frame_events_.size()) - 1; i >= 0; --i) {
                    if (frame_events_[static_cast<size_t>(i)].first == pool.member) {
                        ev = frame_events_[static_cast<size_t>(i)];
                        break;
                    }
                }
                if (!ev.has_value())
                    continue;
                size_t si = pool.available.front();
                pool.available.erase(pool.available.begin());
                auto &s = live_[si];
                s.trigger = {true, ev->second};
                for (auto &e : s.effects)
                    e.state.state = kStay;
                enabled_live_.push_back(si);
                started = true;
            }
        }
        if (started && enabled_live_.size() > 1) {
            std::sort(enabled_live_.begin(), enabled_live_.end(), [&](size_t a, size_t b) {
                return (live_[a].key * 10 + live_[a].index) < (live_[b].key * 10 + live_[b].index);
            });
        }
        for (auto &c : cond_)
            c.updater.begin_frame();
        ctx_.frame_time = t;
        ctx_.current_combo = current_combo_;
        ctx_.prev_confirmed_rank = frame_rank;
        if (ctx_.gk) {
            ctx_.gk->prev_lots = gk_->prev_lots;
            ctx_.gk->prev_lot_ms = gk_->prev_lot_ms;
        }
        for (int32_t ph = 1; ph <= 2; ++ph) {
            std::vector<Listed> listed;
            for (size_t si : enabled_live_) {
                auto &s = live_[si];
                if (s.parent_state == kEndFrame) {
                    s.parent_state = kStay;
                    for (auto &e : s.effects)
                        e.state.state = kStay;
                    continue;
                }
                bool changed = false;
                for (auto &e : s.effects) {
                    if (e.phase == ph)
                        changed = update_live_effect(e, inp, s.trigger, ctx_) || changed;
                }
                if (changed)
                    s.parent_state = aggregate_live_state(s.effects);
                for (size_t ei = 0; ei < s.effects.size(); ++ei) {
                    if (s.effects[ei].state.state != kStay && s.effects[ei].phase == ph)
                        listed.push_back({true, si, ei});
                }
            }
            for (size_t ui = 0; ui < cond_.size(); ++ui) {
                auto &updater = cond_[ui].updater;
                for (size_t x : updater.update(ph, inp, ctx_)) {
                    if (updater.updaters[x].state.state != kStay)
                        listed.push_back({false, ui, x});
                }
            }
            for (const auto &item : listed)
                apply(item);
        }
        if (!enabled_live_.empty()) {
            bool stayed = false;
            for (size_t si : enabled_live_) {
                const auto &s = live_[si];
                if (s.parent_state == kStay) {
                    for (auto &p : live_pools_) {
                        if (p.key == s.key && p.member == s.member) {
                            p.available.push_back(si);
                            break;
                        }
                    }
                    stayed = true;
                }
            }
            if (stayed) {
                enabled_live_.erase(
                    std::remove_if(enabled_live_.begin(), enabled_live_.end(),
                                   [&](size_t si) { return live_[si].parent_state == kStay; }),
                    enabled_live_.end());
            }
        }
        life.sync_current(t);
        score.calculate(t, combo_, info());
        if (gk_)
            gekisou_after(t);
    }

private:
    struct EffectRow {
        int32_t id = 0;
        int32_t type = 0;
        int32_t value = 0;
        int32_t max_value = 0;
        int32_t limit_count = 0;
        std::vector<int32_t> targets;
    };

    struct LiveEffect {
        size_t row = 0;
        float act = 0.0f;
        int32_t phase = 1;
        EffectState state;
        std::shared_ptr<Checker> condition;
        std::shared_ptr<Checker> release;
        std::shared_ptr<Cumulative> cumulative;
    };

    struct LiveSkill {
        int32_t key = 0;
        int32_t member = 0;
        int32_t index = 0;
        int32_t parent_state = kStay;
        Trigger trigger;
        std::vector<LiveEffect> effects;
    };

    struct LivePool {
        int32_t key = 0;
        int32_t member = 0;
        std::vector<size_t> available;
    };

    struct CondSkill {
        int32_t member = 0;
        int32_t skill_type = 0;
        ConditionSkillUpdater updater;
    };

    struct Listed {
        bool live = false;
        size_t idx1 = 0; // skill or updater
        size_t idx2 = 0; // effect or u
    };

    struct GekisouLive {
        Controller ctrl;
        FeverUpdater fever;
        std::array<std::array<int32_t, 5>, 3> factors;
        std::vector<std::pair<int32_t, int32_t>> fever_updates;
        std::vector<int32_t> prev_lots;
        int32_t prev_lot_ms = 0;

        GekisouLive(Controller c, FeverUpdater f, std::array<std::array<int32_t, 5>, 3> fac)
            : ctrl(std::move(c)), fever(std::move(f)), factors(fac) {}
    };

    const GekisouComboInfo *info() const {
        return gk_ ? &gk_->ctrl : nullptr;
    }

    static int32_t aggregate_live_state(const std::vector<LiveEffect> &effects) {
        for (int32_t st : {kExecuting, kExecuteFrame, kEndFrame}) {
            for (const auto &e : effects)
                if (e.state.state == st)
                    return st;
        }
        return kStay;
    }

    static bool update_live_effect(LiveEffect &e, const FrameInput &inp, const Trigger &trigger,
                                   CheckCtx &ctx) {
        int32_t before = e.state.state;
        bool changed = effect_update(e.state, e.act, inp, trigger, e.condition.get(),
                                     e.release.get(), false, ctx);
        if (e.cumulative) {
            int32_t after = e.state.state;
            if ((before == kStay && after == kExecuteFrame) || before == kExecuteFrame ||
                (before == kExecuting && after == kExecuting))
                e.state.cumulative_count = e.cumulative->update_count(ctx);
            else if (before == kEndFrame)
                e.cumulative->reset();
        }
        return changed;
    }

    void gekisou_after(int32_t t) {
        auto &gk = *gk_;
        std::vector<GkNote> judged;
        judged.reserve(results_.size());
        for (const auto &[n, j] : results_)
            judged.push_back({n.id, n.op, n.time_ms, j});
        if (record_) {
            size_t frame = record_->frames.size() - 1;
            for (size_t r = 0; r < gk.ctrl.ranges.size(); ++r) {
                const auto &range = gk.ctrl.ranges[r];
                if (range.mission != 2)
                    continue;
                for (const auto &[id, op, time_ms, judgement] : judged) {
                    if (range.targets.count(id) && judgement != 0 && judgement != 7) {
                        record_->notes.push_back({frame, static_cast<int32_t>(r), op, judgement,
                                                  time_ms, gk.ctrl.gauge_speed_at(time_ms),
                                                  gk.ctrl.lot_buff_at(time_ms)});
                    }
                }
            }
        }
        GkEnv env{random_, scorectl_};
        gk.ctrl.update(t, judged, gk.fever_updates, score.score, env);
        gk.prev_lots = gk.ctrl.lot_results;
        gk.prev_lot_ms = gk.prev_lots.empty() ? 0 : t;
        std::optional<int32_t> comp_idx;
        for (int32_t i : gk.ctrl.state_updates) {
            if (gk.ctrl.states[i].state == kSComplete) {
                comp_idx = i;
                break;
            }
        }
        if (!comp_idx.has_value())
            return;
        int32_t idx = *comp_idx;
        const auto &range = gk.ctrl.ranges[idx];
        int32_t s0 = score.calculate(range.start_ms, combo_, &gk.ctrl);
        int32_t s1 = score.calculate(range.end_ms, combo_, &gk.ctrl);
        gk.ctrl.states[idx].start_score = s0;
        gk.ctrl.states[idx].end_score = s1;
        if (static_cast<size_t>(idx) < gk.factors.size()) {
            int32_t percent = gk.factors[idx][0];
            int64_t p = static_cast<int64_t>(gk.ctrl.states[idx].score()) * percent;
            int32_t bonus = static_cast<int32_t>(p >= 0 ? (p / 100) : -((-p) / 100));
            score.add_fixed(range.end_ms, bonus);
            prev_confirmed_rank_ = 1;
        }
    }

    EffectState &state_of(const Listed &item) {
        if (item.live)
            return live_[item.idx1].effects[item.idx2].state;
        return cond_[item.idx1].updater.updaters[item.idx2].state;
    }

    void apply(const Listed &item) {
        size_t ri = 0;
        int32_t k = 0;
        int32_t owner_type = 0;
        int32_t skill_type = 0;
        std::string key;
        if (item.live) {
            const auto &s = live_[item.idx1];
            ri = s.effects[item.idx2].row;
            k = s.member;
            owner_type = 1;
            skill_type = 1;
            key = "L" + std::to_string(rows_[ri].id) + ":" + std::to_string(s.member) + ":" +
                  std::to_string(s.index);
        } else {
            const auto &c = cond_[item.idx1];
            const auto &upd = c.updater.updaters[item.idx2];
            const auto &ef = c.updater.effects[upd.effect];
            ri = ef.row;
            k = c.member;
            owner_type = (c.skill_type == 4) ? 1 : 2;
            skill_type = c.skill_type;
            key = "C" + std::to_string(ef.effect_id) + ":" + std::to_string(upd.index);
        }
        auto &st = state_of(item);
        int32_t owner = wrap_add(wrap_mul(k, 100), owner_type);
        bool gk_skill = (skill_type == 4 || skill_type == 5);
        const auto &row = rows_[ri];
        int32_t type = row.type;
        bool gk_type = is_gekisou_applier_type(type);
        if ((gk_type || type == 13001) && !gk_skill && !gk_)
            return;
        if (record_ && (type == 11003 || type == 11005)) {
            if (st.state == kExecuteFrame && !item.live) {
                const auto &c = cond_[item.idx1];
                const auto &ef = c.updater.effects[c.updater.updaters[item.idx2].effect];
                record_->actions.push_back({record_->frames.size() - 1,
                                            gk_->ctrl.playing_range_index(), type, row.value,
                                            row.limit_count, chance_of(ef.condition, ctx_)});
            }
            return;
        }
        if ((gk_skill || gk_type) && apply_gekisou(row, st, owner, key))
            return;
        int32_t value = row.value;
        switch (type) {
        case 0:
        case 1000:
        case 1001:
        case 1002:
        case 1003:
        case 1500:
        case 1501:
        case 1502:
        case 1503:
            return;
        case 2000:
        case 2005: {
            int32_t m = note_factor_mill(f32(f32(value) / (type == 2005 ? -10000.0f : 10000.0f)));
            if (st.state == kExecuteFrame)
                score.add_factor({st.execute_ms, owner, m, 0, 0, 0, 0});
            else if (st.state == kEndFrame)
                score.add_factor({st.finish_ms, owner, wrap_sub(0, m), 0, 0, 0, 0});
            return;
        }
        case 2001:
        case 2003:
            apply_cumulative_score(row, st, owner, key);
            return;
        case 2002: {
            int32_t m = judgement_factor_mill(f32(f32(value) / 10000.0f));
            if (st.state == kExecuteFrame)
                score.add_factor({st.execute_ms, owner, 0, m, 0, 0, 0});
            else if (st.state == kEndFrame)
                score.add_factor({st.finish_ms, owner, 0, wrap_sub(0, m), 0, 0, 0});
            return;
        }
        case 2004: {
            int32_t m = judgement_factor_mill(f32(f32(value) / 10000.0f));
            for (int32_t j : row.targets) {
                if (st.state == kExecuteFrame)
                    score.add_factor({st.execute_ms, owner, 0, 0, j, m, 0});
                else if (st.state == kEndFrame)
                    score.add_factor({st.finish_ms, owner, 0, 0, j, wrap_sub(0, m), 0});
            }
            return;
        }
        case 15000:
            if (st.state == kExecuteFrame)
                extend(k, f32(value));
            return;
        case 3000:
            if (st.state == kExecuteFrame) {
                limits_[key] = life.add_limit(value);
            } else if (st.state == kEndFrame) {
                auto it = limits_.find(key);
                if (it != limits_.end()) {
                    life.subtract_limit(it->second);
                    limits_.erase(it);
                }
            }
            return;
        case 3002:
            if (st.state == kExecuteFrame)
                life.skill_damage(st.execute_ms, value, true);
            return;
        case 3004:
            if (st.state == kExecuteFrame) {
                reductions_[key] = life.enable_reduction(st.execute_ms, value);
            } else if (st.state == kEndFrame) {
                auto it = reductions_.find(key);
                if (it != reductions_.end()) {
                    life.disable_reduction(st.finish_ms, it->second);
                    reductions_.erase(it);
                }
            }
            return;
        case 3001:
            if (st.state == kExecuteFrame)
                life.recovery(st.execute_ms, value, true);
            return;
        case 3003:
            if (st.state == kExecuteFrame) {
                guards_[key] = life.enable_guard(st.execute_ms);
            } else if (st.state == kEndFrame) {
                auto it = guards_.find(key);
                if (it != guards_.end()) {
                    life.disable_guard(st.finish_ms, it->second);
                    guards_.erase(it);
                }
            }
            return;
        case 12006:
        case 13005: {
            if (type == 13005 && !gk_)
                return;
            auto t = conversion_.update(key, st.state,
                                        {type, value, row.limit_count, row.targets, row.id});
            if (t.has_value()) {
                st.state = kEndFrame;
                st.finish_ms = *t;
            }
            return;
        }
        default:
            throw std::runtime_error("不支持的技能效果类型: " + std::to_string(type));
        }
    }

    void apply_cumulative_score(const EffectRow &row, EffectState &st, int32_t owner,
                                const std::string &key) {
        bool is_combo = (row.type == 2003);
        int32_t val = wrap_mul(row.value, st.cumulative_count);
        if (row.max_value > 0)
            val = std::min(val, row.max_value);
        float factor = f32(f32(val) / 10000.0f);
        auto add_fn = [&](int32_t t) {
            return is_combo ? scorectl_.add_combo_bonus(owner, t, factor)
                            : scorectl_.add_note_score_up(owner, t, factor);
        };
        auto dis_fn = [&](int32_t t, int32_t id) {
            if (is_combo)
                scorectl_.disable_combo_bonus(t, id);
            else
                scorectl_.disable_note_score_up(t, id);
        };
        if (st.state == kExecuteFrame) {
            ga_.add(key, add_fn(st.execute_ms));
        } else if (st.state == kExecuting) {
            int32_t id = ga_.ids.at(key);
            float old = is_combo ? scorectl_.combo_bonus_factor(id) : scorectl_.score_up_factor(id);
            if (!approximately(old, factor)) {
                dis_fn(frame_time_, id);
                ga_.ids[key] = add_fn(frame_time_);
            }
        } else if (st.state == kEndFrame) {
            dis_fn(st.finish_ms, ga_.pop(key));
        }
    }

    bool apply_gekisou(const EffectRow &row, EffectState &st, int32_t owner,
                       const std::string &key) {
        int32_t et = row.type;
        int32_t v = row.value;
        auto &c = gk_->ctrl;
        int32_t s = st.state;
        switch (et) {
        case 12000:
        case 13000:
            if (s == kExecuteFrame) {
                ga_.add(key, et == 12000 ? c.add_combo_bonus(st.execute_ms, f32(v))
                                         : c.add_just_bonus(st.execute_ms, f32(v)));
            } else if (s == kEndFrame) {
                int32_t id = ga_.pop(key);
                if (et == 12000)
                    c.subtract_combo_bonus(st.finish_ms, id);
                else
                    c.subtract_just_bonus(st.finish_ms, id);
            }
            return true;
        case 13002:
            if (s == kEndFrame) {
                auto it = ga_.ids.find(key);
                if (it != ga_.ids.end()) {
                    c.remove_cumulative_rule(it->second);
                    ga_.ids.erase(it);
                }
            } else if (s == kExecuteFrame && !ga_.ids.count(key)) {
                ga_.ids[key] =
                    c.add_cumulative_rule(st.cumulative_unit, v, st.cumulative_max, row.max_value);
            }
            return true;
        case 11000:
        case 11001:
            if (s == kExecuteFrame) {
                float val = f32(f32(v) / 10000.0f);
                ga_.add(key, et == 11000 ? c.add_lot_probability_up(st.execute_ms, val)
                                         : c.add_gauge_up(st.execute_ms, val));
            } else if (s == kEndFrame) {
                int32_t id = ga_.pop(key);
                if (et == 11000)
                    c.subtract_lot_probability_up(st.finish_ms, id);
                else
                    c.subtract_gauge_up(st.finish_ms, id);
            }
            return true;
        case 11002:
        case 11004:
        case 12002:
        case 12003:
        case 13003:
        case 13004:
            if (s == kExecuteFrame) {
                int32_t count = v;
                if (et == 11004 || et == 12003 || et == 13004) {
                    count = wrap_mul(count, st.cumulative_count);
                    if (row.max_value > 0)
                        count = std::min(count, row.max_value);
                }
                if (et == 11002 || et == 11004)
                    c.add_luck_point(count);
                else if (et == 12002 || et == 12003)
                    c.add_gekisou_combo(frame_time_, count);
                else
                    c.add_just_count(frame_time_, count);
            }
            return true;
        case 11003:
            if (s == kExecuteFrame) {
                int32_t g = wrap_mul(c.current_gauge_max(), v);
                c.add_luck_gauge_percent(floor_to_i32(f32(f32(g) / 10000.0f)));
            }
            return true;
        case 12004:
            ga_.limit(key, st);
            if (st.state == kExecuteFrame) {
                int32_t mask = 0;
                for (int32_t j : row.targets)
                    if (j != 0 && j != -1)
                        mask |= (1 << (j & 31));
                ga_.add(key, c.enable_combo_protect(st.execute_ms, row.limit_count, mask & 0xff));
            } else if (st.state == kEndFrame && !ga_.exhausted.erase(key)) {
                auto it = ga_.ids.find(key);
                if (it != ga_.ids.end()) {
                    c.disable_combo_protect(st.finish_ms, it->second);
                    ga_.ids.erase(it);
                }
            }
            return true;
        case 11005:
            ga_.limit(key, st);
            if (st.state == kExecuteFrame) {
                ga_.add(key, c.machine.enable_minimum(minimum_result_of(v), row.limit_count));
            } else if (st.state == kExecuting) {
                auto it = ga_.ids.find(key);
                if (it != ga_.ids.end() && !c.machine.is_minimum_active(it->second)) {
                    int32_t id = it->second;
                    int32_t t = c.machine.take_last_consumed(id);
                    if (t < 0)
                        t = frame_time_;
                    c.machine.disable_minimum(id);
                    ga_.ids.erase(it);
                    ga_.exhausted.insert(key);
                    ga_.limit_finished[key] = t;
                }
            } else if (st.state == kEndFrame && !ga_.exhausted.erase(key)) {
                auto it = ga_.ids.find(key);
                if (it != ga_.ids.end()) {
                    c.machine.disable_minimum(it->second);
                    ga_.ids.erase(it);
                }
            }
            return true;
        case 2001: {
            int32_t n = wrap_mul(v, st.cumulative_count);
            if (row.max_value > 0 && row.max_value < n)
                n = row.max_value;
            float factor = f32(f32(n) / 10000.0f);
            if (s == kExecuteFrame) {
                ga_.add(key, scorectl_.add_note_score_up(owner, st.execute_ms, factor));
            } else if (s == kExecuting) {
                int32_t id = ga_.ids.at(key);
                if (!approximately(scorectl_.score_up_factor(id), factor)) {
                    scorectl_.disable_note_score_up(frame_time_, id);
                    ga_.ids[key] = scorectl_.add_note_score_up(owner, frame_time_, factor);
                }
            } else if (s == kEndFrame) {
                scorectl_.disable_note_score_up(st.finish_ms, ga_.pop(key));
            }
            return true;
        }
        case 4004:
            return true;
        default:
            return false;
        }
    }

    void extend(int32_t member, float ms) {
        for (auto &s : live_) {
            if (s.member != member)
                continue;
            for (auto &e : s.effects) {
                if (e.state.state == kExecuteFrame || e.state.state == kExecuting)
                    e.state.extended_ms = f32(e.state.extended_ms + ms);
            }
        }
    }

    std::unordered_map<int32_t, LiveNote> notes_;
    std::vector<std::pair<int32_t, int32_t>> events_;
    std::vector<bool> fired_;
    LiveRandom random_;
    ComboCounter combo_;
    Conversion conversion_;
    LiveParams params_;
    std::vector<EffectRow> rows_;
    std::vector<LiveSkill> live_;
    std::vector<LivePool> live_pools_;
    std::vector<size_t> enabled_live_;
    std::vector<CondSkill> cond_;
    std::unordered_map<std::string, int32_t> guards_;
    std::unordered_map<std::string, int32_t> reductions_;
    std::unordered_map<std::string, int32_t> limits_;
    std::vector<std::pair<int32_t, int32_t>> frame_events_;
    std::vector<std::array<int32_t, 3>> judged_;
    std::vector<std::pair<LiveNote, int32_t>> results_;
    size_t fired_count_ = 0;
    CheckCtx ctx_;
    int32_t frame_time_ = 0;
    std::optional<int32_t> prev_confirmed_rank_;
    int32_t simulator_previous_combo_ = 0;
    int32_t current_combo_ = 0;
    ScoreCtl scorectl_;
    std::unique_ptr<GekisouLive> gk_;
    GkAppliers ga_;
    LuckRecord *record_ = nullptr;
    bool finished_ = false;
};

struct MissRule {
    int32_t value = 0;
    double chance = 0.0;
};

struct LuckCurve {
    std::unordered_map<int32_t, double> rush;
    std::unordered_map<int32_t, double> skill;
    std::unordered_map<int32_t, double> both;
};

LuckCurve luck_curve(const GekisouTables &tables, const LuckRecord &record,
                     const std::vector<PlayFrame> &frames, const std::vector<int32_t> &luck_ranges,
                     const std::vector<MissRule> &miss_rules) {
    constexpr int32_t kG = 1;
    constexpr int32_t kL = 1 << 8;
    constexpr int32_t kN = 1 << 11;
    constexpr int32_t kR = 1 << 14;
    constexpr int32_t kMin = 1 << 17;
    constexpr int32_t kMissUsed = 1 << 19;
    constexpr int32_t kPrevMiss = 1 << 20;
    constexpr int32_t kFrameLot = 1 << 21;
    constexpr int32_t kFrameMiss = 1 << 22;
    constexpr int32_t kBonus = 1 << 23;
    constexpr int32_t kSkill = 1 << 24;
    constexpr int32_t kSkillPrev = 1 << 25;
    constexpr int32_t kFlags =
        kMissUsed | kPrevMiss | kFrameLot | kFrameMiss | kBonus | kSkill | kSkillPrev;

    auto gauge_of = [](int32_t s) { return s & 0xff; };
    auto lots_of = [](int32_t s) { return (static_cast<uint32_t>(s) >> 8) & 7; };
    auto next_of = [](int32_t s) {
        return static_cast<int32_t>((static_cast<uint32_t>(s) >> 11) & 7) - 1;
    };
    auto rush_of = [](int32_t s) {
        return static_cast<int32_t>((static_cast<uint32_t>(s) >> 14) & 7);
    };
    auto minimum_of = [](int32_t s) {
        return static_cast<int32_t>((static_cast<uint32_t>(s) >> 17) & 3);
    };
    auto flags_of = [](int32_t s) { return s & kFlags; };
    auto pack = [](int32_t gauge, int32_t lots, int32_t next, int32_t rush, int32_t minimum,
                   int32_t flags) -> int32_t {
        return gauge * kG + std::min(lots, 7) * kL + (next + 1) * kN + rush * kR + minimum * kMin +
               flags;
    };

    auto merge_pairs = [](const std::vector<std::pair<double, int32_t>> &pairs) {
        std::vector<std::pair<double, int32_t>> out;
        for (const auto &[p, v] : pairs) {
            auto it = std::find_if(out.begin(), out.end(),
                                   [&](const auto &item) { return item.second == v; });
            if (it != out.end())
                it->first += p;
            else if (p > 0.0)
                out.emplace_back(p, v);
        }
        return out;
    };

    std::vector<LotItem> base_good, base_great, base_perfect, base_hold;
    for (const auto &r : tables.luck_base_points) {
        if (r.weight <= 0)
            continue;
        if (r.category == 0) {
            if (r.judgement == 3)
                base_good.emplace_back(r.weight, r.point);
            else if (r.judgement == 4)
                base_great.emplace_back(r.weight, r.point);
            else if (r.judgement == 5)
                base_perfect.emplace_back(r.weight, r.point);
        } else if (r.category == 1 && r.judgement == 5) {
            base_hold.emplace_back(r.weight, r.point);
        }
    }
    std::array<std::vector<LotItem>, 5> lots;
    for (const auto &r : tables.luck_bonus_lots) {
        if (r.weight > 0 && r.lot_type >= 0 && r.lot_type < 5)
            lots[r.lot_type].emplace_back(r.weight, r.result);
    }
    for (auto &table : lots) {
        std::sort(table.begin(), table.end(),
                  [](const LotItem &a, const LotItem &b) { return b.second < a.second; });
    }

    auto base_points = [&](int32_t op, int32_t j) -> std::vector<std::pair<double, int32_t>> {
        if (is_non_luck_note(op))
            return {{1.0, 0}};
        uint32_t u = static_cast<uint32_t>(j + 1);
        if (u < 9 && ((0x107u >> (u & 31u)) & 1u) != 0)
            return {{1.0, 0}};
        const std::vector<LotItem> *items = is_sub_note(op)                       ? &base_hold
                                            : (static_cast<uint32_t>(j - 5) < 2u) ? &base_perfect
                                            : (j == 4)                            ? &base_great
                                            : (j == 3)                            ? &base_good
                                                                                  : nullptr;
        if (!items || items->empty())
            return {{1.0, 0}};
        double total = 0.0;
        for (const auto &[w, _] : *items)
            total += w;
        std::vector<std::pair<double, int32_t>> raw;
        for (const auto &[w, v] : *items)
            raw.emplace_back(static_cast<double>(w) / total, v);
        return merge_pairs(raw);
    };

    std::unordered_map<std::string, std::vector<std::pair<double, int32_t>>> draw_cache;
    auto draw = [&](int32_t type, int32_t buff_percent,
                    int32_t minimum) -> const std::vector<std::pair<double, int32_t>> & {
        std::string key = std::to_string(type) + "," + std::to_string(buff_percent) + "," +
                          std::to_string(minimum);
        auto it = draw_cache.find(key);
        if (it != draw_cache.end())
            return it->second;
        const auto &items = lots.at(type);
        float buff = f32(f32(buff_percent) / 100.0f);
        int32_t total = 0;
        for (const auto &[w, _] : items)
            total = wrap_add(total, w);
        int32_t per = 0;
        if (minimum >= 1) {
            int32_t included = 0, excluded = 0, n = 0;
            for (const auto &[w, r] : items) {
                int32_t weight = buffed_weight(buff, w);
                if (r >= minimum) {
                    included += weight;
                    ++n;
                } else {
                    excluded += weight;
                }
            }
            per = sdiv(excluded, n);
            total = included + per * n;
        }
        std::vector<std::pair<double, int32_t>> out;
        int32_t lo = 0;
        for (const auto &[w, r] : items) {
            if (minimum >= 1 && r < minimum)
                continue;
            int32_t weight = per + buffed_weight(buff, w);
            int32_t hi = lo + weight;
            int32_t a = std::min(std::max(lo, 0), total);
            int32_t b = std::min(std::max(hi, 0), total);
            if (b > a)
                out.emplace_back(static_cast<double>(b - a) / static_cast<double>(total), r);
            lo = hi;
        }
        return draw_cache.emplace(std::move(key), merge_pairs(out)).first->second;
    };

    LuckCurve curve;
    auto gauge_max_of = [&](int32_t rush) {
        return rush > 0 ? tables.gauge_max_rush : tables.gauge_max;
    };
    auto add_gauge = [&](int32_t s, int32_t v) {
        int32_t mx = gauge_max_of(rush_of(s));
        int32_t g = wrap_add(gauge_of(s), v);
        if (mx > g)
            return pack(g, lots_of(s), next_of(s), rush_of(s), minimum_of(s), flags_of(s));
        return pack(g % mx, lots_of(s) + (g / mx), next_of(s), rush_of(s), minimum_of(s),
                    flags_of(s));
    };

    std::unordered_map<int32_t, double> dist;
    std::unordered_map<int32_t, double> spare;
    auto push = [](std::unordered_map<int32_t, double> &out, int32_t s, double p) {
        if (p > 0.0)
            out[s] += p;
    };
    auto map_dist = [&](const auto &fn) {
        spare.clear();
        for (const auto &[s, p] : dist)
            fn(s, p, spare);
        std::swap(dist, spare);
    };

    auto consume = [&](int32_t s, double p, int32_t buff,
                       std::unordered_map<int32_t, double> &out) {
        int32_t lot_cnt = lots_of(s) - 1;
        int32_t minimum = minimum_of(s);
        std::vector<std::pair<double, int32_t>> first;
        if (next_of(s) == -1) {
            first = draw(0, buff, minimum);
            minimum = 0;
        } else {
            first = {{1.0, next_of(s)}};
        }
        constexpr std::array<int32_t, 4> kLotByRush{0, 4, 3, 2};
        for (const auto &[pn, result] : first) {
            int32_t flags = flags_of(s);
            int32_t rush = rush_of(s);
            if (rush == 0 || result != 3)
                flags = (result == 3) ? (flags | kBonus) : (flags & ~kBonus);
            int32_t before = gauge_max_of(rush);
            rush = (result >= 0 && result < 3) ? 0 : (result == 3 ? std::min(rush + 1, 4) : rush);
            int32_t after = gauge_max_of(rush);
            int32_t gauge = gauge_of(s);
            int32_t lc = lot_cnt;
            if (before != after && after >= 1 && gauge > after) {
                lc += gauge / after;
                gauge %= after;
            }
            flags |= kFrameLot;
            if (result == 0)
                flags |= kFrameMiss;
            int32_t type = rush < 4 ? kLotByRush[rush] : 1;
            for (const auto &[pr, val] : draw(type, buff, minimum)) {
                push(out, pack(gauge, lc, val, rush, 0, flags), p * pn * pr);
            }
        }
    };

    std::unordered_map<int32_t, std::vector<std::pair<double, int32_t>>> base_point_cache;
    std::unordered_map<size_t, std::vector<LuckNoteRec>> notes_by_frame;
    for (const auto &n : record.notes)
        notes_by_frame[n.frame].push_back(n);

    for (int32_t range : luck_ranges) {
        ptrdiff_t first = -1;
        for (size_t k = 0; k < record.frames.size(); ++k) {
            if (record.frames[k].states[range] == kSStart) {
                first = static_cast<ptrdiff_t>(k);
                break;
            }
        }
        if (first < 0)
            continue;
        size_t last = record.frames.size() - 1;
        for (size_t k = static_cast<size_t>(first) + 1; k < record.frames.size(); ++k) {
            if (record.frames[k].states[range] == kSFinish) {
                last = k;
                break;
            }
        }
        dist.clear();
        dist[pack(0, 0, -1, 0, 0, 0)] = 1.0;
        for (size_t k = static_cast<size_t>(first); k <= last; ++k) {
            const auto &frame = record.frames[k];
            int32_t t = frames[k].time_ms;
            int32_t state = frame.states[range];
            bool changed =
                std::find(frame.updates.begin(), frame.updates.end(), range) != frame.updates.end();
            bool finishing = changed && state == kSFinish;

            for (const auto &action : record.actions) {
                if (action.frame != k || action.range != range)
                    continue;
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    if (action.type == 11005) {
                        int32_t res = minimum_result_of(action.value);
                        push(out,
                             pack(gauge_of(s), lots_of(s), next_of(s), rush_of(s),
                                  std::max(minimum_of(s), res), flags_of(s)),
                             p * action.chance);
                    } else {
                        int32_t g = wrap_mul(gauge_max_of(rush_of(s)), action.value);
                        push(out, add_gauge(s, floor_to_i32(f32(f32(g) / 10000.0f))),
                             p * action.chance);
                    }
                    push(out, s, p * (1.0 - action.chance));
                });
            }

            bool gate_open = changed || (frame.playing == range);
            std::vector<LuckNoteRec> luck_notes;
            if (auto it = notes_by_frame.find(k); it != notes_by_frame.end()) {
                for (const auto &n : it->second)
                    if (n.range == range)
                        luck_notes.push_back(n);
            }
            const auto &judged = frames[k].judged;
            if (luck_notes.empty() && judged.empty()) {
                map_dist([&](int32_t s0, double p0, std::unordered_map<int32_t, double> &out) {
                    std::vector<std::pair<int32_t, double>> branches{{s0, p0}};
                    if (!miss_rules.empty() && gate_open && (s0 & kPrevMiss) && !(s0 & kMissUsed)) {
                        for (const auto &rule : miss_rules) {
                            std::vector<std::pair<int32_t, double>> nb;
                            for (const auto &[b, q] : branches) {
                                int32_t g = wrap_mul(gauge_max_of(rush_of(b)), rule.value);
                                nb.emplace_back(add_gauge(b, floor_to_i32(f32(f32(g) / 10000.0f))),
                                                q * rule.chance);
                                nb.emplace_back(b, q * (1.0 - rule.chance));
                            }
                            branches = std::move(nb);
                        }
                        for (auto &br : branches)
                            br.first |= kMissUsed;
                    }
                    auto tail = [&](int32_t s, double p) {
                        if (finishing)
                            s &= ~kBonus;
                        int32_t next = s & ~(kPrevMiss | kFrameMiss | kFrameLot);
                        if (s & kFrameMiss)
                            next |= kPrevMiss;
                        if (changed && state == kSComplete)
                            next &= ~kMissUsed;
                        push(out, next, p);
                    };
                    for (const auto &[b, q] : branches) {
                        bool sk = (b & kSkill) != 0;
                        if (frame.playing == range)
                            sk = (rush_of(b) != 0);
                        if (changed && (state == kSComplete || state == kSFinish))
                            sk = false;
                        int32_t s = b & ~(kSkill | kSkillPrev);
                        if (b & kSkill)
                            s |= kSkillPrev;
                        if (sk)
                            s |= kSkill;
                        if (state == kSPlaying && lots_of(s) > 0 && !(s & kFrameLot)) {
                            std::unordered_map<int32_t, double> tmp;
                            consume(s, q, 0, tmp);
                            for (const auto &[c, cq] : tmp)
                                tail(c, cq);
                        } else {
                            tail(s, q);
                        }
                    }
                });
                continue;
            }

            if (!miss_rules.empty() && gate_open) {
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    if (!(s & kPrevMiss) || (s & kMissUsed)) {
                        push(out, s, p);
                        return;
                    }
                    std::vector<std::pair<int32_t, double>> branches{{s, p}};
                    for (const auto &rule : miss_rules) {
                        std::vector<std::pair<int32_t, double>> nb;
                        for (const auto &[b, q] : branches) {
                            int32_t g = wrap_mul(gauge_max_of(rush_of(b)), rule.value);
                            nb.emplace_back(add_gauge(b, floor_to_i32(f32(f32(g) / 10000.0f))),
                                            q * rule.chance);
                            nb.emplace_back(b, q * (1.0 - rule.chance));
                        }
                        branches = std::move(nb);
                    }
                    for (const auto &[b, q] : branches)
                        push(out, b | kMissUsed, q);
                });
            }

            map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                bool sk = (s & kSkill) != 0;
                if (frame.playing == range)
                    sk = (rush_of(s) != 0);
                if (changed && (state == kSComplete || state == kSFinish))
                    sk = false;
                int32_t next = s & ~(kSkill | kSkillPrev);
                if (s & kSkill)
                    next |= kSkillPrev;
                if (sk)
                    next |= kSkill;
                push(out, next, p);
            });

            auto observe = [&](int32_t id, bool at_frame_time) {
                double r = 0.0, sk = 0.0, both = 0.0;
                int32_t skill_bit = at_frame_time ? kSkill : kSkillPrev;
                for (const auto &[s, p] : dist) {
                    bool bonus = (s & kBonus) != 0;
                    bool skill = (s & skill_bit) != 0;
                    if (bonus)
                        r += p;
                    if (skill)
                        sk += p;
                    if (bonus && skill)
                        both += p;
                }
                curve.rush[id] += r;
                curve.skill[id] += sk;
                curve.both[id] += both;
            };

            auto luck_note = [&](const LuckNoteRec &n) {
                int32_t key = n.op * 1000 + n.judgement * 100 +
                              static_cast<int32_t>(std::llround(n.speed * 10.0f));
                auto it = base_point_cache.find(key);
                if (it == base_point_cache.end()) {
                    std::vector<std::pair<double, int32_t>> pts;
                    for (const auto &[p, v] : base_points(n.op, n.judgement)) {
                        pts.emplace_back(p, floor_to_i32(f32(f32(n.speed + 1.0f) * f32(v))));
                    }
                    it = base_point_cache.emplace(key, std::move(pts)).first;
                }
                const auto &points = it->second;
                bool consumes = (state <= kSEnd);
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    for (const auto &[pb, add] : points) {
                        int32_t next = add_gauge(s, add);
                        if (consumes && lots_of(next) > 0)
                            consume(next, p * pb, n.buff, out);
                        else
                            push(out, next, p * pb);
                    }
                });
            };

            std::vector<int32_t> times;
            for (const auto &n : luck_notes)
                times.push_back(n.time_ms);
            for (const auto &n : judged)
                times.push_back(n.time_ms);
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());

            for (int32_t time : times) {
                if (time >= t)
                    continue;
                for (const auto &n : luck_notes)
                    if (n.time_ms == time)
                        luck_note(n);
                for (const auto &n : judged)
                    if (n.time_ms == time)
                        observe(n.note_id, false);
            }
            for (const auto &n : luck_notes)
                if (n.time_ms >= t)
                    luck_note(n);
            if (state == kSPlaying) {
                int32_t buff = 0;
                bool found_b = false;
                for (const auto &n : luck_notes) {
                    if (n.time_ms >= t) {
                        buff = n.buff;
                        found_b = true;
                        break;
                    }
                }
                if (!found_b && !luck_notes.empty())
                    buff = luck_notes.back().buff;
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    if (lots_of(s) > 0 && !(s & kFrameLot))
                        consume(s, p, buff, out);
                    else
                        push(out, s, p);
                });
            }
            if (finishing) {
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    push(out, s & ~kBonus, p);
                });
            }
            for (const auto &n : judged)
                if (n.time_ms >= t)
                    observe(n.note_id, true);
            map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                int32_t next = s & ~(kPrevMiss | kFrameMiss | kFrameLot);
                if (s & kFrameMiss)
                    next |= kPrevMiss;
                if (changed && state == kSComplete)
                    next &= ~kMissUsed;
                push(out, next, p);
            });
        }
    }
    return curve;
}
// 谱面判定流、槽位技能解析、120 顺序对称压缩与激奏评分
inline bool is_judgement_note(int32_t op) {
    switch (op) {
    case 0:
    case 80:
    case 82:
    case 100:
    case 103:
    case 121:
    case 122:
    case 123:
        return false;
    default:
        return true;
    }
}

struct ChartNote {
    int32_t op = 0;
    int32_t time_ms = 0;
    int32_t judgement_type = 0;
};

struct ChartSource {
    int32_t duration_ms = 0;
    std::vector<int32_t> skill_times_ms;
    std::vector<std::pair<int32_t, int32_t>> fever_ms;
    std::vector<ChartNote> enumeration;
};

struct FullChart {
    std::vector<LiveNote> notes;
    std::vector<std::pair<int32_t, int32_t>> events;
    std::vector<std::pair<int32_t, int32_t>> fevers;
    int32_t converted_count = 1;
    int32_t last_timing_ms = 0;
    int32_t level = 26;
};

FullChart build_full_chart(const EngineMaster &master, const ChartSource &source, int32_t level) {
    if (source.enumeration.empty())
        throw std::runtime_error("谱面缺少音符枚举列表");
    FullChart fc;
    fc.level = level;
    int32_t sum = 0;
    int32_t last_t = 0;
    for (size_t i = 0; i < source.enumeration.size(); ++i) {
        const auto &n = source.enumeration[i];
        fc.notes.push_back({static_cast<int32_t>(i + 1), n.time_ms, n.op, n.judgement_type});
        if (auto it = master.live.note_percent.find(n.op); it != master.live.note_percent.end())
            sum = wrap_add(sum, it->second);
        last_t = std::max(last_t, n.time_ms);
    }
    for (size_t i = 0; i < source.skill_times_ms.size(); ++i)
        fc.events.emplace_back(static_cast<int32_t>(i), source.skill_times_ms[i]);
    fc.fevers = source.fever_ms;
    fc.converted_count = ceil_to_i32(f32(f32(sum) / 100.0f));
    fc.last_timing_ms = last_t;
    return fc;
}

struct Accuracy {
    double great = 0.0;
    double just = 1.0;
    int32_t miss_every = 0;
};

struct LivePlay {
    std::vector<PlayFrame> frames;
    std::vector<float> dt;
    std::vector<int32_t> just_eligible_ids;
};

LivePlay judgement_stream(const EngineMaster &master, const FullChart &chart,
                          const GekisouSetup *gekisou, const Accuracy &accuracy) {
    std::vector<LiveNote> judged;
    for (const auto &n : chart.notes) {
        if (is_judgement_note(n.op))
            judged.push_back(n);
    }
    std::sort(judged.begin(), judged.end(), [](const LiveNote &a, const LiveNote &b) {
        return (a.time_ms != b.time_ms) ? (a.time_ms < b.time_ms) : (a.id < b.id);
    });
    int32_t last = 0;
    for (const auto &n : chart.notes)
        last = std::max(last, n.time_ms);
    for (const auto &[_, t] : chart.events)
        last = std::max(last, t);
    if (gekisou) {
        for (const auto &[_, e] : gekisou->fevers)
            last = std::max(last, e);
    }
    std::vector<int32_t> times;
    for (int64_t k = 0;; ++k) {
        int32_t t = static_cast<int32_t>((k * 1000) / 60);
        if (t > last + 2000)
            break;
        times.push_back(t);
    }
    std::vector<std::pair<size_t, size_t>> windows;
    if (gekisou) {
        for (size_t i = 0; i < gekisou->fevers.size(); ++i) {
            if (gekisou->missions.at(i) != 3)
                continue;
            auto [start, end] = gekisou->fevers[i];
            auto it1 = std::lower_bound(times.begin(), times.end(), start);
            if (it1 == times.end())
                continue;
            size_t first = static_cast<size_t>(it1 - times.begin());
            auto it2 = std::lower_bound(it1 + 1, times.end(), end);
            size_t second =
                (it2 == times.end()) ? times.size() : static_cast<size_t>(it2 - times.begin());
            windows.emplace_back(first, second);
        }
    }
    auto in_window = [&](size_t frame) {
        for (const auto &[a, b] : windows)
            if (a <= frame && frame < b)
                return true;
        return false;
    };
    struct Row {
        size_t frame;
        LiveNote note;
        int32_t best;
    };
    std::vector<Row> rows;
    rows.reserve(judged.size());
    for (const auto &n : judged) {
        auto it = std::lower_bound(times.begin(), times.end(), n.time_ms);
        size_t frame = static_cast<size_t>(it - times.begin());
        bool just = (gekisou != nullptr && master.live.just_types.count(n.judgement_type) &&
                     in_window(frame));
        rows.push_back({frame, n, just ? 6 : 5});
    }
    auto choose_evenly = [](size_t n, int64_t count, const auto &select) {
        int64_t rem = 0;
        int64_t nn = static_cast<int64_t>(n);
        for (size_t i = 0; i < n; ++i) {
            rem += count;
            if (rem >= nn) {
                rem -= nn;
                select(i);
            }
        }
    };
    size_t total = rows.size();
    std::vector<int32_t> grade(total, 5);
    choose_evenly(total,
                  static_cast<int64_t>(std::llround(static_cast<double>(total) * accuracy.great)),
                  [&](size_t i) { grade[i] = 4; });
    std::vector<size_t> eligible;
    for (size_t i = 0; i < total; ++i) {
        if (grade[i] != 4 && rows[i].best == 6)
            eligible.push_back(i);
    }
    choose_evenly(
        eligible.size(),
        static_cast<int64_t>(std::llround(static_cast<double>(eligible.size()) * accuracy.just)),
        [&](size_t k) { grade[eligible[k]] = 6; });
    if (accuracy.miss_every > 0) {
        for (size_t i = static_cast<size_t>(accuracy.miss_every - 1); i < total;
             i += static_cast<size_t>(accuracy.miss_every)) {
            grade[i] = 1;
        }
    }
    LivePlay out;
    out.frames.resize(times.size());
    for (size_t i = 0; i < times.size(); ++i)
        out.frames[i].time_ms = times[i];
    for (size_t i = 0; i < rows.size(); ++i) {
        out.frames[rows[i].frame].judged.push_back(
            {rows[i].note.id, grade[i], rows[i].note.time_ms});
    }
    out.dt.assign(times.size(), f32(1.0 / 60.0));
    for (size_t idx : eligible)
        out.just_eligible_ids.push_back(rows[idx].note.id);
    return out;
}

struct GekisoChart {
    FullChart full;
    GekisouSetup setup;
    LivePlay play;
    Accuracy accuracy;
    std::vector<LiveNote> notes;
    std::vector<int32_t> times;
    std::vector<float> note_frac;
    std::vector<int8_t> judgements;
    std::vector<int8_t> range_of;
    std::vector<int32_t> range_percent;
    bool luck = false;
    float difficulty_factor = 1.0f;
    int32_t music_length_ms = 1000;
    std::vector<float> combo_factor;
    std::array<float, 7> judge_frac{};
    std::pair<size_t, size_t> span{0, 0};
};

GekisoChart build_gekiso_chart(const EngineMaster &master, const ChartSource &source, int32_t level,
                               const std::vector<int32_t> &missions, const Accuracy &accuracy,
                               int32_t rank = 1) {
    GekisoChart gc;
    gc.full = build_full_chart(master, source, level);
    gc.setup = {gc.full.fevers, missions};
    gc.play = judgement_stream(master, gc.full, &gc.setup, accuracy);
    gc.accuracy = accuracy;

    std::unordered_map<int32_t, int32_t> judgement_of;
    for (const auto &frame : gc.play.frames) {
        for (const auto &j : frame.judged)
            judgement_of[j.note_id] = j.judgement;
    }
    for (const auto &n : gc.full.notes) {
        if (is_judgement_note(n.op) && master.live.note_percent.count(n.op))
            gc.notes.push_back(n);
    }
    std::sort(gc.notes.begin(), gc.notes.end(), [](const LiveNote &a, const LiveNote &b) {
        return (a.time_ms != b.time_ms) ? (a.time_ms < b.time_ms) : (a.id < b.id);
    });
    size_t n = gc.notes.size();
    gc.times.resize(n);
    gc.note_frac.resize(n);
    gc.judgements.resize(n);
    for (size_t i = 0; i < n; ++i) {
        gc.times[i] = gc.notes[i].time_ms;
        gc.note_frac[i] =
            f32(static_cast<double>(master.live.note_percent.at(gc.notes[i].op)) / 100.0);
        auto it = judgement_of.find(gc.notes[i].id);
        gc.judgements[i] = static_cast<int8_t>(it != judgement_of.end() ? it->second : 5);
    }

    auto factors =
        ranking_factors(master.live.gekiso, mission_pattern(missions.size() > 0 ? missions[0] : 0,
                                                            missions.size() > 1 ? missions[1] : 0,
                                                            missions.size() > 2 ? missions[2] : 0));
    size_t num_ranges = std::min<size_t>(3, gc.full.fevers.size());
    for (size_t r = 0; r < num_ranges; ++r) {
        gc.range_percent.push_back(factors[r][rank - 1]);
        if (r < missions.size() && missions[r] == kMLuck)
            gc.luck = true;
    }
    gc.range_of.assign(n, -1);
    for (size_t i = 0; i < n; ++i) {
        int32_t frame = get_frame(gc.notes[i].time_ms);
        for (size_t r = 0; r < num_ranges; ++r) {
            auto [s, e] = gc.full.fevers[r];
            if (get_frame(s) < frame && frame <= get_frame(e) && gc.range_of[i] == -1)
                gc.range_of[i] = static_cast<int8_t>(r);
        }
    }
    gc.combo_factor.resize(n);
    {
        const auto *table = master.live.combo.count(0) ? &master.live.combo.at(0) : nullptr;
        int32_t before = 0, pending = 0, last = -1;
        for (size_t i = 0; i < n; ++i) {
            if (gc.times[i] != last) {
                before = pending;
                last = gc.times[i];
            }
            float val = 0.0f;
            if (table) {
                for (const auto &[req, cum] : *table) {
                    if (req > before)
                        break;
                    val = cum;
                }
            }
            gc.combo_factor[i] = f32(std::min(val, 1.0f) + 1.0f);
            pending = (gc.judgements[i] <= 2) ? 0 : (pending + 1);
        }
    }
    for (int32_t type = 1; type <= 6; ++type) {
        int32_t jp = master.live.judge_percent.count(type) ? master.live.judge_percent.at(type) : 0;
        gc.judge_frac[7 - type] = f32(static_cast<double>(jp) / 100.0);
    }
    gc.difficulty_factor = f32(f32(f32(wrap_sub(level, 5)) * f32(0.005)) + 1.0f);
    gc.music_length_ms = gc.full.last_timing_ms + 1000;

    if (num_ranges > 0) {
        int32_t first_start = gc.full.fevers[0].first;
        int32_t last_end = gc.full.fevers[0].second;
        for (size_t r = 1; r < num_ranges; ++r) {
            first_start = std::min(first_start, gc.full.fevers[r].first);
            last_end = std::max(last_end, gc.full.fevers[r].second);
        }
        size_t s0 = 0;
        for (size_t i = 0; i < gc.play.frames.size(); ++i) {
            if (gc.play.frames[i].time_ms >= first_start - 4500) {
                s0 = (i > 0) ? (i - 1) : 0;
                break;
            }
        }
        size_t s1 = gc.play.frames.size();
        for (size_t i = 0; i < gc.play.frames.size(); ++i) {
            if (gc.play.frames[i].time_ms >= last_end + master.live.after_ms + 1200) {
                s1 = i;
                break;
            }
        }
        gc.span = {s0, s1};
    }
    return gc;
}

struct GekisoContext {
    std::vector<int8_t> judgement;
    std::vector<float> gk;
    std::vector<double> up;
    std::vector<double> luck;
    size_t seeds = 0;
};

std::pair<float, std::vector<MissRule>> luck_readers(const EngineMaster &master,
                                                     const std::vector<Performer> &deck) {
    Factory factory(master, deck, master.live.life_base, 0);
    LifeController life(master.live.life_base, master.live.damage, 1000);
    LiveRandom random(0);
    std::vector<std::array<int32_t, 3>> empty_j;
    std::vector<std::pair<int32_t, int32_t>> empty_ev;
    CheckCtx ctx{std::nullopt, &life, &random, 0, 0, &empty_j, &empty_ev, std::nullopt, true};

    auto reads = [&](int32_t group, int32_t type, std::optional<int32_t> val = std::nullopt) {
        auto it = master.condition_sets.find(group);
        if (it == master.condition_sets.end())
            return false;
        for (const auto &set : it->second) {
            for (int32_t id : set) {
                auto cit = master.conditions.find(id);
                if (cit != master.conditions.end() && cit->second.type == type) {
                    if (!val.has_value() ||
                        (!cit->second.values.empty() && cit->second.values[0] == *val))
                        return true;
                }
            }
        }
        return false;
    };

    float rush = 0.0f;
    std::vector<MissRule> misses;
    for (size_t k = 0; k < deck.size(); ++k) {
        const auto &p = deck[k];
        if (!p.gekisou_skill.has_value() && (!p.mission_only.has_value() || *p.mission_only == 0))
            continue;
        for (const auto &[id, level] : p.gekisou_support_skills) {
            auto sit = master.gekiso_support_skills.find(id);
            if (sit == master.gekiso_support_skills.end())
                continue;
            const auto &skill = sit->second;
            if (skill.mission_type != kMLuck && skill.mission_type != kMAll)
                continue;
            for (const auto &row : skill.effects) {
                if (row.level != level)
                    continue;
                if (row.type == 2000 && reads(row.trigger_group, 7021)) {
                    if (chance_of(factory.group(row.condition_group, k), ctx) > 0.5) {
                        rush = f32(rush + f32(static_cast<float>(note_factor_mill(
                                                  f32(f32(row.value) / 10000.0f))) /
                                              100000.0f));
                    }
                } else if (row.type == 11003 && reads(row.trigger_group, 7000, 0)) {
                    double ch = chance_of(factory.group(row.condition_group, k), ctx);
                    if (ch > 0.0)
                        misses.push_back({row.value, ch});
                }
            }
        }
    }
    return {rush, std::move(misses)};
}

GekisoContext expected_context(const EngineMaster &master, const GekisoChart &chart,
                               const std::vector<Performer> &deck,
                               std::unordered_map<std::string, LuckCurve> &curve_cache) {
    size_t n = chart.notes.size();
    std::unordered_map<int32_t, size_t> index;
    for (size_t i = 0; i < n; ++i)
        index[chart.notes[i].id] = i;
    std::vector<int8_t> judgement = chart.judgements;
    std::vector<float> gk(n, 1.0f);
    std::vector<float> note_up(n, 1.0f);
    std::vector<Performer> reduced;
    reduced.reserve(deck.size());
    for (const auto &p : deck)
        reduced.push_back(gekiso_performer(p));

    LuckRecord record;
    LiveModel model(
        master, reduced, chart.full.notes, chart.full.events,
        {0, 1.0, chart.full.level, chart.full.converted_count, chart.music_length_ms, 1.0f},
        &chart.setup, 0, &record);
    model.score.calc.on_note = [&](int32_t id, int32_t score_type, float note, float factor, float,
                                   float) {
        auto it = index.find(id);
        if (it == index.end())
            return;
        size_t i = it->second;
        judgement[i] = static_cast<int8_t>(7 - score_type);
        gk[i] = factor;
        note_up[i] = note;
    };
    std::vector<PlayFrame> simulated(
        chart.play.frames.begin() + static_cast<ptrdiff_t>(chart.span.first),
        chart.play.frames.begin() + static_cast<ptrdiff_t>(chart.span.second));
    for (size_t k = 0; k < simulated.size(); ++k) {
        model.frame(simulated[k].time_ms, simulated[k].judged, chart.play.dt[chart.span.first + k]);
    }
    auto [rush, misses] = luck_readers(master, reduced);
    std::vector<int32_t> luck_ranges;
    size_t num_r = std::min<size_t>(3, chart.full.fevers.size());
    for (size_t r = 0; r < num_r && r < chart.setup.missions.size(); ++r) {
        if (chart.setup.missions[r] == kMLuck)
            luck_ranges.push_back(static_cast<int32_t>(r));
    }

    std::ostringstream sig;
    for (const auto &a : record.actions)
        sig << a.frame << ":" << a.range << ":" << a.type << ":" << a.value << ":" << a.limit << ":"
            << a.chance << ";";
    sig << "|";
    for (const auto &m : misses)
        sig << m.value << ":" << m.chance << ";";
    sig << "|";
    for (const auto &nt : record.notes)
        sig << nt.frame << ":" << nt.judgement << ":" << nt.speed << ":" << nt.buff << ";";
    sig << "|";
    for (const auto &fr : record.frames) {
        for (int32_t s : fr.states)
            sig << s;
        sig << fr.playing << ",";
    }
    std::string signature = sig.str();
    auto cit = curve_cache.find(signature);
    if (cit == curve_cache.end()) {
        cit = curve_cache
                  .emplace(std::move(signature),
                           luck_curve(master.live.gekiso, record, simulated, luck_ranges, misses))
                  .first;
    }
    const auto &curve = cit->second;
    double bonus =
        static_cast<double>(std::min(100 + master.live.gekiso.rush_percent, 200)) / 100.0 - 1.0;
    std::vector<double> up(n, 0.0);
    std::vector<double> luck(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        int32_t id = chart.notes[i].id;
        double pr = curve.rush.count(id) ? curve.rush.at(id) : 0.0;
        double ps = curve.skill.count(id) ? curve.skill.at(id) : 0.0;
        double both = curve.both.count(id) ? curve.both.at(id) : 0.0;
        luck[i] = 1.0 + bonus * pr;
        up[i] = static_cast<double>(note_up[i]) * luck[i] +
                static_cast<double>(rush) * (ps + bonus * both);
    }
    return {std::move(judgement), std::move(gk), std::move(up), std::move(luck), 0};
}

GekisoContext gekiso_context(const EngineMaster &master, const GekisoChart &chart,
                             const std::vector<Performer> &deck, const std::vector<int32_t> &seeds,
                             std::unordered_map<std::string, LuckCurve> &curve_cache) {
    if (chart.luck && seeds.empty())
        return expected_context(master, chart, deck, curve_cache);
    size_t n = chart.notes.size();
    std::unordered_map<int32_t, size_t> index;
    for (size_t i = 0; i < n; ++i)
        index[chart.notes[i].id] = i;
    std::vector<int8_t> judgement = chart.judgements;
    std::vector<float> gk(n, 1.0f);
    std::vector<double> up(n, 0.0);
    std::vector<double> luck(n, 0.0);
    std::vector<int32_t> runs =
        chart.luck ? seeds : std::vector<int32_t>{seeds.empty() ? 0 : seeds[0]};
    std::vector<Performer> reduced;
    reduced.reserve(deck.size());
    for (const auto &p : deck)
        reduced.push_back(gekiso_performer(p));

    for (int32_t seed : runs) {
        LiveModel model(
            master, reduced, chart.full.notes, chart.full.events,
            {0, 1.0, chart.full.level, chart.full.converted_count, chart.music_length_ms, 1.0f},
            &chart.setup, seed);
        std::vector<float> note_up(n, 1.0f);
        std::vector<float> note_luck(n, 1.0f);
        model.score.calc.on_note = [&](int32_t id, int32_t score_type, float note, float factor,
                                       float luck_factor, float) {
            auto it = index.find(id);
            if (it == index.end())
                return;
            size_t i = it->second;
            judgement[i] = static_cast<int8_t>(7 - score_type);
            gk[i] = factor;
            note_up[i] = note;
            note_luck[i] = luck_factor;
        };
        for (size_t k = chart.span.first; k < chart.span.second; ++k) {
            model.frame(chart.play.frames[k].time_ms, chart.play.frames[k].judged,
                        chart.play.dt[k]);
        }
        for (size_t i = 0; i < n; ++i) {
            up[i] += static_cast<double>(note_luck[i]) * static_cast<double>(note_up[i]);
            luck[i] += static_cast<double>(note_luck[i]);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        up[i] /= static_cast<double>(runs.size());
        luck[i] /= static_cast<double>(runs.size());
    }
    return {std::move(judgement), std::move(gk), std::move(up), std::move(luck), runs.size()};
}

struct LifeGate {
    int32_t kind = 0; // 0: always, 1: life-at-least, 2: never
    int32_t value = 0;
    bool positive = true;
};

struct SlotLiveEffect {
    int32_t type = 2000;
    int32_t delta = 0;
    float seconds = 0.0f;
    LifeGate gate;
    std::vector<int32_t> judgements;
};

struct SlotConvert {
    std::vector<int32_t> judgements;
    int32_t limit = 0;
};

struct SlotSkill {
    std::vector<SlotLiveEffect> effects;
    float extension_ms = 0.0f;
    int32_t recovery = 0;
    std::optional<SlotConvert> convert;
};

inline int32_t score_delta(int32_t value) {
    return static_cast<int32_t>(std::floor(f32(f32(f32(value) / 10000.0f) * 100000.0f)));
}

inline int32_t judgement_delta(int32_t value) {
    float scaled = f32(f32(f32(value) / 10000.0f) * 100000.0f);
    return static_cast<int32_t>(round_ties_even(static_cast<double>(scaled)));
}

inline int32_t window_ms(float seconds, float extension_ms) {
    return static_cast<int32_t>(std::ceil(f32(f32(f32(seconds) * 1000.0f) + extension_ms)));
}

SlotSkill resolve_slot_skill(const EngineMaster &master, const MemberState &member,
                             const SnapState *snap,
                             std::vector<std::string> *unsupported = nullptr) {
    auto cond_value = [&](const ConditionRow &row) -> int32_t {
        // returns: 1 = true, 0 = false, 2 = "life", -1 = null
        switch (row.type) {
        case 5000: {
            bool m = matches_targets(master, member, row.target_ids);
            return (row.positive ? m : !m) ? 1 : 0;
        }
        case 4010:
        case 5020:
            return row.positive ? 1 : -1;
        case 2001:
            return 2;
        default:
            return -1;
        }
    };
    struct GroupRes {
        bool holds = false;
        const ConditionRow *life = nullptr;
    };
    auto group_holds = [&](int32_t group) -> std::optional<GroupRes> {
        if (group <= 0)
            return GroupRes{true, nullptr};
        auto it = master.condition_sets.find(group);
        if (it == master.condition_sets.end() || it->second.empty())
            return std::nullopt;
        bool any = false;
        const ConditionRow *life = nullptr;
        for (const auto &set : it->second) {
            bool all = true;
            for (int32_t id : set) {
                auto cit = master.conditions.find(id);
                if (cit == master.conditions.end())
                    return std::nullopt;
                int32_t v = cond_value(cit->second);
                if (v == -1)
                    return std::nullopt;
                if (v == 2)
                    life = &cit->second;
                else if (v == 0)
                    all = false;
            }
            if (all)
                any = true;
        }
        return GroupRes{any, life};
    };
    auto judgements_of = [&](const std::vector<int32_t> &ids) {
        std::vector<int32_t> out;
        for (int32_t id : ids) {
            auto it = master.targets.find(id);
            int32_t j = (it != master.targets.end()) ? it->second.judgement : -1;
            if (j >= 3 && j <= 6)
                out.push_back(j);
        }
        return out;
    };

    SlotSkill out;
    if (auto it = master.live_skills.find(member.card.live_skill_id);
        it != master.live_skills.end()) {
        for (const auto &row : it->second) {
            if (row.level != member.growth.live_skill_level)
                continue;
            if (row.type != 2000 && row.type != 2004) {
                if (unsupported)
                    unsupported->push_back("live:" + std::to_string(member.card.live_skill_id) +
                                           ":" + std::to_string(row.type));
                continue;
            }
            auto condition = group_holds(row.condition_group);
            if (!condition.has_value()) {
                if (unsupported)
                    unsupported->push_back("live-condition:" + std::to_string(row.condition_group));
                continue;
            }
            if (!condition->holds)
                continue;
            LifeGate gate;
            if (condition->life) {
                gate = {1, condition->life->values.empty() ? 0 : condition->life->values[0],
                        condition->life->positive};
            } else {
                gate = {0, 0, true};
            }
            out.effects.push_back({
                row.type,
                row.type == 2000 ? score_delta(row.value) : judgement_delta(row.value),
                f32(row.seconds),
                gate,
                row.type == 2004 ? judgements_of(row.target_ids) : std::vector<int32_t>{},
            });
        }
    }
    if (snap) {
        for (size_t slot = 0; slot < 2; ++slot) {
            int32_t skill_id = snap->card.support_skill_ids[slot];
            if (!skill_id)
                continue;
            int32_t lv = snap->skill_levels[slot] > 0 ? snap->skill_levels[slot] : 1;
            auto it = master.support_skills.find(skill_id);
            if (it == master.support_skills.end())
                continue;
            for (const auto &row : it->second) {
                if (row.level != lv)
                    continue;
                auto enabled = group_holds(row.condition_group);
                auto trigger = group_holds(row.trigger_group);
                if (!enabled.has_value() || !trigger.has_value()) {
                    if (unsupported)
                        unsupported->push_back("support-condition:" + std::to_string(skill_id));
                    continue;
                }
                if (!enabled->holds)
                    continue;
                if (row.type == 15000) {
                    out.extension_ms = f32(out.extension_ms + f32(row.value));
                } else if (row.type == 3001) {
                    out.recovery += row.value;
                } else if (row.type == 12006) {
                    out.convert = SlotConvert{judgements_of(row.target_ids),
                                              row.limit_count > 0 ? row.limit_count : kI32Max};
                } else if (row.type != 3003) {
                    if (unsupported)
                        unsupported->push_back("support:" + std::to_string(skill_id) + ":" +
                                               std::to_string(row.type));
                }
            }
        }
    }
    return out;
}

const std::vector<std::array<int32_t, 5>> &all_orders() {
    static const std::vector<std::array<int32_t, 5>> kOrders = []() {
        std::vector<std::array<int32_t, 5>> out;
        std::array<int32_t, 5> p{0, 1, 2, 3, 4};
        do {
            out.push_back(p);
        } while (std::next_permutation(p.begin(), p.end()));
        return out;
    }();
    return kOrders;
}

bool equal_slot_skill(const SlotSkill &a, const SlotSkill &b) {
    if (a.extension_ms != b.extension_ms || a.recovery != b.recovery)
        return false;
    if (a.convert.has_value() != b.convert.has_value())
        return false;
    if (a.convert.has_value()) {
        if (a.convert->limit != b.convert->limit || a.convert->judgements != b.convert->judgements)
            return false;
    }
    if (a.effects.size() != b.effects.size())
        return false;
    for (size_t i = 0; i < a.effects.size(); ++i) {
        const auto &ea = a.effects[i];
        const auto &eb = b.effects[i];
        if (ea.type != eb.type || ea.delta != eb.delta || ea.seconds != eb.seconds ||
            ea.gate.kind != eb.gate.kind || ea.judgements != eb.judgements)
            return false;
        if (ea.gate.kind == 1 &&
            (ea.gate.value != eb.gate.value || ea.gate.positive != eb.gate.positive))
            return false;
    }
    return true;
}

struct OrderScores {
    double power = 0.0;
    std::array<double, 120> scores{};
    double mean = 0.0;
    double min = 0.0;
    double max = 0.0;
    size_t min_order = 0;
    size_t max_order = 0;
};

OrderScores score_orders_gekiso(const EngineMaster &master, const GekisoChart &chart,
                                const GekisoContext &ctx, double power,
                                const std::vector<SlotSkill> &slots) {
    size_t n = chart.notes.size();
    float t =
        f32(f32(master.live.adjustment * static_cast<double>(power)) * chart.difficulty_factor);
    int32_t count = chart.full.converted_count;
    float onus = master.live.life_onus;
    const auto &damage = master.live.damage;

    std::vector<int32_t> event_times;
    for (const auto &[_, tm] : chart.full.events)
        event_times.push_back(tm);
    std::vector<size_t> firing;
    for (size_t i = 0; i < std::min<size_t>(5, event_times.size()); ++i)
        firing.push_back(i);
    std::sort(firing.begin(), firing.end(), [&](size_t a, size_t b) {
        return (event_times[a] != event_times[b]) ? (event_times[a] < event_times[b]) : (a < b);
    });

    std::vector<float> note_a(n);
    std::vector<float> combo_factor(n);
    for (size_t i = 0; i < n; ++i) {
        note_a[i] = f32(chart.note_frac[i] * t);
        combo_factor[i] = f32(ctx.gk[i] * chart.combo_factor[i]);
    }
    bool deterministic = (ctx.seeds == 1);
    size_t ranges = chart.range_percent.size();

    std::array<int32_t, 5> groups{};
    int32_t classes = 0;
    for (size_t slot = 0; slot < 5; ++slot) {
        size_t prev = 0;
        while (prev < slot && !equal_slot_skill(slots[slot], slots[prev]))
            ++prev;
        groups[slot] = (prev == slot) ? classes++ : groups[prev];
    }

    const auto &orders = all_orders();
    std::vector<size_t> reps;
    std::array<size_t, 120> sources{};
    if (classes == 5) {
        reps.resize(120);
        for (size_t i = 0; i < 120; ++i) {
            reps[i] = i;
            sources[i] = i;
        }
    } else {
        std::unordered_map<int32_t, size_t> first;
        for (size_t idx = 0; idx < 120; ++idx) {
            int32_t seq = 0;
            for (int32_t s : orders[idx])
                seq = seq * 5 + groups[s];
            auto it = first.find(seq);
            if (it == first.end()) {
                first[seq] = idx;
                reps.push_back(idx);
                sources[idx] = idx;
            } else {
                sources[idx] = it->second;
            }
        }
    }

    OrderScores out;
    out.power = power;
    std::vector<double> range_score(ranges, 0.0);
    struct ActiveEff {
        int32_t end;
        int32_t channel;
        int32_t delta;
    };
    struct ActiveConv {
        int32_t end;
        std::vector<int32_t> judgements;
        int32_t left;
    };

    for (size_t order_idx : reps) {
        const auto &order = orders[order_idx];
        int32_t life = master.live.life_base;
        std::array<float, 7> channel{};
        std::vector<ActiveEff> active;
        float factor = 0.0f;
        size_t cursor = 0;
        std::optional<ActiveConv> convert;
        double score = 0.0;
        std::fill(range_score.begin(), range_score.end(), 0.0);

        auto expire = [&](int32_t time) {
            for (ptrdiff_t i = static_cast<ptrdiff_t>(active.size()) - 1; i >= 0; --i) {
                if (active[static_cast<size_t>(i)].end <= time) {
                    auto [_, which, delta] = active[static_cast<size_t>(i)];
                    float diff = f32(static_cast<double>(delta) / 100000.0);
                    if (which == 0)
                        factor = f32(factor - diff);
                    else
                        channel[which] = f32(channel[which] - diff);
                    active.erase(active.begin() + i);
                }
            }
            if (convert.has_value() && convert->end <= time)
                convert.reset();
        };

        for (size_t i = 0; i <= n; ++i) {
            int32_t time = (i < n) ? chart.times[i] : kI32Max;
            while (cursor < firing.size() && event_times[firing[cursor]] <= time) {
                size_t ev = firing[cursor];
                int32_t start = event_times[ev];
                expire(start);
                const auto &slot = slots[order[ev]];
                for (const auto &eff : slot.effects) {
                    if (eff.gate.kind == 1 && ((life >= eff.gate.value) != eff.gate.positive))
                        continue;
                    int32_t end = start + window_ms(eff.seconds, slot.extension_ms);
                    float diff = f32(static_cast<double>(eff.delta) / 100000.0);
                    if (eff.type == 2000) {
                        factor = f32(factor + diff);
                        active.push_back({end, 0, eff.delta});
                    } else {
                        for (int32_t which : eff.judgements) {
                            channel[which] = f32(channel[which] + diff);
                            active.push_back({end, which, eff.delta});
                        }
                    }
                }
                if (slot.recovery)
                    life = std::min(master.live.life_base, life + slot.recovery);
                if (slot.convert.has_value()) {
                    float sec = slot.effects.empty() ? 5.0f : slot.effects[0].seconds;
                    convert = ActiveConv{start + window_ms(sec, slot.extension_ms),
                                         slot.convert->judgements, slot.convert->limit};
                }
                ++cursor;
            }
            if (i == n)
                break;
            expire(time);
            int32_t judgement = ctx.judgement[i];
            if (convert.has_value() && convert->left > 0 &&
                std::find(convert->judgements.begin(), convert->judgements.end(), judgement) !=
                    convert->judgements.end()) {
                judgement = 5;
                convert->left--;
            }
            float c = f32(f32(chart.judge_frac[judgement] * note_a[i]) * combo_factor[i]);
            float live = f32(factor + channel[judgement]);
            double x = 0.0;
            if (deterministic) {
                float up_f = f32(f32(ctx.up[i] / ctx.luck[i]) + live);
                x = std::floor(
                    f32(f32(f32(ctx.luck[i]) * f32(c * up_f)) / static_cast<float>(count)));
            } else {
                x = std::floor((static_cast<double>(c) *
                                (ctx.up[i] + ctx.luck[i] * static_cast<double>(live))) /
                               static_cast<double>(count));
            }
            if (life <= 0)
                x = std::floor(f32(onus * static_cast<float>(x)));
            score += x;
            int8_t r = chart.range_of[i];
            if (r >= 0)
                range_score[r] += x;
            if (damage[judgement])
                life = std::max(0, life - damage[judgement]);
        }
        for (size_t r = 0; r < ranges; ++r)
            score += std::floor((range_score[r] * chart.range_percent[r]) / 100.0);
        out.scores[order_idx] = score;
    }

    if (classes != 5) {
        for (size_t i = 0; i < 120; ++i)
            out.scores[i] = out.scores[sources[i]];
    }
    std::array<double, 120> sorted_scores = out.scores;
    std::sort(sorted_scores.begin(), sorted_scores.end());
    double total = 0.0;
    for (double v : sorted_scores)
        total += v;
    out.mean = total / 120.0;
    out.min = std::numeric_limits<double>::infinity();
    out.max = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < 120; ++i) {
        if (out.scores[i] < out.min) {
            out.min = out.scores[i];
            out.min_order = i;
        }
        if (out.scores[i] > out.max) {
            out.max = out.scores[i];
            out.max_order = i;
        }
    }
    return out;
}

// Part 5: per-slot power, team search, input parsing and CLI entry points.
struct PlayerState {
    std::map<int32_t, int32_t> character_ranks;
    int32_t total_rank = 0;
    std::map<int32_t, int32_t> band_items;
    int32_t vip_rank = 1;
    std::map<int32_t, int32_t> character_memory;
    std::map<int32_t, int32_t> music_memory;
};

struct MusicView {
    int32_t music_type = 0;
    std::vector<int32_t> best_tags;
    int32_t memory_key = 0;
};

struct SlotBase {
    BP3 common{0, 0, 0};
    int64_t fixed_points = 0;
};

struct LeaderProfile {
    std::vector<SkillEffectRow> effects;
    bool simple = false;
};

inline BP3 add3(const BP3 &a, const BP3 &b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

inline int64_t floor_div_10000(int64_t value) {
    return value >= 0 ? value / 10000 : -((-value + 9999) / 10000);
}

// CardPower.mul then to_floor: trunc(a*b/10000), then binary32 floor to whole points.
BP3 pct_floor(const BP3 &base, const BP3 &pct) {
    BP3 out{0, 0, 0};
    for (int i = 0; i < 3; ++i)
        out[i] = floor_f32_div_10000(base[i] * pct[i]) * 10000;
    return out;
}

BP3 pct_uniform(const BP3 &base, int64_t pct) {
    return pct_floor(base, BP3{pct, pct, pct});
}

int64_t points_of(const BP3 &bp) {
    int64_t total = 0;
    for (int64_t value : bp)
        total += floor_div_10000(value);
    return total;
}

inline bool is_cumulative_type(int32_t type) {
    return (type & ~3) == 1500;
}

void accumulate_pct(int32_t type, int64_t value, BP3 &acc) {
    auto add = [&](int index) {
        acc[index] = static_cast<int32_t>(
            wrap_add(static_cast<int32_t>(acc[index]), static_cast<int32_t>(value)));
    };
    switch (type) {
    case 1000:
    case 1500:
        add(0);
        add(1);
        add(2);
        break;
    case 1001:
    case 1501:
        add(1);
        break;
    case 1002:
    case 1502:
        add(2);
        break;
    case 1003:
    case 1503:
        add(0);
        break;
    default:
        break;
    }
}

int64_t cumulative_count(const EngineMaster &master, int32_t id,
                         const std::vector<MemberState> &members, size_t leader) {
    if (id <= 0)
        return 1;
    auto it = master.cumulative.find(id);
    if (it == master.cumulative.end())
        return 1;
    const CumulativeRow &row = it->second;
    auto hit = [&](const MemberState &member) {
        return matches_targets(master, member, row.target_ids);
    };
    int64_t count = 1;
    switch (row.type) {
    case 3000:
        count = 0;
        for (const auto &member : members)
            count += hit(member) ? 1 : 0;
        break;
    case 3001:
        count = 0;
        for (size_t i = 0; i < members.size(); ++i)
            if (i != leader && hit(members[i]))
                ++count;
        break;
    case 3002:
        count = 0;
        for (const auto &member : members)
            count += member.card.band_id == members[leader].card.band_id ? 1 : 0;
        break;
    case 3003:
        count = 0;
        for (const auto &member : members)
            count += member.card.band_id != members[leader].card.band_id ? 1 : 0;
        break;
    case 3004: {
        std::set<int32_t> bands;
        for (const auto &member : members)
            bands.insert(member.card.band_id);
        count = static_cast<int64_t>(bands.size());
        break;
    }
    case 3005: {
        std::set<int32_t> types;
        for (const auto &member : members)
            types.insert(member.card.card_type);
        count = static_cast<int64_t>(types.size());
        break;
    }
    default:
        return 1;
    }
    return row.cap < 1 ? count : std::min(count, static_cast<int64_t>(row.cap));
}

bool condition_holds(const EngineMaster &master, int32_t group_id,
                     const std::vector<MemberState> &members, const MusicView *music) {
    if (group_id <= 0)
        return true;
    auto git = master.condition_sets.find(group_id);
    if (git == master.condition_sets.end())
        return true;
    auto any = [&](const MemberState &member, const std::vector<int32_t> &targets) {
        return matches_targets(master, member, targets);
    };
    for (const auto &set : git->second) {
        for (int32_t id : set) {
            auto cit = master.conditions.find(id);
            if (cit == master.conditions.end())
                continue;
            const ConditionRow &condition = cit->second;
            if (condition.type == 0 || condition.target_ids.empty())
                continue;
            bool holds = true;
            if (condition.type == 3000) {
                holds = false;
                for (const auto &member : members)
                    if (any(member, condition.target_ids)) {
                        holds = true;
                        break;
                    }
            } else if (condition.type == 3001) {
                holds = true;
                for (const auto &member : members)
                    if (!any(member, condition.target_ids)) {
                        holds = false;
                        break;
                    }
            } else if (condition.type == 4012) {
                holds = false;
                if (music) {
                    for (int32_t target : condition.target_ids) {
                        auto tit = master.targets.find(target);
                        if (tit != master.targets.end() && tit->second.live_music_type != 0 &&
                            tit->second.live_music_type == music->music_type) {
                            holds = true;
                            break;
                        }
                    }
                }
            }
            if (!holds)
                return false;
        }
    }
    return true;
}

LeaderProfile leader_profile(const EngineMaster &master, const MemberState &leader) {
    LeaderProfile profile;
    auto it = master.leader_skills.find(leader.card.leader_skill_id);
    if (it != master.leader_skills.end()) {
        for (const auto &row : it->second)
            if (row.level == leader.leader_skill_level)
                profile.effects.push_back(row);
    }
    profile.simple = true;
    for (const auto &row : profile.effects)
        if (row.condition_group > 0 || is_cumulative_type(row.type)) {
            profile.simple = false;
            break;
        }
    return profile;
}

std::vector<BP3> leader_percents(const EngineMaster &master, const LeaderProfile &profile,
                                 const std::vector<MemberState> &members, size_t leader,
                                 const MusicView *music) {
    std::vector<BP3> out(members.size(), BP3{0, 0, 0});
    for (const auto &effect : profile.effects) {
        if (!condition_holds(master, effect.condition_group, members, music))
            continue;
        int64_t value =
            is_cumulative_type(effect.type)
                ? wrap_mul(effect.value, static_cast<int32_t>(cumulative_count(
                                             master, effect.cumulative_id, members, leader)))
                : effect.value;
        for (size_t i = 0; i < members.size(); ++i) {
            if (!effect.target_ids.empty() &&
                !matches_targets(master, members[i], effect.target_ids))
                continue;
            accumulate_pct(effect.type, value, out[i]);
        }
    }
    return out;
}

[[maybe_unused]] BP3 leader_percent_bound(const EngineMaster &master, const LeaderProfile &profile,
                                          const MemberState &member) {
    BP3 acc{0, 0, 0};
    for (const auto &effect : profile.effects) {
        if (!effect.target_ids.empty() && !matches_targets(master, member, effect.target_ids))
            continue;
        int64_t value = effect.value;
        if (is_cumulative_type(effect.type)) {
            int32_t cap = 5;
            auto cit = master.cumulative.find(effect.cumulative_id);
            if (cit != master.cumulative.end())
                cap = cit->second.cap;
            value = std::max<int64_t>(value, value * (cap >= 1 ? std::min(cap, 5) : 5));
        }
        accumulate_pct(effect.type, profile.simple ? value : std::max<int64_t>(0, value), acc);
    }
    return acc;
}

int64_t leader_points(const SlotBase &base, const BP3 &percent) {
    if (percent[0] == 0 && percent[1] == 0 && percent[2] == 0)
        return 0;
    return points_of(pct_floor(base.common, percent));
}

BP3 band_item_percent(const EngineMaster &master, const PlayerState &player,
                      const MemberState &member) {
    BP3 acc{0, 0, 0};
    const auto &card = member.card;
    for (const auto &[item_id, level] : player.band_items) {
        auto iit = master.band_items.find(item_id);
        if (iit == master.band_items.end())
            continue;
        auto lit = iit->second.levels.find(level);
        if (lit == iit->second.levels.end())
            continue;
        for (const auto &effect : lit->second) {
            for (int32_t target_id : effect.target_ids) {
                auto tit = master.targets.find(target_id);
                if (tit == master.targets.end())
                    continue;
                const TargetRow &target = tit->second;
                int hits =
                    (target.band_id > 0 && target.band_id == card.band_id ? 1 : 0) +
                    (target.character_id > 0 && target.character_id == card.character_id ? 1 : 0) +
                    (target.card_type != 0 && target.card_type == card.card_type ? 1 : 0);
                int32_t type = (effect.type == 1000 || effect.type == 1001 || effect.type == 1002 ||
                                effect.type == 1003)
                                   ? effect.type
                                   : -1;
                for (int i = 0; i < hits; ++i)
                    accumulate_pct(type, effect.value, acc);
            }
        }
    }
    return acc;
}

SlotBase slot_base(const EngineMaster &master, const PlayerState &player, const MemberState &member,
                   const MusicView *music, int32_t member_event_pct = 0) {
    BP3 base = member.own;
    if (member_event_pct)
        base = add3(base, pct_uniform(member.own, member_event_pct));

    auto rank_it = player.character_ranks.find(member.card.character_id);
    int32_t rank = rank_it == player.character_ranks.end() ? 1 : rank_it->second;
    int32_t cr = 0;
    for (const auto &[threshold, bonus] : master.character_rank_bonus)
        if (threshold <= rank)
            cr = bonus;
    int32_t ctr = 0;
    for (const auto &[threshold, bonus] : master.total_rank_bonus)
        if (threshold <= player.total_rank)
            ctr = bonus;
    auto char_mem = player.character_memory.find(member.card.character_id);
    auto music_mem =
        music ? player.music_memory.find(music->memory_key) : player.music_memory.end();
    int64_t memory = (char_mem == player.character_memory.end() ? 0 : char_mem->second) +
                     (music && music_mem != player.music_memory.end() ? music_mem->second : 0);
    int64_t flat = (static_cast<int64_t>(cr) + ctr + memory) * 10000;
    BP3 common = {base[0] + flat, base[1] + flat, base[2] + flat};
    int64_t total = points_of(common);
    total += points_of(pct_floor(common, band_item_percent(master, player, member)));
    if (music) {
        if (member.card.card_type == 99 || music->music_type == 99 ||
            member.card.card_type == music->music_type)
            total += points_of(
                pct_uniform(common, master.params.music_type_base + member.music_type_rate));
        bool tag_matched = false;
        for (int32_t tag : member.card.best_tags)
            if (std::find(music->best_tags.begin(), music->best_tags.end(), tag) !=
                music->best_tags.end()) {
                tag_matched = true;
                break;
            }
        if (tag_matched)
            total += points_of(
                pct_uniform(common, master.params.music_tag_base + member.music_tag_rate));
    }
    auto vip_it = master.vip_bonus.find(player.vip_rank);
    if (vip_it != master.vip_bonus.end() && vip_it->second)
        total += points_of(pct_uniform(common, vip_it->second));
    return {common, total};
}

int64_t snap_points(const EngineMaster &master, const SlotBase &base, const MemberState &member,
                    const SnapState *snap, int32_t snap_event_pct = 0) {
    BP3 percent{0, 0, 0};
    if (snap)
        percent = {snap->percent[0] + snap_event_pct, snap->percent[1] + snap_event_pct,
                   snap->percent[2] + snap_event_pct};
    int64_t total = (snap || snap_event_pct) ? points_of(pct_floor(base.common, percent)) : 0;
    if (snap && snap->card.card_type == member.card.card_type) {
        int64_t link = master.params.type_link_base + snap->type_link_rate;
        total += points_of(pct_uniform(base.common, link));
    }
    return total;
}

struct TeamPower {
    double total = 0.0;
    std::vector<double> slots;
    std::vector<BP3> leader_percents;
};

TeamPower team_power(const Json *prob, const EngineMaster &master, const PlayerState &player,
                     const std::vector<MemberState> &members,
                     const std::vector<const SnapState *> &snaps, size_t leader,
                     const MusicView *music) {
    if (prob) {
        TeamPowerResult result = calc_deckcalc_team_power(prob, master, members, snaps, leader);
        return {result.total, result.slots, {}};
    }
    LeaderProfile profile = leader_profile(master, members[leader]);
    std::vector<BP3> percents = leader_percents(master, profile, members, leader, music);
    TeamPower out;
    out.leader_percents = percents;
    out.slots.reserve(members.size());
    for (size_t i = 0; i < members.size(); ++i) {
        SlotBase base = slot_base(master, player, members[i], music);
        const SnapState *snap = i < snaps.size() ? snaps[i] : nullptr;
        double slot = static_cast<double>(base.fixed_points) +
                      static_cast<double>(snap_points(master, base, members[i], snap)) +
                      static_cast<double>(leader_points(base, percents[i]));
        out.slots.push_back(slot);
        out.total += slot;
    }
    return out;
}

// Maximum-weight assignment of rows to distinct columns (rows <= columns).
int small_assignment(const std::vector<std::vector<double>> &weights, int columns) {
    int n = static_cast<int>(weights.size());
    std::vector<std::vector<int>> small_columns(static_cast<size_t>(n), std::vector<int>(n, -1));
    std::vector<std::vector<double>> small_values(
        static_cast<size_t>(n), std::vector<double>(n, -std::numeric_limits<double>::infinity()));
    for (int r = 0; r < n; ++r) {
        const auto &row = weights[static_cast<size_t>(r)];
        for (int c = 0; c < columns; ++c) {
            double value = row[static_cast<size_t>(c)];
            if (value <= small_values[static_cast<size_t>(r)][static_cast<size_t>(n - 1)])
                continue;
            int k = n - 1;
            while (k > 0 &&
                   small_values[static_cast<size_t>(r)][static_cast<size_t>(k - 1)] < value) {
                small_values[static_cast<size_t>(r)][static_cast<size_t>(k)] =
                    small_values[static_cast<size_t>(r)][static_cast<size_t>(k - 1)];
                small_columns[static_cast<size_t>(r)][static_cast<size_t>(k)] =
                    small_columns[static_cast<size_t>(r)][static_cast<size_t>(k - 1)];
                --k;
            }
            small_values[static_cast<size_t>(r)][static_cast<size_t>(k)] = value;
            small_columns[static_cast<size_t>(r)][static_cast<size_t>(k)] = c;
        }
    }
    std::vector<char> used(static_cast<size_t>(std::max(columns, 1)), 0);
    double best = -std::numeric_limits<double>::infinity();
    std::function<void(int, double)> search = [&](int r, double total) {
        if (r == n) {
            best = std::max(best, total);
            return;
        }
        for (int k = 0; k < n; ++k) {
            int c = small_columns[static_cast<size_t>(r)][static_cast<size_t>(k)];
            if (c < 0 || used[static_cast<size_t>(c)])
                continue;
            used[static_cast<size_t>(c)] = 1;
            search(r + 1, total + small_values[static_cast<size_t>(r)][static_cast<size_t>(k)]);
            used[static_cast<size_t>(c)] = 0;
        }
    };
    search(0, 0.0);
    return best;
}

[[maybe_unused]] double max_assignment(const std::vector<std::vector<double>> &weights, int columns,
                                       std::vector<int> *row_column = nullptr) {
    int n = static_cast<int>(weights.size());
    if (n == 0)
        return 0.0;
    if (row_column == nullptr && n <= 4 && columns <= 4096)
        return small_assignment(weights, columns);
    const double inf = std::numeric_limits<double>::max() / 4;
    int size = std::max(n, columns) + 1;
    std::vector<double> u(static_cast<size_t>(size), 0.0), v(static_cast<size_t>(size), 0.0),
        minv(static_cast<size_t>(size), 0.0);
    std::vector<int> p(static_cast<size_t>(size), 0), way(static_cast<size_t>(size), 0);
    std::vector<char> used(static_cast<size_t>(size), 0);
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::fill(minv.begin(), minv.end(), inf);
        std::fill(used.begin(), used.end(), 0);
        do {
            used[static_cast<size_t>(j0)] = 1;
            int i0 = p[static_cast<size_t>(j0)];
            double delta = inf;
            int j1 = 0;
            const auto &row = weights[static_cast<size_t>(i0 - 1)];
            for (int j = 1; j <= columns; ++j) {
                if (used[static_cast<size_t>(j)])
                    continue;
                double cur = -row[static_cast<size_t>(j - 1)] - u[static_cast<size_t>(i0)] -
                             v[static_cast<size_t>(j)];
                if (cur < minv[static_cast<size_t>(j)]) {
                    minv[static_cast<size_t>(j)] = cur;
                    way[static_cast<size_t>(j)] = j0;
                }
                if (minv[static_cast<size_t>(j)] < delta) {
                    delta = minv[static_cast<size_t>(j)];
                    j1 = j;
                }
            }
            for (int j = 0; j <= columns; ++j) {
                if (used[static_cast<size_t>(j)]) {
                    u[static_cast<size_t>(p[static_cast<size_t>(j)])] += delta;
                    v[static_cast<size_t>(j)] -= delta;
                } else {
                    minv[static_cast<size_t>(j)] -= delta;
                }
            }
            j0 = j1;
        } while (p[static_cast<size_t>(j0)] != 0);
        do {
            int j1 = way[static_cast<size_t>(j0)];
            p[static_cast<size_t>(j0)] = p[static_cast<size_t>(j1)];
            j0 = j1;
        } while (j0);
    }
    double total = 0.0;
    for (int j = 1; j <= columns; ++j)
        if (p[static_cast<size_t>(j)]) {
            total += weights[static_cast<size_t>(p[static_cast<size_t>(j)] - 1)]
                            [static_cast<size_t>(j - 1)];
            if (row_column)
                (*row_column)[static_cast<size_t>(p[static_cast<size_t>(j)] - 1)] = j - 1;
        }
    return total;
}

struct Totals {
    double power = 0.0;
    double skill = 0.0;
    double skill_low = 0.0;
    double bonus = 0.0;
};

struct Team {
    std::vector<int> members;
    std::vector<int> snaps;
};

struct SearchConstraints {
    std::vector<int> required_members;
    std::vector<int> excluded_members;
    std::vector<int> required_snaps;
    std::vector<int> excluded_snaps;
    std::optional<int> leader;
    std::map<int, int> bindings;
    bool no_snaps = false;
    std::optional<double> min_bonus;
    std::optional<double> max_bonus;
    bool allow_empty_snaps = false;
};

struct SearchStats {
    int64_t leaders = 0;
    int64_t member_nodes = 0;
    int64_t snap_nodes = 0;
    int64_t candidates = 0;
    int64_t exact = 0;
    double elapsed_ms = 0.0;
};

struct GekisoDetail {
    OrderScores scores;
    std::vector<SlotSkill> skills;
    size_t seeds = 0;
};

struct SearchHit {
    Team team;
    double power = 0.0;
    std::vector<double> slot_powers;
    Totals totals;
    double key = 0.0;
    GekisoDetail detail;
};

struct SearchExact {
    double key = 0.0;
    double power = 0.0;
    std::vector<double> slot_powers;
    GekisoDetail detail;
};

struct SearchOutput {
    std::vector<SearchHit> hits;
    bool proven = false;
    double bound = 0.0;
    bool has_bound = false;
    SearchStats stats;
};

struct SearchInput {
    const EngineMaster *master = nullptr;
    const PlayerState *player = nullptr;
    const std::vector<MemberState> *members = nullptr;
    const std::vector<SnapState> *snaps = nullptr;
    const MusicView *music = nullptr;
    std::function<std::pair<double, double>(int, int)> skill;
    std::function<double(int)> member_bonus;
    std::function<double(int)> snap_bonus;
    std::function<double(const Totals &)> bound;
    std::function<std::pair<double, double>(const Team &, const Totals &)> interval;
    std::function<SearchExact(const Team &)> exact;
    SearchConstraints constraints;
    int k = 10;
    double time_limit_ms = 0.0;
    std::optional<int> shard_index;
    std::optional<int> shard_count;
    std::optional<double> floor;
};

std::string team_set_key(const Team &team, const std::vector<MemberState> &members) {
    std::vector<std::string> keys;
    keys.reserve(team.members.size());
    for (int member : team.members)
        keys.push_back(members[static_cast<size_t>(member)].key);
    std::sort(keys.begin(), keys.end());
    std::string out;
    for (const auto &key : keys) {
        if (!out.empty())
            out += "|";
        out += key;
    }
    return out;
}

SearchOutput search_teams(const SearchInput &input) {
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    SearchOutput output;
    const EngineMaster &master = *input.master;
    const PlayerState &player = *input.player;
    const std::vector<MemberState> &members = *input.members;
    const std::vector<SnapState> &snaps = *input.snaps;
    const MusicView *music = input.music;
    const SearchConstraints &constraints = input.constraints;
    const int n = static_cast<int>(members.size());
    const int s = static_cast<int>(snaps.size());
    const int width = s + 1;
    const int k = std::max(1, input.k);
    const auto deadline =
        input.time_limit_ms > 0
            ? started + std::chrono::duration_cast<clock::duration>(
                            std::chrono::duration<double, std::milli>(input.time_limit_ms))
            : clock::time_point::max();
    bool timed_out = false;
    double unexplored = -std::numeric_limits<double>::infinity();
    auto expired = [&]() { return input.time_limit_ms > 0 && clock::now() > deadline; };

    std::vector<char> excluded_member(static_cast<size_t>(n), 0);
    for (int i : constraints.excluded_members)
        if (i >= 0 && i < n)
            excluded_member[static_cast<size_t>(i)] = 1;
    std::vector<char> excluded_snap(static_cast<size_t>(s), 0);
    for (int j : constraints.excluded_snaps)
        if (j >= 0 && j < s)
            excluded_snap[static_cast<size_t>(j)] = 1;
    std::vector<char> required_member(static_cast<size_t>(n), 0);
    for (int i : constraints.required_members)
        if (i >= 0 && i < n)
            required_member[static_cast<size_t>(i)] = 1;
    std::set<int> required_snap;
    for (int j : constraints.required_snaps)
        if (j >= 0 && j < s)
            required_snap.insert(j);

    std::vector<SlotBase> bases(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        bases[static_cast<size_t>(i)] =
            slot_base(master, player, members[static_cast<size_t>(i)], music);

    std::vector<char> allowed_snap(static_cast<size_t>(s), 0);
    for (int j = 0; j < s; ++j)
        if (!excluded_snap[static_cast<size_t>(j)] && !constraints.no_snaps)
            allowed_snap[static_cast<size_t>(j)] = 1;

    std::vector<double> snap_power(static_cast<size_t>(n * width), 0.0);
    std::vector<double> skill_high(static_cast<size_t>(n * width), 0.0);
    std::vector<double> skill_low(static_cast<size_t>(n * width), 0.0);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < s; ++j) {
            if (!allowed_snap[static_cast<size_t>(j)])
                continue;
            snap_power[static_cast<size_t>(i * width + j)] = static_cast<double>(
                snap_points(master, bases[static_cast<size_t>(i)], members[static_cast<size_t>(i)],
                            &snaps[static_cast<size_t>(j)]));
            if (input.skill) {
                auto range = input.skill(i, j);
                skill_low[static_cast<size_t>(i * width + j)] = range.first;
                skill_high[static_cast<size_t>(i * width + j)] = range.second;
            }
        }
        if (input.skill) {
            auto range = input.skill(i, -1);
            skill_low[static_cast<size_t>(i * width + s)] = range.first;
            skill_high[static_cast<size_t>(i * width + s)] = range.second;
        }
    }

    std::vector<double> member_bonus(static_cast<size_t>(n), 0.0);
    std::vector<double> snap_bonus(static_cast<size_t>(s), 0.0);
    for (int i = 0; i < n; ++i)
        if (input.member_bonus)
            member_bonus[static_cast<size_t>(i)] = input.member_bonus(i);
    for (int j = 0; j < s; ++j)
        if (input.snap_bonus)
            snap_bonus[static_cast<size_t>(j)] = input.snap_bonus(j);

    struct BestSnap {
        double power = 0.0;
        double weight = 0.0;
        double bonus = 0.0;
    };
    std::vector<BestSnap> best(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto binding = constraints.bindings.find(i);
        if (binding != constraints.bindings.end()) {
            int j = binding->second;
            int column = j < 0 ? s : j;
            best[static_cast<size_t>(i)] = {snap_power[static_cast<size_t>(i * width + column)],
                                            skill_high[static_cast<size_t>(i * width + column)],
                                            j < 0 ? 0.0 : snap_bonus[static_cast<size_t>(j)]};
            continue;
        }
        for (int j = 0; j < s; ++j) {
            if (!allowed_snap[static_cast<size_t>(j)])
                continue;
            best[static_cast<size_t>(i)].power = std::max(
                best[static_cast<size_t>(i)].power, snap_power[static_cast<size_t>(i * width + j)]);
            best[static_cast<size_t>(i)].weight =
                std::max(best[static_cast<size_t>(i)].weight,
                         skill_high[static_cast<size_t>(i * width + j)]);
            best[static_cast<size_t>(i)].bonus =
                std::max(best[static_cast<size_t>(i)].bonus, snap_bonus[static_cast<size_t>(j)]);
        }
        if (constraints.allow_empty_snaps || constraints.no_snaps) {
            best[static_cast<size_t>(i)].power = std::max(
                best[static_cast<size_t>(i)].power, snap_power[static_cast<size_t>(i * width + s)]);
            best[static_cast<size_t>(i)].weight =
                std::max(best[static_cast<size_t>(i)].weight,
                         skill_high[static_cast<size_t>(i * width + s)]);
        }
    }

    std::vector<std::vector<int>> options(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto binding = constraints.bindings.find(i);
        if (binding != constraints.bindings.end()) {
            options[static_cast<size_t>(i)].push_back(binding->second);
            continue;
        }
        for (int j = 0; j < s; ++j)
            if (allowed_snap[static_cast<size_t>(j)])
                options[static_cast<size_t>(i)].push_back(j);
        if (constraints.allow_empty_snaps || constraints.no_snaps)
            options[static_cast<size_t>(i)].push_back(-1);
    }

    std::vector<int> leaders;
    for (int i = 0; i < n; ++i)
        if (!excluded_member[static_cast<size_t>(i)] &&
            (!constraints.leader.has_value() || *constraints.leader == i))
            leaders.push_back(i);
    std::sort(leaders.begin(), leaders.end(), [&](int a, int b) {
        double pa = bases[static_cast<size_t>(a)].fixed_points + best[static_cast<size_t>(a)].power;
        double pb = bases[static_cast<size_t>(b)].fixed_points + best[static_cast<size_t>(b)].power;
        return pa > pb;
    });
    if (input.shard_index && input.shard_count && *input.shard_count > 1) {
        std::vector<int> mine;
        for (size_t pos = 0; pos < leaders.size(); ++pos)
            if (static_cast<int>(pos % static_cast<size_t>(*input.shard_count)) ==
                *input.shard_index)
                mine.push_back(leaders[pos]);
        leaders = std::move(mine);
    }

    auto limit = [&]() {
        double value = input.floor.value_or(-std::numeric_limits<double>::infinity());
        if (static_cast<int>(output.hits.size()) >= k)
            value = std::max(value, output.hits[static_cast<size_t>(k - 1)].key);
        return value;
    };
    auto request_stop = [&]() { timed_out = true; };

    for (int leader : leaders) {
        if (timed_out)
            break;
        if (expired()) {
            request_stop();
            break;
        }
        ++output.stats.leaders;
        const MemberState &leader_member = members[static_cast<size_t>(leader)];
        LeaderProfile profile = leader_profile(master, leader_member);
        int32_t leader_character = leader_member.card.character_id;
        for (int i : constraints.required_members)
            if (i != leader && i >= 0 && i < n &&
                members[static_cast<size_t>(i)].card.character_id == leader_character) {
                request_stop();
                break;
            }
        if (timed_out)
            break;

        std::map<int32_t, std::vector<int>> groups;
        for (int i = 0; i < n; ++i) {
            if (i == leader || excluded_member[static_cast<size_t>(i)])
                continue;
            int32_t character = members[static_cast<size_t>(i)].card.character_id;
            if (character == leader_character)
                continue;
            groups[character].push_back(i);
        }
        if (groups.empty())
            continue;

        std::vector<std::vector<int>> forced;
        std::vector<std::vector<int>> free;
        bool impossible = false;
        for (auto &[_, list] : groups) {
            std::vector<int> must;
            for (int i : list)
                if (required_member[static_cast<size_t>(i)])
                    must.push_back(i);
            if (must.size() > 1) {
                impossible = true;
                break;
            }
            std::vector<std::vector<int>> &target = must.empty() ? free : forced;
            std::vector<int> ordered = must.empty() ? list : must;
            std::sort(ordered.begin(), ordered.end(), [&](int a, int b) {
                double pa = static_cast<double>(bases[static_cast<size_t>(a)].fixed_points) +
                            best[static_cast<size_t>(a)].power;
                double pb = static_cast<double>(bases[static_cast<size_t>(b)].fixed_points) +
                            best[static_cast<size_t>(b)].power;
                return pa > pb;
            });
            target.push_back(std::move(ordered));
        }
        if (impossible || forced.size() > 4)
            continue;
        std::sort(free.begin(), free.end(),
                  [&](const std::vector<int> &a, const std::vector<int> &b) {
                      double pa =
                          static_cast<double>(bases[static_cast<size_t>(a.front())].fixed_points) +
                          best[static_cast<size_t>(a.front())].power;
                      double pb =
                          static_cast<double>(bases[static_cast<size_t>(b.front())].fixed_points) +
                          best[static_cast<size_t>(b.front())].power;
                      return pa > pb;
                  });
        std::vector<std::vector<int>> characters = forced;
        characters.insert(characters.end(), free.begin(), free.end());
        if (static_cast<int>(characters.size()) < 4)
            continue;
        const size_t forced_count = forced.size();

        std::vector<int> chosen;
        chosen.reserve(4);
        std::function<void(size_t)> choose_members = [&](size_t index) {
            if (timed_out || index > characters.size())
                return;
            if (chosen.size() == 4) {
                std::vector<int> team_members;
                team_members.push_back(leader);
                team_members.insert(team_members.end(), chosen.begin(), chosen.end());
                Team team;
                team.members = team_members;
                team.snaps.assign(5, -1);
                if (expired()) {
                    request_stop();
                    return;
                }

                LeaderProfile team_profile = leader_profile(master, leader_member);
                std::vector<MemberState> team_states;
                team_states.reserve(team_members.size());
                for (int member : team_members)
                    team_states.push_back(members[static_cast<size_t>(member)]);
                std::vector<BP3> percents =
                    leader_percents(master, team_profile, team_states, 0, music);
                std::vector<double> base_power(team_members.size(), 0.0);
                for (size_t slot = 0; slot < team_members.size(); ++slot) {
                    const auto &base = bases[static_cast<size_t>(team_members[slot])];
                    base_power[slot] = static_cast<double>(base.fixed_points) +
                                       static_cast<double>(leader_points(base, percents[slot]));
                }
                std::vector<char> used(static_cast<size_t>(std::max(s, 1)), 0);
                for (const auto &[member, snap] : constraints.bindings) {
                    if (snap >= 0 && snap < s &&
                        std::find(team_members.begin(), team_members.end(), member) ==
                            team_members.end())
                        used[static_cast<size_t>(snap)] = 1;
                }
                std::vector<int> assignment(team_members.size(), -1);
                double member_total_bonus = 0.0;
                for (int member : team_members)
                    member_total_bonus += member_bonus[static_cast<size_t>(member)];

                std::function<void(size_t, double, double, double, double)> walk =
                    [&](size_t slot, double power, double weight, double weight_low, double bonus) {
                        if (timed_out)
                            return;
                        ++output.stats.snap_nodes;
                        if ((output.stats.snap_nodes & 1023) == 0 && expired()) {
                            request_stop();
                            return;
                        }
                        if (slot == team_members.size()) {
                            int missing = 0;
                            for (int j : required_snap)
                                if (!used[static_cast<size_t>(j)])
                                    ++missing;
                            if (missing)
                                return;
                            Totals totals{power, weight, weight_low, bonus};
                            if (constraints.min_bonus && bonus < *constraints.min_bonus)
                                return;
                            if (constraints.max_bonus && bonus > *constraints.max_bonus)
                                return;
                            if (input.bound && input.bound(totals) < limit())
                                return;
                            if (input.interval) {
                                auto range = input.interval(team, totals);
                                if (range.second < limit())
                                    return;
                            }
                            ++output.stats.candidates;
                            team.snaps = assignment;
                            SearchExact exact = input.exact(team);
                            ++output.stats.exact;
                            std::string set_key = team_set_key(team, members);
                            size_t existing = output.hits.size();
                            for (size_t i = 0; i < output.hits.size(); ++i)
                                if (team_set_key(output.hits[i].team, members) == set_key) {
                                    existing = i;
                                    break;
                                }
                            if (existing < output.hits.size() &&
                                output.hits[existing].key >= exact.key)
                                return;
                            SearchHit hit;
                            hit.team = team;
                            hit.power = exact.power;
                            hit.slot_powers = exact.slot_powers;
                            hit.totals = totals;
                            hit.key = exact.key;
                            hit.detail = std::move(exact.detail);
                            if (existing < output.hits.size())
                                output.hits.erase(output.hits.begin() +
                                                  static_cast<ptrdiff_t>(existing));
                            size_t at = output.hits.size();
                            while (at > 0 && output.hits[at - 1].key < hit.key)
                                --at;
                            output.hits.insert(output.hits.begin() + static_cast<ptrdiff_t>(at),
                                               std::move(hit));
                            if (static_cast<int>(output.hits.size()) > k)
                                output.hits.resize(static_cast<size_t>(k));
                            return;
                        }
                        int member = team_members[slot];
                        for (int snap : options[static_cast<size_t>(member)]) {
                            if (snap >= 0 && used[static_cast<size_t>(snap)])
                                continue;
                            if (snap >= 0)
                                used[static_cast<size_t>(snap)] = 1;
                            assignment[slot] = snap;
                            int column = snap < 0 ? s : snap;
                            walk(slot + 1,
                                 power + snap_power[static_cast<size_t>(member * width + column)],
                                 weight + skill_high[static_cast<size_t>(member * width + column)],
                                 weight_low +
                                     skill_low[static_cast<size_t>(member * width + column)],
                                 bonus + member_bonus[static_cast<size_t>(member)] +
                                     (snap < 0 ? 0.0 : snap_bonus[static_cast<size_t>(snap)]));
                            if (snap >= 0)
                                used[static_cast<size_t>(snap)] = 0;
                            if (timed_out)
                                return;
                        }
                        assignment[slot] = -1;
                    };
                walk(0, std::accumulate(base_power.begin(), base_power.end(), 0.0), 0.0, 0.0,
                     member_total_bonus);
                return;
            }
            if (characters.size() - index < 4 - chosen.size())
                return;
            if (index < forced_count) {
                for (int candidate : characters[index]) {
                    chosen.push_back(candidate);
                    choose_members(index + 1);
                    chosen.pop_back();
                    if (timed_out)
                        return;
                }
                return;
            }
            if (expired()) {
                request_stop();
                return;
            }
            for (int candidate : characters[index]) {
                chosen.push_back(candidate);
                ++output.stats.member_nodes;
                choose_members(index + 1);
                chosen.pop_back();
                if (timed_out)
                    return;
            }
            choose_members(index + 1);
        };
        choose_members(0);
    }

    output.proven = !timed_out;
    if (timed_out) {
        output.bound = std::max(unexplored, output.hits.empty() ? 0.0 : output.hits[0].key);
        output.has_bound = true;
    }
    output.stats.elapsed_ms =
        std::chrono::duration<double, std::milli>(clock::now() - started).count();
    return output;
}

std::string criterion_key(const std::string &criterion) {
    if (criterion == "max" || criterion == "best-ap" || criterion == "theoretical_score")
        return "max";
    if (criterion == "min" || criterion == "worst-ap")
        return "min";
    return "mean";
}

double order_key(const OrderScores &scores, const std::string &criterion) {
    if (criterion == "max")
        return scores.max;
    if (criterion == "min")
        return scores.min;
    return scores.mean;
}

SearchOutput gekiso_search(const EngineMaster &master, const PlayerState &player,
                           const std::vector<MemberState> &members,
                           const std::vector<SnapState> &snaps, const MusicView *music,
                           const GekisoChart &chart, const std::vector<int32_t> &seeds,
                           const std::string &criterion, const SearchConstraints &constraints,
                           int k, double time_limit_ms, const Json *prob, bool allow_empty_snaps) {
    std::unordered_map<std::string, LuckCurve> curve_cache;
    SearchInput input;
    input.master = &master;
    input.player = &player;
    input.members = &members;
    input.snaps = &snaps;
    input.music = music;
    input.constraints = constraints;
    input.constraints.allow_empty_snaps = allow_empty_snaps;
    input.k = std::max(k, 10);
    input.time_limit_ms = time_limit_ms > 0 ? time_limit_ms : 10000.0;
    input.skill = [](int, int) {
        return std::make_pair(0.0, std::numeric_limits<double>::max() / 16);
    };
    input.member_bonus = [](int) { return 0.0; };
    input.snap_bonus = [](int) { return 0.0; };
    input.bound = [](const Totals &) { return std::numeric_limits<double>::max() / 16; };
    input.interval = [](const Team &, const Totals &) {
        return std::make_pair(0.0, std::numeric_limits<double>::max() / 16);
    };
    input.exact = [&](const Team &team) {
        SearchExact result;
        std::vector<MemberState> ordered;
        std::vector<const SnapState *> selected;
        std::vector<SlotSkill> skills;
        ordered.reserve(team.members.size());
        selected.reserve(team.members.size());
        skills.reserve(team.members.size());
        for (size_t slot = 0; slot < team.members.size(); ++slot) {
            const MemberState &member = members[static_cast<size_t>(team.members[slot])];
            const SnapState *snap =
                team.snaps[slot] < 0 ? nullptr : &snaps[static_cast<size_t>(team.snaps[slot])];
            ordered.push_back(member);
            selected.push_back(snap);
            skills.push_back(resolve_slot_skill(master, member, snap));
        }
        std::vector<Performer> deck;
        deck.reserve(ordered.size());
        for (size_t slot = 0; slot < ordered.size(); ++slot)
            deck.push_back(make_performer(master, ordered[slot], selected[slot]));
        GekisoContext context = gekiso_context(master, chart, deck, seeds, curve_cache);
        TeamPower power = team_power(prob, master, player, ordered, selected, 0, music);
        result.power = power.total;
        result.slot_powers = power.slots;
        result.detail.scores = score_orders_gekiso(master, chart, context, power.total, skills);
        result.key = order_key(result.detail.scores, criterion);
        result.detail.skills = std::move(skills);
        result.detail.seeds = context.seeds;
        return result;
    };
    return search_teams(input);
}

// Input parsing helpers.
bool json_truthy(const Json *value, bool fallback = false) {
    if (!value || value->is_null())
        return fallback;
    if (value->is_bool())
        return value->as_bool();
    if (value->is_number())
        return value->as_double() != 0.0;
    return fallback;
}

std::string json_string(const Json *value, const std::string &fallback = "") {
    if (!value || !value->is_string())
        return fallback;
    return value->as_string();
}

std::vector<int32_t> json_id_list(const Json *value) {
    std::vector<int32_t> out;
    if (!value || !value->is_array())
        return out;
    for (const Json &item : value->as_array()) {
        if (item.is_number())
            out.push_back(json_i32(&item));
        else if (item.is_object())
            out.push_back(row_i32(item, "id"));
    }
    return out;
}

std::optional<int32_t> optional_int(const Json *object, const char *key) {
    const Json *value = object ? object->find(key) : nullptr;
    if (!value || value->is_null() || !value->is_number())
        return std::nullopt;
    return json_i32(value);
}

void validate_chart_fevers(const ChartSource &chart) {
    if (chart.fever_ms.size() < 1 || chart.fever_ms.size() > 3) {
        throw SpecError("激奏谱面必须包含 1 到 3 个 Fever 区间 (chart.fevers)，当前: " +
                        std::to_string(chart.fever_ms.size()));
    }
    for (const auto &[start, end] : chart.fever_ms) {
        if (start >= end)
            throw SpecError("Fever 起始时间必须严格小于结束时间: [" + std::to_string(start) + ", " +
                            std::to_string(end) + "]");
        if (start < 0)
            throw SpecError("Fever 起始时间必须非负: [" + std::to_string(start) + ", " +
                            std::to_string(end) + "]");
    }
    std::vector<std::pair<int32_t, int32_t>> sorted = chart.fever_ms;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i].first < sorted[i - 1].second)
            throw SpecError("Fever 区间不能重叠: [" + std::to_string(sorted[i - 1].first) + ", " +
                            std::to_string(sorted[i - 1].second) + "] 与 [" +
                            std::to_string(sorted[i].first) + ", " +
                            std::to_string(sorted[i].second) + "]");
}

ChartSource normalize_chart_source(const Json &chart_input) {
    Json raw = chart_input;
    if (chart_input.is_string())
        raw = Json::parse_file(chart_input.as_string());
    if (!raw.is_object())
        throw SpecError("未知的谱面输入格式");
    ChartSource source;
    if (json_string(raw.find("format")) == "nnnotes.live-score/1") {
        int32_t last = row_i32(raw, "lastNoteTimeMs");
        source.duration_ms = last + 1000;
        if (const Json *notes = raw.find("notes"); notes && notes->is_array()) {
            for (const Json &note : notes->as_array()) {
                ChartNote item;
                item.time_ms = row_i32(note, "timeMs");
                item.op = row_i32(note, "op");
                item.judgement_type = 0;
                source.enumeration.push_back(item);
            }
        }
        if (const Json *skills = raw.find("skill"); skills && skills->is_array())
            for (const Json &skill : skills->as_array())
                source.skill_times_ms.push_back(row_i32(skill, "timeMs"));
        if (const Json *fevers = raw.find("fever"); fevers && fevers->is_array()) {
            for (const Json &fever : fevers->as_array()) {
                const Json *start = fever.find("start");
                const Json *end = fever.find("end");
                if (start && end)
                    source.fever_ms.emplace_back(row_i32(*start, "timeMs"),
                                                 row_i32(*end, "timeMs"));
            }
        }
        return source;
    }
    const Json *notes = raw.find("notes");
    const Json *skills = raw.find("skill_times_ms");
    if (notes && notes->is_array() && skills && skills->is_array()) {
        int32_t last = 0;
        for (const Json &note : notes->as_array())
            last = std::max(last, row_i32(note, "t", 0, "timeMs"));
        source.duration_ms = row_i32(raw, "duration_ms", last + 1000);
        for (const Json &note : notes->as_array()) {
            ChartNote item;
            item.time_ms = row_i32(note, "t", 0, "timeMs");
            item.op = row_i32(note, "op");
            item.judgement_type = 0;
            source.enumeration.push_back(item);
        }
        for (const Json &skill : skills->as_array())
            source.skill_times_ms.push_back(json_i32(&skill));
        const Json *fevers = raw.find("fevers");
        if (!fevers || !fevers->is_array())
            fevers = raw.find("fever");
        if (fevers && fevers->is_array()) {
            for (const Json &fever : fevers->as_array()) {
                if (fever.is_array()) {
                    if (fever.size() != 2)
                        throw SpecError("Fever 区间必须为 [startMs, endMs] 格式");
                    source.fever_ms.emplace_back(json_i32(&fever.at(0)), json_i32(&fever.at(1)));
                } else if (fever.is_object()) {
                    int32_t start = row_i32(fever, "start_ms");
                    int32_t end = row_i32(fever, "end_ms");
                    if (const Json *s = fever.find("start"); s && s->is_object())
                        start = row_i32(*s, "timeMs");
                    if (const Json *e = fever.find("end"); e && e->is_object())
                        end = row_i32(*e, "timeMs");
                    source.fever_ms.emplace_back(start, end);
                }
            }
        }
        return source;
    }
    throw SpecError("未知的谱面输入格式");
}

struct ProblemView {
    Json problem;
    const Json *chart = nullptr;
    const Json *song = nullptr;
    const Json *master_tables = nullptr;
    std::string master_dir;
    std::vector<int32_t> missions;
    int32_t play_level = 26;
    const Json *candidate_members = nullptr;
    const Json *candidate_snaps = nullptr;
};

ProblemView resolve_problem_view(const Json &input) {
    ProblemView view;
    const Json *problem = input.find("problem");
    if (!problem)
        throw SpecError("输入缺少 problem");
    if (problem->is_string())
        view.problem = Json::parse_file(problem->as_string());
    else
        view.problem = *problem;

    const Json *score_mode = view.problem.find("score_mode");
    if (!score_mode) {
        if (const Json *meta = view.problem.find("meta"); meta && meta->is_object()) {
            score_mode = meta->find("score_mode");
            if (!score_mode)
                if (const Json *source = meta->find("source"); source && source->is_object())
                    score_mode = source->find("score_mode");
        }
    }
    std::string mode = json_string(score_mode);
    if (mode != "gekisou")
        throw SpecError(
            "输入 problem 缺少或未启用激奏模式: score_mode === 'gekisou' 必须满足 (当前: '" + mode +
            "')");

    view.chart = input.find("chart");
    if (!view.chart || view.chart->is_null())
        view.chart = view.problem.find("chart");
    view.song = input.find("song");
    if (!view.song || view.song->is_null())
        view.song = view.problem.find("song");
    if (!view.chart)
        throw SpecError("输入 problem 缺少 chart");

    const Json *missions = input.find("missions");
    if (!missions || missions->is_null()) {
        if (view.song && view.song->is_object())
            missions = view.song->find("gekisou_missions");
    }
    if (!missions || !missions->is_array() || missions->size() != 3)
        throw SpecError("激奏必须包含恰好 3 个有效任务 (song.gekisou_missions)");
    for (const Json &mission : missions->as_array()) {
        if (!mission.is_integer() || mission.as_int64() < 1 || mission.as_int64() > 4)
            throw SpecError("任务 ID 必须为 1..4 的整数，收到: " + mission.dump(0));
        view.missions.push_back(static_cast<int32_t>(mission.as_int64()));
    }

    const Json *play_level = input.find("play_level");
    if (!play_level || play_level->is_null())
        view.play_level = row_i32(*view.chart, "level", 0);
    else
        view.play_level = json_i32(play_level, 0);
    if (view.play_level == 0)
        view.play_level = 26;

    view.master_tables = input.find("master_tables");
    if (!view.master_tables || view.master_tables->is_null())
        view.master_tables = view.problem.find("gekisou_master_tables");
    const Json *master_dir = input.find("master_dir");
    if (master_dir && master_dir->is_string())
        view.master_dir = master_dir->as_string();
    if (view.master_dir.empty()) {
        if (const Json *meta = view.problem.find("meta"); meta && meta->is_object())
            if (const Json *source = meta->find("source"); source && source->is_object())
                view.master_dir = json_string(source->find("master_dir"));
    }

    const Json *candidates = input.find("candidates");
    if (candidates && candidates->is_object()) {
        view.candidate_members = candidates->find("members");
        view.candidate_snaps = candidates->find("snaps");
    } else if (const Json *catalog = view.problem.find("catalog");
               catalog && catalog->is_object()) {
        view.candidate_members = catalog->find("members");
        view.candidate_snaps = catalog->find("snapshots");
    }
    return view;
}

struct CatalogMaps {
    std::unordered_map<int32_t, const Json *> members;
    std::unordered_map<int32_t, const Json *> snaps;
};

CatalogMaps catalog_maps(const Json &problem) {
    CatalogMaps maps;
    const Json *catalog = problem.find("catalog");
    if (!catalog || !catalog->is_object())
        return maps;
    if (const Json *members = catalog->find("members"); members && members->is_array())
        for (const Json &row : members->as_array())
            maps.members[row_i32(row, "id")] = &row;
    if (const Json *snaps = catalog->find("snapshots"); snaps && snaps->is_array())
        for (const Json &row : snaps->as_array())
            maps.snaps[row_i32(row, "id")] = &row;
    return maps;
}

PlayerState parse_player(const Json *player) {
    PlayerState out;
    out.vip_rank = 1;
    if (!player || !player->is_object())
        return out;
    out.total_rank = row_i32(*player, "total_rank", 0);
    out.vip_rank = row_i32(*player, "vip_rank", 1);
    auto read_map = [&](const char *key, std::map<int32_t, int32_t> &target) {
        const Json *value = player->find(key);
        if (!value)
            return;
        if (value->is_object()) {
            for (const auto &[id, level] : value->fields())
                target[std::stoi(id)] = json_i32(&level);
        } else if (value->is_array()) {
            for (const Json &row : value->as_array())
                target[row_i32(row, "master_id", 0, "id")] = row_i32(row, "level", 0);
        }
    };
    read_map("character_ranks", out.character_ranks);
    read_map("band_items", out.band_items);
    read_map("character_memory", out.character_memory);
    read_map("music_memory", out.music_memory);
    return out;
}

MusicView parse_music(const Json *song) {
    MusicView out;
    if (!song || !song->is_object())
        return out;
    out.music_type = row_i32(*song, "type");
    out.best_tags = row_list(*song, "tags");
    out.memory_key = row_i32(*song, "id");
    return out;
}

void validate_growth(const Json *growth, const std::string &label) {
    if (!growth || growth->is_null())
        return;
    if (!growth->is_object())
        throw SpecError(label + "成长参数必须为对象");
    for (const auto &[key, value] : growth->fields()) {
        if (!value.is_integer() || value.as_int64() < 1)
            throw SpecError(label + "成长参数 " + key + " 必须为 >= 1 的整数，收到 " +
                            value.dump(0));
    }
}

void validate_skill_support(const EngineMaster &master, const std::vector<MemberState> &members,
                            const std::vector<const SnapState *> &snaps) {
    std::vector<std::string> unsupported;
    for (const auto &member : members) {
        if (member.card.gekiso_skill_id == 0)
            continue;
        auto it = master.gekiso_skills.find(member.card.gekiso_skill_id);
        if (it == master.gekiso_skills.end()) {
            unsupported.push_back("unknown-gekisou-skill:" +
                                  std::to_string(member.card.gekiso_skill_id));
            continue;
        }
        for (const auto &effect : it->second.effects)
            if (!kSupportedEffectTypes.count(effect.type))
                unsupported.push_back(
                    "gekisou-skill:" + std::to_string(member.card.gekiso_skill_id) +
                    ":effect:" + std::to_string(effect.type));
    }
    for (const SnapState *snap : snaps) {
        if (!snap)
            continue;
        for (int32_t skill_id : snap->card.gekiso_support_skill_ids) {
            if (!skill_id)
                continue;
            auto it = master.gekiso_support_skills.find(skill_id);
            if (it == master.gekiso_support_skills.end()) {
                unsupported.push_back("unknown-gekisou-support-skill:" + std::to_string(skill_id));
                continue;
            }
            for (const auto &effect : it->second.effects)
                if (!kSupportedEffectTypes.count(effect.type))
                    unsupported.push_back("gekisou-support-skill:" + std::to_string(skill_id) +
                                          ":effect:" + std::to_string(effect.type));
        }
    }
    if (!unsupported.empty()) {
        std::string text = "包含不受支持的技能效果或条件: ";
        for (size_t i = 0; i < unsupported.size(); ++i) {
            if (i)
                text += ", ";
            text += unsupported[i];
        }
        throw SpecError(text);
    }
}

Json formation_json(const Team &team, const std::vector<MemberState> &members,
                    const std::vector<SnapState> &snaps) {
    Json formation = Json::object();
    formation.set("schema", Json("ournotes-deck-formation@1"));
    std::vector<int64_t> member_ids, snap_ids;
    Json slots = Json::array();
    for (size_t i = 0; i < team.members.size(); ++i) {
        int32_t member_id = members[static_cast<size_t>(team.members[i])].card.id;
        int32_t snap_id = team.snaps[i] < 0 ? 0 : snaps[static_cast<size_t>(team.snaps[i])].card.id;
        member_ids.push_back(member_id);
        snap_ids.push_back(snap_id);
        Json slot = Json::object();
        slot.set("slot", Json(static_cast<int64_t>(i)));
        slot.set("member", Json(member_id));
        slot.set("snapshot", Json(snap_id));
        slot.set("trigger", Json(static_cast<int64_t>(i)));
        slots.push_back(std::move(slot));
    }
    formation.set("leader", Json(member_ids.empty() ? 0 : member_ids.front()));
    formation.set("slots", std::move(slots));
    Json ids = Json::array();
    for (int64_t id : member_ids)
        ids.push_back(Json(id));
    formation.set("member_ids", std::move(ids));
    Json sids = Json::array();
    for (int64_t id : snap_ids)
        sids.push_back(id ? Json(id) : Json());
    formation.set("snap_ids", std::move(sids));
    return formation;
}

Json search_stats_json(const SearchStats &stats) {
    Json out = Json::object();
    out.set("leaders", Json(stats.leaders));
    out.set("memberNodes", Json(stats.member_nodes));
    out.set("snapNodes", Json(stats.snap_nodes));
    out.set("candidates", Json(stats.candidates));
    out.set("exact", Json(stats.exact));
    out.set("elapsedMs", Json(stats.elapsed_ms));
    return out;
}

Json handle_gekisou_score_impl(const Json &input) {
    ProblemView view = resolve_problem_view(input);
    const Json &problem = view.problem;
    const EngineMaster master = load_master(view.master_dir, view.master_tables);
    ChartSource source = normalize_chart_source(*view.chart);
    validate_chart_fevers(source);

    const Json *options = input.find("options");
    int32_t assumed_placement = options ? row_i32(*options, "assumed_placement", 1) : 1;
    if (assumed_placement < 1 || assumed_placement > 5)
        throw SpecError("无效的 assumed_placement: " + std::to_string(assumed_placement) +
                        " (必须在 1..5 范围内)");
    Accuracy accuracy;
    if (options) {
        if (const Json *value = options->find("accuracy"); value && value->is_object()) {
            accuracy.great = row_f64(*value, "great", 0.0) / 100.0;
            accuracy.just = row_f64(*value, "just", 100.0) / 100.0;
        }
    }
    GekisoChart chart = build_gekiso_chart(master, source, view.play_level, view.missions, accuracy,
                                           assumed_placement);

    const Json *formation_input = input.find("formation");
    const Json formation =
        formation_input && formation_input->is_object() ? *formation_input : Json::object();
    const Json *raw_slots = formation.find("slots");
    std::vector<Json> slots;
    if (raw_slots && raw_slots->is_array())
        slots = raw_slots->as_array();
    if (slots.size() != 5)
        throw SpecError("队伍必须包含恰好 5 个槽位，当前数量: " + std::to_string(slots.size()));

    struct NormalizedSlot {
        int32_t member = 0;
        int32_t snapshot = 0;
        Json member_growth;
        Json snap_growth;
        bool has_member_growth = false;
        bool has_snap_growth = false;
    };
    std::vector<NormalizedSlot> normalized;
    for (size_t i = 0; i < slots.size(); ++i) {
        const Json &slot = slots[i];
        NormalizedSlot item;
        const Json *member = slot.find("member");
        if (!member)
            member = slot.find("member_id");
        item.member = json_i32(member);
        const Json *snapshot = slot.find("snapshot");
        if (!snapshot)
            snapshot = slot.find("snap_id");
        item.snapshot = snapshot ? json_i32(snapshot, 0) : 0;
        if (const Json *growth = slot.find("member_growth"); growth && growth->is_object()) {
            item.member_growth = *growth;
            item.has_member_growth = true;
        }
        if (const Json *growth = slot.find("snap_growth"); growth && growth->is_object()) {
            item.snap_growth = *growth;
            item.has_snap_growth = true;
        }
        normalized.push_back(std::move(item));
    }

    std::optional<int32_t> leader_id;
    if (const Json *leader = formation.find("leader"); leader && !leader->is_null())
        leader_id = json_i32(leader);
    if (leader_id) {
        auto it =
            std::find_if(normalized.begin(), normalized.end(),
                         [&](const NormalizedSlot &slot) { return slot.member == *leader_id; });
        if (it == normalized.end())
            throw SpecError("指定的队长卡片 " + std::to_string(*leader_id) + " 不在编队槽位中");
        if (it != normalized.begin())
            std::swap(normalized.front(), *it);
    }

    bool allow_missing = json_truthy(problem.find("experimental_allow_missing_snapshots")) ||
                         (options && json_truthy(options->find("allow_missing_snapshots")));
    CatalogMaps catalog = catalog_maps(problem);
    for (const auto &slot : normalized) {
        if (!catalog.members.empty() && !catalog.members.count(slot.member))
            throw SpecError("编队成员 " + std::to_string(slot.member) +
                            " 不在输入卡池 (catalog) 中");
        if (slot.snapshot > 0 && !catalog.snaps.empty() && !catalog.snaps.count(slot.snapshot))
            throw SpecError("编队 Snapshot " + std::to_string(slot.snapshot) +
                            " 不在输入卡池 (catalog) 中");
        if (slot.snapshot <= 0 && !allow_missing)
            throw SpecError("编队槽位缺少 Snapshot (常规模式每槽必须绑定 Snapshot)");
        if (slot.has_member_growth)
            validate_growth(&slot.member_growth, "成员");
        if (slot.has_snap_growth)
            validate_growth(&slot.snap_growth, "Snapshot ");
    }

    std::vector<MemberInput> member_inputs;
    std::vector<SnapInput> snap_inputs;
    std::vector<std::string> slot_snap_keys;
    for (size_t i = 0; i < normalized.size(); ++i) {
        const auto &slot = normalized[i];
        const Json *category =
            catalog.members.count(slot.member) ? catalog.members.at(slot.member) : nullptr;
        MemberInput member;
        member.key = "m_" + std::to_string(i) + "_" + std::to_string(slot.member);
        member.card_id = slot.member;
        if (slot.has_member_growth) {
            member.level = optional_int(&slot.member_growth, "level");
            member.awake = optional_int(&slot.member_growth, "awake");
            member.rank = optional_int(&slot.member_growth, "rank");
            member.live_skill_level = optional_int(&slot.member_growth, "live_skill_level");
            member.gekiso_skill_level = optional_int(&slot.member_growth, "gekiso_skill_level");
        }
        if (!member.level && category)
            member.level = optional_int(category, "level");
        if (!member.awake && category)
            member.awake = optional_int(category, "awake");
        if (!member.rank && category)
            member.rank = optional_int(category, "rank");
        if (!member.live_skill_level && category)
            member.live_skill_level = optional_int(category, "live_skill_level");
        if (!member.gekiso_skill_level && category)
            member.gekiso_skill_level = optional_int(category, "gekiso_skill_level");
        member_inputs.push_back(std::move(member));

        if (slot.snapshot > 0) {
            std::string key = "s_" + std::to_string(i) + "_" + std::to_string(slot.snapshot);
            SnapInput snap;
            snap.key = key;
            snap.card_id = slot.snapshot;
            const Json *snap_category =
                catalog.snaps.count(slot.snapshot) ? catalog.snaps.at(slot.snapshot) : nullptr;
            if (slot.has_snap_growth) {
                snap.level = optional_int(&slot.snap_growth, "level");
                snap.rank = optional_int(&slot.snap_growth, "rank");
            }
            if (!snap.level && snap_category)
                snap.level = optional_int(snap_category, "level");
            if (!snap.rank && snap_category)
                snap.rank = optional_int(snap_category, "rank");
            snap_inputs.push_back(std::move(snap));
            slot_snap_keys.push_back(key);
        } else {
            slot_snap_keys.push_back("");
        }
    }

    ResolvedBox box = resolve_box(master, member_inputs, snap_inputs);
    if (!box.unknown.empty()) {
        std::string text = "未知的成员或快照卡片: ";
        for (size_t i = 0; i < box.unknown.size(); ++i) {
            if (i)
                text += ", ";
            text += box.unknown[i];
        }
        throw SpecError(text);
    }
    std::set<int32_t> characters;
    for (const auto &member : box.members)
        characters.insert(member.card.character_id);
    if (characters.size() != 5)
        throw SpecError("编队中存在重复角色 (唯一点数: " + std::to_string(characters.size()) + ")");
    std::set<int32_t> snap_cards;
    for (const auto &snap : box.snaps)
        if (snap.card.id > 0 && !snap_cards.insert(snap.card.id).second)
            throw SpecError("编队中存在重复 Snapshot");

    std::unordered_map<std::string, const SnapState *> snap_map;
    for (const auto &snap : box.snaps)
        snap_map[snap.key] = &snap;
    std::vector<const SnapState *> slot_snaps;
    for (const auto &key : slot_snap_keys) {
        auto it = snap_map.find(key);
        slot_snaps.push_back(key.empty() || it == snap_map.end() ? nullptr : it->second);
    }

    validate_skill_support(master, box.members, slot_snaps);
    std::vector<SlotSkill> slot_skills;
    for (size_t i = 0; i < box.members.size(); ++i)
        slot_skills.push_back(resolve_slot_skill(master, box.members[i], slot_snaps[i]));
    std::vector<Performer> deck;
    for (size_t i = 0; i < box.members.size(); ++i)
        deck.push_back(make_performer(master, box.members[i], slot_snaps[i]));

    std::vector<int32_t> seeds;
    if (options) {
        const Json *values = options->find("seeds");
        if (values && !values->is_null()) {
            if (!values->is_array())
                throw SpecError("seeds 必须为数组");
            for (const Json &value : values->as_array()) {
                if (!value.is_integer() || value.as_int64() < 0)
                    throw SpecError("seeds 元素必须为非负整数: " + value.dump(0));
                seeds.push_back(static_cast<int32_t>(value.as_int64()));
            }
        }
    }
    std::string luck_mode = !chart.luck     ? "nominal_model_exact"
                            : seeds.empty() ? "nominal_expectation"
                                            : "sample_mean";
    std::unordered_map<std::string, LuckCurve> curve_cache;
    GekisoContext context = gekiso_context(master, chart, deck, seeds, curve_cache);

    PlayerState player = parse_player(input.find("player"));
    MusicView music = parse_music(view.song);
    double power = 0.0;
    std::vector<double> slot_powers;
    bool has_slot_powers = false;
    if (const Json *power_input = input.find("power"); power_input && power_input->is_number()) {
        power = power_input->as_double();
    } else {
        TeamPower result = team_power(&problem, master, player, box.members, slot_snaps, 0, &music);
        power = result.total;
        slot_powers = result.slots;
        has_slot_powers = true;
    }
    OrderScores scores = score_orders_gekiso(master, chart, context, power, slot_skills);

    Json out = Json::object();
    out.set("status", Json("ok"));
    out.set("mean", Json(scores.mean));
    out.set("max", Json(scores.max));
    out.set("min", Json(scores.min));
    out.set("max_order", Json(static_cast<int64_t>(scores.max_order)));
    out.set("min_order", Json(static_cast<int64_t>(scores.min_order)));
    Json order_scores = Json::array();
    for (double value : scores.scores)
        order_scores.push_back(Json(value));
    out.set("order_scores", std::move(order_scores));
    out.set("power", Json(power));
    if (has_slot_powers) {
        Json values = Json::array();
        for (double value : slot_powers)
            values.push_back(Json(value));
        out.set("slot_powers", std::move(values));
    } else {
        out.set("slot_powers", Json());
    }
    out.set("assumed_placement", Json(assumed_placement));
    Json seed_values = Json::array();
    for (int32_t seed : seeds)
        seed_values.push_back(Json(seed));
    out.set("seeds", std::move(seed_values));
    out.set("luck_mode", Json(luck_mode));
    Json details = Json::object();
    Json range_percents = Json::array();
    for (int32_t value : chart.range_percent)
        range_percents.push_back(Json(value));
    details.set("range_percents", std::move(range_percents));
    Json span = Json::array();
    span.push_back(Json(static_cast<int64_t>(chart.span.first)));
    span.push_back(Json(static_cast<int64_t>(chart.span.second)));
    details.set("span", std::move(span));
    details.set("notes_count", Json(static_cast<int64_t>(chart.notes.size())));
    details.set("luck_present", Json(chart.luck));
    out.set("details", std::move(details));
    return out;
}

Json handle_gekisou_rank_impl(const Json &input) {
    ProblemView view = resolve_problem_view(input);
    const Json &problem = view.problem;
    const EngineMaster master = load_master(view.master_dir, view.master_tables);
    ChartSource source = normalize_chart_source(*view.chart);
    validate_chart_fevers(source);

    const Json *options = input.find("options");
    int32_t assumed_placement = options ? row_i32(*options, "assumed_placement", 1) : 1;
    if (assumed_placement < 1 || assumed_placement > 5)
        throw SpecError("无效的 assumed_placement: " + std::to_string(assumed_placement) +
                        " (必须在 1..5 范围内)");
    Accuracy accuracy;
    if (options) {
        if (const Json *value = options->find("accuracy"); value && value->is_object()) {
            accuracy.great = row_f64(*value, "great", 0.0) / 100.0;
            accuracy.just = row_f64(*value, "just", 100.0) / 100.0;
        }
    }
    GekisoChart chart = build_gekiso_chart(master, source, view.play_level, view.missions, accuracy,
                                           assumed_placement);
    CatalogMaps catalog = catalog_maps(problem);
    const Json *cand_members = view.candidate_members;
    const Json *cand_snaps = view.candidate_snaps;

    if (cand_members && cand_members->is_array()) {
        std::set<int32_t> seen;
        for (const Json &row : cand_members->as_array()) {
            int32_t id = row_i32(row, "id");
            if (!catalog.members.empty() && !catalog.members.count(id))
                throw SpecError("候选成员卡 " + std::to_string(id) + " 不在输入卡池 (catalog) 中");
            if (!seen.insert(id).second)
                throw SpecError("候选成员卡列表中存在重复 ID: " + std::to_string(id));
        }
    }
    if (cand_snaps && cand_snaps->is_array()) {
        std::set<int32_t> seen;
        for (const Json &row : cand_snaps->as_array()) {
            int32_t id = row_i32(row, "id");
            if (!catalog.snaps.empty() && !catalog.snaps.count(id))
                throw SpecError("候选 Snapshot " + std::to_string(id) +
                                " 不在输入卡池 (catalog) 中");
            if (!seen.insert(id).second)
                throw SpecError("候选 Snapshot 列表中存在重复 ID: " + std::to_string(id));
        }
    }

    std::vector<MemberInput> member_inputs;
    if (cand_members && cand_members->is_array()) {
        size_t index = 0;
        for (const Json &row : cand_members->as_array()) {
            int32_t id = row_i32(row, "id");
            const Json *category = catalog.members.count(id) ? catalog.members.at(id) : nullptr;
            MemberInput member;
            member.key = "m_" + std::to_string(index++) + "_" + std::to_string(id);
            member.card_id = id;
            const Json *growth = row.find("growth");
            member.level = optional_int(&row, "level");
            if (!member.level && growth)
                member.level = optional_int(growth, "level");
            if (!member.level && category)
                member.level = optional_int(category, "level");
            member.awake = optional_int(&row, "awake");
            if (!member.awake && growth)
                member.awake = optional_int(growth, "awake");
            if (!member.awake && category)
                member.awake = optional_int(category, "awake");
            member.rank = optional_int(&row, "rank");
            if (!member.rank && growth)
                member.rank = optional_int(growth, "rank");
            if (!member.rank && category)
                member.rank = optional_int(category, "rank");
            member.live_skill_level = optional_int(&row, "live_skill_level");
            if (!member.live_skill_level && growth)
                member.live_skill_level = optional_int(growth, "live_skill_level");
            if (!member.live_skill_level && category)
                member.live_skill_level = optional_int(category, "live_skill_level");
            member.gekiso_skill_level = optional_int(&row, "gekiso_skill_level");
            if (!member.gekiso_skill_level && growth)
                member.gekiso_skill_level = optional_int(growth, "gekiso_skill_level");
            if (!member.gekiso_skill_level && category)
                member.gekiso_skill_level = optional_int(category, "gekiso_skill_level");
            member_inputs.push_back(std::move(member));
        }
    }
    std::vector<SnapInput> snap_inputs;
    if (cand_snaps && cand_snaps->is_array()) {
        size_t index = 0;
        for (const Json &row : cand_snaps->as_array()) {
            int32_t id = row_i32(row, "id");
            const Json *category = catalog.snaps.count(id) ? catalog.snaps.at(id) : nullptr;
            SnapInput snap;
            snap.key = "s_" + std::to_string(index++) + "_" + std::to_string(id);
            snap.card_id = id;
            const Json *growth = row.find("growth");
            snap.level = optional_int(&row, "level");
            if (!snap.level && growth)
                snap.level = optional_int(growth, "level");
            if (!snap.level && category)
                snap.level = optional_int(category, "level");
            snap.rank = optional_int(&row, "rank");
            if (!snap.rank && growth)
                snap.rank = optional_int(growth, "rank");
            if (!snap.rank && category)
                snap.rank = optional_int(category, "rank");
            snap_inputs.push_back(std::move(snap));
        }
    }

    ResolvedBox box = resolve_box(master, member_inputs, snap_inputs);
    if (!box.unknown.empty()) {
        std::string text = "未知的候选卡片: ";
        for (size_t i = 0; i < box.unknown.size(); ++i) {
            if (i)
                text += ", ";
            text += box.unknown[i];
        }
        throw SpecError(text);
    }
    bool allow_missing = json_truthy(problem.find("experimental_allow_missing_snapshots")) ||
                         (options && json_truthy(options->find("allow_missing_snapshots")));
    if (!allow_missing && box.snaps.size() < 5)
        throw SpecError("候选 Snapshot 数量不足 5 张 (当前: " + std::to_string(box.snaps.size()) +
                        "，常规激奏每槽必须绑定 Snapshot)");
    std::vector<const SnapState *> all_snaps;
    for (const auto &snap : box.snaps)
        all_snaps.push_back(&snap);
    validate_skill_support(master, box.members, all_snaps);

    PlayerState player = parse_player(input.find("player"));
    std::string criterion = criterion_key(json_string(input.find("criterion"), "mean"));
    std::unordered_map<int32_t, int> member_index;
    for (size_t i = 0; i < box.members.size(); ++i)
        member_index[box.members[i].card.id] = static_cast<int>(i);
    std::unordered_map<int32_t, int> snap_index;
    for (size_t i = 0; i < box.snaps.size(); ++i)
        snap_index[box.snaps[i].card.id] = static_cast<int>(i);

    const Json *prob_constraints = problem.find("constraints");
    const Json *cli_constraints = input.find("constraints");
    const Json empty_object = Json::object();
    const Json empty_array = Json::array();
    if (!prob_constraints || !prob_constraints->is_object())
        prob_constraints = &empty_object;
    if (!cli_constraints || !cli_constraints->is_object())
        cli_constraints = &empty_object;
    if (json_truthy(prob_constraints->find("distinct_characters"), true) == false)
        throw SpecError("激奏模式必须满足角色互异 (不支持 distinct_characters=false)");
    if (json_truthy(prob_constraints->find("distinct_snapshots"), true) == false)
        throw SpecError("激奏模式必须满足 Snapshot 互异 (不支持 distinct_snapshots=false)");
    if (const Json *bindings = prob_constraints->find("bindings");
        bindings && bindings->is_object() && bindings->size() > 0)
        throw SpecError("激奏组队暂不支持 bindings 约束");
    if (const Json *pool = prob_constraints->find("member_pool");
        pool && pool->is_array() && pool->size() > 0)
        throw SpecError("激奏暂不支持 member_pool 约束，请直接通过候选卡池过滤");
    if (const Json *pool = prob_constraints->find("snapshot_pool");
        pool && pool->is_array() && pool->size() > 0)
        throw SpecError("激奏暂不支持 snapshot_pool 约束，请直接通过候选卡池过滤");
    if (const Json *pool = prob_constraints->find("leader_pool");
        pool && pool->is_array() && pool->size() > 1)
        throw SpecError("激奏暂不支持多选队长池 (leader_pool 长度 > 1)");
    if (const Json *pool = prob_constraints->find("leader_candidates");
        pool && pool->is_array() && pool->size() > 1)
        throw SpecError("激奏暂不支持多候选队长 (leader_candidates 长度 > 1)");

    auto nonempty_or_null = [](const Json *value) -> const Json * {
        return value && value->is_array() && value->size() > 0 ? value : nullptr;
    };
    const Json *raw_req_m = nonempty_or_null(cli_constraints->find("required_members"));
    if (!raw_req_m)
        raw_req_m = prob_constraints->find("required_members");
    if (!raw_req_m)
        raw_req_m = &empty_array;
    const Json *raw_exc_m = nonempty_or_null(cli_constraints->find("excluded_members"));
    if (!raw_exc_m)
        raw_exc_m = prob_constraints->find("excluded_members");
    if (!raw_exc_m)
        raw_exc_m = prob_constraints->find("banned_members");
    if (!raw_exc_m)
        raw_exc_m = &empty_array;
    const Json *raw_req_s = nonempty_or_null(cli_constraints->find("required_snapshots"));
    if (!raw_req_s)
        raw_req_s = nonempty_or_null(cli_constraints->find("required_snaps"));
    if (!raw_req_s)
        raw_req_s = prob_constraints->find("required_snapshots");
    if (!raw_req_s)
        raw_req_s = prob_constraints->find("required_snaps");
    if (!raw_req_s)
        raw_req_s = &empty_array;
    const Json *raw_exc_s = nonempty_or_null(cli_constraints->find("excluded_snapshots"));
    if (!raw_exc_s)
        raw_exc_s = nonempty_or_null(cli_constraints->find("excluded_snaps"));
    if (!raw_exc_s)
        raw_exc_s = prob_constraints->find("excluded_snapshots");
    if (!raw_exc_s)
        raw_exc_s = prob_constraints->find("excluded_snaps");
    if (!raw_exc_s)
        raw_exc_s = prob_constraints->find("banned_snapshots");
    if (!raw_exc_s)
        raw_exc_s = &empty_array;

    std::vector<int32_t> req_m = json_id_list(raw_req_m);
    std::vector<int32_t> exc_m = json_id_list(raw_exc_m);
    std::vector<int32_t> req_s = json_id_list(raw_req_s);
    std::vector<int32_t> exc_s = json_id_list(raw_exc_s);
    std::optional<int32_t> raw_leader;
    if (const Json *leader = cli_constraints->find("leader"); leader && !leader->is_null())
        raw_leader = json_i32(leader);
    if (!raw_leader)
        if (const Json *leader = prob_constraints->find("leader"); leader && !leader->is_null())
            raw_leader = json_i32(leader);
    if (!raw_leader) {
        if (const Json *pool = prob_constraints->find("leader_pool");
            pool && pool->is_array() && pool->size() == 1)
            raw_leader = json_i32(&pool->at(0));
    }
    if (!raw_leader) {
        if (const Json *pool = prob_constraints->find("leader_candidates");
            pool && pool->is_array() && pool->size() == 1)
            raw_leader = json_i32(&pool->at(0));
    }

    for (int32_t id : req_m)
        if (!member_index.count(id))
            throw SpecError("约束中的成员卡不存在: " + std::to_string(id));
    for (int32_t id : exc_m)
        if (!member_index.count(id))
            throw SpecError("约束中的成员卡不存在: " + std::to_string(id));
    for (int32_t id : req_s)
        if (!snap_index.count(id))
            throw SpecError("约束中的 Snapshot 不存在: " + std::to_string(id));
    for (int32_t id : exc_s)
        if (!snap_index.count(id))
            throw SpecError("约束中的 Snapshot 不存在: " + std::to_string(id));
    if (raw_leader && !member_index.count(*raw_leader))
        throw SpecError("约束中的队长卡不存在: " + std::to_string(*raw_leader));
    if (req_m.size() > 5)
        throw SpecError("必选成员卡数量不能超过 5 张 (当前: " + std::to_string(req_m.size()) + ")");
    for (int32_t id : req_m)
        if (std::find(exc_m.begin(), exc_m.end(), id) != exc_m.end())
            throw SpecError("成员卡 " + std::to_string(id) + " 同时出现在必选和排除列表中");
    for (int32_t id : req_s)
        if (std::find(exc_s.begin(), exc_s.end(), id) != exc_s.end())
            throw SpecError("Snapshot " + std::to_string(id) + " 同时出现在必选和排除列表中");
    if (raw_leader && std::find(exc_m.begin(), exc_m.end(), *raw_leader) != exc_m.end())
        throw SpecError("队长卡片 " + std::to_string(*raw_leader) + " 不能在排除列表中");
    if (raw_leader && req_m.size() == 5 &&
        std::find(req_m.begin(), req_m.end(), *raw_leader) == req_m.end())
        throw SpecError("必选成员已有 5 人且不包含指定队长 " + std::to_string(*raw_leader));

    std::set<int32_t> req_characters;
    for (int32_t id : req_m) {
        int32_t character = box.members[static_cast<size_t>(member_index.at(id))].card.character_id;
        if (!req_characters.insert(character).second)
            throw SpecError("必选成员中存在重复角色");
    }
    if (raw_leader && std::find(req_m.begin(), req_m.end(), *raw_leader) == req_m.end()) {
        int32_t leader_character =
            box.members[static_cast<size_t>(member_index.at(*raw_leader))].card.character_id;
        if (req_characters.count(leader_character))
            throw SpecError("指定队长 " + std::to_string(*raw_leader) +
                            " 的角色与必选成员角色冲突");
    }

    SearchConstraints constraints;
    for (int32_t id : req_m)
        constraints.required_members.push_back(member_index.at(id));
    for (int32_t id : exc_m)
        constraints.excluded_members.push_back(member_index.at(id));
    for (int32_t id : req_s)
        constraints.required_snaps.push_back(snap_index.at(id));
    for (int32_t id : exc_s)
        constraints.excluded_snaps.push_back(snap_index.at(id));
    if (raw_leader)
        constraints.leader = member_index.at(*raw_leader);

    std::vector<int32_t> seeds;
    if (options) {
        const Json *values = options->find("seeds");
        if (values && !values->is_null()) {
            if (!values->is_array())
                throw SpecError("seeds 必须为数组");
            for (const Json &value : values->as_array()) {
                if (!value.is_integer() || value.as_int64() < 0)
                    throw SpecError("seeds 元素必须为非负整数: " + value.dump(0));
                seeds.push_back(static_cast<int32_t>(value.as_int64()));
            }
        }
    }
    std::string luck_mode = !chart.luck     ? "nominal_model_exact"
                            : seeds.empty() ? "nominal_expectation"
                                            : "sample_mean";
    int32_t k = 10;
    if (const Json *value = input.find("k"); value && !value->is_null()) {
        if (!value->is_integer())
            throw SpecError("k (Top-K) 必须为 > 0 的正整数");
        const int64_t requested = value->as_int64();
        if (requested < 0)
            throw SpecError("k (Top-K) 必须为 > 0 的正整数");
        if (requested > 0)
            k = static_cast<int32_t>(requested);
    }
    double time_limit_ms = 0.0;
    if (options) {
        if (const Json *value = options->find("time_limit_ms"); value && !value->is_null()) {
            if (!value->is_number())
                throw SpecError("time_limit_ms 不能为负数");
            time_limit_ms = value->as_double();
            if (time_limit_ms < 0)
                throw SpecError("time_limit_ms 不能为负数");
        }
    }

    MusicView music = parse_music(view.song);
    bool oracle = json_truthy(input.find("oracle"));
    SearchOutput search;
    if (oracle) {
        int32_t available = 0;
        for (int j = 0; j < static_cast<int>(box.snaps.size()); ++j)
            if (std::find(constraints.excluded_snaps.begin(), constraints.excluded_snaps.end(),
                          j) == constraints.excluded_snaps.end())
                ++available;
        if (available < 5 && !allow_missing)
            throw SpecError("可用 Snapshot 数量不足 5 张 (当前: " + std::to_string(available) +
                            ")");
        double c_members = 1.0;
        int num_members = static_cast<int>(box.members.size());
        for (int i = 0; i < 5; ++i)
            c_members = c_members * (num_members - i) / (i + 1);
        double leader_ways = constraints.leader ? 1.0 : 5.0;
        double snap_ways = 0.0;
        if (available >= 5) {
            for (int i = 0; i < 5; ++i)
                snap_ways *= available - i;
        } else {
            for (int sc = 0; sc <= available; ++sc) {
                double comb = 1.0;
                for (int i = 0; i < sc; ++i)
                    comb = comb * (5 - i) / (i + 1);
                double perm = 1.0;
                for (int i = 0; i < sc; ++i)
                    perm *= available - i;
                snap_ways += comb * perm;
            }
        }
        double estimated = c_members * leader_ways * snap_ways;
        if (estimated > 50000.0)
            throw SpecError("Oracle 穷举组合数超过上限 50000 (估算: " +
                            std::to_string(static_cast<int64_t>(estimated)) +
                            ")，请缩小候选池或使用 search");
        search = gekiso_search(master, player, box.members, box.snaps, &music, chart, seeds,
                               criterion, constraints, 1000000, 1e12, &problem, available < 5);
    } else {
        search =
            gekiso_search(master, player, box.members, box.snaps, &music, chart, seeds, criterion,
                          constraints, std::max(k, 10), time_limit_ms > 0 ? time_limit_ms : 10000.0,
                          &problem, allow_missing || box.snaps.size() < 5);
    }

    std::unordered_map<std::string, LuckCurve> curve_cache;
    auto rescore = [&](const Team &team) {
        std::vector<MemberState> ordered;
        std::vector<const SnapState *> selected;
        std::vector<SlotSkill> skills;
        for (size_t slot = 0; slot < team.members.size(); ++slot) {
            const MemberState &member = box.members[static_cast<size_t>(team.members[slot])];
            const SnapState *snap =
                team.snaps[slot] < 0 ? nullptr : &box.snaps[static_cast<size_t>(team.snaps[slot])];
            ordered.push_back(member);
            selected.push_back(snap);
            skills.push_back(resolve_slot_skill(master, member, snap));
        }
        std::vector<Performer> deck;
        for (size_t slot = 0; slot < ordered.size(); ++slot)
            deck.push_back(make_performer(master, ordered[slot], selected[slot]));
        GekisoContext context = gekiso_context(master, chart, deck, seeds, curve_cache);
        TeamPower power = team_power(&problem, master, player, ordered, selected, 0, &music);
        OrderScores scores = score_orders_gekiso(master, chart, context, power.total, skills);
        return std::make_pair(power.total, scores);
    };

    Json hits = Json::array();
    std::vector<SearchHit> sorted = search.hits;
    std::sort(sorted.begin(), sorted.end(), [&](const SearchHit &a, const SearchHit &b) {
        return order_key(a.detail.scores, criterion) > order_key(b.detail.scores, criterion);
    });
    const size_t limit = std::min(static_cast<size_t>(k), sorted.size());
    for (size_t i = 0; i < limit; ++i) {
        auto [power, scores] = rescore(sorted[i].team);
        Json hit = Json::object();
        hit.set("rank", Json(static_cast<int64_t>(i + 1)));
        hit.set("score", Json(order_key(scores, criterion)));
        hit.set("power", Json(power));
        hit.set("formation", formation_json(sorted[i].team, box.members, box.snaps));
        Json score = Json::object();
        score.set("mean", Json(scores.mean));
        score.set("max", Json(scores.max));
        score.set("min", Json(scores.min));
        hit.set("scores", std::move(score));
        hits.push_back(std::move(hit));
    }
    if (!allow_missing && !oracle) {
        for (const Json &hit : hits.items()) {
            const Json *slots = hit.find("formation")->find("slots");
            for (const Json &slot : slots->items())
                if (row_i32(slot, "snapshot") <= 0)
                    throw SpecError("编队槽位缺少 Snapshot (常规模式每槽必须绑定 Snapshot)");
        }
    }

    Json out = Json::object();
    out.set("status", Json(hits.size() > 0 ? "ok" : (search.proven ? "ok" : "incomplete")));
    out.set("hits", std::move(hits));
    if (oracle) {
        out.set("search_complete", Json(search.proven));
        out.set("optimality_certified", Json(search.proven));
        std::string scope;
        if (!search.proven)
            scope = "incomplete_oracle";
        else if (seeds.empty() || !chart.luck)
            scope = "exact_global_oracle";
        else
            scope = "exact_seeded_oracle";
        out.set("optimality_scope", Json(scope));
        out.set("bound", Json(search.hits.empty()
                                  ? 0.0
                                  : order_key(search.hits.front().detail.scores, criterion)));
        Json stats = Json::object();
        stats.set("total_teams", Json(search.stats.exact));
        stats.set("member_sets", Json(static_cast<int64_t>(search.hits.size())));
        stats.set("timed_out", Json(!search.proven));
        out.set("stats", std::move(stats));
    } else {
        out.set("search_complete", Json(false));
        out.set("optimality_certified", Json(false));
        out.set("optimality_scope", Json("heuristic_candidates"));
        double bound = 0.0;
        if (const Json *first = out.find("hits"); first && first->is_array() && first->size() > 0)
            bound = row_f64(first->at(0), "score", 0.0);
        out.set("bound", Json(bound));
        out.set("stats", search_stats_json(search.stats));
    }
    Json seed_values = Json::array();
    for (int32_t seed : seeds)
        seed_values.push_back(Json(seed));
    out.set("luck_mode", Json(luck_mode));
    out.set("seeds", std::move(seed_values));
    return out;
}

} // namespace

Json handle_gekisou_score(const Json &input) {
    return handle_gekisou_score_impl(input);
}

Json handle_gekisou_rank(const Json &input) {
    return handle_gekisou_rank_impl(input);
}

} // namespace deckcalc
