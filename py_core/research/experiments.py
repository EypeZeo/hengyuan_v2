"""实验登记与独立保留数据访问纪律（PLAN-01，对应任务 3.2）。

合同（docs/RESEARCH_EXECUTION_PLAN.md §4.3，蓝图 6.6 任务 3.2）：

- 每次研究保存 trial_id、family_id、父实验、研究目的、候选参数列表、数据版本
  及范围、代码版本标识、成本模型、训练与评价窗口、随机种子、失败原因、观察
  结果的时间与参与选择的人员或程序；完整登记**包括失败候选与无交易候选**。
- 登记是**追加式**的：没有更新与删除，修改策略后创建新实验（必要时以
  ``SUPERSEDED`` + 父链接表达替代关系），不沿用旧版本的最终评价结论。
- 报告总尝试数（含失败与无交易）与按状态的划分；诊断实验若影响了后续规则、
  参数或策略选择，以 ``influenced_selection`` 纳入选择历史。
- 实验登记必须带 ``causal_check``（SAFE-05 / D1 的准入合同）：只有
  ``prefix_differential`` 的实验允许以 ``COMPLETED`` 收尾；``index_only`` 与
  ``not_run`` 是诚实的状态、可以登记，但不能承载最终评价结论。
- 独立保留数据（盲测库）只有在访问记录支持"未用于研究"时才承担独立检验作用：
  任何研究目的的读取永久污染；一次冻结评估用掉即花费；一个盲测库只能服务
  恰好一次最终评估。调参进程读取盲测库必须留下访问记录（蓝图 V-16）。

两个台账都是追加式 JSONL：每行一个自描述记录，加载时逐行校验并拒绝重复
trial_id、先子后父与任何损坏行。本模块是纯逻辑 + 本地文件追加，无网络、
无交易路径、不触碰任何仓库分区数据。
"""

from __future__ import annotations

import json
from collections.abc import Iterable
from dataclasses import dataclass
from datetime import UTC, datetime
from enum import StrEnum
from pathlib import Path
from typing import Any

__all__ = [
    "BlindAccessKind",
    "BlindSetAccess",
    "BlindSetAccessLog",
    "BlindSetStatus",
    "CausalCheck",
    "ExperimentIssue",
    "ExperimentIssueCode",
    "ExperimentRecord",
    "ExperimentRegistry",
    "ExperimentStatus",
    "RegistryCorruptError",
    "RegistryFormatError",
    "RegistryUsageViolation",
    "RegistryUsageViolationCode",
    "counts_by_status",
    "record_from_json_line",
    "record_to_json_line",
    "verify_blind_set_usage",
]

_MAX_ID_LEN = 64


class ExperimentStatus(StrEnum):
    """一次尝试的终局状态；没有中间态——重跑就是新的 trial_id。"""

    PLANNED = "planned"
    COMPLETED = "completed"
    FAILED = "failed"
    NO_TRADE = "no_trade"
    SUPERSEDED = "superseded"


class CausalCheck(StrEnum):
    """因果检查的等级（SAFE-05 / D1）：只有前缀差分承载最终评价结论。"""

    PREFIX_DIFFERENTIAL = "prefix_differential"
    INDEX_ONLY = "index_only"
    NOT_RUN = "not_run"


class BlindAccessKind(StrEnum):
    """一次盲测库访问的性质：研究读取永久污染，冻结评估用掉即花费。"""

    RESEARCH = "research"
    FINAL_EVALUATION = "final_evaluation"


class BlindSetStatus(StrEnum):
    """一个盲测库的访问纪律状态。"""

    UNTOUCHED = "untouched"
    SPENT_BY_FINAL_EVALUATION = "spent_by_final_evaluation"
    TAINTED_BY_RESEARCH = "tainted_by_research"


class ExperimentIssueCode(StrEnum):
    """一条实验记录的确定性校验问题码。"""

    NOT_TIMEZONE_AWARE_UTC = "NOT_TIMEZONE_AWARE_UTC"
    BAD_IDENTIFIER = "BAD_IDENTIFIER"
    EMPTY_TEXT = "EMPTY_TEXT"
    EMPTY_PARAMETER_NAME = "EMPTY_PARAMETER_NAME"
    DUPLICATE_PARAMETER_NAME = "DUPLICATE_PARAMETER_NAME"
    WINDOW_NOT_ORDERED = "WINDOW_NOT_ORDERED"
    TRAIN_EVAL_OVERLAP = "TRAIN_EVAL_OVERLAP"
    DATA_DOES_NOT_COVER_WINDOWS = "DATA_DOES_NOT_COVER_WINDOWS"
    NEGATIVE_RANDOM_SEED = "NEGATIVE_RANDOM_SEED"
    FAILED_WITHOUT_REASON = "FAILED_WITHOUT_REASON"
    FAILURE_REASON_ON_NON_FAILURE = "FAILURE_REASON_ON_NON_FAILURE"
    COMPLETED_WITHOUT_PREFIX_DIFFERENTIAL = "COMPLETED_WITHOUT_PREFIX_DIFFERENTIAL"


@dataclass(frozen=True, slots=True)
class ExperimentIssue:
    """一条实验记录校验问题。"""

    code: ExperimentIssueCode
    message: str


class RegistryFormatError(ValueError):
    """实验记录不满足登记合同；携带全部问题。"""


def _is_utc(value: datetime) -> bool:
    return value.tzinfo is not None and value.utcoffset() == UTC.utcoffset(value)


def _identifier_issue(field: str, value: str) -> ExperimentIssue | None:
    if not value or any(char.isspace() for char in value) or len(value) > _MAX_ID_LEN:
        return ExperimentIssue(ExperimentIssueCode.BAD_IDENTIFIER, f"{field} must be non-empty, whitespace-free and at most {_MAX_ID_LEN} characters.")
    return None


def _text_issue(field: str, value: str) -> ExperimentIssue | None:
    if not value.strip():
        return ExperimentIssue(ExperimentIssueCode.EMPTY_TEXT, f"{field} must be non-empty.")
    return None


@dataclass(frozen=True, slots=True)
class ExperimentRecord:
    """一次研究尝试的完整登记。

    ``parameters`` 是"名称 -> 值"的有序对（值以文本登记，候选空间按名称去重）；
    ``observed_at_utc`` 是这条结果被观察并记录的时刻；``recorded_by`` 是参与选择的
    人员或程序；``influenced_selection`` 把影响后续选择的诊断实验纳入选择历史。
    """

    trial_id: str
    family_id: str
    purpose: str
    status: ExperimentStatus
    parameters: tuple[tuple[str, str], ...]
    data_version: str
    data_start_utc: datetime
    data_end_utc: datetime
    code_version: str
    cost_model: str
    train_start_utc: datetime
    train_end_utc: datetime
    eval_start_utc: datetime
    eval_end_utc: datetime
    random_seed: int
    causal_check: CausalCheck
    observed_at_utc: datetime
    recorded_by: str
    influenced_selection: bool = False
    parent_trial_id: str | None = None
    blind_set_id: str | None = None
    failure_reason: str | None = None


def validate_experiment_record(record: ExperimentRecord) -> tuple[ExperimentIssue, ...]:
    """校验一条实验记录；返回全部问题（空元组 = 通过）。失败关闭，不静默修正。"""
    issues: list[ExperimentIssue] = []

    datetimes = {
        "data_start_utc": record.data_start_utc,
        "data_end_utc": record.data_end_utc,
        "train_start_utc": record.train_start_utc,
        "train_end_utc": record.train_end_utc,
        "eval_start_utc": record.eval_start_utc,
        "eval_end_utc": record.eval_end_utc,
        "observed_at_utc": record.observed_at_utc,
    }
    for field, value in datetimes.items():
        if not _is_utc(value):
            issues.append(ExperimentIssue(ExperimentIssueCode.NOT_TIMEZONE_AWARE_UTC, f"{field} must be timezone-aware UTC."))

    for field, value in (("trial_id", record.trial_id), ("family_id", record.family_id)):
        if issue := _identifier_issue(field, value):
            issues.append(issue)
    if record.parent_trial_id is not None and (issue := _identifier_issue("parent_trial_id", record.parent_trial_id)):
        issues.append(issue)
    if record.blind_set_id is not None and (issue := _identifier_issue("blind_set_id", record.blind_set_id)):
        issues.append(issue)

    for field, value in (
        ("purpose", record.purpose),
        ("data_version", record.data_version),
        ("code_version", record.code_version),
        ("cost_model", record.cost_model),
        ("recorded_by", record.recorded_by),
    ):
        if issue := _text_issue(field, value):
            issues.append(issue)

    names = [name for name, _ in record.parameters]
    if any(not name.strip() for name in names):
        issues.append(ExperimentIssue(ExperimentIssueCode.EMPTY_PARAMETER_NAME, "parameter names must be non-empty."))
    if len(names) != len(set(names)):
        issues.append(ExperimentIssue(ExperimentIssueCode.DUPLICATE_PARAMETER_NAME, "parameter names must be unique."))

    aware = {field: _is_utc(value) for field, value in datetimes.items()}
    if aware["train_start_utc"] and aware["train_end_utc"] and not record.train_start_utc < record.train_end_utc:
        issues.append(ExperimentIssue(ExperimentIssueCode.WINDOW_NOT_ORDERED, "train window must satisfy start < end."))
    if aware["eval_start_utc"] and aware["eval_end_utc"] and not record.eval_start_utc < record.eval_end_utc:
        issues.append(ExperimentIssue(ExperimentIssueCode.WINDOW_NOT_ORDERED, "evaluation window must satisfy start < end."))
    if (
        aware["train_end_utc"]
        and aware["eval_start_utc"]
        and record.train_end_utc > record.eval_start_utc
    ):
        issues.append(ExperimentIssue(ExperimentIssueCode.TRAIN_EVAL_OVERLAP, "the evaluation window must not overlap the training window."))
    if aware["data_start_utc"] and aware["train_start_utc"] and record.data_start_utc > record.train_start_utc:
        issues.append(ExperimentIssue(ExperimentIssueCode.DATA_DOES_NOT_COVER_WINDOWS, "data_start_utc must not start after the training window."))
    if aware["data_end_utc"] and aware["eval_end_utc"] and record.data_end_utc < record.eval_end_utc:
        issues.append(ExperimentIssue(ExperimentIssueCode.DATA_DOES_NOT_COVER_WINDOWS, "data_end_utc must not end before the evaluation window."))

    if record.random_seed < 0:
        issues.append(ExperimentIssue(ExperimentIssueCode.NEGATIVE_RANDOM_SEED, "random_seed must not be negative."))

    if record.status is ExperimentStatus.FAILED and (record.failure_reason is None or not record.failure_reason.strip()):
        issues.append(ExperimentIssue(ExperimentIssueCode.FAILED_WITHOUT_REASON, "a FAILED trial must record its failure reason."))
    if record.status is not ExperimentStatus.FAILED and record.failure_reason is not None:
        issues.append(ExperimentIssue(ExperimentIssueCode.FAILURE_REASON_ON_NON_FAILURE, "failure_reason belongs on FAILED trials only."))

    if record.status is ExperimentStatus.COMPLETED and record.causal_check is not CausalCheck.PREFIX_DIFFERENTIAL:
        issues.append(
            ExperimentIssue(
                ExperimentIssueCode.COMPLETED_WITHOUT_PREFIX_DIFFERENTIAL,
                "a COMPLETED trial must carry causal_check=prefix_differential (SAFE-05 / D1 admission contract).",
            )
        )

    return tuple(issues)


def require_valid_experiment_record(record: ExperimentRecord) -> None:
    """校验失败即抛 :class:`RegistryFormatError`（携带全部问题）。"""
    issues = validate_experiment_record(record)
    if issues:
        raise RegistryFormatError("; ".join(issue.message for issue in issues))


# ---------------------------------------------------------------------------
# JSONL 追加式台账
# ---------------------------------------------------------------------------

_DATETIME_FIELDS = ("data_start_utc", "data_end_utc", "train_start_utc", "train_end_utc", "eval_start_utc", "eval_end_utc", "observed_at_utc")


class RegistryCorruptError(ValueError):
    """台账文件损坏：坏 JSON、未知枚举值、缺失字段或先子后父。携带行号。"""


def record_to_json_line(record: ExperimentRecord) -> str:
    """一条记录的规范 JSON 行（``parameters`` 保序，时刻为 ISO 8601 UTC 文本）。"""
    payload: dict[str, Any] = {
        "trial_id": record.trial_id,
        "family_id": record.family_id,
        "parent_trial_id": record.parent_trial_id,
        "purpose": record.purpose,
        "status": record.status.value,
        "parameters": [[name, value] for name, value in record.parameters],
        "data_version": record.data_version,
        "data_start_utc": record.data_start_utc.isoformat(),
        "data_end_utc": record.data_end_utc.isoformat(),
        "code_version": record.code_version,
        "cost_model": record.cost_model,
        "train_start_utc": record.train_start_utc.isoformat(),
        "train_end_utc": record.train_end_utc.isoformat(),
        "eval_start_utc": record.eval_start_utc.isoformat(),
        "eval_end_utc": record.eval_end_utc.isoformat(),
        "random_seed": record.random_seed,
        "causal_check": record.causal_check.value,
        "blind_set_id": record.blind_set_id,
        "failure_reason": record.failure_reason,
        "observed_at_utc": record.observed_at_utc.isoformat(),
        "recorded_by": record.recorded_by,
        "influenced_selection": record.influenced_selection,
    }
    return json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def record_from_json_line(line: str, line_number: int) -> ExperimentRecord:
    """解析并重建一条记录；任何形状错误都以行号定位并抛 :class:`RegistryCorruptError`。"""

    def corrupt(reason: str) -> RegistryCorruptError:
        return RegistryCorruptError(f"line {line_number}: {reason}")

    try:
        payload = json.loads(line)
    except json.JSONDecodeError as exc:
        raise corrupt(f"invalid JSON ({exc.msg})") from exc
    if not isinstance(payload, dict):
        raise corrupt("a record must be a JSON object")

    def field(name: str) -> Any:
        if name not in payload:
            raise corrupt(f"missing field {name!r}")
        return payload[name]

    def datetime_field(name: str) -> datetime:
        try:
            value = datetime.fromisoformat(field(name))
        except (TypeError, ValueError) as exc:
            raise corrupt(f"field {name!r} is not an ISO 8601 datetime") from exc
        if not _is_utc(value):
            raise corrupt(f"field {name!r} must be timezone-aware UTC")
        return value

    try:
        status = ExperimentStatus(field("status"))
        causal = CausalCheck(field("causal_check"))
    except ValueError as exc:
        raise corrupt(str(exc)) from exc

    raw_parameters = field("parameters")
    if not isinstance(raw_parameters, list) or any(
        not isinstance(pair, list) or len(pair) != 2 or not isinstance(pair[0], str) or not isinstance(pair[1], str) for pair in raw_parameters
    ):
        raise corrupt("parameters must be a list of [name, value] string pairs")

    parameters = tuple((pair[0], pair[1]) for pair in raw_parameters)

    string_fields = ("trial_id", "family_id", "purpose", "data_version", "code_version", "cost_model", "recorded_by")
    for name in string_fields:
        if not isinstance(payload[name], str):
            raise corrupt(f"field {name!r} must be a string")
    for name in ("parent_trial_id", "blind_set_id", "failure_reason"):
        if payload[name] is not None and not isinstance(payload[name], str):
            raise corrupt(f"field {name!r} must be a string or null")
    if not isinstance(payload["influenced_selection"], bool):
        raise corrupt("field 'influenced_selection' must be a boolean")
    seed = payload["random_seed"]
    if not isinstance(seed, int) or isinstance(seed, bool):
        raise corrupt("random_seed must be an integer")

    record = ExperimentRecord(
        trial_id=field("trial_id"),
        family_id=field("family_id"),
        parent_trial_id=field("parent_trial_id"),
        purpose=field("purpose"),
        status=status,
        parameters=parameters,
        data_version=field("data_version"),
        data_start_utc=datetime_field("data_start_utc"),
        data_end_utc=datetime_field("data_end_utc"),
        code_version=field("code_version"),
        cost_model=field("cost_model"),
        train_start_utc=datetime_field("train_start_utc"),
        train_end_utc=datetime_field("train_end_utc"),
        eval_start_utc=datetime_field("eval_start_utc"),
        eval_end_utc=datetime_field("eval_end_utc"),
        random_seed=field("random_seed"),
        causal_check=causal,
        blind_set_id=field("blind_set_id"),
        failure_reason=field("failure_reason"),
        observed_at_utc=datetime_field("observed_at_utc"),
        recorded_by=field("recorded_by"),
        influenced_selection=field("influenced_selection"),
    )
    try:
        require_valid_experiment_record(record)
    except RegistryFormatError as exc:
        raise corrupt(str(exc)) from exc
    return record


class ExperimentRegistry:
    """追加式实验台账。

    没有 update、没有 delete：修正与替代都是新的 ``trial_id``。``register()`` 校验
    记录本身、拒绝重复 ``trial_id``，并要求父实验已经登记（追加式台账的先子后父，
    从结构上排除替代链的环）。``load()`` 按同一规则逐行重建整个文件。
    """

    def __init__(self, path: Path) -> None:
        self._path = path
        self._records: list[ExperimentRecord] = []
        self._trial_ids: set[str] = set()
        self._load()

    @property
    def path(self) -> Path:
        return self._path

    def register(self, record: ExperimentRecord) -> None:
        """校验并追加一条记录；文件追加成功后内存视图才更新（先落盘、后记账）。"""
        require_valid_experiment_record(record)
        if record.trial_id in self._trial_ids:
            raise RegistryFormatError(f"trial_id {record.trial_id!r} is already registered; a rerun takes a new trial_id")
        if record.parent_trial_id is not None and record.parent_trial_id not in self._trial_ids:
            raise RegistryFormatError(f"parent trial {record.parent_trial_id!r} must be registered before {record.trial_id!r}")
        line = record_to_json_line(record)
        with self._path.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(line + "\n")
        self._records.append(record)
        self._trial_ids.add(record.trial_id)

    def all_records(self) -> tuple[ExperimentRecord, ...]:
        """登记顺序的全部记录（报告总尝试数的口径：含失败与无交易候选）。"""
        return tuple(self._records)

    def trial_ids(self) -> frozenset[str]:
        return frozenset(self._trial_ids)

    def by_family(self, family_id: str) -> tuple[ExperimentRecord, ...]:
        return tuple(record for record in self._records if record.family_id == family_id)

    def by_trial(self, trial_id: str) -> ExperimentRecord | None:
        for record in self._records:
            if record.trial_id == trial_id:
                return record
        return None

    def counts_by_status(self) -> dict[ExperimentStatus, int]:
        return counts_by_status(self._records)

    def _load(self) -> None:
        if not self._path.exists():
            return
        with self._path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, start=1):
                stripped = line.strip()
                if not stripped:
                    continue
                record = record_from_json_line(stripped, line_number)
                if record.trial_id in self._trial_ids:
                    raise RegistryCorruptError(f"line {line_number}: duplicate trial_id {record.trial_id!r}")
                if record.parent_trial_id is not None and record.parent_trial_id not in self._trial_ids:
                    raise RegistryCorruptError(f"line {line_number}: parent trial {record.parent_trial_id!r} appears after {record.trial_id!r}")
                self._records.append(record)
                self._trial_ids.add(record.trial_id)


def counts_by_status(records: Iterable[ExperimentRecord]) -> dict[ExperimentStatus, int]:
    """按状态计数；缺席的状态不出现在结果里，总数即总尝试数。"""
    counts: dict[ExperimentStatus, int] = {}
    for record in records:
        counts[record.status] = counts.get(record.status, 0) + 1
    return counts


# ---------------------------------------------------------------------------
# 独立保留数据（盲测库）访问纪律
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class BlindSetAccess:
    """一次盲测库访问：谁、为什么性质、何时、由谁记录。"""

    blind_set_id: str
    kind: BlindAccessKind
    accessed_at_utc: datetime
    recorded_by: str
    trial_id: str | None = None


class BlindSetAccessLog:
    """追加式盲测库访问日志。

    研究目的的读取**必须**被记录（V-16）：日志不拒绝任何访问，污染靠如实登记
    被检出，而不是靠禁止写入。冻结评估也是一次访问，用掉即花费。
    """

    def __init__(self, path: Path) -> None:
        self._path = path
        self._accesses: list[BlindSetAccess] = []
        self._load()

    @property
    def path(self) -> Path:
        return self._path

    def record(self, access: BlindSetAccess) -> None:
        issues = _validate_access(access)
        if issues:
            raise RegistryFormatError("; ".join(issues))
        line = json.dumps(
            {
                "blind_set_id": access.blind_set_id,
                "kind": access.kind.value,
                "accessed_at_utc": access.accessed_at_utc.isoformat(),
                "recorded_by": access.recorded_by,
                "trial_id": access.trial_id,
            },
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        with self._path.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(line + "\n")
        self._accesses.append(access)

    def all_accesses(self) -> tuple[BlindSetAccess, ...]:
        return tuple(self._accesses)

    def status_of(self, blind_set_id: str) -> BlindSetStatus:
        """一个盲测库的当前纪律状态：任何研究读取永久污染，冻结评估用掉即花费。"""
        status = BlindSetStatus.UNTOUCHED
        for access in self._accesses:
            if access.blind_set_id != blind_set_id:
                continue
            if access.kind is BlindAccessKind.RESEARCH:
                return BlindSetStatus.TAINTED_BY_RESEARCH
            status = BlindSetStatus.SPENT_BY_FINAL_EVALUATION
        return status

    def _load(self) -> None:
        if not self._path.exists():
            return
        with self._path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, start=1):
                stripped = line.strip()
                if not stripped:
                    continue
                self._accesses.append(_access_from_json_line(stripped, line_number))


def _validate_access(access: BlindSetAccess) -> tuple[str, ...]:
    issues: list[str] = []
    if not access.blind_set_id or any(char.isspace() for char in access.blind_set_id) or len(access.blind_set_id) > _MAX_ID_LEN:
        issues.append("blind_set_id must be non-empty, whitespace-free and at most 64 characters.")
    if access.trial_id is not None and (not access.trial_id or any(char.isspace() for char in access.trial_id) or len(access.trial_id) > _MAX_ID_LEN):
        issues.append("trial_id must be non-empty, whitespace-free and at most 64 characters when present.")
    if not access.recorded_by.strip():
        issues.append("recorded_by must be non-empty.")
    if not _is_utc(access.accessed_at_utc):
        issues.append("accessed_at_utc must be timezone-aware UTC.")
    return tuple(issues)


def _access_from_json_line(line: str, line_number: int) -> BlindSetAccess:
    def corrupt(reason: str) -> RegistryCorruptError:
        return RegistryCorruptError(f"line {line_number}: {reason}")

    try:
        payload = json.loads(line)
    except json.JSONDecodeError as exc:
        raise corrupt(f"invalid JSON ({exc.msg})") from exc
    if not isinstance(payload, dict) or any(name not in payload for name in ("blind_set_id", "kind", "accessed_at_utc", "recorded_by", "trial_id")):
        raise corrupt("an access record must be a JSON object with the documented fields")
    try:
        kind = BlindAccessKind(payload["kind"])
    except ValueError as exc:
        raise corrupt(str(exc)) from exc
    try:
        accessed_at = datetime.fromisoformat(payload["accessed_at_utc"])
    except (TypeError, ValueError) as exc:
        raise corrupt("accessed_at_utc is not an ISO 8601 datetime") from exc
    if not _is_utc(accessed_at):
        raise corrupt("accessed_at_utc must be timezone-aware UTC")
    access = BlindSetAccess(
        blind_set_id=payload["blind_set_id"],
        kind=kind,
        accessed_at_utc=accessed_at,
        recorded_by=payload["recorded_by"],
        trial_id=payload["trial_id"],
    )
    if issues := _validate_access(access):
        raise corrupt(issues[0])
    return access


class RegistryUsageViolationCode(StrEnum):
    """实验与盲测库联动核验的违约问题码。"""

    BLIND_SET_TAINTED = "BLIND_SET_TAINTED"
    BLIND_SET_NO_RECORDED_ACCESS = "BLIND_SET_NO_RECORDED_ACCESS"
    BLIND_SET_REUSED = "BLIND_SET_REUSED"


@dataclass(frozen=True, slots=True)
class RegistryUsageViolation:
    """一处实验/盲测库联动违约：trial、盲测库与问题码。"""

    trial_id: str
    blind_set_id: str
    code: RegistryUsageViolationCode


def verify_blind_set_usage(
    registry: ExperimentRegistry,
    access_log: BlindSetAccessLog,
) -> tuple[RegistryUsageViolation, ...]:
    """核验每个引用了盲测库的 ``COMPLETED`` 实验是否履行访问纪律。

    - 盲测库被研究读取污染过（``TAINTED_BY_RESEARCH``）→ 它不承担独立检验作用；
    - 没有任何访问记录（``UNTOUCHED``）→ 声称用了盲测库但台账不支持，不得作为
      独立检验的证据（规划 §4.3：访问记录支持未用于研究时才承担独立检验）；
    - 冻结评估记录在另一个 trial 名下（``SPENT`` 且首次评估不是本 trial）→
      一个盲测库只能服务恰好一次最终评估。
    """
    violations: list[RegistryUsageViolation] = []
    for record in registry.all_records():
        if record.status is not ExperimentStatus.COMPLETED or record.blind_set_id is None:
            continue
        status = access_log.status_of(record.blind_set_id)
        if status is BlindSetStatus.TAINTED_BY_RESEARCH:
            violations.append(RegistryUsageViolation(record.trial_id, record.blind_set_id, RegistryUsageViolationCode.BLIND_SET_TAINTED))
        elif status is BlindSetStatus.UNTOUCHED:
            violations.append(
                RegistryUsageViolation(record.trial_id, record.blind_set_id, RegistryUsageViolationCode.BLIND_SET_NO_RECORDED_ACCESS)
            )
        else:
            first_evaluation = next(
                (
                    access
                    for access in access_log.all_accesses()
                    if access.blind_set_id == record.blind_set_id and access.kind is BlindAccessKind.FINAL_EVALUATION
                ),
                None,
            )
            if first_evaluation is None or first_evaluation.trial_id != record.trial_id:
                violations.append(RegistryUsageViolation(record.trial_id, record.blind_set_id, RegistryUsageViolationCode.BLIND_SET_REUSED))
    return tuple(violations)
