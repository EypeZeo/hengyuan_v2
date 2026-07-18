"""Unified structured logging entry-point for the HengYuan application.

All application code should obtain a logger via ``get_logger`` rather than
calling ``logging.getLogger`` or ``structlog.get_logger`` directly.

Setup::

    # Called once at application startup (app_factory.py).
    from app.core.logging import setup_logging
    setup_logging(log_level="INFO", dev_mode=True)

Usage::

    from app.core.logging import get_logger

    logger = get_logger(__name__)
    logger.info("order_placed", order_id=order.id, symbol=order.symbol)

Security rules:
  - Never pass secrets, tokens, API keys, or full credentials as log fields.
  - Do not log raw request bodies that may contain sensitive user data.

Extension points (do NOT implement before the matching task):
  - P1-09+: bind ``request_id`` per-request via
    ``structlog.contextvars.bind_contextvars(request_id=...)``.
    The ``merge_contextvars`` processor below will pick it up automatically.
"""

from __future__ import annotations

import logging
import sys
from typing import Any

import structlog


def setup_logging(log_level: str = "INFO", *, dev_mode: bool = False) -> None:
    """Configure structlog and the standard-library root logger.

    Must be called exactly once, before the first log statement.  Calling it
    multiple times is safe (structlog.configure is idempotent) but unnecessary.

    Args:
        log_level: One of ``DEBUG``, ``INFO``, ``WARNING``, ``ERROR``,
                   ``CRITICAL``.  Case-insensitive.  Defaults to ``"INFO"``.
        dev_mode:  When *True* use a human-friendly coloured console renderer.
                   When *False* (default / production) emit newline-delimited
                   JSON suitable for log aggregation.
    """
    numeric_level: int = getattr(logging, log_level.upper(), logging.INFO)

    # Configure the stdlib root logger so that any stdlib-based libraries
    # (e.g. SQLAlchemy, httpx) also respect the chosen level.
    logging.basicConfig(
        format="%(message)s",
        stream=sys.stdout,
        level=numeric_level,
        force=True,
    )

    # Processors shared between both renderers.
    shared_processors: list[Any] = [
        # Pull bound context variables (e.g. request_id) into each log entry.
        structlog.contextvars.merge_contextvars,
        structlog.stdlib.add_log_level,
        structlog.stdlib.add_logger_name,
        structlog.processors.TimeStamper(fmt="iso"),
        structlog.processors.StackInfoRenderer(),
    ]

    if dev_mode:
        # Human-friendly output for local development.
        renderer: Any = structlog.dev.ConsoleRenderer()
    else:
        # Newline-delimited JSON for production / CI.
        renderer = structlog.processors.JSONRenderer()

    structlog.configure(
        processors=[
            *shared_processors,
            structlog.dev.set_exc_info,
            renderer,
        ],
        wrapper_class=structlog.stdlib.BoundLogger,
        context_class=dict,
        logger_factory=structlog.stdlib.LoggerFactory(),
        cache_logger_on_first_use=True,
    )


def get_logger(name: str | None = None) -> structlog.stdlib.BoundLogger:
    """Return a structlog ``BoundLogger`` scoped to *name*.

    Args:
        name: Optional logger name (typically ``__name__``).  Falls back to
              the root logger when omitted.

    Returns:
        A ``structlog.stdlib.BoundLogger`` ready for structured key-value
        logging.
    """
    return structlog.get_logger(name)  # type: ignore[no-any-return]
