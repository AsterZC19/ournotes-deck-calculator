#!/usr/bin/env python3
"""Independent per-order reward averaging and exact frontier checks."""

import copy
import itertools
import math
from test_event import problem, run


def frontier(rows):
    return {y for y in rows if not any(z != y and all(a >= b for a, b in zip(z, y)) for z in rows)}


def main():
    p = problem()
    p["constraints"] = {"leader_pool": [1], "distinct_snapshots": True}
    p["event"]["assumptions"].update(score_rank_mode="estimated_score", minimum_index=0)
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
        for phase in ("normal", "challenge"):
            expected = set()
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
                    cp = multiplier * (10 if rank == 7 else 3) if phase == "normal" else 0
                    rewards.append(
                        (
                            cp,
                            math.floor(base * multiplier * (1 + pt)),
                            math.floor(items * multiplier * (1 + drop)) * 0.5,
                        )
                    )
                expected.add(tuple(sum(r[i] for r in rewards) / len(rewards) for i in range(3)))
            result = run("event", q, objective="mean", method="exact", time_limit=0)
            observed = {
                tuple(c["yield"][k] for k in ("cp", "pt", "shop_currency_expected"))
                for c in result[phase + "_frontier"]
            }
            assert observed == frontier(expected), (case, phase, observed, frontier(expected))
            assert result["optimality_certified"] and result["reward_objective"] == "mean_reward"
            for c in result[phase + "_frontier"]:
                assert abs(sum(r["probability"] for r in c["score_rank_distribution"]) - 1) < 1e-12
        default = run("event", q, objective=None, method="exact", time_limit=0)
        assert default["reward_objective"] == "mean_reward"
        fast = run("event", q, objective="mean", method="fast", time_limit=2)
        for phase in ("normal", "challenge"):
            assert fast[phase + "_frontier"]
            assert all(c.get("score_rank_distribution") for c in fast[phase + "_frontier"])
    print(
        "Mean event rewards passed independent exhaustive frontier checks, cross-rank averaging and nonmonotonic rewards"
    )


if __name__ == "__main__":
    main()
