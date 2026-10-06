"""Verify real progression, saved decks, and per-character power in both engine paths."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.account import AccountError, apply_account_catalog, formation_for_deck, validate_account
from tools.compile_from_master import _load_chart_runtime
from test_search import fixture, run, expected


def account():
    return {
        "schema": "ournotes-account@1",
        "region": "jp",
        "profile": {"profile_id": "123"},
        "members": [{"master_id": 1, "exp": 10, "rank": 1, "awake": 1, "live_skill_level": 2}],
        "snapshots": [{"master_id": 11, "exp": 0, "rank": 1}],
        "characters": [{"character_id": 1, "exp": 1}],
        "band_items": [{"master_id": 101, "level": 2}],
        "vip_points": 100,
        "main_deck": 7,
        "decks": [],
    }


class Tables:
    def __init__(self):
        rates = {f"_{d}Rate": 5000 for d in ("performance", "technic", "visual")}
        base = {f"_{d}PowerMax": 101 for d in ("performance", "technic", "visual")}
        self.data = {
            "MasterMemberCard": [
                {
                    "_id": 1,
                    **base,
                    "_memberCardLevelGroup": 1,
                    "_memberCardRankGroup": 1,
                    "_memberCardAwakeGroup": 1,
                }
            ],
            "MasterMemberCardLevel": [
                {"_group": 1, "_level": 1, "_exp": 0, **rates},
                {"_group": 1, "_level": 2, "_exp": 10, **{k: 6000 for k in rates}},
            ],
            "MasterMemberCardRank": [
                {
                    "_group": 1,
                    "_rank": 1,
                    **rates,
                    "_leaderSkillLevel": 1,
                    "_musicTypeBonusRate": 0,
                    "_musicTagBonusRate": 0,
                }
            ],
            "MasterMemberCardAwake": [{"_group": 1, "_awakeCount": 1, **rates}],
            "MasterSupportCard": [
                {
                    "_id": 11,
                    **base,
                    "_supportCardLevelGroup": 1,
                    "_supportCardRankGroup": 1,
                    "_supportSkillId01": 10,
                    "_supportSkillId02": 20,
                }
            ],
            "MasterSupportCardLevel": [{"_group": 1, "_level": 1, "_exp": 0, **rates}],
            "MasterSupportCardRank": [
                {
                    "_group": 1,
                    "_rank": 1,
                    "_limitLevel": 30,
                    "_supportSkill01Level": 1,
                    "_supportSkill02Level": 2,
                    "_cardTypeLinkBonusRate": 0,
                }
            ],
            "MasterCharacter": [{"_id": i, "_bandID": i} for i in (1, 2)],
            "MasterCharacterRank": [
                {"_rank": 1, "_exp": 0, "_bonus": 0},
                {"_rank": 2, "_exp": 1, "_bonus": 5},
            ],
            "MasterCharacterTotalRank": [
                {"_totalRank": 2, "_bonus": 0},
                {"_totalRank": 3, "_bonus": 10},
            ],
            "MasterVip": [{"_point": 0, "_vipRank": 1}, {"_point": 100, "_vipRank": 2}],
            "MasterVipRankBonus": [{"_vipRank": 2, "_vipBonusType": 7, "_value": 100}],
            "MasterBandItem": [{"_id": 101}],
            "MasterBandItemSkillEffect": [
                {
                    "_bandItemId": 101,
                    "_level": 2,
                    "_skillEffectType": 1000,
                    "_skillTargetIDs": [1],
                    "_effectValue": 20,
                }
            ],
            "MasterSkillTarget": [{"_id": 1, "_skillTargetType": 3, "_bandID": 1}],
        }

    def table(self, name):
        return self.data[name]


def catalog():
    return {
        "members": [{"id": i, "trained": [9999] * 3, "card_rank_bonus_bp": {}} for i in (1, 2)],
        "snapshots": [{"id": i, "trained": [9999] * 3, "card_rank_bonus_bp": {}} for i in (11, 12)],
        "fix": {"vip_bonus_bp": [9999] * 3},
    }


class AccountTests(unittest.TestCase):
    def test_actual_progression_and_bonuses_replace_ideal_values(self):
        c = catalog()
        metadata = apply_account_catalog(Tables(), c, account())
        self.assertEqual([r["id"] for r in c["members"]], [1])
        self.assertEqual(c["members"][0]["trained"], [160] * 3)  # 60 + 50 + 50, floor each term
        self.assertEqual(c["members"][0]["level"], 2)
        self.assertEqual(c["members"][0]["live_skill_level"], 2)
        self.assertEqual(c["members"][0]["leader_skill_level"], 1)
        self.assertEqual(c["snapshots"][0]["trained"], [50] * 3)
        self.assertEqual(c["snapshots"][0]["support_skill_levels"], {"10": 1, "20": 2})
        self.assertEqual(
            c["fix"]["character_rank_bonus_by_character"], {"1": [5] * 3, "2": [0] * 3}
        )
        self.assertEqual(c["fix"]["character_total_rank_bonus"], [10] * 3)
        self.assertEqual(c["fix"]["band_item_bonus_bp_by_band"], {"1": [20] * 3, "2": [0] * 3})
        self.assertEqual(c["fix"]["vip_bonus_bp"], [100] * 3)
        self.assertEqual(metadata["character_total_rank"], 3)

    def test_missing_ownership_and_invalid_progression_fail(self):
        for field, value in (("master_id", 999), ("rank", 5), ("exp", -1), ("awake", True)):
            a = account()
            a["members"][0][field] = value
            with self.assertRaises(AccountError):
                apply_account_catalog(Tables(), catalog(), a)
        a = account()
        a["members"].append(copy.deepcopy(a["members"][0]))
        with self.assertRaises(AccountError):
            validate_account(a)
        with self.assertRaises(AccountError):
            validate_account({"player_id": "private", "credential": "secret"})

    def test_saved_deck_maps_zero_based_game_trigger_order(self):
        a = account()
        a["members"] = [{**a["members"][0], "master_id": i + 1} for i in range(5)]
        a["snapshots"] = [{**a["snapshots"][0], "master_id": i + 11} for i in range(5)]
        a["decks"] = [
            {
                "id": 7,
                "slots": [
                    {"index": i, "member_id": i + 1, "snapshot_id": i + 11, "trigger_index": 4 - i}
                    for i in range(5)
                ],
            }
        ]
        f = formation_for_deck(a)
        self.assertEqual(f["leader"], 3)
        self.assertEqual([s["trigger"] for s in f["slots"]], [5, 4, 3, 2, 1])
        a["decks"][0]["slots"].reverse()
        self.assertEqual(formation_for_deck(a), f)
        a["decks"][0]["slots"][0]["member_id"] = 0
        with self.assertRaises(AccountError):
            formation_for_deck(a)
        with self.assertRaises(AccountError):
            formation_for_deck(a, 8)

    def test_converted_charts_do_not_need_raw_converter(self):
        with tempfile.TemporaryDirectory() as folder:
            Path(folder, "chart.json").write_text(
                json.dumps({"format": "nnnotes.live-score/1", "notes": []})
            )
            self.assertEqual(_load_chart_runtime(Path(folder), "chart")["notes"], [])

    def test_character_bonus_matches_independent_search_and_detailed_score(self):
        p = fixture(42)
        bonuses = {1: 5, 2: 25, 3: 45}  # character 4 uses the legacy fallback
        p["catalog"]["fix"] = {
            "character_rank_bonus": [99, 0, 0],
            "character_total_rank_bonus": [3, 0, 0],
            "character_rank_bonus_by_character": {str(k): [v, 0, 0] for k, v in bonuses.items()},
        }
        independent = copy.deepcopy(p)
        for member in independent["catalog"]["members"]:
            member["trained"][0] += bonuses.get(member["character"], 99) + 3
        result = run(p)
        self.assertAlmostEqual(result["results"][0]["index"], expected(independent, 40), places=8)
        best = result["results"][0]
        formation = {
            "schema": "ournotes-deck-formation@1",
            "leader": best["leader"],
            "slots": [
                {"member": s["member"], "snapshot": s["snapshot"], "trigger": s["trigger"]}
                for s in best["assignments"]
            ],
        }
        with tempfile.TemporaryDirectory() as folder:
            pp, ff = Path(folder, "p.json"), Path(folder, "f.json")
            pp.write_text(json.dumps(p))
            ff.write_text(json.dumps(formation))
            scored = subprocess.run(
                [
                    str(ROOT / "build/deckcalc"),
                    "score",
                    "--experimental",
                    "-p",
                    str(pp),
                    "-f",
                    str(ff),
                    "--detail",
                ],
                capture_output=True,
                text=True,
                check=True,
            )
            score = json.loads(scored.stdout)["results"][0]
        self.assertEqual(score["power"], best["power"])
        self.assertAlmostEqual(score["index"], best["index"], places=8)


if __name__ == "__main__":
    unittest.main()
