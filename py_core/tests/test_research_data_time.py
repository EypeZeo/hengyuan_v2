"""PLAN-02 数据时间信息与决策读数门禁的测试（py_core/research/data_time.py）。

覆盖：逐条时间信息校验的全部问题码、决策门禁的边界与拒绝原因、批量划分、
来源合同违约、以及固定秒数网格上的缺口/重复/乱序/非 UTC 识别。
"""

from __future__ import annotations

from dataclasses import replace
from datetime import UTC, datetime, timedelta, timezone

import pytest

from py_core.manual_ohlcv import OhlcvTimeframe
from py_core.research.data_time import (
    DataSourceContractIssueCode,
    DataSourceDescriptor,
    DataTimeInfo,
    DataTimeIssueCode,
    DataTimeValidationError,
    DecisionReadVerdict,
    GridIntegrityReport,
    TimeInfoQuality,
    decision_read_verdict,
    find_grid_gaps,
    partition_by_decision_readability,
    require_valid_data_time_info,
    validate_data_time_info,
    validate_source_contract,
)

T0 = datetime(2026, 1, 1, tzinfo=UTC)
ONE_HOUR = timedelta(hours=1)


def observed(event_time: datetime = T0, *, available_at: datetime | None = None, received_at: datetime | None = None,
             source_id: str = "binance_public_rest", revision_id: int = 0) -> DataTimeInfo:
    return DataTimeInfo(
        event_time_utc=event_time,
        available_at_utc=event_time + ONE_HOUR if available_at is None else available_at,
        received_at_utc=event_time + ONE_HOUR if received_at is None else received_at,
        revision_id=revision_id,
        source_id=source_id,
        quality=TimeInfoQuality.OBSERVED,
    )


def modeled(event_time: datetime = T0, *, available_at: datetime | None = None, basis: str = "close_plus_1s") -> DataTimeInfo:
    return DataTimeInfo(
        event_time_utc=event_time,
        available_at_utc=event_time + ONE_HOUR if available_at is None else available_at,
        received_at_utc=None,
        revision_id=0,
        source_id="binance_public_rest",
        quality=TimeInfoQuality.MODELED,
        availability_basis=basis,
    )


def unknown(event_time: datetime = T0) -> DataTimeInfo:
    return DataTimeInfo(
        event_time_utc=event_time,
        available_at_utc=None,
        received_at_utc=None,
        revision_id=0,
        source_id="binance_public_rest",
        quality=TimeInfoQuality.UNKNOWN,
    )


# ---------------------------------------------------------------------------
# 逐条校验
# ---------------------------------------------------------------------------


def test_valid_records_of_every_quality_pass() -> None:
    assert validate_data_time_info(observed()) == ()
    assert validate_data_time_info(modeled()) == ()
    assert validate_data_time_info(unknown()) == ()


def test_revision_id_may_be_zero_or_positive_but_not_negative() -> None:
    assert validate_data_time_info(observed(revision_id=0)) == ()
    assert validate_data_time_info(observed(revision_id=3)) == ()
    issues = validate_data_time_info(observed(revision_id=-1))
    assert [issue.code for issue in issues] == [DataTimeIssueCode.NEGATIVE_REVISION_ID]


@pytest.mark.parametrize(
    ("field", "expected"),
    [
        ("event_time_utc", DataTimeIssueCode.NOT_TIMEZONE_AWARE_UTC),
        ("available_at_utc", DataTimeIssueCode.NOT_TIMEZONE_AWARE_UTC),
        ("received_at_utc", DataTimeIssueCode.NOT_TIMEZONE_AWARE_UTC),
    ],
)
def test_naive_datetimes_are_reported_not_guessed(field: str, expected: DataTimeIssueCode) -> None:
    info = observed()
    mutated = replace(info, **{field: getattr(info, field).replace(tzinfo=None)})
    assert [issue.code for issue in validate_data_time_info(mutated)] == [expected]


def test_a_non_utc_offset_is_not_accepted_as_utc() -> None:
    offset_time = datetime(2026, 1, 1, 8, tzinfo=timezone(timedelta(hours=8)))
    info = DataTimeInfo(
        event_time_utc=offset_time,
        available_at_utc=offset_time + ONE_HOUR,
        received_at_utc=offset_time + ONE_HOUR,
        revision_id=0,
        source_id="s",
        quality=TimeInfoQuality.OBSERVED,
    )
    codes = {issue.code for issue in validate_data_time_info(info)}
    assert codes == {DataTimeIssueCode.NOT_TIMEZONE_AWARE_UTC}


def test_source_id_rules() -> None:
    assert validate_data_time_info(observed(source_id="binance_public_rest")) == ()
    issues = validate_data_time_info(observed(source_id=""))
    assert [issue.code for issue in issues] == [DataTimeIssueCode.EMPTY_SOURCE_ID]
    issues = validate_data_time_info(observed(source_id="binance rest"))
    assert [issue.code for issue in issues] == [DataTimeIssueCode.SOURCE_ID_WITH_WHITESPACE]
    issues = validate_data_time_info(observed(source_id="s" * 65))
    assert [issue.code for issue in issues] == [DataTimeIssueCode.SOURCE_ID_TOO_LONG]


def test_available_or_received_before_event_is_rejected() -> None:
    early = T0 - timedelta(seconds=1)
    issues = validate_data_time_info(observed(available_at=early))
    assert [issue.code for issue in issues] == [DataTimeIssueCode.AVAILABLE_BEFORE_EVENT]
    issues = validate_data_time_info(observed(received_at=early))
    assert [issue.code for issue in issues] == [DataTimeIssueCode.RECEIPT_BEFORE_EVENT]


def test_observed_quality_requires_receipt_and_available() -> None:
    without_receipt = DataTimeInfo(
        event_time_utc=T0, available_at_utc=T0 + ONE_HOUR, received_at_utc=None,
        revision_id=0, source_id="s", quality=TimeInfoQuality.OBSERVED,
    )
    assert [issue.code for issue in validate_data_time_info(without_receipt)] == [DataTimeIssueCode.OBSERVED_WITHOUT_RECEIPT]
    without_available = DataTimeInfo(
        event_time_utc=T0, available_at_utc=None, received_at_utc=T0 + ONE_HOUR,
        revision_id=0, source_id="s", quality=TimeInfoQuality.OBSERVED,
    )
    assert [issue.code for issue in validate_data_time_info(without_available)] == [DataTimeIssueCode.OBSERVED_WITHOUT_AVAILABLE]


def test_modeled_quality_requires_available_and_basis() -> None:
    without_available = DataTimeInfo(
        event_time_utc=T0, available_at_utc=None, received_at_utc=None,
        revision_id=0, source_id="s", quality=TimeInfoQuality.MODELED, availability_basis="close_plus_1s",
    )
    assert [issue.code for issue in validate_data_time_info(without_available)] == [DataTimeIssueCode.MODELED_WITHOUT_AVAILABLE]
    without_basis = DataTimeInfo(
        event_time_utc=T0, available_at_utc=T0 + ONE_HOUR, received_at_utc=None,
        revision_id=0, source_id="s", quality=TimeInfoQuality.MODELED,
    )
    assert [issue.code for issue in validate_data_time_info(without_basis)] == [DataTimeIssueCode.MODELED_WITHOUT_BASIS]
    empty_basis = modeled(basis="  ")
    assert [issue.code for issue in validate_data_time_info(empty_basis)] == [DataTimeIssueCode.MODELED_WITHOUT_BASIS]


def test_unknown_quality_must_not_claim_a_basis() -> None:
    claiming = DataTimeInfo(
        event_time_utc=T0, available_at_utc=None, received_at_utc=None,
        revision_id=0, source_id="s", quality=TimeInfoQuality.UNKNOWN, availability_basis="close_plus_1s",
    )
    assert [issue.code for issue in validate_data_time_info(claiming)] == [DataTimeIssueCode.UNKNOWN_WITH_BASIS]


def test_all_issues_are_reported_together_and_the_error_carries_them() -> None:
    early = T0 - timedelta(seconds=1)
    broken = DataTimeInfo(
        event_time_utc=T0, available_at_utc=early, received_at_utc=early,
        revision_id=-1, source_id="", quality=TimeInfoQuality.OBSERVED,
    )
    codes = [issue.code for issue in validate_data_time_info(broken)]
    assert codes == [
        DataTimeIssueCode.EMPTY_SOURCE_ID,
        DataTimeIssueCode.NEGATIVE_REVISION_ID,
        DataTimeIssueCode.AVAILABLE_BEFORE_EVENT,
        DataTimeIssueCode.RECEIPT_BEFORE_EVENT,
    ]
    with pytest.raises(DataTimeValidationError) as excinfo:
        require_valid_data_time_info(broken)
    assert tuple(issue.code for issue in excinfo.value.issues) == tuple(codes)


# ---------------------------------------------------------------------------
# 决策读数门禁
# ---------------------------------------------------------------------------


def test_readable_when_available_exactly_at_decision_time() -> None:
    decision_time = T0 + ONE_HOUR
    assert decision_read_verdict(observed(available_at=decision_time), decision_time) is DecisionReadVerdict.READABLE
    assert decision_read_verdict(
        observed(available_at=decision_time + timedelta(microseconds=1)), decision_time
    ) is DecisionReadVerdict.AVAILABLE_AFTER_DECISION


def test_future_available_time_is_unreadable_until_the_moment_arrives() -> None:
    embargoed = observed(available_at=T0 + timedelta(days=7))
    assert decision_read_verdict(embargoed, T0) is DecisionReadVerdict.AVAILABLE_AFTER_DECISION
    assert decision_read_verdict(embargoed, T0 + timedelta(days=7)) is DecisionReadVerdict.READABLE


def test_unknown_available_time_is_never_readable() -> None:
    assert decision_read_verdict(unknown(), T0 + timedelta(days=1)) is DecisionReadVerdict.UNKNOWN_AVAILABLE_TIME


def test_require_receipt_refuses_modeled_and_unknown_records() -> None:
    decision_time = T0 + ONE_HOUR
    assert decision_read_verdict(modeled(), decision_time, require_receipt=True) is DecisionReadVerdict.RECEIPT_UNRECORDED
    assert decision_read_verdict(unknown(), decision_time, require_receipt=True) is DecisionReadVerdict.UNKNOWN_AVAILABLE_TIME
    assert decision_read_verdict(modeled(), decision_time, require_receipt=False) is DecisionReadVerdict.READABLE


def test_receipt_after_decision_is_rejected_only_when_required() -> None:
    late_receipt = observed(received_at=T0 + timedelta(hours=2))
    decision_time = T0 + ONE_HOUR
    assert decision_read_verdict(late_receipt, decision_time) is DecisionReadVerdict.READABLE
    assert decision_read_verdict(late_receipt, decision_time, require_receipt=True) is DecisionReadVerdict.RECEIPT_AFTER_DECISION
    assert decision_read_verdict(late_receipt, T0 + timedelta(hours=2), require_receipt=True) is DecisionReadVerdict.READABLE


def test_period_not_closed_refuses_even_an_early_available_record() -> None:
    period_end = T0 + ONE_HOUR
    early_available = observed(available_at=T0 + timedelta(seconds=1))
    assert decision_read_verdict(early_available, T0, period_end_utc=period_end) is DecisionReadVerdict.PERIOD_NOT_CLOSED
    assert decision_read_verdict(early_available, period_end, period_end_utc=period_end) is DecisionReadVerdict.READABLE


def test_naive_decision_or_period_end_is_a_caller_error() -> None:
    with pytest.raises(ValueError, match="decision_time_utc"):
        decision_read_verdict(observed(), T0.replace(tzinfo=None))
    with pytest.raises(ValueError, match="period_end_utc"):
        decision_read_verdict(observed(), T0, period_end_utc=(T0 + ONE_HOUR).replace(tzinfo=None))


def test_partition_keeps_order_and_reports_rejection_reasons() -> None:
    decision_time = T0 + ONE_HOUR
    infos = [
        observed(T0, available_at=T0 + ONE_HOUR),            # readable
        observed(T0 + ONE_HOUR, available_at=T0 + 2 * ONE_HOUR),  # future
        unknown(T0 + 2 * ONE_HOUR),                          # unknown
        modeled(T0, available_at=T0 + ONE_HOUR),             # readable (no receipt requirement)
    ]
    partition = partition_by_decision_readability(infos, decision_time)
    assert partition.readable_indices == (0, 3)
    assert [(r.index, r.verdict) for r in partition.rejected] == [
        (1, DecisionReadVerdict.AVAILABLE_AFTER_DECISION),
        (2, DecisionReadVerdict.UNKNOWN_AVAILABLE_TIME),
    ]


# ---------------------------------------------------------------------------
# 来源合同
# ---------------------------------------------------------------------------


def test_source_descriptor_rejects_a_bad_id_at_construction() -> None:
    with pytest.raises(ValueError, match="source_id"):
        DataSourceDescriptor(source_id="", preserves_receipt_time=False)
    with pytest.raises(ValueError, match="source_id"):
        DataSourceDescriptor(source_id="a b", preserves_receipt_time=False)
    with pytest.raises(ValueError, match="source_id"):
        DataSourceDescriptor(source_id="s" * 65, preserves_receipt_time=False)


def test_source_id_mismatch_is_a_contract_violation() -> None:
    descriptor = DataSourceDescriptor(source_id="binance_public_rest", preserves_receipt_time=False)
    infos = [observed(source_id="binance_public_rest"), observed(source_id="other_source")]
    issues = validate_source_contract(descriptor, infos)
    assert [(issue.index, issue.code) for issue in issues] == [(1, DataSourceContractIssueCode.SOURCE_ID_MISMATCH)]


def test_a_receipt_preserving_source_refuses_modeled_and_unknown_records() -> None:
    descriptor = DataSourceDescriptor(source_id="binance_public_rest", preserves_receipt_time=True)
    infos = [observed(), modeled(), unknown(), observed(source_id="binance_public_rest")]
    assert [issue.index for issue in validate_source_contract(descriptor, infos)] == [1, 2]


def test_a_receipt_preserving_source_accepts_consistent_observed_records() -> None:
    descriptor = DataSourceDescriptor(source_id="binance_public_rest", preserves_receipt_time=True)
    assert validate_source_contract(descriptor, [observed(), observed(T0 + ONE_HOUR)]) == ()


def test_a_source_that_does_not_preserve_receipts_accepts_every_quality() -> None:
    descriptor = DataSourceDescriptor(source_id="binance_public_rest", preserves_receipt_time=False)
    assert validate_source_contract(descriptor, [observed(), modeled(), unknown()]) == ()


# ---------------------------------------------------------------------------
# 网格完整性
# ---------------------------------------------------------------------------


def hourly(times: list[datetime], *, source_id: str = "binance_public_rest") -> list[DataTimeInfo]:
    return [
        observed(time, available_at=time + timedelta(seconds=1), received_at=time + timedelta(seconds=1), source_id=source_id)
        for time in times
    ]


def test_a_perfect_grid_reports_no_problems() -> None:
    times = [T0 + i * ONE_HOUR for i in range(5)]
    report = find_grid_gaps(hourly(times), OhlcvTimeframe("1h"))
    assert report == GridIntegrityReport(
        gaps=(), duplicate_event_times=(), non_monotonic_indices=(), non_utc_indices=()
    )


def test_a_missing_run_is_reported_with_its_expected_start_and_count() -> None:
    times = [T0, T0 + ONE_HOUR, T0 + 4 * ONE_HOUR, T0 + 5 * ONE_HOUR]  # 缺 T0+2h、T0+3h
    report = find_grid_gaps(hourly(times), OhlcvTimeframe("1h"))
    assert len(report.gaps) == 1
    assert report.gaps[0].expected_start_utc == T0 + 2 * ONE_HOUR
    assert report.gaps[0].missing_count == 2


def test_a_non_multiple_delta_is_not_called_a_gap() -> None:
    # 90 分钟的差既不是 1h 的整数倍也不构成可数的缺失区段：如实不报 gap（乱序/错位由上层合同处理）。
    times = [T0, T0 + timedelta(minutes=90)]
    report = find_grid_gaps(hourly(times), OhlcvTimeframe("1h"))
    assert report.gaps == ()


def test_duplicates_and_non_monotonic_order_are_reported() -> None:
    times = [T0, T0 + ONE_HOUR, T0 + ONE_HOUR, T0]  # 重复 + 乱序
    report = find_grid_gaps(hourly(times), OhlcvTimeframe("1h"))
    assert report.duplicate_event_times == (T0, T0 + ONE_HOUR)
    assert report.non_monotonic_indices == (3,)


def test_non_utc_event_times_are_reported_by_index() -> None:
    naive = DataTimeInfo(
        event_time_utc=T0.replace(tzinfo=None), available_at_utc=None, received_at_utc=None,
        revision_id=0, source_id="s", quality=TimeInfoQuality.UNKNOWN,
    )
    report = find_grid_gaps([observed(), naive], OhlcvTimeframe("1h"))
    assert report.non_utc_indices == (1,)


def test_weekly_grid_uses_the_fixed_week_length() -> None:
    week = timedelta(weeks=1)
    times = [T0, T0 + week, T0 + 3 * week]  # 缺 T0+2w
    report = find_grid_gaps(hourly(times), OhlcvTimeframe("1w"))
    assert len(report.gaps) == 1
    assert report.gaps[0].expected_start_utc == T0 + 2 * week
    assert report.gaps[0].missing_count == 1


def test_empty_input_reports_an_empty_grid() -> None:
    report = find_grid_gaps([], OhlcvTimeframe("1h"))
    assert report.gaps == ()
    assert report.duplicate_event_times == ()
    assert report.non_monotonic_indices == ()
    assert report.non_utc_indices == ()
