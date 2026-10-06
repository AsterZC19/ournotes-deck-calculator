#pragma once
#include "solver.hpp"
#include <memory>
namespace deckcalc {
class EventSearchCache {
public:
    EventSearchCache();
    ~EventSearchCache();
    Json audit() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend Json event_recommend(const Engine &, const RankOptions &, const Engine *,
                                EventSearchCache *);
};
Json event_recommend(const Engine &engine, const RankOptions &options,
                     const Engine *challenge_engine = nullptr, EventSearchCache *cache = nullptr);
}
