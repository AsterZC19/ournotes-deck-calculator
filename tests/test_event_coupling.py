#!/usr/bin/env python3
import copy
import random

from test_event import problem, run
from test_event_bounds import enumerate_scores, yield_for


def main():
    rng = random.Random(650021)
    bounded = pruned = 0
    for case in range(8):
        p = problem()
        p["settings"].update(
            team_size=3,
            base_duration_ms=300,
            score_model={"level_alpha": 0.005, "rounding": "float32_floor"},
        )
        p["chart"].update(
            level=25,
            notes=[{"t": i * 100, "op": 1 if i % 3 else 2} for i in range(22)],
            skill_times_ms=[0, 700, 1400],
        )
        p["constraints"] = {"distinct_characters": True, "distinct_snapshots": case % 2 == 0}
        p["catalog"]["snapshots"].append(dict(copy.deepcopy(p["catalog"]["snapshots"][-1]), id=4))
        p["catalog"]["support_skills"] = []
        for m in p["catalog"]["members"]:
            m.update(character=m["id"] + 64, trained=[rng.randrange(10000, 100000)] * 3)
        for s in p["catalog"]["snapshots"]:
            s.update(trained=[rng.randrange(1000, 10000)] * 3, support_skills=[s["id"]])
            p["catalog"]["support_skills"].append(
                {
                    "id": s["id"],
                    "effects": [
                        {"level": 5, "effect_type": 15000, "value": rng.randrange(5) * 100}
                    ],
                }
            )
        for card in p["catalog"]["members"] + p["catalog"]["snapshots"]:
            card["event_pt_bonus_bp"] = rng.randrange(8) * 1179 + case * 0.1
            card["event_drop_bonus_bp"] = rng.randrange(8) * 1913 + case * 0.2
        records = enumerate_scores(p)
        scores = sorted(score for _, _, score in records)
        p["event"]["score_ranks"] = [
            {"rank": 2, "required_score": 0},
            {"rank": 4, "required_score": scores[len(scores) // 2]},
            {"rank": 7, "required_score": scores[-1] + case % 2},
        ]
        p["event"]["assumptions"].update(
            score_rank_mode="estimated_score", cp_bonus_source=("none", "pt", "drop")[case % 3]
        )
        for phase in ("normal", "challenge"):
            p["event"][phase]["pt"] = [
                {"rank": rank, "value": rng.randrange(1, 200)} for rank in (2, 4, 7)
            ]
            p["event"][phase]["rewards"] = [
                {
                    "rank": rank,
                    "resource_type": 1,
                    "resource_id": 43,
                    "count": rng.randrange(1, 200),
                    "probability_bp": 5000,
                }
                for rank in (2, 4, 7)
            ]
        p["event"]["normal"]["cp"] = [
            {"rank": rank, "value": rng.randrange(1, 20)} for rank in (2, 4, 7)
        ]
        actual = run("event", p, method="exact", leaders="1", time_limit=0)
        assert actual["optimality_certified"]
        for phase in ("normal", "challenge"):
            values = {yield_for(p, f, score, phase) for f, _, score in records}
            expected = {
                v
                for v in values
                if not any(o != v and all(a >= b for a, b in zip(o, v)) for o in values)
            }
            observed = {
                tuple(c["yield"][key] for key in ("cp", "pt", "shop_currency_expected"))
                for c in actual[phase + "_frontier"]
            }
            assert observed == expected, (case, phase, observed, expected)
            audit = actual[phase + "_search_audit"]
            bounded += audit["coupled_bound_nodes"]
            pruned += audit["coupled_pruned_branches"]
    assert bounded > 0 and pruned > 0, (bounded, pruned)
    print(f"Coupled bounds passed: 8 exhaustive cases, {bounded} checks, {pruned} branches pruned")


if __name__ == "__main__":
    main()
