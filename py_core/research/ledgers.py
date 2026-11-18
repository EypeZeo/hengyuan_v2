"""三个账本与成本模拟（PLAN-03，对应任务 3.6 与 LEDGER-01 的共用事件设计）。

合同（docs/RESEARCH_EXECUTION_PLAN.md §3）：

- **交易资产账**：按资产记录可用数量；买入、卖出、取消延迟与费用补报都是独立
  事件；同一事件重复输入结果不变（幂等）；费用按收费资产如实扣除——买入手续费
  从基础资产扣除后可卖数量相应减少，卖出手续费从报价资产扣除；数量不足即整笔
  拒绝，不产生部分应用。
- **资金流调整绩效账**：以单位净值计算收益与回撤。充值按充值前净值增加份额，
  提款按充值前净值减少份额；资金进出本身不产生投资收益；充值不重置历史高点。
  份额为零时提款结束一个生命周期，再次充值以单位净值 1 重新开始（单独的生命
  周期规则）。
- **所有者 CNY 综合账**：同时展示投资收益与包含外部支出的综合结果；
  ``P_investment = V + W - C``，``P_allin = P_investment - K_outside``。账户内
  已扣除的费用不再计入 ``K_outside``。

三个账本共用同一套事件词汇（统一 ``event_id`` 幂等键、Decimal 数量），模拟与
实盘用同样的事件字段；实盘适配器负责把交易所回报转换成这些事件，账务投影共享
相同的数量与费用规则。汇率折算（非 CNY 计价资产换算成 CNY）由调用方在
``mark`` 之前完成，本模块不做汇率推断。

本模块是纯逻辑：无文件 I/O、无网络、无交易路径。
"""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from enum import StrEnum

__all__ = [
    "AllInCnyAccount",
    "BalanceState",
    "FlowEvent",
    "FlowEventKind",
    "LedgerError",
    "LedgerEvent",
    "LedgerEventKind",
    "PerformanceAccount",
    "TradingAssetAccount",
]


class LedgerError(ValueError):
    """账本合同违约：数量形状、余额不足、幂等键冲突或生命周期非法。"""


def _require_positive(value: Decimal, name: str) -> None:
    if not value.is_finite() or value <= 0:
        raise LedgerError(f"{name} must be a positive finite decimal, got {value}.")


def _require_nonnegative(value: Decimal, name: str) -> None:
    if not value.is_finite() or value < 0:
        raise LedgerError(f"{name} must be a non-negative finite decimal, got {value}.")


# ---------------------------------------------------------------------------
# 交易资产账
# ---------------------------------------------------------------------------


class LedgerEventKind(StrEnum):
    """统一事件词汇（LEDGER-01 共用事件设计的第一版集合）。"""

    DEPOSIT = "deposit"
    WITHDRAWAL = "withdrawal"
    BUY_FILL = "buy_fill"
    SELL_FILL = "sell_fill"
    FEE_ADJUSTMENT = "fee_adjustment"


@dataclass(frozen=True, slots=True)
class LedgerEvent:
    """一次资产变动。

    单资产事件（充值/提款/费用补报）用 ``asset``/``quantity``；成交事件用
    ``asset``/``asset_quantity``（标的资产的数量）与 ``counter_asset``/
    ``counter_quantity``（对手资产的数量：买入付报价资产，卖出收报价资产）。
    ``fee_asset``/``fee_quantity`` 是本次事件的收费资产与数量（可以是 BNB 一类
    第三方收费资产），无费用时两者同为 ``None``。
    """

    event_id: str
    kind: LedgerEventKind
    asset: str
    quantity: Decimal | None = None        # 单资产事件的数量
    asset_quantity: Decimal | None = None  # 成交事件中标的资产的数量
    counter_asset: str | None = None
    counter_quantity: Decimal | None = None
    fee_asset: str | None = None
    fee_quantity: Decimal | None = None


@dataclass(frozen=True, slots=True)
class BalanceState:
    """按资产的可用数量快照（只含有记录的资产；未记录的资产为 0）。"""

    balances: dict[str, Decimal]

    def available(self, asset: str) -> Decimal:
        return self.balances.get(asset, Decimal(0))


def _validate_event(event: LedgerEvent) -> None:
    if not event.event_id.strip():
        raise LedgerError("event_id must be non-empty.")
    if not event.asset.strip():
        raise LedgerError("asset must be non-empty.")
    single_asset = event.kind in (LedgerEventKind.DEPOSIT, LedgerEventKind.WITHDRAWAL, LedgerEventKind.FEE_ADJUSTMENT)
    fill = event.kind in (LedgerEventKind.BUY_FILL, LedgerEventKind.SELL_FILL)
    if single_asset:
        if event.quantity is None:
            raise LedgerError(f"{event.kind.value} requires quantity.")
        _require_positive(event.quantity, "quantity")
        if any(value is not None for value in (event.asset_quantity, event.counter_asset, event.counter_quantity)):
            raise LedgerError(f"{event.kind.value} must not carry fill fields.")
    elif fill:
        if event.asset_quantity is None or event.counter_asset is None or event.counter_quantity is None:
            raise LedgerError(f"{event.kind.value} requires asset_quantity, counter_asset and counter_quantity.")
        _require_positive(event.asset_quantity, "asset_quantity")
        _require_positive(event.counter_quantity, "counter_quantity")
        if event.counter_asset.strip() == event.asset.strip():
            raise LedgerError(f"{event.kind.value} must trade two distinct assets.")
        if event.quantity is not None:
            raise LedgerError(f"{event.kind.value} must not carry the single-asset quantity field.")
    else:
        raise LedgerError(f"unknown event kind {event.kind!r}")
    if (event.fee_asset is None) != (event.fee_quantity is None):
        raise LedgerError("fee_asset and fee_quantity must be given together or not at all.")
    if event.fee_quantity is not None:
        _require_positive(event.fee_quantity, "fee_quantity")
        if event.kind is LedgerEventKind.FEE_ADJUSTMENT:
            raise LedgerError("fee_adjustment carries its fee in quantity, not in fee_* fields.")


class TradingAssetAccount:
    """事件溯源的交易资产账：同一 ``event_id`` 重复输入结果不变。

    幂等语义：相同 ``event_id`` 且相同载荷的重复事件是无害重放（返回 ``False``，
    状态不变）；相同 ``event_id`` 但载荷不同是数据冲突（抛错），静默吞掉第二种
    情况会掩盖真正的重复成交。任何一步余额不足即整笔拒绝，不产生部分应用。
    """

    def __init__(self) -> None:
        self._balances: dict[str, Decimal] = {}
        self._seen: dict[str, LedgerEvent] = {}

    def state(self) -> BalanceState:
        return BalanceState(balances=dict(self._balances))

    def applied_event_ids(self) -> frozenset[str]:
        return frozenset(self._seen)

    def apply(self, event: LedgerEvent) -> bool:
        """应用一个事件；返回是否真正生效（``False`` = 幂等重放）。"""
        _validate_event(event)
        previous = self._seen.get(event.event_id)
        if previous is not None:
            if previous != event:
                raise LedgerError(f"event_id {event.event_id!r} was already applied with a different payload.")
            return False

        working = dict(self._balances)
        if event.kind is LedgerEventKind.DEPOSIT:
            self._credit(working, event.asset, event.quantity)  # type: ignore[arg-type]
        elif event.kind is LedgerEventKind.WITHDRAWAL or event.kind is LedgerEventKind.FEE_ADJUSTMENT:
            self._debit(working, event.asset, event.quantity)  # type: ignore[arg-type]
        elif event.kind is LedgerEventKind.BUY_FILL:
            self._apply_buy(working, event)
        else:
            self._apply_sell(working, event)
        self._balances = working
        self._seen[event.event_id] = event
        return True

    def _apply_buy(self, working: dict[str, Decimal], event: LedgerEvent) -> None:
        self._debit(working, event.counter_asset or "", event.counter_quantity)  # type: ignore[arg-type]
        received = event.asset_quantity  # type: ignore[arg-type]
        if event.fee_asset == event.asset:
            received = received - event.fee_quantity  # type: ignore[operator]
            self._credit(working, event.asset, received)
            return
        self._credit(working, event.asset, received)
        if event.fee_asset is not None and event.fee_quantity is not None:
            self._debit(working, event.fee_asset, event.fee_quantity)

    def _apply_sell(self, working: dict[str, Decimal], event: LedgerEvent) -> None:
        self._debit(working, event.asset, event.asset_quantity)  # type: ignore[arg-type]
        received = event.counter_quantity  # type: ignore[arg-type]
        if event.fee_asset == event.counter_asset:
            received = received - event.fee_quantity  # type: ignore[operator]
            self._credit(working, event.counter_asset or "", received)
            return
        self._credit(working, event.counter_asset or "", received)
        if event.fee_asset is not None and event.fee_quantity is not None:
            self._debit(working, event.fee_asset, event.fee_quantity)

    @staticmethod
    def _credit(balances: dict[str, Decimal], asset: str, quantity: Decimal) -> None:
        balances[asset] = balances.get(asset, Decimal(0)) + quantity

    @staticmethod
    def _debit(balances: dict[str, Decimal], asset: str, quantity: Decimal) -> None:
        available = balances.get(asset, Decimal(0))
        if available < quantity:
            raise LedgerError(f"insufficient {asset}: available {available}, required {quantity}.")
        balances[asset] = available - quantity


# ---------------------------------------------------------------------------
# 资金流调整绩效账
# ---------------------------------------------------------------------------


class FlowEventKind(StrEnum):
    """绩效账的资金流种类：充值与提款本身不产生投资收益。"""

    DEPOSIT = "deposit"
    WITHDRAWAL = "withdrawal"


@dataclass(frozen=True, slots=True)
class FlowEvent:
    """一次资金进出（金额已是记账货币，汇率折算在调用方完成）。"""

    event_id: str
    kind: FlowEventKind
    quantity: Decimal


class PerformanceAccount:
    """资金流调整绩效账：单位净值、份额与回撤。

    - 充值：份额按**充值前**净值增加（``units += D / N``），同时市值增加 ``D``
      （进来的就是现金）；提款：份额按**提款前**净值减少，同时市值减少 ``W``。
      两边同步变动，单位净值在资金进出时保持不变——资金进出本身不产生投资收益；
    - 提走超过自己的份额即拒绝；份额归零结束当前生命周期；
    - 份额为零时充值重新开始一个生命周期：份额 = 金额、市值 = 金额、净值基线 1；
    - 充值与提款都不重置历史高点（``充值不重置历史高点``）；
    - 第一次资金流之前没有净值，提款因此要求先有一次 ``mark()`` 估值。
    """

    def __init__(self) -> None:
        self._units: Decimal = Decimal(0)
        self._valuation: Decimal | None = None
        self._high_water_nav: Decimal = Decimal(1)
        self._seen: dict[str, FlowEvent] = {}
        self._contributions: Decimal = Decimal(0)
        self._withdrawals: Decimal = Decimal(0)

    def units(self) -> Decimal:
        return self._units

    def valuation(self) -> Decimal | None:
        return self._valuation

    def contributions(self) -> Decimal:
        return self._contributions

    def withdrawals(self) -> Decimal:
        return self._withdrawals

    def apply_flow(self, event: FlowEvent) -> bool:
        """应用一笔资金流；返回是否真正生效（``False`` = 幂等重放）。"""
        if not event.event_id.strip():
            raise LedgerError("event_id must be non-empty.")
        _require_positive(event.quantity, "quantity")
        previous = self._seen.get(event.event_id)
        if previous is not None:
            if previous != event:
                raise LedgerError(f"event_id {event.event_id!r} was already applied with a different payload.")
            return False

        if event.kind is FlowEventKind.DEPOSIT:
            if self._units == 0:
                # 新生命周期：净值基线 1，旧生命周期的高点不再压制新回撤。
                self._units = event.quantity
                self._valuation = (self._valuation or Decimal(0)) + event.quantity
                self._high_water_nav = Decimal(1)
            else:
                nav = self._nav()
                self._units += event.quantity / nav
                self._valuation = (self._valuation or Decimal(0)) + event.quantity
            self._contributions += event.quantity
        else:
            nav = self._nav()
            share = event.quantity / nav
            if share > self._units:
                raise LedgerError(f"withdrawal {event.quantity} exceeds the owned share {self._units} at NAV {nav}.")
            self._units -= share
            self._valuation = self._valuation - event.quantity  # 现金离场：市值同步减少
            if self._units == 0:
                self._valuation = None  # 生命周期结束：净值回到未定义
            self._withdrawals += event.quantity
        self._seen[event.event_id] = event
        return True

    def mark(self, valuation: Decimal) -> None:
        """以当前市值给账本估值（非记账货币的资产由调用方先折算）。"""
        _require_nonnegative(valuation, "valuation")
        if self._units > 0:
            self._high_water_nav = max(self._high_water_nav, self._nav_of(valuation))
        self._valuation = valuation

    def nav(self) -> Decimal | None:
        """当前单位净值；无份额或未估值时为 ``None``。"""
        if self._units == 0 or self._valuation is None:
            return None
        return self._nav_of(self._valuation)

    def drawdown(self) -> Decimal | None:
        """当前净值回撤 ``1 - N / max(N)``；无净值时为 ``None``，未创新低时为 0。"""
        nav = self.nav()
        if nav is None:
            return None
        return Decimal(1) - nav / self._high_water_nav

    def _nav(self) -> Decimal:
        nav = self.nav()
        if nav is None:
            raise LedgerError("no NAV available: the account must hold units and a valuation.")
        return nav

    def _nav_of(self, valuation: Decimal) -> Decimal:
        return valuation / self._units


# ---------------------------------------------------------------------------
# 所有者 CNY 综合账
# ---------------------------------------------------------------------------


class AllInCnyAccount:
    """所有者 CNY 综合账：投资收益与包含外部支出的综合结果并排展示。

    ``C`` 是累计投入交易账户的 CNY，``W`` 是累计提取并折算为 CNY 的金额，
    ``K_outside`` 是由账户外支付、尚未计入 ``V`` 的项目费用；账户内已经扣除的
    费用不再计入 ``K_outside``。
    """

    def __init__(self) -> None:
        self._contributions: Decimal = Decimal(0)
        self._withdrawals: Decimal = Decimal(0)
        self._external_costs: Decimal = Decimal(0)
        self._valuation: Decimal | None = None
        self._seen: dict[str, tuple[str, Decimal]] = {}

    def record_deposit(self, event_id: str, amount_cny: Decimal) -> bool:
        return self._record(event_id, "deposit", amount_cny)

    def record_withdrawal(self, event_id: str, amount_cny: Decimal) -> bool:
        return self._record(event_id, "withdrawal", amount_cny)

    def record_external_cost(self, event_id: str, amount_cny: Decimal) -> bool:
        """登记一笔账户外支付的项目费用（服务器、数据订阅等；进 ``K_outside``）。"""
        return self._record(event_id, "external_cost", amount_cny)

    def mark_total_value_cny(self, valuation_cny: Decimal) -> None:
        """以折算后的全部交易资产与闲置资产市值给 ``V`` 估值。"""
        _require_nonnegative(valuation_cny, "valuation_cny")
        self._valuation = valuation_cny

    def summary(self) -> dict[str, Decimal | None]:
        """``P_investment = V + W - C``；``P_allin = P_investment - K_outside``。"""
        if self._valuation is None:
            return {
                "investment_pnl_cny": None,
                "all_in_pnl_cny": None,
                "contributions_cny": self._contributions,
                "withdrawals_cny": self._withdrawals,
                "external_costs_cny": self._external_costs,
            }
        investment = self._valuation + self._withdrawals - self._contributions
        return {
            "investment_pnl_cny": investment,
            "all_in_pnl_cny": investment - self._external_costs,
            "contributions_cny": self._contributions,
            "withdrawals_cny": self._withdrawals,
            "external_costs_cny": self._external_costs,
        }

    def _record(self, event_id: str, kind: str, amount_cny: Decimal) -> bool:
        _require_positive(amount_cny, "amount_cny")
        previous = self._seen.get(event_id)
        if previous is not None:
            if previous != (kind, amount_cny):
                raise LedgerError(f"event_id {event_id!r} was already applied with a different payload.")
            return False
        self._seen[event_id] = (kind, amount_cny)
        if kind == "deposit":
            self._contributions += amount_cny
        elif kind == "withdrawal":
            self._withdrawals += amount_cny
        else:
            self._external_costs += amount_cny
        return True
