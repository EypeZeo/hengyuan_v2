"""
年化因子推导 — 批次 0

**这个模块修的是一个真实的计算错误，不是风格问题。** 在它存在之前，
``BacktestConfig.annualization_factor`` 的默认值硬编码为 ``252``——那是**股票交易日**的数量
（一年约 252 个交易日），而这个仓库的唯一数据源是 Binance 现货（``py_core/market_data/
binance_public_rest.py``），是 **7×24 全年无休**的市场。后果是所有年化指标被系统性算错：

- 日线：真实因子应为 365，用 252 会把年化收益和 Sharpe 都**低估**约 sqrt(365/252) ≈ 1.20 倍
  （Sharpe）/ 相应比例（年化收益）。
- 小时线：真实因子应为 8760，用 252 会把 Sharpe **低估**约 sqrt(8760/252) ≈ 5.9 倍。

也就是说，时间周期越细，错得越离谱，而且方向是"让策略看起来比实际更差"——这个方向不会诱发
过度自信，但它同样会让一个真实有效的策略被误判为无效而丢弃。

**修法**：不再让调用方记得传对一个魔数，而是**从数据自身携带的 market + timeframe 推导**
（``NormalizedOhlcvRecord`` 本来就带这两个字段）。``BacktestConfig.annualization_factor``
默认改为 ``None``（= 自动推导），显式传值仍然可以覆盖。

所有输出均为回测估算值，不代表财务建议、交易授权或实盘批准。
"""

from __future__ import annotations

from py_core.manual_ohlcv import ManualMarket, NormalizedOhlcvRecord, OhlcvTimeframe

# OhlcvTimeframe 的校验正则是 ^[1-9]\d*[mhdw]$（已规范化为小写），所以单位只可能是这四个。
_SECONDS_PER_UNIT: dict[str, int] = {
    "m": 60,
    "h": 3_600,
    "d": 86_400,
    "w": 604_800,
}

# 7×24 市场（加密货币现货）一年的日历秒数。不扣任何休市时间——Binance 现货不休市。
CALENDAR_SECONDS_PER_YEAR = 365 * 86_400

# 股票市场的年交易日惯例。仅用于日线及以上周期；日内周期见
# annualization_factor_for() 的 fail-closed 分支。
EQUITY_TRADING_DAYS_PER_YEAR = 252

# 7×24 市场按周期直接推导，不需要"交易日"这个概念。
_CONTINUOUS_MARKETS = frozenset({ManualMarket.CRYPTO_SPOT})


def timeframe_seconds(timeframe: OhlcvTimeframe) -> int:
    """把 OhlcvTimeframe 解析成一根 bar 的秒数。

    通用解析（``<正整数><m|h|d|w>``），不是查表——OhlcvTimeframe 的正则允许任意正整数倍数
    （"7h"、"45m" 都是合法的），查表会在遇到没预料到的周期时静默漏掉。

    Raises:
        ValueError: timeframe 的单位不在 m/h/d/w 中（正常情况下 OhlcvTimeframe 自己的
            __post_init__ 已经挡掉了，这里是防御性检查）。
    """
    value = timeframe.value
    unit = value[-1]
    if unit not in _SECONDS_PER_UNIT:
        raise ValueError(
            f"无法解析 timeframe {value!r} 的单位 {unit!r}（支持 {sorted(_SECONDS_PER_UNIT)}）"
        )
    count = int(value[:-1])
    return count * _SECONDS_PER_UNIT[unit]


def annualization_factor_for(market: ManualMarket, timeframe: OhlcvTimeframe) -> float:
    """给定市场与时间周期，返回一年包含多少根 bar（即年化因子）。

    加密货币现货（7×24）：直接用 日历年秒数 / 单根 bar 秒数。1d → 365、1h → 8760、
    15m → 35040、1w → 52.14…（**刻意返回 float 而不是取整**：1w/3d 这类周期除不尽，取整会
    引入不必要的偏差）。

    股票市场（A 股/美股/港股）：只支持日线及以上——日线 252、周线 252/5=50.4，按
    EQUITY_TRADING_DAYS_PER_YEAR 折算。**日内周期直接抛错，不猜**：日内 bar 只存在于开市
    时段内，正确的因子取决于每个交易所每天的连续竞价时长（A 股 4 小时、美股 6.5 小时、港股
    5.5 小时，还要考虑午休），这个模块没有交易日历，猜一个数字比报错更危险。调用方如果确实
    需要，显式给 BacktestConfig.annualization_factor 传值覆盖。

    Raises:
        ValueError: 股票市场 + 日内周期（无交易日历，fail-closed 不猜）。
    """
    bar_seconds = timeframe_seconds(timeframe)

    if market in _CONTINUOUS_MARKETS:
        return CALENDAR_SECONDS_PER_YEAR / bar_seconds

    # 非连续交易市场：只在"整交易日"及以上有明确惯例。
    if bar_seconds < 86_400:
        raise ValueError(
            f"市场 {market.value!r} 不是 7×24 连续交易，日内周期 {timeframe.value!r} 的年化因子"
            "取决于该交易所每日连续竞价时长与交易日历，本模块没有交易日历、不做猜测——"
            "请显式给 BacktestConfig.annualization_factor 传一个你自己核算过的值。"
        )
    bar_days = bar_seconds / 86_400
    return EQUITY_TRADING_DAYS_PER_YEAR / bar_days


def annualization_factor_for_records(records: list[NormalizedOhlcvRecord]) -> float:
    """从一批已校验的 OHLCV 记录推导年化因子。

    只看 records[0] 的 market/timeframe——同一批记录的市场与周期必须一致，这一点由
    ``py_core.manual_ohlcv.validate_ohlcv_batch()`` 在更早的导入/拉取阶段保证，这里不重复
    校验（重复校验会掩盖"某条路径绕过了 batch 校验"这个真正的问题）。

    Raises:
        ValueError: records 为空，或 annualization_factor_for() 自身抛出。
    """
    if not records:
        raise ValueError("records 不能为空，无法推导年化因子")
    head = records[0]
    return annualization_factor_for(head.market, head.timeframe)
