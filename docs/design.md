# ournotes-deck-calculator 设计契约（v1）


## 1. 目标与非目标

目标

- 输入 problem 与可选 formation，输出 result。
- 支持任意歌曲与难度、任意活动加成规则、自定义卡池（成员 / Snapshot / 角色 Rank / 乐队道具 / VIP）。
- 两种求解模式：`score`（给定配队算分）与 `rank`（全卡池搜索前 N 个不同成员组合）。
- 演出条件可配：判定分布、生命、Assist、撃奏、技能时点。
- 所有近似与未校准项必须在输出里显式声明，绝不把未校准绝对值当成游戏结算分。

非目标（v1）

- 不做 UI / 服务端；CLI + 库 API。
- 不解析游戏客户端，不携带任何 Master 数据；数据由 problem JSON 承载，
  `tools/compile_from_master.py` 负责生成。
- 不声称搜索结果是全局最优，除非 `audit.certified` 为 true。

## 2. 术语

- **member**：成员卡（等级 / 觉醒 / 特训属性已折算进 `trained`）。
- **snapshot**：支援卡（Snapshot），提供属性百分比加成与支援技能。
- **slot**：编成一格 = 一名成员 + 一张 Snapshot + 一个技能触发位（1..5）。
- **trigger / order**：第 k 次技能事件（1..5）。原生实现里成员顺序是 Fisher–Yates 随机洗牌，
  因此 trigger 与站位无关。
- **power**：五项综合力之和（Performance + Technique + Visual），整数。
- **weight**：谱面音符的计分权重（`score_percent/100 × 连击倍率`）。
- **index**：模型排序指标 `power × W_total`，`W_total = 1 + Σ_slot w_slot`。
  与参考研究项目输出的 `index` 逐位可比（参考实现里的 `×1e6` 只是 `power/1e6` 缩放的抵消项）。

## 3. 计分模型（必须实现的语义）

### 3.1 单格综合力

对成员 `m`、Snapshot `s`、歌曲 `song`：

```text
trained          = m.trained                        # 3 维整数属性
with_event_bonus = trained + floor32(trained × m.event_bonus_bp / 10000)
common           = with_event_bonus + flat          # flat = 角色 Rank + 角色总 Rank，3 维整数

每条加成来源 v（3 维 bp，10000 = +100%），单独取整：
    part(common, v) = floor32(common × v / 10000)
power_dim        = common + Σ part(common, v)
slot_power       = Σ_dims power_dim
```

`floor32(x) = floor(float32(x) / float32(10000))`，逐维整数运算。

v1 内置来源（由 `settings.power_model.sources` 逐项开关，取 0 即关闭）：

| 来源 | v（bp） |
|---|---|
| `snapshot` | `s.trained + s.event_bonus_bp`（Snap 的 `trained` 本身就是 bp 百分比） |
| `type_link` | `s.card_type == m.card_type` ? `type_link_base_bp + m.card_rank_bonus_bp.type_link` : 0 |
| `band_item` | `band_item_bonus_bp`（全队统一） |
| `music_type` | `song.type == m.card_type` ? `music_type_base_bp + m.card_rank_bonus_bp.music_type` : 0 |
| `music_tag` | `tags(song) ∩ tags(m) ≠ ∅` ? `music_tag_base_bp + m.card_rank_bonus_bp.music_tag` : 0 |
| `leader` | 队长技能加成向量（见 3.2），与其他来源一样走 `part()` |
| `vip` | `vip_bonus_bp` |
| `extra` | problem 的 `power_model.extra_sources[]`，任意 3 维 bp 或标量 |

`extra_sources[]` 是“各种情况”的兜底扩展点，条目形如
`{"id": "...", "rate_bp": [p,t,v] | 5000, "scope": "slot" | "all", "when": {"member_tags_any": [...], "card_types": [...]}}`。
**不允许**表达式语言（不引入 eval / 公式字符串）。

### 3.2 技能

- **LIVE Skill**：成员卡自带。`live_boost(m)` = 该技能在 `settings.live_level` 的效果值中，
  按 `settings.skill.live_effect_select` 选择后的 `value / 10000`。参考模型取 level 5 的最大值
  （AP + LIFE=1000 走“高 LIFE”分支）。
- **Leader Skill**：中央成员的技能。效果行先按 `targets[]` 与成员匹配，再把 `value`（bp）
  累加到 3 维向量；轴由 `settings.leader_effect_axes` 决定（原生枚举映射，不是字段名顺序）：

  ```json
  {"1000": "all", "1001": "technique", "1002": "visual", "1003": "performance"}
  ```

  出现映射表里没有的 `effect_type` 必须报错；`"all"` 表示三维各加。
- **Support Skill**（Snapshot）：`effect_type == duration_extension_effect_type`（默认 15000）的行，
  若 `condition_group` 对该成员成立，则 `value`（毫秒）累加进技能持续时间：

  ```text
  duration_ms(m, s) = base_duration_ms + Σ extension
  ```

- **技能覆盖**：技能位 `k` 在 `skill_times_ms[k]` 触发，覆盖
  `{i | start ≤ notes[i].t < start + duration_ms}`（左闭右开）。
- **重叠**：v1 只实现 `additive`（`skill_factor = 1 + Σ 生效技能 boost`）；
  `settings.skill.overlap` 取其他值必须报错，不得静默降级。
- **目标 / 条件匹配**（`engine.cpp`）：target 行的约束字段为 0 / 空表示不限；
  `live_skill_categories` / `gekisou_skill_categories` 要求交集非空。
  condition 行 `{type: 5000, positive: bool, targets: [...]}`：任一 target 匹配得 true，
  再按 `positive` 取真值；`condition_group` 为该组下**任一** condition 行的**全部**条件成立。

### 3.3 谱面权重与连击

```text
judged  = [n for n in chart.notes if n.scoring]
prior_i = searchsorted(judged.t, judged.t, side='left')   # 严格早于本音符毫秒的判定数
w_i     = note_parameter(op).score_percent / 100
          × (1 + Σ combo_bonuses[type = settings.combo_type].factor
                   for bonus if prior_i >= bonus.required_combo_count)
base    = Σ w_i
converted_note_count = ceil(Σ score_percent / 100)        # 原生 GetConvertedNoteCount
```

同毫秒多押共享之前连击（原生 `FindLastIndexBefore`）。连击表按 `combo_type` 分组承载；
撃奏连击表即 `type = 1`。

### 3.4 指数与技能收益

```text
w_slot(k, m, s) = Σ_{i 被该技能覆盖} w_i × live_boost(m) / base
W_total         = 1 + Σ_{k=1..5} w_slot(k, m_k, s_k)
index           = (Σ_slot slot_power) × W_total
```

### 3.5 绝对分数（可选，默认关闭）

只有 `settings.score_model.level_alpha` 显式给出时才计算；否则 `estimated_score = null`，
并在 warnings 里说明 α 未校准。

```text
base_note_power = power × adjustment_factor
                  × (1 + (chart.level - level_base) × level_alpha) / converted_note_count
score_i         = round_policy(base_note_power × w_i / base
                               × judgement_factor_i × life_factor_i × assist_factor
                               × state_factor_i × luck_factor_i × gekisou_factor_i)
total_score     = Σ score_i
```

`round_policy ∈ {float32_floor, float64_floor, none}`，默认 `float32_floor`
（逐音符 float32 后向下取整）。`judgement` 支持
`all_perfect` / `fixed` / `sequence` / `random`（带 seed）；`life` 支持
`constant` / `per_note`；`assist` 关闭时系数 1.0，开启时 `score_percent/100`。
逐音符取整与技能系数执行时刻仍未核验，输出必须标 `model.calibrated = false`。

### 3.6 撃奏

`settings.gekisou.enabled` 时按 `sections[{start_ms, end_ms, bonus_bp}]` 分段：
`total = Σ_k section_score_k × bonus_k`。v1 只做分段乘算，不建模真人排名。

## 4. 模块边界

| 文件 | 职责 |
|---|---|
| `src/json.hpp` / `json.cpp` | JSON 解析与序列化 |
| `src/model.hpp` / `model.cpp` | problem / formation 的结构、解析、默认值与校验 |
| `src/engine.hpp` / `engine.cpp` | 目标与条件匹配、综合力、谱面权重、配队评估 |
| `src/solver.hpp` / `solver.cpp` | beam 搜索、局部改进、分支定界排名 |
| `src/main.cpp` | 子命令 `validate` / `score` / `rank` |

依赖方向单向：`json → model → engine → solver → main`。可视化与前端一律不进入核心，
`solver` 不得修改 `engine` 的计分语义。

语言为 C++20，只依赖标准库；`Makefile` 直接产出 `build/deckcalc`。

## 5. problem JSON（`schema: ournotes-deck-problem@1`）

```jsonc
{
  "schema": "ournotes-deck-problem@1",
  "meta": {"generated_at": "...", "source": "...", "notes": "..."},
  "song": {"id": 100109, "title": "夢我夢中", "type": 3, "bands": [1], "tags": [7, 8]},
  "chart": {
    "difficulty": "expert", "level": 26, "display_level": 26.0, "full_combo_count": 759,
    "notes": [{"t": 1234, "op": 1, "scoring": true}],
    "skill_times_ms": [9411, 21960, 47058, 59607, 84705]
  },
  "catalog": {
    "members": [{
      "id": 61, "name": "千石 ユノ", "title": "ハート・リブート",
      "character": 21, "band": 3, "card_type": 2, "rarity": 4,
      "trained": [1, 2, 3], "event_bonus_bp": 5000, "tags": [7],
      "live_skill": 1001, "leader_skill": 2001, "gekisou_skill": 0,
      "card_rank_bonus_bp": {"type_link": 2000, "music_type": 2000, "music_tag": 2000},
      "level": 90, "rank": 5, "awake": 5
    }],
    "snapshots": [{
      "id": 63, "name": "...", "title": "...", "characters": [1, 2], "bands": [1],
      "card_type": 2, "rarity": 4, "trained": [3200, 3200, 3200], "event_bonus_bp": 0,
      "rank": 5, "level": 50, "support_skills": [5001, 0]
    }],
    "fix": {
      "character_rank_bonus": [245, 245, 245], "character_total_rank_bonus": [90, 90, 90],
      "band_item_bonus_bp": [2500, 2500, 2500], "vip_bonus_bp": [2000, 2000, 2000],
      "type_link_base_bp": 500, "music_type_base_bp": 500, "music_tag_base_bp": 500
    },
    "live_skills": [{"id": 1001, "categories": [1], "effects": [{"level": 5, "effect_type": 0, "value": 9000}]}],
    "gekisou_skills": [{"id": 0, "mission_type": 0, "categories": []}],
    "leader_skills": [{"id": 2001, "effects": [{"level": 5, "effect_type": 1003, "value": 4000, "targets": [1]}]}],
    "support_skills": [{"id": 5001, "effects": [{"level": 5, "effect_type": 15000, "value": 5000, "condition_group": 0}]}],
    "targets": [{"id": 1, "character": 0, "band": 0, "card_type": 0, "tag": 0,
                 "gekisou_mission_type": 0, "live_skill_categories": [], "gekisou_skill_categories": []}],
    "conditions": [{"id": 1, "type": 5000, "positive": true, "targets": [1]}],
    "condition_groups": [{"group": 3, "rows": [[1, 2]]}],
    "note_parameters": [{"op": 1, "score_percent": 100}],
    "combo_bonuses": [{"type": 0, "required_combo_count": 100, "factor": 0.01}]
  },
  "settings": {
    "team_size": 5,
    "combo_type": 0,
    "base_duration_ms": 5000,
    "live_level": 5, "leader_level": 5, "support_level": 5,
    "duration_extension_effect_type": 15000,
    "leader_effect_axes": {"1000": "all", "1001": "technique", "1002": "visual", "1003": "performance"},
    "skill": {"overlap": "additive", "activation": "scheduled",
              "live_effect_select": {"effect_type": null, "branch": "max"}},
    "power_model": {
      "rounding": "float32_floor",
      "sources": {"snapshot": true, "type_link": true, "band_item": true,
                  "music_type": true, "music_tag": true, "leader": true, "vip": true},
      "extra_sources": []
    },
    "score_model": {"adjustment_factor": 3, "level_base": 5, "level_alpha": null,
                    "rounding": "float32_floor"},
    "judgement": {"mode": "all_perfect", "factors": {"perfect": 1.0}, "default_factor": 0.0, "seed": 0},
    "life": {"mode": "constant", "initial": 1000, "onus_factor": 0.3, "damage": {}},
    "assist": {"enabled": false, "score_percent": 90},
    "gekisou": {"enabled": false, "sections": []}
  },
  "constraints": {
    "distinct_characters": true, "distinct_snapshots": true,
    "member_pool": null, "snapshot_pool": null, "leader_pool": null,
    "banned_members": [], "banned_snapshots": [],
    "required_members": [], "required_snapshots": []
  },
  "search": {"mode": "rank", "top": 20, "time_limit_s": 60, "order_search": "exact",
             "prescreen": true, "seed": 0}
}
```

校验规则：成员 / Snapshot 的 `id` 唯一；`note_parameters` 必须覆盖 chart 中所有 `scoring = true`
的 `op`（缺失即报错）；`skill_times_ms` 长度等于 `team_size`；`constraints.leader_pool` 非空时，
队长必须在该集合内。

## 6. formation JSON（`schema: ournotes-deck-formation@1`）

```json
{
  "schema": "ournotes-deck-formation@1",
  "leader": 61,
  "slots": [
    {"member": 55, "snapshot": 61, "trigger": 1},
    {"member": 59, "snapshot": 62, "trigger": 2},
    {"member": 61, "snapshot": 63, "trigger": 3},
    {"member": 62, "snapshot": 64, "trigger": 4},
    {"member": 63, "snapshot": 65, "trigger": 5}
  ]
}
```

`trigger` 可省略 → 按 slots 顺序 1..5。`leader` 必须出现在 slots 中。

## 7. result JSON（`schema: ournotes-deck-result@1`）

```jsonc
{
  "schema": "ournotes-deck-result@1",
  "kind": "score",
  "tool": {"name": "ournotes-deck-calculator", "version": "0.1.0"},
  "problem": {"song": {"id": 100109, "title": "夢我夢中", "type": 2},
              "chart": {"difficulty": "expert", "level": 26, "judged_notes": 759},
              "catalog": {"members": 63, "snapshots": 64,
                          "available_members": 63, "available_snapshots": 64}},
  "model": {"id": "ournotes-index@1", "calibrated": false, "assumptions": ["..."]},
  "chart_analysis": {"difficulty": "expert", "level": 26, "judged_notes": 759,
                     "converted_note_count": 525, "note_weight_sum": 642.3585,
                     "all_note_weight_sum": 524.1, "prior_combo_rule": "strictly_before",
                     "skill_times_ms": [9411, 21960, 47058, 59607, 84705]},
  "results": [{
    "rank": 1, "leader": 61, "members": [55, 59, 61, 62, 63],
    "assignments": [{"trigger": 1, "member": 62, "snapshot": 63, "duration_ms": 5000,
                     "power": 977786, "live_boost": 0.9, "weighted_skill_gain": 0.051450864276}],
    "power": 3124510,
    "weight_factor": 1.6301943540873147,
    "index": 5093558.561289356,
    "estimated_score": null,
    "order_analysis": {"evaluated_permutations": 120, "best_order": [62, 59, 63, 55, 61],
                       "best_gain": 0.6301943540873147, "worst_gain": 0.61,
                       "worst_loss_percent": 3.2}
  }],
  "search": {"method": "fast", "top": 20, "time_limit_s": 60, "leaders": ""},
  "audit": {"solver": "beam", "certified": false, "stop_reason": "heuristic",
            "elapsed_s": 1.49, "candidates": 85, "leaders_tried": 63,
            "beam_width": 64, "seed": 0},
  "warnings": ["level_alpha 未提供，estimated_score 为 null；index 只用于同一 problem 内排序。"]
}
```

`kind = score` 时 `results` 恰好 1 条，没有 `search` 与 `audit`。
`--detail` 时每个 `assignments[]` 额外带 `power_breakdown`，`estimated_score` 额外带逐音符分数。

## 8. 验证与验收

1. **回归**：用 `tools/compile_from_inputs.py` 从参考 `inputs.json` 生成 100109/expert problem，
   对参考 `results-100109-expert.json` 的首位配队逐项核对 `power`、`weight_factor`、`index`，
   以及每个 `assignments[]` 的 `duration_ms` / `power` / `weighted_skill_gain`，相对误差 ≤ 1e-9。
   `chart_analysis` 中 `judged_notes = 759`、`converted_note_count = 525`、
   `all_note_weight_sum = 524.1`、含连击的 `note_weight_sum = 642.3585`。
2. **搜索**：同一 problem 跑 `rank --method fast --leaders 61 --top 1`，必须得到 leader 61、
   成员集合 {55, 59, 61, 62, 63}，index 与参考一致。
3. **通用性**：四张参考榜单（100109 expert / hard、100056 expert、100063 expert）全部回归；
   自定义卡池时删除成员后结果不含它；关闭某项 power source 后 power 按预期下降。
4. **失败模式**：缺失 `op` 参数、`skill_times_ms` 长度不符、`overlap` 非 `additive`、
   未知 schema 版本、settings 里的未知字段都必须报明确错误。
5. 单元测试：`make test`，逐个运行 `build/test_*`。

## 9. 版本策略

- `ournotes-deck-*@1` 为当前版本；读取未知版本必须报错。
- 模型语义变化时递增 `model.id`，并在 `model.assumptions` 里写明改动。
