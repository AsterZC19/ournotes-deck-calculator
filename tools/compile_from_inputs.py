"""把编译好的 ``inputs.json`` 转成 problem JSON。

    python3 tools/compile_from_inputs.py \
        --inputs /path/to/inputs.json --song 100109 --difficulty expert \
        --output problem.json
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone, timedelta
from pathlib import Path
from typing import Any, Mapping, Sequence

SCHEMA_PROBLEM = "ournotes-deck-problem@1"
JST = timezone(timedelta(hours=9))


def _broadcast(value: Any) -> list[int]:
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
        items = list(value)
        if len(items) == 3:
            return [int(v) for v in items]
        raise ValueError(f"期望 3 个数字，收到 {items!r}")
    return [int(value)] * 3


def _effects(rows: Sequence[Mapping[str, Any]], key: str) -> dict[int, list[dict[str, Any]]]:
    out: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        out.setdefault(int(row[key]), []).append(row)
    return out


def _field(row: Mapping[str, Any], name: str, default: Any = None) -> Any:
    return row.get(name, default)


def build_catalog(inputs: Mapping[str, Any], *, band_item_bp: int = 2500) -> dict[str, Any]:
    tables = inputs["tables"]
    ideal = inputs["ideal"]

    live_effects = _effects(tables.get("MasterLiveSkillEffect", []), "_liveSkillID")
    leader_effects = _effects(tables.get("MasterLeaderSkillEffect", []), "_leaderSkillID")
    support_effects = _effects(tables.get("MasterSupportSkillEffect", []), "_supportSkillID")

    live_skills = [
        {
            "id": int(row["_id"]),
            "categories": list(_field(row, "_skillCategories", []) or []),
            "mission_type": 0,
            "effects": [
                {
                    "level": int(_field(effect, "_level", 0)),
                    "effect_type": int(_field(effect, "_skillEffectType", 0)),
                    "value": int(_field(effect, "_effectValue", 0)),
                    "targets": [int(t) for t in (_field(effect, "_skillTargetIDs", []) or [])],
                    "condition_group": int(_field(effect, "_skillConditionGroup", 0)),
                }
                for effect in live_effects.get(int(row["_id"]), [])
            ],
        }
        for row in tables.get("MasterLiveSkill", [])
    ]
    gekisou_skills = [
        {
            "id": int(row["_id"]),
            "categories": list(_field(row, "_skillCategories", []) or []),
            "mission_type": int(_field(row, "_gekisouMissionType", 0)),
            "effects": [],
        }
        for row in tables.get("MasterGekisouSkill", [])
    ]
    leader_skills = [
        {
            "id": int(row["_id"]),
            "categories": [],
            "effects": [
                {
                    "level": int(_field(effect, "_level", 0)),
                    "effect_type": int(_field(effect, "_skillEffectType", 0)),
                    "value": int(_field(effect, "_effectValue", 0)),
                    "targets": [int(t) for t in (_field(effect, "_skillTargetIDs", []) or [])],
                }
                for effect in leader_effects.get(int(row["_id"]), [])
            ],
        }
        for row in tables.get("MasterLeaderSkill", [])
    ]
    support_skills = [
        {
            "id": int(row["_id"]),
            "categories": [],
            "effects": [
                {
                    "level": int(_field(effect, "_level", 0)),
                    "effect_type": int(_field(effect, "_skillEffectType", 0)),
                    "value": int(_field(effect, "_effectValue", 0)),
                    "condition_group": int(_field(effect, "_skillConditionGroup", 0)),
                }
                for effect in support_effects.get(int(row["_id"]), [])
            ],
        }
        for row in tables.get("MasterSupportSkill", [])
    ]
    targets = [
        {
            "id": int(row["_id"]),
            "character": int(_field(row, "_characterID", 0)),
            "band": int(_field(row, "_bandID", 0)),
            "card_type": int(_field(row, "_cardType", 0)),
            "tag": int(_field(row, "_tagID", 0)),
            "gekisou_mission_type": int(_field(row, "_gekisouMissionType", 0)),
            "live_skill_categories": [
                int(v) for v in (_field(row, "_liveSkillCategories", []) or [])
            ],
            "gekisou_skill_categories": [
                int(v) for v in (_field(row, "_gekisouSkillCategories", []) or [])
            ],
        }
        for row in tables.get("MasterSkillTarget", [])
    ]
    conditions = [
        {
            "id": int(row["_id"]),
            "type": int(_field(row, "_conditionType", 0)),
            "positive": bool(_field(row, "_isPositive", True)),
            "values": [int(v) for v in (_field(row, "_conditionValues", []) or [])],
            "targets": [int(t) for t in (_field(row, "_conditionTargetIDs", []) or [])],
        }
        for row in tables.get("MasterSkillCondition", [])
    ]
    grouped: dict[int, list[list[int]]] = {}
    for row in tables.get("MasterSkillConditionSet", []):
        grouped.setdefault(int(row["_group"]), []).append(
            [int(v) for v in (_field(row, "_conditionIds", []) or [])]
        )
    condition_groups = [{"group": group, "rows": rows} for group, rows in sorted(grouped.items())]

    note_parameters = [
        {"op": int(row["_noteOperateType"]), "score_percent": float(row["_scorePercent"])}
        for row in tables.get("MasterLiveNoteParameter", [])
    ]
    combo_bonuses = [
        {
            "type": int(row["_comboBonusType"]),
            "required_combo_count": int(row["_requiredComboCount"]),
            "factor": float(row["_bonusFactor"]),
        }
        for row in tables.get("MasterLiveComboScoreBonus", [])
    ]

    return {
        "members": [_member(row, tables) for row in inputs["members"]],
        "snapshots": [_snapshot(row) for row in inputs["snaps"]],
        "fix": {
            "character_rank_bonus": _broadcast(ideal["character_rank"]["_bonus"]),
            "character_total_rank_bonus": _broadcast(ideal["character_total_bonus"]["_bonus"]),
            "band_item_bonus_bp": _broadcast(band_item_bp),
            "vip_bonus_bp": _broadcast(ideal["vip_power_bonus_bp"]),
            "type_link_base_bp": 500,
            "music_type_base_bp": 500,
            "music_tag_base_bp": 500,
        },
        "live_skills": live_skills,
        "leader_skills": leader_skills,
        "support_skills": support_skills,
        "gekisou_skills": gekisou_skills,
        "targets": targets,
        "conditions": conditions,
        "condition_groups": condition_groups,
        "note_parameters": note_parameters,
        "combo_bonuses": combo_bonuses,
    }


def _member(row: Mapping[str, Any], tables: Mapping[str, Any]) -> dict[str, Any]:
    raw = next(
        (r for r in tables.get("MasterMemberCard", []) if int(r["_id"]) == int(row["id"])), {}
    )
    rank = row["rank"]
    return {
        "id": int(row["id"]),
        "name": row["name"],
        "title": row["title"],
        "character": int(row["character"]),
        "band": int(row["band"]),
        "card_type": int(row["type"]),
        "rarity": int(row["rarity"]),
        "trained": [int(v) for v in row["trained"]],
        "event_bonus_bp": int(row["event_bonus_bp"]),
        "tags": [int(v) for v in row["tags"]],
        "live_skill": int(row["live_skill"]),
        "leader_skill": int(row["leader_skill"]),
        "gekisou_skill": int(_field(raw, "_gekisouSkillID", 0) or 0),
        "card_rank_bonus_bp": {
            "type_link": 0,
            "music_type": int(_field(rank, "_musicTypeBonusRate", 0) or 0),
            "music_tag": int(_field(rank, "_musicTagBonusRate", 0) or 0),
        },
        "level": int(_field(row["level"], "_level", 0) or 0),
        "rank": int(_field(rank, "_rank", 0) or 0),
        "awake": int(_field(row["awake"], "_awakeCount", 0) or 0),
    }


def _snapshot(row: Mapping[str, Any]) -> dict[str, Any]:
    rank = row["rank"]
    return {
        "id": int(row["id"]),
        "name": row["name"],
        "title": row["title"],
        "characters": [int(v) for v in row["characters"]],
        "bands": [int(v) for v in row["bands"]],
        "card_type": int(row["type"]),
        "rarity": int(row["rarity"]),
        "trained": [int(v) for v in row["trained"]],
        "event_bonus_bp": int(row["event_bonus_bp"]),
        "rank": int(_field(rank, "_rank", 0) or 0),
        "level": int(_field(row["level"], "_level", 0) or 0),
        "support_skills": [int(v) for v in row["support_skills"]],
        "card_rank_bonus_bp": {
            "type_link": int(_field(rank, "_cardTypeLinkBonusRate", 0) or 0),
            "music_type": 0,
            "music_tag": 0,
        },
    }


def build_problem_document(
    inputs: Mapping[str, Any],
    song_id: int,
    difficulty: str,
    *,
    base_dir: Path,
    band_item_bp: int = 2500,
    source: str = "inputs.json",
) -> dict[str, Any]:
    song = next((s for s in inputs["songs"] if int(s["id"]) == int(song_id)), None)
    if song is None:
        raise SystemExit(
            f"inputs.json 中没有歌曲 {song_id}；可用: {[s['id'] for s in inputs['songs']]}"
        )
    chart_entry = next((c for c in song["charts"] if c["difficulty"] == difficulty), None)
    if chart_entry is None:
        raise SystemExit(
            f"歌曲 {song_id} 没有难度 {difficulty}；可用: {[c['difficulty'] for c in song['charts']]}"
        )
    runtime_path = base_dir / chart_entry["path"]
    runtime = json.loads(runtime_path.read_text(encoding="utf-8"))
    master = chart_entry["master"]
    notes = [
        {"t": int(note["timeMs"]), "op": int(note["op"]), "scoring": bool(note["judgement"])}
        for note in runtime["notes"]
    ]
    skill_times = [int(t) for t in chart_entry["skill_times_ms"]]

    document = {
        "schema": SCHEMA_PROBLEM,
        "meta": {
            "generated_at": datetime.now(JST).isoformat(timespec="seconds"),
            "source": {"inputs_json": str(source), "chart": str(runtime_path)},
            "tool": "tools/compile_from_inputs.py",
        },
        "song": {
            "id": int(song["id"]),
            "title": song["title"],
            "type": int(song["type"]),
            "bands": [int(v) for v in song["bands"]],
            "tags": [int(v) for v in song["tags"]],
        },
        "chart": {
            "difficulty": difficulty,
            "level": float(master["_musicScoreLevel"]),
            "display_level": float(
                _field(master, "_musicScoreDisplayLevel", master["_musicScoreLevel"])
            ),
            "full_combo_count": int(master["_fullComboCount"]),
            "notes": notes,
            "skill_times_ms": skill_times,
        },
        "catalog": build_catalog(inputs, band_item_bp=band_item_bp),
        "settings": {
            "combo_type": 0,
            "live_level": 5,
            "leader_level": 5,
            "support_level": 5,
            "base_duration_ms": 5000,
            "duration_extension_effect_type": 15000,
            "type_link_bonus_source": "snapshot",
            "leader_unmapped_effect_types": "ignore",
        },
    }
    check_document(document)
    return document


def check_document(document: Mapping[str, Any]) -> None:
    """基础自检；完整 schema 校验交给 build/deckcalc validate。"""
    if document.get("schema") != SCHEMA_PROBLEM:
        raise SystemExit("内部错误：schema 版本不对")
    for key in ("song", "chart", "catalog", "settings"):
        if key not in document:
            raise SystemExit(f"内部错误：缺少 {key}")
    if len(document["chart"]["skill_times_ms"]) != int(document["settings"].get("team_size", 5)):
        raise SystemExit("内部错误：skill_times_ms 长度与 team_size 不符")


def validate_with_binary(path: Path) -> None:
    binary = Path(__file__).resolve().parent.parent / "build" / "deckcalc"
    if not binary.exists():
        print("提示：build/deckcalc 不存在，跳过 schema 校验")
        return
    completed = subprocess.run(
        [str(binary), "validate", "-p", str(path)], capture_output=True, text=True
    )
    if completed.returncode != 0:
        raise SystemExit(f"deckcalc validate 失败: {completed.stderr.strip()}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="参考 inputs.json -> problem JSON")
    parser.add_argument("--inputs", "-i", required=True)
    parser.add_argument("--song", type=int, required=True)
    parser.add_argument("--difficulty", default="expert")
    parser.add_argument("--output", "-o", required=True)
    parser.add_argument("--band-item-bp", type=int, default=2500)
    args = parser.parse_args(argv)

    inputs_path = Path(args.inputs).resolve()
    inputs = json.loads(inputs_path.read_text(encoding="utf-8"))
    document = build_problem_document(
        inputs,
        args.song,
        args.difficulty,
        base_dir=inputs_path.parent,
        band_item_bp=args.band_item_bp,
        source=str(inputs_path),
    )
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    validate_with_binary(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
