// 组卡搜索：占优剪枝 + 全候选束搜索 + 局部改进，exact 模式追加分支定界。
#include "solver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
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
    const Engine* engine = nullptr;
    std::vector<const Member*> members;
    std::vector<const Snapshot*> snapshots;
    std::vector<Pair> pairs;                // 下标 = member * snap_count + snap
    std::vector<std::vector<double>> gains;  // pairs × team_size
    std::vector<char> keep_at;               // pairs × team_size，占优剪枝结果
    bool distinct_characters = true;
    int team_size = 5;
    int member_count = 0;
    int snap_count = 0;

    bool kept(int pair, int trigger) const {
        return keep_at[static_cast<size_t>(pair) * static_cast<size_t>(team_size) +
                       static_cast<size_t>(trigger)] != 0;
    }
    bool any_kept(int pair) const {
        for (int k = 0; k < team_size; ++k) {
            if (kept(pair, k)) return true;
        }
        return false;
    }
    double max_gain(int pair) const {
        double best = 0.0;
        for (int k = 0; k < team_size; ++k) best = std::max(best, gains[static_cast<size_t>(pair)][static_cast<size_t>(k)]);
        return best;
    }
};

// 同成员同触发位：若 >= team_size 张不同 Snapshot 的 power 与 gain 都不劣且至少一张严格更优，
// 则该 (成员, Snapshot) 不可能出现在最优解里。power 依赖队长，因此每个队长各算一次。
void prune(Shared& shared, const std::vector<std::vector<int64_t>>& power) {
    const int team_size = shared.team_size;
    const int snap_count = shared.snap_count;
    shared.keep_at.assign(shared.pairs.size() * static_cast<size_t>(team_size), 1);
    std::vector<double> p(static_cast<size_t>(snap_count));
    std::vector<double> g(static_cast<size_t>(snap_count));
    for (int member = 0; member < shared.member_count; ++member) {
        for (int k = 0; k < team_size; ++k) {
            for (int snap = 0; snap < snap_count; ++snap) {
                const size_t index = static_cast<size_t>(member) * static_cast<size_t>(snap_count) +
                                     static_cast<size_t>(snap);
                p[static_cast<size_t>(snap)] = static_cast<double>(power[static_cast<size_t>(member)]
                                                                        [static_cast<size_t>(snap)]);
                g[static_cast<size_t>(snap)] = shared.gains[index][static_cast<size_t>(k)];
            }
            for (int snap = 0; snap < snap_count; ++snap) {
                int dominators = 0;
                for (int other = 0; other < snap_count; ++other) {
                    if (other == snap) continue;
                    const bool no_worse = p[static_cast<size_t>(other)] >= p[static_cast<size_t>(snap)] &&
                                          g[static_cast<size_t>(other)] >= g[static_cast<size_t>(snap)];
                    if (!no_worse) continue;
                    const bool strict = p[static_cast<size_t>(other)] > p[static_cast<size_t>(snap)] ||
                                        g[static_cast<size_t>(other)] > g[static_cast<size_t>(snap)] ||
                                        other < snap;
                    if (strict) ++dominators;
                    if (dominators >= team_size) break;
                }
                if (dominators >= team_size) {
                    const size_t flat = (static_cast<size_t>(member) * static_cast<size_t>(snap_count) +
                                         static_cast<size_t>(snap)) *
                                            static_cast<size_t>(team_size) +
                                        static_cast<size_t>(k);
                    shared.keep_at[flat] = 0;
                }
            }
        }
    }
}

double assign_triggers(const Shared& shared, const std::vector<int>& pairs, std::vector<int>& triggers) {
    const int team_size = shared.team_size;
    std::array<int, kMaxTeam> perm{};
    for (int i = 0; i < team_size; ++i) perm[i] = i;
    triggers.assign(static_cast<size_t>(team_size), 0);
    double best = -1.0;
    do {
        double sum = 0.0;
        for (int i = 0; i < team_size; ++i) {
            sum += shared.gains[static_cast<size_t>(pairs[static_cast<size_t>(i)])][static_cast<size_t>(perm[i])];
        }
        if (sum > best) {
            best = sum;
            for (int i = 0; i < team_size; ++i) triggers[static_cast<size_t>(i)] = perm[i];
        }
    } while (std::next_permutation(perm.begin(), perm.begin() + team_size));
    return best;
}

struct Pick {
    int pair = 0;
    int trigger = 0;
    int64_t power = 0;
    double gain = 0.0;
};

struct Game {
    const Shared* shared = nullptr;
    std::vector<std::vector<int64_t>> power;  // 该队长下的 member × snap
    int64_t leader_id = 0;
    int leader_member = -1;
    std::vector<int> by_power;      // 配对下标，power 降序
    std::vector<double> top_gains;  // 所有 (配对, 触发位) gain 降序，仅作上界
    std::vector<double> top_powers;

    int64_t slot_power(int pair) const {
        const Pair& item = shared->pairs[static_cast<size_t>(pair)];
        return power[static_cast<size_t>(item.member)][static_cast<size_t>(item.snap)];
    }
};

struct Solution {
    double index = 0.0;
    uint64_t member_mask = 0;
    std::vector<Pick> picks;
    Evaluation evaluation;
    bool found = false;
};

Evaluation evaluate_picks(const Game& game, const std::vector<Pick>& picks) {
    const Shared& shared = *game.shared;
    Formation formation;
    formation.leader = game.leader_id;
    for (const Pick& pick : picks) {
        const Pair& pair = shared.pairs[static_cast<size_t>(pick.pair)];
        Slot slot;
        slot.member = shared.members[static_cast<size_t>(pair.member)]->id;
        slot.snapshot = shared.snapshots[static_cast<size_t>(pair.snap)]->id;
        slot.trigger = pick.trigger + 1;
        formation.slots.push_back(slot);
    }
    return shared.engine->evaluate(formation, EvalOptions{});
}

uint64_t member_mask_of(const Shared& shared, const std::vector<Pick>& picks) {
    uint64_t mask = 0;
    for (const Pick& pick : picks) {
        mask |= uint64_t{1} << static_cast<uint32_t>(
                    shared.pairs[static_cast<size_t>(pick.pair)].member);
    }
    return mask;
}

Solution make_solution(const Game& game, const std::vector<int>& pairs, bool with_evaluation) {
    const Shared& shared = *game.shared;
    Solution solution;
    if (static_cast<int>(pairs.size()) != shared.team_size) return solution;
    std::vector<int> triggers;
    const double gain = assign_triggers(shared, pairs, triggers);
    int64_t power_sum = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
        const int pair = pairs[i];
        const int64_t slot = game.slot_power(pair);
        power_sum += slot;
        solution.picks.push_back(Pick{pair, triggers[i], slot,
                                      shared.gains[static_cast<size_t>(pair)][static_cast<size_t>(triggers[i])]});
    }
    solution.member_mask = member_mask_of(shared, solution.picks);
    solution.index = static_cast<double>(power_sum) * (1.0 + gain);
    solution.found = true;
    if (with_evaluation) {
        solution.evaluation = evaluate_picks(game, solution.picks);
        solution.index = solution.evaluation.index;
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

// 全候选束搜索：队长占一个槽位，其余每层枚举所有仍可行的 (成员, Snapshot)，边际收益取最优空触发位。
std::vector<BeamState> beam(const Game& game, int beam_width) {
    const Shared& shared = *game.shared;
    const int team_size = shared.team_size;
    const int leader = game.leader_member;
    if (leader < 0) return {};
    std::vector<BeamState> frontier;
    frontier.reserve(static_cast<size_t>(shared.snap_count));
    for (int snap = 0; snap < shared.snap_count; ++snap) {
        const int pair = leader * shared.snap_count + snap;
        if (!shared.any_kept(pair)) continue;
        int best_trigger = -1;
        double best_gain = -1.0;
        for (int k = 0; k < team_size; ++k) {
            if (!shared.kept(pair, k)) continue;
            const double value = shared.gains[static_cast<size_t>(pair)][static_cast<size_t>(k)];
            if (value > best_gain) {
                best_gain = value;
                best_trigger = k;
            }
        }
        if (best_trigger < 0) continue;
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
    if (frontier.empty() || team_size <= 1) return frontier;
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
            const BeamState& state = frontier[index];
            for (int pair : game.by_power) {
                const Pair& item = shared.pairs[static_cast<size_t>(pair)];
                const uint64_t member_bit = uint64_t{1} << static_cast<uint32_t>(item.member);
                if (state.member_mask & member_bit) continue;
                const uint64_t snap_bit = uint64_t{1} << static_cast<uint32_t>(item.snap);
                if (state.snap_mask & snap_bit) continue;
                const int character = shared.members[static_cast<size_t>(item.member)]->character;
                if (shared.distinct_characters &&
                    (state.character_mask & (uint64_t{1} << static_cast<uint32_t>(character)))) {
                    continue;
                }
                int best_trigger = -1;
                double best_gain = -1.0;
                for (int k = 0; k < team_size; ++k) {
                    if (state.trigger_mask & (1 << k)) continue;
                    if (!shared.kept(pair, k)) continue;
                    const double value = shared.gains[static_cast<size_t>(pair)][static_cast<size_t>(k)];
                    if (value > best_gain) {
                        best_gain = value;
                        best_trigger = k;
                    }
                }
                if (best_trigger < 0) continue;
                const int64_t slot = game.slot_power(pair);
                const double score =
                    static_cast<double>(state.power + slot) * (1.0 + state.gain + best_gain);
                children.push_back(Child{score, index, pair, best_trigger, slot, best_gain});
            }
        }
        if (children.empty()) return {};
        const size_t keep = std::min<size_t>(static_cast<size_t>(beam_width), children.size());
        std::nth_element(children.begin(), children.begin() + static_cast<long>(keep), children.end(),
                         [](const Child& a, const Child& b) { return a.score > b.score; });
        children.resize(keep);
        std::vector<BeamState> next;
        next.reserve(keep);
        for (const Child& child : children) {
            BeamState state = frontier[child.parent];
            const Pair& item = shared.pairs[static_cast<size_t>(child.pair)];
            state.picks[static_cast<size_t>(state.depth)] =
                Pick{child.pair, child.trigger, child.power, child.gain};
            state.pairs[static_cast<size_t>(state.depth)] = child.pair;
            state.depth += 1;
            state.member_mask |= uint64_t{1} << static_cast<uint32_t>(item.member);
            state.snap_mask |= uint64_t{1} << static_cast<uint32_t>(item.snap);
            state.character_mask |= uint64_t{1} << static_cast<uint32_t>(
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

void local_improve(const Game& game, int rounds, Solution& solution) {
    const Shared& shared = *game.shared;
    const int team_size = shared.team_size;
    if (team_size <= 1) return;
    for (int round = 0; round < rounds; ++round) {
        bool improved = false;
        for (int position = 0; position < team_size && !improved; ++position) {
            if (shared.pairs[static_cast<size_t>(solution.picks[static_cast<size_t>(position)].pair)].member ==
                game.leader_member) {
                continue;  // 队长固定在这一格
            }
            uint64_t member_mask = 0;
            uint64_t snap_mask = 0;
            uint64_t character_mask = 0;
            for (int i = 0; i < team_size; ++i) {
                if (i == position) continue;
                const Pair& item = shared.pairs[static_cast<size_t>(solution.picks[static_cast<size_t>(i)].pair)];
                member_mask |= uint64_t{1} << static_cast<uint32_t>(item.member);
                snap_mask |= uint64_t{1} << static_cast<uint32_t>(item.snap);
                character_mask |= uint64_t{1} << static_cast<uint32_t>(
                                                             shared.members[static_cast<size_t>(item.member)]->character);
            }
            for (int pair : game.by_power) {
                if (pair == solution.picks[static_cast<size_t>(position)].pair) continue;
                if (!shared.any_kept(pair)) continue;
                const Pair& item = shared.pairs[static_cast<size_t>(pair)];
                if (member_mask & (uint64_t{1} << static_cast<uint32_t>(item.member))) continue;
                if (snap_mask & (uint64_t{1} << static_cast<uint32_t>(item.snap))) continue;
                const int character = shared.members[static_cast<size_t>(item.member)]->character;
                if (shared.distinct_characters &&
                    (character_mask & (uint64_t{1} << static_cast<uint32_t>(character)))) {
                    continue;
                }
                std::vector<int> pairs;
                pairs.reserve(static_cast<size_t>(team_size));
                for (int i = 0; i < team_size; ++i) {
                    pairs.push_back(i == position ? pair : solution.picks[static_cast<size_t>(i)].pair);
                }
                std::vector<int> triggers;
                const double gain = assign_triggers(shared, pairs, triggers);
                double power = 0.0;
                for (int item_pair : pairs) power += static_cast<double>(game.slot_power(item_pair));
                if (power * (1.0 + gain) <= solution.index * (1.0 + 1e-12)) continue;
                std::vector<Pick> picks;
                for (size_t i = 0; i < pairs.size(); ++i) {
                    picks.push_back(Pick{pairs[i], triggers[i], game.slot_power(pairs[i]),
                                         shared.gains[static_cast<size_t>(pairs[i])][static_cast<size_t>(triggers[i])]});
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
        if (!improved) return;
    }
}

struct Bnb {
    const Game* game = nullptr;
    const std::vector<uint64_t>* excluded = nullptr;
    double deadline = 0.0;
    bool timed_out = false;
    double best = 0.0;
    std::vector<int> best_pairs;
    int64_t nodes = 0;
};

void bnb_dfs(Bnb& ctx, int level, int min_pair, uint64_t member_mask, uint64_t snap_mask,
             uint64_t character_mask, int64_t power, std::vector<int>& trial) {
    const Shared& shared = *ctx.game->shared;
    const int team_size = shared.team_size;
    if (ctx.timed_out) return;
    if (((ctx.nodes++) & 255) == 0 && wall_s() > ctx.deadline) {
        ctx.timed_out = true;
        return;
    }
    if (level == team_size) {
        std::vector<int> triggers;
        const double gain = assign_triggers(shared, trial, triggers);
        const double value = static_cast<double>(power) * (1.0 + gain);
        if (value > ctx.best * (1.0 + 1e-12)) {
            ctx.best = value;
            ctx.best_pairs = trial;
        }
        return;
    }

    const int remaining = team_size - level;
    double power_bound = static_cast<double>(power);
    for (int i = 0; i < remaining && i < static_cast<int>(ctx.game->top_powers.size()); ++i) {
        power_bound += ctx.game->top_powers[static_cast<size_t>(i)];
    }
    double gain_bound = 0.0;
    for (int i = 0; i < level; ++i) {
        gain_bound += shared.max_gain(trial[static_cast<size_t>(i)]);
    }
    for (int i = 0; i < remaining && i < static_cast<int>(ctx.game->top_gains.size()); ++i) {
        gain_bound += ctx.game->top_gains[static_cast<size_t>(i)];
    }
    if (power_bound * (1.0 + gain_bound) <= ctx.best * (1.0 - 1e-12)) return;

    for (int pair : ctx.game->by_power) {
        if (pair <= min_pair) continue;  // 配对下标严格递增：每个成员组合只出现一次
        if (!shared.any_kept(pair)) continue;
        const Pair& item = shared.pairs[static_cast<size_t>(pair)];
        const uint64_t member_bit = uint64_t{1} << static_cast<uint32_t>(item.member);
        if (member_mask & member_bit) continue;
        const uint64_t snap_bit = uint64_t{1} << static_cast<uint32_t>(item.snap);
        if (snap_mask & snap_bit) continue;
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
            if (banned) continue;
        }
        trial.push_back(pair);
        bnb_dfs(ctx, level + 1, pair, member_mask | member_bit, snap_mask | snap_bit,
                character_mask | (uint64_t{1} << static_cast<uint32_t>(character)),
                power + ctx.game->slot_power(pair), trial);
        trial.pop_back();
        if (ctx.timed_out) return;
    }
}

}  // namespace

RankResult rank_formations(const Engine& engine, const RankOptions& options) {
    const Problem& problem = engine.problem();
    const int team_size = engine.team_size();
    const double started = wall_s();
    const double budget = options.time_limit_s > 0 ? options.time_limit_s : 1e9;
    const double deadline = started + budget;

    Shared shared;
    shared.engine = &engine;
    shared.members = engine.members();
    shared.snapshots = engine.snapshots();
    shared.team_size = team_size;
    shared.member_count = static_cast<int>(shared.members.size());
    shared.snap_count = static_cast<int>(shared.snapshots.size());
    shared.distinct_characters = problem.constraints.distinct_characters;

    const std::vector<std::vector<std::vector<double>>>& gains = engine.gains();
    shared.pairs.resize(static_cast<size_t>(shared.member_count) * static_cast<size_t>(shared.snap_count));
    shared.gains.assign(shared.pairs.size(), std::vector<double>(static_cast<size_t>(team_size), 0.0));
    for (int member = 0; member < shared.member_count; ++member) {
        for (int snap = 0; snap < shared.snap_count; ++snap) {
            const size_t index = static_cast<size_t>(member) * static_cast<size_t>(shared.snap_count) +
                                 static_cast<size_t>(snap);
            shared.pairs[index] = Pair{member, snap};
            for (int k = 0; k < team_size; ++k) {
                shared.gains[index][static_cast<size_t>(k)] =
                    gains[static_cast<size_t>(member)][static_cast<size_t>(snap)][static_cast<size_t>(k)];
            }
        }
    }
    std::vector<int64_t> leader_ids;
    if (!options.leaders.empty()) {
        leader_ids = options.leaders;
    } else {
        for (const Member* member : problem.leader_candidates()) leader_ids.push_back(member->id);
    }

    bool exact_mode = options.method == "exact";
    if (options.method == "auto") {
        exact_mode = static_cast<int64_t>(shared.member_count) * shared.snap_count <= 1024;
    }

    std::unordered_map<uint64_t, Solution> pool;
    std::unordered_map<int64_t, Solution> best_by_leader;
    int64_t leaders_tried = 0;
    bool certified = exact_mode;

    auto build_game = [&](int64_t leader_id) {
        const int leader_member = engine.member_pos(leader_id);
        if (leader_member < 0) {
            throw SpecError("队长 " + std::to_string(leader_id) + " 不在可用成员中");
        }
        Game game;
        game.shared = &shared;
        game.power = engine.power_matrix(static_cast<size_t>(leader_member));
        game.leader_id = leader_id;
        game.leader_member = leader_member;
        prune(shared, game.power);
        game.by_power.resize(shared.pairs.size());
        for (size_t i = 0; i < shared.pairs.size(); ++i) game.by_power[i] = static_cast<int>(i);
        std::stable_sort(game.by_power.begin(), game.by_power.end(),
                         [&](int a, int b) { return game.slot_power(a) > game.slot_power(b); });
        for (int pair : game.by_power) {
            if (!shared.any_kept(pair)) continue;
            game.top_powers.push_back(static_cast<double>(game.slot_power(pair)));
        }
        for (int k = 0; k < team_size; ++k) {
            for (int pair : game.by_power) {
                if (!shared.kept(pair, k)) continue;
                game.top_gains.push_back(shared.gains[static_cast<size_t>(pair)][static_cast<size_t>(k)]);
            }
        }
        std::stable_sort(game.top_gains.begin(), game.top_gains.end(), std::greater<double>());
        return game;
    };

    // 阶段一：所有队长都先跑一遍束搜索，保证 exact 至少不劣于 fast。
    for (int64_t leader_id : leader_ids) {
        Game game = build_game(leader_id);

        const std::vector<BeamState> frontier = beam(game, options.beam_width);
        std::vector<Solution> candidates;
        candidates.reserve(frontier.size());
        for (const BeamState& state : frontier) {
            if (state.depth != team_size) continue;
            std::vector<int> pairs;
            pairs.reserve(static_cast<size_t>(team_size));
            for (int i = 0; i < team_size; ++i) pairs.push_back(state.pairs[static_cast<size_t>(i)]);
            Solution solution = make_solution(game, pairs, false);
            if (solution.found) candidates.push_back(std::move(solution));
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Solution& a, const Solution& b) { return a.index > b.index; });
        const size_t improve_count = std::min<size_t>(16, candidates.size());
        for (size_t i = 0; i < improve_count; ++i) local_improve(game, 8, candidates[i]);
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Solution& a, const Solution& b) { return a.index > b.index; });

        const size_t evaluate_count = std::min<size_t>(24, candidates.size());
        for (size_t i = 0; i < evaluate_count; ++i) {
            candidates[i].evaluation = evaluate_picks(game, candidates[i].picks);
            candidates[i].index = candidates[i].evaluation.index;
            auto existing = pool.find(candidates[i].member_mask);
            if (existing == pool.end() || existing->second.index < candidates[i].index) {
                pool[candidates[i].member_mask] = candidates[i];
            }
        }

        if (!candidates.empty()) {
            best_by_leader[leader_id] = candidates.front();
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

    // 阶段二：exact 模式用剩余预算逐队长做分支定界，跑完即证明该队长的最优解。
    if (exact_mode) {
        for (int64_t leader_id : leader_ids) {
            if (wall_s() > deadline) {
                certified = false;
                break;
            }
            auto incumbent = best_by_leader.find(leader_id);
            if (incumbent == best_by_leader.end()) continue;
            Game game = build_game(leader_id);
            Bnb ctx;
            ctx.game = &game;
            ctx.deadline = deadline;
            ctx.best = incumbent->second.index;
            std::vector<int> trial;
            for (int snap = 0; snap < shared.snap_count; ++snap) {
                const int pair = game.leader_member * shared.snap_count + snap;
                if (!shared.any_kept(pair)) continue;
                trial.push_back(pair);
                bnb_dfs(ctx, 1, pair,
                        uint64_t{1} << static_cast<uint32_t>(game.leader_member),
                        uint64_t{1} << static_cast<uint32_t>(snap),
                        uint64_t{1} << static_cast<uint32_t>(
                            shared.members[static_cast<size_t>(game.leader_member)]->character),
                        game.slot_power(pair), trial);
                trial.pop_back();
                if (ctx.timed_out) break;
            }
            if (ctx.timed_out) {
                certified = false;
                break;
            }
            if (ctx.best_pairs.size() == static_cast<size_t>(team_size)) {
                Solution solution = make_solution(game, ctx.best_pairs, true);
                auto existing = pool.find(solution.member_mask);
                if (existing == pool.end() || existing->second.index < solution.index) {
                    pool[solution.member_mask] = std::move(solution);
                }
            }
        }
    }

    std::vector<Solution> ordered;
    ordered.reserve(pool.size());
    for (auto& entry : pool) ordered.push_back(entry.second);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Solution& a, const Solution& b) { return a.index > b.index; });
    if (static_cast<int>(ordered.size()) > options.top) ordered.resize(static_cast<size_t>(options.top));

    RankResult result;
    for (Solution& solution : ordered) {
        result.results.push_back(std::move(solution.evaluation));
    }
    if (!result.results.empty() && !certified) {
        result.results.front().warnings.push_back("搜索结果未获最优性证明，只能作为候选。");
    }

    Json audit = Json::object();
    audit.set("solver", Json(exact_mode ? "beam-bnb" : "beam"));
    audit.set("certified", Json(certified));
    audit.set("stop_reason", Json(certified ? "searched" : (exact_mode ? "time_limit" : "heuristic")));
    audit.set("elapsed_s", Json(wall_s() - started));
    audit.set("candidates", Json(static_cast<int64_t>(pool.size())));
    audit.set("leaders_tried", Json(leaders_tried));
    audit.set("beam_width", Json(static_cast<int64_t>(options.beam_width)));
    audit.set("seed", Json(problem.search.seed));
    result.audit = audit;
    return result;
}

}  // namespace deckcalc
