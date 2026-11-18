#!/usr/bin/env python3
"""激奏评分与组队测试套件。

覆盖：
1. 合成 fixture 的金标准评分与槽位综合力
2. 相同基础 power 下激奏技能对得分与组队排名的真实影响
3. 技能顺序影响分数的歌曲上三个优化目标 min < mean < max 验证
4. ournotes-deck-formation@1 标准编队 schema 往返复算及队长重排配对保护
5. 实际养成 (growth) 变化对 power 与激奏得分的直接影响及显式覆盖校验
6. 约束条件严格校验与 fail-fast（未知卡牌、冲突、重复角色、非卡池卡牌）
7. 小候选池完全穷举 Oracle 与有界搜索 Top-K 对齐及 optimality_scope 语义
8. 假设排名 (assumed placement) 机制与 Luck 种子确定性
9. 严格输入校验（score_mode、fevers 1..3、missions 3、不支持技能拒绝）
10. compile_from_master 隔离性与 36 张必要 Master 表自包含导出
"""

from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SYNTHETIC_FIXTURE_PATH = REPO_ROOT / "tests" / "fixtures" / "synthetic_gekisou.json"
MASTER_DIR = REPO_ROOT / "output" / "account-plan-20261010" / "master"
CHARTS_DIR = REPO_ROOT / "output" / "charts"
DECKCALC = REPO_ROOT / "build" / "deckcalc"


class GekisouTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        assert SYNTHETIC_FIXTURE_PATH.is_file(), f"缺少提交的 fixture: {SYNTHETIC_FIXTURE_PATH}"
        if not DECKCALC.is_file():
            raise unittest.SkipTest(f"缺少 {DECKCALC}，请先运行 make")
        cls.fixture_path = SYNTHETIC_FIXTURE_PATH

    def run_bridge(self, subcommand: str, payload: dict) -> dict:
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as tf:
            json.dump(payload, tf, ensure_ascii=False)
            tf_path = tf.name

        try:
            cmd = [str(DECKCALC), f"gekisou-{subcommand}", "-i", tf_path]
            proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(REPO_ROOT))
        finally:
            try:
                os.remove(tf_path)
            except OSError:
                pass

        if proc.returncode != 0:
            try:
                err_doc = json.loads(proc.stderr)
                msg = err_doc.get("message", proc.stderr)
            except Exception:
                msg = proc.stderr
            raise RuntimeError(f"Gekisou error (code {proc.returncode}): {msg}")

        return json.loads(proc.stdout)

    def test_01_upstream_parity_exact(self) -> None:
        """测试 1: 合成 fixture 的评分与槽位综合力严格等于金标准。"""
        formation = {
            "schema": "ournotes-deck-formation@1",
            "leader": 1,
            "slots": [
                {"slot": 0, "member": 1, "snapshot": 1, "trigger": 0},
                {"slot": 1, "member": 2, "snapshot": 2, "trigger": 1},
                {"slot": 2, "member": 3, "snapshot": 3, "trigger": 2},
                {"slot": 3, "member": 4, "snapshot": 4, "trigger": 3},
                {"slot": 4, "member": 5, "snapshot": 5, "trigger": 4},
            ],
        }

        for seeds in ([0], []):
            result = self.run_bridge(
                "score",
                {
                    "problem": str(self.fixture_path),
                    "formation": formation,
                    "options": {"assumed_placement": 1, "seeds": seeds},
                },
            )
            self.assertEqual(result["status"], "ok")
            self.assertEqual(result["power"], 134717)
            self.assertEqual(result["slot_powers"], [26949, 26950, 26921, 26942, 26955])
            self.assertEqual(result["mean"], 3808362.8)
            self.assertEqual(result["min"], 3719907)
            self.assertEqual(result["max"], 4148716)
            self.assertEqual(result["min_order"], 6)
            self.assertEqual(result["max_order"], 0)

    def test_02_identical_power_skills_change_ranking(self) -> None:
        """测试 2: 相同基础 power 的卡片，不同激奏技能导致得分与队伍排名不同。"""
        # 成员 5 (gk_skill 4: Combo) 与成员 3 (gk_skill 1: Just) 基础 power 完全相同
        formation_combo = {
            "schema": "ournotes-deck-formation@1",
            "leader": 1,
            "slots": [
                {"slot": 0, "member": 1, "snapshot": 1},
                {"slot": 1, "member": 2, "snapshot": 2},
                {"slot": 2, "member": 4, "snapshot": 4},
                {"slot": 3, "member": 5, "snapshot": 5},  # combo skill
                {"slot": 4, "member": 7, "snapshot": 3},
            ],
        }
        formation_just = {
            "schema": "ournotes-deck-formation@1",
            "leader": 1,
            "slots": [
                {"slot": 0, "member": 1, "snapshot": 1},
                {"slot": 1, "member": 2, "snapshot": 2},
                {"slot": 2, "member": 4, "snapshot": 4},
                {"slot": 3, "member": 3, "snapshot": 5},  # just skill
                {"slot": 4, "member": 7, "snapshot": 3},
            ],
        }

        res_combo = self.run_bridge(
            "score", {"problem": str(self.fixture_path), "formation": formation_combo}
        )
        res_just = self.run_bridge(
            "score", {"problem": str(self.fixture_path), "formation": formation_just}
        )

        self.assertAlmostEqual(
            res_combo["power"], res_just["power"], delta=50, msg="两队基础队伍 power 必须相近"
        )
        self.assertNotEqual(
            res_combo["mean"], res_just["mean"], "相近 power 下激奏技能必须产生不同得分"
        )

    def test_03_min_mean_max_order_dependence(self) -> None:
        """测试 3: 技能顺序影响分数的歌曲上，三个优化目标严格满足 min < mean < max。"""
        formation = {
            "schema": "ournotes-deck-formation@1",
            "leader": 1,
            "slots": [
                {"slot": 0, "member": 1, "snapshot": 1},
                {"slot": 1, "member": 2, "snapshot": 2},  # +100% live skill
                {"slot": 2, "member": 3, "snapshot": 3},
                {"slot": 3, "member": 4, "snapshot": 4},
                {"slot": 4, "member": 5, "snapshot": 5},
            ],
        }

        res_score = self.run_bridge(
            "score", {"problem": str(self.fixture_path), "formation": formation}
        )
        s_min = res_score["min"]
        s_mean = res_score["mean"]
        s_max = res_score["max"]

        self.assertLess(s_min, s_mean, "最劣顺序得分必须严格小于均值")
        self.assertLess(s_mean, s_max, "均值必须严格小于最优顺序最高分")
        self.assertNotEqual(res_score["min_order"], res_score["max_order"])

        # 验证 rank 命令支持三个 criterion 并分别输出对应目标
        cand = {
            "members": [{"id": i} for i in range(1, 7)],
            "snaps": [{"id": i} for i in range(1, 6)],
        }
        res_rank_min = self.run_bridge(
            "rank",
            {"problem": str(self.fixture_path), "candidates": cand, "criterion": "min", "k": 1},
        )
        res_rank_mean = self.run_bridge(
            "rank",
            {"problem": str(self.fixture_path), "candidates": cand, "criterion": "mean", "k": 1},
        )
        res_rank_max = self.run_bridge(
            "rank",
            {"problem": str(self.fixture_path), "candidates": cand, "criterion": "max", "k": 1},
        )

        self.assertLess(res_rank_min["hits"][0]["score"], res_rank_mean["hits"][0]["score"])
        self.assertLess(res_rank_mean["hits"][0]["score"], res_rank_max["hits"][0]["score"])

    def test_04_standard_formation_schema_and_pipeline(self) -> None:
        """测试 4: 标准编队格式支持、队长重排配对保护、以及 rank 输出直接被 score 复算。"""
        # 1. leader 在槽位 2 时，重排后槽位 0 与槽位 2 交换（成员与 snapshot 配对保持不变）
        formation_reorder = {
            "schema": "ournotes-deck-formation@1",
            "leader": 3,  # leader 在槽位 2
            "slots": [
                {"slot": 0, "member": 1, "snapshot": 1},
                {"slot": 1, "member": 2, "snapshot": 2},
                {"slot": 2, "member": 3, "snapshot": 3},  # leader
                {"slot": 3, "member": 4, "snapshot": 4},
                {"slot": 4, "member": 5, "snapshot": 5},
            ],
        }
        res_reordered = self.run_bridge(
            "score", {"problem": str(self.fixture_path), "formation": formation_reorder}
        )
        self.assertEqual(res_reordered["status"], "ok")

        # 2. rank 结果包含标准 ournotes-deck-formation@1，并可直接传递给 score 计算出完全一致的得分
        cand = {
            "members": [{"id": i} for i in range(1, 7)],
            "snaps": [{"id": i} for i in range(1, 6)],
        }
        rank_res = self.run_bridge(
            "rank", {"problem": str(self.fixture_path), "candidates": cand, "k": 1}
        )
        top_hit = rank_res["hits"][0]
        self.assertIn("formation", top_hit)
        form = top_hit["formation"]
        self.assertEqual(form["schema"], "ournotes-deck-formation@1")
        self.assertIn("leader", form)
        self.assertEqual(len(form["slots"]), 5)

        # 直接复算
        score_res = self.run_bridge("score", {"problem": str(self.fixture_path), "formation": form})
        self.assertEqual(score_res["mean"], top_hit["score"])
        self.assertEqual(score_res["power"], top_hit["power"])

    def test_05_growth_progression_impact(self) -> None:
        """测试 5: catalog 养成参数生效，养成变化直接改变队伍 power 与激奏得分。"""
        # 读取 fixture 并修改成员卡养成至 1 级
        doc = json.loads(self.fixture_path.read_text(encoding="utf-8"))
        doc_lvl1 = copy.deepcopy(doc)
        for m in doc_lvl1["catalog"]["members"]:
            m["level"] = 1
            m["rank"] = 1
            m["awake"] = 1
            m["live_skill_level"] = 1
            m["trained"] = [4149, 4149, 4149]

        formation = {
            "schema": "ournotes-deck-formation@1",
            "leader": 1,
            "slots": [
                {"slot": 0, "member": 1, "snapshot": 1},
                {"slot": 1, "member": 2, "snapshot": 2},
                {"slot": 2, "member": 3, "snapshot": 3},
                {"slot": 3, "member": 4, "snapshot": 4},
                {"slot": 4, "member": 5, "snapshot": 5},
            ],
        }

        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as tf:
            json.dump(doc_lvl1, tf)
            tf_path = tf.name

        try:
            res_max = self.run_bridge(
                "score", {"problem": str(self.fixture_path), "formation": formation}
            )
            res_lvl1 = self.run_bridge("score", {"problem": tf_path, "formation": formation})
            self.assertGreater(
                res_max["power"],
                res_lvl1["power"],
                f"满养成 power {res_max['power']} 必须大于 1 级 power {res_lvl1['power']}",
            )
            self.assertGreater(
                res_max["mean"],
                res_lvl1["mean"],
                f"满养成得分 {res_max['mean']} 必须大于 1 级得分 {res_lvl1['mean']}",
            )
        finally:
            try:
                os.remove(tf_path)
            except OSError:
                pass

        # 测试 Snapshot snap_growth 等级覆盖改变 power 与得分
        snap_lvl1_form = copy.deepcopy(formation)
        for s in snap_lvl1_form["slots"]:
            s["snap_growth"] = {"level": 1, "rank": 1}
        res_snap_lvl1 = self.run_bridge(
            "score", {"problem": str(self.fixture_path), "formation": snap_lvl1_form}
        )
        self.assertGreater(
            res_max["power"],
            res_snap_lvl1["power"],
            f"满级 Snapshot power {res_max['power']} 必须大于 1 级 Snapshot power {res_snap_lvl1['power']}",
        )
        self.assertGreater(
            res_max["mean"],
            res_snap_lvl1["mean"],
            f"满级 Snapshot 得分 {res_max['mean']} 必须大于 1 级 Snapshot 得分 {res_snap_lvl1['mean']}",
        )

        # 测试显式非整数 growth 覆盖报错
        bad_growth_form = copy.deepcopy(formation)
        bad_growth_form["slots"][0]["member_growth"] = {"level": 0}
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge(
                "score", {"problem": str(self.fixture_path), "formation": bad_growth_form}
            )
        self.assertIn("成长参数", str(cm.exception))

    def test_06_constraints_fail_fast(self) -> None:
        """测试 6: 约束条件严格校验与 fail-fast，禁止非法 ID 或冲突约束返回假结果。"""
        # 1. 必选不存在的成员卡 ID
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge(
                "rank",
                {
                    "problem": str(self.fixture_path),
                    "constraints": {"required_members": [999999]},
                },
            )
        self.assertIn("约束中的成员卡不存在: 999999", str(cm.exception))

        # 2. 必选不存在的 Snapshot ID
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge(
                "rank",
                {
                    "problem": str(self.fixture_path),
                    "constraints": {"required_snaps": [888888]},
                },
            )
        self.assertIn("约束中的 Snapshot 不存在: 888888", str(cm.exception))

        # 3. 成员卡同时出现在必选与排除列表中
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge(
                "rank",
                {
                    "problem": str(self.fixture_path),
                    "constraints": {"required_members": [1], "excluded_members": [1]},
                },
            )
        self.assertIn("同时出现在必选和排除列表", str(cm.exception))

        # 4. 必选成员超过 5 张
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge(
                "rank",
                {
                    "problem": str(self.fixture_path),
                    "constraints": {"required_members": [1, 2, 3, 4, 5, 6]},
                },
            )
        self.assertIn("必选成员卡数量不能超过 5 张", str(cm.exception))

        # 5. 编队中包含不在 catalog 中的卡牌
        bad_catalog_form = {
            "slots": [
                {"slot": 0, "member": 999999, "snapshot": 0},
                {"slot": 1, "member": 2, "snapshot": 0},
                {"slot": 2, "member": 3, "snapshot": 0},
                {"slot": 3, "member": 4, "snapshot": 0},
                {"slot": 4, "member": 5, "snapshot": 0},
            ]
        }
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge(
                "score", {"problem": str(self.fixture_path), "formation": bad_catalog_form}
            )
        self.assertIn("不在输入卡池 (catalog) 中", str(cm.exception))

        # 6. distinct_characters=false 明确拒绝
        doc = json.loads(self.fixture_path.read_text(encoding="utf-8"))
        cand_sample = {
            "members": [{"id": i} for i in range(1, 7)],
            "snaps": [{"id": i} for i in range(1, 6)],
        }
        doc_dup_char = copy.deepcopy(doc)
        doc_dup_char["constraints"] = {"distinct_characters": False}
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge("rank", {"problem": doc_dup_char, "candidates": cand_sample})
        self.assertIn("角色互异", str(cm.exception))

        # 7. distinct_snapshots=false 明确拒绝
        doc_dup_snap = copy.deepcopy(doc)
        doc_dup_snap["constraints"] = {"distinct_snapshots": False}
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge("rank", {"problem": doc_dup_snap, "candidates": cand_sample})
        self.assertIn("Snapshot 互异", str(cm.exception))

        # 8. member_pool / snapshot_pool 暂不支持明确拒绝
        doc_pool = copy.deepcopy(doc)
        doc_pool["constraints"] = {"member_pool": [1, 2]}
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge("rank", {"problem": doc_pool, "candidates": cand_sample})
        self.assertIn("member_pool", str(cm.exception))

        # 9. banned_members 正确映射排除
        doc_banned = copy.deepcopy(doc)
        doc_banned["constraints"] = {"banned_members": [1]}
        res_banned = self.run_bridge(
            "rank", {"problem": doc_banned, "candidates": cand_sample, "k": 1}
        )
        self.assertNotIn(1, res_banned["hits"][0]["formation"]["member_ids"])

    def test_07_small_pool_topk_oracle_and_scopes(self) -> None:
        """测试 7: 小候选池完全穷举 Oracle 与有界搜索 Top-K 对齐及 optimality_scope 语义。"""
        cand = {
            "members": [{"id": 1}, {"id": 2}, {"id": 3}, {"id": 4}, {"id": 5}, {"id": 6}],
            "snaps": [{"id": 1}, {"id": 2}, {"id": 3}, {"id": 4}, {"id": 5}],
        }

        # 1. seeds=[0] 时 scope 为 exact_seeded_oracle
        oracle_seeded = self.run_bridge(
            "rank",
            {
                "problem": str(self.fixture_path),
                "candidates": cand,
                "oracle": True,
                "k": 3,
                "options": {"seeds": [0]},
            },
        )
        self.assertTrue(oracle_seeded["search_complete"])
        self.assertEqual(oracle_seeded["optimality_scope"], "exact_seeded_oracle")

        # 2. seeds=[] (nominal exact DP) 时 scope 为 exact_global_oracle
        oracle_global = self.run_bridge(
            "rank",
            {
                "problem": str(self.fixture_path),
                "candidates": cand,
                "oracle": True,
                "k": 3,
                "options": {"seeds": []},
            },
        )
        self.assertTrue(oracle_global["search_complete"])
        self.assertEqual(oracle_global["optimality_scope"], "exact_global_oracle")

        # 3. 有界搜索与 Oracle Top-1 完全一致
        search_res = self.run_bridge(
            "rank",
            {
                "problem": str(self.fixture_path),
                "candidates": cand,
                "oracle": False,
                "k": 1,
                "options": {"seeds": [0]},
            },
        )
        self.assertEqual(oracle_seeded["hits"][0]["score"], search_res["hits"][0]["score"])
        self.assertEqual(oracle_seeded["hits"][0]["power"], search_res["hits"][0]["power"])

    def test_08_luck_and_assumed_placement(self) -> None:
        """测试 8: 假设排名 bonus 单调性与 Luck 种子确定性。"""
        form = {
            "slots": [
                {"slot": 0, "member": 1, "snapshot": 1},
                {"slot": 1, "member": 2, "snapshot": 2},
                {"slot": 2, "member": 3, "snapshot": 3},
                {"slot": 3, "member": 4, "snapshot": 4},
                {"slot": 4, "member": 5, "snapshot": 5},
            ]
        }

        # 假设排名 1 > 2 > 5
        r1 = self.run_bridge(
            "score",
            {
                "problem": str(self.fixture_path),
                "formation": form,
                "options": {"assumed_placement": 1},
            },
        )
        r2 = self.run_bridge(
            "score",
            {
                "problem": str(self.fixture_path),
                "formation": form,
                "options": {"assumed_placement": 2},
            },
        )
        r5 = self.run_bridge(
            "score",
            {
                "problem": str(self.fixture_path),
                "formation": form,
                "options": {"assumed_placement": 5},
            },
        )
        self.assertGreater(r1["mean"], r2["mean"])
        self.assertGreater(r2["mean"], r5["mean"])

        # 种子可复现性
        sa = self.run_bridge(
            "score",
            {"problem": str(self.fixture_path), "formation": form, "options": {"seeds": [123]}},
        )
        sb = self.run_bridge(
            "score",
            {"problem": str(self.fixture_path), "formation": form, "options": {"seeds": [123]}},
        )
        self.assertEqual(sa["mean"], sb["mean"])
        self.assertEqual(sa["luck_mode"], "sample_mean")

    def test_09_strict_schema_validation(self) -> None:
        """测试 9: 严格输入校验：拒绝错误 score_mode、非法 fever/mission 区间及不支持技能。"""
        doc = json.loads(self.fixture_path.read_text(encoding="utf-8"))

        # 1. 拒绝 score_mode !== 'gekisou'
        doc_bad_mode = copy.deepcopy(doc)
        doc_bad_mode["meta"]["source"]["score_mode"] = "normal"
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge("score", {"problem": doc_bad_mode, "formation": {"slots": []}})
        self.assertIn("score_mode === 'gekisou'", str(cm.exception))

        # 2. 拒绝 fever 数量不为 1..3
        doc_bad_fever = copy.deepcopy(doc)
        doc_bad_fever["chart"]["fevers"] = []
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge(
                "score",
                {
                    "problem": doc_bad_fever,
                    "formation": {
                        "slots": [{"slot": i, "member": i + 1, "snapshot": 0} for i in range(5)]
                    },
                },
            )
        self.assertIn("必须包含 1 到 3 个 Fever", str(cm.exception))

        # 3. 拒绝 mission 数量不为 3
        doc_bad_mission = copy.deepcopy(doc)
        doc_bad_mission["song"]["gekisou_missions"] = [1, 2]
        with self.assertRaises(RuntimeError) as cm:
            self.run_bridge(
                "score",
                {
                    "problem": doc_bad_mission,
                    "formation": {
                        "slots": [{"slot": i, "member": i + 1, "snapshot": 0} for i in range(5)]
                    },
                },
            )
        self.assertIn("必须包含恰好 3 个有效任务", str(cm.exception))

    def test_10_compile_isolation_and_master_tables(self) -> None:
        """测试 10: 编译隔离性及 36 张 Master 表严格导出无静默忽略。"""
        if not MASTER_DIR.is_dir() or not CHARTS_DIR.is_dir():
            # 若无完整本地 Master 目录，验证 fixture 已包含全部 36 张表
            doc = json.loads(self.fixture_path.read_text(encoding="utf-8"))
            tables = doc.get("gekisou_master_tables", {})
            self.assertEqual(len(tables), 36, "fixture 必须自包含全部 36 张 Master 表")
            return

        with tempfile.TemporaryDirectory() as td:
            gk_p = Path(td) / "gk.json"
            norm_p = Path(td) / "norm.json"

            cmd_gk = [
                sys.executable,
                str(REPO_ROOT / "tools" / "compile_from_master.py"),
                "--master-dir",
                str(MASTER_DIR),
                "--charts-dir",
                str(CHARTS_DIR),
                "--songs",
                "100109",
                "--difficulty",
                "expert",
                "--score-mode",
                "gekisou",
                "--output",
                str(gk_p),
                "--quiet",
            ]
            subprocess.run(cmd_gk, check=True, cwd=str(REPO_ROOT))

            cmd_norm = [
                sys.executable,
                str(REPO_ROOT / "tools" / "compile_from_master.py"),
                "--master-dir",
                str(MASTER_DIR),
                "--charts-dir",
                str(CHARTS_DIR),
                "--songs",
                "100109",
                "--difficulty",
                "expert",
                "--score-mode",
                "normal",
                "--output",
                str(norm_p),
                "--quiet",
            ]
            subprocess.run(cmd_norm, check=True, cwd=str(REPO_ROOT))

            gk_data = json.loads(gk_p.read_text(encoding="utf-8"))
            norm_data = json.loads(norm_p.read_text(encoding="utf-8"))

            self.assertIn("gekisou_master_tables", gk_data)
            self.assertEqual(len(gk_data["gekisou_master_tables"]), 36)
            self.assertNotIn("gekisou_master_tables", norm_data)
            self.assertNotIn("gekisou_missions", norm_data["song"])

    def test_11_cpp_slot_by_slot_power_parity_and_switches(self) -> None:
        """测试 11: 真实标准输入与各 power 开关下，与 C++ deckcalc 逐槽与总综合力 100% 对齐。"""
        m_dir = (
            REPO_ROOT / "output" / "account-plan-20261009" / "master"
            if (REPO_ROOT / "output" / "account-plan-20261009" / "master").is_dir()
            else MASTER_DIR
        )
        if not m_dir.is_dir() or not CHARTS_DIR.is_dir():
            raise unittest.SkipTest("缺少真实 master 或 charts 目录，跳过 C++ 逐槽综合力比对")

        with tempfile.TemporaryDirectory() as td:
            prob_path = Path(td) / "prob.json"
            form_path = Path(td) / "form.json"

            cmd_compile = [
                sys.executable,
                str(REPO_ROOT / "tools" / "compile_from_master.py"),
                "--master-dir",
                str(m_dir),
                "--charts-dir",
                str(CHARTS_DIR),
                "--songs",
                "100109",
                "--difficulty",
                "expert",
                "--event-id",
                "1",
                "--score-mode",
                "gekisou",
                "--output",
                str(prob_path),
                "--quiet",
            ]
            subprocess.run(cmd_compile, check=True, cwd=str(REPO_ROOT))
            p_base = json.loads(prob_path.read_text(encoding="utf-8"))

            formation = {
                "schema": "ournotes-deck-formation@1",
                "leader": 3,
                "slots": [{"member": i, "snapshot": i, "trigger": i} for i in range(1, 6)],
            }
            form_path.write_text(json.dumps(formation, ensure_ascii=False), encoding="utf-8")

            # 1. 基准综合力与 C++ 逐槽完全一致
            cpp_cmd = [
                str(REPO_ROOT / "build" / "deckcalc"),
                "score",
                "-p",
                str(prob_path),
                "-f",
                str(form_path),
                "--quiet",
            ]
            cpp_out = json.loads(
                subprocess.run(cpp_cmd, capture_output=True, text=True, check=True).stdout
            )
            cpp_total = cpp_out["results"][0]["power"]
            cpp_slots = {a["member"]: a["power"] for a in cpp_out["results"][0]["assignments"]}

            gk_res = self.run_bridge(
                "score",
                {"problem": p_base, "formation": formation, "options": {"seeds": [0]}},
            )
            self.assertEqual(
                gk_res["power"], cpp_total, f"总 power 不一致: {gk_res['power']} vs C++ {cpp_total}"
            )
            gk_slots = gk_res["slot_powers"]
            # 槽位重排后 leader (member 3) 在首槽
            self.assertEqual(gk_slots[0], cpp_slots[3], "队长槽 power 不一致")
            self.assertEqual(gk_slots[1], cpp_slots[2], "槽位 1 power 不一致")
            self.assertEqual(gk_slots[2], cpp_slots[1], "槽位 2 power 不一致")
            self.assertEqual(gk_slots[3], cpp_slots[4], "槽位 3 power 不一致")
            self.assertEqual(gk_slots[4], cpp_slots[5], "槽位 4 power 不一致")

            # 2. 对照各项 power 开关与修正，均与 C++ 严格对齐
            switches = [
                ("no_snapshot", {"settings": {"power_model": {"sources": {"snapshot": False}}}}),
                ("no_band_item", {"settings": {"power_model": {"sources": {"band_item": False}}}}),
                (
                    "no_music_type",
                    {"settings": {"power_model": {"sources": {"music_type": False}}}},
                ),
                ("no_leader", {"settings": {"power_model": {"sources": {"leader": False}}}}),
                ("with_vip", {"catalog": {"fix": {"vip_bonus_bp": [10000, 10000, 10000]}}}),
                ("float64_floor", {"settings": {"power_model": {"rounding": "float64_floor"}}}),
                (
                    "all_off",
                    {
                        "settings": {
                            "power_model": {
                                "sources": {
                                    "snapshot": False,
                                    "band_item": False,
                                    "music_type": False,
                                    "leader": False,
                                }
                            }
                        }
                    },
                ),
            ]

            for name, patch in switches:
                p_mod = copy.deepcopy(p_base)
                p_mod["input_mode"] = "experimental"
                for k, v in patch.items():
                    if k == "settings" and "power_model" in v:
                        p_mod["settings"].setdefault(
                            "power_model", copy.deepcopy(p_base["settings"].get("power_model", {}))
                        )
                        if "sources" in v["power_model"]:
                            p_mod["settings"]["power_model"].setdefault(
                                "sources",
                                copy.deepcopy(
                                    p_base["settings"].get("power_model", {}).get("sources", {})
                                ),
                            ).update(v["power_model"]["sources"])
                        if "rounding" in v["power_model"]:
                            p_mod["settings"]["power_model"]["rounding"] = v["power_model"][
                                "rounding"
                            ]
                    elif k == "catalog" and "fix" in v:
                        p_mod["catalog"].setdefault("fix", {}).update(v["fix"])
                    else:
                        p_mod[k] = v

                prob_mod_path = Path(td) / f"prob_{name}.json"
                prob_mod_path.write_text(json.dumps(p_mod, ensure_ascii=False), encoding="utf-8")

                cpp_sw_cmd = [
                    str(REPO_ROOT / "build" / "deckcalc"),
                    "score",
                    "--experimental",
                    "-p",
                    str(prob_mod_path),
                    "-f",
                    str(form_path),
                    "--quiet",
                ]
                cpp_sw_out = json.loads(
                    subprocess.run(cpp_sw_cmd, capture_output=True, text=True, check=True).stdout
                )
                cpp_sw_power = cpp_sw_out["results"][0]["power"]

                gk_sw_res = self.run_bridge(
                    "score",
                    {"problem": p_mod, "formation": formation, "options": {"seeds": [0]}},
                )
                self.assertEqual(
                    gk_sw_res["power"],
                    cpp_sw_power,
                    f"开关 {name} 下 power 不一致: Gekisou {gk_sw_res['power']} vs C++ {cpp_sw_power}",
                )

    def test_12_cumulative_and_conditioned_leader_skill_parity(self) -> None:
        """测试 12: 带条件组/累计队长技与无累计条件 1500..1503 在 Gekisou 与普通引擎间严格一致。"""
        base = json.loads(self.fixture_path.read_text(encoding="utf-8"))
        gmt = base["gekisou_master_tables"]
        # 对齐 Master 与 catalog 静态加成，使 PrecomputedProbPower、EngineMaster 与 score 完全同基准
        base["catalog"]["fix"] = {
            "character_rank_bonus": [0, 0, 0],
            "character_total_rank_bonus": [0, 0, 0],
            "band_item_bonus_bp": [0, 0, 0],
            "vip_bonus_bp": [0, 0, 0],
            "music_type_base_bp": 500,
            "music_tag_base_bp": 500,
            "type_link_base_bp": 500,
        }
        # 用 Master 属性表填齐 catalog.members / snapshots 的 trained 与 card_rank_bonus_bp
        lvl_m = {(r["_group"], r["_level"]): r for r in gmt["MasterMemberCardLevel"]}
        rk_m = {(r["_group"], r["_rank"]): r for r in gmt["MasterMemberCardRank"]}
        aw_m = {(r["_group"], r["_awakeCount"]): r for r in gmt["MasterMemberCardAwake"]}
        card_m = {r["_id"]: r for r in gmt["MasterMemberCard"]}
        for m in base["catalog"]["members"]:
            mc = card_m[m["id"]]
            lr = lvl_m[(mc["_memberCardLevelGroup"], m["level"])]
            rr = rk_m[(mc["_memberCardRankGroup"], m["rank"])]
            ar = aw_m[(mc["_memberCardAwakeGroup"], m["awake"])]
            m["trained"] = [
                sum(mc[f"_{k}PowerMax"] * row[f"_{k}Rate"] // 10000 for row in (lr, rr, ar))
                for k in ("performance", "technic", "visual")
            ]
            m["card_rank_bonus_bp"] = {
                "type_link": 0,
                "music_type": rr["_musicTypeBonusRate"],
                "music_tag": rr["_musicTagBonusRate"],
            }
        lvl_s = {(r["_group"], r["_level"]): r for r in gmt["MasterSupportCardLevel"]}
        rk_s = {(r["_group"], r["_rank"]): r for r in gmt["MasterSupportCardRank"]}
        card_s = {r["_id"]: r for r in gmt["MasterSupportCard"]}
        for s in base["catalog"]["snapshots"]:
            sc = card_s[s["id"]]
            lr = lvl_s[(sc["_supportCardLevelGroup"], s["level"])]
            rr = rk_s[(sc["_supportCardRankGroup"], s["rank"])]
            s["trained"] = [
                sc[f"_{k}PowerMax"] * lr[f"_{k}Rate"] // 10000
                for k in ("performance", "technic", "visual")
            ]
            s["card_rank_bonus_bp"] = {
                "type_link": rr["_cardTypeLinkBonusRate"],
                "music_type": 0,
                "music_tag": 0,
            }

        base["catalog"]["targets"] = [
            {
                "id": r["_id"],
                "character": r["_characterID"],
                "band": r["_bandID"],
                "card_type": r["_cardType"],
                "tag": r["_tagID"],
                "music_type": r["_liveMusicType"],
                "gekisou_mission_type": r["_gekisouMissionType"],
                "live_skill_categories": r["_liveSkillCategories"],
                "gekisou_skill_categories": r["_gekisouSkillCategories"],
            }
            for r in gmt["MasterSkillTarget"]
        ]
        base["catalog"]["conditions"] = [
            {
                "id": r["_id"],
                "type": r["_conditionType"],
                "positive": bool(r["_isPositive"]),
                "values": r["_conditionValues"],
                "targets": r["_conditionTargetIDs"],
            }
            for r in gmt["MasterSkillCondition"]
        ]
        grouped: dict[int, list[list[int]]] = {}
        for r in gmt["MasterSkillConditionSet"]:
            grouped.setdefault(r["_group"], []).append(r["_conditionIds"])
        base["catalog"]["condition_groups"] = [
            {"group": g, "rows": grouped[g]} for g in sorted(grouped)
        ]
        base["catalog"]["note_parameters"] = [
            {"op": r["_noteOperateType"], "score_percent": r["_scorePercent"]}
            for r in gmt["MasterLiveNoteParameter"]
        ]
        base["catalog"]["combo_bonuses"] = [
            {
                "type": r["_comboBonusType"],
                "required_combo_count": r["_requiredComboCount"],
                "factor": r["_bonusFactor"],
            }
            for r in gmt["MasterLiveComboScoreBonus"]
        ]
        base["catalog"]["live_skills"] = [
            {"id": r["_id"], "effects": [{"level": 5, "effect_type": 2000, "value": 0}]}
            for r in gmt["MasterLiveSkill"]
        ]
        base["catalog"]["gekisou_skills"] = [
            {"id": r["_id"], "mission_type": r["_gekisouMissionType"]}
            for r in gmt["MasterGekisouSkill"]
        ]
        base["catalog"]["support_skills"] = [
            {"id": r["_id"], "effects": []} for r in gmt["MasterSupportSkill"]
        ]

        # 添加测试用条件组 9415（队伍含 card_type 1/2/3/5 任一成员）及 3 类队长技：
        # 1) LS 61: 1001 + 带条件组 9415 的累计 1501 (type=3000, cap=2)
        # 2) LS 6003: 累计 1500 (type=3003, 异乐队种类数)
        # 3) LS 90088: 1503 无累计条件 (cumulative_type=0, 倍率应为 1x)
        gmt["MasterSkillCondition"].append(
            {
                "_id": 9315,
                "_conditionType": 3000,
                "_conditionValues": [],
                "_isPositive": True,
                "_conditionTargetIDs": [8, 9, 10, 12],
            }
        )
        gmt["MasterSkillConditionSet"].append(
            {"_id": 9999, "_group": 9415, "_conditionIds": [9315]}
        )
        base["catalog"]["conditions"].append(
            {
                "id": 9315,
                "type": 3000,
                "positive": True,
                "values": [],
                "targets": [8, 9, 10, 12],
            }
        )
        base["catalog"]["condition_groups"].append({"group": 9415, "rows": [[9315]]})
        gmt["MasterSkillCumulativeCondition"].append(
            {
                "_id": 9070,
                "_skillCumulativeConditionType": 3000,
                "_conditionValues": [1],
                "_conditionTargetIDs": [8, 9, 10, 12],
                "_maxCumulativeCount": 2,
            }
        )
        gmt["MasterSkillCumulativeCondition"].append(
            {
                "_id": 9002,
                "_skillCumulativeConditionType": 3003,
                "_conditionValues": [1],
                "_conditionTargetIDs": [],
                "_maxCumulativeCount": 2,
            }
        )
        # 让成员 5 属于乐队 2、成员 4 属于乐队 3，以便验证 3003（异乐队种类数为 2，而非人数）
        for ch in gmt["MasterCharacter"]:
            if ch["_id"] == 5:
                ch["_bandID"] = 2
            elif ch["_id"] == 4:
                ch["_bandID"] = 3
        base["catalog"]["members"][3]["band"] = 3
        base["catalog"]["members"][4]["band"] = 2

        scenarios = [
            (
                61,
                [
                    {
                        "level": 5,
                        "effect_type": 1001,
                        "value": 10200,
                        "targets": [3],
                        "condition_group": 0,
                    },
                    {
                        "level": 5,
                        "effect_type": 1501,
                        "value": 2400,
                        "targets": [],
                        "condition_group": 9415,
                        "cumulative_type": 3000,
                        "max_cumulative_count": 2,
                        "cumulative_targets": [8, 9, 10, 12],
                    },
                ],
                [
                    (1001, 10200, [3], 0, 0),
                    (1501, 2400, [], 9415, 9070),
                ],
            ),
            (
                6003,
                [
                    {
                        "level": 5,
                        "effect_type": 1500,
                        "value": 2000,
                        "targets": [],
                        "condition_group": 0,
                        "cumulative_type": 3003,
                        "max_cumulative_count": 5,
                        "cumulative_targets": [],
                    }
                ],
                [(1500, 2000, [], 0, 9002)],
            ),
            (
                90088,
                [
                    {
                        "level": 5,
                        "effect_type": 1503,
                        "value": 8000,
                        "targets": [3],
                        "condition_group": 0,
                    }
                ],
                [(1503, 8000, [3], 0, 0)],
            ),
        ]

        formation_score = {
            "schema": "ournotes-deck-formation@1",
            "leader": 1,
            "slots": [{"member": i + 1, "snapshot": i + 1, "trigger": i + 1} for i in range(5)],
        }
        formation_gk = {
            "schema": "ournotes-deck-formation@1",
            "leader": 1,
            "slots": [
                {"slot": i, "member": i + 1, "snapshot": i + 1, "trigger": i} for i in range(5)
            ],
        }

        with tempfile.TemporaryDirectory() as td:
            form_path = Path(td) / "form.json"
            form_path.write_text(json.dumps(formation_score), encoding="utf-8")
            powers_by_sid: dict[int, int] = {}
            for sid, cat_effects, master_specs in scenarios:
                p_case = copy.deepcopy(base)
                p_case["catalog"]["members"][0]["leader_skill"] = sid
                p_case["catalog"]["leader_skills"] = [
                    {"id": r["_id"], "effects": []}
                    for r in p_case["gekisou_master_tables"]["MasterLeaderSkill"]
                ] + [{"id": sid, "effects": cat_effects}]
                p_case["gekisou_master_tables"]["MasterMemberCard"][0]["_leaderSkillID"] = sid
                p_case["gekisou_master_tables"]["MasterLeaderSkill"].append({"_id": sid})
                for idx, (et, val, tids, cg, cid) in enumerate(master_specs):
                    p_case["gekisou_master_tables"]["MasterLeaderSkillEffect"].append(
                        {
                            "_id": 99000 + sid * 10 + idx,
                            "_leaderSkillID": sid,
                            "_level": 5,
                            "_skillConditionGroup": cg,
                            "_skillTargetIDs": tids,
                            "_skillEffectType": et,
                            "_effectValue": val,
                            "_skillCumulativeConditionID": cid,
                        }
                    )
                prob_path = Path(td) / f"prob_{sid}.json"
                prob_path.write_text(json.dumps(p_case), encoding="utf-8")

                cpp_out = json.loads(
                    subprocess.run(
                        [
                            str(DECKCALC),
                            "score",
                            "--experimental",
                            "-p",
                            str(prob_path),
                            "-f",
                            str(form_path),
                            "--order-search",
                            "given",
                            "--quiet",
                        ],
                        capture_output=True,
                        text=True,
                        check=True,
                    ).stdout
                )
                cpp_power = cpp_out["results"][0]["power"]
                cpp_slots = [a["power"] for a in cpp_out["results"][0]["assignments"]]
                powers_by_sid[sid] = cpp_power

                gk_with_cat = self.run_bridge(
                    "score", {"problem": p_case, "formation": formation_gk}
                )
                self.assertEqual(gk_with_cat["power"], cpp_power, f"sid={sid} catalog 分支不一致")
                self.assertEqual(gk_with_cat["slot_powers"], cpp_slots)

                if sid == 61:
                    p_false = copy.deepcopy(p_case)
                    for cond in p_false["catalog"]["conditions"]:
                        if cond["id"] == 9315:
                            cond["positive"] = False
                    prob_false_path = Path(td) / "prob_61_false.json"
                    prob_false_path.write_text(json.dumps(p_false), encoding="utf-8")
                    cpp_false = json.loads(
                        subprocess.run(
                            [
                                str(DECKCALC),
                                "score",
                                "--experimental",
                                "-p",
                                str(prob_false_path),
                                "-f",
                                str(form_path),
                                "--order-search",
                                "given",
                                "--quiet",
                            ],
                            capture_output=True,
                            text=True,
                            check=True,
                        ).stdout
                    )["results"][0]["power"]
                    gk_false = self.run_bridge(
                        "score", {"problem": p_false, "formation": formation_gk}
                    )
                    self.assertEqual(gk_false["power"], cpp_false)
                    self.assertLess(gk_false["power"], gk_with_cat["power"])

            # 进一步验证 LS 61 的 gekisou-rank 也命中相同的累计条件总综合力
            p_rank = copy.deepcopy(base)
            p_rank["catalog"]["members"][0]["leader_skill"] = 61
            p_rank["catalog"]["leader_skills"] = [
                {"id": r["_id"], "effects": []}
                for r in p_rank["gekisou_master_tables"]["MasterLeaderSkill"]
            ] + [{"id": 61, "effects": scenarios[0][1]}]
            p_rank["gekisou_master_tables"]["MasterMemberCard"][0]["_leaderSkillID"] = 61
            p_rank["gekisou_master_tables"]["MasterLeaderSkill"].append({"_id": 61})
            for idx, (et, val, tids, cg, cid) in enumerate(scenarios[0][2]):
                p_rank["gekisou_master_tables"]["MasterLeaderSkillEffect"].append(
                    {
                        "_id": 99610 + idx,
                        "_leaderSkillID": 61,
                        "_level": 5,
                        "_skillConditionGroup": cg,
                        "_skillTargetIDs": tids,
                        "_skillEffectType": et,
                        "_effectValue": val,
                        "_skillCumulativeConditionID": cid,
                    }
                )
            gk_rank = self.run_bridge(
                "rank",
                {
                    "problem": p_rank,
                    "candidates": {
                        "members": [{"id": i} for i in range(1, 6)],
                        "snaps": [{"id": i} for i in range(1, 6)],
                    },
                    "criterion": "mean",
                    "k": 1,
                },
            )
            self.assertEqual(gk_rank["hits"][0]["power"], powers_by_sid[61])


if __name__ == "__main__":
    unittest.main()
