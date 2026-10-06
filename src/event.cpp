#include "event.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <tuple>

namespace deckcalc {
namespace {
struct Yield {
    double cp = 0, pt = 0, shop = 0;
};
struct PhaseSearchStats {
    int64_t complete_formations = 0;
    int64_t index_evaluations = 0;
    int64_t absolute_score_evaluations = 0;
    int64_t dominated_before_evaluation = 0;
    int64_t duplicate_formations = 0;
    int64_t leaders_seeded = 0;
    int64_t score_seeds = 0;
    int64_t bound_nodes = 0, reward_pruned_branches = 0, infeasible_branches = 0;
    int64_t seed_formations = 0, cached_snapshot_nodes = 0;
    int64_t member_groups = 0, bounded_member_groups = 0, pruned_member_prefixes = 0;
    bool score_bound_enabled = false, reward_ceiling_certified = false, phase_cache_hit = false;
    int64_t coupled_bound_nodes = 0, coupled_pruned_branches = 0;
    double score_ceiling = std::numeric_limits<double>::infinity();
    Json to_json() const {
        Json out = Json::object();
        out.set("complete_formations", Json(complete_formations));
        out.set("index_evaluations", Json(index_evaluations));
        out.set("absolute_score_evaluations", Json(absolute_score_evaluations));
        out.set("dominated_before_evaluation", Json(dominated_before_evaluation));
        out.set("duplicate_formations", Json(duplicate_formations));
        out.set("leaders_seeded", Json(leaders_seeded));
        out.set("score_seeds", Json(score_seeds));
        out.set("bound_nodes", Json(bound_nodes));
        out.set("reward_pruned_branches", Json(reward_pruned_branches));
        out.set("infeasible_branches", Json(infeasible_branches));
        out.set("seed_formations", Json(seed_formations));
        out.set("cached_snapshot_nodes", Json(cached_snapshot_nodes));
        out.set("member_groups", Json(member_groups));
        out.set("bounded_member_groups", Json(bounded_member_groups));
        out.set("pruned_member_prefixes", Json(pruned_member_prefixes));
        out.set("score_bound_enabled", Json(score_bound_enabled));
        out.set("reward_ceiling_certified", Json(reward_ceiling_certified));
        out.set("phase_cache_hit", Json(phase_cache_hit));
        out.set("coupled_bound_nodes", Json(coupled_bound_nodes));
        out.set("coupled_pruned_branches", Json(coupled_pruned_branches));
        out.set("score_ceiling_certified", Json(std::isfinite(score_ceiling)));
        out.set("score_ceiling", std::isfinite(score_ceiling) ? Json(score_ceiling) : Json());
        return out;
    }
};
struct Candidate {
    Formation formation;
    Evaluation score;
    Yield yield;
    int score_rank = 0;
    Json rank_distribution;
};
double positive(const Json &j, const char *key, double fallback) {
    double n = num_field(j, key, fallback);
    if (!std::isfinite(n) || n < 0)
        throw SpecError(std::string("event 非负数值无效: ") + key);
    return n;
}
double value(const Json &rows, int rank, const char *field) {
    double result = 0;
    for (const auto &row : rows.items())
        if (int_field(row, "rank") == rank)
            result += positive(row, field, 0);
    return result;
}
const Json &boost(const Json &rows, int cost) {
    for (const auto &row : rows.items())
        if (int_field(row, "cost") == cost)
            return row;
    throw SpecError("event 消耗档位不存在: " + std::to_string(cost));
}
struct PhaseRewards {
    struct Item {
        double count, probability;
    };
    double base_pt = 0, base_cp = 0, pt_rate = 1, cp_rate = 1, reward_rate = 1;
    int cp_bonus_source = 0;
    std::vector<Item> items;
    PhaseRewards(const Engine &engine, const std::string &phase, int rank) {
        const Json &e = engine.problem().raw.at("event");
        const Json &a = e.at("assumptions");
        const Json &p = e.at(phase);
        const Json &b =
            boost(e.at(phase == "normal" ? "normal_boosts" : "challenge_boosts"),
                  int_field(a, phase == "normal" ? "normal_boost_cost" : "challenge_cp_cost"));
        pt_rate = positive(b, "pt_rate", 1);
        reward_rate = positive(b, "reward_rate", 1);
        base_pt = value(p.at("pt"), rank, "value");
        int resource_id = int_field(a, "shop_resource_id", 0),
            resource_type = int_field(a, "shop_resource_type", 1);
        for (const auto &row : p.at("rewards").items()) {
            if (int_field(row, "rank") != rank)
                continue;
            if (resource_id && (int_field(row, "resource_id") != resource_id ||
                                int_field(row, "resource_type") != resource_type))
                continue;
            double probability = positive(row, "probability_bp", 10000) / 10000;
            if (probability > 1)
                throw SpecError("event 概率超过 10000bp");
            items.push_back(Item{positive(row, "count", 0), probability});
        }
        if (phase == "normal") {
            std::string source = str_field(a, "cp_bonus_source", "none");
            if (source != "none" && source != "pt" && source != "drop")
                throw SpecError("cp_bonus_source 必须为 none/pt/drop");
            cp_bonus_source = source == "pt" ? 1 : source == "drop" ? 2 : 0;
            cp_rate = positive(a, "cp_boost_rate", reward_rate);
            base_cp = value(p.at("cp"), rank, "value");
        }
    }
    Yield calculate(double pt, double drop) const {
        Yield y;
        y.pt = std::floor(base_pt * pt_rate * (1 + pt / 10000));
        double cp_bonus = cp_bonus_source == 1 ? pt : cp_bonus_source == 2 ? drop : 0;
        y.cp = std::floor(base_cp * cp_rate * (1 + cp_bonus / 10000));
        for (const Item &item : items)
            y.shop += std::floor(item.count * reward_rate * (1 + drop / 10000)) * item.probability;
        return y;
    }
};
struct CardBonus {
    double pt = 0, drop = 0;
};
std::map<int64_t, CardBonus> card_bonuses(const Engine &engine, const char *key) {
    std::map<int64_t, CardBonus> out;
    for (const auto &card : engine.problem().raw.at("catalog").at(key).items())
        out.emplace(int_field(card, "id"), CardBonus{positive(card, "event_pt_bonus_bp", 0),
                                                     positive(card, "event_drop_bonus_bp", 0)});
    return out;
}
Json yield_json(const Yield &y) {
    Json j = Json::object();
    j.set("cp", Json(y.cp));
    j.set("pt", Json(y.pt));
    j.set("shop_currency_expected", Json(y.shop));
    return j;
}
Json candidate_json(const Candidate &c) {
    Json j = c.score.to_json(false, false, 0);
    j.set("formation", dump_formation(c.formation));
    j.set("yield", yield_json(c.yield));
    j.set("score_rank", Json(c.score_rank));
    if (!c.rank_distribution.is_null())
        j.set("score_rank_distribution", c.rank_distribution);
    return j;
}
bool dominates(const Yield &a, const Yield &b) {
    return a.cp >= b.cp && a.pt >= b.pt && a.shop >= b.shop &&
           (a.cp > b.cp || a.pt > b.pt || a.shop > b.shop);
}
std::vector<Candidate> search(const Engine &engine, const RankOptions &options,
                              const std::string &phase, double seconds, bool &complete,
                              PhaseSearchStats &stats) {
    complete = options.method == "exact";
    auto start = std::chrono::steady_clock::now();
    auto expired = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >=
               seconds;
    };
    const Json &a = engine.problem().raw.at("event").at("assumptions");
    int rank = int_field(a, "score_rank", 0);
    if (rank < 2 || rank > 7)
        throw SpecError("event.assumptions.score_rank 必须为 2..7");
    double min_index = positive(a, "minimum_index", 0);
    if (phase == "challenge") {
        if (const Json *ids = engine.problem().raw.at("event").find("challenge_music_ids")) {
            bool found = false;
            for (const auto &id : ids->items())
                if (id.as_int64() == engine.problem().song.id)
                    found = true;
            if (!found)
                throw SpecError("课题曲不属于该活动，请使用 --challenge-problem");
        }
    }
    const std::string rank_mode = str_field(a, "score_rank_mode", "fixed");
    if (rank_mode != "fixed" && rank_mode != "estimated_score")
        throw SpecError("score_rank_mode 必须为 fixed/estimated_score");
    std::vector<PhaseRewards> reward_models;
    for (int r = 2; r <= 7; ++r)
        reward_models.emplace_back(engine, phase, r);
    const PhaseRewards &reward_model = reward_models[rank_mode == "estimated_score" ? 5 : rank - 2];
    const Json *rank_rows = engine.problem().raw.at("event").find("score_ranks");
    if (rank_mode == "estimated_score") {
        if (!engine.problem().settings.score.level_alpha.has_value())
            throw SpecError("预测档位需要显式提供 score_model.level_alpha；其值仍须实测校准");
        if (!rank_rows || rank_rows->items().empty())
            throw SpecError("预测档位需要 event.score_ranks 阈值表");
        bool base_rank = false;
        std::map<int, double> thresholds;
        for (const auto &row : rank_rows->items()) {
            int r = int_field(row, "rank");
            if (r < 2 || r > 7)
                throw SpecError("score_ranks.rank 必须为 2..7");
            if (!row.find("required_score"))
                throw SpecError("score_ranks 缺少 required_score");
            double threshold = positive(row, "required_score", 0);
            if (!thresholds.emplace(r, threshold).second)
                throw SpecError("score_ranks 的 rank 不能重复");
            if (r == 2 && threshold == 0)
                base_rank = true;
        }
        if (!base_rank)
            throw SpecError("score_ranks 需要 rank=2、required_score=0 的基础档位");
        double previous = -1;
        for (const auto &entry : thresholds) {
            if (entry.second <= previous)
                throw SpecError("score_ranks 的分数门槛必须随 rank 严格递增");
            previous = entry.second;
        }
    }
    const auto member_bonuses = card_bonuses(engine, "members");
    const auto snapshot_bonuses = card_bonuses(engine, "snapshots");
    const std::set<int64_t> required_member_ids(
        engine.problem().constraints.required_members.begin(),
        engine.problem().constraints.required_members.end());
    const std::set<int64_t> required_snapshot_ids(
        engine.problem().constraints.required_snapshots.begin(),
        engine.problem().constraints.required_snapshots.end());
    std::vector<Candidate> pool;

    auto leaders = engine.problem().leader_candidates();
    leaders.erase(std::remove_if(leaders.begin(), leaders.end(),
                                 [&](const Member *m) {
                                     return !options.leaders.empty() &&
                                            std::find(options.leaders.begin(),
                                                      options.leaders.end(),
                                                      m->id) == options.leaders.end();
                                 }),
                  leaders.end());
    std::stable_sort(leaders.begin(), leaders.end(), [&](const Member *l, const Member *r) {
        return member_bonuses.at(l->id).pt > member_bonuses.at(r->id).pt;
    });
    using FormationKey = std::pair<int64_t, std::vector<std::pair<int64_t, int64_t>>>;
    auto formation_key = [](const Formation &f) {
        FormationKey key{f.leader, {}};
        for (const auto &slot : f.slots)
            key.second.emplace_back(slot.member, slot.snapshot);
        std::sort(key.second.begin(), key.second.end());
        return key;
    };
    std::set<FormationKey> evaluated;
    const bool fixed_rank = rank_mode == "fixed";
    const bool mean_rewards = options.objective == "mean_score";
    auto finalize = [&]() {
        for (auto &candidate : pool) {
            if (!candidate.score.has_estimated &&
                engine.problem().settings.score.level_alpha.has_value()) {
                engine.populate_estimated_score(candidate.score);
                ++stats.absolute_score_evaluations;
            }
        }
    };
    auto retain = [&](Formation formation, double pt, double drop) {
        if (options.method != "exact" && !evaluated.insert(formation_key(formation)).second) {
            ++stats.duplicate_formations;
            return;
        }
        ++stats.complete_formations;
        auto yield_dominated = [&](const PhaseRewards &model) {
            const Yield possible = model.calculate(pt, drop);
            return std::any_of(pool.begin(), pool.end(), [&](const Candidate &other) {
                return dominates(other.yield, possible);
            });
        };
        Yield envelope;
        for (const auto &model : reward_models) {
            const Yield possible = model.calculate(pt, drop);
            envelope.cp = std::max(envelope.cp, possible.cp);
            envelope.pt = std::max(envelope.pt, possible.pt);
            envelope.shop = std::max(envelope.shop, possible.shop);
        }
        const bool envelope_dominated =
            std::any_of(pool.begin(), pool.end(),
                        [&](const Candidate &other) { return dominates(other.yield, envelope); });
        if (fixed_rank ? yield_dominated(reward_models[rank - 2])
            : mean_rewards
                ? envelope_dominated
                : std::all_of(reward_models.begin(), reward_models.end(), yield_dominated)) {
            ++stats.dominated_before_evaluation;
            return;
        }
        auto retain_score = [&](Evaluation score, Formation scored_formation,
                                const Yield *expected = nullptr, Json distribution = Json()) {
            if (score.index < min_index)
                return;
            for (auto &slot : scored_formation.slots)
                for (const auto &evaluated : score.slots)
                    if (slot.member == evaluated.member)
                        slot.trigger = evaluated.trigger;
            int result_rank = rank;
            if (!fixed_rank) {
                double total = num_field(score.estimated_score, "total");
                result_rank = 2;
                for (const auto &row : rank_rows->items())
                    if (total >= num_field(row, "required_score") &&
                        int_field(row, "rank") > result_rank)
                        result_rank = int_field(row, "rank");
            }
            Yield realized =
                expected ? *expected : reward_models[result_rank - 2].calculate(pt, drop);
            Candidate c{scored_formation, std::move(score), realized, expected ? 0 : result_rank,
                        std::move(distribution)};
            bool dominated = false;
            for (const auto &other : pool)
                if (dominates(other.yield, c.yield)) {
                    dominated = true;
                    break;
                }
            if (dominated)
                return;
            pool.erase(
                std::remove_if(pool.begin(), pool.end(),
                               [&](const Candidate &o) { return dominates(c.yield, o.yield); }),
                pool.end());

            auto equal = std::find_if(pool.begin(), pool.end(), [&](const Candidate &o) {
                return o.yield.cp == c.yield.cp && o.yield.pt == c.yield.pt &&
                       o.yield.shop == c.yield.shop;
            });
            if (equal == pool.end())
                pool.push_back(std::move(c));
            else if (c.score.index > equal->score.index)
                *equal = std::move(c);
        };
        EvalOptions evaluation_options;
        evaluation_options.calculate_score = !fixed_rank;
        if (fixed_rank) {
            ++stats.index_evaluations;
            retain_score(engine.evaluate(formation, evaluation_options), formation);
            return;
        }
        if (mean_rewards) {
            Yield sum;
            int count = 0;
            double score_sum = 0;
            double maximum = -std::numeric_limits<double>::infinity();
            double minimum = std::numeric_limits<double>::infinity();
            bool feasible = true;
            std::map<int, int> ranks;
            Evaluation representative;
            const bool finished =
                engine.for_each_score_order(formation, [&](const Evaluation &score) {
                    if (expired())
                        return false;
                    ++stats.index_evaluations;
                    ++stats.absolute_score_evaluations;
                    if (score.index < min_index)
                        feasible = false;
                    const double total = num_field(score.estimated_score, "total");
                    int achieved = 2;
                    for (const auto &row : rank_rows->items())
                        if (total >= num_field(row, "required_score"))
                            achieved = std::max(achieved, static_cast<int>(int_field(row, "rank")));
                    const Yield y = reward_models[achieved - 2].calculate(pt, drop);
                    sum.cp += y.cp;
                    sum.pt += y.pt;
                    sum.shop += y.shop;
                    score_sum += total;
                    ++ranks[achieved];
                    if (total > maximum)
                        representative = score;
                    maximum = std::max(maximum, total);
                    minimum = std::min(minimum, total);
                    ++count;
                    return true;
                });
            if (!finished) {
                complete = false;
                return;
            }
            if (count == 0 || !feasible)
                return;
            sum.cp /= count;
            sum.pt /= count;
            sum.shop /= count;
            representative.ranking_objective = "mean_score";
            representative.ranking_score = score_sum / count;
            representative.order_analysis = Json::object();
            representative.order_analysis.set("activation_order", Json("random_permutation"));
            representative.order_analysis.set("best_order_is_controllable", Json(false));
            representative.order_analysis.set("mean_score", Json(score_sum / count));
            representative.order_analysis.set("best_score", Json(maximum));
            representative.order_analysis.set("worst_score", Json(minimum));
            representative.order_analysis.set("score_permutations", Json(count));
            representative.order_analysis.set("order_search_complete", Json(true));
            Json distribution = Json::array();
            for (const auto &[achieved, occurrences] : ranks) {
                Json row = Json::object();
                row.set("rank", Json(achieved));
                row.set("probability", Json(double(occurrences) / count));
                distribution.push_back(std::move(row));
            }
            retain_score(std::move(representative), formation, &sum, std::move(distribution));
            return;
        }
        if (!engine.for_each_score_order(formation, [&](const Evaluation &score) {
                if (expired())
                    return false;
                ++stats.index_evaluations;
                ++stats.absolute_score_evaluations;
                retain_score(score, formation);
                return true;
            }))
            complete = false;
    };
    if (options.method == "exact") {
        if (engine.members().size() < static_cast<size_t>(engine.team_size()) ||
            engine.snapshots().empty() ||
            (engine.problem().constraints.distinct_snapshots &&
             engine.snapshots().size() < static_cast<size_t>(engine.team_size())))
            return pool;
        auto upwards = [&](double v) {
            return std::nextafter(
                v * (1 + (2 * engine.team_size() + 8) * std::numeric_limits<double>::epsilon()),
                std::numeric_limits<double>::infinity());
        };
        auto top_sum = [&](std::vector<double> values, int count) {
            std::sort(values.begin(), values.end(), std::greater<double>());
            double total = 0;
            for (int i = 0; i < count; ++i)
                total += values[i];
            return upwards(total);
        };
        auto covered = [&](const Yield &y) {
            return std::any_of(pool.begin(), pool.end(), [&](const Candidate &c) {
                return c.yield.cp >= y.cp && c.yield.pt >= y.pt && c.yield.shop >= y.shop;
            });
        };
        std::vector<std::pair<int, double>> reward_thresholds;
        if (!fixed_rank)
            for (const auto &row : rank_rows->items())
                reward_thresholds.emplace_back(int_field(row, "rank"),
                                               num_field(row, "required_score"));
        auto reward_covered = [&](double pt, double drop, double upper_score) {
            if (pool.empty())
                return false;
            if (fixed_rank)
                return covered(reward_models[rank - 2].calculate(pt, drop));
            upper_score = std::min(upper_score, stats.score_ceiling);
            if (mean_rewards) {
                Yield envelope;
                for (const auto &[rank, threshold] : reward_thresholds) {
                    if (threshold > upper_score)
                        continue;
                    const Yield y = reward_models[rank - 2].calculate(pt, drop);
                    envelope.cp = std::max(envelope.cp, y.cp);
                    envelope.pt = std::max(envelope.pt, y.pt);
                    envelope.shop = std::max(envelope.shop, y.shop);
                }
                return covered(envelope);
            }
            for (const auto &[rank, threshold] : reward_thresholds)
                if (threshold <= upper_score &&
                    !covered(reward_models[rank - 2].calculate(pt, drop)))
                    return false;
            return true;
        };
        if (engine.members().size() * engine.snapshots().size() > 64 && !expired()) {
            RankOptions seed_options = options;
            seed_options.method = "fast";
            seed_options.verbose = false;
            seed_options.progress = {};
            PhaseSearchStats seed_stats;
            bool seed_complete = false;
            pool = search(engine, seed_options, phase, std::min(0.5, seconds * 0.1), seed_complete,
                          seed_stats);
            stats.seed_formations = seed_stats.complete_formations;
            const auto seeds = pool;
            for (const auto &candidate : seeds) {
                double pt = 0, drop = 0;
                for (const auto &slot : candidate.formation.slots) {
                    pt += member_bonuses.at(slot.member).pt + snapshot_bonuses.at(slot.snapshot).pt;
                    drop += member_bonuses.at(slot.member).drop +
                            snapshot_bonuses.at(slot.snapshot).drop;
                }
                for (size_t slot = 0; slot < candidate.formation.slots.size(); ++slot) {
                    const auto &before =
                        snapshot_bonuses.at(candidate.formation.slots[slot].snapshot);
                    for (const Snapshot *snap : engine.snapshots()) {
                        if (expired())
                            break;
                        const auto &after = snapshot_bonuses.at(snap->id);
                        if (after.pt < before.pt || after.drop < before.drop ||
                            (after.pt == before.pt && after.drop == before.drop))
                            continue;
                        Formation f = candidate.formation;
                        f.slots[slot].snapshot = snap->id;
                        try {
                            f.validate(engine.problem());
                        } catch (const SpecError &) {
                            continue;
                        }
                        retain(f, pt - before.pt + after.pt, drop - before.drop + after.drop);
                    }
                }
            }
        }
        std::vector<double> member_pt, member_drop, all_snapshot_pt, all_snapshot_drop;
        for (const Member *m : engine.members()) {
            member_pt.push_back(member_bonuses.at(m->id).pt);
            member_drop.push_back(member_bonuses.at(m->id).drop);
        }
        for (const Snapshot *snap : engine.snapshots()) {
            all_snapshot_pt.push_back(snapshot_bonuses.at(snap->id).pt);
            all_snapshot_drop.push_back(snapshot_bonuses.at(snap->id).drop);
        }
        auto global_bonus = [&](const std::vector<double> &members,
                                const std::vector<double> &snapshots) {
            const double snap =
                engine.problem().constraints.distinct_snapshots
                    ? top_sum(snapshots, engine.team_size())
                    : upwards(*std::max_element(snapshots.begin(), snapshots.end()) *
                              engine.team_size());
            return upwards(top_sum(members, engine.team_size()) + snap);
        };
        if (reward_covered(global_bonus(member_pt, all_snapshot_pt),
                           global_bonus(member_drop, all_snapshot_drop),
                           std::numeric_limits<double>::infinity())) {
            stats.reward_ceiling_certified = true;
            complete = true;
            finalize();
            return pool;
        }
        NativeScoreBound score_bound;
        if (!fixed_rank) {
            bool supported = true;
            try {
                engine.validate_theoretical_scope();
            } catch (const SpecError &) {
                supported = false;
            }
            if (supported) {
                double live = 0;
                for (double v : engine.boosts())
                    live += v;
                live *= 1 + (engine.members().size() + 2) * std::numeric_limits<float>::epsilon();
                std::vector<float> live_bound(
                    engine.chart().times.size(),
                    std::nextafter(static_cast<float>(live),
                                   std::numeric_limits<float>::infinity()));
                score_bound = engine.linear_score_bound(live_bound);
                stats.score_bound_enabled = score_bound.enabled;
            }
        }
        if (score_bound.enabled && engine.team_size() <= 8 && !expired()) {
            RankOptions bound_options = options;
            bound_options.method = "exact";
            bound_options.objective = "theoretical_score";
            bound_options.top = 1;
            bound_options.restarts = 0;
            bound_options.anneal_steps = 0;
            bound_options.beam_width = std::min(16, options.beam_width);
            bound_options.verbose = false;
            bound_options.progress = {};
            bound_options.excluded_member_sets.clear();
            bound_options.initial_formations.clear();
            for (const auto &candidate : pool)
                bound_options.initial_formations.push_back(candidate.formation);
            bound_options.time_limit_s = std::min(0.5, seconds * 0.1);
            const auto ceiling = rank_formations(engine, bound_options);
            if (!ceiling.results.empty() && ceiling.audit.at("theoretical_max_certified").as_bool())
                stats.score_ceiling = *ceiling.results[0].ranking_score;
        }
        Formation formation;
        const Member *leader = nullptr;
        const std::vector<std::vector<int64_t>> *power_matrix = nullptr;
        std::vector<size_t> scoped_members;
        std::vector<double> snapshot_pt, snapshot_drop;
        for (const Snapshot *snap : engine.snapshots()) {
            snapshot_pt.push_back(snapshot_bonuses.at(snap->id).pt);
            snapshot_drop.push_back(snapshot_bonuses.at(snap->id).drop);
        }
        auto maximum_snapshot_sum = [&](const std::vector<double> &values) {
            return engine.problem().constraints.distinct_snapshots
                       ? top_sum(values, engine.team_size())
                       : upwards(*std::max_element(values.begin(), values.end()) *
                                 engine.team_size());
        };
        const double max_pt = maximum_snapshot_sum(snapshot_pt),
                     max_drop = maximum_snapshot_sum(snapshot_drop);
        std::vector<std::vector<double>> member_gains;
        if (score_bound.enabled) {
            member_gains.assign(engine.members().size(), std::vector<double>(engine.team_size()));
            for (size_t m = 0; m < engine.members().size(); ++m)
                for (size_t snap = 0; snap < engine.snapshots().size(); ++snap)
                    for (int k = 0; k < engine.team_size(); ++k)
                        member_gains[m][k] =
                            std::max(member_gains[m][k],
                                     score_bound.gains[m * engine.snapshots().size() + snap][k]);
        }
        std::vector<std::vector<double>> max_power;
        if (score_bound.enabled) {
            max_power.assign(leaders.size(), std::vector<double>(engine.members().size()));
            for (size_t l = 0; l < leaders.size(); ++l) {
                const auto &matrix = engine.power_matrix(engine.member_pos(leaders[l]->id));
                for (size_t m = 0; m < engine.members().size(); ++m)
                    max_power[l][m] = *std::max_element(matrix[m].begin(), matrix[m].end());
            }
        }
        const size_t snapshot_count = engine.snapshots().size();
        auto descending = [&](const auto &value) {
            std::vector<size_t> order(snapshot_count);
            for (size_t snap = 0; snap < snapshot_count; ++snap)
                order[snap] = snap;
            std::stable_sort(order.begin(), order.end(),
                             [&](size_t a, size_t b) { return value(a) > value(b); });
            return order;
        };
        const auto pt_order = descending([&](size_t snap) { return snapshot_pt[snap]; });
        const auto drop_order = descending([&](size_t snap) { return snapshot_drop[snap]; });
        std::vector<std::vector<std::vector<size_t>>> power_order, gain_order;
        if (score_bound.enabled) {
            power_order.resize(leaders.size());
            gain_order.resize(engine.members().size());
            for (size_t l = 0; l < leaders.size(); ++l) {
                const auto &matrix = engine.power_matrix(engine.member_pos(leaders[l]->id));
                for (size_t m = 0; m < engine.members().size(); ++m)
                    power_order[l].push_back(
                        descending([&](size_t snap) { return matrix[m][snap]; }));
            }
            for (size_t m = 0; m < engine.members().size(); ++m)
                for (int k = 0; k < engine.team_size(); ++k)
                    gain_order[m].push_back(descending([&](size_t snap) {
                        return score_bound.gains[m * snapshot_count + snap][k];
                    }));
        }
        std::vector<size_t> required_positions;
        std::vector<unsigned> snapshot_uses(snapshot_count);
        bool unavailable_required_snapshot = false;
        for (int64_t id : required_snapshot_ids) {
            auto found = std::find_if(engine.snapshots().begin(), engine.snapshots().end(),
                                      [&](const Snapshot *snap) { return snap->id == id; });
            if (found == engine.snapshots().end())
                unavailable_required_snapshot = true;
            else
                required_positions.push_back(found - engine.snapshots().begin());
        }
        const size_t mask_count = score_bound.enabled ? size_t{1} << engine.team_size() : 0;
        std::vector<std::vector<double>> selected_dp(
            engine.team_size() + 1,
            std::vector<double>(mask_count, -std::numeric_limits<double>::infinity()));
        if (score_bound.enabled)
            selected_dp[0][0] = 0;
        std::vector<std::vector<size_t>> trigger_masks(engine.team_size() + 1);
        for (size_t mask = 0; mask < mask_count; ++mask)
            trigger_masks[__builtin_popcount(static_cast<unsigned>(mask))].push_back(mask);
        std::vector<size_t> selected_snapshots(engine.team_size());
        std::vector<double> group_pt(engine.team_size() + 1), group_drop(engine.team_size() + 1);
        size_t selected_leader = 0;
        double group_gain_upper = 0;
        const bool distinct_snapshots = engine.problem().constraints.distinct_snapshots;
        auto first_available = [&](const std::vector<size_t> &order) {
            for (size_t snap : order)
                if (!distinct_snapshots || !snapshot_uses[snap])
                    return snap;
            return snapshot_count;
        };
        auto snapshot_bonus = [&](const std::vector<size_t> &order,
                                  const std::vector<double> &values, size_t remaining) {
            if (!remaining)
                return 0.0;
            if (!distinct_snapshots)
                return upwards(values[order.front()] * remaining);
            double sum = 0;
            for (size_t snap : order)
                if (!snapshot_uses[snap]) {
                    sum += values[snap];
                    if (!--remaining)
                        break;
                }
            return upwards(sum);
        };
        auto cached_prunable = [&](size_t depth, double pt, double drop, double selected_power) {
            ++stats.bound_nodes;
            ++stats.cached_snapshot_nodes;
            const size_t remaining = engine.team_size() - depth;
            const size_t missing_required =
                std::count_if(required_positions.begin(), required_positions.end(),
                              [&](size_t snap) { return !snapshot_uses[snap]; });
            if (unavailable_required_snapshot || missing_required > remaining ||
                (distinct_snapshots && snapshot_count - depth < remaining)) {
                ++stats.infeasible_branches;
                return true;
            }
            const double upper_pt =
                upwards(pt + group_pt[depth] + snapshot_bonus(pt_order, snapshot_pt, remaining));
            const double upper_drop = upwards(drop + group_drop[depth] +
                                              snapshot_bonus(drop_order, snapshot_drop, remaining));
            auto covered_score = [&](double score) {
                if (!reward_covered(upper_pt, upper_drop, score))
                    return false;
                ++stats.reward_pruned_branches;
                return true;
            };
            if (covered_score(stats.score_ceiling))
                return true;
            if (!score_bound.enabled)
                return false;
            double power = selected_power;
            for (size_t i = depth; i < scoped_members.size(); ++i) {
                const size_t m = scoped_members[i];
                power += (*power_matrix)[m][first_available(power_order[selected_leader][m])];
            }
            const double upper_power = upwards(power);
            auto score_upper = [&](double gain) {
                return NativeScoreBound::upper(upper_power, upwards(gain), score_bound.base,
                                               score_bound.max_power, engine.team_size(),
                                               engine.chart().times.size());
            };
            if (covered_score(score_upper(group_gain_upper)))
                return true;
            auto &dp = selected_dp[depth];
            std::fill(dp.begin(), dp.end(), -std::numeric_limits<double>::infinity());
            const auto &last_gain = score_bound.gains[scoped_members[depth - 1] * snapshot_count +
                                                      selected_snapshots[depth - 1]];
            for (size_t mask : trigger_masks[depth - 1])
                for (int k = 0; k < engine.team_size(); ++k)
                    if (!(mask & (size_t{1} << k)))
                        dp[mask | (size_t{1} << k)] =
                            std::max(dp[mask | (size_t{1} << k)],
                                     selected_dp[depth - 1][mask] + last_gain[k]);
            auto completion_gain = [&](const std::vector<std::vector<double>> &rows) {
                auto work = dp;
                for (size_t i = 0; i < rows.size(); ++i) {
                    std::vector<double> next(mask_count, -std::numeric_limits<double>::infinity());
                    for (size_t mask : trigger_masks[depth + i])
                        for (int k = 0; k < engine.team_size(); ++k)
                            if (!(mask & (size_t{1} << k)))
                                next[mask | (size_t{1} << k)] = std::max(
                                    next[mask | (size_t{1} << k)], work[mask] + rows[i][k]);
                    work = std::move(next);
                }
                return work.back();
            };
            std::vector<std::vector<double>> future_gain(remaining,
                                                         std::vector<double>(engine.team_size()));
            for (size_t i = depth; i < scoped_members.size(); ++i) {
                const size_t m = scoped_members[i];
                for (int k = 0; k < engine.team_size(); ++k) {
                    const size_t snap = first_available(gain_order[m][k]);
                    future_gain[i - depth][k] = score_bound.gains[m * snapshot_count + snap][k];
                }
            }
            const double upper_score = score_upper(completion_gain(future_gain));
            if (covered_score(upper_score))
                return true;
            if (mean_rewards)
                return false;
            if (remaining < 2 || depth > 2)
                return false;
            ++stats.coupled_bound_nodes;
            auto axis_value = [](const Yield &y, int axis) {
                return axis == 0 ? y.cp : axis == 1 ? y.pt : y.shop;
            };
            for (const auto &[rank, threshold] : reward_thresholds) {
                if (threshold > std::min(upper_score, stats.score_ceiling))
                    continue;
                const auto &model = reward_models[rank - 2];
                const Yield upper_yield = model.calculate(upper_pt, upper_drop);
                if (covered(upper_yield))
                    continue;
                bool rank_covered = false;
                for (const auto &witness : pool) {
                    bool can_escape = false;
                    for (int axis = 0; axis < 3 && !can_escape; ++axis) {
                        if (axis_value(upper_yield, axis) <= axis_value(witness.yield, axis))
                            continue;
                        double conditional_power = selected_power;
                        std::vector<std::vector<double>> rows(
                            remaining, std::vector<double>(engine.team_size()));
                        bool feasible = true;
                        const double other_pt =
                            snapshot_bonus(pt_order, snapshot_pt, remaining - 1);
                        const double other_drop =
                            snapshot_bonus(drop_order, snapshot_drop, remaining - 1);
                        for (size_t i = depth; i < scoped_members.size(); ++i) {
                            const size_t m = scoped_members[i];
                            double best_power = -1;
                            for (size_t snap = 0; snap < snapshot_count; ++snap) {
                                if (distinct_snapshots && snapshot_uses[snap])
                                    continue;
                                const Yield edge_yield = model.calculate(
                                    upwards(pt + group_pt[depth] + snapshot_pt[snap] + other_pt),
                                    upwards(drop + group_drop[depth] + snapshot_drop[snap] +
                                            other_drop));
                                if (axis_value(edge_yield, axis) <= axis_value(witness.yield, axis))
                                    continue;
                                best_power = std::max(best_power, double((*power_matrix)[m][snap]));
                                for (int k = 0; k < engine.team_size(); ++k)
                                    rows[i - depth][k] =
                                        std::max(rows[i - depth][k],
                                                 score_bound.gains[m * snapshot_count + snap][k]);
                            }
                            if (best_power < 0) {
                                feasible = false;
                                break;
                            }
                            conditional_power += best_power;
                        }
                        if (feasible &&
                            NativeScoreBound::upper(
                                upwards(conditional_power), upwards(completion_gain(rows)),
                                score_bound.base, score_bound.max_power, engine.team_size(),
                                engine.chart().times.size()) >= threshold)
                            can_escape = true;
                    }
                    if (!can_escape) {
                        rank_covered = true;
                        break;
                    }
                }
                if (!rank_covered)
                    return false;
            }
            ++stats.coupled_pruned_branches;
            ++stats.reward_pruned_branches;
            return true;
        };
        std::function<void(size_t, double, double, double)> snapshots;
        snapshots = [&](size_t depth, double pt, double drop, double power) {
            if (expired()) {
                complete = false;
                return;
            }
            if (depth && cached_prunable(depth, pt, drop, power))
                return;
            if (depth == scoped_members.size()) {
                formation.validate(engine.problem());
                retain(formation, pt, drop);
                return;
            }
            const size_t m = scoped_members[depth];
            for (size_t snap = 0; snap < snapshot_count; ++snap) {
                if (distinct_snapshots && snapshot_uses[snap])
                    continue;
                const auto &mb = member_bonuses.at(engine.members()[m]->id);
                const auto &sb = snapshot_bonuses.at(engine.snapshots()[snap]->id);
                formation.slots.push_back(Slot{engine.members()[m]->id,
                                               engine.snapshots()[snap]->id,
                                               static_cast<int>(depth) + 1});
                selected_snapshots[depth] = snap;
                ++snapshot_uses[snap];
                snapshots(depth + 1, pt + mb.pt + sb.pt, drop + mb.drop + sb.drop,
                          score_bound.enabled ? power + (*power_matrix)[m][snap] : 0);
                --snapshot_uses[snap];
                formation.slots.pop_back();
                if (!complete)
                    return;
            }
        };
        std::vector<std::vector<double>> suffix_pt(engine.members().size() + 1),
            suffix_drop(engine.members().size() + 1);
        for (size_t first = 0; first <= engine.members().size(); ++first) {
            std::vector<double> pt_values, drop_values;
            for (size_t m = first; m < engine.members().size(); ++m) {
                const auto &bonus = member_bonuses.at(engine.members()[m]->id);
                pt_values.push_back(bonus.pt);
                drop_values.push_back(bonus.drop);
            }
            int limit = std::min(static_cast<int>(pt_values.size()), engine.team_size());
            for (int count = 0; count <= limit; ++count) {
                suffix_pt[first].push_back(top_sum(pt_values, count));
                suffix_drop[first].push_back(top_sum(drop_values, count));
            }
        }
        std::function<void(size_t, double, double)> groups;
        groups = [&](size_t first, double pt, double drop) {
            if (expired()) {
                complete = false;
                return;
            }
            const size_t remaining = engine.team_size() - scoped_members.size();
            if (remaining >= suffix_pt[first].size())
                return;
            if (remaining &&
                reward_covered(upwards(pt + suffix_pt[first][remaining] + max_pt),
                               upwards(drop + suffix_drop[first][remaining] + max_drop),
                               stats.score_ceiling)) {
                ++stats.pruned_member_prefixes;
                return;
            }
            if (scoped_members.size() == static_cast<size_t>(engine.team_size())) {
                ++stats.member_groups;
                for (int64_t required : required_member_ids)
                    if (std::none_of(scoped_members.begin(), scoped_members.end(),
                                     [&](size_t m) { return engine.members()[m]->id == required; }))
                        return;
                std::vector<size_t> eligible_leaders;
                for (size_t l = 0; l < leaders.size(); ++l)
                    if (std::any_of(scoped_members.begin(), scoped_members.end(), [&](size_t m) {
                            return engine.members()[m]->id == leaders[l]->id;
                        }))
                        eligible_leaders.push_back(l);
                if (eligible_leaders.empty())
                    return;
                double upper_pt = upwards(pt + max_pt), upper_drop = upwards(drop + max_drop);
                bool bounded = reward_covered(upper_pt, upper_drop, stats.score_ceiling);
                if (!bounded && score_bound.enabled) {
                    double power = 0;
                    for (size_t l : eligible_leaders) {
                        double sum = 0;
                        for (size_t m : scoped_members)
                            sum += max_power[l][m];
                        power = std::max(power, sum);
                    }
                    std::vector<double> dp(size_t{1} << engine.team_size(),
                                           -std::numeric_limits<double>::infinity());
                    dp[0] = 0;
                    for (size_t mask = 0; mask + 1 < dp.size(); ++mask) {
                        size_t m = scoped_members[__builtin_popcount(static_cast<unsigned>(mask))];
                        for (int k = 0; k < engine.team_size(); ++k)
                            if (!(mask & (size_t{1} << k)))
                                dp[mask | (size_t{1} << k)] = std::max(
                                    dp[mask | (size_t{1} << k)], dp[mask] + member_gains[m][k]);
                    }
                    group_gain_upper = dp.back();
                    const double score = NativeScoreBound::upper(
                        upwards(power), upwards(group_gain_upper), score_bound.base,
                        score_bound.max_power, engine.team_size(), engine.chart().times.size());
                    bounded = reward_covered(upper_pt, upper_drop, score);
                }
                if (bounded) {
                    ++stats.bounded_member_groups;
                    return;
                }
                group_pt.back() = group_drop.back() = 0;
                for (size_t i = scoped_members.size(); i-- > 0;) {
                    const auto &bonus = member_bonuses.at(engine.members()[scoped_members[i]]->id);
                    group_pt[i] = group_pt[i + 1] + bonus.pt;
                    group_drop[i] = group_drop[i + 1] + bonus.drop;
                }
                for (size_t l : eligible_leaders) {
                    selected_leader = l;
                    leader = leaders[l];
                    formation.leader = leader->id;
                    power_matrix = score_bound.enabled
                                       ? &engine.power_matrix(engine.member_pos(leader->id))
                                       : nullptr;
                    snapshots(0, 0, 0, 0);
                    if (!complete)
                        return;
                }
                return;
            }
            for (size_t m = first; m < engine.members().size(); ++m) {
                bool legal = true;
                if (engine.problem().constraints.distinct_characters)
                    for (size_t selected : scoped_members)
                        legal = legal && engine.members()[selected]->character !=
                                             engine.members()[m]->character;
                if (!legal)
                    continue;
                const auto &bonus = member_bonuses.at(engine.members()[m]->id);
                scoped_members.push_back(m);
                groups(m + 1, pt + bonus.pt, drop + bonus.drop);
                scoped_members.pop_back();
                if (!complete)
                    return;
            }
        };
        groups(0, 0, 0);
        finalize();
        return pool;
    }
    struct State {
        Formation f;
        double power = 0, gain = 0, pt = 0, drop = 0;
        Yield y;
    };
    auto metric = [](const State &s, int axis) {
        const double index = s.power * (1 + s.gain);
        switch (axis) {
        case 0:
            return s.y.cp;
        case 1:
            return s.y.pt;
        case 2:
            return s.y.shop;
        case 3:
            return index;
        case 4:
            return s.y.pt / 100 + s.y.shop / 120;
        case 5:
            return s.y.pt * s.y.shop;
        case 6:
            return index * (1 + s.pt / 10000);
        case 7:
            return index * (1 + s.drop / 10000);
        default:
            return index * (1 + s.pt / 10000) * (1 + s.drop / 10000);
        }
    };
    auto extend = [&](const State &state, const Member *m, const Snapshot *snap, State &n) {
        for (const auto &slot : state.f.slots) {
            const Member *used = engine.index().member_by_id.at(slot.member);
            if (slot.member == m->id ||
                (engine.problem().constraints.distinct_characters &&
                 used->character == m->character) ||
                (engine.problem().constraints.distinct_snapshots && slot.snapshot == snap->id))
                return false;
        }
        int missing_members = 0, missing_snapshots = 0;
        for (int64_t id : required_member_ids) {
            bool found = id == m->id;
            for (const auto &slot : state.f.slots)
                found = found || slot.member == id;
            missing_members += !found;
        }
        for (int64_t id : required_snapshot_ids) {
            bool found = id == snap->id;
            for (const auto &slot : state.f.slots)
                found = found || slot.snapshot == id;
            missing_snapshots += !found;
        }
        int remaining = engine.team_size() - static_cast<int>(state.f.slots.size()) - 1;
        if (missing_members > remaining || missing_snapshots > remaining)
            return false;
        n = state;
        n.f.slots.push_back(Slot{m->id, snap->id, static_cast<int>(n.f.slots.size()) + 1});
        int mi = engine.member_pos(m->id), si = engine.snapshot_pos(snap->id);
        n.power += engine.slot_power(engine.member_pos(state.f.leader), mi, si);
        const auto &gains = engine.gains()[mi][si];
        n.gain += *std::max_element(gains.begin(), gains.end());
        const auto &mb = member_bonuses.at(m->id);
        const auto &sb = snapshot_bonuses.at(snap->id);
        n.pt += mb.pt + sb.pt;
        n.drop += mb.drop + sb.drop;
        n.y = reward_model.calculate(n.pt, n.drop);
        return true;
    };
    auto retain_valid = [&](const State &state) {
        if (state.f.slots.size() != static_cast<size_t>(engine.team_size()))
            return;
        try {
            state.f.validate(engine.problem());
        } catch (const SpecError &) {
            return;
        }
        retain(state.f, state.pt, state.drop);
    };
    const bool seed_scope =
        engine.team_size() <= 8 && std::all_of(engine.members().begin(), engine.members().end(),
                                               [](const Member *m) { return m->character >= 0; });
    if (!fixed_rank && seed_scope && !expired()) {
        RankOptions seed_options = options;
        seed_options.method = "fast";
        seed_options.verbose = false;
        seed_options.progress = {};
        seed_options.objective = "index";
        seed_options.top = 4;
        seed_options.beam_width = std::min(16, options.beam_width);
        seed_options.restarts = 0;
        seed_options.anneal_steps = 0;
        seed_options.time_limit_s = std::min(0.25, seconds * 0.15);
        auto seeds = rank_formations(engine, seed_options);
        for (const auto &score : seeds.results) {
            if (expired())
                break;
            State state;
            state.f.leader = score.leader;
            for (const auto &slot : score.slots) {
                state.f.slots.push_back(Slot{slot.member, slot.snapshot, slot.trigger});
                state.pt +=
                    member_bonuses.at(slot.member).pt + snapshot_bonuses.at(slot.snapshot).pt;
                state.drop +=
                    member_bonuses.at(slot.member).drop + snapshot_bonuses.at(slot.snapshot).drop;
            }
            retain_valid(state);
            ++stats.score_seeds;
        }
    }
    for (int axis : {1, 2, 6, 3, 5, 7, 8}) {
        if (expired())
            break;
        for (const Member *leader : leaders) {
            if (expired())
                break;
            if (axis == 1)
                ++stats.leaders_seeded;
            State state;
            state.f.leader = leader->id;
            for (int depth = 0; depth < engine.team_size(); ++depth) {
                State best;
                double best_metric = -1, best_index = -1;
                for (const Member *m : engine.members()) {
                    if (depth == 0 && m->id != leader->id)
                        continue;
                    for (const Snapshot *snap : engine.snapshots()) {
                        State next;
                        if (!extend(state, m, snap, next))
                            continue;
                        double value = metric(next, axis), index = metric(next, 3);
                        if (value > best_metric || (value == best_metric && index > best_index)) {
                            best_metric = value;
                            best_index = index;
                            best = std::move(next);
                        }
                    }
                }
                if (best_metric < 0)
                    break;
                state = std::move(best);
            }
            retain_valid(state);
            if (expired())
                break;
        }
    }
    size_t leaders_remaining = leaders.size();
    for (const Member *leader : leaders) {
        if (expired())
            break;
        double remaining =
            seconds -
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto leader_start = std::chrono::steady_clock::now();
        double leader_seconds = remaining / std::max(size_t{1}, leaders_remaining--);
        auto leader_expired = [&] {
            return expired() ||
                   std::chrono::duration<double>(std::chrono::steady_clock::now() - leader_start)
                           .count() >= leader_seconds;
        };
        const int width = !std::isfinite(leader_seconds)
                              ? options.beam_width
                              : std::min(options.beam_width, leader_seconds < 0.1 ? 1 : 8);
        std::vector<State> beam;
        State anchor;
        anchor.f.leader = leader->id;
        for (const Snapshot *snap : engine.snapshots()) {
            State state;
            if (extend(anchor, leader, snap, state))
                beam.push_back(std::move(state));
        }
        for (int depth = 1; depth < engine.team_size() && !beam.empty(); ++depth) {
            std::vector<State> next;
            auto trim = [&]() {
                std::vector<State> selected;
                std::set<FormationKey> seen;
                std::vector<size_t> indices(next.size());
                for (size_t i = 0; i < indices.size(); ++i)
                    indices[i] = i;
                const size_t count = std::min(next.size(), static_cast<size_t>(width));
                for (int axis = 0; axis < 9; ++axis) {
                    std::partial_sort(indices.begin(), indices.begin() + count, indices.end(),
                                      [&](size_t l, size_t r) {
                                          double x = metric(next[l], axis),
                                                 y = metric(next[r], axis);
                                          if (x != y)
                                              return x > y;
                                          double lx = metric(next[l], 3), rx = metric(next[r], 3);
                                          return lx == rx ? l < r : lx > rx;
                                      });
                    for (size_t i = 0; i < count; ++i) {
                        size_t at = indices[i];
                        if (seen.insert(formation_key(next[at].f)).second)
                            selected.push_back(next[at]);
                    }
                }
                next = std::move(selected);
            };
            for (const auto &state : beam) {
                if (leader_expired())
                    break;
                for (const Member *m : engine.members())
                    for (const Snapshot *s : engine.snapshots()) {
                        State n;
                        if (!extend(state, m, s, n))
                            continue;
                        next.push_back(std::move(n));
                        if (next.size() > 8192)
                            trim();
                    }
            }
            trim();
            beam = std::move(next);
        }
        for (const auto &state : beam) {
            if (expired())
                break;
            retain_valid(state);
        }
    }
    finalize();
    return pool;
}
}
struct EventSearchCache::Impl {
    struct Entry {
        std::weak_ptr<const int> identity;
        std::string phase;
        std::string objective;
        std::vector<int64_t> leaders;
        std::vector<Candidate> candidates;
        PhaseSearchStats proof;
    };
    std::vector<Entry> entries;
    int64_t hits = 0, misses = 0;
};
EventSearchCache::EventSearchCache() : impl_(std::make_unique<Impl>()) {}
EventSearchCache::~EventSearchCache() = default;
Json EventSearchCache::audit() const {
    Json out = Json::object();
    out.set("phase_hits", Json(impl_->hits));
    out.set("phase_misses", Json(impl_->misses));
    out.set(
        "phase_entries",
        Json(int64_t(std::count_if(impl_->entries.begin(), impl_->entries.end(),
                                   [](const auto &entry) { return !entry.identity.expired(); }))));
    return out;
}
Json event_recommend(const Engine &engine, const RankOptions &options,
                     const Engine *challenge_engine, EventSearchCache *cache) {
    if (!engine.problem().raw.find("event"))
        throw SpecError("event 需要 problem.event，使用 Master 导入器生成");
    if (options.method != "fast" && options.method != "auto" && options.method != "exact")
        throw SpecError("未知活动 method");
    if (options.objective != "mean_score" && options.objective != "theoretical_score")
        throw SpecError("活动 objective 必须为 mean/score");
    if (!std::isfinite(options.time_limit_s) || options.time_limit_s < 0)
        throw SpecError("活动 time-limit 必须为非负数");
    const Engine &challenge_model = challenge_engine ? *challenge_engine : engine;
    const Json *ce = challenge_model.problem().raw.find("event");
    if (!ce || int_field(*ce, "id") != int_field(engine.problem().raw.at("event"), "id"))
        throw SpecError("普通曲与课题曲必须属于同一活动");
    const Json &na = engine.problem().raw.at("event").at("assumptions");
    const Json &ca = ce->at("assumptions");
    if (int_field(na, "shop_resource_id") != int_field(ca, "shop_resource_id") ||
        int_field(na, "shop_resource_type", 1) != int_field(ca, "shop_resource_type", 1))
        throw SpecError("两个阶段必须比较同一种商店货币");
    bool normal_complete = false, challenge_complete = false;
    PhaseSearchStats normal_stats, challenge_stats;
    const double phase_seconds = options.time_limit_s == 0 ? std::numeric_limits<double>::infinity()
                                                           : options.time_limit_s / 2;
    auto phase_search = [&](const Engine &model, const std::string &phase, bool &complete,
                            PhaseSearchStats &stats) {
        if (!cache || options.method != "exact")
            return search(model, options, phase, phase_seconds, complete, stats);
        auto &entries = cache->impl_->entries;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [](const auto &entry) { return entry.identity.expired(); }),
                      entries.end());
        auto leaders = options.leaders;
        std::sort(leaders.begin(), leaders.end());
        leaders.erase(std::unique(leaders.begin(), leaders.end()), leaders.end());
        const auto identity = model.cache_identity().lock();
        for (const auto &entry : entries) {
            if (entry.identity.lock() == identity && entry.phase == phase &&
                entry.objective == options.objective && entry.leaders == leaders) {
                ++cache->impl_->hits;
                complete = true;
                stats.phase_cache_hit = true;
                stats.reward_ceiling_certified = entry.proof.reward_ceiling_certified;
                stats.score_bound_enabled = entry.proof.score_bound_enabled;
                stats.score_ceiling = entry.proof.score_ceiling;
                return entry.candidates;
            }
        }
        ++cache->impl_->misses;
        auto candidates = search(model, options, phase, phase_seconds, complete, stats);
        if (complete) {
            if (entries.size() >= 64)
                entries.erase(entries.begin());
            entries.push_back({model.cache_identity(), phase, options.objective, std::move(leaders),
                               candidates, stats});
        }
        return candidates;
    };
    auto normal = phase_search(engine, "normal", normal_complete, normal_stats);
    auto challenge =
        phase_search(challenge_model, "challenge", challenge_complete, challenge_stats);
    if (normal.empty() || challenge.empty())
        throw SpecError("活动搜索未找到满足约束的完整组队，请增加 time-limit 或检查约束");
    Json out = Json::object();
    out.set("schema", Json("ournotes-event-plan@1"));
    out.set("input_mode", Json(engine.problem().input_mode));
    out.set("reward_objective",
            Json(options.objective == "mean_score" ? "mean_reward" : "best_order_reward"));
    Json normal_assumptions = na;
    if (!na.find("cp_boost_rate"))
        normal_assumptions.set(
            "cp_boost_rate",
            Json(num_field(boost(engine.problem().raw.at("event").at("normal_boosts"),
                                 int_field(na, "normal_boost_cost")),
                           "reward_rate", 1)));
    normal_assumptions.set("cp_bonus_source", Json(str_field(na, "cp_bonus_source", "none")));
    normal_assumptions.set("score_rank_mode", Json(str_field(na, "score_rank_mode", "fixed")));
    out.set("normal_assumptions", normal_assumptions);
    out.set("challenge_assumptions", ca);
    out.set("normal_song_id", Json(engine.problem().song.id));
    out.set("challenge_song_id", Json(challenge_model.problem().song.id));
    out.set("search_method", Json(options.method));
    out.set("normal_search_audit", normal_stats.to_json());
    out.set("challenge_search_audit", challenge_stats.to_json());
    out.set("normal_search_complete", Json(normal_complete));
    out.set("challenge_search_complete", Json(challenge_complete));
    Json scope = Json::array();
    for (int64_t id : options.leaders)
        scope.push_back(Json(id));
    out.set("requested_leaders", scope);
    out.set("certified", Json(false));
    out.set("optimality_certified", Json(normal_complete && challenge_complete));
    out.set("optimality_scope", Json("input_songs_card_pools_leaders_and_reward_assumptions"));
    out.set("score_rank_verified", Json(false));
    out.set("reward_formula_calibrated", Json(false));
    Json n = Json::array(), c = Json::array();
    for (const auto &x : normal)
        n.push_back(candidate_json(x));
    for (const auto &x : challenge)
        c.push_back(candidate_json(x));
    out.set("normal_frontier", std::move(n));
    out.set("challenge_frontier", std::move(c));
    double cost = int_field(ca, "challenge_cp_cost", 200);
    if (cost <= 0)
        throw SpecError("challenge_cp_cost 必须为正数");
    Json cycles = Json::array();
    for (size_t i = 0; i < normal.size(); ++i)
        for (size_t j = 0; j < challenge.size(); ++j) {
            const auto &a = normal[i].yield;
            const auto &b = challenge[j].yield;
            Yield y{0, a.pt + a.cp / cost * b.pt, a.shop + a.cp / cost * b.shop};
            Json x = Json::object();
            x.set("normal_candidate", Json(static_cast<int64_t>(i)));
            x.set("challenge_candidate", Json(static_cast<int64_t>(j)));
            x.set("amortized_per_normal_live", yield_json(y));
            x.set("challenge_runs_per_normal_live", Json(a.cp / cost));
            cycles.push_back(std::move(x));
        }
    Json recommended = Json::object();
    for (const char *objective : {"pt", "shop_currency_expected"}) {
        const Json *best = nullptr;
        double best_value = -1;
        for (const auto &cycle : cycles.items()) {
            double v = num_field(cycle.at("amortized_per_normal_live"), objective);
            if (v > best_value) {
                best = &cycle;
                best_value = v;
            }
        }
        recommended.set(objective, best ? *best : Json());
    }
    const Candidate *best_cp = nullptr;
    for (const auto &x : normal)
        if (!best_cp || x.yield.cp > best_cp->yield.cp)
            best_cp = &x;
    recommended.set("cp", best_cp ? candidate_json(*best_cp) : Json());
    out.set("recommended", std::move(recommended));
    bool deterministic_cp = true;
    for (const auto &candidate : normal) {
        if (candidate.rank_distribution.is_null())
            continue;
        const auto &rows = candidate.rank_distribution.items();
        double first = -1;
        // Finite budgets require CP to be constant across reachable ranks.
        for (const auto &row : rows) {
            const double cp = value(engine.problem().raw.at("event").at("normal").at("cp"),
                                    int_field(row, "rank"), "value");
            if (first < 0)
                first = cp;
            else if (first != cp)
                deterministic_cp = false;
        }
    }
    if (na.find("normal_runs") && deterministic_cp) {
        double count = positive(na, "normal_runs", 0), initial = positive(na, "initial_cp", 0);
        if (count != std::floor(count) || initial != std::floor(initial))
            throw SpecError("normal_runs/initial_cp 必须为非负整数");
        Json finite = Json::array();
        for (size_t i = 0; i < normal.size(); ++i)
            for (size_t j = 0; j < challenge.size(); ++j) {
                double cp = initial + count * normal[i].yield.cp, plays = std::floor(cp / cost);
                Json x = Json::object();
                x.set("normal_candidate", Json(static_cast<int64_t>(i)));
                x.set("challenge_candidate", Json(static_cast<int64_t>(j)));
                x.set("challenge_runs", Json(plays));
                x.set("remaining_cp", Json(cp - plays * cost));
                x.set("pt", Json(count * normal[i].yield.pt + plays * challenge[j].yield.pt));
                x.set("shop_currency_expected",
                      Json(count * normal[i].yield.shop + plays * challenge[j].yield.shop));
                finite.push_back(std::move(x));
            }
        Json finite_recommended = Json::object();
        for (const char *objective : {"pt", "shop_currency_expected"}) {
            const Json *best = nullptr;
            double best_value = -1;
            for (const auto &plan : finite.items()) {
                double v = num_field(plan, objective);
                if (v > best_value) {
                    best = &plan;
                    best_value = v;
                }
            }
            finite_recommended.set(objective, best ? *best : Json());
        }
        out.set("recommended_finite_budget", std::move(finite_recommended));
        out.set("finite_budget", std::move(finite));
    }
    out.set("cycles", std::move(cycles));
    Json warnings = Json::array();
    if (na.find("normal_runs") && !deterministic_cp)
        warnings.push_back(Json("平均模式下 CP 随技能顺序变化；暂不提供有限预算计划，避免对平均 CP "
                                "取整导致错误。cycles 提供长期期望收益。"));
    if (options.objective == "mean_score")
        warnings.push_back(Json("平均收益按每种技能顺序分别确定分数档位、取整奖励，再求平均；单局收"
                                "益可能不同。score_rank=0 表示档位分布。"));
    warnings.push_back(
        Json("exact 完成表示已搜索或通过安全上界排除指定范围内的合法成员/Snapshot 组合；fixed 按 "
             "AP index "
             "优化技能顺序，"
             "estimated_score 枚举全部技能顺序。限时中断或 fast/auto 不证明全局最优。"));
    warnings.push_back(Json("fixed 模式以输入档位为条件；estimated_score 按未校准的预测分数匹配 "
                            "Master 阈值。minimum_index 不证明实际档位。"));
    warnings.push_back(
        Json("普通曲 B 档、3 体力样本已复现；其他档位、课题曲与完整取整流程仍待校准。"));
    warnings.push_back(Json("cycles 是长期平均收益；有限 CP 应按完整课题曲次数结算并保留余量。"));
    out.set("warnings", std::move(warnings));
    return out;
}
}
