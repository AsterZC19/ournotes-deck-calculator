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

搜索最高分队伍和最佳技能顺序：

```sh
./build/deckcalc rank -p problem.json --method fast --top 10 --detail -o rank.json
```

不限时精确搜索第一名：

```sh
./build/deckcalc rank -p problem.json --method exact --time-limit 0 --detail -o best.json
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

`ranking_score` 为理论最高分，`order_analysis.best_score_order` 为对应技能顺序。实际技能顺序随机。精确搜索完成后，`audit.theoretical_max_certified` 标记模型内第一名已证明；普通 `--top 10` 的其他名次仍是候选。

`--time-limit` 单位为秒，0 表示不限时；`--warm-start` 可复用历史结果。查看完整参数：`./build/deckcalc --help`。

## 许可证

MIT，见 [LICENSE](LICENSE)。
