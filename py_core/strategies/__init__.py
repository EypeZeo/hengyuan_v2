"""py_core.strategies — 可插拔策略框架 — P2-STRAT-01.

Non-authorizing. 策略只产出研究/回测用途的 signal/目标仓位序列，不做仓位管理、风险控制
或下单——这个仓库里目前没有任何下单能力。不代表任何交易建议或交易授权。
"""

from py_core.strategies.base import (
    Strategy,
    assert_no_lookahead,
    load_strategy,
    validate_signal_output,
)
from py_core.strategies.momentum import MomentumStrategy
from py_core.strategies.sma_crossover import SmaCrossoverStrategy

__all__ = [
    "MomentumStrategy",
    "SmaCrossoverStrategy",
    "Strategy",
    "assert_no_lookahead",
    "load_strategy",
    "validate_signal_output",
]
