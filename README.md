# ournotes-deck-calculator

BanG Dream! Our Notes 的组卡计算器。

## 构建

```sh
make        # 产出 build/deckcalc
make test   # 跑单元测试
```

## 用法

```sh
# 校验 problem
./build/deckcalc validate -p problem.json

# 给一个编队算分
./build/deckcalc score -p problem.json -f formation.json -o score.json

# 搜索前 20 个不同成员组合
./build/deckcalc rank -p problem.json --top 20 --time-limit 60 -o rank.json
```

`-p` 和 `-f` 传 `-` 表示从 stdin 读，`-o` 省略就写到 stdout。

`rank` 有三种搜索方式：

- `fast`：默认。束搜索加局部改进，很快，结果是候选
- `exact`：先跑一遍 fast，再用剩下的时间做分支定界，跑完才在 audit 里标 `certified`
- `auto`：卡池小的时候用 exact，否则用 fast

`--time-limit` 是整个搜索的秒数预算，超时就输出当前最好的结果。

## 输入输出

字段定义见 [docs/design.md](docs/design.md)，计分模型见 [docs/model.md](docs/model.md)。

现在只算组卡的出分指数和单曲预计分数，不算活动 pt：

- `index` = `综合力 × (1 + Σ 技能收益)`，只在同一份 problem 内排序用，不是游戏结算分
- `estimated_score` 只有 problem 给出 `settings.score_model.level_alpha` 时才算，且没和游戏结算核对过
- 活动加成只作为属性加成进入综合力；活动 pt、道具掉落加成、撃奏真人排名都不计算

## 生成 problem

从解包 Master 直接生成：

```sh
python3 tools/compile_from_master.py \
  --master-dir /path/to/master-decrypted/<CURRENT> \
  --charts-dir /path/to/event-detail/charts \
  --event-id 1 --songs 100109 --difficulty expert \
  --output problem.json
```

已经有研究项目编译好的 `inputs.json` 时，用 `tools/compile_from_inputs.py --help`。
`examples/` 里有一份能直接跑的小 problem。

## 许可证

MIT。`tools/convert_chart.py` 来自 [empty-sekai/nnnotes](https://github.com/empty-sekai/nnnotes)，MIT。
