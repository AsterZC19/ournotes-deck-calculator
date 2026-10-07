#!/usr/bin/env python3
"""Compare coordinated neighborhood moves and exact-search fallback with an oracle."""

import copy

from test_game_rules import invoke
from test_theoretical import oracle, prepared


def rank(p, objective, method, top=1):
    result, data = invoke(
        p,
        "rank",
        experimental=True,
        flags=(
            "--objective",
            objective,
            "--method",
            method,
            "--leaders",
            "40",
            "--time-limit",
            "0",
            "--top",
            str(top),
            "--beam-width",
            "1",
            "--restarts",
            "0",
        ),
    )
    assert result.returncode == 0, result.stderr
    assert data["audit"]["dfs_branch_order"] == "power_and_skill_potential"
    return data


def main():
    for objective, key in (("mean", "mean_score"), ("score", "score")):
        improvements = 0
        for seed in range(8):
            p = prepared(seed)
            data = rank(p, objective, "fast")
            audit = data["audit"]
            candidate = data["results"][0]
            assert not audit["certified"] and audit["lns_proposals"] > 0
            assert audit["lns_evaluations"] + audit["lns_bound_pruned"] > 0
            assert candidate["ranking_score"] == oracle(p)[key]
            assert 10 in candidate["members"]
            assignments = candidate["assignments"]
            assert 2 in {s["snapshot"] for s in assignments}
            assert len({s["snapshot"] for s in assignments}) == len(assignments)
            improvements += audit["lns_improvements"]
        assert improvements > 0

    p = prepared(0, small=True)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    for i in range(4, 8):
        member = copy.deepcopy(p["catalog"]["members"][0])
        member.update(id=10 * (i + 1), character=i + 1)
        p["catalog"]["members"].append(member)
    for member in p["catalog"]["members"]:
        member["trained"][0] = 1
    for snapshot in p["catalog"]["snapshots"]:
        snapshot["trained"][0] = 0
    for skill in p["catalog"]["leader_skills"]:
        skill["effects"][0]["value"] = 0
    for skill in p["catalog"]["live_skills"]:
        skill["effects"][0]["value"] = 1000
    expected = oracle(p)
    for objective, key in (("mean", "mean_score"), ("score", "score")):
        data = rank(p, objective, "exact", top=2)
        assert data["audit"]["certified"] and data["audit"]["lns_proposals"] > 0
        assert data["results"][0]["ranking_score"] == expected[key]
        exact_top1 = rank(p, objective, "exact", top=1)
        assert exact_top1["audit"]["certified"]
        assert exact_top1["results"][0]["ranking_score"] == expected[key]

    infeasible = prepared(1)
    for member in infeasible["catalog"]["members"]:
        member["character"] = 1
    for objective in ("mean", "score"):
        data = rank(infeasible, objective, "exact")
        assert not data["results"] and data["audit"]["infeasible_proven"]
    print("Neighborhood search passed: 16 fast-score oracles, required cards and exact fallback")


if __name__ == "__main__":
    main()
