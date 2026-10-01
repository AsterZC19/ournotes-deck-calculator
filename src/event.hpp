#pragma once
#include "solver.hpp"
namespace deckcalc {
Json event_recommend(const Engine &engine, const RankOptions &options,
                     const Engine *challenge_engine = nullptr);
}
