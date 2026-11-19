#include "gekisou_internal.hpp"

#include <filesystem>

namespace deckcalc {
namespace gekisou_detail {

const Json *row_field(const Json &row, const std::string &name, const std::string &alt) {
    if (!row.is_object())
        return nullptr;
    if (const Json *p = row.find(name))
        return p;
    if (const Json *p = row.find("_" + name))
        return p;
    if (!alt.empty()) {
        if (const Json *p = row.find(alt))
            return p;
        if (const Json *p = row.find("_" + alt))
            return p;
    }
    if (const Json *raw = row.find("raw"); raw && raw->is_object()) {
        if (const Json *p = raw->find(name))
            return p;
        if (const Json *p = raw->find("_" + name))
            return p;
        if (!alt.empty()) {
            if (const Json *p = raw->find(alt))
                return p;
            if (const Json *p = raw->find("_" + alt))
                return p;
        }
    }
    return nullptr;
}

double json_num(const Json *val, double fallback) {
    if (!val || val->is_null())
        return fallback;
    if (val->is_number()) {
        double v = val->as_double();
        return std::isfinite(v) ? v : fallback;
    }
    if (val->is_string()) {
        const std::string &s = val->as_string();
        if (s.empty())
            return fallback;
        try {
            size_t idx = 0;
            double v = std::stod(s, &idx);
            return std::isfinite(v) ? v : fallback;
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

int32_t json_i32(const Json *val, int32_t fallback) {
    return static_cast<int32_t>(json_num(val, static_cast<double>(fallback)));
}

int32_t row_i32(const Json &row, const std::string &name, int32_t fallback,
                const std::string &alt) {
    return json_i32(row_field(row, name, alt), fallback);
}

double row_f64(const Json &row, const std::string &name, double fallback, const std::string &alt) {
    return json_num(row_field(row, name, alt), fallback);
}

std::string row_str(const Json &row, const std::string &name, const std::string &fallback) {
    const Json *v = row_field(row, name);
    if (!v || v->is_null())
        return fallback;
    if (v->is_string())
        return v->as_string();
    if (v->is_integer())
        return std::to_string(v->as_int64());
    if (v->is_number())
        return std::to_string(v->as_double());
    return fallback;
}

std::vector<int32_t> row_ids(const Json &row, const std::string &name, const std::string &alt) {
    std::vector<int32_t> out;
    const Json *v = row_field(row, name, alt);
    if (!v || !v->is_array())
        return out;
    for (const Json &item : v->as_array()) {
        int32_t id = json_i32(&item, 0);
        if (id > 0)
            out.push_back(id);
    }
    return out;
}

std::vector<int32_t> row_list(const Json &row, const std::string &name, const std::string &alt) {
    std::vector<int32_t> out;
    const Json *v = row_field(row, name, alt);
    if (!v || !v->is_array())
        return out;
    for (const Json &item : v->as_array()) {
        out.push_back(json_i32(&item, 0));
    }
    return out;
}

Stat3 row_stat(const Json &row, const std::string &p, const std::string &t, const std::string &v) {
    return {row_i32(row, p), row_i32(row, t), row_i32(row, v)};
}

SkillEffectRow parse_skill_effect(const Json &row) {
    SkillEffectRow out;
    out.id = row_i32(row, "id");
    out.level = row_i32(row, "level");
    out.trigger_type = row_i32(row, "skillTriggerType");
    out.execute_limit = row_i32(row, "effectExecuteLimitCount");
    out.reset_group = row_i32(row, "effectExecuteLimitResetConditionGroup");
    out.type = row_i32(row, "skillEffectType", 0, "effectType");
    out.value = row_i32(row, "effectValue");
    out.seconds = row_f64(row, "activationTimeSecond");
    out.condition_group = row_i32(row, "skillConditionGroup", 0, "conditionGroup");
    out.trigger_group = row_i32(row, "skillTriggerConditionGroup", 0, "triggerConditionGroup");
    out.release_group = row_i32(row, "skillReleaseConditionGroup", 0, "releaseConditionGroup");
    out.cumulative_id = row_i32(row, "skillCumulativeConditionID", 0, "cumulativeConditionId");
    out.target_ids = row_ids(row, "skillTargetIDs", "targetIds");
    out.limit_count = row_i32(row, "effectLimitCount");
    out.max_value = row_i32(row, "maxEffectValue");
    return out;
}

TargetRow parse_target_row(const Json &row) {
    TargetRow out;
    out.id = row_i32(row, "id");
    out.band_id = row_i32(row, "bandID", 0, "bandId");
    out.card_type = row_i32(row, "cardType");
    out.character_id = row_i32(row, "characterID", 0, "characterId");
    out.tag_id = row_i32(row, "tagID", 0, "tagId");
    out.live_skill_categories = row_list(row, "liveSkillCategories");
    out.gekisou_skill_categories = row_list(row, "gekisouSkillCategories");
    out.gekisou_mission_type = row_i32(row, "gekisouMissionType");
    out.live_music_type = row_i32(row, "liveMusicType");
    out.judgement = row_i32(row, "judgement", -1);
    out.skill_target_type = row_i32(row, "skillTargetType");
    return out;
}

EngineMaster load_master(const std::string &master_dir, const Json *tables_map) {
    std::vector<std::string> missing;
    for (const char *t : kAll36Tables) {
        if (tables_map && tables_map->is_object() && tables_map->contains(t))
            continue;
        if (!master_dir.empty() &&
            std::filesystem::exists(std::filesystem::path(master_dir) / (std::string(t) + ".json")))
            continue;
        missing.push_back(t);
    }
    if (!missing.empty()) {
        std::string msg =
            "缺少必需的 Master 数据表 (" + std::to_string(missing.size()) + "/36 张缺失): ";
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i > 0)
                msg += ", ";
            msg += missing[i];
        }
        throw SpecError(msg);
    }

    std::unordered_map<std::string, Json> loaded_files;
    auto read_table = [&](const std::string &name) -> const Json::array_t * {
        const Json *src = nullptr;
        if (tables_map && tables_map->is_object())
            src = tables_map->find(name);
        if (!src && !master_dir.empty()) {
            auto p = std::filesystem::path(master_dir) / (name + ".json");
            if (std::filesystem::exists(p)) {
                auto [it, _] = loaded_files.emplace(name, Json::read(p.string()));
                src = &it->second;
            }
        }
        if (!src)
            return nullptr;
        if (src->is_array())
            return &src->as_array();
        if (src->is_object()) {
            if (const Json *a = src->find("_allData"); a && a->is_array())
                return &a->as_array();
            if (const Json *a = src->find("entries"); a && a->is_array())
                return &a->as_array();
        }
        return nullptr;
    };

    const Json::array_t empty_arr;
    auto rows = [&](const std::string &name) -> const Json::array_t & {
        const Json::array_t *p = read_table(name);
        return p ? *p : empty_arr;
    };

    EngineMaster master;
    const auto &parameters = rows("MasterParameter");
    auto get_param = [&](const std::string &key, int32_t fallback) -> int32_t {
        for (const Json &r : parameters) {
            if (row_str(r, "id") == key)
                return row_i32(r, "value", fallback);
        }
        return fallback;
    };
    master.params.music_type_base = get_param("music_type_base_bonus_rate", 500);
    master.params.music_tag_base = get_param("music_tag_base_bonus_rate", 500);
    master.params.type_link_base = get_param("type_link_base_bonus_rate", 500);

    const auto &live_settings = rows("MasterLiveSettings");
    auto get_setting = [&](const std::string &key, double fallback) -> double {
        for (const Json &r : live_settings) {
            if (row_str(r, "key") == key)
                return row_f64(r, "value", fallback);
        }
        return fallback;
    };
    master.live.adjustment = f32(get_setting("note_score_adjustment_factor", 3.0));
    master.live.life_onus = f32(get_setting("note_score_life_onus_factor", 0.3));
    master.live.life_base = static_cast<int32_t>(get_setting("life_base", 1000.0));
    master.live.assist_percent = static_cast<int32_t>(get_setting("assist_score_percent", 90.0));
    master.live.gekiso.gauge_max =
        static_cast<int32_t>(get_setting("gekisou_luck_gauge_max", 100.0));
    master.live.gekiso.gauge_max_rush =
        static_cast<int32_t>(get_setting("gekisou_luck_gauge_max_rush", 50.0));
    master.live.gekiso.rush_percent =
        static_cast<int32_t>(get_setting("gekisou_luck_rush_score_bonus_percent", 0.0));

    for (const Json &r : rows("MasterLiveNoteParameter")) {
        master.live.note_percent[row_i32(r, "noteOperateType")] = row_i32(r, "scorePercent");
    }
    for (const Json &r : rows("MasterLiveJudgementParameter")) {
        int32_t sim = row_i32(r, "noteSimulateJudgement", -1);
        if (sim >= 1 && sim <= 6) {
            master.live.judge_percent[7 - sim] = row_i32(r, "scorePercent");
            master.live.damage[sim] = row_i32(r, "damage");
        }
    }

    std::unordered_map<int32_t, std::vector<std::pair<int32_t, float>>> raw_combo;
    for (const Json &r : rows("MasterLiveComboScoreBonus")) {
        raw_combo[row_i32(r, "comboBonusType")].emplace_back(row_i32(r, "requiredComboCount"),
                                                             f32(row_f64(r, "bonusFactor")));
    }
    for (auto &[type, items] : raw_combo) {
        std::stable_sort(items.begin(), items.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        float sum = 0.0f;
        for (auto &it : items) {
            sum = f32(sum + it.second);
            it.second = sum;
        }
        master.live.combo[type] = std::move(items);
    }

    int32_t max_after = 0;
    for (const Json &r : rows("MasterLiveJudgementTiming")) {
        int32_t jt = row_i32(r, "noteJudgementType");
        master.live.timing_types.insert(jt);
        if (row_i32(r, "noteSimulateJudgement") == 6)
            master.live.just_types.insert(jt);
        int32_t after = row_i32(r, "afterMs", 0, "after");
        if (after > max_after)
            max_after = after;
    }
    master.live.after_ms = max_after;

    for (const Json &r : rows("MasterSkillEffectSetting")) {
        master.live.phases[row_i32(r, "skillEffectType")] = row_i32(r, "phase", 1);
    }

    for (const Json &r : rows("MasterLiveGekisouLuckBasePoint")) {
        master.live.gekiso.luck_base_points.push_back({
            row_i32(r, "noteCategory"),
            row_i32(r, "noteSimulateJudgement"),
            row_i32(r, "weight"),
            row_i32(r, "basePoint"),
        });
    }
    for (const Json &r : rows("MasterLiveGekisouLuckBonusLot")) {
        master.live.gekiso.luck_bonus_lots.push_back({
            row_i32(r, "chanceLotType"),
            row_i32(r, "lotResult"),
            row_i32(r, "weight"),
        });
    }
    for (const Json &r : rows("MasterLiveGekisouRankingScoreBonus")) {
        master.live.gekiso.ranking_bonuses.push_back({
            row_i32(r, "missionPattern"),
            row_i32(r, "count"),
            row_i32(r, "rank"),
            row_i32(r, "scoreBonusPercent"),
        });
    }

    for (const Json &r : rows("MasterCharacter")) {
        int32_t cid = row_i32(r, "id", 0, "characterId");
        master.character_band[cid] = row_i32(r, "bandID", 0, "bandId");
    }

    for (const Json &r : rows("MasterMemberCard")) {
        GkMemberCard card;
        card.id = row_i32(r, "id");
        card.character_id = row_i32(r, "characterID", 0, "characterId");
        auto bit = master.character_band.find(card.character_id);
        card.band_id = (bit != master.character_band.end()) ? bit->second : 0;
        card.rarity = row_i32(r, "rarity");
        card.card_type = row_i32(r, "cardType");
        card.best_tags = row_ids(r, "bestMusicTagIDs", "bestMusicTagIds");
        card.stat_max = row_stat(r, "performancePowerMax", "technicPowerMax", "visualPowerMax");
        card.level_group = row_i32(r, "memberCardLevelGroup");
        card.awake_group = row_i32(r, "memberCardAwakeGroup");
        card.rank_group = row_i32(r, "memberCardRankGroup");
        card.live_skill_id = row_i32(r, "liveSkillID", 0, "liveSkillId");
        card.gekiso_skill_id = row_i32(r, "gekisouSkillID", 0, "gekisouSkillId");
        card.leader_skill_id = row_i32(r, "leaderSkillID", 0, "leaderSkillId");
        master.members[card.id] = std::move(card);
    }

    std::unordered_set<int32_t> gk_sup_ids;
    for (const Json &r : rows("MasterGekisouSupportSkill")) {
        gk_sup_ids.insert(row_i32(r, "id"));
    }
    auto norm_gk = [&](int32_t id) -> int32_t {
        if (gk_sup_ids.count(id))
            return id;
        if (id > 1000 && gk_sup_ids.count(id / 1000))
            return id / 1000;
        return id;
    };
    for (const Json &r : rows("MasterSupportCard")) {
        GkSnapCard card;
        card.id = row_i32(r, "id");
        card.character_ids = row_ids(r, "characterIDs", "characterIds");
        for (int32_t cid : card.character_ids) {
            int32_t bid = master.character_band[cid];
            if (bid != 0 &&
                std::find(card.band_ids.begin(), card.band_ids.end(), bid) == card.band_ids.end()) {
                card.band_ids.push_back(bid);
            }
        }
        card.rarity = row_i32(r, "rarity");
        card.card_type = row_i32(r, "cardType");
        card.stat_max = row_stat(r, "performancePowerMax", "technicPowerMax", "visualPowerMax");
        card.level_group = row_i32(r, "supportCardLevelGroup");
        card.rank_group = row_i32(r, "supportCardRankGroup");
        card.support_skill_ids = {row_i32(r, "supportSkillId01"), row_i32(r, "supportSkillId02")};
        card.gekiso_support_skill_ids = {
            norm_gk(row_i32(r, "gekisouSupportSkillId01")),
            norm_gk(row_i32(r, "gekisouSupportSkillId02")),
        };
        master.snaps[card.id] = std::move(card);
    }

    for (const Json &r : rows("MasterMemberCardLevel")) {
        master.member_levels[row_i32(r, "group")][row_i32(r, "level")] =
            row_stat(r, "performanceRate", "technicRate", "visualRate");
    }
    for (const Json &r : rows("MasterMemberCardAwake")) {
        master.member_awake[row_i32(r, "group")][row_i32(r, "awakeCount")] =
            row_stat(r, "performanceRate", "technicRate", "visualRate");
    }
    for (const Json &r : rows("MasterMemberCardRank")) {
        master.member_ranks[row_i32(r, "group")][row_i32(r, "rank")] = {
            row_stat(r, "performanceRate", "technicRate", "visualRate"),
            row_i32(r, "leaderSkillLevel", 1),
            row_i32(r, "musicTypeBonusRate"),
            row_i32(r, "musicTagBonusRate"),
        };
    }
    for (const Json &r : rows("MasterMemberCardLevelLimit")) {
        int64_t key = (static_cast<int64_t>(row_i32(r, "rarity")) << 32) |
                      static_cast<uint32_t>(row_i32(r, "awakeCount"));
        master.member_level_limits[key] = row_i32(r, "limitLevel");
    }
    for (const Json &r : rows("MasterSupportCardLevel")) {
        master.snap_levels[row_i32(r, "group")][row_i32(r, "level")] =
            row_stat(r, "performanceRate", "technicRate", "visualRate");
    }
    for (const Json &r : rows("MasterSupportCardRank")) {
        master.snap_ranks[row_i32(r, "group")][row_i32(r, "rank")] = {
            row_i32(r, "cardTypeLinkBonusRate"),
            row_i32(r, "limitLevel"),
            {row_i32(r, "supportSkill01Level"), row_i32(r, "supportSkill02Level")},
            {row_i32(r, "gekisouSupportSkill01Level"), row_i32(r, "gekisouSupportSkill02Level")},
        };
    }
    for (const Json &r : rows("MasterCharacterRank")) {
        master.character_rank_bonus.emplace_back(row_i32(r, "rank"), row_i32(r, "bonus"));
    }
    std::sort(master.character_rank_bonus.begin(), master.character_rank_bonus.end());
    for (const Json &r : rows("MasterCharacterTotalRank")) {
        master.total_rank_bonus.emplace_back(row_i32(r, "totalRank"), row_i32(r, "bonus"));
    }
    std::sort(master.total_rank_bonus.begin(), master.total_rank_bonus.end());
    for (const Json &r : rows("MasterVipRankBonus")) {
        if (row_i32(r, "vipBonusType") == 7)
            master.vip_bonus[row_i32(r, "vipRank")] = row_i32(r, "value");
    }

    for (const Json &r : rows("MasterSkillTarget")) {
        TargetRow tr = parse_target_row(r);
        master.targets[tr.id] = std::move(tr);
    }
    for (const Json &r : rows("MasterSkillCondition")) {
        ConditionRow cr;
        cr.id = row_i32(r, "id");
        cr.type = row_i32(r, "conditionType");
        cr.target_ids = row_ids(r, "conditionTargetIDs");
        cr.values = row_list(r, "conditionValues");
        const Json *pos = row_field(r, "isPositive");
        cr.positive = !(pos && pos->is_bool() && !pos->as_bool());
        master.conditions[cr.id] = std::move(cr);
    }
    for (const Json &r : rows("MasterSkillConditionSet")) {
        master.condition_sets[row_i32(r, "group")].push_back(row_ids(r, "conditionIds"));
    }
    for (const Json &r : rows("MasterSkillCumulativeCondition")) {
        CumulativeRow cr;
        cr.id = row_i32(r, "id");
        cr.type = row_i32(r, "skillCumulativeConditionType", 0, "conditionType");
        cr.target_ids = row_ids(r, "conditionTargetIDs");
        cr.values = row_list(r, "conditionValues");
        cr.cap = row_i32(r, "maxCumulativeCount");
        master.cumulative[cr.id] = std::move(cr);
    }

    auto load_skill_map = [&](const std::string &skill_table, const std::string &effect_table,
                              const std::string &fk) {
        std::unordered_map<int32_t, std::vector<SkillEffectRow>> grouped;
        for (const Json &r : rows(effect_table)) {
            grouped[row_i32(r, fk)].push_back(parse_skill_effect(r));
        }
        std::unordered_map<int32_t, std::vector<SkillEffectRow>> out;
        for (const Json &r : rows(skill_table)) {
            int32_t sid = row_i32(r, "id");
            out[sid] = grouped[sid];
        }
        return out;
    };
    master.leader_skills =
        load_skill_map("MasterLeaderSkill", "MasterLeaderSkillEffect", "leaderSkillID");
    master.live_skills = load_skill_map("MasterLiveSkill", "MasterLiveSkillEffect", "liveSkillID");
    master.support_skills =
        load_skill_map("MasterSupportSkill", "MasterSupportSkillEffect", "supportSkillID");

    auto load_gekiso_map = [&](const std::string &skill_table, const std::string &effect_table,
                               const std::string &fk) {
        std::unordered_map<int32_t, std::vector<SkillEffectRow>> grouped;
        for (const Json &r : rows(effect_table)) {
            grouped[row_i32(r, fk)].push_back(parse_skill_effect(r));
        }
        std::unordered_map<int32_t, GekisoSkillEntry> out;
        for (const Json &r : rows(skill_table)) {
            int32_t sid = row_i32(r, "id");
            out[sid] = {row_i32(r, "gekisouMissionType"), std::move(grouped[sid])};
        }
        return out;
    };
    master.gekiso_skills =
        load_gekiso_map("MasterGekisouSkill", "MasterGekisouSkillEffect", "gekisouSkillID");
    master.gekiso_support_skills = load_gekiso_map(
        "MasterGekisouSupportSkill", "MasterGekisouSupportSkillEffect", "gekisouSupportSkillID");

    for (const Json &r : rows("MasterLiveSkill")) {
        master.live_skill_categories[row_i32(r, "id")] = row_list(r, "skillCategories");
    }
    for (const Json &r : rows("MasterGekisouSkill")) {
        master.gekiso_skill_categories[row_i32(r, "id")] = row_list(r, "skillCategories");
    }
    for (const Json &r : rows("MasterBandItem")) {
        BandItemEntry item;
        item.band_id = row_i32(r, "bandID", 0, "bandId");
        master.band_items[row_i32(r, "id")] = std::move(item);
    }
    for (const Json &r : rows("MasterBandItemSkillEffect")) {
        int32_t id = row_i32(r, "bandItemID", 0, "bandItemId");
        auto it = master.band_items.find(id);
        if (it == master.band_items.end())
            continue;
        auto effect = parse_skill_effect(r);
        it->second.levels[effect.level].push_back(std::move(effect));
    }

    return master;
}

int32_t member_level_cap(const EngineMaster &master, const GkMemberCard &card, int32_t awake) {
    int64_t k = (static_cast<int64_t>(card.rarity) << 32) | static_cast<uint32_t>(awake);
    auto it = master.member_level_limits.find(k);
    if (it != master.member_level_limits.end())
        return it->second;
    int32_t mx = 1;
    auto git = master.member_levels.find(card.level_group);
    if (git != master.member_levels.end()) {
        for (const auto &[lv, _] : git->second)
            mx = std::max(mx, lv);
    }
    return mx;
}

int32_t snap_level_cap(const EngineMaster &master, const GkSnapCard &card, int32_t rank) {
    auto rgit = master.snap_ranks.find(card.rank_group);
    if (rgit != master.snap_ranks.end()) {
        auto rit = rgit->second.find(rank);
        if (rit != rgit->second.end())
            return rit->second.limit_level;
    }
    int32_t mx = 1;
    auto lgit = master.snap_levels.find(card.level_group);
    if (lgit != master.snap_levels.end()) {
        for (const auto &[lv, _] : lgit->second)
            mx = std::max(mx, lv);
    }
    return mx;
}

int32_t max_awake(const EngineMaster &master, const GkMemberCard &card) {
    int32_t mx = 1;
    auto it = master.member_awake.find(card.awake_group);
    if (it != master.member_awake.end()) {
        for (const auto &[aw, _] : it->second)
            mx = std::max(mx, aw);
    }
    return mx;
}

int32_t max_member_rank(const EngineMaster &master, const GkMemberCard &card) {
    int32_t mx = 1;
    auto it = master.member_ranks.find(card.rank_group);
    if (it != master.member_ranks.end()) {
        for (const auto &[rk, _] : it->second)
            mx = std::max(mx, rk);
    }
    return mx;
}

int32_t max_snap_rank(const EngineMaster &master, const GkSnapCard &card) {
    int32_t mx = 1;
    auto it = master.snap_ranks.find(card.rank_group);
    if (it != master.snap_ranks.end()) {
        for (const auto &[rk, _] : it->second)
            mx = std::max(mx, rk);
    }
    return mx;
}

int32_t max_skill_level(const std::vector<SkillEffectRow> *rows) {
    int32_t mx = 1;
    if (rows) {
        for (const auto &r : *rows)
            mx = std::max(mx, r.level);
    }
    return mx;
}

std::optional<MemberState> resolve_member(const EngineMaster &master, const std::string &key,
                                          const GkMemberCard &card, const MemberGrowth &growth) {
    auto lgit = master.member_levels.find(card.level_group);
    if (lgit == master.member_levels.end())
        return std::nullopt;
    auto lit = lgit->second.find(growth.level);
    if (lit == lgit->second.end())
        return std::nullopt;

    Stat3 awake{0, 0, 0};
    auto agit = master.member_awake.find(card.awake_group);
    if (agit != master.member_awake.end() && agit->second.count(growth.awake)) {
        awake = agit->second.at(growth.awake);
    } else if (growth.awake > 1) {
        return std::nullopt;
    }

    auto rgit = master.member_ranks.find(card.rank_group);
    if (rgit == master.member_ranks.end())
        return std::nullopt;
    auto rit = rgit->second.find(growth.rank);
    if (rit == rgit->second.end())
        return std::nullopt;

    MemberState st;
    st.key = key;
    st.card = card;
    st.growth = growth;
    st.leader_skill_level = rit->second.leader_skill_level;
    st.music_type_rate = rit->second.music_type_rate;
    st.music_tag_rate = rit->second.music_tag_rate;
    for (int i = 0; i < 3; ++i) {
        int64_t lp = floor_f32_div_10000(static_cast<int64_t>(lit->second[i]) * card.stat_max[i]);
        int64_t ap = static_cast<int64_t>(
            std::floor(f32(f32(static_cast<double>(awake[i]) / 10000.0) * f32(card.stat_max[i]))));
        int64_t rp = floor_f32_div_10000(
            static_cast<int64_t>(wrap_mul(rit->second.rates[i], card.stat_max[i])));
        st.own[i] = (lp + ap + rp) * 10000;
    }
    if (auto it = master.live_skill_categories.find(card.live_skill_id);
        it != master.live_skill_categories.end())
        st.live_skill_categories = it->second;
    if (auto it = master.gekiso_skill_categories.find(card.gekiso_skill_id);
        it != master.gekiso_skill_categories.end())
        st.gekiso_skill_categories = it->second;
    if (auto it = master.gekiso_skills.find(card.gekiso_skill_id); it != master.gekiso_skills.end())
        st.gekiso_mission_type = it->second.mission_type;
    return st;
}

std::optional<SnapState> resolve_snap(const EngineMaster &master, const std::string &key,
                                      const GkSnapCard &card, const SnapGrowth &growth) {
    auto lgit = master.snap_levels.find(card.level_group);
    if (lgit == master.snap_levels.end())
        return std::nullopt;
    auto lit = lgit->second.find(growth.level);
    if (lit == lgit->second.end())
        return std::nullopt;

    auto rgit = master.snap_ranks.find(card.rank_group);
    if (rgit == master.snap_ranks.end())
        return std::nullopt;
    auto rit = rgit->second.find(growth.rank);
    if (rit == rgit->second.end())
        return std::nullopt;

    SnapState st;
    st.key = key;
    st.card = card;
    st.growth = growth;
    for (int i = 0; i < 3; ++i) {
        st.percent[i] =
            floor_f32_div_10000(static_cast<int64_t>(wrap_mul(lit->second[i], card.stat_max[i])));
    }
    st.type_link_rate = rit->second.type_link_rate;
    st.skill_levels = rit->second.skill_levels;
    st.gekiso_skill_levels = rit->second.gekiso_skill_levels;
    return st;
}

ResolvedBox resolve_box(const EngineMaster &master, const std::vector<MemberInput> &member_inputs,
                        const std::vector<SnapInput> &snap_inputs) {
    ResolvedBox out;
    for (const auto &inp : member_inputs) {
        auto cit = master.members.find(inp.card_id);
        if (cit == master.members.end()) {
            out.unknown.push_back(inp.key);
            continue;
        }
        const GkMemberCard &card = cit->second;
        int32_t max_aw = max_awake(master, card);
        int32_t max_rk = max_member_rank(master, card);
        const auto *ls = master.live_skills.count(card.live_skill_id)
                             ? &master.live_skills.at(card.live_skill_id)
                             : nullptr;
        const auto *gs = master.gekiso_skills.count(card.gekiso_skill_id)
                             ? &master.gekiso_skills.at(card.gekiso_skill_id).effects
                             : nullptr;
        int32_t max_lv_sk = max_skill_level(ls);
        int32_t max_gk_sk = max_skill_level(gs);
        int32_t awake = std::min(inp.awake.value_or(max_aw), max_aw);
        int32_t cap = member_level_cap(master, card, awake);
        MemberGrowth g;
        g.level = std::max(1, std::min(inp.level.value_or(cap), cap));
        g.awake = awake;
        g.rank = std::min(inp.rank.value_or(max_rk), max_rk);
        g.live_skill_level = std::min(inp.live_skill_level.value_or(max_lv_sk), max_lv_sk);
        g.gekiso_skill_level = std::min(inp.gekiso_skill_level.value_or(max_gk_sk), max_gk_sk);
        auto st = resolve_member(master, inp.key, card, g);
        if (st)
            out.members.push_back(std::move(*st));
        else
            out.unknown.push_back(inp.key);
    }
    for (const auto &inp : snap_inputs) {
        auto cit = master.snaps.find(inp.card_id);
        if (cit == master.snaps.end()) {
            out.unknown.push_back(inp.key);
            continue;
        }
        const GkSnapCard &card = cit->second;
        int32_t max_rk = max_snap_rank(master, card);
        int32_t rank = std::min(inp.rank.value_or(max_rk), max_rk);
        int32_t cap = snap_level_cap(master, card, rank);
        SnapGrowth g;
        g.level = std::max(1, std::min(inp.level.value_or(cap), cap));
        g.rank = rank;
        auto st = resolve_snap(master, inp.key, card, g);
        if (st)
            out.snaps.push_back(std::move(*st));
        else
            out.unknown.push_back(inp.key);
    }
    return out;
}

bool is_target_member(const MemberState &member, const TargetRow &target) {
    const auto &card = member.card;
    if (target.band_id > 0 && target.band_id == card.band_id)
        return true;
    if (target.card_type != 0 && target.card_type == card.card_type)
        return true;
    if (target.character_id > 0 && target.character_id == card.character_id)
        return true;
    if (target.tag_id > 0 && std::find(card.best_tags.begin(), card.best_tags.end(),
                                       target.tag_id) != card.best_tags.end())
        return true;
    for (int32_t c : target.live_skill_categories) {
        if (std::find(member.live_skill_categories.begin(), member.live_skill_categories.end(),
                      c) != member.live_skill_categories.end())
            return true;
    }
    for (int32_t c : target.gekisou_skill_categories) {
        if (std::find(member.gekiso_skill_categories.begin(), member.gekiso_skill_categories.end(),
                      c) != member.gekiso_skill_categories.end())
            return true;
    }
    return target.gekisou_mission_type != 0 &&
           target.gekisou_mission_type == member.gekiso_mission_type;
}

bool matches_targets(const EngineMaster &master, const MemberState &member,
                     const std::vector<int32_t> &target_ids) {
    for (int32_t id : target_ids) {
        auto it = master.targets.find(id);
        if (it != master.targets.end() && is_target_member(member, it->second))
            return true;
    }
    return false;
}

PrecomputedProbPower::PrecomputedProbPower(const Json *prob) {
    if (!prob || !prob->is_object())
        return;
    valid = true;
    const Json *catalog = prob->find("catalog");
    const Json *settings = prob->find("settings");
    const Json *song = prob->find("song");
    const Json *fix = (catalog && catalog->is_object()) ? catalog->find("fix") : nullptr;
    const Json *pm = (settings && settings->is_object()) ? settings->find("power_model") : nullptr;
    const Json *sources = (pm && pm->is_object()) ? pm->find("sources") : nullptr;

    auto src_enabled = [&](const std::string &name) -> bool {
        if (!sources || !sources->is_object())
            return true;
        const Json *v = sources->find(name);
        return !(v && v->is_bool() && !v->as_bool());
    };
    en_trained = src_enabled("trained");
    en_char_rank = src_enabled("character_rank");
    en_char_total_rank = src_enabled("character_total_rank");
    en_snapshot = src_enabled("snapshot");
    en_type_link = src_enabled("type_link");
    en_leader = src_enabled("leader");
    en_band_item = src_enabled("band_item");
    en_music_type = src_enabled("music_type");
    en_music_tag = src_enabled("music_tag");
    en_vip = src_enabled("vip");

    std::string rounding = "float32_floor";
    if (pm && pm->is_object()) {
        if (const Json *r = pm->find("rounding"); r && r->is_string())
            rounding = r->as_string();
    }
    if (rounding == "float32_floor")
        round_mode = RoundMode::Float32Floor;
    else if (rounding == "float64_floor")
        round_mode = RoundMode::Float64Floor;
    else if (rounding == "round")
        round_mode = RoundMode::Round;
    else
        round_mode = RoundMode::None;

    std::string type_link_source = "snapshot";
    if (settings && settings->is_object()) {
        if (const Json *tls = settings->find("type_link_bonus_source"); tls && tls->is_string())
            type_link_source = tls->as_string();
    }
    if (type_link_source == "snapshot")
        tl_source = TypeLinkSrc::Snapshot;
    else if (type_link_source == "member")
        tl_source = TypeLinkSrc::Member;
    else if (type_link_source == "both")
        tl_source = TypeLinkSrc::Both;
    else
        tl_source = TypeLinkSrc::Other;

    if (settings && settings->is_object()) {
        if (const Json *ev = settings->find("event"); ev && ev->is_object()) {
            if (const Json *aep = ev->find("apply_event_power"); aep && aep->is_bool())
                apply_event_power = aep->as_bool();
        }
    }

    auto get_vec3 = [](const Json *obj, const std::string &key,
                       int64_t def = 0) -> std::array<int64_t, 3> {
        std::array<int64_t, 3> res{def, def, def};
        if (!obj || !obj->is_object())
            return res;
        const Json *a = obj->find(key);
        if (!a || !a->is_array())
            return res;
        const auto &arr = a->as_array();
        for (size_t i = 0; i < 3 && i < arr.size(); ++i)
            res[i] = json_i32(&arr[i], static_cast<int32_t>(def));
        return res;
    };
    auto parse_vec3_map = [&](const Json *obj, const std::string &map_key,
                              const std::array<int64_t, 3> &fallback,
                              std::unordered_map<int32_t, std::array<int64_t, 3>> &out_map) {
        if (!obj || !obj->is_object())
            return;
        const Json *mp = obj->find(map_key);
        if (!mp || !mp->is_object())
            return;
        for (const auto &[k, entry] : mp->fields()) {
            if (!entry.is_array())
                continue;
            int32_t id = 0;
            try {
                id = std::stoi(k);
            } catch (...) {
                continue;
            }
            std::array<int64_t, 3> res = fallback;
            const auto &arr = entry.as_array();
            for (size_t i = 0; i < 3 && i < arr.size(); ++i)
                res[i] = json_i32(&arr[i], static_cast<int32_t>(fallback[i]));
            out_map[id] = res;
        }
    };

    song_type = song ? row_i32(*song, "type") : 0;
    song_tags = song ? row_list(*song, "tags") : std::vector<int32_t>{};

    def_cr = get_vec3(fix, "character_rank_bonus");
    def_ctr = get_vec3(fix, "character_total_rank_bonus");
    def_bi = get_vec3(fix, "band_item_bonus_bp");
    def_vip = get_vec3(fix, "vip_bonus_bp");
    mt_base_bp = fix ? row_i32(*fix, "music_type_base_bp") : 0;
    mtag_base_bp = fix ? row_i32(*fix, "music_tag_base_bp") : 0;
    tl_base_bp = fix ? row_i32(*fix, "type_link_base_bp") : 0;

    parse_vec3_map(fix, "character_rank_bonus_by_character", def_cr, cr_by_char);
    parse_vec3_map(fix, "band_item_bonus_bp_by_character", {0, 0, 0}, bi_by_char);
    parse_vec3_map(fix, "band_item_bonus_bp_by_band", def_bi, bi_by_band);
    parse_vec3_map(fix, "band_item_bonus_bp_by_card_type", {0, 0, 0}, bi_by_ct);

    if (pm && pm->is_object()) {
        if (const Json *exs = pm->find("extra_sources"); exs && exs->is_array()) {
            for (const Json &src : exs->as_array()) {
                std::string kind = row_str(src, "kind", "rate_bp");
                auto vals = get_vec3(&src, "rate_bp");
                if (vals[0] == 0 && vals[1] == 0 && vals[2] == 0)
                    vals = get_vec3(&src, "flat_power");
                extra_sources.push_back({kind == "flat", vals});
            }
        }
    }

    if (catalog && catalog->is_object()) {
        if (const Json *ms = catalog->find("members"); ms && ms->is_array()) {
            for (const Json &m : ms->as_array()) {
                CatMember cm;
                if (const Json *ls = m.find("leader_skill"); ls && ls->is_number())
                    cm.leader_skill = json_i32(ls);
                cm.ev_bp = row_i32(m, "event_bonus_bp");
                if (const Json *crb = m.find("card_rank_bonus_bp"); crb && crb->is_object()) {
                    cm.has_crb = true;
                    if (const Json *v = crb->find("music_type"); v && v->is_number())
                        cm.crb_mt = json_i32(v);
                    if (const Json *v = crb->find("music_tag"); v && v->is_number())
                        cm.crb_mtag = json_i32(v);
                    cm.crb_tl = row_i32(*crb, "type_link", 0);
                }
                cat_members[row_i32(m, "id")] = cm;
            }
        }
        if (const Json *ss = catalog->find("snapshots"); ss && ss->is_array()) {
            for (const Json &s : ss->as_array()) {
                CatSnap cs;
                cs.ev_bp = row_i32(s, "event_bonus_bp");
                if (const Json *rk = s.find("rank"); rk && rk->is_number())
                    cs.rank = json_i32(rk);
                if (const Json *crb = s.find("card_rank_bonus_bp"); crb && crb->is_object()) {
                    cs.has_crb = true;
                    if (const Json *v = crb->find("type_link"); v && v->is_number())
                        cs.crb_tl = json_i32(v);
                }
                cat_snaps[row_i32(s, "id")] = cs;
            }
        }
        if (const Json *ts = catalog->find("targets"); ts && ts->is_array()) {
            for (const Json &t : ts->as_array()) {
                CatTarget ct;
                ct.band = row_i32(t, "band");
                ct.character = row_i32(t, "character");
                ct.card_type = row_i32(t, "card_type");
                ct.tag = row_i32(t, "tag");
                ct.gekisou_mission_type = row_i32(t, "gekisou_mission_type");
                ct.music_type = row_i32(t, "music_type");
                ct.live_skill_categories = row_list(t, "live_skill_categories");
                ct.gekisou_skill_categories = row_list(t, "gekisou_skill_categories");
                target_map[row_i32(t, "id")] = std::move(ct);
            }
        }
        if (const Json *cs = catalog->find("conditions"); cs && cs->is_array()) {
            for (const Json &c : cs->as_array()) {
                CatCond cc;
                cc.condition_type = row_i32(c, "type", 0, "condition_type");
                const Json *pos = c.find("positive");
                cc.is_pos = !(pos && pos->is_bool() && !pos->as_bool());
                cc.targets = row_list(c, "targets");
                cond_map[row_i32(c, "id")] = std::move(cc);
            }
        }
        if (const Json *cgs = catalog->find("condition_groups"); cgs && cgs->is_array()) {
            for (const Json &cg : cgs->as_array()) {
                std::vector<std::vector<int32_t>> sets_vec;
                const Json *sets = cg.find("rows");
                if (!sets)
                    sets = cg.find("sets");
                if (sets && sets->is_array()) {
                    for (const Json &set_item : sets->as_array()) {
                        const Json *cids_json =
                            set_item.is_object() ? set_item.find("conditions") : &set_item;
                        if (!cids_json || !cids_json->is_array())
                            continue;
                        std::vector<int32_t> cids;
                        for (const Json &cid_val : cids_json->as_array())
                            cids.push_back(json_i32(&cid_val));
                        sets_vec.push_back(std::move(cids));
                    }
                }
                cg_map[row_i32(cg, "group", 0, "id")] = std::move(sets_vec);
            }
        }
        if (const Json *gks = catalog->find("gekisou_skills"); gks && gks->is_array()) {
            for (const Json &gk : gks->as_array())
                gk_mission_by_skill[row_i32(gk, "id")] = row_i32(gk, "mission_type");
        }
        if (const Json *lskills = catalog->find("leader_skills"); lskills && lskills->is_array()) {
            for (const Json &sk : lskills->as_array()) {
                std::vector<CatLeaderEff> eff_vec;
                if (const Json *effs = sk.find("effects"); effs && effs->is_array()) {
                    for (const Json &eff : effs->as_array()) {
                        CatLeaderEff ce;
                        ce.level = row_i32(eff, "level");
                        ce.condition_group = row_i32(eff, "condition_group");
                        ce.effect_type = row_i32(eff, "effect_type");
                        ce.cumulative_type = row_i32(eff, "cumulative_type");
                        ce.max_cumulative_count = row_i32(eff, "max_cumulative_count");
                        ce.value = row_i32(eff, "value");
                        ce.max_value = row_i32(eff, "max_value");
                        ce.cumulative_targets = row_list(eff, "cumulative_targets");
                        ce.targets = row_list(eff, "targets");
                        eff_vec.push_back(std::move(ce));
                    }
                }
                leader_skills[row_i32(sk, "id")] = std::move(eff_vec);
            }
        }
    }
}

double PrecomputedProbPower::round_val(double value) const {
    switch (round_mode) {
    case RoundMode::Float32Floor: {
        int64_t scaled = static_cast<int64_t>(std::trunc(value * 10000.0));
        return std::floor(f32(f32(scaled) / 10000.0f));
    }
    case RoundMode::Float64Floor:
        return std::floor(value);
    case RoundMode::Round:
        return std::round(value);
    case RoundMode::None:
        return value;
    }
    return value;
}

double PrecomputedProbPower::apply_rate(double base_val, int64_t rate_bp) const {
    if (rate_bp == 0)
        return 0.0;
    if (round_mode == RoundMode::Float32Floor) {
        int64_t base_scaled = static_cast<int64_t>(std::llround(base_val * 10000.0));
        int64_t mul =
            static_cast<int64_t>(std::trunc(static_cast<double>(base_scaled * rate_bp) / 10000.0));
        return std::floor(f32(f32(mul) / 10000.0f));
    }
    return round_val((base_val * static_cast<double>(rate_bp)) / 10000.0);
}

bool PrecomputedProbPower::match_cat_targets(const MemberState &m,
                                             const std::vector<int32_t> &tids) const {
    if (tids.empty())
        return true;
    for (int32_t tid : tids) {
        auto tit = target_map.find(tid);
        if (tit == target_map.end())
            continue;
        const CatTarget &t = tit->second;
        bool matched = false;
        if (t.band > 0 && t.band == m.card.band_id)
            matched = true;
        if (!matched && t.character > 0 && t.character == m.card.character_id)
            matched = true;
        if (!matched && t.card_type > 0 && t.card_type == m.card.card_type)
            matched = true;
        if (!matched && t.tag > 0 &&
            std::find(m.card.best_tags.begin(), m.card.best_tags.end(), t.tag) !=
                m.card.best_tags.end())
            matched = true;
        if (!matched && t.gekisou_mission_type > 0) {
            int32_t m_gm = m.gekiso_mission_type;
            if (m_gm == 0) {
                auto gkit = gk_mission_by_skill.find(m.card.gekiso_skill_id);
                if (gkit != gk_mission_by_skill.end())
                    m_gm = gkit->second;
            }
            if (m_gm == t.gekisou_mission_type)
                matched = true;
        }
        if (!matched) {
            for (int32_t cat : t.live_skill_categories) {
                if (cat != 0 &&
                    std::find(m.live_skill_categories.begin(), m.live_skill_categories.end(),
                              cat) != m.live_skill_categories.end()) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            for (int32_t cat : t.gekisou_skill_categories) {
                if (cat != 0 &&
                    std::find(m.gekiso_skill_categories.begin(), m.gekiso_skill_categories.end(),
                              cat) != m.gekiso_skill_categories.end()) {
                    matched = true;
                    break;
                }
            }
        }
        if (matched)
            return true;
    }
    return false;
}

std::vector<std::array<int64_t, 3>>
PrecomputedProbPower::compute_leader_rates(const std::vector<MemberState> &members,
                                           size_t leader_idx) const {
    std::vector<std::array<int64_t, 3>> leader_rates(members.size(), {0, 0, 0});
    if (!en_leader || leader_idx >= members.size())
        return leader_rates;
    const MemberState &leader_m = members[leader_idx];
    auto cmit = cat_members.find(leader_m.card.id);
    int32_t l_skill_id = (cmit != cat_members.end() && cmit->second.leader_skill.has_value())
                             ? *cmit->second.leader_skill
                             : leader_m.card.leader_skill_id;
    int32_t l_level = leader_m.leader_skill_level > 0 ? leader_m.leader_skill_level : 5;
    auto lsit = leader_skills.find(l_skill_id);
    if (lsit == leader_skills.end())
        return leader_rates;

    auto check_cond_group = [&](int32_t group_id) -> bool {
        if (group_id <= 0)
            return true;
        auto git = cg_map.find(group_id);
        if (git == cg_map.end())
            return false;
        if (git->second.empty())
            return true;
        for (const auto &cids : git->second) {
            bool set_ok = true;
            for (int32_t cid : cids) {
                auto cit = cond_map.find(cid);
                if (cit == cond_map.end()) {
                    set_ok = false;
                    break;
                }
                const CatCond &cond = cit->second;
                bool raw_ok = false;
                if (cond.condition_type == 0) {
                    raw_ok = true;
                } else if (cond.condition_type == 3000) {
                    for (const auto &m : members) {
                        if (match_cat_targets(m, cond.targets)) {
                            raw_ok = true;
                            break;
                        }
                    }
                } else if (cond.condition_type == 3001) {
                    raw_ok = !members.empty();
                    for (const auto &m : members) {
                        if (!match_cat_targets(m, cond.targets)) {
                            raw_ok = false;
                            break;
                        }
                    }
                } else if (cond.condition_type == 4012) {
                    if (cond.targets.empty()) {
                        raw_ok = true;
                    } else {
                        for (int32_t tid : cond.targets) {
                            auto tit = target_map.find(tid);
                            if (tit != target_map.end() && tit->second.music_type == song_type) {
                                raw_ok = true;
                                break;
                            }
                        }
                    }
                } else if (cond.condition_type == 5000) {
                    raw_ok = match_cat_targets(leader_m, cond.targets);
                }
                if ((cond.is_pos ? raw_ok : !raw_ok) == false) {
                    set_ok = false;
                    break;
                }
            }
            if (set_ok)
                return true;
        }
        return false;
    };

    for (const CatLeaderEff &eff : lsit->second) {
        if (eff.level != 0 && eff.level != l_level)
            continue;
        const int32_t et = eff.effect_type;
        if (et != 1000 && et != 1001 && et != 1002 && et != 1003 && et != 1500 && et != 1501 &&
            et != 1502 && et != 1503)
            continue;
        if (!check_cond_group(eff.condition_group))
            continue;
        int64_t mult = 1;
        if (eff.cumulative_type != 0) {
            int64_t count = 0;
            std::vector<int32_t> seen;
            switch (eff.cumulative_type) {
            case 3000:
                for (const auto &m : members)
                    if (match_cat_targets(m, eff.cumulative_targets))
                        ++count;
                break;
            case 3001:
                for (size_t i = 0; i < members.size(); ++i)
                    if (i != leader_idx && match_cat_targets(members[i], eff.cumulative_targets))
                        ++count;
                break;
            case 3002:
                for (const auto &m : members)
                    if (m.card.band_id == leader_m.card.band_id)
                        ++count;
                break;
            case 3003:
                for (const auto &m : members) {
                    if (m.card.band_id != leader_m.card.band_id &&
                        std::find(seen.begin(), seen.end(), m.card.band_id) == seen.end()) {
                        seen.push_back(m.card.band_id);
                    }
                }
                count = static_cast<int64_t>(seen.size());
                break;
            case 3004:
                for (const auto &m : members) {
                    if (std::find(seen.begin(), seen.end(), m.card.band_id) == seen.end())
                        seen.push_back(m.card.band_id);
                }
                count = static_cast<int64_t>(seen.size());
                break;
            case 3005:
                for (const auto &m : members) {
                    if (std::find(seen.begin(), seen.end(), m.card.card_type) == seen.end())
                        seen.push_back(m.card.card_type);
                }
                count = static_cast<int64_t>(seen.size());
                break;
            default:
                throw SpecError("队长技能累计条件类型未支持: " +
                                std::to_string(eff.cumulative_type));
            }
            if (eff.max_cumulative_count > 0 && count > eff.max_cumulative_count)
                count = eff.max_cumulative_count;
            mult = count;
        }
        int64_t val = static_cast<int64_t>(eff.value) * mult;
        if (eff.max_value > 0 && val > eff.max_value)
            val = eff.max_value;
        for (size_t i = 0; i < members.size(); ++i) {
            if (!match_cat_targets(members[i], eff.targets))
                continue;
            if (et == 1000 || et == 1500) {
                leader_rates[i][0] += val;
                leader_rates[i][1] += val;
                leader_rates[i][2] += val;
            } else if (et == 1003 || et == 1503) {
                leader_rates[i][0] += val;
            } else if (et == 1001 || et == 1501) {
                leader_rates[i][1] += val;
            } else if (et == 1002 || et == 1502) {
                leader_rates[i][2] += val;
            }
        }
    }
    return leader_rates;
}

std::array<double, 3> PrecomputedProbPower::member_base_vals(const MemberState &m) const {
    auto cmit = cat_members.find(m.card.id);
    int64_t m_ev_bp = (apply_event_power && cmit != cat_members.end()) ? cmit->second.ev_bp : 0;
    auto crit = cr_by_char.find(m.card.character_id);
    const auto &cr_vec = (crit != cr_by_char.end()) ? crit->second : def_cr;
    std::array<double, 3> out{0.0, 0.0, 0.0};
    for (int d = 0; d < 3; ++d) {
        double trained_d = std::round(static_cast<double>(m.own[d]) / 10000.0);
        double mem_val = en_trained ? trained_d : 0.0;
        if (en_trained && m_ev_bp != 0)
            mem_val += apply_rate(mem_val, m_ev_bp);
        double cr_val = en_char_rank ? static_cast<double>(cr_vec[d]) : 0.0;
        double ctr_val = en_char_total_rank ? static_cast<double>(def_ctr[d]) : 0.0;
        out[d] = round_val(mem_val + cr_val + ctr_val);
    }
    return out;
}

double PrecomputedProbPower::member_fixed_power(const MemberState &m) const {
    auto cmit = cat_members.find(m.card.id);
    const CatMember *cat_m = (cmit != cat_members.end()) ? &cmit->second : nullptr;
    int64_t m_mt_bp = (cat_m && cat_m->has_crb && cat_m->crb_mt.has_value())
                          ? *cat_m->crb_mt
                          : static_cast<int64_t>(m.music_type_rate);
    int64_t m_mtag_bp = (cat_m && cat_m->has_crb && cat_m->crb_mtag.has_value())
                            ? *cat_m->crb_mtag
                            : static_cast<int64_t>(m.music_tag_rate);
    std::array<int64_t, 3> bi_char{0, 0, 0};
    if (auto it = bi_by_char.find(m.card.character_id); it != bi_by_char.end())
        bi_char = it->second;
    std::array<int64_t, 3> bi_band = def_bi;
    if (auto it = bi_by_band.find(m.card.band_id); it != bi_by_band.end())
        bi_band = it->second;
    std::array<int64_t, 3> bi_ct{0, 0, 0};
    if (auto it = bi_by_ct.find(m.card.card_type); it != bi_by_ct.end())
        bi_ct = it->second;

    bool tag_matched = false;
    for (int32_t tg : m.card.best_tags) {
        if (std::find(song_tags.begin(), song_tags.end(), tg) != song_tags.end()) {
            tag_matched = true;
            break;
        }
    }
    auto base_vals = member_base_vals(m);
    double total = 0.0;
    for (int d = 0; d < 3; ++d) {
        double base_val = base_vals[d];
        double bi_val =
            en_band_item ? apply_rate(base_val, bi_char[d] + bi_band[d] + bi_ct[d]) : 0.0;
        double mt_val = 0.0;
        if (en_music_type &&
            (song_type == 99 || m.card.card_type == 99 || m.card.card_type == song_type)) {
            mt_val = apply_rate(base_val, mt_base_bp + m_mt_bp);
        }
        double mtag_val = 0.0;
        if (en_music_tag && tag_matched) {
            mtag_val = apply_rate(base_val, mtag_base_bp + m_mtag_bp);
        }
        double vip_val = en_vip ? apply_rate(base_val, def_vip[d]) : 0.0;
        double extra_val = 0.0;
        for (const auto &src : extra_sources) {
            if (src.is_flat)
                extra_val += round_val(static_cast<double>(src.vals[d]));
            else
                extra_val += apply_rate(base_val, src.vals[d]);
        }
        total += base_val + bi_val + mt_val + mtag_val + vip_val + extra_val;
    }
    return total;
}

double PrecomputedProbPower::snap_add_power(const MemberState &m, const SnapState *s) const {
    if (!s)
        return 0.0;
    auto cmit = cat_members.find(m.card.id);
    const CatMember *cat_m = (cmit != cat_members.end()) ? &cmit->second : nullptr;
    auto csit = cat_snaps.find(s->card.id);
    const CatSnap *cat_s = (csit != cat_snaps.end()) ? &csit->second : nullptr;
    int64_t s_ev_bp = (apply_event_power && cat_s) ? cat_s->ev_bp : 0;
    int64_t m_tl_bp = (cat_m && cat_m->has_crb) ? cat_m->crb_tl : 0;
    int64_t s_tl_bp = static_cast<int64_t>(s->type_link_rate);
    if (cat_s && cat_s->has_crb && s->growth.rank == cat_s->rank.value_or(s->growth.rank)) {
        s_tl_bp = cat_s->crb_tl.value_or(s->type_link_rate);
    }
    auto base_vals = member_base_vals(m);
    double total = 0.0;
    for (int d = 0; d < 3; ++d) {
        double base_val = base_vals[d];
        double snap_val = 0.0;
        if (en_snapshot) {
            int64_t s_rate = s->percent[d] + s_ev_bp;
            snap_val = apply_rate(base_val, s_rate);
        }
        double tl_val = 0.0;
        if (en_type_link && s->card.card_type == m.card.card_type) {
            int64_t r = tl_base_bp;
            if (tl_source == TypeLinkSrc::Member)
                r += m_tl_bp;
            else if (tl_source == TypeLinkSrc::Snapshot)
                r += s_tl_bp;
            else if (tl_source == TypeLinkSrc::Both)
                r += m_tl_bp + s_tl_bp;
            tl_val = apply_rate(base_val, r);
        }
        total += snap_val + tl_val;
    }
    return total;
}

std::vector<double>
PrecomputedProbPower::leader_slot_powers(const std::vector<MemberState> &members,
                                         size_t leader_idx) const {
    std::vector<double> out(members.size(), 0.0);
    if (!en_leader || leader_idx >= members.size())
        return out;
    auto leader_rates = compute_leader_rates(members, leader_idx);
    for (size_t idx = 0; idx < members.size(); ++idx) {
        auto base_vals = member_base_vals(members[idx]);
        double ld_total = 0.0;
        for (int d = 0; d < 3; ++d)
            ld_total += apply_rate(base_vals[d], leader_rates[idx][d]);
        out[idx] = ld_total;
    }
    return out;
}

TeamPowerResult PrecomputedProbPower::calc(const std::vector<MemberState> &members,
                                           const std::vector<const SnapState *> &snaps,
                                           size_t leader_idx) const {
    auto leader_rates = compute_leader_rates(members, leader_idx);
    TeamPowerResult out;
    out.slots.resize(members.size(), 0.0);
    for (size_t idx = 0; idx < members.size(); ++idx) {
        const MemberState &m = members[idx];
        const SnapState *s = idx < snaps.size() ? snaps[idx] : nullptr;
        auto cmit = cat_members.find(m.card.id);
        const CatMember *cat_m = (cmit != cat_members.end()) ? &cmit->second : nullptr;
        auto csit = s ? cat_snaps.find(s->card.id) : cat_snaps.end();
        const CatSnap *cat_s = (s && csit != cat_snaps.end()) ? &csit->second : nullptr;

        int64_t m_ev_bp = (apply_event_power && cat_m) ? cat_m->ev_bp : 0;
        int64_t s_ev_bp = (apply_event_power && cat_s) ? cat_s->ev_bp : 0;

        int64_t m_mt_bp = (cat_m && cat_m->has_crb && cat_m->crb_mt.has_value())
                              ? *cat_m->crb_mt
                              : static_cast<int64_t>(m.music_type_rate);
        int64_t m_mtag_bp = (cat_m && cat_m->has_crb && cat_m->crb_mtag.has_value())
                                ? *cat_m->crb_mtag
                                : static_cast<int64_t>(m.music_tag_rate);
        int64_t m_tl_bp = (cat_m && cat_m->has_crb) ? cat_m->crb_tl : 0;
        int64_t s_tl_bp = s ? static_cast<int64_t>(s->type_link_rate) : 0;
        if (s && cat_s && cat_s->has_crb &&
            s->growth.rank == cat_s->rank.value_or(s->growth.rank)) {
            s_tl_bp = cat_s->crb_tl.value_or(s->type_link_rate);
        }

        auto crit = cr_by_char.find(m.card.character_id);
        const auto &cr_vec = (crit != cr_by_char.end()) ? crit->second : def_cr;
        std::array<int64_t, 3> bi_char{0, 0, 0};
        if (auto it = bi_by_char.find(m.card.character_id); it != bi_by_char.end())
            bi_char = it->second;
        std::array<int64_t, 3> bi_band = def_bi;
        if (auto it = bi_by_band.find(m.card.band_id); it != bi_by_band.end())
            bi_band = it->second;
        std::array<int64_t, 3> bi_ct{0, 0, 0};
        if (auto it = bi_by_ct.find(m.card.card_type); it != bi_by_ct.end())
            bi_ct = it->second;

        bool tag_matched = false;
        for (int32_t tg : m.card.best_tags) {
            if (std::find(song_tags.begin(), song_tags.end(), tg) != song_tags.end()) {
                tag_matched = true;
                break;
            }
        }

        double slot_total = 0.0;
        for (int d = 0; d < 3; ++d) {
            double trained_d = std::round(static_cast<double>(m.own[d]) / 10000.0);
            double mem_val = en_trained ? trained_d : 0.0;
            if (en_trained && m_ev_bp != 0)
                mem_val += apply_rate(mem_val, m_ev_bp);
            double cr_val = en_char_rank ? static_cast<double>(cr_vec[d]) : 0.0;
            double ctr_val = en_char_total_rank ? static_cast<double>(def_ctr[d]) : 0.0;
            double base_val = round_val(mem_val + cr_val + ctr_val);

            double snap_val = 0.0;
            if (en_snapshot && s) {
                int64_t s_rate = s->percent[d] + s_ev_bp;
                snap_val = apply_rate(base_val, s_rate);
            }

            double tl_val = 0.0;
            if (en_type_link && s && s->card.card_type == m.card.card_type) {
                int64_t r = tl_base_bp;
                if (tl_source == TypeLinkSrc::Member)
                    r += m_tl_bp;
                else if (tl_source == TypeLinkSrc::Snapshot)
                    r += s_tl_bp;
                else if (tl_source == TypeLinkSrc::Both)
                    r += m_tl_bp + s_tl_bp;
                tl_val = apply_rate(base_val, r);
            }

            double ld_val = en_leader ? apply_rate(base_val, leader_rates[idx][d]) : 0.0;
            double bi_val =
                en_band_item ? apply_rate(base_val, bi_char[d] + bi_band[d] + bi_ct[d]) : 0.0;

            double mt_val = 0.0;
            if (en_music_type &&
                (song_type == 99 || m.card.card_type == 99 || m.card.card_type == song_type)) {
                mt_val = apply_rate(base_val, mt_base_bp + m_mt_bp);
            }

            double mtag_val = 0.0;
            if (en_music_tag && tag_matched) {
                mtag_val = apply_rate(base_val, mtag_base_bp + m_mtag_bp);
            }

            double vip_val = en_vip ? apply_rate(base_val, def_vip[d]) : 0.0;

            double extra_val = 0.0;
            for (const auto &src : extra_sources) {
                if (src.is_flat)
                    extra_val += round_val(static_cast<double>(src.vals[d]));
                else
                    extra_val += apply_rate(base_val, src.vals[d]);
            }

            slot_total += base_val + snap_val + tl_val + ld_val + bi_val + mt_val + mtag_val +
                          vip_val + extra_val;
        }
        out.slots[idx] = slot_total;
        out.total += slot_total;
    }
    return out;
}

TeamPowerResult calc_deckcalc_team_power(const Json *prob, const EngineMaster &master,
                                         const std::vector<MemberState> &members,
                                         const std::vector<const SnapState *> &snaps,
                                         size_t leader_idx) {
    (void)master;
    PrecomputedProbPower pre(prob);
    return pre.calc(members, snaps, leader_idx);
}

} // namespace gekisou_detail
} // namespace deckcalc
