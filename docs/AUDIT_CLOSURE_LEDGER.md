# 审计缺陷关闭清单（GATE-01 批 1）

<!-- ledger-meta baseline=f5ce782 -->
<!-- ledger-meta adjudicated=2026-10-07 -->

**范围**：蓝图 `4.2` 的 17 项上游审计缺陷与 `4.3` 的 9 项补充审计（共 26 项）。`4.4` 的 5 项暂缓台账是其中 P2-006、P2-007、P2-008、P3-001、P3-002 的解冻条件视图，随来源项裁定。蓝图第 7 卷的 45 条故障用例属批 2。
**基线**：主干 `f5ce782`（2026-10-07，GATE-01 批 1 开工时的主干）。
**权威性**：本文件是蓝图 `4.2`、`4.3`、`4.4` 状态词的逐项复验记录。蓝图表中的状态词、BKL 台账状态与 FI 状态必须与本文件一致，由 `tools/closure_ledger_check.py` 机械核对；二者不一致时先改本文件的证据，再改蓝图。

核对命令（Windows PowerShell 与 bash 均可）：

```bash
python tools/closure_ledger_check.py
python tools/closure_ledger_check.py --online
python tools/closure_ledger_check.py --self-test
```

## 1 裁定规则

### 1.1 状态词与六段证据链

状态词沿用蓝图 `4.1.1`（`REMEDIATING`、`DEFERRED`、`PARTIAL`、`CLOSED`），六段证据链沿用 `4.1.2`：Code → Contract → Fault → Test → Runtime → Budget。每一段只有三种判定：

| 判定 | 含义 |
| :---: | :--- |
| `PRESENT` | 该段证据完整成立，且全部引用可被机械核对 |
| `NA` | 该缺陷的修复不经过这一段所指的路径；必须写明理由，且只有下表允许的类别才可使用 |
| `MISSING` | 证据不完整或不存在；必须写明缺的是什么。部分满足也记 `MISSING`，已有的部分以引用列出 |

规则：**`CLOSED` 当且仅当六段均无 `MISSING`**，工具双向核对——缺段的不得标 `CLOSED`，六段齐全的不得停在未关闭。`CLOSED` 必须引用至少一个已合并 PR，且不得再带“缺陷仍在”的引用；未关闭的项必须带至少一个“缺陷仍在”的引用（`open:` 或 `absent:`），修复落地删掉对应符号的那天，核对就会失败，迫使本文件与蓝图同步前移。

### 1.2 缺陷类别与可使用 `NA` 的段

| 类别 | 含义 | 可 `NA` 的段 | Runtime 段凭据 |
| :---: | :--- | :--- | :--- |
| `live` | 触及外部 I/O、时钟、崩溃恢复或资金路径 | 无 | 须有真实、测试网或崩溃恢复运行日志（`log:` 引用） |
| `lib` | 纯库逻辑，行为由确定性测试完全覆盖 | Budget | 一次真实成功的 CI run（`run:` 引用） |
| `type` | 靠类型或编译期封闭 | Fault、Runtime、Budget | 无要求 |
| `build` | 构建与 CI 配置 | Fault、Budget | 一次真实成功的 CI run（`run:` 引用） |
| `doc` | 纯文档或纯文本配置，无执行路径 | Fault、Test、Runtime、Budget | 无要求 |

Code 与 Contract 两段永不可 `NA`。仓库至今没有任何真实或测试网运行日志（`CLAUDE.md` 的“当前诚实状态”），所以 `live` 类缺陷现阶段不可能被关闭——这是规则的结果，不是对个别项的判断。

### 1.3 Fault 段的分级（待 Owner 确认的解释）

`4.1.2` 写“Fault 段指该缺陷对应的故障注入用例编号及其执行结果”，而注册表 45 条里没有一条已通过，较早关闭的缺陷（SUPP 类）也不对应任何 FI 编号。本文件的做法：

- `PRESENT:FI`——引用 FI 编号且该用例已通过。目前没有任何项能达到这一级。
- `PRESENT:UT`——没有 FI 编号可引时，引用一个**命名的 L0 负向测试**，它把缺陷条件注入进去并断言被拒；本仓库自己的 FI 用例也把 L0 单元测试当证据产物（FI-032、FI-037）。
- 这是对 `4.1.2` 的一个解释，不是修改；它由 Owner 在合并本 PR 时确认。若要求只认 FI 编号，用 `--strict-fault` 运行核对工具，会列出全部依赖 `UT` 级证据的项，这些项随之降为 `PARTIAL`。

### 1.4 门禁结果

`CLOSED` → `CLEARED`；`DEFERRED` → `DEFERRED`（带解冻条件的显式暂缓，不阻塞其门禁）；`REMEDIATING` 与 `PARTIAL` → `BLOCKED`。

### 1.5 引用记法

引用以 `种类:路径#内容` 书写，由工具逐条解析。符号引用按整词匹配，`A::B` 要求 B 出现在 A 之后；带引号的内容按字面匹配。

| 引用 | 含义 | 核对 |
| :--- | :--- | :--- |
| `code:` `test:` | 现行代码符号、测试用例 | 文件存在，符号存在 |
| `open:` | 缺陷位置，声明“仍在” | 文件存在，内容仍在；消失即失败 |
| `absent:` | 声明“不存在”的东西（路径可用通配符） | 内容不得出现 |
| `countN:` | 声明某内容在文件中恰好出现 N 次 | 次数必须相等 |
| `doc:` `bp:` `frozen:` | 规格文字、蓝图文字、冻结原文（v2.5.6）里的旧标识 | 文字存在 |
| `ignored:` | `.gitignore` 必须忽略的路径 | `git check-ignore` |
| `pr:N@SHA` | 已合并 PR 及其合并提交前缀 | 合并提交在 `HEAD` 历史中；`--online` 再查 GitHub 的合并状态与提交前缀 |
| `run:ID@SHA/文件[作业]` | CI run、被测提交、workflow 文件与可选作业名 | `--online`：必须 `completed` 且 `success`，提交与 workflow 相符；被取消、失败、未结束的 run 一律不是证据 |
| `fi:FI-nnn=状态` | 故障用例及其在蓝图注册表中的状态 | 必须与蓝图第 7 卷一致 |
| `log:` | 真实运行日志文件 | 文件存在 |

## 2 总览

下表由 `python tools/closure_ledger_check.py --emit-overview` 生成，核对工具会比对它与明细是否一致。

<!-- overview:begin -->
| 审计 ID | 等级 | 类别 | 状态 | 阻塞门禁 | 门禁结果 | 缺失段 |
| :---: | :---: | :---: | :---: | :--- | :---: | :--- |
| **P0-001** | CRITICAL | live | REMEDIATING | 实盘订单提交 | BLOCKED | code, fault, test, runtime, budget |
| **P1-001** | HIGH | lib | REMEDIATING | 真实网络调用 | BLOCKED | code, fault, test, runtime |
| **P1-002** | HIGH | live | REMEDIATING | 订单生命周期 | BLOCKED | code, fault, test, runtime, budget |
| **P1-003** | HIGH | lib | REMEDIATING | 账户数据解析 | BLOCKED | code, fault, test, runtime, budget |
| **P1-004** | HIGH | lib | PARTIAL | 策略回测准入 | BLOCKED | contract |
| **P1-005** | HIGH | lib | REMEDIATING | 数据质量门禁 | BLOCKED | code, fault, test, runtime, budget |
| **P2-001** | MEDIUM | lib | REMEDIATING | REST 通信门禁 | BLOCKED | code, fault, test, runtime, budget |
| **P2-002** | MEDIUM | lib | REMEDIATING | REST 通信门禁 | BLOCKED | code, fault, test, runtime |
| **P2-003** | MEDIUM | lib | REMEDIATING | 下单状态转换 | BLOCKED | code, fault, test, runtime |
| **P2-004** | MEDIUM | live | REMEDIATING | 实盘发单风控 | BLOCKED | code, fault, test, runtime, budget |
| **P2-005** | MEDIUM | lib | REMEDIATING | 订单跟踪器 | BLOCKED | code, fault, test, runtime |
| **P2-006** | MEDIUM | lib | DEFERRED | 鲁棒性优化 | DEFERRED | code, contract, fault, test, runtime, budget |
| **P2-007** | MEDIUM | build | PARTIAL | CI 增强 | BLOCKED | code, test |
| **P2-008** | MEDIUM | lib | PARTIAL | 平台安全测试 | BLOCKED | test, runtime |
| **P2-009** | MEDIUM | build | REMEDIATING | 全项目研发准入 | BLOCKED | code, test, runtime |
| **P3-001** | LOW | lib | DEFERRED | 日志健壮性 | DEFERRED | code, contract, fault, test, runtime, budget |
| **P3-002** | LOW | build | DEFERRED | 供应链安全 | DEFERRED | code, contract, test, runtime |
| **SUPP-001** | MEDIUM | build | CLOSED | 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |
| **SUPP-002** | MEDIUM | type | CLOSED | 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |
| **SUPP-003** | MEDIUM | lib | CLOSED | 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |
| **SUPP-004** | HIGH | lib | CLOSED | 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |
| **SUPP-005** | HIGH | type | CLOSED | 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |
| **SUPP-006** | LOW | doc | CLOSED | 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |
| **SUPP-007** | LOW | doc | CLOSED | 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |
| **SUPP-008** | LOW | doc | CLOSED | 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |
| **SUPP-009** | MEDIUM | lib | CLOSED | 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |

| 门禁 | 结果 | 未关闭或暂缓的项 |
| :--- | :---: | :--- |
| 实盘订单提交 | BLOCKED | P0-001 |
| 真实网络调用 | BLOCKED | P1-001 |
| 订单生命周期 | BLOCKED | P1-002 |
| 账户数据解析 | BLOCKED | P1-003 |
| 策略回测准入 | BLOCKED | P1-004 |
| 数据质量门禁 | BLOCKED | P1-005 |
| REST 通信门禁 | BLOCKED | P2-001, P2-002 |
| 下单状态转换 | BLOCKED | P2-003 |
| 实盘发单风控 | BLOCKED | P2-004 |
| 订单跟踪器 | BLOCKED | P2-005 |
| 鲁棒性优化 | DEFERRED | P2-006 |
| CI 增强 | BLOCKED | P2-007 |
| 平台安全测试 | BLOCKED | P2-008 |
| 全项目研发准入 | BLOCKED | P2-009 |
| 日志健壮性 | DEFERRED | P3-001 |
| 供应链安全 | DEFERRED | P3-002 |
| 无（蓝图 4.3 不登记阻塞门禁） | CLEARED | — |

状态计数：REMEDIATING 11，DEFERRED 3，PARTIAL 3，CLOSED 9，合计 26。
<!-- overview:end -->

## 3 复验发现（与蓝图原文不一致之处）

1. **P1-004 由 `REMEDIATING` 改判 `PARTIAL`。** SAFE-05（PR #146，合并提交 `68c393a07`）已合入，Code、Fault（UT 级）、Test、Runtime、Budget 五段齐备。缺的是 Contract 段里的“准入合同”：预计算信号只得 `index_only`，要求实验登记必须带 `causal_check == "prefix_differential"` 属于 D1，目前不存在。`REMEDIATING`（修复未完成）已不准确，`PARTIAL` 才准确；关闭条件是 D1 的准入合同落地。
2. **P2-008 由 `DEFERRED` 改判 `PARTIAL`。** 蓝图写“待编写平台专有测试”，但 `OpenDirectory.RejectsDirectoryJunction`（`native/tests/test_windows_native_io.cpp`）已经用真实 NTFS 目录联接断言 `open_directory` 返回 `RejectedReparsePoint`，`compaction_lease.hpp` 据此拒绝租约。缺口有两处：该测试在 `#ifdef _WIN32` 之内，只在 Windows 编译，而 CI 没有任何 Windows 作业，所以它从未在 CI 里执行；租约层与控制面层没有端到端的联接测试。BKL-003 随之改为 `PARTIAL`。
3. **SUPP-003、SUPP-005 的描述更正（状态不变）。** 冻结原文把 `L4-LOCK-ORDER-003` 写成“ClockSync 内部锁”、把 `L4-BINDING-REF-005` 写成“pybind11 绑定”。PR #60 在这两个标签下实际修的是凭据缓冲区必须先加锁再写入密钥（`binance_signer.hpp`、`binance_environment.hpp`），以及 `EnvironmentBinding` 的引用悬挂（`binance_environment.hpp`）。本仓库的历史里没有出现过 pybind11 绑定源文件。顺带观察（不在本次裁定范围）：`ClockOffsetPublisher` 是单互斥量、无嵌套加锁，`test_binance_clock_sync.cpp` 没有并发用例，冻结原文所写“并发压力测试通过”找不到对应的测试。
4. **SUPP-006、SUPP-007 的归属更正（状态不变）。** PR #60 对 L4 规格的改动只有标题与状态行（11 行补丁），冻结原文所写“PR #60 文档校准”不成立。现行规格与代码在可核对的细节上一致（TTL、`recvWindow`、发布机制、非保留字符集）：字符集规则由 PR #5 引入，发布机制的措辞由 PR #8 引入。
5. **冻结原文的“PR #60（`25528c3`）”哈希是错的。** `25528c3` 是 PR #61 的合并提交；PR #60 的合并提交是 `b1107a291`。
6. **P3-002 的描述更正（状态不变）。** `py_core/requirements.txt` 是 `==` 精确钉死，只是没有哈希；浮动区间在 `py_core/pyproject.toml` 与 `recorder/pyproject.toml` 的声明里。GitHub Actions 全部按主版本标签引用，Dependabot 只监控 `github-actions`。
7. **P0-001 的现状。** 真实 POST 适配器只被两个不调用它的程序装配：`live_submit_real_credentials_demo.cpp` 装配后“刻意从不 `.call()`”，`live_submit_preflight_harness.cpp` 装配后不使用；`orchestrate_submit` 没有任何可运行的调用者。
8. **Sanitizer 凭据。** run `37258555091`（`CI Native Sanitizers`，定时触发，提交 `d401ed78e`）三个作业全部成功：ASan+UBSan 28.5 分钟（上限 90）；TSan 1.0 分钟（上限 25，只构建十个并发目标，构建 25 秒、并发测试 3 秒，随后两个负控步骤均成功，即 TSan 报出了被注入的竞争）；ARM64 1.4 分钟。同一 workflow 的 run `35556436339`、`34801501343`、`34078573391` 是被取消的，`36810545639` 是失败的（TSan 负控当时依赖调度）；它们不是证据，`--online` 会先用这四个真实 run 证明核对工具拒收它们。

## 4 明细

每项一个 `ledger` 块；`seg.*` 行是六段判定，`--` 之后是说明。

### 4.1 上游审计（蓝图 4.2）

```ledger
id: P0-001
legacy: P0-001
title: 实盘发单编排器：真实订单提交、未知状态对账与全局限流
severity: CRITICAL
kind: live
status: REMEDIATING
task: 1A.1 至 1A.3
gate: 实盘订单提交
gate_result: BLOCKED
open: open:native/src/live_submit_real_credentials_demo.cpp#"deliberately never .call()'d" open:native/src/live_submit_preflight_harness.cpp#real_submit_port_unused
refs: pr:71@8ea914030 pr:72@24dd06f24 pr:119@ee97a59ff pr:120@3e2bd8da0 pr:121@8fa47bab2
seg.code: MISSING code:native/include/hengyuan/binance_private_rest.hpp#submit_order code:native/include/hengyuan/binance_submit_adapter.hpp#make_binance_submit_port code:native/include/hengyuan/live_submit_orchestrator.hpp#orchestrate_submit code:native/include/hengyuan/spot_rate_limit_budget.hpp#PartitionedRateBudget -- 真实 POST（PR #72）、限流分道（PR #71）、未知状态隔离判据（PR #119 至 #121）均已合入；缺口是没有任何可运行进程调用 orchestrate_submit，真实端口只被装配、从不调用
seg.contract: PRESENT doc:docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md#SubmitPort doc:docs/SPEC_INVARIANTS.md#orchestrate_submit -- 规格 rev 73 与不变量台账已定义
seg.fault: MISSING fi:FI-001=PARTIAL fi:FI-002=NOT_TESTABLE_YET fi:FI-003=NOT_TESTABLE_YET fi:FI-004=PARTIAL fi:FI-005=NOT_TESTABLE_YET fi:FI-006=PARTIAL fi:FI-007=PARTIAL -- FI-001 至 007 无一项执行通过，其中 3 项尚不可测
seg.test: MISSING test:native/tests/test_live_submit_orchestrator.cpp#SubmitTimeoutAmbiguous test:native/tests/test_binance_submit_adapter.cpp#RealSubmitDispatchThroughCompositeAdapterSignsAndParses test:native/tests/test_spot_rate_limit_budget.cpp#LaneIsolationStrategyExhaustionDoesNotAffectOthers -- 现有单元与桩测试只覆盖 mock 与注入路径，不覆盖 FI-002、FI-003、FI-005 的场景
seg.runtime: MISSING -- 仓库内没有任何真实或测试网提交的运行日志，也没有崩溃恢复实验日志
seg.budget: MISSING -- 没有对真实 POST 路径的时延与请求配额的量化测量，限流分道只有单元级验证
closure: 1A.1 至 1A.3 完成并经 LIVE-01 在测试网取得运行日志后复审；FI-001 至 007 通过
```

```ledger
id: P1-001
legacy: P1-001
title: 私有 REST 配置暴露可注入主机与自签 CA（行情快照与 K 线客户端另有同名字段）
severity: HIGH
kind: lib
status: REMEDIATING
task: SAFE-01
gate: 真实网络调用
gate_result: BLOCKED
open: open:native/include/hengyuan/binance_private_rest.hpp#PrivateRestConfig::extra_trusted_ca_pem_path open:native/include/hengyuan/binance_private_rest.hpp#PrivateRestConfig::connect_host_override open:native/include/hengyuan/binance_klines_rest.hpp#PublicRestConfig::extra_trusted_ca_pem_path open:native/include/hengyuan/binance_rest_snapshot.hpp#RestSnapshotConfig::extra_trusted_ca_pem_path
seg.code: MISSING -- 三个结构体仍暴露额外信任 CA 路径，私有 REST 配置还暴露连接主机覆盖
seg.contract: PRESENT doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#EnvironmentBinding bp:"任务 SAFE-01" -- 环境一次绑定且不可变；SAFE-01 规定三处一并收口为仅测试接缝类型可注入
seg.fault: MISSING fi:FI-032=PARTIAL -- 用例已登记但无实现，字段仍在
seg.test: MISSING -- 尚无“生产类型不暴露该字段”的单测
seg.runtime: MISSING -- 无 CI run 可引：修复尚未落地
seg.budget: NA -- 删除字段不增加运行期成本
closure: SAFE-01 完成，FI-032 的单测通过，且三个结构体同时收口
```

```ledger
id: P1-002
legacy: P1-002
title: 订单跟踪器对账终态只写内存环形缓冲即释放槽位（用户数据流排空函数同形）
severity: HIGH
kind: live
status: REMEDIATING
task: SAFE-03
gate: 订单生命周期
gate_result: BLOCKED
open: open:native/include/hengyuan/order_tracker.hpp#"AuditRingSink* audit," open:native/include/hengyuan/binance_user_data_event.hpp#"AuditRingSink* audit," absent:native/include/hengyuan/order_tracker.hpp#DurableAuditSink
refs: code:native/include/hengyuan/order_tracker.hpp#drain_reconcile_events code:native/include/hengyuan/binance_user_data_event.hpp#drain_user_data_events code:native/include/hengyuan/durable_audit_sink.hpp#DurableAuditSink
seg.code: MISSING -- drain_reconcile_events 把终态记录 append 到内存环形缓冲 AuditRingSink* 后立即释放槽位；drain_user_data_events 同样只写这个内存环形缓冲（它持有的是 const 注册表，不释放槽位）；持久接收器 DurableAuditSink 不在这两条路径上，终态记录从未进入持久 sink
seg.contract: PRESENT doc:docs/SPEC_INVARIANTS.md#drain_reconcile_events bp:"任务 SAFE-03" -- 规格与任务已定义“先持久确认、再释放”
seg.fault: MISSING fi:FI-031=NOT_TESTABLE_YET -- 前置阻塞是终态记录尚无持久出口
seg.test: MISSING test:native/tests/test_order_tracker.cpp#ExchangeFinalReleasesSlotAndAudits -- 现有 DrainEventsTest 只验证内存审计与槽位释放，不涉及持久确认与崩溃恢复
seg.runtime: MISSING -- 无崩溃重启恢复日志
seg.budget: MISSING -- 持久确认进入对账路径后的时延与 fsync 成本未测量
closure: SAFE-03 完成；FI-031 的崩溃重启恢复日志取得
```

```ledger
id: P1-003
legacy: P1-003
title: 账户真值快照：资产计数越界、无界字符串长度、余额相加未检查、未来时间戳不被拒绝
severity: HIGH
kind: lib
status: REMEDIATING
task: SAFE-02
gate: 账户数据解析
gate_result: BLOCKED
open: open:native/include/hengyuan/account_truth.hpp#"std::size_t asset_count{0};" open:native/include/hengyuan/account_truth.hpp#"for (std::size_t i = 0; i < asset_count; ++i)" open:native/include/hengyuan/account_truth.hpp#"std::int64_t total() const noexcept { return free_ticks + locked_ticks; }"
seg.code: MISSING code:native/include/hengyuan/account_truth.hpp#AccountSnapshot -- 资产计数公开可写，find() 按计数循环而不钳制，total() 直接相加；安全工厂不存在
seg.contract: PRESENT doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#AccountSnapshot bp:"任务 SAFE-02" -- 任务规定私有化数组与计数、仅经安全工厂构造
seg.fault: MISSING fi:FI-033=PARTIAL -- 模糊测试用例已登记，工厂不存在故无法执行
seg.test: MISSING test:native/tests/test_account_truth.cpp#NotionalOverflowFailsClosed test:native/tests/test_account_truth.cpp#AddDetectsOverflow -- 溢出检查只存在于预检算术，不覆盖快照构造
seg.runtime: MISSING -- 无 CI run 可引：修复尚未落地
seg.budget: MISSING -- 工厂校验对解析路径的时延未测量
closure: SAFE-02 完成，FI-033 的模糊测试报告取得
```

```ledger
id: P1-004
legacy: P1-004
title: 回测命令行与滚动验证放行未来函数策略
severity: HIGH
kind: lib
status: PARTIAL
task: SAFE-05
gate: 策略回测准入
gate_result: BLOCKED
open: open:py_core/backtests/vectorized_engine.py#'causal_check = "index_only"' open:py_core/strategies/base.py#"experiment_id: str | None = None"
refs: pr:146@68c393a07 code:py_core/strategies/base.py#CausalEvidence test:py_core/tests/test_safe05_causal_gate.py#test_cli_run_labels_a_precomputed_signals_file_index_only
seg.code: PRESENT code:py_core/strategies/base.py#checked_signals code:py_core/strategies/base.py#LookaheadBiasError code:py_core/backtests/vectorized_engine.py#validate_inputs -- 6 个取信号入口全部改走受检入口，引擎在重排之前校验原始索引，解析期拒绝 lag.n < 1
seg.contract: MISSING doc:docs/STRATEGY_SPEC.md#checked_signals bp:"任务 SAFE-05" -- STRATEGY_SPEC 2.3 定义了门禁与覆盖边界；但“实验登记必须带 causal_check == prefix_differential、预计算信号不得入登记”的准入合同属于 D1，尚不存在，所以预计算信号仍能以 index_only 跑完回测
seg.fault: PRESENT:UT test:py_core/tests/test_safe05_causal_gate.py#test_gate_rejects_each_realistic_leak test:py_core/tests/test_safe05_causal_gate.py#test_cli_run_refuses_a_peeking_strategy test:py_core/tests/test_safe05_causal_gate.py#test_walk_forward_rejects_a_peeking_strategy_and_accepts_an_honest_one -- 偷看下一根收盘、全样本归一化等泄露在每个入口都被拒；变异检查 17 个杀死 16 个，唯一幸存者是等价变异（PR #146 正文）
seg.test: PRESENT test:py_core/tests/test_safe05_causal_gate.py#test_no_production_code_calls_generate_signals_outside_the_gate test:py_core/tests/test_safe05_causal_gate.py#test_engine_rejects_a_signal_index_shifted_by_a_day test:py_core/tests/test_safe05_causal_gate.py#test_schema_rejects_a_lag_that_does_not_look_back -- 37 个用例（含一条 AST 测试钉死“生产代码不绕过受检入口”），py_core 全量 691 → 728 通过，ruff 为零
seg.runtime: PRESENT run:37618152776@68c393a07/ci-python.yml -- 合并提交上的 CI Python 全绿；同一探针在 2000 根小时线上的前后对比见 PR #146 正文（前：偷看策略夏普 49.67 且 is_valid=True；后：LookaheadBiasError）
seg.budget: PRESENT pr:146@68c393a07 -- 实测开销见 PR #146 正文：20000 根 K 线下单次求值 157 ms，门禁 k=8 为 0.80 s（5.1 倍）、k=16 为 1.44 s（9.2 倍）；流水线 walk-forward 0.83 → 1.00 s，PBO 35.6 → 36.4 s，CPCV 4.8 → 5.0 s
closure: D1 的实验登记强制 causal_check == prefix_differential（预计算信号不得入登记）；覆盖边界（仅采样 bar、0/1 信号可能漏检）写入准入合同
```

```ledger
id: P1-005
legacy: P1-005
title: 公共 REST 行情客户端跨页连续性与请求起点过滤、回填报告缺连续性字段
severity: HIGH
kind: lib
status: REMEDIATING
task: SAFE-06
gate: 数据质量门禁
gate_result: BLOCKED
open: open:py_core/market_data/binance_public_rest.py#"if last_accumulated_open_time_ms is not None:" absent:py_core/market_data/binance_public_rest.py#"open_time_ms < start_ms" absent:py_core/market_data/warehouse_backfill.py#is_contiguous
refs: code:py_core/market_data/warehouse.py#is_contiguous
seg.code: MISSING code:py_core/market_data/binance_public_rest.py#BinanceKlineGapError -- 跨页连续性只在已累积过上一页时校验，返回前没有按请求起点过滤，早于游标的首页会被整体接受；BackfillReport 没有连续性字段（覆盖度报告 warehouse.py 已有 is_contiguous）
seg.contract: PRESENT bp:"任务 SAFE-06" -- 任务已定义请求窗口与游标语义
seg.fault: MISSING -- 没有对应的故障注入用例编号；也没有“首页早于游标”的负向测试
seg.test: MISSING test:py_core/tests/test_binance_public_rest.py#test_gap_between_pages_raises_gap_error test:py_core/tests/test_binance_public_rest.py#test_pagination_advances_cursor_and_stitches_pages -- 现有测试覆盖页间与页内缺口，不覆盖起点之前的页
seg.runtime: MISSING -- 无 CI run 可引：修复尚未落地
seg.budget: MISSING -- 未测量
closure: SAFE-06 完成：首页按起点过滤并校验、回填报告带连续性字段、请求窗口与游标单测通过
```

```ledger
id: P2-001
legacy: P2-001
title: 传输策略未接线，白名单计数超限可致越界读取
severity: MEDIUM
kind: lib
status: REMEDIATING
task: SAFE-01
gate: REST 通信门禁
gate_result: BLOCKED
open: open:native/include/hengyuan/transport_policy.hpp#"if (p.endpoint_allowlist.count == 0) return TransportCheck::EndpointNotAllowed;" absent:native/include/hengyuan/transport_policy.hpp#"count > kMaxEndpoints" absent:native/src/*.cpp#check_endpoint absent:native/include/hengyuan/binance_private_rest.hpp#check_endpoint
refs: code:native/include/hengyuan/transport_policy.hpp#validate_policy code:native/include/hengyuan/transport_policy.hpp#check_endpoint
seg.code: MISSING -- check_endpoint 在 native/src 与私有 REST 客户端中没有任何调用点；EndpointAllowlist::contains 按 count 循环，validate_policy 只检查 count 为零，不检查上界
seg.contract: PRESENT doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#TransportPolicy bp:"任务 SAFE-01" -- 规格与任务已定义“发起连接前强制检查”
seg.fault: MISSING fi:FI-037=PARTIAL -- 端点校验函数非测试调用点为零
seg.test: MISSING test:native/tests/test_transport_policy.cpp#EndpointCheckRejectsInvalidHost test:native/tests/test_transport_policy.cpp#RejectsEmptyEndpointList -- 现有测试只验证函数本身，不验证接线，也没有计数超限的用例
seg.runtime: MISSING -- 无 CI run 可引：修复尚未落地
seg.budget: MISSING -- 未测量
closure: SAFE-01 完成：连接前强制调用 check_endpoint，validate_policy 钳制计数上界，FI-037 的单测通过
```

```ledger
id: P2-002
legacy: P2-002
title: 账户查询在凭据为空时解引用（唯一缺少空凭据守卫的公开 REST 方法）
severity: MEDIUM
kind: lib
status: REMEDIATING
task: SAFE-01
gate: REST 通信门禁
gate_result: BLOCKED
open: open:native/include/hengyuan/binance_private_rest.hpp#"build_signed_query(*creds_, unsigned_query, fresh_ts_ms)" count4:native/include/hengyuan/binance_private_rest.hpp#"if (!creds_)"
refs: code:native/include/hengyuan/binance_private_rest.hpp#fetch_account test:native/tests/test_binance_private_rest.cpp#NullCredentialsFoldsIntoNotSentWithoutNetworkAttempt
seg.code: MISSING -- fetch_account(out, cfg) 直接解引用 *creds_；其余四处使用凭据的位置均有 if (!creds_) 守卫（该字面量在文件中恰好出现 4 次，修复后变为 5 次，核对会提醒）
seg.contract: PRESENT doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#fetch_account bp:"任务 SAFE-01" -- 兄弟方法的“空凭据折叠为 NotSent”即合同
seg.fault: MISSING -- 没有对应的 FI 编号
seg.test: MISSING test:native/tests/test_binance_private_rest.cpp#NullCredentialsFoldsIntoNotSentWithoutNetworkAttempt -- 该测试属于 query_order；fetch_account 没有空凭据用例
seg.runtime: MISSING -- 无 CI run 可引：修复尚未落地
seg.budget: NA -- 一次指针判空，不增加可测的运行期成本
closure: SAFE-01 完成：fetch_account 加守卫并有空凭据用例
```

```ledger
id: P2-003
legacy: P2-003
title: 发单状态转换缺少缺省分支，已接受分支未校验交易所订单号为正
severity: MEDIUM
kind: lib
status: REMEDIATING
task: SAFE-06
gate: 下单状态转换
gate_result: BLOCKED
open: open:native/include/hengyuan/live_submit_orchestrator.hpp#"switch (resp.outcome) {" absent:native/include/hengyuan/live_submit_orchestrator.hpp#"default:" absent:native/include/hengyuan/live_submit_orchestrator.hpp#"exchange_order_id > 0"
seg.code: MISSING code:native/include/hengyuan/live_submit_orchestrator.hpp#SubmitOutcome code:native/include/hengyuan/live_submit_orchestrator.hpp#"= 5, not 4" -- 转换开关没有 default 分支，Accepted 分支不校验 exchange_order_id 为正；当前 5 个枚举值（0 至 3 与 5）均被显式覆盖，无现存未定义行为；规格另定义了 RateLimited（值 4，代码里留空），加入时若漏改开关就会静默落空
seg.contract: PRESENT doc:docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md#StaleRulesVersion bp:"任务 SAFE-06" -- 规格与任务已定义状态完备性
seg.fault: MISSING -- 没有对应的 FI 编号
seg.test: MISSING test:native/tests/test_live_submit_orchestrator.cpp#SubmitRejectedRouting test:native/tests/test_live_submit_orchestrator.cpp#SubmitTimeoutAmbiguous -- 现有测试逐个覆盖已有枚举值，没有“未知枚举值失败即关闭”的用例
seg.runtime: MISSING -- 无 CI run 可引：修复尚未落地
seg.budget: NA -- 分支完备性不增加热路径成本
closure: SAFE-06 完成：加 default 失败即关闭与订单号校验，转换完备性测试通过
```

```ledger
id: P2-004
legacy: P2-004
title: 熔断开关状态为普通非原子字段，缺跨重启持久化与发布次序
severity: MEDIUM
kind: live
status: REMEDIATING
task: SAFE-04
gate: 实盘发单风控
gate_result: BLOCKED
open: open:native/include/hengyuan/kill_switch.hpp#"KillState state_{KillState::Normal};" absent:native/include/hengyuan/kill_switch.hpp#std::atomic
refs: code:native/include/hengyuan/kill_switch.hpp#KillSwitch test:native/tests/test_kill_switch.cpp#OnlyOperatorResetClears
seg.code: MISSING -- state_ 是普通字段，风控写与下单读之间存在数据竞争；内存锁存已实现且仅由操作员重置解除，缺的是跨重启持久化与“持久确认后发布”的次序
seg.contract: PRESENT doc:docs/SPEC_INVARIANTS.md#KillSwitch bp:"任务 SAFE-04" -- 任务已定义持久锁存与线性化次序
seg.fault: MISSING fi:FI-023=NOT_TESTABLE_YET fi:FI-036=NOT_TESTABLE_YET -- 持久锁存与跨线程次序均无可执行用例
seg.test: MISSING test:native/tests/test_kill_switch.cpp#LatchedIsTerminal_NoAutoRearm -- 现有 7 个用例是单线程状态机测试，没有线程检测压力用例
seg.runtime: MISSING -- 无线程检测压力日志，无重启后保持锁定的运行日志
seg.budget: MISSING -- 持久确认进入风控路径的时延未测量
closure: SAFE-04 完成；FI-036 的线程检测压力日志与 FI-023 的重启保持锁定日志取得
```

```ledger
id: P2-005
legacy: P2-005
title: 订单跟踪器的轮询与超时计时接口不强制单调时钟
severity: MEDIUM
kind: lib
status: REMEDIATING
task: SAFE-06
gate: 订单跟踪器
gate_result: BLOCKED
open: open:native/include/hengyuan/order_tracker.hpp#"std::int64_t now_ms," open:native/include/hengyuan/order_tracker.hpp#"void note_polled(std::string_view coid, std::int64_t now_ms) noexcept {"
refs: code:native/include/hengyuan/order_tracker.hpp#reconcile_wall_now_ms test:native/tests/test_order_tracker.cpp#WallClockSteppingBackwardsNeverPostponesQuarantine
seg.code: MISSING -- 计时参数仍是普通 int64 毫秒，接口不强制单调时钟；下溢已有哨兵值保护，现有调用者均传单调时钟，缺口是时钟回拨会静默延迟轮询而不是报错（R-10 已为隔离判据加入挂钟，不改变这一点）
seg.contract: PRESENT doc:docs/SPEC_INVARIANTS.md#OrderTracker bp:"任务 SAFE-06" -- 任务已定义单调时钟约束
seg.fault: MISSING -- 没有对应的 FI 编号
seg.test: MISSING test:native/tests/test_order_tracker.cpp#WallClockSteppingBackwardsNeverPostponesQuarantine -- 现有用例覆盖隔离判据的挂钟回拨，不覆盖轮询与退避计时的单调性
seg.runtime: MISSING -- 无 CI run 可引：修复尚未落地
seg.budget: NA -- 类型层面的约束，不增加运行期成本
closure: SAFE-06 完成：计时接口强制单调时钟类型，单调时钟超时测试通过
```

```ledger
id: P2-006
legacy: P2-006
title: 声明为不抛异常的持久审计接收器构造函数在初始化列表里做字符串拼接
severity: MEDIUM
kind: lib
status: DEFERRED
gate: 鲁棒性优化
gate_result: DEFERRED
bkl: BKL-001
trigger: 启动持久层重构并引入固定大小静态预分配缓冲区时激活
open: open:native/include/hengyuan/durable_audit_sink.hpp#'rotation_log_store_(path + ".keyrotations"' open:native/include/hengyuan/control_plane_log_sink.hpp#'log_store_(path, path + ".cp.lock", path + ".cp.tip")' open:native/include/hengyuan/export_worker.hpp#'log_store_(path + ".unused_log", path + ".lock", path + ".tip")'
seg.code: MISSING -- 三处同形构造函数仍在 noexcept 初始化列表中拼接路径，分配失败将直接终止进程
seg.contract: MISSING -- 尚无静态预分配缓冲区的设计合同
seg.fault: MISSING -- 没有对应的 FI 编号
seg.test: MISSING -- 没有分配失败注入测试
seg.runtime: MISSING -- 未排期，无运行证据
seg.budget: MISSING -- 未排期
closure: 见解冻条件
```

```ledger
id: P2-007
legacy: P2-007
title: 代码扫描曾被短路空绿；Python 通道缺综合覆盖率门禁
severity: MEDIUM
kind: build
status: PARTIAL
gate: CI 增强
gate_result: BLOCKED
bkl: BKL-002
trigger: 仓库转为公开或获得高级安全授权时激活——条件已满足（仓库 2026-09-30 公开，默认配置 CodeQL 2026-10-01 启用）；剩余 Python 综合覆盖率门禁
open: absent:.github/workflows/ci-python.yml#--cov absent:py_core/requirements.txt#pytest-cov absent:py_core/pyproject.toml#pytest-cov
refs: pr:132@826f7f36f pr:127@819844e19 pr:128@eb0250f19 pr:137@e75316caf
seg.code: MISSING code:.github/workflows/ci-python.yml#"python -m pytest" -- 代码扫描部分已落地（默认配置 CodeQL，自带高级配置已于 PR #132 移除）；Python 通道没有覆盖率工具与门禁
seg.contract: PRESENT doc:docs/REQUIRED_CHECKS_RUNBOOK.md#CodeQL -- 必需检查与 CodeQL 的契约见 runbook 与蓝图 2.2.3；覆盖率门禁的阈值合同尚未定义
seg.fault: NA -- 构建与 CI 配置缺陷，没有运行期故障路径
seg.test: MISSING -- 无覆盖率测量，谈不上覆盖率回归
seg.runtime: PRESENT run:37618152776@68c393a07/ci-python.yml -- CI Python 在主干上真实运行并全绿；仅代表通道本身可用，不代表覆盖率门禁存在
seg.budget: NA -- 覆盖率门禁尚未引入，没有可量化的成本
closure: 引入覆盖率工具与阈值并列为必需检查
```

```ledger
id: P2-008
legacy: P2-008
title: Windows 平台缺少目录联接与重解析点防穿透专项测试
severity: MEDIUM
kind: lib
status: PARTIAL
gate: 平台安全测试
gate_result: BLOCKED
bkl: BKL-003
trigger: 开展 Windows 平台生产部署合规专项测试时激活
open: absent:.github/workflows/*.yml#windows- absent:native/tests/test_compaction_lease.cpp#junction
refs: code:native/include/hengyuan/windows_native_io.hpp#open_directory code:native/include/hengyuan/compaction_lease.hpp#RejectedSymlinkOrReparse
seg.code: PRESENT code:native/include/hengyuan/windows_native_io.hpp#open_directory code:native/include/hengyuan/compaction_lease.hpp#RejectedSymlinkOrReparse code:native/include/hengyuan/env_loader.hpp#FILE_ATTRIBUTE_REPARSE_POINT -- 原语层与租约层都拒绝重解析点
seg.contract: PRESENT code:native/include/hengyuan/windows_native_io.hpp#RejectedReparsePoint -- 原语的返回值即合同；租约层把它映射为 RejectedSymlinkOrReparse
seg.fault: PRESENT:UT test:native/tests/test_windows_native_io.cpp#RejectsDirectoryJunction -- 真实 NTFS 目录联接，断言 open_directory 返回 RejectedReparsePoint；仅原语层
seg.test: MISSING test:native/tests/test_windows_native_io.cpp#RejectsDirectoryJunction -- 该文件整体在 #ifdef _WIN32 之内；租约层与控制面层没有端到端的联接或重解析点测试
seg.runtime: MISSING -- CI 没有任何 Windows 作业（absent: 引用），该测试只在本机 MSVC 构建里执行，没有 CI run 可引
seg.budget: NA -- 测试覆盖项，不改变运行期路径
closure: REM-0.1 新增 Windows 与 MSVC 作业，并补租约层联接测试
```

```ledger
id: P2-009
legacy: P2-009
title: CI Native 缺少机械强制的多平台与 Sanitizer 门禁
severity: MEDIUM
kind: build
status: REMEDIATING
task: REM-0.1
gate: 全项目研发准入
gate_result: BLOCKED
open: absent:.github/workflows/ci-native.yml#windows- open:.github/workflows/ci-native-sanitizers.yml#"- cron: '0 3 * * 1'"
refs: pr:110@5aa42428d pr:137@e75316caf pr:126@a6739fa1a pr:129@d401ed78e
seg.code: MISSING code:.github/workflows/ci-native.yml#"runs-on: ubuntu-24.04" code:.github/workflows/ci-native-sanitizers.yml#"timeout-minutes: 90" -- 没有任何 MSVC 或 Windows 作业；Sanitizer 通道只在每周一定时触发，不是必需检查（native: gate 只汇总 GCC-14 的 ci-native 作业）
seg.contract: PRESENT doc:docs/REQUIRED_CHECKS_RUNBOOK.md#gate bp:"任务 REM-0.1" -- 必需检查与 gate 汇总的契约见 runbook；REM-0.1 定义了覆盖矩阵
seg.fault: NA -- 构建与 CI 配置缺陷，没有运行期故障路径；Sanitizer 作业内的两个负控另行证明检测有效
seg.test: MISSING -- 缺少 MSVC 构建测试作业；Sanitizer 不是每次提交触发
seg.runtime: MISSING run:37586509738@e75316caf/ci-native.yml run:"37258555091@d401ed78e/ci-native-sanitizers.yml[ASan+UBSan: full suite]" -- 已有真实成功的 CI Native 与 Sanitizer run（见引用），但它们只覆盖 GCC-14 与周更，达不到 REM-0.1 的覆盖矩阵
seg.budget: PRESENT run:"37258555091@d401ed78e/ci-native-sanitizers.yml[ASan+UBSan: full suite]" -- 实测：ASan+UBSan 28.5 分钟（上限 90）、TSan 1.0 分钟（上限 25，只构建并发目标）、ARM64 1.4 分钟；每次提交触发的预算前提尚待评估
closure: REM-0.1 新增 Windows 与 MSVC 必需作业，Sanitizer 在预算前提下改为每次提交触发
```

```ledger
id: P3-001
legacy: P3-001
title: 交易记录器使用裸文件打开与格式化写入，缺跨平台文件锁保护
severity: LOW
kind: lib
status: DEFERRED
gate: 日志健壮性
gate_result: DEFERRED
bkl: BKL-004
trigger: 交易记录器迁移至跨平台独占文件 I/O 时激活，可复用持久日志存储中的既有实现
open: open:native/include/hengyuan/trade_logger.hpp#'std::fopen(path.c_str(), "w")' open:native/src/binance_dry_run_demo.cpp#TradeLogger
refs: code:native/include/hengyuan/durable_log_store.hpp#flock
seg.code: MISSING -- TradeLogger 仍用 std::fopen 与 fprintf，没有锁；它只被 binance_dry_run_demo 使用，不在交易路径上；可复用的跨平台锁在 durable_log_store.hpp
seg.contract: MISSING -- 尚无独占文件 I/O 的合同
seg.fault: MISSING -- 没有对应的 FI 编号
seg.test: MISSING -- 没有并发写入测试
seg.runtime: MISSING -- 未排期
seg.budget: MISSING -- 未排期
closure: 见解冻条件
```

```ledger
id: P3-002
legacy: P3-002
title: 外部动作与依赖采用浮动主版本标签，缺少摘要锁死
severity: LOW
kind: build
status: DEFERRED
gate: 供应链安全
gate_result: DEFERRED
bkl: BKL-005
trigger: 执行生产环境供应链依赖锁死时激活
open: open:.github/workflows/ci-native.yml#"actions/checkout@v7" open:.github/workflows/ci-python.yml#"actions/setup-python@v7" absent:py_core/requirements.txt#--hash
refs: code:.github/dependabot.yml#github-actions
seg.code: MISSING -- 全部 GitHub Actions 按主版本标签引用而非提交摘要；py_core/requirements.txt 是 == 精确钉死但没有哈希；Dependabot 只监控 github-actions
seg.contract: MISSING -- 尚无依赖锁死策略文件
seg.fault: NA -- 供应链配置缺陷，没有运行期故障路径
seg.test: MISSING -- 没有校验锁死状态的检查
seg.runtime: MISSING -- 未排期
seg.budget: NA -- 摘要固定不改变运行期成本
closure: 见解冻条件
```

### 4.2 补充审计（蓝图 4.3）

```ledger
id: SUPP-001
legacy: BUILD-TSAN-GUARD
title: 构建缺少对线程检测运行环境的统一守卫（三个线程相关目标曾静默跳过 TSan）
severity: MEDIUM
kind: build
status: CLOSED
gate: 无（蓝图 4.3 不登记阻塞门禁）
gate_result: CLEARED
refs: code:native/CMakeLists.txt#"AUDIT BUILD-TSAN-GUARD-001" pr:60@b1107a291
seg.code: PRESENT code:native/CMakeLists.txt#CMAKE_CROSSCOMPILING_EMULATOR code:native/CMakeLists.txt#"AUDIT BUILD-TSAN-GUARD-001" -- 逐目标的重复设置收口为一个全局变量，新建目标自动继承；三个曾漏设的目标因此补齐
seg.contract: PRESENT doc:native/cmake/Sanitizers.cmake#"-DHY_SANITIZER=thread" -- 构建脚本头注释定义 HY_SANITIZER 取值与互斥语义
seg.fault: NA -- 构建配置缺陷，没有运行期故障路径；同作业的两个 TSan 负控负责证明检测仍然有效
seg.test: PRESENT test:native/tests/tsan_control_relaxed_ring.cpp#main test:native/tests/tsan_control_export_worker_dual_consumer.cpp#main -- 两个负控必须在 TSan 下失败，由 TSan 作业判定
seg.runtime: PRESENT run:"37258555091@d401ed78e/ci-native-sanitizers.yml[TSan: concurrency tests]" -- 真实成功的 Sanitizer run：TSan 作业的步骤记录里，并发测试与两个负控步骤（TSan 必须报出被注入的竞争）均为 success
seg.budget: PRESENT run:"37258555091@d401ed78e/ci-native-sanitizers.yml[ASan+UBSan: full suite]" -- TSan 作业 1.0 分钟（上限 25，只构建并发目标），ASan+UBSan 28.5 分钟（上限 90）
```

```ledger
id: SUPP-002
legacy: CLOCKPAIR-API
title: 时钟读数对 API 的整型与时间类型混用（两个独立来源的时间戳可被拼成一对）
severity: MEDIUM
kind: type
status: CLOSED
gate: 无（蓝图 4.3 不登记阻塞门禁）
gate_result: CLEARED
refs: code:native/include/hengyuan/binance_clock_sync.hpp#"AUDIT L4-CLOCKPAIR-API-002" pr:60@b1107a291
seg.code: PRESENT code:native/include/hengyuan/binance_clock_sync.hpp#ClockPairSample code:native/include/hengyuan/binance_clock_sync.hpp#fetch_clock_pair -- ClockPairSample 改为非聚合类型，构造只经 fetch_clock_pair 与测试钩子
seg.contract: PRESENT code:native/include/hengyuan/binance_clock_sync.hpp#"Construction is now restricted to fetch_clock_pair()" doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#ClockOffsetPublisher -- 头文件注释与规格 2.2 定义构造约束
seg.fault: NA -- 编译期封闭：调用方无法用两个独立来源的整型拼出一对，故障在类型层面不可构造
seg.test: PRESENT test:native/tests/test_binance_clock_sync.cpp#is_aggregate_v test:native/tests/test_binance_clock_sync.cpp#NormalRttComputesCorrectly -- static_assert(!std::is_aggregate_v<ClockPairSample>) 在每次编译时执行，ComputeClockOffset 用例在每次 CI 中运行
seg.runtime: NA -- 纯类型与纯函数逻辑，无外部 I/O 与崩溃恢复路径
seg.budget: NA -- 头文件内联的类型约束，无运行期成本
```

```ledger
id: SUPP-003
legacy: LOCK-ORDER
title: 凭据缓冲区必须先加锁、后写入密钥（冻结原文把位置写成 ClockSync，见第 3 节第 3 条）
severity: MEDIUM
kind: lib
status: CLOSED
gate: 无（蓝图 4.3 不登记阻塞门禁）
gate_result: CLEARED
refs: code:native/include/hengyuan/binance_signer.hpp#"AUDIT L4-LOCK-ORDER-003" code:native/include/hengyuan/binance_environment.hpp#"AUDIT L4-LOCK-ORDER-003" pr:60@b1107a291
seg.code: PRESENT code:native/include/hengyuan/binance_signer.hpp#"AUDIT L4-LOCK-ORDER-003" code:native/include/hengyuan/binance_environment.hpp#"AUDIT L4-LOCK-ORDER-003" -- 缓冲区在仍为全零时先 mlock 或 VirtualLock，再复制密钥
seg.contract: PRESENT code:native/include/hengyuan/binance_signer.hpp#"Locking empty memory first and only" -- 头文件注释写明次序约束
seg.fault: PRESENT:UT test:native/tests/test_binance_signer.cpp#LockSeesEmptyBufferBeforeSecretIsWritten test:native/tests/test_binance_environment.cpp#LockSeesEmptyBuffersBeforeCredentialsAreWritten -- 注入加锁桩，断言每次复制都发生在加锁成功之后
seg.test: PRESENT test:native/tests/test_binance_signer.cpp#StubPairingFailurePath test:native/tests/test_binance_environment.cpp#LockFailurePathWipesAndReportsError test:native/tests/test_binance_signer.cpp#RealSyscallSmoke -- 加锁失败路径清零并报错；真实系统调用只有冒烟用例
seg.runtime: PRESENT run:37586509738@e75316caf/ci-native.yml -- GCC-14 Release 全量 ctest（含上述用例）；Windows 的 VirtualLock 路径只在本机 MSVC 构建执行，CI 没有 Windows 作业
seg.budget: NA -- 凭据装载发生在进程启动期，一次性加锁与写入，不在热路径，次序调整不增加调用次数
```

```ledger
id: SUPP-004
legacy: RESERVED-PARAM
title: 查询签名未拦截调用方传入的保留参数（timestamp、signature）
severity: HIGH
kind: lib
status: CLOSED
gate: 无（蓝图 4.3 不登记阻塞门禁）
gate_result: CLEARED
refs: code:native/include/hengyuan/binance_environment.hpp#"AUDIT L4-RESERVED-PARAM-004" pr:60@b1107a291
seg.code: PRESENT code:native/include/hengyuan/binance_query_signing.hpp#is_reserved_query_key code:native/include/hengyuan/binance_query_signing.hpp#"detail::is_reserved_query_key(kv.first)" code:native/include/hengyuan/binance_environment.hpp#ReservedParamName -- 判定函数与强制调用点都存在，命中即返回 ReservedParamName
seg.contract: PRESENT code:native/include/hengyuan/binance_query_signing.hpp#"those two are appended by build_signed_query()" -- 头文件注释定义：timestamp 与 signature 只能由签名器自己追加
seg.fault: PRESENT:UT test:native/tests/test_binance_query_signing.cpp#RejectsReservedTimestampKey test:native/tests/test_binance_query_signing.cpp#RejectsReservedSignatureKey -- 调用方传入保留键必须被拒
seg.test: PRESENT test:native/tests/test_binance_query_signing.cpp#RejectsReservedTimestampKey test:native/tests/test_binance_query_signing.cpp#RejectsReservedSignatureKey test:native/tests/test_binance_query_signing.cpp#PercentEncodesKeyToo -- 键与值都做百分号编码
seg.runtime: PRESENT run:37586509738@e75316caf/ci-native.yml -- GCC-14 Release 全量 ctest
seg.budget: NA -- 请求构造路径（非行情热路径）上对少量键做常数次字符串比较
```

```ledger
id: SUPP-005
legacy: BINDING-REF
title: EnvironmentBinding 引用悬挂（冻结原文写作“pybind11 绑定”，见第 3 节第 3 条）
severity: HIGH
kind: type
status: CLOSED
gate: 无（蓝图 4.3 不登记阻塞门禁）
gate_result: CLEARED
refs: code:native/include/hengyuan/binance_environment.hpp#"AUDIT L4-BINDING-REF-005" pr:60@b1107a291
seg.code: PRESENT code:native/include/hengyuan/binance_environment.hpp#"AUDIT L4-BINDING-REF-005" code:native/include/hengyuan/binance_environment.hpp#load_and_bind_credentials -- 成员 binding_ 与仅为其而设的删除右值重载已移除，BoundHmacCredentials 不再保存对 EnvironmentBinding 的引用
seg.contract: PRESENT code:native/include/hengyuan/binance_environment.hpp#"this used to take `const EnvironmentBinding&`" -- 头文件注释记录了修复前后的生命周期合同
seg.fault: NA -- 编译期封闭：悬挂引用的来源（保存引用的成员）已删除，故障在类型层面不可构造
seg.test: PRESENT test:native/tests/test_binance_environment.cpp#AcceptsTemporaryEnvironmentBinding -- 修复前传入临时对象无法编译，现在必须编译并成功
seg.runtime: NA -- 类型层面的生命周期约束，无运行期路径
seg.budget: NA -- 删除成员，运行期成本只减不增
```

```ledger
id: SUPP-006
legacy: DOC-DRIFT-1
title: L4 规格与 ClockSync 实现细节漂移
severity: LOW
kind: doc
status: CLOSED
gate: 无（蓝图 4.3 不登记阻塞门禁）
gate_result: CLEARED
refs: pr:8@67d30ffbd pr:60@b1107a291
seg.code: PRESENT code:native/include/hengyuan/binance_clock_sync.hpp#"kOffsetTtlMs = 5 * 60 * 1000" code:native/include/hengyuan/binance_clock_sync.hpp#"mutable std::mutex mu_" code:native/include/hengyuan/transport_policy.hpp#"recv_window_ms{5000}" -- 规格 2.2 中可核对的三处细节对应的代码常量
seg.contract: PRESENT doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#"fresh for 5 minutes" doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#"mutex-guarded" doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#"(currently defaults to 5000ms)" -- 规格 2.2 的三处文字与上面的代码常量一致
seg.fault: NA -- 纯文档校准，无运行期故障路径
seg.test: NA -- 一致性由本清单的 doc: 与 code: 引用在每次核对时检查
seg.runtime: NA -- 纯文档校准，无执行路径
seg.budget: NA -- 纯文档校准，无运行期成本
```

```ledger
id: SUPP-007
legacy: DOC-DRIFT-2
title: 查询签名字符集编码规则的文档说明漂移
severity: LOW
kind: doc
status: CLOSED
gate: 无（蓝图 4.3 不登记阻塞门禁）
gate_result: CLEARED
refs: pr:5@5f0b78ac2 pr:60@b1107a291
seg.code: PRESENT code:native/include/hengyuan/binance_query_signing.hpp#is_unreserved code:native/include/hengyuan/binance_query_signing.hpp#append_percent_encoded -- 非保留字符集 A-Za-z0-9-._~ 之外一律百分号编码
seg.contract: PRESENT doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#"A-Za-z0-9-._~" -- 规格 2 的编码规则与代码一致
seg.fault: NA -- 纯文档校准，无运行期故障路径
seg.test: NA -- 一致性由本清单的 doc: 与 code: 引用在每次核对时检查；编码行为本身由 SUPP-009 的单测覆盖
seg.runtime: NA -- 纯文档校准，无执行路径
seg.budget: NA -- 纯文档校准，无运行期成本
```

```ledger
id: SUPP-008
legacy: GITIGNORE
title: 忽略规则未覆盖测试数据与中间产物
severity: LOW
kind: doc
status: CLOSED
gate: 无（蓝图 4.3 不登记阻塞门禁）
gate_result: CLEARED
refs: pr:60@b1107a291 pr:116@0c33f9437
seg.code: PRESENT code:.gitignore#"*.obj" code:.gitignore#"*.o" -- 对象文件规则 *.obj、*.o 由 PR #116 补入
seg.contract: PRESENT doc:.gitignore#"# Build artifacts" -- .gitignore 自身即规则的唯一合同
seg.fault: NA -- 纯文本配置，无运行期故障路径
seg.test: PRESENT ignored:scratch.obj ignored:scratch.o ignored:native/build-msvc/CMakeCache.txt -- git check-ignore 在每次核对时执行
seg.runtime: NA -- 纯文本配置，无执行路径
seg.budget: NA -- 纯文本配置，无运行期成本
```

```ledger
id: SUPP-009
legacy: TEST-GAP
title: 缺少空载荷与极值特殊字符的签名单测
severity: MEDIUM
kind: lib
status: CLOSED
gate: 无（蓝图 4.3 不登记阻塞门禁）
gate_result: CLEARED
refs: pr:60@b1107a291
seg.code: PRESENT code:native/include/hengyuan/binance_signer.hpp#"payload.empty()" code:native/include/hengyuan/binance_query_signing.hpp#kMaxQueryLen code:native/include/hengyuan/binance_query_signing.hpp#append_percent_encoded -- 被补测的三个行为：空载荷拒签、查询串长度上界、百分号编码
seg.contract: PRESENT code:native/include/hengyuan/binance_signer.hpp#"Fail-closed: returns empty span on any error" doc:docs/BINANCE_PRIVATE_REST_L4_SPEC.md#percent-encoded -- 签名器失败即关闭；规格要求签名前先编码
seg.fault: PRESENT:UT test:native/tests/test_binance_signer.cpp#EmptyPayloadRefused test:native/tests/test_binance_query_signing.cpp#OneByteOverMaxLengthFails test:native/tests/test_binance_query_signing.cpp#EmptyCanonicalQueryRejected -- 空载荷与超长查询串必须被拒
seg.test: PRESENT test:native/tests/test_binance_query_signing.cpp#PercentEncodesReservedCharacters test:native/tests/test_binance_query_signing.cpp#MaxLengthBoundarySucceeds test:native/tests/test_binance_signer.cpp#EmptySecretFails -- 极值与特殊字符的边界向量
seg.runtime: PRESENT run:37586509738@e75316caf/ci-native.yml -- GCC-14 Release 全量 ctest
seg.budget: NA -- 测试补全，不改变运行期代码路径
```
