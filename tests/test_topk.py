#!/usr/bin/env python3
"""Exercise Top-K search and proof tool against independent exhaustive oracles."""

import copy
import itertools
import json
import subprocess
import tempfile
from pathlib import Path
from test_theoretical import prepared, oracle
from test_game_rules import game_problem

ROOT = Path(__file__).resolve().parents[1]


def run_rank(p, flags):
    with tempfile.NamedTemporaryFile("w", suffix=".json") as f:
        json.dump(p, f)
        f.flush()
        cmd = [
            str(ROOT / "build/deckcalc"),
            "rank",
            "--experimental",
            "-p",
            f.name,
        ] + flags
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(res.stderr)
        return json.loads(res.stdout)


def topk_oracle(p, k=3, objective="score", allowed_leaders=None, excluded=()):
    c = p["catalog"]
    team_size = p["settings"]["team_size"]
    all_combos = [
        tuple(sorted(m["id"] for m in combo))
        for combo in itertools.combinations(c["members"], team_size)
    ]
    req_m = set(p["constraints"].get("required_members", []))
    ex_sets = {tuple(sorted(x)) for x in excluded}
    valid_combos = [cb for cb in all_combos if req_m <= set(cb) and cb not in ex_sets]

    if allowed_leaders is None:
        allowed_leaders = [m["id"] for m in c["members"]]

    best_per_combo = []
    for combo in valid_combos:
        best_score = -1
        best_leader = -1
        other_combos = [cb for cb in all_combos if cb != combo]
        for leader_id in combo:
            if leader_id not in allowed_leaders:
                continue
            res = oracle(p, leader_id=leader_id, excluded=other_combos)
            s = res[objective if objective == "score" else "mean_score"]
            if s > best_score:
                best_score = s
                best_leader = leader_id
        if best_score >= 0:
            best_per_combo.append((best_score, best_leader, list(combo)))

    best_per_combo.sort(key=lambda x: x[0], reverse=True)
    return best_per_combo[:k]


def test_multiple_leaders():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    expected = topk_oracle(p, k=3, objective="score")
    actual = run_rank(p, ["--top", "3", "--objective", "score", "--method", "exact", "--quiet"])
    assert len(actual["results"]) == 3
    for exp, act in zip(expected, actual["results"]):
        assert act["ranking_score"] == exp[0]
        assert act["leader"] == exp[1]
        assert sorted(act["members"]) == exp[2]


def test_varying_snapshot_durations():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    for i, snap in enumerate(p["catalog"]["snapshots"]):
        snap["support_skills"] = [i + 1]
    for i, sk in enumerate(p["catalog"]["support_skills"]):
        sk["effects"] = [{"level": 5, "effect_type": 15000, "value": i * 250}]
    expected = topk_oracle(p, k=3, objective="score")
    actual = run_rank(p, ["--top", "3", "--objective", "score", "--method", "exact", "--quiet"])
    for exp, act in zip(expected, actual["results"]):
        assert act["ranking_score"] == exp[0]
        assert sorted(act["members"]) == exp[2]


def test_repeated_colliding_resources():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    dup = copy.deepcopy(p["catalog"]["snapshots"][0])
    dup["id"] = 99
    p["catalog"]["snapshots"].append(dup)
    expected = topk_oracle(p, k=3, objective="score")
    actual = run_rank(p, ["--top", "3", "--objective", "score", "--method", "exact", "--quiet"])
    for exp, act in zip(expected, actual["results"]):
        assert act["ranking_score"] == exp[0]
        assert sorted(act["members"]) == exp[2]


def test_required_members_snapshots():
    p = prepared(3)
    p["constraints"]["required_members"] = [10]
    p["constraints"]["required_snapshots"] = [2]
    expected = topk_oracle(p, k=2, objective="score")
    actual = run_rank(p, ["--top", "2", "--objective", "score", "--method", "exact", "--quiet"])
    for exp, act in zip(expected, actual["results"]):
        assert act["ranking_score"] == exp[0]
        assert sorted(act["members"]) == exp[2]
        assert 10 in act["members"]
        assert 2 in {s["snapshot"] for s in act["assignments"]}


def test_excluded_sets():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    with tempfile.TemporaryDirectory() as folder:
        ex_file = Path(folder) / "excluded.json"
        ex_file.write_text(json.dumps([[10, 20, 30]]))
        expected = topk_oracle(p, k=2, objective="score", excluded=[[10, 20, 30]])
        actual = run_rank(
            p,
            [
                "--top",
                "2",
                "--objective",
                "score",
                "--method",
                "exact",
                "--exclude-member-sets",
                str(ex_file),
                "--quiet",
            ],
        )
        assert len(actual["results"]) == 2
        for exp, act in zip(expected, actual["results"]):
            assert act["ranking_score"] == exp[0]
            assert sorted(act["members"]) == exp[2]
            assert sorted(act["members"]) != [10, 20, 30]


def test_warm_starts():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    with tempfile.TemporaryDirectory() as folder:
        base = run_rank(p, ["--top", "1", "--method", "exact", "--quiet"])
        winner = base["results"][0]
        true_score = winner["ranking_score"]
        sf = Path(folder) / "seeds.json"
        sf.write_text(json.dumps({"results": [winner]}))
        seeded = run_rank(
            p,
            [
                "--top",
                "3",
                "--method",
                "exact",
                "--warm-start",
                str(sf),
                "--quiet",
            ],
        )
        assert seeded["audit"]["accepted_seed_formations"] == 1
        assert seeded["results"][0]["ranking_score"] == true_score
        forged = copy.deepcopy(winner)
        forged["ranking_score"] = 99999999.0
        forged["estimated_score"]["total"] = 99999999.0
        sf.write_text(json.dumps({"results": [forged]}))
        rechecked = run_rank(
            p,
            [
                "--top",
                "3",
                "--method",
                "exact",
                "--warm-start",
                str(sf),
                "--quiet",
            ],
        )
        assert rechecked["audit"]["accepted_seed_formations"] == 1
        assert rechecked["results"][0]["ranking_score"] == true_score


def test_mean_and_score():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    exp_score = topk_oracle(p, k=3, objective="score")
    act_score = run_rank(p, ["--top", "3", "--objective", "score", "--method", "exact", "--quiet"])
    for exp, act in zip(exp_score, act_score["results"]):
        assert act["ranking_score"] == exp[0]
        assert sorted(act["members"]) == exp[2]
    exp_mean = topk_oracle(p, k=3, objective="mean")
    act_mean = run_rank(p, ["--top", "3", "--objective", "mean", "--method", "exact", "--quiet"])
    for exp, act in zip(exp_mean, act_mean["results"]):
        assert abs(act["ranking_score"] - exp[0]) < 1e-6
        assert sorted(act["members"]) == exp[2]


def test_equal_score_ties():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    p["catalog"]["members"][1]["trained"] = [100, 0, 0]
    p["catalog"]["members"][2]["trained"] = [100, 0, 0]
    p["catalog"]["members"][1]["live_skill"] = 1
    p["catalog"]["members"][2]["live_skill"] = 1
    p["catalog"]["members"][1]["leader_skill"] = 1
    p["catalog"]["members"][2]["leader_skill"] = 1
    actual = run_rank(p, ["--top", "4", "--method", "exact", "--quiet"])
    r0, r1 = actual["results"][0], actual["results"][1]
    assert r0["ranking_score"] == r1["ranking_score"]
    assert r0["power"] == r1["power"]
    assert r0["leader"] == r1["leader"]
    assert r0["members"] < r1["members"]


def test_k_larger_than_feasible():
    p = prepared(3)
    p["constraints"]["required_members"] = [10, 20]
    p["constraints"]["required_snapshots"] = []
    actual = run_rank(p, ["--top", "5", "--method", "exact", "--quiet"])
    assert len(actual["results"]) == 2
    audit = actual["audit"]
    assert audit["top_k_exhausted"] and audit["top_k_complete"] and audit["top_k_certified"]
    assert audit["returned_count"] == 2 and audit["requested_count"] == 5

    proved = run_rank(p, ["--prove-top", "5", "--quiet"])
    assert len(proved["results"]) == 2
    p_audit = proved["audit"]
    assert p_audit["top_k_exhausted"] and p_audit["top_k_complete"] and p_audit["top_k_certified"]
    assert p_audit["returned_count"] == 2 and p_audit["requested_count"] == 5

    auto = run_rank(p, ["--prove-top", "5", "--method", "auto", "--quiet"])
    assert auto["audit"]["top_k_certified"]
    for count in ("0", "-1"):
        bad = subprocess.run(
            [str(ROOT / "build/deckcalc"), "rank", "--prove-top", count],
            capture_output=True,
            text=True,
        )
        assert bad.returncode and "--prove-top 必须为正数" in bad.stderr


def test_budget_expiry():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    for i in range(4, 15):
        m = copy.deepcopy(p["catalog"]["members"][0])
        m["id"] = 10 * (i + 1)
        m["character"] = i + 1
        p["catalog"]["members"].append(m)
    actual = run_rank(
        p,
        [
            "--top",
            "5",
            "--method",
            "exact",
            "--time-limit",
            "0.00001",
            "--quiet",
        ],
    )
    audit = actual["audit"]
    assert not audit["top_k_complete"]
    assert not audit["top_k_certified"]


def test_unsupported_fallback():
    p = prepared(3)
    p["constraints"]["distinct_snapshots"] = False
    actual = run_rank(p, ["--top", "2", "--method", "exact", "--quiet"])
    assert actual["audit"]["solver"] == "beam-annealing-lns-dp-dfs"
    assert len(actual["results"]) == 2


def test_default_auto_large_pool():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    for i in range(4, 35):
        m = copy.deepcopy(p["catalog"]["members"][0])
        m["id"] = 10 * (i + 1)
        m["character"] = i + 1
        p["catalog"]["members"].append(m)
    for i in range(4, 30):
        s = copy.deepcopy(p["catalog"]["snapshots"][0])
        s["id"] = i + 1
        p["catalog"]["snapshots"].append(s)
    actual = run_rank(p, ["--quiet"])
    assert actual["audit"]["solver"] == "snapshot-classes-matching-dfs"
    assert actual["audit"]["top_k_certified"]


def test_explicit_fast_preservation():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    for i in range(4, 35):
        m = copy.deepcopy(p["catalog"]["members"][0])
        m["id"] = 10 * (i + 1)
        m["character"] = i + 1
        p["catalog"]["members"].append(m)
    for i in range(4, 30):
        s = copy.deepcopy(p["catalog"]["snapshots"][0])
        s["id"] = i + 1
        p["catalog"]["snapshots"].append(s)
    actual = run_rank(p, ["--method", "fast", "--quiet"])
    assert actual["audit"]["solver"] == "beam-annealing-lns-dp"
    assert not actual["audit"]["certified"]


def test_proof_tool():
    p = prepared(3)
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        source = root / "problem.json"
        source.write_text(json.dumps(p))
        out = root / "proof"
        cmd = [
            "python3",
            str(ROOT / "tools/prove_top_k.py"),
            "--experimental",
            "-p",
            str(source),
            "-o",
            str(out),
            "--count",
            "3",
        ]
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        d = json.loads((out / "topk-certified.json").read_text())
        assert d["complete"] and len(d["results"]) == 3
        excluded = []
        for r, proof in zip(d["results"], d["proofs"]):
            maximum = max(oracle(p, m["id"], excluded)["score"] for m in p["catalog"]["members"])
            assert r["ranking_score"] == maximum
            assert proof["audit"]["theoretical_max_certified"] and proof["witness_replayed"]
            assert proof["audit"]["top_k_certified"]
            excluded.append(sorted(r["members"]))
        assert (out / "top10-certified.md").exists()
        stamp = (out / "proof-topk.json").stat().st_mtime_ns
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        assert (out / "proof-topk.json").stat().st_mtime_ns == stamp
        changed_count = cmd[:-1] + ["2"]
        wrong = subprocess.run(changed_count, capture_output=True, text=True)
        assert wrong.returncode and "configuration changed" in wrong.stderr
        wrong = subprocess.run(cmd + ["--objective", "mean"], capture_output=True, text=True)
        assert wrong.returncode and "configuration changed" in wrong.stderr
        proof_path = out / "proof-topk.json"
        saved_proof = proof_path.read_bytes()
        proof_path.write_bytes(saved_proof + b"\n")
        wrong = subprocess.run(cmd, capture_output=True, text=True)
        assert wrong.returncode and "Search proof changed" in wrong.stderr
        proof_path.write_bytes(saved_proof)
        proof_path.rename(out / "saved-proof.json")
        wrong = subprocess.run(cmd, capture_output=True, text=True)
        assert wrong.returncode and "Legacy proof chain" in wrong.stderr
        (out / "saved-proof.json").rename(proof_path)
        p["chart"]["level"] = 28
        source.write_text(json.dumps(p))
        wrong = subprocess.run(cmd, capture_output=True, text=True)
        assert wrong.returncode and "another problem" in wrong.stderr



def test_class_search_no_conflict():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    for i, m in enumerate(p["catalog"]["members"]):
        m["card_type"] = i + 1
    for i, s in enumerate(p["catalog"]["snapshots"]):
        s["card_type"] = i + 1
        s["trained"] = [1000 * (i + 1), 0, 0]
    expected = topk_oracle(p, k=3, objective="score")
    actual = run_rank(p, ["--top", "3", "--objective", "score", "--method", "exact", "--quiet"])
    assert len(actual["results"]) == 3
    for exp, act in zip(expected, actual["results"]):
        assert act["ranking_score"] == exp[0]
        assert sorted(act["members"]) == exp[2]
    assert actual["audit"]["top_k_certified"]


def test_class_search_collision_fallback():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    p["catalog"]["snapshots"][0]["trained"] = [10000, 0, 0]
    p["catalog"]["snapshots"][1]["trained"] = [0, 0, 0]
    p["catalog"]["snapshots"][2]["trained"] = [0, 0, 0]
    expected = topk_oracle(p, k=3, objective="score")
    actual = run_rank(p, ["--top", "3", "--objective", "score", "--method", "exact", "--quiet"])
    assert len(actual["results"]) == 3
    for exp, act in zip(expected, actual["results"]):
        assert act["ranking_score"] == exp[0]
        assert sorted(act["members"]) == exp[2]
    assert actual["audit"]["top_k_certified"]


def test_class_search_tied_maximum():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    p["catalog"]["snapshots"][0]["trained"] = [5000, 0, 0]
    p["catalog"]["snapshots"][1]["trained"] = [5000, 0, 0]
    p["catalog"]["snapshots"][2]["trained"] = [100, 0, 0]
    expected = topk_oracle(p, k=3, objective="score")
    actual = run_rank(p, ["--top", "3", "--objective", "score", "--method", "exact", "--quiet"])
    assert len(actual["results"]) == 3
    for exp, act in zip(expected, actual["results"]):
        assert act["ranking_score"] == exp[0]
        assert sorted(act["members"]) == exp[2]
    assert actual["audit"]["top_k_certified"]


def test_class_search_required_snapshot():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = [p["catalog"]["snapshots"][2]["id"]]
    expected = topk_oracle(p, k=2, objective="score")
    actual = run_rank(p, ["--top", "2", "--objective", "score", "--method", "exact", "--quiet"])
    assert len(actual["results"]) == 2
    for exp, act in zip(expected, actual["results"]):
        assert act["ranking_score"] == exp[0]
        assert sorted(act["members"]) == exp[2]
        assert 3 in {s["snapshot"] for s in act["assignments"]}
    assert actual["audit"]["top_k_certified"]


def test_class_search_infeasible():
    p = prepared(3)
    p["constraints"]["required_snapshots"] = [1, 2, 3, 4]
    actual = run_rank(p, ["--top", "1", "--method", "exact", "--quiet"])
    assert len(actual["results"]) == 0
    assert actual["audit"]["infeasible_proven"]


def test_class_search_formation_leader():
    p = game_problem()
    actual = run_rank(p, ["--top", "1", "--objective", "score", "--method", "exact", "--quiet"])
    assert len(actual["results"]) == 1
    winner = actual["results"][0]
    assert winner["leader"] == 1
    assert sorted(winner["members"]) == [1, 2, 3, 4, 5]
    assert actual["audit"]["top_k_certified"]
    p2 = copy.deepcopy(p)
    p2["constraints"]["required_members"] = [6]
    actual2 = run_rank(p2, ["--top", "1", "--objective", "score", "--method", "exact", "--quiet"])
    assert actual2["results"][0]["ranking_score"] < winner["ranking_score"]


def test_class_search_topk_mean_and_score():
    p = prepared(3)
    p["constraints"]["required_members"] = []
    p["constraints"]["required_snapshots"] = []
    for i, s in enumerate(p["catalog"]["snapshots"]):
        s["support_skills"] = [i + 1]
    for i, sk in enumerate(p["catalog"]["support_skills"]):
        sk["effects"] = [{"level": 5, "effect_type": 15000, "value": i * 250}]
    exp_s = topk_oracle(p, k=3, objective="score")
    act_s = run_rank(p, ["--top", "3", "--objective", "score", "--method", "exact", "--quiet"])
    for exp, act in zip(exp_s, act_s["results"]):
        assert act["ranking_score"] == exp[0]
        assert sorted(act["members"]) == exp[2]
    exp_m = topk_oracle(p, k=3, objective="mean")
    act_m = run_rank(p, ["--top", "3", "--objective", "mean", "--method", "exact", "--quiet"])
    for exp, act in zip(exp_m, act_m["results"]):
        assert abs(act["ranking_score"] - exp[0]) < 1e-6
        assert sorted(act["members"]) == exp[2]


def main():
    test_multiple_leaders()
    test_varying_snapshot_durations()
    test_repeated_colliding_resources()
    test_required_members_snapshots()
    test_excluded_sets()
    test_warm_starts()
    test_mean_and_score()
    test_equal_score_ties()
    test_k_larger_than_feasible()
    test_budget_expiry()
    test_unsupported_fallback()
    test_default_auto_large_pool()
    test_explicit_fast_preservation()
    test_proof_tool()
    test_class_search_no_conflict()
    test_class_search_collision_fallback()
    test_class_search_tied_maximum()
    test_class_search_required_snapshot()
    test_class_search_infeasible()
    test_class_search_formation_leader()
    test_class_search_topk_mean_and_score()
    print("Top-K exhaustive oracle checks passed: 21 scenarios verified")


if __name__ == "__main__":
    main()

