"""
策略基类 — 可插拔策略接口 — P2-STRAT-01

策略只负责"给定历史 OHLCV，产出目标仓位/signal 序列"这一件事——不做仓位管理、不做风险
控制（那是 RiskCalculator 的职责，见 py_core/risk/），不做下单（这个仓库里目前没有任何
下单能力）。输出的 signal 会被喂进 run_vectorized_backtest()/run_risk_aware_backtest()，
走既有的 next-bar-open 执行 + 线性成本模型，语义不变。

所有输出均为研究/回测用途，不代表交易信号的实盘授权。
"""

from __future__ import annotations

import importlib
from abc import ABC, abstractmethod
from typing import Any

import numpy as np
import pandas as pd

# load_strategy() 默认只允许加载这个命名空间下的策略模块——这是一个本地单人研究 CLI，不是
# 接受不可信输入的服务，这道限制主要是防误用/防手滑（比如粘贴错了一个 spec 字符串），不是
# 应对恶意输入的安全边界。allow_external=True 显式放开。
_DEFAULT_ALLOWED_PREFIX = "py_core.strategies."


class Strategy(ABC):
    """所有策略的基类。

    子类只需实现 generate_signals()：输入是 records_to_dataframe()（vectorized_engine.py）
    产出的 OHLCV DataFrame，输出必须是与输入同索引对齐的 pd.Series。

    并发契约：Strategy 子类的实例必须是无共享可变状态的一次性调用对象——同一个实例可以被
    反复调用 generate_signals()，但绝不能被多个线程并发调用同一个实例（Python/pandas 不是
    为这个场景设计的）。未来如果要做参数扫描并行化，应该用多进程 + 每个任务自己的独立实例，
    不能共享一个实例跨线程池调用。
    """

    @abstractmethod
    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        """给定 OHLCV DataFrame，产出对齐的 signal/目标仓位 Series。

        Args:
            df: OHLCV DataFrame，DatetimeIndex，columns = [open, high, low, close, volume]，
                按时间升序（records_to_dataframe() 的输出形状）。

        Returns:
            与 df.index 完全一致（不只是等长）的 pd.Series，值 ∈ {0.0, 1.0} 或实数仓位
            （不支持负值/做空——risk_integration.py 的 _signal_value_to_target() 只认
            long/none）。warm-up 区间（例如均线还没攒够窗口长度）可以返回 NaN——下游
            reindex().fillna(0.0) 会把 NaN 当成空仓处理，不需要策略自己填充。

        不做反 lookahead 校验——那是实现者的责任（用 .rolling()/.shift() 而不是直接访问
        未来的 index）。本模块的 assert_no_lookahead() 是一个可选的事后核验工具，不是
        generate_signals() 自动享有的保护。
        """
        raise NotImplementedError


def validate_signal_output(signal: pd.Series[Any], df: pd.DataFrame) -> None:
    """校验 generate_signals() 的返回值形状合法，不合法就抛错，不静默放行。

    校验规则：
    - 非 None，且是 pd.Series（不是 DataFrame/ndarray/list）。
    - 一维（pd.Series 恒真，仍显式检查 ndim 防御性）。
    - 索引是 DatetimeIndex，且与 df.index 完全一致（不只是等长——顺序/取值都要一样）。
    - 索引无重复。
    - dtype 是数值型。
    - 不含 +inf/-inf。**NaN 明确允许**——这是 warm-up 区间的合法产出，跟
      run_vectorized_backtest() 自己 reindex().fillna(0.0) 的既有容忍度一致。

    刻意不校验取值范围（例如卡死到 [0, 1]）——Strategy.generate_signals() 的文档本来就
    允许"实数仓位"，这是一个刻意的设计决定，不是遗漏。

    Raises:
        TypeError: signal 不是 pd.Series，或索引不是 DatetimeIndex，或 dtype 非数值。
        ValueError: 索引跟 df.index 不一致、索引有重复、或含 +inf/-inf。
    """
    if signal is None:
        raise TypeError("generate_signals() 不能返回 None")
    if not isinstance(signal, pd.Series):
        raise TypeError(f"generate_signals() 必须返回 pd.Series，实际返回 {type(signal).__name__}")
    if signal.ndim != 1:
        raise ValueError(f"signal 必须是一维 Series，实际 ndim={signal.ndim}")
    if not isinstance(signal.index, pd.DatetimeIndex):
        raise TypeError("signal 的索引必须是 DatetimeIndex")
    if signal.index.has_duplicates:
        raise ValueError("signal 的索引存在重复值")
    if not signal.index.equals(df.index):
        raise ValueError("signal 的索引必须跟 df.index 完全一致（不只是等长）")
    if not pd.api.types.is_numeric_dtype(signal.dtype):
        raise TypeError(f"signal 的 dtype 必须是数值型，实际是 {signal.dtype}")
    if np.any(np.isinf(signal.to_numpy(dtype=float))):
        raise ValueError("signal 不能包含 +inf/-inf（NaN 允许，代表 warm-up 区间的合法空仓）")


def assert_no_lookahead(strategy: Strategy, df: pd.DataFrame, sample_points: list[int]) -> None:
    """可选的因果性（反 lookahead）核验工具，不是框架强制的关卡。

    在 sample_points 给定的若干整数位置上，用「只截到该点为止的历史前缀」（df.iloc[:point+1]）
    重新跑一遍 generate_signals()，对比该点的信号是否跟用全量 df 算出来的一致——如果不一致，
    说明策略在该点用到了截断之后才存在的数据（未来数据），generate_signals() 本身没有任何
    机制自动阻止这种情况，需要策略作者自己在测试里调用这个函数核验。

    Args:
        strategy: 待核验的策略实例。
        df: 完整的 OHLCV DataFrame。
        sample_points: 要核验的整数位置列表（0-based，对应 df.iloc 的位置，不是时间戳）。

    Raises:
        ValueError: df 为空、sample_points 中有越界位置、或某个采样点的信号不一致
            （因果性核验失败）。
    """
    if df.empty:
        raise ValueError("df 不能为空")

    full_signal = strategy.generate_signals(df)
    validate_signal_output(full_signal, df)

    for point in sample_points:
        if point < 0 or point >= len(df):
            raise ValueError(f"sample_points 中的位置 {point} 超出 df 范围 [0, {len(df) - 1}]")

        truncated_df = df.iloc[: point + 1]
        truncated_signal = strategy.generate_signals(truncated_df)
        validate_signal_output(truncated_signal, truncated_df)

        full_value = full_signal.iloc[point]
        truncated_value = truncated_signal.iloc[-1]

        both_nan = pd.isna(full_value) and pd.isna(truncated_value)
        if not both_nan and full_value != truncated_value:
            raise ValueError(
                f"因果性核验失败：位置 {point}（{df.index[point]}）用全量数据算出的信号"
                f"（{full_value!r}）跟只用历史前缀算出的信号（{truncated_value!r}）不一致——"
                "策略的 generate_signals() 可能用到了截断点之后才存在的数据。"
            )


def load_strategy(spec: str, *, allow_external: bool = False, **params: Any) -> Strategy:
    """按 "module.path:ClassName" 形式动态加载并实例化一个策略。

    不建注册表——用户可以在 py_core.strategies 命名空间之外的任意可 import 路径写自己的
    策略，不需要改这个包的任何文件（需要显式传 allow_external=True）。

    Args:
        spec: 形如 "py_core.strategies.sma_crossover:SmaCrossoverStrategy"。
        allow_external: 默认 False，只允许加载 "py_core.strategies." 命名空间下的模块。
            设为 True 放开限制。
        **params: 传给策略构造函数的关键字参数。

    Returns:
        Strategy 实例。

    Raises:
        ValueError: spec 格式不合法（缺少 ":"，或冒号前后为空）。
        PermissionError: module_path 不在允许的命名空间下，且 allow_external=False。
        ImportError / AttributeError: 模块或类不存在。
        TypeError: 目标类不是 Strategy 子类，或构造参数不匹配。
    """
    if ":" not in spec:
        raise ValueError(f'strategy spec 必须是 "module.path:ClassName" 形式，收到: {spec!r}')
    module_path, class_name = spec.rsplit(":", 1)
    if not module_path or not class_name:
        raise ValueError(f'strategy spec 的模块路径和类名都不能为空，收到: {spec!r}')

    if not allow_external and not module_path.startswith(_DEFAULT_ALLOWED_PREFIX):
        raise PermissionError(
            f"策略模块 {module_path!r} 不在 {_DEFAULT_ALLOWED_PREFIX!r} 命名空间下——默认"
            "只允许加载仓库自带的策略，仓库外的策略需要显式传 allow_external=True"
            "（CLI 对应 --allow-external-strategy）。"
        )

    module = importlib.import_module(module_path)
    cls = getattr(module, class_name)
    if not (isinstance(cls, type) and issubclass(cls, Strategy)):
        raise TypeError(f"{spec} 不是 Strategy 的子类")
    instance = cls(**params)
    return instance
