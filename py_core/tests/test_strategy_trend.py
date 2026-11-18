"""STR-TREND-01（TrendFollowingStrategy）的测试：状态机、边界与 SAFE-05 因果门。

参考语义来自研究规划 §5.1 冻结的第一批规则。所有用例直接构造价格序列驱动状态机的
每一条边，最后用 checked_signals() 证明前缀确定性（因果检查通过）。
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from typing import Any

import pandas as pd
import pytest

from py_core.strategies.base import checked_signals
from py_core.strategies.trend import (
    MAX_HOLD_BARS_4H_21D,
    TREND_BARS_4H_4D,
    TREND_BARS_4H_28D,
    TrendFollowingStrategy,
)

_BASE_DT = datetime(2024, 1, 1, tzinfo=UTC)


def make_df(closes: list[float]) -> pd.DataFrame:
    index = pd.DatetimeIndex([_BASE_DT + timedelta(hours=4 * i) for i in range(len(closes))])
    return pd.DataFrame(
        {
            "open": closes,
            "high": closes,
            "low": closes,
            "close": closes,
            "volume": [1000.0] * len(closes),
        },
        index=index,
    )


def signals_of(closes: list[float], **kwargs: Any) -> list[float]:
    return list(TrendFollowingStrategy(**kwargs).generate_signals(make_df(closes)))


# ---------------------------------------------------------------------------
# 构造参数
# ---------------------------------------------------------------------------


def test_frozen_first_batch_defaults_match_the_plan() -> None:
    assert TREND_BARS_4H_4D == 24
    assert TREND_BARS_4H_28D == 168
    assert MAX_HOLD_BARS_4H_21D == 126


@pytest.mark.parametrize("bad", [{"trend_bars": 0}, {"trend_bars": -3}, {"max_hold_bars": 0}])
def test_non_positive_parameters_are_rejected(bad: dict[str, int]) -> None:
    with pytest.raises(ValueError, match="必须为正整数"):
        TrendFollowingStrategy(**bad)


def test_a_hold_cap_shorter_than_the_trend_window_is_rejected_as_a_unit_error() -> None:
    with pytest.raises(ValueError, match="换算错误"):
        TrendFollowingStrategy(trend_bars=24, max_hold_bars=10)


# ---------------------------------------------------------------------------
# warm-up 与基本方向
# ---------------------------------------------------------------------------


def test_warm_up_is_nan_not_flat() -> None:
    closes = [100.0] * 30
    result = signals_of(closes, trend_bars=5, max_hold_bars=10)
    assert all(pd.isna(v) for v in result[:5])
    assert all(not pd.isna(v) for v in result[5:])


def test_rising_trend_enters_and_falling_trend_exits() -> None:
    closes = [100.0, 100.0, 100.0, 110.0, 120.0, 130.0, 140.0, 100.0, 90.0, 95.0]
    result = signals_of(closes, trend_bars=3, max_hold_bars=100)
    # bar 0-2: warm-up NaN
    assert all(pd.isna(result[i]) for i in range(3))
    # bar 3: close 110 vs close[0]=100 → trend > 0 → 进入
    assert result[3] == 1.0
    # bar 4-6: 持续上升 → 持有
    assert result[4] == 1.0 and result[5] == 1.0 and result[6] == 1.0
    # bar 7: close 100 vs close[4]=120 → trend < 0 → 退出
    assert result[7] == 0.0
    # bar 8: close 90 vs close[5]=130 → trend < 0 → 空仓
    assert result[8] == 0.0
    # bar 9: close 95 vs close[6]=140 → trend < 0 → 空仓
    assert result[9] == 0.0


def test_flat_trend_is_an_exit_not_an_entry() -> None:
    closes = [100.0] * 10
    result = signals_of(closes, trend_bars=3, max_hold_bars=100)
    # 价格不变 → pct_change = 0 → 不大于零 → 空仓
    assert all(v == 0.0 for v in result[3:])


def test_a_trend_exit_allows_immediate_reentry_when_the_trend_turns_positive_again() -> None:
    # 上升 → 持有；下跌 → 趋势退出（无重置门槛）；再上升 → 直接再进入。
    closes = [100.0, 100.0, 100.0, 120.0, 140.0, 160.0, 180.0, 200.0, 90.0, 90.0, 90.0, 150.0, 180.0, 210.0]
    result = signals_of(closes, trend_bars=3, max_hold_bars=100)
    assert result[3] == 1.0  # 首次进入
    assert result[8] == 0.0  # close 90 vs close[5]=160 → 趋势退出
    assert result[9] == 0.0 and result[10] == 0.0  # 下跌中空仓
    assert result[11] == 1.0  # close 150 vs close[8]=90 → 趋势再转正，直接再进入
    assert result[12] == 1.0 and result[13] == 1.0


# ---------------------------------------------------------------------------
# 持有时间上限与重置门槛
# ---------------------------------------------------------------------------


def test_the_hold_cap_forces_an_exit_even_while_the_trend_is_still_positive() -> None:
    closes = [100.0] * 3 + [100.0 + 10.0 * i for i in range(1, 20)]  # 持续上升 16 bar
    result = signals_of(closes, trend_bars=3, max_hold_bars=5)
    # bar 3 进入（held=1），bar 4-7 持有（held=2..5），bar 7 held=5 → 达上限。
    assert result[3] == 1.0
    assert result[4] == 1.0 and result[5] == 1.0 and result[6] == 1.0 and result[7] == 1.0
    # bar 8: held 已达 5（bar 3..7 共持有 5 bar），强制退出。
    assert result[8] == 0.0
    # 趋势仍为正：AWAIT_RESET 保持等待，不进入。
    assert result[9] == 0.0 and result[10] == 0.0 and result[11] == 0.0


def test_after_a_hold_cap_exit_a_negative_trend_releases_the_reset_gate() -> None:
    # 上升触发上限退出 → 下跌解除重置 → 再上升可以再进入。
    closes = [100.0] * 3 + [100.0 + 10.0 * i for i in range(1, 12)]  # bar 3..11 上升（9 bar > 5 上限）
    closes += [80.0, 70.0, 60.0]  # 下跌：解除重置
    closes += [100.0, 130.0, 160.0, 190.0, 220.0, 250.0, 280.0]  # 再上升
    result = signals_of(closes, trend_bars=3, max_hold_bars=5)
    held = [i for i, v in enumerate(result) if v == 1.0]
    # 第一段持有：bar 3..7（5 bar），bar 8 强制退出。
    assert held[:5] == [3, 4, 5, 6, 7]
    assert result[8] == 0.0
    # 下跌段（bar 12..14 附近）全部空仓。
    assert all(result[i] == 0.0 for i in range(9, 15))
    # 重置完成后（trend ≤ 0 出现过），趋势再转正 → 再进入。
    reentry = held[5]
    assert result[reentry] == 1.0 and reentry >= 15


def test_a_trend_exit_does_not_arm_the_reset_gate() -> None:
    # 上限退出需要重置；趋势退出不需要——两类退出后的再进入行为必须不同。
    closes = [100.0] * 3 + [120.0, 140.0, 160.0, 170.0, 90.0, 90.0, 150.0, 170.0, 190.0, 210.0, 230.0]
    result = signals_of(closes, trend_bars=3, max_hold_bars=100)
    assert result[3] == 1.0 and result[4] == 1.0 and result[5] == 1.0 and result[6] == 1.0  # 持有
    assert result[7] == 0.0  # close 90 vs close[4]=140 → 趋势退出
    assert result[8] == 0.0 and result[9] == 0.0  # 空仓（bar 9 的 trend 仍为负）
    assert result[10] == 1.0  # close 170 vs close[7]=90 → 趋势转正 → 直接再进入（无重置门槛）
    assert result[11] == 1.0


def test_the_hold_cap_counts_holding_bars_not_calendar_bars() -> None:
    # 进入前（空仓）的 bar 不计入持有时长——只有持有中的 bar 才计数。
    closes = [100.0] * 3 + [100.0, 100.0, 100.0, 200.0, 210.0, 220.0, 230.0, 240.0, 250.0, 260.0]
    # bar 0-5：warm-up + trend=0 空仓（6 bar 不持有）；bar 6 进入。
    result = signals_of(closes, trend_bars=3, max_hold_bars=3)
    assert result[6] == 1.0 and result[7] == 1.0 and result[8] == 1.0  # 持有 3 bar
    assert result[9] == 0.0  # held=3 达上限 → 强制退出（趋势仍正）


# ---------------------------------------------------------------------------
# SAFE-05 因果门（前缀差分）
# ---------------------------------------------------------------------------


def test_the_strategy_passes_the_causal_gate_on_a_rich_path() -> None:
    # 一段同时经历：warm-up、进入、趋势退出、再进入、上限退出、重置等待、再进入的序列。
    closes = [100.0] * 3
    closes += [100.0 + 10.0 * i for i in range(1, 8)]  # 上升
    closes += [80.0, 70.0]  # 下跌
    closes += [100.0 + 10.0 * i for i in range(1, 10)]  # 再上升
    closes += [50.0, 40.0, 50.0, 90.0, 130.0, 170.0, 210.0]  # 下跌 + 重置 + 再上升
    strategy = TrendFollowingStrategy(trend_bars=3, max_hold_bars=4)
    signal, evidence = checked_signals(strategy, make_df(closes), points=12)
    assert evidence.points_compared > 0
    assert (signal == 1.0).any() and (signal == 0.0).any()  # 状态机的两个输出都出现过


def test_an_upward_trend_only_series_also_passes() -> None:
    closes = [100.0 + i for i in range(40)]
    strategy = TrendFollowingStrategy(trend_bars=5, max_hold_bars=8)
    signal, _ = checked_signals(strategy, make_df(closes), points=10)
    # 持续上升：进入 → 上限退出 → 等待（无 trend≤0 解除）→ 停在 AWAIT_RESET。
    assert (signal == 1.0).any()
    assert (signal.tail(5) == 0.0).all()


def test_a_truncated_prefix_reproduces_the_same_signal_prefix() -> None:
    # 手动做一次前缀差分（与 _prefix_differential 同一语义），双保险。
    closes = [100.0] * 3 + [110.0, 120.0, 90.0, 95.0, 150.0, 160.0, 170.0, 180.0, 120.0, 110.0, 140.0, 160.0]
    strategy = TrendFollowingStrategy(trend_bars=3, max_hold_bars=4)
    full = strategy.generate_signals(make_df(closes))
    for cut in (6, 9, len(closes) - 1):
        truncated = strategy.generate_signals(make_df(closes[:cut]))
        for i in range(cut):
            a, b = full.iloc[i], truncated.iloc[i]
            assert (pd.isna(a) and pd.isna(b)) or a == b, f"prefix mismatch at {i} (cut={cut})"
