#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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

}

int main() {
    const char *from_env = std::getenv("OURNOTES_PROBLEM");
    std::string path = from_env != nullptr ? from_env : "/tmp/prob-100109-expert.json";
    if (!std::filesystem::exists(path)) {
        std::printf("test_engine: 跳过，找不到 %s\n", path.c_str());
        return 0;
    }

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
        order_outputs_match &=
            ordered.to_json(false, false, 0).dump(0) == expected.to_json(false, false, 0).dump(0);
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

    std::printf("test_engine: %d checks passed, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
