"""PLAN-05a HAC/Sharpe 标准误的测试（py_core/research/hac.py）。

参考值由本文件内的纯 Python 循环独立重算；生产模块不参与参考值生成。
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from py_core.research.hac import HacStatus, estimate_sharpe_hac


def reference_hac(values: list[float], lag: int) -> dict[str, object]:
    """规划 §6.3 的独立纯 Python 实现（与生产代码只共享公式，不共享函数）。"""
    n = len(values)
    mean = sum(values) / n
    variance = sum((value - mean) ** 2 for value in values) / n
    sigma = math.sqrt(variance)
    sharpe = mean / sigma
    influence = [
        (value - mean) / sigma - (sharpe / 2) * (((value - mean) ** 2 / variance) - 1)
        for value in values
    ]
    influence_mean = sum(influence) / n
    centered = [value - influence_mean for value in influence]
    gamma = [
        sum(centered[t] * centered[t - k] for t in range(k, n)) / n
        for k in range(lag + 1)
    ]
    omega = gamma[0] + 2 * sum((1 - k / (lag + 1)) * gamma[k] for k in range(1, lag + 1))
    return {
        "sharpe": sharpe,
        "influence": influence,
        "influence_mean": influence_mean,
        "gamma": gamma,
        "omega": omega,
        "se": math.sqrt(omega / n),
    }


def test_reference_implementation_matches_every_reported_quantity() -> None:
    values = [0.01, -0.02, 0.03, 0.005, -0.01, 0.02]
    expected = reference_hac(values, 2)
    report = estimate_sharpe_hac(values, lag=2, include_adjacent=False)

    assert report.primary.status is HacStatus.OK
    assert report.sensitivity == ()
    assert report.primary.n_observations == len(values)
    assert report.primary.sharpe_ratio == pytest.approx(expected["sharpe"], abs=1e-14)
    assert report.primary.influence_mean == pytest.approx(expected["influence_mean"], abs=1e-14)
    assert report.primary.influence_function == pytest.approx(expected["influence"], abs=1e-14)
    assert report.primary.autocovariances == pytest.approx(expected["gamma"], abs=1e-14)
    assert report.primary.long_run_variance == pytest.approx(expected["omega"], abs=1e-14)
    assert report.primary.standard_error == pytest.approx(expected["se"], abs=1e-14)


def test_alternating_returns_have_closed_form_lag_zero_and_lag_one_errors() -> None:
    values = [-1.0, 1.0, -1.0, 1.0]
    lag_zero = estimate_sharpe_hac(values, lag=0, include_adjacent=False).primary
    assert lag_zero.sharpe_ratio == 0.0
    assert lag_zero.long_run_variance == pytest.approx(1.0)
    assert lag_zero.standard_error == pytest.approx(0.5)

    lag_one = estimate_sharpe_hac(values, lag=1, include_adjacent=False).primary
    assert lag_one.autocovariances == pytest.approx((1.0, -0.75))
    assert lag_one.long_run_variance == pytest.approx(0.25)
    assert lag_one.standard_error == pytest.approx(0.25)

    lag_two = estimate_sharpe_hac(values, lag=2, include_adjacent=False).primary
    assert lag_two.autocovariances == pytest.approx((1.0, -0.75, 0.5))
    assert lag_two.long_run_variance == pytest.approx(1.0 / 3.0)
    assert lag_two.standard_error == pytest.approx(math.sqrt(1.0 / 12.0))


def test_positive_serial_dependence_can_increase_the_hac_standard_error() -> None:
    values = [1.0, 1.0, -1.0, -1.0, 1.0, 1.0, -1.0, -1.0]
    lag_zero = estimate_sharpe_hac(values, lag=0, include_adjacent=False).primary
    lag_one = estimate_sharpe_hac(values, lag=1, include_adjacent=False).primary
    assert lag_one.autocovariances[1] == pytest.approx(0.125)
    assert lag_one.standard_error > lag_zero.standard_error
    assert lag_one.long_run_variance == pytest.approx(1.125)


def test_adjacent_sensitivity_is_reported_without_changing_the_frozen_primary_lag() -> None:
    values = [-1.0, 1.0, -1.0, 1.0]
    report = estimate_sharpe_hac(values, lag=1)
    assert report.primary.lag == 1
    assert [estimate.lag for estimate in report.sensitivity] == [0, 2]
    assert all(estimate.status is HacStatus.OK for estimate in (report.primary, *report.sensitivity))
    assert report.primary.standard_error == pytest.approx(0.25)
    assert report.sensitivity[0].standard_error == pytest.approx(0.5)
    assert report.sensitivity[1].standard_error == pytest.approx(math.sqrt(1.0 / 12.0))


def test_lag_zero_only_has_the_positive_adjacent_bandwidth() -> None:
    report = estimate_sharpe_hac([-1.0, 1.0, -1.0, 1.0], lag=0)
    assert [estimate.lag for estimate in report.sensitivity] == [1]


def test_sharpe_ratio_is_per_period_never_annualized() -> None:
    values = [1.0, 2.0, 3.0, 4.0]
    estimate = estimate_sharpe_hac(values, lag=0, include_adjacent=False).primary
    mean = sum(values) / len(values)
    sigma = math.sqrt(sum((value - mean) ** 2 for value in values) / len(values))
    assert estimate.sharpe_ratio == pytest.approx(mean / sigma)


@pytest.mark.parametrize("values", [[], [1.0]])
def test_insufficient_data_is_an_explicit_status(values: list[float]) -> None:
    estimate = estimate_sharpe_hac(values).primary
    assert estimate.status is HacStatus.INSUFFICIENT_DATA
    assert estimate.standard_error is None


@pytest.mark.parametrize("lag", [-1, 4, 100])
def test_out_of_range_bandwidth_is_rejected(lag: int) -> None:
    estimate = estimate_sharpe_hac([1.0, 2.0, 3.0, 4.0], lag=lag).primary
    assert estimate.status is HacStatus.INVALID_BANDWIDTH


@pytest.mark.parametrize("lag", [0.5, "1", True, None])
def test_non_integer_bandwidth_is_rejected(lag: object) -> None:
    estimate = estimate_sharpe_hac([1.0, 2.0, 3.0, 4.0], lag=lag).primary  # type: ignore[arg-type]
    assert estimate.status is HacStatus.INVALID_BANDWIDTH


@pytest.mark.parametrize("values", [[1.0, float("nan"), 2.0], [1.0, float("inf"), 2.0], [1.0, float("-inf"), 2.0]])
def test_non_finite_returns_are_rejected(values: list[float]) -> None:
    estimate = estimate_sharpe_hac(values).primary
    assert estimate.status is HacStatus.NON_FINITE_INPUT
    assert estimate.standard_error is None


def test_constant_returns_have_zero_variance_not_a_fake_standard_error() -> None:
    estimate = estimate_sharpe_hac([2.0, 2.0, 2.0]).primary
    assert estimate.status is HacStatus.ZERO_VARIANCE
    assert estimate.standard_error is None


def test_a_two_dimensional_input_is_rejected() -> None:
    estimate = estimate_sharpe_hac([[1.0, 2.0], [3.0, 4.0]]).primary
    assert estimate.status is HacStatus.INVALID_INPUT


def test_an_unconvertible_input_is_rejected() -> None:
    estimate = estimate_sharpe_hac([object()]).primary  # type: ignore[list-item]
    assert estimate.status is HacStatus.INVALID_INPUT


def test_a_failed_primary_estimate_has_no_sensitivity_estimates() -> None:
    report = estimate_sharpe_hac([1.0, 1.0, 1.0], lag=0)
    assert report.primary.status is HacStatus.ZERO_VARIANCE
    assert report.sensitivity == ()


def test_a_nonpositive_long_run_variance_is_a_failure_not_a_nan(monkeypatch: pytest.MonkeyPatch) -> None:
    # 故障注入：直接给 Bartlett 汇总提供 gamma0=1, gamma1=-2，使 Omega=0。
    # 它验证的是公开 API 的失败语义，不声称找到了自然产生该协方差结构的收益序列。
    monkeypatch.setattr("py_core.research.hac._autocovariances", lambda centered, lag: (1.0, -2.0))
    estimate = estimate_sharpe_hac([-1.0, 1.0, -1.0, 1.0], lag=1, include_adjacent=False).primary
    assert estimate.status is HacStatus.INVALID_LONG_RUN_VARIANCE
    assert estimate.standard_error is None


def test_degenerate_influence_is_a_failure_not_a_nan(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("py_core.research.hac._autocovariances", lambda centered, lag: (0.0, 0.0))
    estimate = estimate_sharpe_hac([-1.0, 1.0], lag=1, include_adjacent=False).primary
    assert estimate.status is HacStatus.DEGENERATE_INFLUENCE


def test_a_failed_adjacent_bandwidth_keeps_its_own_status(monkeypatch: pytest.MonkeyPatch) -> None:
    real = __import__("py_core.research.hac", fromlist=["_autocovariances"])._autocovariances

    def inject(centered: np.ndarray, lag: int) -> tuple[float, ...]:
        return (1.0, -2.0) if lag == 1 else real(centered, lag)

    monkeypatch.setattr("py_core.research.hac._autocovariances", inject)
    report = estimate_sharpe_hac([-1.0, 1.0, -1.0, 1.0], lag=0)
    assert report.primary.status is HacStatus.OK
    assert len(report.sensitivity) == 1
    assert report.sensitivity[0].status is HacStatus.INVALID_LONG_RUN_VARIANCE


def test_extreme_but_finite_values_fail_cleanly_without_runtime_warnings() -> None:
    estimate = estimate_sharpe_hac([1e308, -1e308], lag=0, include_adjacent=False).primary
    assert estimate.status in {HacStatus.NUMERIC_FAILURE, HacStatus.ZERO_VARIANCE}
    assert estimate.standard_error is None


def test_an_autocovariance_summation_overflow_is_a_numeric_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    def overflow(_: object) -> float:
        raise OverflowError("forced fsum overflow")

    monkeypatch.setattr("py_core.research.hac.math.fsum", overflow)
    estimate = estimate_sharpe_hac([-1.0, 1.0], lag=0, include_adjacent=False).primary
    assert estimate.status is HacStatus.NUMERIC_FAILURE
    assert estimate.standard_error is None


def test_a_positive_long_run_variance_cannot_produce_a_zero_standard_error(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr("py_core.research.hac._autocovariances", lambda centered, lag: (5e-324,))
    estimate = estimate_sharpe_hac([-1.0, 1.0], lag=0, include_adjacent=False).primary
    assert estimate.status is HacStatus.NUMERIC_FAILURE
    assert estimate.standard_error is None
