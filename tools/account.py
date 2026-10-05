"""Read ournotes-account@1 exports and apply actual ownership and progression."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Mapping

SCHEMA_ACCOUNT = "ournotes-account@1"


class AccountError(ValueError):
    pass


def integer(value: Any, path: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise AccountError(f"{path} 必须是 >= {minimum} 的整数")
    return value


def indexed(
    document: Mapping[str, Any], field: str, identity: str, fields: Mapping[str, int]
) -> dict[int, dict]:
    rows = document.get(field)
    if not isinstance(rows, list):
        raise AccountError(f"account.{field} 必须是数组")
    result = {}
    for row in rows:
        if not isinstance(row, dict):
            raise AccountError(f"account.{field} 元素必须是对象")
        for name, minimum in fields.items():
            integer(row.get(name), f"account.{field}.{name}", minimum)
        key = row[identity]
        if key in result:
            raise AccountError(f"account.{field} 存在重复 ID {key}")
        result[key] = row
    return result


def validate_account(document: Any) -> dict:
    if not isinstance(document, dict) or document.get("schema") != SCHEMA_ACCOUNT:
        raise AccountError(f"账号文件必须使用 {SCHEMA_ACCOUNT}，不能使用认证凭据文件")
    if document.get("region") != "jp":
        raise AccountError("当前账号导入只支持日服 jp")
    profile = document.get("profile")
    if (
        not isinstance(profile, dict)
        or not isinstance(profile.get("profile_id"), str)
        or not profile["profile_id"].isdigit()
    ):
        raise AccountError("account.profile.profile_id 必须是数字字符串")
    indexed(
        document,
        "members",
        "master_id",
        {"master_id": 1, "exp": 0, "rank": 1, "awake": 1, "live_skill_level": 1},
    )
    indexed(document, "snapshots", "master_id", {"master_id": 1, "exp": 0, "rank": 1})
    indexed(document, "characters", "character_id", {"character_id": 1, "exp": 0})
    indexed(document, "band_items", "master_id", {"master_id": 1, "level": 0})
    integer(document.get("vip_points"), "account.vip_points")
    integer(document.get("main_deck"), "account.main_deck")
    decks = indexed(document, "decks", "id", {"id": 1})
    for deck in decks.values():
        indexed(
            deck,
            "slots",
            "index",
            {"index": 0, "member_id": 0, "snapshot_id": 0, "trigger_index": 0},
        )
    return document


def load_account(path: Path) -> dict:
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        raise AccountError(f"无法读取账号导出文件 {path}") from error
    return validate_account(document)


def row_at(rows: list[dict], group: Any, key: str, value: int) -> dict:
    matches = [row for row in rows if row.get("_group") == group and row.get(key) == value]
    if len(matches) != 1:
        raise AccountError(f"Master 缺少唯一的 group={group} {key}={value} 行，拒绝回退到满养成")
    return matches[0]


def threshold(rows: list[dict], key: str, value: int, context: str) -> dict:
    eligible = [row for row in rows if isinstance(row.get(key), int) and row[key] <= value]
    if not eligible:
        raise AccountError(f"{context} 缺少可用于 {key}={value} 的经验档位")
    return max(eligible, key=lambda row: row[key])


def trained(card: dict, rates: list[dict]) -> list[int]:
    return [
        sum(card[f"_{dimension}PowerMax"] * row[f"_{dimension}Rate"] // 10000 for row in rates)
        for dimension in ("performance", "technic", "visual")
    ]


def apply_account_catalog(tables: Any, catalog: dict, account: dict) -> dict:
    """Resolve experience against Master, never inherit ideal progression or bonuses."""
    validate_account(account)
    owned_members = {row["master_id"]: row for row in account["members"]}
    owned_snapshots = {row["master_id"]: row for row in account["snapshots"]}
    raw_members = {row["_id"]: row for row in tables.table("MasterMemberCard")}
    raw_snapshots = {row["_id"]: row for row in tables.table("MasterSupportCard")}
    for owned, raw, kind in [
        (owned_members, raw_members, "成员卡"),
        (owned_snapshots, raw_snapshots, "Snapshot"),
    ]:
        missing = set(owned) - set(raw)
        if missing:
            raise AccountError(f"Master 不包含账号持有的{kind} {sorted(missing)}，请更新 Master")
    members = []
    for card in catalog["members"]:
        state = owned_members.get(card["id"])
        if state is None:
            continue
        raw = raw_members[card["id"]]
        level_rows = [
            row
            for row in tables.table("MasterMemberCardLevel")
            if row["_group"] == raw["_memberCardLevelGroup"]
        ]
        level = threshold(level_rows, "_exp", state["exp"], "成员等级")
        rank = row_at(
            tables.table("MasterMemberCardRank"),
            raw["_memberCardRankGroup"],
            "_rank",
            state["rank"],
        )
        awake = row_at(
            tables.table("MasterMemberCardAwake"),
            raw["_memberCardAwakeGroup"],
            "_awakeCount",
            state["awake"],
        )
        card.update(
            trained=trained(raw, [level, rank, awake]),
            level=level["_level"],
            rank=state["rank"],
            awake=state["awake"],
            live_skill_level=state["live_skill_level"],
            leader_skill_level=rank["_leaderSkillLevel"],
        )
        card["card_rank_bonus_bp"].update(
            music_type=rank["_musicTypeBonusRate"], music_tag=rank["_musicTagBonusRate"]
        )
        members.append(card)
    snapshots = []
    for card in catalog["snapshots"]:
        state = owned_snapshots.get(card["id"])
        if state is None:
            continue
        raw = raw_snapshots[card["id"]]
        level_rows = [
            row
            for row in tables.table("MasterSupportCardLevel")
            if row["_group"] == raw["_supportCardLevelGroup"]
        ]
        level = threshold(level_rows, "_exp", state["exp"], "Snapshot 等级")
        rank = row_at(
            tables.table("MasterSupportCardRank"),
            raw["_supportCardRankGroup"],
            "_rank",
            state["rank"],
        )
        if level["_level"] > rank["_limitLevel"]:
            raise AccountError(f"Snapshot {card['id']} 的经验等级超过 Rank {state['rank']} 上限")
        card.update(trained=trained(raw, [level]), level=level["_level"], rank=state["rank"])
        card["support_skill_levels"] = {
            str(raw[f"_supportSkillId0{i}"]): rank[f"_supportSkill0{i}Level"]
            for i in (1, 2)
            if raw.get(f"_supportSkillId0{i}", 0)
        }
        card["card_rank_bonus_bp"]["type_link"] = rank["_cardTypeLinkBonusRate"]
        snapshots.append(card)
    catalog["members"], catalog["snapshots"] = members, snapshots
    fix = catalog["fix"]
    characters = {row["_id"] for row in tables.table("MasterCharacter")}
    state_characters = {row["character_id"]: row["exp"] for row in account["characters"]}
    if set(state_characters) - characters:
        raise AccountError("账号角色不在当前 Master 中，请更新 Master")
    ranks = {
        character: threshold(
            tables.table("MasterCharacterRank"),
            "_exp",
            state_characters.get(character, 0),
            "角色 Rank",
        )
        for character in characters
    }
    total_rank = sum(row["_rank"] for row in ranks.values())
    total = threshold(
        tables.table("MasterCharacterTotalRank"), "_totalRank", total_rank, "角色总 Rank"
    )
    fix["character_rank_bonus"] = [0] * 3
    fix["character_rank_bonus_by_character"] = {
        str(character): [row["_bonus"]] * 3 for character, row in sorted(ranks.items())
    }
    fix["character_total_rank_bonus"] = [total["_bonus"]] * 3
    vip = threshold(tables.table("MasterVip"), "_point", account["vip_points"], "VIP")
    vip_bonus = [
        row["_value"]
        for row in tables.table("MasterVipRankBonus")
        if row["_vipRank"] == vip["_vipRank"] and row["_vipBonusType"] == 7
    ]
    if len(vip_bonus) > 1:
        raise AccountError("VIP 力量加成存在重复档位")
    fix["vip_bonus_bp"] = [vip_bonus[0] if vip_bonus else 0] * 3
    bands = {row["_bandID"] for row in tables.table("MasterCharacter")}
    band_bonus = {str(band): [0] * 3 for band in bands}
    items = {row["_id"]: row for row in tables.table("MasterBandItem")}
    targets = {row["_id"]: row for row in tables.table("MasterSkillTarget")}
    for state in account["band_items"]:
        item = items.get(state["master_id"])
        if item is None:
            raise AccountError("账号乐队道具不在当前 Master 中，请更新 Master")
        if state["level"] == 0:
            continue
        effects = [
            row
            for row in tables.table("MasterBandItemSkillEffect")
            if row["_bandItemId"] == state["master_id"] and row["_level"] == state["level"]
        ]
        if not effects:
            raise AccountError(f"乐队道具 {state['master_id']} 缺少等级 {state['level']} 的效果")
        for effect in effects:
            if effect["_skillEffectType"] != 1000 or not effect["_skillTargetIDs"]:
                raise AccountError("当前模型不支持此乐队道具效果")
            effect_bands = set()
            for target_id in effect["_skillTargetIDs"]:
                target = targets.get(target_id)
                if (
                    target is None
                    or target.get("_skillTargetType") != 3
                    or target.get("_bandID") not in bands
                    or any(
                        target.get(key, 0)
                        for key in [
                            "_characterID",
                            "_cardType",
                            "_tagID",
                            "_liveMusicType",
                            "_gekisouMissionType",
                        ]
                    )
                    or target.get("_liveSkillCategories")
                    or target.get("_gekisouSkillCategories")
                ):
                    raise AccountError("当前模型只支持按乐队生效的道具力量加成")
                effect_bands.add(target["_bandID"])
            for band in effect_bands:
                band_bonus[str(band)] = [
                    value + effect["_effectValue"] for value in band_bonus[str(band)]
                ]
    fix["band_item_bonus_bp"] = [0] * 3
    fix["band_item_bonus_bp_by_band"] = band_bonus
    return {
        "profile_id": account["profile"]["profile_id"],
        "exported_at": account.get("exported_at"),
        "members": len(members),
        "snapshots": len(snapshots),
        "character_total_rank": total_rank,
        "vip_rank": vip["_vipRank"],
    }


def formation_for_deck(account: dict, deck_id: int | None = None) -> dict:
    validate_account(account)
    deck_id = account["main_deck"] if deck_id is None else deck_id
    deck = next((row for row in account["decks"] if row["id"] == deck_id), None)
    if deck is None:
        raise AccountError(f"账号中没有编队 {deck_id}")
    slots = sorted(deck["slots"], key=lambda row: row["index"])
    if [row["index"] for row in slots] != list(range(5)) or sorted(
        row["trigger_index"] for row in slots
    ) != list(range(5)):
        raise AccountError("保存编队必须有 5 个槽位和完整技能顺序")
    members = {row["master_id"] for row in account["members"]}
    snapshots = {row["master_id"] for row in account["snapshots"]}
    if any(row["member_id"] not in members or row["snapshot_id"] not in snapshots for row in slots):
        raise AccountError("保存编队不完整或引用了未持有的卡牌")
    return {
        "schema": "ournotes-deck-formation@1",
        "leader": slots[0]["member_id"],
        "slots": [
            {
                "member": row["member_id"],
                "snapshot": row["snapshot_id"],
                "trigger": row["trigger_index"] + 1,
            }
            for row in slots
        ],
    }
