#!/usr/bin/env python3
"""Check large catalogs against independent exhaustive small-domain oracles."""

import copy

from test_search import fixture, expected, run as run_index
from test_theoretical import prepared, oracle, run as run_score
from test_event import problem as event_problem, run as run_event


def expand(p, count):
    """Place useful cards across old word boundaries, padding with zero-value cards."""
    p = copy.deepcopy(p)
    catalog = p["catalog"]
    members, snapshots = catalog["members"], catalog["snapshots"]
    positions = [0, 1, 63, 64] if count == 65 else [0, 63, 64, count - 1]
    assert len(members) <= len(positions) and len(snapshots) <= len(positions)
    for i, member in enumerate(members):
        member["character"] = 1000000000 + 64 * i
    catalog["live_skills"].append({"id": 999, "effects": []})
    catalog["members"] = [
        dict(
            members[0],
            id=10000 + i,
            character=100000 + i if count == 65 else members[-1]["character"],
            trained=[0, 0, 0],
            live_skill=999,
            event_pt_bonus_bp=0,
            event_drop_bonus_bp=0,
        )
        for i in range(count)
    ]
    catalog["snapshots"] = [
        dict(
            snapshots[0],
            id=10000 + i,
            trained=[0, 0, 0],
            support_skills=[],
            event_pt_bonus_bp=0,
            event_drop_bonus_bp=0,
        )
        for i in range(count)
    ]
    for i, member in enumerate(members):
        catalog["members"][positions[i]] = member
    for i, snapshot in enumerate(snapshots):
        catalog["snapshots"][positions[i]] = snapshot
    return p


def check_result(p, result):
    row = result["results"][0]
    members = {m["id"]: m for m in p["catalog"]["members"]}
    assert len(set(row["members"])) == p["settings"]["team_size"]
    if p["constraints"].get("distinct_characters", True):
        assert len({members[m]["character"] for m in row["members"]}) == len(row["members"])
    photos = [slot["snapshot"] for slot in row["assignments"]]
    if p["constraints"]["distinct_snapshots"]:
        assert len(set(photos)) == len(photos)
    assert set(p["constraints"].get("required_members", [])) <= set(row["members"])
    assert set(p["constraints"].get("required_snapshots", [])) <= set(photos)


def main():
    for count in (65, 129, 257):
        for reuse in (False, True):
            small = fixture(3, reuse)
            p = expand(small, count)
            exact = run_index(p, seconds=0)
            check_result(p, exact)
            assert exact["audit"]["certified"]
            assert abs(exact["results"][0]["index"] - expected(small, 40)) < 1e-8
            if count == 129 and not reuse:
                fast = run_index(p, method="fast", seconds=0)
                check_result(p, fast)
                assert fast["audit"]["anneal_proposals"] > 0
                assert fast["results"][0]["index"] <= expected(small, 40) + 1e-8

    small = prepared(3)
    p = expand(small, 129)
    score = run_score(p, seconds=0)
    check_result(p, score)
    assert score["audit"]["theoretical_max_certified"]
    assert score["results"][0]["ranking_score"] == oracle(small)["score"]
    excluded = [score["results"][0]["members"]]
    second = run_score(p, seconds=0, excluded=excluded)
    check_result(p, second)
    assert second["audit"]["theoretical_max_certified"]
    assert set(second["results"][0]["members"]) != set(excluded[0])
    assert second["results"][0]["ranking_score"] == oracle(small, excluded=excluded)["score"]

    small = event_problem()
    p = expand(small, 65)
    # Require the useful cards/photos to keep this event proof tiny while its
    # rank-seed and score-ceiling searches still see the full selected catalog.
    for model in (small, p):
        model.setdefault("constraints", {"distinct_characters": True, "distinct_snapshots": True})
        model["constraints"]["required_members"] = [1, 3]
        model["constraints"]["required_snapshots"] = [2, 3]
        model["settings"]["score_model"] = {"level_alpha": 0.005, "rounding": "float32_floor"}
    flags = dict(method="exact", time_limit=0, leaders="1")
    baseline = run_event("event", small, **flags)
    large = run_event("event", p, **flags)
    assert large["optimality_certified"]
    for objective in ("pt", "shop_currency_expected"):
        assert large["recommended"][objective] == baseline["recommended"][objective]
    print(
        "Large-pool checks passed: 65/129/257 cards, sparse characters, reuse, fast/exact, score/exclusions and event search"
    )


if __name__ == "__main__":
    main()
