#include "solver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace deckcalc {
namespace {

constexpr int kMaxTeam = 8;
constexpr double kEps = 1e-12;

// Selected slots only: independent of catalog size and character IDs.
struct SelectionSet {
    std::array<int, kMaxTeam> values{};
    int size = 0;

    bool contains(int value) const {
        return std::binary_search(values.begin(), values.begin() + size, value);
    }
    void insert(int value) {
        auto end = values.begin() + size;
        auto at = std::lower_bound(values.begin(), end, value);
        if (at != end && *at == value)
            return;
        if (size == kMaxTeam)
            throw SpecError("selected set exceeds team size");
        std::move_backward(at, end, end + 1);
        *at = value;
        ++size;
    }
    SelectionSet with(int value) const {
        auto result = *this;
        result.insert(value);
        return result;
    }
    static SelectionSet single(int value) {
        SelectionSet result;
        result.insert(value);
        return result;
    }
    bool operator==(const SelectionSet &other) const {
        return size == other.size &&
               std::equal(values.begin(), values.begin() + size, other.values.begin());
    }
};

struct SelectionHash {
    size_t operator()(const SelectionSet &set) const {
        size_t hash = 0;
        for (int i = 0; i < set.size; ++i)
            hash ^=
                std::hash<int>{}(set.values[i]) + size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
        return hash;
    }
};

using Clock = std::chrono::steady_clock;

double wall_s() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

struct Pair {
    int member = 0;
    int snap = 0;
};

struct Shared {
    const Engine *engine = nullptr;
    std::vector<const Member *> members;
    std::vector<const Snapshot *> snapshots;
    std::vector<Pair> pairs;
    std::vector<double> index_max_gains, score_max_gains;
    std::vector<std::vector<double>> gains;
    std::vector<char> keep_at;
    bool distinct_characters = true;
    bool distinct_snapshots = true;
    bool score_objective = false;
    bool average_objective = false;
    std::vector<SelectionSet> excluded_member_sets;
    std::vector<float> score_live_bound;
    mutable int64_t score_evaluations = 0;
    mutable int64_t score_cache_hits = 0;
    mutable std::map<std::array<int, kMaxTeam + 1>, Evaluation> score_cache;
    bool linear_score_bound_enabled = false;
    double score_base_coefficient = 0;
    double score_linear_max_power = 0;
    std::vector<std::vector<double>> score_gain_coefficients;
    double bound_gain(int pair, int trigger) const {
        return score_objective && linear_score_bound_enabled
                   ? score_gain_coefficients[pair][trigger]
                   : gains[pair][trigger];
    }
    double deadline = 0;
    std::vector<int64_t> required_members, required_snapshots;
    int team_size = 5;
    int member_count = 0;
    int snap_count = 0;
    std::vector<int> character_groups;
    int character_count = 0;

    bool kept(int pair, int trigger) const {
        return keep_at[static_cast<size_t>(pair) * static_cast<size_t>(team_size) +
                       static_cast<size_t>(trigger)] != 0;
    }
    bool any_kept(int pair) const {
        for (int k = 0; k < team_size; ++k) {
            if (kept(pair, k))
                return true;
        }
        return false;
    }
    double max_gain(int pair) const {
        return score_objective && linear_score_bound_enabled ? score_max_gains[pair]
                                                             : index_max_gains[pair];
    }
};

void prune(Shared &shared, const std::vector<std::vector<int64_t>> &power) {
    const int team_size = shared.team_size;
    const int snap_count = shared.snap_count;
    shared.keep_at.assign(shared.pairs.size() * static_cast<size_t>(team_size), 1);
    if (!shared.required_snapshots.empty())
        return;
    std::vector<double> p(static_cast<size_t>(snap_count));
    std::vector<double> g(static_cast<size_t>(snap_count));
    for (int member = 0; member < shared.member_count; ++member) {
        for (int k = 0; k < team_size; ++k) {
            for (int snap = 0; snap < snap_count; ++snap) {
                const size_t index = static_cast<size_t>(member) * static_cast<size_t>(snap_count) +
                                     static_cast<size_t>(snap);
                p[static_cast<size_t>(snap)] = static_cast<double>(
                    power[static_cast<size_t>(member)][static_cast<size_t>(snap)]);
                g[static_cast<size_t>(snap)] = shared.gains[index][static_cast<size_t>(k)];
            }
            for (int snap = 0; snap < snap_count; ++snap) {
                int dominators = 0;
                for (int other = 0; other < snap_count; ++other) {
                    if (other == snap)
                        continue;
                    const bool no_worse =
                        p[static_cast<size_t>(other)] >= p[static_cast<size_t>(snap)] &&
                        g[static_cast<size_t>(other)] >= g[static_cast<size_t>(snap)];
                    if (!no_worse)
                        continue;
                    const bool strict =
                        p[static_cast<size_t>(other)] > p[static_cast<size_t>(snap)] ||
                        g[static_cast<size_t>(other)] > g[static_cast<size_t>(snap)] ||
                        other < snap;
                    if (strict)
                        ++dominators;
                    if (dominators >= team_size)
                        break;
                }
                if (dominators >= team_size) {
                    const size_t flat =
                        (static_cast<size_t>(member) * static_cast<size_t>(snap_count) +
                         static_cast<size_t>(snap)) *
                            static_cast<size_t>(team_size) +
                        static_cast<size_t>(k);
                    shared.keep_at[flat] = 0;
                }
            }
        }
    }
}

double assign_triggers(const Shared &shared, const std::vector<int> &pairs,
                       std::vector<int> &triggers) {
    const int team_size = shared.team_size;

    const int states = 1 << team_size;
    std::array<double, 1 << kMaxTeam> dp;
    std::array<int, 1 << kMaxTeam> chosen{};
    dp.fill(-std::numeric_limits<double>::infinity());
    dp[0] = 0;
    for (int mask = 0; mask < states; ++mask) {
        const int row = __builtin_popcount(static_cast<unsigned>(mask));
        if (row >= team_size)
            continue;
        for (int k = 0; k < team_size; ++k) {
            if (mask & (1 << k))
                continue;
            const int next = mask | (1 << k);
            const double value = dp[mask] + shared.gains[pairs[row]][k];
            if (value > dp[next]) {
                dp[next] = value;
                chosen[next] = k;
            }
        }
    }
    triggers.resize(team_size);
    int mask = states - 1;
    for (int row = team_size - 1; row >= 0; --row) {
        triggers[row] = chosen[mask];
        mask ^= 1 << chosen[mask];
    }
    return dp[states - 1];
}

struct Pick {
    int pair = 0;
    int trigger = 0;
    int64_t power = 0;
    double gain = 0.0;
};

struct Game {
    const Shared *shared = nullptr;
    std::vector<int64_t> flat_power;
    int64_t leader_id = 0;
    int leader_member = -1;
    std::vector<int> by_power;
    std::vector<int> by_potential;
    std::vector<double> member_max_power, member_max_gain;

    int64_t slot_power(int pair) const {
        return flat_power[static_cast<size_t>(pair)];
    }
};

struct Solution {
    double index = 0.0;
    SelectionSet member_set;
    std::vector<Pick> picks;
    Evaluation evaluation;
    bool found = false;
};

Evaluation evaluate_picks(const Game &game, const std::vector<Pick> &picks) {
    const Shared &shared = *game.shared;
    Formation formation;
    formation.leader = game.leader_id;
    for (const Pick &pick : picks) {
        const Pair &pair = shared.pairs[static_cast<size_t>(pick.pair)];
        Slot slot;
        slot.member = shared.members[static_cast<size_t>(pair.member)]->id;
        slot.snapshot = shared.snapshots[static_cast<size_t>(pair.snap)]->id;
        slot.trigger = pick.trigger + 1;
        formation.slots.push_back(slot);
    }
    if (shared.score_objective) {
        std::array<int, kMaxTeam + 1> key;
        key.fill(-1);
        key[0] = game.leader_member;
        for (size_t i = 0; i < picks.size(); ++i)
            key[i + 1] = picks[i].pair;
        std::sort(key.begin() + 1, key.begin() + 1 + picks.size());
        const auto cached = shared.score_cache.find(key);
        if (cached != shared.score_cache.end()) {
            ++shared.score_cache_hits;
            return cached->second;
        }
        ++shared.score_evaluations;
        auto result = shared.average_objective ? shared.engine->evaluate_mean(formation)
                                               : shared.engine->evaluate_theoretical(formation);
        if (shared.score_cache.size() >= 1024)
            shared.score_cache.clear();
        shared.score_cache.emplace(key, result);
        return result;
    }
    EvalOptions options;
    options.calculate_score = false;
    return shared.engine->evaluate(formation, options);
}

SelectionSet member_set_of(const Shared &shared, const std::vector<Pick> &picks) {
    SelectionSet mask;
    for (const Pick &pick : picks) {
        mask.insert(shared.pairs[static_cast<size_t>(pick.pair)].member);
    }
    return mask;
}

bool legal_pairs(const Game &game, const std::vector<int> &pairs) {
    const auto &shared = *game.shared;
    std::set<int> members, snapshots, characters;
    std::set<int64_t> member_ids, snapshot_ids;
    for (int p : pairs) {
        const auto &pair = shared.pairs[p];
        if (!members.insert(pair.member).second)
            return false;
        if (!snapshots.insert(pair.snap).second && shared.distinct_snapshots)
            return false;
        if (!characters.insert(shared.members[pair.member]->character).second &&
            shared.distinct_characters)
            return false;
        member_ids.insert(shared.members[pair.member]->id);
        snapshot_ids.insert(shared.snapshots[pair.snap]->id);
    }
    SelectionSet mask;
    for (int member : members)
        mask.insert(member);
    for (auto excluded : shared.excluded_member_sets)
        if (mask == excluded)
            return false;
    if (!members.count(game.leader_member))
        return false;
    for (auto id : shared.required_members)
        if (!member_ids.count(id))
            return false;
    for (auto id : shared.required_snapshots)
        if (!snapshot_ids.count(id))
            return false;
    return true;
}

Solution make_solution(const Game &game, const std::vector<int> &pairs, bool with_evaluation) {
    const Shared &shared = *game.shared;
    Solution solution;
    if (static_cast<int>(pairs.size()) != shared.team_size || !legal_pairs(game, pairs))
        return solution;
    std::vector<int> triggers;
    const double gain = assign_triggers(shared, pairs, triggers);
    int64_t power_sum = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
        const int pair = pairs[i];
        const int64_t slot = game.slot_power(pair);
        power_sum += slot;
        solution.picks.push_back(
            Pick{pair, triggers[i], slot,
                 shared.gains[static_cast<size_t>(pair)][static_cast<size_t>(triggers[i])]});
    }
    solution.member_set = member_set_of(shared, solution.picks);
    solution.index = static_cast<double>(power_sum) * (1.0 + gain);
    solution.found = true;
    if (with_evaluation) {
        solution.evaluation = evaluate_picks(game, solution.picks);
        solution.index = solution.evaluation.ranking_score.value_or(solution.evaluation.index);
    }
    return solution;
}

struct SnapshotClass {
    int pair = 0;
};

// Order retained candidates; equally scoring sets at the cutoff are interchangeable.
struct SolutionBetter {
    bool operator()(const Solution &a, const Solution &b) const {
        if (a.index != b.index)
            return a.index > b.index;
        if (a.evaluation.power != b.evaluation.power)
            return a.evaluation.power > b.evaluation.power;
        if (a.evaluation.leader != b.evaluation.leader)
            return a.evaluation.leader < b.evaluation.leader;
        return a.member_set.values < b.member_set.values;
    }
};

struct TopKPool {
    int k = 1;
    std::unordered_map<SelectionSet, Solution, SelectionHash> pool;
    double kth_score = -std::numeric_limits<double>::infinity();

    static bool is_better(const Solution &a, const Solution &b) {
        return SolutionBetter{}(a, b);
    }

    void update_kth() {
        if (static_cast<int>(pool.size()) < k) {
            kth_score = -std::numeric_limits<double>::infinity();
            return;
        }
        auto worst_it = pool.begin();
        for (auto it = std::next(pool.begin()); it != pool.end(); ++it) {
            if (is_better(worst_it->second, it->second)) {
                worst_it = it;
            }
        }
        kth_score = worst_it->second.index;
    }

    double cutoff() const {
        return kth_score;
    }

    double set_score(const SelectionSet &set) const {
        auto it = pool.find(set);
        if (it != pool.end())
            return it->second.index;
        return -std::numeric_limits<double>::infinity();
    }

    bool consider(Solution candidate) {
        auto it = pool.find(candidate.member_set);
        if (it != pool.end()) {
            if (is_better(candidate, it->second)) {
                it->second = std::move(candidate);
                update_kth();
                return true;
            }
            return false;
        }
        if (static_cast<int>(pool.size()) < k) {
            pool.emplace(candidate.member_set, std::move(candidate));
            update_kth();
            return true;
        }
        auto worst_it = pool.begin();
        for (auto iter = std::next(pool.begin()); iter != pool.end(); ++iter) {
            if (is_better(worst_it->second, iter->second)) {
                worst_it = iter;
            }
        }
        if (is_better(candidate, worst_it->second)) {
            pool.erase(worst_it);
            pool.emplace(candidate.member_set, std::move(candidate));
            update_kth();
            return true;
        }
        return false;
    }
};

struct ClassSearch {
    Shared &shared;
    NativeScoreBound bound;
    std::vector<std::vector<SnapshotClass>> classes;
    std::vector<std::vector<int>> class_of;
    std::vector<int> members, selected_classes;
    std::vector<std::vector<int64_t>> exact_power;
    std::vector<std::vector<int>> class_order;
    std::vector<std::vector<int64_t>> class_power;
    Game game;
    std::vector<int> candidates;
    TopKPool pool;
    SelectionSet current_set_{};
    ScoreOrderStats order_stats;
    int64_t nodes = 0, compositions = 0, class_nodes = 0, matchings = 0, pruned = 0;
    bool timed_out = false;
    std::vector<int64_t> dp_, next_;
    std::vector<int> assigned_;
    std::vector<int64_t> rem_power_;
    std::vector<double> rem_gain_;
    std::vector<std::vector<double>> member_pos_gain;
    std::vector<std::vector<int>> all_orders_;
    struct ClassKeyHash {
        size_t operator()(const std::array<int, kMaxTeam> &key) const {
            size_t hash = 0;
            for (int value : key)
                hash ^= std::hash<int>{}(value) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    std::unordered_set<std::array<int, kMaxTeam>, ClassKeyHash> handled_classes_;
    int64_t matching_cache_hits = 0, composition_bound_pruned = 0, order_bound_pruned = 0;
    std::vector<std::vector<double>> dfs_powers_, dfs_gains_;
    std::vector<double> rem_gain_order_;
    std::vector<std::vector<int>> order_class_order_;
    struct OrderEntry {
        const std::vector<int> *pos = nullptr;
        double cap = 0.0;
    };
    std::vector<OrderEntry> sorted_orders_;

    explicit ClassSearch(Shared &s, int top_k = 1) : shared(s) {
        pool.k = std::max(1, top_k);
        bound.enabled = s.linear_score_bound_enabled;
        bound.base = s.score_base_coefficient;
        bound.max_power = s.score_linear_max_power;
        bound.gains = s.score_gain_coefficients;
        classes.resize(s.member_count);
        class_of.assign(s.member_count, std::vector<int>(s.snap_count));
        for (int m = 0; m < s.member_count; ++m) {
            std::map<int64_t, int> ids;
            for (int snap = 0; snap < s.snap_count; ++snap) {
                auto [it, added] =
                    ids.try_emplace(s.engine->duration_ms(m, snap), classes[m].size());
                if (added)
                    classes[m].push_back({m * s.snap_count + snap});
                class_of[m][snap] = it->second;
            }
        }
        member_pos_gain.assign(s.member_count, std::vector<double>(s.team_size, 0.0));
        for (int m = 0; m < s.member_count; ++m) {
            for (int k = 0; k < s.team_size; ++k) {
                double max_g = 0.0;
                for (const auto &sc : classes[m]) {
                    if (sc.pair < static_cast<int>(bound.gains.size()) &&
                        k < static_cast<int>(bound.gains[sc.pair].size())) {
                        max_g = std::max(max_g, bound.gains[sc.pair][k]);
                    }
                }
                member_pos_gain[m][k] = max_g;
            }
        }
        std::vector<int> ord(s.team_size);
        std::iota(ord.begin(), ord.end(), 0);
        do {
            all_orders_.push_back(ord);
        } while (std::next_permutation(ord.begin(), ord.end()));
        sorted_orders_.reserve(all_orders_.size());

        dfs_powers_.resize(s.team_size + 1);
        dfs_gains_.resize(s.team_size + 1);
        const int groups = s.distinct_characters ? s.character_count : s.member_count;
        for (int d = 0; d <= s.team_size; ++d) {
            dfs_powers_[d].resize(groups);
            dfs_gains_[d].resize(groups);
        }
        rem_gain_order_.resize(s.team_size + 1);
        order_class_order_.resize(s.team_size);

        dp_.resize(1 << s.team_size);
        next_.resize(1 << s.team_size);
        assigned_.resize(s.snap_count * (1 << s.team_size));
        rem_power_.resize(s.team_size + 1);
        rem_gain_.resize(s.team_size + 1);
    }

    double cutoff() const {
        return pool.cutoff();
    }
    double current_set_cutoff() const {
        return std::max(pool.cutoff(), pool.set_score(current_set_));
    }
    bool expired() {
        if (wall_s() > shared.deadline)
            timed_out = true;
        return timed_out;
    }
    double cap(double power, double gain) const {
        return NativeScoreBound::upper(power, gain, bound.base, bound.max_power, shared.team_size,
                                       shared.engine->chart().times.size());
    }
    double class_gain(int member, int c) const {
        const auto &row = bound.gains[classes[member][c].pair];
        return *std::max_element(row.begin(), row.end());
    }

    std::vector<int> match() {
        ++matchings;
        const int n = shared.team_size, states = 1 << n;
        const int64_t missing = std::numeric_limits<int64_t>::min();
        std::fill(dp_.begin(), dp_.end(), missing);
        std::fill(assigned_.begin(), assigned_.end(), -2);
        dp_[0] = 0;
        for (int s = 0; s < shared.snap_count; ++s) {
            if (expired())
                return {};
            const bool required =
                std::find(shared.required_snapshots.begin(), shared.required_snapshots.end(),
                          shared.snapshots[s]->id) != shared.required_snapshots.end();
            const int offset = s * states;
            if (required)
                std::fill(next_.begin(), next_.end(), missing);
            else {
                next_ = dp_;
                for (int mask = 0; mask < states; ++mask)
                    if (dp_[mask] != missing)
                        assigned_[offset + mask] = -1;
            }
            for (int mask = 0; mask < states; ++mask) {
                if (dp_[mask] == missing)
                    continue;
                for (int row = 0; row < n; ++row) {
                    if ((mask & (1 << row)) || class_of[members[row]][s] != selected_classes[row])
                        continue;
                    const int target = mask | (1 << row);
                    const int64_t value = dp_[mask] + exact_power[row][s];
                    if (value > next_[target]) {
                        next_[target] = value;
                        assigned_[offset + target] = row;
                    }
                }
            }
            dp_.swap(next_);
        }
        if (dp_.back() == missing)
            return {};
        std::vector<int> pairs(n);
        int mask = states - 1;
        for (int s = shared.snap_count - 1; s >= 0; --s) {
            const int row = assigned_[s * states + mask];
            if (row >= 0) {
                pairs[row] = members[row] * shared.snap_count + s;
                mask ^= 1 << row;
            }
        }
        return pairs;
    }

    void bindings(int depth, int64_t power, double gain) {
        ++class_nodes;
        if (expired())
            return;
        if (cap(power + rem_power_[depth],
                std::nextafter(gain + rem_gain_[depth], std::numeric_limits<double>::infinity())) <=
            current_set_cutoff()) {
            ++pruned;
            return;
        }
        if (depth != shared.team_size) {
            for (int c : class_order[depth]) {
                selected_classes[depth] = c;
                bindings(depth + 1, power + class_power[depth][c],
                         std::nextafter(gain + class_gain(members[depth], c),
                                        std::numeric_limits<double>::infinity()));
                if (timed_out)
                    return;
            }
            return;
        }
        const auto pairs = match();
        if (pairs.empty())
            return;
        Formation formation;
        formation.leader = game.leader_id;
        int64_t matched_power = 0;
        for (int row = 0; row < shared.team_size; ++row) {
            const auto &pair = shared.pairs[pairs[row]];
            formation.slots.push_back(
                {shared.members[pair.member]->id, shared.snapshots[pair.snap]->id, row + 1});
            matched_power += exact_power[row][pair.snap];
        }
        if (cap(matched_power, gain) <= current_set_cutoff()) {
            ++pruned;
            return;
        }
        ++shared.score_evaluations;
        std::optional<Evaluation> evaluation;
        if (shared.average_objective || matched_power < 0) {
            evaluation = shared.average_objective ? shared.engine->evaluate_mean(formation)
                                                  : shared.engine->evaluate_theoretical(formation);
            int64_t orders = 1;
            for (int i = 2; i <= shared.team_size; ++i)
                orders *= i;
            order_stats.evaluated += orders;
        } else
            evaluation = shared.engine->evaluate_best_order(formation, bound, current_set_cutoff(),
                                                            order_stats);
        if (!evaluation || *evaluation->ranking_score <= current_set_cutoff())
            return;
        Solution candidate;
        candidate.found = true;
        candidate.index = *evaluation->ranking_score;
        candidate.member_set = current_set_;
        candidate.evaluation = std::move(*evaluation);
        pool.consider(std::move(candidate));
    }

    void order_classes_dfs(const std::vector<int> &ord, int depth, int64_t power, double gain) {
        ++class_nodes;
        if (expired())
            return;
        if (cap(power + rem_power_[depth],
                std::nextafter(gain + rem_gain_order_[depth],
                               std::numeric_limits<double>::infinity())) <= current_set_cutoff()) {
            ++pruned;
            return;
        }
        const int n = shared.team_size;
        if (depth != n) {
            for (int c : order_class_order_[depth]) {
                selected_classes[depth] = c;
                const double g = bound.gains[classes[members[depth]][c].pair][ord[depth]];
                order_classes_dfs(
                    ord, depth + 1, power + class_power[depth][c],
                    std::nextafter(gain + g, std::numeric_limits<double>::infinity()));
                if (timed_out)
                    return;
            }
            return;
        }

        std::array<int, kMaxTeam> class_key{};
        std::copy(selected_classes.begin(), selected_classes.end(), class_key.begin());
        if (handled_classes_.count(class_key)) {
            ++matching_cache_hits;
            return;
        }

        const auto pairs = match();
        if (pairs.empty()) {
            if (!timed_out)
                handled_classes_.insert(class_key);
            return;
        }

        Formation formation;
        formation.leader = game.leader_id;
        int64_t matched_power = 0;
        double max_all_orders_gain = 0.0;
        for (int row = 0; row < n; ++row) {
            const auto &pair = shared.pairs[pairs[row]];
            formation.slots.push_back(
                {shared.members[pair.member]->id, shared.snapshots[pair.snap]->id, row + 1});
            matched_power += exact_power[row][pair.snap];
            const auto &g_row = bound.gains[pairs[row]];
            max_all_orders_gain =
                std::nextafter(max_all_orders_gain + *std::max_element(g_row.begin(), g_row.end()),
                               std::numeric_limits<double>::infinity());
        }

        if (cap(matched_power, max_all_orders_gain) <= current_set_cutoff()) {
            ++pruned;
            handled_classes_.insert(class_key);
            return;
        }

        ++shared.score_evaluations;
        std::optional<Evaluation> evaluation;
        if (matched_power < 0) {
            evaluation = shared.engine->evaluate_theoretical(formation);
            int64_t orders = 1;
            for (int i = 2; i <= n; ++i)
                orders *= i;
            order_stats.evaluated += orders;
        } else {
            evaluation = shared.engine->evaluate_best_order(formation, bound, current_set_cutoff(),
                                                            order_stats);
        }

        if (!timed_out)
            handled_classes_.insert(class_key);

        if (!evaluation || *evaluation->ranking_score <= current_set_cutoff())
            return;

        Solution candidate;
        candidate.found = true;
        candidate.index = *evaluation->ranking_score;
        candidate.member_set = current_set_;
        candidate.evaluation = std::move(*evaluation);
        pool.consider(std::move(candidate));
    }

    void orders_search() {
        const int n = shared.team_size;
        handled_classes_.clear();

        sorted_orders_.clear();
        for (const auto &ord : all_orders_) {
            double order_gain = 0.0;
            for (int row = 0; row < n; ++row) {
                const int m = members[row];
                order_gain = std::nextafter(order_gain + member_pos_gain[m][ord[row]],
                                            std::numeric_limits<double>::infinity());
            }
            sorted_orders_.push_back({&ord, cap(rem_power_[0], order_gain)});
        }
        std::stable_sort(sorted_orders_.begin(), sorted_orders_.end(),
                         [](const OrderEntry &a, const OrderEntry &b) { return a.cap > b.cap; });

        for (size_t order_index = 0; order_index < sorted_orders_.size(); ++order_index) {
            const auto &entry = sorted_orders_[order_index];
            if (expired())
                return;
            if (entry.cap <= current_set_cutoff()) {
                order_bound_pruned += sorted_orders_.size() - order_index;
                break;
            }

            const auto &ord = *entry.pos;
            rem_gain_order_[n] = 0.0;
            for (int row = n - 1; row >= 0; --row) {
                const int m = members[row];
                const int pos = ord[row];
                rem_gain_order_[row] =
                    std::nextafter(rem_gain_order_[row + 1] + member_pos_gain[m][pos],
                                   std::numeric_limits<double>::infinity());
            }

            for (int row = 0; row < n; ++row) {
                const int m = members[row];
                const int pos = ord[row];
                const double scale = std::max(1.0, game.member_max_power[m]);
                order_class_order_[row].resize(classes[m].size());
                std::iota(order_class_order_[row].begin(), order_class_order_[row].end(), 0);
                std::stable_sort(
                    order_class_order_[row].begin(), order_class_order_[row].end(),
                    [&](int a, int b) {
                        return class_power[row][a] * (bound.base + game.member_max_gain[m]) +
                                   scale * bound.gains[classes[m][a].pair][pos] >
                               class_power[row][b] * (bound.base + game.member_max_gain[m]) +
                                   scale * bound.gains[classes[m][b].pair][pos];
                    });
            }

            order_classes_dfs(ord, 0, 0, 0);
            if (timed_out)
                return;
        }
    }

    void complete(double power_upper) {
        SelectionSet mask;
        std::vector<const Member *> team;
        for (int m : members) {
            mask.insert(m);
            team.push_back(shared.members[m]);
        }
        for (auto excluded : shared.excluded_member_sets)
            if (mask == excluded)
                return;
        for (auto id : shared.required_members)
            if (std::none_of(team.begin(), team.end(),
                             [&](const Member *m) { return m->id == id; }))
                return;

        current_set_ = mask;
        const int n = shared.team_size;
        const int states = 1 << n;
        std::array<double, 1 << kMaxTeam> assign_dp;
        assign_dp.fill(-1.0);
        assign_dp[0] = 0.0;
        for (int msk = 0; msk < states; ++msk) {
            if (assign_dp[msk] < 0)
                continue;
            const int row = __builtin_popcount(static_cast<unsigned>(msk));
            if (row >= n)
                continue;
            const int m = members[row];
            for (int k = 0; k < n; ++k) {
                if (!(msk & (1 << k))) {
                    const int next = msk | (1 << k);
                    const double v = std::nextafter(assign_dp[msk] + member_pos_gain[m][k],
                                                    std::numeric_limits<double>::infinity());
                    if (v > assign_dp[next])
                        assign_dp[next] = v;
                }
            }
        }
        const double max_assigned_gain = assign_dp[states - 1];
        if (cap(power_upper, max_assigned_gain) <= current_set_cutoff()) {
            ++pruned;
            ++composition_bound_pruned;
            return;
        }

        ++compositions;
        exact_power.assign(n, std::vector<int64_t>(shared.snap_count));
        class_power.resize(n);
        class_order.resize(n);
        selected_classes.resize(n);
        for (int row = 0; row < n; ++row) {
            const int m = members[row];
            class_power[row].assign(classes[m].size(), std::numeric_limits<int64_t>::min());
            class_order[row].resize(classes[m].size());
            std::iota(class_order[row].begin(), class_order[row].end(), 0);
            for (int s = 0; s < shared.snap_count; ++s) {
                const auto p = shared.engine->slot_power(game.leader_member, m, s, &team);
                exact_power[row][s] = p;
                class_power[row][class_of[m][s]] = std::max(class_power[row][class_of[m][s]], p);
            }
        }
        rem_power_[n] = 0;
        for (int row = n - 1; row >= 0; --row) {
            rem_power_[row] = rem_power_[row + 1] +
                              *std::max_element(class_power[row].begin(), class_power[row].end());
        }

        if (shared.average_objective) {
            for (int row = 0; row < n; ++row) {
                const int m = members[row];
                const double scale = std::max(1.0, game.member_max_power[m]);
                std::stable_sort(
                    class_order[row].begin(), class_order[row].end(), [&](int a, int b) {
                        return class_power[row][a] * (bound.base + game.member_max_gain[m]) +
                                   scale * class_gain(m, a) >
                               class_power[row][b] * (bound.base + game.member_max_gain[m]) +
                                   scale * class_gain(m, b);
                    });
            }
            rem_gain_[n] = 0.0;
            for (int row = n - 1; row >= 0; --row) {
                double maximum = 0;
                for (int c : class_order[row])
                    maximum = std::max(maximum, class_gain(members[row], c));
                rem_gain_[row] = std::nextafter(rem_gain_[row + 1] + maximum,
                                                std::numeric_limits<double>::infinity());
            }
            bindings(0, 0, 0);
            return;
        }

        orders_search();
    }

    void compositions_dfs(int begin, SelectionSet characters, double power, double gain,
                          std::array<double, kMaxTeam> selected_pos_max) {
        ++nodes;
        if (expired())
            return;
        const int remaining = shared.team_size - members.size();
        if (!remaining) {
            complete(power);
            return;
        }
        const int depth = members.size();
        auto &powers = dfs_powers_[depth];
        auto &gains = dfs_gains_[depth];
        std::fill(powers.begin(), powers.end(), -1.0);
        std::fill(gains.begin(), gains.end(), 0.0);
        std::array<double, kMaxTeam> cand_pos_max{};
        for (int i = begin; i < static_cast<int>(candidates.size()); ++i) {
            const int m = candidates[i], character = shared.members[m]->character;
            if (m == game.leader_member ||
                (shared.distinct_characters && characters.contains(character)))
                continue;
            const int group = shared.distinct_characters ? shared.character_groups[m] : m;
            powers[group] = std::max(powers[group], game.member_max_power[m]);
            gains[group] = std::max(gains[group], game.member_max_gain[m]);
            for (int k = 0; k < shared.team_size; ++k)
                cand_pos_max[k] = std::max(cand_pos_max[k], member_pos_gain[m][k]);
        }
        if (std::count_if(powers.begin(), powers.end(), [](double p) { return p >= 0; }) <
            remaining)
            return;
        std::sort(powers.begin(), powers.end(), std::greater<double>());
        std::sort(gains.begin(), gains.end(), std::greater<double>());
        double p = power, g = gain;
        for (int i = 0; i < remaining; ++i) {
            p += powers[i];
            g = std::nextafter(g + gains[i], std::numeric_limits<double>::infinity());
        }
        double pos_gain = 0.0;
        for (int k = 0; k < shared.team_size; ++k) {
            pos_gain = std::nextafter(pos_gain + std::max(selected_pos_max[k], cand_pos_max[k]),
                                      std::numeric_limits<double>::infinity());
        }
        const double safe_gain = std::min(g, pos_gain);
        if (cap(p, safe_gain) <= cutoff()) {
            ++pruned;
            return;
        }
        for (int i = begin; i < static_cast<int>(candidates.size()); ++i) {
            const int m = candidates[i], character = shared.members[m]->character;
            if (m == game.leader_member ||
                (shared.distinct_characters && characters.contains(character)))
                continue;
            members.push_back(m);
            std::array<double, kMaxTeam> next_pos_max = selected_pos_max;
            for (int k = 0; k < shared.team_size; ++k)
                next_pos_max[k] = std::max(next_pos_max[k], member_pos_gain[m][k]);
            compositions_dfs(i + 1, characters.with(character), power + game.member_max_power[m],
                             std::nextafter(gain + game.member_max_gain[m],
                                            std::numeric_limits<double>::infinity()),
                             next_pos_max);
            members.pop_back();
            if (timed_out)
                return;
        }
    }
};

struct BeamState {
    int depth = 0;
    SelectionSet member_set;
    SelectionSet snapshot_set;
    SelectionSet character_set;
    int trigger_mask = 0;
    int64_t power = 0;
    double gain = 0.0;
    std::array<Pick, kMaxTeam> picks{};
    std::array<int, kMaxTeam> pairs{};
};

std::vector<BeamState> beam(const Game &game, int beam_width) {
    const Shared &shared = *game.shared;
    const int team_size = shared.team_size;
    const int leader = game.leader_member;
    if (leader < 0)
        return {};
    std::vector<BeamState> frontier;
    frontier.reserve(static_cast<size_t>(shared.snap_count));
    for (int snap = 0; snap < shared.snap_count; ++snap) {
        const int pair = leader * shared.snap_count + snap;
        if (!shared.any_kept(pair))
            continue;
        int best_trigger = -1;
        double best_gain = -1.0;
        for (int k = 0; k < team_size; ++k) {
            if (!shared.kept(pair, k))
                continue;
            const double value = shared.gains[static_cast<size_t>(pair)][static_cast<size_t>(k)];
            if (value > best_gain) {
                best_gain = value;
                best_trigger = k;
            }
        }
        if (best_trigger < 0)
            continue;
        BeamState state;
        state.depth = 1;
        state.power = game.slot_power(pair);
        state.gain = best_gain;
        state.picks[0] = Pick{pair, best_trigger, state.power, best_gain};
        state.pairs[0] = pair;
        state.member_set = SelectionSet::single(leader);
        state.snapshot_set = SelectionSet::single(snap);
        state.character_set =
            SelectionSet::single(shared.members[static_cast<size_t>(leader)]->character);
        state.trigger_mask = 1 << best_trigger;
        frontier.push_back(state);
    }
    if (frontier.empty() || team_size <= 1)
        return frontier;
    for (int depth = 1; depth < team_size; ++depth) {
        struct Child {
            double score;
            size_t parent;
            int pair;
            int trigger;
            int64_t power;
            double gain;
        };
        std::vector<Child> children;
        for (size_t index = 0; index < frontier.size(); ++index) {
            if (wall_s() > shared.deadline)
                return {};
            const BeamState &state = frontier[index];
            for (int pair : game.by_power) {
                const Pair &item = shared.pairs[static_cast<size_t>(pair)];
                const int member_index = item.member;
                if (state.member_set.contains(member_index))
                    continue;
                const int snapshot_index = item.snap;
                if (shared.distinct_snapshots && (state.snapshot_set.contains(snapshot_index)))
                    continue;
                const int character = shared.members[static_cast<size_t>(item.member)]->character;
                if (shared.distinct_characters && (state.character_set.contains(character))) {
                    continue;
                }
                int best_trigger = -1;
                double best_gain = -1.0;
                for (int k = 0; k < team_size; ++k) {
                    if (state.trigger_mask & (1 << k))
                        continue;
                    if (!shared.kept(pair, k))
                        continue;
                    const double value =
                        shared.gains[static_cast<size_t>(pair)][static_cast<size_t>(k)];
                    if (value > best_gain) {
                        best_gain = value;
                        best_trigger = k;
                    }
                }
                if (best_trigger < 0)
                    continue;
                const int64_t slot = game.slot_power(pair);
                const double score =
                    static_cast<double>(state.power + slot) * (1.0 + state.gain + best_gain);
                children.push_back(Child{score, index, pair, best_trigger, slot, best_gain});
            }
        }
        if (children.empty())
            return {};

        std::stable_sort(children.begin(), children.end(),
                         [](const Child &a, const Child &b) { return a.score > b.score; });
        std::unordered_set<std::string> seen;
        std::unordered_map<SelectionSet, int, SelectionHash> member_set_count;
        std::vector<Child> unique;
        for (const auto &child : children) {
            const auto &state = frontier[child.parent];
            std::vector<int> ids;
            for (int i = 0; i < state.depth; ++i)
                ids.push_back(state.pairs[i]);
            ids.push_back(child.pair);
            std::sort(ids.begin(), ids.end());
            std::string key = std::to_string(state.trigger_mask | (1 << child.trigger)) + ":";
            for (int id : ids)
                key += std::to_string(id) + ",";
            if (!seen.insert(key).second)
                continue;

            if (depth + 1 == team_size) {
                const SelectionSet member_set =
                    state.member_set.with(shared.pairs[child.pair].member);
                if (member_set_count[member_set] >= 4)
                    continue;
                ++member_set_count[member_set];
            }
            unique.push_back(child);
            if (static_cast<int>(unique.size()) >= beam_width)
                break;
        }
        children = std::move(unique);
        const size_t keep = children.size();
        std::vector<BeamState> next;
        next.reserve(keep);
        for (const Child &child : children) {
            BeamState state = frontier[child.parent];
            const Pair &item = shared.pairs[static_cast<size_t>(child.pair)];
            state.picks[static_cast<size_t>(state.depth)] =
                Pick{child.pair, child.trigger, child.power, child.gain};
            state.pairs[static_cast<size_t>(state.depth)] = child.pair;
            state.depth += 1;
            state.member_set.insert(item.member);
            state.snapshot_set.insert(item.snap);
            state.character_set.insert(shared.members[static_cast<size_t>(item.member)]->character);
            state.trigger_mask |= 1 << child.trigger;
            state.power += child.power;
            state.gain += child.gain;
            next.push_back(state);
        }
        frontier = std::move(next);
    }
    return frontier;
}

void local_improve(const Game &game, int rounds, Solution &solution) {
    const Shared &shared = *game.shared;
    const int team_size = shared.team_size;
    if (team_size <= 1)
        return;
    for (int round = 0; round < rounds; ++round) {
        bool improved = false;
        for (int position = 0; position < team_size && !improved; ++position) {
            if (wall_s() > shared.deadline)
                return;
            const bool leader_slot =
                shared.pairs[solution.picks[position].pair].member == game.leader_member;
            SelectionSet member_set;
            SelectionSet snapshot_set;
            SelectionSet character_set;
            for (int i = 0; i < team_size; ++i) {
                if (i == position)
                    continue;
                const Pair &item =
                    shared.pairs[static_cast<size_t>(solution.picks[static_cast<size_t>(i)].pair)];
                member_set.insert(item.member);
                snapshot_set.insert(item.snap);
                character_set.insert(shared.members[static_cast<size_t>(item.member)]->character);
            }
            for (int pair : game.by_power) {
                if (pair == solution.picks[static_cast<size_t>(position)].pair)
                    continue;
                if (!shared.any_kept(pair))
                    continue;
                const Pair &item = shared.pairs[static_cast<size_t>(pair)];
                if (leader_slot && item.member != game.leader_member)
                    continue;
                if (member_set.contains(item.member))
                    continue;
                if (shared.distinct_snapshots && (snapshot_set.contains(item.snap)))
                    continue;
                const int character = shared.members[static_cast<size_t>(item.member)]->character;
                if (shared.distinct_characters && (character_set.contains(character))) {
                    continue;
                }
                std::vector<int> pairs;
                pairs.reserve(static_cast<size_t>(team_size));
                for (int i = 0; i < team_size; ++i) {
                    pairs.push_back(i == position ? pair
                                                  : solution.picks[static_cast<size_t>(i)].pair);
                }
                if (!legal_pairs(game, pairs))
                    continue;
                std::vector<int> triggers;
                const double gain = assign_triggers(shared, pairs, triggers);
                double power = 0.0;
                for (int item_pair : pairs)
                    power += static_cast<double>(game.slot_power(item_pair));
                if (power * (1.0 + gain) <= solution.index * (1.0 + 1e-12))
                    continue;
                std::vector<Pick> picks;
                for (size_t i = 0; i < pairs.size(); ++i) {
                    picks.push_back(Pick{pairs[i], triggers[i], game.slot_power(pairs[i]),
                                         shared.gains[static_cast<size_t>(pairs[i])]
                                                     [static_cast<size_t>(triggers[i])]});
                }
                const Evaluation evaluation = evaluate_picks(game, picks);
                if (evaluation.index > solution.index * (1.0 + 1e-12)) {
                    solution.picks = std::move(picks);
                    solution.evaluation = evaluation;
                    solution.index = evaluation.index;
                    solution.member_set = member_set_of(shared, solution.picks);
                    improved = true;
                    break;
                }
            }
        }
        if (!improved)
            return;
    }
}

struct AnnealStats {
    int64_t proposals = 0, accepted = 0, downhill = 0, restarts = 0;
};

std::vector<int> random_team(const Game &game, std::mt19937_64 &rng) {
    const auto &shared = *game.shared;
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::vector<int> members{game.leader_member};
        for (auto id : shared.required_members) {
            const int m = shared.engine->member_pos(id);
            if (m < 0)
                return {};
            if (std::find(members.begin(), members.end(), m) == members.end())
                members.push_back(m);
        }
        if (static_cast<int>(members.size()) > shared.team_size)
            return {};
        std::vector<int> choices;
        for (int m = 0; m < shared.member_count; ++m)
            choices.push_back(m);
        std::shuffle(choices.begin(), choices.end(), rng);
        for (int m : choices) {
            if (static_cast<int>(members.size()) == shared.team_size)
                break;
            if (std::find(members.begin(), members.end(), m) != members.end())
                continue;
            bool conflict = false;
            for (int old : members)
                if (shared.distinct_characters &&
                    shared.members[old]->character == shared.members[m]->character)
                    conflict = true;
            if (!conflict)
                members.push_back(m);
        }
        if (static_cast<int>(members.size()) != shared.team_size)
            continue;
        std::vector<int> snapshots;
        for (auto id : shared.required_snapshots) {
            int snap = shared.engine->snapshot_pos(id);
            if (snap < 0)
                return {};
            if (std::find(snapshots.begin(), snapshots.end(), snap) == snapshots.end())
                snapshots.push_back(snap);
        }
        if (static_cast<int>(snapshots.size()) > shared.team_size)
            return {};
        choices.clear();
        for (int snap = 0; snap < shared.snap_count; ++snap)
            choices.push_back(snap);
        std::shuffle(choices.begin(), choices.end(), rng);
        for (int snap : choices) {
            if (static_cast<int>(snapshots.size()) == shared.team_size)
                break;
            if (!shared.distinct_snapshots ||
                std::find(snapshots.begin(), snapshots.end(), snap) == snapshots.end())
                snapshots.push_back(snap);
        }
        while (!shared.distinct_snapshots && static_cast<int>(snapshots.size()) < shared.team_size)
            snapshots.push_back(static_cast<int>(rng() % shared.snap_count));
        if (static_cast<int>(snapshots.size()) != shared.team_size)
            continue;
        std::shuffle(snapshots.begin(), snapshots.end(), rng);
        std::vector<int> pairs;
        for (int i = 0; i < shared.team_size; ++i)
            pairs.push_back(members[i] * shared.snap_count + snapshots[i]);
        if (legal_pairs(game, pairs))
            return pairs;
    }
    return {};
}

Solution anneal(const Game &game, const Solution &seed, const RankOptions &options,
                std::mt19937_64 &rng, AnnealStats &stats) {
    const auto &shared = *game.shared;
    Solution best = seed;
    auto unit = [&] { return static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0); };
    for (int restart = 0; restart < options.restarts && wall_s() < shared.deadline; ++restart) {
        ++stats.restarts;
        std::vector<int> pairs;
        if (restart == 0 && seed.found)
            for (const auto &pick : seed.picks)
                pairs.push_back(pick.pair);
        else
            pairs = random_team(game, rng);
        Solution current = make_solution(game, pairs, false);
        if (!current.found)
            continue;
        if (!best.found || current.index > best.index)
            best = current;
        for (int step = 0; step < options.anneal_steps; ++step) {
            if ((step & 31) == 0 && wall_s() > shared.deadline)
                break;
            ++stats.proposals;
            std::vector<int> proposal;
            for (const auto &pick : current.picks)
                proposal.push_back(pick.pair);
            int pos = static_cast<int>(rng() % shared.team_size);
            if (shared.team_size > 1 && rng() % 4 == 0) {
                int other =
                    (pos + 1 + static_cast<int>(rng() % (shared.team_size - 1))) % shared.team_size;
                const auto left = shared.pairs[proposal[pos]],
                           right = shared.pairs[proposal[other]];
                proposal[pos] = left.member * shared.snap_count + right.snap;
                proposal[other] = right.member * shared.snap_count + left.snap;
            } else {
                const int replacements = (shared.team_size > 1 && rng() % 5 == 0) ? 2 : 1;
                for (int change = 0; change < replacements; ++change) {
                    int at = (pos + change) % shared.team_size;
                    auto item = shared.pairs[proposal[at]];
                    if (item.member != game.leader_member && rng() % 2 == 0)
                        item.member = static_cast<int>(rng() % shared.member_count);
                    item.snap = static_cast<int>(rng() % shared.snap_count);
                    proposal[at] = item.member * shared.snap_count + item.snap;
                }
            }
            Solution next = make_solution(game, proposal, false);
            if (!next.found)
                continue;
            const double delta = (next.index - current.index) / std::max(1.0, current.index);
            const double temperature =
                0.02 *
                std::pow(0.0005, static_cast<double>(step) / std::max(1, options.anneal_steps - 1));
            if (delta >= 0 || unit() < std::exp(delta / temperature)) {
                ++stats.accepted;
                if (delta < 0)
                    ++stats.downhill;
                current = std::move(next);
                if (!best.found || current.index > best.index)
                    best = current;
            }
        }
    }
    if (best.found) {
        local_improve(game, 8, best);
        best.evaluation = evaluate_picks(game, best.picks);
        best.index = best.evaluation.index;
    }
    return best;
}

struct NeighborhoodStats {
    int64_t neighborhoods = 0, proposals = 0, evaluations = 0, improvements = 0;
    int64_t bound_pruned = 0;
    double elapsed_s = 0;
};

double linear_score_upper(const Shared &shared, double power, double gain);
double completed_linear_gain(const Shared &shared, const std::vector<int> &pairs);

Solution large_neighborhood(const Game &game, Solution best, double deadline,
                            NeighborhoodStats &stats) {
    const auto &shared = *game.shared;
    if (!best.found || shared.team_size < 2)
        return best;
    const double started = wall_s();
    std::set<std::vector<int>> evaluated;
    for (int round = 0; round < 2 && wall_s() < deadline; ++round) {
        bool improved = false;
        for (int mask = 1; mask < (1 << shared.team_size) && wall_s() < deadline; ++mask) {
            const int count = __builtin_popcount(static_cast<unsigned>(mask));
            if (count != 2 && count != 3)
                continue;
            ++stats.neighborhoods;
            std::vector<int> pairs, positions;
            SelectionSet members, snapshots, characters;
            for (int i = 0; i < shared.team_size; ++i) {
                pairs.push_back(best.picks[i].pair);
                if (mask & (1 << i)) {
                    positions.push_back(i);
                } else {
                    const auto &item = shared.pairs[pairs.back()];
                    members.insert(item.member);
                    snapshots.insert(item.snap);
                    characters.insert(shared.members[item.member]->character);
                }
            }
            std::vector<std::vector<int>> choices;
            for (int position : positions) {
                const bool leader = shared.pairs[pairs[position]].member == game.leader_member;
                std::vector<int> row;
                std::unordered_map<int, int> per_member;
                auto add = [&](int pair, bool repair) {
                    const auto &item = shared.pairs[pair];
                    if ((item.member == game.leader_member) != leader ||
                        members.contains(item.member) ||
                        (shared.distinct_snapshots && snapshots.contains(item.snap)) ||
                        (shared.distinct_characters &&
                         characters.contains(shared.members[item.member]->character)) ||
                        std::find(row.begin(), row.end(), pair) != row.end())
                        return;
                    if (!repair && per_member[item.member] >= 2)
                        return;
                    row.push_back(pair);
                    ++per_member[item.member];
                };
                // Keep current cards and released Snapshots available for coordinated swaps.
                for (int other : positions)
                    add(shared.pairs[pairs[position]].member * shared.snap_count +
                            shared.pairs[pairs[other]].snap,
                        true);
                for (int pair : game.by_potential) {
                    if (row.size() >= 10)
                        break;
                    add(pair, false);
                }
                choices.push_back(std::move(row));
            }
            std::vector<std::pair<double, std::vector<int>>> shortlist;
            int64_t leaves = 0;
            auto visit = [&](auto &&self, size_t depth, SelectionSet used_members,
                             SelectionSet used_snapshots, SelectionSet used_characters) -> void {
                if (leaves >= 2048 || wall_s() >= deadline)
                    return;
                if (depth == positions.size()) {
                    ++leaves;
                    if (!legal_pairs(game, pairs))
                        return;
                    ++stats.proposals;
                    std::vector<int> key = pairs;
                    std::sort(key.begin(), key.end());
                    if (evaluated.count(key))
                        return;
                    double power = 0, gain = 0;
                    for (int pair : pairs) {
                        power += game.slot_power(pair);
                        gain += shared.max_gain(pair);
                    }
                    const double base = shared.score_objective && shared.linear_score_bound_enabled
                                            ? shared.score_base_coefficient
                                            : 1.0;
                    const double value = power * (base + gain);
                    if (shortlist.size() == 4 && value <= shortlist.back().first)
                        return;
                    if (std::any_of(shortlist.begin(), shortlist.end(), [&](const auto &entry) {
                            auto existing = entry.second;
                            std::sort(existing.begin(), existing.end());
                            return existing == key;
                        }))
                        return;
                    shortlist.emplace_back(value, pairs);
                    std::stable_sort(
                        shortlist.begin(), shortlist.end(),
                        [](const auto &a, const auto &b) { return a.first > b.first; });
                    if (shortlist.size() > 4)
                        shortlist.pop_back();
                    return;
                }
                for (int pair : choices[depth]) {
                    const auto &item = shared.pairs[pair];
                    const int character = shared.members[item.member]->character;
                    if (used_members.contains(item.member) ||
                        (shared.distinct_snapshots && used_snapshots.contains(item.snap)) ||
                        (shared.distinct_characters && used_characters.contains(character)))
                        continue;
                    pairs[positions[depth]] = pair;
                    self(self, depth + 1, used_members.with(item.member),
                         used_snapshots.with(item.snap), used_characters.with(character));
                }
            };
            visit(visit, 0, members, snapshots, characters);
            for (const auto &[potential, proposal] : shortlist) {
                if (wall_s() >= deadline)
                    break;
                auto key = proposal;
                std::sort(key.begin(), key.end());
                evaluated.insert(std::move(key));
                if (shared.score_objective && shared.linear_score_bound_enabled) {
                    double power = 0;
                    for (int pair : proposal)
                        power += game.slot_power(pair);
                    if (linear_score_upper(shared, power,
                                           completed_linear_gain(shared, proposal)) <= best.index) {
                        ++stats.bound_pruned;
                        continue;
                    }
                }
                ++stats.evaluations;
                auto candidate = make_solution(game, proposal, true);
                if (candidate.found && candidate.index > best.index) {
                    best = std::move(candidate);
                    ++stats.improvements;
                    improved = true;
                }
            }
        }
        if (!improved)
            break;
    }
    stats.elapsed_s += wall_s() - started;
    return best;
}

std::vector<float> completed_live_bound(const Shared &shared, const std::vector<int> &pairs) {
    const auto &engine = *shared.engine;
    const auto &chart = engine.chart();
    std::vector<float> bound(chart.times.size());
    for (size_t i = 0; i < chart.times.size(); ++i) {
        double sum = 0;
        for (int k = 0; k < shared.team_size; ++k) {
            const int64_t start = engine.problem().chart.skill_times_ms[k];
            float maximum = 0;
            for (int p : pairs) {
                const auto &pair = shared.pairs[p];
                if (chart.times[i] >= start &&
                    chart.times[i] < start + engine.duration_ms(pair.member, pair.snap))
                    maximum = std::max(maximum, static_cast<float>(engine.live_boost(pair.member)));
            }
            sum += maximum;
        }
        sum *= 1 + (shared.team_size + 1) * std::numeric_limits<float>::epsilon();
        bound[i] = std::nextafter(static_cast<float>(sum), std::numeric_limits<float>::infinity());
    }
    return bound;
}

void prepare_linear_score_bound(Shared &shared) {
    auto bound = shared.engine->linear_score_bound(shared.score_live_bound);
    shared.score_base_coefficient = bound.base;
    shared.score_gain_coefficients = std::move(bound.gains);
    if (shared.average_objective && bound.enabled)
        for (auto &row : shared.score_gain_coefficients) {
            // Each member occupies each trigger equally often in a uniform shuffle.
            long double sum = 0;
            for (double gain : row)
                sum += gain;
            const double mean = std::nextafter(static_cast<double>(sum / row.size()),
                                               std::numeric_limits<double>::infinity());
            std::fill(row.begin(), row.end(), mean);
        }
    shared.score_linear_max_power = bound.max_power;
    shared.linear_score_bound_enabled = bound.enabled;
}

double linear_score_upper(const Shared &shared, double power, double gain) {
    return NativeScoreBound::upper(power, gain, shared.score_base_coefficient,
                                   shared.score_linear_max_power, shared.team_size,
                                   shared.engine->chart().times.size());
}

double completed_linear_gain(const Shared &shared, const std::vector<int> &pairs) {
    const int count = 1 << shared.team_size;
    std::vector<double> dp(count, -std::numeric_limits<double>::infinity());
    dp[0] = 0;
    for (int mask = 0; mask < count; ++mask) {
        int n = __builtin_popcount(static_cast<unsigned>(mask));
        if (n == shared.team_size)
            continue;
        for (int k = 0; k < shared.team_size; ++k)
            if (!(mask & (1 << k))) {
                const double value =
                    std::nextafter(dp[mask] + shared.score_gain_coefficients[pairs[n]][k],
                                   std::numeric_limits<double>::infinity());
                dp[mask | (1 << k)] = std::max(dp[mask | (1 << k)], value);
            }
    }
    return dp.back();
}

struct Bnb {
    const Game *game = nullptr;
    const std::vector<SelectionSet> *excluded = nullptr;
    double deadline = 0.0;
    bool timed_out = false;
    double best = 0.0;
    std::vector<int> best_pairs;
    int64_t nodes = 0;
    int64_t cheap_bound_pruned = 0, last_improvement_node = 0;
    bool neighborhood_attempted = false;
    double lambda = 0;
    std::vector<std::array<double, kMaxTeam>> joint;
    Solution neighborhood_seed;
    NeighborhoodStats *neighborhood_stats = nullptr;
};

double relaxed_joint(const Bnb &ctx, const std::vector<int> &trial,
                     const std::vector<std::array<double, kMaxTeam>> &rows,
                     const std::vector<char> &available, int64_t power) {
    const auto &shared = *ctx.game->shared;
    const int count = 1 << shared.team_size;
    std::array<double, 1 << kMaxTeam> dp;
    dp.fill(-std::numeric_limits<double>::infinity());
    dp[0] = 0;
    for (int pair : trial) {
        std::array<double, 1 << kMaxTeam> next;
        next.fill(-std::numeric_limits<double>::infinity());
        for (int mask = 0; mask < count; ++mask)
            if (std::isfinite(dp[mask]))
                for (int k = 0; k < shared.team_size; ++k)
                    if (!(mask & (1 << k)))
                        next[mask | (1 << k)] = std::max(
                            next[mask | (1 << k)],
                            dp[mask] + ctx.lambda * shared.score_gain_coefficients[pair][k]);
        dp = next;
    }
    for (size_t row = 0; row < rows.size(); ++row)
        if (available[row])
            for (int mask = count - 1; mask >= 0; --mask)
                if (std::isfinite(dp[mask]))
                    for (int k = 0; k < shared.team_size; ++k)
                        if (!(mask & (1 << k)))
                            dp[mask | (1 << k)] =
                                std::max(dp[mask | (1 << k)], dp[mask] + rows[row][k]);
    const double q =
        static_cast<double>(power) + ctx.lambda * shared.score_base_coefficient + dp[count - 1];
    const double inflated =
        std::nextafter(q * (1 + (shared.team_size + 32) * std::numeric_limits<double>::epsilon()),
                       std::numeric_limits<double>::infinity());
    const double result =
        inflated * inflated / (4 * ctx.lambda) *
        std::pow(1 + std::numeric_limits<float>::epsilon(), shared.team_size + 40) *
        (1 + (shared.engine->chart().times.size() + 32) * std::numeric_limits<double>::epsilon());
    return std::nextafter(result, std::numeric_limits<double>::infinity());
}

void bnb_dfs(Bnb &ctx, int level, int min_pair, const SelectionSet &member_set,
             const SelectionSet &snapshot_set, const SelectionSet &character_set, int64_t power,
             std::vector<int> &trial) {
    const Shared &shared = *ctx.game->shared;
    const int team_size = shared.team_size;
    if (ctx.timed_out)
        return;
    ++ctx.nodes;
    if ((shared.score_objective || (ctx.nodes & 255) == 0) && wall_s() > ctx.deadline) {
        ctx.timed_out = true;
        return;
    }
    // Improve only competitive leaders whose exact search has stopped finding better teams.
    if (!ctx.neighborhood_attempted && ctx.nodes - ctx.last_improvement_node >= 512 &&
        ctx.neighborhood_stats && ctx.neighborhood_seed.found &&
        ctx.neighborhood_seed.index >= ctx.best * 0.98 &&
        ctx.neighborhood_stats->elapsed_s < 0.06) {
        ctx.neighborhood_attempted = true;
        const double neighborhood_deadline = std::min(
            ctx.deadline, wall_s() + std::min(0.02, 0.06 - ctx.neighborhood_stats->elapsed_s));
        auto candidate = large_neighborhood(*ctx.game, ctx.neighborhood_seed, neighborhood_deadline,
                                            *ctx.neighborhood_stats);
        if (candidate.index > ctx.best) {
            ctx.best = candidate.index;
            ctx.best_pairs.clear();
            for (const auto &pick : candidate.picks)
                ctx.best_pairs.push_back(pick.pair);
            ctx.last_improvement_node = ctx.nodes;
        }
        ctx.neighborhood_seed = std::move(candidate);
        if (wall_s() > ctx.deadline) {
            ctx.timed_out = true;
            return;
        }
    }
    if (level == team_size) {
        if (!legal_pairs(*ctx.game, trial))
            return;
        if (shared.score_objective) {
            if (shared.linear_score_bound_enabled &&
                linear_score_upper(shared, static_cast<double>(power),
                                   completed_linear_gain(shared, trial)) <= ctx.best)
                return;
            if (shared.engine->theoretical_upper_bound(static_cast<double>(power),
                                                       shared.score_live_bound) <= ctx.best)
                return;
            if (shared.engine->theoretical_upper_bound(
                    static_cast<double>(power), completed_live_bound(shared, trial)) <= ctx.best)
                return;
        }
        std::vector<int> triggers;
        const double gain = assign_triggers(shared, trial, triggers);
        Solution candidate;
        const bool evaluated =
            shared.score_objective || shared.engine->rules().formation_leader_conditions();
        if (evaluated)
            candidate = make_solution(*ctx.game, trial, true);
        const double value =
            evaluated ? candidate.index : static_cast<double>(power) * (1.0 + gain);
        if (shared.score_objective ? value > ctx.best : value > ctx.best * (1.0 + 1e-12)) {
            ctx.best = value;
            ctx.best_pairs = trial;
            ctx.last_improvement_node = ctx.nodes;
            if (evaluated)
                ctx.neighborhood_seed = std::move(candidate);
        }
        return;
    }

    const int remaining = team_size - level;

    const int group_count =
        shared.distinct_characters ? shared.character_count : shared.member_count;
    if (group_count < remaining)
        return;
    std::vector<double> group_power(group_count), group_gain(group_count);
    std::vector<char> allowed_members(shared.member_count);
    for (int m = min_pair + 1; m < shared.member_count; ++m) {
        if (member_set.contains(m))
            continue;
        const int c = shared.members[m]->character;
        if (shared.distinct_characters && (character_set.contains(c)))
            continue;
        allowed_members[m] = 1;
        const int group = shared.distinct_characters ? shared.character_groups[m] : m;
        group_power[group] = std::max(group_power[group], ctx.game->member_max_power[m]);
        group_gain[group] = std::max(group_gain[group], ctx.game->member_max_gain[m]);
    }
    std::sort(group_power.begin(), group_power.end(), std::greater<double>());
    std::sort(group_gain.begin(), group_gain.end(), std::greater<double>());
    double cheap_power = power, cheap_gain = 0;
    for (int pair : trial)
        cheap_gain += shared.max_gain(pair);
    for (int i = 0; i < remaining; ++i) {
        cheap_power += group_power[i];
        cheap_gain += group_gain[i];
    }
    // Relax Snapshot conflicts first, before scanning the full pair matrix.
    if (shared.score_objective ? shared.linear_score_bound_enabled &&
                                     linear_score_upper(shared, cheap_power, cheap_gain) <= ctx.best
                               : cheap_power * (1 + cheap_gain) <= ctx.best * (1 - 1e-12)) {
        ++ctx.cheap_bound_pruned;
        return;
    }
    std::fill(group_power.begin(), group_power.end(), 0);
    std::fill(group_gain.begin(), group_gain.end(), 0);
    std::array<double, kMaxTeam> trigger_max{};
    std::vector<double> snapshot_power(shared.snap_count), snapshot_gain(shared.snap_count);
    std::vector<std::array<double, kMaxTeam>> joint_groups(ctx.lambda > 0 ? group_count : 0),
        joint_snapshots(ctx.lambda > 0 ? shared.snap_count : 0);
    std::vector<char> group_available(group_count), snapshot_available(shared.snap_count);
    for (int p : trial)
        for (int k = 0; k < team_size; ++k)
            trigger_max[k] = std::max(trigger_max[k], shared.bound_gain(p, k));
    for (int pair : ctx.game->by_power) {
        const auto &item = shared.pairs[pair];
        if (!allowed_members[item.member])
            continue;
        if (shared.distinct_snapshots && (snapshot_set.contains(item.snap)))
            continue;
        const int character = shared.members[item.member]->character;
        if (shared.distinct_characters && (character_set.contains(character)))
            continue;
        const int key =
            shared.distinct_characters ? shared.character_groups[item.member] : item.member;
        const double pair_power = static_cast<double>(ctx.game->slot_power(pair)),
                     pair_gain = shared.max_gain(pair);
        group_power[key] = std::max(group_power[key], pair_power);
        group_gain[key] = std::max(group_gain[key], pair_gain);
        snapshot_power[item.snap] = std::max(snapshot_power[item.snap], pair_power);
        snapshot_gain[item.snap] = std::max(snapshot_gain[item.snap], pair_gain);
        group_available[key] = 1;
        snapshot_available[item.snap] = 1;
        for (int k = 0; k < team_size; ++k) {
            trigger_max[k] = std::max(trigger_max[k], shared.bound_gain(pair, k));
            if (ctx.lambda > 0) {
                joint_groups[key][k] = std::max(joint_groups[key][k], ctx.joint[pair][k]);
                joint_snapshots[item.snap][k] =
                    std::max(joint_snapshots[item.snap][k], ctx.joint[pair][k]);
            }
        }
    }
    if (std::count(group_available.begin(), group_available.end(), char{1}) < remaining)
        return;
    std::sort(group_power.begin(), group_power.end(), std::greater<double>());
    std::sort(group_gain.begin(), group_gain.end(), std::greater<double>());
    double power_bound = power, gain_bound = 0, trigger_bound = 0;
    for (int p : trial)
        gain_bound += shared.max_gain(p);
    for (int i = 0; i < remaining; ++i) {
        power_bound += group_power[i];
        gain_bound += group_gain[i];
    }
    for (int k = 0; k < team_size; ++k)
        trigger_bound += trigger_max[k];
    gain_bound = std::min(gain_bound, trigger_bound);
    if (shared.distinct_snapshots) {
        std::sort(snapshot_power.begin(), snapshot_power.end(), std::greater<double>());
        std::sort(snapshot_gain.begin(), snapshot_gain.end(), std::greater<double>());
        double sp = power, sg = 0;
        for (int p : trial)
            sg += shared.max_gain(p);
        for (int i = 0; i < remaining; ++i) {
            sp += snapshot_power[i];
            sg += snapshot_gain[i];
        }
        power_bound = std::min(power_bound, sp);
        gain_bound = std::min(gain_bound, sg);
    }
    if (shared.score_objective) {
        if (shared.linear_score_bound_enabled &&
            linear_score_upper(shared, power_bound, gain_bound) <= ctx.best)
            return;
        if (ctx.lambda > 0 && power_bound * (1 + 4 * std::numeric_limits<float>::epsilon()) <
                                  shared.score_linear_max_power) {
            if (relaxed_joint(ctx, trial, joint_groups, group_available, power) <= ctx.best)
                return;
            if (shared.distinct_snapshots &&
                relaxed_joint(ctx, trial, joint_snapshots, snapshot_available, power) <= ctx.best)
                return;
        }
        if (shared.engine->theoretical_upper_bound(power_bound, shared.score_live_bound) <=
            ctx.best)
            return;
    } else if (power_bound * (1 + gain_bound) <= ctx.best * (1 - 1e-12))
        return;

    for (int pair : ctx.game->by_potential) {
        if (!allowed_members[shared.pairs[pair].member])
            continue;
        if (!shared.any_kept(pair))
            continue;
        const Pair &item = shared.pairs[static_cast<size_t>(pair)];
        const int member_index = item.member;
        if (member_set.contains(member_index))
            continue;
        const int snapshot_index = item.snap;
        if (shared.distinct_snapshots && (snapshot_set.contains(snapshot_index)))
            continue;
        const int character = shared.members[static_cast<size_t>(item.member)]->character;
        if (shared.distinct_characters && (character_set.contains(character))) {
            continue;
        }
        if (level == team_size - 1 && ctx.excluded != nullptr) {
            const SelectionSet mask = member_set.with(member_index);
            bool banned = false;
            for (const SelectionSet &item_mask : *ctx.excluded) {
                if (item_mask == mask) {
                    banned = true;
                    break;
                }
            }
            if (banned)
                continue;
        }
        trial.push_back(pair);
        bnb_dfs(ctx, level + 1, item.member, member_set.with(member_index),
                snapshot_set.with(snapshot_index), character_set.with(character),
                power + ctx.game->slot_power(pair), trial);
        trial.pop_back();
        if (ctx.timed_out)
            return;
    }
}

}

RankResult rank_formations(const Engine &engine, const RankOptions &options) {
    const Problem &problem = engine.problem();
    const bool theoretical =
        options.objective == "theoretical_score" || options.objective == "mean_score";
    if (!theoretical && options.objective != "index")
        throw SpecError("objective 必须为 mean_score/theoretical_score/index");
    if (theoretical)
        engine.validate_theoretical_scope();
    const int team_size = engine.team_size();
    const double started = wall_s();
    const double budget =
        options.time_limit_s > 0 ? options.time_limit_s : std::numeric_limits<double>::infinity();
    const double deadline = started + budget;

    Shared shared;
    shared.engine = &engine;
    shared.average_objective = options.objective == "mean_score";
    shared.members = engine.members();
    shared.snapshots = engine.snapshots();
    shared.team_size = team_size;
    shared.member_count = static_cast<int>(shared.members.size());
    shared.snap_count = static_cast<int>(shared.snapshots.size());
    shared.distinct_characters = problem.constraints.distinct_characters;
    shared.distinct_snapshots = problem.constraints.distinct_snapshots;
    shared.required_members = problem.constraints.required_members;
    shared.required_snapshots = problem.constraints.required_snapshots;
    shared.deadline = deadline;
    if (team_size > kMaxTeam)
        throw SpecError("搜索支持最多 8 人队伍");
    std::map<int, int> character_groups;
    for (auto m : shared.members) {
        if (m->character < 0)
            throw SpecError("搜索角色编号必须非负");
        auto [it, added] = character_groups.try_emplace(m->character, shared.character_count);
        if (added)
            ++shared.character_count;
        shared.character_groups.push_back(it->second);
    }

    for (const auto &ids : options.excluded_member_sets) {
        if (static_cast<int>(ids.size()) != team_size)
            throw SpecError("排除组合必须恰有 team_size 张不同成员卡");
        SelectionSet mask;
        for (auto id : ids) {
            const int member = engine.member_pos(id);
            if (member < 0)
                throw SpecError("排除组合含不可用成员 " + std::to_string(id));
            if (mask.contains(member))
                throw SpecError("排除组合不能含重复成员");
            mask.insert(member);
        }
        shared.excluded_member_sets.push_back(mask);
    }
    const std::vector<std::vector<std::vector<double>>> &gains = engine.gains();
    shared.pairs.resize(static_cast<size_t>(shared.member_count) *
                        static_cast<size_t>(shared.snap_count));
    shared.gains.assign(shared.pairs.size(),
                        std::vector<double>(static_cast<size_t>(team_size), 0.0));
    for (int member = 0; member < shared.member_count; ++member) {
        for (int snap = 0; snap < shared.snap_count; ++snap) {
            const size_t index =
                static_cast<size_t>(member) * static_cast<size_t>(shared.snap_count) +
                static_cast<size_t>(snap);
            shared.pairs[index] = Pair{member, snap};
            for (int k = 0; k < team_size; ++k) {
                shared.gains[index][static_cast<size_t>(k)] =
                    gains[static_cast<size_t>(member)][static_cast<size_t>(snap)]
                         [static_cast<size_t>(k)];
            }
        }
    }
    if (theoretical) {

        const auto &chart = engine.chart();
        std::vector<std::vector<float>> maxima(team_size,
                                               std::vector<float>(chart.times.size(), 0));
        const auto &durations = engine.durations();
        const auto &boosts = engine.boosts();
        for (int m = 0; m < shared.member_count; ++m) {
            const int64_t duration = *std::max_element(durations[m].begin(), durations[m].end());
            const float value = static_cast<float>(boosts[m]);
            for (int k = 0; k < team_size; ++k) {
                int64_t start = problem.chart.skill_times_ms[k], end = start + duration;
                for (size_t i = 0; i < chart.times.size(); ++i)
                    if (chart.times[i] >= start && chart.times[i] < end)
                        maxima[k][i] = std::max(maxima[k][i], value);
            }
        }
        shared.score_live_bound.resize(chart.times.size());
        for (size_t i = 0; i < chart.times.size(); ++i) {
            double sum = 0;
            for (int k = 0; k < team_size; ++k)
                sum += maxima[k][i];
            sum *= 1 + (team_size + 1) * std::numeric_limits<float>::epsilon();
            shared.score_live_bound[i] =
                std::nextafter(static_cast<float>(sum), std::numeric_limits<float>::infinity());
        }
    }
    if (theoretical)
        prepare_linear_score_bound(shared);
    shared.index_max_gains.resize(shared.pairs.size());
    shared.score_max_gains.resize(shared.pairs.size());
    for (size_t pair = 0; pair < shared.pairs.size(); ++pair) {
        shared.index_max_gains[pair] =
            std::max(0.0, *std::max_element(shared.gains[pair].begin(), shared.gains[pair].end()));
        if (shared.linear_score_bound_enabled)
            shared.score_max_gains[pair] =
                std::max(0.0, *std::max_element(shared.score_gain_coefficients[pair].begin(),
                                                shared.score_gain_coefficients[pair].end()));
    }
    std::vector<int64_t> leader_ids;
    if (!options.leaders.empty()) {
        leader_ids = options.leaders;
    } else {
        for (const Member *member : problem.leader_candidates())
            leader_ids.push_back(member->id);
    }

    std::unordered_map<int64_t, double> leader_potential;
    for (auto id : leader_ids) {
        const int lm = engine.member_pos(id);
        if (lm < 0)
            throw SpecError("队长不在可用卡池: " + std::to_string(id));
        const auto &matrix = engine.power_matrix(static_cast<size_t>(lm));
        if (theoretical)
            for (const auto &row : matrix)
                for (auto value : row)
                    if (value < 0)
                        throw SpecError("理论分搜索的安全上界要求每格综合力非负");
        std::map<int, double> groups;
        for (int m = 0; m < shared.member_count; ++m) {
            if (m == lm)
                continue;
            if (shared.distinct_characters &&
                shared.members[m]->character == shared.members[lm]->character)
                continue;
            const int key = shared.distinct_characters ? shared.members[m]->character : m;
            groups[key] = std::max(groups[key], static_cast<double>(*std::max_element(
                                                    matrix[m].begin(), matrix[m].end())));
        }
        std::vector<double> values;
        for (const auto &entry : groups)
            values.push_back(entry.second);
        std::sort(values.begin(), values.end(), std::greater<double>());
        double bound = *std::max_element(matrix[lm].begin(), matrix[lm].end());
        for (int i = 0; i < team_size - 1 && i < static_cast<int>(values.size()); ++i)
            bound += values[i];
        leader_potential[id] = bound;
    }
    std::stable_sort(leader_ids.begin(), leader_ids.end(), [&](int64_t a, int64_t b) {
        return leader_potential[a] > leader_potential[b];
    });

    const auto is_class_search_eligible = [&]() {
        if (options.method == "fast")
            return false;
        if (!theoretical)
            return false;
        if (options.top < 1)
            return false;
        if (!shared.distinct_snapshots)
            return false;
        if (!shared.linear_score_bound_enabled)
            return false;
        if (shared.team_size > kMaxTeam)
            return false;
        for (auto id : leader_ids) {
            auto it = leader_potential.find(id);
            if (it == leader_potential.end())
                return false;
            double upper = linear_score_upper(
                shared, it->second,
                *std::max_element(shared.score_max_gains.begin(), shared.score_max_gains.end()) *
                    shared.team_size);
            if (!std::isfinite(upper) ||
                upper >= static_cast<double>(std::numeric_limits<int64_t>::max() / 2))
                return false;
        }
        return true;
    };
    const bool class_search_eligible = is_class_search_eligible();

    bool exact_mode = options.method == "exact";
    if (options.method == "auto") {
        if (class_search_eligible) {
            exact_mode = true;
        } else {
            exact_mode = static_cast<int64_t>(shared.member_count) * shared.snap_count <= 1024;
        }
    }

    std::unordered_map<SelectionSet, Solution, SelectionHash> pool;
    std::unordered_map<int64_t, Solution> best_by_leader;
    std::mt19937_64 rng(static_cast<uint64_t>(problem.search.seed));
    AnnealStats anneal_stats;
    NeighborhoodStats neighborhood_stats;
    int64_t dfs_nodes = 0, leaders_proven = 0, cheap_bound_pruned = 0;
    int64_t leaders_tried = 0;
    bool certified = exact_mode;

    auto build_game = [&](int64_t leader_id, bool use_index_pruning = true) {
        const int leader_member = engine.member_pos(leader_id);
        if (leader_member < 0) {
            throw SpecError("队长 " + std::to_string(leader_id) + " 不在可用成员中");
        }
        Game game;
        game.shared = &shared;
        const auto &matrix = engine.power_matrix(static_cast<size_t>(leader_member));
        game.flat_power.reserve(shared.pairs.size());
        for (const auto &row : matrix)
            game.flat_power.insert(game.flat_power.end(), row.begin(), row.end());
        game.leader_id = leader_id;
        game.leader_member = leader_member;
        if (use_index_pruning && !engine.rules().formation_leader_conditions())
            prune(shared, matrix);
        else
            shared.keep_at.assign(shared.pairs.size() * static_cast<size_t>(team_size), 1);
        game.by_power.resize(shared.pairs.size());
        for (size_t i = 0; i < shared.pairs.size(); ++i)
            game.by_power[i] = static_cast<int>(i);
        std::stable_sort(game.by_power.begin(), game.by_power.end(),
                         [&](int a, int b) { return game.slot_power(a) > game.slot_power(b); });
        const double reference_power = std::max(1.0, leader_potential[leader_id] * 0.5);
        const auto &potential_gains = theoretical && shared.linear_score_bound_enabled
                                          ? shared.score_max_gains
                                          : shared.index_max_gains;
        const double reference_gain =
            *std::max_element(potential_gains.begin(), potential_gains.end()) * team_size * 0.5;
        const double base =
            theoretical && shared.linear_score_bound_enabled ? shared.score_base_coefficient : 1.0;
        game.member_max_power.assign(shared.member_count, 0);
        game.member_max_gain.assign(shared.member_count, 0);
        std::vector<double> potential(shared.pairs.size());
        // A first-order score estimate orders branches; only proven bounds prune them.
        for (size_t pair = 0; pair < shared.pairs.size(); ++pair) {
            const int member = shared.pairs[pair].member;
            game.member_max_power[member] =
                std::max(game.member_max_power[member], static_cast<double>(game.slot_power(pair)));
            game.member_max_gain[member] =
                std::max(game.member_max_gain[member], potential_gains[pair]);
            potential[pair] = game.slot_power(pair) * (base + reference_gain) +
                              reference_power * potential_gains[pair];
        }
        game.by_potential = game.by_power;
        std::stable_sort(game.by_potential.begin(), game.by_potential.end(),
                         [&](int a, int b) { return potential[a] > potential[b]; });
        return game;
    };

    std::vector<Solution> verified_seed_solutions;
    int64_t supplied_seeds = 0, accepted_seeds = 0, excluded_seeds = 0;
    shared.score_objective = theoretical;
    for (const auto &formation : options.initial_formations) {
        ++supplied_seeds;
        formation.validate(problem);
        if (std::find(leader_ids.begin(), leader_ids.end(), formation.leader) == leader_ids.end())
            continue;
        Game game = build_game(formation.leader, false);
        std::vector<int> pairs;
        for (const auto &slot : formation.slots)
            pairs.push_back(engine.member_pos(slot.member) * shared.snap_count +
                            engine.snapshot_pos(slot.snapshot));
        if (!legal_pairs(game, pairs)) {
            ++excluded_seeds;
            continue;
        }
        Solution candidate = make_solution(game, pairs, true);
        ++accepted_seeds;
        verified_seed_solutions.push_back(candidate);
        auto it = pool.find(candidate.member_set);
        if (it == pool.end() || it->second.index < candidate.index)
            pool[candidate.member_set] = candidate;
        auto leader = best_by_leader.find(formation.leader);
        if (leader == best_by_leader.end() || leader->second.index < candidate.index)
            best_by_leader[formation.leader] = candidate;
    }
    const bool seeded_exact = exact_mode && !pool.empty();
    const bool class_search_enabled = exact_mode && class_search_eligible;
    std::unique_ptr<ClassSearch> class_search;
    const int64_t seed_score_evaluations = shared.score_evaluations;
    if (class_search_enabled) {
        class_search = std::make_unique<ClassSearch>(shared, options.top);
        for (const auto &entry : pool)
            class_search->pool.consider(entry.second);
    }
    const bool bypass_heuristics = seeded_exact || class_search_enabled;

    shared.score_objective = false;
    if (!bypass_heuristics) {

        for (auto &entry : pool)
            entry.second.index = entry.second.evaluation.index;
        for (auto &entry : best_by_leader)
            entry.second.index = entry.second.evaluation.index;

        for (int64_t leader_id : leader_ids) {
            if (wall_s() > deadline) {
                certified = false;
                break;
            }
            shared.deadline =
                std::min(deadline,
                         wall_s() + std::max(0.03, (deadline - wall_s()) /
                                                       std::max<int64_t>(1, static_cast<int64_t>(
                                                                                leader_ids.size()) -
                                                                                leaders_tried) *
                                                       (exact_mode ? 0.45 : 1.0)));
            Game game = build_game(leader_id);

            const std::vector<BeamState> frontier = beam(game, options.beam_width);
            std::vector<Solution> candidates;
            candidates.reserve(frontier.size());
            for (const BeamState &state : frontier) {
                if (state.depth != team_size)
                    continue;
                std::vector<int> pairs;
                pairs.reserve(static_cast<size_t>(team_size));
                for (int i = 0; i < team_size; ++i)
                    pairs.push_back(state.pairs[static_cast<size_t>(i)]);
                Solution solution = make_solution(game, pairs, false);
                if (solution.found)
                    candidates.push_back(std::move(solution));
            }
            std::stable_sort(
                candidates.begin(), candidates.end(),
                [](const Solution &a, const Solution &b) { return a.index > b.index; });

            std::unordered_set<SelectionSet, SelectionHash> raw_member_sets;
            for (const auto &raw : candidates) {
                if (static_cast<int>(raw_member_sets.size()) >=
                    std::max(8, std::min(24, options.top * 2)))
                    break;
                if (!raw_member_sets.insert(raw.member_set).second)
                    continue;
                Solution candidate = raw;
                candidate.evaluation = evaluate_picks(game, candidate.picks);
                candidate.index = candidate.evaluation.index;
                auto it = pool.find(candidate.member_set);
                if (it == pool.end() || it->second.index < candidate.index)
                    pool[candidate.member_set] = std::move(candidate);
            }
            const size_t improve_count = std::min<size_t>(16, candidates.size());
            for (size_t i = 0; i < improve_count; ++i)
                local_improve(game, 8, candidates[i]);
            std::stable_sort(
                candidates.begin(), candidates.end(),
                [](const Solution &a, const Solution &b) { return a.index > b.index; });

            const size_t evaluate_count = std::min<size_t>(24, candidates.size());
            for (size_t i = 0; i < evaluate_count; ++i) {
                candidates[i].evaluation = evaluate_picks(game, candidates[i].picks);
                candidates[i].index = candidates[i].evaluation.index;
                auto existing = pool.find(candidates[i].member_set);
                if (existing == pool.end() || existing->second.index < candidates[i].index) {
                    pool[candidates[i].member_set] = candidates[i];
                }
            }

            Solution seed;
            if (!candidates.empty())
                seed = candidates.front();
            if (!seed.found)
                seed = make_solution(game, random_team(game, rng), false);
            Solution strengthened = anneal(game, seed, options, rng, anneal_stats);
            if (!theoretical && strengthened.found)
                strengthened = large_neighborhood(game, std::move(strengthened),
                                                  std::min(shared.deadline, wall_s() + 0.005),
                                                  neighborhood_stats);
            if (strengthened.found) {
                auto existing = pool.find(strengthened.member_set);
                if (existing == pool.end() || existing->second.index < strengthened.index)
                    pool[strengthened.member_set] = strengthened;
                best_by_leader[leader_id] = strengthened;
            }

            ++leaders_tried;
            if (options.progress) {
                options.progress("leader " + std::to_string(leader_id) + " candidates " +
                                 std::to_string(pool.size()));
            }
            if (wall_s() > deadline) {
                certified = false;
                break;
            }
        }
    }
    if (theoretical && !bypass_heuristics) {

        std::vector<Solution> shortlist;
        for (auto &entry : pool)
            shortlist.push_back(std::move(entry.second));
        std::stable_sort(shortlist.begin(), shortlist.end(),
                         [](const Solution &a, const Solution &b) { return a.index > b.index; });
        pool.clear();
        best_by_leader.clear();
        shared.score_objective = true;
        for (auto &candidate : shortlist) {
            if (!pool.empty() && wall_s() > deadline) {
                certified = false;
                break;
            }
            Game game = build_game(candidate.evaluation.leader, false);
            candidate.evaluation = evaluate_picks(game, candidate.picks);
            candidate.index = *candidate.evaluation.ranking_score;
            auto incumbent = best_by_leader.find(game.leader_id);
            if (incumbent == best_by_leader.end() || incumbent->second.index < candidate.index)
                best_by_leader[game.leader_id] = candidate;
            pool[candidate.member_set] = std::move(candidate);
        }
    }

    for (const auto &seed : verified_seed_solutions) {
        auto existing = pool.find(seed.member_set);
        if (existing == pool.end() || existing->second.index < seed.index)
            pool[seed.member_set] = seed;
        auto leader = best_by_leader.find(seed.evaluation.leader);
        if (leader == best_by_leader.end() || leader->second.index < seed.index)
            best_by_leader[seed.evaluation.leader] = seed;
    }
    shared.score_objective = theoretical;

    if (theoretical && !bypass_heuristics && !exact_mode) {
        std::vector<int64_t> neighborhood_leaders;
        for (const auto &[leader_id, candidate] : best_by_leader)
            neighborhood_leaders.push_back(leader_id);
        std::stable_sort(neighborhood_leaders.begin(), neighborhood_leaders.end(),
                         [&](int64_t a, int64_t b) {
                             const double left = best_by_leader.at(a).index;
                             const double right = best_by_leader.at(b).index;
                             return left != right ? left > right : a < b;
                         });
        if (neighborhood_leaders.size() > 3)
            neighborhood_leaders.resize(3);
        const double neighborhood_deadline = std::min(deadline, wall_s() + 0.06);
        for (int64_t leader_id : neighborhood_leaders) {
            auto incumbent = best_by_leader.find(leader_id);
            if (wall_s() >= neighborhood_deadline)
                continue;
            Game game = build_game(leader_id, false);
            auto candidate = large_neighborhood(game, incumbent->second,
                                                std::min(neighborhood_deadline, wall_s() + 0.02),
                                                neighborhood_stats);
            incumbent->second = candidate;
            auto existing = pool.find(candidate.member_set);
            if (existing == pool.end() || existing->second.index < candidate.index)
                pool[candidate.member_set] = std::move(candidate);
        }
    }

    shared.deadline = deadline;
    if (exact_mode) {
        for (int64_t leader_id : leader_ids) {
            if (wall_s() > deadline) {
                certified = false;
                break;
            }
            if (class_search) {
                auto &search = *class_search;
                const double max_leader_gain =
                    shared.team_size *
                    *std::max_element(shared.score_max_gains.begin(), shared.score_max_gains.end());
                const double leader_score_cap =
                    search.cap(leader_potential[leader_id], max_leader_gain);
                if (leader_score_cap <= search.pool.cutoff()) {
                    ++leaders_proven;
                    continue;
                }
                search.game = build_game(leader_id, false);
                search.candidates.resize(shared.member_count);
                std::iota(search.candidates.begin(), search.candidates.end(), 0);
                const double p = std::max(1.0, leader_potential[leader_id] * 0.5);
                const double g = *std::max_element(search.game.member_max_gain.begin(),
                                                   search.game.member_max_gain.end()) *
                                 team_size * 0.5;
                std::stable_sort(
                    search.candidates.begin(), search.candidates.end(), [&](int a, int b) {
                        return search.game.member_max_power[a] * (search.bound.base + g) +
                                   p * search.game.member_max_gain[a] >
                               search.game.member_max_power[b] * (search.bound.base + g) +
                                   p * search.game.member_max_gain[b];
                    });
                const int lm = search.game.leader_member;
                search.members = {lm};
                std::array<double, kMaxTeam> selected_pos_max{};
                for (int k = 0; k < shared.team_size; ++k)
                    selected_pos_max[k] = search.member_pos_gain[lm][k];
                search.compositions_dfs(0, SelectionSet::single(shared.members[lm]->character),
                                        search.game.member_max_power[lm],
                                        search.game.member_max_gain[lm], selected_pos_max);
                if (search.timed_out) {
                    certified = false;
                    break;
                }
                ++leaders_proven;
                if (options.progress)
                    options.progress("proven leader " + std::to_string(leader_id) +
                                     " member nodes " + std::to_string(search.nodes));
                continue;
            }
            auto incumbent = best_by_leader.find(leader_id);
            Game game = build_game(leader_id, !theoretical);
            Bnb ctx;
            ctx.game = &game;
            ctx.excluded = &shared.excluded_member_sets;
            ctx.deadline = deadline;
            if (theoretical && !seeded_exact && incumbent != best_by_leader.end()) {
                ctx.neighborhood_seed = incumbent->second;
                ctx.neighborhood_stats = &neighborhood_stats;
            }
            ctx.best = incumbent == best_by_leader.end() ? -std::numeric_limits<double>::infinity()
                                                         : incumbent->second.index;

            for (const auto &entry : pool)
                ctx.best = std::max(ctx.best, entry.second.index);
            if (theoretical && shared.linear_score_bound_enabled && ctx.best > 0) {
                double best_power = 1;
                for (const auto &entry : pool)
                    if (entry.second.index == ctx.best) {
                        best_power = entry.second.evaluation.power;
                        break;
                    }
                ctx.lambda = best_power * best_power / ctx.best;
                ctx.joint.resize(shared.pairs.size());
                for (size_t pair = 0; pair < shared.pairs.size(); ++pair)
                    for (int k = 0; k < team_size; ++k)
                        ctx.joint[pair][k] = static_cast<double>(game.slot_power(pair)) +
                                             ctx.lambda * shared.score_gain_coefficients[pair][k];
            }
            std::vector<int> trial;
            int roots = 0;
            for (int pair : game.by_potential) {
                if (shared.pairs[pair].member != game.leader_member)
                    continue;
                ++roots;
                if (!shared.any_kept(pair))
                    continue;
                trial.push_back(pair);
                bnb_dfs(ctx, 1, -1, SelectionSet::single(game.leader_member),
                        SelectionSet::single(shared.pairs[pair].snap),
                        SelectionSet::single(
                            shared.members[static_cast<size_t>(game.leader_member)]->character),
                        game.slot_power(pair), trial);
                trial.pop_back();
                if (ctx.timed_out)
                    break;
                if (options.progress && (roots % 8 == 0 || roots == shared.snap_count))
                    options.progress("leader " + std::to_string(leader_id) + " Snapshot roots " +
                                     std::to_string(roots) + "/" +
                                     std::to_string(shared.snap_count) + " nodes " +
                                     std::to_string(ctx.nodes));
            }
            dfs_nodes += ctx.nodes;
            cheap_bound_pruned += ctx.cheap_bound_pruned;
            if (ctx.best_pairs.size() == static_cast<size_t>(team_size)) {
                Solution solution = ctx.neighborhood_seed.found
                                        ? std::move(ctx.neighborhood_seed)
                                        : make_solution(game, ctx.best_pairs, true);
                auto existing = pool.find(solution.member_set);
                if (existing == pool.end() || existing->second.index < solution.index) {
                    pool[solution.member_set] = std::move(solution);
                }
            }
            if (ctx.timed_out) {
                certified = false;
                break;
            }
            ++leaders_proven;
            if (options.progress)
                options.progress("proven leader " + std::to_string(leader_id) + " nodes " +
                                 std::to_string(ctx.nodes) + " global lower bound " +
                                 std::to_string(ctx.best));
        }
    }

    if (class_search) {
        for (const auto &entry : class_search->pool.pool) {
            auto it = pool.find(entry.first);
            if (it == pool.end() || SolutionBetter{}(entry.second, it->second)) {
                pool[entry.first] = entry.second;
            }
        }
    }

    std::vector<Solution> ordered;
    ordered.reserve(pool.size());
    for (auto &entry : pool)
        ordered.push_back(entry.second);
    std::stable_sort(ordered.begin(), ordered.end(), SolutionBetter{});
    if (static_cast<int>(ordered.size()) > options.top)
        ordered.resize(static_cast<size_t>(options.top));

    RankResult result;
    for (Solution &solution : ordered) {
        if (class_search) {
            Formation formation;
            formation.leader = solution.evaluation.leader;
            for (const auto &slot : solution.evaluation.slots)
                formation.slots.push_back({slot.member, slot.snapshot, slot.trigger});
            const auto replay = shared.average_objective ? engine.evaluate_mean(formation)
                                                         : engine.evaluate_theoretical(formation);
            if (replay.ranking_score != solution.evaluation.ranking_score)
                throw SpecError("技能分类搜索与完整分数复算不一致");
            solution.evaluation = replay;
            int64_t orders = 1;
            for (int i = 2; i <= team_size; ++i)
                orders *= i;
            class_search->order_stats.evaluated += orders;
        }
        engine.populate_estimated_score(solution.evaluation);
        result.results.push_back(std::move(solution.evaluation));
    }
    if (!result.results.empty() && !certified) {
        result.results.front().warnings.push_back("搜索结果未获最优性证明，只能作为候选。");
    }

    Json audit = Json::object();
    audit.set("solver", Json(class_search ? "snapshot-classes-matching-dfs"
                             : exact_mode ? "beam-annealing-lns-dp-dfs"
                                          : "beam-annealing-lns-dp"));
    audit.set("lns_neighborhoods", Json(neighborhood_stats.neighborhoods));
    audit.set("lns_proposals", Json(neighborhood_stats.proposals));
    audit.set("lns_evaluations", Json(neighborhood_stats.evaluations));
    audit.set("lns_improvements", Json(neighborhood_stats.improvements));
    audit.set("lns_bound_pruned", Json(neighborhood_stats.bound_pruned));
    audit.set("lns_elapsed_s", Json(neighborhood_stats.elapsed_s));
    audit.set("dfs_branch_order", Json("power_and_skill_potential"));
    audit.set("dfs_nodes",
              Json(class_search ? class_search->nodes + class_search->class_nodes : dfs_nodes));
    if (class_search) {
        int64_t classes = 0;
        for (const auto &row : class_search->classes)
            classes += row.size();
        audit.set("snapshot_skill_classes", Json(classes));
        audit.set("member_compositions", Json(class_search->compositions));
        audit.set("snapshot_matchings", Json(class_search->matchings));
        audit.set("snapshot_matching_cache_hits", Json(class_search->matching_cache_hits));
        audit.set("composition_bound_pruned", Json(class_search->composition_bound_pruned));
        audit.set("class_orders_pruned", Json(class_search->order_bound_pruned));
        audit.set("class_bound_pruned", Json(class_search->pruned));
        audit.set("score_orders_pruned", Json(class_search->order_stats.pruned));
    }
    audit.set("cheap_bound_pruned", Json(cheap_bound_pruned));
    audit.set("anneal_proposals", Json(anneal_stats.proposals));
    audit.set("anneal_accepted", Json(anneal_stats.accepted));
    audit.set("anneal_downhill_accepted", Json(anneal_stats.downhill));
    audit.set("anneal_restarts", Json(anneal_stats.restarts));
    audit.set("objective", Json(options.objective));
    Json excluded = Json::array();
    for (const auto &ids : options.excluded_member_sets) {
        Json row = Json::array();
        for (auto id : ids)
            row.push_back(Json(id));
        excluded.push_back(std::move(row));
    }
    audit.set("excluded_member_sets", std::move(excluded));
    audit.set("theoretical_max_certified", Json(options.objective == "theoretical_score" &&
                                                certified && !result.results.empty()));
    audit.set("mean_score_certified",
              Json(shared.average_objective && certified && !result.results.empty()));
    const bool is_topk_certified = class_search != nullptr && certified;
    audit.set("top_k_certified", Json(is_topk_certified));
    audit.set("top_k_complete", Json(is_topk_certified));
    audit.set("top_k_exhausted",
              Json(is_topk_certified && static_cast<int>(result.results.size()) < options.top));
    audit.set("requested_count", Json(static_cast<int64_t>(options.top)));
    audit.set("returned_count", Json(static_cast<int64_t>(result.results.size())));
    audit.set("input_mode", Json(problem.input_mode));
    audit.set("infeasible_proven", Json(certified && result.results.empty()));
    audit.set("score_dfs_pruning",
              Json(theoretical ? "native_float32_monotone_and_linear_envelope" : "index_bound"));
    audit.set("linear_score_bound_enabled", Json(shared.linear_score_bound_enabled));
    audit.set("mean_score_bound_enabled",
              Json(shared.average_objective && shared.linear_score_bound_enabled));
    audit.set("global_incumbent_pruning", Json(exact_mode));
    audit.set("supplied_seed_formations", Json(supplied_seeds));
    audit.set("accepted_seed_formations", Json(accepted_seeds));
    audit.set("excluded_seed_formations", Json(excluded_seeds));
    audit.set("warm_start_strategy", Json(seeded_exact   ? "validated_prior_formations"
                                          : class_search ? "none"
                                                         : "beam_annealing"));
    audit.set("complete_beam_variants_per_member_set", Json(4));
    audit.set("index_snapshot_pruning_used_in_score_dfs", Json(false));
    if (is_topk_certified && options.top > 1) {
        audit.set(
            "certified_scope",
            Json(
                shared.average_objective
                    ? "exact top-k distinct member sets by maximum mean rounded AP model score "
                      "over uniformly random skill permutations within requested leaders/card pool"
                    : "exact top-k distinct member sets by maximum rounded AP model score over all "
                      "legal formations and all skill permutations within requested leaders/card "
                      "pool after excluded member sets"));
    } else {
        audit.set("certified_scope",
                  Json(shared.average_objective
                           ? "maximum mean rounded AP model score over uniformly random skill "
                             "permutations "
                             "within requested leaders/card pool; other top entries are candidates"
                       : theoretical
                           ? "maximum rounded AP model score over all legal formations and all "
                             "skill permutations within requested leaders/card pool after "
                             "excluded member sets; other top entries are candidates"
                           : "best index within requested leaders and card constraints; "
                             "remaining top entries are candidates"));
    }
    audit.set("warm_start_objective", Json("index"));
    audit.set("score_model_calibrated", Json(false));
    audit.set("score_formations_evaluated", Json(shared.score_evaluations));
    audit.set("score_cache_hits", Json(shared.score_cache_hits));
    int64_t permutations = 1;
    for (int i = 2; i <= team_size; ++i)
        permutations *= i;
    audit.set("score_orders_evaluated",
              Json(class_search
                       ? seed_score_evaluations * permutations + class_search->order_stats.evaluated
                       : shared.score_evaluations * permutations));
    audit.set("certified", Json(certified));
    audit.set("stop_reason",
              Json(certified ? "searched" : (exact_mode ? "time_limit" : "heuristic")));
    audit.set("elapsed_s", Json(wall_s() - started));
    audit.set("candidates", Json(static_cast<int64_t>(pool.size())));
    audit.set("leaders_tried", Json(leaders_tried));
    audit.set("leaders_requested", Json(static_cast<int64_t>(leader_ids.size())));
    audit.set("leaders_proven", Json(leaders_proven));
    audit.set("beam_width", Json(static_cast<int64_t>(options.beam_width)));
    audit.set("restarts", Json(static_cast<int64_t>(options.restarts)));
    audit.set("anneal_steps", Json(static_cast<int64_t>(options.anneal_steps)));
    audit.set("seed", Json(problem.search.seed));
    result.audit = audit;
    return result;
}

}
