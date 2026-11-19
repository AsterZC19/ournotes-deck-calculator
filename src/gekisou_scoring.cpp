#include "gekisou_internal.hpp"

#include <numeric>
#include <sstream>

namespace deckcalc {
namespace gekisou_detail {

LuckCurve luck_curve(const GekisouTables &tables, const LuckRecord &record,
                     const std::vector<PlayFrame> &frames, size_t span_begin,
                     const std::vector<int32_t> &luck_ranges,
                     const std::vector<MissRule> &miss_rules) {
    constexpr int32_t kG = 1;
    constexpr int32_t kL = 1 << 8;
    constexpr int32_t kN = 1 << 11;
    constexpr int32_t kR = 1 << 14;
    constexpr int32_t kMin = 1 << 17;
    constexpr int32_t kMissUsed = 1 << 19;
    constexpr int32_t kPrevMiss = 1 << 20;
    constexpr int32_t kFrameLot = 1 << 21;
    constexpr int32_t kFrameMiss = 1 << 22;
    constexpr int32_t kBonus = 1 << 23;
    constexpr int32_t kSkill = 1 << 24;
    constexpr int32_t kSkillPrev = 1 << 25;
    constexpr int32_t kFlags =
        kMissUsed | kPrevMiss | kFrameLot | kFrameMiss | kBonus | kSkill | kSkillPrev;

    auto gauge_of = [](int32_t s) { return s & 0xff; };
    auto lots_of = [](int32_t s) { return (static_cast<uint32_t>(s) >> 8) & 7; };
    auto next_of = [](int32_t s) {
        return static_cast<int32_t>((static_cast<uint32_t>(s) >> 11) & 7) - 1;
    };
    auto rush_of = [](int32_t s) {
        return static_cast<int32_t>((static_cast<uint32_t>(s) >> 14) & 7);
    };
    auto minimum_of = [](int32_t s) {
        return static_cast<int32_t>((static_cast<uint32_t>(s) >> 17) & 3);
    };
    auto flags_of = [](int32_t s) { return s & kFlags; };
    auto pack = [](int32_t gauge, int32_t lots, int32_t next, int32_t rush, int32_t minimum,
                   int32_t flags) -> int32_t {
        return gauge * kG + std::min(lots, 7) * kL + (next + 1) * kN + rush * kR + minimum * kMin +
               flags;
    };

    auto merge_pairs = [](const std::vector<std::pair<double, int32_t>> &pairs) {
        std::vector<std::pair<double, int32_t>> out;
        for (const auto &[p, v] : pairs) {
            auto it = std::find_if(out.begin(), out.end(),
                                   [&](const auto &item) { return item.second == v; });
            if (it != out.end())
                it->first += p;
            else if (p > 0.0)
                out.emplace_back(p, v);
        }
        return out;
    };

    std::vector<LotItem> base_good, base_great, base_perfect, base_hold;
    for (const auto &r : tables.luck_base_points) {
        if (r.weight <= 0)
            continue;
        if (r.category == 0) {
            if (r.judgement == 3)
                base_good.emplace_back(r.weight, r.point);
            else if (r.judgement == 4)
                base_great.emplace_back(r.weight, r.point);
            else if (r.judgement == 5)
                base_perfect.emplace_back(r.weight, r.point);
        } else if (r.category == 1 && r.judgement == 5) {
            base_hold.emplace_back(r.weight, r.point);
        }
    }
    std::array<std::vector<LotItem>, 5> lots;
    for (const auto &r : tables.luck_bonus_lots) {
        if (r.weight > 0 && r.lot_type >= 0 && r.lot_type < 5)
            lots[r.lot_type].emplace_back(r.weight, r.result);
    }
    for (auto &table : lots) {
        std::sort(table.begin(), table.end(),
                  [](const LotItem &a, const LotItem &b) { return b.second < a.second; });
    }

    auto base_points = [&](int32_t op, int32_t j) -> std::vector<std::pair<double, int32_t>> {
        if (is_non_luck_note(op))
            return {{1.0, 0}};
        uint32_t u = static_cast<uint32_t>(j + 1);
        if (u < 9 && ((0x107u >> (u & 31u)) & 1u) != 0)
            return {{1.0, 0}};
        const std::vector<LotItem> *items = is_sub_note(op)                       ? &base_hold
                                            : (static_cast<uint32_t>(j - 5) < 2u) ? &base_perfect
                                            : (j == 4)                            ? &base_great
                                            : (j == 3)                            ? &base_good
                                                                                  : nullptr;
        if (!items || items->empty())
            return {{1.0, 0}};
        double total = 0.0;
        for (const auto &[w, _] : *items)
            total += w;
        std::vector<std::pair<double, int32_t>> raw;
        for (const auto &[w, v] : *items)
            raw.emplace_back(static_cast<double>(w) / total, v);
        return merge_pairs(raw);
    };

    std::unordered_map<std::string, std::vector<std::pair<double, int32_t>>> draw_cache;
    auto draw = [&](int32_t type, int32_t buff_percent,
                    int32_t minimum) -> const std::vector<std::pair<double, int32_t>> & {
        std::string key = std::to_string(type) + "," + std::to_string(buff_percent) + "," +
                          std::to_string(minimum);
        auto it = draw_cache.find(key);
        if (it != draw_cache.end())
            return it->second;
        const auto &items = lots.at(type);
        float buff = f32(f32(buff_percent) / 100.0f);
        int32_t total = 0;
        for (const auto &[w, _] : items)
            total = wrap_add(total, w);
        int32_t per = 0;
        if (minimum >= 1) {
            int32_t included = 0, excluded = 0, n = 0;
            for (const auto &[w, r] : items) {
                int32_t weight = buffed_weight(buff, w);
                if (r >= minimum) {
                    included += weight;
                    ++n;
                } else {
                    excluded += weight;
                }
            }
            per = sdiv(excluded, n);
            total = included + per * n;
        }
        std::vector<std::pair<double, int32_t>> out;
        int32_t lo = 0;
        for (const auto &[w, r] : items) {
            if (minimum >= 1 && r < minimum)
                continue;
            int32_t weight = per + buffed_weight(buff, w);
            int32_t hi = lo + weight;
            int32_t a = std::min(std::max(lo, 0), total);
            int32_t b = std::min(std::max(hi, 0), total);
            if (b > a)
                out.emplace_back(static_cast<double>(b - a) / static_cast<double>(total), r);
            lo = hi;
        }
        return draw_cache.emplace(std::move(key), merge_pairs(out)).first->second;
    };

    LuckCurve curve;
    auto gauge_max_of = [&](int32_t rush) {
        return rush > 0 ? tables.gauge_max_rush : tables.gauge_max;
    };
    auto add_gauge = [&](int32_t s, int32_t v) {
        int32_t mx = gauge_max_of(rush_of(s));
        int32_t g = wrap_add(gauge_of(s), v);
        if (mx > g)
            return pack(g, lots_of(s), next_of(s), rush_of(s), minimum_of(s), flags_of(s));
        return pack(g % mx, lots_of(s) + (g / mx), next_of(s), rush_of(s), minimum_of(s),
                    flags_of(s));
    };

    std::unordered_map<int32_t, double> dist;
    std::unordered_map<int32_t, double> spare;
    auto push = [](std::unordered_map<int32_t, double> &out, int32_t s, double p) {
        if (p > 0.0)
            out[s] += p;
    };
    auto map_dist = [&](const auto &fn) {
        spare.clear();
        for (const auto &[s, p] : dist)
            fn(s, p, spare);
        std::swap(dist, spare);
    };

    auto consume = [&](int32_t s, double p, int32_t buff,
                       std::unordered_map<int32_t, double> &out) {
        int32_t lot_cnt = lots_of(s) - 1;
        int32_t minimum = minimum_of(s);
        std::vector<std::pair<double, int32_t>> first;
        if (next_of(s) == -1) {
            first = draw(0, buff, minimum);
            minimum = 0;
        } else {
            first = {{1.0, next_of(s)}};
        }
        constexpr std::array<int32_t, 4> kLotByRush{0, 4, 3, 2};
        for (const auto &[pn, result] : first) {
            int32_t flags = flags_of(s);
            int32_t rush = rush_of(s);
            if (rush == 0 || result != 3)
                flags = (result == 3) ? (flags | kBonus) : (flags & ~kBonus);
            int32_t before = gauge_max_of(rush);
            rush = (result >= 0 && result < 3) ? 0 : (result == 3 ? std::min(rush + 1, 4) : rush);
            int32_t after = gauge_max_of(rush);
            int32_t gauge = gauge_of(s);
            int32_t lc = lot_cnt;
            if (before != after && after >= 1 && gauge > after) {
                lc += gauge / after;
                gauge %= after;
            }
            flags |= kFrameLot;
            if (result == 0)
                flags |= kFrameMiss;
            int32_t type = rush < 4 ? kLotByRush[rush] : 1;
            for (const auto &[pr, val] : draw(type, buff, minimum)) {
                push(out, pack(gauge, lc, val, rush, 0, flags), p * pn * pr);
            }
        }
    };

    std::unordered_map<int32_t, std::vector<std::pair<double, int32_t>>> base_point_cache;
    std::unordered_map<size_t, std::vector<LuckNoteRec>> notes_by_frame;
    for (const auto &n : record.notes)
        notes_by_frame[n.frame].push_back(n);

    for (int32_t range : luck_ranges) {
        ptrdiff_t first = -1;
        for (size_t k = 0; k < record.frames.size(); ++k) {
            if (record.frames[k].states[range] == kSStart) {
                first = static_cast<ptrdiff_t>(k);
                break;
            }
        }
        if (first < 0)
            continue;
        size_t last = record.frames.size() - 1;
        for (size_t k = static_cast<size_t>(first) + 1; k < record.frames.size(); ++k) {
            if (record.frames[k].states[range] == kSFinish) {
                last = k;
                break;
            }
        }
        dist.clear();
        dist[pack(0, 0, -1, 0, 0, 0)] = 1.0;
        for (size_t k = static_cast<size_t>(first); k <= last; ++k) {
            const auto &frame = record.frames[k];
            int32_t t = frames[span_begin + k].time_ms;
            int32_t state = frame.states[range];
            bool changed =
                std::find(frame.updates.begin(), frame.updates.end(), range) != frame.updates.end();
            bool finishing = changed && state == kSFinish;

            for (const auto &action : record.actions) {
                if (action.frame != k || action.range != range)
                    continue;
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    if (action.type == 11005) {
                        int32_t res = minimum_result_of(action.value);
                        push(out,
                             pack(gauge_of(s), lots_of(s), next_of(s), rush_of(s),
                                  std::max(minimum_of(s), res), flags_of(s)),
                             p * action.chance);
                    } else {
                        int32_t g = wrap_mul(gauge_max_of(rush_of(s)), action.value);
                        push(out, add_gauge(s, floor_to_i32(f32(f32(g) / 10000.0f))),
                             p * action.chance);
                    }
                    push(out, s, p * (1.0 - action.chance));
                });
            }

            bool gate_open = changed || (frame.playing == range);
            std::vector<LuckNoteRec> luck_notes;
            if (auto it = notes_by_frame.find(k); it != notes_by_frame.end()) {
                for (const auto &n : it->second)
                    if (n.range == range)
                        luck_notes.push_back(n);
            }
            const auto &judged = frames[span_begin + k].judged;
            if (luck_notes.empty() && judged.empty()) {
                map_dist([&](int32_t s0, double p0, std::unordered_map<int32_t, double> &out) {
                    std::vector<std::pair<int32_t, double>> branches{{s0, p0}};
                    if (!miss_rules.empty() && gate_open && (s0 & kPrevMiss) && !(s0 & kMissUsed)) {
                        for (const auto &rule : miss_rules) {
                            std::vector<std::pair<int32_t, double>> nb;
                            for (const auto &[b, q] : branches) {
                                int32_t g = wrap_mul(gauge_max_of(rush_of(b)), rule.value);
                                nb.emplace_back(add_gauge(b, floor_to_i32(f32(f32(g) / 10000.0f))),
                                                q * rule.chance);
                                nb.emplace_back(b, q * (1.0 - rule.chance));
                            }
                            branches = std::move(nb);
                        }
                        for (auto &br : branches)
                            br.first |= kMissUsed;
                    }
                    auto tail = [&](int32_t s, double p) {
                        if (finishing)
                            s &= ~kBonus;
                        int32_t next = s & ~(kPrevMiss | kFrameMiss | kFrameLot);
                        if (s & kFrameMiss)
                            next |= kPrevMiss;
                        if (changed && state == kSComplete)
                            next &= ~kMissUsed;
                        push(out, next, p);
                    };
                    for (const auto &[b, q] : branches) {
                        bool sk = (b & kSkill) != 0;
                        if (frame.playing == range)
                            sk = (rush_of(b) != 0);
                        if (changed && (state == kSComplete || state == kSFinish))
                            sk = false;
                        int32_t s = b & ~(kSkill | kSkillPrev);
                        if (b & kSkill)
                            s |= kSkillPrev;
                        if (sk)
                            s |= kSkill;
                        if (state == kSPlaying && lots_of(s) > 0 && !(s & kFrameLot)) {
                            std::unordered_map<int32_t, double> tmp;
                            consume(s, q, 0, tmp);
                            for (const auto &[c, cq] : tmp)
                                tail(c, cq);
                        } else {
                            tail(s, q);
                        }
                    }
                });
                continue;
            }

            if (!miss_rules.empty() && gate_open) {
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    if (!(s & kPrevMiss) || (s & kMissUsed)) {
                        push(out, s, p);
                        return;
                    }
                    std::vector<std::pair<int32_t, double>> branches{{s, p}};
                    for (const auto &rule : miss_rules) {
                        std::vector<std::pair<int32_t, double>> nb;
                        for (const auto &[b, q] : branches) {
                            int32_t g = wrap_mul(gauge_max_of(rush_of(b)), rule.value);
                            nb.emplace_back(add_gauge(b, floor_to_i32(f32(f32(g) / 10000.0f))),
                                            q * rule.chance);
                            nb.emplace_back(b, q * (1.0 - rule.chance));
                        }
                        branches = std::move(nb);
                    }
                    for (const auto &[b, q] : branches)
                        push(out, b | kMissUsed, q);
                });
            }

            map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                bool sk = (s & kSkill) != 0;
                if (frame.playing == range)
                    sk = (rush_of(s) != 0);
                if (changed && (state == kSComplete || state == kSFinish))
                    sk = false;
                int32_t next = s & ~(kSkill | kSkillPrev);
                if (s & kSkill)
                    next |= kSkillPrev;
                if (sk)
                    next |= kSkill;
                push(out, next, p);
            });

            auto observe = [&](int32_t id, bool at_frame_time) {
                double r = 0.0, sk = 0.0, both = 0.0;
                int32_t skill_bit = at_frame_time ? kSkill : kSkillPrev;
                for (const auto &[s, p] : dist) {
                    bool bonus = (s & kBonus) != 0;
                    bool skill = (s & skill_bit) != 0;
                    if (bonus)
                        r += p;
                    if (skill)
                        sk += p;
                    if (bonus && skill)
                        both += p;
                }
                curve.rush[id] += r;
                curve.skill[id] += sk;
                curve.both[id] += both;
            };

            auto luck_note = [&](const LuckNoteRec &n) {
                int32_t key = n.op * 1000 + n.judgement * 100 +
                              static_cast<int32_t>(std::llround(n.speed * 10.0f));
                auto it = base_point_cache.find(key);
                if (it == base_point_cache.end()) {
                    std::vector<std::pair<double, int32_t>> pts;
                    for (const auto &[p, v] : base_points(n.op, n.judgement)) {
                        pts.emplace_back(p, floor_to_i32(f32(f32(n.speed + 1.0f) * f32(v))));
                    }
                    it = base_point_cache.emplace(key, std::move(pts)).first;
                }
                const auto &points = it->second;
                bool consumes = (state <= kSEnd);
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    for (const auto &[pb, add] : points) {
                        int32_t next = add_gauge(s, add);
                        if (consumes && lots_of(next) > 0)
                            consume(next, p * pb, n.buff, out);
                        else
                            push(out, next, p * pb);
                    }
                });
            };

            std::vector<int32_t> times;
            for (const auto &n : luck_notes)
                times.push_back(n.time_ms);
            for (const auto &n : judged)
                times.push_back(n.time_ms);
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());

            for (int32_t time : times) {
                if (time >= t)
                    continue;
                for (const auto &n : luck_notes)
                    if (n.time_ms == time)
                        luck_note(n);
                for (const auto &n : judged)
                    if (n.time_ms == time)
                        observe(n.note_id, false);
            }
            for (const auto &n : luck_notes)
                if (n.time_ms >= t)
                    luck_note(n);
            if (state == kSPlaying) {
                int32_t buff = 0;
                bool found_b = false;
                for (const auto &n : luck_notes) {
                    if (n.time_ms >= t) {
                        buff = n.buff;
                        found_b = true;
                        break;
                    }
                }
                if (!found_b && !luck_notes.empty())
                    buff = luck_notes.back().buff;
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    if (lots_of(s) > 0 && !(s & kFrameLot))
                        consume(s, p, buff, out);
                    else
                        push(out, s, p);
                });
            }
            if (finishing) {
                map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                    push(out, s & ~kBonus, p);
                });
            }
            for (const auto &n : judged)
                if (n.time_ms >= t)
                    observe(n.note_id, true);
            map_dist([&](int32_t s, double p, std::unordered_map<int32_t, double> &out) {
                int32_t next = s & ~(kPrevMiss | kFrameMiss | kFrameLot);
                if (s & kFrameMiss)
                    next |= kPrevMiss;
                if (changed && state == kSComplete)
                    next &= ~kMissUsed;
                push(out, next, p);
            });
        }
    }
    return curve;
}
// 谱面判定流、槽位技能解析、120 顺序对称压缩与激奏评分
inline bool is_judgement_note(int32_t op) {
    switch (op) {
    case 0:
    case 80:
    case 82:
    case 100:
    case 103:
    case 121:
    case 122:
    case 123:
        return false;
    default:
        return true;
    }
}

FullChart build_full_chart(const EngineMaster &master, const ChartSource &source, int32_t level) {
    if (source.enumeration.empty())
        throw std::runtime_error("谱面缺少音符枚举列表");
    FullChart fc;
    fc.level = level;
    int32_t sum = 0;
    int32_t last_t = 0;
    for (size_t i = 0; i < source.enumeration.size(); ++i) {
        const auto &n = source.enumeration[i];
        fc.notes.push_back({static_cast<int32_t>(i + 1), n.time_ms, n.op, n.judgement_type});
        if (auto it = master.live.note_percent.find(n.op); it != master.live.note_percent.end())
            sum = wrap_add(sum, it->second);
        last_t = std::max(last_t, n.time_ms);
    }
    for (size_t i = 0; i < source.skill_times_ms.size(); ++i)
        fc.events.emplace_back(static_cast<int32_t>(i), source.skill_times_ms[i]);
    fc.fevers = source.fever_ms;
    fc.converted_count = ceil_to_i32(f32(f32(sum) / 100.0f));
    fc.last_timing_ms = last_t;
    return fc;
}

LivePlay judgement_stream(const EngineMaster &master, const FullChart &chart,
                          const GekisouSetup *gekisou, const Accuracy &accuracy) {
    std::vector<LiveNote> judged;
    for (const auto &n : chart.notes) {
        if (is_judgement_note(n.op))
            judged.push_back(n);
    }
    std::sort(judged.begin(), judged.end(), [](const LiveNote &a, const LiveNote &b) {
        return (a.time_ms != b.time_ms) ? (a.time_ms < b.time_ms) : (a.id < b.id);
    });
    int32_t last = 0;
    for (const auto &n : chart.notes)
        last = std::max(last, n.time_ms);
    for (const auto &[_, t] : chart.events)
        last = std::max(last, t);
    if (gekisou) {
        for (const auto &[_, e] : gekisou->fevers)
            last = std::max(last, e);
    }
    std::vector<int32_t> times;
    for (int64_t k = 0;; ++k) {
        int32_t t = static_cast<int32_t>((k * 1000) / 60);
        if (t > last + 2000)
            break;
        times.push_back(t);
    }
    std::vector<std::pair<size_t, size_t>> windows;
    if (gekisou) {
        for (size_t i = 0; i < gekisou->fevers.size(); ++i) {
            if (gekisou->missions.at(i) != 3)
                continue;
            auto [start, end] = gekisou->fevers[i];
            auto it1 = std::lower_bound(times.begin(), times.end(), start);
            if (it1 == times.end())
                continue;
            size_t first = static_cast<size_t>(it1 - times.begin());
            auto it2 = std::lower_bound(it1 + 1, times.end(), end);
            size_t second =
                (it2 == times.end()) ? times.size() : static_cast<size_t>(it2 - times.begin());
            windows.emplace_back(first, second);
        }
    }
    auto in_window = [&](size_t frame) {
        for (const auto &[a, b] : windows)
            if (a <= frame && frame < b)
                return true;
        return false;
    };
    struct Row {
        size_t frame;
        LiveNote note;
        int32_t best;
    };
    std::vector<Row> rows;
    rows.reserve(judged.size());
    for (const auto &n : judged) {
        auto it = std::lower_bound(times.begin(), times.end(), n.time_ms);
        size_t frame = static_cast<size_t>(it - times.begin());
        bool just = (gekisou != nullptr && master.live.just_types.count(n.judgement_type) &&
                     in_window(frame));
        rows.push_back({frame, n, just ? 6 : 5});
    }
    auto choose_evenly = [](size_t n, int64_t count, const auto &select) {
        int64_t rem = 0;
        int64_t nn = static_cast<int64_t>(n);
        for (size_t i = 0; i < n; ++i) {
            rem += count;
            if (rem >= nn) {
                rem -= nn;
                select(i);
            }
        }
    };
    size_t total = rows.size();
    std::vector<int32_t> grade(total, 5);
    choose_evenly(total,
                  static_cast<int64_t>(std::llround(static_cast<double>(total) * accuracy.great)),
                  [&](size_t i) { grade[i] = 4; });
    std::vector<size_t> eligible;
    for (size_t i = 0; i < total; ++i) {
        if (grade[i] != 4 && rows[i].best == 6)
            eligible.push_back(i);
    }
    choose_evenly(
        eligible.size(),
        static_cast<int64_t>(std::llround(static_cast<double>(eligible.size()) * accuracy.just)),
        [&](size_t k) { grade[eligible[k]] = 6; });
    if (accuracy.miss_every > 0) {
        for (size_t i = static_cast<size_t>(accuracy.miss_every - 1); i < total;
             i += static_cast<size_t>(accuracy.miss_every)) {
            grade[i] = 1;
        }
    }
    LivePlay out;
    out.frames.resize(times.size());
    for (size_t i = 0; i < times.size(); ++i)
        out.frames[i].time_ms = times[i];
    for (size_t i = 0; i < rows.size(); ++i) {
        out.frames[rows[i].frame].judged.push_back(
            {rows[i].note.id, grade[i], rows[i].note.time_ms});
    }
    out.dt.assign(times.size(), f32(1.0 / 60.0));
    for (size_t idx : eligible)
        out.just_eligible_ids.push_back(rows[idx].note.id);
    return out;
}

GekisoChart build_gekiso_chart(const EngineMaster &master, const ChartSource &source, int32_t level,
                               const std::vector<int32_t> &missions, const Accuracy &accuracy,
                               int32_t rank) {
    GekisoChart gc;
    gc.full = build_full_chart(master, source, level);
    gc.setup = {gc.full.fevers, missions};
    gc.play = judgement_stream(master, gc.full, &gc.setup, accuracy);
    gc.accuracy = accuracy;

    std::unordered_map<int32_t, int32_t> judgement_of;
    for (const auto &frame : gc.play.frames) {
        for (const auto &j : frame.judged)
            judgement_of[j.note_id] = j.judgement;
    }
    for (const auto &n : gc.full.notes) {
        if (is_judgement_note(n.op) && master.live.note_percent.count(n.op))
            gc.notes.push_back(n);
    }
    std::sort(gc.notes.begin(), gc.notes.end(), [](const LiveNote &a, const LiveNote &b) {
        return (a.time_ms != b.time_ms) ? (a.time_ms < b.time_ms) : (a.id < b.id);
    });
    size_t n = gc.notes.size();
    gc.times.resize(n);
    gc.note_frac.resize(n);
    gc.judgements.resize(n);
    gc.note_id_to_index.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        gc.note_id_to_index[gc.notes[i].id] = i;
        gc.times[i] = gc.notes[i].time_ms;
        gc.note_frac[i] =
            f32(static_cast<double>(master.live.note_percent.at(gc.notes[i].op)) / 100.0);
        auto it = judgement_of.find(gc.notes[i].id);
        gc.judgements[i] = static_cast<int8_t>(it != judgement_of.end() ? it->second : 5);
    }

    auto factors =
        ranking_factors(master.live.gekiso, mission_pattern(missions.size() > 0 ? missions[0] : 0,
                                                            missions.size() > 1 ? missions[1] : 0,
                                                            missions.size() > 2 ? missions[2] : 0));
    size_t num_ranges = std::min<size_t>(3, gc.full.fevers.size());
    for (size_t r = 0; r < num_ranges; ++r) {
        gc.range_percent.push_back(factors[r][rank - 1]);
        if (r < missions.size() && missions[r] == kMLuck)
            gc.luck = true;
    }
    gc.range_of.assign(n, -1);
    for (size_t i = 0; i < n; ++i) {
        int32_t frame = get_frame(gc.notes[i].time_ms);
        for (size_t r = 0; r < num_ranges; ++r) {
            auto [s, e] = gc.full.fevers[r];
            if (get_frame(s) < frame && frame <= get_frame(e) && gc.range_of[i] == -1)
                gc.range_of[i] = static_cast<int8_t>(r);
        }
    }
    gc.combo_factor.resize(n);
    {
        const auto *table = master.live.combo.count(0) ? &master.live.combo.at(0) : nullptr;
        int32_t before = 0, pending = 0, last = -1;
        for (size_t i = 0; i < n; ++i) {
            if (gc.times[i] != last) {
                before = pending;
                last = gc.times[i];
            }
            float val = 0.0f;
            if (table) {
                for (const auto &[req, cum] : *table) {
                    if (req > before)
                        break;
                    val = cum;
                }
            }
            gc.combo_factor[i] = f32(std::min(val, 1.0f) + 1.0f);
            pending = (gc.judgements[i] <= 2) ? 0 : (pending + 1);
        }
    }
    for (int32_t type = 1; type <= 6; ++type) {
        int32_t jp = master.live.judge_percent.count(type) ? master.live.judge_percent.at(type) : 0;
        gc.judge_frac[7 - type] = f32(static_cast<double>(jp) / 100.0);
    }
    gc.difficulty_factor = f32(f32(f32(wrap_sub(level, 5)) * f32(0.005)) + 1.0f);
    gc.music_length_ms = gc.full.last_timing_ms + 1000;

    if (num_ranges > 0) {
        int32_t first_start = gc.full.fevers[0].first;
        int32_t last_end = gc.full.fevers[0].second;
        for (size_t r = 1; r < num_ranges; ++r) {
            first_start = std::min(first_start, gc.full.fevers[r].first);
            last_end = std::max(last_end, gc.full.fevers[r].second);
        }
        size_t s0 = 0;
        for (size_t i = 0; i < gc.play.frames.size(); ++i) {
            if (gc.play.frames[i].time_ms >= first_start - 4500) {
                s0 = (i > 0) ? (i - 1) : 0;
                break;
            }
        }
        size_t s1 = gc.play.frames.size();
        for (size_t i = 0; i < gc.play.frames.size(); ++i) {
            if (gc.play.frames[i].time_ms >= last_end + master.live.after_ms + 1200) {
                s1 = i;
                break;
            }
        }
        gc.span = {s0, s1};
    }
    return gc;
}

GekisoContext expected_context(const EngineMaster &master, const GekisoChart &chart,
                               const std::vector<Performer> &deck,
                               std::unordered_map<std::string, LuckCurve> &curve_cache) {
    size_t n = chart.notes.size();
    const auto &index = chart.note_id_to_index;
    std::vector<int8_t> judgement = chart.judgements;
    std::vector<float> gk(n, 1.0f);
    std::vector<float> note_up(n, 1.0f);
    std::vector<Performer> reduced;
    reduced.reserve(deck.size());
    for (const auto &p : deck)
        reduced.push_back(gekiso_performer(p));

    LuckRecord record;
    simulate_live_span(
        master, reduced, chart.full.notes, chart.full.events,
        {0, 1.0, chart.full.level, chart.full.converted_count, chart.music_length_ms, 1.0f},
        &chart.setup, 0, chart.play.frames, chart.play.dt, chart.span.first, chart.span.second,
        &record, [&](int32_t id, int32_t score_type, float note, float factor, float, float) {
            auto it = index.find(id);
            if (it == index.end())
                return;
            size_t i = it->second;
            judgement[i] = static_cast<int8_t>(7 - score_type);
            gk[i] = factor;
            note_up[i] = note;
        });
    auto [rush, misses] = compute_luck_readers(master, reduced);
    std::vector<int32_t> luck_ranges;
    size_t num_r = std::min<size_t>(3, chart.full.fevers.size());
    for (size_t r = 0; r < num_r && r < chart.setup.missions.size(); ++r) {
        if (chart.setup.missions[r] == kMLuck)
            luck_ranges.push_back(static_cast<int32_t>(r));
    }

    std::ostringstream sig;
    for (const auto &a : record.actions)
        sig << a.frame << ":" << a.range << ":" << a.type << ":" << a.value << ":" << a.limit << ":"
            << a.chance << ";";
    sig << "|";
    for (const auto &m : misses)
        sig << m.value << ":" << m.chance << ";";
    sig << "|";
    for (const auto &nt : record.notes)
        sig << nt.frame << ":" << nt.judgement << ":" << nt.speed << ":" << nt.buff << ";";
    sig << "|";
    for (const auto &fr : record.frames) {
        for (int32_t s : fr.states)
            sig << s;
        sig << fr.playing << ",";
    }
    std::string signature = sig.str();
    auto cit = curve_cache.find(signature);
    if (cit == curve_cache.end()) {
        cit = curve_cache
                  .emplace(std::move(signature),
                           luck_curve(master.live.gekiso, record, chart.play.frames,
                                      chart.span.first, luck_ranges, misses))
                  .first;
    }
    const auto &curve = cit->second;
    double bonus =
        static_cast<double>(std::min(100 + master.live.gekiso.rush_percent, 200)) / 100.0 - 1.0;
    std::vector<double> up(n, 0.0);
    std::vector<double> luck(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        int32_t id = chart.notes[i].id;
        double pr = curve.rush.count(id) ? curve.rush.at(id) : 0.0;
        double ps = curve.skill.count(id) ? curve.skill.at(id) : 0.0;
        double both = curve.both.count(id) ? curve.both.at(id) : 0.0;
        luck[i] = 1.0 + bonus * pr;
        up[i] = static_cast<double>(note_up[i]) * luck[i] +
                static_cast<double>(rush) * (ps + bonus * both);
    }
    return {std::move(judgement), std::move(gk), std::move(up), std::move(luck), 0};
}

GekisoContext gekiso_context(const EngineMaster &master, const GekisoChart &chart,
                             const std::vector<Performer> &deck, const std::vector<int32_t> &seeds,
                             std::unordered_map<std::string, LuckCurve> &curve_cache) {
    if (chart.luck && seeds.empty())
        return expected_context(master, chart, deck, curve_cache);
    size_t n = chart.notes.size();
    const auto &index = chart.note_id_to_index;
    std::vector<int8_t> judgement = chart.judgements;
    std::vector<float> gk(n, 1.0f);
    std::vector<double> up(n, 0.0);
    std::vector<double> luck(n, 0.0);
    std::vector<int32_t> runs =
        chart.luck ? seeds : std::vector<int32_t>{seeds.empty() ? 0 : seeds[0]};
    std::vector<Performer> reduced;
    reduced.reserve(deck.size());
    for (const auto &p : deck)
        reduced.push_back(gekiso_performer(p));

    for (int32_t seed : runs) {
        std::vector<float> note_up(n, 1.0f);
        std::vector<float> note_luck(n, 1.0f);
        simulate_live_span(
            master, reduced, chart.full.notes, chart.full.events,
            {0, 1.0, chart.full.level, chart.full.converted_count, chart.music_length_ms, 1.0f},
            &chart.setup, seed, chart.play.frames, chart.play.dt, chart.span.first,
            chart.span.second, nullptr,
            [&](int32_t id, int32_t score_type, float note, float factor, float luck_factor,
                float) {
                auto it = index.find(id);
                if (it == index.end())
                    return;
                size_t i = it->second;
                judgement[i] = static_cast<int8_t>(7 - score_type);
                gk[i] = factor;
                note_up[i] = note;
                note_luck[i] = luck_factor;
            });
        for (size_t i = 0; i < n; ++i) {
            up[i] += static_cast<double>(note_luck[i]) * static_cast<double>(note_up[i]);
            luck[i] += static_cast<double>(note_luck[i]);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        up[i] /= static_cast<double>(runs.size());
        luck[i] /= static_cast<double>(runs.size());
    }
    return {std::move(judgement), std::move(gk), std::move(up), std::move(luck), runs.size()};
}

SlotSkill resolve_slot_skill(const EngineMaster &master, const MemberState &member,
                             const SnapState *snap, std::vector<std::string> *unsupported) {
    auto cond_value = [&](const ConditionRow &row) -> int32_t {
        // returns: 1 = true, 0 = false, 2 = "life", -1 = null
        switch (row.type) {
        case 5000: {
            bool m = matches_targets(master, member, row.target_ids);
            return (row.positive ? m : !m) ? 1 : 0;
        }
        case 4010:
        case 5020:
            return row.positive ? 1 : -1;
        case 2001:
            return 2;
        default:
            return -1;
        }
    };
    struct GroupRes {
        bool holds = false;
        const ConditionRow *life = nullptr;
    };
    auto group_holds = [&](int32_t group) -> std::optional<GroupRes> {
        if (group <= 0)
            return GroupRes{true, nullptr};
        auto it = master.condition_sets.find(group);
        if (it == master.condition_sets.end() || it->second.empty())
            return std::nullopt;
        bool any = false;
        const ConditionRow *life = nullptr;
        for (const auto &set : it->second) {
            bool all = true;
            for (int32_t id : set) {
                auto cit = master.conditions.find(id);
                if (cit == master.conditions.end())
                    return std::nullopt;
                int32_t v = cond_value(cit->second);
                if (v == -1)
                    return std::nullopt;
                if (v == 2)
                    life = &cit->second;
                else if (v == 0)
                    all = false;
            }
            if (all)
                any = true;
        }
        return GroupRes{any, life};
    };
    auto judgements_of = [&](const std::vector<int32_t> &ids) {
        std::vector<int32_t> out;
        for (int32_t id : ids) {
            auto it = master.targets.find(id);
            int32_t j = (it != master.targets.end()) ? it->second.judgement : -1;
            if (j >= 3 && j <= 6)
                out.push_back(j);
        }
        return out;
    };

    SlotSkill out;
    if (auto it = master.live_skills.find(member.card.live_skill_id);
        it != master.live_skills.end()) {
        for (const auto &row : it->second) {
            if (row.level != member.growth.live_skill_level)
                continue;
            if (row.type != 2000 && row.type != 2004) {
                if (unsupported)
                    unsupported->push_back("live:" + std::to_string(member.card.live_skill_id) +
                                           ":" + std::to_string(row.type));
                continue;
            }
            auto condition = group_holds(row.condition_group);
            if (!condition.has_value()) {
                if (unsupported)
                    unsupported->push_back("live-condition:" + std::to_string(row.condition_group));
                continue;
            }
            if (!condition->holds)
                continue;
            LifeGate gate;
            if (condition->life) {
                gate = {1, condition->life->values.empty() ? 0 : condition->life->values[0],
                        condition->life->positive};
            } else {
                gate = {0, 0, true};
            }
            out.effects.push_back({
                row.type,
                row.type == 2000 ? score_delta(row.value) : judgement_delta(row.value),
                f32(row.seconds),
                gate,
                row.type == 2004 ? judgements_of(row.target_ids) : std::vector<int32_t>{},
            });
        }
    }
    if (snap) {
        for (size_t slot = 0; slot < 2; ++slot) {
            int32_t skill_id = snap->card.support_skill_ids[slot];
            if (!skill_id)
                continue;
            int32_t lv = snap->skill_levels[slot] > 0 ? snap->skill_levels[slot] : 1;
            auto it = master.support_skills.find(skill_id);
            if (it == master.support_skills.end())
                continue;
            for (const auto &row : it->second) {
                if (row.level != lv)
                    continue;
                auto enabled = group_holds(row.condition_group);
                auto trigger = group_holds(row.trigger_group);
                if (!enabled.has_value() || !trigger.has_value()) {
                    if (unsupported)
                        unsupported->push_back("support-condition:" + std::to_string(skill_id));
                    continue;
                }
                if (!enabled->holds)
                    continue;
                if (row.type == 15000) {
                    out.extension_ms = f32(out.extension_ms + f32(row.value));
                } else if (row.type == 3001) {
                    out.recovery += row.value;
                } else if (row.type == 12006) {
                    out.convert = SlotConvert{judgements_of(row.target_ids),
                                              row.limit_count > 0 ? row.limit_count : kI32Max};
                } else if (row.type != 3003) {
                    if (unsupported)
                        unsupported->push_back("support:" + std::to_string(skill_id) + ":" +
                                               std::to_string(row.type));
                }
            }
        }
    }
    return out;
}

const std::vector<std::array<int32_t, 5>> &all_orders() {
    static const std::vector<std::array<int32_t, 5>> kOrders = []() {
        std::vector<std::array<int32_t, 5>> out;
        std::array<int32_t, 5> p{0, 1, 2, 3, 4};
        do {
            out.push_back(p);
        } while (std::next_permutation(p.begin(), p.end()));
        return out;
    }();
    return kOrders;
}

bool equal_slot_skill(const SlotSkill &a, const SlotSkill &b) {
    if (a.extension_ms != b.extension_ms || a.recovery != b.recovery)
        return false;
    if (a.convert.has_value() != b.convert.has_value())
        return false;
    if (a.convert.has_value()) {
        if (a.convert->limit != b.convert->limit || a.convert->judgements != b.convert->judgements)
            return false;
    }
    if (a.effects.size() != b.effects.size())
        return false;
    for (size_t i = 0; i < a.effects.size(); ++i) {
        const auto &ea = a.effects[i];
        const auto &eb = b.effects[i];
        if (ea.type != eb.type || ea.delta != eb.delta || ea.seconds != eb.seconds ||
            ea.gate.kind != eb.gate.kind || ea.judgements != eb.judgements)
            return false;
        if (ea.gate.kind == 1 &&
            (ea.gate.value != eb.gate.value || ea.gate.positive != eb.gate.positive))
            return false;
    }
    return true;
}

OrderScores score_orders_gekiso(const EngineMaster &master, const GekisoChart &chart,
                                const GekisoContext &ctx, double power,
                                const std::vector<SlotSkill> &slots) {
    size_t n = chart.notes.size();
    float t =
        f32(f32(master.live.adjustment * static_cast<double>(power)) * chart.difficulty_factor);
    int32_t count = chart.full.converted_count;
    float onus = master.live.life_onus;
    const auto &damage = master.live.damage;

    std::vector<int32_t> event_times;
    for (const auto &[_, tm] : chart.full.events)
        event_times.push_back(tm);
    std::vector<size_t> firing;
    for (size_t i = 0; i < std::min<size_t>(5, event_times.size()); ++i)
        firing.push_back(i);
    std::sort(firing.begin(), firing.end(), [&](size_t a, size_t b) {
        return (event_times[a] != event_times[b]) ? (event_times[a] < event_times[b]) : (a < b);
    });

    std::vector<float> note_a(n);
    std::vector<float> combo_factor(n);
    for (size_t i = 0; i < n; ++i) {
        note_a[i] = f32(chart.note_frac[i] * t);
        combo_factor[i] = f32(ctx.gk[i] * chart.combo_factor[i]);
    }
    bool deterministic = (ctx.seeds == 1);
    size_t ranges = chart.range_percent.size();

    std::array<int32_t, 5> groups{};
    int32_t classes = 0;
    for (size_t slot = 0; slot < 5; ++slot) {
        size_t prev = 0;
        while (prev < slot && !equal_slot_skill(slots[slot], slots[prev]))
            ++prev;
        groups[slot] = (prev == slot) ? classes++ : groups[prev];
    }

    const auto &orders = all_orders();
    std::vector<size_t> reps;
    std::array<size_t, 120> sources{};
    if (classes == 5) {
        reps.resize(120);
        for (size_t i = 0; i < 120; ++i) {
            reps[i] = i;
            sources[i] = i;
        }
    } else {
        std::unordered_map<int32_t, size_t> first;
        for (size_t idx = 0; idx < 120; ++idx) {
            int32_t seq = 0;
            for (int32_t s : orders[idx])
                seq = seq * 5 + groups[s];
            auto it = first.find(seq);
            if (it == first.end()) {
                first[seq] = idx;
                reps.push_back(idx);
                sources[idx] = idx;
            } else {
                sources[idx] = it->second;
            }
        }
    }

    OrderScores out;
    out.power = power;
    struct ActiveEff {
        int32_t end;
        int32_t channel;
        int32_t delta;
    };
    struct ActiveConv {
        int32_t end;
        std::vector<int32_t> judgements;
        int32_t left;
    };

    // Fast path when life stays constant (>0), no judgement conversion occurs, each slot adds at
    // most one active effect per channel (preserving exact float32 add/sub parity with slow path),
    // and skill windows are pairwise disjoint.
    bool can_use_window_deltas = (master.live.life_base > 0) && (ranges <= 3);
    if (can_use_window_deltas) {
        for (size_t slot = 0; slot < 5; ++slot) {
            if (slots[slot].convert.has_value() || slots[slot].recovery < 0) {
                can_use_window_deltas = false;
                break;
            }
        }
    }
    if (can_use_window_deltas) {
        for (size_t i = 0; i < n; ++i) {
            if (damage[ctx.judgement[i]] != 0) {
                can_use_window_deltas = false;
                break;
            }
        }
    }
    int32_t max_window_all = 0;
    std::array<size_t, 5> class_rep_slot{};
    if (can_use_window_deltas) {
        for (int32_t cls = 0; cls < classes; ++cls) {
            for (size_t s = 0; s < 5; ++s) {
                if (groups[s] == cls) {
                    class_rep_slot[static_cast<size_t>(cls)] = s;
                    break;
                }
            }
        }
        for (int32_t cls = 0; cls < classes && can_use_window_deltas; ++cls) {
            const auto &slot = slots[class_rep_slot[static_cast<size_t>(cls)]];
            int factor_count = 0;
            std::array<int, 7> channel_count{};
            for (const auto &eff : slot.effects) {
                if (eff.gate.kind == 1 &&
                    ((master.live.life_base >= eff.gate.value) != eff.gate.positive))
                    continue;
                int32_t w = window_ms(eff.seconds, slot.extension_ms);
                if (w < 0) {
                    can_use_window_deltas = false;
                    break;
                }
                if (eff.type == 2000) {
                    if (++factor_count > 1) {
                        can_use_window_deltas = false;
                        break;
                    }
                } else {
                    for (int32_t which : eff.judgements) {
                        if (++channel_count[static_cast<size_t>(which)] > 1) {
                            can_use_window_deltas = false;
                            break;
                        }
                    }
                    if (!can_use_window_deltas)
                        break;
                }
                max_window_all = std::max(max_window_all, w);
            }
        }
        for (size_t c = 0; c + 1 < firing.size() && can_use_window_deltas; ++c) {
            if (static_cast<int64_t>(event_times[firing[c]]) + max_window_all >
                static_cast<int64_t>(event_times[firing[c + 1]])) {
                can_use_window_deltas = false;
            }
        }
        if (can_use_window_deltas && !firing.empty() &&
            static_cast<int64_t>(event_times[firing.back()]) + max_window_all > kI32Max) {
            can_use_window_deltas = false;
        }
    }

    if (can_use_window_deltas) {
        const float count_f = static_cast<float>(count);
        const double count_d = static_cast<double>(count);
        std::vector<float> note_c(n);
        std::vector<double> base_note(n);
        double base_total = 0.0;
        std::array<double, 3> base_range{0.0, 0.0, 0.0};
        auto score_note = [&](size_t i, float live) -> double {
            if (deterministic) {
                float up_f = f32(f32(ctx.up[i] / ctx.luck[i]) + live);
                return std::floor(f32(f32(f32(ctx.luck[i]) * f32(note_c[i] * up_f)) / count_f));
            }
            return std::floor((static_cast<double>(note_c[i]) *
                               (ctx.up[i] + ctx.luck[i] * static_cast<double>(live))) /
                              count_d);
        };
        for (size_t i = 0; i < n; ++i) {
            const int32_t j = ctx.judgement[i];
            note_c[i] = f32(f32(chart.judge_frac[j] * note_a[i]) * combo_factor[i]);
            const double x = score_note(i, 0.0f);
            base_note[i] = x;
            base_total += x;
            int8_t r = chart.range_of[i];
            if (r >= 0)
                base_range[static_cast<size_t>(r)] += x;
        }

        const size_t num_ev = firing.size();
        std::array<std::array<double, 5>, 5> delta_total{};
        std::array<std::array<std::array<double, 3>, 5>, 5> delta_range{};
        std::vector<ActiveEff> active;
        active.reserve(8);

        for (size_t c = 0; c < num_ev; ++c) {
            const int32_t start = event_times[firing[c]];
            const size_t lo = static_cast<size_t>(
                std::lower_bound(chart.times.begin(), chart.times.end(), start) -
                chart.times.begin());
            for (int32_t cls = 0; cls < classes; ++cls) {
                const auto &slot = slots[class_rep_slot[static_cast<size_t>(cls)]];
                active.clear();
                float factor = 0.0f;
                std::array<float, 7> channel{};
                int32_t max_end = start;
                for (const auto &eff : slot.effects) {
                    if (eff.gate.kind == 1 &&
                        ((master.live.life_base >= eff.gate.value) != eff.gate.positive))
                        continue;
                    int32_t end = start + window_ms(eff.seconds, slot.extension_ms);
                    max_end = std::max(max_end, end);
                    float diff = f32(static_cast<double>(eff.delta) / 100000.0);
                    if (eff.type == 2000) {
                        factor = f32(factor + diff);
                        active.push_back({end, 0, eff.delta});
                    } else {
                        for (int32_t which : eff.judgements) {
                            channel[which] = f32(channel[which] + diff);
                            active.push_back({end, which, eff.delta});
                        }
                    }
                }
                if (active.empty() || max_end <= start)
                    continue;
                const size_t hi = static_cast<size_t>(
                    std::lower_bound(chart.times.begin() + static_cast<ptrdiff_t>(lo),
                                     chart.times.end(), max_end) -
                    chart.times.begin());
                double dtot = 0.0;
                std::array<double, 3> dr{0.0, 0.0, 0.0};
                for (size_t i = lo; i < hi; ++i) {
                    const int32_t time = chart.times[i];
                    for (ptrdiff_t a = static_cast<ptrdiff_t>(active.size()) - 1; a >= 0; --a) {
                        if (active[static_cast<size_t>(a)].end <= time) {
                            auto [_, which, delta] = active[static_cast<size_t>(a)];
                            float diff = f32(static_cast<double>(delta) / 100000.0);
                            if (which == 0)
                                factor = f32(factor - diff);
                            else
                                channel[which] = f32(channel[which] - diff);
                            active.erase(active.begin() + a);
                        }
                    }
                    const float live = f32(factor + channel[ctx.judgement[i]]);
                    if (live == 0.0f)
                        continue;
                    const double dx = score_note(i, live) - base_note[i];
                    dtot += dx;
                    const int8_t r = chart.range_of[i];
                    if (r >= 0)
                        dr[static_cast<size_t>(r)] += dx;
                }
                delta_total[static_cast<size_t>(cls)][c] = dtot;
                delta_range[static_cast<size_t>(cls)][c] = dr;
            }
        }

        for (size_t order_idx : reps) {
            const auto &order = orders[order_idx];
            double score = base_total;
            std::array<double, 3> rs = base_range;
            for (size_t c = 0; c < num_ev; ++c) {
                const size_t cls = static_cast<size_t>(groups[order[firing[c]]]);
                score += delta_total[cls][c];
                for (size_t r = 0; r < ranges; ++r)
                    rs[r] += delta_range[cls][c][r];
            }
            for (size_t r = 0; r < ranges; ++r)
                score += std::floor((rs[r] * chart.range_percent[r]) / 100.0);
            out.scores[order_idx] = score;
        }
    } else {
        std::vector<double> range_score(ranges, 0.0);
        for (size_t order_idx : reps) {
            const auto &order = orders[order_idx];
            int32_t life = master.live.life_base;
            std::array<float, 7> channel{};
            std::vector<ActiveEff> active;
            float factor = 0.0f;
            size_t cursor = 0;
            std::optional<ActiveConv> convert;
            double score = 0.0;
            std::fill(range_score.begin(), range_score.end(), 0.0);

            auto expire = [&](int32_t time) {
                for (ptrdiff_t i = static_cast<ptrdiff_t>(active.size()) - 1; i >= 0; --i) {
                    if (active[static_cast<size_t>(i)].end <= time) {
                        auto [_, which, delta] = active[static_cast<size_t>(i)];
                        float diff = f32(static_cast<double>(delta) / 100000.0);
                        if (which == 0)
                            factor = f32(factor - diff);
                        else
                            channel[which] = f32(channel[which] - diff);
                        active.erase(active.begin() + i);
                    }
                }
                if (convert.has_value() && convert->end <= time)
                    convert.reset();
            };

            for (size_t i = 0; i <= n; ++i) {
                int32_t time = (i < n) ? chart.times[i] : kI32Max;
                while (cursor < firing.size() && event_times[firing[cursor]] <= time) {
                    size_t ev = firing[cursor];
                    int32_t start = event_times[ev];
                    expire(start);
                    const auto &slot = slots[order[ev]];
                    for (const auto &eff : slot.effects) {
                        if (eff.gate.kind == 1 && ((life >= eff.gate.value) != eff.gate.positive))
                            continue;
                        int32_t end = start + window_ms(eff.seconds, slot.extension_ms);
                        float diff = f32(static_cast<double>(eff.delta) / 100000.0);
                        if (eff.type == 2000) {
                            factor = f32(factor + diff);
                            active.push_back({end, 0, eff.delta});
                        } else {
                            for (int32_t which : eff.judgements) {
                                channel[which] = f32(channel[which] + diff);
                                active.push_back({end, which, eff.delta});
                            }
                        }
                    }
                    if (slot.recovery)
                        life = std::min(master.live.life_base, life + slot.recovery);
                    if (slot.convert.has_value()) {
                        float sec = slot.effects.empty() ? 5.0f : slot.effects[0].seconds;
                        convert = ActiveConv{start + window_ms(sec, slot.extension_ms),
                                             slot.convert->judgements, slot.convert->limit};
                    }
                    ++cursor;
                }
                if (i == n)
                    break;
                expire(time);
                int32_t judgement = ctx.judgement[i];
                if (convert.has_value() && convert->left > 0 &&
                    std::find(convert->judgements.begin(), convert->judgements.end(), judgement) !=
                        convert->judgements.end()) {
                    judgement = 5;
                    convert->left--;
                }
                float c = f32(f32(chart.judge_frac[judgement] * note_a[i]) * combo_factor[i]);
                float live = f32(factor + channel[judgement]);
                double x = 0.0;
                if (deterministic) {
                    float up_f = f32(f32(ctx.up[i] / ctx.luck[i]) + live);
                    x = std::floor(
                        f32(f32(f32(ctx.luck[i]) * f32(c * up_f)) / static_cast<float>(count)));
                } else {
                    x = std::floor((static_cast<double>(c) *
                                    (ctx.up[i] + ctx.luck[i] * static_cast<double>(live))) /
                                   static_cast<double>(count));
                }
                if (life <= 0)
                    x = std::floor(f32(onus * static_cast<float>(x)));
                score += x;
                int8_t r = chart.range_of[i];
                if (r >= 0)
                    range_score[r] += x;
                if (damage[judgement])
                    life = std::max(0, life - damage[judgement]);
            }
            for (size_t r = 0; r < ranges; ++r)
                score += std::floor((range_score[r] * chart.range_percent[r]) / 100.0);
            out.scores[order_idx] = score;
        }
    }

    if (classes != 5) {
        for (size_t i = 0; i < 120; ++i)
            out.scores[i] = out.scores[sources[i]];
    }
    std::array<double, 120> sorted_scores = out.scores;
    std::sort(sorted_scores.begin(), sorted_scores.end());
    double total = 0.0;
    for (double v : sorted_scores)
        total += v;
    out.mean = total / 120.0;
    out.min = std::numeric_limits<double>::infinity();
    out.max = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < 120; ++i) {
        if (out.scores[i] < out.min) {
            out.min = out.scores[i];
            out.min_order = i;
        }
        if (out.scores[i] > out.max) {
            out.max = out.scores[i];
            out.max_order = i;
        }
    }
    return out;
}

} // namespace gekisou_detail
} // namespace deckcalc
