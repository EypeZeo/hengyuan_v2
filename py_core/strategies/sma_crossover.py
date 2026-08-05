"""
SMA 交叉策略 — 策略框架的一个具体示例实现 — P2-STRAT-01

快线上穿慢线时持有多头仓位（1.0），否则空仓（0.0）。这是教科书级别的示例，用来验证
Strategy 抽象本身是否够用，不是一个经过验证的可交易策略——不代表任何交易建议。
"""

from __future__ import annotations

from typing import Any

import pandas as pd

from py_core.strategies.base import Strategy, validate_signal_output


class SmaCrossoverStrategy(Strategy):
    """快/慢简单移动平均线交叉。fast_window < slow_window 时才有意义。"""

    def __init__(self, fast_window: int = 10, slow_window: int = 30) -> None:
        if fast_window <= 0 or slow_window <= 0:
            raise ValueError(
                f"fast_window/slow_window 必须为正整数，当前值: fast_window={fast_window}, "
                f"slow_window={slow_window}"
            )
        if fast_window >= slow_window:
            raise ValueError(f"fast_window ({fast_window}) 必须小于 slow_window ({slow_window})")
        self.fast_window = fast_window
        self.slow_window = slow_window

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        fast = df["close"].rolling(window=self.fast_window, min_periods=self.fast_window).mean()
        slow = df["close"].rolling(window=self.slow_window, min_periods=self.slow_window).mean()
        signal = (fast > slow).astype(float)
        # rolling() 产生的 warm-up 区间：fast/slow 至少一个为 NaN 时比较结果是 False，
        # astype(float) 后已经变成 0.0——为了不让"看起来像有效信号"的 0.0 掩盖掉"其实数据
        # 还不够"这个事实，显式把两者任一为 NaN 的位置设回 NaN，交给下游
        # reindex().fillna(0.0) 统一按空仓处理，语义跟直接产出 0.0 一致，但更诚实地暴露了
        # warm-up 区间。
        signal = signal.mask(fast.isna() | slow.isna(), other=float("nan"))
        validate_signal_output(signal, df)
        return signal
