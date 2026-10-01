#!/usr/bin/env python3
"""核对参考榜单首位配队的数值。

    python3 tools/check_reference.py \
        --reference-dir /path/to/our-notes-event-20260930 \
        --binary build/deckcalc

参考目录里需要 inputs.json 与 results-<song>-<difficulty>.json。缺少就跳过并返回 0。
"""

from __future__ import annotations

import argparse
import bisect
import math
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compile_from_inputs import build_problem_document  # noqa: E402

TOLERANCE = 1e-9
CHECKS = ("power", "weight_factor", "index")


def compare(name: str, reference: float, actual: float, failures: list[str]) -> None:
    if name == "power":
        ok = int(reference) == int(actual)
        detail = f"{int(reference)} vs {int(actual)}"
    else:
        scale = max(1.0, abs(reference))
        ok = abs(reference - actual) / scale <= TOLERANCE
        detail = f"{reference!r} vs {actual!r} (rel {abs(reference - actual) / scale:.3e})"
    print(f"  {'PASS' if ok else 'FAIL'} {name}: {detail}")
    if not ok:
        failures.append(f"{name}: {detail}")


def check_chart(reference_dir: Path, binary: Path, song_id: int, difficulty: str) -> list[str]:
    failures: list[str] = []
    print(f"== song {song_id} {difficulty} ==")
    inputs = json.loads((reference_dir / "inputs.json").read_text(encoding="utf-8"))
    reference = json.loads(
        (reference_dir / f"results-{song_id}-{difficulty}.json").read_text(encoding="utf-8")
    )
    top = reference["results"][0]

    document = build_problem_document(inputs, song_id, difficulty, base_dir=reference_dir)

    legacy_index = top["index"]
    notes = sorted(
        (n for n in document["chart"]["notes"] if n.get("scoring", True)), key=lambda n: n["t"]
    )
    times = [n["t"] for n in notes]
    percentages = {n["op"]: n["score_percent"] for n in document["catalog"]["note_parameters"]}
    combo = [
        b
        for b in document["catalog"]["combo_bonuses"]
        if b["type"] == document.get("settings", {}).get("combo_type", 0)
    ]
    weights = [
        percentages[n["op"]]
        / 100
        * (
            1
            + math.fsum(
                b["factor"]
                for b in combo
                if bisect.bisect_left(times, n["t"]) >= b["required_combo_count"]
            )
        )
        for n in notes
    ]
    base = math.fsum(weights)
    member_skills = {m["id"]: m["live_skill"] for m in document["catalog"]["members"]}
    skills = {s["id"]: s for s in document["catalog"]["live_skills"]}
    for slot in top["assignments"]:
        live_boost = (
            max(
                e["value"]
                for e in skills[member_skills[slot["member"]]]["effects"]
                if e["level"] == 5
            )
            / 10000
        )
        start = document["chart"]["skill_times_ms"][slot["trigger"] - 1]
        slot["weighted_skill_gain"] = (
            math.fsum(
                w for n, w in zip(notes, weights) if start <= n["t"] < start + slot["duration_ms"]
            )
            * live_boost
            / base
        )
    top["weight_factor"] = 1 + math.fsum(a["weighted_skill_gain"] for a in top["assignments"])
    top["index"] = top["power"] * top["weight_factor"]
    print(
        f"  archived i+1 combo index={legacy_index}; strictly-before/multiplicative expected={top['index']}"
    )
    formation = {
        "schema": "ournotes-deck-formation@1",
        "leader": top["leader"],
        "slots": [
            {"member": a["member"], "snapshot": a["snap"], "trigger": a["trigger"]}
            for a in top["assignments"]
        ],
    }

    with tempfile.TemporaryDirectory() as tmp:
        problem_path = Path(tmp) / "problem.json"
        formation_path = Path(tmp) / "formation.json"
        problem_path.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
        formation_path.write_text(json.dumps(formation, ensure_ascii=False), encoding="utf-8")
        completed = subprocess.run(
            [
                str(binary),
                "score",
                "-p",
                str(problem_path),
                "-f",
                str(formation_path),
                "--order-search",
                "given",
            ],
            capture_output=True,
            text=True,
        )
    if completed.returncode != 0:
        print(f"  FAIL deckcalc 退出码 {completed.returncode}: {completed.stderr.strip()}")
        return [f"song {song_id} {difficulty}: deckcalc 退出码 {completed.returncode}"]

    result = json.loads(completed.stdout)["results"][0]
    for key in CHECKS:
        compare(key, float(top[key]), float(result[key]), failures)

    reference_slots = {(a["member"], a["snap"]): a for a in top["assignments"]}
    for slot in result["assignments"]:
        expected = reference_slots[(slot["member"], slot["snapshot"])]
        if int(slot["duration_ms"]) != int(expected["duration_ms"]):
            failures.append(
                f"duration_ms {slot}: {slot['duration_ms']} vs {expected['duration_ms']}"
            )
        if int(slot["power"]) != int(expected["power"]):
            failures.append(f"slot power {slot}: {slot['power']} vs {expected['power']}")
        gain_scale = max(1.0, abs(expected["weighted_skill_gain"]))
        if (
            abs(slot["weighted_skill_gain"] - expected["weighted_skill_gain"]) / gain_scale
            > TOLERANCE
        ):
            failures.append(
                f"gain {slot}: {slot['weighted_skill_gain']} vs {expected['weighted_skill_gain']}"
            )
    print(f"  {'PASS' if not failures else 'FAIL'} 逐格 duration/power/gain")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-dir", required=True)
    parser.add_argument("--binary", default="build/deckcalc")
    parser.add_argument("--charts", default="100109:expert,100109:hard,100056:expert,100063:expert")
    args = parser.parse_args()

    reference_dir = Path(args.reference_dir)
    binary = Path(args.binary).resolve()
    if not (reference_dir / "inputs.json").exists():
        print(f"跳过：{reference_dir} 里没有 inputs.json")
        return 0
    if not binary.exists():
        print(f"跳过：找不到 {binary}，先跑 make")
        return 0

    failures: list[str] = []
    checked = 0
    for item in args.charts.split(","):
        song_text, _, difficulty = item.partition(":")
        if not (reference_dir / f"results-{song_text}-{difficulty}.json").exists():
            print(f"跳过 song {song_text} {difficulty}：没有对应榜单")
            continue
        checked += 1
        failures.extend(check_chart(reference_dir, binary, int(song_text), difficulty))

    print()
    if failures:
        print(f"回归失败 {len(failures)} 项:")
        for line in failures:
            print(f"  - {line}")
        return 1
    if checked == 0:
        print("没有执行任何参考谱面核对，不能确认回归通过")
        return 2
    print(f"回归通过（{checked} 首谱面）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
