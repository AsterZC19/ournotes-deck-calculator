#!/usr/bin/env python3
"""Prove distinct member-set ranks using native Top-K exact search."""

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
        "--objective",
        choices=["score", "mean"],
        default="score",
        help="Ranking objective: score (theoretical_score) or mean (mean_score)",
    )
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

    native_proof = out / "proof-topk.json"
    if not native_proof.exists() and (out / "topk-certified.json").exists():
        raise RuntimeError("Legacy proof chain; use a new output directory")
    binary_hash_file = out / "proof-topk-binary.sha256"
    proof_hash_file = out / "proof-topk.sha256"
    started = time.monotonic()

    if native_proof.exists():
        if not binary_hash_file.exists() or binary_hash_file.read_text().strip() != binary:
            raise RuntimeError("Solver changed; use a new output directory")
        if not proof_hash_file.exists() or proof_hash_file.read_text().strip() != digest(
            native_proof
        ):
            raise RuntimeError("Search proof changed; use a new output directory")
        search_doc = json.loads(native_proof.read_text())
    else:
        print(f"Proving top {args.count} with native exact search...", flush=True)
        command = [
            str(solver),
            "rank",
            "-p",
            str(snapshot),
            "--objective",
            args.objective,
            "--prove-top",
            str(args.count),
            "--time-limit",
            "0",
            "--detail",
            "-o",
            str(native_proof),
        ]
        if args.experimental:
            command += ["--experimental"]
        if args.warm_start:
            command += ["--warm-start", str(args.warm_start.resolve())]
        log = out / "proof-topk.log"
        with log.open("w") as stream:
            subprocess.run(command, stderr=stream, check=True)
        if digest(solver) != binary:
            raise RuntimeError("Solver changed during search; use a new output directory")
        binary_hash_file.write_text(binary + "\n")
        proof_hash_file.write_text(digest(native_proof) + "\n")
        search_doc = json.loads(native_proof.read_text())

    audit = search_doc["audit"]
    results = search_doc["results"]
    objective_key = "mean_score" if args.objective == "mean" else "theoretical_score"
    if audit.get("requested_count") != args.count or audit.get("objective") != objective_key:
        raise RuntimeError("Search configuration changed; use a new output directory")
    if audit.get("infeasible_proven") and not results:
        raise RuntimeError("Only 0 legal member sets exist")
    if not audit.get("top_k_certified") and not (
        args.count == 1 and audit.get("theoretical_max_certified")
    ):
        raise RuntimeError("Incomplete certificate")
    if not results:
        raise RuntimeError("Only 0 legal member sets exist")

    manifest = {
        "schema": "ournotes-topk-proof@2",
        "problem_sha256": fingerprint,
        "solver_sha256": binary,
        "ranking_objective": objective_key,
        "distinct_member_sets": True,
        "requested_count": args.count,
        "returned_count": len(results),
        "time_limit_s": 0,
        "no_time_deadline": True,
        "search_proof": {
            "file": native_proof.name,
            "sha256": digest(native_proof),
            "audit": audit,
        },
        "results": [],
        "proofs": [],
        "complete": False,
    }

    seen_sets = set()
    for rank, result in enumerate(results, 1):
        if digest(solver) != binary:
            raise RuntimeError("Solver changed during witness checks; use a new output directory")
        key = tuple(sorted(result["members"]))
        if key in seen_sets:
            raise RuntimeError("Repeated member set in top-k results")
        seen_sets.add(key)
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

        witness_cmd = [
            str(solver),
            "score",
            "-p",
            str(snapshot),
            "-f",
            str(fp),
            "--order-search",
            "given" if args.objective == "score" else "exact",
        ]
        if args.experimental:
            witness_cmd += ["--experimental"]
        score_doc = json.loads(subprocess.check_output(witness_cmd, text=True))
        if args.objective == "mean":
            witness = score_doc["results"][0]["order_analysis"]["mean_score"]
        else:
            witness = score_doc["results"][0]["estimated_score"]["total"]
        if witness != result["ranking_score"]:
            raise RuntimeError("Witness score mismatch")

        result["rank"] = rank
        manifest["results"].append(result)
        manifest["proofs"].append(
            {
                "rank": rank,
                "search_proof_file": native_proof.name,
                "search_proof_sha256": digest(native_proof),
                "formation_file": fp.name,
                "audit": audit,
                "witness_replayed": True,
                "witness_score": witness,
            }
        )

    manifest["complete"] = (len(results) == args.count) or audit.get("top_k_exhausted", False)
    manifest["elapsed_runner_s"] = time.monotonic() - started
    save(out / "topk-certified.json", manifest)

    members = {m["id"]: m for m in problem["catalog"]["members"]}
    snaps = {s["id"]: s for s in problem["catalog"]["snapshots"]}
    obj_label = "理论平均分" if args.objective == "mean" else "理论最高分"
    lines = [
        f'# {problem["song"].get("title", problem["song"]["id"])}：{obj_label}前 {len(results)} 名',
        "",
        f'{problem["chart"]["difficulty"]} Lv.{problem["chart"]["level"]}；成员 {len(members)} 张、Snapshot {len(snaps)} 张。输入见 problem.json。',
        "",
        "按成员组合区分名次，原生精确证明。AP、恒定生命；技能顺序随机。",
        "",
        f"| 全局名次 | {obj_label} | 队长 | 配对与技能顺序（从左到右；★为队长） |",
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
    lines.append(
        f'| 1..{len(results)} | {audit["leaders_proven"]}/{audit["leaders_requested"]} | '
        f'{audit.get("dfs_nodes", 0):,} | {audit.get("elapsed_s", 0):.2f} | top_k_certified=true |'
    )
    lines += [
        "",
        f"输入 SHA-256：`{fingerprint}`",
        f"计算器 SHA-256：`{binary}`",
        "",
        f"原生搜索证明保存于 {native_proof.name}；formation-XX.json 可重放验证。",
    ]
    (out / "top10-certified.md").write_text("\n".join(lines) + "\n")
    print("Completed report:", out / "top10-certified.md", flush=True)


if __name__ == "__main__":
    main()
