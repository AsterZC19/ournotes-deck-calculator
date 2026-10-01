# `inputs.json` data semantics — exact reference

Source of truth (read-only, not modified by this document):

- `\\wsl.localhost\Debian\home\Minus\projects\work\work\our-notes-event-20260930\inputs.json` (4,756,638 bytes)
- Consumer under study: `.../our-notes-event-20260930/model.py`
- Producer: `.../our-notes-event-20260930/prepare.py` (imports `chart_converter.py`, MIT, from `empty-sekai/nnnotes`)

Every number, field name, row and row-count below was read out of the real file. Nothing is inferred
unless it is explicitly marked **unverified** or **modelling assumption**.

Conventions used here: `_`-prefixed names are the raw Master column names. `bp` means "basis
points" as used by `model.py` (i.e. a value that gets divided by 10000 after multiplication).

---

## 1. Top-level shape

`inputs.json` is a single JSON object with exactly six keys:

```text
{
  "event":   { ... },          // object, 22 fields — tables["MasterEvent"][0]
  "members": [ ... ],          // array, 63 rows
  "snaps":   [ ... ],          // array, 64 rows
  "songs":   [ ... ],          // array,  3 rows
  "tables":  { ... },          // object, 36 tables
  "ideal":   { ... }           // object,  4 fields
}
```

### 1.1 `tables` — every table and its row count

36 tables, in file order.

| # | table | rows |
|---:|---|---:|
| 1 | `MasterEvent` | 1 |
| 2 | `MasterChallengeMusic` | 3 |
| 3 | `MasterEventEffect` | 20 |
| 4 | `MasterLiveMusic` | 85 |
| 5 | `MasterLiveMusicScore` | 340 |
| 6 | `MasterMemberCard` | 63 |
| 7 | `MasterMemberCardLevel` | 330 |
| 8 | `MasterMemberCardRank` | 5 |
| 9 | `MasterMemberCardAwake` | 5 |
| 10 | `MasterSupportCard` | 64 |
| 11 | `MasterSupportCardLevel` | 420 |
| 12 | `MasterSupportCardRank` | 25 |
| 13 | `MasterCharacter` | 25 |
| 14 | `MasterCharacterRank` | 50 |
| 15 | `MasterCharacterTotalRank` | 57 |
| 16 | `MasterBandItemSkillEffect` | 1250 |
| 17 | `MasterBandItemLevel` | 750 |
| 18 | `MasterVipRankBonus` | 125 |
| 19 | `MasterText` | 9928 |
| 20 | `MasterLiveSkill` | 8 |
| 21 | `MasterLiveSkillEffect` | 50 |
| 22 | `MasterLeaderSkill` | 220 |
| 23 | `MasterLeaderSkillEffect` | 1145 |
| 24 | `MasterSupportSkill` | 63 |
| 25 | `MasterSupportSkillEffect` | 615 |
| 26 | `MasterSkillCondition` | 308 |
| 27 | `MasterSkillTarget` | 56 |
| 28 | `MasterParameter` | 76 |
| 29 | `MasterLiveSettings` | 62 |
| 30 | `MasterLiveNoteParameter` | 16 |
| 31 | `MasterLiveComboScoreBonus` | 90 |
| 32 | `MasterMemoryMemberLevel` | **0** (empty array) |
| 33 | `MasterMemorySupportLevel` | **0** (empty array) |
| 34 | `MasterMemoryMusicBonus` | **0** (empty array) |
| 35 | `MasterSkillConditionSet` | 417 |
| 36 | `MasterGekisouSkill` | 22 |

Note: `MasterMemoryMemberLevel`, `MasterMemorySupportLevel` and `MasterMemoryMusicBonus` are present
but **empty**, so no memory/member-level data is available in this dataset at all.

---

## 2. `ideal` block — verbatim

```json
{
  "character_rank": {
    "_id": 50,
    "_rank": 50,
    "_exp": 310,
    "_bonus": 245
  },
  "character_total_rank": 1250,
  "character_total_bonus": {
    "_id": 19,
    "_totalRank": 1200,
    "_bonus": 90
  },
  "vip_power_bonus_bp": 2000
}
```

Origin (from `prepare.py` lines 122–129):

- `character_rank` = `max(MasterCharacterRank, key=_rank)` → the `_rank == 50` row, `_bonus == 245`. `MasterCharacterRank` has 50 rows.
- `character_total_rank` = `len(MasterCharacter) * max(_rank)` = `25 * 50` = **1250**.
- `character_total_bonus` = `max(row for row in MasterCharacterTotalRank if row["_totalRank"] <= 1250)`.
  `MasterCharacterTotalRank` has rows at `_totalRank` 25, …, 5000 in steps of 100 (plus 25). There is
  **no `_totalRank == 1250` row**; the greatest row `<= 1250` is `_totalRank = 1200` with `_bonus = 90`.
  The next row up is `_id 20, _totalRank 1300, _bonus 95`. This floored lookup is a **modelling
  assumption** of `prepare.py`, not a game constant.
- `vip_power_bonus_bp` = `max(_value for row in MasterVipRankBonus if row["_vipBonusType"] == 7)` = **2000**.
  `MasterVipRankBonus` has 125 rows; `_vipBonusType` distribution is `{7: 20, 1: 20, 5: 20, 9: 19, 2: 14, 3: 14, 6: 13, 8: 5}`.

---

## 3. `event` row — verbatim

`event` is a single object (`tables["MasterEvent"][0]`, the only row), 22 fields:

```json
{
  "_backgroundAsset": "01/Top/event_top_01_0001",
  "_bannerAsset": "Banner_1",
  "_carouselHelpId": 0,
  "_challengeLiveEventPointGroup": 1,
  "_challengeLiveEventRewardGroup": 2,
  "_displayEndAt": "2026/10/10 20:59:59",
  "_endAt": "2026/10/08 20:59:59",
  "_eventItemId": 43,
  "_eventType": 1,
  "_id": 1,
  "_imageAsset": "",
  "_isMusicRankingDisabled": false,
  "_isRankingDisabled": true,
  "_isTotalMusicRankingDisabled": false,
  "_liveEventPointGroup": 1,
  "_liveEventRewardGroup": 2,
  "_logoAsset": "01/Logo/event_logo_01_0001",
  "_musicId": 100109,
  "_nameTextId": "Event_Name_0001",
  "_startAt": "2026/09/30 18:00:00",
  "_storyChapterId": 6
}
```

Note: both `_startAt`/`_endAt` in `MasterEvent` use `YYYY/MM/DD HH:MM:SS`, whereas the `start` field
that `prepare.py` copies into `members[]`/`snaps[]` uses `YYYY-MM-DD HH:MM:SS`, and one snap value is
malformed (`"2026-01-01 0:00:00"`, single-digit hour — see §4.2). Timestamps are **strings, not
parsed**; nothing in `model.py` reads them.

---
## 4. `members[]` and `snaps[]`

### 4.1 `members[]` — exact field list (18 fields, 63 rows)

| field | type | notes |
|---|---|---|
| `id` | int | `MasterMemberCard._id` |
| `name` | string | Japanese text resolved through `MasterText` |
| `title` | string | card subtitle |
| `character` | int | `_characterID`, 1..25 |
| `band` | int | `MasterCharacter[_characterID]._bandID`, 1..5 |
| `type` | int | `_cardType`, 1..5 |
| `rarity` | int | 2, 3 or 4 |
| `base` | int[3] | `[_performancePowerMax, _technicPowerMax, _visualPowerMax]` |
| `trained` | int[3] | `base*levelRate//10000 + base*rankRate//10000 + base*awakeRate//10000`, summed per component |
| `rank` | object | the `MasterMemberCardRank` row with max `_rank` in the card's group (10 fields) |
| `awake` | object | the `MasterMemberCardAwake` row with max `_awakeCount` in the group (6 fields) |
| `level` | object | the `MasterMemberCardLevel` row with max `_level` in the group (7 fields) |
| `live_skill` | int | `_liveSkillID`, 1..7 in this dataset |
| `leader_skill` | int | `_leaderSkillID` |
| `tags` | int[] | `_bestMusicTagIDs` — in this dataset always `[3]` for members with a tag |
| `event_bonus_bp` | int | sum of `_rank5EffectValue` over matching `MasterEventEffect` rows with `_resourceTypeConstraint == 2` and `_eventBonusType == 2` |
| `event_rules` | int[] | the `_id`s of those `MasterEventEffect` rows |
| `start` | string | `_startAt` |

Sub-object field lists (verbatim from the rows):

- `rank`: `_id, _group, _rank, _requiredRankUpItemCount, _performanceRate, _technicRate, _visualRate, _leaderSkillLevel, _musicTypeBonusRate, _musicTagBonusRate`
- `awake`: `_id, _group, _awakeCount, _performanceRate, _technicRate, _visualRate`
- `level`: `_id, _group, _level, _exp, _performanceRate, _technicRate, _visualRate`

#### Complete example member row — `id == 61` (highest `event_bonus_bp` tier)

```json
{
  "id": 61,
  "name": "千石 ユノ",
  "title": "ハート・リブート",
  "character": 15,
  "band": 3,
  "type": 2,
  "rarity": 4,
  "base": [
    38776,
    30654,
    31702
  ],
  "trained": [
    46530,
    36784,
    38042
  ],
  "rank": {
    "_id": 5,
    "_group": 1,
    "_rank": 5,
    "_requiredRankUpItemCount": 240,
    "_performanceRate": 1000,
    "_technicRate": 1000,
    "_visualRate": 1000,
    "_leaderSkillLevel": 5,
    "_musicTypeBonusRate": 2000,
    "_musicTagBonusRate": 2000
  },
  "awake": {
    "_id": 5,
    "_group": 1,
    "_awakeCount": 5,
    "_performanceRate": 1000,
    "_technicRate": 1000,
    "_visualRate": 1000
  },
  "level": {
    "_id": 240,
    "_group": 3,
    "_level": 90,
    "_exp": 1006950,
    "_performanceRate": 10000,
    "_technicRate": 10000,
    "_visualRate": 10000
  },
  "live_skill": 5,
  "leader_skill": 53,
  "tags": [
    3
  ],
  "event_bonus_bp": 10000,
  "event_rules": [
    2,
    8,
    10
  ],
  "start": "2026-09-30 18:00:00"
}
```

`event_bonus_bp = 10000` decomposes exactly as `MasterEventEffect` rows 2 + 8 + 10 at rank 5:

| rule `_id` | constraint | `_rank5EffectValue` |
|---:|---|---:|
| 2 | `_memberCardId == 61` | 5000 |
| 8 | `_bandId == 3` | 3200 |
| 10 | `_cardType == 2` | 1800 |
| | **sum** | **10000** |

#### Member statistics (exact)

- Members with **non-zero** `event_bonus_bp`: **23 of 63**.
  `(id, event_bonus_bp)`:
  `(3,1800) (8,1800) (11,5000) (12,3200) (13,3200) (14,3200) (15,3200) (16,1800) (22,1800) (26,1800) (32,1800) (36,3200) (37,3200) (38,3200) (39,5000) (40,3200) (42,1800) (49,1800) (55,1800) (59,1800) (61,10000) (62,10000) (63,7500)`
  Distinct values: `{1800, 3200, 5000, 7500, 10000}`. Only member ids 61 and 62 reach **10000 bp**.
- Distinct member `type` values: `{1, 2, 3, 4, 5}` with counts `{2: 15, 5: 12, 1: 12, 4: 12, 3: 12}`.
- Distinct member `band` values: `{1, 2, 3, 4, 5}` with counts `{1: 15, 2: 15, 3: 13, 4: 10, 5: 10}`.
- Member `rarity` counts: `{3: 26, 2: 25, 4: 12}`.
- `trained` min / max per component (all 63 rows, every row has exactly 3 components):
  - `trained[0]` performance: min **8551**, max **47796**
  - `trained[1]` technic: min **8382**, max **47569**
  - `trained[2]` visual: min **8141**, max **47071**
- Distinct `live_skill` ids used by members: `{1, 2, 3, 4, 5, 6, 7}` (Live skill 8 is never used by a member).
- Distinct `leader_skill` ids used by members: **52** distinct values in `[1..54]`, all present in `MasterLeaderSkill`.

### 4.2 `snaps[]` — exact field list (15 fields, 64 rows)

| field | type | notes |
|---|---|---|
| `id` | int | `MasterSupportCard._id` |
| `name` | string | |
| `title` | string | |
| `characters` | int[] | `_characterIDs` |
| `bands` | int[] | `sorted({MasterCharacter[c]._bandID for c in _characterIDs})` |
| `type` | int | `_cardType` |
| `rarity` | int | 2, 3, 4, or `10` for exactly one row |
| `base` | int[3] | `[_performancePowerMax, _technicPowerMax, _visualPowerMax]` |
| `trained` | int[3] | `base*levelRate//10000` per component (no rank/awake terms) |
| `rank` | object | max-`_rank` `MasterSupportCardRank` row in the group (10 fields) |
| `level` | object | max-`_level` `MasterSupportCardLevel` row in the group (7 fields) |
| `support_skills` | int[2] | `[_supportSkillId01, _supportSkillId02]`; slot 2 is `0` (sentinel) for 63 of 64 snaps |
| `event_bonus_bp` | int | same construction as members, but `_resourceTypeConstraint == 3` |
| `event_rules` | int[] | |
| `start` | string | `_startAt` |

Sub-object field lists: `rank`: `_id, _group, _rank, _limitLevel, _requiredRankUpItemCount, _supportSkill01Level, _supportSkill02Level, _gekisouSupportSkill01Level, _gekisouSupportSkill02Level, _cardTypeLinkBonusRate`.
`level`: `_id, _group, _level, _exp, _performanceRate, _technicRate, _visualRate`.

#### Complete example snap row — `id == 61` (the only two-skill snap)

```json
{
  "id": 61,
  "name": "燈&オブリビオニス&あられ&蛍&蕾叶",
  "title": "事前登録KV",
  "characters": [
    1,
    10,
    11,
    16,
    21
  ],
  "bands": [
    1,
    2,
    3,
    4,
    5
  ],
  "type": 2,
  "rarity": 10,
  "base": [
    1400,
    1200,
    1300
  ],
  "trained": [
    1400,
    1200,
    1300
  ],
  "rank": {
    "_id": 25,
    "_group": 5,
    "_rank": 5,
    "_limitLevel": 90,
    "_requiredRankUpItemCount": 1,
    "_supportSkill01Level": 5,
    "_supportSkill02Level": 5,
    "_gekisouSupportSkill01Level": 5,
    "_gekisouSupportSkill02Level": 5,
    "_cardTypeLinkBonusRate": 2000
  },
  "level": {
    "_id": 420,
    "_group": 5,
    "_level": 90,
    "_exp": 1006950,
    "_performanceRate": 10000,
    "_technicRate": 10000,
    "_visualRate": 10000
  },
  "support_skills": [
    71,
    73
  ],
  "event_bonus_bp": 5000,
  "event_rules": [
    18,
    20
  ],
  "start": "2026-01-01 0:00:00"
}
```

Second complete example snap with non-zero `event_bonus_bp` — `id == 3` (single-band, one-skill):

```json
{
  "id": 3,
  "name": "要 楽奈",
  "title": "猫、ふらり",
  "characters": [
    3
  ],
  "bands": [
    1
  ],
  "type": 2,
  "rarity": 2,
  "base": [
    500,
    500,
    500
  ],
  "trained": [
    500,
    500,
    500
  ],
  "rank": {
    "_id": 5,
    "_group": 1,
    "_rank": 5,
    "_limitLevel": 70,
    "_requiredRankUpItemCount": 1,
    "_supportSkill01Level": 5,
    "_supportSkill02Level": 5,
    "_gekisouSupportSkill01Level": 5,
    "_gekisouSupportSkill02Level": 5,
    "_cardTypeLinkBonusRate": 2000
  },
  "level": {
    "_id": 70,
    "_group": 1,
    "_level": 70,
    "_exp": 494900,
    "_performanceRate": 10000,
    "_technicRate": 10000,
    "_visualRate": 10000
  },
  "support_skills": [
    1,
    0
  ],
  "event_bonus_bp": 1800,
  "event_rules": [
    20
  ],
  "start": "2026-01-01 0:00:00"
}
```

#### Snap statistics (exact)

- Snaps with **non-zero** `event_bonus_bp`: **25 of 64**, values `{1800, 3200, 5000, 7500, 10000}`.
  - `10000` bp: snaps **62** (千石 ユノ) and **63** (あられ&ののか&律&都子) — `event_rules` `[12, 20]` / `[14, 20]`.
  - `7500` bp: snap **64** (都子＆律).
  - `5000` bp: snaps **11**, **37**, **61**.
  - `3200` bp: snaps **12, 13, 14, 15, 36, 38, 39, 40, 55, 56**.
  - `1800` bp: snaps **3, 8, 16, 22, 29, 33, 41, 46, 52**.
- Distinct snap `type` values: `{1, 2, 3, 4, 5}` with counts `{2: 15, 5: 13, 1: 13, 3: 12, 4: 11}`.
- Snap `rarity` counts: `{3: 26, 2: 25, 4: 12, 10: 1}` — the `10` is snap 61 only.
- Distinct snap `bands` shapes (multiset, sorted):
  `[1]` ×12, `[2]` ×12, `[3]` ×15, `[4]` ×12, `[5]` ×12, `[1,2,3,4,5]` ×1 (snap 61).
  There are **no** two-band or three-band snaps in this dataset except the single all-band snap 61.
- Snaps whose `support_skills[1] != 0`: **1 of 64** — snap **61**, `[71, 73]`. Every other snap is `[k, 0]`
  with `k` in `{1..15, 31..35, 37, 38, 51..56, 59, 60}`.

---
## 5. `songs[]`

### 5.1 Exact fields

`songs[]` has 3 rows, 7 fields each: `id, title, type, bands, tags, challenge, charts`.
`charts[]` entries have 9 fields: `difficulty, master, path, source_path, source_sha256, judgement_note_count, skill_times_ms, runtime_sha256, counts`.

- `challenge` = the whole `MasterChallengeMusic` row for the song.
- `master` = the whole `MasterLiveMusicScore` row for that difficulty.
- `counts` = note-op histogram of the converted chart (`chart_converter.convert` output).
- `path` is relative to the source directory; the runtime chart files live in `.../our-notes-event-20260930/charts/`.

### 5.2 All songs — `(id, title, type, bands, tags)`

```
(100109, "夢我夢中",                      2, [3], [3])
(100056, "これはぼくたちの生存のあらすじ", 2, [3], [3])
(100063, "オリオンをなぞる",              2, [3], [3])
```

All three songs are `_musicType == 2`, `_bandIDs == [3]`, `_bestMusicTagIDs == [3]` — i.e. the three
songs are **not distinguishable** by the `type`/`bands`/`tags` features that `model.py` consumes.
Only chart timing differs.

### 5.3 Full `charts[]` entry — song `100109`, difficulty `expert` (verbatim)

```json
{
  "difficulty": "expert",
  "master": {
    "_id": 10010903,
    "_musicScoreTextFileName": "0109/0109_03",
    "_musicScoreLevel": 26,
    "_musicScoreDisplayLevel": 26.0,
    "_fullComboCount": 759
  },
  "path": "charts/0109_03.json",
  "source_path": "/home/Minus/projects/work/sirius-asset-updater/downloads/new-pack-20260929/highlights/event-detail/charts/0109/0109_03.json",
  "source_sha256": "5531cded9fd42c26a9ab852f8d96258250a09c77f8558e1f914eba655d29497c",
  "judgement_note_count": 759,
  "skill_times_ms": [
    9411,
    21960,
    47058,
    59607,
    84705
  ],
  "runtime_sha256": "b951d1724b79632484722f74fd0ae6352385ee318f4c2912f59c617f7da1df2a",
  "counts": {
    "Normal": 258,
    "SlideBegin": 137,
    "SlideEnd": 78,
    "Flick": 14,
    "SlideEndFlick": 8,
    "Trace": 31,
    "SlideEndTrace": 43,
    "HiddenSlideBegin": 3,
    "HiddenSlideEnd": 5,
    "GuideBeginNormal": 3,
    "GuideEnd": 3,
    "Combo": 187,
    "ComboSkip": 1,
    "Hidden": 280
  }
}
```

### 5.4 All 12 chart entries — key values

| song id | difficulty | `judgement_note_count` | `_fullComboCount` | `_musicScoreLevel` | `skill_times_ms` |
|---|---:|---:|---:|---:|---|
| 100109 | easy | 272 | 272 | 7 | 9411, 21960, 47058, 59607, 84705 |
| 100109 | normal | 350 | 350 | 15 | 9411, 21960, 47058, 59607, 84705 |
| 100109 | hard | 600 | 600 | 19 | 9411, 21960, 47058, 59607, 84705 |
| 100109 | expert | 759 | 759 | 26 | 9411, 21960, 47058, 59607, 84705 |
| 100056 | easy | 364 | 364 | 8 | 7382, 16178, 36282, 61413, 81518 |
| 100056 | normal | 488 | 488 | 16 | 7382, 16178, 36282, 61413, 81518 |
| 100056 | hard | 651 | 651 | 21 | 7382, 16178, 36282, 61413, 81518 |
| 100056 | expert | 843 | 843 | 28 | 7382, 16178, 36282, 61413, 81518 |
| 100063 | easy | 304 | 304 | 9 | 13186, 36923, 55219, 65934, 87032 |
| 100063 | normal | 492 | 492 | 17 | 13186, 36923, 55219, 65934, 87032 |
| 100063 | hard | 615 | 615 | 22 | 13186, 36923, 55219, 65934, 87032 |
| 100063 | expert | 718 | 718 | 29 | 13186, 36923, 55219, 65934, 87032 |

There are exactly **5 `skill_times_ms`** per chart (not 4), matching the 5 skill slots the model scores.
Chart file names: `0109_00..03`, `0056_00..03`, `0063_00..03`.

### 5.5 Converted chart runtime shape (needed to read `model.py`)

`charts/*.json` top-level keys:
`format, mirror, startNoteId, laneCount, judgementNoteCount, counts, notes, lines, bpmChanges, barChanges, tickSegments, barLineTimeMs, fever, skill, call, lastNoteTimeMs, log`.

For `charts/0109_03.json` (100109 expert): `len(notes) == 1051`, of which **759** have `judgement == true`
(so 292 are decorative: slide ends / hidden / guide notes). Note fields include
`id, op, opName, timeMs, bar, rhythm, rhythmicUnit, barProgress, laneStart, laneEnd, ..., judgement, ...`.

`judgement` is a **JSON boolean**, not an enum. `model.py` treats it as truthy/falsy
(the raw field histogram is `{true: 759, false: 292}` for this chart).

Op histogram (all 1051 notes) vs. (the 759 judged notes):

```
all:    122:280, 1:258, 120:187, 20:137, 22:78, 62:43, 60:31, 40:14, 42:8, 82:5, 80:3, 101:3, 103:3, 121:1
judged:   1:258, 120:187, 20:137, 22:78, 62:43, 60:31, 40:14, 42:8, 101:3
```

---

## 6. `tables` semantics

### 6.1 `MasterLiveSettings` — 62 rows, all rows verbatim

Fields: `_id` (int), `_key` (string), `_value` (string), `_subValue` (string, **always `""` in all 62 rows**).

`_id` runs 1..66 with **49, 50, 51, 52 missing** (62 rows present).

```json
{"_id": 1, "_key": "life_base", "_value": "1000", "_subValue": ""}
{"_id": 2, "_key": "life_denger", "_value": "300", "_subValue": ""}
{"_id": 3, "_key": "flick_distance", "_value": "0.2", "_subValue": ""}
{"_id": 4, "_key": "note_speed_min", "_value": "1", "_subValue": ""}
{"_id": 5, "_key": "note_speed_max", "_value": "12", "_subValue": ""}
{"_id": 6, "_key": "note_speed_view_min", "_value": "4", "_subValue": ""}
{"_id": 7, "_key": "note_speed_view_max", "_value": "0.35", "_subValue": ""}
{"_id": 8, "_key": "note_slide_combo_rhythmic_unit", "_value": "8", "_subValue": ""}
{"_id": 9, "_key": "note_score_adjustment_factor", "_value": "3", "_subValue": ""}
{"_id": 10, "_key": "gekisou_judgement_accuracy_one_percent_ms", "_value": "3", "_subValue": ""}
{"_id": 11, "_key": "gekisou_just_timing_skill_base_factor", "_value": "8", "_subValue": ""}
{"_id": 12, "_key": "motion_speed_base_bpm", "_value": "190", "_subValue": ""}
{"_id": 13, "_key": "motion_speed_delta_per_bpm", "_value": "0.0025", "_subValue": ""}
{"_id": 14, "_key": "IPhone13_greaterEqual_input_offset_ms", "_value": "20", "_subValue": ""}
{"_id": 15, "_key": "gekisou_luck_gauge_max", "_value": "140", "_subValue": ""}
{"_id": 16, "_key": "gekisou_luck_gauge_max_rush", "_value": "70", "_subValue": ""}
{"_id": 17, "_key": "gekisou_luck_rush_score_bonus_percent", "_value": "10", "_subValue": ""}
{"_id": 18, "_key": "assist_gauge_point_max", "_value": "100", "_subValue": ""}
{"_id": 19, "_key": "assist_great_point", "_value": "25", "_subValue": ""}
{"_id": 20, "_key": "assist_good_point", "_value": "60", "_subValue": ""}
{"_id": 21, "_key": "assist_bad_point", "_value": "80", "_subValue": ""}
{"_id": 22, "_key": "assist_miss_point", "_value": "100", "_subValue": ""}
{"_id": 23, "_key": "assist_level_down_perfect_count", "_value": "20", "_subValue": ""}
{"_id": 24, "_key": "assist_score_percent", "_value": "90", "_subValue": ""}
{"_id": 25, "_key": "slide_offset_min_note_lane_width", "_value": "4", "_subValue": ""}
{"_id": 26, "_key": "slide_offset_max_note_lane_width", "_value": "5", "_subValue": ""}
{"_id": 27, "_key": "live_side_monitor_switch_interval", "_value": "8", "_subValue": ""}
{"_id": 28, "_key": "live_stage_light_speed_low", "_value": "0.75", "_subValue": ""}
{"_id": 29, "_key": "live_stage_light_speed_mid", "_value": "1", "_subValue": ""}
{"_id": 30, "_key": "live_stage_light_speed_high", "_value": "1.5", "_subValue": ""}
{"_id": 31, "_key": "live_stage_light_speed_very_low", "_value": "0.5", "_subValue": ""}
{"_id": 32, "_key": "live_stage_light_speed_very_high", "_value": "2", "_subValue": ""}
{"_id": 33, "_key": "live_finish_direction_duration", "_value": "3", "_subValue": ""}
{"_id": 34, "_key": "live_combo_call_disableLast_n_notes", "_value": "50", "_subValue": ""}
{"_id": 35, "_key": "note_score_life_onus_factor", "_value": "0.3", "_subValue": ""}
{"_id": 36, "_key": "stop_in_vain_time_ms_diff", "_value": "200", "_subValue": ""}
{"_id": 37, "_key": "note_overlap_lane_buffer", "_value": "4", "_subValue": ""}
{"_id": 38, "_key": "tutorial_sound_tap", "_value": "1000100", "_subValue": ""}
{"_id": 39, "_key": "tutorial_music_score_file_name_tap", "_value": "tut_tap", "_subValue": ""}
{"_id": 40, "_key": "tutorial_sound_flick", "_value": "1000101", "_subValue": ""}
{"_id": 41, "_key": "tutorial_music_score_file_name_flick", "_value": "tut_flick", "_subValue": ""}
{"_id": 42, "_key": "tutorial_sound_slide", "_value": "1000102", "_subValue": ""}
{"_id": 43, "_key": "tutorial_music_score_file_name_slide", "_value": "tut_slide", "_subValue": ""}
{"_id": 44, "_key": "tutorial_sound_gekisou", "_value": "1000103", "_subValue": ""}
{"_id": 45, "_key": "tutorial_music_score_file_name_gekisou", "_value": "tutorial_gekisou", "_subValue": ""}
{"_id": 46, "_key": "tutorial_carouse_note_summary_id", "_value": "7", "_subValue": ""}
{"_id": 47, "_key": "tutorial_carouse_gekisou_id", "_value": "8", "_subValue": ""}
{"_id": 48, "_key": "tutorial_start_direction_sound", "_value": "100017", "_subValue": ""}
{"_id": 53, "_key": "tutorial_sound_bgm_id", "_value": "1200000000001", "_subValue": ""}
{"_id": 54, "_key": "tutorial_video_enjoy_mode", "_value": "4202000001", "_subValue": ""}
{"_id": 55, "_key": "tutorial_video_concentrate_mode", "_value": "4202000002", "_subValue": ""}
{"_id": 56, "_key": "tutorial_concentrate_note_effect_skin_id", "_value": "2", "_subValue": ""}
{"_id": 57, "_key": "tutorial_guide_freeze_times_tap", "_value": "0", "_subValue": ""}
{"_id": 58, "_key": "tutorial_guide_freeze_times_flick", "_value": "0,3000,4000", "_subValue": ""}
{"_id": 59, "_key": "tutorial_guide_freeze_times_slide", "_value": "0,4000", "_subValue": ""}
{"_id": 60, "_key": "tutorial_guide_freeze_times_gekisou", "_value": "5200,8000", "_subValue": ""}
{"_id": 61, "_key": "tutorial_music_score_file_name_tap_practice", "_value": "tut_tap2", "_subValue": ""}
{"_id": 62, "_key": "tutorial_music_score_file_name_flick_practice", "_value": "tut_flick2", "_subValue": ""}
{"_id": 63, "_key": "tutorial_music_score_file_name_slide_practice", "_value": "tut_slide2", "_subValue": ""}
{"_id": 64, "_key": "tutorial_sound_timing_adjust", "_value": "1500", "_subValue": ""}
{"_id": 65, "_key": "tutorial_music_score_file_name_timing_adjust", "_value": "timing_adjust", "_subValue": ""}
{"_id": 66, "_key": "gekisou_live_ingame_load_timeout_sec", "_value": "30", "_subValue": ""}
```

Score-relevant settings a general calculator would need (none of these are read by `model.py`):

| `_key` | `_value` |
|---|---|
| `life_base` | `1000` |
| `life_denger` | `300` |
| `note_score_adjustment_factor` | `3` |
| `note_score_life_onus_factor` | `0.3` |
| `note_slide_combo_rhythmic_unit` | `8` |
| `assist_score_percent` | `90` |
| `gekisou_luck_rush_score_bonus_percent` | `10` |
| `gekisou_luck_gauge_max` | `140` |
| `gekisou_luck_gauge_max_rush` | `70` |
| `gekisou_just_timing_skill_base_factor` | `8` |
| `stop_in_vain_time_ms_diff` | `200` |
| `live_combo_call_disableLast_n_notes` | `50` |

All `_value`s are **strings**, even numeric ones; `_subValue` is always `""`.

### 6.2 `MasterLiveNoteParameter` — 16 rows, all rows verbatim

Fields: `_id` (int), `_noteOperateType` (int), `_scorePercent` (int, this dataset: only `100` or `10`).

```json
{"_id": 1, "_noteOperateType": 1, "_scorePercent": 100}
{"_id": 2, "_noteOperateType": 20, "_scorePercent": 100}
{"_id": 3, "_noteOperateType": 21, "_scorePercent": 10}
{"_id": 4, "_noteOperateType": 22, "_scorePercent": 100}
{"_id": 5, "_noteOperateType": 40, "_scorePercent": 100}
{"_id": 6, "_noteOperateType": 41, "_scorePercent": 100}
{"_id": 7, "_noteOperateType": 42, "_scorePercent": 100}
{"_id": 8, "_noteOperateType": 60, "_scorePercent": 10}
{"_id": 9, "_noteOperateType": 120, "_scorePercent": 10}
{"_id": 10, "_noteOperateType": 61, "_scorePercent": 10}
{"_id": 11, "_noteOperateType": 62, "_scorePercent": 10}
{"_id": 12, "_noteOperateType": 63, "_scorePercent": 10}
{"_id": 13, "_noteOperateType": 101, "_scorePercent": 100}
{"_id": 14, "_noteOperateType": 102, "_scorePercent": 100}
{"_id": 15, "_noteOperateType": 104, "_scorePercent": 10}
{"_id": 16, "_noteOperateType": 105, "_scorePercent": 10}
```

- `_scorePercent == 100` for ops `{1, 20, 22, 40, 41, 42, 101, 102}` (**8 rows**).
- `_scorePercent == 10` for ops `{21, 60, 120, 61, 62, 63, 104, 105}` (**8 rows**).
- Ops present in the table but **never used** by any of the 12 charts: `{21, 41, 61, 63, 102, 104, 105}`.
- Ops used by charts but **absent** from the table: `{100: 11, 103: 35, 121: 1, 122: 592, 80: 3, 82: 6}` (counts across all 12 charts). None of those is judged, so `model.py`'s `noteparam[n['op']]` lookup (which only runs on judged notes) never raises `KeyError` here.

### 6.3 `MasterLiveComboScoreBonus` — 90 rows, all rows verbatim

Fields: `_id` (int), `_comboBonusType` (int), `_requiredComboCount` (int), `_bonusFactor` (float).

`_comboBonusType` distribution: `{0: 50, 1: 40}`. **`model.py` only reads type `0`.**

Type `0` (the branch `model.py` sums): thresholds 10..100 step 10 → `0.01`; 110..500 step 10 → `0.005`.
Type `1` (never read): thresholds 10..200 step 10 → `0.01`; 210..400 step 10 → `0.005`.

```json
{"_id": 1, "_comboBonusType": 0, "_requiredComboCount": 10, "_bonusFactor": 0.01}
{"_id": 2, "_comboBonusType": 0, "_requiredComboCount": 20, "_bonusFactor": 0.01}
{"_id": 3, "_comboBonusType": 0, "_requiredComboCount": 30, "_bonusFactor": 0.01}
{"_id": 4, "_comboBonusType": 0, "_requiredComboCount": 40, "_bonusFactor": 0.01}
{"_id": 5, "_comboBonusType": 0, "_requiredComboCount": 50, "_bonusFactor": 0.01}
{"_id": 6, "_comboBonusType": 0, "_requiredComboCount": 60, "_bonusFactor": 0.01}
{"_id": 7, "_comboBonusType": 0, "_requiredComboCount": 70, "_bonusFactor": 0.01}
{"_id": 8, "_comboBonusType": 0, "_requiredComboCount": 80, "_bonusFactor": 0.01}
{"_id": 9, "_comboBonusType": 0, "_requiredComboCount": 90, "_bonusFactor": 0.01}
{"_id": 10, "_comboBonusType": 0, "_requiredComboCount": 100, "_bonusFactor": 0.01}
{"_id": 11, "_comboBonusType": 0, "_requiredComboCount": 110, "_bonusFactor": 0.005}
{"_id": 12, "_comboBonusType": 0, "_requiredComboCount": 120, "_bonusFactor": 0.005}
{"_id": 13, "_comboBonusType": 0, "_requiredComboCount": 130, "_bonusFactor": 0.005}
{"_id": 14, "_comboBonusType": 0, "_requiredComboCount": 140, "_bonusFactor": 0.005}
{"_id": 15, "_comboBonusType": 0, "_requiredComboCount": 150, "_bonusFactor": 0.005}
{"_id": 16, "_comboBonusType": 0, "_requiredComboCount": 160, "_bonusFactor": 0.005}
{"_id": 17, "_comboBonusType": 0, "_requiredComboCount": 170, "_bonusFactor": 0.005}
{"_id": 18, "_comboBonusType": 0, "_requiredComboCount": 180, "_bonusFactor": 0.005}
{"_id": 19, "_comboBonusType": 0, "_requiredComboCount": 190, "_bonusFactor": 0.005}
{"_id": 20, "_comboBonusType": 0, "_requiredComboCount": 200, "_bonusFactor": 0.005}
{"_id": 21, "_comboBonusType": 0, "_requiredComboCount": 210, "_bonusFactor": 0.005}
{"_id": 22, "_comboBonusType": 0, "_requiredComboCount": 220, "_bonusFactor": 0.005}
{"_id": 23, "_comboBonusType": 0, "_requiredComboCount": 230, "_bonusFactor": 0.005}
{"_id": 24, "_comboBonusType": 0, "_requiredComboCount": 240, "_bonusFactor": 0.005}
{"_id": 25, "_comboBonusType": 0, "_requiredComboCount": 250, "_bonusFactor": 0.005}
{"_id": 26, "_comboBonusType": 0, "_requiredComboCount": 260, "_bonusFactor": 0.005}
{"_id": 27, "_comboBonusType": 0, "_requiredComboCount": 270, "_bonusFactor": 0.005}
{"_id": 28, "_comboBonusType": 0, "_requiredComboCount": 280, "_bonusFactor": 0.005}
{"_id": 29, "_comboBonusType": 0, "_requiredComboCount": 290, "_bonusFactor": 0.005}
{"_id": 30, "_comboBonusType": 0, "_requiredComboCount": 300, "_bonusFactor": 0.005}
{"_id": 31, "_comboBonusType": 0, "_requiredComboCount": 310, "_bonusFactor": 0.005}
{"_id": 32, "_comboBonusType": 0, "_requiredComboCount": 320, "_bonusFactor": 0.005}
{"_id": 33, "_comboBonusType": 0, "_requiredComboCount": 330, "_bonusFactor": 0.005}
{"_id": 34, "_comboBonusType": 0, "_requiredComboCount": 340, "_bonusFactor": 0.005}
{"_id": 35, "_comboBonusType": 0, "_requiredComboCount": 350, "_bonusFactor": 0.005}
{"_id": 36, "_comboBonusType": 0, "_requiredComboCount": 360, "_bonusFactor": 0.005}
{"_id": 37, "_comboBonusType": 0, "_requiredComboCount": 370, "_bonusFactor": 0.005}
{"_id": 38, "_comboBonusType": 0, "_requiredComboCount": 380, "_bonusFactor": 0.005}
{"_id": 39, "_comboBonusType": 0, "_requiredComboCount": 390, "_bonusFactor": 0.005}
{"_id": 40, "_comboBonusType": 0, "_requiredComboCount": 400, "_bonusFactor": 0.005}
{"_id": 41, "_comboBonusType": 0, "_requiredComboCount": 410, "_bonusFactor": 0.005}
{"_id": 42, "_comboBonusType": 0, "_requiredComboCount": 420, "_bonusFactor": 0.005}
{"_id": 43, "_comboBonusType": 0, "_requiredComboCount": 430, "_bonusFactor": 0.005}
{"_id": 44, "_comboBonusType": 0, "_requiredComboCount": 440, "_bonusFactor": 0.005}
{"_id": 45, "_comboBonusType": 0, "_requiredComboCount": 450, "_bonusFactor": 0.005}
{"_id": 46, "_comboBonusType": 0, "_requiredComboCount": 460, "_bonusFactor": 0.005}
{"_id": 47, "_comboBonusType": 0, "_requiredComboCount": 470, "_bonusFactor": 0.005}
{"_id": 48, "_comboBonusType": 0, "_requiredComboCount": 480, "_bonusFactor": 0.005}
{"_id": 49, "_comboBonusType": 0, "_requiredComboCount": 490, "_bonusFactor": 0.005}
{"_id": 50, "_comboBonusType": 0, "_requiredComboCount": 500, "_bonusFactor": 0.005}
{"_id": 10001, "_comboBonusType": 1, "_requiredComboCount": 10, "_bonusFactor": 0.01}
{"_id": 10002, "_comboBonusType": 1, "_requiredComboCount": 20, "_bonusFactor": 0.01}
{"_id": 10003, "_comboBonusType": 1, "_requiredComboCount": 30, "_bonusFactor": 0.01}
{"_id": 10004, "_comboBonusType": 1, "_requiredComboCount": 40, "_bonusFactor": 0.01}
{"_id": 10005, "_comboBonusType": 1, "_requiredComboCount": 50, "_bonusFactor": 0.01}
{"_id": 10006, "_comboBonusType": 1, "_requiredComboCount": 60, "_bonusFactor": 0.01}
{"_id": 10007, "_comboBonusType": 1, "_requiredComboCount": 70, "_bonusFactor": 0.01}
{"_id": 10008, "_comboBonusType": 1, "_requiredComboCount": 80, "_bonusFactor": 0.01}
{"_id": 10009, "_comboBonusType": 1, "_requiredComboCount": 90, "_bonusFactor": 0.01}
{"_id": 10010, "_comboBonusType": 1, "_requiredComboCount": 100, "_bonusFactor": 0.01}
{"_id": 10011, "_comboBonusType": 1, "_requiredComboCount": 110, "_bonusFactor": 0.01}
{"_id": 10012, "_comboBonusType": 1, "_requiredComboCount": 120, "_bonusFactor": 0.01}
{"_id": 10013, "_comboBonusType": 1, "_requiredComboCount": 130, "_bonusFactor": 0.01}
{"_id": 10014, "_comboBonusType": 1, "_requiredComboCount": 140, "_bonusFactor": 0.01}
{"_id": 10015, "_comboBonusType": 1, "_requiredComboCount": 150, "_bonusFactor": 0.01}
{"_id": 10016, "_comboBonusType": 1, "_requiredComboCount": 160, "_bonusFactor": 0.01}
{"_id": 10017, "_comboBonusType": 1, "_requiredComboCount": 170, "_bonusFactor": 0.01}
{"_id": 10018, "_comboBonusType": 1, "_requiredComboCount": 180, "_bonusFactor": 0.01}
{"_id": 10019, "_comboBonusType": 1, "_requiredComboCount": 190, "_bonusFactor": 0.01}
{"_id": 10020, "_comboBonusType": 1, "_requiredComboCount": 200, "_bonusFactor": 0.01}
{"_id": 10021, "_comboBonusType": 1, "_requiredComboCount": 210, "_bonusFactor": 0.005}
{"_id": 10022, "_comboBonusType": 1, "_requiredComboCount": 220, "_bonusFactor": 0.005}
{"_id": 10023, "_comboBonusType": 1, "_requiredComboCount": 230, "_bonusFactor": 0.005}
{"_id": 10024, "_comboBonusType": 1, "_requiredComboCount": 240, "_bonusFactor": 0.005}
{"_id": 10025, "_comboBonusType": 1, "_requiredComboCount": 250, "_bonusFactor": 0.005}
{"_id": 10026, "_comboBonusType": 1, "_requiredComboCount": 260, "_bonusFactor": 0.005}
{"_id": 10027, "_comboBonusType": 1, "_requiredComboCount": 270, "_bonusFactor": 0.005}
{"_id": 10028, "_comboBonusType": 1, "_requiredComboCount": 280, "_bonusFactor": 0.005}
{"_id": 10029, "_comboBonusType": 1, "_requiredComboCount": 290, "_bonusFactor": 0.005}
{"_id": 10030, "_comboBonusType": 1, "_requiredComboCount": 300, "_bonusFactor": 0.005}
{"_id": 10031, "_comboBonusType": 1, "_requiredComboCount": 310, "_bonusFactor": 0.005}
{"_id": 10032, "_comboBonusType": 1, "_requiredComboCount": 320, "_bonusFactor": 0.005}
{"_id": 10033, "_comboBonusType": 1, "_requiredComboCount": 330, "_bonusFactor": 0.005}
{"_id": 10034, "_comboBonusType": 1, "_requiredComboCount": 340, "_bonusFactor": 0.005}
{"_id": 10035, "_comboBonusType": 1, "_requiredComboCount": 350, "_bonusFactor": 0.005}
{"_id": 10036, "_comboBonusType": 1, "_requiredComboCount": 360, "_bonusFactor": 0.005}
{"_id": 10037, "_comboBonusType": 1, "_requiredComboCount": 370, "_bonusFactor": 0.005}
{"_id": 10038, "_comboBonusType": 1, "_requiredComboCount": 380, "_bonusFactor": 0.005}
{"_id": 10039, "_comboBonusType": 1, "_requiredComboCount": 390, "_bonusFactor": 0.005}
{"_id": 10040, "_comboBonusType": 1, "_requiredComboCount": 400, "_bonusFactor": 0.005}
```

How `model.py` uses this (lines 97–103): it keeps only `_comboBonusType == 0`, computes a per-note
`prior_combo` with `np.searchsorted(times, times, side='left')` (ties/chords share the same prior
combo), then multiplies the note weight by `1 + sum(_bonusFactor for rows with prior_combo >= _requiredComboCount)`.

---
### 6.4 `MasterLiveSkill` / `MasterLiveSkillEffect`

`MasterLiveSkill`: **8 rows**. Fields:
`_id, _nameTextID, _descriptionTextFormatID, _skillIconID, _skillCategories, _displaySkillCategories, _skillDisplayCategories`.

One complete row:

```json
{"_id": 5, "_nameTextID": "Live_Skill_Name_5", "_descriptionTextFormatID": "Live_Skill_Name_Description_5", "_skillIconID": 7, "_skillCategories": [3], "_displaySkillCategories": [1], "_skillDisplayCategories": [3]}
```

All 8 rows, compactly (`_id, _skillCategories, _displaySkillCategories, _skillDisplayCategories`):

```
(1, [1], [1], [1])  (2, [1], [1], [1])  (3, [1], [1], [1])  (4, [3], [1], [3])
(5, [3], [1], [3])  (6, [2], [1], [2])  (7, [2], [1], [2])  (8, [1], [1], [1])
```

`_skillCategories` distinct values: `{1, 2, 3}`. Note `_displaySkillCategories` is `[1]` for all 8 rows
while `_skillDisplayCategories` mirrors `_skillCategories`; `model.py` uses `_skillCategories` only.

`MasterLiveSkillEffect`: **50 rows**. Fields, in file order:
`_id, _liveSkillID, _level, _skillConditionGroup, _skillReleaseConditionGroup, _skillTargetIDs, _skillEffectType, _activationTimeSecond, _effectValue, _maxEffectValue, _effectLimitCount, _skillCumulativeConditionID, _effectExecuteLimitCount, _effectExecuteLimitResetConditionGroup, _icon`.

- Row count per `_level`: **10 each** for levels 1,2,3,4,5 (50 total).
- Distinct `_skillEffectType`: `{2000: 40, 2004: 10}`.
- `_level == 5` rows are **10**, listed verbatim below.

```json
{"_id": 5, "_liveSkillID": 1, "_level": 5, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 2000, "_activationTimeSecond": 5.0, "_effectValue": 7000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 10, "_liveSkillID": 2, "_level": 5, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 2000, "_activationTimeSecond": 5.0, "_effectValue": 10000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 15, "_liveSkillID": 3, "_level": 5, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 2000, "_activationTimeSecond": 5.0, "_effectValue": 13000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 20, "_liveSkillID": 4, "_level": 5, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [41, 46], "_skillEffectType": 2004, "_activationTimeSecond": 5.0, "_effectValue": 12000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 25, "_liveSkillID": 5, "_level": 5, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [41, 46], "_skillEffectType": 2004, "_activationTimeSecond": 5.0, "_effectValue": 15000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 30, "_liveSkillID": 6, "_level": 5, "_skillConditionGroup": 15, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 2000, "_activationTimeSecond": 5.0, "_effectValue": 9000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 35, "_liveSkillID": 6, "_level": 5, "_skillConditionGroup": 14, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 2000, "_activationTimeSecond": 5.0, "_effectValue": 11000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 40, "_liveSkillID": 7, "_level": 5, "_skillConditionGroup": 15, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 2000, "_activationTimeSecond": 5.0, "_effectValue": 12000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 45, "_liveSkillID": 7, "_level": 5, "_skillConditionGroup": 14, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 2000, "_activationTimeSecond": 5.0, "_effectValue": 14000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 50, "_liveSkillID": 8, "_level": 5, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 2000, "_activationTimeSecond": 5.0, "_effectValue": 11500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
```

Distinct `_effectValue` at `_level == 5` per effect type:

| `_skillEffectType` | n at level 5 | distinct `_effectValue` |
|---|---:|---|
| 2000 | 8 | `7000, 9000, 10000, 11000, 11500, 12000, 13000, 14000` |
| 2004 | 2 | `12000, 15000` |

Full effect rows for one skill id — `_liveSkillID == 5` (all 5 levels):

```json
{"_id": 21, "_liveSkillID": 5, "_level": 1, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [41, 46], "_skillEffectType": 2004, "_activationTimeSecond": 5.0, "_effectValue": 9000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 22, "_liveSkillID": 5, "_level": 2, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [41, 46], "_skillEffectType": 2004, "_activationTimeSecond": 5.0, "_effectValue": 10000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 23, "_liveSkillID": 5, "_level": 3, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [41, 46], "_skillEffectType": 2004, "_activationTimeSecond": 5.0, "_effectValue": 11000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 24, "_liveSkillID": 5, "_level": 4, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [41, 46], "_skillEffectType": 2004, "_activationTimeSecond": 5.0, "_effectValue": 12000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 25, "_liveSkillID": 5, "_level": 5, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [41, 46], "_skillEffectType": 2004, "_activationTimeSecond": 5.0, "_effectValue": 15000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
```

`model.py`'s `live_boost` (lines 50–53) returns `max(_effectValue for level==5) / 10000`, ignoring
`_skillConditionGroup`, `_skillTargetIDs` and `_skillEffectType`. Level-5 factors produced:

| live skill | level-5 `_effectValue` | `live_boost` (max/10000) |
|---:|---|---:|
| 1 | 7000 | 0.7 |
| 2 | 10000 | 1.0 |
| 3 | 13000 | 1.3 |
| 4 | 12000 | 1.2 |
| 5 | 15000 | 1.5 |
| 6 | 9000 (group 15), 11000 (group 14) | **1.1** |
| 7 | 12000 (group 15), 14000 (group 14) | **1.4** |
| 8 | 11500 | 1.15 |

---
### 6.5 `MasterLeaderSkill` / `MasterLeaderSkillEffect`

`MasterLeaderSkill`: **220 rows**, fields `_id, _nameTextID, _descriptionTextFormatID, _skillIconID`.
Example row:

```json
{"_id": 53, "_nameTextID": "Leader_Skill_Name_53", "_descriptionTextFormatID": "Leader_Skill_Name_Description_53", "_skillIconID": 28}
```

`MasterLeaderSkillEffect`: **1145 rows**, fields
`_id, _leaderSkillID, _level, _skillConditionGroup, _skillTargetIDs, _skillEffectType, _effectValue, _skillCumulativeConditionID, _effectExecuteLimitCount, _effectExecuteLimitResetConditionGroup, _icon`.
It is consumed only in `leader_bp` (lines 55–66).

- Rows per `_level`: **229 each** for levels 1..5 (1145 total).
- `_level == 5` rows: **229**.
- Distinct `_skillEffectType` (all rows): `{1001: 265, 1000: 245, 1003: 240, 1002: 215, 0: 165, 1503: 15}`.
- 165 rows have empty `_skillTargetIDs`, and **all 165 of those are `_skillEffectType == 0`**. All 15
  `_skillEffectType == 1503` rows have non-empty `_skillTargetIDs` (`[51]`).
- Distinct `_leaderSkillID` present in the effect table: **209** (of 220 skill rows).

Distinct `_effectValue` at `_level == 5` per `_skillEffectType`:

| `_skillEffectType` | n at level 5 | distinct `_effectValue` |
|---|---:|---|
| 0 | 33 | `0, 1300, 1800, 2000, 2300, 3000, 5000, 10000, 14000, 15000, 20000` |
| 1000 | 49 | `2000, 2400, 2500, 3000, 3400, 3500, 4000, 4700` |
| 1001 | 53 | `1800, 3600, 4800, 5000, 5100, 7000, 7200, 8000, 9000, 10000, 10200, 13200, 14100` |
| 1002 | 43 | `1800, 3600, 5000, 5100, 7000, 7200, 8000, 9000, 10000, 10200, 13200, 14100` |
| 1003 | 48 | `1800, 3600, 4800, 5000, 5100, 7000, 7200, 8000, 9000, 10200, 13200, 14100` |
| 1503 | 3 | `8000, 9000, 10000` |

`_skillConditionGroup` at level 5: `{0: 198, 26: 4, 167: 2, 169: 2, 174: 2, 175: 2, 173: 2, 176: 2, 182: 2, 183: 2, 184: 2, 185: 2, 171: 1, 168: 1, 36: 1, 178: 1, 179: 1, 180: 1, 181: 1}`.
For the **52 leader skills actually used by members**, every level-5 row has `_skillConditionGroup == 0`,
so `leader_bp`'s omission of condition groups does not bite in this dataset.

Most common level-5 `_skillTargetIDs` lists: `[3]`×41, `[4]`×40, `[]`×33, `[5]`×23, `[6]`×22, `[7]`×22,
`[48]`×6, `[49]`×5, `[50]`×5, `[53]`×5, `[11]`×5, `[9]`×4, `[8]`×4, `[52]`×4, `[51]`×4, `[10]`×3, `[12]`×3, ….

#### Example: row with target IDs — `_id == 1`

```json
{"_id": 1, "_leaderSkillID": 1, "_level": 1, "_skillConditionGroup": 0, "_skillTargetIDs": [3], "_skillEffectType": 1000, "_effectValue": 400, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
```

#### Example: composite row with empty target IDs — `_id == 90651`

```json
{"_id": 90651, "_leaderSkillID": 90122, "_level": 1, "_skillConditionGroup": 167, "_skillTargetIDs": [], "_skillEffectType": 0, "_effectValue": 1200, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
```

#### Level-5 effect rows for `_leaderSkillID == 53` (the leader skill of member 61)

```json
{"_id": 285, "_leaderSkillID": 53, "_level": 5, "_skillConditionGroup": 0, "_skillTargetIDs": [9], "_skillEffectType": 1003, "_effectValue": 10200, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 290, "_leaderSkillID": 53, "_level": 5, "_skillConditionGroup": 0, "_skillTargetIDs": [5], "_skillEffectType": 1003, "_effectValue": 4800, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
```

Target 9 is `_cardType == 2`; target 5 is `_bandID == 3`. Both match member 61 (`type 2`, `band 3`).
Type 1003 maps to index **0 (Performance)** in `model.py`; 1001→index 1 (Technique), 1002→index 2 (Visual).

---
### 6.6 `MasterSupportSkill` / `MasterSupportSkillEffect`

`MasterSupportSkill`: **63 rows**, fields `_id, _nameTextID, _descriptionTextFormatID, _skillIconID, _displaySkillCategories`.
`_id` values present: 1..40 and 51..73 (40 + 23 = 63).

`MasterSupportSkillEffect`: **615 rows**, fields
`_id, _supportSkillID, _level, _skillTriggerConditionGroup, _skillTriggerType, _skillConditionGroup, _skillReleaseConditionGroup, _skillTargetIDs, _skillEffectType, _activationTimeSecond, _effectValue, _maxEffectValue, _effectLimitCount, _skillCumulativeConditionID, _effectExecuteLimitCount, _effectExecuteLimitResetConditionGroup, _icon`.

- Rows per `_level`: **123 each** for levels 1..5.
- Distinct support skills in the effect table: **63**.
- Rows per skill: **10** for skills 1–40 and 51–70; **5** for skills 71, 72, 73.
- Distinct `_skillEffectType` (all rows): `{15000: 155, 2000: 150, 12006: 105, 3001: 105, 3003: 100}`.
- `_skillTriggerType == 1` for **every** one of the 615 rows.
- `_skillTriggerConditionGroup` distribution: `{53: 365, 238: 150, 335: 100}`. It is **not** constant; it is
  determined by the effect type / skill block:

  | `_skillTriggerConditionGroup` | rows | `_skillEffectType` | support skills |
  |---:|---:|---|---|
  | 53 | 365 | `15000` (155), `12006` (105), `3001` (105) | 1–15, 31–40, 51–60, 71, 72, 73 |
  | 238 | 150 | `2000` | 16–30 |
  | 335 | 100 | `3003` | 61–70 |

  At `_level == 5` the split is `{53: 73, 238: 30, 335: 20}`; all 31 `15000` level-5 rows are group 53.
- Per-skill effect type is a single type each:
  skills 1–15 and 71 → `15000`; skills 16–30 → `2000`; skills 31–40 and 72 → `12006`;
  skills 51–60 and 73 → `3001`; skills 61–70 → `3003`.

`_level == 5` rows: **123**. Distinct `_skillConditionGroup` at level 5: `{0, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195}`.

Distinct `_effectValue` and `_skillConditionGroup` at `_level == 5`, grouped by `_skillEffectType`:

| `_skillEffectType` | n @ L5 | distinct `_effectValue` | `_skillConditionGroup` (counts) | `_skillTargetIDs` |
|---|---:|---|---|---|
| `15000` | 31 | `1500, 2000, 2500, 3000, 4000, 5000` | `{187:3, 186:3, 189:3, 188:3, 191:3, 190:3, 193:3, 192:3, 195:3, 194:3, 0:1}` | `[]` ×31 |
| `2000` | 30 | `500, 750, 1000, 1500, 2000` | `{187:3, 186:3, 189:3, 188:3, 191:3, 190:3, 193:3, 192:3, 195:3, 194:3}` | `[]` ×30 |
| `3001` | 21 | `350, 450, 550, 700, 900` | `{187:2, 186:2, 189:2, 188:2, 191:2, 190:2, 193:2, 192:2, 195:2, 194:2, 0:1}` | `[]` ×21 |
| `3003` | 20 | `0` | `{187:2, 186:2, 189:2, 188:2, 191:2, 190:2, 193:2, 192:2, 195:2, 194:2}` | `[]` ×20 |
| `12006` | 21 | `5` | `{187:2, 186:2, 189:2, 188:2, 191:2, 190:2, 193:2, 192:2, 195:2, 194:2, 0:1}` | `[42]` ×11, `[43, 42]` ×10 |

#### The extension mechanism (`_skillEffectType == 15000`) — every level-5 row

`model.py`'s `duration_ms` = `5000 + sum(_effectValue)` over level-5 `15000` rows whose
`_skillConditionGroup` matches. All 31 such level-5 rows are listed verbatim below.

```json
{"_id": 5, "_supportSkillID": 1, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 187, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 10, "_supportSkillID": 1, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 186, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 3000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 15, "_supportSkillID": 2, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 189, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 20, "_supportSkillID": 2, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 188, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 3000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 25, "_supportSkillID": 3, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 191, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 30, "_supportSkillID": 3, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 190, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 3000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 35, "_supportSkillID": 4, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 193, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 40, "_supportSkillID": 4, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 192, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 3000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 45, "_supportSkillID": 5, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 195, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 50, "_supportSkillID": 5, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 194, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 3000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 55, "_supportSkillID": 6, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 187, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 60, "_supportSkillID": 6, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 186, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 4000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 65, "_supportSkillID": 7, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 189, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 70, "_supportSkillID": 7, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 188, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 4000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 75, "_supportSkillID": 8, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 191, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 80, "_supportSkillID": 8, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 190, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 4000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 85, "_supportSkillID": 9, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 193, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 90, "_supportSkillID": 9, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 192, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 4000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 95, "_supportSkillID": 10, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 195, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 100, "_supportSkillID": 10, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 194, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 4000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 105, "_supportSkillID": 11, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 187, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 110, "_supportSkillID": 11, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 186, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 5000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 115, "_supportSkillID": 12, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 189, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 120, "_supportSkillID": 12, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 188, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 5000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 125, "_supportSkillID": 13, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 191, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 130, "_supportSkillID": 13, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 190, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 5000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 135, "_supportSkillID": 14, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 193, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 140, "_supportSkillID": 14, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 192, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 5000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 145, "_supportSkillID": 15, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 195, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 150, "_supportSkillID": 15, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 194, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 5000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 705, "_supportSkillID": 71, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 3000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
```

#### What the condition groups mean (exact)

Every group referenced above contains exactly one condition set with exactly one condition, of
`_conditionType == 5000`, whose single `_conditionTargetIDs` entry is a `_bandID`-constrained
`MasterSkillTarget`:

| group | condition `_id` | `_isPositive` | target `_id` | `MasterSkillTarget._bandID` |
|---:|---:|---|---:|---:|
| 186 | 178 | `true` | 3 | 1 |
| 187 | 179 | `false` | 3 | 1 |
| 188 | 180 | `true` | 4 | 2 |
| 189 | 181 | `false` | 4 | 2 |
| 190 | 182 | `true` | 5 | 3 |
| 191 | 183 | `false` | 5 | 3 |
| 192 | 184 | `true` | 6 | 4 |
| 193 | 185 | `false` | 6 | 4 |
| 194 | 186 | `true` | 7 | 5 |
| 195 | 187 | `false` | 7 | 5 |
| 0 | — | — | — | `condition_group_matches(0, m) == True` by convention |

So the rule is: **the positive-`_bandID` group fires when the member's band equals the snap's band,
otherwise the negative group fires.** Consequently:

| support skill | in-band extension | out-of-band extension |
|---:|---:|---:|
| 1 | 3000 | 1500 |
| 2 | 3000 | 1500 |
| 3 | 3000 | 1500 |
| 4 | 3000 | 1500 |
| 5 | 3000 | 1500 |
| 6 | 4000 | 2000 |
| 7 | 4000 | 2000 |
| 8 | 4000 | 2000 |
| 9 | 4000 | 2000 |
| 10 | 4000 | 2000 |
| 11 | 5000 | 2500 |
| 12 | 5000 | 2500 |
| 13 | 5000 | 2500 |
| 14 | 5000 | 2500 |
| 15 | 5000 | 2500 |
| 71 | 3000 (always) | 3000 (always) |
| 16–40, 51–70, 72, 73 | 0 (no `15000` row) | 0 |

---
#### Snaps by achievable `duration_ms`

`duration_ms` was computed for all 64 snaps × 63 members. The observed per-snap value sets are exactly
`{8000, 6500}`, `{9000, 7000}`, `{10000, 7500}`, or `{5000}` (snaps whose first support skill is one of
the no-extension skills 16–40 / 51–70). Exact per-snap results:

| snap ids | `support_skills[0]` | `duration_ms` set |
|---|---|---|
| 1–5 (高松 燈, 千早 愛音, 要 楽奈, 長崎 そよ, 椎名 立希) | 1 | in-band (15 members) → **8000** (+3000); out (48) → **6500** (+1500) |
| 6–10 (三角 初華, 若葉 睦, 八幡 海鈴, 祐天寺 にゃむ, 豊川 祥子) | 2 | in-band 15 → **8000**; out 48 → **6500** |
| 11–15 (仲町 あられ, 宮永 ののか, 峰月 律, 藤 都子, 千石 ユノ) | 3 | in-band 13 → **8000**; out 50 → **6500** |
| 16–20 (汐見 蛍, 伊沢 なつめ, 琴平 凪, 浜崎 まほろ, 和泉 朋花) | 4 | in-band 10 → **8000**; out 53 → **6500** |
| 21–25 (須賀 蕾叶, 馬橋 心玖, 矢倉 蓬咲, 梅里 ちえり, 四宮 寧月) | 5 | in-band 10 → **8000**; out 53 → **6500** |
| 28, 30 (要 楽奈, 椎名 立希) | 6 | in-band 15 → **9000** (+4000); out 48 → **7000** (+2000) |
| 33, 34 (八幡 海鈴, 祐天寺 にゃむ) | 7 | in-band 15 → **9000**; out 48 → **7000** |
| 39, 40 (藤 都子, 千石 ユノ) | 8 | in-band 13 → **9000**; out 50 → **7000** |
| 41, 45 (汐見 蛍, 和泉 朋花) | 9 | in-band 10 → **9000**; out 53 → **7000** |
| 46, 47 (須賀 蕾叶, 馬橋 心玖) | 10 | in-band 10 → **9000**; out 53 → **7000** |
| 51 (MyGO!!!!!) | 11 | in-band 15 → **10000** (+5000); out 48 → **7500** (+2500) |
| 53 (Ave Mujica) | 12 | in-band 15 → **10000**; out 48 → **7500** |
| 55, 56 (夢限大みゅーたいぷ, ののか&都子&ユノ), 62 (千石 ユノ) | 13 | in-band 13 → **10000**; out 50 → **7500** |
| 58 (millsage) | 14 | in-band 10 → **10000**; out 53 → **7500** |
| 59 (須賀 蕾叶) | 15 | in-band 10 → **10000**; out 53 → **7500** |
| 61 (all-band) | 71 | **8000 for all 63 members** (+3000 always) |
| 26, 27, 29, 31, 32, 35, 36, 37, 38, 42, 43, 44, 48, 49, 50, 52, 54, 57, 60, 63, 64 | 31–35, 37, 38, 51–56, 59, 60 | **5000 for all 63 members** (+0) |

The 5000 ms extension is therefore reachable only through snaps **51, 53, 55, 56, 58, 59, 62**
(support skills 11–15) and then only for members whose `band` equals the snap's band. The 2500 ms
extension likewise comes only from the out-of-band branch of those same skills.

#### Snap support-skill frequency (which skills "appear most often")

Non-zero slots only, over 64 snaps:

```
skill 1 → 5 snaps   skill 2 → 5   skill 3 → 5   skill 4 → 5   skill 5 → 5
skill 13 → 3        skill 51 → 2  skill 6 → 2   skill 32 → 2  skill 7 → 2
skill 53 → 2        skill 33 → 2  skill 8 → 2   skill 9 → 2   skill 54 → 2
skill 10 → 2        skill 55 → 2  skill 31 → 1  skill 52 → 1  skill 34 → 1
skill 35 → 1        skill 11 → 1  skill 56 → 1  skill 12 → 1  skill 37 → 1
skill 59 → 1        skill 14 → 1  skill 15 → 1  skill 60 → 1  skill 71 → 1
skill 73 → 1        skill 38 → 1
```

Most frequent (5 snaps each) are support skills **1, 2, 3, 4, 5**. Full effect rows for support
skill **1** (all 10 rows, verbatim):

```json
{"_id": 1, "_supportSkillID": 1, "_level": 1, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 187, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 250, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 2, "_supportSkillID": 1, "_level": 2, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 187, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 3, "_supportSkillID": 1, "_level": 3, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 187, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 750, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 4, "_supportSkillID": 1, "_level": 4, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 187, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 5, "_supportSkillID": 1, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 187, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 6, "_supportSkillID": 1, "_level": 1, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 186, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 7, "_supportSkillID": 1, "_level": 2, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 186, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 8, "_supportSkillID": 1, "_level": 3, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 186, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 9, "_supportSkillID": 1, "_level": 4, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 186, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 10, "_supportSkillID": 1, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 186, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 3000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
```

Support skills 2, 3, 4, 5 are the same shape with `_skillConditionGroup` 189/188, 191/190, 193/192,
195/194 respectively and the same level curve `250, 500, 750, 1000, 1500` (out-of-band) /
`500, 1000, 1500, 2000, 3000` (in-band).

Support skill 71 (used by snap 61; unconditional 3000 ms extension, all 5 rows verbatim):

```json
{"_id": 701, "_supportSkillID": 71, "_level": 1, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 702, "_supportSkillID": 71, "_level": 2, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 703, "_supportSkillID": 71, "_level": 3, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 1500, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 704, "_supportSkillID": 71, "_level": 4, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 2000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
{"_id": 705, "_supportSkillID": 71, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 15000, "_activationTimeSecond": 0.0, "_effectValue": 3000, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
```

Support skill 73 (snap 61's second slot; type `3001`, **no** `15000` row, so it contributes no
duration). Its level-5 row is:

```json
{"_id": 715, "_supportSkillID": 73, "_level": 5, "_skillTriggerConditionGroup": 53, "_skillTriggerType": 1, "_skillConditionGroup": 0, "_skillReleaseConditionGroup": 0, "_skillTargetIDs": [], "_skillEffectType": 3001, "_activationTimeSecond": 0.0, "_effectValue": 550, "_maxEffectValue": 0, "_effectLimitCount": 0, "_skillCumulativeConditionID": 0, "_effectExecuteLimitCount": 0, "_effectExecuteLimitResetConditionGroup": 0, "_icon": ""}
```

Snap 61 therefore gets `5000 + 3000 = 8000 ms` for every member, confirmed in §7.2.

---
### 6.7 `MasterSkillTarget` — 56 rows

Fields (11): `_id, _skillTargetType, _characterID, _bandID, _cardType, _tagID, _judgement, _liveMusicType, _gekisouMissionType, _liveSkillCategories, _gekisouSkillCategories`.

`_id` values present are 1..57 **with 25 missing** (56 rows).

Constraint-field distribution over all 56 rows:

| field | rows non-zero / non-empty | distinct values among those rows |
|---|---:|---|
| `_characterID` | **25** | 1..25, each exactly once |
| `_bandID` | **6** | `{1:1, 2:1, 3:2, 4:1, 5:1}` |
| `_cardType` | **6** | `{1:1, 2:1, 3:1, 4:2, 5:1}` |
| `_tagID` | **0** | — (no row in this dataset constrains a tag) |
| `_gekisouMissionType` | **6** | `{1:2, 2:2, 3:2}` |
| `_liveSkillCategories` (non-empty) | **3** | `[1]`, `[2]`, `[3]` (one row each) |
| `_gekisouSkillCategories` (non-empty) | **0** | — |
| `_judgement` **not** `-1` | **7** | `{5:2, 1:1, 2:1, 3:1, 4:1, 6:1}` |
| `_liveMusicType` | **1** | `{1:1}` |

`_skillTargetType` distribution: `{3: 44, 4: 7, 5: 3, 1: 1, 2: 1}`.

**`model.py`'s `target_matches` checks only `_characterID, _bandID, _cardType, _tagID, _gekisouMissionType,
_liveSkillCategories, _gekisouSkillCategories` — it silently ignores `_judgement` and `_liveMusicType`.**

Three example rows covering different constraint kinds:

```json
{"_id": 21, "_skillTargetType": 3, "_characterID": 8, "_bandID": 0, "_cardType": 0, "_tagID": 0, "_judgement": -1, "_liveMusicType": 0, "_gekisouMissionType": 0, "_liveSkillCategories": [], "_gekisouSkillCategories": []}
{"_id": 6, "_skillTargetType": 3, "_characterID": 0, "_bandID": 4, "_cardType": 0, "_tagID": 0, "_judgement": -1, "_liveMusicType": 0, "_gekisouMissionType": 0, "_liveSkillCategories": [], "_gekisouSkillCategories": []}
{"_id": 57, "_skillTargetType": 5, "_characterID": 0, "_bandID": 0, "_cardType": 0, "_tagID": 0, "_judgement": -1, "_liveMusicType": 0, "_gekisouMissionType": 3, "_liveSkillCategories": [], "_gekisouSkillCategories": []}
```

Three more covering the remaining kinds (`_cardType`+`_bandID`, `_liveSkillCategories`, and
`_judgement`+`_liveMusicType`):

```json
{"_id": 13, "_skillTargetType": 3, "_characterID": 0, "_bandID": 3, "_cardType": 4, "_tagID": 0, "_judgement": -1, "_liveMusicType": 0, "_gekisouMissionType": 0, "_liveSkillCategories": [], "_gekisouSkillCategories": []}
{"_id": 48, "_skillTargetType": 3, "_characterID": 0, "_bandID": 0, "_cardType": 0, "_tagID": 0, "_judgement": -1, "_liveMusicType": 0, "_gekisouMissionType": 0, "_liveSkillCategories": [1], "_gekisouSkillCategories": []}
{"_id": 47, "_skillTargetType": 4, "_characterID": 0, "_bandID": 0, "_cardType": 0, "_tagID": 0, "_judgement": 5, "_liveMusicType": 1, "_gekisouMissionType": 0, "_liveSkillCategories": [], "_gekisouSkillCategories": []}
```

Wildcard rows (all constraints zero/empty): `_id` **1** (`_skillTargetType 1`), **2** (`_skillTargetType 2`),
**24** and **54**. `_id 24` and `_id 54` are identical to each other in every field, and both are
`_skillTargetType 3` with all constraints unset (i.e. two copies of an "any member" target).

### 6.8 `MasterSkillCondition` / `MasterSkillConditionSet`

`MasterSkillCondition`: **308 rows**, fields `_id, _conditionType, _conditionValues, _isPositive, _conditionTargetIDs`.

- Distinct `_conditionType` (32 of them) with counts:
  `{4011: 96, 1030: 74, 3000: 22, 2001: 14, 4000: 11, 5000: 10, 3001: 9, 1000: 7, 4001: 7, 1020: 6, 2002: 6, 7005: 6, 7002: 6, 1010: 5, 7000: 4, 7010: 3, 7012: 3, 7020: 3, 4009: 2, 4010: 2, 4007: 1, 4008: 1, 1040: 1, 4012: 1, 8000: 1, 7001: 1, 7004: 1, 7011: 1, 7013: 1, 7007: 1, 7021: 1, 2003: 1}`.
- `_isPositive`: `{true: 292, false: 16}`. **Negative conditions are rare — only 16 of 308.**
- `_conditionTargetIDs` length: `{0: 167, 1: 117, 2: 24}`.
- Conditions with a **non-empty** `_conditionValues`: 172 rows, spread over types
  `{4011: 96, 1030: 74, 2001: 14, 4000: 11, 4001: 7, 2002: 6, 7005: 6, 7002: 6, 7000: 4, 7012: 3, 4009: 2, 3001: 2, 4007: 1, 4008: 1, 1040: 1, 4012: 1, 3000: 1, 7001: 1, 7004: 1, 7007: 1, 2003: 1}`.
- All **167** rows with empty `_conditionTargetIDs` also have `_conditionValues` **empty**
  (types `4011, 2001, 4000, 4001, 2002, 7005, 7002, 7000, 7012, 4009, 4010, 4007, 4008, 8000, 7001, 7004, 7011, 7013, 7007, 7021, 2003`).
- **All 10 `_conditionType == 5000` rows have `_conditionValues == []` and exactly one `_conditionTargetIDs` entry**
  (a `MasterSkillTarget._id`). 5 are positive, 5 negative:

```json
{"_id": 178, "_conditionType": 5000, "_conditionValues": [], "_isPositive": true, "_conditionTargetIDs": [3]}
{"_id": 179, "_conditionType": 5000, "_conditionValues": [], "_isPositive": false, "_conditionTargetIDs": [3]}
{"_id": 180, "_conditionType": 5000, "_conditionValues": [], "_isPositive": true, "_conditionTargetIDs": [4]}
{"_id": 181, "_conditionType": 5000, "_conditionValues": [], "_isPositive": false, "_conditionTargetIDs": [4]}
{"_id": 182, "_conditionType": 5000, "_conditionValues": [], "_isPositive": true, "_conditionTargetIDs": [5]}
{"_id": 183, "_conditionType": 5000, "_conditionValues": [], "_isPositive": false, "_conditionTargetIDs": [5]}
{"_id": 184, "_conditionType": 5000, "_conditionValues": [], "_isPositive": true, "_conditionTargetIDs": [6]}
{"_id": 185, "_conditionType": 5000, "_conditionValues": [], "_isPositive": false, "_conditionTargetIDs": [6]}
{"_id": 186, "_conditionType": 5000, "_conditionValues": [], "_isPositive": true, "_conditionTargetIDs": [7]}
{"_id": 187, "_conditionType": 5000, "_conditionValues": [], "_isPositive": false, "_conditionTargetIDs": [7]}
```

So a condition references targets by `_conditionTargetIDs -> MasterSkillTarget._id`, and its polarity
is `_isPositive`. `model.py`'s `condition_matches` implements exactly
`any(target_matches(t, m) for t in _conditionTargetIDs) == _isPositive` for type 5000 and
**raises `ValueError('unsupported support condition', r)` for every other type**.

`MasterSkillConditionSet`: **417 rows**, fields `_id, _group, _conditionIds`.

- `_group` distinct values: **408**.
- `len(_conditionIds)` distribution: `{1: 251, 2: 166}` — 251 single-condition sets, 166 two-condition
  sets (an AND).
- Largest groups: group `37` has **5** sets; groups `56, 163, 164, 166, 236` have **2** sets each;
  all other groups have 1.
- First rows:

```json
{"_id": 1, "_group": 1, "_conditionIds": [1]}
{"_id": 2, "_group": 2, "_conditionIds": [2]}
{"_id": 3, "_group": 3, "_conditionIds": [3]}
{"_id": 4, "_group": 4, "_conditionIds": [4]}
```

`condition_group_matches(group, m)` = `True` for `group == 0`; otherwise
`any(all(condition_matches(c, m) for c in set._conditionIds) for set in sets with that _group)` — i.e.
OR across sets, AND within a set. It asserts that at least one set exists for the group.

---
### 6.9 `MasterLiveMusic` / `MasterLiveMusicScore` / `MasterChallengeMusic`

Only the fields below are consumed by `prepare.py` (none by `model.py`). Row counts: `MasterLiveMusic`
**85**, `MasterLiveMusicScore` **340**, `MasterChallengeMusic` **3**.

| table | fields consumed | consumed by |
|---|---|---|
| `MasterLiveMusic` | `_id`, `_titleTextID`, `_bandIDs`, `_bestMusicTagIDs`, `_musicType`, `_easyID`, `_normalID`, `_hardID`, `_expertID` | `prepare.py` L100–108 |
| `MasterLiveMusicScore` | `_id`, `_musicScoreTextFileName`, `_musicScoreLevel`, `_musicScoreDisplayLevel`, `_fullComboCount` | `prepare.py` L99, L107–111 |
| `MasterChallengeMusic` | `_id`, `_eventId`, `_liveMusicId`, `_musicType` | `prepare.py` L101–104 |

All 3 `MasterChallengeMusic` rows verbatim:

```json
{"_id": 1, "_eventId": 1, "_liveMusicId": 100109, "_musicType": 2, "_gekisouMission1": 0, "_gekisouMission2": 0, "_gekisouMission3": 0, "_rankingRewardGroup": 100101, "_stratAt": "", "_endAt": ""}
{"_id": 2, "_eventId": 1, "_liveMusicId": 100056, "_musicType": 2, "_gekisouMission1": 0, "_gekisouMission2": 0, "_gekisouMission3": 0, "_rankingRewardGroup": 100102, "_stratAt": "", "_endAt": ""}
{"_id": 3, "_eventId": 1, "_liveMusicId": 100063, "_musicType": 2, "_gekisouMission1": 0, "_gekisouMission2": 0, "_gekisouMission3": 0, "_rankingRewardGroup": 100103, "_stratAt": "", "_endAt": ""}
```

The three `MasterLiveMusic` rows behind `songs[]`, **projected** to the fields listed in the table
above (a projection, not a full row — the real rows carry 55 fields each):

```json
{"_id": 100109, "_titleTextID": "Music_Tilte_100109", "_musicType": 2, "_bandIDs": [3], "_bestMusicTagIDs": [3], "_easyID": 10010900, "_normalID": 10010901, "_hardID": 10010902, "_expertID": 10010903}
{"_id": 100056, "_titleTextID": "Music_Tilte_100056", "_musicType": 2, "_bandIDs": [3], "_bestMusicTagIDs": [3], "_easyID": 10005600, "_normalID": 10005601, "_hardID": 10005602, "_expertID": 10005603}
{"_id": 100063, "_titleTextID": "Music_Tilte_100063", "_musicType": 2, "_bandIDs": [3], "_bestMusicTagIDs": [3], "_easyID": 10006300, "_normalID": 10006301, "_hardID": 10006302, "_expertID": 10006303}
```

(`_titleTextID` really is spelled `Tilte` in the table.)

The four `MasterLiveMusicScore` rows of song 100109:

```json
{"_id": 10010900, "_musicScoreTextFileName": "0109/0109_00", "_musicScoreLevel": 7, "_musicScoreDisplayLevel": 7.0, "_fullComboCount": 272}
{"_id": 10010901, "_musicScoreTextFileName": "0109/0109_01", "_musicScoreLevel": 15, "_musicScoreDisplayLevel": 15.0, "_fullComboCount": 350}
{"_id": 10010902, "_musicScoreTextFileName": "0109/0109_02", "_musicScoreLevel": 19, "_musicScoreDisplayLevel": 19.0, "_fullComboCount": 600}
{"_id": 10010903, "_musicScoreTextFileName": "0109/0109_03", "_musicScoreLevel": 26, "_musicScoreDisplayLevel": 26.0, "_fullComboCount": 759}
```

`MasterLiveMusicScore` has exactly 340 rows = 85 songs × 4 difficulties; score `_id`s are
`musicId * 100 + difficultyIndex` (00 easy, 01 normal, 02 hard, 03 expert).

---
## 7. Worked micro-examples

### 7.1 `slot_power` for member **61** (千石 ユノ), snap **61**, leader = member 61, song = `songs[0]` (100109)

Inputs that the algorithm reads:

| quantity | value |
|---|---|
| `m['id']` | `61` |
| `m['trained']` | `[46530, 36784, 38042]` |
| `m['event_bonus_bp']` | `10000` |
| `m['type']` / `m['band']` / `m['tags']` | `2` / `3` / `[3]` |
| `s['id']` | `61` |
| `s['trained']` | `[1400, 1200, 1300]` |
| `s['event_bonus_bp']` | `5000` |
| `s['type']` | `2` |
| `s['rank']['_cardTypeLinkBonusRate']` | `2000` |
| `m['rank']['_musicTypeBonusRate']` | `2000` |
| `m['rank']['_musicTagBonusRate']` | `2000` |
| `song['type']` / `song['tags']` | `2` / `[3]` |
| `ideal['character_rank']['_bonus']` | `245` |
| `ideal['character_total_bonus']['_bonus']` | `90` |
| `ideal['vip_power_bonus_bp']` | `2000` |

**Step 1 — `native_floor(trained * event_bonus_bp)`** (cast to float32, `/10000`, then floor):

| component | `trained` | `trained*10000` (int64) | as float32 (exact?) | `/10000` | floor |
|---|---:|---:|---|---:|---:|
| performance | 46530 | 465300000 | 465300000.0 (exact) | 46530.0 | **46530** |
| technic | 36784 | 367840000 | 367840000.0 (exact) | 36784.0 | **36784** |
| visual | 38042 | 380420000 | 380420000.0 (exact) | 38042.0 | **38042** |

`native_floor(...) = [46530, 36784, 38042]`.

**Step 2 — `base` and `common`**

```
base   = trained + native_floor(...)  = [46530+46530, 36784+36784, 38042+38042] = [93060, 73568, 76084]
common = base + 245 + 90              = [93395, 73903, 76419]
```

**Step 3 — bonus vectors**

`leader_bp(leader=61, m=61)`: `leader_skill` is `53`. Level-5 effect rows for skill 53 are `_id 285`
(`_skillEffectType 1003`, `_effectValue 10200`, target `[9]`) and `_id 290`
(`_skillEffectType 1003`, `_effectValue 4800`, target `[5]`). Target 9 requires `_cardType == 2`
(member 61 has type 2 → match); target 5 requires `_bandID == 3` (member 61 has band 3 → match).
Both are type 1003 → index **0** (Performance). So `leader_bp = [15000, 0, 0]`.

| bonus | vector | derivation |
|---|---|---|
| `snap` | `[6400, 6200, 6300]` | `s['trained'] + s['event_bonus_bp']` = `[1400+5000, 1200+5000, 1300+5000]` |
| `type_link` | `[2500, 2500, 2500]` | `500 + 2000`, `m['type']==s['type']` (2==2) |
| `band_item` | `[2500, 2500, 2500]` | hard-coded `np.full(3, 2500)` |
| `music_type` | `[2500, 2500, 2500]` | `500 + 2000`, `m['type']==song['type']` (2==2) |
| `music_tag` | `[2500, 2500, 2500]` | `500 + 2000`, `{3} & {3}` non-empty |
| `leader` | `[15000, 0, 0]` | see above |
| `vip` | `[2000, 2000, 2000]` | `ideal['vip_power_bonus_bp']` |

**Step 4 — each floored part `native_floor(common * bonus)`** (float32 rounding *does* fire here —
the int64 products are not all float32-representable):

| bonus | `common * bonus` (int64) | as float32 | `/10000` | floor |
|---|---|---|---|---|
| `snap` | `[597728000, 458198600, 481439700]` | `[597728000.0, 458198592.0, 481439712.0]` | `[59772.80078125, 45819.859375, 48143.97265625]` | `[59772, 45819, 48143]` |
| `type_link` | `[233487500, 184757500, 191047500]` | `[233487504.0, 184757504.0, 191047504.0]` | `[23348.75, 18475.75, 19104.75]` | `[23348, 18475, 19104]` |
| `band_item` | `[233487500, 184757500, 191047500]` | `[233487504.0, 184757504.0, 191047504.0]` | `[23348.75, 18475.75, 19104.75]` | `[23348, 18475, 19104]` |
| `music_type` | `[233487500, 184757500, 191047500]` | `[233487504.0, 184757504.0, 191047504.0]` | `[23348.75, 18475.75, 19104.75]` | `[23348, 18475, 19104]` |
| `music_tag` | `[233487500, 184757500, 191047500]` | `[233487504.0, 184757504.0, 191047504.0]` | `[23348.75, 18475.75, 19104.75]` | `[23348, 18475, 19104]` |
| `leader` | `[1400925000, 0, 0]` | `[1400925056.0, 0.0, 0.0]` | `[140092.5, 0.0, 0.0]` | `[140092, 0, 0]` |
| `vip` | `[186790000, 147806000, 152838000]` | `[186790000.0, 147806000.0, 152838000.0]` | `[18679.0, 14780.599609375, 15283.7998046875]` | `[18679, 14780, 15283]` |

**Step 5 — final 3-dim vector and sum**

```
sum of parts = [59772+23348+23348+23348+23348+140092+18679,
                45819+18475+18475+18475+18475+0+14780,
                48143+19104+19104+19104+19104+0+15283]
             = [311935, 134499, 139842]
power        = common + sum = [93395+311935, 73903+134499, 76419+139842]
             = [405330, 208402, 216261]
sum(power)   = 405330 + 208402 + 216261 = 829993
```

`slot_power` returns **`829993`**, and its `detail` dict is:

```json
{"base": [93060, 73568, 76084], "common": [93395, 73903, 76419],
 "parts": {"snap": [59772, 45819, 48143], "type_link": [23348, 18475, 19104],
           "band_item": [23348, 18475, 19104], "music_type": [23348, 18475, 19104],
           "music_tag": [23348, 18475, 19104], "leader": [140092, 0, 0],
           "vip": [18679, 14780, 15283]},
 "total": [405330, 208402, 216261]}
```

This was cross-checked by importing the real `model.py` (from `.../our-notes-event-20260930`, using that
project's own `.venv` Python, numpy 2.5.3) and calling `model.slot_power(m, m, s, song)`: it returns the
same `829993` and the same detail dict.

**Float32 caveat, confirmed:** of the 21 int64 components across the seven bonus vectors, **15 are not
float32-representable** and get rounded on the cast — 14 round *up*, 1 rounds *down*:

| int64 product | float32 result | direction | occurrences |
|---|---|---|---:|
| 458198600 | 458198592 | down | 1 (`snap[1]`) |
| 481439700 | 481439712 | up | 1 (`snap[2]`) |
| 233487500 | 233487504 | up | 4 (`*[0]` of `type_link`, `band_item`, `music_type`, `music_tag`) |
| 184757500 | 184757504 | up | 4 (`*[1]` of the same four) |
| 191047500 | 191047504 | up | 4 (`*[2]` of the same four) |
| 1400925000 | 1400925056 | up | 1 (`leader[0]`) |

The remaining 6 components (597728000, 186790000, 147806000, 152838000, and the two zero `leader`
components) are exact. Here the floor is unchanged for every component, but a general reimplementation
must reproduce `np.float32` rounding before `//10000` or it can be off by one.

---
### 7.2 `duration_ms(member, snap)` — same band vs. different band

Member **61** (千石 ユノ, `band == 3`, `type == 2`, `character == 15`).

**Same-band snap — snap 15 (千石 ユノ), `bands == [3]`, `support_skills == [3, 0]`:**

| effect row `_id` | `_supportSkillID` | `_skillConditionGroup` | `_effectValue` | `condition_group_matches` |
|---:|---:|---:|---:|---|
| 25 | 3 | 191 | **1500** | `false` |
| 30 | 3 | 190 | **3000** | `true` |

Group 191 is `_conditionType 5000, _isPositive false, _conditionTargetIDs [5]`, and target 5 is
`_bandID == 3`; member 61 has band 3, so the negative condition is false. Group 190 is the positive
twin and matches. `Support skill 0` has no rows at all in `MasterSupportSkillEffect` (the slot is
empty), so it contributes nothing.

**`duration_ms(member 61, snap 15) = 5000 + 3000 = 8000`.**

**Different-band snap — snap 1 (高松 燈), `bands == [1]`, `support_skills == [1, 0]`:**

| effect row `_id` | `_supportSkillID` | `_skillConditionGroup` | `_effectValue` | `condition_group_matches` |
|---:|---:|---:|---:|---|
| 5 | 1 | 187 | **1500** | `true` |
| 10 | 1 | 186 | **3000** | `false` |

Group 187 is `_conditionType 5000, _isPositive false, _conditionTargetIDs [3]`, and target 3 is
`_bandID == 1`; member 61 has band 3, so the negative condition is true.

**`duration_ms(member 61, snap 1) = 5000 + 1500 = 6500`.**

Both values were cross-checked against the real `model.duration_ms`. For reference, snap 61 gives
`duration_ms = 8000` for **all** 63 members (support skill 71 has `_skillConditionGroup == 0`, which
`condition_group_matches` short-circuits to `True`; support skill 73 has no `15000` row).

---

## 8. Tables that `model.py` does **not** consume but a general calculator likely needs

`model.py` reads only: `MasterSkillTarget`, `MasterSkillCondition`, `MasterSkillConditionSet`,
`MasterMemberCard`, `MasterLiveSkill`, `MasterGekisouSkill`, `MasterSupportSkillEffect`,
`MasterLiveSkillEffect`, `MasterLeaderSkillEffect`, `MasterLiveNoteParameter`,
`MasterLiveComboScoreBonus`, plus the derived `members`, `snaps`, `songs`, `ideal`, and the chart files.
Everything else is available in `inputs.json` but unused:

| table | rows | key fields | why a calculator needs it |
|---|---:|---|---|
| `MasterBandItemSkillEffect` | 1250 | `_id, _bandItemId, _level, _skillTargetIDs, _skillEffectType, _effectValue` | `model.py` hard-codes `band_item = np.full(3, 2500)`. The real band-item bonus is here: `_skillEffectType` is **`1000` for all 1250 rows** (flat AP), 25 distinct `_bandItemId` (101–105, 201–205, …, 501–505) × 50 levels. Example: `{"_id": 1, "_bandItemId": 101, "_level": 1, "_skillTargetIDs": [3], "_skillEffectType": 1000, "_effectValue": 10}`. `_skillTargetIDs` points into `MasterSkillTarget`, so band-item value is **band-scoped**, not flat. |
| `MasterBandItemLevel` | 750 | `_id, _bandItemId, _level, _playerRank` | Unlock schedule (25 items × 30 ranks). Needed to know which band-item levels are reachable. |
| `MasterVipRankBonus` | 125 | `_id, _vipRank, _vipBonusType, _value` | `model.py` bakes in `vip_power_bonus_bp = 2000` (the max over `_vipBonusType == 7`). A general calculator needs the whole curve: `_vipRank` 1..20 and 8 distinct `_vipBonusType` values. |
| `MasterCharacterRank` | 50 | `_id, _rank, _exp, _bonus` | Source of `ideal['character_rank']`. `_bonus` is a **flat AP number** (not a bp), added directly in `slot_power`. |
| `MasterCharacterTotalRank` | 57 | `_id, _totalRank, _bonus` | Source of `ideal['character_total_bonus']` (floored lookup ≤ 1250). Full curve runs `_totalRank` 25 … 5000 step 100 with `_bonus` 0 … 280. |
| `MasterCharacter` | 25 | `_id, _nameTextID, _shortNameTextID, _bandID, _displayOrder, _mainColorCode, _subColorCode, _instrumentTypes, _bandPart, _birthdayMonth, _birthdayDay, …` | Supplies `band` for members/snaps. Also needed for character identity/display. |
| `MasterText` | 9928 | `_id, _english, _japanese, _korean, _simplifiedChinese, _traditionalChinese` | All names/titles in `members[]`, `snaps[]`, `songs[]` are resolved through this table. Keys are **strings** (e.g. `"ui_binary_version"`), not only ints. |
| `MasterMemberCardLevel` | 330 | `_id, _group, _level, _exp, _performanceRate, _technicRate, _visualRate` | Level curve for the member `trained` computation. |
| `MasterMemberCardRank` | 5 | `_id, _group, _rank, _requiredRankUpItemCount, _performanceRate, _technicRate, _visualRate, _leaderSkillLevel, _musicTypeBonusRate, _musicTagBonusRate` | Supplies `rank` in `members[]` and the `_musicTypeBonusRate` / `_musicTagBonusRate` used as bps. |
| `MasterMemberCardAwake` | 5 | `_id, _group, _awakeCount, _performanceRate, _technicRate, _visualRate` | Third term of `trained`. |
| `MasterSupportCardLevel` | 420 | `_id, _group, _level, _exp, _performanceRate, _technicRate, _visualRate` | Level curve for snap `trained`. |
| `MasterSupportCardRank` | 25 | `_id, _group, _rank, _limitLevel, _requiredRankUpItemCount, _supportSkill01Level, _supportSkill02Level, _gekisouSupportSkill01Level, _gekisouSupportSkill02Level, _cardTypeLinkBonusRate` | Supplies snap `rank` and `_cardTypeLinkBonusRate`. |
| `MasterLiveSkill` | 8 | see §6.4 | `_skillCategories` is used by `target_matches`; `_displaySkillCategories` / `_skillDisplayCategories` are not. |
| `MasterLeaderSkill` / `MasterSupportSkill` | 220 / 63 | `_id, _nameTextID, _descriptionTextFormatID, _skillIconID` (+ `_displaySkillCategories` for support) | Metadata (names, icons, display categories) not read by `model.py`; needed for any UI. |
| `MasterGekisouSkill` | 22 | `_id, _nameTextID, _descriptionTextFormatID, _skillIconID, _gekisouMissionType, _skillCategories, _displaySkillCategories, _skillDisplayCategories` | `_gekisouMissionType` and `_skillCategories` are used by `target_matches`; the display fields are not. Example: `{"_id": 1, ..., "_gekisouMissionType": 3, "_skillCategories": [1], ...}`. |
| `MasterEventEffect` | 20 | `_id, _eventId, _resourceTypeConstraint, _characterId, _bandId, _cardType, _tagId, _memberCardId, _supportCardId, _eventBonusType, _rank1EffectValue … _rank5EffectValue` | The full event-bonus rule table. `prepare.py` reads only rank 5 and only `_eventBonusType == 2`, discarding `_eventBonusType` 1 (5 rows) and 0 (5 rows) and all ranks 1–4 — so any rank other than 5 is not modelled. `_resourceTypeConstraint` is 2 for member cards, 3 for support cards. |
| `MasterLiveSettings` | 62 | `_id, _key, _value, _subValue` | Score/life constants entirely unused by `model.py` (see §6.1). |
| `MasterParameter` | 76 | `_id` (string), `_type`, `_value` | `prepare.py` builds `params` from this and then **never uses it**. `_type` values: `{Int32: 54, String: 11, Int64: 10, int32: 1}`. Example: `{"_id": "live_boost_recovery_time", "_type": "Int32", "_value": "1800"}`. |
| `MasterLiveMusic` / `MasterLiveMusicScore` / `MasterChallengeMusic` | 85 / 340 / 3 | see §6.9 | Needed for any song beyond the 3 pre-joined ones. |
| `MasterMemoryMemberLevel` / `MasterMemorySupportLevel` / `MasterMemoryMusicBonus` | **0 / 0 / 0** | — | Present but empty: memory/support-level bonuses **cannot** be computed from this dataset. |
| `MasterMemberCard` / `MasterSupportCard` | 63 / 64 | raw card rows | Consumed only indirectly by `prepare.py` to build `members[]` / `snaps[]`; the derived arrays are the intended interface. |

---
## 9. Where the data contradicts, or fails to exercise, `model.py`

1. **`MasterLeaderSkillEffect` contains effect types outside `model.py`'s assert set.**
   `leader_bp` asserts `typ in [1001, 1002, 1003]` for any matched, non-1000 row. The table has
   `_skillEffectType == 0` (165 rows) and `== 1503` (15 rows). The `0` rows all have **empty**
   `_skillTargetIDs`, so `any(...)` is `False` and they are skipped. The `1503` rows (values 8000/9000/10000,
   `_skillTargetIDs == [51]`) **do** have targets, but belong to `_leaderSkillID` 90088/90089/90090,
   which no member in this dataset uses. The code only survives because of the specific 63-card pool —
   this is a **latent `AssertionError`**, not a structural guarantee.

2. **`leader_bp` ignores `_skillConditionGroup`.** At level 5, 31 of 229 rows have a non-zero condition
   group (26, 36, 167–185). For the 52 leader skills used by members, all level-5 rows have group 0,
   so this dataset never exercises the omission.

3. **`target_matches` ignores `_judgement` and `_liveMusicType`.** `MasterSkillTarget` rows 41–47 carry
   real `_judgement` values (1..6) and row 47 also carries `_liveMusicType == 1`. These are silently
   treated as always-true. `_tagID` and `_gekisouSkillCategories` are handled by the code but are
   **never** non-zero in this dataset, so those branches are dead here.

4. **`condition_matches` only supports `_conditionType == 5000`** and raises `ValueError` otherwise.
   `MasterSkillCondition` has 32 distinct types; the reachable set from the support-skill level-5
   condition groups (`186..195`) is exactly `{5000}`, so no `ValueError` occurs. Leader/live/gekisou
   effect condition groups (e.g. 14, 15, 26, 36, 167–185) reference other types and are never evaluated.

5. **`live_boost` takes `max()` over all level-5 rows of the live skill**, ignoring `_skillConditionGroup`,
   `_skillTargetIDs` and `_skillEffectType`. Live skills 6 and 7 have two competing level-5 branches
   (skill 6: 11000 at group 14 vs 9000 at group 15 → max 11000; skill 7: 14000 at group 14 vs
   12000 at group 15 → max 14000). The code comment claims "choose the matching high-LIFE branch" but
   no condition is actually evaluated. **Unverified** which branch is correct.

6. **Chart ops missing from `MasterLiveNoteParameter`.** Ops `100, 103, 121, 122, 80, 82` appear in the
   12 charts (592 notes with op 122 alone, across 1051 notes in `0109_03` and the other 11 charts) but
   have no row in the table. All of them are `judgement == false`, and `model.py` indexes the table only
   for judged notes, so there is no `KeyError` — but the table is **not** a complete op enum for the
   charts, and the model relies on that coincidence.

7. **`MasterLiveComboScoreBonus._comboBonusType == 1` is completely unused.** 40 rows (thresholds
   10..400) are dropped. `model.py` hard-codes type `0`, whose own top threshold is 500.

8. **Snap base power is reused as a bp vector.** In `slot_power`, `bonuses['snap'] = np.array(s['trained'])
   + s['event_bonus_bp']`. `s['trained']` is raw AP (500–3000 scale, e.g. `[1400, 1200, 1300]`), yet it
   is added to a basis-points value and then multiplied by `common` and divided by 10000. This is
   dimensionally inconsistent; it reproduces the author's intent only by construction. **Flagged, not resolved.**

9. **`prepare.py`'s event-bonus rule filter is lossy.** It requires `_eventBonusType == 2` and reads only
   `_rank5EffectValue`. `MasterEventEffect` also has 5 rows with `_eventBonusType == 1`, 5 with `0`, and
   ranks 1–4 are dropped. For member 61 the result is still `10000` bp (rules 2+8+10), but a rank-1..4
   calculation is not supported by the data as prepared.

10. **`MasterMemoryMemberLevel`, `MasterMemorySupportLevel`, `MasterMemoryMusicBonus` are empty (0 rows).**
    Any memory-based power/bonus term is uncomputable from this `inputs.json`.

11. **`prepare.py` builds `params` from `MasterParameter` but never uses it**, and `ideal` floors
    `_totalRank` to 1200 although `character_total_rank` is 1250. Both are modelling choices, not
    readable constants.

12. **`MasterLiveSettings` is not read at all** — including `life_base` (1000) and
    `note_score_adjustment_factor` (3), which the `chart_weights` AP model arguably needs.
    `model.py`'s comment ("At AP and LIFE=1000") hard-codes what the table states.

13. **`ideal['character_rank']['_bonus']` (245) and `ideal['character_total_bonus']['_bonus']` (90) are
    added as flat AP to all three components** in `slot_power`. Whether the real game applies these
    before or after the bonus multipliers is not evidenced in this repo.

---

## 10. Open questions

1. **Leader-skill condition groups.** 31 level-5 leader-skill rows carry a non-zero `_skillConditionGroup`
   (26, 36, 167–185). No member in this pool uses those skills, so their semantics are unverified.
   Do they gate on judgement counts, combo, or score rank? Which `_conditionType`s apply?
2. **`_skillEffectType` 0 and 1503.** `0` (165 rows) and `1503` (15 rows, values 8000/9000/10000,
   target `[51]`) have no mapping in `model.py`. Is `1503` a distinct AP channel (e.g. a gekisou score
   bonus) that should feed `slot_power`, `live_boost`, or a separate term?
3. **`live_boost` branch selection.** For live skills 6 and 7, which of the two level-5 rows (groups 14
   vs 15) is live at LIFE = 1000 / AP? Is `max()` correct, or does group 14 vs 15 distinguish
   LIFE thresholds or PERFECT/GREAT states?
4. **The `snap` bonus vector.** Should `s['trained']` (raw AP, e.g. 1400) really be added to
   `event_bonus_bp` (5000) as a bp vector? What is the intended unit?
5. **Band-item bonus.** `model.py` hard-codes 2500 flat. `MasterBandItemSkillEffect` has 1250 rows
   (`_skillEffectType` 1000, `_skillTargetIDs` band-scoped, `_effectValue` per level). Does the real
   term equal `2500` for every configuration, or is that the author's placeholder?
6. **`_judgement` / `_liveMusicType` targets.** Targets 41–47 encode judgement and music-type
   conditions. How do these interact with `MasterLeaderSkillEffect` and `MasterSupportSkillEffect`
   rows that reference them (e.g. `MasterLiveSkillEffect._skillTargetIDs == [41, 46]` for live skills
   4 and 5)?
7. **`MasterLiveNoteParameter` completeness.** Six ops used by the shipped charts (`100, 103, 121, 122,
   80, 82`) have no `_scorePercent`. Are they non-scoring decorations by design, or is the table
   truncated in this overlay? Op 122 alone accounts for 280 of 1051 notes in 100109 expert.
8. **`_comboBonusType == 1`.** What mode uses it, and does it replace or add to type 0?
9. **`MasterLiveSettings` gaps.** `_id` 49–52 are absent. Are those settings retired, or redacted?
10. **Memory tables are empty.** Where do `MasterMemoryMemberLevel` / `MasterMemorySupportLevel` /
    `MasterMemoryMusicBonus` rows come from in a build that includes memory content?
11. **`_skillConditionGroup` / `_conditionValues` semantics for types other than 5000.** 172 conditions
    carry non-empty `_conditionValues` (dominant types 4011, 1030) but no consumer in this repo defines
    their meaning.
12. **Score-weighting boundary.** `chart_weights` uses `times >= start and times < start + duration`
    with `np.searchsorted(..., side='left')` for the prior combo. The author's own comment says the
    "Protected runtime boundary details must still be verified against a client score trace" — the
    inclusive/exclusive boundary and chord/combo tie-breaking remain unvalidated here.
13. **`MasterPlayerRank`-style level caps.** `MasterBandItemLevel._playerRank` hints at a player-rank
    gate, but no `MasterPlayerRank` table is present in `inputs.json`, so player-rank-dependent
    availability cannot be modelled.
