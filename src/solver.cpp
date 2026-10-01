#include "solver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
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
    std::vector<uint64_t> excluded_member_masks;
    std::vector<float> score_live_bound;
    mutable int64_t score_evaluations = 0;
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

    int64_t slot_power(int pair) const {
        return flat_power[static_cast<size_t>(pair)];
    }
};

struct Solution {
    double index = 0.0;
    uint64_t member_mask = 0;
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
        ++shared.score_evaluations;
        return shared.engine->evaluate_theoretical(formation);
    }
    EvalOptions options;
    options.calculate_score = false;
    return shared.engine->evaluate(formation, options);
}

uint64_t member_mask_of(const Shared &shared, const std::vector<Pick> &picks) {
    uint64_t mask = 0;
    for (const Pick &pick : picks) {
        mask |= uint64_t{1} << static_cast<uint32_t>(
                    shared.pairs[static_cast<size_t>(pick.pair)].member);
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
    uint64_t mask = 0;
    for (int member : members)
        mask |= uint64_t{1} << member;
    for (auto excluded : shared.excluded_member_masks)
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
    solution.member_mask = member_mask_of(shared, solution.picks);
    solution.index = static_cast<double>(power_sum) * (1.0 + gain);
    solution.found = true;
    if (with_evaluation) {
        solution.evaluation = evaluate_picks(game, solution.picks);
        solution.index = solution.evaluation.ranking_score.value_or(solution.evaluation.index);
    }
    return solution;
}

struct BeamState {
    int depth = 0;
    uint64_t member_mask = 0;
    uint64_t snap_mask = 0;
    uint64_t character_mask = 0;
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
        state.member_mask = uint64_t{1} << static_cast<uint32_t>(leader);
        state.snap_mask = uint64_t{1} << static_cast<uint32_t>(snap);
        state.character_mask = uint64_t{1} << static_cast<uint32_t>(
                                   shared.members[static_cast<size_t>(leader)]->character);
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
                const uint64_t member_bit = uint64_t{1} << static_cast<uint32_t>(item.member);
                if (state.member_mask & member_bit)
                    continue;
                const uint64_t snap_bit = uint64_t{1} << static_cast<uint32_t>(item.snap);
                if (shared.distinct_snapshots && (state.snap_mask & snap_bit))
                    continue;
                const int character = shared.members[static_cast<size_t>(item.member)]->character;
                if (shared.distinct_characters &&
                    (state.character_mask & (uint64_t{1} << static_cast<uint32_t>(character)))) {
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
        std::unordered_map<uint64_t, int> member_set_count;
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
                const uint64_t member_set =
                    state.member_mask | (uint64_t{1} << shared.pairs[child.pair].member);
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
            state.member_mask |= uint64_t{1} << static_cast<uint32_t>(item.member);
            state.snap_mask |= uint64_t{1} << static_cast<uint32_t>(item.snap);
            state.character_mask |=
                uint64_t{1} << static_cast<uint32_t>(
                    shared.members[static_cast<size_t>(item.member)]->character);
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
            uint64_t member_mask = 0;
            uint64_t snap_mask = 0;
            uint64_t character_mask = 0;
            for (int i = 0; i < team_size; ++i) {
                if (i == position)
                    continue;
                const Pair &item =
                    shared.pairs[static_cast<size_t>(solution.picks[static_cast<size_t>(i)].pair)];
                member_mask |= uint64_t{1} << static_cast<uint32_t>(item.member);
                snap_mask |= uint64_t{1} << static_cast<uint32_t>(item.snap);
                character_mask |= uint64_t{1} << static_cast<uint32_t>(
                                      shared.members[static_cast<size_t>(item.member)]->character);
            }
            for (int pair : game.by_power) {
                if (pair == solution.picks[static_cast<size_t>(position)].pair)
                    continue;
                if (!shared.any_kept(pair))
                    continue;
                const Pair &item = shared.pairs[static_cast<size_t>(pair)];
                if (leader_slot && item.member != game.leader_member)
                    continue;
                if (member_mask & (uint64_t{1} << static_cast<uint32_t>(item.member)))
                    continue;
                if (shared.distinct_snapshots &&
                    (snap_mask & (uint64_t{1} << static_cast<uint32_t>(item.snap))))
                    continue;
                const int character = shared.members[static_cast<size_t>(item.member)]->character;
                if (shared.distinct_characters &&
                    (character_mask & (uint64_t{1} << static_cast<uint32_t>(character)))) {
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
                    solution.member_mask = member_mask_of(shared, solution.picks);
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
    const auto &e = *shared.engine;
    const auto &chart = e.chart();
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
        return;
    std::vector<long double> coefficients(chart.times.size());
    long double base = 0;
    for (size_t i = 0; i < chart.times.size(); ++i) {
        const float weight = static_cast<float>(chart.note_weights[i]);
        const float combo = chart.native_combo_bonuses[i] + 1.0f;
        if (weight == 0)
            continue;
        if (weight < 0 || !std::isnormal(weight) || combo <= 0 || !std::isnormal(combo))
            return;
        float check = adjustment;
        for (float operand : {difficulty, weight, judgement, combo}) {
            check *= operand;
            if (!std::isnormal(check))
                return;
        }
        check /= denominator;
        if (!std::isnormal(check))
            return;

        check = life * assist;
        if (!std::isnormal(check))
            return;
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
    shared.score_base_coefficient = up(base);
    shared.score_gain_coefficients.assign(shared.pairs.size(),
                                          std::vector<double>(shared.team_size));
    for (size_t p = 0; p < shared.pairs.size(); ++p) {
        const auto &pair = shared.pairs[p];
        const float boost = static_cast<float>(e.live_boost(pair.member));
        if (boost < 0 || !std::isfinite(boost))
            return;
        for (int k = 0; k < shared.team_size; ++k) {
            const auto start = e.problem().chart.skill_times_ms[k];
            const auto end = start + e.duration_ms(pair.member, pair.snap);
            long double sum = 0;
            for (size_t i = 0; i < chart.times.size(); ++i)
                if (chart.times[i] >= start && chart.times[i] < end)
                    sum += coefficients[i] * boost;
            shared.score_gain_coefficients[p][k] = up(sum);
        }
    }
    long double peak = 1;
    float max_weight = 0, max_combo = 1, max_live = 0;
    for (size_t i = 0; i < chart.times.size(); ++i) {
        max_weight = std::max(max_weight, static_cast<float>(chart.note_weights[i]));
        max_combo = std::max(max_combo, chart.native_combo_bonuses[i] + 1.0f);
        max_live = std::max(max_live, shared.score_live_bound[i]);
    }
    if (!std::isfinite(max_live))
        return;
    for (long double f :
         {static_cast<long double>(adjustment), static_cast<long double>(difficulty),
          static_cast<long double>(max_weight), static_cast<long double>(judgement),
          static_cast<long double>(max_combo), 1 + static_cast<long double>(max_live),
          1 / static_cast<long double>(denominator), static_cast<long double>(life),
          static_cast<long double>(assist)})
        peak *= std::max(1.0L, f);
    peak *= std::pow(1 + static_cast<long double>(std::numeric_limits<float>::epsilon()), 64);
    shared.score_linear_max_power =
        std::nextafter(static_cast<double>(std::numeric_limits<float>::max() / peak), 0.0);
    shared.linear_score_bound_enabled = true;
}

double linear_score_upper(const Shared &shared, double power, double gain) {
    const double inflated = std::nextafter(
        power * (1 + (shared.team_size + 2) * std::numeric_limits<double>::epsilon()),
        std::numeric_limits<double>::infinity());
    const float native_power =
        std::nextafter(static_cast<float>(inflated), std::numeric_limits<float>::infinity());
    if (!std::isfinite(native_power) || native_power > shared.score_linear_max_power)
        return std::numeric_limits<double>::infinity();
    const double rounding =
        std::pow(1 + std::numeric_limits<float>::epsilon(), 32 + shared.team_size);
    const double sums =
        1 + (shared.engine->chart().times.size() + 32) * std::numeric_limits<double>::epsilon();
    return std::nextafter(static_cast<double>(native_power) *
                              (shared.score_base_coefficient + gain) * rounding * sums,
                          std::numeric_limits<double>::infinity());
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
    const std::vector<uint64_t> *excluded = nullptr;
    double deadline = 0.0;
    bool timed_out = false;
    double best = 0.0;
    std::vector<int> best_pairs;
    int64_t nodes = 0;
    double lambda = 0;
    std::vector<std::array<double, kMaxTeam>> joint;
};

double relaxed_joint(const Bnb &ctx, const std::vector<int> &trial,
                     const std::array<std::array<double, kMaxTeam>, 64> &rows, uint64_t available,
                     int64_t power) {
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
    for (int row = 0; row < 64; ++row)
        if (available & (uint64_t{1} << row))
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

void bnb_dfs(Bnb &ctx, int level, int min_pair, uint64_t member_mask, uint64_t snap_mask,
             uint64_t character_mask, int64_t power, std::vector<int> &trial) {
    const Shared &shared = *ctx.game->shared;
    const int team_size = shared.team_size;
    if (ctx.timed_out)
        return;
    ++ctx.nodes;
    if ((shared.score_objective || (ctx.nodes & 255) == 0) && wall_s() > ctx.deadline) {
        ctx.timed_out = true;
        return;
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
        const double value = shared.score_objective ? make_solution(*ctx.game, trial, true).index
                                                    : static_cast<double>(power) * (1.0 + gain);
        if (shared.score_objective ? value > ctx.best : value > ctx.best * (1.0 + 1e-12)) {
            ctx.best = value;
            ctx.best_pairs = trial;
        }
        return;
    }

    const int remaining = team_size - level;

    std::array<double, 64> group_power{}, group_gain{};
    uint64_t allowed_members = 0;
    for (int m = min_pair + 1; m < shared.member_count; ++m) {
        if (member_mask & (uint64_t{1} << m))
            continue;
        const int c = shared.members[m]->character;
        if (shared.distinct_characters && (character_mask & (uint64_t{1} << c)))
            continue;
        allowed_members |= uint64_t{1} << m;
    }
    std::array<double, kMaxTeam> trigger_max{};
    std::array<double, 64> snapshot_power{}, snapshot_gain{};
    std::array<std::array<double, kMaxTeam>, 64> joint_groups{}, joint_snapshots{};
    uint64_t group_available = 0, snapshot_available = 0;
    for (int p : trial)
        for (int k = 0; k < team_size; ++k)
            trigger_max[k] = std::max(trigger_max[k], shared.bound_gain(p, k));
    for (int pair : ctx.game->by_power) {
        const auto &item = shared.pairs[pair];
        if (!(allowed_members & (uint64_t{1} << item.member)))
            continue;
        if (shared.distinct_snapshots && (snap_mask & (uint64_t{1} << item.snap)))
            continue;
        const int character = shared.members[item.member]->character;
        if (shared.distinct_characters && (character_mask & (uint64_t{1} << character)))
            continue;
        const int key = shared.distinct_characters ? character : item.member;
        const double pair_power = static_cast<double>(ctx.game->slot_power(pair)),
                     pair_gain = shared.max_gain(pair);
        group_power[key] = std::max(group_power[key], pair_power);
        group_gain[key] = std::max(group_gain[key], pair_gain);
        snapshot_power[item.snap] = std::max(snapshot_power[item.snap], pair_power);
        snapshot_gain[item.snap] = std::max(snapshot_gain[item.snap], pair_gain);
        group_available |= uint64_t{1} << key;
        snapshot_available |= uint64_t{1} << item.snap;
        for (int k = 0; k < team_size; ++k) {
            trigger_max[k] = std::max(trigger_max[k], shared.bound_gain(pair, k));
            if (ctx.lambda > 0) {
                joint_groups[key][k] = std::max(joint_groups[key][k], ctx.joint[pair][k]);
                joint_snapshots[item.snap][k] =
                    std::max(joint_snapshots[item.snap][k], ctx.joint[pair][k]);
            }
        }
    }
    if (__builtin_popcountll(group_available) < remaining)
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

    for (int pair : ctx.game->by_power) {
        if (!(allowed_members & (uint64_t{1} << shared.pairs[pair].member)))
            continue;
        if (!shared.any_kept(pair))
            continue;
        const Pair &item = shared.pairs[static_cast<size_t>(pair)];
        const uint64_t member_bit = uint64_t{1} << static_cast<uint32_t>(item.member);
        if (member_mask & member_bit)
            continue;
        const uint64_t snap_bit = uint64_t{1} << static_cast<uint32_t>(item.snap);
        if (shared.distinct_snapshots && (snap_mask & snap_bit))
            continue;
        const int character = shared.members[static_cast<size_t>(item.member)]->character;
        if (shared.distinct_characters &&
            (character_mask & (uint64_t{1} << static_cast<uint32_t>(character)))) {
            continue;
        }
        if (level == team_size - 1 && ctx.excluded != nullptr) {
            const uint64_t mask = member_mask | member_bit;
            bool banned = false;
            for (uint64_t item_mask : *ctx.excluded) {
                if (item_mask == mask) {
                    banned = true;
                    break;
                }
            }
            if (banned)
                continue;
        }
        trial.push_back(pair);
        bnb_dfs(ctx, level + 1, item.member, member_mask | member_bit, snap_mask | snap_bit,
                character_mask | (uint64_t{1} << static_cast<uint32_t>(character)),
                power + ctx.game->slot_power(pair), trial);
        trial.pop_back();
        if (ctx.timed_out)
            return;
    }
}

}

RankResult rank_formations(const Engine &engine, const RankOptions &options) {
    const Problem &problem = engine.problem();
    const bool theoretical = options.objective == "theoretical_score";
    if (!theoretical && options.objective != "index")
        throw SpecError("objective 必须为 theoretical_score/index");
    if (theoretical)
        engine.validate_theoretical_scope();
    const int team_size = engine.team_size();
    const double started = wall_s();
    const double budget =
        options.time_limit_s > 0 ? options.time_limit_s : std::numeric_limits<double>::infinity();
    const double deadline = started + budget;

    Shared shared;
    shared.engine = &engine;
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
    if (shared.member_count > 64 || shared.snap_count > 64 || team_size > kMaxTeam)
        throw SpecError("搜索支持最多 64 成员、64 Snapshot、8 人队伍");
    for (auto m : shared.members)
        if (m->character < 0 || m->character >= 64)
            throw SpecError("搜索角色编号必须为 0..63");

    for (const auto &ids : options.excluded_member_sets) {
        if (static_cast<int>(ids.size()) != team_size)
            throw SpecError("排除组合必须恰有 team_size 张不同成员卡");
        uint64_t mask = 0;
        for (auto id : ids) {
            const int member = engine.member_pos(id);
            if (member < 0)
                throw SpecError("排除组合含不可用成员 " + std::to_string(id));
            const uint64_t bit = uint64_t{1} << member;
            if (mask & bit)
                throw SpecError("排除组合不能含重复成员");
            mask |= bit;
        }
        shared.excluded_member_masks.push_back(mask);
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

    bool exact_mode = options.method == "exact";
    if (options.method == "auto") {
        exact_mode = static_cast<int64_t>(shared.member_count) * shared.snap_count <= 1024;
    }

    std::unordered_map<uint64_t, Solution> pool;
    std::unordered_map<int64_t, Solution> best_by_leader;
    std::mt19937_64 rng(static_cast<uint64_t>(problem.search.seed));
    AnnealStats anneal_stats;
    int64_t dfs_nodes = 0, leaders_proven = 0;
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
        if (use_index_pruning)
            prune(shared, matrix);
        else
            shared.keep_at.assign(shared.pairs.size() * static_cast<size_t>(team_size), 1);
        game.by_power.resize(shared.pairs.size());
        for (size_t i = 0; i < shared.pairs.size(); ++i)
            game.by_power[i] = static_cast<int>(i);
        std::stable_sort(game.by_power.begin(), game.by_power.end(),
                         [&](int a, int b) { return game.slot_power(a) > game.slot_power(b); });
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
        auto it = pool.find(candidate.member_mask);
        if (it == pool.end() || it->second.index < candidate.index)
            pool[candidate.member_mask] = candidate;
        auto leader = best_by_leader.find(formation.leader);
        if (leader == best_by_leader.end() || leader->second.index < candidate.index)
            best_by_leader[formation.leader] = candidate;
    }
    const bool seeded_exact = exact_mode && !pool.empty();

    shared.score_objective = false;
    if (!seeded_exact) {

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

            std::unordered_set<uint64_t> raw_member_sets;
            for (const auto &raw : candidates) {
                if (static_cast<int>(raw_member_sets.size()) >=
                    std::max(8, std::min(24, options.top * 2)))
                    break;
                if (!raw_member_sets.insert(raw.member_mask).second)
                    continue;
                Solution candidate = raw;
                candidate.evaluation = evaluate_picks(game, candidate.picks);
                candidate.index = candidate.evaluation.index;
                auto it = pool.find(candidate.member_mask);
                if (it == pool.end() || it->second.index < candidate.index)
                    pool[candidate.member_mask] = std::move(candidate);
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
                auto existing = pool.find(candidates[i].member_mask);
                if (existing == pool.end() || existing->second.index < candidates[i].index) {
                    pool[candidates[i].member_mask] = candidates[i];
                }
            }

            Solution seed;
            if (!candidates.empty())
                seed = candidates.front();
            if (!seed.found)
                seed = make_solution(game, random_team(game, rng), false);
            Solution strengthened = anneal(game, seed, options, rng, anneal_stats);
            if (strengthened.found) {
                auto existing = pool.find(strengthened.member_mask);
                if (existing == pool.end() || existing->second.index < strengthened.index)
                    pool[strengthened.member_mask] = strengthened;
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
    if (theoretical && !seeded_exact) {

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
            pool[candidate.member_mask] = std::move(candidate);
        }
    }

    for (const auto &seed : verified_seed_solutions) {
        auto existing = pool.find(seed.member_mask);
        if (existing == pool.end() || existing->second.index < seed.index)
            pool[seed.member_mask] = seed;
        auto leader = best_by_leader.find(seed.evaluation.leader);
        if (leader == best_by_leader.end() || leader->second.index < seed.index)
            best_by_leader[seed.evaluation.leader] = seed;
    }
    shared.score_objective = theoretical;

    shared.deadline = deadline;
    if (exact_mode) {
        for (int64_t leader_id : leader_ids) {
            if (wall_s() > deadline) {
                certified = false;
                break;
            }
            auto incumbent = best_by_leader.find(leader_id);
            Game game = build_game(leader_id, !theoretical);
            Bnb ctx;
            ctx.game = &game;
            ctx.excluded = &shared.excluded_member_masks;
            ctx.deadline = deadline;
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
            for (int snap = 0; snap < shared.snap_count; ++snap) {
                const int pair = game.leader_member * shared.snap_count + snap;
                if (!shared.any_kept(pair))
                    continue;
                trial.push_back(pair);
                bnb_dfs(ctx, 1, -1, uint64_t{1} << static_cast<uint32_t>(game.leader_member),
                        uint64_t{1} << static_cast<uint32_t>(snap),
                        uint64_t{1} << static_cast<uint32_t>(
                            shared.members[static_cast<size_t>(game.leader_member)]->character),
                        game.slot_power(pair), trial);
                trial.pop_back();
                if (ctx.timed_out)
                    break;
                if (options.progress && (snap % 8 == 7 || snap + 1 == shared.snap_count))
                    options.progress("leader " + std::to_string(leader_id) + " Snapshot roots " +
                                     std::to_string(snap + 1) + "/" +
                                     std::to_string(shared.snap_count) + " nodes " +
                                     std::to_string(ctx.nodes));
            }
            dfs_nodes += ctx.nodes;
            if (ctx.best_pairs.size() == static_cast<size_t>(team_size)) {
                Solution solution = make_solution(game, ctx.best_pairs, true);
                auto existing = pool.find(solution.member_mask);
                if (existing == pool.end() || existing->second.index < solution.index) {
                    pool[solution.member_mask] = std::move(solution);
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

    std::vector<Solution> ordered;
    ordered.reserve(pool.size());
    for (auto &entry : pool)
        ordered.push_back(entry.second);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Solution &a, const Solution &b) { return a.index > b.index; });
    if (static_cast<int>(ordered.size()) > options.top)
        ordered.resize(static_cast<size_t>(options.top));

    RankResult result;
    for (Solution &solution : ordered) {
        engine.populate_estimated_score(solution.evaluation);
        result.results.push_back(std::move(solution.evaluation));
    }
    if (!result.results.empty() && !certified) {
        result.results.front().warnings.push_back("搜索结果未获最优性证明，只能作为候选。");
    }

    Json audit = Json::object();
    audit.set("solver", Json(exact_mode ? "beam-annealing-dp-dfs" : "beam-annealing-dp"));
    audit.set("dfs_nodes", Json(dfs_nodes));
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
    audit.set("theoretical_max_certified",
              Json(theoretical && certified && !result.results.empty()));
    audit.set("infeasible_proven", Json(certified && result.results.empty()));
    audit.set("score_dfs_pruning",
              Json(theoretical ? "native_float32_monotone_and_linear_envelope" : "index_bound"));
    audit.set("linear_score_bound_enabled", Json(shared.linear_score_bound_enabled));
    audit.set("global_incumbent_pruning", Json(exact_mode));
    audit.set("supplied_seed_formations", Json(supplied_seeds));
    audit.set("accepted_seed_formations", Json(accepted_seeds));
    audit.set("excluded_seed_formations", Json(excluded_seeds));
    audit.set("warm_start_strategy",
              Json(seeded_exact ? "validated_prior_formations" : "beam_annealing"));
    audit.set("complete_beam_variants_per_member_set", Json(4));
    audit.set("index_snapshot_pruning_used_in_score_dfs", Json(false));
    audit.set("certified_scope",
              Json(theoretical ? "maximum rounded AP model score over all legal formations and all "
                                 "skill permutations within requested leaders/card pool after "
                                 "excluded member sets; other top entries are candidates"
                               : "best index within requested leaders and card constraints; "
                                 "remaining top entries are candidates"));
    audit.set("warm_start_objective", Json("index"));
    audit.set("score_model_calibrated", Json(false));
    audit.set("score_formations_evaluated", Json(shared.score_evaluations));
    int64_t permutations = 1;
    for (int i = 2; i <= team_size; ++i)
        permutations *= i;
    audit.set("score_orders_evaluated", Json(shared.score_evaluations * permutations));
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
