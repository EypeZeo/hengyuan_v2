"""Application settings loaded from environment variables and .env file.

This module is the single configuration entry-point for the entire
application.  All other modules must read configuration through
``get_settings``; they must NOT call ``os.getenv`` or ``python-dotenv``
directly.

Extension points (do NOT add before the matching task lands):
  - P1-05: pass ``settings.log_level`` to the structured logging initialiser.
  - P1-06: pass ``settings.database_url`` to the SQLAlchemy engine factory.
  - Future: exchange API key groups, scheduler config, notification config.
"""

from __future__ import annotations

from functools import lru_cache

from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """Centralised application settings.

    Values are loaded (in priority order) from:
      1. Real environment variables
      2. A ``.env`` file in the working directory (when present)
      3. Defaults defined below

    All defaults are safe and conservative — in particular, live trading is
    disabled by default and must be explicitly enabled via environment variable.
    """

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        case_sensitive=False,
        extra="ignore",
    )

    # ── App identity ──────────────────────────────────────────────────────────
    app_name: str = Field(default="hengyuan")
    app_env: str = Field(default="local")
    app_debug: bool = Field(default=False)
    app_version: str = Field(default="0.1.0")

    # ── API ───────────────────────────────────────────────────────────────────
    api_host: str = Field(default="127.0.0.1")
    api_port: int = Field(default=8000)

    # ── Database (placeholder — wired in P1-06) ───────────────────────────────
    database_url: str = Field(default="")

    # ── Logging (placeholder — wired in P1-05) ────────────────────────────────
    log_level: str = Field(default="INFO")

    # ── Safety / feature flags ────────────────────────────────────────────────
    enable_live_trading: bool = Field(default=False)
    enable_dry_run: bool = Field(default=True)
    enable_research_features: bool = Field(default=True)

    # ── Dry-run policy (P2-32 / P2-38) ───────────────────────────────────────
    dry_run_allowed_symbols: str = Field(
        default="",
        description=(
            "Comma-separated list of canonical symbols (BASE/QUOTE) allowed in dry-run, "
            "e.g. BTC/USDT,ETH/USDT. "
            "When blank or unset, defaults to the caller-supplied symbol (MVP behaviour). "
            "Malformed values cause the dry-run command to exit with code 2."
        ),
    )
    dry_run_simulated_quote_balance: str = Field(
        default="",
        description=(
            "Simulated available quote-currency balance for dry-run capital guard (P2-38). "
            "THIS IS NOT A REAL EXCHANGE BALANCE. "
            "When blank or unset, the simulated-capital guard is disabled. "
            "When set, must be a positive decimal (e.g. '1000'). "
            "Malformed, zero, or negative values cause the dry-run command to exit with code 2."
        ),
    )

    # ── Dry-run CEX cost-reserve guard (P2-40) ────────────────────────────────
    dry_run_cex_taker_fee_reserve_bps: str = Field(
        default="",
        description=(
            "Simulated taker-fee reserve in basis points for dry-run capital guard (P2-40). "
            "THIS IS NOT A REAL EXCHANGE FEE SCHEDULE. Local simulation only. "
            "Requires DRY_RUN_SIMULATED_QUOTE_BALANCE to be set. "
            "When blank or unset, fee reserve is disabled. "
            "Explicit 0 is valid and means zero fee reserve. "
            "Negative or malformed values cause the dry-run command to exit with code 2."
        ),
    )
    dry_run_cex_slippage_reserve_bps: str = Field(
        default="",
        description=(
            "Simulated slippage reserve in basis points for dry-run capital guard (P2-40). "
            "THIS IS NOT A REAL SLIPPAGE MEASUREMENT. Local simulation only. "
            "Requires DRY_RUN_SIMULATED_QUOTE_BALANCE to be set. "
            "When blank or unset, slippage reserve is disabled. "
            "Explicit 0 is valid and means zero slippage reserve. "
            "Negative or malformed values cause the dry-run command to exit with code 2."
        ),
    )

    # ── Kraken exchange credentials (P2-117) ─────────────────────────────────
    # Both default to empty string (disabled / stub mode).
    # When both are non-empty, the live CLI path wires the real Kraken spot
    # submit adapter.  Missing or empty values fail-closed to the stub port.
    # Secrets must NEVER be logged; repr=False prevents accidental inclusion
    # in __repr__ / string formatting.
    #
    # These values are expected from environment variables or a .env file only
    # (no ad hoc file fallback, no hard-coded defaults).  See P2-46 for the
    # full secret/config boundary contract.
    kraken_api_key: str = Field(
        default="",
        repr=False,
        description=(
            "Kraken API key for authenticated private endpoints (P2-117). "
            "When empty, the live CLI path uses the AmbiguousStubSubmitPort. "
            "Must be paired with KRAKEN_API_SECRET to enable the real adapter. "
            "Never logged."
        ),
    )
    kraken_api_secret: str = Field(
        default="",
        repr=False,
        description=(
            "Kraken API secret for authenticated private endpoints (P2-117). "
            "When empty, the live CLI path uses the AmbiguousStubSubmitPort. "
            "Must be paired with KRAKEN_API_KEY to enable the real adapter. "
            "Never logged."
        ),
    )

    # ── Live reconcile poller (P2-120) ────────────────────────────────────────
    # Bounds per-cycle fan-out and candidate age for the scheduled truth-refresh.
    # These are NOT live trading authorizations.
    live_reconcile_poller_max_candidates: int = Field(
        default=5,
        description=(
            "Maximum number of unresolved candidates to query per reconcile poll cycle "
            "(P2-120). Bounds per-cycle fan-out. "
            "THIS IS NOT A LIVE TRADING AUTHORIZATION."
        ),
    )
    live_reconcile_poller_age_cutoff_hours: int = Field(
        default=72,
        description=(
            "Age cutoff in hours for unresolved candidate intents (P2-120). "
            "Intents created before this threshold are skipped with a "
            "'too_old' skip reason. 0 disables the cutoff. "
            "THIS IS NOT A LIVE TRADING AUTHORIZATION."
        ),
    )
    live_reconcile_poller_max_provider_failures: int = Field(
        default=3,
        description=(
            "Maximum consecutive provider query failures before aborting the "
            "current poll cycle (P2-120). Provides bounded backoff behaviour. "
            "THIS IS NOT A LIVE TRADING AUTHORIZATION."
        ),
    )


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    """Return the shared application settings instance (cached).

    Use ``get_settings()`` rather than instantiating ``Settings`` directly in
    production code.  Tests should call ``get_settings.cache_clear()`` between
    cases that mutate environment variables, or instantiate ``Settings``
    directly with ``_env_file=None`` to avoid .env file interference.
    """
    return Settings()
