"""py_core.market_data — 真实网络 I/O 的历史行情拉取 — P2-MD-02.

跟 py_core/README.md 里"backtests/risk 是纯模拟、不碰网络"的既有披露不一样：这个包会真的连接
Binance 公开市场数据端点（data-api.binance.vision），发出真实 HTTP 请求。只连公开、无需签名的
端点，不涉及账户、API Key 或任何私有签名 REST（那部分仍然是
docs/BINANCE_PRIVATE_REST_L4_SPEC.md 里"Draft, Not Implemented"的范围）。

所有输出均为研究/回测用途的历史行情数据，不代表实盘数据源的授权或点位精度保证。
"""

from py_core.market_data.binance_public_rest import (
    BinanceHttpError,
    BinanceIncompleteCoverageError,
    BinanceKlineGapError,
    BinancePublicRestError,
    BinanceRateLimitedError,
    BinanceResponseError,
    BinanceTransportError,
    FetchMeta,
    fetch_binance_ohlcv,
)

__all__ = [
    "BinanceHttpError",
    "BinanceIncompleteCoverageError",
    "BinanceKlineGapError",
    "BinancePublicRestError",
    "BinanceRateLimitedError",
    "BinanceResponseError",
    "BinanceTransportError",
    "FetchMeta",
    "fetch_binance_ohlcv",
]
