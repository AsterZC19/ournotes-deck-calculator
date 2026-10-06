#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine.hpp"
#include "event.hpp"
#include "json.hpp"
#include "model.hpp"
#include "solver.hpp"

namespace {

using deckcalc::Engine;
using deckcalc::Evaluation;
using deckcalc::Formation;
using deckcalc::Json;
using deckcalc::JsonError;
using deckcalc::Problem;
using deckcalc::SpecError;

void usage() {
    std::cout
        << "deckcalc " << deckcalc::kToolVersion
        << "\n"
           "\n"
           "用法:\n"
           "  deckcalc validate -p problem.json [-o out.json]\n"
           "  deckcalc score    -p problem.json -f formation.json [-o out.json] [--detail]\n"
           "                    [--order-search exact|given]\n"
           "  deckcalc event    -p problem.json [-o out.json] [--time-limit S] [--method "
           "fast|exact]\n"
           "                    [--objective mean|score] (默认平均收益，score 为最佳顺序收益)\n"
           "                    [--challenge-problem challenge.json] [--leaders 61,62]\n"
           "                    [-p normal2.json ...] (批量计算并复用缓存)\n"
           "  deckcalc rank     -p problem.json [-o out.json] [--top N] [--time-limit S]\n"
           "                    [--method auto|fast|exact] [--leaders 61,62] [--detail] [--quiet]\n"
           "                    [--objective mean|score|index] [--beam-width 64] [--restarts 4] "
           "[--anneal-steps 3000] [--seed 0]\n"
           "                    [--warm-start previous-results.json]\n"
           "                    [--exclude-member-sets excluded.json] "
           "(逐名证明时排除已证明成员组合)\n"
           "\n"
           "实验输入必须标记 input_mode=experimental，并显式传 --experimental。\n"
           "-p/-f 传 \"-\" 表示从 stdin 读取，-o 省略表示写到 stdout。\n";
}

struct Args {
    std::string command;
    std::string problem;
    std::vector<std::string> problems;
    std::string formation;
    std::string challenge_problem;
    std::string excluded_member_sets;
    std::string warm_start;
    std::string output;
    std::string order_search;
    std::string method = "auto";
    std::string objective = "theoretical_score";
    bool objective_explicit = false;
    bool experimental = false;
    std::string leaders;
    int top = 0;
    int beam_width = 64, restarts = 4, anneal_steps = 3000;
    std::optional<int64_t> seed;
    double time_limit = -1;
    bool detail = false;
    bool quiet = false;
};

std::string need_value(int argc, char **argv, int &i, const char *flag) {
    if (i + 1 >= argc) {
        throw SpecError(std::string("参数缺少取值: ") + flag);
    }
    return argv[++i];
}

Args parse_args(int argc, char **argv) {
    Args args;
    if (argc < 2) {
        usage();
        std::exit(0);
    }
    args.command = argv[1];
    if (args.command == "-h" || args.command == "--help" || args.command == "help") {
        usage();
        std::exit(0);
    }
    if (args.command == "--version") {
        std::cout << deckcalc::kToolVersion << "\n";
        std::exit(0);
    }
    for (int i = 2; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "-p" || flag == "--problem") {
            args.problems.push_back(need_value(argc, argv, i, "--problem"));
            args.problem = args.problems.front();
        } else if (flag == "--experimental") {
            args.experimental = true;
        } else if (flag == "--warm-start") {
            args.warm_start = need_value(argc, argv, i, "--warm-start");
        } else if (flag == "--exclude-member-sets") {
            args.excluded_member_sets = need_value(argc, argv, i, "--exclude-member-sets");
        } else if (flag == "--challenge-problem") {
            args.challenge_problem = need_value(argc, argv, i, "--challenge-problem");
        } else if (flag == "-f" || flag == "--formation") {
            args.formation = need_value(argc, argv, i, "--formation");
        } else if (flag == "-o" || flag == "--output") {
            args.output = need_value(argc, argv, i, "--output");
        } else if (flag == "--order-search") {
            args.order_search = need_value(argc, argv, i, "--order-search");
        } else if (flag == "--objective") {
            args.objective_explicit = true;
            args.objective = need_value(argc, argv, i, "--objective");
            if (args.objective == "mean")
                args.objective = "mean_score";
            if (args.objective == "score")
                args.objective = "theoretical_score";
        } else if (flag == "--method") {
            args.method = need_value(argc, argv, i, "--method");
        } else if (flag == "--leaders") {
            args.leaders = need_value(argc, argv, i, "--leaders");
        } else if (flag == "--top") {
            args.top = std::stoi(need_value(argc, argv, i, "--top"));
        } else if (flag == "--time-limit") {
            args.time_limit = std::stod(need_value(argc, argv, i, "--time-limit"));
        } else if (flag == "--beam-width") {
            args.beam_width = std::stoi(need_value(argc, argv, i, "--beam-width"));
        } else if (flag == "--restarts") {
            args.restarts = std::stoi(need_value(argc, argv, i, "--restarts"));
        } else if (flag == "--anneal-steps") {
            args.anneal_steps = std::stoi(need_value(argc, argv, i, "--anneal-steps"));
        } else if (flag == "--seed") {
            args.seed = std::stoll(need_value(argc, argv, i, "--seed"));
        } else if (flag == "--detail") {
            args.detail = true;
        } else if (flag == "--quiet") {
            args.quiet = true;
        } else {
            throw SpecError("未知参数: " + flag);
        }
    }
    if (args.problem.empty()) {
        throw SpecError("缺少 --problem");
    }
    if (args.problems.size() > 1 && args.command != "event")
        throw SpecError("多个 --problem 仅用于 event 批量计算");
    if (std::count(args.problems.begin(), args.problems.end(), "-") +
            (args.challenge_problem == "-") >
        1)
        throw SpecError("stdin 只能读取一个输入");
    if (args.beam_width < 1 || args.restarts < 0 || args.anneal_steps < 0)
        throw SpecError("beam-width 必须 >=1，restarts/anneal-steps 必须 >=0");
    if (args.objective != "theoretical_score" && args.objective != "index" &&
        args.objective != "mean_score")
        throw SpecError("objective 必须为 mean/score/index");
    if (args.method != "auto" && args.method != "fast" && args.method != "exact")
        throw SpecError("method 必须为 auto/fast/exact");
    return args;
}

void write_output(const Json &document, const std::string &path) {
    if (path.empty() || path == "-") {
        std::cout << document.dump(2) << "\n";
        return;
    }
    FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        throw SpecError("无法写入 " + path);
    }
    std::string text = document.dump(2);
    text.push_back('\n');
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

Json problem_block(const Problem &problem, const Engine &engine) {
    Json catalog = Json::object();
    catalog.set("members", Json(static_cast<int64_t>(problem.catalog.members.size())));
    catalog.set("snapshots", Json(static_cast<int64_t>(problem.catalog.snapshots.size())));
    catalog.set("available_members", Json(static_cast<int64_t>(engine.members().size())));
    catalog.set("available_snapshots", Json(static_cast<int64_t>(engine.snapshots().size())));

    Json chart = Json::object();
    chart.set("difficulty", Json(problem.chart.difficulty));
    chart.set("level", Json(problem.chart.level));
    chart.set("judged_notes", Json(static_cast<int64_t>(engine.chart().judged_notes)));

    Json song = Json::object();
    song.set("id", Json(problem.song.id));
    song.set("title", Json(problem.song.title));
    song.set("type", Json(problem.song.type));

    Json block = Json::object();
    block.set("song", song);
    block.set("chart", chart);
    block.set("catalog", catalog);
    return block;
}

Json score_document(const Problem &problem, const Engine &engine, const Formation &formation,
                    const Args &args) {
    deckcalc::EvalOptions options;
    options.order_search =
        args.order_search.empty() ? problem.search.order_search : args.order_search;
    options.detail = args.detail;
    if (args.objective_explicit &&
        (args.objective == "theoretical_score" || args.objective == "mean_score") &&
        options.order_search == "given")
        throw SpecError("平均分或理论最高分需要枚举技能顺序，不能同时使用 --order-search given");
    Evaluation evaluation = args.objective_explicit && args.objective == "theoretical_score"
                                ? engine.evaluate_theoretical(formation, args.detail)
                            : args.objective == "mean_score" && options.order_search != "given"
                                ? engine.evaluate_mean(formation, args.detail)
                                : engine.evaluate(formation, options);

    Json results = Json::array();
    results.push_back(evaluation.to_json(args.detail, false, 0));

    Json document = Json::object();
    document.set("schema", Json(deckcalc::kSchemaResult));
    document.set("kind", Json("score"));
    document.set("input_mode", Json(problem.input_mode));
    Json tool = Json::object();
    tool.set("name", Json(deckcalc::kToolName));
    tool.set("version", Json(deckcalc::kToolVersion));
    document.set("tool", tool);
    document.set("problem", problem_block(problem, engine));
    document.set("model", engine.model_block());
    document.set("chart_analysis", engine.chart_analysis());
    document.set("results", results);
    Json warnings = Json::array();
    for (const std::string &warning : evaluation.warnings) {
        warnings.push_back(Json(warning));
    }
    document.set("warnings", warnings);
    return document;
}

Json rank_document(const Problem &problem, const Engine &engine, const deckcalc::RankResult &ranked,
                   const Args &args) {
    Json results = Json::array();
    int rank = 0;
    std::vector<std::string> warnings;
    for (const Evaluation &candidate : ranked.results) {
        Evaluation evaluation = candidate;
        if (args.detail) {
            Formation formation;
            formation.leader = candidate.leader;
            for (const auto &slot : candidate.slots)
                formation.slots.push_back(deckcalc::Slot{slot.member, slot.snapshot, slot.trigger});
            deckcalc::EvalOptions options;
            options.detail = true;
            evaluation = candidate.ranking_objective == "theoretical_score"
                             ? engine.evaluate_theoretical(formation, true)
                         : candidate.ranking_objective == "mean_score"
                             ? engine.evaluate_mean(formation, true)
                             : engine.evaluate(formation, options);
        }
        results.push_back(evaluation.to_json(args.detail, true, ++rank));
        for (const std::string &warning : evaluation.warnings) {
            bool seen = false;
            for (const std::string &existing : warnings) {
                seen = seen || existing == warning;
            }
            if (!seen) {
                warnings.push_back(warning);
            }
        }
    }

    Json document = Json::object();
    document.set("schema", Json(deckcalc::kSchemaResult));
    document.set("kind", Json("rank"));
    document.set("input_mode", Json(problem.input_mode));
    Json tool = Json::object();
    tool.set("name", Json(deckcalc::kToolName));
    tool.set("version", Json(deckcalc::kToolVersion));
    document.set("tool", tool);
    document.set("problem", problem_block(problem, engine));
    document.set("model", engine.model_block());
    document.set("chart_analysis", engine.chart_analysis());
    document.set("results", results);

    Json search = Json::object();
    search.set("method", Json(args.method));
    search.set("top", Json(static_cast<int64_t>(args.top > 0 ? args.top : problem.search.top)));
    search.set("time_limit_s",
               Json(args.time_limit >= 0 ? args.time_limit : problem.search.time_limit_s));
    search.set("leaders", Json(args.leaders));
    document.set("search", search);
    document.set("audit", ranked.audit);

    Json warning_array = Json::array();
    for (const std::string &warning : warnings) {
        warning_array.push_back(Json(warning));
    }
    document.set("warnings", warning_array);
    return document;
}

std::vector<int64_t> parse_id_list(const std::string &text) {
    std::vector<int64_t> ids;
    std::string current;
    for (size_t i = 0; i <= text.size(); ++i) {
        char c = i < text.size() ? text[i] : ',';
        if (c == ',' || c == ' ') {
            if (!current.empty()) {
                ids.push_back(std::stoll(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    return ids;
}

}

int main(int argc, char **argv) {
    try {
        Args args = parse_args(argc, argv);
        if (args.command != "validate" && args.command != "score" && args.command != "rank" &&
            args.command != "event") {
            throw SpecError("未知子命令: " + args.command);
        }

        Problem problem = deckcalc::parse_problem(Json::read(args.problem));
        auto check_mode = [&](const Problem &input) {
            if (input.input_mode == "experimental" && !args.experimental)
                throw SpecError("实验输入需要显式传 --experimental");
        };
        check_mode(problem);
        if (!args.objective_explicit && (problem.input_mode == "game" || args.command == "event"))
            args.objective = "mean_score";
        if (args.objective == "index" && problem.input_mode == "game")
            throw SpecError("index 是实验代理目标；游戏推荐请使用 mean 或 score");
        if (args.seed)
            problem.search.seed = *args.seed;

        if (args.command == "validate") {
            Json describe = problem.describe();
            describe.set("ok", Json(true));
            Json tool = Json::object();
            tool.set("name", Json(deckcalc::kToolName));
            tool.set("version", Json(deckcalc::kToolVersion));
            describe.set("tool", tool);
            write_output(describe, args.output);
            return 0;
        }

        deckcalc::EngineCache engine_cache;
        Engine engine(problem, args.command == "event" ? &engine_cache : nullptr);
        if (args.command == "score") {
            if (args.formation.empty()) {
                throw SpecError("score 需要 --formation");
            }
            Formation formation = deckcalc::parse_formation(Json::read(args.formation));
            write_output(score_document(problem, engine, formation, args), args.output);
            return 0;
        }

        deckcalc::RankOptions options;
        options.method = args.method;
        options.objective = args.objective;
        options.top = args.top > 0 ? args.top : problem.search.top;
        options.time_limit_s = args.time_limit >= 0 ? args.time_limit : problem.search.time_limit_s;
        options.beam_width = args.beam_width;
        options.restarts = args.restarts;
        options.anneal_steps = args.anneal_steps;
        options.verbose = !args.quiet;
        options.leaders = parse_id_list(args.leaders);
        if (!args.warm_start.empty()) {
            if (args.command != "rank")
                throw SpecError("warm-start 仅用于 rank");
            const Json seeds = Json::read(args.warm_start);
            const Json *rows = seeds.is_array() ? &seeds : seeds.find("results");
            if (!rows || !rows->is_array())
                throw SpecError("warm-start 需要 formation 数组或带 results 的计算结果");
            for (const auto &row : rows->items()) {
                if (row.find("slots"))
                    options.initial_formations.push_back(deckcalc::parse_formation(row));
                else {
                    Formation formation;
                    formation.leader = row.at("leader").as_int64();
                    for (const auto &a : row.at("assignments").items()) {
                        const auto trigger = a.at("trigger").as_int64();
                        if (trigger < 1 || trigger > engine.team_size())
                            throw SpecError("warm-start 技能触发位越界");
                        formation.slots.push_back(deckcalc::Slot{a.at("member").as_int64(),
                                                                 a.at("snapshot").as_int64(),
                                                                 static_cast<int>(trigger)});
                    }
                    options.initial_formations.push_back(std::move(formation));
                }
            }
        }
        if (!args.excluded_member_sets.empty()) {
            if (args.command != "rank")
                throw SpecError("exclude-member-sets 仅用于 rank");
            Json excluded = Json::read(args.excluded_member_sets);
            for (const auto &row : excluded.items()) {
                std::vector<int64_t> ids;
                for (const auto &value : row.items())
                    ids.push_back(value.as_int64());
                options.excluded_member_sets.push_back(std::move(ids));
            }
        }
        if (!args.quiet) {
            options.progress = [](const std::string &message) { std::cerr << message << "\n"; };
        }
        if (args.command == "event") {
            std::optional<Problem> challenge;
            std::unique_ptr<Engine> challenge_engine;
            if (!args.challenge_problem.empty()) {
                challenge.emplace(deckcalc::parse_problem(Json::read(args.challenge_problem)));
                check_mode(*challenge);
                if (challenge->input_mode != problem.input_mode)
                    throw SpecError("普通曲与课题曲的输入模式必须一致");
                challenge_engine = std::make_unique<Engine>(*challenge, &engine_cache);
            }
            deckcalc::EventSearchCache phase_cache;
            auto calculate = [&](const Problem &input, const Engine &model) {
                auto per_song = options;
                per_song.time_limit_s =
                    args.time_limit >= 0 ? args.time_limit : input.search.time_limit_s;
                return deckcalc::event_recommend(model, per_song, challenge_engine.get(),
                                                 &phase_cache);
            };
            Json first = calculate(problem, engine);
            if (args.problems.size() == 1) {
                write_output(first, args.output);
                return 0;
            }
            Json plans = Json::array();
            plans.push_back(std::move(first));
            for (size_t i = 1; i < args.problems.size(); ++i) {
                Problem next = deckcalc::parse_problem(Json::read(args.problems[i]));
                check_mode(next);
                if (next.input_mode != problem.input_mode)
                    throw SpecError("批量输入的模式必须一致");
                if (args.seed)
                    next.search.seed = *args.seed;
                Engine model(next, &engine_cache);
                plans.push_back(calculate(next, model));
            }
            Json out = Json::object();
            out.set("schema", Json("ournotes-event-batch@1"));
            out.set("plans", std::move(plans));
            out.set("engine_cache", engine_cache.audit());
            out.set("search_cache", phase_cache.audit());
            write_output(out, args.output);
            return 0;
        }
        deckcalc::RankResult ranked = deckcalc::rank_formations(engine, options);
        write_output(rank_document(problem, engine, ranked, args), args.output);
        return 0;
    } catch (const SpecError &error) {
        std::cerr << "错误: " << error.what() << "\n";
        return 2;
    } catch (const JsonError &error) {
        std::cerr << "错误: " << error.what() << "\n";
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "错误: " << error.what() << "\n";
        return 3;
    }
}
