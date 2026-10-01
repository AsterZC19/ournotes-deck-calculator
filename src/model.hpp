// problem / formation 的结构与解析。
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"

namespace deckcalc {

class SpecError : public std::runtime_error {
public:
    explicit SpecError(const std::string& message) : std::runtime_error(message) {}
};

inline constexpr const char* kSchemaProblem = "ournotes-deck-problem@1";
inline constexpr const char* kSchemaFormation = "ournotes-deck-formation@1";
inline constexpr const char* kSchemaResult = "ournotes-deck-result@1";
inline constexpr const char* kModelId = "ournotes-index@1";
inline constexpr const char* kToolName = "ournotes-deck-calculator";
inline constexpr const char* kToolVersion = "0.1.0";

using Triple = std::array<int64_t, 3>;

struct Song {
    int64_t id = 0;
    std::string title;
    int type = 0;
    std::vector<int> bands;
    std::vector<int> tags;
};

struct Note {
    int64_t t = 0;
    int op = 0;
    bool scoring = true;
};

struct ChartSpec {
    std::string difficulty;
    double level = 0;
    std::vector<Note> notes;
    std::vector<int64_t> skill_times_ms;
    std::optional<double> display_level;
    std::optional<int64_t> full_combo_count;
};

struct CardRankBonus {
    int64_t type_link = 0;
    int64_t music_type = 0;
    int64_t music_tag = 0;
};

struct Member {
    int64_t id = 0;
    std::string name;
    std::string title;
    int character = 0;
    int band = 0;
    int card_type = 0;
    int rarity = 0;
    Triple trained{};
    int64_t event_bonus_bp = 0;
    std::vector<int> tags;
    int64_t live_skill = 0;
    int64_t leader_skill = 0;
    int64_t gekisou_skill = 0;
    CardRankBonus card_rank_bonus_bp;
    int level = 0;
    int rank = 0;
    int awake = 0;
};

struct Snapshot {
    int64_t id = 0;
    std::string name;
    std::string title;
    std::vector<int> characters;
    std::vector<int> bands;
    int card_type = 0;
    int rarity = 0;
    Triple trained{};
    int64_t event_bonus_bp = 0;
    int rank = 0;
    int level = 0;
    std::vector<int64_t> support_skills;
    CardRankBonus card_rank_bonus_bp;
};

struct Fix {
    Triple character_rank_bonus{};
    Triple character_total_rank_bonus{};
    Triple band_item_bonus_bp{};
    Triple vip_bonus_bp{};
    int64_t type_link_base_bp = 0;
    int64_t music_type_base_bp = 0;
    int64_t music_tag_base_bp = 0;
};

struct SkillEffect {
    int level = 0;
    int effect_type = 0;
    int64_t value = 0;
    std::vector<int64_t> targets;
    int condition_group = 0;
};

struct Skill {
    int64_t id = 0;
    std::vector<int> categories;
    int mission_type = 0;
    std::vector<SkillEffect> effects;
};

struct Target {
    int64_t id = 0;
    int character = 0;
    int band = 0;
    int card_type = 0;
    int tag = 0;
    int gekisou_mission_type = 0;
    std::vector<int> live_skill_categories;
    std::vector<int> gekisou_skill_categories;
};

struct Condition {
    int64_t id = 0;
    int type = 0;
    bool positive = true;
    std::vector<int64_t> targets;
};

struct ConditionGroup {
    int group = 0;
    std::vector<std::vector<int64_t>> rows;
};

struct NoteParameter {
    int op = 0;
    double score_percent = 0;
};

struct ComboBonus {
    int type = 0;
    int required_combo_count = 0;
    double factor = 0;
};

struct Catalog {
    std::vector<Member> members;
    std::vector<Snapshot> snapshots;
    Fix fix;
    std::vector<Skill> live_skills;
    std::vector<Skill> leader_skills;
    std::vector<Skill> support_skills;
    std::vector<Skill> gekisou_skills;
    std::vector<Target> targets;
    std::vector<Condition> conditions;
    std::vector<ConditionGroup> condition_groups;
    std::vector<NoteParameter> note_parameters;
    std::vector<ComboBonus> combo_bonuses;
};

struct ExtraSource {
    std::string id;
    Triple rate_bp{};
    std::string scope = "slot";
    std::string kind = "percent";
    Json when;
};

struct PowerModel {
    std::string rounding = "float32_floor";
    bool snapshot = true;
    bool type_link = true;
    bool band_item = true;
    bool music_type = true;
    bool music_tag = true;
    bool leader = true;
    bool vip = true;
    std::vector<ExtraSource> extra_sources;
};

struct ScoreModel {
    double adjustment_factor = 3.0;
    double level_base = 5.0;
    std::optional<double> level_alpha;
    std::string rounding = "float32_floor";
};

struct Judgement {
    std::string mode = "all_perfect";
    std::map<std::string, double> factors;
    double default_factor = 0.0;
    std::string fixed_label = "perfect";
    std::vector<Json> sequence;
    std::map<std::string, double> distribution;
    bool has_distribution = false;
    int64_t seed = 0;
};

struct Life {
    std::string mode = "constant";
    double initial = 1000;
    double onus_factor = 0.3;
    std::map<std::string, double> damage;
    double floor = 0;
};

struct Assist {
    bool enabled = false;
    double score_percent = 90;
};

struct GekisouSection {
    std::string label;
    int64_t start_ms = 0;
    std::optional<int64_t> end_ms;
    int64_t bonus_bp = 10000;
};

struct Gekisou {
    bool enabled = false;
    std::vector<GekisouSection> sections;
};

struct SkillSettings {
    std::string overlap = "additive";
    std::string activation = "scheduled";
    std::optional<int> live_effect_type;
    std::string live_effect_branch = "max";
};

struct Settings {
    int team_size = 5;
    int combo_type = 0;
    int64_t base_duration_ms = 5000;
    int live_level = 5;
    int leader_level = 5;
    int support_level = 5;
    int duration_extension_effect_type = 15000;
    std::string type_link_bonus_source = "snapshot";
    std::string leader_unmapped_effect_types = "ignore";
    std::map<int, std::string> leader_effect_axes;
    SkillSettings skill;
    PowerModel power;
    ScoreModel score;
    Judgement judgement;
    Life life;
    Assist assist;
    Gekisou gekisou;
};

struct Constraints {
    bool distinct_characters = true;
    bool distinct_snapshots = true;
    std::vector<int64_t> member_pool;
    std::vector<int64_t> snapshot_pool;
    std::vector<int64_t> leader_pool;
    bool has_member_pool = false;
    bool has_snapshot_pool = false;
    bool has_leader_pool = false;
    std::vector<int64_t> banned_members;
    std::vector<int64_t> banned_snapshots;
    std::vector<int64_t> required_members;
    std::vector<int64_t> required_snapshots;
};

struct SearchSettings {
    std::string mode = "rank";
    int top = 20;
    double time_limit_s = 60;
    std::string order_search = "exact";
    bool prescreen = true;
    int64_t seed = 0;
    bool verbose = false;
};

struct Problem {
    Song song;
    ChartSpec chart;
    Catalog catalog;
    Settings settings;
    Constraints constraints;
    SearchSettings search;
    Json raw;

    void validate() const;
    std::vector<const Member*> available_members() const;
    std::vector<const Snapshot*> available_snapshots() const;
    std::vector<const Member*> leader_candidates() const;
    Json describe() const;
};

struct Slot {
    int64_t member = 0;
    int64_t snapshot = 0;
    int trigger = 0;
};

struct Formation {
    int64_t leader = 0;
    std::vector<Slot> slots;  // 按 trigger 升序
    void validate(const Problem& problem) const;
    std::vector<int64_t> members() const;
};

// 解析助手，供 model.cpp 与其它模块复用
const Json* opt(const Json& object, const char* key);
int64_t int_field(const Json& object, const char* key, int64_t fallback = 0);
double num_field(const Json& object, const char* key, double fallback = 0);
bool bool_field(const Json& object, const char* key, bool fallback = false);
std::string str_field(const Json& object, const char* key, const std::string& fallback = "");
std::vector<int> int_array_field(const Json& object, const char* key);
std::vector<int64_t> int64_array_field(const Json& object, const char* key);
Triple triple_field(const Json& object, const char* key, Triple fallback = Triple{0, 0, 0});

Problem parse_problem(const Json& document);
Formation parse_formation(const Json& document);
Json dump_formation(const Formation& formation);

}  // namespace deckcalc
