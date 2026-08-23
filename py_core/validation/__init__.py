"""Overfitting-resistance validation for py_core strategies — 批次 2.

This package exists because a backtest's headline Sharpe/return means nothing on its own --
it is one number produced by one particular walk through one particular slice of history,
and a strategy tuned to look good on that one walk is indistinguishable, from the headline
number alone, from a strategy with genuine edge. Everything here answers a narrower
question: "is this result more than what one lucky/overfit walk through history would
produce?"

All outputs are research/backtest estimates. No trading authorization, no live-readiness
claim.
"""

from __future__ import annotations
