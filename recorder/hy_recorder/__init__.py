"""HengYuan v2 D0 read-only market-data recorder.

Public market data only: no credentials, no signed requests, no order paths.
The recorder writes raw WebSocket message payloads to ``.jsonl.zst`` segments and an
append-only ledger; ``hy_recorder verify`` re-derives continuity from the raw data.
"""

__version__ = "0.1.0"
