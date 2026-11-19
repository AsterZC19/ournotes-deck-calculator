#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "engine.hpp"
#include "json.hpp"
#include "model.hpp"

using namespace deckcalc;

namespace {

int checks = 0;
int failures = 0;

void check(bool value, const char *what, int line) {
    ++checks;
    if (!value) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

void check_near(double actual, double expected, double tolerance, const char *what, int line) {
    ++checks;
    double scale = std::max(1.0, std::fabs(expected));
    if (std::fabs(actual - expected) / scale > tolerance) {
        std::printf("FAIL line %d: %s -> %.17g expected %.17g\n", line, what, actual, expected);
        ++failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_NEAR(actual, expected, tol) check_near((actual), (expected), (tol), #actual, __LINE__)

const char *kSyntheticSequenceProblem = R"JSON({
  "schema": "ournotes-deck-problem@1",
  "input_mode": "experimental",
  "song": {"id": 1001, "title": "seq", "type": 1, "tags": [10]},
  "chart": {
    "difficulty": "expert",
    "level": 24,
    "notes": [
      {"t": 1000, "op": 1}, {"t": 1500, "op": 1}, {"t": 2000, "op": 1}, {"t": 2500, "op": 1},
      {"t": 5000, "op": 1}, {"t": 5500, "op": 1}, {"t": 6000, "op": 1}, {"t": 6500, "op": 1},
      {"t": 9000, "op": 1}, {"t": 9500, "op": 1}, {"t": 10000, "op": 1}, {"t": 10500, "op": 1}
    ],
    "skill_times_ms": [900, 4900, 8900]
  },
  "catalog": {
    "fix": {"music_type_base_bp": 500, "type_link_base_bp": 500},
    "members": [
      {"id": 1, "character": 1, "band": 1, "card_type": 1, "trained": [3000, 3100, 3200], "live_skill": 1, "leader_skill": 1},
      {"id": 2, "character": 2, "band": 1, "card_type": 1, "trained": [2900, 3000, 3100], "live_skill": 2, "leader_skill": 2},
      {"id": 3, "character": 3, "band": 2, "card_type": 2, "trained": [2800, 2900, 3000], "live_skill": 3, "leader_skill": 1},
      {"id": 4, "character": 4, "band": 1, "card_type": 1, "trained": [2700, 2800, 2900], "live_skill": 1, "leader_skill": 2}
    ],
    "snapshots": [
      {"id": 11, "card_type": 1, "trained": [1000, 1000, 1000], "support_skills": [1]},
      {"id": 12, "card_type": 1, "trained": [900, 900, 900], "support_skills": [2]},
      {"id": 13, "card_type": 2, "trained": [800, 800, 800], "support_skills": []}
    ],
    "targets": [
      {"id": 1, "band": 1},
      {"id": 41, "judgement": 5},
      {"id": 46, "judgement": 6}
    ],
    "live_skills": [
      {"id": 1, "effects": [{"level": 5, "effect_type": 2000, "value": 8000, "duration_ms": 2000}]},
      {"id": 2, "effects": [{"level": 5, "effect_type": 2004, "value": 12000, "duration_ms": 2000, "targets": [41, 46]}]},
      {"id": 3, "effects": [{"level": 5, "effect_type": 2000, "value": 4000, "duration_ms": 2000}]}
    ],
    "leader_skills": [
      {"id": 1, "effects": [{"level": 5, "effect_type": 1000, "value": 2000}]},
      {"id": 2, "effects": [{"level": 5, "effect_type": 1500, "value": 1500, "cumulative_type": 3000, "max_cumulative_count": 3, "cumulative_targets": [1]}]}
    ],
    "support_skills": [
      {"id": 1, "effects": [{"level": 5, "effect_type": 15000, "value": 500}]},
      {"id": 2, "effects": [{"level": 5, "effect_type": 15000, "value": 1000}]}
    ],
    "note_parameters": [{"op": 1, "score_percent": 100}],
    "combo_bonuses": [{"type": 0, "required_combo_count": 0, "factor": 0.0}]
  },
  "settings": {
    "team_size": 3,
    "base_duration_ms": 2000,
    "score_model": {"level_alpha": 0.005, "rounding": "float32_floor"},
    "judgement": {
      "mode": "sequence",
      "sequence": ["perfect", "great", "perfect", "perfect", "perfect", "great", "perfect", "perfect", "perfect", "perfect", "great", "perfect"]
    }
  }
})JSON";

void test_self_contained_engine() {
    Problem p = parse_problem(Json::parse(kSyntheticSequenceProblem));
    Engine engine(p);

    // Per-leader formation condition tracking.
    CHECK(engine.rules().formation_leader_conditions());
    CHECK(!engine.rules().leader_has_formation_conditions(*engine.members()[0]));
    CHECK(engine.rules().leader_has_formation_conditions(*engine.members()[1]));

    // Deterministic non-AP sequence fast scoring + effect_type == 2004 support.
    CHECK(engine.fast_score_applicable());
    CHECK(engine.member_perfect_only()[0] == 0);
    CHECK(engine.member_perfect_only()[1] == 1);

    // base_power_matrix + leader_power_delta == slot_power for both simple and cumulative leaders.
    const auto &base_pm = engine.base_power_matrix();
    std::vector<const Member *> team{engine.members()[1], engine.members()[0], engine.members()[2]};
    for (size_t m = 0; m < engine.members().size(); ++m) {
        const int64_t simple_delta = engine.leader_power_delta(0, m, nullptr);
        const int64_t cum_delta = engine.leader_power_delta(1, m, &team);
        for (size_t s = 0; s < engine.snapshots().size(); ++s) {
            CHECK(base_pm[m][s] + simple_delta == engine.slot_power(0, m, s, nullptr));
            CHECK(base_pm[m][s] + cum_delta == engine.slot_power(1, m, s, &team));
        }
    }

    // WindowDeltaTable fast order enumeration vs full slow evaluation parity on sequence + 2004.
    Formation f;
    f.leader = 2;
    f.slots = {Slot{2, 11, 1}, Slot{1, 12, 2}, Slot{3, 13, 3}};
    EvalOptions given;
    given.order_search = "given";
    double sum_full = 0.0;
    double best_full = -1.0;
    int fast_count = 0;
    bool all_match = true;
    CHECK(engine.for_each_score_order_fast(
        f, [&](double total, double index, const std::vector<int> &perm) {
            Formation replay = f;
            for (size_t i = 0; i < 3; ++i)
                replay.slots[i].trigger = perm[i] + 1;
            Evaluation ref = engine.evaluate(replay, given);
            all_match &= (total == ref.estimated_score.at("total").as_double());
            all_match &= (std::fabs(index - ref.index) <= 1e-9);
            sum_full += total;
            best_full = std::max(best_full, total);
            ++fast_count;
            return true;
        }));
    CHECK(fast_count == 6);
    CHECK(all_match);

    Evaluation th = engine.evaluate_theoretical(f, false);
    Evaluation mn = engine.evaluate_mean(f, false);
    CHECK(th.ranking_score.has_value() && *th.ranking_score == best_full);
    CHECK(mn.ranking_score.has_value());
    CHECK_NEAR(*mn.ranking_score, sum_full / 6.0, 1e-12);

    // evaluate_mean_if_above cutoff pruning and exact parity.
    ScoreOrderStats st_pass;
    auto above = engine.evaluate_mean_if_above(f, *mn.ranking_score - 1.0, st_pass);
    CHECK(above.has_value());
    CHECK_NEAR(*above->ranking_score, *mn.ranking_score, 1e-12);
    CHECK(st_pass.evaluated == 6 && st_pass.pruned == 0);

    ScoreOrderStats st_cut;
    auto below = engine.evaluate_mean_if_above(f, best_full + 1000.0, st_cut);
    CHECK(!below.has_value());
    CHECK(st_cut.evaluated == 6);

    // evaluate_best_order cutoff pruning check.
    std::vector<float> max_live(engine.chart().times.size(), 1.5f);
    NativeScoreBound bound = engine.linear_score_bound(max_live);
    CHECK(bound.enabled);
    ScoreOrderStats st_best;
    auto best_pruned = engine.evaluate_best_order(f, bound, best_full + 1000.0, st_best);
    CHECK(!best_pruned.has_value());
    CHECK(st_best.pruned > 0);

    // Malformed formation checks on evaluate_mean_if_above and negative-boost check on
    // native_min_score.
    {
        Formation bad_len = f;
        bad_len.slots.pop_back();
        bool threw = false;
        try {
            ScoreOrderStats dummy;
            (void)engine.evaluate_mean_if_above(bad_len, 0.0, dummy);
        } catch (const SpecError &) {
            threw = true;
        }
        CHECK(threw);

        std::string neg_json(kSyntheticSequenceProblem);
        const auto pos = neg_json.find("\"value\": 4000");
        CHECK(pos != std::string::npos);
        neg_json.replace(pos, 13, "\"value\": -4000");
        Problem neg_p = parse_problem(Json::parse(neg_json));
        Engine neg_engine(neg_p);
        CHECK(neg_engine.native_min_score(100000, f) < 0.0);
    }

    // Concurrent const reads on a shared Engine instance (thread safety check).
    std::vector<std::thread> threads;
    std::vector<double> thread_means(4, 0.0);
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            for (int iter = 0; iter < 20; ++iter) {
                Evaluation ev = engine.evaluate_mean(f, false);
                thread_means[static_cast<size_t>(t)] = *ev.ranking_score;
            }
        });
    }
    for (auto &thrd : threads)
        thrd.join();
    for (double val : thread_means)
        CHECK_NEAR(val, *mn.ranking_score, 1e-12);
}

}

int main() {
    test_self_contained_engine();

    const char *from_env = std::getenv("OURNOTES_PROBLEM");
    std::string path = from_env != nullptr ? from_env : "/tmp/prob-100109-expert.json";
    if (std::filesystem::exists(path)) {
        Problem problem = parse_problem(Json::read(path));
        Engine engine(problem);

        ChartInfo info = engine.chart_info();
        CHECK(info.judged_notes == 759);
        CHECK(info.converted_note_count == 525);
        CHECK_NEAR(info.all_note_weight_sum, 524.1, 1e-12);
        CHECK_NEAR(info.note_weight_sum, 642.3585, 1e-9);
        CHECK(info.skill_times.size() == 5);
        CHECK(info.skill_times[0] == 9411 && info.skill_times[4] == 84705);

        Formation formation;
        formation.leader = 61;
        formation.slots = {
            Slot{62, 63, 1}, Slot{59, 33, 2}, Slot{63, 55, 3}, Slot{55, 61, 4}, Slot{61, 62, 5},
        };
        EvalOptions options;
        options.order_search = "given";
        options.validate = true;
        Evaluation evaluation = engine.evaluate(formation, options);
        EvalOptions deferred_options = options;
        deferred_options.calculate_score = false;
        Evaluation deferred = engine.evaluate(formation, deferred_options);
        CHECK(!deferred.has_estimated);
        CHECK(deferred.power == evaluation.power);
        CHECK_NEAR(deferred.index, evaluation.index, 1e-15);
        engine.populate_estimated_score(deferred);
        CHECK(deferred.has_estimated == evaluation.has_estimated);
        CHECK(deferred.estimated_score.dump(0) == evaluation.estimated_score.dump(0));

        int order_count = 0;
        bool order_outputs_match = true;
        CHECK(engine.for_each_score_order(formation, [&](const Evaluation &ordered) {
            Formation replay = formation;
            for (auto &slot : replay.slots)
                for (const auto &scored : ordered.slots)
                    if (slot.member == scored.member)
                        slot.trigger = scored.trigger;
            const auto expected = engine.evaluate(replay, options);
            order_outputs_match &= ordered.to_json(false, false, 0).dump(0) ==
                                   expected.to_json(false, false, 0).dump(0);
            ++order_count;
            return true;
        }));
        CHECK(order_count == 120);
        CHECK(order_outputs_match);
        order_count = 0;
        CHECK(!engine.for_each_score_order(formation, [&](const Evaluation &) {
            ++order_count;
            return false;
        }));
        CHECK(order_count == 1);

        CHECK(evaluation.power == 3124510);
        CHECK_NEAR(evaluation.weight_factor, 1.6301943540873203, 1e-12);
        CHECK_NEAR(evaluation.index, 5093558.561289373, 1e-12);

        if (evaluation.slots.size() == 5) {
            CHECK(evaluation.slots[0].member == 62 && evaluation.slots[0].snapshot == 63);
            CHECK(evaluation.slots[0].power == 977786 && evaluation.slots[0].duration_ms == 5000);
            CHECK(evaluation.slots[1].power == 377142 && evaluation.slots[1].duration_ms == 9000);
            CHECK(evaluation.slots[2].power == 339095 && evaluation.slots[2].duration_ms == 10000);
            CHECK(evaluation.slots[3].power == 427888 && evaluation.slots[3].duration_ms == 8000);
            CHECK(evaluation.slots[4].power == 1002599 && evaluation.slots[4].duration_ms == 10000);
        }

        int leader_pos = engine.member_pos(61);
        int member_pos = engine.member_pos(61);
        int snap_pos = engine.snapshot_pos(62);
        CHECK(leader_pos >= 0 && member_pos >= 0 && snap_pos >= 0);
        CHECK(engine.slot_power(leader_pos, member_pos, snap_pos) == 1002599);
        CHECK(engine.duration_ms(member_pos, snap_pos) == 10000);
        int snap61 = engine.snapshot_pos(61);
        CHECK(engine.slot_power(engine.member_pos(61), engine.member_pos(61), snap61) == 829993);
    }

    std::printf("test_engine: %d checks passed, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
