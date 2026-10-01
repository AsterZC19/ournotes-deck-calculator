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
    Json to_json() const {
        Json out = Json::object();
        out.set("complete_formations", Json(complete_formations));
        out.set("index_evaluations", Json(index_evaluations));
        out.set("absolute_score_evaluations", Json(absolute_score_evaluations));
        out.set("dominated_before_evaluation", Json(dominated_before_evaluation));
        return out;
    }
};
struct Candidate {
    Formation formation;
    Evaluation score;
    Yield yield;
    int score_rank = 0;
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
    std::vector<Candidate> pool;

    auto leaders = engine.problem().leader_candidates();
    std::stable_sort(leaders.begin(), leaders.end(), [&](const Member *l, const Member *r) {
        const auto &rows = engine.problem().raw.at("catalog").at("members");
        auto bonus = [&](int64_t id) {
            for (const auto &row : rows.items())
                if (int_field(row, "id") == id)
                    return num_field(row, "event_pt_bonus_bp");
            return 0.0;
        };
        return bonus(l->id) > bonus(r->id);
    });
    const bool fixed_rank = rank_mode == "fixed";
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
        ++stats.complete_formations;
        const Yield fixed_yield = reward_models[rank - 2].calculate(pt, drop);
        if (fixed_rank && std::any_of(pool.begin(), pool.end(), [&](const Candidate &other) {
                return dominates(other.yield, fixed_yield);
            })) {
            ++stats.dominated_before_evaluation;
            return;
        }
        EvalOptions evaluation_options;
        evaluation_options.calculate_score = !fixed_rank;
        Evaluation score = engine.evaluate(formation, evaluation_options);
        ++stats.index_evaluations;
        if (score.has_estimated)
            ++stats.absolute_score_evaluations;
        if (score.index < min_index)
            return;
        for (auto &slot : formation.slots)
            for (const auto &evaluated : score.slots)
                if (slot.member == evaluated.member)
                    slot.trigger = evaluated.trigger;
        int result_rank = rank;
        if (rank_mode == "estimated_score") {
            double total = num_field(score.estimated_score, "total");
            result_rank = 2;
            for (const auto &row : rank_rows->items())
                if (total >= num_field(row, "required_score") &&
                    int_field(row, "rank") > result_rank)
                    result_rank = int_field(row, "rank");
        }
        Yield realized = reward_models[result_rank - 2].calculate(pt, drop);
        Candidate c{formation, std::move(score), realized, result_rank};
        bool dominated = false;
        for (const auto &other : pool)
            if (dominates(other.yield, c.yield)) {
                dominated = true;
                break;
            }
        if (dominated)
            return;
        pool.erase(std::remove_if(pool.begin(), pool.end(),
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
    if (options.method == "exact") {
        for (const Member *leader : leaders) {
            if (!options.leaders.empty() &&
                std::find(options.leaders.begin(), options.leaders.end(), leader->id) ==
                    options.leaders.end())
                continue;
            Formation formation;
            formation.leader = leader->id;
            std::function<void(size_t, double, double)> visit;
            visit = [&](size_t first, double pt, double drop) {
                if (expired()) {
                    complete = false;
                    return;
                }
                if (formation.slots.size() == static_cast<size_t>(engine.team_size())) {
                    try {
                        formation.validate(engine.problem());
                    } catch (const SpecError &) {
                        return;
                    }
                    retain(formation, pt, drop);
                    return;
                }
                const bool anchor = formation.slots.empty();
                for (size_t i = first; i < engine.members().size(); ++i) {
                    const Member *m = engine.members()[i];
                    if (anchor && m->id != leader->id)
                        continue;
                    bool legal = true;
                    for (const auto &used : formation.slots)
                        if (used.member == m->id ||
                            (engine.problem().constraints.distinct_characters &&
                             engine.index().member_by_id.at(used.member)->character ==
                                 m->character))
                            legal = false;
                    if (!legal)
                        continue;
                    for (const Snapshot *snap : engine.snapshots()) {
                        bool free = true;
                        for (const auto &used : formation.slots)
                            if (engine.problem().constraints.distinct_snapshots &&
                                used.snapshot == snap->id)
                                free = false;
                        if (!free)
                            continue;
                        const auto &mb = member_bonuses.at(m->id);
                        const auto &sb = snapshot_bonuses.at(snap->id);
                        formation.slots.push_back(
                            Slot{m->id, snap->id, static_cast<int>(formation.slots.size()) + 1});
                        visit(anchor ? 0 : i + 1, pt + mb.pt + sb.pt, drop + mb.drop + sb.drop);
                        formation.slots.pop_back();
                        if (!complete)
                            return;
                    }
                }
            };
            visit(0, 0, 0);
            if (!complete)
                break;
        }
        finalize();
        return pool;
    }
    for (const Member *leader : leaders) {
        if (expired())
            break;
        if (!options.leaders.empty() && std::find(options.leaders.begin(), options.leaders.end(),
                                                  leader->id) == options.leaders.end())
            continue;
        struct State {
            Formation f;
            double power = 0, gain = 0, pt = 0, drop = 0;
            Yield y;
        };
        std::vector<State> beam;
        for (const Snapshot *snap : engine.snapshots()) {
            State state;
            state.f.leader = leader->id;
            state.f.slots.push_back(Slot{leader->id, snap->id, 1});
            int mi = engine.member_pos(leader->id), si = engine.snapshot_pos(snap->id);
            state.power = engine.slot_power(mi, mi, si);
            const auto &gains = engine.gains()[mi][si];
            state.gain = *std::max_element(gains.begin(), gains.end());
            const auto &mb = member_bonuses.at(leader->id);
            const auto &sb = snapshot_bonuses.at(snap->id);
            state.pt = mb.pt + sb.pt;
            state.drop = mb.drop + sb.drop;
            state.y = reward_model.calculate(state.pt, state.drop);
            beam.push_back(std::move(state));
        }
        for (int depth = 1; depth < engine.team_size() && !beam.empty(); ++depth) {
            std::vector<State> next;
            auto trim = [&]() {
                std::vector<State> selected;
                std::set<std::string> seen;
                int width = std::max(8, options.beam_width);
                for (int axis = 0; axis < 6; ++axis) {
                    auto metric = [&](const State &s) {
                        return axis == 0   ? s.y.cp
                               : axis == 1 ? s.y.pt
                               : axis == 2 ? s.y.shop
                               : axis == 3 ? s.power * (1 + s.gain)
                               : axis == 4 ? s.y.pt / 100 + s.y.shop / 120
                                           : s.y.pt * s.y.shop;
                    };
                    std::stable_sort(next.begin(), next.end(), [&](const State &l, const State &r) {
                        double x = metric(l), y = metric(r);
                        return x == y ? l.power * (1 + l.gain) > r.power * (1 + r.gain) : x > y;
                    });
                    for (size_t i = 0; i < std::min(next.size(), static_cast<size_t>(width)); ++i) {
                        Formation canonical = next[i].f;
                        std::sort(canonical.slots.begin(), canonical.slots.end(),
                                  [](const Slot &a, const Slot &b) { return a.member < b.member; });
                        for (size_t k = 0; k < canonical.slots.size(); ++k)
                            canonical.slots[k].trigger = static_cast<int>(k) + 1;
                        std::string key = dump_formation(canonical).dump(0);
                        if (seen.insert(key).second)
                            selected.push_back(next[i]);
                    }
                }
                next = std::move(selected);
            };
            for (const auto &state : beam) {
                if (expired())
                    break;
                for (const Member *m : engine.members())
                    for (const Snapshot *s : engine.snapshots()) {
                        bool legal = true;
                        for (const auto &slot : state.f.slots) {
                            const Member *used = engine.index().member_by_id.at(slot.member);
                            if (slot.member == m->id ||
                                (engine.problem().constraints.distinct_characters &&
                                 used->character == m->character) ||
                                (engine.problem().constraints.distinct_snapshots &&
                                 slot.snapshot == s->id))
                                legal = false;
                        }
                        if (!legal)
                            continue;
                        int missing_members = 0, missing_snapshots = 0;
                        for (int64_t id : engine.problem().constraints.required_members) {
                            bool found = id == m->id;
                            for (const auto &slot : state.f.slots)
                                found = found || slot.member == id;
                            if (!found)
                                ++missing_members;
                        }
                        for (int64_t id : engine.problem().constraints.required_snapshots) {
                            bool found = id == s->id;
                            for (const auto &slot : state.f.slots)
                                found = found || slot.snapshot == id;
                            if (!found)
                                ++missing_snapshots;
                        }
                        int remaining = engine.team_size() - depth - 1;
                        if (missing_members > remaining || missing_snapshots > remaining)
                            continue;

                        State n = state;
                        n.f.slots.push_back(Slot{m->id, s->id, depth + 1});
                        int mi = engine.member_pos(m->id), si = engine.snapshot_pos(s->id);
                        n.power += engine.slot_power(engine.member_pos(leader->id), mi, si);
                        const auto &gains = engine.gains()[mi][si];
                        n.gain += *std::max_element(gains.begin(), gains.end());
                        const auto &mb = member_bonuses.at(m->id);
                        const auto &sb = snapshot_bonuses.at(s->id);
                        n.pt += mb.pt + sb.pt;
                        n.drop += mb.drop + sb.drop;
                        n.y = reward_model.calculate(n.pt, n.drop);
                        next.push_back(std::move(n));
                        if (next.size() > 8192)
                            trim();
                    }
            }
            trim();
            beam = std::move(next);
        }
        for (auto &state : beam) {
            if (state.f.slots.size() != static_cast<size_t>(engine.team_size()))
                continue;
            try {
                state.f.validate(engine.problem());
            } catch (const SpecError &) {
                continue;
            }
            retain(state.f, state.pt, state.drop);
        }
    }
    finalize();
    return pool;
}
}
Json event_recommend(const Engine &engine, const RankOptions &options,
                     const Engine *challenge_engine) {
    if (!engine.problem().raw.find("event"))
        throw SpecError("event 需要 problem.event，使用 Master 导入器生成");
    if (options.method != "fast" && options.method != "auto" && options.method != "exact")
        throw SpecError("未知活动 method");
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
    auto normal = search(engine, options, "normal", phase_seconds, normal_complete, normal_stats);
    auto challenge = search(challenge_model, options, "challenge", phase_seconds,
                            challenge_complete, challenge_stats);
    if (normal.empty() || challenge.empty())
        throw SpecError("活动搜索未找到满足约束的完整组队，请增加 time-limit 或检查约束");
    Json out = Json::object();
    out.set("schema", Json("ournotes-event-plan@1"));
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
    if (na.find("normal_runs")) {
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
    warnings.push_back(
        Json("exact 完成表示已枚举指定队长范围内的合法成员/Snapshot 组合；技能顺序按 AP index "
             "优化。限时中断或 fast/auto 不证明全局最优。"));
    warnings.push_back(Json("fixed 模式以输入档位为条件；estimated_score 按未校准的预测分数匹配 "
                            "Master 阈值。minimum_index 不证明实际档位。"));
    warnings.push_back(
        Json("普通曲 B 档、3 体力样本已复现；其他档位、课题曲与完整取整流程仍待校准。"));
    warnings.push_back(Json("cycles 是长期平均收益；有限 CP 应按完整课题曲次数结算并保留余量。"));
    out.set("warnings", std::move(warnings));
    return out;
}
}
