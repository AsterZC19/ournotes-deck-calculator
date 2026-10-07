#!/usr/bin/env python3
"""Independent per-order reward averaging and exact frontier checks."""

import copy
import itertools
import math
from test_event import problem, run


def frontier(rows):
    return {y for y in rows if not any(z != y and all(a >= b for a, b in zip(z, y)) for z in rows)}


def main():
    distribution_pruned = bound_pruned = False
    p = problem()
    p["constraints"] = {"leader_pool": [1], "distinct_snapshots": True}
    p["event"]["assumptions"].update(score_rank_mode="estimated_score", minimum_index=0)
    p["event"]["assumptions"].pop("normal_runs", None)
    records = []
    for members in ((1, 2), (1, 3)):
        for snapshots in itertools.permutations(range(1, 4), 2):
            f = {
                "schema": "ournotes-deck-formation@1",
                "leader": 1,
                "slots": [
                    {"member": m, "snapshot": s, "trigger": i + 1}
                    for i, (m, s) in enumerate(zip(members, snapshots))
                ],
            }
            scores = []
            for triggers in ((1, 2), (2, 1)):
                for slot, trigger in zip(f["slots"], triggers):
                    slot["trigger"] = trigger
                scores.append(
                    run("score", p, f, order_search="given")["results"][0]["estimated_score"][
                        "total"
                    ]
                )
            records.append((members, snapshots, scores))
    scores = sorted({s for _, _, values in records for s in values})
    for case, threshold in enumerate(scores[1 :: max(1, len(scores) // 5)]):
        q = copy.deepcopy(p)
        q["event"]["score_ranks"] = [
            {"rank": 2, "required_score": 0},
            {"rank": 7, "required_score": threshold},
        ]
        for phase in ("normal", "challenge"):
            q["event"][phase]["pt"] = [{"rank": 2, "value": 31}, {"rank": 7, "value": 100}]
            q["event"][phase]["rewards"] = [
                {
                    "rank": r,
                    "count": n,
                    "resource_type": 1,
                    "resource_id": 43,
                    "probability_bp": 5000,
                }
                for r, n in ((2, 93 if case % 2 else 13), (7, 43))
            ]
        q["event"]["normal"]["cp"] = [{"rank": 2, "value": 3}, {"rank": 7, "value": 10}]
        q["event"]["assumptions"]["cp_bonus_source"] = "pt" if case % 2 == 0 else "drop"
        laws = {}
        for phase in ("normal", "challenge"):
            expected = set()
            laws[phase] = []
            for members, snapshots, values in records:
                pt = (
                    sum(q["catalog"]["members"][m - 1].get("event_pt_bonus_bp", 0) for m in members)
                    / 10000
                )
                drop = (
                    sum(
                        q["catalog"]["snapshots"][s - 1].get("event_drop_bonus_bp", 0)
                        for s in snapshots
                    )
                    / 10000
                )
                multiplier = 5 if phase == "normal" else 1
                rewards = []
                for score in values:
                    rank = 7 if score >= threshold else 2
                    tables = q["event"][phase]
                    base = next(r["value"] for r in tables["pt"] if r["rank"] == rank)
                    items = next(r["count"] for r in tables["rewards"] if r["rank"] == rank)
                    cp_bonus = pt if q["event"]["assumptions"]["cp_bonus_source"] == "pt" else drop
                    cp = (
                        math.floor(multiplier * (10 if rank == 7 else 3) * (1 + cp_bonus))
                        if phase == "normal"
                        else 0
                    )
                    rewards.append(
                        (
                            cp,
                            math.floor(base * multiplier * (1 + pt)),
                            math.floor(items * multiplier * (1 + drop)) * 0.5,
                        )
                    )
                expected.add(tuple(sum(r[i] for r in rewards) / len(rewards) for i in range(3)))
                laws[phase].append(rewards)
            result = run("event", q, objective="mean", method="exact", time_limit=0)
            observed = {
                tuple(c["yield"][k] for k in ("cp", "pt", "shop_currency_expected"))
                for c in result[phase + "_frontier"]
            }
            assert observed == frontier(expected), (case, phase, observed, frontier(expected))
            assert result["optimality_certified"] and result["reward_objective"] == "mean_reward"
            for c in result[phase + "_frontier"]:
                assert abs(sum(r["probability"] for r in c["score_rank_distribution"]) - 1) < 1e-12
                assert sum(r["orders"] for r in c["score_rank_distribution"]) == 2
        default = run("event", q, objective=None, method="exact", time_limit=0)
        assert default["reward_objective"] == "mean_reward"
        fast = run("event", q, objective="mean", method="fast", time_limit=2)
        for phase in ("normal", "challenge"):
            assert fast[phase + "_frontier"]
            assert all(c.get("score_rank_distribution") for c in fast[phase + "_frontier"])
        if case < 6:
            for runs in (0, 1, 2, 3):
                finite = copy.deepcopy(q)
                finite["event"]["assumptions"].update(
                    normal_runs=runs, initial_cp=11, challenge_cp_cost=37
                )
                finite["event"]["challenge_boosts"][0]["cost"] = 37
                result = run("event", finite, objective="mean", method="exact", time_limit=0)
                audit = result["normal_search_audit"]
                distribution_pruned |= audit["cp_distribution_pruned_candidates"] > 0
                bound_pruned |= audit["cp_distribution_pruned_branches"] > 0
                expected = []
                for normal in laws["normal"]:
                    totals = [
                        sum(outcomes)
                        for outcomes in itertools.product([r[0] for r in normal], repeat=runs)
                    ]
                    plays = sum((11 + cp) // 37 for cp in totals) / len(totals)
                    for challenge in laws["challenge"]:
                        expected.append(
                            tuple(
                                runs * sum(r[i] for r in normal) / len(normal)
                                + plays * sum(r[i] for r in challenge) / len(challenge)
                                for i in (1, 2)
                            )
                        )
                for key, axis in (("pt", 0), ("shop_currency_expected", 1)):
                    got = result["recommended_finite_budget"][key][key]
                    assert abs(got - max(r[axis] for r in expected)) < 1e-8, (
                        runs,
                        key,
                        got,
                        expected,
                    )
                for plan in result["finite_budget"]:
                    assert 0 <= plan["remaining_cp"] < 37
                    if "remaining_cp_distribution" in plan:
                        assert (
                            abs(
                                sum(r["probability"] for r in plan["remaining_cp_distribution"]) - 1
                            )
                            < 1e-12
                        )
    members, snapshots, scores = next(row for row in records if row[2][0] != row[2][1])
    example = copy.deepcopy(q)
    example["constraints"].update(
        required_members=list(members), required_snapshots=list(snapshots)
    )
    example["event"]["score_ranks"] = [
        {"rank": 2, "required_score": 0},
        {"rank": 7, "required_score": sum(scores) / 2},
    ]
    example["event"]["normal"]["cp"] = [{"rank": 2, "value": 12}, {"rank": 7, "value": 18}]
    example["event"]["assumptions"].update(
        normal_runs=2, initial_cp=0, challenge_cp_cost=160, cp_bonus_source="none"
    )
    example["event"]["challenge_boosts"][0]["cost"] = 160
    result = run("event", example, objective="mean", method="exact", time_limit=0)
    for plan in result["finite_budget"]:
        assert abs(plan["challenge_runs"] - 0.25) < 1e-12, plan
        assert abs(plan["remaining_cp"] - 110) < 1e-12, plan
    example["event"]["assumptions"]["normal_runs"] = 1000000
    result = run("event", example, objective="mean", method="exact", time_limit=0)
    for plan in result["finite_budget"]:
        assert abs(plan["remaining_cp"] - 75) < 1e-8
        assert abs(plan["challenge_runs"] - (75000000 - 75) / 160) < 1e-8
    assert distribution_pruned and bound_pruned
    print(
        "Mean event rewards passed independent exhaustive frontier checks, cross-rank averaging and nonmonotonic rewards"
    )


if __name__ == "__main__":
    main()
