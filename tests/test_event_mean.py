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

    # Non-AP deterministic sequence judgement + effect_type == 2004 (perfect-only live skill):
    # score_bound_enabled must be True and exact frontier must match brute-force oracle.
    seq_p = copy.deepcopy(p)
    seq_p["settings"]["score_model"] = {"level_alpha": 0.005, "rounding": "float32_floor"}
    seq_p["chart"]["notes"] = [{"t": 100 * i, "op": 1} for i in range(20)]
    seq_p["chart"]["skill_times_ms"] = [0, 900]
    seq_p["settings"]["base_duration_ms"] = 600
    seq_p["settings"]["judgement"] = {
        "mode": "sequence",
        "sequence": ["great" if i % 5 == 0 else "perfect" for i in range(20)],
    }
    seq_p["catalog"]["targets"] = [{"id": 41, "judgement": 5}, {"id": 46, "judgement": 6}]
    seq_p["catalog"]["live_skills"] = [
        {"id": 1, "effects": [{"level": 5, "effect_type": 2000, "value": 8000}]},
        {
            "id": 2,
            "effects": [{"level": 5, "effect_type": 2004, "value": 15000, "targets": [41, 46]}],
        },
    ]
    seq_p["catalog"]["members"][0]["live_skill"] = 1
    seq_p["catalog"]["members"][1]["live_skill"] = 2
    seq_p["catalog"]["members"][2]["live_skill"] = 1
    seq_records = []
    for m_pair in ((1, 2), (1, 3)):
        for s_pair in itertools.permutations(range(1, 4), 2):
            f = {
                "schema": "ournotes-deck-formation@1",
                "leader": 1,
                "slots": [
                    {"member": m, "snapshot": s, "trigger": i + 1}
                    for i, (m, s) in enumerate(zip(m_pair, s_pair))
                ],
            }
            sc = []
            for tr in ((1, 2), (2, 1)):
                for sl, t in zip(f["slots"], tr):
                    sl["trigger"] = t
                sc.append(
                    run("score", seq_p, f, order_search="given")["results"][0]["estimated_score"][
                        "total"
                    ]
                )
            seq_records.append((m_pair, s_pair, sc))
    all_seq_scores = sorted(s for _, _, sc in seq_records for s in sc)
    mid_th = all_seq_scores[len(all_seq_scores) // 2]
    seq_p["event"]["score_ranks"] = [
        {"rank": 2, "required_score": 0},
        {"rank": 7, "required_score": mid_th},
    ]
    for phase in ("normal", "challenge"):
        seq_p["event"][phase]["pt"] = [{"rank": 2, "value": 31}, {"rank": 7, "value": 100}]
        seq_p["event"][phase]["rewards"] = [
            {"rank": r, "count": n, "resource_type": 1, "resource_id": 43, "probability_bp": 5000}
            for r, n in ((2, 13), (7, 43))
        ]
    seq_p["event"]["normal"]["cp"] = [{"rank": 2, "value": 3}, {"rank": 7, "value": 10}]
    seq_p["event"]["assumptions"]["cp_bonus_source"] = "none"
    for obj in ("mean", "score"):
        seq_res = run("event", seq_p, objective=obj, method="exact", time_limit=0)
        assert seq_res["optimality_certified"]
        for phase in ("normal", "challenge"):
            assert seq_res[phase + "_search_audit"]["score_bound_enabled"] is True
            if obj == "mean":
                expected_seq = set()
                for m_pair, s_pair, sc in seq_records:
                    pt_b = (
                        sum(
                            seq_p["catalog"]["members"][m - 1].get("event_pt_bonus_bp", 0)
                            for m in m_pair
                        )
                        / 10000
                    )
                    dr_b = (
                        sum(
                            seq_p["catalog"]["snapshots"][s - 1].get("event_drop_bonus_bp", 0)
                            for s in s_pair
                        )
                        / 10000
                    )
                    mult = 5 if phase == "normal" else 1
                    ys = []
                    for s_val in sc:
                        rk = 7 if s_val >= mid_th else 2
                        cp_v = math.floor(mult * (10 if rk == 7 else 3)) if phase == "normal" else 0
                        pt_v = math.floor(mult * (100 if rk == 7 else 31) * (1 + pt_b))
                        sh_v = math.floor(mult * (43 if rk == 7 else 13) * (1 + dr_b)) * 0.5
                        ys.append((cp_v, pt_v, sh_v))
                    expected_seq.add(tuple(round(sum(col) / len(ys), 9) for col in zip(*ys)))
                expected_seq = frontier(expected_seq)
                observed_seq = {
                    (
                        round(c["yield"]["cp"], 9),
                        round(c["yield"]["pt"], 9),
                        round(c["yield"]["shop_currency_expected"], 9),
                    )
                    for c in seq_res[phase + "_frontier"]
                }
                assert observed_seq == expected_seq, (phase, observed_seq, expected_seq)

    # rank_lower == rank_upper shortcut must report true order statistics (worst < mean < best).
    same_rank = copy.deepcopy(seq_p)
    same_rank["constraints"].update(
        required_members=list(members), required_snapshots=list(snapshots)
    )
    same_rank["event"]["score_ranks"] = [
        {"rank": 2, "required_score": 0},
        {"rank": 7, "required_score": max(all_seq_scores) + 1000000},
    ]
    sr_res = run("event", same_rank, objective="mean", method="exact", time_limit=0)
    assert sr_res["normal_search_audit"]["score_bound_enabled"] is True
    cand = sr_res["normal_frontier"][0]
    oa = cand["order_analysis"]
    assert oa["worst_score"] < oa["mean_score"] < oa["best_score"], oa

    # Negative live_boost + non-monotonic rank rewards must not be falsely pruned by native_min_score.
    neg_p = copy.deepcopy(seq_p)
    neg_p["settings"]["judgement"] = {"mode": "all_perfect"}
    neg_p["catalog"]["live_skills"] = [
        {"id": 1, "effects": [{"level": 5, "effect_type": 2000, "value": 5000}]},
        {"id": 2, "effects": [{"level": 5, "effect_type": 2000, "value": -5000}]},
    ]
    neg_p["catalog"]["members"][0]["live_skill"] = 1
    neg_p["catalog"]["members"][1]["live_skill"] = 1
    neg_p["catalog"]["members"][2]["live_skill"] = 2
    neg_p["catalog"]["members"][2]["trained"] = list(neg_p["catalog"]["members"][1]["trained"])
    neg_records = []
    for m_pair in ((1, 2), (1, 3)):
        for s_pair in itertools.permutations(range(1, 4), 2):
            f = {
                "schema": "ournotes-deck-formation@1",
                "leader": 1,
                "slots": [
                    {"member": m, "snapshot": s, "trigger": i + 1}
                    for i, (m, s) in enumerate(zip(m_pair, s_pair))
                ],
            }
            sc = []
            for tr in ((1, 2), (2, 1)):
                for sl, t in zip(f["slots"], tr):
                    sl["trigger"] = t
                sc.append(
                    run("score", neg_p, f, order_search="given")["results"][0]["estimated_score"][
                        "total"
                    ]
                )
            neg_records.append((m_pair, s_pair, sc))
    # Pick a threshold strictly below [1,2]'s scores and above [1,3]'s minimum score.
    s_12_min = min(s for mp, _, sc in neg_records if mp == (1, 2) for s in sc)
    s_13_min = min(s for mp, _, sc in neg_records if mp == (1, 3) for s in sc)
    assert s_13_min < s_12_min
    neg_th = (s_13_min + s_12_min) / 2.0
    neg_p["event"]["score_ranks"] = [
        {"rank": 2, "required_score": 0},
        {"rank": 7, "required_score": neg_th},
    ]
    # Non-monotonic rewards: rank 2 pays much more CP/pt/shop than rank 7!
    neg_p["event"]["normal"]["cp"] = [{"rank": 2, "value": 100}, {"rank": 7, "value": 10}]
    for phase in ("normal", "challenge"):
        neg_p["event"][phase]["pt"] = [{"rank": 2, "value": 100}, {"rank": 7, "value": 10}]
        neg_p["event"][phase]["rewards"] = [
            {"rank": r, "count": n, "resource_type": 1, "resource_id": 43, "probability_bp": 10000}
            for r, n in ((2, 100), (7, 10))
        ]
    neg_res = run("event", neg_p, objective="mean", method="exact", time_limit=0)
    assert neg_res["optimality_certified"]
    assert not neg_res["normal_search_audit"]["score_bound_enabled"]
    # Member 3 (negative live boost) MUST appear on the optimal frontier since it hits rank 2.
    assert any(3 in c["members"] for c in neg_res["normal_frontier"]), neg_res["normal_frontier"]

    print(
        "Mean event rewards passed independent exhaustive frontier checks, cross-rank averaging and nonmonotonic rewards"
    )


if __name__ == "__main__":
    main()
