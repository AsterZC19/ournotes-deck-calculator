#!/usr/bin/env python3
"""Independent float32/per-note exhaustive oracle for theoretical maximum score."""

import copy
import itertools
import json
import math
import random
import struct
import subprocess
import tempfile
from pathlib import Path
from test_search import fixture

ROOT = Path(__file__).resolve().parents[1]


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def prepared(seed, small=False, reuse=False):
    p = fixture(seed, reuse)
    p["settings"]["score_model"] = {"level_alpha": 0.005, "rounding": "float32_floor"}
    p["chart"]["level"] = 27
    if small:
        rng = random.Random(seed + 1000)
        for m in p["catalog"]["members"]:
            m["trained"][0] = rng.randrange(1, 20)
    p["catalog"]["note_parameters"].append({"op": 2, "score_percent": 10})
    for i, n in enumerate(p["chart"]["notes"]):
        n["op"] = 2 if i % 3 == 1 else 1
    p["catalog"]["combo_bonuses"] = [
        {"type": 0, "required_combo_count": 3, "factor": 0.01},
        {"type": 0, "required_combo_count": 6, "factor": 0.01},
    ]
    return p


def oracle(p, leader_id=40, excluded=()):
    c = p["catalog"]
    leader = next(m for m in c["members"] if m["id"] == leader_id)
    leader_rate = c["leader_skills"][leader["leader_skill"] - 1]["effects"][0]["value"]
    boost = {s["id"]: s["effects"][0]["value"] / 10000 for s in c["live_skills"]}
    percentages = {n["op"]: n["score_percent"] for n in c["note_parameters"]}
    notes = p["chart"]["notes"]
    converted = math.ceil(f32(sum(percentages[n["op"]] for n in notes) / 100))
    combos = []
    weights = []
    for i, n in enumerate(notes):
        native = 0.0
        combo = 0.0
        for row in c.get("combo_bonuses", []):
            if i >= row["required_combo_count"]:
                native = f32(native + f32(row["factor"]))
                combo += row["factor"]
        combos.append(native)
        weights.append(percentages[n["op"]] / 100 * (1 + combo))
    baseweight = math.fsum(weights)
    maxscore = -1
    maxindex = -1
    score_at_index = -1
    order_diff = False
    best = None
    for members in itertools.combinations(c["members"], p["settings"]["team_size"]):
        ids = {m["id"] for m in members}
        if tuple(sorted(ids)) in {tuple(sorted(x)) for x in excluded}:
            continue
        if leader_id not in ids or not set(p["constraints"]["required_members"]) <= ids:
            continue
        for snaps in itertools.product(c["snapshots"], repeat=p["settings"]["team_size"]):
            sid = [s["id"] for s in snaps]
            if (
                p["constraints"]["distinct_snapshots"]
                and len(set(sid)) < p["settings"]["team_size"]
            ):
                continue
            if not set(p["constraints"]["required_snapshots"]) <= set(sid):
                continue
            power = sum(
                m["trained"][0]
                + m["trained"][0] * s["trained"][0] // 10000
                + m["trained"][0] * leader_rate // 10000
                for m, s in zip(members, snaps)
            )
            formation_index = -1
            formation_score = -1
            score_for_best_index = -1
            for order in itertools.permutations(range(p["settings"]["team_size"])):
                windows = [
                    (
                        p["chart"]["skill_times_ms"][k],
                        p["chart"]["skill_times_ms"][k] + 500 + (s["id"] - 1) * 250,
                        boost[m["live_skill"]],
                        k,
                    )
                    for m, s, k in zip(members, snaps, order)
                ]
                gain = sum(
                    math.fsum(w for n, w in zip(notes, weights) if start <= n["t"] < end)
                    * b
                    / baseweight
                    for start, end, b, k in windows
                )
                index = power * (1 + gain)
                model = p["settings"]["score_model"]
                difficulty = f32(
                    f32(
                        f32(p["chart"]["level"] - model.get("level_base", 5))
                        * f32(model["level_alpha"])
                    )
                    + 1
                )
                base = f32(f32(f32(model.get("adjustment_factor", 3)) * f32(power)) * difficulty)
                score = 0
                for i, n in enumerate(notes):
                    live = 0.0
                    for start, end, b, k in sorted(windows, key=lambda x: x[3]):
                        if start <= n["t"] < end:
                            live = f32(live + f32(b))
                    value = f32(f32(percentages[n["op"]] / 100) * base)
                    value = f32(
                        value
                        * f32(
                            p["settings"].get("judgement", {}).get("factors", {}).get("perfect", 1)
                        )
                    )
                    value = f32(value * f32(1 + combos[i]))
                    value = f32(value * f32(1 + live))
                    value = math.floor(f32(value / f32(converted)))
                    life = p["settings"].get("life", {})
                    assist = p["settings"].get("assist", {})
                    value = f32(
                        value
                        * f32(life.get("onus_factor", 0.3) if life.get("initial", 1000) == 0 else 1)
                    )
                    value = f32(
                        value
                        * f32(
                            assist.get("score_percent", 90) / 100
                            if assist.get("enabled", False)
                            else 1
                        )
                    )
                    score += math.floor(value)
                if index > formation_index:
                    formation_index = index
                    score_for_best_index = score
                formation_score = max(formation_score, score)
            order_diff |= formation_score > score_for_best_index
            if formation_index > maxindex:
                maxindex = formation_index
                score_at_index = formation_score
            elif abs(formation_index - maxindex) < 1e-9:
                score_at_index = max(score_at_index, formation_score)
            if formation_score > maxscore:
                maxscore = formation_score
                best = ([m["id"] for m in members], sid)
    return {
        "score": maxscore,
        "index": maxindex,
        "score_at_index": score_at_index,
        "best": best,
        "order_diff": order_diff,
    }


def run(p, objective=None, detail=False, seconds=10, leaders="40", excluded=()):
    with tempfile.TemporaryDirectory() as folder:
        f = Path(folder) / "p.json"
        f.write_text(json.dumps(p))
        args = [
            str(ROOT / "build/deckcalc"),
            "rank",
            "-p",
            str(f),
            "--method",
            "exact",
            "--leaders",
            leaders,
            "--time-limit",
            str(seconds),
            "--top",
            "1",
            "--quiet",
        ]
        if excluded:
            e = Path(folder) / "excluded.json"
            e.write_text(json.dumps(excluded))
            args += ["--exclude-member-sets", str(e)]
        if objective:
            args += ["--objective", objective]
        if detail:
            args += ["--detail"]
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        return json.loads(result.stdout)


def main():
    for seed in range(8):
        for reuse in (False, True):
            p = prepared(seed, reuse=reuse)
            e = oracle(p)
            d = run(p, detail=seed == 0)
            a = d["results"][0]
            assert d["audit"]["theoretical_max_certified"], d["audit"]
            assert a["ranking_objective"] == "theoretical_score"
            assert (
                a["ranking_score"]
                == a["estimated_score"]["total"]
                == a["order_analysis"]["best_score"]
                == e["score"]
            ), (seed, reuse, a["ranking_score"], e)
            assert (
                a["order_analysis"]["score_permutations"] == 6
                and a["order_analysis"]["order_search_complete"]
            )

    for seed in range(4):
        p = prepared(seed)
        p["settings"]["life"] = {"mode": "constant", "initial": 0, "onus_factor": 0.3}
        p["settings"]["assist"] = {"enabled": True, "score_percent": 90}
        p["settings"]["score_model"]["level_alpha"] = 0.007
        assert run(p)["results"][0]["ranking_score"] == oracle(p)["score"]

    for seed in range(3):
        p = prepared(seed + 20)
        expected = max(oracle(p, leader_id=m["id"])["score"] for m in p["catalog"]["members"])
        d = run(p, leaders="10,20,30,40", seconds=0)
        assert d["audit"]["leaders_proven"] == 4 and d["audit"]["theoretical_max_certified"]
        assert d["results"][0]["ranking_score"] == expected

    for seed in (2, 7):
        p = prepared(seed)
        excluded = []
        previous = math.inf
        for rank in range(3):
            expected = max(oracle(p, m["id"], excluded)["score"] for m in p["catalog"]["members"])
            d = run(p, leaders="10,20,30,40", seconds=0, excluded=excluded)
            a = d["results"][0]
            assert d["audit"]["theoretical_max_certified"] and d["audit"]["leaders_proven"] == 4
            assert a["ranking_score"] == expected and a["ranking_score"] <= previous
            assert sorted(a["members"]) not in excluded
            previous = a["ranking_score"]
            excluded.append(sorted(a["members"]))

    p = prepared(17)
    p["settings"]["team_size"] = 5
    p["chart"]["skill_times_ms"] = [0, 500, 1000, 1500, 2000]
    c = p["catalog"]
    c["members"].append(
        {
            "id": 50,
            "character": 5,
            "band": 1,
            "card_type": 1,
            "trained": [777, 0, 0],
            "live_skill": 5,
            "leader_skill": 5,
        }
    )
    c["snapshots"].append(
        {
            "id": 5,
            "characters": [1],
            "bands": [1],
            "card_type": 1,
            "trained": [1900, 0, 0],
            "support_skills": [5],
        }
    )
    c["live_skills"].append(
        {"id": 5, "effects": [{"level": 5, "effect_type": 2000, "value": 13500}]}
    )
    c["leader_skills"].append(
        {"id": 5, "effects": [{"level": 5, "effect_type": 1000, "value": 3200}]}
    )
    c["support_skills"].append(
        {"id": 5, "effects": [{"level": 5, "effect_type": 15000, "value": 1000}]}
    )
    d = run(p, seconds=0)
    assert d["audit"]["theoretical_max_certified"] and d["audit"]["linear_score_bound_enabled"]
    assert d["results"][0]["ranking_score"] == oracle(p)["score"]
    assert d["results"][0]["order_analysis"]["score_permutations"] == 120
    p = json.loads((ROOT / "tests/fixtures/theoretical-rounding-inversion.json").read_text())
    e = oracle(p)
    a = run(p)["results"][0]
    b = run(p, "index")["results"][0]
    assert e["score"] > e["score_at_index"]
    assert a["ranking_score"] == e["score"]
    assert a["index"] < b["index"]
    assert e["order_diff"]

    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "p.json"
        path.write_text(json.dumps(p))
        formation = {
            "schema": "ournotes-deck-formation@1",
            "leader": a["leader"],
            "slots": [
                {k: s[k] for k in ("member", "snapshot", "trigger")} for s in a["assignments"]
            ],
        }
        f = Path(folder) / "f.json"
        f.write_text(json.dumps(formation))
        fixed = subprocess.run(
            [
                str(ROOT / "build/deckcalc"),
                "score",
                "-p",
                str(path),
                "-f",
                str(f),
                "--order-search",
                "given",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        assert (
            json.loads(fixed.stdout)["results"][0]["estimated_score"]["total"] == a["ranking_score"]
        )
        theoretical = subprocess.run(
            [
                str(ROOT / "build/deckcalc"),
                "score",
                "-p",
                str(path),
                "-f",
                str(f),
                "--objective",
                "score",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        assert json.loads(theoretical.stdout)["results"][0]["ranking_score"] == a["ranking_score"]

    tiny = run(prepared(0), seconds=0.00001)
    assert tiny["audit"]["theoretical_max_certified"] is False
    assert tiny["audit"]["certified"] is False
    print(
        "Theoretical score exhaustive oracle passed: 16 native float32 cases and index/score inversion"
    )


if __name__ == "__main__":
    main()
