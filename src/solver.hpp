#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine.hpp"

namespace deckcalc {

struct RankOptions {
    std::string objective = "theoretical_score";
    std::string method = "auto";
    int top = 20;
    double time_limit_s = 60;
    std::vector<Formation> initial_formations;
    std::vector<std::vector<int64_t>> excluded_member_sets;
    std::vector<int64_t> leaders;
    int beam_width = 64;
    int restarts = 4;
    int anneal_steps = 3000;
    bool verbose = false;
    std::function<void(const std::string &)> progress;
};

struct RankResult {
    std::vector<Evaluation> results;
    Json audit;
};

RankResult rank_formations(const Engine &engine, const RankOptions &options);

}
