#!/usr/bin/env python3
"""激奏评分与组队 CLI 工具。

提供 score（激奏真实评分）与 rank（激奏最优组队）子命令，
底层调用 build/deckcalc 的原生实现。
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


def _run_bridge(subcommand: str, payload: dict[str, Any]) -> dict[str, Any]:
    repo_root = Path(__file__).resolve().parent.parent
    binary = repo_root / "build" / "deckcalc"
    if not binary.is_file():
        raise RuntimeError(f"找不到可执行文件: {binary}，请先运行 make")

    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as tf:
        json.dump(payload, tf, ensure_ascii=False)
        tf_path = tf.name

    try:
        cmd = [str(binary), f"gekisou-{subcommand}", "-i", tf_path]
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(repo_root))
    finally:
        try:
            os.remove(tf_path)
        except OSError:
            pass

    if proc.returncode != 0:
        err_msg = proc.stderr.strip()
        try:
            err_doc = json.loads(err_msg)
            if isinstance(err_doc, dict) and "message" in err_doc:
                raise RuntimeError(err_doc["message"])
        except json.JSONDecodeError:
            pass
        raise RuntimeError(err_msg or f"deckcalc 退出码 {proc.returncode}")

    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"解析 Gekisou 引擎返回 JSON 失败: {exc}\n输出: {proc.stdout}") from exc


def _parse_int_list(arg: str | None) -> list[int]:
    if arg is None:
        return []
    arg = arg.strip()
    if not arg:
        return []
    res: list[int] = []
    for chunk in arg.split(","):
        chunk = chunk.strip()
        if chunk:
            try:
                res.append(int(chunk))
            except ValueError:
                raise ValueError(f"无法将 '{chunk}' 解析为整数 ID")
    return res


def _parse_seeds(arg: str | None) -> list[int]:
    if arg is None:
        return []
    seeds = _parse_int_list(arg)
    for s in seeds:
        if s < 0:
            raise ValueError(f"随机种子必须为非负整数: {s}")
    return seeds


def cmd_score(args: argparse.Namespace) -> int:
    problem_path = Path(args.problem).expanduser().resolve()
    if not problem_path.is_file():
        print(f"错误: 找不到 problem 文件: {problem_path}", file=sys.stderr)
        return 2

    formation_path = Path(args.formation).expanduser().resolve()
    if not formation_path.is_file():
        print(f"错误: 找不到 formation 文件: {formation_path}", file=sys.stderr)
        return 2

    if args.accuracy_just < 0 or args.accuracy_great < 0:
        print("错误: accuracy 判定比例不能为负数", file=sys.stderr)
        return 2
    if args.accuracy_just + args.accuracy_great > 100.0:
        print(
            f"错误: Just ({args.accuracy_just}%) 与 Great ({args.accuracy_great}%) 判定比例总和不能超过 100%",
            file=sys.stderr,
        )
        return 2

    try:
        problem_doc = json.loads(problem_path.read_text(encoding="utf-8"))
        formation_doc = json.loads(formation_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"错误: JSON 解析失败: {exc}", file=sys.stderr)
        return 2

    try:
        seeds = _parse_seeds(args.seeds)
    except ValueError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 2

    payload: dict[str, Any] = {
        "problem": problem_doc,
        "formation": formation_doc,
        "options": {
            "assumed_placement": args.assumed_placement,
            "seeds": seeds,
            "accuracy": {
                "great": args.accuracy_great,
                "just": args.accuracy_just,
            },
        },
    }
    if args.master_dir:
        payload["master_dir"] = str(Path(args.master_dir).expanduser().resolve())

    try:
        result = _run_bridge("score", payload)
    except RuntimeError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1

    if args.output:
        out_path = Path(args.output).expanduser()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        if not args.quiet:
            print(f"评分结果已写入: {out_path}", file=sys.stderr)

    if not args.quiet or not args.output:
        print(
            f"激奏评分: mean={result['mean']} max={result['max']} min={result['min']} power={result['power']}"
        )
        print(
            f"假设排名 bonus: placement={result['assumed_placement']} range_percents={result['details']['range_percents']}"
        )
        print(
            f"抽签模式: {result['luck_mode']} (seeds={result['seeds']}, luck_present={result['details']['luck_present']})"
        )
        print(f"技能顺序: 最优 order={result['max_order']} 最劣 order={result['min_order']}")

    return 0


def cmd_rank(args: argparse.Namespace) -> int:
    problem_path = Path(args.problem).expanduser().resolve()
    if not problem_path.is_file():
        print(f"错误: 找不到 problem 文件: {problem_path}", file=sys.stderr)
        return 2

    if args.top_k <= 0:
        print(f"错误: top_k 必须为正整数 (当前: {args.top_k})", file=sys.stderr)
        return 2
    if args.time_limit_ms < 0:
        print(f"错误: time_limit_ms 不能为负数 (当前: {args.time_limit_ms})", file=sys.stderr)
        return 2
    if args.accuracy_just < 0 or args.accuracy_great < 0:
        print("错误: accuracy 判定比例不能为负数", file=sys.stderr)
        return 2
    if args.accuracy_just + args.accuracy_great > 100.0:
        print(
            f"错误: Just ({args.accuracy_just}%) 与 Great ({args.accuracy_great}%) 判定比例总和不能超过 100%",
            file=sys.stderr,
        )
        return 2

    try:
        problem_doc = json.loads(problem_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"错误: JSON 解析失败: {exc}", file=sys.stderr)
        return 2

    try:
        seeds = _parse_seeds(args.seeds)
        constraints: dict[str, Any] = {
            "required_members": _parse_int_list(args.required_members),
            "excluded_members": _parse_int_list(args.excluded_members),
            "required_snaps": _parse_int_list(args.required_snaps),
            "excluded_snaps": _parse_int_list(args.excluded_snaps),
            "leader": args.leader,
        }
    except ValueError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 2

    payload: dict[str, Any] = {
        "problem": problem_doc,
        "k": args.top_k,
        "criterion": args.criterion,
        "oracle": bool(args.oracle),
        "constraints": constraints,
        "options": {
            "assumed_placement": args.assumed_placement,
            "seeds": seeds,
            "time_limit_ms": args.time_limit_ms,
            "accuracy": {
                "great": args.accuracy_great,
                "just": args.accuracy_just,
            },
        },
    }
    if args.master_dir:
        payload["master_dir"] = str(Path(args.master_dir).expanduser().resolve())

    if args.candidates_file:
        cand_path = Path(args.candidates_file).expanduser().resolve()
        if not cand_path.is_file():
            print(f"错误: 找不到 candidates 文件: {cand_path}", file=sys.stderr)
            return 2
        payload["candidates"] = json.loads(cand_path.read_text(encoding="utf-8"))

    try:
        result = _run_bridge("rank", payload)
    except RuntimeError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1

    if args.output:
        out_path = Path(args.output).expanduser()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        if not args.quiet:
            print(f"组队结果已写入: {out_path}", file=sys.stderr)

    if not args.quiet or not args.output:
        print(f"激奏最优组队 (Top {len(result['hits'])}, 优化目标: {args.criterion}):")
        print(
            f"搜索完备性: search_complete={result['search_complete']} scope={result['optimality_scope']}"
        )
        print(f"抽签模式: {result['luck_mode']} (seeds={result['seeds']})")
        for hit in result["hits"]:
            members = hit["formation"]["member_ids"]
            snaps = hit["formation"]["snap_ids"]
            print(
                f"  #{hit['rank']} score={hit['score']} power={hit['power']} "
                f"members={members} snaps={snaps} (mean={hit['scores']['mean']})"
            )

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="gekisou.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="激奏模式评分与最优组队计算工具。",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # score
    p_score = subparsers.add_parser("score", help="计算指定编队的激奏真实得分")
    p_score.add_argument("-p", "--problem", required=True, help="激奏 problem JSON 路径")
    p_score.add_argument("-f", "--formation", required=True, help="队伍 formation JSON 路径")
    p_score.add_argument(
        "--assumed-placement",
        type=int,
        default=1,
        choices=(1, 2, 3, 4, 5),
        help="假设排名 (1..5，默认 1)",
    )
    p_score.add_argument(
        "--seeds",
        default=None,
        help="Luck 随机种子列表 (逗号分隔；如 '0' 或 '0,1,2'；未指定表示 nominal exact 期望)",
    )
    p_score.add_argument(
        "--accuracy-just", type=float, default=100.0, help="Just 判定比例 (默认 100)"
    )
    p_score.add_argument(
        "--accuracy-great", type=float, default=0.0, help="Great 判定比例 (默认 0)"
    )
    p_score.add_argument("--master-dir", default=None, help="可选 Master 表目录覆盖")
    p_score.add_argument("-o", "--output", default=None, help="输出 JSON 结果路径")
    p_score.add_argument("-q", "--quiet", action="store_true", help="静默模式")
    p_score.set_defaults(func=cmd_score)

    # rank
    p_rank = subparsers.add_parser("rank", help="搜索激奏最优编队")
    p_rank.add_argument("-p", "--problem", required=True, help="激奏 problem JSON 路径")
    p_rank.add_argument("-k", "--top-k", type=int, default=5, help="返回前 K 个方案 (默认 5)")
    p_rank.add_argument(
        "--criterion",
        choices=("mean", "max", "min"),
        default="mean",
        help="目标准则：mean (标称期望均值), max (最优技能顺序最高分), min (最劣顺序最低分)",
    )
    p_rank.add_argument(
        "--assumed-placement",
        type=int,
        default=1,
        choices=(1, 2, 3, 4, 5),
        help="假设排名 (1..5，默认 1)",
    )
    p_rank.add_argument(
        "--seeds",
        default=None,
        help="Luck 随机种子列表 (逗号分隔；如 '0'；未指定表示 nominal exact 期望)",
    )
    p_rank.add_argument(
        "--oracle",
        action="store_true",
        help="对小候选池启用独立完全穷举 Oracle (用于验证)",
    )
    p_rank.add_argument("--candidates-file", default=None, help="可选候选卡池 JSON 文件")
    p_rank.add_argument(
        "--time-limit-ms", type=int, default=0, help="搜索超时时间 (毫秒，0 表示不限)"
    )
    p_rank.add_argument("--required-members", default=None, help="必选成员卡牌 ID 列表 (逗号分隔)")
    p_rank.add_argument("--excluded-members", default=None, help="排除成员卡牌 ID 列表 (逗号分隔)")
    p_rank.add_argument(
        "--required-snaps", default=None, help="必选 Snapshot 卡牌 ID 列表 (逗号分隔)"
    )
    p_rank.add_argument(
        "--excluded-snaps", default=None, help="排除 Snapshot 卡牌 ID 列表 (逗号分隔)"
    )
    p_rank.add_argument("--leader", type=int, default=None, help="固定队长成员卡牌 ID")
    p_rank.add_argument(
        "--accuracy-just", type=float, default=100.0, help="Just 判定比例 (默认 100)"
    )
    p_rank.add_argument("--accuracy-great", type=float, default=0.0, help="Great 判定比例 (默认 0)")
    p_rank.add_argument("--master-dir", default=None, help="可选 Master 表目录覆盖")
    p_rank.add_argument("-o", "--output", default=None, help="输出 JSON 结果路径")
    p_rank.add_argument("-q", "--quiet", action="store_true", help="静默模式")
    p_rank.set_defaults(func=cmd_rank)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (RuntimeError, ValueError) as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
