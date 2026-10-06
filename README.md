# ournotes-deck-calculator

BanG Dream! Our Notes 组队与活动收益计算器。

## 构建

需要 C++20、Make、Python 3；导入原始谱面另需 NumPy。

```sh
make
make test
```

## 导入

账号 JSON 来自 Penlight-Notes-API 的 `GET /api/jp/user/export`，保存到 `imports/account.json`。

```sh
python3 tools/compile_from_master.py \
  --master-dir /path/to/master --charts-dir /path/to/charts \
  --event-id 1 --songs 100054 --song-context normal --difficulty expert \
  --account imports/account.json --output output/problem.json \
  --formation-output output/formation.json
```

- 自由 Live 用 `normal`，课题曲用 `challenge`；省略 `--songs` 导入活动全部课题曲。
- 不传 `--account` 时使用满养成卡池。`--deck-id` 可选择保存编队，队长为中间第 3 人。
- 谱面路径如 `charts/0109/0109_03.json`。账号文件与结果分别放入已忽略的 `imports/`、`output/`。

## 计算

默认按平均分、活动平均收益推荐；假设 AP、恒定生命、技能顺序等概率。

```sh
# 推荐队伍
./build/deckcalc rank -p output/problem.json --method fast --top 10 -o output/rank.json

# 计算指定队伍
./build/deckcalc score -p output/problem.json -f output/formation.json -o output/score.json

# 活动配队
./build/deckcalc event -p normal.json --challenge-problem challenge.json -o output/event.json
```

- `--objective score`：理论最高分／最佳技能顺序收益。
- `--method exact --time-limit 0`：不限时精确搜索；`fast` 和限时未完成的搜索不保证最优。
- `--detail`：计算明细；`--warm-start`：复用结果；`score --order-search given`：计算指定顺序。
- 活动批量比较可重复传 `-p`；`event.assumptions.normal_runs`、`initial_cp` 设置有限预算，结果为期望次数及收益。
- 逐名证明理论最高分：`python3 tools/prove_top_k.py -p problem.json -o reports/top10 --count 10`。

正常输入固定 5 人，成员卡、角色和 Snapshot 不重复，卡池数量无固定上限。
实验输入需同时设置 `input_mode: experimental` 并传 `--experimental`。
其余参数见各工具的 `--help`；示例输入见 [examples](examples/)。

## 许可证

[MIT](LICENSE)。谱面转换器的第三方许可证见 [nnnotes-LICENSE](tools/nnnotes-LICENSE)。
