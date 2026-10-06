#!/usr/bin/env python3
import copy
import json
from pathlib import Path
import subprocess
import tempfile

from test_event import BINARY, problem, run


def main():
    base = problem()
    inputs = [base]
    for field in ("trained", "event_bonus_bp", "card_type"):
        changed = copy.deepcopy(base)
        changed["song"]["id"] += len(inputs)
        changed["catalog"]["snapshots"][0][field] = [3000] * 3 if field == "trained" else 2000
        inputs.append(changed)
    changed = copy.deepcopy(base)
    changed["song"]["id"] = 5
    changed["catalog"]["fix"] = {"band_item_bonus_bp": [1000] * 3}
    inputs.append(changed)
    changed = copy.deepcopy(base)
    changed["song"]["id"] = 6
    changed["constraints"] = {"required_members": [2], "required_snapshots": [3]}
    inputs.append(changed)
    changed = copy.deepcopy(base)
    changed["song"]["id"] = 8
    changed["catalog"]["live_skills"][0]["effects"][0]["value"] = 80000
    inputs.append(changed)
    changed = copy.deepcopy(base)
    changed["song"]["id"] = 9
    changed["catalog"]["support_skills"] = [
        {"id": 7, "effects": [{"level": 5, "effect_type": 15000, "value": 150}]}
    ]
    changed["catalog"]["snapshots"][0]["support_skills"] = [7]
    inputs.append(changed)
    repeated = copy.deepcopy(base)
    repeated["song"]["id"] = 7
    inputs.append(repeated)
    for method, rank_mode in (("exact", "fixed"), ("fast", "fixed"), ("exact", "estimated_score")):
        for p in inputs + [base]:
            p["event"]["assumptions"]["score_rank_mode"] = rank_mode
            p["event"]["score_ranks"] = [
                {"rank": 2, "required_score": 0},
                {"rank": 7, "required_score": 15000},
            ]
        flags = dict(method=method, time_limit=0 if method == "exact" else 0.1, leaders="1")
        with tempfile.TemporaryDirectory() as folder:
            paths = []
            for i, p in enumerate(inputs + [base]):
                path = Path(folder) / f"{i}.json"
                path.write_text(json.dumps(p))
                paths.append(path)
            args = [str(BINARY), "event", "--quiet", "--challenge-problem", str(paths[-1])]
            for path in paths[:-1]:
                args += ["-p", str(path)]
            for flag, value in flags.items():
                args += ["--" + flag.replace("_", "-"), str(value)]
            result = subprocess.run(args, text=True, capture_output=True, check=True)
            batch = json.loads(result.stdout)
            assert batch["schema"] == "ournotes-event-batch@1"
            assert len(batch["plans"]) == len(inputs)
            assert batch["engine_cache"]["power_matrix_hits"] > 0
            for i, (p, plan) in enumerate(zip(inputs, batch["plans"])):
                assert plan["normal_song_id"] == p["song"]["id"]
                assert plan["optimality_certified"] == (method == "exact")
                assert plan["challenge_search_audit"]["phase_cache_hit"] == (
                    method == "exact" and i > 0
                )
                single = run("event", p, challenge=base, **flags)
                for phase in ("normal", "challenge"):
                    yields = lambda doc: sorted(
                        tuple(c["yield"].values()) for c in doc[phase + "_frontier"]
                    )
                    assert yields(plan) == yields(single)
                    for c in plan[phase + "_frontier"]:
                        replay = run(
                            "score",
                            p if phase == "normal" else base,
                            c["formation"],
                            order_search="given",
                        )["results"][0]
                        assert replay["power"] == c["power"]
                        assert replay["estimated_score"] == c["estimated_score"]
            assert batch["search_cache"]["phase_hits"] == (
                len(inputs) - 1 if method == "exact" else 0
            )
            bad = subprocess.run(
                [str(BINARY), "rank", "-p", str(paths[0]), "-p", str(paths[1])],
                text=True,
                capture_output=True,
            )
            assert bad.returncode == 2 and "多个 --problem" in bad.stderr
    print("Event batch checks passed: cache reuse, invalidation, witnesses and method scopes")


if __name__ == "__main__":
    main()
