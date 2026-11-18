"""STR-TREND-01 — 首个固定名称的趋势策略候选（PLAN-04 / 任务 3.7 第一片）。

研究规划 §5.1 冻结的第一批规则（参数变更须新实验编号，不在本模块内调参）：

- **决策周期**：4h（本策略只看 bar 序列本身；周期由数据的时间框决定，构造参数是
  ``trend_bars`` 与 ``max_hold_bars`` 的 bar 数，不是日数——调用方负责按决策周期
  换算：4h 决策下 4 日 = 24 bar，28 日 = 168 bar，21 日持有上限 = 126 bar）。
- **方向信号**：过去 ``trend_bars`` 个 bar 的价格变化（当前收盘 / ``trend_bars`` 前
  收盘 − 1）。大于零时允许进入，不大于零时退出。
- **持有时间上限**：连续持有多达 ``max_hold_bars`` 个 bar 后强制退出，**即使趋势仍为
  正**；退出后必须先看到进入条件解除（方向信号 ≤ 0 至少一次），才允许下一次进入
  ——"达到时间上限后，即使趋势仍为正，也需先看到进入条件解除，再允许下一次进入"。
- **趋势自身退出**（方向信号 ≤ 0）本身就是进入条件解除，退出后趋势再转正即可直接
  再进入，不受重置门槛约束。

状态机（三个状态，逐 bar 前缀确定——SAFE-05 前缀差分对每个截断前缀给出相同结果）：

    FLAT ──trend>0──▶ HOLDING
    HOLDING ──trend≤0──▶ FLAT            （趋势退出，无重置门槛）
    HOLDING ──held≥max_hold──▶ AWAIT_RESET（时间上限退出）
    AWAIT_RESET ──trend≤0──▶ FLAT        （进入条件解除 = 重置）
    AWAIT_RESET ──trend>0──▶ AWAIT_RESET（继续等待，不进入）

warm-up 区间（不足 ``trend_bars`` 历史）输出 NaN，与既有策略约定一致。

所有输出均为研究/回测用途，不代表交易授权。本模块不做参数选择：trend_bars=24/168
两个候选是登记在案的独立实验（PLAN-01），不是本策略的默认值之争。
"""

from __future__ import annotations

from typing import Any

import pandas as pd

from py_core.strategies.base import Strategy, validate_signal_output

#: 研究规划 §5.1 冻结的第一批默认参数（4h 决策）：4 日趋势 = 24 个 4h bar。
TREND_BARS_4H_4D = 24
#: 28 日趋势 = 168 个 4h bar。
TREND_BARS_4H_28D = 168
#: 21 日持有上限 = 126 个 4h bar。
MAX_HOLD_BARS_4H_21D = 126

# 状态机的三个状态（内部用，不导出——外部观察的只有信号值本身）。
_FLAT = 0
_HOLDING = 1
_AWAIT_RESET = 2


class TrendFollowingStrategy(Strategy):
    """STR-TREND-01：趋势方向 + 持有时间上限 + 上限退出后的重置门槛。"""

    def __init__(
        self,
        trend_bars: int = TREND_BARS_4H_4D,
        max_hold_bars: int = MAX_HOLD_BARS_4H_21D,
    ) -> None:
        if trend_bars <= 0:
            raise ValueError(f"trend_bars 必须为正整数，当前值: {trend_bars}")
        if max_hold_bars <= 0:
            raise ValueError(f"max_hold_bars 必须为正整数，当前值: {max_hold_bars}")
        if max_hold_bars < trend_bars:
            # 不是非法，但几乎必然是参数换算错误（持有上限短于趋势窗口）——loudly 拒绝
            # 而不是静默接受一个进不去的策略。
            raise ValueError(
                f"max_hold_bars ({max_hold_bars}) 小于 trend_bars ({trend_bars})——"
                "持有上限短于趋势窗口几乎必然是 bar 数换算错误（例如把日数当 bar 数传入）"
            )
        self.trend_bars = trend_bars
        self.max_hold_bars = max_hold_bars

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        close = df["close"]
        past_return = close.pct_change(periods=self.trend_bars)

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
