#!/usr/bin/env python3
"""Compare solver against independent exhaustive assignment/permutation enumeration."""

import itertools
import json
import random
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def fixture(seed, reuse=False):
    rng = random.Random(seed)
    members = [
        {
            "id": 10 * (i + 1),
            "character": i + 1,
            "band": 1,
            "card_type": 1,
            "trained": [rng.randrange(100, 1000), 0, 0],
            "live_skill": i + 1,
            "leader_skill": i + 1,
        }
        for i in range(4)
    ]
    snaps = [
        {
            "id": i + 1,
            "characters": [1],
            "bands": [1],
            "card_type": 1,
            "trained": [rng.randrange(0, 3000), 0, 0],
            "support_skills": [i + 1],
        }
        for i in range(4)
    ]
    return {
        "schema": "ournotes-deck-problem@1",
        "input_mode": "experimental",
        "song": {"id": 1, "type": 1},
        "chart": {
            "difficulty": "expert",
            "level": 5,
            "notes": [{"t": i * 250, "op": 1} for i in range(12)],
            "skill_times_ms": [0, 1000, 2000],
        },
        "catalog": {
            "members": members,
            "snapshots": snaps,
            "note_parameters": [{"op": 1, "score_percent": 100}],
            "live_skills": [
                {
                    "id": i + 1,
                    "effects": [
                        {"level": 5, "effect_type": 2000, "value": rng.randrange(1000, 15000)}
                    ],
                }
                for i in range(4)
            ],
            "leader_skills": [
                {
                    "id": i + 1,
                    "effects": [{"level": 5, "effect_type": 1000, "value": rng.randrange(0, 5000)}],
                }
                for i in range(4)
            ],
            "support_skills": [
                {"id": i + 1, "effects": [{"level": 5, "effect_type": 15000, "value": i * 250}]}
                for i in range(4)
            ],
        },
        "settings": {
            "team_size": 3,
            "base_duration_ms": 500,
            "score_model": {"level_alpha": None},
            "power_model": {
                "rounding": "float64_floor",
                "sources": {
                    "type_link": False,
                    "band_item": False,
                    "music_type": False,
                    "music_tag": False,
                    "vip": False,
                },
            },
        },
        "constraints": {
            "distinct_snapshots": not reuse,
            "required_members": [10],
            "required_snapshots": [2],
        },
        "search": {"seed": seed},
    }


def expected(p, leader_id):
    c = p["catalog"]
    leader = next(m for m in c["members"] if m["id"] == leader_id)
    live = {s["id"]: s["effects"][0]["value"] / 10000 for s in c["live_skills"]}
    leader_rate = c["leader_skills"][leader["leader_skill"] - 1]["effects"][0]["value"]
    best = -1
    for members in itertools.combinations(c["members"], 3):
        ids = {m["id"] for m in members}
        if leader_id not in ids or not set(p["constraints"]["required_members"]) <= ids:
            continue
        for snaps in itertools.product(c["snapshots"], repeat=3):
            sid = [s["id"] for s in snaps]
            if p["constraints"]["distinct_snapshots"] and len(set(sid)) < 3:
                continue
            if not set(p["constraints"]["required_snapshots"]) <= set(sid):
                continue
            power = sum(
                m["trained"][0]
                + m["trained"][0] * s["trained"][0] // 10000
                + m["trained"][0] * leader_rate // 10000
                for m, s in zip(members, snaps)
            )
            for order in itertools.permutations(range(3)):
                gain = 0
                for m, s, k in zip(members, snaps, order):
                    start = p["chart"]["skill_times_ms"][k]
                    end = start + 500 + (s["id"] - 1) * 250
                    gain += (
                        sum(start <= n["t"] < end for n in p["chart"]["notes"])
                        / 12
                        * live[m["live_skill"]]
                    )
                best = max(best, power * (1 + gain))
    return best


def run(p, method="exact", leaders="40", seconds=10, beam=64, restarts=4):
    with tempfile.TemporaryDirectory() as folder:
        file = Path(folder) / "p.json"
        file.write_text(json.dumps(p))
        result = subprocess.run(
            [
                str(ROOT / "build/deckcalc"),
                "rank",
                "--experimental",
                "-p",
                str(file),
                "--method",
                method,
                "--objective",
                "index",
                "--leaders",
                leaders,
                "--time-limit",
                str(seconds),
                "--top",
                "1",
                "--quiet",
                "--beam-width",
                str(beam),
                "--restarts",
                str(restarts),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError(result.stderr)
        return json.loads(result.stdout)


def main():
    for seed in range(8):
        for reuse in (False, True):
            p = fixture(seed, reuse)
            r = run(p)
            e = expected(p, 40)
            assert r["audit"]["certified"], r["audit"]
            actual = r["results"][0]
            assert abs(actual["index"] - e) < 1e-8, (seed, reuse, actual["index"], e)
            assert 10 in actual["members"] and 2 in {s["snapshot"] for s in actual["assignments"]}
            assert r["audit"]["dfs_nodes"] > 0

    p = fixture(123)
    a = run(p, "fast")
    b = run(p, "fast")
    assert a["results"][0]["index"] == b["results"][0]["index"]
    assert a["audit"]["anneal_proposals"] > 0 and a["audit"]["anneal_downhill_accepted"] > 0
    assert a["results"][0]["index"] <= expected(p, 40) + 1e-8

    trap = fixture(0)
    trap["constraints"]["required_members"] = []
    trap["constraints"]["required_snapshots"] = []
    plain = run(trap, "fast", beam=1, restarts=0)
    strong = run(trap, "fast", beam=1)
    assert strong["results"][0]["index"] >= plain["results"][0]["index"]
    assert plain["audit"]["lns_improvements"] > 0
    assert abs(plain["results"][0]["index"] - expected(trap, 40)) < 1e-8
    assert abs(strong["results"][0]["index"] - expected(trap, 40)) < 1e-8

    print(
        "Search exhaustive oracle checks passed: 16 exact cases, required cards, snapshot reuse, seeded annealing"
    )


if __name__ == "__main__":
    main()
