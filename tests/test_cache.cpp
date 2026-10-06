#include "event.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>

using namespace deckcalc;
namespace {
int checks = 0;
void check(bool condition) {
    ++checks;
    if (!condition)
        throw SpecError("cache assertion " + std::to_string(checks));
}
void verify(const Engine &engine) {
    for (size_t l = 0; l < engine.members().size(); ++l) {
        const auto &matrix = engine.power_matrix(l);
        for (size_t m = 0; m < engine.members().size(); ++m)
            for (size_t s = 0; s < engine.snapshots().size(); ++s)
                check(matrix[m][s] == engine.slot_power(l, m, s));
    }
}
}
int main() {
    Problem base = parse_problem(Json::read("tests/fixtures/theoretical-rounding-inversion.json"));
    EngineCache cache;
    {
        Engine first(base, &cache);
        verify(first);
        const int64_t misses = cache.audit().at("power_matrix_misses").as_int64();
        Problem other = base;
        other.song.id += 1;
        other.chart.notes[0].t += 10;
        Engine second(other, &cache);
        verify(second);
        check(cache.audit().at("power_matrix_misses").as_int64() == misses);
        check(cache.audit().at("power_matrix_hits").as_int64() > 0);
    }
    const std::vector<std::function<void(Problem &)>> changes = {
        [](Problem &p) { p.catalog.members[0].trained[0] += 123; },
        [](Problem &p) { p.catalog.members[0].event_bonus_bp += 123; },
        [](Problem &p) { p.catalog.members[0].card_rank_bonus_bp.type_link += 123; },
        [](Problem &p) { p.catalog.members[0].card_rank_bonus_bp.music_type += 123; },
        [](Problem &p) { p.catalog.members[0].card_type += 1; },
        [](Problem &p) { p.catalog.snapshots[0].trained[1] += 123; },
        [](Problem &p) { p.catalog.snapshots[0].event_bonus_bp += 123; },
        [](Problem &p) { p.catalog.snapshots[0].card_rank_bonus_bp.type_link += 123; },
        [](Problem &p) { p.catalog.snapshots[0].card_type += 1; },
        [](Problem &p) { p.catalog.fix.band_item_bonus_bp[0] += 123; },
        [](Problem &p) { p.catalog.fix.character_rank_bonus[0] += 123; },
        [](Problem &p) { p.catalog.fix.vip_bonus_bp[0] += 123; },
        [](Problem &p) { p.catalog.fix.type_link_base_bp += 123; },
        [](Problem &p) { p.catalog.fix.music_type_base_bp += 123; },
        [](Problem &p) {
            p.catalog.fix.music_tag_base_bp += 123;
            p.song.tags = {1};
        },
        [](Problem &p) { p.song.type += 1; },
        [](Problem &p) { p.settings.power.leader = false; },
        [](Problem &p) { p.settings.power.snapshot = false; },
        [](Problem &p) { p.settings.power.music_type = false; },
        [](Problem &p) { p.settings.power.music_tag = false; },
        [](Problem &p) { p.settings.power.band_item = false; },
        [](Problem &p) { p.settings.power.type_link = false; },
        [](Problem &p) { p.settings.power.vip = false; },
        [](Problem &p) { p.settings.power.rounding = "float64_floor"; },
        [](Problem &p) { p.settings.type_link_bonus_source = "member"; },
        [](Problem &p) { std::reverse(p.catalog.members.begin(), p.catalog.members.end()); },
        [](Problem &p) { std::reverse(p.catalog.snapshots.begin(), p.catalog.snapshots.end()); },
        [](Problem &p) {
            ExtraSource e;
            e.kind = "flat";
            e.rate_bp = {1, 2, 3};
            p.settings.power.extra_sources.push_back(e);
        },
    };
    for (const auto &change : changes) {
        Problem changed = base;
        change(changed);
        Engine shared(changed, &cache), fresh(changed);
        verify(shared);
        for (size_t l = 0; l < shared.members().size(); ++l)
            check(shared.power_matrix(l) == fresh.power_matrix(l));
    }
    {
        Engine held(base, &cache);
        const auto &matrix = held.power_matrix(0);
        const auto original = matrix;
        for (int i = 0; i < 520; ++i) {
            Problem changed = base;
            changed.catalog.members[0].trained[0] += 1000 + i;
            Engine transient(changed, &cache);
            check(transient.power_matrix(0)[0][0] == transient.slot_power(0, 0, 0));
        }
        check(cache.audit().at("power_matrix_evictions").as_int64() > 0);
        check(cache.audit().at("power_matrix_entries").as_int64() <= 512);
        check(matrix == original);
        Engine restored(base, &cache);
        check(restored.power_matrix(0) == original);
    }
    base.raw.set(
        "event",
        Json::parse(
            R"({"id":1,"normal":{"pt":[{"rank":7,"value":100}],"cp":[{"rank":7,"value":10}],"rewards":[]},"challenge":{"pt":[{"rank":7,"value":1000}],"rewards":[]},"normal_boosts":[{"cost":1,"pt_rate":1}],"challenge_boosts":[{"cost":200,"pt_rate":1}],"assumptions":{"score_rank":7,"score_rank_mode":"fixed","normal_boost_cost":1,"challenge_cp_cost":200}})"));
    EventSearchCache phases;
    RankOptions options;
    options.method = "exact";
    options.time_limit_s = 0;
    options.verbose = false;
    options.leaders = {base.catalog.members[0].id};
    {
        Engine normal(base), challenge(base);
        auto first = event_recommend(normal, options, &challenge, &phases);
        auto second = event_recommend(normal, options, &challenge, &phases);
        check(first.at("optimality_certified").as_bool());
        check(second.at("normal_search_audit").at("phase_cache_hit").as_bool());
        check(second.at("challenge_search_audit").at("phase_cache_hit").as_bool());
        check(first.at("normal_frontier").dump() == second.at("normal_frontier").dump());
        auto different = options;
        different.leaders = {base.catalog.members[1].id};
        auto scoped = event_recommend(normal, different, &challenge, &phases);
        check(!scoped.at("challenge_search_audit").at("phase_cache_hit").as_bool());
        check(scoped.at("challenge_frontier").items()[0].at("leader").as_int64() ==
              different.leaders[0]);
        different.method = "fast";
        different.time_limit_s = 0.01;
        auto fast = event_recommend(normal, different, &challenge, &phases);
        check(!fast.at("optimality_certified").as_bool());
        check(!fast.at("challenge_search_audit").at("phase_cache_hit").as_bool());
    }
    {
        Engine timed(base);
        EventSearchCache incomplete;
        auto limited = options;
        limited.time_limit_s = 1e-30;
        try {
            event_recommend(timed, limited, &timed, &incomplete);
        } catch (const SpecError &) {
        }
        check(incomplete.audit().at("phase_entries").as_int64() == 0);
        auto proven = event_recommend(timed, options, &timed, &incomplete);
        check(proven.at("optimality_certified").as_bool());
        check(!proven.at("normal_search_audit").at("phase_cache_hit").as_bool());
    }
    check(phases.audit().at("phase_entries").as_int64() == 0);
    Problem changed = base;
    auto event = changed.raw.at("event");
    auto assumptions = event.at("assumptions");
    assumptions.set("minimum_index", Json(1e30));
    event.set("assumptions", assumptions);
    changed.raw.set("event", event);
    Engine impossible(changed);
    bool rejected = false;
    try {
        event_recommend(impossible, options, &impossible, &phases);
    } catch (const SpecError &) {
        rejected = true;
    }
    check(rejected);
    check(phases.audit().at("phase_hits").as_int64() == 2);
    std::printf("Cache checks passed: %d assertions\n", checks);
}
