"""STR-TREND-01 趋势策略候选（PLAN-04，研究规划 §5.1）。

4 小时决策周期下，分别登记 14 日和 28 日的价格变化窗口，并使用 21 日持有上限。
信号为正时进入，为零或负时退出。触及持有上限后，须先观察到信号为零或负，
才允许在信号再次转正时进入。输入须为连续的 4 小时 K 线，历史不足时输出 NaN。
"""

from __future__ import annotations

from numbers import Integral
from typing import Any

import pandas as pd

from py_core.strategies.base import Strategy, validate_signal_output

#: 研究规划 §5.1 冻结的第一批默认参数（4h 决策）：14 日趋势 = 84 个 4h bar。
TREND_BARS_4H_14D = 84
#: 28 日趋势 = 168 个 4h bar。
TREND_BARS_4H_28D = 168
#: 21 日持有上限 = 126 个 4h bar。
MAX_HOLD_BARS_4H_21D = 126

_DECISION_INTERVAL = pd.Timedelta(4, unit="h")

# 状态机的三个状态（内部用，不导出——外部观察的只有信号值本身）。
_FLAT = 0
_HOLDING = 1
_AWAIT_RESET = 2


class TrendFollowingStrategy(Strategy):
    """STR-TREND-01：趋势方向 + 持有时间上限 + 上限退出后的重置门槛。"""

    def __init__(
        self,
        trend_bars: int = TREND_BARS_4H_14D,
        max_hold_bars: int = MAX_HOLD_BARS_4H_21D,
    ) -> None:
        for name, bars in (("trend_bars", trend_bars), ("max_hold_bars", max_hold_bars)):
            if isinstance(bars, bool) or not isinstance(bars, Integral) or bars <= 0:
                raise ValueError(f"{name} 必须为正整数，当前值为 {bars!r}")
        self.trend_bars = int(trend_bars)
        self.max_hold_bars = int(max_hold_bars)

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        intervals = df.index[1:] - df.index[:-1]
        if not (intervals == _DECISION_INTERVAL).all():
            raise ValueError("STR-TREND-01 需要连续的 4 小时 K 线")
        close = df["close"]
        past_return = close.pct_change(periods=self.trend_bars, fill_method=None)

        values: list[float] = []
        state = _FLAT
        held_bars = 0
        for i in range(len(close)):
            trend = past_return.iloc[i]
            if pd.isna(trend):
                # warm-up：趋势窗口未攒够——与既有策略约定一致，NaN 而不是 0.0，
                # 让"没有足够历史"与"明确空仓"保持可区分。
                values.append(float("nan"))
                continue

            if state == _FLAT:
                if trend > 0:
                    state = _HOLDING
                    held_bars = 1
                    values.append(1.0)
                else:
                    values.append(0.0)
            elif state == _HOLDING:
                if trend <= 0:
                    # 趋势退出：本身就是进入条件解除，回到 FLAT（无重置门槛）。
                    state = _FLAT
                    held_bars = 0
                    values.append(0.0)
                elif held_bars >= self.max_hold_bars:
                    # 时间上限强制退出：即使趋势仍为正。进入 AWAIT_RESET。
                    state = _AWAIT_RESET
                    held_bars = 0
                    values.append(0.0)
                else:
                    held_bars += 1
                    values.append(1.0)
            else:  # _AWAIT_RESET
                if trend <= 0:
                    # 进入条件解除：重置完成，回到 FLAT。
                    state = _FLAT
                # trend > 0 时继续等待（不进入）。
                values.append(0.0)

        signal = pd.Series(values, index=df.index, dtype=float)
        validate_signal_output(signal, df)
        return signal
