# StrategySpec v1 — 声明式策略规格

**状态**：schema 定义（批次 0）。**本轮只定形状，不实现求值器**——Python 侧求值器是批次 3，
C++ 侧求值器是批次 5。

---

## 1. 这个东西为什么存在

研究在 Python（pandas、向量化、分钟级），执行在 C++（实时 book、微秒级）。两边都需要「同一个
策略」，于是有三种做法：

| 做法 | 问题 |
|---|---|
| 策略写两遍（Python 一份、C++ 一份） | 两份实现会**悄悄漂移**。回测验证的是 A，实盘跑的是 B，而且没有任何机制会告诉你它们不一样了。这是量化系统最经典的亏钱方式之一 |
| Python 进程直接把买卖信号写给 C++ | 违反 ADR-018 的 Control Plane 条款（*"不得直接成为买卖信号/风险放行/order intent/执行参数"*）。安全理由也是成立的：一个有 GC 停顿、可以热改代码的进程不该直接指挥真钱下单 |
| **策略是一份声明式数据，两侧各自解释** ← 本文档 | 只有一份真相；Python 永远不发信号、只发「已验证的规则」；C++ 自己从实时行情算信号 |

附带的好处：spec 的参数化形式**同时**是批次 3 参数扫描的接口和批次 7 AI 策略生成的接口。
LLM 生成/修改的是一份受 schema 约束的数据，不是可执行代码——可校验、可审计、可自动回测，
而且没有任意代码执行面。

---

## 2. 三条不可协商的设计约束

这三条决定了算子集合能包含什么，违反其中任何一条的算子一律不进 v1。

### 2.1 每个算子必须有**有界状态的流式实现**

C++ 侧在实盘里一根 bar 一根 bar 地看数据，**不能回头重扫历史**。因此每个算子必须能用
O(window) 的状态增量推进，并且必须声明自己的 warm-up 长度。

这条直接排除掉：全历史排名/分位数、需要未来数据才能定标的归一化、任何 window 不定长的算子。
研究侧写起来毫无障碍的 `df.rank(pct=True)` 恰恰是这类——它在 v1 里不存在，是有意的。

### 2.2 两侧必须**逐 bar 完全一致**，因此算法要唯一确定

「数学上等价」不够，浮点累加顺序不同就会有末位差异，长序列上会放大。所以每个算子的**计算
方式在本文档里被唯一指定**，不是留给实现者选：

- **SMA**：每根 bar 用当前窗口内的原始值重新求和，**不用滑动加减**。滑动加减是 O(1) 更快，
  但它会累积舍入误差且误差依赖于历史路径，两侧实现只要有一点点不同就会分叉。窗口重算是
  O(window)/bar，在 bar 级（不是 tick 级）求值下完全负担得起——**确定性优先于微优化**。
- **EMA**：递推式 `ema[t] = alpha * x[t] + (1 - alpha) * ema[t-1]`，`alpha = 2 / (window + 1)`。
  种子值 `ema[warmup-1] = SMA(x[0..warmup-1])`（**不是** `x[0]`）。种子选择是 EMA 实现之间
  最常见的分歧点，这里钉死。
- 所有中间量一律 `float64`（C++ 侧 `double`）。**不允许**任何一侧用 `float32` 或扩展精度累加器。

批次 5 的一致性测试就是核这一条：同一份 spec + 同一段历史数据，两侧逐 bar 输出必须相同。

### 2.3 结构上不可能有 lookahead

算子只能引用**当前及更早**的 bar。`lag(n)` 只能往回移；**刻意不提供 `lead`**。
这使得「这个策略偷看了未来」在 v1 里不是一个需要靠测试去抓的 bug，而是一个表达不出来的状态。

（`py_core/strategies/base.py:97` 的 `assert_no_lookahead()` 仍然保留，用于核查手写的
`Strategy` 子类——那条路径不受 spec 约束。）

---

## 3. 文件格式

TOML（人写、LLM 生成、diff 都友好）。JSON 是等价的可选序列化，schema 相同。

一份 spec 描述**一个品种、一个周期**的一个策略。多品种组合是批次 3 组合层的职责，由多份 spec
加一份组合配置构成，不塞进单份 spec。

---

## 4. Schema

### 4.1 顶层

```toml
spec_version = 1              # 必填。整数。schema 版本，不是策略版本
name = "sma_crossover_btc_1h" # 必填。^[a-z0-9_]{1,64}$，全局唯一，用作产物目录名
description = "20/50 SMA 金叉做多"  # 可选
```

### 4.2 `[market]` — 这份 spec 绑定的市场

```toml
[market]
market    = "crypto_spot"   # 必填。ManualMarket 的取值
symbol    = "BTCUSDT"       # 必填。Binance 现货符号，^[A-Z0-9]+$
timeframe = "1h"            # 必填。OhlcvTimeframe 形状：^[1-9]\d*[mhdw]$
```

**绑定是强制的、执行期要核对的**：C++ 求值器必须拒绝把一份 `1h` 的 spec 喂给 `15m` 的行情流。
年化因子也从这里推导（`py_core/backtests/annualization.py`）。

### 4.3 `[[indicators]]` — 命名中间量，构成 DAG

按**声明顺序**求值。每个节点可以引用原始字段或**任何在它之前声明过的**节点 id。
只能向前引用这一点让环成为不可能，不需要单独的环检测。

```toml
[[indicators]]
id     = "fast"      # 必填。^[a-z0-9_]{1,32}$，spec 内唯一
op     = "sma"       # 必填。见 §5 算子表
input  = "close"     # 原始字段或更早的 indicator id
window = 20          # 该算子要求的参数
```

原始字段（永远可用，无需声明）：`open`、`high`、`low`、`close`、`volume`。

### 4.4 `[signal]` — 从 DAG 产出目标仓位

```toml
[signal]
node = "entry"          # 必填。某个 indicator id
mode = "boolean"        # 必填。"boolean" | "scaled"
```

- `mode = "boolean"`：节点值为真 → 目标仓位 `1.0`，假 → `0.0`。warm-up 期（NaN）→ `0.0`。
- `mode = "scaled"`：节点值直接作为目标仓位，**硬裁剪到 `[-1.0, 1.0]`**。NaN → `0.0`。

**v1 不支持做空**：`scaled` 模式下负值虽然在 schema 上合法，但当前
`py_core/backtests/risk_integration.py` 的 `_signal_value_to_target()` 只认 long/none，会拒绝。
做空是批次 2a 的范围，届时放开。

### 4.5 `[validation]` — 验证凭据

**这一节由验证流程写入，不由策略作者手写。** C++ 求值器**必须拒绝加载缺少本节或本节不完整的
spec**——这是「Python 只发已验证的规则」这条边界的落地点。

```toml
[validation]
validated_at   = "2026-09-15T08:30:00Z"  # RFC3339 UTC
validated_by   = "walk_forward+cpcv"      # 用了哪套验证流程
data_start_utc = "2021-01-01T00:00:00Z"  # 验证所用数据区间
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:..."             # 数据仓库快照指纹，保证可复现
oos_sharpe     = 1.34                     # 样本外 Sharpe（不是样本内）
pbo            = 0.21                     # 过拟合概率，越低越好
trials         = 480                      # 一共试了多少组参数——deflated Sharpe 的输入
```

`trials` 是刻意必填的：**不知道试了多少次，样本外 Sharpe 就没有意义**。参数扫描必须如实上报
搜索规模，否则多重检验校正无从谈起。

---

## 5. v1 算子集合

每个算子都满足 §2 的三条约束。集合**刻意开得很小**——随批次 3 的真实需要增量扩展，
每次扩展都要同时补 Python 与 C++ 两侧实现加一致性测试。一个「什么都能表达」的 spec
等于没有边界，那就退化回「策略写两遍」了。

### 5.1 窗口类（单输入 + `window`）

| `op` | 含义 | warm-up | 备注 |
|---|---|---|---|
| `sma` | 简单移动平均 | `window` | 窗口重算，见 §2.2 |
| `ema` | 指数移动平均 | `window` | `alpha = 2/(window+1)`，SMA 种子，见 §2.2 |
| `stddev` | 滚动标准差 | `window` | 样本标准差（分母 `n-1`） |
| `rolling_max` / `rolling_min` | 滚动极值 | `window` | |
| `roc` | 变化率 `x[t]/x[t-window] - 1` | `window` | |
| `rsi` | 相对强弱 | `window + 1` | 用 Wilder 平滑（`alpha = 1/window`），**不是** EMA 的 alpha |

### 5.2 移位类

| `op` | 含义 | warm-up |
|---|---|---|
| `lag` | 向后移 `n` 根 bar（`n ≥ 1`） | `n` |

### 5.3 二元算术（`left` / `right`，各自可以是节点 id 或字面量数字）

`add`、`sub`、`mul`、`div`

`div` 的右操作数为 0 时产出 NaN（**不抛异常、不产出 inf**）——两侧行为必须一致，NaN 会被
`[signal]` 当作空仓处理。

### 5.4 二元比较 → 布尔

`gt`、`lt`、`ge`、`le`

`crosses_above`、`crosses_below`：仅在**穿越发生的那一根 bar** 为真，即
`crosses_above(a,b)` ⟺ `a[t] > b[t] && a[t-1] <= b[t-1]`。warm-up 需要多一根 bar。

### 5.5 逻辑

`and`、`or`（二元）、`not`（一元）

### 5.6 三元

`if_then_else`：`cond` / `then` / `otherwise` 三个字段，各自是节点 id 或字面量。

### 5.7 明确不在 v1 的算子

全历史排名/分位数、任何形式的 z-score 全样本归一化（会泄漏未来）、跨品种引用（组合层的
职责）、`lead`/任何前视移位、用户自定义表达式或 lambda。

---

## 6. warm-up 语义（两侧分歧的头号来源）

- 节点的有效 warm-up = **自身 warm-up + 其所有输入 warm-up 的最大值**，逐层向上累加。
- warm-up 未满时节点产出 **NaN**，不是 0、不是前值填充。
- `[signal]` 把 NaN 一律当作目标仓位 `0.0`（空仓）。这跟
  `py_core/backtests/vectorized_engine.py:173` 既有的 `reindex().fillna(0.0)` 容忍度一致。
- **spec 的有效 warm-up 必须能被静态算出**，加载时就报给调用方，不是跑起来才知道。

批次 5 的一致性测试**必须覆盖 warm-up 边界那几根 bar**——两侧最容易差一根的就是这里。

---

## 7. 完整示例

```toml
spec_version = 1
name = "sma_crossover_btc_1h"
description = "20/50 SMA 金叉做多，用 RSI 过滤超买"

[market]
market    = "crypto_spot"
symbol    = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "fast"
op = "sma"
input = "close"
window = 20

[[indicators]]
id = "slow"
op = "sma"
input = "close"
window = 50

[[indicators]]
id = "rsi14"
op = "rsi"
input = "close"
window = 14

[[indicators]]
id = "not_overbought"
op = "lt"
left = "rsi14"
right = 70.0

[[indicators]]
id = "trend_up"
op = "gt"
left = "fast"
right = "slow"

[[indicators]]
id = "entry"
op = "and"
left = "trend_up"
right = "not_overbought"

[signal]
node = "entry"
mode = "boolean"

# 由验证流程写入，作者不手填
[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "walk_forward+cpcv"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0000000000000000000000000000000000000000000000000000000000000000"
oos_sharpe     = 1.34
pbo            = 0.21
trials         = 480
```

该 spec 的有效 warm-up = `max(sma50=50, rsi14=15) = 50` 根 bar。

---

## 8. 后续批次如何接这份 schema

| 批次 | 要做的事 |
|---|---|
| 3 | Python 求值器 + `py_core/indicators/` 指标库（§5 算子的向量化实现）+ 参数扫描回填 `[validation]` |
| 5 | C++ 流式求值器；**逐 bar 一致性测试进 CI**；C++ 侧强制校验 `[validation]` 完整性与 `[market]` 绑定 |
| 7 | LLM 生成/修改 spec → 自动跑批次 2 全套验证 → 达标才写入 `[validation]` 并采纳 |

**schema 变更规则**：任何破坏兼容的改动必须递增 `spec_version`，且两侧求值器同时更新——
不允许出现一侧支持 v2、另一侧只认 v1 的中间状态。
