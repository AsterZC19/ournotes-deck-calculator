#!/usr/bin/env python3
"""Synthetic integration checks for score ordering, reward tradeoffs and CP budgets."""

import copy
import importlib.util
import itertools
import json
import math
import random
from pathlib import Path
import subprocess
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/deckcalc"


def run(command, problem, formation=None, challenge=None, **flags):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "problem.json"
        path.write_text(json.dumps(problem))
        args = [str(BINARY), command, "-p", str(path), "--quiet"]
        if command == "rank" and "objective" not in flags:
            args += ["--objective", "index"]
        if challenge is not None:
            other = Path(folder) / "challenge.json"
            other.write_text(json.dumps(challenge))
            args += ["--challenge-problem", str(other)]
        if formation:
            f = Path(folder) / "formation.json"
            f.write_text(json.dumps(formation))
            args += ["-f", str(f)]
        for key, value in flags.items():
            args += ["--" + key.replace("_", "-")]
            if value is not True:
                args += [str(value)]
        completed = subprocess.run(args, text=True, capture_output=True)
        if completed.returncode:
            raise subprocess.CalledProcessError(
                completed.returncode, args, output=completed.stdout, stderr=completed.stderr
            )
        return json.loads(completed.stdout)


def problem():
    members = [
        {
            "id": i,
            "character": i,
            "band": 1,
            "card_type": 1,
            "trained": [100 * i] * 3,
            "live_skill": i,
            "event_pt_bonus_bp": {1: 10000, 2: 0, 3: 5000}[i],
        }
        for i in range(1, 4)
    ]
    snapshots = [
        {
            "id": i,
            "characters": [i],
            "bands": [1],
            "card_type": 1,
            "trained": [1000] * 3,
            "event_drop_bonus_bp": {1: 0, 2: 10000, 3: 2000}[i],
        }
        for i in range(1, 4)
    ]
    rewards = [
        {"rank": 7, "resource_type": 1, "resource_id": 43, "count": 120, "probability_bp": 10000}
    ]
    return {
        "schema": "ournotes-deck-problem@1",
        "song": {"id": 1, "type": 1},
        "chart": {
            "difficulty": "expert",
            "level": 5,
            "notes": [{"t": 0, "op": 1}, {"t": 1000, "op": 2}],
            "skill_times_ms": [0, 1000],
        },
        "catalog": {
            "members": members,
            "snapshots": snapshots,
            "live_skills": [
                {"id": i, "effects": [{"level": 5, "effect_type": 2000, "value": 10000 * i}]}
                for i in range(1, 4)
            ],
            "note_parameters": [{"op": 1, "score_percent": 100}, {"op": 2, "score_percent": 200}],
        },
        "settings": {
            "team_size": 2,
            "base_duration_ms": 100,
            "score_model": {"level_alpha": 0, "rounding": "none"},
        },
        "event": {
            "id": 1,
            "normal": {
                "cp": [{"rank": 7, "value": 10}],
                "pt": [{"rank": 7, "value": 100}],
                "rewards": rewards,
            },
            "challenge": {
                "pt": [{"rank": 7, "value": 5000}],
                "rewards": [dict(rewards[0], count=4950)],
            },
            "normal_boosts": [{"cost": 1, "pt_rate": 5, "reward_rate": 5}],
            "challenge_boosts": [{"cost": 200, "pt_rate": 1, "reward_rate": 1}],
            "assumptions": {
                "score_rank": 7,
                "normal_boost_cost": 1,
                "challenge_cp_cost": 200,
                "shop_resource_id": 43,
                "normal_runs": 3,
                "initial_cp": 51,
            },
        },
    }


def check_event_oracle():
    base = problem()
    base["constraints"] = {"distinct_snapshots": False}
    records = []
    for members in ((1, 2), (1, 3)):
        for snapshots in itertools.product(range(1, 4), repeat=2):
            for order in ((1, 2), (2, 1)):
                formation = {
                    "schema": "ournotes-deck-formation@1",
                    "leader": 1,
                    "slots": [
                        {"member": m, "snapshot": snap, "trigger": trigger}
                        for m, snap, trigger in zip(members, snapshots, order)
                    ],
                }
                score = run("score", base, formation, order_search="given")["results"][0]
                records.append(
                    (members, snapshots, score["index"], score["estimated_score"]["total"])
                )
    rng = random.Random(28017)
    for case in range(12):
        p = copy.deepcopy(base)
        p["constraints"]["distinct_snapshots"] = bool(case % 2)
        assumptions = p["event"]["assumptions"]
        mode = "fixed" if case % 4 == 0 else "estimated_score"
        source = ("none", "pt", "drop")[case % 3]
        minimum = sorted(x[2] for x in records)[len(records) // 2] if case % 3 == 0 else 0
        assumptions.update(score_rank_mode=mode, cp_bonus_source=source, minimum_index=minimum)
        p["event"]["score_ranks"] = [
            {"rank": 2, "required_score": 0},
            {"rank": 4, "required_score": 12000},
            {"rank": 7, "required_score": 17000},
        ]
        for card in p["catalog"]["members"] + p["catalog"]["snapshots"]:
            card["event_pt_bonus_bp"] = rng.randrange(5) * 1379
            card["event_drop_bonus_bp"] = rng.randrange(5) * 1723
        for phase in ("normal", "challenge"):
            p["event"][phase]["pt"] = [
                {"rank": rank, "value": rng.randrange(1, 100)} for rank in (2, 4, 7)
            ]
            p["event"][phase]["rewards"] = [
                {
                    "rank": rank,
                    "resource_type": 1,
                    "resource_id": 43,
                    "count": rng.randrange(1, 100),
                    "probability_bp": 5000,
                }
                for rank in (2, 4, 7)
            ]
        p["event"]["normal"]["cp"] = [
            {"rank": rank, "value": rng.randrange(1, 20)} for rank in (2, 4, 7)
        ]
        expected = {}
        for phase in ("normal", "challenge"):
            yields = set()
            multiplier = 5 if phase == "normal" else 1
            for members, snapshots, index, score in records:
                if index < minimum or (
                    p["constraints"]["distinct_snapshots"] and len(set(snapshots)) < 2
                ):
                    continue
                cards = [p["catalog"]["members"][m - 1] for m in members] + [
                    p["catalog"]["snapshots"][snap - 1] for snap in snapshots
                ]
                pt = sum(card["event_pt_bonus_bp"] for card in cards) / 10000
                drop = sum(card["event_drop_bonus_bp"] for card in cards) / 10000
                rank = (
                    7
                    if mode == "fixed"
                    else max(
                        row["rank"]
                        for row in p["event"]["score_ranks"]
                        if score >= row["required_score"]
                    )
                )
                reward = p["event"][phase]
                cp = 0
                if phase == "normal":
                    cp_bonus = {"none": 0, "pt": pt, "drop": drop}[source]
                    cp = math.floor(
                        next(row["value"] for row in reward["cp"] if row["rank"] == rank)
                        * 5
                        * (1 + cp_bonus)
                    )
                points = math.floor(
                    next(row["value"] for row in reward["pt"] if row["rank"] == rank)
                    * multiplier
                    * (1 + pt)
                )
                shop = (
                    math.floor(
                        next(row["count"] for row in reward["rewards"] if row["rank"] == rank)
                        * multiplier
                        * (1 + drop)
                    )
                    * 0.5
                )
                yields.add((cp, points, shop))
            expected[phase] = {
                y
                for y in yields
                if not any(all(a >= b for a, b in zip(other, y)) and other != y for other in yields)
            }
        actual = run("event", p, method="exact", leaders="1", time_limit=0)
        for phase in ("normal", "challenge"):
            assert actual[f"{phase}_search_complete"]
            observed = {
                tuple(c["yield"][key] for key in ("cp", "pt", "shop_currency_expected"))
                for c in actual[f"{phase}_frontier"]
            }
            assert observed == expected[phase], (case, phase, observed, expected[phase])
        fast = run("event", p, method="fast", leaders="1", time_limit=2)
        assert not fast["optimality_certified"]
        for phase in ("normal", "challenge"):
            assert not fast[f"{phase}_search_complete"]
            observed = {
                tuple(c["yield"][key] for key in ("cp", "pt", "shop_currency_expected"))
                for c in fast[f"{phase}_frontier"]
            }
            assert observed == expected[phase], ("fast", case, phase, observed, expected[phase])
            for candidate in fast[f"{phase}_frontier"]:
                replay = run("score", p, candidate["formation"], order_search="given")["results"][0]
                assert replay["estimated_score"] == candidate["estimated_score"]
                assert candidate["formation"]["leader"] == 1
        for name, axis in (("pt", 1), ("shop_currency_expected", 2)):
            best = max(
                n[axis] + n[0] / 200 * c[axis]
                for n in expected["normal"]
                for c in expected["challenge"]
            )
            assert math.isclose(
                actual["recommended"][name]["amortized_per_normal_live"][name], best
            )
            finite = max(
                3 * n[axis] + math.floor((51 + 3 * n[0]) / 200) * c[axis]
                for n in expected["normal"]
                for c in expected["challenge"]
            )
            assert actual["recommended_finite_budget"][name][name] == finite


def main():
    p = problem()
    f = {
        "schema": "ournotes-deck-formation@1",
        "leader": 1,
        "slots": [
            {"member": 3, "snapshot": 1, "trigger": 1},
            {"member": 1, "snapshot": 2, "trigger": 2},
        ],
    }

    exact = run("event", p, method="exact", time_limit=5)
    assert exact["normal_search_complete"] and exact["challenge_search_complete"]
    assert exact["certified"] is False
    unlimited = run("event", p, method="exact", time_limit=0)
    assert unlimited == exact
    for phase in ("normal", "challenge"):
        audit = exact[f"{phase}_search_audit"]
        assert audit["complete_formations"] >= audit["index_evaluations"]
        assert audit["absolute_score_evaluations"] == len(exact[f"{phase}_frontier"])
        for candidate in exact[f"{phase}_frontier"]:
            replay = run("score", p, candidate["formation"], order_search="given")["results"][0]
            assert replay["estimated_score"] == candidate["estimated_score"]

    def all_yields(phase, required_snapshot=None):
        yields = set()
        for members in itertools.combinations(p["catalog"]["members"], 2):
            for snapshots in itertools.combinations(p["catalog"]["snapshots"], 2):
                if required_snapshot and required_snapshot not in {x["id"] for x in snapshots}:
                    continue
                pt = sum(x.get("event_pt_bonus_bp", 0) for x in members + snapshots) / 10000
                drop = sum(x.get("event_drop_bonus_bp", 0) for x in members + snapshots) / 10000
                yields.add(
                    (
                        50 if phase == "normal" else 0,
                        int((500 if phase == "normal" else 5000) * (1 + pt)),
                        int((600 if phase == "normal" else 4950) * (1 + drop)),
                    )
                )
        return yields

    import math

    normal_yields = all_yields("normal")
    challenge_yields = all_yields("challenge")
    for objective, idx in [("pt", 1), ("shop_currency_expected", 2)]:
        expected = max(
            n[idx] + n[0] / 200 * c[idx] for n in normal_yields for c in challenge_yields
        )
        assert exact["recommended"][objective]["amortized_per_normal_live"][objective] == expected
        expected_finite = max(
            3 * n[idx] + math.floor((51 + 3 * n[0]) / 200) * c[idx]
            for n in normal_yields
            for c in challenge_yields
        )
        assert exact["recommended_finite_budget"][objective][objective] == expected_finite
    restricted = copy.deepcopy(p)
    restricted["constraints"] = {"required_snapshots": [1]}
    limited = run("event", restricted, method="exact", time_limit=5, leaders="1")
    assert limited["requested_leaders"] == [1]
    assert limited["normal_search_complete"] and limited["challenge_search_complete"]
    assert all(
        c["formation"]["leader"] == 1 and 1 in {a["snapshot"] for a in c["formation"]["slots"]}
        for c in limited["normal_frontier"] + limited["challenge_frontier"]
    )
    fast = run("event", p, method="fast", time_limit=5)
    assert fast["normal_search_complete"] is False and fast["challenge_search_complete"] is False
    diverse = problem()
    diverse["catalog"]["members"] = [
        dict(
            diverse["catalog"]["members"][0],
            id=i,
            character=i,
            live_skill=0,
            trained=[100000 if i == 20 else 1] * 3,
            event_pt_bonus_bp=10000 if i < 20 else 0,
        )
        for i in range(1, 21)
    ]
    diverse["event"]["assumptions"]["score_rank_mode"] = "estimated_score"
    diverse["event"]["score_ranks"] = [
        {"rank": 2, "required_score": 0},
        {"rank": 7, "required_score": 100000},
    ]
    diverse["event"]["normal"]["cp"].append({"rank": 2, "value": 1})
    broad = run("event", diverse, method="fast", time_limit=2)
    assert broad["recommended"]["cp"]["yield"]["cp"] == 50
    for phase in ("normal", "challenge"):
        assert broad[f"{phase}_search_audit"]["leaders_seeded"] == 20
        assert broad[f"{phase}_search_audit"]["score_seeds"] > 0
    constrained = copy.deepcopy(diverse)
    constrained["constraints"] = {"required_members": [20], "required_snapshots": [1]}
    limited_fast = run("event", constrained, method="fast", leaders="20", time_limit=2)
    for candidate in limited_fast["normal_frontier"] + limited_fast["challenge_frontier"]:
        assert candidate["formation"]["leader"] == 20
        assert 20 in {slot["member"] for slot in candidate["formation"]["slots"]}
        assert 1 in {slot["snapshot"] for slot in candidate["formation"]["slots"]}
    optimized = run("score", p, f)["results"][0]
    assert optimized["assignments"][1]["member"] == 3
    assert all(a["duration_ms"] == 100 for a in optimized["assignments"])
    skill_level = copy.deepcopy(p)
    skill_level["settings"]["live_level"] = 1
    assert all(
        a["live_boost"] == 0 for a in run("score", skill_level, f)["results"][0]["assignments"]
    )

    individual = problem()
    individual["catalog"]["members"][0]["live_skill_level"] = 1
    per_card = run("score", individual, f, order_search="given")["results"][0]
    assert {x["member"]: x["live_boost"] for x in per_card["assignments"]} == {1: 0, 3: 3}
    leader_levels = problem()
    leader_levels["catalog"]["members"][0].update(leader_skill=101, leader_skill_level=1)
    leader_levels["catalog"]["leader_skills"] = [
        {
            "id": 101,
            "effects": [
                {"level": 1, "effect_type": 1000, "value": 5000},
                {"level": 5, "effect_type": 1000, "value": 20000},
            ],
        }
    ]
    leader_one = run("score", leader_levels, f, order_search="given")["results"][0]
    assert leader_one["power"] == optimized["power"] + 600
    del leader_levels["catalog"]["members"][0]["leader_skill_level"]
    assert run("score", leader_levels, f)["results"][0]["power"] == optimized["power"] + 2400
    support_levels = problem()
    support_levels["catalog"]["support_skills"] = [
        {
            "id": 10,
            "effects": [
                {"level": 1, "effect_type": 15000, "value": 100},
                {"level": 5, "effect_type": 15000, "value": 400},
            ],
        }
    ]
    for card, level in zip(support_levels["catalog"]["snapshots"], [1, 5, 1]):
        card.update(support_skills=[10], support_skill_levels={"10": level})
    supported = run("score", support_levels, f, order_search="given")["results"][0]
    assert {x["snapshot"]: x["duration_ms"] for x in supported["assignments"]} == {1: 200, 2: 500}
    rewritten = copy.deepcopy(f)
    rewritten["slots"] = [
        {k: a[k] for k in ("member", "snapshot", "trigger")} for a in optimized["assignments"]
    ]
    given = run("score", p, rewritten, order_search="given")["results"][0]
    assert abs(given["estimated_score"]["total"] - optimized["estimated_score"]["total"]) < 1e-9
    assert (
        abs(
            optimized["estimated_score"]["unrounded_total"]
            - optimized["estimated_score"]["linearized_total"]
        )
        < 1e-9
    )

    assert optimized["estimated_score"]["total"] > optimized["power"] * 3
    event = run("event", p, method="fast", time_limit=5)
    n = event["normal_frontier"][0]
    c = event["challenge_frontier"][0]
    assert n["members"] == [1, 3]
    assert n["yield"] == {"cp": 50, "pt": 1250, "shop_currency_expected": 1320}
    assert c["yield"] == {"cp": 0, "pt": 12500, "shop_currency_expected": 10890}
    assert event["recommended"]["pt"]["amortized_per_normal_live"]["pt"] == 4375
    assert event["finite_budget"][0] == {
        "normal_candidate": 0,
        "challenge_candidate": 0,
        "challenge_runs": 1,
        "remaining_cp": 1,
        "pt": 16250,
        "shop_currency_expected": 14850,
    }
    rank = run("rank", p, method="exact", top=1, time_limit=5)["results"][0]
    assert rank["members"] != n["members"]

    finite_tradeoff = copy.deepcopy(p)
    finite_tradeoff["event"]["assumptions"].update(
        normal_runs=1, initial_cp=0, score_rank_mode="estimated_score"
    )
    finite_tradeoff["event"]["score_ranks"] = [
        {"rank": 2, "required_score": 0},
        {"rank": 4, "required_score": 1},
        {"rank": 7, "required_score": 16000},
    ]
    finite_tradeoff["catalog"]["members"][0]["event_drop_bonus_bp"] = 80000
    finite_tradeoff["event"]["normal"]["cp"].append({"rank": 4, "value": 5})
    finite_tradeoff["event"]["normal"]["pt"].append({"rank": 4, "value": 35})
    finite_tradeoff["event"]["normal"]["rewards"].append(
        dict(finite_tradeoff["event"]["normal"]["rewards"][0], rank=4, count=42)
    )
    finite_tradeoff["event"]["challenge"]["pt"].append({"rank": 4, "value": 2550})
    finite_tradeoff["event"]["challenge"]["rewards"].append(
        dict(finite_tradeoff["event"]["challenge"]["rewards"][0], rank=4, count=3400)
    )
    plans = run("event", finite_tradeoff, method="fast", time_limit=5)
    long_normal = plans["recommended"]["shop_currency_expected"]["normal_candidate"]
    short_plan = plans["recommended_finite_budget"]["shop_currency_expected"]
    assert plans["normal_frontier"][long_normal]["members"] == [2, 3]
    assert plans["normal_frontier"][short_plan["normal_candidate"]]["members"] == [1, 3]
    assert short_plan["challenge_runs"] == 0
    assert short_plan["shop_currency_expected"] == 2142

    predicted = copy.deepcopy(p)
    predicted["event"]["assumptions"]["score_rank_mode"] = "estimated_score"
    predicted["event"]["score_ranks"] = [
        {"rank": 2, "required_score": 0},
        {"rank": 7, "required_score": 16000},
    ]
    predicted["event"]["normal"]["cp"].append({"rank": 2, "value": 1})
    predicted["event"]["normal"]["pt"].append({"rank": 2, "value": 1})
    predicted["event"]["normal"]["rewards"].append(
        dict(predicted["event"]["normal"]["rewards"][0], rank=2, count=1)
    )
    predicted["event"]["challenge"]["pt"].append({"rank": 2, "value": 1})
    predicted["event"]["challenge"]["rewards"].append(
        dict(predicted["event"]["challenge"]["rewards"][0], rank=2, count=1)
    )
    ranked_yields = run("event", predicted, method="fast", time_limit=5)
    candidate = ranked_yields["normal_frontier"][0]
    assert candidate["members"] == [2, 3]
    assert candidate["score_rank"] == 7
    assert candidate["yield"]["pt"] == 750
    assert candidate["yield"]["cp"] == 50
    assert ranked_yields["score_rank_verified"] is False
    inversion = json.loads(
        (ROOT / "tests/fixtures/theoretical-rounding-inversion.json").read_text()
    )
    inversion["event"] = copy.deepcopy(predicted["event"])
    native = run("rank", inversion, objective="score", method="exact", leaders="40", time_limit=0)[
        "results"
    ][0]
    indexed = run("rank", inversion, objective="index", method="exact", leaders="40", time_limit=0)[
        "results"
    ][0]
    assert native["ranking_score"] > indexed["estimated_score"]["total"]
    inversion["event"]["score_ranks"][1]["required_score"] = native["ranking_score"]
    for row in inversion["event"]["normal"]["cp"]:
        row["value"] = 100 if row["rank"] == 2 else 10
    orders = run("event", inversion, method="exact", leaders="40", time_limit=0)
    assert orders["normal_search_complete"] and orders["challenge_search_complete"]
    assert {x["score_rank"] for x in orders["normal_frontier"]} == {2, 7}
    assert orders["recommended"]["cp"]["yield"]["cp"] == 500
    assert orders["challenge_frontier"][0]["score_rank"] == 7
    for phase in ("normal", "challenge"):
        audit = orders[f"{phase}_search_audit"]
        assert audit["index_evaluations"] == audit["complete_formations"] * 6
        for candidate in orders[f"{phase}_frontier"]:
            replay = run("score", inversion, candidate["formation"], order_search="given")[
                "results"
            ][0]
            assert replay["estimated_score"] == candidate["estimated_score"]
    constrained = copy.deepcopy(inversion)
    constrained["event"]["assumptions"]["minimum_index"] = indexed["index"]
    filtered = run("event", constrained, method="exact", leaders="40", time_limit=0)
    assert all(
        x["score_rank"] == 2 for x in filtered["normal_frontier"] + filtered["challenge_frontier"]
    )
    assert all(
        x["index"] >= indexed["index"]
        for x in filtered["normal_frontier"] + filtered["challenge_frontier"]
    )

    unknown_alpha = copy.deepcopy(predicted)
    unknown_alpha["settings"]["score_model"]["level_alpha"] = None
    try:
        run("event", unknown_alpha, method="fast", time_limit=5)
    except subprocess.CalledProcessError as error:
        assert "显式提供 score_model.level_alpha" in error.stderr
    else:
        raise AssertionError("estimated rank accepted without alpha")

    separate = copy.deepcopy(p)
    separate["event"]["challenge_music_ids"] = [2]
    challenge = copy.deepcopy(separate)
    challenge["song"]["id"] = 2
    challenge["event"]["assumptions"]["score_rank"] = 4
    challenge["event"]["challenge"]["pt"] = [{"rank": 4, "value": 4000}]
    challenge["event"]["challenge"]["rewards"][0]["rank"] = 4
    two = run("event", separate, challenge=challenge, method="fast", time_limit=5)
    assert (two["normal_song_id"], two["challenge_song_id"]) == (1, 2)
    assert two["normal_frontier"][0]["yield"] == n["yield"]
    assert two["challenge_frontier"][0]["yield"]["pt"] == 10000
    assert two["recommended"]["pt"]["amortized_per_normal_live"]["pt"] == 3750
    try:
        run("event", separate, method="fast", time_limit=5)
    except subprocess.CalledProcessError as error:
        assert "课题曲不属于该活动" in error.stderr
    else:
        raise AssertionError("unlisted challenge song accepted")

    ticks = problem()
    ticks["chart"]["notes"] = [{"t": i * 100, "op": 120} for i in range(30)]
    ticks["catalog"]["note_parameters"] = [{"op": 120, "score_percent": 10}]
    assert run("score", ticks, f)["results"][0]["estimated_score"]["converted_note_count"] == 3
    mixed = copy.deepcopy(ticks)
    mixed["catalog"]["note_parameters"].append({"op": 1, "score_percent": 100})
    mixed["chart"]["notes"].append({"t": 3000, "op": 1})
    assert run("score", mixed, f)["results"][0]["estimated_score"]["converted_note_count"] == 4

    alpha = problem()
    alpha["chart"]["level"] = 22
    alpha["settings"]["score_model"]["level_alpha"] = 0.005
    alpha_score = run("score", alpha, f)["results"][0]["estimated_score"]
    assert abs(alpha_score["total"] / optimized["estimated_score"]["total"] - 1.085) < 1e-12
    assert alpha_score["calibrated"] is False

    default_alpha = copy.deepcopy(alpha)
    del default_alpha["settings"]["score_model"]["level_alpha"]
    default_score = run("score", default_alpha, f)["results"][0]["estimated_score"]
    assert default_score["level_alpha"] == 0.005 and default_score["calibrated"] is False
    assert default_score["total"] == alpha_score["total"]
    default_alpha["settings"]["score_model"]["level_alpha"] = None
    assert run("score", default_alpha, f)["results"][0]["estimated_score"] is None

    additive = problem()
    additive["catalog"]["combo_bonuses"] = [{"type": 0, "required_combo_count": 0, "factor": 0.5}]
    a = run("score", additive, f, order_search="given")["results"][0]

    assert abs(a["estimated_score"]["total"] - a["estimated_score"]["base_note_power"] * 12) < 1e-9
    assert (
        abs(a["estimated_score"]["unrounded_total"] - a["estimated_score"]["linearized_total"])
        < 1e-9
    )
    capped = copy.deepcopy(additive)
    capped["catalog"]["combo_bonuses"][0]["factor"] = 3
    cap = run("score", capped, f, order_search="given")["results"][0]
    assert (
        abs(cap["estimated_score"]["total"] - cap["estimated_score"]["base_note_power"] * 16) < 1e-9
    )

    native = copy.deepcopy(additive)
    native["chart"]["level"] = 22
    native["settings"].update(
        score_model={"level_alpha": 0.005, "rounding": "float32_floor"},
        judgement={"mode": "fixed", "fixed_label": "great", "factors": {"great": 0.7}},
        life={"mode": "per_note", "initial": 10, "onus_factor": 0.3, "damage": {"great": 6}},
        assist={"enabled": True, "score_percent": 90},
    )
    actual_native = run("score", native, f, order_search="given")["results"][0]

    def f32(x):
        return struct.unpack("<f", struct.pack("<f", x))[0]

    difficulty = f32(f32(f32(17) * f32(0.005)) + f32(1))
    base = f32(f32(f32(3) * f32(actual_native["power"])) * difficulty)
    expected = 0
    for weight, live, life in [(1, 3, 1), (2, 1, 0.3)]:
        v = f32(f32(weight) * base)
        v = f32(v * f32(0.7))
        v = f32(v * f32(f32(1) + f32(0.5)))
        v = f32(v * f32(f32(1) + f32(live)))
        v = f32(v / f32(3))
        v = f32(int(v))
        v = f32(v * f32(life))
        v = f32(v * f32(0.9))
        expected += int(v)
    assert actual_native["estimated_score"]["total"] == expected
    assert actual_native["estimated_score"]["life"]["minimum"] == 0

    specific = copy.deepcopy(native)
    specific["catalog"]["targets"] = [{"id": 41}, {"id": 46}]
    specific["catalog"]["live_skills"][2]["effects"][0].update(effect_type=2004, targets=[41, 46])
    without = copy.deepcopy(native)
    without["catalog"]["live_skills"][2]["effects"][0]["value"] = 0
    assert (
        run("score", specific, f, order_search="given")["results"][0]["estimated_score"]["total"]
        == run("score", without, f, order_search="given")["results"][0]["estimated_score"]["total"]
    )
    specific["settings"]["judgement"]["fixed_label"] = "perfect"
    all_notes = copy.deepcopy(specific)
    all_notes["catalog"]["live_skills"][2]["effects"][0]["effect_type"] = 2000
    assert (
        run("score", specific, f, order_search="given")["results"][0]["estimated_score"]["total"]
        == run("score", all_notes, f, order_search="given")["results"][0]["estimated_score"][
            "total"
        ]
    )

    conditioned = problem()
    conditioned["catalog"]["conditions"] = [
        {"id": 18, "type": 2001, "positive": True, "values": [700]},
        {"id": 19, "type": 2001, "positive": False, "values": [700]},
    ]
    conditioned["catalog"]["condition_groups"] = [
        {"group": 14, "rows": [[18]]},
        {"group": 15, "rows": [[19]]},
    ]
    conditioned["catalog"]["live_skills"][2]["effects"] = [
        {"level": 5, "effect_type": 2000, "value": 10000, "condition_group": 15},
        {"level": 5, "effect_type": 2000, "value": 30000, "condition_group": 14},
    ]
    for life, expected_boost in [(699, 1), (700, 3), (701, 3)]:
        conditioned["settings"]["life"] = {"mode": "constant", "initial": life}
        result = run("score", conditioned, f, order_search="given")["results"][0]
        assert {a["member"]: a["live_boost"] for a in result["assignments"]}[3] == expected_boost
        explicit = copy.deepcopy(conditioned)
        explicit["catalog"]["live_skills"][2]["effects"] = [
            {"level": 5, "effect_type": 2000, "value": expected_boost * 10000}
        ]
        assert (
            result["estimated_score"]["total"]
            == run("score", explicit, f, order_search="given")["results"][0]["estimated_score"][
                "total"
            ]
        )
    conditioned["settings"]["life"]["mode"] = "per_note"
    try:
        run("event", conditioned, method="fast", time_limit=5)
        assert False, "Dynamic life branches must not silently use the highest boost"
    except subprocess.CalledProcessError as error:
        assert "恒定生命" in error.stderr

    zero_life = problem()
    zero_life["settings"]["life"] = {"mode": "constant", "initial": 0, "onus_factor": 0.3}
    zero = run("score", zero_life, f, order_search="given")["results"][0]
    full = run("score", problem(), f, order_search="given")["results"][0]
    assert abs(zero["estimated_score"]["total"] - full["estimated_score"]["total"] * 0.3) < 1e-9
    assert zero["estimated_score"]["life"]["minimum"] == 0
    zero_life["settings"]["life"]["mode"] = "per_note"
    assert (
        run("score", zero_life, f, order_search="given")["results"][0]["estimated_score"]["total"]
        == zero["estimated_score"]["total"]
    )

    ticks = problem()
    for skill in ticks["catalog"]["live_skills"]:
        skill["effects"] = []
    ticks["chart"]["notes"] = [
        {"t": 0, "op": 1},
        {"t": 500, "op": 3, "scoring": True},
        {"t": 1000, "op": 1},
    ]
    ticks["catalog"]["note_parameters"].append({"op": 3, "score_percent": 10})
    ticks["catalog"]["combo_bonuses"] = [
        {"type": 0, "required_combo_count": 1, "factor": 0.1},
        {"type": 0, "required_combo_count": 2, "factor": 0.2},
    ]
    tick_result = run("score", ticks, f, order_search="given")["results"][0]
    assert abs(tick_result["estimated_score"]["total"] - tick_result["power"] * 2.41) < 1e-9

    video = problem()
    video["settings"]["power_model"] = {
        "sources": {
            key: False
            for key in (
                "snapshot",
                "type_link",
                "band_item",
                "music_type",
                "music_tag",
                "leader",
                "vip",
            )
        }
    }
    for m in video["catalog"]["members"]:
        m["trained"] = [0, 0, 0]
        m["live_skill"] = 0
    video["catalog"]["members"][2]["trained"] = [75038, 0, 0]
    video["catalog"]["members"][2]["live_skill"] = 3
    video["catalog"]["live_skills"][2]["effects"] = [
        {"level": 5, "effect_type": 2000, "value": 1000}
    ]
    video["settings"]["score_model"] = {"level_alpha": 0.005, "rounding": "float32_floor"}
    video["chart"]["level"] = 27
    video["chart"]["notes"] = [{"t": i, "op": 1} for i in range(60)] + [{"t": 1000, "op": 1}]
    video["chart"]["skill_times_ms"] = [1000, 2000]
    video["catalog"]["combo_bonuses"] = [
        {"type": 0, "required_combo_count": i, "factor": 0.01} for i in range(10, 61, 10)
    ]
    native_video = run("score", video, f, order_search="given")["results"][0]["estimated_score"][
        "total"
    ]

    def f32(x):
        return struct.unpack("f", struct.pack("f", x))[0]

    combo_native = 0.0
    expected_video = 0
    for i in range(61):
        if i and i % 10 == 0:
            combo_native = f32(combo_native + f32(0.01))
        v = f32(
            f32(f32(3 * 75038) * f32(f32(f32(27 - 5) * f32(0.005)) + 1)) * f32(1 + combo_native)
        )
        v = f32(v * f32(1.1 if i == 60 else 1))
        expected_video += int(f32(v / f32(61)))
    assert native_video == expected_video

    detailed = run("score", p, f, detail=True)["results"][0]
    orders = []
    for sequence in itertools.permutations(f["slots"]):
        formation = copy.deepcopy(f)
        formation["slots"] = [dict(slot, trigger=i + 1) for i, slot in enumerate(sequence)]
        orders.append(
            run("score", p, formation, order_search="given")["results"][0]["estimated_score"][
                "total"
            ]
        )
    rank_detailed = run("rank", p, method="fast", top=1, time_limit=5, detail=True)["results"][0]
    assert rank_detailed["order_analysis"]["score_permutations"] == 2
    analysis = detailed["order_analysis"]
    assert analysis["best_order_is_controllable"] is False
    assert analysis["best_score"] == max(orders) and analysis["worst_score"] == min(orders)
    assert analysis["mean_score"] == sum(orders) / len(orders)

    section = copy.deepcopy(p)
    section["settings"]["score_model"]["rounding"] = "float32_floor"
    plain = run("score", section, f, order_search="given")["results"][0]["estimated_score"]["total"]
    section["settings"]["gekisou"] = {
        "enabled": True,
        "sections": [{"start_ms": 0, "end_ms": 100, "bonus_bp": 35000}],
    }
    decorated = run("score", section, f, order_search="given")["results"][0]["estimated_score"]
    segment = (
        decorated["gekisou"][0]
        if isinstance(decorated["gekisou"], list)
        else decorated["gekisou"]["sections"][0]
    )
    assert decorated["total"] == plain + int(segment["base_score"] * 2.5)

    spec = importlib.util.spec_from_file_location(
        "master_compiler", ROOT / "tools/compile_from_master.py"
    )
    compiler = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(compiler)
    from unittest.mock import patch

    class SongTables:
        def table(self, name):
            assert name == "MasterLiveMusicScore"
            return [{"_id": 10005703, "_musicScoreTextFileName": "0057/0057_03"}]

    ordinary_music = {"_id": 100057, "_expertID": 10005703, "_musicType": 4}
    with patch.object(compiler, "build_chart", return_value={"difficulty": "expert"}):
        assert (
            compiler.build_song(SongTables(), {}, ordinary_music, [], Path("."), "expert")["type"]
            == 4
        )
        assert (
            compiler.build_song(
                SongTables(), {}, ordinary_music, [{"_musicType": 2}], Path("."), "expert"
            )["type"]
            == 2
        )
        assert (
            compiler.build_song(
                SongTables(), {}, ordinary_music, [{"_musicType": 2}], Path("."), "expert", "normal"
            )["type"]
            == 4
        )
        assert (
            compiler.build_song(
                SongTables(),
                {},
                ordinary_music,
                [{"_musicType": 2}],
                Path("."),
                "expert",
                "challenge",
            )["type"]
            == 2
        )
        try:
            compiler.build_song(
                SongTables(), {}, ordinary_music, [], Path("."), "expert", "challenge"
            )
        except compiler.CompileError:
            pass
        else:
            raise AssertionError("Challenge context accepted missing active task")
    rules = [{"_memberCardId": 1, "_rank1EffectValue": 1000, "_rank5EffectValue": 5000}]
    assert compiler._event_bonus_bp(rules, 2, {"_id": 1}, {1}, {1}, 1, set(), 1) == 1000

    class SnapshotTables:
        def table(self, name):
            return {
                "MasterSupportCardLevel": [
                    {
                        "_group": 1,
                        "_level": 30,
                        "_performanceRate": 0,
                        "_technicRate": 0,
                        "_visualRate": 0,
                    },
                    {
                        "_group": 1,
                        "_level": 50,
                        "_performanceRate": 1000,
                        "_technicRate": 1000,
                        "_visualRate": 1000,
                    },
                ],
                "MasterSupportCardRank": [
                    {
                        "_group": 1,
                        "_rank": 1,
                        "_limitLevel": 30,
                        "_supportSkill01Level": 1,
                        "_supportSkill02Level": 1,
                    }
                ],
                "MasterSupportCard": [
                    {
                        "_id": 1,
                        "_supportCardLevelGroup": 1,
                        "_supportCardRankGroup": 1,
                        "_performancePowerMax": 1000,
                        "_technicPowerMax": 1000,
                        "_visualPowerMax": 1000,
                        "_characterIDs": [1],
                        "_supportSkillId01": 10,
                        "_supportSkillId02": 11,
                    }
                ],
            }.get(name, [])

    built = compiler.build_snapshots(SnapshotTables(), {}, {1: {"_bandID": 1}}, 0, None, 1)
    assert built[0]["level"] == 30
    assert built[0]["support_skill_levels"] == {"10": 1, "11": 1}
    try:
        compiler.build_snapshots(SnapshotTables(), {}, {1: {"_bandID": 1}}, 0, 50, 1)
    except compiler.CompileError as error:
        assert "等级上限为 30" in str(error)
    else:
        raise AssertionError("rank-limited Snapshot accepted invalid level")

    sample = json.loads((ROOT / "tests/fixtures/normal-live-observed.json").read_text())
    observed = problem()
    observed["event"]["normal"] = {
        "cp": [{"rank": 4, "value": 5}],
        "pt": [{"rank": 4, "value": 35}],
        "rewards": [
            {"rank": 4, "resource_type": 1, "resource_id": 43, "count": 42, "probability_bp": 10000}
        ],
    }
    observed["event"]["challenge"] = {
        "pt": [{"rank": 4, "value": 2550}],
        "rewards": [
            {
                "rank": 4,
                "resource_type": 1,
                "resource_id": 43,
                "count": 3400,
                "probability_bp": 10000,
            }
        ],
    }
    observed["event"]["normal_boosts"] = [{"cost": 3, "pt_rate": 15, "reward_rate": 15}]
    observed["event"]["assumptions"].update(score_rank=4, normal_boost_cost=3)
    for card in observed["catalog"]["members"]:
        card["event_pt_bonus_bp"] = 0
    observed["catalog"]["members"][0]["event_pt_bonus_bp"] = 12400
    for card in observed["catalog"]["snapshots"]:
        card["event_drop_bonus_bp"] = 0
    observed["catalog"]["snapshots"][0]["event_drop_bonus_bp"] = 12000
    result = run("event", observed, method="fast", time_limit=5)
    assert result["normal_frontier"][0]["yield"] == sample["observed"]

    band = problem()
    band["catalog"]["members"][0]["band"] = 1
    band["catalog"]["members"][2]["band"] = 3
    band["catalog"]["fix"] = {"band_item_bonus_bp_by_band": {"1": [230] * 3, "3": [210] * 3}}
    changed = run("score", band, f)["results"][0]
    assert changed["power"] == optimized["power"] + 6 + 18
    changed_rank = run("rank", band, method="exact", top=1, time_limit=5)["results"][0]
    rescored = {
        "schema": "ournotes-deck-formation@1",
        "leader": changed_rank["leader"],
        "slots": [
            {k: a[k] for k in ("member", "snapshot", "trigger")}
            for a in changed_rank["assignments"]
        ],
    }
    assert run("score", band, rescored)["results"][0]["power"] == changed_rank["power"]
    check_event_oracle()
    print(
        "Synthetic score/event integration checks passed; 12 exact/fast exhaustive reward-frontier cases"
    )


if __name__ == "__main__":
    main()
