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

普通曲使用 `--song-context normal`；难度可选 `easy`、`normal`、`hard`、`expert`。默认按满养成导入，个人计算需调整卡池、Rank、等级和技能等级。可直接使用 `examples/` 中的示例输入。

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
./build/deckcalc event -p normal.json --challenge-problem challenge.json -o event.json
```

`ranking_score` 为理论最高分，`order_analysis.best_score_order` 为对应技能顺序。实际技能顺序随机。精确搜索完成后，`audit.theoretical_max_certified` 标记模型内第一名已证明；普通 `--top 10` 的其他名次仍是候选。

`--time-limit` 单位为秒，0 表示不限时；`--warm-start` 可复用历史结果。查看完整参数：`./build/deckcalc --help`。

## 许可证

MIT，见 [LICENSE](LICENSE)。
