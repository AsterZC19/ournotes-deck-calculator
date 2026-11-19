#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "engine.hpp"
#include "json.hpp"
#include "model.hpp"
#include "solver.hpp"

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

#define CHECK(cond) check((cond), #cond, __LINE__)

const char *kParallelSolverProblem = R"JSON({
  "schema": "ournotes-deck-problem@1",
  "input_mode": "experimental",
  "song": {"id": 2002, "title": "par", "type": 1},
  "chart": {
    "difficulty": "expert",
    "level": 25,
    "notes": [
      {"t": 1000, "op": 1}, {"t": 1500, "op": 1}, {"t": 2000, "op": 1},
      {"t": 5000, "op": 1}, {"t": 5500, "op": 1}, {"t": 6000, "op": 1},
      {"t": 9000, "op": 1}, {"t": 9500, "op": 1}, {"t": 10000, "op": 1}
    ],
    "skill_times_ms": [900, 4900, 8900]
  },
  "catalog": {
    "fix": {"music_type_base_bp": 500, "type_link_base_bp": 500},
    "members": [
      {"id": 1, "character": 1, "band": 1, "card_type": 1, "trained": [3100, 3100, 3100], "live_skill": 1, "leader_skill": 1},
      {"id": 2, "character": 2, "band": 1, "card_type": 1, "trained": [3000, 3000, 3000], "live_skill": 2, "leader_skill": 2},
      {"id": 3, "character": 3, "band": 1, "card_type": 2, "trained": [2900, 2900, 2900], "live_skill": 1, "leader_skill": 1},
      {"id": 4, "character": 4, "band": 2, "card_type": 1, "trained": [2800, 2800, 2800], "live_skill": 2, "leader_skill": 2},
      {"id": 5, "character": 5, "band": 2, "card_type": 2, "trained": [2700, 2700, 2700], "live_skill": 1, "leader_skill": 1}
    ],
    "snapshots": [
      {"id": 11, "card_type": 1, "trained": [1200, 1200, 1200], "support_skills": [1]},
      {"id": 12, "card_type": 1, "trained": [1100, 1100, 1100], "support_skills": [1]},
      {"id": 13, "card_type": 2, "trained": [1000, 1000, 1000], "support_skills": [2]},
      {"id": 14, "card_type": 2, "trained": [950, 950, 950], "support_skills": [2]},
      {"id": 15, "card_type": 1, "trained": [900, 900, 900], "support_skills": []},
      {"id": 16, "card_type": 2, "trained": [850, 850, 850], "support_skills": []}
    ],
    "targets": [{"id": 1, "band": 1}],
    "live_skills": [
      {"id": 1, "effects": [{"level": 5, "effect_type": 2000, "value": 8000, "duration_ms": 2000}]},
      {"id": 2, "effects": [{"level": 5, "effect_type": 2000, "value": 11000, "duration_ms": 2000}]}
    ],
    "leader_skills": [
      {"id": 1, "effects": [{"level": 5, "effect_type": 1000, "value": 2500}]},
      {"id": 2, "effects": [{"level": 5, "effect_type": 1500, "value": 1800, "cumulative_type": 3000, "max_cumulative_count": 3, "cumulative_targets": [1]}]}
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
    "score_model": {"level_alpha": 0.005, "rounding": "float32_floor"}
  }
})JSON";

void test_self_contained_solver() {
    Problem p = parse_problem(Json::parse(kParallelSolverProblem));
    Engine engine(p);

    // Parallel exact search with progress callback + mixed cumulative/simple leaders.
    for (const std::string &obj : {std::string("mean_score"), std::string("theoretical_score")}) {
        std::atomic<int> progress_calls{0};
        RankOptions opts;
        opts.method = "exact";
        opts.objective = obj;
        opts.top = 3;
        opts.time_limit_s = 0;
        opts.verbose = true;
        opts.progress = [&](const std::string &) { ++progress_calls; };
        RankResult res = rank_formations(engine, opts);
        CHECK(res.results.size() == 3);
        CHECK(res.audit.at("top_k_certified").as_bool());
        CHECK(res.audit.at("leaders_proven").as_int64() == 5);
        CHECK(progress_calls.load() > 0);
        // Verify Top-1 matches independent brute-force maximum across all 5 leaders.
        double oracle_best = -1.0;
        const auto &ms = engine.members();
        const auto &ss = engine.snapshots();
        for (size_t l = 0; l < ms.size(); ++l) {
            for (size_t a = 0; a < ms.size(); ++a) {
                if (a == l)
                    continue;
                for (size_t b = a + 1; b < ms.size(); ++b) {
                    if (b == l)
                        continue;
                    for (size_t s0 = 0; s0 < ss.size(); ++s0)
                        for (size_t s1 = 0; s1 < ss.size(); ++s1) {
                            if (s1 == s0)
                                continue;
                            for (size_t s2 = 0; s2 < ss.size(); ++s2) {
                                if (s2 == s0 || s2 == s1)
                                    continue;
                                Formation f;
                                f.leader = ms[l]->id;
                                f.slots = {Slot{ms[l]->id, ss[s0]->id, 1},
                                           Slot{ms[a]->id, ss[s1]->id, 2},
                                           Slot{ms[b]->id, ss[s2]->id, 3}};
                                Evaluation ev = (obj == "mean_score")
                                                    ? engine.evaluate_mean(f, false)
                                                    : engine.evaluate_theoretical(f, false);
                                oracle_best = std::max(oracle_best, *ev.ranking_score);
                            }
                        }
                }
            }
        }
        CHECK(std::fabs(*res.results.front().ranking_score - oracle_best) <= 1e-9);
    }
}

}

int main() {
    test_self_contained_solver();

    const char *from_env = std::getenv("OURNOTES_PROBLEM");
    std::string path = from_env != nullptr ? from_env : "/tmp/prob-100109-expert.json";
    if (std::filesystem::exists(path)) {
        Problem problem = parse_problem(Json::read(path));
        Engine engine(problem);

        RankOptions options;
        options.method = "fast";
        options.objective = "index";
        options.top = 1;
        options.leaders = {61};
        RankResult ranked = rank_formations(engine, options);
        CHECK(!ranked.results.empty());
        if (!ranked.results.empty()) {
            const Evaluation &best = ranked.results.front();
            CHECK(best.leader == 61);
            const std::set<int64_t> expected{55, 59, 61, 62, 63};
            const std::vector<int64_t> ids = best.member_ids();
            const std::set<int64_t> actual(ids.begin(), ids.end());
            CHECK(actual == expected);
            double scale = 5093558.561289373;
            CHECK(std::fabs(best.index - scale) / scale <= 1e-9);
        }
    }

    std::printf("test_solver: %d checks passed, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
