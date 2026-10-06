#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "model.hpp"

using namespace deckcalc;

namespace {

int g_passed = 0;
int g_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++g_passed;                                                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                                          \
    } while (false)

#define CHECK_SPEC_ERROR(expression, needle)                                                       \
    do {                                                                                           \
        bool ok = false;                                                                           \
        try {                                                                                      \
            expression;                                                                            \
        } catch (const SpecError &error) {                                                         \
            ok = std::string(error.what()).find(needle) != std::string::npos;                      \
            if (!ok) {                                                                             \
                std::fprintf(stderr, "%s:%d: 错误消息不符: %s\n", __FILE__, __LINE__,              \
                             error.what());                                                        \
            }                                                                                      \
        } catch (const std::exception &error) {                                                    \
            std::fprintf(stderr, "%s:%d: 非 SpecError: %s\n", __FILE__, __LINE__, error.what());   \
        }                                                                                          \
        if (ok) {                                                                                  \
            ++g_passed;                                                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
        }                                                                                          \
    } while (false)

bool replace_all(std::string &text, const std::string &from, const std::string &to) {
    const std::size_t position = text.find(from);
    if (position == std::string::npos) {
        return false;
    }
    text.replace(position, from.size(), to);
    return true;
}

const Member *find_member(const Problem &problem, int64_t id) {
    for (const Member &member : problem.catalog.members) {
        if (member.id == id) {
            return &member;
        }
    }
    return nullptr;
}

const Snapshot *find_snapshot(const Problem &problem, int64_t id) {
    for (const Snapshot &snapshot : problem.catalog.snapshots) {
        if (snapshot.id == id) {
            return &snapshot;
        }
    }
    return nullptr;
}

bool parses(const std::string &text) {
    try {
        parse_problem(Json::parse(text));
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

std::string formation_text(int64_t leader, const std::vector<Slot> &slots, bool with_trigger) {
    std::string out = "{\"schema\":\"" + std::string(kSchemaFormation) +
                      "\",\"leader\":" + std::to_string(leader) + ",\"slots\":[";
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += "{\"member\":" + std::to_string(slots[i].member) +
               ",\"snapshot\":" + std::to_string(slots[i].snapshot);
        if (with_trigger) {
            out += ",\"trigger\":" + std::to_string(slots[i].trigger);
        }
        out += "}";
    }
    return out + "]}";
}

const char *kMinimalProblem = R"JSON({
  "schema": "ournotes-deck-problem@1",
  "input_mode": "experimental",
  "song": {"id": 1, "title": "t", "type": 1},
  "chart": {"difficulty": "expert", "level": 20, "notes": [{"t": 0, "op": 1}], "skill_times_ms": [1000]},
  "catalog": {
    "members": [{"id": 1, "character": 1, "band": 1, "card_type": 1, "trained": [100, 100, 100]}],
    "snapshots": [{"id": 10, "trained": [1000, 1000, 1000]}],
    "note_parameters": [{"op": 1, "score_percent": 100}]
  },
  "settings": {"team_size": 1}
})JSON";

const char *kSettingsKey = "\"settings\": {\"team_size\": 1}";

}

int main() {
    const char *from_env = std::getenv("OURNOTES_PROBLEM");
    const std::string path = from_env != nullptr ? from_env : "/tmp/prob-100109-expert.json";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::printf("test_model: %s 不存在，跳过（先用 tools/compile_from_inputs.py 生成）\n",
                    path.c_str());
        return 0;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();

    Problem problem;
    try {
        problem = parse_problem(Json::parse(buffer.str()));
    } catch (const std::exception &error) {
        std::printf("test_model: 参考 problem 解析失败: %s\n", error.what());
        return 1;
    }

    CHECK(problem.catalog.members.size() == 63);
    CHECK(problem.catalog.snapshots.size() == 64);

    std::size_t judged = 0;
    for (const Note &note : problem.chart.notes) {
        if (note.scoring) {
            ++judged;
        }
    }
    CHECK(judged == 759);

    const std::vector<int64_t> expected_times{9411, 21960, 47058, 59607, 84705};
    CHECK(problem.chart.skill_times_ms == expected_times);

    bool op_1 = false;
    bool op_120 = false;
    for (const NoteParameter &parameter : problem.catalog.note_parameters) {
        if (parameter.op == 1 && parameter.score_percent == 100) {
            op_1 = true;
        }
        if (parameter.op == 120 && parameter.score_percent == 10) {
            op_120 = true;
        }
    }
    CHECK(op_1);
    CHECK(op_120);

    const Member *member_61 = find_member(problem, 61);
    CHECK(member_61 != nullptr);
    if (member_61 != nullptr) {
        CHECK(member_61->live_skill == 5);
        CHECK(member_61->leader_skill == 53);
        CHECK(member_61->rank == 5);
        CHECK(member_61->card_rank_bonus_bp.type_link == 0);
        CHECK(member_61->card_rank_bonus_bp.music_type == 2000);
        CHECK(member_61->card_rank_bonus_bp.music_tag == 2000);
    }
    const Snapshot *snapshot_33 = find_snapshot(problem, 33);
    CHECK(snapshot_33 != nullptr);
    if (snapshot_33 != nullptr) {
        CHECK(snapshot_33->card_rank_bonus_bp.type_link == 2000);
    }

    CHECK(problem.available_members().size() == 63);
    CHECK(problem.available_snapshots().size() == 64);
    CHECK(problem.leader_candidates().size() == 63);

    const Json described = problem.describe();
    CHECK(described.at("song").at("id").as_int64() == 100109);
    CHECK(described.at("chart").at("scoring_notes").as_int64() == 759);
    CHECK(described.at("catalog").at("members").as_int64() == 63);
    CHECK(described.at("available").at("members").as_int64() == 63);
    CHECK(described.at("available").at("leaders").as_int64() == 63);
    CHECK(described.at("settings").at("team_size").as_int64() == 5);
    CHECK(described.at("settings").at("power_model").at("sources").at("snapshot").as_bool());
    CHECK(described.at("settings").at("skill").at("overlap").as_string() == "additive");
    CHECK(described.at("constraints").at("member_pool").is_null());
    CHECK(described.at("search").at("mode").as_string() == "rank");

    std::vector<const Member *> picked;
    std::set<int> characters;
    for (const Member *member : problem.available_members()) {
        if (characters.insert(member->character).second) {
            picked.push_back(member);
        }
        if (picked.size() == 5) {
            break;
        }
    }
    const std::vector<const Snapshot *> snapshots = problem.available_snapshots();
    CHECK(picked.size() == 5);
    CHECK(snapshots.size() >= 5);
    if (picked.size() == 5 && snapshots.size() >= 5) {
        const int triggers[5] = {3, 1, 5, 2, 4};
        std::vector<int64_t> member_for_trigger(5, 0);
        std::vector<int64_t> snapshot_for_trigger(5, 0);
        std::vector<Slot> document_slots;
        for (std::size_t position = 0; position < 5; ++position) {
            const std::size_t trigger = static_cast<std::size_t>(triggers[position] - 1);
            member_for_trigger[trigger] = picked[position]->id;
            snapshot_for_trigger[trigger] = snapshots[position]->id;
            document_slots.push_back(
                Slot{picked[position]->id, snapshots[position]->id, triggers[position]});
        }

        Formation formation;
        formation.leader = picked[2]->id;
        for (int trigger = 1; trigger <= 5; ++trigger) {
            formation.slots.push_back(
                Slot{member_for_trigger[static_cast<std::size_t>(trigger - 1)],
                     snapshot_for_trigger[static_cast<std::size_t>(trigger - 1)], trigger});
        }
        bool validated = true;
        try {
            formation.validate(problem);
        } catch (const SpecError &error) {
            validated = false;
            std::fprintf(stderr, "formation.validate 失败: %s\n", error.what());
        }
        CHECK(validated);

        const Json dumped = dump_formation(formation);
        CHECK(dumped.size() == 3);
        CHECK(dumped.at("schema").as_string() == kSchemaFormation);
        CHECK(dumped.at("leader").as_int64() == formation.leader);
        CHECK(dumped.at("slots").size() == 5);
        CHECK(dumped.at("slots").at(0).size() == 3);

        const Formation round_trip =
            parse_formation(Json::parse(formation_text(formation.leader, document_slots, true)));
        CHECK(round_trip.leader == formation.leader);
        CHECK(round_trip.slots.size() == 5);
        bool ordered = true;
        for (std::size_t i = 0; i < round_trip.slots.size(); ++i) {
            ordered = ordered && round_trip.slots[i].trigger == static_cast<int>(i) + 1 &&
                      round_trip.slots[i].member == member_for_trigger[i] &&
                      round_trip.slots[i].snapshot == snapshot_for_trigger[i];
        }
        CHECK(ordered);
        CHECK(parse_formation(Json::parse(dump_formation(round_trip).dump())).slots.size() == 5);

        const Formation defaults =
            parse_formation(Json::parse(formation_text(formation.leader, formation.slots, false)));
        bool default_triggers = defaults.slots.size() == 5;
        for (std::size_t i = 0; i < defaults.slots.size(); ++i) {
            default_triggers =
                default_triggers && defaults.slots[i].trigger == static_cast<int>(i) + 1;
        }
        CHECK(default_triggers);
        CHECK_SPEC_ERROR(
            parse_formation(Json::parse("{\"schema\":\"" + std::string(kSchemaFormation) +
                                        "\",\"leader\":1,\"slots\":[]}")),
            "formation.slots");
    }

    CHECK(parses(kMinimalProblem));

    std::string wrong_schema(kMinimalProblem);
    CHECK(replace_all(wrong_schema, "ournotes-deck-problem@1", "ournotes-deck-problem@2"));
    CHECK_SPEC_ERROR(parse_problem(Json::parse(wrong_schema)), "problem.schema");

    std::string wrong_times(kMinimalProblem);
    CHECK(replace_all(wrong_times, "\"skill_times_ms\": [1000]",
                      "\"skill_times_ms\": [1000, 2000, 3000, 4000]"));
    CHECK_SPEC_ERROR(parse_problem(Json::parse(wrong_times)), "chart.skill_times_ms");

    std::string missing_op(kMinimalProblem);
    CHECK(replace_all(missing_op, "{\"op\": 1, \"score_percent\": 100}",
                      "{\"op\": 2, \"score_percent\": 100}"));
    CHECK_SPEC_ERROR(parse_problem(Json::parse(missing_op)), "catalog.note_parameters");

    std::string unknown_key(kMinimalProblem);
    CHECK(replace_all(unknown_key, kSettingsKey, "\"settings\": {\"team_size\": 1, \"bogus\": 1}"));
    CHECK_SPEC_ERROR(parse_problem(Json::parse(unknown_key)), "settings.bogus");

    std::string bad_overlap(kMinimalProblem);
    CHECK(replace_all(bad_overlap, kSettingsKey,
                      "\"settings\": {\"team_size\": 1, \"skill\": {\"overlap\": \"multiply\"}}"));
    CHECK_SPEC_ERROR(parse_problem(Json::parse(bad_overlap)),
                     "settings.skill.overlap: v1 只支持 'additive'");

    if (g_failed != 0) {
        std::printf("test_model: %d checks passed, %d failed\n", g_passed, g_failed);
        return 1;
    }
    std::printf("test_model: %d checks passed\n", g_passed);
    return 0;
}
