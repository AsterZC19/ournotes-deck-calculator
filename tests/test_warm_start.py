#!/usr/bin/env python3
"""Validate prior formations as incumbents without trusting supplied scores."""

import copy
import json
import subprocess
import tempfile
from pathlib import Path
from test_theoretical import prepared, oracle

ROOT = Path(__file__).resolve().parents[1]


def calculate(p, warm=None, excluded=(), method="exact", top=1):
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        problem = root / "p.json"
        problem.write_text(json.dumps(p))
        cmd = [
            str(ROOT / "build/deckcalc"),
            "rank",
            "--experimental",
            "-p",
            str(problem),
            "--method",
            method,
            "--time-limit",
            "0",
            "--top",
            str(top),
            "--quiet",
            "--restarts",
            "2",
            "--anneal-steps",
            "1000",
        ]
        if warm is not None:
            f = root / "seeds.json"
            f.write_text(json.dumps(warm))
            cmd += ["--warm-start", str(f)]
        if excluded:
            e = root / "excluded.json"
            e.write_text(json.dumps(excluded))
            cmd += ["--exclude-member-sets", str(e)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r, json.loads(r.stdout) if r.returncode == 0 else None


def main():
    p = prepared(11)
    r, original = calculate(p)
    assert r.returncode == 0
    witness = copy.deepcopy(original["results"][0])
    witness["ranking_score"] = 1e100
    witness["estimated_score"]["total"] = 1e100
    _, seeded = calculate(p, {"results": [witness]}, top=10)
    assert seeded["audit"]["accepted_seed_formations"] == 1
    assert seeded["audit"]["warm_start_strategy"] == "validated_prior_formations"
    assert seeded["audit"]["anneal_proposals"] == 0 and seeded["audit"]["leaders_tried"] == 0
    assert (
        seeded["audit"]["global_incumbent_pruning"] and seeded["audit"]["theoretical_max_certified"]
    )
    expected = max(oracle(p, m["id"])["score"] for m in p["catalog"]["members"])
    assert seeded["results"][0]["ranking_score"] == expected

    formation = {
        "leader": witness["leader"],
        "slots": [
            {k: s[k] for k in ("member", "snapshot", "trigger")} for s in witness["assignments"]
        ],
    }
    _, array = calculate(p, [formation])
    assert array["results"][0]["ranking_score"] == expected

    reordered = copy.deepcopy(formation)
    reordered["slots"].reverse()
    _, repeated = calculate(p, [formation, reordered])
    assert repeated["audit"]["accepted_seed_formations"] == 2
    assert repeated["audit"]["score_cache_hits"] > 0
    assert repeated["results"][0]["ranking_score"] == expected

    forced = copy.deepcopy(p)
    forced["constraints"]["required_members"] = [s["member"] for s in formation["slots"]]
    cards = [
        m
        for m in forced["catalog"]["members"]
        if m["id"] in forced["constraints"]["required_members"]
    ]
    skills = {s["id"]: s["effects"][0]["value"] for s in forced["catalog"]["leader_skills"]}
    ordered = sorted(cards, key=lambda m: skills[m["leader_skill"]])
    leaders = [ordered[0]["id"], ordered[-1]["id"]]
    forced["constraints"]["leader_pool"] = leaders
    seeds = [dict(formation, leader=leader) for leader in leaders]
    _, leader_cache = calculate(forced, seeds)
    assert leader_cache["audit"]["accepted_seed_formations"] == 2
    assert leader_cache["results"][0]["ranking_score"] == max(
        oracle(forced, leader)["score"] for leader in leaders
    )

    excluded = [sorted(witness["members"])]
    _, next_rank = calculate(p, {"results": [witness]}, excluded)
    assert (
        next_rank["audit"]["accepted_seed_formations"] == 0
        and next_rank["audit"]["excluded_seed_formations"] == 1
    )
    assert next_rank["audit"]["warm_start_strategy"] == "none"
    assert next_rank["audit"]["solver"] == "snapshot-classes-matching-dfs"
    expected_next = max(oracle(p, m["id"], excluded)["score"] for m in p["catalog"]["members"])
    assert next_rank["results"][0]["ranking_score"] == expected_next

    _, fast = calculate(p, {"results": [witness]}, method="fast", top=10)
    assert (
        fast["results"][0]["ranking_score"] == expected
        and not fast["audit"]["theoretical_max_certified"]
    )

    bad = copy.deepcopy(witness)
    bad["assignments"][0]["trigger"] = 4294967297
    result, _ = calculate(p, {"results": [bad]})
    assert result.returncode and "越界" in result.stderr
    bad = copy.deepcopy(formation)
    bad["slots"][0]["member"] = 999
    result, _ = calculate(p, [bad])
    assert result.returncode

    bad = copy.deepcopy(p)
    bad["catalog"]["live_skills"][0]["effects"][0]["value"] = -20000
    result, _ = calculate(bad)
    assert result.returncode and "非负" in result.stderr
    bad = copy.deepcopy(p)
    for row in bad["catalog"]["leader_skills"]:
        row["effects"][0]["value"] = -100000
    result, _ = calculate(bad)
    assert result.returncode and "综合力非负" in result.stderr
    print(
        "Warm-start checks: forged scores, exclusions, fast/exact units, invalid inputs and numerical scope passed"
    )


if __name__ == "__main__":
    main()
