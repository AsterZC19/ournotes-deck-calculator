#include "gekisou.hpp"

#include "gekisou_internal.hpp"

#include <cstdio>

namespace deckcalc {
namespace {

using namespace gekisou_detail;

// Input parsing helpers.
bool json_truthy(const Json *value, bool fallback = false) {
    if (!value || value->is_null())
        return fallback;
    if (value->is_bool())
        return value->as_bool();
    if (value->is_number())
        return value->as_double() != 0.0;
    return fallback;
}

std::string json_string(const Json *value, const std::string &fallback = "") {
    if (!value || !value->is_string())
        return fallback;
    return value->as_string();
}

std::vector<int32_t> json_id_list(const Json *value) {
    std::vector<int32_t> out;
    if (!value || !value->is_array())
        return out;
    for (const Json &item : value->as_array()) {
        if (item.is_number())
            out.push_back(json_i32(&item));
        else if (item.is_object())
            out.push_back(row_i32(item, "id"));
    }
    return out;
}

std::optional<int32_t> optional_int(const Json *object, const char *key) {
    const Json *value = object ? object->find(key) : nullptr;
    if (!value || value->is_null() || !value->is_number())
        return std::nullopt;
    return json_i32(value);
}

void validate_chart_fevers(const ChartSource &chart) {
    if (chart.fever_ms.size() < 1 || chart.fever_ms.size() > 3) {
        throw SpecError("激奏谱面必须包含 1 到 3 个 Fever 区间 (chart.fevers)，当前: " +
                        std::to_string(chart.fever_ms.size()));
    }
    for (const auto &[start, end] : chart.fever_ms) {
        if (start >= end)
            throw SpecError("Fever 起始时间必须严格小于结束时间: [" + std::to_string(start) + ", " +
                            std::to_string(end) + "]");
        if (start < 0)
            throw SpecError("Fever 起始时间必须非负: [" + std::to_string(start) + ", " +
                            std::to_string(end) + "]");
    }
    std::vector<std::pair<int32_t, int32_t>> sorted = chart.fever_ms;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i].first < sorted[i - 1].second)
            throw SpecError("Fever 区间不能重叠: [" + std::to_string(sorted[i - 1].first) + ", " +
                            std::to_string(sorted[i - 1].second) + "] 与 [" +
                            std::to_string(sorted[i].first) + ", " +
                            std::to_string(sorted[i].second) + "]");
}

ChartSource normalize_chart_source(const Json &chart_input) {
    Json raw = chart_input;
    if (chart_input.is_string())
        raw = Json::parse_file(chart_input.as_string());
    if (!raw.is_object())
        throw SpecError("未知的谱面输入格式");
    ChartSource source;
    if (json_string(raw.find("format")) == "nnnotes.live-score/1") {
        int32_t last = row_i32(raw, "lastNoteTimeMs");
        source.duration_ms = last + 1000;
        if (const Json *notes = raw.find("notes"); notes && notes->is_array()) {
            for (const Json &note : notes->as_array()) {
                ChartNote item;
                item.time_ms = row_i32(note, "timeMs");
                item.op = row_i32(note, "op");
                item.judgement_type = 0;
                source.enumeration.push_back(item);
            }
        }
        if (const Json *skills = raw.find("skill"); skills && skills->is_array())
            for (const Json &skill : skills->as_array())
                source.skill_times_ms.push_back(row_i32(skill, "timeMs"));
        if (const Json *fevers = raw.find("fever"); fevers && fevers->is_array()) {
            for (const Json &fever : fevers->as_array()) {
                const Json *start = fever.find("start");
                const Json *end = fever.find("end");
                if (start && end)
                    source.fever_ms.emplace_back(row_i32(*start, "timeMs"),
                                                 row_i32(*end, "timeMs"));
            }
        }
        return source;
    }
    const Json *notes = raw.find("notes");
    const Json *skills = raw.find("skill_times_ms");
    if (notes && notes->is_array() && skills && skills->is_array()) {
        int32_t last = 0;
        for (const Json &note : notes->as_array())
            last = std::max(last, row_i32(note, "t", 0, "timeMs"));
        source.duration_ms = row_i32(raw, "duration_ms", last + 1000);
        for (const Json &note : notes->as_array()) {
            ChartNote item;
            item.time_ms = row_i32(note, "t", 0, "timeMs");
            item.op = row_i32(note, "op");
            item.judgement_type = 0;
            source.enumeration.push_back(item);
        }
        for (const Json &skill : skills->as_array())
            source.skill_times_ms.push_back(json_i32(&skill));
        const Json *fevers = raw.find("fevers");
        if (!fevers || !fevers->is_array())
            fevers = raw.find("fever");
        if (fevers && fevers->is_array()) {
            for (const Json &fever : fevers->as_array()) {
                if (fever.is_array()) {
                    if (fever.size() != 2)
                        throw SpecError("Fever 区间必须为 [startMs, endMs] 格式");
                    source.fever_ms.emplace_back(json_i32(&fever.at(0)), json_i32(&fever.at(1)));
                } else if (fever.is_object()) {
                    int32_t start = row_i32(fever, "start_ms");
                    int32_t end = row_i32(fever, "end_ms");
                    if (const Json *s = fever.find("start"); s && s->is_object())
                        start = row_i32(*s, "timeMs");
                    if (const Json *e = fever.find("end"); e && e->is_object())
                        end = row_i32(*e, "timeMs");
                    source.fever_ms.emplace_back(start, end);
                }
            }
        }
        return source;
    }
    throw SpecError("未知的谱面输入格式");
}

struct ProblemView {
    Json problem;
    const Json *chart = nullptr;
    const Json *song = nullptr;
    const Json *master_tables = nullptr;
    std::string master_dir;
    std::vector<int32_t> missions;
    int32_t play_level = 26;
    const Json *candidate_members = nullptr;
    const Json *candidate_snaps = nullptr;
};

ProblemView resolve_problem_view(const Json &input) {
    ProblemView view;
    const Json *problem = input.find("problem");
    if (!problem)
        throw SpecError("输入缺少 problem");
    if (problem->is_string())
        view.problem = Json::parse_file(problem->as_string());
    else
        view.problem = *problem;

    const Json *score_mode = view.problem.find("score_mode");
    if (!score_mode) {
        if (const Json *meta = view.problem.find("meta"); meta && meta->is_object()) {
            score_mode = meta->find("score_mode");
            if (!score_mode)
                if (const Json *source = meta->find("source"); source && source->is_object())
                    score_mode = source->find("score_mode");
        }
    }
    std::string mode = json_string(score_mode);
    if (mode != "gekisou")
        throw SpecError(
            "输入 problem 缺少或未启用激奏模式: score_mode === 'gekisou' 必须满足 (当前: '" + mode +
            "')");

    view.chart = input.find("chart");
    if (!view.chart || view.chart->is_null())
        view.chart = view.problem.find("chart");
    view.song = input.find("song");
    if (!view.song || view.song->is_null())
        view.song = view.problem.find("song");
    if (!view.chart)
        throw SpecError("输入 problem 缺少 chart");

    const Json *missions = input.find("missions");
    if (!missions || missions->is_null()) {
        if (view.song && view.song->is_object())
            missions = view.song->find("gekisou_missions");
    }
    if (!missions || !missions->is_array() || missions->size() != 3)
        throw SpecError("激奏必须包含恰好 3 个有效任务 (song.gekisou_missions)");
    for (const Json &mission : missions->as_array()) {
        if (!mission.is_integer() || mission.as_int64() < 1 || mission.as_int64() > 4)
            throw SpecError("任务 ID 必须为 1..4 的整数，收到: " + mission.dump(0));
        view.missions.push_back(static_cast<int32_t>(mission.as_int64()));
    }

    const Json *play_level = input.find("play_level");
    if (!play_level || play_level->is_null())
        view.play_level = row_i32(*view.chart, "level", 0);
    else
        view.play_level = json_i32(play_level, 0);
    if (view.play_level == 0)
        view.play_level = 26;

    view.master_tables = input.find("master_tables");
    if (!view.master_tables || view.master_tables->is_null())
        view.master_tables = view.problem.find("gekisou_master_tables");
    const Json *master_dir = input.find("master_dir");
    if (master_dir && master_dir->is_string())
        view.master_dir = master_dir->as_string();
    if (view.master_dir.empty()) {
        if (const Json *meta = view.problem.find("meta"); meta && meta->is_object())
            if (const Json *source = meta->find("source"); source && source->is_object())
                view.master_dir = json_string(source->find("master_dir"));
    }

    const Json *candidates = input.find("candidates");
    if (candidates && candidates->is_object()) {
        view.candidate_members = candidates->find("members");
        view.candidate_snaps = candidates->find("snaps");
    } else if (const Json *catalog = view.problem.find("catalog");
               catalog && catalog->is_object()) {
        view.candidate_members = catalog->find("members");
        view.candidate_snaps = catalog->find("snapshots");
    }
    return view;
}

struct CatalogMaps {
    std::unordered_map<int32_t, const Json *> members;
    std::unordered_map<int32_t, const Json *> snaps;
};

CatalogMaps catalog_maps(const Json &problem) {
    CatalogMaps maps;
    const Json *catalog = problem.find("catalog");
    if (!catalog || !catalog->is_object())
        return maps;
    if (const Json *members = catalog->find("members"); members && members->is_array())
        for (const Json &row : members->as_array())
            maps.members[row_i32(row, "id")] = &row;
    if (const Json *snaps = catalog->find("snapshots"); snaps && snaps->is_array())
        for (const Json &row : snaps->as_array())
            maps.snaps[row_i32(row, "id")] = &row;
    return maps;
}

PlayerState parse_player(const Json *player) {
    PlayerState out;
    out.vip_rank = 1;
    if (!player || !player->is_object())
        return out;
    out.total_rank = row_i32(*player, "total_rank", 0);
    out.vip_rank = row_i32(*player, "vip_rank", 1);
    auto read_map = [&](const char *key, std::map<int32_t, int32_t> &target) {
        const Json *value = player->find(key);
        if (!value)
            return;
        if (value->is_object()) {
            for (const auto &[id, level] : value->fields())
                target[std::stoi(id)] = json_i32(&level);
        } else if (value->is_array()) {
            for (const Json &row : value->as_array())
                target[row_i32(row, "master_id", 0, "id")] = row_i32(row, "level", 0);
        }
    };
    read_map("character_ranks", out.character_ranks);
    read_map("band_items", out.band_items);
    read_map("character_memory", out.character_memory);
    read_map("music_memory", out.music_memory);
    return out;
}

MusicView parse_music(const Json *song) {
    MusicView out;
    if (!song || !song->is_object())
        return out;
    out.music_type = row_i32(*song, "type");
    out.best_tags = row_list(*song, "tags");
    out.memory_key = row_i32(*song, "id");
    return out;
}

void validate_growth(const Json *growth, const std::string &label) {
    if (!growth || growth->is_null())
        return;
    if (!growth->is_object())
        throw SpecError(label + "成长参数必须为对象");
    for (const auto &[key, value] : growth->fields()) {
        if (!value.is_integer() || value.as_int64() < 1)
            throw SpecError(label + "成长参数 " + key + " 必须为 >= 1 的整数，收到 " +
                            value.dump(0));
    }
}

void validate_skill_support(const EngineMaster &master, const std::vector<MemberState> &members,
                            const std::vector<const SnapState *> &snaps) {
    std::vector<std::string> unsupported;
    for (const auto &member : members) {
        if (member.card.gekiso_skill_id == 0)
            continue;
        auto it = master.gekiso_skills.find(member.card.gekiso_skill_id);
        if (it == master.gekiso_skills.end()) {
            unsupported.push_back("unknown-gekisou-skill:" +
                                  std::to_string(member.card.gekiso_skill_id));
            continue;
        }
        for (const auto &effect : it->second.effects)
            if (!kSupportedEffectTypes.count(effect.type))
                unsupported.push_back(
                    "gekisou-skill:" + std::to_string(member.card.gekiso_skill_id) +
                    ":effect:" + std::to_string(effect.type));
    }
    for (const SnapState *snap : snaps) {
        if (!snap)
            continue;
        for (int32_t skill_id : snap->card.gekiso_support_skill_ids) {
            if (!skill_id)
                continue;
            auto it = master.gekiso_support_skills.find(skill_id);
            if (it == master.gekiso_support_skills.end()) {
                unsupported.push_back("unknown-gekisou-support-skill:" + std::to_string(skill_id));
                continue;
            }
            for (const auto &effect : it->second.effects)
                if (!kSupportedEffectTypes.count(effect.type))
                    unsupported.push_back("gekisou-support-skill:" + std::to_string(skill_id) +
                                          ":effect:" + std::to_string(effect.type));
        }
    }
    if (!unsupported.empty()) {
        std::string text = "包含不受支持的技能效果或条件: ";
        for (size_t i = 0; i < unsupported.size(); ++i) {
            if (i)
                text += ", ";
            text += unsupported[i];
        }
        throw SpecError(text);
    }
}

Json formation_json(const Team &team, const std::vector<MemberState> &members,
                    const std::vector<SnapState> &snaps) {
    Json formation = Json::object();
    formation.set("schema", Json("ournotes-deck-formation@1"));
    std::vector<int64_t> member_ids, snap_ids;
    Json slots = Json::array();
    for (size_t i = 0; i < team.members.size(); ++i) {
        int32_t member_id = members[static_cast<size_t>(team.members[i])].card.id;
        int32_t snap_id = team.snaps[i] < 0 ? 0 : snaps[static_cast<size_t>(team.snaps[i])].card.id;
        member_ids.push_back(member_id);
        snap_ids.push_back(snap_id);
        Json slot = Json::object();
        slot.set("slot", Json(static_cast<int64_t>(i)));
        slot.set("member", Json(member_id));
        slot.set("snapshot", Json(snap_id));
        slot.set("trigger", Json(static_cast<int64_t>(i)));
        slots.push_back(std::move(slot));
    }
    formation.set("leader", Json(member_ids.empty() ? 0 : member_ids.front()));
    formation.set("slots", std::move(slots));
    Json ids = Json::array();
    for (int64_t id : member_ids)
        ids.push_back(Json(id));
    formation.set("member_ids", std::move(ids));
    Json sids = Json::array();
    for (int64_t id : snap_ids)
        sids.push_back(id ? Json(id) : Json());
    formation.set("snap_ids", std::move(sids));
    return formation;
}

Json search_stats_json(const SearchStats &stats) {
    Json out = Json::object();
    out.set("leaders", Json(stats.leaders));
    out.set("memberNodes", Json(stats.member_nodes));
    out.set("snapNodes", Json(stats.snap_nodes));
    out.set("candidates", Json(stats.candidates));
    out.set("exact", Json(stats.exact));
    out.set("elapsedMs", Json(stats.elapsed_ms));
    return out;
}

Json handle_gekisou_score_impl(const Json &input) {
    ProblemView view = resolve_problem_view(input);
    const Json &problem = view.problem;
    const EngineMaster master = load_master(view.master_dir, view.master_tables);
    ChartSource source = normalize_chart_source(*view.chart);
    validate_chart_fevers(source);

    const Json *options = input.find("options");
    int32_t assumed_placement = options ? row_i32(*options, "assumed_placement", 1) : 1;
    if (assumed_placement < 1 || assumed_placement > 5)
        throw SpecError("无效的 assumed_placement: " + std::to_string(assumed_placement) +
                        " (必须在 1..5 范围内)");
    Accuracy accuracy;
    if (options) {
        if (const Json *value = options->find("accuracy"); value && value->is_object()) {
            const double great_pct = row_f64(*value, "great", 0.0);
            const double just_pct = row_f64(*value, "just", 100.0);
            if (!std::isfinite(great_pct) || !std::isfinite(just_pct) || great_pct < 0.0 ||
                just_pct < 0.0 || great_pct + just_pct > 100.0 + 1e-9)
                throw SpecError(
                    "无效的 accuracy 判定比例 (要求 great >= 0, just >= 0, great + just <= 100)");
            accuracy.great = great_pct / 100.0;
            accuracy.just = just_pct / 100.0;
        }
    }
    GekisoChart chart = build_gekiso_chart(master, source, view.play_level, view.missions, accuracy,
                                           assumed_placement);

    const Json *formation_input = input.find("formation");
    const Json formation =
        formation_input && formation_input->is_object() ? *formation_input : Json::object();
    const Json *raw_slots = formation.find("slots");
    std::vector<Json> slots;
    if (raw_slots && raw_slots->is_array())
        slots = raw_slots->as_array();
    if (slots.size() != 5)
        throw SpecError("队伍必须包含恰好 5 个槽位，当前数量: " + std::to_string(slots.size()));

    struct NormalizedSlot {
        int32_t member = 0;
        int32_t snapshot = 0;
        Json member_growth;
        Json snap_growth;
        bool has_member_growth = false;
        bool has_snap_growth = false;
    };
    std::vector<NormalizedSlot> normalized;
    for (size_t i = 0; i < slots.size(); ++i) {
        const Json &slot = slots[i];
        NormalizedSlot item;
        const Json *member = slot.find("member");
        if (!member)
            member = slot.find("member_id");
        item.member = json_i32(member);
        const Json *snapshot = slot.find("snapshot");
        if (!snapshot)
            snapshot = slot.find("snap_id");
        item.snapshot = snapshot ? json_i32(snapshot, 0) : 0;
        if (const Json *growth = slot.find("member_growth"); growth && growth->is_object()) {
            item.member_growth = *growth;
            item.has_member_growth = true;
        }
        if (const Json *growth = slot.find("snap_growth"); growth && growth->is_object()) {
            item.snap_growth = *growth;
            item.has_snap_growth = true;
        }
        normalized.push_back(std::move(item));
    }

    std::optional<int32_t> leader_id;
    if (const Json *leader = formation.find("leader"); leader && !leader->is_null())
        leader_id = json_i32(leader);
    if (leader_id) {
        auto it =
            std::find_if(normalized.begin(), normalized.end(),
                         [&](const NormalizedSlot &slot) { return slot.member == *leader_id; });
        if (it == normalized.end())
            throw SpecError("指定的队长卡片 " + std::to_string(*leader_id) + " 不在编队槽位中");
        if (it != normalized.begin())
            std::swap(normalized.front(), *it);
    }

    bool allow_missing = json_truthy(problem.find("experimental_allow_missing_snapshots")) ||
                         (options && json_truthy(options->find("allow_missing_snapshots")));
    CatalogMaps catalog = catalog_maps(problem);
    for (const auto &slot : normalized) {
        if (!catalog.members.empty() && !catalog.members.count(slot.member))
            throw SpecError("编队成员 " + std::to_string(slot.member) +
                            " 不在输入卡池 (catalog) 中");
        if (slot.snapshot > 0 && !catalog.snaps.empty() && !catalog.snaps.count(slot.snapshot))
            throw SpecError("编队 Snapshot " + std::to_string(slot.snapshot) +
                            " 不在输入卡池 (catalog) 中");
        if (slot.snapshot <= 0 && !allow_missing)
            throw SpecError("编队槽位缺少 Snapshot (常规模式每槽必须绑定 Snapshot)");
        if (slot.has_member_growth)
            validate_growth(&slot.member_growth, "成员");
        if (slot.has_snap_growth)
            validate_growth(&slot.snap_growth, "Snapshot ");
    }

    std::vector<MemberInput> member_inputs;
    std::vector<SnapInput> snap_inputs;
    std::vector<std::string> slot_snap_keys;
    for (size_t i = 0; i < normalized.size(); ++i) {
        const auto &slot = normalized[i];
        const Json *category =
            catalog.members.count(slot.member) ? catalog.members.at(slot.member) : nullptr;
        MemberInput member;
        member.key = "m_" + std::to_string(i) + "_" + std::to_string(slot.member);
        member.card_id = slot.member;
        if (slot.has_member_growth) {
            member.level = optional_int(&slot.member_growth, "level");
            member.awake = optional_int(&slot.member_growth, "awake");
            member.rank = optional_int(&slot.member_growth, "rank");
            member.live_skill_level = optional_int(&slot.member_growth, "live_skill_level");
            member.gekiso_skill_level = optional_int(&slot.member_growth, "gekiso_skill_level");
        }
        if (!member.level && category)
            member.level = optional_int(category, "level");
        if (!member.awake && category)
            member.awake = optional_int(category, "awake");
        if (!member.rank && category)
            member.rank = optional_int(category, "rank");
        if (!member.live_skill_level && category)
            member.live_skill_level = optional_int(category, "live_skill_level");
        if (!member.gekiso_skill_level && category)
            member.gekiso_skill_level = optional_int(category, "gekiso_skill_level");
        member_inputs.push_back(std::move(member));

        if (slot.snapshot > 0) {
            std::string key = "s_" + std::to_string(i) + "_" + std::to_string(slot.snapshot);
            SnapInput snap;
            snap.key = key;
            snap.card_id = slot.snapshot;
            const Json *snap_category =
                catalog.snaps.count(slot.snapshot) ? catalog.snaps.at(slot.snapshot) : nullptr;
            if (slot.has_snap_growth) {
                snap.level = optional_int(&slot.snap_growth, "level");
                snap.rank = optional_int(&slot.snap_growth, "rank");
            }
            if (!snap.level && snap_category)
                snap.level = optional_int(snap_category, "level");
            if (!snap.rank && snap_category)
                snap.rank = optional_int(snap_category, "rank");
            snap_inputs.push_back(std::move(snap));
            slot_snap_keys.push_back(key);
        } else {
            slot_snap_keys.push_back("");
        }
    }

    ResolvedBox box = resolve_box(master, member_inputs, snap_inputs);
    if (!box.unknown.empty()) {
        std::string text = "未知的成员或快照卡片: ";
        for (size_t i = 0; i < box.unknown.size(); ++i) {
            if (i)
                text += ", ";
            text += box.unknown[i];
        }
        throw SpecError(text);
    }
    std::set<int32_t> characters;
    for (const auto &member : box.members)
        characters.insert(member.card.character_id);
    if (characters.size() != 5)
        throw SpecError("编队中存在重复角色 (唯一点数: " + std::to_string(characters.size()) + ")");
    std::set<int32_t> snap_cards;
    for (const auto &snap : box.snaps)
        if (snap.card.id > 0 && !snap_cards.insert(snap.card.id).second)
            throw SpecError("编队中存在重复 Snapshot");

    std::unordered_map<std::string, const SnapState *> snap_map;
    for (const auto &snap : box.snaps)
        snap_map[snap.key] = &snap;
    std::vector<const SnapState *> slot_snaps;
    for (const auto &key : slot_snap_keys) {
        auto it = snap_map.find(key);
        slot_snaps.push_back(key.empty() || it == snap_map.end() ? nullptr : it->second);
    }

    validate_skill_support(master, box.members, slot_snaps);
    std::vector<SlotSkill> slot_skills;
    for (size_t i = 0; i < box.members.size(); ++i)
        slot_skills.push_back(resolve_slot_skill(master, box.members[i], slot_snaps[i]));
    std::vector<Performer> deck;
    for (size_t i = 0; i < box.members.size(); ++i)
        deck.push_back(make_performer(master, box.members[i], slot_snaps[i]));

    std::vector<int32_t> seeds;
    if (options) {
        const Json *values = options->find("seeds");
        if (values && !values->is_null()) {
            if (!values->is_array())
                throw SpecError("seeds 必须为数组");
            for (const Json &value : values->as_array()) {
                if (!value.is_integer() || value.as_int64() < 0)
                    throw SpecError("seeds 元素必须为非负整数: " + value.dump(0));
                seeds.push_back(static_cast<int32_t>(value.as_int64()));
            }
        }
    }
    std::string luck_mode = !chart.luck     ? "nominal_model_exact"
                            : seeds.empty() ? "nominal_expectation"
                                            : "sample_mean";
    std::unordered_map<std::string, LuckCurve> curve_cache;
    GekisoContext context = gekiso_context(master, chart, deck, seeds, curve_cache);

    PlayerState player = parse_player(input.find("player"));
    MusicView music = parse_music(view.song);
    double power = 0.0;
    std::vector<double> slot_powers;
    bool has_slot_powers = false;
    if (const Json *power_input = input.find("power"); power_input && power_input->is_number()) {
        power = power_input->as_double();
    } else {
        TeamPower result = team_power(&problem, master, player, box.members, slot_snaps, 0, &music);
        power = result.total;
        slot_powers = result.slots;
        has_slot_powers = true;
    }
    OrderScores scores = score_orders_gekiso(master, chart, context, power, slot_skills);

    Json out = Json::object();
    out.set("status", Json("ok"));
    out.set("mean", Json(scores.mean));
    out.set("max", Json(scores.max));
    out.set("min", Json(scores.min));
    out.set("max_order", Json(static_cast<int64_t>(scores.max_order)));
    out.set("min_order", Json(static_cast<int64_t>(scores.min_order)));
    Json order_scores = Json::array();
    for (double value : scores.scores)
        order_scores.push_back(Json(value));
    out.set("order_scores", std::move(order_scores));
    out.set("power", Json(power));
    if (has_slot_powers) {
        Json values = Json::array();
        for (double value : slot_powers)
            values.push_back(Json(value));
        out.set("slot_powers", std::move(values));
    } else {
        out.set("slot_powers", Json());
    }
    out.set("assumed_placement", Json(assumed_placement));
    Json seed_values = Json::array();
    for (int32_t seed : seeds)
        seed_values.push_back(Json(seed));
    out.set("seeds", std::move(seed_values));
    out.set("luck_mode", Json(luck_mode));
    Json details = Json::object();
    Json range_percents = Json::array();
    for (int32_t value : chart.range_percent)
        range_percents.push_back(Json(value));
    details.set("range_percents", std::move(range_percents));
    Json span = Json::array();
    span.push_back(Json(static_cast<int64_t>(chart.span.first)));
    span.push_back(Json(static_cast<int64_t>(chart.span.second)));
    details.set("span", std::move(span));
    details.set("notes_count", Json(static_cast<int64_t>(chart.notes.size())));
    details.set("luck_present", Json(chart.luck));
    out.set("details", std::move(details));
    return out;
}

Json handle_gekisou_rank_impl(const Json &input) {
    ProblemView view = resolve_problem_view(input);
    const Json &problem = view.problem;
    const EngineMaster master = load_master(view.master_dir, view.master_tables);
    ChartSource source = normalize_chart_source(*view.chart);
    validate_chart_fevers(source);

    const Json *options = input.find("options");
    int32_t assumed_placement = options ? row_i32(*options, "assumed_placement", 1) : 1;
    if (assumed_placement < 1 || assumed_placement > 5)
        throw SpecError("无效的 assumed_placement: " + std::to_string(assumed_placement) +
                        " (必须在 1..5 范围内)");
    Accuracy accuracy;
    if (options) {
        if (const Json *value = options->find("accuracy"); value && value->is_object()) {
            const double great_pct = row_f64(*value, "great", 0.0);
            const double just_pct = row_f64(*value, "just", 100.0);
            if (!std::isfinite(great_pct) || !std::isfinite(just_pct) || great_pct < 0.0 ||
                just_pct < 0.0 || great_pct + just_pct > 100.0 + 1e-9)
                throw SpecError(
                    "无效的 accuracy 判定比例 (要求 great >= 0, just >= 0, great + just <= 100)");
            accuracy.great = great_pct / 100.0;
            accuracy.just = just_pct / 100.0;
        }
    }
    GekisoChart chart = build_gekiso_chart(master, source, view.play_level, view.missions, accuracy,
                                           assumed_placement);
    CatalogMaps catalog = catalog_maps(problem);
    const Json *cand_members = view.candidate_members;
    const Json *cand_snaps = view.candidate_snaps;

    if (cand_members && cand_members->is_array()) {
        std::set<int32_t> seen;
        for (const Json &row : cand_members->as_array()) {
            int32_t id = row_i32(row, "id");
            if (!catalog.members.empty() && !catalog.members.count(id))
                throw SpecError("候选成员卡 " + std::to_string(id) + " 不在输入卡池 (catalog) 中");
            if (!seen.insert(id).second)
                throw SpecError("候选成员卡列表中存在重复 ID: " + std::to_string(id));
        }
    }
    if (cand_snaps && cand_snaps->is_array()) {
        std::set<int32_t> seen;
        for (const Json &row : cand_snaps->as_array()) {
            int32_t id = row_i32(row, "id");
            if (!catalog.snaps.empty() && !catalog.snaps.count(id))
                throw SpecError("候选 Snapshot " + std::to_string(id) +
                                " 不在输入卡池 (catalog) 中");
            if (!seen.insert(id).second)
                throw SpecError("候选 Snapshot 列表中存在重复 ID: " + std::to_string(id));
        }
    }

    std::vector<MemberInput> member_inputs;
    if (cand_members && cand_members->is_array()) {
        size_t index = 0;
        for (const Json &row : cand_members->as_array()) {
            int32_t id = row_i32(row, "id");
            const Json *category = catalog.members.count(id) ? catalog.members.at(id) : nullptr;
            MemberInput member;
            member.key = "m_" + std::to_string(index++) + "_" + std::to_string(id);
            member.card_id = id;
            const Json *growth = row.find("growth");
            member.level = optional_int(&row, "level");
            if (!member.level && growth)
                member.level = optional_int(growth, "level");
            if (!member.level && category)
                member.level = optional_int(category, "level");
            member.awake = optional_int(&row, "awake");
            if (!member.awake && growth)
                member.awake = optional_int(growth, "awake");
            if (!member.awake && category)
                member.awake = optional_int(category, "awake");
            member.rank = optional_int(&row, "rank");
            if (!member.rank && growth)
                member.rank = optional_int(growth, "rank");
            if (!member.rank && category)
                member.rank = optional_int(category, "rank");
            member.live_skill_level = optional_int(&row, "live_skill_level");
            if (!member.live_skill_level && growth)
                member.live_skill_level = optional_int(growth, "live_skill_level");
            if (!member.live_skill_level && category)
                member.live_skill_level = optional_int(category, "live_skill_level");
            member.gekiso_skill_level = optional_int(&row, "gekiso_skill_level");
            if (!member.gekiso_skill_level && growth)
                member.gekiso_skill_level = optional_int(growth, "gekiso_skill_level");
            if (!member.gekiso_skill_level && category)
                member.gekiso_skill_level = optional_int(category, "gekiso_skill_level");
            member_inputs.push_back(std::move(member));
        }
    }
    std::vector<SnapInput> snap_inputs;
    if (cand_snaps && cand_snaps->is_array()) {
        size_t index = 0;
        for (const Json &row : cand_snaps->as_array()) {
            int32_t id = row_i32(row, "id");
            const Json *category = catalog.snaps.count(id) ? catalog.snaps.at(id) : nullptr;
            SnapInput snap;
            snap.key = "s_" + std::to_string(index++) + "_" + std::to_string(id);
            snap.card_id = id;
            const Json *growth = row.find("growth");
            snap.level = optional_int(&row, "level");
            if (!snap.level && growth)
                snap.level = optional_int(growth, "level");
            if (!snap.level && category)
                snap.level = optional_int(category, "level");
            snap.rank = optional_int(&row, "rank");
            if (!snap.rank && growth)
                snap.rank = optional_int(growth, "rank");
            if (!snap.rank && category)
                snap.rank = optional_int(category, "rank");
            snap_inputs.push_back(std::move(snap));
        }
    }

    ResolvedBox box = resolve_box(master, member_inputs, snap_inputs);
    if (!box.unknown.empty()) {
        std::string text = "未知的候选卡片: ";
        for (size_t i = 0; i < box.unknown.size(); ++i) {
            if (i)
                text += ", ";
            text += box.unknown[i];
        }
        throw SpecError(text);
    }
    bool allow_missing = json_truthy(problem.find("experimental_allow_missing_snapshots")) ||
                         (options && json_truthy(options->find("allow_missing_snapshots")));
    if (!allow_missing && box.snaps.size() < 5)
        throw SpecError("候选 Snapshot 数量不足 5 张 (当前: " + std::to_string(box.snaps.size()) +
                        "，常规激奏每槽必须绑定 Snapshot)");
    std::vector<const SnapState *> all_snaps;
    for (const auto &snap : box.snaps)
        all_snaps.push_back(&snap);
    validate_skill_support(master, box.members, all_snaps);

    PlayerState player = parse_player(input.find("player"));
    std::string criterion = criterion_key(json_string(input.find("criterion"), "mean"));
    std::unordered_map<int32_t, int> member_index;
    for (size_t i = 0; i < box.members.size(); ++i)
        member_index[box.members[i].card.id] = static_cast<int>(i);
    std::unordered_map<int32_t, int> snap_index;
    for (size_t i = 0; i < box.snaps.size(); ++i)
        snap_index[box.snaps[i].card.id] = static_cast<int>(i);

    const Json *prob_constraints = problem.find("constraints");
    const Json *cli_constraints = input.find("constraints");
    const Json empty_object = Json::object();
    const Json empty_array = Json::array();
    if (!prob_constraints || !prob_constraints->is_object())
        prob_constraints = &empty_object;
    if (!cli_constraints || !cli_constraints->is_object())
        cli_constraints = &empty_object;
    if (json_truthy(prob_constraints->find("distinct_characters"), true) == false)
        throw SpecError("激奏模式必须满足角色互异 (不支持 distinct_characters=false)");
    if (json_truthy(prob_constraints->find("distinct_snapshots"), true) == false)
        throw SpecError("激奏模式必须满足 Snapshot 互异 (不支持 distinct_snapshots=false)");
    if (const Json *bindings = prob_constraints->find("bindings");
        bindings && bindings->is_object() && bindings->size() > 0)
        throw SpecError("激奏组队暂不支持 bindings 约束");
    if (const Json *pool = prob_constraints->find("member_pool");
        pool && pool->is_array() && pool->size() > 0)
        throw SpecError("激奏暂不支持 member_pool 约束，请直接通过候选卡池过滤");
    if (const Json *pool = prob_constraints->find("snapshot_pool");
        pool && pool->is_array() && pool->size() > 0)
        throw SpecError("激奏暂不支持 snapshot_pool 约束，请直接通过候选卡池过滤");
    if (const Json *pool = prob_constraints->find("leader_pool");
        pool && pool->is_array() && pool->size() > 1)
        throw SpecError("激奏暂不支持多选队长池 (leader_pool 长度 > 1)");
    if (const Json *pool = prob_constraints->find("leader_candidates");
        pool && pool->is_array() && pool->size() > 1)
        throw SpecError("激奏暂不支持多候选队长 (leader_candidates 长度 > 1)");

    auto nonempty_or_null = [](const Json *value) -> const Json * {
        return value && value->is_array() && value->size() > 0 ? value : nullptr;
    };
    const Json *raw_req_m = nonempty_or_null(cli_constraints->find("required_members"));
    if (!raw_req_m)
        raw_req_m = prob_constraints->find("required_members");
    if (!raw_req_m)
        raw_req_m = &empty_array;
    const Json *raw_exc_m = nonempty_or_null(cli_constraints->find("excluded_members"));
    if (!raw_exc_m)
        raw_exc_m = prob_constraints->find("excluded_members");
    if (!raw_exc_m)
        raw_exc_m = prob_constraints->find("banned_members");
    if (!raw_exc_m)
        raw_exc_m = &empty_array;
    const Json *raw_req_s = nonempty_or_null(cli_constraints->find("required_snapshots"));
    if (!raw_req_s)
        raw_req_s = nonempty_or_null(cli_constraints->find("required_snaps"));
    if (!raw_req_s)
        raw_req_s = prob_constraints->find("required_snapshots");
    if (!raw_req_s)
        raw_req_s = prob_constraints->find("required_snaps");
    if (!raw_req_s)
        raw_req_s = &empty_array;
    const Json *raw_exc_s = nonempty_or_null(cli_constraints->find("excluded_snapshots"));
    if (!raw_exc_s)
        raw_exc_s = nonempty_or_null(cli_constraints->find("excluded_snaps"));
    if (!raw_exc_s)
        raw_exc_s = prob_constraints->find("excluded_snapshots");
    if (!raw_exc_s)
        raw_exc_s = prob_constraints->find("excluded_snaps");
    if (!raw_exc_s)
        raw_exc_s = prob_constraints->find("banned_snapshots");
    if (!raw_exc_s)
        raw_exc_s = &empty_array;

    std::vector<int32_t> req_m = json_id_list(raw_req_m);
    std::vector<int32_t> exc_m = json_id_list(raw_exc_m);
    std::vector<int32_t> req_s = json_id_list(raw_req_s);
    std::vector<int32_t> exc_s = json_id_list(raw_exc_s);
    std::optional<int32_t> raw_leader;
    if (const Json *leader = cli_constraints->find("leader"); leader && !leader->is_null())
        raw_leader = json_i32(leader);
    if (!raw_leader)
        if (const Json *leader = prob_constraints->find("leader"); leader && !leader->is_null())
            raw_leader = json_i32(leader);
    if (!raw_leader) {
        if (const Json *pool = prob_constraints->find("leader_pool");
            pool && pool->is_array() && pool->size() == 1)
            raw_leader = json_i32(&pool->at(0));
    }
    if (!raw_leader) {
        if (const Json *pool = prob_constraints->find("leader_candidates");
            pool && pool->is_array() && pool->size() == 1)
            raw_leader = json_i32(&pool->at(0));
    }

    for (int32_t id : req_m)
        if (!member_index.count(id))
            throw SpecError("约束中的成员卡不存在: " + std::to_string(id));
    for (int32_t id : exc_m)
        if (!member_index.count(id))
            throw SpecError("约束中的成员卡不存在: " + std::to_string(id));
    for (int32_t id : req_s)
        if (!snap_index.count(id))
            throw SpecError("约束中的 Snapshot 不存在: " + std::to_string(id));
    for (int32_t id : exc_s)
        if (!snap_index.count(id))
            throw SpecError("约束中的 Snapshot 不存在: " + std::to_string(id));
    if (raw_leader && !member_index.count(*raw_leader))
        throw SpecError("约束中的队长卡不存在: " + std::to_string(*raw_leader));
    if (req_m.size() > 5)
        throw SpecError("必选成员卡数量不能超过 5 张 (当前: " + std::to_string(req_m.size()) + ")");
    for (int32_t id : req_m)
        if (std::find(exc_m.begin(), exc_m.end(), id) != exc_m.end())
            throw SpecError("成员卡 " + std::to_string(id) + " 同时出现在必选和排除列表中");
    for (int32_t id : req_s)
        if (std::find(exc_s.begin(), exc_s.end(), id) != exc_s.end())
            throw SpecError("Snapshot " + std::to_string(id) + " 同时出现在必选和排除列表中");
    if (raw_leader && std::find(exc_m.begin(), exc_m.end(), *raw_leader) != exc_m.end())
        throw SpecError("队长卡片 " + std::to_string(*raw_leader) + " 不能在排除列表中");
    if (raw_leader && req_m.size() == 5 &&
        std::find(req_m.begin(), req_m.end(), *raw_leader) == req_m.end())
        throw SpecError("必选成员已有 5 人且不包含指定队长 " + std::to_string(*raw_leader));

    std::set<int32_t> req_characters;
    for (int32_t id : req_m) {
        int32_t character = box.members[static_cast<size_t>(member_index.at(id))].card.character_id;
        if (!req_characters.insert(character).second)
            throw SpecError("必选成员中存在重复角色");
    }
    if (raw_leader && std::find(req_m.begin(), req_m.end(), *raw_leader) == req_m.end()) {
        int32_t leader_character =
            box.members[static_cast<size_t>(member_index.at(*raw_leader))].card.character_id;
        if (req_characters.count(leader_character))
            throw SpecError("指定队长 " + std::to_string(*raw_leader) +
                            " 的角色与必选成员角色冲突");
    }

    SearchConstraints constraints;
    for (int32_t id : req_m)
        constraints.required_members.push_back(member_index.at(id));
    for (int32_t id : exc_m)
        constraints.excluded_members.push_back(member_index.at(id));
    for (int32_t id : req_s)
        constraints.required_snaps.push_back(snap_index.at(id));
    for (int32_t id : exc_s)
        constraints.excluded_snaps.push_back(snap_index.at(id));
    if (raw_leader)
        constraints.leader = member_index.at(*raw_leader);

    std::vector<int32_t> seeds;
    if (options) {
        const Json *values = options->find("seeds");
        if (values && !values->is_null()) {
            if (!values->is_array())
                throw SpecError("seeds 必须为数组");
            for (const Json &value : values->as_array()) {
                if (!value.is_integer() || value.as_int64() < 0)
                    throw SpecError("seeds 元素必须为非负整数: " + value.dump(0));
                seeds.push_back(static_cast<int32_t>(value.as_int64()));
            }
        }
    }
    std::string luck_mode = !chart.luck     ? "nominal_model_exact"
                            : seeds.empty() ? "nominal_expectation"
                                            : "sample_mean";
    int32_t k = 10;
    if (const Json *value = input.find("k"); value && !value->is_null()) {
        if (!value->is_integer())
            throw SpecError("k (Top-K) 必须为 > 0 的正整数");
        const int64_t requested = value->as_int64();
        if (requested <= 0 || requested > std::numeric_limits<int32_t>::max())
            throw SpecError("k (Top-K) 必须为 > 0 的正整数");
        k = static_cast<int32_t>(requested);
    }
    std::optional<double> time_limit_ms;
    if (options) {
        if (const Json *value = options->find("time_limit_ms"); value && !value->is_null()) {
            if (!value->is_number())
                throw SpecError("time_limit_ms 不能为负数");
            const double ms = value->as_double();
            if (!std::isfinite(ms) || ms < 0.0)
                throw SpecError("time_limit_ms 不能为负数");
            time_limit_ms = ms;
        }
    }

    MusicView music = parse_music(view.song);
    bool oracle = json_truthy(input.find("oracle"));
    SearchOutput search;
    if (oracle) {
        int32_t available_members = 0;
        for (int i = 0; i < static_cast<int>(box.members.size()); ++i)
            if (std::find(constraints.excluded_members.begin(), constraints.excluded_members.end(),
                          i) == constraints.excluded_members.end())
                ++available_members;
        int32_t available = 0;
        for (int j = 0; j < static_cast<int>(box.snaps.size()); ++j)
            if (std::find(constraints.excluded_snaps.begin(), constraints.excluded_snaps.end(),
                          j) == constraints.excluded_snaps.end())
                ++available;
        if (available < 5 && !allow_missing)
            throw SpecError("可用 Snapshot 数量不足 5 张 (当前: " + std::to_string(available) +
                            ")");
        double c_members = 0.0;
        if (available_members >= 5) {
            c_members = 1.0;
            for (int i = 0; i < 5; ++i)
                c_members = c_members * (available_members - i) / (i + 1);
        }
        double leader_ways = constraints.leader ? 1.0 : 5.0;
        const bool oracle_allow_empty = allow_missing || available < 5;
        double snap_ways = 0.0;
        if (!oracle_allow_empty) {
            snap_ways = 1.0;
            for (int i = 0; i < 5; ++i)
                snap_ways *= available - i;
        } else {
            const int32_t max_sc = std::min<int32_t>(5, available);
            for (int32_t sc = 0; sc <= max_sc; ++sc) {
                double comb = 1.0;
                for (int i = 0; i < sc; ++i)
                    comb = comb * (5 - i) / (i + 1);
                double perm = 1.0;
                for (int i = 0; i < sc; ++i)
                    perm *= available - i;
                snap_ways += comb * perm;
            }
        }
        double estimated = c_members * leader_ways * snap_ways;
        if (!std::isfinite(estimated) || estimated > 50000.0) {
            std::string est_str;
            if (std::isfinite(estimated) && estimated < 9e18) {
                est_str = std::to_string(static_cast<int64_t>(estimated));
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.3e", estimated);
                est_str = buf;
            }
            throw SpecError("Oracle 穷举组合数超过上限 50000 (估算: " + est_str +
                            ")，请缩小候选池或使用 search");
        }
        search = gekiso_search(master, player, box.members, box.snaps, &music, chart, seeds,
                               criterion, constraints, 1000000, time_limit_ms.value_or(0.0),
                               &problem, oracle_allow_empty, true);
    } else {
        search =
            gekiso_search(master, player, box.members, box.snaps, &music, chart, seeds, criterion,
                          constraints, std::max(k, 10), time_limit_ms.value_or(10000.0), &problem,
                          allow_missing || box.snaps.size() < 5, false);
    }

    Json hits = Json::array();
    std::vector<SearchHit> sorted = search.hits;
    std::sort(sorted.begin(), sorted.end(), [&](const SearchHit &a, const SearchHit &b) {
        return order_key(a.detail.scores, criterion) > order_key(b.detail.scores, criterion);
    });
    const size_t limit = std::min(static_cast<size_t>(k), sorted.size());
    for (size_t i = 0; i < limit; ++i) {
        const double power = sorted[i].power;
        const OrderScores &scores = sorted[i].detail.scores;
        Json hit = Json::object();
        hit.set("rank", Json(static_cast<int64_t>(i + 1)));
        hit.set("score", Json(order_key(scores, criterion)));
        hit.set("power", Json(power));
        hit.set("formation", formation_json(sorted[i].team, box.members, box.snaps));
        Json score = Json::object();
        score.set("mean", Json(scores.mean));
        score.set("max", Json(scores.max));
        score.set("min", Json(scores.min));
        hit.set("scores", std::move(score));
        hits.push_back(std::move(hit));
    }
    if (!allow_missing && !oracle) {
        for (const Json &hit : hits.items()) {
            const Json *slots = hit.find("formation")->find("slots");
            for (const Json &slot : slots->items())
                if (row_i32(slot, "snapshot") <= 0)
                    throw SpecError("编队槽位缺少 Snapshot (常规模式每槽必须绑定 Snapshot)");
        }
    }

    Json out = Json::object();
    out.set("status", Json(hits.size() > 0 ? "ok" : (search.proven ? "ok" : "incomplete")));
    out.set("hits", std::move(hits));
    if (oracle) {
        out.set("search_complete", Json(search.proven));
        out.set("optimality_certified", Json(search.proven));
        std::string scope;
        if (!search.proven)
            scope = "incomplete_oracle";
        else if (seeds.empty() || !chart.luck)
            scope = "exact_global_oracle";
        else
            scope = "exact_seeded_oracle";
        out.set("optimality_scope", Json(scope));
        out.set("bound", Json(search.hits.empty()
                                  ? 0.0
                                  : order_key(search.hits.front().detail.scores, criterion)));
        Json stats = Json::object();
        stats.set("total_teams", Json(search.stats.exact));
        stats.set("member_sets", Json(static_cast<int64_t>(search.hits.size())));
        stats.set("timed_out", Json(!search.proven));
        out.set("stats", std::move(stats));
    } else {
        out.set("search_complete", Json(search.proven));
        out.set("optimality_certified", Json(search.proven));
        std::string scope = "heuristic_candidates";
        if (search.proven)
            scope = (seeds.empty() || !chart.luck) ? "exact_global" : "exact_seeded";
        out.set("optimality_scope", Json(scope));
        double bound = 0.0;
        if (const Json *first = out.find("hits"); first && first->is_array() && first->size() > 0)
            bound = row_f64(first->at(0), "score", 0.0);
        out.set("bound", Json(bound));
        out.set("stats", search_stats_json(search.stats));
    }
    Json seed_values = Json::array();
    for (int32_t seed : seeds)
        seed_values.push_back(Json(seed));
    out.set("luck_mode", Json(luck_mode));
    out.set("seeds", std::move(seed_values));
    return out;
}

} // namespace

Json handle_gekisou_score(const Json &input) {
    return handle_gekisou_score_impl(input);
}

Json handle_gekisou_rank(const Json &input) {
    return handle_gekisou_rank_impl(input);
}

} // namespace deckcalc
