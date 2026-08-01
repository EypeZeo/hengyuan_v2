# SPEC_INVARIANTS.md — 机械可核查的规格不变量清单

来源：`docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md`（当前 Revision 73 / Architect review round 72）+
`docs/BINANCE_PRIVATE_REST_L4_SPEC.md`（当前 Revision 19 / round 19；注意 L4 文件内部的 round 计数与
SUBMITPORT 文件不是同一套编号，两者靠 changelog 里的 "see L4 revision N" 互相交叉引用）。

## 这份清单解决什么问题

两份 spec 经过几十轮审阅后，绝大多数新 bug 不是原始设计缺陷，而是标记为
"self-inflicted"、"conflicts between round-N fixes themselves"、
"one branch updated, sibling states/docs still on old semantics" 的**同一个事实在 prose 里被复述多处，
改一处漏了另一处**。这份清单把这些反复被确认过的"权威事实"从 changelog 里提取出来，作为独立于正文的
一等公民产物，配合 `tools/spec_xref_check.py` 做机械交叉核查——而不是每次改动都要求审阅者凭记忆判断
"还有没有别的地方也提到这个"。

## 用法

修改任一 spec **或已落地的 header** 前，运行：

```bash
python tools/spec_xref_check.py is_exchange_final AuditAppendResult
```

搜索范围是两份 spec **加上** `native/include/hengyuan/durable_control_plane.hpp`、
`order_lifecycle.hpp`——一旦某条不变量落地成代码，spec↔代码漂移就是最主要的失效模式，只搜 prose
看不见它。不带参数运行会检查本文件登记的全部符号；`--quiet` 只报问题（CI 模式）；登记符号在所有
被搜文件里零命中时退出码为 1（清单过期信号），由
`.github/workflows/ci-spec-verification.yml` 作为门禁。

**这个工具已经抓到过真实漂移**：`FrameTimeKind` 的枚举值在移植进 header 时被写成
`ServerCorrected`，而 spec §6.1.1 的权威定义是 `ServerCorrectedUtc`——把 spec 与 header 的命中并排
列出来的那一刻就暴露了。同时暴露的还有一个更隐蔽的语义错误：header 当时把默认值设成
`UnknownBootstrap` 并自称"fail-closed"，实际恰恰相反——`UnknownBootstrap` 是**被排除在** age/TTL
计算之外的，未打戳的帧会静默通过所有陈旧性检查；spec 的默认值 `ServerCorrectedUtc` 配
`recorded_utc_ms=0` 反而会算出极大的 age 而触发 hard-lag 围栏，才是真正的失败关闭。

## 不变量清单

每条格式：`符号名` — 当前生效的权威定义 | 首次确立 round | 曾经因为"漏改兄弟处"导致 bug 的 round（如有）

### 状态机 / 分类函数

- `is_exchange_final` — 权威返回集合仅为 `{Filled, Cancelled, Rejected, Expired}`。**不包括**
  `Reconciled`（round 24 曾被错误纳入，与 §6.5 的权威定义矛盾，round 24 修复移除）；**不包括**
  `EscalatedToOperator`（这是它和 `is_terminal()` 的关键区别，round 9 引入这个区分，为了不让 compaction
  丢弃一个未解决的升级订单仍需要的快照）。| 首次确立 round 9 | self-inflicted: round 15→16
  （曾遗漏 `AbortedPreSend` 导致 checkpoint 永久泄漏，round 16 修复补上；但 round 17 该状态本身被整体
  移除，这个特判又变得没有意义——见下一条）
  - **代码现状：已定义，未启用。** `order_lifecycle.hpp` 已落地该谓词，但除 ABI 测试外**无任何生产
    调用点**。不要把它读成"已强制执行"——真正会用到它的消费者（compaction 保留规则、COID 释放、
    poll/reconcile 循环）尚不存在。
  - **不要**把 `order_lifecycle.hpp:93`（`validate_transition` 开头的 `is_terminal(from)` →
    `AlreadyTerminal`）改成 `is_exchange_final`：那里是纯状态机守卫（"没有任何自动转移离开此状态"），
    改了会**放行** `Reconciled →` 和 `EscalatedToOperator →` 的自动转移，是回归而非修复。
- **[已裁决] `Reconciled` 不是活跃转移目标** — 曾经的分歧：spec round 24 说"no code path in this
  design ever targets it as a live transition"，但代码 `order_lifecycle.hpp` 的 `validate_transition`
  在 `case OrderState::Ambiguous` 下曾**明确放行** `Ambiguous → Reconciled`，两者不能同时为真。
  **裁决：改代码，采纳 spec 一侧。** `Ambiguous` 现在直接指向 reconciliation 查询实际发现的具体
  exchange-final 状态（`Filled`/`Cancelled`/`Rejected`/`Expired`）或 `EscalatedToOperator`，不再有
  任何路径指向 `Reconciled`——这也更贴近 L4 spec 里 `apply_confirmed_state()` 的真实设计（把
  `confirmed_state` 映射到对应的具体 `OrderState`，而不是丢弃细节塞进一个通用"已核实"桶）。
  `Reconciled` 枚举值本身**保留**（不像 `AbortedPreSend` 那样整体删除），仅作为历史/文档意义上的
  标记——`is_terminal(Reconciled)` 仍为 `true`，`is_exchange_final(Reconciled)` 仍为 `false`，
  但由于该值现在彻底不可达，这两个判定实际上已经不会再被触发。
  **刻意未做的事**：`Ambiguous → Accepted`/`Ambiguous → PartialFill`（"发现订单其实还活着，需要继续
  跟踪"这类结果）没有加进来——这属于轨道 A 第 3 项（reconcile/poll 循环）的范围，不属于这次纯状态机
  层面的裁决，留到那时候一起设计,避免本次改动的范围超出"消除 spec/代码矛盾"这一件事。
- `OrderState::AbortedPreSend` — **已被移除，不是活跃状态**。round 17 的结论：`Submitting` 只在 Gate 9
  `OrderSubmitPrepared` 真正 `.acked()` 之后才会被设置一次，pre-send 失败根本不会经过 `Submitting`，
  因此这个状态、它的 transition、`is_terminal()`/`is_exchange_final()` 里的特判全部撤销。**任何 spec
  文本如果还把 `AbortedPreSend` 当作活跃状态处理，即为过期表述。**| 引入 round 13(rev14)，
  废弃 round 17(rev18)
- `Submitting` 状态赋值时机 — 权威定义：仅在 **Gate 9**，`OrderSubmitPrepared` 被 `.acked()` 之后设置
  一次；**不在 Gate 6**（in-flight 注册）设置。| round 17 修正（此前版本的 pseudocode 注释曾在 Gate 6
  处赋值，是当时那一整条矛盾链的根源）

### ABI / 结构体

- `AuditAppendResult` — 权威定义：**struct**（不是裸 enum），必须带 `.sequence` 字段 +
  `.acked()` 方法；两份文件内所有 `append_*`/`append_durable` 的返回值比较都必须用 `.acked()`。
  唯一定义位置：共享头 `hengyuan/durable_control_plane.hpp`（round 10 起强制——此前两份文件各自维护
  同名 mirror 定义，是 ODR 问题）。| 首次确立 round 9(rev10) | self-inflicted 修复 round 14(rev15)
- `OrderRecoveryCheckpoint` — 权威定义：**不含** `own_sequence`/`last_event_sequence` 字段
  （round 14 曾引入又在 round 15 撤回——该字段要求写入前先知道自己尚未分配的 sequence，逻辑循环，
  在 group commit 下不可构造）。checkpoint 的 sequence 一律从 frame header 解码时读取；后续引用一律用
  `AuditAppendResult.sequence`（写入后立即捕获），从不预测。| 引入 round 13(rev14) | self-inflicted
  修复 round 14→15(rev15→16)
- `ExportOutboxRing` — 权威定义：固定容量 SPSC ring，**唯一所有者是 durable sink**（不是
  `ExternalAnchorClient`——round 15 曾有归属矛盾：prose 说 export worker 拥有/耗尽 FIFO，
  `ExternalAnchorClient` 接口注释又说 client 自己维护，round 16 修复统一到 sink 唯一所有）。
  | 首次确立 round 15(rev16) | self-inflicted 修复 round 16(rev17)
- `ExportTuple` — round 17(rev18) 才补齐实际结构定义（此前 `ExportOutboxRing`/`ExternalAnchorClient`
  已经在引用一个未定义的类型）。
- `DecodedOrderFrame` — §6.1.4 replay 循环用的解码后 frame 类型；同状态 replay 分支必须传
  `record.event`（不是原始 `DecodedOrderFrame` 本身）——round 14 的类型安全修复遗漏了一个分支，
  round 15 才补齐两个分支一致。
- `FrameTimeKind` / `recorded_utc_ms` — 权威位置：**frame header**（不是任一 payload 字段），
  被完整 frame MAC 覆盖，所有 frame 类型统一读取、统一写入。（round 18 之前曾要求逐个 payload 各自
  携带，对没有该字段的 payload 类型直接不可实现。）**枚举定义站点是
  `hengyuan/durable_control_plane.hpp`**（spec §6.1.1 的 CANONICAL 块自己这么写的），枚举名与数值
  必须与 spec 逐字一致：`ServerCorrectedUtc = 0`、`UnknownBootstrap = 1`。数值是 wire ABI 且被 MAC
  覆盖，重新编号会让所有历史帧被静默重解释。
  - **默认值必须是 `ServerCorrectedUtc`**，不是看起来更保守的 `UnknownBootstrap`：后者被排除在
    age/TTL 计算之外，未打戳的帧会静默通过陈旧性检查；前者配 `recorded_utc_ms=0` 会算出极大 age
    而触发 hard-lag 围栏。spec 对"无可信时钟"的失败关闭手段是**拒绝 append**，不是靠这个默认值。
  - `UnknownBootstrap` 仅对 spec §6.1.1 那张封闭 allowlist 里的 record type 合法；其余类型在无时钟时
    必须拒绝/推迟 append。
- `ExportTuple` — **tip 锚点记录**，字段为 `store_uuid_lo`/`store_uuid_hi`/`generation`/`sequence`/
  `tip_mac`(32 字节)/`key_id`/`enqueued_utc_ms`/`time_kind`。outbox 导出的是**持久化日志的链尖**，
  **没有 payload 字段**。`time_kind` 按 spec 原样是裸 `std::uint8_t`（承载 `FrameTimeKind` 值的
  wire 字段），不是枚举成员——照抄，不要"改进"。
  - **反面教材（本条存在的原因）**：初版 header 在这里**虚构**了一个 `payload[256]` + `payload_len` +
    `set_payload()` + `payload_view()`，与 spec 字段几乎零重叠；随后还在那个虚构缓冲区上"发现并修复"
    了一个越界读，并为它写了 4 个测试。全部 381 个测试通过、符号级 xref 也通过。
    **单元测试证明类型「行为符合它自己的写法」，完全不证明「它是照 spec 写的」**。
    定义级检查由 `tools/spec_enum_diff.py` 负责。
- `ExportOutboxRing` — `class`（**不是** `SpscRing` 的别名），`kCapacity = 256`，消费侧是**两阶段**：
  `peek_oldest()` 读取但**不移除**；只有在外部锚点真正 Ack 且 `LastRemoteAckedTip` 已持久更新后，才
  调用 `pop_after_remote_ack()` 推进 head。**pop-on-read 的环会在远端确认前就丢帧**，正是两阶段设计
  要防的那类数据丢失。所有权（round 16 P0）：环由 durable sink **独占**，`ExternalAnchorClient` 是
  无队列、无积压、无重试状态的纯传输客户端。
  - `size()` 的 load 顺序：**先读 `head_` 再读 `tail_`**。生产者推进 `tail_`、消费者推进 `head_`
    （与 `hy::SpscRing` 的索引角色相反），后读 `tail_` 保证 `tail >= head`，结果不会无符号下溢。
- `.xgc` / `CompactionIntentGcAuthorizedWire` — **当前权威版本：v2 / 316 字节 /
  `HY-COMPINTENT-GC-v2`**，内嵌 `DurableCleanupAuthEvidence`；legacy v1（204 字节）只作为 fail-closed
  识别对象，不再是活跃写入格式（round 67 引入 v1/204B，round 71 升级到 v2/316B）。**任何 spec 文本里
  把 "204B" 或 "v1" 当作活跃格式描述均为过期表述**——round 72(rev73) 本身就是在修复正文残留的
  204B/顺序错误引用，是这份清单存在意义最直接的证据。
- `CompactionIntentGcAuthorizedWire` 清理顺序（PostSeal/Abandon 终止路径）— 权威顺序：
  **CAPTURE → unlink C/A → CREATE `.xgc`(v2/316B) → TipExportProducerResume（幂等）→ GC `.x1` →
  clear Intent → unlink `.xgc`**。`TipExportProducerResume` 是幂等操作，**不是** Mode B 恢复判定的
  证据来源（round 72 明确排除这个曾经的误用）。

### `SubmitOutcome` / `OrchestratorGate`（rules_version fast-reject，部分落地）

- `SubmitOutcome::StaleRulesVersion = 5`、`OrchestratorGate::SubmitStaleRulesVersion = 21` — **数值
  照抄 spec**（`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md:789`/`:842`），故意跳过 `SubmitOutcome::
  RateLimited = 4` 和 `OrchestratorGate` 的 `SubmitPartialFill=18`/`SubmitFilled=19`/
  `SubmitRateLimited=20` 三个值——这三个值在代码里**尚未存在**，编号上留空，不是"下一个可用位置往上排"。
  `tools/spec_enum_diff.py` 把 `RateLimited` 记为 `MISSING_IN_CODE`（警告，非冲突），这是故意的当前
  状态，不是待修的 bug。
- `rules_version`（`SymbolRules` 的字段，`account_truth.hpp`）+ `SubmitPort::CurrentRulesVersionFn`/
  `current_rules_version()`（`live_submit_orchestrator.hpp`）+ `orchestrate_submit()` 里紧跟在
  `pre_trade_rules_snapshot` 捕获之后的 Gate 1 快速拒绝——这是 spec §2.1/§2.2 TOCTOU 防护链**已落地
  的部分**。`ctx.pre_trade_rules_snapshot` 是从 `*ctx.symbol_rules` 捕获的唯一副本，全程只用它，
  `validate_pre_trade()` 和 `SubmitPort::call()` 都不再二次解引用原始指针——这才是真正关闭 TOCTOU
  窗口的机制，Gate 1 本身只是快速失败，不是正确性的来源（spec 原话）。
  - **刻意未做的事**（都不是遗漏，是明确的范围裁决）：Gate 1 的位置是插在既有"Gate 6+7 预交易校验"
    块的开头，**不是**挪到整个函数最前面（spec 自己的 §3 把它排在 Audit 之前）——把它挪到最前面等于
    要连带重排 Audit/KillSwitch/DryRun/Signer/Depth 这几个既有 gate 的顺序，是单独一件更大的事。
    `SubmitOutcome::RateLimited`、`PartialFill`/`Filled` 的 fill-data 路由、L4 的限流冻结接线，全部
    留待后续。
  - **一个真实的 GCC-only 发现**：`live_submit_evidence_harness.cpp` 里有第二个 `switch
    (OrchestratorGate)`（`gate_name()` 辅助函数），MSVC `/W4` 没有对新增枚举值缺失 case 报警，GCC
    `-Wswitch` 报了——本仓库"两边工具链都要跑"这条规则又抓到一次真实分歧。

### 容量 / 生命周期

- `InFlightRegistry` / `kMaxInFlight` — 容量 64，`register_submit()` 在满时**失败关闭**（拒绝第 65 笔，
  绝不驱逐已跟踪订单）。全仓库**唯一**的 `mark_resolved()` 调用点是
  `live_submit_orchestrator.hpp` 的 `case SubmitOutcome::Rejected:`。
  - **这是正确行为，不是 bug**：`Rejected` 是四种 outcome 里唯一属于 `is_exchange_final` 的，即唯一
    能证明订单从未在交易所挂上的。`Accepted`/`Timeout`/`NetworkError` 保留槽位正是为了在订单可能仍
    存活时阻断盲目重发。**"提前释放槽位"是危险修法**——会把 COID 交还给一个交易所仍持有的订单，
    造成重复下单，比下面的耗尽问题严重得多。
  - **真正的缺口是缺组件，不是逻辑错**：`native/` 里没有任何东西驱动 `Accepted`/`Ambiguous` 订单走向
    终态（`determine_reconcile_action()` 无生产调用点），所以第二个 `mark_resolved()` 调用点还不存在。
    在生命周期驱动器落地前，每一笔非拒绝订单都单调消耗容量，64 笔后提交通道停摆。
  - 行为已由 `test_durable_control_plane_abi.cpp` 的 `InFlightRegistryCapacity.*` 钉死——那两个测试是
    **把现状记录为"正确但不完整"**，不是 bug 报告。
  - **[已关闭，Ambiguous 分支]**：`order_tracker.hpp`（新文件）新增 `OrderTracker`/`poll_once()`/
    `drain_reconcile_events()`，是 `determine_reconcile_action()` 的第一个生产调用点。热线程
    （`live_submit_orchestrator.hpp` 的 `orchestrate_submit()`）与对账线程之间只通过两条
    `SpscRing<T,N>`（`ToReconcileRing`/`ReconcileEventRing`）交接，`InFlightRegistry`/`AuditRingSink`
    继续保持单一写者（热线程），`OrderTracker` 单一写者（对账线程）——没有给这两个结构体加原子量。
    `InFlightRegistry` 新增 `InFlightHandle{slot_index, generation}` + `mark_resolved_handle()`
    防止跨线程陈旧消息误释放被复用的槽位（ABA）。第二个 `mark_resolved*()` 调用点现在存在于
    `drain_reconcile_events()`，仅当 `is_exchange_final(resulting_state)` 为真时才释放。跨线程真实
    并发由 `test_reconcile_concurrency.cpp` 验证（WSL2 TSan 通过，见 `tools/wsl_verify.sh thread`）。
    **仍未关闭的部分**：`Accepted`/`PartialFill`（"仍然存活"而非"已解决"）发现后不会被持续轮询直到
    成交/撤单——那是 spec L4 §6.6 的独立机制，不在这次范围内，见 `order_tracker.hpp` 文件头的
    "明确排除的范围"。`OrderTracker` 本身仍是纯内存态，进程崩溃后其查询次数/退避进度归零，效率损失
    而非正确性问题（`InFlightRegistry` 现状同等级别，不是新引入的回归）。

### 崩溃恢复 / Freeze 子系统

- `FreezeProbeCredit` — 8 次总尝试上限，durable-persisted attempt count（跨崩溃循环不重置），
  exponential backoff 上限 5 分钟；持久化写入必须走 L4-owned 的
  `DurableControlPlaneSink::append_freeze_probe_attempt()`，**不能**调用 L5-only 的
  `DurableAuditSink::append_durable()`（round 16 曾错误调用后者，重新引入 round 9 已经关闭的
  L4-depends-on-L5 循环依赖，round 17 修复）。
- `wait_ok`（`try_reserve_probe(..., bool wait_ok)` 参数）— **不可信任调用方传入的布尔值**；
  sink 侧必须独立重新验证 `conservative_wait_ms == 0 || matching sink-verified FreezeWaitSatisfied`
  才能 Ack（round 30 修复，此前调用方可以在 wait 真正满足前伪造 Ack）。
- `wait_generation` — compaction 保留/fold 规则仅针对**当前** `wait_generation`；跨代（新的未知 429
  之后）不得重新匹配旧代的 `WaitSatisfied`（round 31 引入，round 32 发现 legacy `gen=0→1` 升级路径
  重新打开了这个漏洞，round 32 修复：绝不 promote legacy Satisfy）。

## 已知的"自我引入"事件时间线（供交叉核查脚本的验证用例）

1. round 14→15：`AuditAppendResult` 缺 `.sequence` 字段（P0 self-inflicted）
2. round 14→15：`OrderRecoveryCheckpoint::own_sequence` 循环依赖（P0 self-inflicted, cross-file）
3. round 14→15：`advance_replayed_fill` 分支遗漏 `record.event` 类型统一（P0 self-inflicted）
4. round 15→16：`ExportOutboxRing` 归属矛盾（P0，round 15 自己的修复引入）
5. round 16→17：`is_exchange_final()` 遗漏 `AbortedPreSend`（P1，随后 round 17 该状态本身被移除，
   这个修复本身作废）
6. round 24：`is_exchange_final()` 错误包含 `Reconciled`（P0 self-inflicted，与 §6.5 权威定义矛盾）
7. round 62→63：ClrAbandoned 收敛逻辑遗漏 A0 `.abd` 步骤（P0）
8. round 63→64：`no Started + no A + (G|T) → Corrupt` 规则在合法 PreSeal-build 崩溃下出现假阳性
   （P0，round 64 引入的规则本身有漏洞）
9. round 71→72：Mode B `PhysicalCleanupPreconditions` 在合法 mid-GC 崩溃后不可构造
   （P0，round 71 自己的修复引入）
10. round 72→73：正文残留 204B/v1 引用与 round 71 已经升级到 316B/v2 的 ABI 矛盾
    （P0 residual，round 72 修复了 ABI 但正文其余引用没跟上）

这份时间线本身就是"改一处漏改兄弟处"模式的证据——`spec_xref_check.py` 至少应该能在给定符号名时，
正确列出上述每一对 round 里两处矛盾表述各自所在的行号，作为脚本本身是否可用的验证基准。

## 维护规则

- 每当审阅（人工或本工具）发现一个新的 self-inflicted / sibling-not-updated bug，先把涉及的符号名
  和权威定义补进`## 不变量清单`，再修正 spec 正文——清单要先于正文修复更新，否则下一次交叉核查还是
  漏的。
- 不追求一次性覆盖全部 7000+ 行 changelog；优先登记 changelog 里明确标注过
  "self-inflicted" / "sibling ... not updated" / "contradicts ... authoritative" 的符号。
