"""逐条数据时间信息与决策读数门禁（PLAN-02，对应任务 3.1 与 3.4 的数据时间字段）。

合同（docs/RESEARCH_EXECUTION_PLAN.md §4.2，蓝图 6.6 任务 3.1/3.4）：

- 每条数据记录至少表达 ``event_time``、``available_at``、``received_at``、
  ``revision_id``、``source_id`` 与时间信息质量；``available_at`` 不晚于
  ``decision_time`` 的记录才允许参与决策。
- 历史回填不能补造真实接收时间：真实接收时刻只存在于 ``OBSERVED`` 质量；
  ``MODELED`` 必须携带估计依据（``availability_basis``），``UNKNOWN`` 两者皆不可信。
- 决策读数 fail-closed：可用时间未知、晚于决策时刻、要求真实回执却没有回执、
  或周期尚未收盘的记录一律拒绝，并给出可解释的拒绝原因。
- 缺口可识别：在固定秒数网格上找出缺失的 bar 区段、重复时刻与乱序位置。

与 :mod:`py_core.manual_ohlcv` 的分工：那边是导入时的记录形状与校验；本模块是
导入之后、进入任何回测/统计之前的点时合同。warehouse 的分区 schema 迁移
（把时间信息列写进 Parquet）是后续切片，不在本片。

本模块是纯逻辑：无文件 I/O、无网络、无 pandas/numpy 依赖。
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from enum import StrEnum
from itertools import pairwise

from py_core.backtests.annualization import timeframe_seconds
from py_core.manual_ohlcv import OhlcvTimeframe

__all__ = [
    "DataSourceContractIssue",
    "DataSourceContractIssueCode",
    "DataSourceDescriptor",
    "DataTimeInfo",
    "DataTimeIssue",
    "DataTimeIssueCode",
    "DataTimeValidationError",
    "DecisionReadVerdict",
    "GridIntegrityReport",
    "MissingGridRun",
    "PartitionedReadability",
    "RejectedRecord",
    "decision_read_verdict",
    "find_grid_gaps",
    "partition_by_decision_readability",
    "require_valid_data_time_info",
    "validate_data_time_info",
    "validate_source_contract",
]

_MAX_SOURCE_ID_LEN = 64


class TimeInfoQuality(StrEnum):
    """时间信息质量：真实回执、模型估计、还是历史回填的未知。

    历史回填不能补造真实接收时间（规划 §4.2）：只有 ``OBSERVED`` 允许声称
    ``received_at`` 是真实接收时刻；``MODELED`` 的 ``available_at`` 必须给出估计
    依据；``UNKNOWN`` 表示两者都不可信，参与决策前永远被拒。
    """

    OBSERVED = "observed"
    MODELED = "modeled"
    UNKNOWN = "unknown"


class DataTimeIssueCode(StrEnum):
    """逐条时间信息的确定性校验问题码。"""

    NOT_TIMEZONE_AWARE_UTC = "NOT_TIMEZONE_AWARE_UTC"
    EMPTY_SOURCE_ID = "EMPTY_SOURCE_ID"
    SOURCE_ID_WITH_WHITESPACE = "SOURCE_ID_WITH_WHITESPACE"
    SOURCE_ID_TOO_LONG = "SOURCE_ID_TOO_LONG"
    NEGATIVE_REVISION_ID = "NEGATIVE_REVISION_ID"
    AVAILABLE_BEFORE_EVENT = "AVAILABLE_BEFORE_EVENT"
    RECEIPT_BEFORE_EVENT = "RECEIPT_BEFORE_EVENT"
    OBSERVED_WITHOUT_RECEIPT = "OBSERVED_WITHOUT_RECEIPT"
    OBSERVED_WITHOUT_AVAILABLE = "OBSERVED_WITHOUT_AVAILABLE"
    MODELED_WITHOUT_AVAILABLE = "MODELED_WITHOUT_AVAILABLE"
    MODELED_WITHOUT_BASIS = "MODELED_WITHOUT_BASIS"
    UNKNOWN_WITH_BASIS = "UNKNOWN_WITH_BASIS"


@dataclass(frozen=True, slots=True)
class DataTimeIssue:
    """一条时间信息校验问题。"""

    code: DataTimeIssueCode
    message: str


class DataTimeValidationError(ValueError):
    """时间信息未通过校验；携带全部问题，与 manual_ohlcv 的校验错误同构。"""

    def __init__(self, issues: tuple[DataTimeIssue, ...]) -> None:
        if not issues:
            raise ValueError("DataTimeValidationError requires at least one issue.")
        self.issues = issues
        super().__init__("Data time info validation failed: " + "; ".join(i.message for i in issues))


@dataclass(frozen=True, slots=True)
class DataTimeInfo:
    """一条数据记录的点时时间信息。

    ``event_time_utc`` 是 bar 的开盘时刻；``available_at_utc`` 是该记录合法可用于
    决策的时刻（``None`` 仅允许 ``UNKNOWN`` 质量）；``received_at_utc`` 是真实接收
    时刻（仅 ``OBSERVED`` 必填，回填导入可以如实填写导入时刻但质量仍按合同标注）；
    ``revision_id`` 为 0 表示原始版本，更大的值表示后续修订。
    """

    event_time_utc: datetime
    available_at_utc: datetime | None
    received_at_utc: datetime | None
    revision_id: int
    source_id: str
    quality: TimeInfoQuality
    availability_basis: str | None = None


def _require_utc(value: datetime) -> bool:
    """datetime 必须带时区且恰好是 UTC（与仓库/导入层的 UTC 合同一致）。"""
    return value.tzinfo is not None and value.utcoffset() == UTC.utcoffset(value)


def _issues_for_source_id(source_id: str) -> list[DataTimeIssue]:
    issues: list[DataTimeIssue] = []
    if not source_id:
        issues.append(DataTimeIssue(DataTimeIssueCode.EMPTY_SOURCE_ID, "source_id must not be empty."))
        return issues
    if any(char.isspace() for char in source_id):
        issues.append(DataTimeIssue(DataTimeIssueCode.SOURCE_ID_WITH_WHITESPACE, "source_id must not contain whitespace."))
    if len(source_id) > _MAX_SOURCE_ID_LEN:
        issues.append(DataTimeIssue(DataTimeIssueCode.SOURCE_ID_TOO_LONG, f"source_id longer than {_MAX_SOURCE_ID_LEN} characters."))
    return issues


def validate_data_time_info(info: DataTimeInfo) -> tuple[DataTimeIssue, ...]:
    """校验一条时间信息；返回全部问题（空元组 = 通过）。失败关闭，不静默修正。"""
    issues: list[DataTimeIssue] = []

    aware: dict[str, bool] = {}
    for name, value in (
        ("event_time_utc", info.event_time_utc),
        ("available_at_utc", info.available_at_utc),
        ("received_at_utc", info.received_at_utc),
    ):
        is_utc = value is not None and _require_utc(value)
        aware[name] = is_utc
        if value is not None and not is_utc:
            issues.append(DataTimeIssue(DataTimeIssueCode.NOT_TIMEZONE_AWARE_UTC, f"{name} must be timezone-aware UTC."))

    issues.extend(_issues_for_source_id(info.source_id))

    if info.revision_id < 0:
        issues.append(DataTimeIssue(DataTimeIssueCode.NEGATIVE_REVISION_ID, "revision_id must not be negative."))

    # 顺序比较只在两侧都是 UTC-aware 时进行：naive 混入已经按问题码报告，不能再让它把比较炸成 TypeError。
    if aware["available_at_utc"] and aware["event_time_utc"] and info.available_at_utc < info.event_time_utc:
        issues.append(DataTimeIssue(DataTimeIssueCode.AVAILABLE_BEFORE_EVENT, "available_at must not precede event_time."))
    if aware["received_at_utc"] and aware["event_time_utc"] and info.received_at_utc < info.event_time_utc:
        issues.append(DataTimeIssue(DataTimeIssueCode.RECEIPT_BEFORE_EVENT, "received_at must not precede event_time."))

    if info.quality is TimeInfoQuality.OBSERVED:
        if info.received_at_utc is None:
            issues.append(DataTimeIssue(DataTimeIssueCode.OBSERVED_WITHOUT_RECEIPT, "OBSERVED quality requires received_at."))
        if info.available_at_utc is None:
            issues.append(DataTimeIssue(DataTimeIssueCode.OBSERVED_WITHOUT_AVAILABLE, "OBSERVED quality requires available_at."))
    elif info.quality is TimeInfoQuality.MODELED:
        if info.available_at_utc is None:
            issues.append(DataTimeIssue(DataTimeIssueCode.MODELED_WITHOUT_AVAILABLE, "MODELED quality requires available_at."))
        if info.availability_basis is None or not info.availability_basis.strip():
            issues.append(DataTimeIssue(DataTimeIssueCode.MODELED_WITHOUT_BASIS, "MODELED quality requires a non-empty availability_basis."))
    else:
        if info.availability_basis is not None:
            issues.append(DataTimeIssue(DataTimeIssueCode.UNKNOWN_WITH_BASIS, "UNKNOWN quality must not claim an availability_basis."))

    return tuple(issues)


def require_valid_data_time_info(info: DataTimeInfo) -> None:
    """校验失败即抛 :class:`DataTimeValidationError`（携带全部问题）。"""
    issues = validate_data_time_info(info)
    if issues:
        raise DataTimeValidationError(issues)


class DecisionReadVerdict(StrEnum):
    """决策读数裁决：READABLE 之外的一切都是拒绝原因（fail-closed，可解释）。"""

    READABLE = "readable"
    INVALID_TIME_INFO = "invalid_time_info"
    UNKNOWN_AVAILABLE_TIME = "unknown_available_time"
    AVAILABLE_AFTER_DECISION = "available_after_decision"
    RECEIPT_UNRECORDED = "receipt_unrecorded"
    RECEIPT_AFTER_DECISION = "receipt_after_decision"
    PERIOD_NOT_CLOSED = "period_not_closed"


def decision_read_verdict(
    info: DataTimeInfo,
    decision_time_utc: datetime,
    *,
    require_receipt: bool = False,
    period_end_utc: datetime | None = None,
) -> DecisionReadVerdict:
    """一条记录在 ``decision_time`` 时刻能否参与决策。

    - 只读 ``available_at <= decision_time`` 的记录（含恰好相等的边界）；
    - ``period_end_utc`` 是该记录覆盖周期的收盘时刻：决策时刻尚未越过收盘的
      记录不参与决策（"当前周期收盘在确实收到后才可以参与决策"）。周期未收盘
      的裁决排在可用时间之前：一条既未收盘又"未到可用时间"的记录，结构性的
      原因（等收盘）先于元数据原因报告；
    - ``require_receipt=True`` 时还要求真实接收时刻已被记录且不晚于决策时刻
      （``MODELED``/``UNKNOWN`` 记录因此被拒，规划 §4.2 的"依赖实际接收路径"）；
    - 未来可用时间在这里表现为 ``AVAILABLE_AFTER_DECISION``：导入时合法的
      禁运数据，决策时刻未到之前不可读，不需要单独的"未来数据"问题码。

    ``decision_time_utc``/``period_end_utc`` 不是数据：不带时区或非 UTC 属于调用方
    缺陷，直接抛 ``ValueError``，不折进数据问题码。记录里的 naive/非 UTC 时间戳
    则相反是数据缺陷：整条记录以 ``INVALID_TIME_INFO`` 拒绝（批处理里一条坏
    记录不能把整批划分炸成 ``TypeError``）。
    """
    if not _require_utc(decision_time_utc):
        raise ValueError("decision_time_utc must be timezone-aware UTC.")
    if period_end_utc is not None and not _require_utc(period_end_utc):
        raise ValueError("period_end_utc must be timezone-aware UTC.")
    if any(
        value is not None and not _require_utc(value)
        for value in (info.event_time_utc, info.available_at_utc, info.received_at_utc)
    ):
        return DecisionReadVerdict.INVALID_TIME_INFO

    if info.available_at_utc is None:
        return DecisionReadVerdict.UNKNOWN_AVAILABLE_TIME
    if period_end_utc is not None and period_end_utc > decision_time_utc:
        return DecisionReadVerdict.PERIOD_NOT_CLOSED
    if info.available_at_utc > decision_time_utc:
        return DecisionReadVerdict.AVAILABLE_AFTER_DECISION
    if require_receipt:
        if info.received_at_utc is None:
            return DecisionReadVerdict.RECEIPT_UNRECORDED
        if info.received_at_utc > decision_time_utc:
            return DecisionReadVerdict.RECEIPT_AFTER_DECISION
    return DecisionReadVerdict.READABLE


@dataclass(frozen=True, slots=True)
class RejectedRecord:
    """一条被决策门禁拒绝的记录：位置、记录本体与拒绝原因。"""

    index: int
    info: DataTimeInfo
    verdict: DecisionReadVerdict


@dataclass(frozen=True, slots=True)
class PartitionedReadability:
    """一批记录的决策读数划分：可读的下标与被拒的记录（原因内聚）。"""

    readable_indices: tuple[int, ...]
    rejected: tuple[RejectedRecord, ...]


def partition_by_decision_readability(
    infos: Sequence[DataTimeInfo],
    decision_time_utc: datetime,
    *,
    require_receipt: bool = False,
    period_end_utc: datetime | None = None,
) -> PartitionedReadability:
    """按决策门禁划分一批记录；顺序保持，拒绝原因逐条可解释。"""
    readable: list[int] = []
    rejected: list[RejectedRecord] = []
    for index, info in enumerate(infos):
        verdict = decision_read_verdict(
            info, decision_time_utc, require_receipt=require_receipt, period_end_utc=period_end_utc
        )
        if verdict is DecisionReadVerdict.READABLE:
            readable.append(index)
        else:
            rejected.append(RejectedRecord(index=index, info=info, verdict=verdict))
    return PartitionedReadability(readable_indices=tuple(readable), rejected=tuple(rejected))


@dataclass(frozen=True, slots=True)
class DataSourceDescriptor:
    """一个数据来源的登记描述。

    ``preserves_receipt_time`` 是来源的合同声明：声明为 ``True`` 的来源必须为
    每条记录提供真实接收时刻（``OBSERVED`` 质量）。声明与记录不符是合同违约，
    不得静默降级记录质量。
    """

    source_id: str
    preserves_receipt_time: bool
    description: str = ""

    def __post_init__(self) -> None:
        if not self.source_id or any(char.isspace() for char in self.source_id) or len(self.source_id) > _MAX_SOURCE_ID_LEN:
            raise ValueError("source_id must be non-empty, whitespace-free and at most 64 characters.")


class DataSourceContractIssueCode(StrEnum):
    """来源合同违约问题码。"""

    SOURCE_ID_MISMATCH = "SOURCE_ID_MISMATCH"
    RECEIPT_PROMISE_BROKEN = "RECEIPT_PROMISE_BROKEN"


@dataclass(frozen=True, slots=True)
class DataSourceContractIssue:
    """一条来源合同违约：记录位置与问题码。"""

    index: int
    code: DataSourceContractIssueCode


def validate_source_contract(
    descriptor: DataSourceDescriptor,
    infos: Iterable[DataTimeInfo],
) -> tuple[DataSourceContractIssue, ...]:
    """核对一批记录是否履行其来源的合同声明。

    - 记录的 ``source_id`` 必须与描述符一致（拿错来源是最危险的一类错误）；
    - 声明保留真实接收时间的来源，其每条记录都必须是带 ``received_at`` 的
      ``OBSERVED`` 质量——历史回填或模型估计混进来就是合同违约。
    """
    issues: list[DataSourceContractIssue] = []
    for index, info in enumerate(infos):
        if info.source_id != descriptor.source_id:
            issues.append(DataSourceContractIssue(index=index, code=DataSourceContractIssueCode.SOURCE_ID_MISMATCH))
        if descriptor.preserves_receipt_time and (info.quality is not TimeInfoQuality.OBSERVED or info.received_at_utc is None):
            issues.append(DataSourceContractIssue(index=index, code=DataSourceContractIssueCode.RECEIPT_PROMISE_BROKEN))
    return tuple(issues)


@dataclass(frozen=True, slots=True)
class MissingGridRun:
    """一段缺失的 bar 区段：缺失起点（该有却没有的第一个 bar）与缺失数量。"""

    expected_start_utc: datetime
    missing_count: int


@dataclass(frozen=True, slots=True)
class GridIntegrityReport:
    """一批记录在 bar 网格上的完整性报告。

    ``gaps`` 基于排序后的去重时刻计算；``duplicate_event_times`` 与
    ``non_monotonic_indices`` 反映原始顺序的问题；``non_utc_indices`` 是无法参与
    网格计算的记录位置（fail-closed，不猜测时区）。
    """

    gaps: tuple[MissingGridRun, ...]
    duplicate_event_times: tuple[datetime, ...]
    non_monotonic_indices: tuple[int, ...]
    non_utc_indices: tuple[int, ...]


def find_grid_gaps(infos: Sequence[DataTimeInfo], timeframe: OhlcvTimeframe) -> GridIntegrityReport:
    """在 ``timeframe`` 的固定秒数网格上识别缺失区段、重复时刻与乱序位置。

    网格以相邻记录的实际差为准（不假设整点对齐），差值必须是 bar 长度的整数倍；
    ``1w`` 之类按固定 604800 秒处理，与 Binance 周线 bar 的实际行为一致。不是
    UTC 的 event_time 无法参与网格计算，原样报告位置而不是猜测时区。
    """
    non_utc = tuple(index for index, info in enumerate(infos) if not _require_utc(info.event_time_utc))
    usable = [(index, info.event_time_utc) for index, info in enumerate(infos) if _require_utc(info.event_time_utc)]

    non_monotonic: list[int] = []
    for (prev_index, prev_time), (cur_index, cur_time) in pairwise(usable):
        if cur_time < prev_time:
            non_monotonic.append(cur_index)

    counts: dict[datetime, int] = {}
    for _, event_time in usable:
        counts[event_time] = counts.get(event_time, 0) + 1
    duplicates = tuple(sorted(time for time, count in counts.items() if count > 1))

    bar_seconds = timeframe_seconds(timeframe)
    step = timedelta(seconds=bar_seconds)
    gaps: list[MissingGridRun] = []
    distinct_times = sorted(counts)
    for prev_time, cur_time in pairwise(distinct_times):
        delta = cur_time - prev_time
        steps = delta // step
        if steps > 1 and steps * step == delta:
            gaps.append(MissingGridRun(expected_start_utc=prev_time + step, missing_count=int(steps - 1)))
    return GridIntegrityReport(
        gaps=tuple(gaps),
        duplicate_event_times=duplicates,
        non_monotonic_indices=tuple(non_monotonic),
        non_utc_indices=non_utc,
    )
