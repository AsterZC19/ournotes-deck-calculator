#include "gekisou_internal.hpp"

#include <atomic>
#include <mutex>
#include <numeric>
#include <thread>

namespace deckcalc {
namespace gekisou_detail {

struct SlotBase {
    BP3 common{0, 0, 0};
    int64_t fixed_points = 0;
};

struct LeaderProfile {
    std::vector<SkillEffectRow> effects;
    bool simple = false;
};

inline BP3 add3(const BP3 &a, const BP3 &b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

inline int64_t floor_div_10000(int64_t value) {
    return value >= 0 ? value / 10000 : -((-value + 9999) / 10000);
}

// CardPower.mul then to_floor: trunc(a*b/10000), then binary32 floor to whole points.
BP3 pct_floor(const BP3 &base, const BP3 &pct) {
    BP3 out{0, 0, 0};
    for (int i = 0; i < 3; ++i)
        out[i] = floor_f32_div_10000(base[i] * pct[i]) * 10000;
    return out;
}

BP3 pct_uniform(const BP3 &base, int64_t pct) {
    return pct_floor(base, BP3{pct, pct, pct});
}

int64_t points_of(const BP3 &bp) {
    int64_t total = 0;
    for (int64_t value : bp)
        total += floor_div_10000(value);
    return total;
}

inline bool is_cumulative_type(int32_t type) {
    return (type & ~3) == 1500;
}

void accumulate_pct(int32_t type, int64_t value, BP3 &acc) {
    auto add = [&](int index) {
        acc[index] = static_cast<int32_t>(
            wrap_add(static_cast<int32_t>(acc[index]), static_cast<int32_t>(value)));
    };
    switch (type) {
    case 1000:
    case 1500:
        add(0);
        add(1);
        add(2);
        break;
    case 1001:
    case 1501:
        add(1);
        break;
    case 1002:
    case 1502:
        add(2);
        break;
    case 1003:
    case 1503:
        add(0);
        break;
    default:
        break;
    }
}

int64_t cumulative_count(const EngineMaster &master, int32_t id,
                         const std::vector<MemberState> &members, size_t leader) {
    if (id <= 0)
        return 1;
    auto it = master.cumulative.find(id);
    if (it == master.cumulative.end())
        return 1;
    const CumulativeRow &row = it->second;
    auto hit = [&](const MemberState &member) {
        return row.target_ids.empty() || matches_targets(master, member, row.target_ids);
    };
    int64_t count = 1;
    switch (row.type) {
    case 3000:
        count = 0;
        for (const auto &member : members)
            count += hit(member) ? 1 : 0;
        break;
    case 3001:
        count = 0;
        for (size_t i = 0; i < members.size(); ++i)
            if (i != leader && hit(members[i]))
                ++count;
        break;
    case 3002:
        count = 0;
        for (const auto &member : members)
            count += member.card.band_id == members[leader].card.band_id ? 1 : 0;
        break;
    case 3003: {
        std::set<int32_t> bands;
        for (const auto &member : members)
            if (member.card.band_id != members[leader].card.band_id)
                bands.insert(member.card.band_id);
        count = static_cast<int64_t>(bands.size());
        break;
    }
    case 3004: {
        std::set<int32_t> bands;
        for (const auto &member : members)
            bands.insert(member.card.band_id);
        count = static_cast<int64_t>(bands.size());
        break;
    }
    case 3005: {
        std::set<int32_t> types;
        for (const auto &member : members)
            types.insert(member.card.card_type);
        count = static_cast<int64_t>(types.size());
        break;
    }
    default:
        return 1;
    }
    return row.cap < 1 ? count : std::min(count, static_cast<int64_t>(row.cap));
}

bool condition_holds(const EngineMaster &master, int32_t group_id,
                     const std::vector<MemberState> &members, const MusicView *music) {
    if (group_id <= 0)
        return true;
    auto git = master.condition_sets.find(group_id);
    if (git == master.condition_sets.end())
        return true;
    auto any = [&](const MemberState &member, const std::vector<int32_t> &targets) {
        return matches_targets(master, member, targets);
    };
    for (const auto &set : git->second) {
        for (int32_t id : set) {
            auto cit = master.conditions.find(id);
            if (cit == master.conditions.end())
                continue;
            const ConditionRow &condition = cit->second;
            if (condition.type == 0 || condition.target_ids.empty())
                continue;
            bool holds = true;
            if (condition.type == 3000) {
                holds = false;
                for (const auto &member : members)
                    if (any(member, condition.target_ids)) {
                        holds = true;
                        break;
                    }
            } else if (condition.type == 3001) {
                holds = true;
                for (const auto &member : members)
                    if (!any(member, condition.target_ids)) {
                        holds = false;
                        break;
                    }
            } else if (condition.type == 4012) {
                holds = false;
                if (music) {
                    for (int32_t target : condition.target_ids) {
                        auto tit = master.targets.find(target);
                        if (tit != master.targets.end() && tit->second.live_music_type != 0 &&
                            tit->second.live_music_type == music->music_type) {
                            holds = true;
                            break;
                        }
                    }
                }
            }
            if (!holds)
                return false;
        }
    }
    return true;
}

LeaderProfile leader_profile(const EngineMaster &master, const MemberState &leader) {
    LeaderProfile profile;
    auto it = master.leader_skills.find(leader.card.leader_skill_id);
    if (it != master.leader_skills.end()) {
        for (const auto &row : it->second)
            if (row.level == leader.leader_skill_level)
                profile.effects.push_back(row);
    }
    profile.simple = true;
    for (const auto &row : profile.effects)
        if (row.condition_group > 0 || row.cumulative_id > 0) {
            profile.simple = false;
            break;
        }
    return profile;
}

std::vector<BP3> leader_percents(const EngineMaster &master, const LeaderProfile &profile,
                                 const std::vector<MemberState> &members, size_t leader,
                                 const MusicView *music) {
    std::vector<BP3> out(members.size(), BP3{0, 0, 0});
    for (const auto &effect : profile.effects) {
        if (!condition_holds(master, effect.condition_group, members, music))
            continue;
        int64_t value =
            (is_cumulative_type(effect.type) || effect.cumulative_id > 0)
                ? wrap_mul(effect.value, static_cast<int32_t>(cumulative_count(
                                             master, effect.cumulative_id, members, leader)))
                : effect.value;
        for (size_t i = 0; i < members.size(); ++i) {
            if (!effect.target_ids.empty() &&
                !matches_targets(master, members[i], effect.target_ids))
                continue;
            accumulate_pct(effect.type, value, out[i]);
        }
    }
    return out;
}

int64_t leader_points(const SlotBase &base, const BP3 &percent) {
    if (percent[0] == 0 && percent[1] == 0 && percent[2] == 0)
        return 0;
    return points_of(pct_floor(base.common, percent));
}

BP3 band_item_percent(const EngineMaster &master, const PlayerState &player,
                      const MemberState &member) {
    BP3 acc{0, 0, 0};
    const auto &card = member.card;
    for (const auto &[item_id, level] : player.band_items) {
        auto iit = master.band_items.find(item_id);
        if (iit == master.band_items.end())
            continue;
        auto lit = iit->second.levels.find(level);
        if (lit == iit->second.levels.end())
            continue;
        for (const auto &effect : lit->second) {
            for (int32_t target_id : effect.target_ids) {
                auto tit = master.targets.find(target_id);
                if (tit == master.targets.end())
                    continue;
                const TargetRow &target = tit->second;
                int hits =
                    (target.band_id > 0 && target.band_id == card.band_id ? 1 : 0) +
                    (target.character_id > 0 && target.character_id == card.character_id ? 1 : 0) +
                    (target.card_type != 0 && target.card_type == card.card_type ? 1 : 0);
                int32_t type = (effect.type == 1000 || effect.type == 1001 || effect.type == 1002 ||
                                effect.type == 1003)
                                   ? effect.type
                                   : -1;
                for (int i = 0; i < hits; ++i)
                    accumulate_pct(type, effect.value, acc);
            }
        }
    }
    return acc;
}

SlotBase slot_base(const EngineMaster &master, const PlayerState &player, const MemberState &member,
                   const MusicView *music, int32_t member_event_pct = 0) {
    BP3 base = member.own;
    if (member_event_pct)
        base = add3(base, pct_uniform(member.own, member_event_pct));

    auto rank_it = player.character_ranks.find(member.card.character_id);
    int32_t rank = rank_it == player.character_ranks.end() ? 1 : rank_it->second;
    int32_t cr = 0;
    for (const auto &[threshold, bonus] : master.character_rank_bonus)
        if (threshold <= rank)
            cr = bonus;
    int32_t ctr = 0;
    for (const auto &[threshold, bonus] : master.total_rank_bonus)
        if (threshold <= player.total_rank)
            ctr = bonus;
    auto char_mem = player.character_memory.find(member.card.character_id);
    auto music_mem =
        music ? player.music_memory.find(music->memory_key) : player.music_memory.end();
    int64_t memory = (char_mem == player.character_memory.end() ? 0 : char_mem->second) +
                     (music && music_mem != player.music_memory.end() ? music_mem->second : 0);
    int64_t flat = (static_cast<int64_t>(cr) + ctr + memory) * 10000;
    BP3 common = {base[0] + flat, base[1] + flat, base[2] + flat};
    int64_t total = points_of(common);
    total += points_of(pct_floor(common, band_item_percent(master, player, member)));
    if (music) {
        if (member.card.card_type == 99 || music->music_type == 99 ||
            member.card.card_type == music->music_type)
            total += points_of(
                pct_uniform(common, master.params.music_type_base + member.music_type_rate));
        bool tag_matched = false;
        for (int32_t tag : member.card.best_tags)
            if (std::find(music->best_tags.begin(), music->best_tags.end(), tag) !=
                music->best_tags.end()) {
                tag_matched = true;
                break;
            }
        if (tag_matched)
            total += points_of(
                pct_uniform(common, master.params.music_tag_base + member.music_tag_rate));
    }
    auto vip_it = master.vip_bonus.find(player.vip_rank);
    if (vip_it != master.vip_bonus.end() && vip_it->second)
        total += points_of(pct_uniform(common, vip_it->second));
    return {common, total};
}

int64_t snap_points(const EngineMaster &master, const SlotBase &base, const MemberState &member,
                    const SnapState *snap, int32_t snap_event_pct = 0) {
    BP3 percent{0, 0, 0};
    if (snap)
        percent = {snap->percent[0] + snap_event_pct, snap->percent[1] + snap_event_pct,
                   snap->percent[2] + snap_event_pct};
    int64_t total = (snap || snap_event_pct) ? points_of(pct_floor(base.common, percent)) : 0;
    if (snap && snap->card.card_type == member.card.card_type) {
        int64_t link = master.params.type_link_base + snap->type_link_rate;
        total += points_of(pct_uniform(base.common, link));
    }
    return total;
}

TeamPower team_power(const Json *prob, const EngineMaster &master, const PlayerState &player,
                     const std::vector<MemberState> &members,
                     const std::vector<const SnapState *> &snaps, size_t leader,
                     const MusicView *music) {
    if (prob) {
        TeamPowerResult result = calc_deckcalc_team_power(prob, master, members, snaps, leader);
        return {result.total, result.slots, {}};
    }
    LeaderProfile profile = leader_profile(master, members[leader]);
    std::vector<BP3> percents = leader_percents(master, profile, members, leader, music);
    TeamPower out;
    out.leader_percents = percents;
    out.slots.reserve(members.size());
    for (size_t i = 0; i < members.size(); ++i) {
        SlotBase base = slot_base(master, player, members[i], music);
        const SnapState *snap = i < snaps.size() ? snaps[i] : nullptr;
        double slot = static_cast<double>(base.fixed_points) +
                      static_cast<double>(snap_points(master, base, members[i], snap)) +
                      static_cast<double>(leader_points(base, percents[i]));
        out.slots.push_back(slot);
        out.total += slot;
    }
    return out;
}

std::string team_set_key(const Team &team, const std::vector<MemberState> &members) {
    std::vector<std::string> keys;
    keys.reserve(team.members.size());
    for (int member : team.members)
        keys.push_back(members[static_cast<size_t>(member)].key);
    std::sort(keys.begin(), keys.end());
    std::string out;
    for (const auto &key : keys) {
        if (!out.empty())
            out += "|";
        out += key;
    }
    return out;
}

SearchOutput search_teams(const SearchInput &input) {
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    SearchOutput output;
    const EngineMaster &master = *input.master;
    const PlayerState &player = *input.player;
    const std::vector<MemberState> &members = *input.members;
    const std::vector<SnapState> &snaps = *input.snaps;
    const MusicView *music = input.music;
    const SearchConstraints &constraints = input.constraints;
    const int n = static_cast<int>(members.size());
    const int s = static_cast<int>(snaps.size());
    const int width = s + 1;
    const int k = std::max(1, input.k);
    const auto deadline =
        input.time_limit_ms > 0
            ? started + std::chrono::duration_cast<clock::duration>(
                            std::chrono::duration<double, std::milli>(input.time_limit_ms))
            : clock::time_point::max();
    bool timed_out = false;
    double unexplored = -std::numeric_limits<double>::infinity();
    auto expired = [&]() { return input.time_limit_ms > 0 && clock::now() > deadline; };

    std::vector<char> excluded_member(static_cast<size_t>(n), 0);
    for (int i : constraints.excluded_members)
        if (i >= 0 && i < n)
            excluded_member[static_cast<size_t>(i)] = 1;
    std::vector<char> excluded_snap(static_cast<size_t>(s), 0);
    for (int j : constraints.excluded_snaps)
        if (j >= 0 && j < s)
            excluded_snap[static_cast<size_t>(j)] = 1;
    std::vector<char> required_member(static_cast<size_t>(n), 0);
    for (int i : constraints.required_members)
        if (i >= 0 && i < n)
            required_member[static_cast<size_t>(i)] = 1;
    std::set<int> required_snap;
    for (int j : constraints.required_snaps)
        if (j >= 0 && j < s)
            required_snap.insert(j);

    const bool use_pre_power = input.pre_power != nullptr && input.pre_power->valid;
    std::vector<SlotBase> bases(static_cast<size_t>(n));
    std::vector<double> fixed_member_power(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        if (use_pre_power) {
            fixed_member_power[static_cast<size_t>(i)] =
                input.pre_power->member_fixed_power(members[static_cast<size_t>(i)]);
        } else {
            bases[static_cast<size_t>(i)] =
                slot_base(master, player, members[static_cast<size_t>(i)], music);
            fixed_member_power[static_cast<size_t>(i)] =
                static_cast<double>(bases[static_cast<size_t>(i)].fixed_points);
        }
    }

    std::vector<char> allowed_snap(static_cast<size_t>(s), 0);
    for (int j = 0; j < s; ++j)
        if (!excluded_snap[static_cast<size_t>(j)] && !constraints.no_snaps)
            allowed_snap[static_cast<size_t>(j)] = 1;

    std::vector<double> snap_power(static_cast<size_t>(n * width), 0.0);
    std::vector<double> skill_high(static_cast<size_t>(n * width), 0.0);
    std::vector<double> skill_low(static_cast<size_t>(n * width), 0.0);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < s; ++j) {
            if (!allowed_snap[static_cast<size_t>(j)])
                continue;
            if (use_pre_power) {
                snap_power[static_cast<size_t>(i * width + j)] = input.pre_power->snap_add_power(
                    members[static_cast<size_t>(i)], &snaps[static_cast<size_t>(j)]);
            } else {
                snap_power[static_cast<size_t>(i * width + j)] = static_cast<double>(
                    snap_points(master, bases[static_cast<size_t>(i)],
                                members[static_cast<size_t>(i)], &snaps[static_cast<size_t>(j)]));
            }
            if (input.skill) {
                auto range = input.skill(i, j);
                skill_low[static_cast<size_t>(i * width + j)] = range.first;
                skill_high[static_cast<size_t>(i * width + j)] = range.second;
            }
        }
        if (input.skill) {
            auto range = input.skill(i, -1);
            skill_low[static_cast<size_t>(i * width + s)] = range.first;
            skill_high[static_cast<size_t>(i * width + s)] = range.second;
        }
    }

    std::vector<double> member_bonus(static_cast<size_t>(n), 0.0);
    std::vector<double> snap_bonus(static_cast<size_t>(s), 0.0);
    for (int i = 0; i < n; ++i)
        if (input.member_bonus)
            member_bonus[static_cast<size_t>(i)] = input.member_bonus(i);
    for (int j = 0; j < s; ++j)
        if (input.snap_bonus)
            snap_bonus[static_cast<size_t>(j)] = input.snap_bonus(j);

    struct BestSnap {
        double power = 0.0;
        double weight = 0.0;
        double bonus = 0.0;
    };
    std::vector<BestSnap> best(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto binding = constraints.bindings.find(i);
        if (binding != constraints.bindings.end()) {
            int j = binding->second;
            int column = j < 0 ? s : j;
            best[static_cast<size_t>(i)] = {snap_power[static_cast<size_t>(i * width + column)],
                                            skill_high[static_cast<size_t>(i * width + column)],
                                            j < 0 ? 0.0 : snap_bonus[static_cast<size_t>(j)]};
            continue;
        }
        for (int j = 0; j < s; ++j) {
            if (!allowed_snap[static_cast<size_t>(j)])
                continue;
            best[static_cast<size_t>(i)].power = std::max(
                best[static_cast<size_t>(i)].power, snap_power[static_cast<size_t>(i * width + j)]);
            best[static_cast<size_t>(i)].weight =
                std::max(best[static_cast<size_t>(i)].weight,
                         skill_high[static_cast<size_t>(i * width + j)]);
            best[static_cast<size_t>(i)].bonus =
                std::max(best[static_cast<size_t>(i)].bonus, snap_bonus[static_cast<size_t>(j)]);
        }
        if (constraints.allow_empty_snaps || constraints.no_snaps) {
            best[static_cast<size_t>(i)].power = std::max(
                best[static_cast<size_t>(i)].power, snap_power[static_cast<size_t>(i * width + s)]);
            best[static_cast<size_t>(i)].weight =
                std::max(best[static_cast<size_t>(i)].weight,
                         skill_high[static_cast<size_t>(i * width + s)]);
        }
    }

    const bool can_prune_classes = input.prune_snap_classes && input.snap_skill_class != nullptr &&
                                   !constraints.min_bonus.has_value() &&
                                   !constraints.max_bonus.has_value();

    std::vector<std::vector<int>> options(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto binding = constraints.bindings.find(i);
        if (binding != constraints.bindings.end()) {
            options[static_cast<size_t>(i)].push_back(binding->second);
            continue;
        }
        auto &opt = options[static_cast<size_t>(i)];
        for (int j = 0; j < s; ++j)
            if (allowed_snap[static_cast<size_t>(j)])
                opt.push_back(j);
        if (constraints.allow_empty_snaps || constraints.no_snaps)
            opt.push_back(-1);
        std::sort(opt.begin(), opt.end(), [&](int a, int b) {
            int col_a = a < 0 ? s : a;
            int col_b = b < 0 ? s : b;
            double pa = snap_power[static_cast<size_t>(i * width + col_a)];
            double pb = snap_power[static_cast<size_t>(i * width + col_b)];
            if (pa != pb)
                return pa > pb;
            return a < b;
        });
        if (can_prune_classes) {
            // Pigeonhole principle: at most 4 other slots (+ external bindings) can consume
            // snapshots from the same equivalence class.
            const int per_class_cap = 5 + static_cast<int>(constraints.bindings.size());
            std::unordered_map<int, int> class_nonreq_count;
            std::vector<int> filtered;
            filtered.reserve(opt.size());
            for (int snap : opt) {
                if (snap >= 0 && required_snap.count(snap)) {
                    filtered.push_back(snap);
                    continue;
                }
                int col = snap < 0 ? s : snap;
                int cls = (*input.snap_skill_class)[static_cast<size_t>(i * width + col)];
                if (++class_nonreq_count[cls] <= per_class_cap)
                    filtered.push_back(snap);
            }
            opt = std::move(filtered);
        }
    }

    // Precompute per-member skill classes (each class holds required snaps + top-5 non-required
    // snaps sorted descending by snap_power, exact by pigeonhole principle).
    struct MemberClassGroup {
        int cls = 0;
        std::vector<int> snaps;
        double max_power = -std::numeric_limits<double>::infinity();
    };
    std::vector<std::vector<MemberClassGroup>> member_classes(static_cast<size_t>(n));
    if (can_prune_classes) {
        for (int i = 0; i < n; ++i) {
            std::unordered_map<int, size_t> cls_to_idx;
            for (int snap : options[static_cast<size_t>(i)]) {
                const int col = snap < 0 ? s : snap;
                const int cls = (*input.snap_skill_class)[static_cast<size_t>(i * width + col)];
                const double p = snap_power[static_cast<size_t>(i * width + col)];
                auto it = cls_to_idx.find(cls);
                if (it == cls_to_idx.end()) {
                    const size_t idx = member_classes[static_cast<size_t>(i)].size();
                    cls_to_idx[cls] = idx;
                    member_classes[static_cast<size_t>(i)].push_back({cls, {snap}, p});
                } else {
                    auto &grp = member_classes[static_cast<size_t>(i)][it->second];
                    grp.snaps.push_back(snap);
                    grp.max_power = std::max(grp.max_power, p);
                }
            }
            std::sort(member_classes[static_cast<size_t>(i)].begin(),
                      member_classes[static_cast<size_t>(i)].end(),
                      [](const MemberClassGroup &a, const MemberClassGroup &b) {
                          if (a.max_power != b.max_power)
                              return a.max_power > b.max_power;
                          return a.cls < b.cls;
                      });
        }
    }

    // Filter out leaders that conflict with required_members of the same character.
    std::vector<int> leaders;
    for (int i = 0; i < n; ++i) {
        if (excluded_member[static_cast<size_t>(i)] ||
            (constraints.leader.has_value() && *constraints.leader != i))
            continue;
        const int32_t lc = members[static_cast<size_t>(i)].card.character_id;
        bool conflicts_required = false;
        for (int req : constraints.required_members) {
            if (req != i && req >= 0 && req < n &&
                members[static_cast<size_t>(req)].card.character_id == lc) {
                conflicts_required = true;
                break;
            }
        }
        if (!conflicts_required)
            leaders.push_back(i);
    }

    // Rank leaders by estimated team power (including leader skill bonus) rather than raw self
    // power.
    std::vector<double> leader_score(static_cast<size_t>(n), 0.0);
    for (int l : leaders) {
        const MemberState &lm = members[static_cast<size_t>(l)];
        double est =
            fixed_member_power[static_cast<size_t>(l)] + best[static_cast<size_t>(l)].power;
        // Probe with top 4 distinct-character teammates matching leader's band/type or raw power.
        std::map<int32_t, int> best_by_char;
        for (int m = 0; m < n; ++m) {
            if (m == l || excluded_member[static_cast<size_t>(m)])
                continue;
            const int32_t ch = members[static_cast<size_t>(m)].card.character_id;
            if (ch == lm.card.character_id)
                continue;
            const double pm =
                fixed_member_power[static_cast<size_t>(m)] + best[static_cast<size_t>(m)].power;
            auto it = best_by_char.find(ch);
            if (it == best_by_char.end()) {
                best_by_char[ch] = m;
            } else {
                const double prev = fixed_member_power[static_cast<size_t>(it->second)] +
                                    best[static_cast<size_t>(it->second)].power;
                if (pm > prev)
                    it->second = m;
            }
        }
        std::vector<int> mates;
        mates.reserve(best_by_char.size());
        for (const auto &[_, m] : best_by_char)
            mates.push_back(m);
        auto score_probe = [&](std::vector<int> probe_idx) -> double {
            if (probe_idx.size() < 5)
                return est;
            std::vector<MemberState> states;
            states.reserve(5);
            for (int idx : probe_idx)
                states.push_back(members[static_cast<size_t>(idx)]);
            double tot = 0.0;
            if (use_pre_power) {
                auto ld = input.pre_power->leader_slot_powers(states, 0);
                for (size_t sl = 0; sl < 5; ++sl)
                    tot += fixed_member_power[static_cast<size_t>(probe_idx[sl])] +
                           best[static_cast<size_t>(probe_idx[sl])].power + ld[sl];
            } else {
                LeaderProfile prof = leader_profile(master, lm);
                auto pcts = leader_percents(master, prof, states, 0, music);
                for (size_t sl = 0; sl < 5; ++sl) {
                    const auto &b = bases[static_cast<size_t>(probe_idx[sl])];
                    tot += static_cast<double>(b.fixed_points) +
                           static_cast<double>(leader_points(b, pcts[sl])) +
                           best[static_cast<size_t>(probe_idx[sl])].power;
                }
            }
            return tot;
        };
        if (mates.size() >= 4) {
            std::sort(mates.begin(), mates.end(), [&](int a, int b) {
                return fixed_member_power[static_cast<size_t>(a)] +
                           best[static_cast<size_t>(a)].power >
                       fixed_member_power[static_cast<size_t>(b)] +
                           best[static_cast<size_t>(b)].power;
            });
            est = std::max(est, score_probe({l, mates[0], mates[1], mates[2], mates[3]}));
            // Also try same-band and same-card-type probes for band/attribute/cumulative leaders.
            std::vector<int> same_band, same_type;
            for (int m : mates) {
                if (members[static_cast<size_t>(m)].card.band_id == lm.card.band_id)
                    same_band.push_back(m);
                if (members[static_cast<size_t>(m)].card.card_type == lm.card.card_type)
                    same_type.push_back(m);
            }
            if (same_band.size() >= 4)
                est = std::max(
                    est, score_probe({l, same_band[0], same_band[1], same_band[2], same_band[3]}));
            if (same_type.size() >= 4)
                est = std::max(
                    est, score_probe({l, same_type[0], same_type[1], same_type[2], same_type[3]}));
        }
        leader_score[static_cast<size_t>(l)] = est;
    }
    std::sort(leaders.begin(), leaders.end(), [&](int a, int b) {
        if (leader_score[static_cast<size_t>(a)] != leader_score[static_cast<size_t>(b)])
            return leader_score[static_cast<size_t>(a)] > leader_score[static_cast<size_t>(b)];
        return a < b;
    });
    if (input.shard_index && input.shard_count && *input.shard_count > 1) {
        std::vector<int> mine;
        for (size_t pos = 0; pos < leaders.size(); ++pos)
            if (static_cast<int>(pos % static_cast<size_t>(*input.shard_count)) ==
                *input.shard_index)
                mine.push_back(leaders[pos]);
        leaders = std::move(mine);
    }

    auto limit = [&]() {
        double value = input.floor.value_or(-std::numeric_limits<double>::infinity());
        if (static_cast<int>(output.hits.size()) >= k)
            value = std::max(value, output.hits[static_cast<size_t>(k - 1)].key);
        return value;
    };
    auto request_stop = [&]() { timed_out = true; };

    std::vector<int> match_cand_snaps;
    match_cand_snaps.reserve(static_cast<size_t>(std::max(s, 1)));
    std::vector<char> match_snap_seen(static_cast<size_t>(std::max(s, 1)), 0);
    std::vector<int8_t> match_assigned;

    for (int leader : leaders) {
        if (timed_out)
            break;
        if (expired()) {
            request_stop();
            break;
        }
        const MemberState &leader_member = members[static_cast<size_t>(leader)];
        int32_t leader_character = leader_member.card.character_id;
        bool leader_conflicts_required = false;
        for (int i : constraints.required_members)
            if (i != leader && i >= 0 && i < n &&
                members[static_cast<size_t>(i)].card.character_id == leader_character) {
                leader_conflicts_required = true;
                break;
            }
        if (leader_conflicts_required)
            continue;
        ++output.stats.leaders;

        std::map<int32_t, std::vector<int>> groups;
        for (int i = 0; i < n; ++i) {
            if (i == leader || excluded_member[static_cast<size_t>(i)])
                continue;
            int32_t character = members[static_cast<size_t>(i)].card.character_id;
            if (character == leader_character)
                continue;
            groups[character].push_back(i);
        }
        if (groups.empty())
            continue;

        std::vector<std::vector<int>> forced;
        std::vector<std::vector<int>> free;
        bool impossible = false;
        for (auto &[_, list] : groups) {
            std::vector<int> must;
            for (int i : list)
                if (required_member[static_cast<size_t>(i)])
                    must.push_back(i);
            if (must.size() > 1) {
                impossible = true;
                break;
            }
            std::vector<std::vector<int>> &target = must.empty() ? free : forced;
            std::vector<int> ordered = must.empty() ? list : must;
            std::sort(ordered.begin(), ordered.end(), [&](int a, int b) {
                double pa =
                    fixed_member_power[static_cast<size_t>(a)] + best[static_cast<size_t>(a)].power;
                double pb =
                    fixed_member_power[static_cast<size_t>(b)] + best[static_cast<size_t>(b)].power;
                return pa > pb;
            });
            target.push_back(std::move(ordered));
        }
        if (impossible || forced.size() > 4)
            continue;
        std::sort(free.begin(), free.end(),
                  [&](const std::vector<int> &a, const std::vector<int> &b) {
                      double pa = fixed_member_power[static_cast<size_t>(a.front())] +
                                  best[static_cast<size_t>(a.front())].power;
                      double pb = fixed_member_power[static_cast<size_t>(b.front())] +
                                  best[static_cast<size_t>(b.front())].power;
                      return pa > pb;
                  });
        std::vector<std::vector<int>> characters = forced;
        characters.insert(characters.end(), free.begin(), free.end());
        if (static_cast<int>(characters.size()) < 4)
            continue;
        const size_t forced_count = forced.size();

        std::vector<int> chosen;
        chosen.reserve(4);
        std::function<void(size_t)> choose_members = [&](size_t index) {
            if (timed_out || index > characters.size())
                return;
            if (chosen.size() == 4) {
                std::vector<int> team_members;
                team_members.push_back(leader);
                team_members.insert(team_members.end(), chosen.begin(), chosen.end());
                Team team;
                team.members = team_members;
                team.snaps.assign(5, -1);
                if (expired()) {
                    request_stop();
                    return;
                }

                std::vector<MemberState> team_states;
                team_states.reserve(team_members.size());
                for (int member : team_members)
                    team_states.push_back(members[static_cast<size_t>(member)]);
                std::vector<double> base_power(team_members.size(), 0.0);
                if (use_pre_power) {
                    auto ld_powers = input.pre_power->leader_slot_powers(team_states, 0);
                    for (size_t slot = 0; slot < team_members.size(); ++slot) {
                        base_power[slot] =
                            fixed_member_power[static_cast<size_t>(team_members[slot])] +
                            ld_powers[slot];
                    }
                } else {
                    LeaderProfile team_profile = leader_profile(master, leader_member);
                    std::vector<BP3> percents =
                        leader_percents(master, team_profile, team_states, 0, music);
                    for (size_t slot = 0; slot < team_members.size(); ++slot) {
                        const auto &base = bases[static_cast<size_t>(team_members[slot])];
                        base_power[slot] = static_cast<double>(base.fixed_points) +
                                           static_cast<double>(leader_points(base, percents[slot]));
                    }
                }

                std::vector<char> used(static_cast<size_t>(std::max(s, 1)), 0);
                for (const auto &[member, snap] : constraints.bindings) {
                    if (snap >= 0 && snap < s &&
                        std::find(team_members.begin(), team_members.end(), member) ==
                            team_members.end())
                        used[static_cast<size_t>(snap)] = 1;
                }
                double member_total_bonus = 0.0;
                for (int member : team_members)
                    member_total_bonus += member_bonus[static_cast<size_t>(member)];

                const std::string cur_set_key = team_set_key(team, members);
                auto record_exact_hit = [&](const Team &eval_team, const Totals &totals,
                                            SearchExact &&exact) {
                    size_t existing = output.hits.size();
                    for (size_t i = 0; i < output.hits.size(); ++i)
                        if (output.hits[i].set_key == cur_set_key) {
                            existing = i;
                            break;
                        }
                    if (existing < output.hits.size() && output.hits[existing].key >= exact.key)
                        return;
                    SearchHit hit;
                    hit.team = eval_team;
                    hit.set_key = cur_set_key;
                    hit.power = exact.power;
                    hit.slot_powers = std::move(exact.slot_powers);
                    hit.totals = totals;
                    hit.key = exact.key;
                    hit.detail = std::move(exact.detail);
                    if (existing < output.hits.size())
                        output.hits.erase(output.hits.begin() + static_cast<ptrdiff_t>(existing));
                    size_t at = output.hits.size();
                    while (at > 0 && output.hits[at - 1].key < hit.key)
                        --at;
                    output.hits.insert(output.hits.begin() + static_cast<ptrdiff_t>(at),
                                       std::move(hit));
                    if (static_cast<int>(output.hits.size()) > k)
                        output.hits.resize(static_cast<size_t>(k));
                };

                if (can_prune_classes) {
                    // Enumerate 5-slot skill-class tuples and solve exact max-weight distinct
                    // snapshot matching per tuple, evaluating immediately so results arrive fast.
                    const double base_sum =
                        std::accumulate(base_power.begin(), base_power.end(), 0.0);
                    std::array<const std::vector<MemberClassGroup> *, 5> slot_classes{};
                    bool any_empty_class = false;
                    for (size_t sl = 0; sl < 5; ++sl) {
                        slot_classes[sl] = &member_classes[static_cast<size_t>(team_members[sl])];
                        if (slot_classes[sl]->empty()) {
                            any_empty_class = true;
                            break;
                        }
                    }
                    if (any_empty_class)
                        return;

                    auto match_class_tuple = [&](const std::array<size_t, 5> &cls_idx,
                                                 std::vector<int> &out_snaps,
                                                 double &out_total_power) -> bool {
                        // 1. Verify every required snapshot is present in at least one chosen
                        // class.
                        for (int req : required_snap) {
                            if (used[static_cast<size_t>(req)])
                                return false;
                            bool covered = false;
                            for (size_t sl = 0; sl < 5; ++sl) {
                                const auto &snaps_vec = (*slot_classes[sl])[cls_idx[sl]].snaps;
                                if (std::find(snaps_vec.begin(), snaps_vec.end(), req) !=
                                    snaps_vec.end()) {
                                    covered = true;
                                    break;
                                }
                            }
                            if (!covered)
                                return false;
                        }

                        // 2. Fast greedy path: pick the best non-externally-used snap per slot.
                        std::array<int, 5> fast_snaps{-1, -1, -1, -1, -1};
                        double fast_power = base_sum;
                        bool fast_ok = true;
                        for (size_t sl = 0; sl < 5; ++sl) {
                            const auto &snaps_vec = (*slot_classes[sl])[cls_idx[sl]].snaps;
                            int chosen_snap = -2;
                            for (int snap : snaps_vec) {
                                if (snap >= 0 && used[static_cast<size_t>(snap)])
                                    continue;
                                chosen_snap = snap;
                                break;
                            }
                            if (chosen_snap == -2) {
                                fast_ok = false;
                                break;
                            }
                            if (chosen_snap >= 0) {
                                for (size_t prev = 0; prev < sl; ++prev) {
                                    if (fast_snaps[prev] == chosen_snap) {
                                        fast_ok = false;
                                        break;
                                    }
                                }
                            }
                            if (!fast_ok)
                                break;
                            fast_snaps[sl] = chosen_snap;
                            const int col = chosen_snap < 0 ? s : chosen_snap;
                            fast_power +=
                                snap_power[static_cast<size_t>(team_members[sl] * width + col)];
                        }
                        if (fast_ok && !required_snap.empty()) {
                            for (int req : required_snap) {
                                if (std::find(fast_snaps.begin(), fast_snaps.end(), req) ==
                                    fast_snaps.end()) {
                                    fast_ok = false;
                                    break;
                                }
                            }
                        }
                        if (fast_ok) {
                            out_snaps.assign(fast_snaps.begin(), fast_snaps.end());
                            out_total_power = fast_power;
                            return true;
                        }

                        // 3. Exact 5-slot bitmask DP over the <=30 union of class snapshots.
                        match_cand_snaps.clear();
                        std::array<bool, 5> slot_allows_empty{false, false, false, false, false};
                        for (size_t sl = 0; sl < 5; ++sl) {
                            for (int snap : (*slot_classes[sl])[cls_idx[sl]].snaps) {
                                if (snap < 0) {
                                    slot_allows_empty[sl] = true;
                                    continue;
                                }
                                if (used[static_cast<size_t>(snap)])
                                    continue;
                                if (!match_snap_seen[static_cast<size_t>(snap)]) {
                                    match_snap_seen[static_cast<size_t>(snap)] = 1;
                                    match_cand_snaps.push_back(snap);
                                }
                            }
                        }
                        for (int snap : match_cand_snaps)
                            match_snap_seen[static_cast<size_t>(snap)] = 0;

                        const size_t num_cands = match_cand_snaps.size();
                        constexpr int kStates = 32;
                        constexpr double kNegInf = -std::numeric_limits<double>::infinity();
                        std::array<double, kStates> dp{}, next_dp{};
                        dp.fill(kNegInf);
                        dp[0] = base_sum;
                        if (match_assigned.size() < num_cands * kStates)
                            match_assigned.resize(num_cands * kStates);

                        for (size_t ci = 0; ci < num_cands; ++ci) {
                            const int snap = match_cand_snaps[ci];
                            const bool is_req = required_snap.count(snap) > 0;
                            const size_t off = ci * kStates;
                            if (is_req) {
                                next_dp.fill(kNegInf);
                                std::fill(match_assigned.begin() + static_cast<ptrdiff_t>(off),
                                          match_assigned.begin() +
                                              static_cast<ptrdiff_t>(off + kStates),
                                          static_cast<int8_t>(-2));
                            } else {
                                next_dp = dp;
                                for (int mask = 0; mask < kStates; ++mask)
                                    match_assigned[off + static_cast<size_t>(mask)] =
                                        (dp[static_cast<size_t>(mask)] > kNegInf) ? -1 : -2;
                            }
                            // Precompute which slots allow this snap in their chosen class.
                            uint8_t eligible_slots = 0;
                            for (size_t sl = 0; sl < 5; ++sl) {
                                const auto &sv = (*slot_classes[sl])[cls_idx[sl]].snaps;
                                if (std::find(sv.begin(), sv.end(), snap) != sv.end())
                                    eligible_slots |= static_cast<uint8_t>(1u << sl);
                            }
                            for (int mask = 0; mask < kStates; ++mask) {
                                if (dp[static_cast<size_t>(mask)] == kNegInf)
                                    continue;
                                const uint8_t free_elig =
                                    static_cast<uint8_t>(eligible_slots & (~mask));
                                for (int sl = 0; sl < 5; ++sl) {
                                    if (!(free_elig & (1u << sl)))
                                        continue;
                                    const int target = mask | (1 << sl);
                                    const double val = dp[static_cast<size_t>(mask)] +
                                                       snap_power[static_cast<size_t>(
                                                           team_members[sl] * width + snap)];
                                    if (val > next_dp[static_cast<size_t>(target)]) {
                                        next_dp[static_cast<size_t>(target)] = val;
                                        match_assigned[off + static_cast<size_t>(target)] =
                                            static_cast<int8_t>(sl);
                                    }
                                }
                            }
                            dp = next_dp;
                        }

                        // Find best final mask where every remaining unset bit allows empty snap.
                        int best_mask = -1;
                        double best_val = kNegInf;
                        for (int mask = 0; mask < kStates; ++mask) {
                            if (dp[static_cast<size_t>(mask)] == kNegInf)
                                continue;
                            bool ok = true;
                            double total_p = dp[static_cast<size_t>(mask)];
                            for (int sl = 0; sl < 5; ++sl) {
                                if (!(mask & (1 << sl))) {
                                    if (!slot_allows_empty[static_cast<size_t>(sl)]) {
                                        ok = false;
                                        break;
                                    }
                                    total_p += snap_power[static_cast<size_t>(
                                        team_members[sl] * width + s)];
                                }
                            }
                            if (ok && total_p > best_val) {
                                best_val = total_p;
                                best_mask = mask;
                            }
                        }
                        if (best_mask < 0)
                            return false;
                        out_snaps.assign(5, -1);
                        int cur_mask = best_mask;
                        for (int ci = static_cast<int>(num_cands) - 1; ci >= 0; --ci) {
                            const int8_t sl = match_assigned[static_cast<size_t>(ci) * kStates +
                                                             static_cast<size_t>(cur_mask)];
                            if (sl >= 0) {
                                out_snaps[static_cast<size_t>(sl)] =
                                    match_cand_snaps[static_cast<size_t>(ci)];
                                cur_mask ^= (1 << sl);
                            }
                        }
                        out_total_power = best_val;
                        return true;
                    };

                    std::array<size_t, 5> cls_idx{};
                    std::vector<int> matched_snaps(5, -1);
                    std::function<void(size_t)> walk_class_tuples = [&](size_t slot) {
                        if (timed_out)
                            return;
                        ++output.stats.snap_nodes;
                        if ((output.stats.snap_nodes & 63) == 0 && expired()) {
                            request_stop();
                            return;
                        }
                        if (slot == 5) {
                            double matched_power = 0.0;
                            if (!match_class_tuple(cls_idx, matched_snaps, matched_power))
                                return;
                            Totals totals{matched_power, 0.0, 0.0, member_total_bonus};
                            if (input.bound && input.bound(totals) < limit())
                                return;
                            ++output.stats.candidates;
                            if (expired()) {
                                request_stop();
                                return;
                            }
                            team.snaps = matched_snaps;
                            SearchExact exact;
                            if (input.exact_with_power &&
                                (!use_pre_power || input.pre_power->round_mode !=
                                                       PrecomputedProbPower::RoundMode::None)) {
                                std::vector<double> slot_powers(5, 0.0);
                                for (size_t sl = 0; sl < 5; ++sl) {
                                    int member = team_members[sl];
                                    int col = matched_snaps[sl] < 0 ? s : matched_snaps[sl];
                                    slot_powers[sl] =
                                        base_power[sl] +
                                        snap_power[static_cast<size_t>(member * width + col)];
                                }
                                exact = input.exact_with_power(team, matched_power, slot_powers);
                            } else {
                                exact = input.exact(team);
                            }
                            ++output.stats.exact;
                            record_exact_hit(team, totals, std::move(exact));
                            return;
                        }
                        const size_t cnt = slot_classes[slot]->size();
                        for (size_t ci = 0; ci < cnt; ++ci) {
                            cls_idx[slot] = ci;
                            walk_class_tuples(slot + 1);
                            if (timed_out)
                                return;
                        }
                    };
                    walk_class_tuples(0);
                    return;
                }

                std::vector<int> assignment(team_members.size(), -1);
                std::function<void(size_t, double, double, double, double)> walk =
                    [&](size_t slot, double power, double weight, double weight_low, double bonus) {
                        if (timed_out)
                            return;
                        ++output.stats.snap_nodes;
                        if ((output.stats.snap_nodes & 1023) == 0 && expired()) {
                            request_stop();
                            return;
                        }
                        if (slot == team_members.size()) {
                            int missing = 0;
                            for (int j : required_snap)
                                if (!used[static_cast<size_t>(j)])
                                    ++missing;
                            if (missing)
                                return;
                            Totals totals{power, weight, weight_low, bonus};
                            if (constraints.min_bonus && bonus < *constraints.min_bonus)
                                return;
                            if (constraints.max_bonus && bonus > *constraints.max_bonus)
                                return;
                            if (input.bound && input.bound(totals) < limit())
                                return;
                            if (input.interval) {
                                auto range = input.interval(team, totals);
                                if (range.second < limit())
                                    return;
                            }
                            ++output.stats.candidates;
                            team.snaps = assignment;
                            SearchExact exact = input.exact(team);
                            ++output.stats.exact;
                            record_exact_hit(team, totals, std::move(exact));
                            return;
                        }
                        int member = team_members[slot];
                        for (int snap : options[static_cast<size_t>(member)]) {
                            if (snap >= 0 && used[static_cast<size_t>(snap)])
                                continue;
                            if (snap >= 0)
                                used[static_cast<size_t>(snap)] = 1;
                            assignment[slot] = snap;
                            int column = snap < 0 ? s : snap;
                            walk(slot + 1,
                                 power + snap_power[static_cast<size_t>(member * width + column)],
                                 weight + skill_high[static_cast<size_t>(member * width + column)],
                                 weight_low +
                                     skill_low[static_cast<size_t>(member * width + column)],
                                 bonus + member_bonus[static_cast<size_t>(member)] +
                                     (snap < 0 ? 0.0 : snap_bonus[static_cast<size_t>(snap)]));
                            if (snap >= 0)
                                used[static_cast<size_t>(snap)] = 0;
                            if (timed_out)
                                return;
                        }
                        assignment[slot] = -1;
                    };
                walk(0, std::accumulate(base_power.begin(), base_power.end(), 0.0), 0.0, 0.0,
                     member_total_bonus);
                return;
            }
            if (characters.size() - index < 4 - chosen.size())
                return;
            if (index < forced_count) {
                for (int candidate : characters[index]) {
                    chosen.push_back(candidate);
                    choose_members(index + 1);
                    chosen.pop_back();
                    if (timed_out)
                        return;
                }
                return;
            }
            if (expired()) {
                request_stop();
                return;
            }
            for (int candidate : characters[index]) {
                chosen.push_back(candidate);
                ++output.stats.member_nodes;
                choose_members(index + 1);
                chosen.pop_back();
                if (timed_out)
                    return;
            }
            choose_members(index + 1);
        };
        choose_members(0);
    }

    output.proven = !timed_out;
    if (timed_out) {
        output.bound = std::max(unexplored, output.hits.empty() ? 0.0 : output.hits[0].key);
        output.has_bound = true;
    }
    output.stats.elapsed_ms =
        std::chrono::duration<double, std::milli>(clock::now() - started).count();
    return output;
}

std::string criterion_key(const std::string &criterion) {
    if (criterion == "max" || criterion == "best-ap" || criterion == "theoretical_score")
        return "max";
    if (criterion == "min" || criterion == "worst-ap")
        return "min";
    return "mean";
}

double order_key(const OrderScores &scores, const std::string &criterion) {
    if (criterion == "max")
        return scores.max;
    if (criterion == "min")
        return scores.min;
    return scores.mean;
}

struct WarmStartResult {
    std::vector<SearchHit> hits;
    int64_t candidates = 0;
    int64_t exact = 0;
};

WarmStartResult run_gekisou_warm_start(
    const EngineMaster &master, const PlayerState &player, const std::vector<MemberState> &members,
    const std::vector<SnapState> &snaps, const MusicView *music, const GekisoChart &chart,
    const PrecomputedProbPower &pre_power, const std::vector<SlotSkill> &slot_skill_table,
    const std::vector<int> &snap_skill_class, const SearchConstraints &constraints,
    bool allow_empty_snaps, int eff_k, double budget_ms,
    const std::function<SearchExact(const Team &, const double *, const std::vector<double> *)>
        &eval_fn) {
    using clock = std::chrono::steady_clock;
    const auto ws_start = clock::now();
    const auto ws_deadline =
        ws_start + std::chrono::duration_cast<clock::duration>(
                       std::chrono::duration<double, std::milli>(std::max(1.0, budget_ms)));
    auto ws_expired = [&]() { return clock::now() >= ws_deadline; };

    WarmStartResult out;
    const int n = static_cast<int>(members.size());
    const int s = static_cast<int>(snaps.size());
    const int width = s + 1;
    if (n < 5)
        return out;

    std::vector<char> excluded_member(static_cast<size_t>(n), 0);
    for (int idx : constraints.excluded_members)
        if (idx >= 0 && idx < n)
            excluded_member[static_cast<size_t>(idx)] = 1;
    std::vector<char> excluded_snap(static_cast<size_t>(s), 0);
    for (int idx : constraints.excluded_snaps)
        if (idx >= 0 && idx < s)
            excluded_snap[static_cast<size_t>(idx)] = 1;
    std::vector<char> required_member(static_cast<size_t>(n), 0);
    std::vector<int> req_member_list;
    for (int idx : constraints.required_members) {
        if (idx >= 0 && idx < n && !required_member[static_cast<size_t>(idx)]) {
            required_member[static_cast<size_t>(idx)] = 1;
            req_member_list.push_back(idx);
        }
    }
    std::set<int> required_snap;
    for (int idx : constraints.required_snaps)
        if (idx >= 0 && idx < s)
            required_snap.insert(idx);
    if (req_member_list.size() > 5 || required_snap.size() > 5)
        return out;

    const bool use_pre = pre_power.valid;
    const bool can_pass_power =
        !use_pre || pre_power.round_mode != PrecomputedProbPower::RoundMode::None;
    std::vector<SlotBase> bases(static_cast<size_t>(n));
    std::vector<double> fixed_power(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        if (use_pre) {
            fixed_power[static_cast<size_t>(i)] =
                pre_power.member_fixed_power(members[static_cast<size_t>(i)]);
        } else {
            bases[static_cast<size_t>(i)] =
                slot_base(master, player, members[static_cast<size_t>(i)], music);
            fixed_power[static_cast<size_t>(i)] =
                static_cast<double>(bases[static_cast<size_t>(i)].fixed_points);
        }
    }

    std::vector<char> allowed_snap(static_cast<size_t>(s), 0);
    for (int j = 0; j < s; ++j)
        if (!excluded_snap[static_cast<size_t>(j)] && !constraints.no_snaps)
            allowed_snap[static_cast<size_t>(j)] = 1;
    for (int req : required_snap)
        if (!allowed_snap[static_cast<size_t>(req)])
            return out;

    std::vector<double> snap_power(static_cast<size_t>(n * width), 0.0);
    std::vector<double> best_snap_power(static_cast<size_t>(n), 0.0);
    std::vector<double> skill_proxy(static_cast<size_t>(n), 0.0);
    auto slot_skill_proxy = [](const SlotSkill &sk) -> double {
        double v = 0.0;
        for (const auto &eff : sk.effects)
            if (eff.delta > 0 && eff.seconds > 0.0f)
                v += static_cast<double>(eff.delta) *
                     (static_cast<double>(eff.seconds) +
                      static_cast<double>(sk.extension_ms) / 1000.0);
        return v;
    };

    struct WsClassGroup {
        int cls = 0;
        std::vector<int> snaps;
        double max_power = -std::numeric_limits<double>::infinity();
    };
    std::vector<std::vector<int>> top_all_snaps(static_cast<size_t>(n));
    std::vector<std::vector<WsClassGroup>> member_classes(static_cast<size_t>(n));

    for (int i = 0; i < n; ++i) {
        std::vector<int> opt;
        for (int j = 0; j < s; ++j) {
            if (!allowed_snap[static_cast<size_t>(j)])
                continue;
            double sp = use_pre
                            ? pre_power.snap_add_power(members[static_cast<size_t>(i)],
                                                       &snaps[static_cast<size_t>(j)])
                            : static_cast<double>(snap_points(master, bases[static_cast<size_t>(i)],
                                                              members[static_cast<size_t>(i)],
                                                              &snaps[static_cast<size_t>(j)]));
            snap_power[static_cast<size_t>(i * width + j)] = sp;
            best_snap_power[static_cast<size_t>(i)] =
                std::max(best_snap_power[static_cast<size_t>(i)], sp);
            skill_proxy[static_cast<size_t>(i)] =
                std::max(skill_proxy[static_cast<size_t>(i)],
                         slot_skill_proxy(slot_skill_table[static_cast<size_t>(i * width + j)]));
            opt.push_back(j);
        }
        if (allow_empty_snaps || constraints.no_snaps) {
            opt.push_back(-1);
            skill_proxy[static_cast<size_t>(i)] =
                std::max(skill_proxy[static_cast<size_t>(i)],
                         slot_skill_proxy(slot_skill_table[static_cast<size_t>(i * width + s)]));
        }
        std::sort(opt.begin(), opt.end(), [&](int a, int b) {
            const int ca = a < 0 ? s : a;
            const int cb = b < 0 ? s : b;
            const double pa = snap_power[static_cast<size_t>(i * width + ca)];
            const double pb = snap_power[static_cast<size_t>(i * width + cb)];
            if (pa != pb)
                return pa > pb;
            return a < b;
        });
        int nonreq_all = 0;
        std::unordered_map<int, size_t> cls_to_idx;
        std::unordered_map<int, int> cls_nonreq;
        for (int snap : opt) {
            const bool is_req = snap >= 0 && required_snap.count(snap) > 0;
            if (is_req || nonreq_all < 5) {
                top_all_snaps[static_cast<size_t>(i)].push_back(snap);
                if (!is_req)
                    ++nonreq_all;
            }
            const int col = snap < 0 ? s : snap;
            const int cls = snap_skill_class[static_cast<size_t>(i * width + col)];
            if (is_req || cls_nonreq[cls] < 5) {
                if (!is_req)
                    ++cls_nonreq[cls];
                const double p = snap_power[static_cast<size_t>(i * width + col)];
                auto it = cls_to_idx.find(cls);
                if (it == cls_to_idx.end()) {
                    cls_to_idx[cls] = member_classes[static_cast<size_t>(i)].size();
                    member_classes[static_cast<size_t>(i)].push_back({cls, {snap}, p});
                } else {
                    auto &grp = member_classes[static_cast<size_t>(i)][it->second];
                    grp.snaps.push_back(snap);
                    grp.max_power = std::max(grp.max_power, p);
                }
            }
        }
        std::sort(member_classes[static_cast<size_t>(i)].begin(),
                  member_classes[static_cast<size_t>(i)].end(),
                  [](const WsClassGroup &a, const WsClassGroup &b) {
                      if (a.max_power != b.max_power)
                          return a.max_power > b.max_power;
                      return a.cls < b.cls;
                  });
    }

    auto compute_base_powers = [&](const std::array<int, 5> &tm) -> std::array<double, 5> {
        std::array<double, 5> bp{};
        std::vector<MemberState> states;
        states.reserve(5);
        for (int m : tm)
            states.push_back(members[static_cast<size_t>(m)]);
        if (use_pre) {
            auto ld = pre_power.leader_slot_powers(states, 0);
            for (size_t sl = 0; sl < 5; ++sl)
                bp[sl] = fixed_power[static_cast<size_t>(tm[sl])] + ld[sl];
        } else {
            LeaderProfile prof = leader_profile(master, members[static_cast<size_t>(tm[0])]);
            auto pcts = leader_percents(master, prof, states, 0, music);
            for (size_t sl = 0; sl < 5; ++sl) {
                const auto &b = bases[static_cast<size_t>(tm[sl])];
                bp[sl] = static_cast<double>(b.fixed_points) +
                         static_cast<double>(leader_points(b, pcts[sl]));
            }
        }
        return bp;
    };

    std::vector<int> cand_snaps;
    cand_snaps.reserve(static_cast<size_t>(std::max(s, 1)));
    std::vector<char> snap_seen(static_cast<size_t>(std::max(s, 1)), 0);
    std::vector<int8_t> assigned;

    auto match_snaps = [&](const std::array<int, 5> &tm, const std::array<double, 5> &bp,
                           const std::array<const std::vector<int> *, 5> &slot_allowed,
                           std::vector<int> &out_snaps, double &out_total,
                           std::vector<double> &out_slots) -> bool {
        for (int req : required_snap) {
            bool covered = false;
            for (size_t sl = 0; sl < 5; ++sl) {
                if (std::find(slot_allowed[sl]->begin(), slot_allowed[sl]->end(), req) !=
                    slot_allowed[sl]->end()) {
                    covered = true;
                    break;
                }
            }
            if (!covered)
                return false;
        }
        const double base_sum = bp[0] + bp[1] + bp[2] + bp[3] + bp[4];
        cand_snaps.clear();
        std::array<bool, 5> allows_empty{false, false, false, false, false};
        for (size_t sl = 0; sl < 5; ++sl) {
            for (int snap : *slot_allowed[sl]) {
                if (snap < 0) {
                    allows_empty[sl] = true;
                    continue;
                }
                if (!snap_seen[static_cast<size_t>(snap)]) {
                    snap_seen[static_cast<size_t>(snap)] = 1;
                    cand_snaps.push_back(snap);
                }
            }
        }
        for (int snap : cand_snaps)
            snap_seen[static_cast<size_t>(snap)] = 0;

        const size_t num_cands = cand_snaps.size();
        constexpr int kStates = 32;
        constexpr double kNegInf = -std::numeric_limits<double>::infinity();
        std::array<double, kStates> dp{}, next_dp{};
        dp.fill(kNegInf);
        dp[0] = base_sum;
        if (assigned.size() < num_cands * kStates)
            assigned.resize(num_cands * kStates);

        for (size_t ci = 0; ci < num_cands; ++ci) {
            const int snap = cand_snaps[ci];
            const bool is_req = required_snap.count(snap) > 0;
            const size_t off = ci * kStates;
            if (is_req) {
                next_dp.fill(kNegInf);
                std::fill(assigned.begin() + static_cast<ptrdiff_t>(off),
                          assigned.begin() + static_cast<ptrdiff_t>(off + kStates),
                          static_cast<int8_t>(-2));
            } else {
                next_dp = dp;
                for (int mask = 0; mask < kStates; ++mask)
                    assigned[off + static_cast<size_t>(mask)] =
                        (dp[static_cast<size_t>(mask)] > kNegInf) ? -1 : -2;
            }
            uint8_t elig = 0;
            for (size_t sl = 0; sl < 5; ++sl) {
                if (std::find(slot_allowed[sl]->begin(), slot_allowed[sl]->end(), snap) !=
                    slot_allowed[sl]->end())
                    elig |= static_cast<uint8_t>(1u << sl);
            }
            for (int mask = 0; mask < kStates; ++mask) {
                if (dp[static_cast<size_t>(mask)] == kNegInf)
                    continue;
                const uint8_t free_elig = static_cast<uint8_t>(elig & (~mask));
                for (int sl = 0; sl < 5; ++sl) {
                    if (!(free_elig & (1u << sl)))
                        continue;
                    const int target = mask | (1 << sl);
                    const double val =
                        dp[static_cast<size_t>(mask)] +
                        snap_power[static_cast<size_t>(tm[static_cast<size_t>(sl)] * width + snap)];
                    if (val > next_dp[static_cast<size_t>(target)]) {
                        next_dp[static_cast<size_t>(target)] = val;
                        assigned[off + static_cast<size_t>(target)] = static_cast<int8_t>(sl);
                    }
                }
            }
            dp = next_dp;
        }
        int best_mask = -1;
        double best_val = kNegInf;
        for (int mask = 0; mask < kStates; ++mask) {
            if (dp[static_cast<size_t>(mask)] == kNegInf)
                continue;
            bool ok = true;
            double total_p = dp[static_cast<size_t>(mask)];
            for (int sl = 0; sl < 5; ++sl) {
                if (!(mask & (1 << sl))) {
                    if (!allows_empty[static_cast<size_t>(sl)]) {
                        ok = false;
                        break;
                    }
                    total_p +=
                        snap_power[static_cast<size_t>(tm[static_cast<size_t>(sl)] * width + s)];
                }
            }
            if (ok && total_p > best_val) {
                best_val = total_p;
                best_mask = mask;
            }
        }
        if (best_mask < 0)
            return false;
        out_snaps.assign(5, -1);
        int cur_mask = best_mask;
        for (int ci = static_cast<int>(num_cands) - 1; ci >= 0; --ci) {
            const int8_t sl =
                assigned[static_cast<size_t>(ci) * kStates + static_cast<size_t>(cur_mask)];
            if (sl >= 0) {
                out_snaps[static_cast<size_t>(sl)] = cand_snaps[static_cast<size_t>(ci)];
                cur_mask ^= (1 << sl);
            }
        }
        out_total = best_val;
        out_slots.resize(5);
        for (size_t sl = 0; sl < 5; ++sl) {
            const int col = out_snaps[sl] < 0 ? s : out_snaps[sl];
            out_slots[sl] = bp[sl] + snap_power[static_cast<size_t>(tm[sl] * width + col)];
        }
        return true;
    };

    auto record_hit = [&](const Team &team, SearchExact &&ex) -> bool {
        const std::string sk = team_set_key(team, members);
        size_t existing = out.hits.size();
        for (size_t i = 0; i < out.hits.size(); ++i) {
            if (out.hits[i].set_key == sk) {
                existing = i;
                break;
            }
        }
        if (existing < out.hits.size() && out.hits[existing].key >= ex.key)
            return false;
        const bool enters = existing < out.hits.size() ||
                            static_cast<int>(out.hits.size()) < eff_k ||
                            ex.key > out.hits.back().key;
        if (!enters)
            return false;
        SearchHit hit;
        hit.team = team;
        hit.set_key = sk;
        hit.power = ex.power;
        hit.slot_powers = std::move(ex.slot_powers);
        hit.totals = {ex.power, 0.0, 0.0, 0.0};
        hit.key = ex.key;
        hit.detail = std::move(ex.detail);
        if (existing < out.hits.size())
            out.hits.erase(out.hits.begin() + static_cast<ptrdiff_t>(existing));
        size_t at = out.hits.size();
        while (at > 0 && out.hits[at - 1].key < hit.key)
            --at;
        out.hits.insert(out.hits.begin() + static_cast<ptrdiff_t>(at), std::move(hit));
        if (static_cast<int>(out.hits.size()) > eff_k)
            out.hits.resize(static_cast<size_t>(eff_k));
        return true;
    };

    struct EvalAttempt {
        bool valid = false;
        double key = -std::numeric_limits<double>::infinity();
        Team team;
        std::array<double, 5> bp{};
        bool entered_topk = false;
    };

    auto eval_team_fast = [&](const std::array<int, 5> &tm) -> EvalAttempt {
        EvalAttempt res;
        res.bp = compute_base_powers(tm);
        std::array<const std::vector<int> *, 5> allowed{};
        for (size_t sl = 0; sl < 5; ++sl)
            allowed[sl] = &top_all_snaps[static_cast<size_t>(tm[sl])];
        double tot_p = 0.0;
        std::vector<double> slot_p;
        std::vector<int> matched;
        if (!match_snaps(tm, res.bp, allowed, matched, tot_p, slot_p))
            return res;
        res.team.members.assign(tm.begin(), tm.end());
        res.team.snaps = std::move(matched);
        ++out.candidates;
        SearchExact ex = can_pass_power ? eval_fn(res.team, &tot_p, &slot_p)
                                        : eval_fn(res.team, nullptr, nullptr);
        ++out.exact;
        res.valid = true;
        res.key = ex.key;
        res.entered_topk = record_hit(res.team, std::move(ex));
        return res;
    };

    auto optimize_snaps_1opt = [&](const std::array<int, 5> &tm, const std::array<double, 5> &bp,
                                   Team &cur_team, double &cur_key) {
        std::array<size_t, 5> cls_idx{};
        for (size_t sl = 0; sl < 5; ++sl) {
            const int m = tm[sl];
            const int col = cur_team.snaps[sl] < 0 ? s : cur_team.snaps[sl];
            const int target_cls = snap_skill_class[static_cast<size_t>(m * width + col)];
            const auto &mcls = member_classes[static_cast<size_t>(m)];
            for (size_t ci = 0; ci < mcls.size(); ++ci) {
                if (mcls[ci].cls == target_cls) {
                    cls_idx[sl] = ci;
                    break;
                }
            }
        }
        std::vector<int> matched;
        std::vector<double> slot_p;
        for (int pass = 0; pass < 2; ++pass) {
            bool improved = false;
            for (size_t sl = 0; sl < 5; ++sl) {
                if (ws_expired())
                    return;
                const auto &mcls = member_classes[static_cast<size_t>(tm[sl])];
                if (mcls.size() <= 1)
                    continue;
                size_t best_ci = cls_idx[sl];
                for (size_t ci = 0; ci < mcls.size(); ++ci) {
                    if (ci == cls_idx[sl])
                        continue;
                    if (ws_expired())
                        return;
                    std::array<const std::vector<int> *, 5> allowed{};
                    for (size_t r = 0; r < 5; ++r) {
                        const size_t use_ci = (r == sl) ? ci : cls_idx[r];
                        allowed[r] = &member_classes[static_cast<size_t>(tm[r])][use_ci].snaps;
                    }
                    double tot_p = 0.0;
                    if (!match_snaps(tm, bp, allowed, matched, tot_p, slot_p))
                        continue;
                    Team cand;
                    cand.members.assign(tm.begin(), tm.end());
                    cand.snaps = matched;
                    ++out.candidates;
                    SearchExact ex = can_pass_power ? eval_fn(cand, &tot_p, &slot_p)
                                                    : eval_fn(cand, nullptr, nullptr);
                    ++out.exact;
                    const double val = ex.key;
                    record_hit(cand, std::move(ex));
                    if (val > cur_key) {
                        cur_key = val;
                        cur_team = std::move(cand);
                        best_ci = ci;
                        improved = true;
                    }
                }
                cls_idx[sl] = best_ci;
            }
            if (!improved)
                break;
        }
    };

    // Valid leaders filtered by required-member character conflict.
    std::vector<int> leaders;
    for (int i = 0; i < n; ++i) {
        if (excluded_member[static_cast<size_t>(i)] ||
            (constraints.leader.has_value() && *constraints.leader != i))
            continue;
        const int32_t lc = members[static_cast<size_t>(i)].card.character_id;
        bool conflict = false;
        for (int req : req_member_list) {
            if (req != i && members[static_cast<size_t>(req)].card.character_id == lc) {
                conflict = true;
                break;
            }
        }
        if (!conflict)
            leaders.push_back(i);
    }
    if (leaders.empty())
        return out;

    // Score leaders via multi-probe power estimate.
    std::vector<double> lscore(static_cast<size_t>(n), 0.0);
    for (int l : leaders) {
        const MemberState &lm = members[static_cast<size_t>(l)];
        std::map<int32_t, int> best_by_char;
        for (int m = 0; m < n; ++m) {
            if (m == l || excluded_member[static_cast<size_t>(m)])
                continue;
            const int32_t ch = members[static_cast<size_t>(m)].card.character_id;
            if (ch == lm.card.character_id)
                continue;
            const double pm =
                fixed_power[static_cast<size_t>(m)] + best_snap_power[static_cast<size_t>(m)];
            auto it = best_by_char.find(ch);
            if (it == best_by_char.end() ||
                pm > fixed_power[static_cast<size_t>(it->second)] +
                         best_snap_power[static_cast<size_t>(it->second)])
                best_by_char[ch] = m;
        }
        std::vector<int> mates;
        for (const auto &[_, m] : best_by_char)
            mates.push_back(m);
        std::sort(mates.begin(), mates.end(), [&](int a, int b) {
            return fixed_power[static_cast<size_t>(a)] + best_snap_power[static_cast<size_t>(a)] >
                   fixed_power[static_cast<size_t>(b)] + best_snap_power[static_cast<size_t>(b)];
        });
        double est = fixed_power[static_cast<size_t>(l)] + best_snap_power[static_cast<size_t>(l)];
        auto probe = [&](std::array<int, 5> tm) -> double {
            auto bp = compute_base_powers(tm);
            double sum = 0.0;
            for (size_t sl = 0; sl < 5; ++sl)
                sum += bp[sl] + best_snap_power[static_cast<size_t>(tm[sl])];
            return sum;
        };
        if (mates.size() >= 4) {
            est = std::max(est, probe({l, mates[0], mates[1], mates[2], mates[3]}));
            std::vector<int> sb, st;
            for (int m : mates) {
                if (members[static_cast<size_t>(m)].card.band_id == lm.card.band_id)
                    sb.push_back(m);
                if (members[static_cast<size_t>(m)].card.card_type == lm.card.card_type)
                    st.push_back(m);
            }
            if (sb.size() >= 4)
                est = std::max(est, probe({l, sb[0], sb[1], sb[2], sb[3]}));
            if (st.size() >= 4)
                est = std::max(est, probe({l, st[0], st[1], st[2], st[3]}));
        }
        lscore[static_cast<size_t>(l)] = est;
    }
    std::sort(leaders.begin(), leaders.end(), [&](int a, int b) {
        if (lscore[static_cast<size_t>(a)] != lscore[static_cast<size_t>(b)])
            return lscore[static_cast<size_t>(a)] > lscore[static_cast<size_t>(b)];
        return a < b;
    });

    std::unordered_set<int32_t> active_missions(chart.setup.missions.begin(),
                                                chart.setup.missions.end());
    auto build_greedy_team = [&](int l, int strategy, std::array<int, 5> &tm_out) -> bool {
        std::vector<int> picked;
        std::unordered_set<int32_t> used_chars;
        picked.push_back(l);
        used_chars.insert(members[static_cast<size_t>(l)].card.character_id);
        for (int req : req_member_list) {
            if (req == l)
                continue;
            const int32_t ch = members[static_cast<size_t>(req)].card.character_id;
            if (!used_chars.insert(ch).second)
                return false;
            picked.push_back(req);
        }
        if (picked.size() > 5)
            return false;
        const MemberState &lm = members[static_cast<size_t>(l)];
        std::vector<int> pool;
        for (int m = 0; m < n; ++m) {
            if (excluded_member[static_cast<size_t>(m)] ||
                used_chars.count(members[static_cast<size_t>(m)].card.character_id))
                continue;
            pool.push_back(m);
        }
        auto rank_val = [&](int m) -> double {
            double v =
                fixed_power[static_cast<size_t>(m)] + best_snap_power[static_cast<size_t>(m)];
            const auto &mc = members[static_cast<size_t>(m)];
            if (strategy == 1 && mc.card.band_id == lm.card.band_id)
                v += 1e7;
            else if (strategy == 2 && mc.card.card_type == lm.card.card_type)
                v += 1e7;
            else if (strategy == 3 && (mc.gekiso_mission_type == kMAll ||
                                       active_missions.count(mc.gekiso_mission_type)))
                v += 1e7;
            else if (strategy == 4)
                v += skill_proxy[static_cast<size_t>(m)] * 0.05;
            return v;
        };
        std::sort(pool.begin(), pool.end(), [&](int a, int b) {
            const double va = rank_val(a);
            const double vb = rank_val(b);
            if (va != vb)
                return va > vb;
            return a < b;
        });
        for (int m : pool) {
            if (picked.size() == 5)
                break;
            const int32_t ch = members[static_cast<size_t>(m)].card.character_id;
            if (used_chars.insert(ch).second)
                picked.push_back(m);
        }
        if (picked.size() != 5)
            return false;
        for (size_t i = 0; i < 5; ++i)
            tm_out[i] = picked[i];
        return true;
    };

    auto canon_order_key = [](std::array<int, 5> tm) -> std::array<int, 5> {
        std::sort(tm.begin() + 1, tm.end());
        return tm;
    };
    std::set<std::array<int, 5>> seen_orders;

    const auto phase1_deadline =
        ws_start + std::chrono::duration_cast<clock::duration>(
                       std::chrono::duration<double, std::milli>(std::max(1.0, budget_ms * 0.15)));
    const auto phase2_deadline =
        ws_start + std::chrono::duration_cast<clock::duration>(
                       std::chrono::duration<double, std::milli>(std::max(1.0, budget_ms * 0.45)));

    // Phase 1: fast breadth across top leaders x strategies (1 eval each) to populate Top-K sets.
    const size_t max_seed_leaders = std::min<size_t>(leaders.size(), 20);
    for (size_t li = 0; li < max_seed_leaders && clock::now() < phase1_deadline; ++li) {
        const int l = leaders[li];
        for (int strat = 0; strat < 5 && clock::now() < phase1_deadline; ++strat) {
            std::array<int, 5> tm{};
            if (!build_greedy_team(l, strat, tm))
                continue;
            const auto ck = canon_order_key(tm);
            if (!seen_orders.insert(ck).second)
                continue;
            (void)eval_team_fast(tm);
        }
    }

    // Phase 2: 1-opt member swaps & leader rotation (fast eval) to diversify & improve member sets.
    std::vector<int> all_valid_members;
    for (int m = 0; m < n; ++m)
        if (!excluded_member[static_cast<size_t>(m)])
            all_valid_members.push_back(m);
    std::sort(all_valid_members.begin(), all_valid_members.end(), [&](int a, int b) {
        return fixed_power[static_cast<size_t>(a)] + best_snap_power[static_cast<size_t>(a)] >
               fixed_power[static_cast<size_t>(b)] + best_snap_power[static_cast<size_t>(b)];
    });

    const size_t seeds_to_swap =
        std::min<size_t>(out.hits.size(), static_cast<size_t>(std::max(eff_k, 6)));
    std::vector<Team> initial_seeds;
    initial_seeds.reserve(seeds_to_swap);
    for (size_t i = 0; i < seeds_to_swap; ++i)
        initial_seeds.push_back(out.hits[i].team);

    for (const Team &seed_team : initial_seeds) {
        if (clock::now() >= phase2_deadline)
            break;
        std::array<int, 5> cur_tm{};
        for (size_t i = 0; i < 5; ++i)
            cur_tm[i] = seed_team.members[i];
        double best_k = -std::numeric_limits<double>::infinity();
        for (const auto &h : out.hits) {
            if (h.team.members == seed_team.members) {
                best_k = h.key;
                break;
            }
        }
        for (int round = 0; round < 2 && clock::now() < phase2_deadline; ++round) {
            bool any_improved = false;
            if (!constraints.leader.has_value()) {
                for (size_t sl = 1; sl < 5 && clock::now() < phase2_deadline; ++sl) {
                    std::array<int, 5> rot = cur_tm;
                    std::swap(rot[0], rot[sl]);
                    if (!seen_orders.insert(canon_order_key(rot)).second)
                        continue;
                    EvalAttempt att = eval_team_fast(rot);
                    if (att.valid && att.key > best_k) {
                        best_k = att.key;
                        cur_tm = rot;
                        any_improved = true;
                    }
                }
            }
            for (size_t sl = 0; sl < 5 && clock::now() < phase2_deadline; ++sl) {
                if (sl == 0 && constraints.leader.has_value())
                    continue;
                if (required_member[static_cast<size_t>(cur_tm[sl])])
                    continue;
                std::unordered_set<int32_t> other_chars;
                for (size_t r = 0; r < 5; ++r)
                    if (r != sl)
                        other_chars.insert(
                            members[static_cast<size_t>(cur_tm[r])].card.character_id);
                const MemberState &lm = members[static_cast<size_t>(cur_tm[0])];
                std::vector<int> cands;
                cands.reserve(24);
                for (int m : all_valid_members) {
                    if (m == cur_tm[sl])
                        continue;
                    if (other_chars.count(members[static_cast<size_t>(m)].card.character_id))
                        continue;
                    cands.push_back(m);
                }
                std::sort(cands.begin(), cands.end(), [&](int a, int b) {
                    auto score_c = [&](int m) {
                        double sc = fixed_power[static_cast<size_t>(m)] +
                                    best_snap_power[static_cast<size_t>(m)] +
                                    skill_proxy[static_cast<size_t>(m)] * 0.02;
                        const auto &mc = members[static_cast<size_t>(m)];
                        if (sl > 0 && mc.card.band_id == lm.card.band_id)
                            sc += 15000.0;
                        if (sl > 0 && mc.card.card_type == lm.card.card_type)
                            sc += 15000.0;
                        if (mc.gekiso_mission_type == kMAll ||
                            active_missions.count(mc.gekiso_mission_type))
                            sc += 10000.0;
                        return sc;
                    };
                    return score_c(a) > score_c(b);
                });
                const size_t try_lim = std::min<size_t>(cands.size(), 12);
                for (size_t ci = 0; ci < try_lim && clock::now() < phase2_deadline; ++ci) {
                    std::array<int, 5> next_tm = cur_tm;
                    next_tm[sl] = cands[ci];
                    if (!seen_orders.insert(canon_order_key(next_tm)).second)
                        continue;
                    EvalAttempt att = eval_team_fast(next_tm);
                    if (att.valid && att.key > best_k) {
                        best_k = att.key;
                        cur_tm = next_tm;
                        any_improved = true;
                    }
                }
            }
            if (!any_improved)
                break;
        }
    }

    // Phase 3: snapshot 1-opt skill-class coordinate ascent on top distinct member sets.
    const size_t polish_count =
        std::min<size_t>(out.hits.size(), static_cast<size_t>(std::max(eff_k, 5)));
    std::vector<Team> polish_teams;
    std::vector<double> polish_keys;
    polish_teams.reserve(polish_count);
    polish_keys.reserve(polish_count);
    for (size_t i = 0; i < polish_count; ++i) {
        polish_teams.push_back(out.hits[i].team);
        polish_keys.push_back(out.hits[i].key);
    }
    for (size_t i = 0; i < polish_teams.size() && !ws_expired(); ++i) {
        std::array<int, 5> tm{};
        for (size_t sl = 0; sl < 5; ++sl)
            tm[sl] = polish_teams[i].members[sl];
        auto bp = compute_base_powers(tm);
        optimize_snaps_1opt(tm, bp, polish_teams[i], polish_keys[i]);
    }

    return out;
}

SearchOutput gekiso_search(const EngineMaster &master, const PlayerState &player,
                           const std::vector<MemberState> &members,
                           const std::vector<SnapState> &snaps, const MusicView *music,
                           const GekisoChart &chart, const std::vector<int32_t> &seeds,
                           const std::string &criterion, const SearchConstraints &constraints,
                           int k, double time_limit_ms, const Json *prob, bool allow_empty_snaps,
                           bool is_oracle) {
    using clock = std::chrono::steady_clock;
    const auto search_started = clock::now();
    PrecomputedProbPower pre_power(prob);

    const size_t n = members.size();
    const size_t s = snaps.size();
    const size_t width = s + 1;

    std::vector<SlotSkill> slot_skill_table(n * width);
    std::vector<char> has_gk_skill(n, 0);
    for (size_t i = 0; i < n; ++i) {
        has_gk_skill[i] = (members[i].card.gekiso_skill_id != 0 &&
                           master.gekiso_skills.count(members[i].card.gekiso_skill_id))
                              ? 1
                              : 0;
        for (size_t j = 0; j < s; ++j)
            slot_skill_table[i * width + j] = resolve_slot_skill(master, members[i], &snaps[j]);
        slot_skill_table[i * width + s] = resolve_slot_skill(master, members[i], nullptr);
    }

    std::vector<int> snap_gk_sup_id(width, 0);
    std::vector<std::array<int32_t, 4>> gk_sup_sigs;
    for (size_t j = 0; j < s; ++j) {
        std::array<int32_t, 4> sig{
            snaps[j].card.gekiso_support_skill_ids[0],
            snaps[j].card.gekiso_support_skill_ids[0] ? snaps[j].gekiso_skill_levels[0] : 0,
            snaps[j].card.gekiso_support_skill_ids[1],
            snaps[j].card.gekiso_support_skill_ids[1] ? snaps[j].gekiso_skill_levels[1] : 0};
        if (sig[0] == 0 && sig[2] == 0) {
            snap_gk_sup_id[j] = 0;
        } else {
            size_t found = 0;
            while (found < gk_sup_sigs.size() && gk_sup_sigs[found] != sig)
                ++found;
            if (found == gk_sup_sigs.size())
                gk_sup_sigs.push_back(sig);
            snap_gk_sup_id[j] = static_cast<int>(found + 1);
        }
    }

    std::vector<int> snap_skill_class(n * width, 0);
    for (size_t i = 0; i < n; ++i) {
        std::vector<size_t> rep_cols;
        for (size_t col = 0; col < width; ++col) {
            int gk_id = has_gk_skill[i] ? snap_gk_sup_id[col] : 0;
            const SlotSkill &sk = slot_skill_table[i * width + col];
            int assigned = -1;
            for (size_t c_idx = 0; c_idx < rep_cols.size(); ++c_idx) {
                size_t rcol = rep_cols[c_idx];
                int rgk = has_gk_skill[i] ? snap_gk_sup_id[rcol] : 0;
                if (gk_id == rgk && equal_slot_skill(sk, slot_skill_table[i * width + rcol])) {
                    assigned = static_cast<int>(c_idx);
                    break;
                }
            }
            if (assigned < 0) {
                assigned = static_cast<int>(rep_cols.size());
                rep_cols.push_back(col);
            }
            snap_skill_class[i * width + col] = assigned;
        }
    }

    const int eff_k = std::max(k, 10);

    auto make_evaluator = [&]() {
        auto local_curve_cache = std::make_shared<std::unordered_map<std::string, LuckCurve>>();
        auto last_members =
            std::make_shared<std::array<int, 5>>(std::array<int, 5>{-1, -1, -1, -1, -1});
        auto team_ctx_cache = std::make_shared<std::vector<std::pair<uint64_t, GekisoContext>>>();
        return [&, local_curve_cache, last_members,
                team_ctx_cache](const Team &team, const double *pre_total,
                                const std::vector<double> *pre_slots) -> SearchExact {
            SearchExact result;
            std::vector<SlotSkill> skills;
            skills.reserve(team.members.size());
            std::array<int, 5> cur_members{-1, -1, -1, -1, -1};
            uint64_t gk_sup_key = 0;
            for (size_t slot = 0; slot < team.members.size(); ++slot) {
                size_t m_idx = static_cast<size_t>(team.members[slot]);
                size_t col = team.snaps[slot] < 0 ? s : static_cast<size_t>(team.snaps[slot]);
                skills.push_back(slot_skill_table[m_idx * width + col]);
                if (slot < 5)
                    cur_members[slot] = team.members[slot];
                uint64_t eff_gk =
                    static_cast<uint64_t>(has_gk_skill[m_idx] ? snap_gk_sup_id[col] : 0);
                gk_sup_key = (gk_sup_key << 12) | (eff_gk & 0xFFFu);
            }

            if (cur_members != *last_members) {
                *last_members = cur_members;
                team_ctx_cache->clear();
            }
            const GekisoContext *ctx_ptr = nullptr;
            for (const auto &entry : *team_ctx_cache) {
                if (entry.first == gk_sup_key) {
                    ctx_ptr = &entry.second;
                    break;
                }
            }
            if (!ctx_ptr) {
                std::vector<Performer> deck;
                deck.reserve(team.members.size());
                for (size_t slot = 0; slot < team.members.size(); ++slot) {
                    const MemberState &member = members[static_cast<size_t>(team.members[slot])];
                    const SnapState *snap = team.snaps[slot] < 0
                                                ? nullptr
                                                : &snaps[static_cast<size_t>(team.snaps[slot])];
                    deck.push_back(make_performer(master, member, snap));
                }
                team_ctx_cache->emplace_back(
                    gk_sup_key, gekiso_context(master, chart, deck, seeds, *local_curve_cache));
                ctx_ptr = &team_ctx_cache->back().second;
            }

            if (pre_total && pre_slots) {
                result.power = *pre_total;
                result.slot_powers = *pre_slots;
            } else {
                std::vector<MemberState> ordered;
                std::vector<const SnapState *> selected;
                ordered.reserve(team.members.size());
                selected.reserve(team.members.size());
                for (size_t slot = 0; slot < team.members.size(); ++slot) {
                    ordered.push_back(members[static_cast<size_t>(team.members[slot])]);
                    selected.push_back(team.snaps[slot] < 0
                                           ? nullptr
                                           : &snaps[static_cast<size_t>(team.snaps[slot])]);
                }
                if (pre_power.valid) {
                    TeamPowerResult pr = pre_power.calc(ordered, selected, 0);
                    result.power = pr.total;
                    result.slot_powers = std::move(pr.slots);
                } else {
                    TeamPower power = team_power(prob, master, player, ordered, selected, 0, music);
                    result.power = power.total;
                    result.slot_powers = std::move(power.slots);
                }
            }
            result.detail.scores =
                score_orders_gekiso(master, chart, *ctx_ptr, result.power, skills);
            result.key = order_key(result.detail.scores, criterion);
            result.detail.skills = std::move(skills);
            result.detail.seeds = ctx_ptr->seeds;
            return result;
        };
    };

    // Fast warm-start heuristic on large pools when bounded in non-oracle mode so strong Top-K
    // teams are found immediately before exact sharded DFS spends the remaining budget.
    WarmStartResult ws_res;
    double exact_time_limit_ms = time_limit_ms;
    if (!is_oracle && time_limit_ms > 0.0 && (n >= 8 || s >= 10)) {
        const double ws_budget = std::min(time_limit_ms * 0.40, 1500.0);
        auto ws_eval = make_evaluator();
        ws_res = run_gekisou_warm_start(master, player, members, snaps, music, chart, pre_power,
                                        slot_skill_table, snap_skill_class, constraints,
                                        allow_empty_snaps, eff_k, ws_budget, ws_eval);
        const double spent_ms =
            std::chrono::duration<double, std::milli>(clock::now() - search_started).count();
        exact_time_limit_ms = std::max(1.0, time_limit_ms - spent_ms);
    }

    auto run_shard = [&](std::optional<int> shard_idx,
                         std::optional<int> shard_cnt) -> SearchOutput {
        auto eval_team = make_evaluator();

        SearchInput input;
        input.master = &master;
        input.player = &player;
        input.members = &members;
        input.snaps = &snaps;
        input.music = music;
        input.pre_power = pre_power.valid ? &pre_power : nullptr;
        input.snap_skill_class = &snap_skill_class;
        input.prune_snap_classes = true;
        input.constraints = constraints;
        input.constraints.allow_empty_snaps = allow_empty_snaps;
        input.k = eff_k;
        input.time_limit_ms = exact_time_limit_ms;
        input.shard_index = shard_idx;
        input.shard_count = shard_cnt;
        input.skill = [](int, int) {
            return std::make_pair(0.0, std::numeric_limits<double>::max() / 16);
        };
        input.member_bonus = [](int) { return 0.0; };
        input.snap_bonus = [](int) { return 0.0; };
        input.bound = [](const Totals &) { return std::numeric_limits<double>::max() / 16; };
        input.interval = [](const Team &, const Totals &) {
            return std::make_pair(0.0, std::numeric_limits<double>::max() / 16);
        };
        input.exact = [&](const Team &team) { return eval_team(team, nullptr, nullptr); };
        input.exact_with_power = [&](const Team &team, double total_power,
                                     const std::vector<double> &slot_powers) {
            return eval_team(team, &total_power, &slot_powers);
        };
        return search_teams(input);
    };

    auto merge_hit = [&](std::vector<SearchHit> &target, SearchHit &&hit) {
        if (hit.set_key.empty())
            hit.set_key = team_set_key(hit.team, members);
        size_t existing = target.size();
        for (size_t i = 0; i < target.size(); ++i) {
            if (target[i].set_key == hit.set_key) {
                existing = i;
                break;
            }
        }
        if (existing < target.size() && target[existing].key >= hit.key)
            return;
        if (existing < target.size())
            target.erase(target.begin() + static_cast<ptrdiff_t>(existing));
        size_t at = target.size();
        while (at > 0 && target[at - 1].key < hit.key)
            --at;
        target.insert(target.begin() + static_cast<ptrdiff_t>(at), std::move(hit));
        if (static_cast<int>(target.size()) > eff_k)
            target.resize(static_cast<size_t>(eff_k));
    };

    int leader_candidates = constraints.leader ? 1 : static_cast<int>(n);
    unsigned int hw = std::thread::hardware_concurrency();
    int num_shards = std::min(leader_candidates, static_cast<int>(hw > 0 ? hw : 4));
    num_shards = std::min(num_shards, 8);
    std::vector<SearchOutput> shard_outputs;
    if (num_shards <= 1) {
        shard_outputs.push_back(run_shard(std::nullopt, std::nullopt));
    } else {
        shard_outputs.resize(static_cast<size_t>(num_shards));
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(num_shards - 1));
        std::exception_ptr worker_exception;
        std::mutex exception_mtx;
        for (int idx = 1; idx < num_shards; ++idx) {
            workers.emplace_back([&, idx]() {
                try {
                    shard_outputs[static_cast<size_t>(idx)] = run_shard(idx, num_shards);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(exception_mtx);
                    if (!worker_exception)
                        worker_exception = std::current_exception();
                }
            });
        }
        try {
            shard_outputs[0] = run_shard(0, num_shards);
        } catch (...) {
            std::lock_guard<std::mutex> lock(exception_mtx);
            if (!worker_exception)
                worker_exception = std::current_exception();
        }
        for (auto &w : workers)
            w.join();
        if (worker_exception)
            std::rethrow_exception(worker_exception);
    }

    SearchOutput merged;
    merged.proven = true;
    merged.stats.candidates = ws_res.candidates;
    merged.stats.exact = ws_res.exact;
    for (auto &hit : ws_res.hits)
        merge_hit(merged.hits, std::move(hit));

    for (auto &out : shard_outputs) {
        merged.proven = merged.proven && out.proven;
        if (out.has_bound) {
            merged.bound = merged.has_bound ? std::max(merged.bound, out.bound) : out.bound;
            merged.has_bound = true;
        }
        merged.stats.leaders += out.stats.leaders;
        merged.stats.member_nodes += out.stats.member_nodes;
        merged.stats.snap_nodes += out.stats.snap_nodes;
        merged.stats.candidates += out.stats.candidates;
        merged.stats.exact += out.stats.exact;
        for (auto &hit : out.hits)
            merge_hit(merged.hits, std::move(hit));
    }
    merged.stats.elapsed_ms =
        std::chrono::duration<double, std::milli>(clock::now() - search_started).count();
    return merged;
}

} // namespace gekisou_detail
} // namespace deckcalc
