# PLAN-04 2025 年时间外验证

## 预先固定的范围与规则

使用 UTC 2025-01-01 00:00 至 2026-01-01 00:00 的 BTCUSDT 和 ETHUSDT 现货 4 小时行情，左端包含、右端不含。该自然年紧接 2024 年诊断区间，评价区间互不重叠。每个交易对须覆盖 2,190 根已收盘 K 线。2025 年现货档案使用微秒时间戳。 [Binance 档案说明](https://github.com/binance/binance-public-data#klines)

两种趋势窗口、21 日持有上限、50% 入场配置、入金时点、三个参照方案及成本假设均沿用 2024 年方案。每种交易对分别以初始 1,000 CNY 和月追加 0、200、500 CNY 运行。趋势从评价区间内的数据预热。汇率继续使用已收盘的 Coinbase USDT/USD 日线和前一日已发布的 ECB USD/CNY 交叉汇率，最老成分观察时间限制仍为 168 小时。 [2024 年固定方案](PLAN_04_HISTORICAL_DIAGNOSTIC.md)

## 判定标准

- 数据必须覆盖全部 365 个 UTC 日，30 个组合均进入实验台账。任何组合失败均保留并报告，时间外比较整体标为未完成。
- 每个趋势候选在同交易对、同入金情景下，以买入持有为主要参照。综合盈亏严格高于买入持有且最大回撤不高于买入持有时，该情景达到收益改进标准。
- 最大回撤低于买入持有、综合盈亏低于或等于买入持有时，单列为回撤改善伴随收益未改善。报告两项变化的大小，并同时列出持有 CNY 和持有 USDT 的结果。
- 某一趋势窗口须在两个标的和三种入金情景的全部六项比较中均达到收益改进标准，才能记为本轮一致性标准通过。逐项保留结果，评价后不据此调整本轮参数。

这一轮使用公开档案完成固定方案的时间外复核。正式盲测、经过选择控制的统计推断和实盘准入按各自流程验证。

## 时间外结果

两个交易对各覆盖 2,190 根连续 K 线和 365 个 UTC 日，30 个组合均完成，无拒绝订单。每个交易对使用相同输入复跑，JSON 报告逐字一致，两轮共登记 60 项。

| 交易对 | 每月追加 CNY | 持有 CNY | 持有 USDT | 买入持有 | 趋势 14 日 | 趋势 28 日 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| BTCUSDT | 0 | 0.00 | −42.88 | −104.71 | −502.66 | −450.60 |
| BTCUSDT | 200 | 0.00 | −108.29 | −435.43 | −814.13 | −566.26 |
| BTCUSDT | 500 | 0.00 | −206.41 | −933.36 | −1,278.79 | −742.91 |
| ETHUSDT | 0 | 0.00 | −42.88 | −149.08 | −67.12 | −195.47 |
| ETHUSDT | 200 | 0.00 | −108.29 | −56.92 | 126.48 | −148.27 |
| ETHUSDT | 500 | 0.00 | −206.41 | 79.91 | 417.82 | −76.29 |

方案列为扣除适用费用后的综合盈亏，单位为 CNY。月追加 0、200、500 CNY 的累计投入分别为 1,000、3,200 和 6,500 CNY。

下表各项为趋势候选减去同情景买入持有的差值。回撤差为负表示趋势方案回撤更小。差值按原始精度计算后舍入。

| 交易对 | 每月追加 CNY | 14 日盈亏差 CNY | 14 日回撤差（百分点） | 28 日盈亏差 CNY | 28 日回撤差（百分点） |
| --- | ---: | ---: | ---: | ---: | ---: |
| BTCUSDT | 0 | −397.96 | −14.55 | −345.89 | −23.07 |
| BTCUSDT | 200 | −378.70 | −14.46 | −130.83 | −23.14 |
| BTCUSDT | 500 | −345.44 | −14.51 | 190.44 | −23.19 |
| ETHUSDT | 0 | 81.96 | −43.20 | −46.40 | −48.72 |
| ETHUSDT | 200 | 183.40 | −43.23 | −91.35 | −48.75 |
| ETHUSDT | 500 | 337.90 | −43.27 | −156.20 | −48.81 |

14 日候选的三个 ETHUSDT 情景达到收益改进标准，28 日候选的 BTCUSDT 每月追加 500 CNY 情景达到标准。两组分别通过 3/6 和 1/6，均未达到跨情景一致性标准。两组趋势策略在十二项比较中的最大回撤均较低，其中八项的综合盈亏也较低。上述结果延续了 2024 年诊断中的回撤差异，综合盈亏的相对排序随交易对及入金情景变化。两组趋势方案以 50% 配置比例入场，买入持有使用全部可用报价资产，回撤比较包含不同仓位规模的影响。

## 本地复现

原始公开档案、转换后的输入、两份比较报告和实验台账保存在 `%LOCALAPPDATA%/HengYuan-Research/plan04-2025-v1`。准备命令读取 24 个 Binance 月度档案、ECB 的 `D.CNY+USD.EUR.SP00.A` CSV 及 Coinbase 日线 JSON 文件。ECB 数据日期为 2024-12-20 至 2025-12-31，Coinbase UTC 请求区间以 2024-12-20、2025-07-01、2026-01-01 为边界。 [Coinbase K 线接口](https://docs.cdp.coinbase.com/api-reference/exchange-api/rest-api/products/get-product-candles) · [ECB 发布安排](https://www.ecb.europa.eu/stats/policy_and_exchange_rates/euro_reference_exchange_rates/html/index.en.html)

在仓库根目录运行，准备输入时使用一个新目录。比较命令每次使用新的运行编号。

```powershell
$root = Join-Path $env:LOCALAPPDATA 'HengYuan-Research/plan04-2025-v1'
$inputs = Join-Path $root 'replay-inputs'
python -B -m py_core.tools.prepare_plan04_history --year 2025 --raw-dir (Join-Path $root 'raw') --output-dir $inputs
python -B -m py_core.tools.compare_plan04 --run-id btc-2025-repro --historical-manifest (Join-Path $inputs 'BTCUSDT.json')
python -B -m py_core.tools.compare_plan04 --run-id eth-2025-repro --historical-manifest (Join-Path $inputs 'ETHUSDT.json')
```
