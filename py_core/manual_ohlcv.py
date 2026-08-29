"""Pure manual OHLCV import contracts for P2-MD-01.

This module defines the domain vocabulary for the first file-based Market Data
Layer slice. It contains no database access, network I/O, provider SDK usage,
or execution behavior.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from datetime import UTC, datetime
from decimal import Decimal
from enum import StrEnum


class ManualMarket(StrEnum):
    """Supported market values for manual OHLCV imports."""

    A_SHARE = "a_share"
    US_STOCK = "us_stock"
    HK_STOCK = "hk_stock"
    CRYPTO_SPOT = "crypto_spot"


class OhlcvInputFormat(StrEnum):
    """Input file formats supported by the manual import MVP and CCXT adapter."""

    CSV = "csv"
    JSON = "json"
    CCXT_PUBLIC = "ccxt_public"
    BINANCE_PUBLIC_REST = "binance_public_rest"


class TimestampPosture(StrEnum):
    """How event timestamps were interpreted during normalization.

    AUDIT TZ-POSTURE-DEAD-058: ``ManualOhlcvImportManifest`` is constructed at exactly one
    call site in this codebase (``py_core/market_data/cli.py::publish_fetch_output()``),
    and it always fetches from Binance's public REST API, whose payload always carries an
    explicit UTC offset -- so that producer is correctly, unconditionally
    ``OFFSET_PROVIDED_IN_PAYLOAD``. ``NAIVE_SOURCE_TIMEZONE_APPLIED`` is therefore reserved
    for a manual/naive-timestamp import path (e.g. a CSV whose timestamps carry no
    timezone and get a source timezone assumption applied during normalization) that does
    not currently exist as a manifest-producing code path anywhere in this codebase -- it
    is a legal value of this schema field, appears in ``manifest_to_dict()``/
    ``manifest_from_dict()`` round-tripping, but no code path writes it today. Do not
    assume it is exercised by any existing producer; before relying on a manifest's
    ``timestamp_posture`` to distinguish "naive assumption applied" from "explicit offset"
    in production data, confirm which (if any) producer is actually populating it.
    """

    NAIVE_SOURCE_TIMEZONE_APPLIED = "naive_source_timezone_applied"
    OFFSET_PROVIDED_IN_PAYLOAD = "offset_provided_in_payload"


class AvailableTimePosture(StrEnum):
    """Point-in-time availability posture for manual imports."""

    UNKNOWN_MANUAL_IMPORT = "unknown_manual_import"


class OhlcvValidationIssueCode(StrEnum):
    """Deterministic validation issue codes for manual OHLCV imports."""

    EMPTY_INPUT = "EMPTY_INPUT"
    UNSUPPORTED_INPUT_FORMAT = "UNSUPPORTED_INPUT_FORMAT"
    INVALID_JSON_PAYLOAD = "INVALID_JSON_PAYLOAD"
    INVALID_JSON_RECORD = "INVALID_JSON_RECORD"
    MISSING_REQUIRED_FIELD = "MISSING_REQUIRED_FIELD"
    INVALID_DECIMAL = "INVALID_DECIMAL"
    NON_POSITIVE_PRICE = "NON_POSITIVE_PRICE"
    NEGATIVE_VOLUME = "NEGATIVE_VOLUME"
    INVALID_TIMESTAMP = "INVALID_TIMESTAMP"
    # AUDIT TZ-POSTURE-DEAD-058: reserved for a batch of records within one import that
    # mixes more than one TimestampPosture (e.g. some rows naive-with-assumed-timezone,
    # others carrying an explicit offset). No current validation path tracks posture
    # per-record -- NormalizedOhlcvRecord itself carries no posture field, only
    # ManualOhlcvImportManifest does, once per whole import -- so this code is never
    # actually emitted by validate_ohlcv_record()/validate_ohlcv_batch() today. Do not
    # assume a caller will ever see this issue code without first checking whether a
    # per-record posture-tracking path has been added.
    MIXED_TIMESTAMP_POSTURE = "MIXED_TIMESTAMP_POSTURE"
    DUPLICATE_EVENT_TIME = "DUPLICATE_EVENT_TIME"
    NON_MONOTONIC_EVENT_TIME = "NON_MONOTONIC_EVENT_TIME"
    INVALID_OHLC_BOUNDS = "INVALID_OHLC_BOUNDS"
    NON_FINITE_DECIMAL = "NON_FINITE_DECIMAL"


_TIMEFRAME_PATTERN = re.compile(r"^[1-9]\d*[mhdw]$", re.IGNORECASE)


def _ensure_utc(value: datetime) -> None:
    if value.tzinfo is None or value.utcoffset() != UTC.utcoffset(value):
        raise ValueError("Datetime must be timezone-aware UTC.")


@dataclass(frozen=True, slots=True)
class CanonicalMarketSymbol:
    """Canonical symbol text preserved separately from market type."""

    value: str

    def __post_init__(self) -> None:
        normalized = self.value.strip().upper()
        if not normalized:
            raise ValueError("Symbol must be non-empty.")
        if any(char.isspace() for char in normalized):
            raise ValueError("Symbol must not contain whitespace.")
        object.__setattr__(self, "value", normalized)

    def __str__(self) -> str:
        return self.value


@dataclass(frozen=True, slots=True)
class OhlcvTimeframe:
    """Canonical timeframe text for manual OHLCV imports."""

    value: str

    def __post_init__(self) -> None:
        normalized = self.value.strip().lower()
        if not _TIMEFRAME_PATTERN.fullmatch(normalized):
            raise ValueError("Timeframe must match <positive integer><m|h|d|w>.")
        object.__setattr__(self, "value", normalized)

    def __str__(self) -> str:
        return self.value


@dataclass(frozen=True, slots=True)
class OhlcvValidationIssue:
    """One validation issue detected during import or normalization."""

    code: OhlcvValidationIssueCode
    message: str
    row_number: int | None = None
    field_name: str | None = None


class ManualOhlcvValidationError(ValueError):
    """Raised when manual OHLCV input fails validation."""

    def __init__(self, issues: tuple[OhlcvValidationIssue, ...]) -> None:
        if not issues:
            raise ValueError("Validation error requires at least one issue.")
        self.issues = issues
        super().__init__("Manual OHLCV validation failed.")


@dataclass(frozen=True, slots=True)
class NormalizedOhlcvRecord:
    """Canonical normalized OHLCV record for file-based storage."""

    market: ManualMarket
    symbol: CanonicalMarketSymbol
    timeframe: OhlcvTimeframe
    event_time_utc: datetime
    open_price: Decimal
    high_price: Decimal
    low_price: Decimal
    close_price: Decimal
    volume: Decimal

    def __post_init__(self) -> None:
        _ensure_utc(self.event_time_utc)


@dataclass(frozen=True, slots=True)
class ManualOhlcvImportManifest:
    """Metadata describing one stored manual OHLCV import."""

    import_id: str
    market: ManualMarket
    symbol: CanonicalMarketSymbol
    timeframe: OhlcvTimeframe
    source_name: str
    input_path: str
    input_format: OhlcvInputFormat
    source_timezone: str
    timestamp_posture: TimestampPosture
    available_time_posture: AvailableTimePosture
    point_in_time_safe: bool
    imported_at_utc: datetime
    file_sha256: str
    record_count: int
    first_event_time_utc: datetime | None
    last_event_time_utc: datetime | None

    def __post_init__(self) -> None:
        if not self.import_id or self.import_id.strip() != self.import_id:
            raise ValueError("Import ID must be non-empty and trimmed.")
        if not self.source_name or self.source_name.strip() != self.source_name:
            raise ValueError("Source name must be non-empty and trimmed.")
        if not self.input_path or self.input_path.strip() != self.input_path:
            raise ValueError("Input path must be non-empty and trimmed.")
        if not self.file_sha256:
            raise ValueError("File SHA-256 must be non-empty.")
        if self.record_count < 0:
            raise ValueError("Record count must be non-negative.")
        _ensure_utc(self.imported_at_utc)
        if self.first_event_time_utc is not None:
            _ensure_utc(self.first_event_time_utc)
        if self.last_event_time_utc is not None:
            _ensure_utc(self.last_event_time_utc)


def validate_ohlcv_record(record: NormalizedOhlcvRecord) -> tuple[OhlcvValidationIssue, ...]:
    """Validate one normalized OHLCV record."""

    issues: list[OhlcvValidationIssue] = []
    prices = {
        "open": record.open_price,
        "high": record.high_price,
        "low": record.low_price,
        "close": record.close_price,
    }

    # NaN/Infinity must be caught BEFORE any ordering comparison below: Python's decimal
    # module traps InvalidOperation on ordering comparisons involving NaN under the default
    # context (directly verified: `Decimal('NaN') <= Decimal('0')` raises
    # decimal.InvalidOperation), so a malformed/corrupted price field would crash this
    # function instead of producing a clean validation issue. Infinity does not raise on
    # comparison but is just as meaningless as a price/volume value.
    non_finite_fields: set[str] = set()
    for field_name, value in {**prices, "volume": record.volume}.items():
        if not value.is_finite():
            non_finite_fields.add(field_name)
            issues.append(
                OhlcvValidationIssue(
                    code=OhlcvValidationIssueCode.NON_FINITE_DECIMAL,
                    field_name=field_name,
                    message=f"Field '{field_name}' must be a finite decimal (not NaN/Infinity).",
                )
            )

    for field_name, value in prices.items():
        if field_name in non_finite_fields:
            continue  # already reported; comparing a non-finite value further is unsafe
        if value <= Decimal(0):
            issues.append(
                OhlcvValidationIssue(
                    code=OhlcvValidationIssueCode.NON_POSITIVE_PRICE,
                    field_name=field_name,
                    message=f"Field '{field_name}' must be positive.",
                )
            )

    if "volume" not in non_finite_fields and record.volume < Decimal(0):
        issues.append(
            OhlcvValidationIssue(
                code=OhlcvValidationIssueCode.NEGATIVE_VOLUME,
                field_name="volume",
                message="Volume must be non-negative.",
            )
        )

    if not non_finite_fields.intersection({"open", "high", "low", "close"}):
        max_price = max(record.open_price, record.close_price, record.low_price)
        min_price = min(record.open_price, record.close_price, record.high_price)
        if record.high_price < max_price or record.low_price > min_price:
            issues.append(
                OhlcvValidationIssue(
                    code=OhlcvValidationIssueCode.INVALID_OHLC_BOUNDS,
                    message=(
                        "OHLC values are inconsistent: high must be >= open/close/low and "
                        "low must be <= open/close/high."
                    ),
                )
            )

    return tuple(issues)


def validate_ohlcv_batch(
    records: tuple[NormalizedOhlcvRecord, ...],
) -> tuple[OhlcvValidationIssue, ...]:
    """Validate duplicate and monotonic timestamp constraints for a batch."""

    issues: list[OhlcvValidationIssue] = []
    seen_event_times: dict[datetime, int] = {}
    previous_event_time: datetime | None = None

    for index, record in enumerate(records, start=1):
        if record.event_time_utc in seen_event_times:
            issues.append(
                OhlcvValidationIssue(
                    code=OhlcvValidationIssueCode.DUPLICATE_EVENT_TIME,
                    row_number=index,
                    field_name="timestamp",
                    message=(
                        "Duplicate normalized event_time_utc detected within the imported batch."
                    ),
                )
            )
        else:
            seen_event_times[record.event_time_utc] = index

        if previous_event_time is not None and record.event_time_utc <= previous_event_time:
            issues.append(
                OhlcvValidationIssue(
                    code=OhlcvValidationIssueCode.NON_MONOTONIC_EVENT_TIME,
                    row_number=index,
                    field_name="timestamp",
                    message=(
                        "Normalized event_time_utc values must be strictly increasing in input order."
                    ),
                )
            )
        previous_event_time = record.event_time_utc

    return tuple(issues)


def format_utc_timestamp(value: datetime) -> str:
    """Return an RFC3339-like UTC timestamp using a trailing Z."""

    _ensure_utc(value)
    return value.isoformat().replace("+00:00", "Z")


def normalized_record_to_dict(record: NormalizedOhlcvRecord) -> dict[str, str]:
    """Serialize one normalized record for file-based storage."""

    return {
        "market": record.market.value,
        "symbol": record.symbol.value,
        "timeframe": record.timeframe.value,
        "event_time_utc": format_utc_timestamp(record.event_time_utc),
        "open": str(record.open_price),
        "high": str(record.high_price),
        "low": str(record.low_price),
        "close": str(record.close_price),
        "volume": str(record.volume),
    }


def normalized_record_from_dict(payload: dict[str, object]) -> NormalizedOhlcvRecord:
    """Deserialize one normalized record from file-based storage."""

    return NormalizedOhlcvRecord(
        market=ManualMarket(str(payload["market"])),
        symbol=CanonicalMarketSymbol(str(payload["symbol"])),
        timeframe=OhlcvTimeframe(str(payload["timeframe"])),
        event_time_utc=datetime.fromisoformat(str(payload["event_time_utc"])),
        open_price=Decimal(str(payload["open"])),
        high_price=Decimal(str(payload["high"])),
        low_price=Decimal(str(payload["low"])),
        close_price=Decimal(str(payload["close"])),
        volume=Decimal(str(payload["volume"])),
    )


def manifest_to_dict(manifest: ManualOhlcvImportManifest) -> dict[str, object]:
    """Serialize one manual import manifest for file-based storage."""

    return {
        "import_id": manifest.import_id,
        "market": manifest.market.value,
        "symbol": manifest.symbol.value,
        "timeframe": manifest.timeframe.value,
        "source_name": manifest.source_name,
        "input_path": manifest.input_path,
        "input_format": manifest.input_format.value,
        "source_timezone": manifest.source_timezone,
        "timestamp_posture": manifest.timestamp_posture.value,
        "available_time_posture": manifest.available_time_posture.value,
        "point_in_time_safe": manifest.point_in_time_safe,
        "imported_at_utc": format_utc_timestamp(manifest.imported_at_utc),
        "file_sha256": manifest.file_sha256,
        "record_count": manifest.record_count,
        "first_event_time_utc": (
            format_utc_timestamp(manifest.first_event_time_utc)
            if manifest.first_event_time_utc is not None
            else None
        ),
        "last_event_time_utc": (
            format_utc_timestamp(manifest.last_event_time_utc)
            if manifest.last_event_time_utc is not None
            else None
        ),
    }


def manifest_from_dict(payload: dict[str, object]) -> ManualOhlcvImportManifest:
    """Deserialize one manual import manifest from file-based storage."""

    first_event_time = payload.get("first_event_time_utc")
    last_event_time = payload.get("last_event_time_utc")
    return ManualOhlcvImportManifest(
        import_id=str(payload["import_id"]),
        market=ManualMarket(str(payload["market"])),
        symbol=CanonicalMarketSymbol(str(payload["symbol"])),
        timeframe=OhlcvTimeframe(str(payload["timeframe"])),
        source_name=str(payload["source_name"]),
        input_path=str(payload["input_path"]),
        input_format=OhlcvInputFormat(str(payload["input_format"])),
        source_timezone=str(payload["source_timezone"]),
        timestamp_posture=TimestampPosture(str(payload["timestamp_posture"])),
        available_time_posture=AvailableTimePosture(str(payload["available_time_posture"])),
        point_in_time_safe=bool(payload["point_in_time_safe"]),
        imported_at_utc=datetime.fromisoformat(str(payload["imported_at_utc"])),
        file_sha256=str(payload["file_sha256"]),
        record_count=int(str(payload["record_count"])),
        first_event_time_utc=(
            datetime.fromisoformat(str(first_event_time)) if first_event_time is not None else None
        ),
        last_event_time_utc=(
            datetime.fromisoformat(str(last_event_time)) if last_event_time is not None else None
        ),
    )
