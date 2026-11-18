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
