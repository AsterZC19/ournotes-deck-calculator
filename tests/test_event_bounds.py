#!/usr/bin/env python3
"""Compare bounded reward searches with independent native-score enumeration."""

import copy
import itertools
import math
import random
import subprocess

from test_event import problem, run


def yield_for(p, formation, score, phase):
    ranks = p["event"]["score_ranks"]
    rank = max(row["rank"] for row in ranks if score >= row["required_score"])
    cards = {key: {c["id"]: c for c in p["catalog"][key]} for key in ("members", "snapshots")}
    selected = [
        cards[key][slot[field]]
        for slot in formation["slots"]
        for key, field in (("members", "member"), ("snapshots", "snapshot"))
    ]
    pt = sum(c.get("event_pt_bonus_bp", 0) for c in selected) / 10000
    drop = sum(c.get("event_drop_bonus_bp", 0) for c in selected) / 10000
    rate = 5 if phase == "normal" else 1
    rewards = p["event"][phase]
    points = math.floor(
        next(x["value"] for x in rewards["pt"] if x["rank"] == rank) * rate * (1 + pt)
    )
    currency = sum(
        math.floor(x["count"] * rate * (1 + drop)) * x["probability_bp"] / 10000
        for x in rewards["rewards"]
        if x["rank"] == rank
    )
    cp = 0
    if phase == "normal":
        bonus = {"none": 0, "pt": pt, "drop": drop}[p["event"]["assumptions"]["cp_bonus_source"]]
        cp = math.floor(
            next(x["value"] for x in rewards["cp"] if x["rank"] == rank) * 5 * (1 + bonus)
        )
    return cp, points, currency


def enumerate_scores(p, leader_id=1):
    constraints = p["constraints"]
    size = p["settings"]["team_size"]
    records = []
    members = [
        m
        for m in p["catalog"]["members"]
        if m["id"] in constraints.get("member_pool", [c["id"] for c in p["catalog"]["members"]])
    ]
    leader = next(m for m in members if m["id"] == leader_id)
    for tail in itertools.combinations([m for m in members if m["id"] != leader_id], size - 1):
        group = (leader, *tail)
        ids = {m["id"] for m in group}
        if constraints["distinct_characters"] and len({m["character"] for m in group}) < size:
            continue
        if not set(constraints.get("required_members", [])).issubset(ids):
            continue
        for snapshots in itertools.product(
            constraints.get("snapshot_pool", [c["id"] for c in p["catalog"]["snapshots"]]),
            repeat=size,
        ):
            if constraints["distinct_snapshots"] and len(set(snapshots)) < size:
                continue
            if not set(constraints.get("required_snapshots", [])).issubset(snapshots):
                continue
            for order in itertools.permutations(range(1, size + 1)):
                f = {
                    "schema": "ournotes-deck-formation@1",
                    "leader": leader_id,
                    "slots": [
                        {"member": m["id"], "snapshot": s, "trigger": t}
                        for m, s, t in zip(group, snapshots, order)
                    ],
                }
                result = run("score", p, f, order_search="given")["results"][0]
                records.append((f, result["index"], result["estimated_score"]["total"]))
    return records


def main():
    rng = random.Random(92061)
    pruned = 0
    for case in range(22):
        p = problem()
        p["catalog"]["members"].append(dict(p["catalog"]["members"][2], id=4, character=4))
        p["settings"]["score_model"] = {"level_alpha": 0.005, "rounding": "float32_floor"}
        p["chart"]["level"] = 19
        p["chart"]["notes"] = [{"t": 100 * i, "op": 1 if i % 3 else 2} for i in range(16)]
        p["chart"]["skill_times_ms"] = [0, 700]
        p["settings"]["base_duration_ms"] = 500
        p["constraints"] = {"distinct_snapshots": bool(case % 2), "distinct_characters": True}
        if case >= 12:
            p["settings"]["team_size"] = 3
            p["chart"]["skill_times_ms"].append(1200)
        if case % 3 == 0:
            p["constraints"].update(required_members=[3, 3], required_snapshots=[2, 2])
        if case % 5 == 0:
            p["catalog"]["members"][3]["character"] = 2
        if case == 17:
            p["settings"]["assist"] = {"enabled": True, "score_percent": 0}
        if case == 18:
            p["settings"]["judgement"] = {"mode": "fixed", "fixed_label": "great"}
            p["settings"]["life"] = {
                "mode": "per_note",
                "initial": 10,
                "onus_factor": 0.3,
                "damage": {"great": 6},
            }
        if case in (19, 20):
            p["settings"]["score_model"]["rounding"] = "float64_floor" if case == 19 else "none"
        if case == 21:
            p["settings"]["gekisou"] = {
                "enabled": True,
                "sections": [{"start_ms": 0, "end_ms": 1000, "bonus_bp": 35000}],
            }
        for m in p["catalog"]["members"]:
            m["trained"] = [rng.randrange(10, 2000000)] * 3
        for card in p["catalog"]["members"] + p["catalog"]["snapshots"]:
            card["event_pt_bonus_bp"] = rng.randrange(6) * 1723 + (0.1 if case % 2 else 0)
            card["event_drop_bonus_bp"] = rng.randrange(6) * 2131 + (0.2 if case % 2 else 0)
        p["catalog"]["leader_skills"] = [
            {"id": m["id"], "effects": [{"level": 5, "effect_type": 3000, "value": m["id"] * 1250}]}
            for m in p["catalog"]["members"]
        ]
        for m in p["catalog"]["members"]:
            m["leader_skill"] = m["id"]
        if case in (6, 12):
            p["constraints"]["member_pool"] = [1, 3, 4]
        if case == 7:
            p["constraints"]["snapshot_pool"] = [1, 2]
        leaders = [1, 4] if case in (5, 11, 14) else [1]
        records = [record for leader in leaders for record in enumerate_scores(p, leader)]
        scores = sorted(r[2] for r in records)
        middle = max(1, scores[len(scores) // 2])
        maximum = max(middle + 1, scores[-1] + (1 if case % 2 else 0))
        p["event"]["score_ranks"] = [
            {"rank": 2, "required_score": 0},
            {"rank": 4, "required_score": middle},
            {"rank": 7, "required_score": maximum},
        ]
        minimum = sorted(r[1] for r in records)[len(records) // 3] if case % 4 == 0 else 0
        p["event"]["assumptions"].update(
            score_rank_mode="estimated_score",
            minimum_index=minimum,
            cp_bonus_source=("none", "pt", "drop")[case % 3],
        )
        for phase in ("normal", "challenge"):
            p["event"][phase]["pt"] = [
                {"rank": rank, "value": rng.randrange(1, 500)} for rank in (2, 4, 7)
            ]
            p["event"][phase]["rewards"] = [
                {
                    "rank": rank,
                    "resource_type": 1,
                    "resource_id": 43,
                    "count": rng.randrange(1, 500),
                    "probability_bp": 5000,
                }
                for rank in (2, 4, 7)
            ]
        p["event"]["normal"]["cp"] = [
            {"rank": rank, "value": rng.randrange(1, 20)} for rank in (2, 4, 7)
        ]
        actual = run("event", p, method="exact", leaders=",".join(map(str, leaders)), time_limit=0)
        assert actual["optimality_certified"]
        expected = {}
        for phase in ("normal", "challenge"):
            values = {
                yield_for(p, f, score, phase) for f, index, score in records if index >= minimum
            }
            frontier = {
                v
                for v in values
                if not any(all(x >= y for x, y in zip(o, v)) and o != v for o in values)
            }
            expected[phase] = frontier
            observed = {
                tuple(c["yield"][k] for k in ("cp", "pt", "shop_currency_expected"))
                for c in actual[phase + "_frontier"]
            }
            assert observed == frontier, (case, phase, observed, frontier)
            audit = actual[phase + "_search_audit"]
            assert audit["score_bound_enabled"] == (case < 17)
            pruned += audit["reward_pruned_branches"]
            for c in actual[phase + "_frontier"]:
                replay = run("score", p, c["formation"], order_search="given")["results"][0]
                assert replay["estimated_score"] == c["estimated_score"]
        for name, axis in (("pt", 1), ("shop_currency_expected", 2)):
            best = max(
                n[axis] + n[0] / 200 * c[axis]
                for n in expected["normal"]
                for c in expected["challenge"]
            )
            assert math.isclose(
                actual["recommended"][name]["amortized_per_normal_live"][name], best
            )
    assert pruned > 0
    for count, distinct, member_count in (
        (64, True, 3),
        (64, False, 3),
        (65, True, 3),
        (65, False, 3),
        (129, True, 3),
        (3, True, 129),
    ):
        p = problem()
        p["settings"]["score_model"] = {"level_alpha": 0.005, "rounding": "float32_floor"}
        p["settings"]["base_duration_ms"] = 500
        p["chart"]["notes"] = [{"t": 100 * i, "op": 1} for i in range(16)]
        p["chart"]["skill_times_ms"] = [0, 700]
        template = p["catalog"]["snapshots"][2]
        p["catalog"]["snapshots"] = [
            dict(copy.deepcopy(template), id=i) for i in range(1, count + 1)
        ]
        for card in p["catalog"]["snapshots"]:
            if card["id"] not in (1, count):
                card.update(trained=[1000000] * 3, event_pt_bonus_bp=100000)
        p["constraints"] = {
            "distinct_characters": True,
            "distinct_snapshots": distinct,
            "required_snapshots": [1, count, count],
        }
        if member_count > 3:
            template_member = p["catalog"]["members"][2]
            p["catalog"]["members"] = [
                dict(copy.deepcopy(template_member), id=i, character=i)
                for i in range(1, member_count + 1)
            ]
            p["constraints"]["required_members"] = [1, member_count, member_count]
        records = enumerate_scores(p)
        maximum = max(score for _, _, score in records)
        p["event"]["score_ranks"] = [
            {"rank": 2, "required_score": 0},
            {"rank": 7, "required_score": maximum + 1},
        ]
        p["event"]["assumptions"].update(score_rank_mode="estimated_score", cp_bonus_source="none")
        p["event"]["normal"]["cp"] = [{"rank": 2, "value": 1}, {"rank": 7, "value": 10}]
        for phase in ("normal", "challenge"):
            p["event"][phase]["pt"] = [{"rank": 2, "value": 1}, {"rank": 7, "value": 100}]
        actual = run("event", p, method="exact", leaders="1", time_limit=0)
        assert actual["optimality_certified"]
        for phase in ("normal", "challenge"):
            values = {yield_for(p, f, score, phase) for f, _, score in records}
            expected = {
                v
                for v in values
                if not any(all(x >= y for x, y in zip(o, v)) and o != v for o in values)
            }
            observed = {
                tuple(c["yield"][k] for k in ("cp", "pt", "shop_currency_expected"))
                for c in actual[phase + "_frontier"]
            }
            assert observed == expected, (count, distinct, phase, observed, expected)
            audit = actual[phase + "_search_audit"]
            assert audit["cached_snapshot_nodes"] > 0
            assert audit["infeasible_branches"] > 0
            for c in actual[phase + "_frontier"]:
                replay = run("score", p, c["formation"], order_search="given")["results"][0]
                assert replay["estimated_score"] == c["estimated_score"]
    p = problem()
    p["settings"].update(
        team_size=6, score_model={"level_alpha": 0.005, "rounding": "float32_floor"}
    )
    p["catalog"]["members"] = [
        dict(copy.deepcopy(p["catalog"]["members"][0]), id=i, character=i) for i in range(1, 7)
    ]
    p["catalog"]["snapshots"] = p["catalog"]["snapshots"][:1]
    p["chart"]["skill_times_ms"] = [0] * 6
    p["constraints"] = {
        "distinct_characters": True,
        "distinct_snapshots": False,
        "required_members": list(range(1, 7)),
        "required_snapshots": [1],
    }
    p["event"]["score_ranks"] = [{"rank": 2, "required_score": 0}, {"rank": 7, "required_score": 1}]
    p["event"]["assumptions"]["cp_bonus_source"] = "none"
    formation = {
        "schema": "ournotes-deck-formation@1",
        "leader": 1,
        "slots": [{"member": i, "snapshot": 1, "trigger": i} for i in range(1, 7)],
    }
    score = run("score", p, formation, order_search="given")["results"][0]["estimated_score"][
        "total"
    ]
    assert score >= 1
    for mode in ("estimated_score", "fixed"):
        p["event"]["assumptions"]["score_rank_mode"] = mode
        actual = run("event", p, method="exact", leaders="1", time_limit=0)
        assert actual["optimality_certified"]
        for phase in ("normal", "challenge"):
            expected = yield_for(p, formation, score, phase)
            observed = {
                tuple(c["yield"][k] for k in ("cp", "pt", "shop_currency_expected"))
                for c in actual[phase + "_frontier"]
            }
            assert observed == {expected}, (mode, phase, observed, expected)
            assert not actual[phase + "_search_audit"]["score_bound_enabled"]
            assert actual[phase + "_search_audit"]["cached_snapshot_nodes"] > 0
        fast = run("event", p, method="fast", leaders="1", time_limit=1)
        assert not fast["optimality_certified"]
        for phase in ("normal", "challenge"):
            assert {
                tuple(c["yield"][k] for k in ("cp", "pt", "shop_currency_expected"))
                for c in fast[phase + "_frontier"]
            } == {yield_for(p, formation, score, phase)}
    for distinct, banned in ((True, []), (False, [1])):
        invalid = copy.deepcopy(p)
        invalid["constraints"].update(
            distinct_snapshots=distinct, banned_snapshots=banned, required_snapshots=[]
        )
        try:
            run("event", invalid, method="exact", leaders="1", time_limit=0)
        except subprocess.CalledProcessError as error:
            assert "可用 Snapshot" in error.stderr
        else:
            raise AssertionError("Invalid snapshot pool was accepted")
    print("Bounded event oracle passed: 30 cases, scoring models, large card pools and constraints")


if __name__ == "__main__":
    main()
