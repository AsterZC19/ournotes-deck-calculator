// deckcalc 命令行入口。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "engine.hpp"
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
    std::cout <<
        "deckcalc " << deckcalc::kToolVersion << "\n"
        "\n"
        "用法:\n"
        "  deckcalc validate -p problem.json [-o out.json]\n"
        "  deckcalc score    -p problem.json -f formation.json [-o out.json] [--detail]\n"
        "                    [--order-search exact|given]\n"
        "  deckcalc rank     -p problem.json [-o out.json] [--top N] [--time-limit S]\n"
        "                    [--method auto|fast|exact] [--leaders 61,62] [--detail] [--quiet]\n"
        "\n"
        "-p/-f 传 \"-\" 表示从 stdin 读取，-o 省略表示写到 stdout。\n";
}

struct Args {
    std::string command;
    std::string problem;
    std::string formation;
    std::string output;
    std::string order_search;
    std::string method = "auto";
    std::string leaders;
    int top = 0;
    double time_limit = -1;
    bool detail = false;
    bool quiet = false;
};

std::string need_value(int argc, char** argv, int& i, const char* flag) {
    if (i + 1 >= argc) {
        throw SpecError(std::string("参数缺少取值: ") + flag);
    }
    return argv[++i];
}

Args parse_args(int argc, char** argv) {
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
            args.problem = need_value(argc, argv, i, "--problem");
        } else if (flag == "-f" || flag == "--formation") {
            args.formation = need_value(argc, argv, i, "--formation");
        } else if (flag == "-o" || flag == "--output") {
            args.output = need_value(argc, argv, i, "--output");
        } else if (flag == "--order-search") {
            args.order_search = need_value(argc, argv, i, "--order-search");
        } else if (flag == "--method") {
            args.method = need_value(argc, argv, i, "--method");
        } else if (flag == "--leaders") {
            args.leaders = need_value(argc, argv, i, "--leaders");
        } else if (flag == "--top") {
            args.top = std::stoi(need_value(argc, argv, i, "--top"));
        } else if (flag == "--time-limit") {
            args.time_limit = std::stod(need_value(argc, argv, i, "--time-limit"));
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
    return args;
}

void write_output(const Json& document, const std::string& path) {
    if (path.empty() || path == "-") {
        std::cout << document.dump(2) << "\n";
        return;
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        throw SpecError("无法写入 " + path);
    }
    std::string text = document.dump(2);
    text.push_back('\n');
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

Json problem_block(const Problem& problem, const Engine& engine) {
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

Json score_document(const Problem& problem, const Engine& engine, const Formation& formation,
                    const Args& args) {
    deckcalc::EvalOptions options;
    options.order_search = args.order_search.empty() ? problem.search.order_search : args.order_search;
    options.detail = args.detail;
    Evaluation evaluation = engine.evaluate(formation, options);

    Json results = Json::array();
    results.push_back(evaluation.to_json(args.detail, false, 0));

    Json document = Json::object();
    document.set("schema", Json(deckcalc::kSchemaResult));
    document.set("kind", Json("score"));
    Json tool = Json::object();
    tool.set("name", Json(deckcalc::kToolName));
    tool.set("version", Json(deckcalc::kToolVersion));
    document.set("tool", tool);
    document.set("problem", problem_block(problem, engine));
    document.set("model", engine.model_block());
    document.set("chart_analysis", engine.chart_analysis());
    document.set("results", results);
    Json warnings = Json::array();
    for (const std::string& warning : evaluation.warnings) {
        warnings.push_back(Json(warning));
    }
    document.set("warnings", warnings);
    return document;
}

Json rank_document(const Problem& problem, const Engine& engine, const deckcalc::RankResult& ranked,
                   const Args& args) {
    Json results = Json::array();
    int rank = 0;
    std::vector<std::string> warnings;
    for (const Evaluation& evaluation : ranked.results) {
        results.push_back(evaluation.to_json(args.detail, true, ++rank));
        for (const std::string& warning : evaluation.warnings) {
            bool seen = false;
            for (const std::string& existing : warnings) {
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
    search.set("time_limit_s", Json(args.time_limit >= 0 ? args.time_limit : problem.search.time_limit_s));
    search.set("leaders", Json(args.leaders));
    document.set("search", search);
    document.set("audit", ranked.audit);

    Json warning_array = Json::array();
    for (const std::string& warning : warnings) {
        warning_array.push_back(Json(warning));
    }
    document.set("warnings", warning_array);
    return document;
}

std::vector<int64_t> parse_id_list(const std::string& text) {
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

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);
        if (args.command != "validate" && args.command != "score" && args.command != "rank") {
            throw SpecError("未知子命令: " + args.command);
        }

        Problem problem = deckcalc::parse_problem(Json::read(args.problem));

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

        Engine engine(problem);
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
        options.top = args.top > 0 ? args.top : problem.search.top;
        options.time_limit_s = args.time_limit >= 0 ? args.time_limit : problem.search.time_limit_s;
        options.verbose = !args.quiet;
        options.leaders = parse_id_list(args.leaders);
        if (!args.quiet) {
            options.progress = [](const std::string& message) {
                std::cerr << message << "\n";
            };
        }
        deckcalc::RankResult ranked = deckcalc::rank_formations(engine, options);
        write_output(rank_document(problem, engine, ranked, args), args.output);
        return 0;
    } catch (const SpecError& error) {
        std::cerr << "错误: " << error.what() << "\n";
        return 2;
    } catch (const JsonError& error) {
        std::cerr << "错误: " << error.what() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "错误: " << error.what() << "\n";
        return 3;
    }
}
