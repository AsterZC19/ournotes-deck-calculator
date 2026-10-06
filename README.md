# ournotes-deck-calculator

BanG Dream! Our Notes 组队计算器：搜索成员与 Snapshot 配对、最高分技能顺序，以及 CP 活动的 PT 和商店货币收益。

## 构建

需要支持 C++20 的编译器、Make 和 Python 3。导入原始谱面时需要 NumPy。

```sh
make
make test
```

## 准备输入

从 Master 和谱面生成计算输入：

```sh
python3 tools/compile_from_master.py \
  --master-dir /path/to/master \
  --charts-dir /path/to/charts \
  --event-id 1 --song-context challenge \
  --songs 100109 --difficulty expert --output problem.json
```

普通曲使用 `--song-context normal`；难度可选 `easy`、`normal`、`hard`、`expert`。未提供账号文件时，默认按满养成导入。可直接使用 `examples/` 中的示例输入。

## 导入实际账号

将 Penlight-Notes-API 的 `GET /api/jp/user/export` 导出的 `ournotes-account@1` JSON 保存为 `imports/account.json`，按实际卡池、养成和道具加成生成输入：

```sh
python3 tools/compile_from_master.py \
  --master-dir /path/to/master --charts-dir /path/to/charts \
  --event-id 1 --song-context challenge --difficulty expert \
  --account imports/account.json --output output/account.json \
  --formation-output output/main-deck.json
```

省略 `--songs` 时生成该期全部课题曲。`--formation-output` 可选，默认转换当前编队；`--deck-id 7` 可选择其他保存编队。技能序号会从游戏的 0–4 转换为计算器的 1–5。

普通曲使用 `--song-context normal`；课题曲使用 `--song-context challenge`，应用活动综合力加成。账号模式需要匹配的 Master 和谱面，不能与 `--ideal-*`、`--band-item-bp` 混用。账号文件和计算结果可分别放在已忽略的 `imports/`、`output/` 目录。

## 计算

日常推荐默认按随机技能顺序的平均分选队（AP、恒定生命模型）：

```sh
./build/deckcalc rank -p problem.json --method fast --top 10 --detail -o rank.json
```

不限时精确搜索理论最高分第一名：

```sh
./build/deckcalc rank -p problem.json --objective score --method exact --time-limit 0 --detail -o best.json
```

逐名证明前十：

```sh
python3 tools/prove_top_k.py -p problem.json -o reports/top10 --count 10
```

计算指定队伍的最高分和最佳技能顺序：

```sh
./build/deckcalc score -p problem.json -f formation.json --objective score --detail -o score.json
```

计算 CP 活动普通曲与课题曲的收益配队：

```sh
./build/deckcalc event -p normal.json --challenge-problem challenge.json \
  --method fast --time-limit 3 -o event.json
```

批量比较普通曲并复用课题曲搜索结果：

```sh
./build/deckcalc event -p normal1.json -p normal2.json \
  --challenge-problem challenge.json --method exact --time-limit 0 -o batch.json
```

活动精确搜索使用 `--method exact --time-limit 0`；`optimality_certified` 表示输入歌曲、卡池和队长范围内的收益最优已证明，限时未完成时为 `false`。收益按最佳技能顺序比较。

`--objective mean` 优化全部 120 种技能顺序的算术平均分；`--objective score` 优化其中的最高分。正常输入的 `rank` 默认使用 `mean`。两者都保留 `mean_score`、`best_score` 和 `worst_score`，技能顺序随机、最佳顺序不可控制。

`ranking_score` 对应当前目标。精确搜索完成后，`audit.mean_score_certified` 或 `audit.theoretical_max_certified` 标记模型内第一名已证明；普通 `--top 10` 的其他名次仍是候选。平均分使用最高分的安全上界剪枝，可能需要更长时间证明最优；限时中断和快速搜索只提供候选。

成员和 Snapshot 卡池不设固定数量上限，角色编号也不受 64 位位掩码限制；实际可处理规模取决于内存、搜索时间和卡池组合数。正常游戏输入强制恰好 5 人、成员卡/角色/Snapshot 不重复。

`--time-limit` 单位为秒，0 表示不限时；`--warm-start` 可复用历史结果。查看完整参数：`./build/deckcalc --help`。

## 游戏输入与实验输入

`input_mode` 默认为 `game`。正常输入锁定客户端计分公式、取整方式和加成来源；不支持的队长效果或条件会报错。当前支持队长条件：目标匹配、整队中任意成员匹配、整队全部成员匹配、歌曲属性匹配及正反条件。整队条件按实际选择的成员计算，不用候选卡池代替编队。未提供账号时的满养成输入仍然属于理想卡池，不代表实际持有情况。

算法测试和自定义模型必须在 problem 顶层写 `"input_mode": "experimental"`，并显式使用：

```sh
./build/deckcalc rank -p simulation.json --experimental --objective index
```

实验模式允许 1–8 人、自定义计分和去重设置，但始终禁止同一张成员卡重复。小规模穷举测试已迁入实验模式；输出标明 `input_mode`。`--experimental` 不会放宽标记为 `game` 的输入。旧输入如有 `leader_unmapped_effect_types: "ignore"`，游戏模式需改为 `"error"`；其他实验设置需显式迁移。使用实验输入运行 `prove_top_k.py` 时也必须传 `--experimental`。

生命变化、竞技激奏和 CP 奖励的未校准部分没有因本次修改变成完整客户端模型；活动收益仍按最佳技能顺序比较。新增平均分目标用于 `rank` 和显式 `score --objective mean`，不代表已实现活动奖励的期望值优化。

## 许可证

MIT，见 [LICENSE](LICENSE)。
