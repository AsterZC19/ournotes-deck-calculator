#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "model.hpp"

namespace deckcalc {

struct CatalogIndex {
    std::unordered_map<int64_t, const Member *> member_by_id;
    std::unordered_map<int64_t, const Snapshot *> snapshot_by_id;
    std::unordered_map<int64_t, const Skill *> live_skill_by_id;
    std::unordered_map<int64_t, const Skill *> leader_skill_by_id;
    std::unordered_map<int64_t, const Skill *> support_skill_by_id;
    std::unordered_map<int64_t, const Skill *> gekisou_skill_by_id;
    std::unordered_map<int64_t, const Target *> target_by_id;
    std::unordered_map<int64_t, const Condition *> condition_by_id;
    std::unordered_map<int, const ConditionGroup *> condition_group_by_group;
    std::unordered_map<int, const NoteParameter *> note_parameter_by_op;
    std::map<int, std::vector<const ComboBonus *>> combo_by_type;

    void build(const Catalog &catalog);
};

class Rules {
public:
    Rules(const Problem &problem, const CatalogIndex &index);

    bool target_matches(const Target &target, const Member &member) const;
    bool condition_group_matches(int group, const Member &member) const;
    double live_boost(const Member &member) const;
    Triple leader_bonus_bp(const Member &leader, const Member &member) const;
    int64_t duration_ms(const Member &member, const Snapshot &snapshot) const;

    const std::map<std::pair<int64_t, int>, std::pair<int, int64_t>> &
    unmapped_leader_effects() const {
        return unmapped_;
    }

private:
    const Problem &problem_;
    const CatalogIndex &index_;
    int live_level_;
    int leader_level_;
    int support_level_;
    int64_t base_duration_ms_;
    int duration_effect_type_;
    std::optional<int> live_effect_type_;
    std::string live_effect_branch_;
    std::map<int, std::string> leader_axes_;
    bool unmapped_is_error_;

    mutable std::unordered_map<uint64_t, bool> target_cache_;
    mutable std::unordered_map<uint64_t, bool> group_cache_;
    mutable std::unordered_map<int64_t, double> live_cache_;
    mutable std::unordered_map<uint64_t, Triple> leader_cache_;
    mutable std::map<std::pair<int64_t, int>, std::pair<int, int64_t>> unmapped_;
};

struct ChartData {
    std::vector<int64_t> times;
    std::vector<int> ops;
    std::vector<double> weights;
    std::vector<double> note_weights;
    std::vector<double> combo_bonuses;
    std::vector<float> native_combo_bonuses;
    std::vector<double> prefix;
    double base = 0;
    double all_note_weight_sum = 0;
    int64_t converted_note_count = 0;
    int judged_notes = 0;

    double coverage(int64_t start_ms, int64_t duration_ms) const;
};

struct SlotEval {
    int trigger = 0;
    int64_t member = 0;
    int64_t snapshot = 0;
    int64_t duration_ms = 0;
    int64_t power = 0;
    double live_boost = 0;
    double weighted_skill_gain = 0;
    Json breakdown;
    bool has_breakdown = false;
};

struct Evaluation {
    int64_t leader = 0;
    std::vector<SlotEval> slots;
    int64_t power = 0;
    double weight_factor = 0;
    double index = 0;
    std::string ranking_objective = "index";
    std::optional<double> ranking_score;
    Json estimated_score;
    bool has_estimated = false;
    Json order_analysis;
    bool has_order = false;
    std::vector<std::string> warnings;

    std::vector<int64_t> member_ids() const;
    Json to_json(bool detail, bool with_rank, int rank) const;
};

struct EvalOptions {
    std::string order_search = "exact";
    bool detail = false;
    bool validate = true;
    bool calculate_score = true;
};

struct ChartInfo {
    int judged_notes = 0;
    int64_t converted_note_count = 0;
    double note_weight_sum = 0;
    double all_note_weight_sum = 0;
    std::vector<int64_t> skill_times;
};

class Engine {
public:
    explicit Engine(const Problem &problem);

    const Problem &problem() const {
        return problem_;
    }
    const CatalogIndex &index() const {
        return index_;
    }
    const Rules &rules() const {
        return rules_;
    }
    const ChartData &chart() const {
        return chart_;
    }

    const std::vector<const Member *> &members() const {
        return members_;
    }
    const std::vector<const Snapshot *> &snapshots() const {
        return snapshots_;
    }
    int team_size() const {
        return problem_.settings.team_size;
    }
    int member_pos(int64_t id) const;
    int snapshot_pos(int64_t id) const;

    int64_t duration_ms(int member_index, int snapshot_index) const;
    double live_boost(int member_index) const;

    int64_t slot_power(size_t leader_index, size_t member_index, size_t snapshot_index) const;
    Json slot_breakdown(size_t leader_index, size_t member_index, size_t snapshot_index) const;

    const std::vector<std::vector<int64_t>> &power_matrix(size_t leader_index) const;
    const std::vector<std::vector<int64_t>> &durations() const;
    const std::vector<double> &boosts() const;
    const std::vector<std::vector<std::vector<double>>> &gains() const;

    ChartInfo chart_info() const;
    Json chart_analysis() const;
    Json model_block() const;

    Evaluation evaluate(const Formation &formation, const EvalOptions &options) const;
    void populate_estimated_score(Evaluation &evaluation) const;
    void validate_theoretical_scope() const;
    Evaluation evaluate_theoretical(const Formation &formation, bool detail = false) const;
    double theoretical_upper_bound(double power_bound, const std::vector<float> &live_bound) const;

private:
    void build_chart();

    const Problem &problem_;
    CatalogIndex index_;
    Rules rules_;
    std::vector<const Member *> members_;
    std::vector<const Snapshot *> snapshots_;
    std::unordered_map<int64_t, int> member_pos_;
    std::unordered_map<int64_t, int> snapshot_pos_;
    ChartData chart_;

    mutable std::vector<std::vector<int64_t>> durations_;
    mutable std::vector<double> boosts_;
    mutable std::vector<std::vector<std::vector<double>>> gains_;
    mutable bool durations_ready_ = false;
    mutable bool boosts_ready_ = false;
    mutable bool gains_ready_ = false;
    mutable std::unordered_map<size_t, std::vector<std::vector<int64_t>>> power_matrices_;
};

}
