#pragma once

#include "gekisou.hpp"
#include "model.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace deckcalc {
namespace gekisou_detail {

// 激奏（Gekisou / Fever Mission）内部模块共享声明。
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

inline constexpr std::array<const char *, 36> kAll36Tables = {
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

inline const std::unordered_set<int32_t> kSupportedEffectTypes = {
    0,     1000,  1001,  1002,  1003,  1500,  1501,  1502,  1503,  2000,  2001,  2002,  2003,
    2004,  2005,  3000,  3001,  3002,  3003,  3004,  4004,  11000, 11001, 11002, 11003, 11004,
    11005, 12000, 12002, 12003, 12004, 12006, 13000, 13002, 13003, 13004, 13005, 15000,
};

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

struct ResolvedBox {
    std::vector<MemberState> members;
    std::vector<SnapState> snaps;
    std::vector<std::string> unknown;
};

struct TeamPowerResult {
    double total = 0.0;
    std::vector<double> slots;
};

struct PrecomputedProbPower {
    enum class RoundMode { Float32Floor, Float64Floor, Round, None };
    enum class TypeLinkSrc { Snapshot, Member, Both, Other };

    bool valid = false;
    bool en_trained = true;
    bool en_char_rank = true;
    bool en_char_total_rank = true;
    bool en_snapshot = true;
    bool en_type_link = true;
    bool en_leader = true;
    bool en_band_item = true;
    bool en_music_type = true;
    bool en_music_tag = true;
    bool en_vip = true;
    bool apply_event_power = true;
    RoundMode round_mode = RoundMode::Float32Floor;
    TypeLinkSrc tl_source = TypeLinkSrc::Snapshot;

    int32_t song_type = 0;
    std::vector<int32_t> song_tags;

    std::array<int64_t, 3> def_cr{0, 0, 0};
    std::array<int64_t, 3> def_ctr{0, 0, 0};
    std::array<int64_t, 3> def_bi{0, 0, 0};
    std::array<int64_t, 3> def_vip{0, 0, 0};
    int64_t mt_base_bp = 0;
    int64_t mtag_base_bp = 0;
    int64_t tl_base_bp = 0;

    std::unordered_map<int32_t, std::array<int64_t, 3>> cr_by_char;
    std::unordered_map<int32_t, std::array<int64_t, 3>> bi_by_char;
    std::unordered_map<int32_t, std::array<int64_t, 3>> bi_by_band;
    std::unordered_map<int32_t, std::array<int64_t, 3>> bi_by_ct;

    struct ExtraSrc {
        bool is_flat = false;
        std::array<int64_t, 3> vals{0, 0, 0};
    };
    std::vector<ExtraSrc> extra_sources;

    struct CatMember {
        std::optional<int32_t> leader_skill;
        int64_t ev_bp = 0;
        bool has_crb = false;
        std::optional<int32_t> crb_mt;
        std::optional<int32_t> crb_mtag;
        int64_t crb_tl = 0;
    };
    struct CatSnap {
        int64_t ev_bp = 0;
        std::optional<int32_t> rank;
        bool has_crb = false;
        std::optional<int32_t> crb_tl;
    };
    struct CatTarget {
        int32_t band = 0;
        int32_t character = 0;
        int32_t card_type = 0;
        int32_t tag = 0;
        int32_t gekisou_mission_type = 0;
        int32_t music_type = 0;
        std::vector<int32_t> live_skill_categories;
        std::vector<int32_t> gekisou_skill_categories;
    };
    struct CatCond {
        int32_t condition_type = 0;
        bool is_pos = true;
        std::vector<int32_t> targets;
    };
    struct CatLeaderEff {
        int32_t level = 0;
        int32_t condition_group = 0;
        int32_t effect_type = 0;
        int32_t cumulative_type = 0;
        int32_t max_cumulative_count = 0;
        int32_t value = 0;
        int32_t max_value = 0;
        std::vector<int32_t> cumulative_targets;
        std::vector<int32_t> targets;
    };

    std::unordered_map<int32_t, CatMember> cat_members;
    std::unordered_map<int32_t, CatSnap> cat_snaps;
    std::unordered_map<int32_t, CatTarget> target_map;
    std::unordered_map<int32_t, CatCond> cond_map;
    std::unordered_map<int32_t, std::vector<std::vector<int32_t>>> cg_map;
    std::unordered_map<int32_t, int32_t> gk_mission_by_skill;
    std::unordered_map<int32_t, std::vector<CatLeaderEff>> leader_skills;

    explicit PrecomputedProbPower(const Json *prob);
    double round_val(double value) const;
    double apply_rate(double base_val, int64_t rate_bp) const;
    bool match_cat_targets(const MemberState &m, const std::vector<int32_t> &tids) const;
    std::vector<std::array<int64_t, 3>>
    compute_leader_rates(const std::vector<MemberState> &members, size_t leader_idx) const;
    std::array<double, 3> member_base_vals(const MemberState &m) const;
    double member_fixed_power(const MemberState &m) const;
    double snap_add_power(const MemberState &m, const SnapState *s) const;
    std::vector<double> leader_slot_powers(const std::vector<MemberState> &members,
                                           size_t leader_idx) const;
    TeamPowerResult calc(const std::vector<MemberState> &members,
                         const std::vector<const SnapState *> &snaps, size_t leader_idx) const;
};

// JSON helpers & Master/Box resolution (gekisou_master.cpp)
const Json *row_field(const Json &row, const std::string &name, const std::string &alt = "");
double json_num(const Json *val, double fallback = 0.0);
int32_t json_i32(const Json *val, int32_t fallback = 0);
int32_t row_i32(const Json &row, const std::string &name, int32_t fallback = 0,
                const std::string &alt = "");
double row_f64(const Json &row, const std::string &name, double fallback = 0.0,
               const std::string &alt = "");
std::string row_str(const Json &row, const std::string &name, const std::string &fallback = "");
std::vector<int32_t> row_ids(const Json &row, const std::string &name, const std::string &alt = "");
std::vector<int32_t> row_list(const Json &row, const std::string &name,
                              const std::string &alt = "");
EngineMaster load_master(const std::string &master_dir, const Json *tables_map);
ResolvedBox resolve_box(const EngineMaster &master, const std::vector<MemberInput> &member_inputs,
                        const std::vector<SnapInput> &snap_inputs);
bool is_target_member(const MemberState &member, const TargetRow &target);
bool matches_targets(const EngineMaster &master, const MemberState &member,
                     const std::vector<int32_t> &target_ids);
TeamPowerResult calc_deckcalc_team_power(const Json *prob, const EngineMaster &master,
                                         const std::vector<MemberState> &members,
                                         const std::vector<const SnapState *> &snaps,
                                         size_t leader_idx);

// Shared simulator & scoring constants/types
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

inline int32_t minimum_result_of(int32_t v) {
    return (v >= 2 && v <= 4) ? (v - 1) : 0;
}

struct MissRule {
    int32_t value = 0;
    double chance = 0.0;
};

struct LuckCurve {
    std::unordered_map<int32_t, double> rush;
    std::unordered_map<int32_t, double> skill;
    std::unordered_map<int32_t, double> both;
};

// Live frame simulator entrypoints (gekisou_sim.cpp)
std::pair<float, std::vector<MissRule>> compute_luck_readers(const EngineMaster &master,
                                                             const std::vector<Performer> &deck);
void simulate_live_span(
    const EngineMaster &master, const std::vector<Performer> &deck,
    const std::vector<LiveNote> &notes, const std::vector<std::pair<int32_t, int32_t>> &events,
    const LiveParams &params, const GekisouSetup *setup, int32_t seed,
    const std::vector<PlayFrame> &frames, const std::vector<float> &dt, size_t span_begin,
    size_t span_end, LuckRecord *record,
    const std::function<void(int32_t, int32_t, float, float, float, float)> &on_note);

// Scoring types & entrypoints (gekisou_scoring.cpp)
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
    std::unordered_map<int32_t, size_t> note_id_to_index;
};

struct GekisoContext {
    std::vector<int8_t> judgement;
    std::vector<float> gk;
    std::vector<double> up;
    std::vector<double> luck;
    size_t seeds = 0;
};

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

struct OrderScores {
    double power = 0.0;
    std::array<double, 120> scores{};
    double mean = 0.0;
    double min = 0.0;
    double max = 0.0;
    size_t min_order = 0;
    size_t max_order = 0;
};

GekisoChart build_gekiso_chart(const EngineMaster &master, const ChartSource &source, int32_t level,
                               const std::vector<int32_t> &missions, const Accuracy &accuracy,
                               int32_t rank = 1);
GekisoContext gekiso_context(const EngineMaster &master, const GekisoChart &chart,
                             const std::vector<Performer> &deck, const std::vector<int32_t> &seeds,
                             std::unordered_map<std::string, LuckCurve> &curve_cache);
SlotSkill resolve_slot_skill(const EngineMaster &master, const MemberState &member,
                             const SnapState *snap,
                             std::vector<std::string> *unsupported = nullptr);
const std::vector<std::array<int32_t, 5>> &all_orders();
bool equal_slot_skill(const SlotSkill &a, const SlotSkill &b);
OrderScores score_orders_gekiso(const EngineMaster &master, const GekisoChart &chart,
                                const GekisoContext &ctx, double power,
                                const std::vector<SlotSkill> &slots);

// Power & team search types & entrypoints (gekisou_search.cpp)
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

struct TeamPower {
    double total = 0.0;
    std::vector<double> slots;
    std::vector<BP3> leader_percents;
};

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
    std::string set_key;
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
    const PrecomputedProbPower *pre_power = nullptr;
    const std::vector<int> *snap_skill_class = nullptr;
    bool prune_snap_classes = false;
    std::function<std::pair<double, double>(int, int)> skill;
    std::function<double(int)> member_bonus;
    std::function<double(int)> snap_bonus;
    std::function<double(const Totals &)> bound;
    std::function<std::pair<double, double>(const Team &, const Totals &)> interval;
    std::function<SearchExact(const Team &)> exact;
    std::function<SearchExact(const Team &, double, const std::vector<double> &)> exact_with_power;
    SearchConstraints constraints;
    int k = 10;
    double time_limit_ms = 0.0;
    std::optional<int> shard_index;
    std::optional<int> shard_count;
    std::optional<double> floor;
};

TeamPower team_power(const Json *prob, const EngineMaster &master, const PlayerState &player,
                     const std::vector<MemberState> &members,
                     const std::vector<const SnapState *> &snaps, size_t leader_idx,
                     const MusicView *music);
std::string criterion_key(const std::string &criterion);
double order_key(const OrderScores &scores, const std::string &criterion);
SearchOutput gekiso_search(const EngineMaster &master, const PlayerState &player,
                           const std::vector<MemberState> &members,
                           const std::vector<SnapState> &snaps, const MusicView *music,
                           const GekisoChart &chart, const std::vector<int32_t> &seeds,
                           const std::string &criterion, const SearchConstraints &constraints,
                           int k, double time_limit_ms, const Json *prob,
                           bool allow_empty_snaps = false, bool is_oracle = false);

} // namespace gekisou_detail
} // namespace deckcalc
