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
from tools.compile_from_master import _load_chart_runtime, build_fix, compile_problem
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

    def _minimal_master_tables(self, folder: Path, *, band2_max_level: int = 30) -> None:
        rates = {"_performanceRate": 10000, "_technicRate": 10000, "_visualRate": 10000}
        tables = {
            "MasterCharacter": [
                {
                    "_id": i,
                    "_bandID": 1 if i <= 4 else 2,
                    "_nameTextID": f"c{i}",
                    "_isNonPlayable": False,
                }
                for i in range(1, 6)
            ]
            + [{"_id": 1001, "_bandID": 2, "_nameTextID": "npc", "_isNonPlayable": True}],
            "MasterCharacterRank": [{"_id": 1, "_rank": 50, "_exp": 0, "_bonus": 200}],
            "MasterCharacterTotalRank": [
                {"_id": 1, "_totalRank": 250, "_bonus": 90},
                {"_id": 2, "_totalRank": 300, "_bonus": 95},
            ],
            "MasterVip": [{"_id": 1, "_vipRank": 1, "_point": 0}],
            "MasterVipRankBonus": [{"_id": 1, "_vipRank": 1, "_vipBonusType": 7, "_value": 500}],
            "MasterBandItem": [
                {"_id": 101, "_bandId": 1},
                {"_id": 201, "_bandId": 2},
            ],
            "MasterBandItemLevel": [
                {"_id": 101030, "_bandItemId": 101, "_level": 30, "_playerRank": 1},
                {"_id": 201030, "_bandItemId": 201, "_level": band2_max_level, "_playerRank": 1},
            ],
            "MasterBandItemSkillEffect": [
                {
                    "_id": 1,
                    "_bandItemId": 101,
                    "_level": 30,
                    "_skillEffectType": 1000,
                    "_effectValue": 300,
                    "_skillTargetIDs": [3],
                },
                {
                    "_id": 2,
                    "_bandItemId": 101,
                    "_level": 50,
                    "_skillEffectType": 1000,
                    "_effectValue": 500,
                    "_skillTargetIDs": [3],
                },
                {
                    "_id": 3,
                    "_bandItemId": 201,
                    "_level": 20,
                    "_skillEffectType": 1000,
                    "_effectValue": 200,
                    "_skillTargetIDs": [4],
                },
                {
                    "_id": 4,
                    "_bandItemId": 201,
                    "_level": 30,
                    "_skillEffectType": 1000,
                    "_effectValue": 300,
                    "_skillTargetIDs": [4],
                },
                {
                    "_id": 5,
                    "_bandItemId": 201,
                    "_level": 50,
                    "_skillEffectType": 1000,
                    "_effectValue": 500,
                    "_skillTargetIDs": [4],
                },
            ],
            "MasterMemberCard": [
                {
                    "_id": i,
                    "_characterID": i if i <= 5 else 5,
                    "_cardType": 1,
                    "_rarity": 5,
                    "_nameTextID": f"m{i}",
                    "_performancePowerMax": 1000,
                    "_technicPowerMax": 1000,
                    "_visualPowerMax": 1000,
                    "_memberCardLevelGroup": 1,
                    "_memberCardRankGroup": 1,
                    "_memberCardAwakeGroup": 1,
                    "_liveSkillID": 1,
                    "_leaderSkillID": 61 if i == 1 else 1,
                    "_gekisouSkillID": 0,
                    "_bestMusicTagIDs": [],
                }
                for i in range(1, 7)
            ],
            "MasterMemberCardLevel": [{"_id": 1, "_group": 1, "_level": 10, "_exp": 0, **rates}],
            "MasterMemberCardRank": [
                {
                    "_id": 1,
                    "_group": 1,
                    "_rank": 1,
                    "_liveSkillLevel": 5,
                    "_leaderSkillLevel": 5,
                    "_musicTypeBonusRate": 0,
                    "_musicTagBonusRate": 0,
                    **rates,
                }
            ],
            "MasterMemberCardAwake": [{"_id": 1, "_group": 1, "_awakeCount": 1, **rates}],
            "MasterSupportCard": [
                {
                    "_id": i,
                    "_characterIDs": [1],
                    "_cardType": 1,
                    "_rarity": 5,
                    "_nameTextID": f"s{i}",
                    "_performancePowerMax": 500,
                    "_technicPowerMax": 500,
                    "_visualPowerMax": 500,
                    "_supportCardLevelGroup": 1,
                    "_supportCardRankGroup": 1,
                    "_supportSkillId01": 1,
                    "_supportSkillId02": 0,
                }
                for i in range(1, 6)
            ],
            "MasterSupportCardLevel": [{"_id": 1, "_group": 1, "_level": 10, "_exp": 0, **rates}],
            "MasterSupportCardRank": [
                {
                    "_id": 1,
                    "_group": 1,
                    "_rank": 1,
                    "_limitLevel": 10,
                    "_supportSkill01Level": 5,
                    "_supportSkill02Level": 1,
                    "_cardTypeLinkBonusRate": 0,
                }
            ],
            "MasterLiveSkill": [{"_id": 1, "_skillCategories": []}],
            "MasterLiveSkillEffect": [
                {
                    "_id": 1,
                    "_liveSkillID": 1,
                    "_level": 5,
                    "_skillEffectType": 2000,
                    "_effectValue": 10000,
                }
            ],
            "MasterLeaderSkill": [{"_id": 1}, {"_id": 61}, {"_id": 90123}],
            "MasterLeaderSkillEffect": [
                {
                    "_id": 1,
                    "_leaderSkillID": 1,
                    "_level": 5,
                    "_skillEffectType": 1000,
                    "_effectValue": 3000,
                    "_skillTargetIDs": [3],
                    "_skillConditionGroup": 0,
                    "_skillCumulativeConditionID": 0,
                },
                {
                    "_id": 2,
                    "_leaderSkillID": 61,
                    "_level": 5,
                    "_skillEffectType": 1001,
                    "_effectValue": 10200,
                    "_skillTargetIDs": [3],
                    "_skillConditionGroup": 0,
                    "_skillCumulativeConditionID": 0,
                },
                {
                    "_id": 3,
                    "_leaderSkillID": 61,
                    "_level": 5,
                    "_skillEffectType": 1501,
                    "_effectValue": 2400,
                    "_skillTargetIDs": [],
                    "_skillConditionGroup": 415,
                    "_skillCumulativeConditionID": 70,
                },
                {
                    "_id": 4,
                    "_leaderSkillID": 90123,
                    "_level": 5,
                    "_skillEffectType": 0,
                    "_effectValue": 1800,
                    "_skillTargetIDs": [],
                    "_skillConditionGroup": 171,
                    "_skillCumulativeConditionID": 12,
                },
            ],
            "MasterSupportSkill": [{"_id": 1}],
            "MasterSupportSkillEffect": [],
            "MasterSkillTarget": [
                {"_id": 3, "_bandID": 1},
                {"_id": 4, "_bandID": 2},
                {"_id": 9, "_bandID": 2},
            ],
            "MasterSkillCondition": [
                {
                    "_id": 315,
                    "_conditionType": 3000,
                    "_isPositive": True,
                    "_conditionTargetIDs": [9],
                },
                {
                    "_id": 101,
                    "_conditionType": 1000,
                    "_isPositive": True,
                    "_conditionTargetIDs": [],
                },
            ],
            "MasterSkillConditionSet": [
                {"_id": 1, "_group": 415, "_conditionIds": [315]},
                {"_id": 2, "_group": 171, "_conditionIds": [101]},
            ],
            "MasterSkillCumulativeCondition": [
                {
                    "_id": 70,
                    "_skillCumulativeConditionType": 3000,
                    "_maxCumulativeCount": 2,
                    "_conditionTargetIDs": [9],
                },
                {
                    "_id": 12,
                    "_skillCumulativeConditionType": 1000,
                    "_maxCumulativeCount": 5,
                    "_conditionTargetIDs": [],
                },
            ],
            "MasterLiveNoteParameter": [{"_id": 1, "_noteOperateType": 1, "_scorePercent": 100}],
            "MasterLiveComboScoreBonus": [
                {"_id": 1, "_comboBonusType": 0, "_requiredComboCount": 0, "_bonusFactor": 1.0}
            ],
            "MasterEventEffect": [],
            "MasterLiveMusic": [
                {
                    "_id": 1,
                    "_titleTextID": "song1",
                    "_musicType": 1,
                    "_expertID": 1,
                    "_bandIDs": [1],
                    "_tagIDs": [],
                }
            ],
            "MasterLiveMusicScore": [
                {
                    "_id": 1,
                    "_musicScoreTextFileName": "chart1",
                    "_musicScoreLevel": 25,
                    "_musicScoreDisplayLevel": 25.0,
                    "_fullComboCount": 5,
                }
            ],
            "MasterChallengeMusic": [],
            "MasterGekisouSkill": [],
            "MasterText": [
                {"_id": k, "_text": k}
                for k in (
                    *(f"c{i}" for i in range(1, 6)),
                    "npc",
                    *(f"m{i}" for i in range(1, 7)),
                    *(f"s{i}" for i in range(1, 6)),
                    "song1",
                )
            ],
        }
        master = folder / "master"
        charts = folder / "charts"
        master.mkdir(parents=True, exist_ok=True)
        charts.mkdir(parents=True, exist_ok=True)
        for name, rows in tables.items():
            (master / f"{name}.json").write_text(json.dumps(rows), encoding="utf-8")
        (charts / "chart1.json").write_text(
            json.dumps(
                {
                    "format": "nnnotes.live-score/1",
                    "log": [],
                    "judgementNoteCount": 5,
                    "notes": [
                        {"timeMs": i * 1000, "op": 1, "judgement": True} for i in range(1, 6)
                    ],
                    "skill": [{"index": i, "timeMs": (i + 1) * 1000} for i in range(5)],
                }
            ),
            encoding="utf-8",
        )

    def test_compile_from_master_ignores_non_playable_and_resolves_band_item_caps(self):
        t = Tables()
        t.data["MasterCharacter"].append({"_id": 1001, "_bandID": 2, "_isNonPlayable": True})
        t.data["MasterCharacterTotalRank"].extend(
            [{"_totalRank": 4, "_bonus": 20}, {"_totalRank": 6, "_bonus": 25}]
        )
        fix, ideal = build_fix(t)
        self.assertEqual(ideal["character_count"], 2)
        self.assertEqual(ideal["character_total_rank"], 4)
        self.assertEqual(fix["character_total_rank_bonus"], [20, 20, 20])
        self.assertEqual(fix["band_item_bonus_bp"], [2500, 2500, 2500])

        a = account()
        c = catalog()
        apply_account_catalog(t, c, a)
        self.assertEqual(c["fix"]["character_total_rank_bonus"], [10, 10, 10])
        bad = account()
        bad["characters"].append({"character_id": 1001, "exp": 0})
        with self.assertRaises(AccountError):
            apply_account_catalog(t, catalog(), bad)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._minimal_master_tables(root, band2_max_level=30)
            p = compile_problem(root / "master", root / "charts", 0, "expert", 1)
            self.assertEqual(p["catalog"]["fix"]["character_total_rank_bonus"], [90, 90, 90])
            self.assertEqual(p["catalog"]["fix"]["band_item_bonus_bp"], [300, 300, 300])
            self.assertEqual(p["meta"]["ideal"]["band_item_bp"], 300)

            ls61 = next(s for s in p["catalog"]["leader_skills"] if s["id"] == 61)
            self.assertEqual(ls61["effects"][1]["condition_group"], 415)
            self.assertEqual(ls61["effects"][1]["cumulative_type"], 3000)
            self.assertEqual(ls61["effects"][1]["max_cumulative_count"], 2)
            self.assertEqual(ls61["effects"][1]["cumulative_targets"], [9])

            pp = root / "problem.json"
            pp.write_text(json.dumps(p), encoding="utf-8")
            ranked = subprocess.run(
                [
                    str(ROOT / "build/deckcalc"),
                    "rank",
                    "-p",
                    str(pp),
                    "--method",
                    "exact",
                    "--objective",
                    "score",
                    "--quiet",
                ],
                capture_output=True,
                text=True,
                check=True,
            )
            winner = json.loads(ranked.stdout)["results"][0]
            self.assertEqual(winner["leader"], 1)
            self.assertGreater(winner["power"], 0)

            p_override = compile_problem(
                root / "master", root / "charts", 0, "expert", 1, band_item_bp=2500
            )
            self.assertEqual(p_override["catalog"]["fix"]["band_item_bonus_bp"], [2500, 2500, 2500])

            self._minimal_master_tables(root, band2_max_level=20)
            p_asym = compile_problem(root / "master", root / "charts", 0, "expert", 1)
            self.assertEqual(p_asym["catalog"]["fix"]["band_item_bonus_bp"], [0, 0, 0])
            self.assertEqual(
                p_asym["catalog"]["fix"]["band_item_bonus_bp_by_band"],
                {"1": [300, 300, 300], "2": [200, 200, 200]},
            )


if __name__ == "__main__":
    unittest.main()
