// 组卡搜索：束搜索加分支定界。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine.hpp"

namespace deckcalc {

struct RankOptions {
    std::string method = "auto";  // auto | fast | exact
    int top = 20;
    double time_limit_s = 60;
    std::vector<int64_t> leaders;  // 空 = 全部候选
    int beam_width = 64;
    int restarts = 4;
    bool verbose = false;
    std::function<void(const std::string&)> progress;
};

struct RankResult {
    std::vector<Evaluation> results;
    Json audit;
};

RankResult rank_formations(const Engine& engine, const RankOptions& options);

}  // namespace deckcalc
