#!/usr/bin/env python3
"""Production legality and conditional/mean-score search regression tests."""

import copy
import itertools
import json
import math
import subprocess
import tempfile
from pathlib import Path

from test_theoretical import f32, prepared, oracle, run as rank_score

ROOT = Path(__file__).resolve().parents[1]


def invoke(p, command="validate", formation=None, flags=(), experimental=False, warm=None):
    with tempfile.TemporaryDirectory() as folder:
        folder = Path(folder)
        source = folder / "problem.json"
        source.write_text(json.dumps(p))
        args = [str(ROOT / "build/deckcalc"), command, "-p", str(source), "--quiet"]
        if experimental:
            args.append("--experimental")
        if formation is not None:
            witness = folder / "formation.json"
            witness.write_text(json.dumps(formation))
            args += ["-f", str(witness)]
        if warm is not None:
            seeds = folder / "seeds.json"
            seeds.write_text(json.dumps(warm))
            args += ["--warm-start", str(seeds)]
        args += list(flags)
        result = subprocess.run(args, capture_output=True, text=True)
        return result, json.loads(result.stdout) if result.returncode == 0 else None


def game_problem():
    members = [
        dict(
            id=i,
            character=i,
            band=1 if i < 6 else 2,
            card_type=1,
            trained=[100 + i * 37, 0, 0],
            live_skill=i,
            leader_skill=1 if i == 1 else 0,
        )
        for i in range(1, 7)
    ]
    return {
        "schema": "ournotes-deck-problem@1",
        "song": {"id": 1, "type": 1},
        "chart": {
            "level": 27,
            "skill_times_ms": [6000 * i for i in range(5)],
            "notes": [
                {"t": k * 6000 + j * 100, "op": 1}
                for k, count in enumerate([1, 2, 3, 1, 4])
                for j in range(count)
            ],
        },
        "catalog": {
            "members": members,
            "snapshots": [dict(id=i, trained=[0, 0, 0]) for i in range(1, 6)],
            "live_skills": [
                dict(id=i, effects=[dict(level=5, effect_type=2000, value=i * 1723)])
                for i in range(1, 7)
            ],
            "leader_skills": [
                dict(
                    id=1, effects=[dict(level=5, effect_type=1000, value=10000, condition_group=1)]
                )
            ],
            "targets": [{"id": 1, "band": 1}, {"id": 2, "band": 2}, {"id": 3, "music_type": 1}],
            "conditions": [{"id": 1, "type": 3001, "targets": [1]}],
            "condition_groups": [{"group": 1, "rows": [[1]]}],
            "note_parameters": [{"op": 1, "score_percent": 100}],
        },
        "constraints": {"leader_pool": [1]},
    }


def formation(ids=(1, 2, 3, 4, 5)):
    return {
        "leader": 1,
        "slots": [dict(member=m, snapshot=i + 1, trigger=i + 1) for i, m in enumerate(ids)],
    }


def independent_mean(p, ids):
    cards = {m["id"]: m for m in p["catalog"]["members"]}
    power = sum(cards[m]["trained"][0] for m in ids)
    if all(cards[m]["band"] == 1 for m in ids):
        power *= 2
    values = []
    for order in itertools.permutations(ids):
        total = 0
        for n in p["chart"]["notes"]:
            trigger = n["t"] // 6000
            boost = f32(order[trigger] * 1723 / 10000)
            value = f32(3 * power)
            difficulty = f32(f32(f32(27 - 5) * f32(0.005)) + 1)
            value = f32(value * difficulty)
            value = f32(value * f32(1 + boost))
            value = f32(value / len(p["chart"]["notes"]))
            total += math.floor(value)
        values.append(total)
    return sum(values) / len(values)


def main():
    p = game_problem()
    result, doc = invoke(p)
    assert result.returncode == 0 and doc["input_mode"] == "game", result.stderr
    for mutation, needle in [
        (lambda q: q.update(settings={"team_size": 4}), "5 人"),
        (lambda q: q["constraints"].update(distinct_characters=False), "去重"),
        (lambda q: q["constraints"].update(distinct_snapshots=False), "去重"),
        (lambda q: q.update(settings={"score_model": {"rounding": "none"}}), "experimental"),
        (lambda q: q.update(settings={"leader_unmapped_effect_types": "ignore"}), "experimental"),
    ]:
        changed = copy.deepcopy(p)
        mutation(changed)
        result, _ = invoke(changed)
        assert result.returncode and needle in result.stderr, result.stderr
    f = formation()
    for field in ("member", "snapshot"):
        bad = copy.deepcopy(f)
        bad["slots"][1][field] = bad["slots"][0][field]
        result, _ = invoke(p, "score", bad)
        assert result.returncode and "重复" in result.stderr
    experiment = copy.deepcopy(p)
    experiment["input_mode"] = "experimental"
    experiment["constraints"]["distinct_characters"] = False
    bad = copy.deepcopy(f)
    bad["slots"][1]["member"] = 1
    result, _ = invoke(experiment, "score", bad, experimental=True)
    assert result.returncode and "重复成员卡" in result.stderr
    result, _ = invoke(experiment)
    assert result.returncode and "--experimental" in result.stderr
    result, doc = invoke(experiment, experimental=True)
    assert result.returncode == 0 and doc["input_mode"] == "experimental"
    result, _ = invoke(p, "rank", flags=("--objective", "index"))
    assert result.returncode and "实验代理" in result.stderr
    negative = copy.deepcopy(experiment)
    negative["catalog"]["members"][0]["trained"][0] = -100
    result, _ = invoke(negative, "rank", experimental=True)
    assert result.returncode and "安全上界" in result.stderr, result.stderr

    # Formation ALL, ANY, negation, song-property and recipient target checks.
    for kind, targets, positive, ids, multiplier in [
        (3001, [1], True, (1, 2, 3, 4, 5), 2),
        (3001, [1], True, (1, 2, 3, 4, 6), 1),
        (3000, [2], True, (1, 2, 3, 4, 5), 1),
        (3000, [2], True, (1, 2, 3, 4, 6), 2),
        (3000, [2], False, (1, 2, 3, 4, 5), 2),
        (4012, [3], True, (1, 2, 3, 4, 5), 2),
    ]:
        q = copy.deepcopy(p)
        q["catalog"]["conditions"][0].update(type=kind, targets=targets, positive=positive)
        result, doc = invoke(
            q, "score", formation(ids), flags=("--detail", "--order-search", "given")
        )
        assert result.returncode == 0, result.stderr
        actual = doc["results"][0]
        power = sum(q["catalog"]["members"][m - 1]["trained"][0] for m in ids)
        assert actual["power"] == power * multiplier, (kind, ids, actual["power"])
        assert sum(s["power"] for s in actual["assignments"]) == actual["power"]
    q = copy.deepcopy(p)
    q["catalog"]["conditions"][0].update(type=5000)
    result, doc = invoke(q, "score", formation((1, 2, 3, 4, 6)))
    assert result.returncode == 0, result.stderr
    assert doc["results"][0]["power"] == sum(
        m["trained"][0] * (2 if m["band"] == 1 else 1)
        for m in q["catalog"]["members"]
        if m["id"] != 5
    )
    q = copy.deepcopy(p)
    q["catalog"]["conditions"] += [{"id": 2, "type": 3000, "targets": [2]}]
    q["catalog"]["condition_groups"][0]["rows"] = [[1], [2]]
    result, doc = invoke(q, "score", f)
    assert result.returncode == 0 and doc["results"][0]["power"] == sum(
        m["trained"][0] for m in q["catalog"]["members"][:5]
    )
    q = copy.deepcopy(p)
    q["catalog"]["conditions"][0]["type"] = 99999
    result, _ = invoke(q, "score", f)
    assert result.returncode and "未支持" in result.stderr
    q = copy.deepcopy(p)
    q["catalog"]["leader_skills"][0]["effects"][0]["effect_type"] = 99999
    result, _ = invoke(q, "rank", flags=("--method", "exact", "--time-limit", "0"))
    assert result.returncode and "未在" in result.stderr

    expected = max(
        independent_mean(p, (1, *tail)) for tail in itertools.combinations(range(2, 7), 4)
    )
    result, doc = invoke(p, "rank", flags=("--method", "exact", "--time-limit", "0", "--detail"))
    assert result.returncode == 0, result.stderr
    row = doc["results"][0]
    assert row["ranking_objective"] == "mean_score"
    assert abs(row["ranking_score"] - expected) < 1e-8, (row["ranking_score"], expected)
    assert doc["audit"]["mean_score_certified"] and not doc["audit"]["theoretical_max_certified"]
    result, warmed = invoke(
        p, "rank", flags=("--method", "exact", "--time-limit", "0"), warm={"results": [row]}
    )
    assert result.returncode == 0 and warmed["results"][0]["ranking_score"] == row["ranking_score"]
    assert warmed["audit"]["accepted_seed_formations"] == 1
    result, maximum = invoke(
        p, "rank", flags=("--objective", "score", "--method", "exact", "--time-limit", "0")
    )
    assert result.returncode == 0 and maximum["audit"]["theoretical_max_certified"]
    for seed in range(16):
        small = prepared(seed, small=bool(seed % 2), reuse=bool(seed % 3 == 0))
        exact = rank_score(small, objective="mean", seconds=0, detail=True)
        assert exact["audit"]["mean_score_certified"]
        assert exact["audit"]["mean_score_bound_enabled"]
        assert abs(exact["results"][0]["ranking_score"] - oracle(small)["mean_score"]) < 1e-8
    print(
        "Game-rule checks passed: five-slot legality, experimental isolation, formation conditions and independent mean-score oracles"
    )


if __name__ == "__main__":
    main()
