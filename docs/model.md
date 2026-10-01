# 计分模型


## 1. 排序指标

```
index = power × (1 + Σ 槽位技能收益)
```

`power` 是五人综合力之和，`Σ 槽位技能收益` 是五个技能位按覆盖权重归一化后的收益之和。
指标只用于同一 problem 内排序，不是游戏结算分。

## 2. 综合力

单格先是成员属性，再逐条加加成来源：

```
with_event = trained + floor32(trained × event_bonus_bp / 10000)
common     = with_event + 角色 Rank 加成 + 角色总 Rank 加成
part(v)    = floor32(common × v / 10000)          # 每个来源单独取整
slot_power = Σ_dims (common + Σ part)
```

`floor32(x) = floor(float32(x) / float32(10000))`。来源包括 Snapshot 属性、类型链接、乐队道具、
音乐类型、音乐标签、队长技能、VIP/T.G.W，全部以 bp 为单位载入 problem。

类型链接的加成率来自 **Snapshot** 的 Rank 行，音乐类型与音乐标签来自 **成员** 的 Rank 行。
这一点由 `MasterMemberCardRank` 与 `MasterSupportCardRank` 的实际字段决定：成员 Rank 行没有
`_cardTypeLinkBonusRate`，Snapshot Rank 行没有音乐类型与标签字段。
`settings.type_link_bonus_source` 可以切换，默认 `snapshot`。

## 3. 谱面权重与连击

```
prior_i = 严格早于第 i 个音符毫秒数的判定音符数
w_i     = score_percent(op_i) / 100 × (1 + Σ_满足门槛的连击倍率(prior_i))
```

`prior_i` 用二分查找得到，同毫秒的多押共享之前的连击值，对应原生 `ComboCounter.FindLastIndexBefore`
的严格小于语义。连击表按 `combo_bonus.type` 分组，普通模式用 type 0，撃奏模式用 type 1。

`converted_note_count = ceil(Σ score_percent / 100)`，与原生 `GetConvertedNoteCount` 一致。

## 4. 技能

- 技能位 `k` 在 `skill_times_ms[k]` 触发，覆盖 `[start, start + duration)` 内的音符
- `duration = base_duration_ms + Σ 命中的支援技能延长`，命中与否由目标与条件组决定
- 技能收益 `w_slot = Σ覆盖权重 × live_boost / base`
- `live_boost` 取 LIVE 技能指定等级的效果值除以 10000，默认走 `max` 分支
- 队长技能的效果类型到属性轴的映射写在 `settings.leader_effect_axes`，默认
  `1000 → 三维`、`1001 → Technique`、`1002 → Visual`、`1003 → Performance`。这个映射来自原生枚举，
  不是字段顺序
- 重叠按倍率相加。多个技能同时生效时忽略交叉项，因此 `index` 是线性化口径

## 5. 绝对分数

只有 problem 显式给出 `score_model.level_alpha` 才计算：

```
base_note_power = power × adjustment_factor × (1 + (level − level_base) × α) / converted_note_count
score_i = round(base_note_power × w_i / base × 判定 × 生命 × 技能 × Assist × 撃奏)
```

`adjustment_factor` 来自 Master 的 `note_score_adjustment_factor`，值为 3。
`α` 是受保护的难度系数常量，尚未还原，所以默认不算绝对分数。
逐音符取整位置、技能系数命令的实际执行时刻也还没和客户端逐音符对齐，输出一律标注 `calibrated = false`。

## 6. 回归验证

同一份数据用参考研究项目（`our-notes-event-20260930`）的 `model.py` 与本题的实现分别计算，
对比四张榜单的首位配队，逐格核对综合力、技能持续时间、技能收益与最终 index：

| 歌曲 | 难度 | 首位 index（参考） | 相对误差 | 逐格综合力 |
|---|---|---:|---:|---|
| 夢我夢中 100109 | expert | 5093558.561289373 | 3.3e-15 | 一致 |
| 夢我夢中 100109 | hard | 5493843.786019833 | 2.1e-15 | 一致 |
| これはぼくたちの生存のあらすじ 100056 | expert | 5307919.1248984225 | 4.2e-15 | 一致 |
| オリオンをなぞる 100063 | expert | 5237263.818256518 | 3.0e-15 | 一致 |

其余核对点：100109 expert 判定音符 759、`converted_note_count` 525、`all_note_weight_sum` 524.1、
含连击倍率的权重和 642.3585；成员 61 配 Snapshot 61、队长 61 的单格综合力 829993，
分项 `common = [93395, 73903, 76419]`；成员 61 的技能持续时间同乐队 8000 ms、跨乐队 6500 ms。

## 7. 还没校准的部分

1. 难度系数 α，以及绝对分数里每一个取整位置
2. 技能系数命令的实际执行时刻，当前按计划时刻 `[触发, 触发+持续)` 判定
3. 生命惩罚、护盾与技能回血的精确公式
4. 撃奏区间 Bonus 依赖真人排名，当前只支持外部给定的分段系数
5. 搜索是否达到全局最优：`fast` 方法不证明，`exact` 方法只在其上界剪枝跑完时才有条件性证明
6. 编成合法性规则尚未从客户端核实，`constraints` 只是保守近似

## 8. 证据来源

- 本地 JP 1.0.3 arm64 客户端的原生核对：连击严格小于语义、`note_score_adjustment_factor = 3`、
  `ExecuteCommand` 的分桶与合并顺序、成员洗牌
- 参考研究项目 `our-notes-event-20260930` 的 Master 覆盖层与已解谱面
- 谱面转换来自 [empty-sekai/nnnotes](https://github.com/empty-sekai/nnnotes)，固定 commit，MIT
