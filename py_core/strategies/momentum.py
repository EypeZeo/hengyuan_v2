"""
动量策略 — 策略框架的第二个示例实现 — P2-STRAT-01

过去 lookback 根 bar 的收益率为正则持有多头仓位（1.0），否则空仓（0.0）。同样是教科书级别
的示例，不是经过验证的可交易策略——不代表任何交易建议。
"""

from __future__ import annotations

from typing import Any

import pandas as pd

from py_core.strategies.base import Strategy, validate_signal_output


class MomentumStrategy(Strategy):
    """N 根 bar 的时间序列动量：过去 lookback 根 bar 的收益率为正则做多。"""

    def __init__(self, lookback: int = 20) -> None:
        if lookback <= 0:
            raise ValueError(f"lookback 必须为正整数，当前值: {lookback}")
        self.lookback = lookback

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        past_return = df["close"].pct_change(periods=self.lookback)
        signal = (past_return > 0).astype(float)
        # 同 SmaCrossoverStrategy：warm-up 区间（前 lookback 根 bar，pct_change 尚无足够历史）
        # 显式设回 NaN，不让"看起来像有效信号"的 0.0 掩盖"数据还不够"这个事实。
        signal = signal.mask(past_return.isna(), other=float("nan"))
        validate_signal_output(signal, df)
        return signal
