"""PLAN-03 三个账本与成本模拟的测试（py_core/research/ledgers.py）。

锚点是规划 §3 自己给出的校验例：
- 交易资产账：价 100、买入金额 100、每边 10bp、买入手续费从基础资产扣除、
  卖出手续费从报价资产扣除，最终所得 99.8001；
- 绩效账：初始 1,000 CNY 得 1,000 份，市值 900 后充值 500 净值仍 0.9，资产上涨
  10% 后市值 1,540、单位净值 0.99、回撤 1%、累计投入 1,500、投资盈亏 40；
- 综合账：P_investment = V + W - C，P_allin = P_investment - K_outside。
"""

from __future__ import annotations

from decimal import Decimal

import pytest

from py_core.research.ledgers import (
    AllInCnyAccount,
    FlowEvent,
    FlowEventKind,
    LedgerError,
    LedgerEvent,
    LedgerEventKind,
    PerformanceAccount,
    TradingAssetAccount,
)

D = Decimal
_ONE = D("1")
_BTC_FEE = D("0.001")
_HUNDRED = D("100")
_ZERO_POINT_ONE = D("0.1")
_NINETY_NINE_POINT_NINE = D("99.9")
_ZERO_POINT_ZERO_NINE_NINE_NINE = D("0.0999")


# ---------------------------------------------------------------------------
# 交易资产账
# ---------------------------------------------------------------------------


def buy(event_id: str, *, base_qty: D = _ONE, quote_qty: D = _HUNDRED, fee_asset: str | None = "BTC",
        fee_qty: D | None = _BTC_FEE) -> LedgerEvent:
    return LedgerEvent(
        event_id=event_id, kind=LedgerEventKind.BUY_FILL, asset="BTC", asset_quantity=base_qty,
        counter_asset="USDT", counter_quantity=quote_qty, fee_asset=fee_asset, fee_quantity=fee_qty,
    )


def sell(event_id: str, *, base_qty: D = _ONE, quote_qty: D = _HUNDRED, fee_asset: str | None = "USDT",
         fee_qty: D | None = _ZERO_POINT_ONE) -> LedgerEvent:
    return LedgerEvent(
        event_id=event_id, kind=LedgerEventKind.SELL_FILL, asset="BTC", asset_quantity=base_qty,
        counter_asset="USDT", counter_quantity=quote_qty, fee_asset=fee_asset, fee_quantity=fee_qty,
    )


def test_the_plans_round_trip_example_lands_on_99_8001() -> None:
    """规划 §3.1 校验例：买入费从基础资产扣、卖出费从报价资产扣 → 99.8001。"""
    account = TradingAssetAccount()
    account.apply(LedgerEvent(event_id="seed", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("100")))
    account.apply(buy("buy-1", base_qty=D("1"), quote_qty=D("100"), fee_asset="BTC", fee_qty=D("0.001")))
    assert account.state().available("BTC") == D("0.999")  # 买入手续费从基础资产扣除，可卖数量相应减少
    account.apply(sell("sell-1", base_qty=D("0.999"), quote_qty=D("99.9"), fee_asset="USDT", fee_qty=D("0.0999")))
    assert account.state().available("USDT") == D("99.8001")


def test_replaying_the_same_event_is_a_no_op_and_a_conflicting_payload_is_an_error() -> None:
    account = TradingAssetAccount()
    assert account.apply(LedgerEvent(event_id="d1", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("100"))) is True
    replay = LedgerEvent(event_id="d1", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("100"))
    assert account.apply(replay) is False
    assert account.state().available("USDT") == D("100")
    with pytest.raises(LedgerError, match="different payload"):
        account.apply(LedgerEvent(event_id="d1", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("200")))


def test_insufficient_balance_rejects_the_whole_event_without_partial_application() -> None:
    account = TradingAssetAccount()
    account.apply(LedgerEvent(event_id="d1", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("50")))
    with pytest.raises(LedgerError, match="insufficient USDT"):
        account.apply(buy("buy-1"))
    assert account.state().available("USDT") == D("50")
    assert account.state().available("BTC") == D("0")


def test_a_buy_fee_paid_in_the_counter_asset_is_charged_on_top() -> None:
    account = TradingAssetAccount()
    account.apply(LedgerEvent(event_id="d1", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("100.1")))
    account.apply(buy("buy-1", fee_asset="USDT", fee_qty=D("0.1")))
    assert account.state().available("BTC") == D("1")
    assert account.state().available("USDT") == D("0")


def test_a_buy_fee_paid_in_a_third_party_asset_is_debited_from_it() -> None:
    account = TradingAssetAccount()
    account.apply(LedgerEvent(event_id="d1", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("100")))
    account.apply(LedgerEvent(event_id="d2", kind=LedgerEventKind.DEPOSIT, asset="BNB", quantity=D("1")))
    account.apply(buy("buy-1", fee_asset="BNB", fee_qty=D("0.01")))
    assert account.state().available("BTC") == D("1")
    assert account.state().available("USDT") == D("0")
    assert account.state().available("BNB") == D("0.99")


def test_a_sell_fee_paid_in_the_base_asset_reduces_the_base_balance_further() -> None:
    account = TradingAssetAccount()
    account.apply(LedgerEvent(event_id="d1", kind=LedgerEventKind.DEPOSIT, asset="BTC", quantity=D("1.002")))
    account.apply(sell("sell-1", base_qty=D("1"), quote_qty=D("100"), fee_asset="BTC", fee_qty=D("0.001")))
    assert account.state().available("BTC") == D("0.001")
    assert account.state().available("USDT") == D("100")


def test_fee_adjustment_deducts_a_late_reported_fee() -> None:
    account = TradingAssetAccount()
    account.apply(LedgerEvent(event_id="d1", kind=LedgerEventKind.DEPOSIT, asset="BTC", quantity=D("1")))
    account.apply(LedgerEvent(event_id="fee", kind=LedgerEventKind.FEE_ADJUSTMENT, asset="BTC", quantity=D("0.001")))
    assert account.state().available("BTC") == D("0.999")


def test_a_withdrawal_is_a_debit_and_every_event_shape_is_enforced() -> None:
    account = TradingAssetAccount()
    account.apply(LedgerEvent(event_id="d1", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("100")))
    account.apply(LedgerEvent(event_id="w1", kind=LedgerEventKind.WITHDRAWAL, asset="USDT", quantity=D("40")))
    assert account.state().available("USDT") == D("60")

    with pytest.raises(LedgerError, match="quantity"):
        account.apply(LedgerEvent(event_id="bad", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("0")))
    with pytest.raises(LedgerError, match="distinct assets"):
        account.apply(LedgerEvent(event_id="bad", kind=LedgerEventKind.BUY_FILL, asset="BTC", asset_quantity=D("1"),
                                  counter_asset="BTC", counter_quantity=D("100")))
    with pytest.raises(LedgerError, match="fee_asset and fee_quantity"):
        account.apply(LedgerEvent(event_id="bad", kind=LedgerEventKind.BUY_FILL, asset="BTC", asset_quantity=D("1"),
                                  counter_asset="USDT", counter_quantity=D("100"), fee_asset="BTC"))
    with pytest.raises(LedgerError, match="fee_quantity"):
        account.apply(LedgerEvent(event_id="bad", kind=LedgerEventKind.BUY_FILL, asset="BTC", asset_quantity=D("1"),
                                  counter_asset="USDT", counter_quantity=D("100"), fee_asset="BTC", fee_quantity=D("0")))
    with pytest.raises(LedgerError, match="fill fields"):
        account.apply(LedgerEvent(event_id="bad", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("1"), counter_asset="USDT"))
    with pytest.raises(LedgerError, match="fee in quantity"):
        account.apply(LedgerEvent(event_id="bad", kind=LedgerEventKind.FEE_ADJUSTMENT, asset="BTC", quantity=D("0.1"),
                                  fee_asset="BTC", fee_quantity=D("0.1")))
    with pytest.raises(LedgerError, match="event_id"):
        account.apply(LedgerEvent(event_id=" ", kind=LedgerEventKind.DEPOSIT, asset="USDT", quantity=D("1")))


# ---------------------------------------------------------------------------
# 资金流调整绩效账
# ---------------------------------------------------------------------------


def deposit(event_id: str, amount: str) -> FlowEvent:
    return FlowEvent(event_id=event_id, kind=FlowEventKind.DEPOSIT, quantity=D(amount))


def withdrawal(event_id: str, amount: str) -> FlowEvent:
    return FlowEvent(event_id=event_id, kind=FlowEventKind.WITHDRAWAL, quantity=D(amount))


def test_the_plans_nav_example_deposit_does_not_change_nav_and_pnl_is_40() -> None:
    account = PerformanceAccount()
    account.apply_flow(deposit("f1", "1000"))
    assert account.units() == D("1000")
    account.mark(D("900"))
    assert account.nav() == D("0.9")

    account.apply_flow(deposit("f2", "500"))  # 充值按充值前净值增加份额：500 / 0.9
    assert account.nav() is not None and account.nav().quantize(D("0.000001")) == D("0.900000")  # 净值仍 0.9（28 位 Decimal 舍入残差量化掉）
    account.mark(D("1540"))  # 资产上涨 10%
    assert account.nav().quantize(D("0.000001")) == D("0.990000")
    assert account.drawdown().quantize(D("0.000001")) == D("0.010000")  # 回撤 1%
    assert account.contributions() == D("1500")  # 累计投入 1,500
    assert account.contributions() + account.withdrawals() - D("0") == D("1500")
    # 投资盈亏 = V - C = 1540 - 1500 = 40
    assert account.nav() is not None and (account.nav() * account.units() - account.contributions()).quantize(D("0.000001")) == D("40.000000")


def test_withdrawal_reduces_shares_at_the_pre_withdrawal_nav() -> None:
    account = PerformanceAccount()
    account.apply_flow(deposit("f1", "1000"))
    account.mark(D("1200"))  # N = 1.2
    account.apply_flow(withdrawal("w1", "600"))  # 提走 600/1.2 = 500 份，现金同步离场
    assert account.units() == D("500")
    assert account.valuation() == D("600")
    assert account.nav() == D("1.2")  # 资金进出本身不产生投资收益
    account.mark(D("600"))
    assert account.nav() == D("1.2")


def test_withdrawing_more_than_the_owned_share_is_refused() -> None:
    account = PerformanceAccount()
    account.apply_flow(deposit("f1", "1000"))
    account.mark(D("800"))  # N = 0.8，份额 1000，可提金额 800
    with pytest.raises(LedgerError, match="exceeds the owned share"):
        account.apply_flow(withdrawal("w1", "900"))
    assert account.units() == D("1000")


def test_a_full_withdrawal_ends_the_lifecycle_and_a_new_deposit_restarts_at_one() -> None:
    account = PerformanceAccount()
    account.apply_flow(deposit("f1", "1000"))
    account.mark(D("2000"))  # N = 2，历史高点 2
    account.apply_flow(withdrawal("w1", "2000"))
    assert account.units() == D("0")
    assert account.nav() is None
    assert account.drawdown() is None

    account.apply_flow(deposit("f2", "500"))
    assert account.units() == D("500")
    account.mark(D("500"))
    assert account.nav() == D("1")
    assert account.drawdown() == D("0")  # 新生命周期不被旧高点压制


def test_flows_never_reset_the_high_water_mark_inside_a_lifecycle() -> None:
    account = PerformanceAccount()
    account.apply_flow(deposit("f1", "1000"))
    account.mark(D("2000"))  # N 高点 2
    account.mark(D("1000"))  # 回撤 50%
    account.apply_flow(deposit("f2", "1000"))  # 充值不重置历史高点
    account.mark(D("2000"))  # N = 1，回撤仍是 50%
    assert account.drawdown() == D("0.5")


def test_flow_idempotency_and_conflicts(tmp_path: object) -> None:
    account = PerformanceAccount()
    assert account.apply_flow(deposit("f1", "1000")) is True
    assert account.apply_flow(deposit("f1", "1000")) is False
    assert account.units() == D("1000")
    with pytest.raises(LedgerError, match="different payload"):
        account.apply_flow(deposit("f1", "2000"))
    with pytest.raises(LedgerError, match="quantity"):
        account.apply_flow(deposit("f0", "0"))
    with pytest.raises(LedgerError, match="event_id"):
        account.apply_flow(FlowEvent(event_id="", kind=FlowEventKind.DEPOSIT, quantity=D("1")))


def test_marking_before_any_deposit_is_legal_and_reads_as_no_nav() -> None:
    account = PerformanceAccount()
    account.mark(D("0"))
    assert account.nav() is None
    assert account.drawdown() is None


# ---------------------------------------------------------------------------
# 所有者 CNY 综合账
# ---------------------------------------------------------------------------


def test_the_all_in_account_reports_investment_and_all_in_pnl() -> None:
    account = AllInCnyAccount()
    account.record_deposit("c1", D("1000"))
    account.record_deposit("c2", D("500"))
    account.record_withdrawal("w1", D("100"))
    account.record_external_cost("k1", D("14"))  # 账户外支付的服务器费用
    account.mark_total_value_cny(D("1540"))
    summary = account.summary()
    assert summary["investment_pnl_cny"] == D("1540") + D("100") - D("1500")  # V + W - C = 140
    assert summary["all_in_pnl_cny"] == D("126")  # 再减 K_outside = 14
    assert summary["contributions_cny"] == D("1500")
    assert summary["withdrawals_cny"] == D("100")
    assert summary["external_costs_cny"] == D("14")


def test_the_all_in_account_is_honest_before_the_first_valuation() -> None:
    account = AllInCnyAccount()
    account.record_deposit("c1", D("1000"))
    account.record_external_cost("k1", D("5"))
    summary = account.summary()
    assert summary["investment_pnl_cny"] is None
    assert summary["all_in_pnl_cny"] is None
    assert summary["contributions_cny"] == D("1000")
    assert summary["external_costs_cny"] == D("5")


def test_all_in_idempotency_and_refusals() -> None:
    account = AllInCnyAccount()
    assert account.record_deposit("c1", D("1000")) is True
    assert account.record_deposit("c1", D("1000")) is False
    with pytest.raises(LedgerError, match="different payload"):
        account.record_deposit("c1", D("2000"))
    with pytest.raises(LedgerError, match="amount_cny"):
        account.record_external_cost("k0", D("0"))
    with pytest.raises(LedgerError, match="valuation_cny"):
        account.mark_total_value_cny(D("-1"))
