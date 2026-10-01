#!/usr/bin/env python3
"""从解包后的 Master 数据表生成 problem JSON。

    python3 tools/compile_from_master.py \
        --master-dir /path/to/master-decrypted/<CURRENT> \
        --charts-dir /path/to/charts \
        --event-id 1 --songs 100109 --difficulty expert \
        --output problem.json

``--charts-dir`` 放原始谱面，代码 ``0109/0109_03`` 对应 ``<charts-dir>/0109/0109_03.json``。
``--songs`` 给多首歌时按 ``OUT-<id>-<难度>.json`` 分别写出，单首歌精确写 ``--output``。
``--latest-dir`` 是可选表覆盖，``--event-id 0`` 关闭活动加成，
``--ideal-level`` / ``--ideal-rank`` / ``--ideal-awake`` 指定养成档位。
需要转换谱面时才会 import numpy。
"""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from collections.abc import Iterable, Mapping, Sequence
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

TOOL = "tools/compile_from_master.py"
SCHEMA_PROBLEM = "ournotes-deck-problem@1"
DIFFICULTIES = ("easy", "normal", "hard", "expert")
JST = timezone(timedelta(hours=9), "+09:00")

# 谱面里引用的 op 必须都能在 MasterLiveNoteParameter 找到 score_percent（见 chart-schema.md 第 7 节）。
EXPECTED_SKILL_COUNT = 5

# 复刻 prepare.py 的逐字段挑选规则需要这些表；缺任何一张都直接报错。
REQUIRED_TABLES = (
    "MasterCharacter",
    "MasterChallengeMusic",
    "MasterEventEffect",
    "MasterLiveMusic",
    "MasterLiveMusicScore",
    "MasterMemberCard",
    "MasterMemberCardAwake",
    "MasterMemberCardLevel",
    "MasterMemberCardRank",
    "MasterSupportCard",
    "MasterSupportCardLevel",
    "MasterSupportCardRank",
    "MasterCharacterRank",
    "MasterCharacterTotalRank",
    "MasterVipRankBonus",
    "MasterText",
    "MasterLiveSkill",
    "MasterLiveSkillEffect",
    "MasterGekisouSkill",
    "MasterLeaderSkill",
    "MasterLeaderSkillEffect",
    "MasterSupportSkill",
    "MasterSupportSkillEffect",
    "MasterSkillTarget",
    "MasterSkillCondition",
    "MasterSkillConditionSet",
    "MasterLiveNoteParameter",
    "MasterLiveComboScoreBonus",
)

# 与其它模块保持一致的问题设置；其余字段交给 spec 的默认值。
PROBLEM_SETTINGS: dict[str, Any] = {
    "combo_type": 0,
    "live_level": 5,
    "leader_level": 5,
    "support_level": 5,
    "base_duration_ms": 5000,
    "duration_extension_effect_type": 15000,
    "type_link_bonus_source": "snapshot",
    "leader_unmapped_effect_types": "ignore",
}


class CompileError(RuntimeError):
    """Master 数据缺失或自相矛盾，无法产出可信的 problem。"""


# --------------------------------------------------------------------------------------
# Master 表读取
# --------------------------------------------------------------------------------------


def _read_table_file(path: Path) -> list[dict[str, Any]]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CompileError(f"无法读取 Master 表 {path}: {exc}") from exc
    try:
        doc = json.loads(text)
    except json.JSONDecodeError as exc:
        raise CompileError(f"Master 表 {path} 不是合法 JSON: {exc}") from exc
    rows = doc if isinstance(doc, list) else doc.get("_allData", doc.get("entries"))
    if not isinstance(rows, list):
        raise CompileError(f"Master 表 {path} 既不是数组也没有 _allData / entries")
    return rows


class MasterTables:
    """按「latest overlay 优先，其次 master-dir」的规则惰性读取 Master 表。"""

    def __init__(self, master_dir: Path, latest_dir: Path | None = None) -> None:
        self.master_dir = Path(master_dir)
        self.latest_dir = Path(latest_dir) if latest_dir else None
        self._cache: dict[str, list[dict[str, Any]]] = {}
        self._paths: dict[str, Path] = {}

    def path_for(self, name: str) -> Path:
        if self.latest_dir is not None:
            overlay = self.latest_dir / f"{name}.json"
            if overlay.is_file():
                return overlay
        return self.master_dir / f"{name}.json"

    def table(self, name: str) -> list[dict[str, Any]]:
        if name not in self._cache:
            path = self.path_for(name)
            rows = _read_table_file(path)
            self._cache[name] = rows
            self._paths[name] = path
        return self._cache[name]

    def source_path(self, name: str) -> str:
        if name not in self._paths:
            self.table(name)
        return str(self._paths[name])

    def require(self, names: Iterable[str]) -> None:
        missing = [name for name in names if not self.path_for(name).is_file()]
        if missing:
            raise CompileError(
                "Master 目录缺少必需的表: " + ", ".join(sorted(missing))
                + f"（master-dir={self.master_dir}）"
            )
        for name in names:
            self.table(name)


# --------------------------------------------------------------------------------------
# 小工具
# --------------------------------------------------------------------------------------


def _int(value: Any, *, default: int = 0) -> int:
    if value is None:
        return default
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        if not value.is_integer():
            raise CompileError(f"期望整数，实际是 {value!r}")
        return int(value)
    return int(value)


def _int_list(value: Any) -> list[int]:
    if value is None:
        return []
    if isinstance(value, (list, tuple)):
        return [_int(v) for v in value]
    return [_int(value)]


def _index(rows: Sequence[Mapping[str, Any]], key: str = "_id") -> dict[int, Mapping[str, Any]]:
    return {_int(r[key]): r for r in rows}


def _max_row(rows: Sequence[Mapping[str, Any]], order: str) -> Mapping[str, Any]:
    if not rows:
        raise CompileError("需要至少一行数据来取最大值")
    return max(rows, key=lambda r: _int(r[order]))


def _group_max_row(
    rows: Sequence[Mapping[str, Any]], group_key: str, group: Any, order: str
) -> Mapping[str, Any]:
    matched = [r for r in rows if r.get(group_key) == group]
    if not matched:
        raise CompileError(f"{group_key}={group!r} 在表中没有任何行")
    return _max_row(matched, order)


def _group_row_at(
    rows: Sequence[Mapping[str, Any]],
    group_key: str,
    group: Any,
    value_key: str,
    value: int,
) -> Mapping[str, Any]:
    """取某 group 下 ``value_key == value`` 的行；缺失时退回该 group 的最大行。

    用于 ``--ideal-level`` / ``--ideal-rank`` / ``--ideal-awake``：覆盖值应当换成
    *该等级对应的那一行比率*，而不是把最大行的比率按比例缩放。
    """
    matched = [r for r in rows if r.get(group_key) == group]
    if not matched:
        raise CompileError(f"{group_key}={group!r} 在表中没有任何行")
    for row in matched:
        if _int(row.get(value_key)) == value:
            return row
    return _max_row(matched, value_key)


def _trained(base: Sequence[int], rates: Sequence[Mapping[str, Any]], keys) -> list[int]:
    """``int(base*rate//10000)`` 逐项相加；rates 用于 level / rank / awake 三分量。"""
    out = []
    for value, key in zip(base, keys):
        total = 0
        for rate_row in rates:
            total += int(value * _int(rate_row[f"_{key}Rate"]) // 10000)
        out.append(total)
    return out


def _sorted_unique(values: Iterable[int]) -> list[int]:
    return sorted(set(values))


def _isoformat_jst(moment: datetime | None = None) -> str:
    return (moment or datetime.now(JST)).astimezone(JST).isoformat()


# --------------------------------------------------------------------------------------
# 目录（catalog）构建
# --------------------------------------------------------------------------------------


def _load_texts(tables: MasterTables) -> dict[str, str]:
    return {str(r["_id"]): str(r.get("_japanese") or "") for r in tables.table("MasterText")}


def _event_bonus_index(
    tables: MasterTables, event_id: int
) -> dict[int, list[Mapping[str, Any]]]:
    """把 ``_eventBonusType == 2`` 且 ``_eventId == event_id`` 的行按 kind 分桶。"""
    if event_id == 0:
        return {2: [], 3: []}
    buckets: dict[int, list[Mapping[str, Any]]] = {2: [], 3: []}
    for row in tables.table("MasterEventEffect"):
        if _int(row.get("_eventId")) != event_id:
            continue
        if _int(row.get("_eventBonusType")) != 2:
            continue
        constraint = _int(row.get("_resourceTypeConstraint"))
        for kind in (2, 3):
            if constraint in (0, kind):
                buckets[kind].append(row)
    return buckets


def _event_bonus_bp(
    rules: Sequence[Mapping[str, Any]],
    kind: int,
    card: Mapping[str, Any],
    character_ids: set[int],
    band_ids: set[int],
    card_type: int,
    tags: set[int],
) -> int:
    """复刻 prepare.py 的 event_bonus(card, kind)：逐条过滤后对 _rank5EffectValue 求和。"""
    total = 0
    for row in rules:
        member_card_id = _int(row.get("_memberCardId"))
        if member_card_id and (kind != 2 or member_card_id != _int(card["_id"])):
            continue
        support_card_id = _int(row.get("_supportCardId"))
        if support_card_id and (kind != 3 or support_card_id != _int(card["_id"])):
            continue
        character_id = _int(row.get("_characterId"))
        if character_id and character_id not in character_ids:
            continue
        band_id = _int(row.get("_bandId"))
        if band_id and band_id not in band_ids:
            continue
        row_card_type = _int(row.get("_cardType"))
        if row_card_type and row_card_type != card_type:
            continue
        tag_id = _int(row.get("_tagId"))
        if tag_id and tag_id not in tags:
            continue
        total += _int(row.get("_rank5EffectValue"))
    return total


def build_members(
    tables: MasterTables,
    texts: Mapping[str, str],
    characters: Mapping[int, Mapping[str, Any]],
    event_id: int,
    ideal_level: int | None,
    ideal_rank: int | None,
    ideal_awake: int | None,
) -> list[dict[str, Any]]:
    level_rows = tables.table("MasterMemberCardLevel")
    rank_rows = tables.table("MasterMemberCardRank")
    awake_rows = tables.table("MasterMemberCardAwake")
    buckets = _event_bonus_index(tables, event_id)

    members: list[dict[str, Any]] = []
    for card in tables.table("MasterMemberCard"):
        level_group = card.get("_memberCardLevelGroup")
        rank_group = card.get("_memberCardRankGroup")
        awake_group = card.get("_memberCardAwakeGroup")

        level_row = _group_max_row(level_rows, "_group", level_group, "_level")
        awake_row = _group_max_row(awake_rows, "_group", awake_group, "_awakeCount")
        # 覆盖值走「该等级/该觉醒数对应的行」，比率不做任何缩放。
        if ideal_level is not None:
            level_row = _group_row_at(
                level_rows, "_group", level_group, "_level", int(ideal_level)
            )
        if ideal_awake is not None:
            awake_row = _group_row_at(
                awake_rows, "_group", awake_group, "_awakeCount", int(ideal_awake)
            )
        if ideal_rank is not None:
            rank_row = _group_row_at(
                rank_rows, "_group", rank_group, "_rank", int(ideal_rank)
            )
        else:
            rank_row = _group_max_row(rank_rows, "_group", rank_group, "_rank")

        base = [
            _int(card[f"_{key}PowerMax"]) for key in ("performance", "technic", "visual")
        ]
        trained = _trained(
            base, (level_row, rank_row, awake_row), ("performance", "technic", "visual")
        )

        character_id = _int(card.get("_characterID"))
        character = characters.get(character_id)
        if character is None:
            raise CompileError(f"成员卡 {card['_id']} 引用了未知角色 {character_id}")
        tags = _int_list(card.get("_bestMusicTagIDs"))
        members.append(
            {
                "id": _int(card["_id"]),
                "name": texts.get(str(card.get("_nameTextID")), str(card.get("_nameTextID") or "")),
                "title": texts.get(
                    str(card.get("_subtitleTextID")), str(card.get("_subtitleTextID") or "")
                ),
                "character": character_id,
                "band": _int(character.get("_bandID")),
                "card_type": _int(card.get("_cardType")),
                "rarity": _int(card.get("_rarity")),
                "trained": trained,
                "event_bonus_bp": _event_bonus_bp(
                    buckets[2], 2, card, {character_id}, {_int(character.get("_bandID"))},
                    _int(card.get("_cardType")), set(tags),
                ),
                "tags": tags,
                "live_skill": _int(card.get("_liveSkillID")),
                "leader_skill": _int(card.get("_leaderSkillID")),
                "gekisou_skill": _int(card.get("_gekisouSkillID")),
                "card_rank_bonus_bp": {
                    "type_link": 0,  # 刻意的不对称：type_link 只取自 Snapshot Rank 行
                    "music_type": _int(rank_row.get("_musicTypeBonusRate")),
                    "music_tag": _int(rank_row.get("_musicTagBonusRate")),
                },
                "level": _int(level_row.get("_level")),
                "rank": _int(rank_row.get("_rank")),
                "awake": _int(awake_row.get("_awakeCount")),
            }
        )
    return members


def build_snapshots(
    tables: MasterTables,
    texts: Mapping[str, str],
    characters: Mapping[int, Mapping[str, Any]],
    event_id: int,
    ideal_level: int | None,
    ideal_rank: int | None,
) -> list[dict[str, Any]]:
    level_rows = tables.table("MasterSupportCardLevel")
    rank_rows = tables.table("MasterSupportCardRank")
    buckets = _event_bonus_index(tables, event_id)

    snapshots: list[dict[str, Any]] = []
    for card in tables.table("MasterSupportCard"):
        level_group = card.get("_supportCardLevelGroup")
        rank_group = card.get("_supportCardRankGroup")
        level_row = _group_max_row(level_rows, "_group", level_group, "_level")
        if ideal_level is not None:
            level_row = _group_row_at(level_rows, "_group", level_group, "_level", int(ideal_level))
        if ideal_rank is not None:
            rank_row = _group_row_at(rank_rows, "_group", rank_group, "_rank", int(ideal_rank))
        else:
            rank_row = _group_max_row(rank_rows, "_group", rank_group, "_rank")

        base = [
            _int(card[f"_{key}PowerMax"]) for key in ("performance", "technic", "visual")
        ]
        # Snapshot 的 trained 只有 level 一项（见 prepare.py）。
        trained = _trained(base, (level_row,), ("performance", "technic", "visual"))

        character_ids = _int_list(card.get("_characterIDs"))
        bands: set[int] = set()
        for character_id in character_ids:
            character = characters.get(character_id)
            if character is None:
                raise CompileError(f"Snapshot {card['_id']} 引用了未知角色 {character_id}")
            bands.add(_int(character.get("_bandID")))

        snapshots.append(
            {
                "id": _int(card["_id"]),
                "name": texts.get(str(card.get("_nameTextID")), str(card.get("_nameTextID") or "")),
                "title": texts.get(
                    str(card.get("_descriptionTextID")), str(card.get("_descriptionTextID") or "")
                ),
                "characters": character_ids,
                "bands": sorted(bands),
                "card_type": _int(card.get("_cardType")),
                "rarity": _int(card.get("_rarity")),
                "trained": trained,
                "event_bonus_bp": _event_bonus_bp(
                    buckets[3], 3, card, set(character_ids), bands,
                    _int(card.get("_cardType")), set(),
                ),
                "rank": _int(rank_row.get("_rank")),
                "level": _int(level_row.get("_level")),
                "support_skills": [
                    _int(card.get("_supportSkillId01")),
                    _int(card.get("_supportSkillId02")),
                ],
                "card_rank_bonus_bp": {
                    "type_link": _int(rank_row.get("_cardTypeLinkBonusRate")),
                    "music_type": 0,
                    "music_tag": 0,
                },
            }
        )
    return snapshots


def build_fix(
    tables: MasterTables, band_item_bp: int
) -> tuple[dict[str, Any], dict[str, Any]]:
    """静态加成 + 供 --ideal-* 默认值使用的「表内最大值」信息。"""
    character_rank_rows = tables.table("MasterCharacterRank")
    max_rank_row = _max_row(character_rank_rows, "_rank")
    character_count = len(tables.table("MasterCharacter"))
    total_rank = character_count * _int(max_rank_row["_rank"])
    total_rows = [
        r for r in tables.table("MasterCharacterTotalRank") if _int(r["_totalRank"]) <= total_rank
    ]
    if not total_rows:
        raise CompileError(f"MasterCharacterTotalRank 没有 _totalRank <= {total_rank} 的行")
    total_bonus_row = _max_row(total_rows, "_totalRank")
    vip_rows = [
        r for r in tables.table("MasterVipRankBonus") if _int(r.get("_vipBonusType")) == 7
    ]
    if not vip_rows:
        raise CompileError("MasterVipRankBonus 没有 _vipBonusType == 7 的行")
    vip_bonus = max(_int(r["_value"]) for r in vip_rows)

    fix = {
        # _bonus 是标量，按 model.py 的广播语义展开成三维。
        "character_rank_bonus": [_int(max_rank_row["_bonus"])] * 3,
        "character_total_rank_bonus": [_int(total_bonus_row["_bonus"])] * 3,
        "band_item_bonus_bp": [int(band_item_bp)] * 3,
        "vip_bonus_bp": [vip_bonus] * 3,
        "type_link_base_bp": 500,
        "music_type_base_bp": 500,
        "music_tag_base_bp": 500,
    }
    ideal = {
        "character_rank": _int(max_rank_row["_rank"]),
        "character_total_rank": total_rank,
        "character_total_bonus": _int(total_bonus_row["_bonus"]),
        "vip_bonus_bp": vip_bonus,
        "character_count": character_count,
    }
    return fix, ideal


def build_catalog(
    tables: MasterTables,
    band_item_bp: int,
    event_id: int,
    ideal_level: int | None = None,
    ideal_rank: int | None = None,
    ideal_awake: int | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """构建 catalog 与 fix；ideal_* 为 None 时一律取各 group 的表内最大值。"""
    texts = _load_texts(tables)
    characters = _index(tables.table("MasterCharacter"))
    members = build_members(
        tables, texts, characters, int(event_id), ideal_level, ideal_rank, ideal_awake
    )
    snapshots = build_snapshots(
        tables, texts, characters, int(event_id), ideal_level, ideal_rank
    )
    fix, ideal_info = build_fix(tables, band_item_bp)

    live_skills = [
        {
            "id": _int(row["_id"]),
            "categories": _int_list(row.get("_skillCategories")),
            "mission_type": 0,
            "effects": [
                {
                    "level": _int(e.get("_level")),
                    "effect_type": _int(e.get("_skillEffectType")),
                    "value": _int(e.get("_effectValue")),
                }
                for e in tables.table("MasterLiveSkillEffect")
                if _int(e.get("_liveSkillID")) == _int(row["_id"])
            ],
        }
        for row in tables.table("MasterLiveSkill")
    ]
    gekisou_skills = [
        {
            "id": _int(row["_id"]),
            "categories": _int_list(row.get("_skillCategories")),
            "mission_type": _int(row.get("_gekisouMissionType")),
            "effects": [],
        }
        for row in tables.table("MasterGekisouSkill")
    ]
    leader_skills = [
        {
            "id": _int(row["_id"]),
            "categories": [],
            "effects": [
                {
                    "level": _int(e.get("_level")),
                    "effect_type": _int(e.get("_skillEffectType")),
                    "value": _int(e.get("_effectValue")),
                    "targets": _int_list(e.get("_skillTargetIDs")),
                }
                for e in tables.table("MasterLeaderSkillEffect")
                if _int(e.get("_leaderSkillID")) == _int(row["_id"])
            ],
        }
        for row in tables.table("MasterLeaderSkill")
    ]
    support_skills = [
        {
            "id": _int(row["_id"]),
            "categories": [],
            "effects": [
                {
                    "level": _int(e.get("_level")),
                    "effect_type": _int(e.get("_skillEffectType")),
                    "value": _int(e.get("_effectValue")),
                    "condition_group": _int(e.get("_skillConditionGroup")),
                }
                for e in tables.table("MasterSupportSkillEffect")
                if _int(e.get("_supportSkillID")) == _int(row["_id"])
            ],
        }
        for row in tables.table("MasterSupportSkill")
    ]
    targets = [
        {
            "id": _int(row["_id"]),
            "character": _int(row.get("_characterID")),
            "band": _int(row.get("_bandID")),
            "card_type": _int(row.get("_cardType")),
            "tag": _int(row.get("_tagID")),
            "gekisou_mission_type": _int(row.get("_gekisouMissionType")),
            "live_skill_categories": _int_list(row.get("_liveSkillCategories")),
            "gekisou_skill_categories": _int_list(row.get("_gekisouSkillCategories")),
        }
        for row in tables.table("MasterSkillTarget")
    ]
    conditions = [
        {
            "id": _int(row["_id"]),
            "type": _int(row.get("_conditionType")),
            "positive": bool(row.get("_isPositive")),
            "targets": _int_list(row.get("_conditionTargetIDs")),
        }
        for row in tables.table("MasterSkillCondition")
    ]
    grouped: dict[int, list[list[int]]] = {}
    for row in tables.table("MasterSkillConditionSet"):
        grouped.setdefault(_int(row.get("_group")), []).append(_int_list(row.get("_conditionIds")))
    condition_groups = [
        {"group": group, "rows": grouped[group]} for group in sorted(grouped)
    ]
    note_parameters = [
        {
            "op": _int(row.get("_noteOperateType")),
            "score_percent": row.get("_scorePercent"),
        }
        for row in tables.table("MasterLiveNoteParameter")
    ]
    combo_bonuses = [
        {
            "type": _int(row.get("_comboBonusType")),
            "required_combo_count": _int(row.get("_requiredComboCount")),
            "factor": row.get("_bonusFactor"),
        }
        for row in tables.table("MasterLiveComboScoreBonus")
    ]

    catalog = {
        "members": members,
        "snapshots": snapshots,
        "fix": fix,
        "live_skills": live_skills,
        "gekisou_skills": gekisou_skills,
        "leader_skills": leader_skills,
        "support_skills": support_skills,
        "targets": targets,
        "conditions": conditions,
        "condition_groups": condition_groups,
        "note_parameters": note_parameters,
        "combo_bonuses": combo_bonuses,
    }
    return catalog, ideal_info


# --------------------------------------------------------------------------------------
# 谱面转换
# --------------------------------------------------------------------------------------


def _load_convert_chart() -> Any:
    """import 同目录下 vendored 的 convert_chart.py（MIT, empty-sekai/nnnotes）。

    convert_chart 依赖 numpy，所以只有真的要转换谱面时才 import；这样本模块本身
    保持「标准库即可 import」并且没有 import 期副作用。
    """
    path = Path(__file__).resolve().parent / "convert_chart.py"
    if not path.is_file():
        raise CompileError(f"找不到谱面转换器 {path}")
    spec = importlib.util.spec_from_file_location("our_notes_convert_chart", path)
    if spec is None or spec.loader is None:  # pragma: no cover - 只在异常布局下发生
        raise CompileError(f"无法加载谱面转换器 {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["our_notes_convert_chart"] = module
    try:
        spec.loader.exec_module(module)
    except ModuleNotFoundError as exc:
        raise CompileError(
            f"谱面转换器 {path} 需要 {exc.name}（请用带 numpy 的解释器运行，"
            "例如 `uv run --with numpy python3 tools/compile_from_master.py ...`）"
        ) from exc
    if not hasattr(module, "convert"):
        raise CompileError(f"谱面转换器 {path} 没有 convert() 入口")
    return module


def _load_chart_runtime(charts_dir: Path, code: str) -> dict[str, Any]:
    path = charts_dir / f"{code}.json"
    if not path.is_file():
        raise CompileError(f"找不到原始谱面 {path}")
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise CompileError(f"原始谱面 {path} 不是合法 JSON: {exc}") from exc
    try:
        return _load_convert_chart().convert(raw)
    except CompileError:
        raise
    except Exception as exc:  # 转换器的失败模式（见 docs/reference/chart-schema.md 第 1 节）
        raise CompileError(f"谱面 {path} 转换失败: {type(exc).__name__}: {exc}") from exc


def build_chart(
    score: Mapping[str, Any], charts_dir: Path, difficulty: str
) -> dict[str, Any]:
    code = score.get("_musicScoreTextFileName")
    if not code:
        raise CompileError(f"MasterLiveMusicScore {score.get('_id')} 缺少 _musicScoreTextFileName")
    runtime = _load_chart_runtime(charts_dir, str(code))

    if runtime.get("format") != "nnnotes.live-score/1":
        raise CompileError(
            f"谱面 {code} 的 format 是 {runtime.get('format')!r}，只支持 'nnnotes.live-score/1'"
        )
    log = runtime.get("log") or []
    if log:
        raise CompileError(
            f"谱面 {code} 转换产生了 {len(log)} 条诊断（转换器丢音符的信号），拒绝使用: {log[:3]}"
        )

    notes = runtime.get("notes") or []
    judged = [n for n in notes if n.get("judgement")]
    full_combo_count = score.get("_fullComboCount")
    if full_combo_count is not None and len(judged) != _int(full_combo_count):
        raise CompileError(
            f"谱面 {code} 判定音符数 {len(judged)} != Master _fullComboCount {full_combo_count}"
        )
    if runtime.get("judgementNoteCount") != len(judged):
        raise CompileError(
            f"谱面 {code} 的 judgementNoteCount={runtime.get('judgementNoteCount')} "
            f"与 judgement 标记计数 {len(judged)} 不一致"
        )

    times = [n["timeMs"] for n in judged]
    if any(times[i] > times[i + 1] for i in range(len(times) - 1)):
        raise CompileError(f"谱面 {code} 的判定音符未按 timeMs 升序排列")

    skills = runtime.get("skill") or []
    if len(skills) != EXPECTED_SKILL_COUNT:
        raise CompileError(
            f"谱面 {code} 的技能时点有 {len(skills)} 个，期望 {EXPECTED_SKILL_COUNT} 个"
        )
    if sorted(_int(s.get("index")) for s in skills) != list(range(EXPECTED_SKILL_COUNT)):
        raise CompileError(f"谱面 {code} 的技能 index 不是 0..{EXPECTED_SKILL_COUNT - 1}")

    return {
        "difficulty": difficulty,
        "level": _int(score.get("_musicScoreLevel")),
        "display_level": float(score.get("_musicScoreDisplayLevel") or 0.0),
        "full_combo_count": _int(full_combo_count) if full_combo_count is not None else len(judged),
        "notes": [
            {"t": _int(n["timeMs"]), "op": _int(n["op"]), "scoring": bool(n.get("judgement"))}
            for n in notes
        ],
        "skill_times_ms": [_int(s["timeMs"]) for s in skills],
    }


# --------------------------------------------------------------------------------------
# 歌曲选择
# --------------------------------------------------------------------------------------


def _songs_for_event(
    tables: MasterTables, event_id: int
) -> dict[int, list[Mapping[str, Any]]]:
    """返回 {liveMusicId: [MasterChallengeMusic 行...]}。

    ``event_id == 0`` 表示没有活动：收录所有被 MasterChallengeMusic 引用到的曲子。
    """
    grouped: dict[int, list[Mapping[str, Any]]] = {}
    for task in tables.table("MasterChallengeMusic"):
        if event_id != 0 and _int(task.get("_eventId")) != event_id:
            continue
        grouped.setdefault(_int(task.get("_liveMusicId")), []).append(task)
    return grouped


def _resolve_song_ids(
    tables: MasterTables, songs_arg: str | None, event_id: int
) -> tuple[list[int], dict[int, list[Mapping[str, Any]]]]:
    grouped = _songs_for_event(tables, event_id)

    if songs_arg:
        requested: list[int] = []
        for chunk in songs_arg.split(","):
            chunk = chunk.strip()
            if not chunk:
                continue
            if not re.fullmatch(r"-?\d+", chunk):
                raise CompileError(f"--songs 里 {chunk!r} 不是整数 id")
            value = int(chunk)
            if value not in requested:
                requested.append(value)
        if not requested:
            raise CompileError("--songs 没有解析出任何歌曲 id")
        tasks: dict[int, list[Mapping[str, Any]]] = {}
        for value in requested:
            if value in grouped:
                tasks[value] = grouped[value]
            else:
                tasks[value] = []
        return requested, tasks

    if event_id != 0:
        if not grouped:
            raise CompileError(f"MasterChallengeMusic 里没有 _eventId == {event_id} 的曲目")
        return sorted(grouped), grouped

    # event 0：默认收录 MasterLiveMusic 全集。
    all_music = sorted(_int(row["_id"]) for row in tables.table("MasterLiveMusic"))
    if not all_music:
        raise CompileError("MasterLiveMusic 为空")
    return all_music, {music_id: grouped.get(music_id, []) for music_id in all_music}


def build_song(
    tables: MasterTables,
    texts: Mapping[str, str],
    music: Mapping[str, Any],
    tasks: Sequence[Mapping[str, Any]],
    charts_dir: Path,
    difficulty: str,
) -> dict[str, Any]:
    if not tasks:
        raise CompileError(
            f"MasterLiveMusic {music.get('_id')} 没有被 MasterChallengeMusic 引用，无法确定 _musicType"
        )
    task = tasks[0]
    if len(tasks) > 1:
        types = {_int(t.get("_musicType")) for t in tasks}
        if len(types) > 1:
            raise CompileError(
                f"曲目 {music.get('_id')} 有多条 MasterChallengeMusic 且 _musicType 不一致: {sorted(types)}"
            )

    diff_key = f"_{difficulty}ID"
    if music.get(diff_key) is None:
        raise CompileError(f"MasterLiveMusic {music.get('_id')} 缺少 {diff_key}")
    scores = tables.table("MasterLiveMusicScore")
    score = next((s for s in scores if _int(s["_id"]) == _int(music[diff_key])), None)
    if score is None:
        raise CompileError(f"MasterLiveMusic {music.get('_id')} 的 {diff_key}={music[diff_key]} 没有对应谱面行")

    title_id = str(music.get("_titleTextID") or "")
    return {
        "id": _int(music["_id"]),
        "title": texts.get(title_id, title_id),
        "type": _int(task.get("_musicType")),
        "bands": _int_list(music.get("_bandIDs")),
        "tags": _int_list(music.get("_bestMusicTagIDs")),
        "chart": build_chart(score, charts_dir, difficulty),
    }


# --------------------------------------------------------------------------------------
# problem 组装与校验
# --------------------------------------------------------------------------------------


def validate_problem(problem: Mapping[str, Any]) -> None:
    """交给 build/deckcalc validate 校验 schema；没有二进制就跳过。"""
    binary = Path(__file__).resolve().parent.parent / "build" / "deckcalc"
    if not binary.is_file():
        print("提示：build/deckcalc 不存在，跳过 schema 校验")
        return
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "problem.json"
        path.write_text(json.dumps(problem, ensure_ascii=False), encoding="utf-8")
        completed = subprocess.run(
            [str(binary), "validate", "-p", str(path)], capture_output=True, text=True
        )
    if completed.returncode != 0:
        raise CompileError(f"problem 未通过 deckcalc 校验: {completed.stderr.strip()}")


def compile_problem(
    master_dir: Path,
    charts_dir: Path,
    event_id: int,
    difficulty: str,
    song_id: int,
    latest_dir: Path | None = None,
    ideal_level: int | None = None,
    ideal_rank: int | None = None,
    ideal_awake: int | None = None,
    band_item_bp: int = 2500,
    validate: bool = True,
    generated_at: str | None = None,
) -> dict[str, Any]:
    """编译一首歌的 problem JSON（纯函数：不做文件写入）。"""
    if difficulty not in DIFFICULTIES:
        raise CompileError(f"--difficulty 只支持 {DIFFICULTIES}，收到 {difficulty!r}")

    tables = MasterTables(master_dir, latest_dir)
    tables.require(REQUIRED_TABLES)

    texts = _load_texts(tables)
    catalog, ideal_info = build_catalog(
        tables,
        band_item_bp,
        event_id=event_id,
        ideal_level=ideal_level,
        ideal_rank=ideal_rank,
        ideal_awake=ideal_awake,
    )
    tasks = _songs_for_event(tables, event_id).get(song_id, [])
    music = next((m for m in tables.table("MasterLiveMusic") if _int(m["_id"]) == song_id), None)
    if music is None:
        raise CompileError(f"MasterLiveMusic 里没有 _id == {song_id}")

    song = build_song(tables, texts, music, tasks, Path(charts_dir), difficulty)

    problem: dict[str, Any] = {
        "schema": SCHEMA_PROBLEM,
        "meta": {
            "generated_at": generated_at or _isoformat_jst(),
            "source": {
                "master_dir": str(master_dir),
                "latest_dir": None if latest_dir is None else str(latest_dir),
                "charts_dir": str(charts_dir),
                "event_id": int(event_id),
            },
            "tool": TOOL,
            "ideal": ideal_info,
        },
        "song": {k: song[k] for k in ("id", "title", "type", "bands", "tags")},
        "chart": song["chart"],
        "catalog": catalog,
        "settings": copy.deepcopy(PROBLEM_SETTINGS),
    }

    if validate:
        validate_problem(problem)
    return problem


def output_path_for(base: Path, song_id: int, difficulty: str, multiple: bool) -> Path:
    """单曲：精确使用 --output；多曲：在扩展名前插入 ``-<song>-<difficulty>``。"""
    base = Path(base)
    if not multiple:
        return base
    suffix = base.suffix
    stem = base.name[: -len(suffix)] if suffix else base.name
    return base.with_name(f"{stem}-{song_id}-{difficulty}{suffix}")


def write_problem(problem: Mapping[str, Any], path: Path) -> None:
    path = Path(path)
    if path.parent and str(path.parent):
        path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(problem, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="compile_from_master.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "从本地解密的 Master 表 + 原始谱面编译 ournotes-deck-problem@1 problem JSON。\n"
            "运行时不联网，只读用户自己的数据目录。"
        ),
        epilog=(
            "输出文件规则：\n"
            "  * --songs 只有一个 id 时，精确写入 --output；\n"
            "  * --songs 有多个 id 时，每首歌写一个文件，在扩展名前插入歌曲 id 与难度，\n"
            "    例如 --output OUT.json 会得到 OUT-100109-expert.json、OUT-100056-expert.json ……\n"
            "\n"
            "示例：\n"
            "  python3 tools/compile_from_master.py --master-dir /path/master \\\n"
            "      --latest-dir /path/latest --charts-dir /path/charts \\\n"
            "      --event-id 1 --songs 100109,100056,100063 --difficulty expert \\\n"
            "      --output OUT.json\n"
        ),
    )
    parser.add_argument(
        "--master-dir",
        required=True,
        help="存放 Master<Name>.json 的目录（通常 .../master-decrypted/<CURRENT>）",
    )
    parser.add_argument(
        "--latest-dir",
        default=None,
        help="可选 overlay 目录：某张表在这里存在则优先使用（例如 latest-pack-check/master）",
    )
    parser.add_argument(
        "--charts-dir",
        required=True,
        help="原始谱面目录；谱面代码 0109/0109_03 -> <charts-dir>/0109/0109_03.json",
    )
    parser.add_argument(
        "--event-id",
        type=int,
        default=1,
        help="活动 id；0 表示「没有活动加成」（所有 event_bonus_bp = 0，并收录所有曲目）",
    )
    parser.add_argument(
        "--songs",
        default=None,
        help=(
            "逗号分隔的 MasterLiveMusic id 列表（如 100109,100056,100063）。"
            "省略时：event-id != 0 取该活动的全部挑战曲；event-id == 0 取 MasterLiveMusic 全集"
        ),
    )
    parser.add_argument(
        "--difficulty",
        choices=DIFFICULTIES,
        default="expert",
        help="谱面难度（默认 expert）",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="输出路径；多首歌时作为文件名模板（见下方说明）",
    )
    parser.add_argument(
        "--ideal-level",
        type=int,
        default=None,
        help="覆盖等级：用该等级对应的比率代替各 group 的表内最高等级（默认 = 表内最大值）",
    )
    parser.add_argument(
        "--ideal-rank",
        type=int,
        default=None,
        help="覆盖卡牌 Rank：用该 Rank 行的比率代替各 group 的表内最高 Rank（默认 = 表内最大值）",
    )
    parser.add_argument(
        "--ideal-awake",
        type=int,
        default=None,
        help="覆盖觉醒数：用该觉醒数的比率代替各 group 的表内最大值（默认 = 表内最大值）",
    )
    parser.add_argument(
        "--band-item-bp",
        type=int,
        default=2500,
        help="乐队道具加成 bp（默认 2500 = 5 件 Lv50 合计 +25%%）",
    )
    parser.add_argument(
        "--no-validate",
        action="store_true",
        help="跳过 deckcalc validate 校验，默认会校验且失败即报错",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="只输出错误，不打印每个文件的结果摘要",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    master_dir = Path(args.master_dir).expanduser().resolve()
    if not master_dir.is_dir():
        parser.error(f"--master-dir 不是目录: {master_dir}")
    latest_dir = Path(args.latest_dir).expanduser().resolve() if args.latest_dir else None
    if latest_dir is not None and not latest_dir.is_dir():
        parser.error(f"--latest-dir 不是目录: {latest_dir}")
    charts_dir_arg = Path(args.charts_dir).expanduser().resolve()
    if not charts_dir_arg.is_dir():
        parser.error(f"--charts-dir 不是目录: {charts_dir_arg}")
    output = Path(args.output).expanduser()

    try:
        tables = MasterTables(master_dir, latest_dir)
        tables.require(("MasterChallengeMusic", "MasterLiveMusic"))
        song_ids, _tasks = _resolve_song_ids(tables, args.songs, int(args.event_id))
        generated_at = _isoformat_jst()
        multiple = len(song_ids) > 1
        written: list[Path] = []
        for song_id in song_ids:
            problem = compile_problem(
                master_dir=master_dir,
                charts_dir=charts_dir_arg,
                event_id=int(args.event_id),
                difficulty=args.difficulty,
                song_id=song_id,
                latest_dir=latest_dir,
                ideal_level=args.ideal_level,
                ideal_rank=args.ideal_rank,
                ideal_awake=args.ideal_awake,
                band_item_bp=args.band_item_bp,
                validate=not args.no_validate,
                generated_at=generated_at,
            )
            path = output_path_for(output, song_id, args.difficulty, multiple)
            write_problem(problem, path)
            written.append(path)
            if not args.quiet:
                chart = problem["chart"]
                print(
                    f"[compile_from_master] song={song_id} {problem['song']['title']} "
                    f"difficulty={args.difficulty} level={chart['level']} "
                    f"judged={sum(1 for n in chart['notes'] if n['scoring'])} "
                    f"members={len(problem['catalog']['members'])} "
                    f"snapshots={len(problem['catalog']['snapshots'])} -> {path}",
                    file=sys.stderr,
                )
    except CompileError as exc:
        print(f"compile_from_master: 错误: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
