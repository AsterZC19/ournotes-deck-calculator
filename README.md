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

Penlight-Notes-API 的 `GET /api/jp/user/export` 输出版本化的 `ournotes-account@1` JSON。可用 API 项目中的 `scripts/export_account.py` 将文件保存到本项目的 `imports/account.json`；API Key 从 `PENLIGHT_API_KEY` 环境变量读取。导出包含卡池、实际养成、角色 Rank、乐队道具、VIP 和保存编队，不包含登录凭据。`imports/` 已加入 Git 忽略规则。

```sh
python3 tools/compile_from_master.py \
  --master-dir /path/to/master --charts-dir /path/to/charts \
  --event-id 1 --song-context challenge --difficulty expert \
  --account imports/account.json --output output/account.json \
  --formation-output output/main-deck.json
```

省略 `--songs` 时，按 `--event-id` 生成该期所有课题曲，每首歌分别输出计算输入。`--formation-output` 可选，默认转换账号当前编队；用 `--deck-id 7` 可选择其他已保存编队。空编队不能转换。游戏的技能序号 0–4 会转换为计算器的 1–5。

账号模式只使用实际持有的卡牌，根据 Master 的经验档位解析等级，并应用实际 Rank、觉醒数和技能等级；角色加成分别按角色计算。不能与 `--ideal-*` 或 `--band-item-bp` 混用。请使用匹配的 Master；未知卡牌或缺失养成档位会报错。账号 JSON 不包含 Master 和谱面；谱面可使用原始文件或已转换的 `nnnotes.live-score/1` JSON。

生成的输入可直接用于下方的 `rank` 搜索或 `score` 编队计算；文件名中的歌曲 ID 和难度应与所选输入对应。账号输入与计算结果建议放在已忽略的 `imports/`、`output/` 或 `reports/` 目录。

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
