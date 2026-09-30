"""Offline verification: re-derive continuity from the raw data; never trust the online ledger."""

from .report import Report, verify_lake

__all__ = ["Report", "verify_lake"]
