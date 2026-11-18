# PLAN-04 历史行情诊断

## 预先固定的范围

分别使用 BTCUSDT 和 ETHUSDT 现货 4 小时行情，评价窗口为 2024-01-01 00:00 UTC 至 2025-01-01 00:00 UTC，左端包含、右端不含。每个交易对须覆盖 2,196 根已收盘 K 线。选择 2024 年作为固定历史区间，行情使用同一毫秒时间格式。两种标的独立运行，全部组合均记录结果。 [Binance 公开行情格式](https://github.com/binance/binance-public-data#klines)

每个交易对各运行初始 1,000 CNY、随后每月追加 0、200、500 CNY 的三种情景。每种情景比较持有 CNY、持有 USDT、按入金日买入持有、14 日趋势和 28 日趋势。趋势策略使用 21 日持有上限和 50% 入场配置比例。趋势从评价窗口内的数据预热。固定周期、成本情景和评价区间后运行一次历史诊断，保留全部 30 个结果。 [相同资金流比较规则](PLAN_04_MATCHED_CASH_FLOW.md)

## 时点和成本

按 Coinbase Exchange 的 USDT-USD 日 K 线收盘价估值 1 USDT 对应的美元数，自该日结束后的 UTC 零点开始使用。USD/CNY 由 ECB 同一观察日的 EUR/CNY 除以 EUR/USD 得出，统一从观察日次日 00:00 UTC 起使用。每根市场 K 线开盘使用此前最近可获得的两项估值。任一成分的观察时间超过 168 小时则拒绝运行。档案日期和保守可用时间用于重放，修订前的原始报价及成交汇率仍待获取。 [Coinbase 日 K 线接口](https://docs.cdp.coinbase.com/api-reference/exchange-api/rest-api/products/get-product-candles) · [ECB 发布安排](https://www.ecb.europa.eu/stats/pdf/exchange/Frameworkfortheeuroforeignexchangereferencerates.en.pdf)

| 假设 | BTCUSDT | ETHUSDT |
| --- | ---: | ---: |
| 价格步长 | 0.01 USDT | 0.01 USDT |
| 数量步长 | 0.00001 BTC | 0.0001 ETH |
| 最小订单金额 | 5 USDT | 5 USDT |
| 交易费和滑点，每侧 | 10 bp、5 bp | 10 bp、5 bp |
| CNY 兑换费用 | 20 bp | 20 bp |
| 趋势策略账户外运行费用 | 每月 30 CNY | 每月 30 CNY |

以上为固定模拟值。研究台账登记数据版本、交易对、资金情景与成本。报告期末综合盈亏和最大回撤，并保留日收益均值与 Sharpe 差异、拒绝订单以及未成交结果。经济比较属于历史诊断，后续独立保留数据的评价须使用另行冻结的实验方案。

## 历史诊断结果

两个交易对各读入 2,196 根连续 K 线，涵盖 366 个 UTC 日。30 个组合均已完成，无拒绝订单。每个交易对使用同一输入重新运行，JSON 报告分别逐字一致，两轮共登记 60 项。

| 交易对 | 每月追加 CNY | 累计投入 CNY | 持有 CNY | 持有 USDT | 买入持有 | 趋势 14 日 | 趋势 28 日 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| BTCUSDT | 0 | 1,000 | 0.00 | 23.66 | 1,261.55 | −172.46 | −384.07 |
| BTCUSDT | 200 | 3,200 | 0.00 | 51.61 | 2,350.93 | 220.22 | −528.87 |
| BTCUSDT | 500 | 6,500 | 0.00 | 93.54 | 3,991.76 | 803.05 | −742.98 |
| ETHUSDT | 0 | 1,000 | 0.00 | 23.66 | 494.76 | −215.25 | −144.74 |
| ETHUSDT | 200 | 3,200 | 0.00 | 51.61 | 768.61 | −46.61 | −15.95 |
| ETHUSDT | 500 | 6,500 | 0.00 | 93.54 | 1,177.20 | 203.14 | 177.34 |

方案列为扣除适用费用后的综合盈亏，单位为 CNY。每月追加 200 CNY 时，BTCUSDT 的买入持有、14 日、28 日方案最大回撤分别为 29.95%、15.28%、23.70%，ETHUSDT 分别为 46.20%、18.58%、20.18%。两组趋势在该年份的最大回撤均低于对应买入持有方案，综合盈亏也低于对应买入持有方案。结果保留为历史诊断，独立样本的策略评价仍待完成。

[2025 年时间外验证](PLAN_04_TEMPORAL_VALIDATION.md) 使用相同策略参数和成本假设复核这些观察。

## 本地输入与复现

`--historical-manifest` 读取单个交易对的本地 JSON 清单，清单记录标的、数据版本、评价区间、费用与数量规则。K 线 CSV 使用 `open_time_utc`、`close_time_utc`、`open_price`、`close_price` 四列，关闭时点为下根 K 线的开盘时点。汇率 CSV 使用 `observed_at_utc`、`available_at_utc`、`quote_cny_rate` 三列，观察时间使用两个汇率成分中较早的时点。命令拒绝时间缺口、覆盖范围不一致、开盘时尚无可用汇率和过期汇率。

原始下载文件、归一化 CSV、清单和比较报告保存在本地 `%LOCALAPPDATA%/HengYuan-Research/plan04-2024-v1`。准备输入时使用 24 个 Binance 月度文件。ECB 序列为 EXR 的 `D.CNY+USD.EUR.SP00.A`，日期范围从 2023-12-20 到 2024-12-31。Coinbase 的两个 UTC 请求区间以 2023-12-20、2024-07-01、2025-01-01 为边界，粒度为 86,400 秒。从仓库根目录运行，每次使用新的运行编号。

```powershell
$inputs = Join-Path $env:LOCALAPPDATA 'HengYuan-Research/plan04-2024-v1'
python -B -m py_core.tools.compare_plan04 --run-id btc-2024-repro --historical-manifest (Join-Path $inputs 'BTCUSDT.json')
python -B -m py_core.tools.compare_plan04 --run-id eth-2024-repro --historical-manifest (Join-Path $inputs 'ETHUSDT.json')
```
