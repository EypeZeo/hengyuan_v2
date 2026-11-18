"""PLAN-01 实验登记与独立保留数据访问纪律的测试（py_core/research/experiments.py）。

覆盖：记录校验的全部问题码、追加式 JSONL 台账（往返、重复、先子后父、损坏行
定位）、状态计数口径（含失败与无交易）、盲测库访问日志的三种纪律状态，以及
实验与盲测库联动的核验（污染、无访问记录、复用）。
"""

from __future__ import annotations

import json
from dataclasses import replace
from datetime import UTC, datetime
from pathlib import Path

import pytest

from py_core.research.experiments import (
    BlindAccessKind,
    BlindSetAccess,
    BlindSetAccessLog,
    BlindSetStatus,
    CausalCheck,
    ExperimentIssueCode,
    ExperimentRecord,
    ExperimentRegistry,
    ExperimentStatus,
    RegistryCorruptError,
    RegistryFormatError,
    RegistryUsageViolationCode,
    counts_by_status,
    is_admission_eligible,
    params_from_mapping,
    record_from_json_line,
    record_to_json_line,
    validate_experiment_record,
    verify_blind_set_usage,
)

T0 = datetime(2025, 1, 1, tzinfo=UTC)
TRAIN_END = datetime(2025, 10, 1, tzinfo=UTC)
EVAL_END = datetime(2026, 1, 1, tzinfo=UTC)
DATA_END = datetime(2026, 2, 1, tzinfo=UTC)
OBSERVED_AT = datetime(2026, 2, 2, tzinfo=UTC)


def make_record(**overrides: object) -> ExperimentRecord:
    """一条默认合法的 COMPLETED 记录；测试按字段名覆盖。"""
    defaults: dict[str, object] = {
        "trial_id": "t-0001",
        "family_id": "trend-family",
        "parent_trial_id": None,
        "purpose": "first trend candidate against buy-and-hold",
        "status": ExperimentStatus.COMPLETED,
        "parameters": (("threshold", "-1.5"),),
        "data_version": "ohlcv@2025-full",
        "data_start_utc": T0,
        "data_end_utc": DATA_END,
        "code_version": "py_core@abc1234",
        "cost_model": "10bp_per_side_fixed",
        "train_start_utc": T0,
        "train_end_utc": TRAIN_END,
        "eval_start_utc": TRAIN_END,
        "eval_end_utc": EVAL_END,
        "random_seed": 7,
        "causal_check": CausalCheck.PREFIX_DIFFERENTIAL,
        "observed_at_utc": OBSERVED_AT,
        "recorded_by": "researcher-a",
        "influenced_selection": False,
        "blind_set_id": None,
        "failure_reason": None,
    }
    return ExperimentRecord(**{**defaults, **overrides})  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# 记录校验
# ---------------------------------------------------------------------------


def validate_issues(record: ExperimentRecord) -> tuple[ExperimentIssueCode, ...]:
    return tuple(issue.code for issue in validate_experiment_record(record))


def test_a_default_record_and_every_status_are_valid() -> None:
    assert validate_issues(make_record()) == ()
    for status, causal in (
        (ExperimentStatus.FAILED, CausalCheck.PREFIX_DIFFERENTIAL),
        (ExperimentStatus.NO_TRADE, CausalCheck.PREFIX_DIFFERENTIAL),
        (ExperimentStatus.PLANNED, CausalCheck.NOT_RUN),
        (ExperimentStatus.SUPERSEDED, CausalCheck.INDEX_ONLY),
    ):
        overrides: dict[str, object] = {"status": status, "causal_check": causal, "trial_id": f"t-{status.value}"}
        if status is ExperimentStatus.FAILED:
            overrides["failure_reason"] = "insufficient data coverage"
        assert validate_issues(make_record(**overrides)) == (), status


def test_completed_with_a_weaker_causal_check_is_registrable_but_not_admission_eligible() -> None:
    # 登记 vs 准入的分工（任务 3.2 / 3.4）：预计算信号的诊断回测必须能进台账
    # （causal_check 如实登记为 index_only），但它过不了登记侧的准入必要条件。
    for causal in (CausalCheck.INDEX_ONLY, CausalCheck.NOT_RUN):
        record = make_record(causal_check=causal)
        assert validate_issues(record) == ()
        assert is_admission_eligible(record) is False
    assert is_admission_eligible(make_record()) is True
    # 缺少训练窗口的无训练评估运行同样登记合法、准入不合格。
    no_train = make_record(train_start_utc=None, train_end_utc=None)
    assert validate_issues(no_train) == ()
    assert is_admission_eligible(no_train) is False
    assert is_admission_eligible(make_record(status=ExperimentStatus.FAILED, failure_reason="boom")) is False


def test_failed_requires_a_reason_and_others_must_not_carry_one() -> None:
    assert validate_issues(make_record(status=ExperimentStatus.FAILED)) == (ExperimentIssueCode.FAILED_WITHOUT_REASON,)
    assert validate_issues(make_record(status=ExperimentStatus.FAILED, failure_reason="   ")) == (
        ExperimentIssueCode.FAILED_WITHOUT_REASON,
    )
    assert validate_issues(make_record(failure_reason="looks broken")) == (ExperimentIssueCode.FAILURE_REASON_ON_NON_FAILURE,)


@pytest.mark.parametrize(
    "field",
    ["data_start_utc", "data_end_utc", "train_start_utc", "train_end_utc", "eval_start_utc", "eval_end_utc", "observed_at_utc"],
)
def test_every_datetime_must_be_utc_aware(field: str) -> None:
    naive_value: datetime = getattr(make_record(), field).replace(tzinfo=None)
    assert validate_issues(make_record(**{field: naive_value})) == (ExperimentIssueCode.NOT_TIMEZONE_AWARE_UTC,)


@pytest.mark.parametrize("field", ["trial_id", "family_id", "parent_trial_id", "blind_set_id"])
@pytest.mark.parametrize("bad", ["", "a b", "x" * 65])
def test_identifiers_have_shape_rules(field: str, bad: str) -> None:
    assert validate_issues(make_record(**{field: bad})) == (ExperimentIssueCode.BAD_IDENTIFIER,)


@pytest.mark.parametrize("field", ["purpose", "data_version", "code_version", "cost_model", "recorded_by"])
def test_text_fields_must_be_non_empty(field: str) -> None:
    assert validate_issues(make_record(**{field: ""})) == (ExperimentIssueCode.EMPTY_TEXT,)
    assert validate_issues(make_record(**{field: "   "})) == (ExperimentIssueCode.EMPTY_TEXT,)


def test_parameter_names_must_be_unique_and_non_empty() -> None:
    assert validate_issues(make_record(parameters=(("threshold", "-1.5"), ("hold_days", "21")))) == ()
    assert validate_issues(make_record(parameters=(("", "1"),))) == (ExperimentIssueCode.EMPTY_PARAMETER_NAME,)
    assert validate_issues(make_record(parameters=(("threshold", "-1.5"), ("threshold", "-2")))) == (
        ExperimentIssueCode.DUPLICATE_PARAMETER_NAME,
    )


def test_windows_must_be_ordered_and_disjoint_and_covered_by_the_data() -> None:
    assert validate_issues(make_record(train_start_utc=TRAIN_END)) == (ExperimentIssueCode.WINDOW_NOT_ORDERED,)
    assert validate_issues(make_record(eval_start_utc=EVAL_END)) == (ExperimentIssueCode.WINDOW_NOT_ORDERED,)
    assert validate_issues(make_record(eval_start_utc=datetime(2025, 9, 1, tzinfo=UTC))) == (ExperimentIssueCode.TRAIN_EVAL_OVERLAP,)
    assert validate_issues(make_record(data_start_utc=datetime(2025, 2, 1, tzinfo=UTC))) == (
        ExperimentIssueCode.DATA_DOES_NOT_COVER_WINDOWS,
    )
    assert validate_issues(make_record(data_end_utc=datetime(2025, 12, 1, tzinfo=UTC))) == (
        ExperimentIssueCode.DATA_DOES_NOT_COVER_WINDOWS,
    )
    assert validate_issues(make_record(random_seed=-1)) == (ExperimentIssueCode.NEGATIVE_RANDOM_SEED,)
    assert validate_issues(make_record(random_seed=True)) == (ExperimentIssueCode.NEGATIVE_RANDOM_SEED,)


def test_an_evaluation_window_is_required_even_without_training() -> None:
    no_windows = make_record(eval_start_utc=None, eval_end_utc=None, train_start_utc=None, train_end_utc=None)
    assert validate_issues(no_windows) == (ExperimentIssueCode.EVALUATION_WINDOW_REQUIRED,)
    # 一次无训练的评估运行（策略/参数已冻结的全样本回测）登记合法。
    evaluation_only = make_record(train_start_utc=None, train_end_utc=None, random_seed=None)
    assert validate_issues(evaluation_only) == ()


def test_the_data_range_covers_evaluation_when_there_is_no_training_window() -> None:
    assert validate_issues(
        make_record(train_start_utc=None, train_end_utc=None, data_end_utc=datetime(2025, 12, 1, tzinfo=UTC))
    ) == (ExperimentIssueCode.DATA_DOES_NOT_COVER_WINDOWS,)
    # 评估窗是唯一的窗口：数据起点早于评估起点即合法，不再要求覆盖一段训练窗。
    assert validate_issues(
        make_record(train_start_utc=None, train_end_utc=None, data_start_utc=datetime(2025, 2, 1, tzinfo=UTC))
    ) == ()


def test_params_from_mapping_is_sorted_and_stable() -> None:
    pairs = params_from_mapping({"zeta": "1", "alpha": {"b": 2}, "mid": 3.5})
    assert [name for name, _ in pairs] == ["alpha", "mid", "zeta"]
    assert pairs == (("alpha", '{"b": 2}'), ("mid", "3.5"), ("zeta", "1"))
    assert params_from_mapping({"alpha": '{"b": 2}'}) == params_from_mapping({"alpha": '{"b": 2}'})


def test_the_json_line_round_trip_preserves_null_train_windows_and_null_seed() -> None:
    record = make_record(train_start_utc=None, train_end_utc=None, random_seed=None)
    restored = record_from_json_line(record_to_json_line(record), 1)
    assert restored == record


def test_adjacent_windows_share_a_boundary_legally() -> None:
    assert validate_issues(make_record()) == ()  # train_end == eval_start 是合法相邻


def test_the_data_range_itself_must_be_ordered() -> None:
    assert validate_issues(make_record(data_start_utc=DATA_END, data_end_utc=T0)) == (
        ExperimentIssueCode.DATA_WINDOW_NOT_ORDERED,
        ExperimentIssueCode.DATA_DOES_NOT_COVER_WINDOWS,
        ExperimentIssueCode.DATA_DOES_NOT_COVER_WINDOWS,
    )
    assert validate_issues(make_record(data_start_utc=T0, data_end_utc=T0)) == (
        ExperimentIssueCode.DATA_WINDOW_NOT_ORDERED,
        ExperimentIssueCode.DATA_DOES_NOT_COVER_WINDOWS,
    )


def test_a_completed_trial_is_observed_at_or_after_its_evaluation_window_ends() -> None:
    early_observation = replace(make_record(), observed_at_utc=datetime(2025, 12, 1, tzinfo=UTC))
    assert validate_issues(early_observation) == (ExperimentIssueCode.OBSERVED_BEFORE_EVALUATION_END,)
    assert validate_issues(make_record()) == ()  # observed_at 在 eval_end 之后
    # 非 COMPLETED 状态不受约束：计划中的实验在窗口结束前登记是正常的。
    planned = replace(make_record(), status=ExperimentStatus.PLANNED, causal_check=CausalCheck.NOT_RUN,
                      observed_at_utc=datetime(2025, 1, 2, tzinfo=UTC))
    assert validate_issues(planned) == ()


# ---------------------------------------------------------------------------
# 追加式台账
# ---------------------------------------------------------------------------


def test_register_round_trips_through_the_file_in_order(tmp_path: Path) -> None:
    path = tmp_path / "experiments.jsonl"
    registry = ExperimentRegistry(path)
    first = make_record()
    second = make_record(trial_id="t-0002", parent_trial_id="t-0001", status=ExperimentStatus.SUPERSEDED)
    registry.register(first)
    registry.register(second)

    reloaded = ExperimentRegistry(path)
    assert reloaded.all_records() == (first, second)
    assert reloaded.trial_ids() == frozenset({"t-0001", "t-0002"})
    assert reloaded.by_trial("t-0002") is not None and reloaded.by_trial("missing") is None
    assert reloaded.by_family("trend-family") == (first, second)


def test_a_rerun_takes_a_new_trial_id(tmp_path: Path) -> None:
    path = tmp_path / "experiments.jsonl"
    registry = ExperimentRegistry(path)
    registry.register(make_record())
    with pytest.raises(RegistryFormatError, match="already registered"):
        registry.register(make_record(trial_id="t-0001"))


def test_a_parent_must_be_registered_before_its_child(tmp_path: Path) -> None:
    path = tmp_path / "experiments.jsonl"
    registry = ExperimentRegistry(path)
    with pytest.raises(RegistryFormatError, match="must be registered before"):
        registry.register(make_record(parent_trial_id="t-0000"))


def test_a_child_before_its_parent_is_a_corrupt_file(tmp_path: Path) -> None:
    path = tmp_path / "experiments.jsonl"
    path.write_text(
        record_to_json_line(make_record(trial_id="t-0002", parent_trial_id="t-0001")) + "\n"
        + record_to_json_line(make_record(trial_id="t-0001")) + "\n",
        encoding="utf-8",
    )
    with pytest.raises(RegistryCorruptError, match="line 1"):
        ExperimentRegistry(path)


@pytest.mark.parametrize(
    "line_builder",
    [
        lambda: "{not json",
        lambda: json.dumps({"trial_id": "t-x"}),
        lambda: record_to_json_line(make_record(status=ExperimentStatus.PLANNED)).replace('"planned"', '"finished"'),
        lambda: record_to_json_line(make_record(status=ExperimentStatus.PLANNED)).replace("2025-01-01T00:00:00+00:00", "2025-01-01T00:00:00"),
        lambda: record_to_json_line(make_record()).replace('"random_seed":7', '"random_seed":true'),
        lambda: record_to_json_line(make_record()).replace('"random_seed":7', '"random_seed":7.5'),
        lambda: record_to_json_line(make_record()).replace('"purpose":"', '"purpose":42,"x":"'),
        lambda: record_to_json_line(make_record()) + '{"extra":true}',
        lambda: record_to_json_line(make_record()).replace('"eval_end_utc":"2026-01-01T00:00:00+00:00"', '"eval_end_utc":null'),
        lambda: record_to_json_line(make_record()) + record_to_json_line(make_record()),
    ],
)
def test_a_corrupt_line_is_located_and_refused(tmp_path: Path, line_builder) -> None:
    path = tmp_path / "experiments.jsonl"
    path.write_text(line_builder() + "\n", encoding="utf-8")
    with pytest.raises(RegistryCorruptError, match="line 1"):
        ExperimentRegistry(path)


def test_blank_lines_are_tolerated_and_an_empty_or_missing_file_is_an_empty_ledger(tmp_path: Path) -> None:
    path = tmp_path / "experiments.jsonl"
    assert ExperimentRegistry(path).all_records() == ()
    path.write_text("\n" + record_to_json_line(make_record()) + "\n\n", encoding="utf-8")
    reloaded = ExperimentRegistry(path)
    assert len(reloaded.all_records()) == 1


def test_counts_report_every_attempt_including_failures_and_no_trades() -> None:
    records = [
        make_record(trial_id="t-0001"),
        make_record(trial_id="t-0002", status=ExperimentStatus.FAILED, failure_reason="boom"),
        make_record(trial_id="t-0003", status=ExperimentStatus.NO_TRADE),
        make_record(trial_id="t-0004", status=ExperimentStatus.PLANNED, causal_check=CausalCheck.NOT_RUN),
        make_record(trial_id="t-0005", status=ExperimentStatus.FAILED, failure_reason="boom"),
    ]
    counts = counts_by_status(records)
    assert counts == {
        ExperimentStatus.COMPLETED: 1,
        ExperimentStatus.FAILED: 2,
        ExperimentStatus.NO_TRADE: 1,
        ExperimentStatus.PLANNED: 1,
    }
    assert sum(counts.values()) == 5


def test_the_json_line_round_trip_preserves_every_field(tmp_path: Path) -> None:
    record = make_record(
        parent_trial_id="t-0000",
        blind_set_id="holdout-2026",
        influenced_selection=True,
        parameters=(("threshold", "-1.5"), ("hold_days", "21")),
        status=ExperimentStatus.PLANNED,
        causal_check=CausalCheck.NOT_RUN,
    )
    restored = record_from_json_line(record_to_json_line(record), 1)
    assert restored == record


# ---------------------------------------------------------------------------
# 盲测库访问日志
# ---------------------------------------------------------------------------


def access(blind_set_id: str = "holdout-2026", kind: BlindAccessKind = BlindAccessKind.FINAL_EVALUATION,
           trial_id: str | None = "t-0001") -> BlindSetAccess:
    return BlindSetAccess(
        blind_set_id=blind_set_id,
        kind=kind,
        accessed_at_utc=OBSERVED_AT,
        recorded_by="researcher-a",
        trial_id=trial_id,
    )


def test_blind_set_statuses(tmp_path: Path) -> None:
    log = BlindSetAccessLog(tmp_path / "blind.jsonl")
    assert log.status_of("holdout-2026") is BlindSetStatus.UNTOUCHED
    log.record(access(kind=BlindAccessKind.FINAL_EVALUATION))
    assert log.status_of("holdout-2026") is BlindSetStatus.SPENT_BY_FINAL_EVALUATION
    log.record(access(kind=BlindAccessKind.RESEARCH, trial_id=None))
    assert log.status_of("holdout-2026") is BlindSetStatus.TAINTED_BY_RESEARCH


def test_research_access_taints_even_when_it_precedes_the_final_evaluation(tmp_path: Path) -> None:
    log = BlindSetAccessLog(tmp_path / "blind.jsonl")
    log.record(access(kind=BlindAccessKind.RESEARCH, trial_id=None))
    log.record(access(kind=BlindAccessKind.FINAL_EVALUATION))
    assert log.status_of("holdout-2026") is BlindSetStatus.TAINTED_BY_RESEARCH


def test_access_log_round_trips_and_refuses_bad_records(tmp_path: Path) -> None:
    path = tmp_path / "blind.jsonl"
    log = BlindSetAccessLog(path)
    log.record(access())
    log.record(access(kind=BlindAccessKind.RESEARCH, trial_id=None))
    reloaded = BlindSetAccessLog(path)
    assert reloaded.all_accesses() == log.all_accesses()

    for bad in (
        access(blind_set_id=""),
        access(trial_id="a b"),
        replace(access(), accessed_at_utc=OBSERVED_AT.replace(tzinfo=None)),
        replace(access(), recorded_by="  "),
    ):
        with pytest.raises(RegistryFormatError):
            log.record(bad)


def test_a_corrupt_access_line_is_located(tmp_path: Path) -> None:
    path = tmp_path / "blind.jsonl"
    path.write_text('{"blind_set_id":"holdout-2026","kind":"peep","accessed_at_utc":"2026-02-02T00:00:00+00:00","recorded_by":"a","trial_id":null}\n', encoding="utf-8")
    with pytest.raises(RegistryCorruptError, match="line 1"):
        BlindSetAccessLog(path)


# ---------------------------------------------------------------------------
# 实验 × 盲测库联动核验
# ---------------------------------------------------------------------------


def setup_linked(tmp_path: Path, *, registry_status: ExperimentStatus = ExperimentStatus.COMPLETED,
                 accesses: tuple[BlindSetAccess, ...] = ()) -> tuple[ExperimentRegistry, BlindSetAccessLog]:
    registry = ExperimentRegistry(tmp_path / "experiments.jsonl")
    registry.register(make_record(status=registry_status, blind_set_id="holdout-2026"))
    log = BlindSetAccessLog(tmp_path / "blind.jsonl")
    for entry in accesses:
        log.record(entry)
    return registry, log


def test_a_completed_trial_whose_final_evaluation_is_recorded_is_clean(tmp_path: Path) -> None:
    registry, log = setup_linked(tmp_path, accesses=(access(),))
    assert verify_blind_set_usage(registry, log) == ()


def test_a_completed_trial_on_an_untouched_blind_set_is_unverifiable(tmp_path: Path) -> None:
    registry, log = setup_linked(tmp_path)
    violations = verify_blind_set_usage(registry, log)
    assert [(v.trial_id, v.code) for v in violations] == [("t-0001", RegistryUsageViolationCode.BLIND_SET_NO_RECORDED_ACCESS)]


def test_a_tainted_blind_set_carries_no_independent_evidence(tmp_path: Path) -> None:
    registry, log = setup_linked(tmp_path, accesses=(access(kind=BlindAccessKind.RESEARCH, trial_id=None), access()))
    violations = verify_blind_set_usage(registry, log)
    assert [(v.trial_id, v.code) for v in violations] == [("t-0001", RegistryUsageViolationCode.BLIND_SET_TAINTED)]


def test_a_blind_set_serves_exactly_one_final_evaluation(tmp_path: Path) -> None:
    registry, log = setup_linked(tmp_path)
    registry.register(make_record(trial_id="t-0002", blind_set_id="holdout-2026"))
    log.record(access(trial_id="t-0001"))
    violations = verify_blind_set_usage(registry, log)
    assert [(v.trial_id, v.code) for v in violations] == [("t-0002", RegistryUsageViolationCode.BLIND_SET_REUSED)]


def test_non_completed_trials_do_not_claim_blind_set_evidence(tmp_path: Path) -> None:
    registry = ExperimentRegistry(tmp_path / "experiments.jsonl")
    registry.register(make_record(trial_id="t-0001", status=ExperimentStatus.FAILED, failure_reason="boom", blind_set_id="holdout-2026"))
    log = BlindSetAccessLog(tmp_path / "blind.jsonl")
    assert verify_blind_set_usage(registry, log) == ()


def test_a_trial_without_a_blind_set_reference_is_out_of_scope(tmp_path: Path) -> None:
    registry = ExperimentRegistry(tmp_path / "experiments.jsonl")
    registry.register(make_record())
    log = BlindSetAccessLog(tmp_path / "blind.jsonl")
    assert verify_blind_set_usage(registry, log) == ()
