#!/usr/bin/env python3
"""Exercise the resumable proof-chain tool against an independent small oracle."""

import json
import subprocess
import tempfile
from pathlib import Path
from test_theoretical import prepared, oracle

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = prepared(3)
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        source = root / "problem.json"
        source.write_text(json.dumps(p))
        out = root / "proof"
        cmd = [
            "python3",
            str(ROOT / "tools/prove_top_k.py"),
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
            assert proof["audit"]["excluded_member_sets"] == excluded
            excluded.append(sorted(r["members"]))
        assert (out / "top10-certified.md").exists()
        times = [(out / f"proof-{i:02d}.json").stat().st_mtime_ns for i in range(1, 4)]
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        assert times == [(out / f"proof-{i:02d}.json").stat().st_mtime_ns for i in range(1, 4)]
        p["chart"]["level"] = 28
        source.write_text(json.dumps(p))
        wrong = subprocess.run(cmd, capture_output=True, text=True)
        assert wrong.returncode and "another problem" in wrong.stderr
    print(
        "Top-K certificate tool: independent ranks, witness replay, resume and input hash checks passed"
    )


if __name__ == "__main__":
    main()
