#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

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

}

int main() {
    const char *from_env = std::getenv("OURNOTES_PROBLEM");
    std::string path = from_env != nullptr ? from_env : "/tmp/prob-100109-expert.json";
    if (!std::filesystem::exists(path)) {
        std::printf("test_solver: 跳过，找不到 %s\n", path.c_str());
        return 0;
    }

    Problem problem = parse_problem(Json::read(path));
    Engine engine(problem);

    RankOptions options;
    options.method = "fast";
    options.objective = "index";
    options.top = 1;
    options.leaders = {61};
    RankResult ranked = rank_formations(engine, options);
    CHECK(!ranked.results.empty());
    if (ranked.results.empty()) {
        std::printf("test_solver: %d checks passed, %d failed\n", checks, failures);
        return 1;
    }

    const Evaluation &best = ranked.results.front();
    CHECK(best.leader == 61);
    const std::set<int64_t> expected{55, 59, 61, 62, 63};
    const std::vector<int64_t> ids = best.member_ids();
    const std::set<int64_t> actual(ids.begin(), ids.end());
    CHECK(actual == expected);
    double scale = 5093558.561289373;
    CHECK(std::fabs(best.index - scale) / scale <= 1e-9);

    std::printf("test_solver: %d checks passed, %d failed, index %.9f\n", checks, failures,
                best.index);
    return failures == 0 ? 0 : 1;
}
