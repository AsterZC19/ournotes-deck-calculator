#!/usr/bin/env python3
"""Comprehensive test suite for event finite budget and boost_budget extensions."""

import copy
import json
from pathlib import Path
import subprocess
import tempfile

from test_event import BINARY, problem, run


def main():
    base = problem()
    base["constraints"] = {"required_members": [1], "required_snapshots": [1]}
    base["event"]["normal_boosts"].append({"cost": 3, "pt_rate": 15, "reward_rate": 15})

    # 1. Test boost_budget calculation and unused_boost
    p1 = copy.deepcopy(base)
    p1["event"]["assumptions"].pop("normal_runs", None)
    p1["event"]["assumptions"]["boost_budget"] = 302
    p1["event"]["assumptions"]["normal_boost_cost"] = 3
    p1["event"]["assumptions"]["initial_cp"] = 100
    res1 = run("event", p1, method="fast", time_limit=0.1)

    assert "recommended_finite_budget" in res1, "Must contain recommended_finite_budget"
    assert res1.get("unused_boost") == 2, f"Expected unused_boost=2, got {res1.get('unused_boost')}"
    normal_assumptions = res1["normal_assumptions"]
    assert normal_assumptions.get("boost_budget") == 302
    assert normal_assumptions.get("unused_boost") == 2
    assert normal_assumptions.get("normal_runs") == 100
    assert "tradeoff_summary" in res1["recommended_finite_budget"], "Must contain tradeoff_summary"
    tradeoff = res1["recommended_finite_budget"]["tradeoff_summary"]
    assert "event_points_lost_with_shop_plan" in tradeoff
    assert "event_items_lost_with_pt_plan" in tradeoff
    assert tradeoff["event_points_lost_with_shop_plan"] >= 0
    assert tradeoff["event_items_lost_with_pt_plan"] >= 0

    # 2. Test conflict: both boost_budget and normal_runs
    p_conflict = copy.deepcopy(base)
    p_conflict["event"]["assumptions"]["normal_runs"] = 50
    p_conflict["event"]["assumptions"]["boost_budget"] = 150
    p_conflict["event"]["assumptions"]["normal_boost_cost"] = 3
    try:
        run("event", p_conflict, method="fast", time_limit=0.1)
        assert False, "Should have thrown on both boost_budget and normal_runs"
    except subprocess.CalledProcessError as exc:
        assert "不能同时指定" in exc.stderr or "SpecError" in exc.stderr

    # 3. Test cost=0 rejection (division by zero)
    p_zero_cost = copy.deepcopy(base)
    p_zero_cost["event"]["assumptions"].pop("normal_runs", None)
    p_zero_cost["event"]["assumptions"]["boost_budget"] = 100
    p_zero_cost["event"]["assumptions"]["normal_boost_cost"] = 0
    try:
        run("event", p_zero_cost, method="fast", time_limit=0.1)
        assert False, "Should have thrown on normal_boost_cost=0"
    except subprocess.CalledProcessError as exc:
        assert "不能为零" in exc.stderr or "正整数" in exc.stderr or "SpecError" in exc.stderr

    # 4. Test invalid boost_budget: negative or non-integer
    for bad_budget in (-10, 15.5):
        p_bad = copy.deepcopy(base)
        p_bad["event"]["assumptions"].pop("normal_runs", None)
        p_bad["event"]["assumptions"]["boost_budget"] = bad_budget
        p_bad["event"]["assumptions"]["normal_boost_cost"] = 3
        try:
            run("event", p_bad, method="fast", time_limit=0.1)
            assert False, f"Should have thrown on bad budget: {bad_budget}"
        except subprocess.CalledProcessError:
            pass

    # 5. Test zero budget: boost_budget = 0
    p_zero_budget = copy.deepcopy(base)
    p_zero_budget["event"]["assumptions"].pop("normal_runs", None)
    p_zero_budget["event"]["assumptions"]["boost_budget"] = 0
    p_zero_budget["event"]["assumptions"]["normal_boost_cost"] = 3
    p_zero_budget["event"]["assumptions"]["initial_cp"] = 450
    p_zero_budget["event"]["assumptions"]["challenge_cp_cost"] = 200
    res_zero = run("event", p_zero_budget, method="fast", time_limit=0.1)
    assert res_zero.get("unused_boost") == 0
    assert res_zero["normal_assumptions"]["normal_runs"] == 0
    finite_plans = res_zero["finite_budget"]
    for plan in finite_plans:
        assert plan["challenge_runs"] == 2.0
        assert plan["remaining_cp"] == 50.0

    # 6. Test initial CP exceeding cost with exact remainder hand-calculation
    p_large_cp = copy.deepcopy(base)
    p_large_cp["event"]["assumptions"].pop("normal_runs", None)
    p_large_cp["event"]["assumptions"]["normal_runs"] = 1
    p_large_cp["event"]["assumptions"]["initial_cp"] = 650
    p_large_cp["event"]["assumptions"]["challenge_cp_cost"] = 200
    res_large = run("event", p_large_cp, method="fast", time_limit=0.1)
    # 1 normal run with candidate cp yield + 650 initial cp -> plays = floor((650 + cp) / 200)
    for plan in res_large["finite_budget"]:
        normal_idx = plan["normal_candidate"]
        cand_cp = res_large["normal_frontier"][normal_idx]["yield"]["cp"]
        total_cp = 650 + cand_cp
        expected_plays = total_cp // 200
        expected_rem = total_cp % 200
        assert plan["challenge_runs"] == expected_plays
        assert plan["remaining_cp"] == expected_rem

    # 7. Test cross-song batch event recommendation with finite budget
    song1 = copy.deepcopy(base)
    song1["song"]["id"] = 101
    song1["event"]["assumptions"].pop("normal_runs", None)
    song1["event"]["assumptions"]["boost_budget"] = 300
    song1["event"]["assumptions"]["normal_boost_cost"] = 3

    song2 = copy.deepcopy(base)
    song2["song"]["id"] = 102
    song2["event"]["assumptions"].pop("normal_runs", None)
    song2["event"]["assumptions"]["boost_budget"] = 300
    song2["event"]["assumptions"]["normal_boost_cost"] = 3
    # Make song2 have lower card bonuses so song1 dominates song2
    song2["catalog"]["snapshots"][0]["event_bonus_bp"] = 0

    with tempfile.TemporaryDirectory() as folder:
        p1_path = Path(folder) / "s1.json"
        p2_path = Path(folder) / "s2.json"
        p1_path.write_text(json.dumps(song1))
        p2_path.write_text(json.dumps(song2))
        cmd = [
            str(BINARY),
            "event",
            "--experimental",
            "-p",
            str(p1_path),
            "-p",
            str(p2_path),
            "--method",
            "fast",
            "--time-limit",
            "0.1",
            "--quiet",
        ]
        completed = subprocess.run(cmd, text=True, capture_output=True, check=True)
        batch_out = json.loads(completed.stdout)
        assert batch_out["schema"] == "ournotes-event-batch@1"
        assert "recommended_finite_budget" in batch_out
        batch_rec = batch_out["recommended_finite_budget"]
        assert "pt" in batch_rec and "shop_currency_expected" in batch_rec
        assert "song_id" in batch_rec["pt"]
        assert "plan_index" in batch_rec["pt"]
        assert "tradeoff_summary" in batch_rec
        tradeoff = batch_rec["tradeoff_summary"]
        assert tradeoff["event_points_lost_with_shop_plan"] >= 0
        assert tradeoff["event_items_lost_with_pt_plan"] >= 0

    # 8. Test non-divisible boost budget: 7 boost with cost 3 gives 2 runs, unused 1
    p_non_div = copy.deepcopy(base)
    p_non_div["event"]["assumptions"].pop("normal_runs", None)
    p_non_div["event"]["assumptions"]["boost_budget"] = 7
    p_non_div["event"]["assumptions"]["normal_boost_cost"] = 3
    res_non_div = run("event", p_non_div, method="fast", time_limit=0.1)
    assert res_non_div["normal_assumptions"]["boost_budget"] == 7
    assert res_non_div["normal_assumptions"]["normal_runs"] == 2
    assert res_non_div["unused_boost"] == 1
    assert res_non_div["normal_assumptions"]["unused_boost"] == 1

    # 9. Test order-invariant tie-breaking between identical songs (101 vs 102)
    song_a = copy.deepcopy(base)
    song_a["song"]["id"] = 101
    song_a["event"]["assumptions"].pop("normal_runs", None)
    song_a["event"]["assumptions"]["boost_budget"] = 150
    song_a["event"]["assumptions"]["normal_boost_cost"] = 3

    song_b = copy.deepcopy(base)
    song_b["song"]["id"] = 102
    song_b["event"]["assumptions"].pop("normal_runs", None)
    song_b["event"]["assumptions"]["boost_budget"] = 150
    song_b["event"]["assumptions"]["normal_boost_cost"] = 3

    with tempfile.TemporaryDirectory() as folder:
        pa_path = Path(folder) / "sa.json"
        pb_path = Path(folder) / "sb.json"
        pa_path.write_text(json.dumps(song_a))
        pb_path.write_text(json.dumps(song_b))

        # Order 1: [101, 102]
        cmd1 = [
            str(BINARY),
            "event",
            "--experimental",
            "-p",
            str(pa_path),
            "-p",
            str(pb_path),
            "--method",
            "fast",
            "--time-limit",
            "0.1",
            "--quiet",
        ]
        out1 = json.loads(subprocess.run(cmd1, text=True, capture_output=True, check=True).stdout)
        winner1 = out1["recommended_finite_budget"]["pt"]["song_id"]

        # Order 2: [102, 101]
        cmd2 = [
            str(BINARY),
            "event",
            "--experimental",
            "-p",
            str(pb_path),
            "-p",
            str(pa_path),
            "--method",
            "fast",
            "--time-limit",
            "0.1",
            "--quiet",
        ]
        out2 = json.loads(subprocess.run(cmd2, text=True, capture_output=True, check=True).stdout)
        winner2 = out2["recommended_finite_budget"]["pt"]["song_id"]

        assert winner1 == 101, f"Expected winner 101, got {winner1}"
        assert (
            winner2 == 101
        ), f"Expected winner 101 in reverse order, got {winner2} (tie-breaker failed)"

    # 10. Test mixed CP distribution (50 CP prob 0.5, 150 CP prob 0.5, 2 runs, initial 100, cost 200 -> 1.25 runs, 50 rem)
    p_dist = copy.deepcopy(base)
    p_dist["constraints"] = {"required_members": [1, 2], "required_snapshots": [1, 2]}
    p_dist["event"]["assumptions"].pop("normal_runs", None)
    p_dist["event"]["assumptions"]["normal_runs"] = 2
    p_dist["event"]["assumptions"]["initial_cp"] = 100
    p_dist["event"]["assumptions"]["challenge_cp_cost"] = 200
    p_dist["event"]["assumptions"]["score_rank_mode"] = "estimated_score"
    p_dist["event"]["score_ranks"] = [
        {"rank": 2, "required_score": 0},
        {"rank": 3, "required_score": 7500},
    ]
    p_dist["event"]["normal"]["cp"] = [
        {"rank": 2, "value": 10},  # normal_boost_cost 1, boost rate 5 -> 10 * 5 = 50 CP
        {"rank": 3, "value": 30},  # 30 * 5 = 150 CP
    ]
    p_dist["event"]["normal"]["pt"] = [
        {"rank": 2, "value": 100},
        {"rank": 3, "value": 100},
    ]
    res_dist = run("event", p_dist, method="exact", objective="mean_score")
    assert len(res_dist["finite_budget"]) >= 1, "Must produce finite_budget plan"
    plan_dist = res_dist["finite_budget"][0]
    assert (
        abs(plan_dist["challenge_runs"] - 1.25) < 1e-9
    ), f"Expected challenge_runs 1.25, got {plan_dist['challenge_runs']}"
    assert (
        abs(plan_dist["remaining_cp"] - 50.0) < 1e-9
    ), f"Expected remaining_cp 50.0, got {plan_dist['remaining_cp']}"
    assert "remaining_cp_distribution" in plan_dist, "Must contain remaining_cp_distribution"
    dist_map = {row["cp"]: row["probability"] for row in plan_dist["remaining_cp_distribution"]}
    assert abs(dist_map.get(0, 0) - 0.5) < 1e-6, f"Expected prob 0.5 for 0 rem CP, got {dist_map}"
    assert (
        abs(dist_map.get(100, 0) - 0.5) < 1e-6
    ), f"Expected prob 0.5 for 100 rem CP, got {dist_map}"

    # 11. Test short-budget vs long-budget song selection reversal
    # Song A: High normal PT (500), 0 CP
    song_rev_a = copy.deepcopy(base)
    song_rev_a["song"]["id"] = 101
    song_rev_a["event"]["normal"]["pt"] = [{"rank": 7, "value": 100}]  # 100 * 5 = 500 PT
    song_rev_a["event"]["normal"]["cp"] = [{"rank": 7, "value": 0}]
    song_rev_a["event"]["challenge"]["pt"] = [{"rank": 7, "value": 10000}]

    # Song B: Low normal PT (100), high CP (20 * 5 = 100 CP)
    song_rev_b = copy.deepcopy(base)
    song_rev_b["song"]["id"] = 102
    song_rev_b["event"]["normal"]["pt"] = [{"rank": 7, "value": 20}]  # 20 * 5 = 100 PT
    song_rev_b["event"]["normal"]["cp"] = [{"rank": 7, "value": 20}]  # 20 * 5 = 100 CP
    song_rev_b["event"]["challenge"]["pt"] = [{"rank": 7, "value": 10000}]

    with tempfile.TemporaryDirectory() as td:
        pa = Path(td) / "rev_a.json"
        pb = Path(td) / "rev_b.json"

        # Short budget (runs=1, cost=200): Song A wins (500 PT > 100 PT)
        sa_short = copy.deepcopy(song_rev_a)
        sb_short = copy.deepcopy(song_rev_b)
        sa_short["event"]["assumptions"].pop("normal_runs", None)
        sa_short["event"]["assumptions"]["normal_runs"] = 1
        sa_short["event"]["assumptions"]["initial_cp"] = 0
        sa_short["event"]["assumptions"]["challenge_cp_cost"] = 200
        sb_short["event"]["assumptions"].pop("normal_runs", None)
        sb_short["event"]["assumptions"]["normal_runs"] = 1
        sb_short["event"]["assumptions"]["initial_cp"] = 0
        sb_short["event"]["assumptions"]["challenge_cp_cost"] = 200
        pa.write_text(json.dumps(sa_short))
        pb.write_text(json.dumps(sb_short))
        cmd_short = [
            str(BINARY),
            "event",
            "--experimental",
            "-p",
            str(pa),
            "-p",
            str(pb),
            "--method",
            "fast",
            "--quiet",
        ]
        out_short = json.loads(
            subprocess.run(cmd_short, capture_output=True, text=True, check=True).stdout
        )
        winner_short = out_short["recommended_finite_budget"]["pt"]["song_id"]
        assert winner_short == 101, f"Short budget should prefer Song 101, got {winner_short}"

        # Long budget (runs=20, cost=200): Song B converts 2000 CP to 10 challenge plays, beating Song A
        sa_long = copy.deepcopy(song_rev_a)
        sb_long = copy.deepcopy(song_rev_b)
        sa_long["event"]["assumptions"].pop("normal_runs", None)
        sa_long["event"]["assumptions"]["normal_runs"] = 20
        sa_long["event"]["assumptions"]["initial_cp"] = 0
        sa_long["event"]["assumptions"]["challenge_cp_cost"] = 200
        sb_long["event"]["assumptions"].pop("normal_runs", None)
        sb_long["event"]["assumptions"]["normal_runs"] = 20
        sb_long["event"]["assumptions"]["initial_cp"] = 0
        sb_long["event"]["assumptions"]["challenge_cp_cost"] = 200
        pa.write_text(json.dumps(sa_long))
        pb.write_text(json.dumps(sb_long))
        cmd_long = [
            str(BINARY),
            "event",
            "--experimental",
            "-p",
            str(pa),
            "-p",
            str(pb),
            "--method",
            "fast",
            "--quiet",
        ]
        out_long = json.loads(
            subprocess.run(cmd_long, capture_output=True, text=True, check=True).stdout
        )
        winner_long = out_long["recommended_finite_budget"]["pt"]["song_id"]
        assert winner_long == 102, f"Long budget should prefer Song 102, got {winner_long}"

    print("All event finite budget and boost_budget tests passed successfully!")


if __name__ == "__main__":
    main()
