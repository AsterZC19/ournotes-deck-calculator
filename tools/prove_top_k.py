#!/usr/bin/env python3
"""Prove distinct member-set ranks by successive unlimited exact searches."""

import argparse
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, data):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--problem", type=Path, required=True)
    parser.add_argument("-o", "--output-dir", type=Path, required=True)
    parser.add_argument("--experimental", action="store_true")
    parser.add_argument("--count", type=int, default=10)
    parser.add_argument(
        "--warm-start", type=Path, help="Prior rank/topk results, always revalidated and rescored"
    )
    parser.add_argument("--solver", type=Path, default=ROOT / "build/deckcalc")
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be positive")
    source = args.problem.resolve()
    solver = args.solver.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    problem = json.loads(source.read_text())
    fingerprint = digest(source)
    binary = digest(solver)
    snapshot = out / "problem.json"
    if snapshot.exists() and digest(snapshot) != fingerprint:
        raise RuntimeError("Output directory belongs to another problem")
    if not snapshot.exists():
        shutil.copyfile(source, snapshot)
    manifest = {
        "schema": "ournotes-topk-proof@1",
        "problem_sha256": fingerprint,
        "solver_sha256": binary,
        "ranking_objective": "theoretical_score",
        "distinct_member_sets": True,
        "requested_count": args.count,
        "time_limit_s": 0,
        "no_time_deadline": True,
        "results": [],
        "proofs": [],
        "complete": False,
    }
    members = {m["id"]: m for m in problem["catalog"]["members"]}
    snaps = {s["id"]: s for s in problem["catalog"]["snapshots"]}
    excluded = []
    started = time.monotonic()
    candidate_bank = []
    if args.warm_start:
        supplied = json.loads(args.warm_start.read_text())
        candidate_bank = supplied if isinstance(supplied, list) else supplied["results"]
    for rank in range(1, args.count + 1):
        if digest(solver) != binary:
            raise RuntimeError(
                "Solver changed during proof chain; stop and use a new output directory"
            )
        exclusions = out / f"excluded-before-{rank:02d}.json"
        save(exclusions, excluded)
        proof = out / f"proof-{rank:02d}.json"
        log = out / f"proof-{rank:02d}.log"
        if proof.exists():
            prior = out / f"proof-{rank:02d}-binary.sha256"
            if not prior.exists() or prior.read_text().strip() != binary:
                raise RuntimeError("Solver changed; use a new output directory")
        else:
            print(f"Proving rank {rank}/{args.count} without time deadline...", flush=True)
            command = [
                str(solver),
                "rank",
                "-p",
                str(snapshot),
                "--objective",
                "score",
                "--method",
                "exact",
                "--time-limit",
                "0",
                "--beam-width",
                "128",
                "--restarts",
                "8",
                "--anneal-steps",
                "10000",
                "--top",
                str(max(20, args.count)),
                "--detail",
                "--exclude-member-sets",
                str(exclusions),
                "-o",
                str(proof),
            ]
            if args.experimental:
                command += ["--experimental"]
            if candidate_bank:
                seeds = out / f"warm-before-{rank:02d}.json"
                save(seeds, {"results": candidate_bank})
                command += ["--warm-start", str(seeds)]
            with log.open("w") as stream:
                subprocess.run(command, stderr=stream, check=True)
            (out / f"proof-{rank:02d}-binary.sha256").write_text(binary + "\n")
        document = json.loads(proof.read_text())
        audit = document["audit"]
        candidate_bank = document["results"]
        if audit.get("infeasible_proven") and not candidate_bank:
            raise RuntimeError(f"Only {rank-1} legal member sets exist")
        if (
            not audit["theoretical_max_certified"]
            or audit["leaders_proven"] != audit["leaders_requested"]
        ):
            raise RuntimeError("Incomplete certificate")
        if audit["excluded_member_sets"] != excluded:
            raise RuntimeError("Exclusion chain mismatch")
        if not document["results"]:
            raise RuntimeError(f"Only {rank-1} legal member sets exist")
        result = document["results"][0]
        key = sorted(result["members"])
        if key in excluded:
            raise RuntimeError("Repeated member set")
        if (
            manifest["results"]
            and result["ranking_score"] > manifest["results"][-1]["ranking_score"]
        ):
            raise RuntimeError("Rank scores must be nonincreasing")
        slots = sorted(result["assignments"], key=lambda x: x["trigger"])
        formation = {
            "schema": "ournotes-deck-formation@1",
            "leader": result["leader"],
            "slots": [{k: s[k] for k in ("member", "snapshot", "trigger")} for s in slots],
        }
        fp = out / f"formation-{rank:02d}.json"
        save(fp, formation)
        witness = json.loads(
            subprocess.check_output(
                [
                    str(solver),
                    "score",
                    "-p",
                    str(snapshot),
                    "-f",
                    str(fp),
                    "--order-search",
                    "given",
                    *(["--experimental"] if args.experimental else []),
                ],
                text=True,
            )
        )["results"][0]["estimated_score"]["total"]
        if witness != result["ranking_score"]:
            raise RuntimeError("Witness score mismatch")
        result["rank"] = rank
        manifest["results"].append(result)
        manifest["proofs"].append(
            {
                "rank": rank,
                "file": proof.name,
                "sha256": digest(proof),
                "audit": audit,
                "witness_replayed": True,
            }
        )
        excluded.append(key)
        manifest["complete"] = rank == args.count
        manifest["elapsed_runner_s"] = time.monotonic() - started
        save(out / "topk-certified.json", manifest)
        print(
            f'Rank {rank} proven: {int(result["ranking_score"]):,}; members {key}; nodes {audit["dfs_nodes"]:,}; seconds {audit["elapsed_s"]:.2f}',
            flush=True,
        )
    lines = [
        f'# {problem["song"].get("title",problem["song"]["id"])}：理论最高分前 {args.count} 名',
        "",
        f'{problem["chart"]["difficulty"]} Lv.{problem["chart"]["level"]}；成员 {len(members)} 张、Snapshot {len(snaps)} 张。输入见 problem.json。',
        "",
        "按成员组合区分名次，在输入范围内逐名证明。AP、恒定生命；技能顺序随机。",
        "",
        "| 全局名次 | 理论最高分 | 队长 | 配对与技能顺序（从左到右；★为队长） |",
        "|---:|---:|---|---|",
    ]
    usedm = set()
    useds = set()
    for result in manifest["results"]:
        pairs = []
        for s in sorted(result["assignments"], key=lambda x: x["trigger"]):
            usedm.add(s["member"])
            useds.add(s["snapshot"])
            name = members[s["member"]].get("name", str(s["member"])).split("/")[-1].strip()
            pairs.append(
                f'{name}[M{s["member"]}]'
                + ("★" if s["member"] == result["leader"] else "")
                + f'/S{s["snapshot"]}'
            )
        lines.append(
            f'| {result["rank"]} | {int(result["ranking_score"]):,} | M{result["leader"]} | '
            + " → ".join(pairs)
            + " |"
        )
    lines += ["", "## 成员卡", "", "| ID | 角色 | 卡名 |", "|---|---|---|"]
    for mid in sorted(usedm):
        m = members[mid]
        lines.append(f'| M{mid} | {m.get("name", "")} | {m.get("title", "")} |')
    lines += ["", "## Snapshot", "", "| ID | 名称 | 卡名 |", "|---|---|---|"]
    for sid in sorted(useds):
        s = snaps[sid]
        lines.append(f'| S{sid} | {s.get("name", "")} | {s.get("title", "")} |')
    lines += [
        "",
        "## 证明记录",
        "",
        "| 名次 | 已证明队长 | DFS 节点 | 搜索秒数 | 完成标志 |",
        "|---:|---:|---:|---:|---|",
    ]
    for proof in manifest["proofs"]:
        a = proof["audit"]
        lines.append(
            f'| {proof["rank"]} | {a["leaders_proven"]}/{a["leaders_requested"]} | {a["dfs_nodes"]:,} | {a["elapsed_s"]:.2f} | theoretical_max_certified=true |'
        )
    lines += [
        "",
        f"输入 SHA-256：`{fingerprint}`",
        f"计算器 SHA-256：`{binary}`",
        "",
        "各名次 proof-XX.json 保存排除组合、完整最优编成、分数及证书；proof-XX.log 保存队长/根分支完成日志。formation-XX.json 可按 score --order-search given 重放，所有名次已验证重放分数一致。",
        "剪枝使用原生 float32 单调上界、含保守浮点误差的线性包络、成员/角色与 Snapshot 两种独立放松的触发位分配上界；理论分证明禁用指数占优删除。",
    ]
    (out / "top10-certified.md").write_text("\n".join(lines) + "\n")
    print("Completed report:", out / "top10-certified.md", flush=True)


if __name__ == "__main__":
    main()
