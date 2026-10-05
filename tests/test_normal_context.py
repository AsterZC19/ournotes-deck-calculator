import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import compile_from_master as compiler


class Tables:
    def __init__(self, *args):
        pass

    def require(self, *args):
        pass

    def table(self, name):
        return [{"_id": 7}] if name == "MasterLiveMusic" else []


class NormalContextTests(unittest.TestCase):
    def test_normal_live_excludes_challenge_power_but_keeps_event_yields(self):
        def catalog(*args, **kwargs):
            return {
                "members": [{"id": 1, "trained": [100, 100, 100]}],
                "snapshots": [{"id": 2, "trained": [500, 500, 500]}],
            }, {}

        def event(tables, cards, *args):
            for kind in ("members", "snapshots"):
                for card in cards[kind]:
                    card.update(
                        event_bonus_bp=5000, event_pt_bonus_bp=3000, event_drop_bonus_bp=4000
                    )
            return {"id": 1, "normal": {"cp": [{"rank": 7, "value": 10}]}}

        def song(*args):
            return {"id": 7, "title": "song", "type": 2, "bands": [3], "tags": [3], "chart": {}}

        with (
            patch.object(compiler, "MasterTables", Tables),
            patch.object(compiler, "_load_texts", return_value={}),
            patch.object(compiler, "build_catalog", side_effect=catalog),
            patch.object(compiler, "_songs_for_event", return_value={7: [{}]}),
            patch.object(compiler, "build_song", side_effect=song),
            patch.object(compiler, "build_event", side_effect=event),
        ):
            normal = compiler.compile_problem(
                Path("master"),
                Path("charts"),
                1,
                "expert",
                7,
                song_context="normal",
                validate=False,
            )
            challenge = compiler.compile_problem(
                Path("master"),
                Path("charts"),
                1,
                "expert",
                7,
                song_context="challenge",
                validate=False,
            )
        for kind in ("members", "snapshots"):
            n, c = normal["catalog"][kind][0], challenge["catalog"][kind][0]
            self.assertEqual(n["event_bonus_bp"], 0)
            self.assertEqual(c["event_bonus_bp"], 5000)
            self.assertEqual(n["trained"], c["trained"])
            self.assertEqual(n["event_pt_bonus_bp"], 3000)
            self.assertEqual(n["event_drop_bonus_bp"], 4000)
        self.assertEqual(normal["event"], challenge["event"])
        self.assertEqual(normal["song"], challenge["song"])


if __name__ == "__main__":
    unittest.main()
