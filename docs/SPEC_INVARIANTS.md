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

### durable 审计日志（`DurableAuditSink`，轨道 A 第四项，最小闭环切片）

上一条提到的"`OrderTracker` 仍是纯内存态"就是这一项要关闭的缺口：进程一崩，`InFlightRegistry`/
`OrderTracker` 的在途订单追踪全部丢失。两份 spec 里"durable 审计日志"字面上是一整套很大的子系统
（HMAC 链式帧格式 + tip anchor 本地文件 + 外部锚定服务网络客户端 + compaction/generation 切换 +
seal journal + L4 冻结子系统持久化 + group-commit 批量提交 SLA）；已和用户确认范围，这一轮只做能
真正闭环解决"崩溃后 InFlightRegistry/OrderRecord 状态丢失"的最小垂直切片。以下是这一轮决定要偏离
spec 字面文本的地方，连同理由——**不是遗漏，是范围裁决**：

- **`AuditRecord`（`audit_trail.hpp`）新增 `resulting_state`/`filled_qty_ticks`/`avg_fill_price_ticks`
  三个字段** [已实现]。不是可选项：`order_tracker.hpp` 的 `drain_reconcile_events()` 对六种
  exchange-final 状态（`Accepted`/`PartialFill`/`Filled`/`Cancelled`/`Rejected`/`Expired`）里的
  Reconciled 结果统一写 `AuditEventType::OrderReconciled`，具体是哪一种、成交了多少，在这三个字段
  加上去之前会在写审计记录的那一刻就永久丢失——`recovery_scan()` 不可能凭空补回来。
  `live_submit_orchestrator.hpp` 里所有带 `client_order_id` 的审计记录（`OrderIntentCreated` 到
  `OrderAccepted`/`OrderRejected`/`OrderAmbiguous` 全部路径，含提前失败的 `PreflightFailed`/
  `RateLimitApproaching`）也同步补上 `ar.resulting_state = result.order.state`，否则逐 COID 重放只
  在"被 reconcile 过"的那一段有精确状态，其余路径全靠 `event_type` 猜，猜不出具体状态。
- **`OrderRecoveryCheckpoint`（`durable_control_plane.hpp`）加 `symbol_id`/`exchange_order_id`
  两个字段** [已实现]，不追随 spec 当前文本（`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md:1289`）那个更大
  的、带 `RecoveredOrderRecord`/`side`/`order_type`/`rules_snapshot_at_submit: SymbolRules` 联查/
  `kMaxEscalated` 升级台账的版本——那个版本本身在代码里从未移植过，追上去会把 symbol registry 快照
  恢复也拖进这一轮。`rules_version`/`query_attempts`/`last_poll_completed_utc_ms` 恢复后一律归零/
  never-polled，这是效率损失不是正确性问题（同上一条 `OrderTracker` 纯内存态的定性）。
- **`DurableAuditSink` 这一轮做成独立的具体类，不继承 spec 里的 `DurableControlPlaneSink`**
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:4707`，14 个纯虚 L4-owned 方法：`append_rate_freeze`/
  `append_freeze_probe_attempt`/`append_seal_journal_apply`/... 已用 grep 核实）。字面继承会强迫这
  14 个方法全部 stub 才能实例化，而这些全部是 L4 冻结/合规子系统的持久化，明确排除在这一轮之外。
- **只写 `DurableRecordType::OrderEvent` 帧**，L4-owned 的其余 16 种记录类型（`RateLimitFreeze`/
  `SymbolRegistrySnapshot`/`GenerationBridge`/`FreezeProbeAttempt`/... ）这一轮都不产生。
- **`ExternalAnchorClient` 真实网络实现不做**，比照 `SubmitPort`/`QueryPort` 先例。`ExportOutboxRing`
  （已有，纯内存）每次 `append_durable()` 成功 Acked 后照常 push 一个 tip 记录进去保留接线点；
  `try_push()` 失败（环满，因为还没有消费者）不 fail/fence 本地写入——本地落盘才是正确性关键的部分。
- **compaction / generation 切换 / seal journal 不做**，日志无限增长，这是已知缺口，等后续单独排期。
- **§6.1.1.2 密钥派生/轮换/KEK 包裹存储不做**：`DurableAuditSink` 构造时接收一个预先派生好的原始
  key，`key_id` 固定为 0，不做轮换。**真实上线前这一点必须补上**，不是可以无限期搁置的简化。
  **[Phase 0 已补上底座，Phase 2 已接入]**——见下方"Phase 0：共享 durability 基础设施"/
  "Phase 2：DurableAuditSink 接入 KeyRing"两个小节；`key_id` 现在是真实值，不再固定为 0，但真正的
  运行时轮换仍然是 Phase 4 的范围。
- **只读写帧格式 v3，不做 v1/v2 历史格式解码**：这个代码库从未写过 durable 帧，没有历史格式要兼容；
  版本号不对时，实际代码路径是 `decode_order_event_frame` 返回 `UnknownVersion`，
  `run_recovery_scan()` 把它和其他非 `Ok`/`Truncated` 状态一样映射成 `RecoveryScanStatus::Corrupt`
  ——**订正**：这条记录原文写的"一律 `IoError`"是描述失真，`IoError` 在这个类里专门留给文件锁/打开
  失败、读日志 I/O 失败、非空日志缺失 tip-anchor 三种场景，版本不匹配走的是 `Corrupt` 路径，不是
  `IoError`。**[Phase 0 已升级到 v4]**——见下方小节，v3 落地当天到升级只隔了一天，零真实数据，
  零迁移成本。
- **HMAC-SHA256 用 vendored 实现（新文件 `sha256.hpp`），不用 `binance_signer.hpp` 已有的 OpenSSL
  封装**：`native/CMakeLists.txt` 里 OpenSSL 只在 `HY_BUILD_DEMO=ON` 时才链接，而 `tools/wsl_verify.sh`
  的 TSan job 和 `CLAUDE.md` 自己文档化的 MSVC 标准验证流程都没有传这个开关——挂在它后面意味着
  durable log 在这个仓库自己的两条"标准验证流程"里都不会被默认测试到，是实质性覆盖率倒退。用 RFC
  4231 + NIST 官方已知答案测试向量钉死正确性，不是凭感觉相信自制实现。
- **这一轮的"启动恢复集成"只做 `recovery_scan()` 输出重建 `InFlightRegistry`，不改
  `live_submit_orchestrator.hpp` 的 Gate 6/8/9**（即 `orchestrate_submit()` 本身还不会真的调用
  `append_durable()`）——那是触及热提交路径 gate 顺序的独立大改动，留到后续排期。

**[已实现]** 以上全部决策已落地：`sha256.hpp`（vendored SHA-256/HMAC-SHA256，RFC 4231 + NIST 已知答案
测试向量钉死正确性）、`durable_frame_codec.hpp`（纯内存帧编解码，spec 定义的 version+record_type+
sequence_number+time_kind+recorded_utc_ms+length+payload+prev_mac+mac 格式，HMAC 覆盖包括 prev_mac
在内的全部字段，构成真正的哈希链）、`durable_audit_sink.hpp`（`DurableAuditSink`：跨平台文件锁
POSIX flock/Windows 无共享 CreateFileA、每次 append 同步 fsync、本地 tip anchor 原子替换含 POSIX
目录 fsync（Windows 侧诚实声明不做等价的目录持久化）、`recovery_scan()` 含截断/校验和损坏两类失败
分离处理 + tip anchor 交叉核对 + 逐 COID 重放）、`checkpoint_to_order_record()`/
`repopulate_in_flight_registry()`（启动恢复集成辅助函数）。

测试阶段（`test_durable_audit_sink.cpp`，真实文件 I/O）抓到的真实问题：
- **GCC-only 编译错误**：`durable_audit_sink.hpp` 只 include 了 `<fcntl.h>`，没有 `<sys/file.h>`。
  `<fcntl.h>` 自己也定义了一个叫 `flock` 的 POSIX record-lock **struct**，和 `<sys/file.h>` 里
  BSD 风格的 `flock()` **函数**同名——没有后者的声明时，GCC 把 `::flock(fd, LOCK_EX|LOCK_NB)` 解析成
  了对 `struct flock` 的聚合初始化，而不是函数调用。MSVC 这条分支完全不存在（Windows 侧用的是
  `CreateFileA` 独占打开），两边工具链都要跑的规则又抓到一次真正的分歧。
- **测试自己的建模错误，不是实现 bug，但值得记录**：最初的"截断写入"测试是先让两次 `append_durable()`
  都真正成功（tip anchor 正确前进到 seq 1），再事后截断日志文件字节——这其实模拟的是"删除日志尾部"
  （tip anchor 指向的 seq 在截断后的日志里已经读不到），必然是 `Corrupt`，而不是真实崩溃场景（真实
  崩溃时，只有日志字节的写入可能被打断，tip anchor 从未被更新到那笔未完成的 append，所以 anchor 应该
  还停在上一笔成功的 seq）。改用"只写入一笔真实帧的前缀字节、绕过 `append_durable()`"才是真正复现
  崩溃场景，验证通过——这个反例反而确认了"anchor 领先于日志尾 = 尾部删除攻击"这条判定规则本身是对的，
  留作 test_durable_audit_sink.cpp 的 AnchorAheadOfTruncatedLogIsCorruptTailDeletion 测试单独钉死这个场景。

验证：MSVC 481/481、WSL2 GCC-14 none/address/thread 三模式全绿（TSan 负控制仍正确报出注入的竞争）、
`spec_xref_check.py`/`spec_enum_diff.py` 均 exit 0。

### Phase 0：共享 durability 基础设施（KEK/密钥轮换底座 + keyed frame v4）

**[已实现]** `DurableControlPlaneSink` 真实实现 / `orchestrate_submit` 热路径接入 / 密钥轮换 /
`ExternalAnchorClient` 总路线图（5 个 Phase）的第一个执行单元。给 `§6.1.1.2` 的密钥轮换建一个真正
可用的底座，并把这个底座接进现有帧格式——**这一轮不做**`DurableAuditSink` 本身切换到用这套底座
（Phase 2，**已接入**，见下方对应小节）、真正的运行时轮换生命周期（Phase 4）、`ControlPlaneSink`
（Phase 1，**已实现**）。

- **KEK 权威来源 = 独立的 0600/0400 权限文件**（用户决策），复用 `env_loader.hpp` 已有的权限检查/
  反符号链接/mlock/安全擦除模式，但独立成新类 KekLoader（`kek_loader.hpp`）——载荷是定长 32
  字节原始 KEK，不是 .env key=value 文本，硬凑复用 SecureEnvLoader 会让权限检查和载荷解析逻辑
  纠缠，所以是并列的新类，不是继承/复用。KEK 文件路径与 Binance API secret 文件路径分开。
- **KEK 包裹算法 = 基于已有 HMAC-SHA256 的 keystream 构造**（用户决策，不引入 OpenSSL、不新
  vendor AES-GCM，理由同 `sha256.hpp` 自己的"两条标准 CI 验证链都要覆盖"原则）：`HY-KEKWRAP-v1`——
  从 KEK 派生独立的 encrypt/tag 子密钥（`HMAC-SHA256(KEK, "HY-KEKWRAP-v1-ENC"/"HY-KEKWRAP-v1-TAG")`，
  密钥分离，不用同一把 key 既加密又算完整性标签）；keystream 按 32 字节分块用
  `HMAC-SHA256(enc_subkey, "HY-KEKWRAP-v1" || key_id || salt || block_counter)` 生成，和明文 HMAC
  key 逐块异或得到 `wrapped_key_blob`；完整性标签
  `tag = HMAC-SHA256(tag_subkey, "HY-KEKWRAP-v1-TAG" || key_id || salt || wrapped_key_blob)`。
  **这不是标准库/第三方提供的 AEAD，是这个仓库自己拼的构造**，没有第三方 known-answer 向量可以
  对照，正确性靠往返测试 + 篡改测试（`test_key_ring.cpp`）+ 结构化推理，测试文件头部必须显式声明
  这一点，不能含糊成"标准算法"。KeyRing（`key_ring.hpp`）持有这套逻辑，未知 `key_id` 查找返回
  明确失败（不是默认值/静默失败），`retire(key_id)` 只做"退役请求一到就真退役 + 安全擦除进程内
  明文副本"，不追踪"谁还在用哪个 key_id"——那是调用方（Phase 2/4）的责任。
- **帧格式 v3 → v4**：当前帧格式（`durable_frame_codec.hpp`）完全没有 `key_id` 字段——已直接读
  代码确认布局是 `[format_version][record_type][sequence_number][time_kind][recorded_utc_ms]
  [payload_length][payload][prev_mac][mac]`，87 字节固定开销，`encode_order_event_frame`/
  `decode_order_event_frame` 对 key 选择完全无感知。v4 在 `format_version`/`record_type` 之后、
  `sequence_number` 之前插入 `key_id: u32 LE`，固定开销变成 91 字节，`kFrameFormatVersion` 从 3
  改成 4。**v3 落地到升级只隔一天，`git log` 核实全仓库历史里没有任何真实 v3 日志产物、没有任何
  deploy/prod 分支**——升版本零迁移成本，和"这个代码库从未写过 durable 帧"这条既有理由完全一致。
  新增两阶段解码能力（先读 `key_id`，查 key，再验证整帧 MAC）供 Phase 2 接入时使用。
- **tip-anchor 的 `key_id` 落地，wire 格式不变**：tip-anchor 布局（`durable_audit_sink.hpp`）本来
  就有 `key_id` 字段（`kTipAnchorSize` 已经把这 4 字节算进去了），只是 `encode_tip_anchor`/
  `decode_tip_anchor` 硬编码成 0/直接丢弃——这一轮让这两个纯函数真正读写 `key_id`。顺带发现并
  修正一个小问题：tip-anchor 的版本字节此前直接复用 `kFrameFormatVersion` 这同一个常量，不是
  独立版本号——拆成独立的 `kTipAnchorFormatVersion`，避免"帧格式以后再变一次"被迫连带绑架
  tip-anchor 格式。**订正一处本轮早先记录里的不准确表述**：原计划设想"这一轮只改这两个纯函数
  本身，不改 `DurableAuditSink` 类的调用逻辑"——实际执行时发现这站不住脚：`durable_frame_codec.hpp`
  的 `encode_order_event_frame` 签名新增了必填的 `key_id` 参数后，`DurableAuditSink::
  append_durable()`（调用帧编码）和两个 `write_tip_anchor()` 平台分支（调用 tip-anchor 编码）
  **不加任何参数就无法编译**——这不是可以绕开的选择，是编译期的硬约束。实际做法：在这些既有调用点
  插入字面量 `/*key_id=*/0u`（明确注释标注"这是编译兼容占位，不是功能变化，真正的 KeyRing 接入是
  Phase 2 的范围"），行为和这一轮之前完全一致（一直都是隐式的固定 key，现在只是变成显式的 0）——
  没有引入任何新的运行时行为，只是把"能编译"这个约束诚实地记下来，而不是假装这一轮真的完全没碰
  `DurableAuditSink`。
- **单写者 + recovery 契约（文档化，Phase 2/4 落地时照做）**：写入必须由单一 owner 线程串行化；
  recovery 遇到未知/已退役 `key_id` 必须直接 fence，不允许"试其他 key"——spec 原文
  （`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md:1230`）已经这么要求，这里只是把它落成这个仓库自己的
  ledger 记录。

`spec_enum_diff.py`/`spec_xref_check.py` 均**零改动**——本轮零新增枚举。

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

**[已实现，最小垂直切片]** 以上三条不变量此前只有行为描述，没有对应的持久化类型可指——`native/
include/hengyuan/durable_control_plane.hpp` 里 L4 §10（`docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2449-
5327`，占整份 spec 文件一半以上，约 35 个类型）此前只转写了 6 个，freeze 子系统的 payload 类型全部
缺失。这一轮补上这三条不变量的 ABI 载体（只转写字段，不实现行为，和这个头文件其余部分同一治理级别：
"no append(), no fsync, no MAC verification, no recovery_scan()"）：

- `SealQueryStatus` — 4 值（`Found=0`/`NotFound=1`/`TransportUnavailable=2`/`Corrupt=3`）。spec 原文
  显式警告**不可复用 `RecoveryScanStatus`**：那个枚举没有 `Found`，且会把"传输层不可用"和"记录确实
  不存在"混为一谈（round 39 P0）。首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:2501`）。
- `FreezeClearKind` — 3 值（`ProbeVerified=0`/`ConservativeWaitCompleted=1`/`OperatorAuthorized=2`），
  为下面 `FreezeClearPayload` 的 clear_kind 字段提供载体，也是 `wait_ok`/`wait_generation` 两条不变量
  第一次有类型可以挂。首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:2570`）。
- `FreezeProbePurpose` — 2 值（`DeadlineOrVerify=0`/`ClockRepublishOrVerify=1`）；`ClockRepublishOrVerify`
  可以在同一响应上以 `ProbeVerified` 清除冻结（round 29 P0 撤回了此前"republish 永不清除"的判断）。
  首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:2616`）。
- `RateLimitFreezePayload` — `FreezeProbeCredit`/`wait_generation` 两条不变量的持久化载体；
  `conservative_wait_ms` 字段是**这一帧自己的**等待贡献量，不是重新陈述的运行时最大值——恢复重放/
  在线所有者都必须对同一 epoch 下的全部帧取 `max(deadline)`/`max(wait)`/`max(wait_generation)`，
  绝不能只看最新一帧（round 23/31）。`pad[3]` 字段是 spec 原文自带的，不是本轮计算出来的对齐字节。
  首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:2576`）。
- `FreezeProbeAttemptPayload` — `FreezeProbeCredit`（8 次上限 + 指数退避上限 5 分钟）的持久化载体；
  `cleared` 字段写入时必须是 `false`（sink 拒绝调用方传入 `true`）。首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:2623`）。
- `FreezeTimeProbeProof` — MAC 覆盖范围含 `freeze_epoch`/`bound_deadline_utc_ms`/`request_nonce`，
  防的是跨 episode 重放，不是第三方可验证证明，只是本地防篡改。首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:2694`）。
- `FreezeClearPayload` — 唯一的终态清除记录；`bound_conservative_wait_ms` 字段**仅供参考**——sink
  必须要求一条独立的、sink 自己验证过的 `FreezeWaitSatisfied` 记录（`wait_ok` 不变量，round 30），
  不能仅凭这个字段数值相等就放行。首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:2715`）。
- `FreezeWaitArmPayload` / `FreezeWaitSatisfiedPayload` — `wait_ok`/`wait_generation`（round 30/31/32）
  现在有了对应的持久化结构；`FreezeWaitSatisfiedPayload` 的 arm_ordinal/arm_frame_seq 字段与对应 Arm
  记录的绑定关系，此前只在不变量文字里隐含，没有类型可以指。首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:2747` / `2760`）。
- `CompactedFreezeWaitEvidencePayload` — compaction-only 单帧折叠替代品；**禁止**用一对
  `FreezeWaitArm`+`FreezeWaitSatisfied` 双帧代替（round 33/34）。首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:2779`）。
- `FreezeEpochWatermarkPayload` — `next_freeze_epoch` 命名的是**下一个未使用的值**，不是"正在消费的
  epoch"（round 21 P0 措辞裁决）；默认值必须是 `1`，不是 `0`——`0` 是保留值。首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:2810`）。

### DurableControlPlaneSink 接口族（L4 §10 接口表面收尾）

**[已实现]** L4 §10（`BINANCE_PRIVATE_REST_L4_SPEC.md:2449-5327`）此前只转写了 6 个原始类型 +
freeze-episode 一轮的 3 枚举 8 struct。这一轮补上 L4-owned 接口本身——三个抽象类
（`DurableControlPlaneSink`/`ExternalAnchorClient`/`OperatorOverrideSidecar`）连同它们方法签名
依赖的 7 个 payload 结构体。补完之后，L4 §10 唯一仍未转写的是 seal-journal/`.xgc` wire-format
家族（§10.1/§10.3，约 2,400 行，密集互相引用），维持既有裁决——"compaction / generation 切换 /
seal journal 不做，日志无限增长，这是已知缺口，等后续单独排期"（上面 durable 审计日志条目原话）。

- `LastRemoteAckedTip` — 追溯性补漏：这是紧邻已转写的 `ExportTuple`/`ExportOutboxRing` 的
  §10.2.1 类型（导出 worker 与 compaction seal 路径共同维护的 tip 断点），此前完全缺失，连文件
  自己的"尚未转写"清单都没点名——本轮修正这个疏漏。首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:4586`）。
- `EndpointWeightConfig` — 唯一来自 §7.4 而非 §10 的类型，因为 `append_weight_config`/
  `recover_control_plane` 硬依赖它才必须挂在这个文件里；已用 grep 核实 `native/include/hengyuan/
  *.hpp` 里此前完全不存在这个符号（只有作为枚举判别符的 `EndpointWeightConfigSet` 是同名不同物）。
  首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:2339`）。
- `SymbolRegistrySnapshotPayload` / `RateLimitUsageSnapshotPayload` / `OperatorOverridePayload` /
  `GenerationBridgePayload` / `GenerationSeal` — 首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:2880` / `2889` / `2903` / `2940` / `2973`）。
  `GenerationBridgePayload::bridge_mac` 的 MAC 域固定用 `new_key_id`（而非"当前生效的 key"）——照抄，
  不要"简化"。`GenerationSeal` 内嵌一个 `GenerationBridgePayload bridge{}` 成员。
- `DurableControlPlaneSink`（`BINANCE_PRIVATE_REST_L4_SPEC.md:4707`）——**口径更正**：14 个
  `append_*` + 1 个 `recover_control_plane` = **15 个纯虚声明**，不是此前准备文档里写的 16；
  `virtual ~DurableControlPlaneSink() = default;` 不是纯虚，不计入。**已知的、本轮刻意不解决的
  接口差距**：`append_seal_journal_apply` 的 spec 签名参数类型 `SealJournalAppliedView` 属于仍然
  排除在外的 seal-journal 家族——本轮只前向声明该类型（不完整类型，仅出现在函数声明里合法，不定义
  它），这保住了接口签名的逐字转写，同时不把 seal-journal wire format 本身提前拖进这一轮。
  `ExternalAnchorClient`（5 个纯虚方法，`:4659`）与 `OperatorOverrideSidecar`（3 个纯虚方法，
  `:4999`）两者均无 seal-journal 依赖，完整转写、无前向声明需要。
- **抽象类治理规则（本文件第一次出现 vtable 类型）**：不再适用
  `is_trivially_copyable_v`/`is_standard_layout_v`；改用 `static_assert(std::is_abstract_v<T>)` +
  `static_assert(std::has_virtual_destructor_v<T>)`。`tools/spec_enum_diff.py` 对方法签名没有
  等价的 diff 能力（只解析 `enum class` 块）——这是本轮明确承认、不解决的验证缺口，补偿手段是
  `test_durable_control_plane_sink_interface_abi.cpp` 里手写的 stub 子类：每个接口一个具体子类
  覆写全部纯虚方法，覆写签名与基类纯虚不完全一致就是编译错误，起到 `is_trivially_copyable_v` 对
  结构体同等的"证明形状为真"作用。
- 补充上面 durable 审计日志条目：`DurableControlPlaneSink` 现在是 `durable_control_plane.hpp`
  里真实存在的类（此前只是 spec 里的假设性引用）；`DurableAuditSink` 不继承它的裁决维持不变，
  本轮不重新审视。

`spec_enum_diff.py`/`spec_xref_check.py` 均**零改动**——前者对本轮新增的 3 个接口类 + 7 个结构体
没有任何 `enum class` 需要处理（match tally 数字不变）；后者的搜索文件列表已经包含
`durable_control_plane.hpp` 与 `account_truth.hpp`，新符号通过 ledger 反引号自动被下一次抓取，
无需手工登记列表。

### Seal-journal Round A（`.xgc` 家族 3 轮之第 1 轮）

**[已实现]** L4 §10 唯一仍未转写的 seal-journal/`.xgc` wire-format 家族体量巨大（约 2,400 spec 行，
密集互相引用），一轮做不完，研究确认自然分成 3 轮：**Round A**（这一轮，id/journal 管理集群 +
`SealJournalAppliedView` 收尾）、**Round B**（未来，Started 五元组：`SealExportStartedWire`/
`SealExportStartedMigrationWire`/`SealStartedCleanupTombstoneWire`/`SealStartedAbandonWire`，
依赖本轮的 `SealJournalIntakeCloseControl` 拓扑字段）、**Round C**（未来，编译意图 GC 家族：
`CompactionCandidateIntentWire`/`CompactionIntentTransitionWire`/`CompactionIntentGcAuthorizedWire`，
耦合最密集、体量最大，研究确认不可再切）。崩溃窗口表格（`BINANCE_PRIVATE_REST_L4_SPEC.md:5080-
5146`）、§10.1/10.2/10.3 程序性说明文字、must-pass 故障注入清单（`:5259-5327`）——两轮并行研究均
确认零新增具名类型，纯行为/程序文本，和 `DurableControlPlaneSink` 方法体本身同一治理级别（只转写
签名不实现行为），不转写，也不做"仅作 TLA+ 模型锚点"式的部分转写（会引用尚不存在的类型，产生
半成品）。

- `SealIdWatermark` / `SealJournalCommitWatermark` — 首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:3009` / `3028`）。`SealIdWatermark` 的 next_candidate_id/
  next_request_id 字段默认值是 **1 不是 0**（"next_* is the NEXT allocatable value"，同
  `FreezeEpochWatermarkPayload` 的 next_freeze_epoch 字段裁决同源）。
- `kMaxSealHandoffProducers`/`kSealJournalIntakeCloseDeadlineMs`/`kSealJournalIntakeCloseMaxPollIters`/
  `SealHandoffRingId`（类型别名）— 首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:3050-3057`）。
  spec 原文自带 `static_assert(kMaxSealHandoffProducers <= 8, ...)`（`registered_producer_mask` 是
  `uint8_t`，位宽上限），照抄，不省略。
- **`SealJournalIntakeCloseProducerSlot`/`SealJournalIntakeCloseControl` — 本文件第二次出现
  atomic 承载的 RAM-only 治理类型**（第一次是 `ExportOutboxRing`）：spec 原文显式声明"RAM control
  block; NOT a durable breadcrumb"，两者都含 `std::atomic<...>` 成员，**不是**
  `is_trivially_copyable_v`。治理沿用 `ExportOutboxRing` 已有先例：`struct`（不是 `class`，因为
  spec 里零方法体、零行为，和 `ExportOutboxRing` 不同）+ 显式 `= default` 构造函数 + 四个
  `= delete`（拷贝/移动构造+赋值——一旦声明拷贝构造 `= delete`，编译器会抑制隐式默认构造函数生成，
  这一步不能省略）+ `hy::kCacheLine` 替换 spec 原文的
  `alignas(std::hardware_destructive_interference_size)`（GCC 12+ 对裸 std 常量报
  `-Winterference-size`，本仓库 `-Werror`，`ExportOutboxRing` 自己的注释已经解释过这条偏离，不
  重复）。spec 第 3092-3095 行有一段**注释掉的伪字段**（`close_deadline_steady`）——不是真实成员，
  转写时保留为注释，不当作真字段加入。
- `SealJournalTombstoneWire` — 首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:3116`）。
  和这个头文件其余每一个"Wire"类型同规矩：C++ struct 本身只有 `mac[32]`，完整 108 字节 packed
  布局只记在注释里，不展开成真实字段。
- `SealJournalOriginKey` — 首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:4363`）。spec
  原文明确 `entry_mac` **故意不**是这个 de-dup key 的一部分。
- `is_seal_journal_embeddable_type` — 首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:4373`）。对 `DurableRecordType` 17 个值里的 8 个返回
  `true`（`OrderEvent`/`OrderCheckpoint`/`RateLimitFreeze`/`FreezeEpochWatermark`/
  `FreezeProbeAttempt`/`FreezeClear`/`FreezeWaitArm`/`FreezeWaitSatisfied`），其余 9 个 `false`——
  已用完整枚举值核对穷举覆盖，不是抽样。
- `kSealJournalFormatVersion`/`kSealJournalFixedMetaBytes`/`kSealJournalMaxEmbeddedBytes`/
  `kSealJournalMaxEntryBytes` — 首次移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:4448-4452`）。
  最后一个是求和表达式（`kSealJournalFixedMetaBytes + kSealJournalMaxEmbeddedBytes` = 4234），spec
  自己就没写成独立字面量，转写时保留表达式形式。
- **`SealJournalAppliedView` 从前向声明变成真实定义**（`BINANCE_PRIVATE_REST_L4_SPEC.md:4478`）——
  这是本轮的核心收尾：`DurableControlPlaneSink::append_seal_journal_apply` 此前因为这个类型是
  不完整类型而无法在测试里真正调用，本轮之后和其余 14 个方法一样可以构造真实参数、通过基类引用
  真正调用；`test_durable_control_plane_sink_interface_abi.cpp` 相应更新，删除此前描述这个限制的
  文件头注释段落。类型本身的定义位置也从"紧邻 `DurableControlPlaneSink` 类之前的前向声明"移到
  "和 Round A 其余家族成员一起、更靠前的位置"——`DurableControlPlaneSink` 现在像看到
  `GenerationSeal`/`RateLimitFreezePayload` 等其他依赖类型一样，自然看到一个完整类型，不再需要
  前向声明这个折中手段。

`spec_enum_diff.py`/`spec_xref_check.py` 均**零改动**——本轮零新增枚举；`durable_control_plane.hpp`
已在 xref 搜索列表里，新符号靠 ledger 反引号自动被下一次抓取。

### Seal-journal Round B（Started 五元组，`.xgc` 家族 3 轮之第 2 轮）

**[已实现]** 按上一小节写死的 3 轮划分，这一轮做 Round B——`SealExportStartedWire`（含别名
`SealExportStarted`）、`SealExportStartedMigrationWire`、`SealStartedCleanupTombstoneWire`、
`SealStartedAbandonWire`。Round C（`CompactionCandidateIntentWire`/`CompactionIntentTransitionWire`/
`CompactionIntentGcAuthorizedWire`）维持"耦合最密集、不可再切"的既有裁决，这一轮不碰。

- `SealExportStartedWire`（`L`，别名 `using SealExportStarted = SealExportStartedWire;`）——首次
  移植 this round（`BINANCE_PRIVATE_REST_L4_SPEC.md:3124-3200`）。v2/238B 当前版
  （`kSealExportStartedFormatVersion=2`/`kSealExportStartedWireBytes=238`）；legacy v1/192B
  （`kSealExportStartedLegacyV1Bytes=192`，fail-closed、immutable、never rewritten）；draft-only
  234B（`kSealExportStartedDraft234Bytes=234`，永远 Corrupt）。MAC 域 `HY-SEALSTART-v2`。**拓扑
  字段（`registered_producer_mask`/`producer_count`/`ring_id[8]`）必须与
  `SealJournalIntakeCloseControl`（Round A）冻结拓扑字节相等**——spec 原文"Topology MUST match
  frozen SealJournalIntakeCloseControl (round-54/55)"，连同"Forbidden: try-all / current-key
  fallback when kek_key_id wrapper missing"这句禁止性文字一并照抄进 MAC 域注释，这是 Round A→
  Round B 唯一的跨轮次绑定关系，Round A 小节（上方）已经预告过。
- **`V`（`seal-export-started.v2`）不是独立类型**——是同一个 `SealExportStartedWire` C++ 类型用于
  第二个文件，只有拓扑元组 + `kek_key_id` 允许和 L 不同（L↔V closed-field-bind 规则，
  `BINANCE_PRIVATE_REST_L4_SPEC.md:3900-3914`），不需要为 V 建第二个 struct——记录这个判断避免
  未来误以为漏转写了一个类型。
- `SealExportStartedMigrationWire`（`M`/`.mig`）——首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:3202-3230`）。`kSealExportStartedMigrationFormatVersion=2`/
  `kSealExportStartedMigrationWireBytes=208`，草稿态 `kSealExportStartedMigrationDraft136Bytes=136`。
  MAC 域 `HY-SEALSTARTMIG-v2`。字段本质是 L/V 两个文件的 SHA-256 摘要 + trailer MAC，不是业务字段
  的拷贝——"sole KEK used to verify L (explicit; no try-all)"这句禁止性文字照抄。
- `SealStartedCleanupTombstoneWire`（`C`/`.clr`）——首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:3232-3289`）。`kSealStartedCleanupFormatVersion=2`/
  `kSealStartedCleanupWireBytes=304`，草稿态 `kSealStartedCleanupDraft176Bytes=176`（"never
  CleanupInProgress; never pad to 304"）。MAC 域 `HY-SEALSTARTCLR-v2`。`phase` 单调递增，5 个值
  （`kSealStartedCleanupPhaseAuthorized=0`/`MGone=1`/`VGone=2`/`LGone=3`/`ClrPending=4`）。
- `SealStartedAbandonWire`（`A`/`.abd`）——首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:3291-3335`）。`kSealStartedAbandonFormatVersion=1`/
  `kSealStartedAbandonWireBytes=192`。MAC 域 `HY-SEALSTARTABD-v1`。`phase` 单调递增，8 个值
  （`kSealStartedAbandonPhaseAuthorized=0`/`CGone=1`/`MGone=2`/`VGone=3`/`LGone=4`/`GenGone=5`/
  `ResumeAuthorized=6`/`AbdPending=7`）；`abandon_reason` 只定义了一个值
  （`kSealStartedAbandonReasonNotFound=1`，spec 字段注释写"AuthenticatedNotFound"、常量名是
  `NotFound`，两处措辞都照抄不统一）。`kSealStartedKindNativeV2=1`/`kSealStartedKindMigratedV2=2`
  两个 `started_kind` 常量被 Cleanup 和 Abandon 两个类型共享，只定义一次。
- **`phase`/`started_kind`/`abandon_reason` 一律 `constexpr std::uint8_t` 命名常量，不用
  `enum class`**——spec 原文对这整个家族的小状态字段（含尚未转写的 Round C 的
  `terminal_disposition`）统一这么写；本仓库已转写的 `RateLimitFreezePayload` 的 source 字段也是裸
  `std::uint8_t` 先例。这四个类型本身又都只有 `mac[32]`（同 Round A 的"Wire 类型只有 mac[32]"规矩），
  这些常量只作为自由 `constexpr` 存在，不是任何 struct 的真实字段。

`spec_enum_diff.py`/`spec_xref_check.py` 均**零改动**——本轮零新增枚举（全部是 `constexpr
std::uint8_t`/`std::uint32_t`/`std::size_t` 常量，不是 `enum class`）；xref 搜索列表已覆盖
`durable_control_plane.hpp`。

### Seal-journal Round C（CompactionCandidateIntent GC 家族，`.xgc` 家族 3 轮之第 3/最后一轮）

**[已实现，声明层]** 按 3 轮划分做完最后一轮——`CompactionCandidateIntentWire`（Intent 生命周期）、
`CompactionIntentTransitionWire`（`.x1`，crash-verifiable 路径证明）、
`CompactionIntentGcAuthorizedWire`（`.xgc`，GC 授权凭证）。**范围边界，务必明确**：这一轮和 Round
A/B 同一治理级别——只转写字段布局/常量/spec 原文注释，**不实现 encode/decode、不实现真实文件发布、
不实现 MAC 验证、不实现恢复状态机、不实现并发所有权协议**。合并后只能宣称"L4 §10 的具名类型声明
已经齐全"，`.x1`/`.xgc` 的持久化、GC、恢复逻辑**没有落地，也不是"P0 已关闭"**——这三个类型和这个
文件里其余每一个"Wire"类型一样只有 `mac[32]` 一个真实成员，`cleanup_auth_flags`/`phase`/
`terminal_transition_mac` 等字段目前只存在于注释里的 packed 布局描述中，代码里没有可读可写的真实
字段。

- `CompactionCandidateIntentWire` — 首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:3375-3447`）。`phase` 5 个值（`kCompactionCandidateIntent
  PhaseBuilding=0`/`Reserved=1`/`StartedPublished=2`/`PostSealFinalizing=3`/
  `AbandonFinalizing=4`）——**这是独立于上面 Round B `SealStartedCleanupTombstoneWire`/
  `SealStartedAbandonWire` 各自 phase 的第三套状态机，命名空间不重叠，语义也不能混用**。合法链只有
  两条：`Building→Reserved→StartedPublished→PostSealFinalizing` 或
  `Building→Reserved→StartedPublished→AbandonFinalizing`；REPLACE 只能沿链单调前进，且必须先有
  对应的 `CompactionIntentTransitionWire`（见下条）。
- `CompactionIntentTransitionWire`（`.x1`）— 首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:3494-3515`）。没有自己的 phase 枚举，`from_phase`/`to_phase`
  引用上面 `CompactionCandidateIntentWire` 的同一套 phase 常量。存在的唯一理由：`Intent.phase`
  单独 REPLACE 无法证明历史路径（跨重启不能验证 no-cross/no-skip），必须靠这个 no-replace 的
  transition 收据链（`prev_transition_mac` 形成哈希链）补上crash-verifiable 的路径证明。
- `CompactionIntentGcAuthorizedWire`（`.xgc`）— 首次移植 this round
  （`BINANCE_PRIVATE_REST_L4_SPEC.md:3602-3689`，这一轮体量最大、注释最密的类型）。延伸本文件第
  124-132 行既有条目（v2/316B 权威版本 + CAPTURE→unlink C/A→CREATE .xgc→
  TipExportProducerResume→GC .x1→clear Intent→unlink .xgc 清理顺序，不重复）。这一轮新增记录：
  `terminal_disposition` 3 个值（`PreSealAbandonClear=0`/`PostSealFinalizingClear=1`/
  `AbandonFinalizingClear=2`）；`cleanup_auth_flags` 是**位标志**（不是顺序枚举）——
  `kCompactionIntentGcAuthFlagJournalDrain=1u<<0`/`PostSealBound=1u<<1`/`GenGone=1u<<2`/
  `ResumeAuthorized=1u<<3`/`GateAbsentAtCreate=1u<<4`，转写时保留 `1u << N` 移位写法而非算成
  十进制字面量。legacy v1/204B（`kCompactionIntentGcAuthorizedLegacyV1Bytes`）继续 fail-closed
  only，不是活跃写入格式。

**里程碑（声明层，非实现层）**：这一轮完成后，`durable_control_plane.hpp` 顶部"尚未转写清单"清空
——L4 §10 除了永久排除的崩溃窗口表格（`BINANCE_PRIVATE_REST_L4_SPEC.md:5080-5146`）和
§10.1/10.2/10.3 程序性说明文字（纯行为文本，和 `DurableControlPlaneSink` 方法体不实现行为同一
治理级别）之外，全部具名类型已经转写。这不等于 L4 §10 已经实现——见下方"为未来 codec/recovery
轮记录的设计输入"。

`spec_enum_diff.py`/`spec_xref_check.py` 均**零改动**——本轮零新增枚举。

#### 为未来 codec/recovery 轮记录的设计输入（这一轮不实现，只记录供复用）

两轮外部（GPT）评审对这三个类型提出了实现级约束，判定为未来 codec/recovery 轮（真正给这些类型写
encode/decode、真实文件发布、MAC 验证、恢复状态机、并发锁）的输入，不是这一轮的缺口——记录在这里
避免真正做那一轮时重新分析一遍 spec：

- **`cleanup_auth_flags` 按 disposition 的合法组合真值表**（需要真实字段可读后才能测）：
  `PreSealAbandonClear` 必须 `bit1..4 == 0`；`PostSealFinalizingClear` 必须
  `bit0(JournalDrain) | bit1(PostSealBound)` 都置位；`AbandonFinalizingClear` 必须
  `bit0 | bit2(GenGone) | bit3(ResumeAuthorized)` 都置位；`bit4(GateAbsentAtCreate)` 只允许在
  recovery reconstruction 路径出现，且此时 `gate_trailer_mac` 必须全零；live path 下 `bit4`
  必须是 0 且必须捕获真实 C/A 的 trailer MAC；未定义的高位必须是 0。
- **candidate-scoped 单写者/lease 契约**：`.x1`/Intent/`.xgc` 的写入必须由同一 owner actor 串行化
  （按 `build_nonce` 加锁）；`.x1` 完整落盘 + 父目录 flush 之后才能 REPLACE Intent phase；`.xgc`
  只能由完成终态前置校验的同一 owner 创建；recovery 必须先取得排他所有权才能补 REPLACE/GC `.x1`/
  清 Intent；不允许多个线程或多个 recovery 实例同时扫描清理同一 candidate。
- **CAPTURE→unlink→CREATE `.xgc` 的故障注入矩阵**：capture/校验/unlink/`.xgc` create 之间禁止
  yield；C/A unlink 后、`.xgc` 尚未创建时崩溃只能走 reconstruction 分支；reconstruction 必须重新
  验证 GenerationSeal/bridge/tip/Intent/`.x1`，不能从已删除的 C/A 或进程内缓存恢复；每个关键步骤
  （capture 后/unlink 后/`.xgc` publish 前后/每条 `.x1` unlink 后/Intent unlink 后/`.xgc` unlink
  前后）都需要独立的断电测试点。
- **`terminal_transition_mac`/`intent_mac` 必须是 owner 重新验证后导出的，不能信任调用者传入**：
  创建 `.xgc` 时必须重新走完整 `.x1` 链验证，`intent_mac` 必须等于当前 Intent 的真实 trailer
  MAC，`terminal_transition_mac` 必须等于链上最高 seq 的 `.x1` 真实 trailer MAC，唯一允许全零的
  情况是 Building-only PreSeal clear。
- **性能/内存/文件名边界**（真正写 codec 时的约束，不是这一轮 ABI 声明的约束）：固定
  `std::array<std::byte, 316>` 预分配缓冲，不用 `memcpy` 整个 C++ struct 当磁盘格式、不用
  reinterpret_cast/`#pragma pack`；文件名用固定长度 hex buffer，不用可空 `const char*`／
  `snprintf` 截断；`.x1` 最多 3 条，recovery 应定点读取而非全目录扫描；
  `SealJournalIntakeCloseControl` 的拓扑字段必须先由 owner 复制成不可变快照，再进入任何 MAC 输入，
  不能在编码时读取会被其他线程并发修改的 live 控制块。

### Phase 1：共享 DurableLogStore + ControlPlaneLogSink（以下 DurableLogStore/ControlPlaneLogSink
及其成员方法均为本轮新引入的实现层符号，代码落地前不作为 spec_xref_check.py 反引号登记项）

`DurableControlPlaneSink` 真实实现 / `orchestrate_submit` 热路径接入 / 密钥轮换 /
`ExternalAnchorClient` 总路线图（5 个 Phase）的第二个执行单元，紧接上面"Phase 0：共享 durability
基础设施"小节交付的 KEK/KeyRing/keyed frame v4 底座。这一轮把这套底座接成一个真正能用的
`DurableControlPlaneSink` 具体实现（新类 ControlPlaneLogSink），并顺带抽取一个 `DurableAuditSink`/
ControlPlaneLogSink 两者共用的平台 I/O 层。

**范围决定及其演变**：起点是用户拍板"只做持久化机制，业务规则验证留待后续"——11 个 append 方法
只做真实的 fsync/哈希链帧/tip-anchor/KeyRing 支持的真实 key_id 选择，不实现每个方法自己
SPEC-METHOD 注释里那些跨帧业务校验规则（`wait_generation` 顺序、`attempt_ordinal` 不允许跳号、
compaction 会话 baseline 校验等）。第一版计划据此设计了 `recover_control_plane` 的纯
latest-frame-wins fold，并把文件锁/fsync/tip-anchor 平台代码按 `DurableAuditSink` 现成模式重新
实现一份（不碰那个文件）。用户随后转发一份外部（GPT）评审，指出两处架构级风险，核实后确认都成立，
拍板了两处修正：

1. **纯 latest-frame-wins 会把已被 `FreezeClear` 清除的 freeze 重新报告为 active、把过期
   `WaitSatisfied` 当作当前证据**——完整修复需要跨帧推理，属于业务规则范畴，但这个具体安全隐患
   足够窄、足够关键，值得在这一轮堵上。**决定**：只加两条窄的安全关键 fold 规则（见下方
   ControlPlaneLogSink 条目），append 时序校验（`attempt_ordinal` 不跳号等）依然全部延后——
   这不推翻原有范围决定，只是把"recovery 不能把已撤销的危险状态报告为仍然有效"划进机制范畴，
   而不是业务范畴。
2. **文件锁/fsync/tip-anchor 平台代码不应该在新 sink 里重新抄一份**——会形成两套独立维护的崩溃
   一致性代码，长期语义漂移。**决定**：这一轮抽取共享的 DurableLogStore（`durable_log_store.hpp`，
   新文件），`DurableAuditSink` 重构为委托调用——**纯重构，不改变对外行为**，验证依据是
   `test_durable_audit_sink.cpp` 本轮零改动、原样重跑全绿。

**这一轮仍然不做**：11 个 append 方法的跨帧业务校验规则本体（`wait_generation` 顺序、
`attempt_ordinal` 不允许跳号、`arm_ordinal`/`satisfaction_ordinal` 引用完整性、compaction 会话
baseline 校验等）——刻意延后，`recover_control_plane` 里 `out_uncleared_epoch_count` 仍是 0/1
简化、`out_has_wait_arm` 仍是纯 latest-wins。真正的运行时密钥轮换生命周期（Phase 4 的范围）也不做
——ControlPlaneLogSink 的 active_key_id 构造后不可变，没有运行时切换接口。

- **DurableLogStore（新文件 `durable_log_store.hpp`）**——非多态、非 ABI 冻结的内部实现细节类
  （不同于 `DurableControlPlaneSink` 这种 spec-pinned 接口），把 `DurableAuditSink` 现有的文件锁
  （POSIX flock/Windows 无共享 CreateFileA）、open/close、append_and_fsync、tip-anchor 原子替换
  （`.tmp`写+rename/MoveFileEx+父目录fsync）原样搬迁。新增两个读取原语：read_whole_log（行为
  与 `DurableAuditSink` 现有 `read_entire_log` 逐字节等价，供其委托调用，是"零行为变化"验证的
  核心）、read_chunk（有界流式读取，仅供 ControlPlaneLogSink 使用——`DurableAuditSink` 不改
  行为，继续用整份读入）。
- **`DurableAuditSink` 的重构（改 `durable_audit_sink.hpp`）**——私有面新增一个 DurableLogStore
  成员，7 个私有 I/O 方法体替换为对它的薄委托调用。类的公开接口/行为一律不变；本轮**不改**它的
  `key_id=0u` 硬编码调用点（那是 Phase 2 的范围，与这次重构无关）。
- **ControlPlaneLogSink（新文件 `control_plane_log_sink.hpp`）**——`DurableControlPlaneSink` 的
  真实实现，基于 DurableLogStore：
  - active_key_id 是构造参数，**构造后不可变**（`const` 成员），没有运行时切换接口；每次
    `append_*` 都做一次实时的 `KeyRing::active_key()` 查找（从不跨调用缓存）。查找失败（未知/
    已退役 key_id）→ append 返回 `Failed`，**不 fence**（可恢复的配置错误）。
  - **恢复时 key 解析的处理不同于 append 时**：`peek_frame_key_id()` 读出每帧自己的 key_id，
    查找失败（未知/已退役/环境不匹配）→ **`Corrupt`（fence）**——已落盘、MAC 链上的帧签名 key
    在恢复时解析不出来，说明当前进程的 `KeyRing` 环境和写入时不匹配，唯一安全选择是拒绝恢复。
    前提条件：调用方必须在构造这个 sink 之前，把日志里所有仍被引用的 key_id 都通过
    `load_wrapped_key()` 加载进 `KeyRing`。
  - `recover_control_plane`：对多数记录类型是 latest-frame-wins（同 Phase 0 之前的原始设计），
    但对 `RateLimitFreeze`/`FreezeClear`/`FreezeWaitSatisfied` 三者加两条**安全关键** fold 规则：
    (a) 扫描到 `freeze_epoch` 匹配（或更晚）的 `FreezeClear` 帧时，`out_has_freeze` 置 false，
    之后若有更新的 `RateLimitFreeze` 帧再重新置 true；(b) `out_has_wait_satisfied` 只有在折叠出的
    最新 `WaitSatisfied`/`CompactedFreezeWaitEvidence` 帧的 `wait_generation` 等于折叠出的
    `out_active_freeze.wait_generation`（且 > 0）时才为 true，跨代残留证据一律视为不满足。这两条
    是这一轮唯一从"业务规则"里挑出来实现的部分。8 槽 `FreezeProbeAttemptPayload` 环：索引 0 最老，
    最后一个已填充索引最新。
  - 恢复扫描是**流式**的（通过 DurableLogStore 的 read_chunk 原语，按帧边界读取，先校验
    `payload_length` 不超过本轮新引入的 kMaxControlPlaneFrameBytes 常量再分配/读取），不整份读入文件——防止
    恶意/损坏文件在启动期把内存吃爆；这一点特意不同于 `DurableAuditSink`（后者保留整份读入以维持
    行为不变）。
  - `supports_compaction()` 返回 `false`；`append_compacted_freeze_snapshot`/
    `append_compacted_wait_evidence`/`append_seal_journal_apply` 均返回 `Failed`、不 fence——与
    这个接口里其余"不支持的记录类型返回 Failed"的既有写法一致，不新增 `AuditAppendResult::Status`
    第 3 个值。
- **`control_plane_frame_codec.hpp`（新文件）**——11 种 payload 类型各自的 encode/decode 函数对
  + 帧封装函数，复用 `durable_frame_codec.hpp` 的 `detail::` 原语，不修改那个文件本身（它的定位
  仍是 OrderEvent 专用）。`SymbolRegistrySnapshotPayload` 的变长 `SymbolRules` 数组：本地定义
  一个新常量 kMaxSnapshotSymbols = 64（`durable_control_plane.hpp:531` 注释引用的 `kMaxSymbols`
  常量未在该文件定义，只在不相关的 `binance_json_parser.hpp`/`input_validator.hpp` 里定义，这是
  既存小缺口，非本轮引入）。**防溢出纪律**：decode 时先校验 `payload_length` 派生的 entry_count
  不超过 kMaxSnapshotSymbols，校验通过后才做基于它的乘法/缓冲区定位，不允许先乘后比较——
  `entry_count` 来自不可信的帧头字段。所有 `pad[3]` 等保留字段 encode 时清零、decode 时校验
  必须为零。

`spec_enum_diff.py` 本轮**零改动**（零新增枚举）；`spec_xref_check.py --quiet` 需重跑并通过——
新增文字里非 spec 转写的实现层标识符（DurableLogStore、ControlPlaneLogSink、
kMaxSnapshotSymbols 等）均未加反引号，避免误报"未在任何搜索文件命中"。

### Phase 2：DurableAuditSink 接入 KeyRing

`DurableControlPlaneSink` 真实实现 / `orchestrate_submit` 热路径接入 / 密钥轮换 /
`ExternalAnchorClient` 总路线图的第三个执行单元。Phase 0（PR #18）建了 KEK/KeyRing/keyed-frame-v4
底座；Phase 1（PR #22）用这套底座建了第一个真实消费者 ControlPlaneLogSink，同时把
`DurableAuditSink` 的平台 I/O 抽成共享的 DurableLogStore，但没有接 KeyRing——`append_durable()`
仍然用字面量 `key_id=0u`，tip-anchor 也是同样的硬编码（见上面"durable 审计日志"条目 §6.1.1.2
bullet 和"Phase 0"小节开篇行）。这一轮把 `DurableAuditSink` 接上真正的 KeyRing，范围完全对齐早就
写好的预告：先支持按 key_id 正确验证，**不要求这一步就有运行时轮换**——真正的运行时密钥轮换生命
周期依然是 Phase 4 的范围，`KeyRing` 本身无内部并发保护这一点也已经在 Phase 1 那轮外部评审里定过
调（运行时轮换的并发安全设计整体留给 Phase 4），这一轮不重复展开。

设计上尽量镜像 ControlPlaneLogSink（Phase 1 建立的先例），但有三处刻意不同，照搬会引入真实回归：

1. **`finalize_scan_with_anchor_check()` 保留 4 参数签名和"迟到 fsync 窗口"容忍分支**——
   `DurableAuditSink` 版本比 `ControlPlaneLogSink` 的同名方法多一个 `macs_by_sequence` 参数，
   包含一段 `anchor.sequence_number < log_tip_sequence` 时仍可判 Clean 的容忍逻辑（只要 anchor
   声称的 mac 匹配日志里对应 sequence 的真实帧 mac）。这一轮只换这个方法内部解析 anchor 签名
   key 的来源，不向 `ControlPlaneLogSink` 的 3 参数版本看齐。
2. **`peek_frame_key_id()` 失败不等于 `Corrupt`**——它自己的头注释明确写了"返回 false 不代表
   Corrupt"，只需要 6 字节；`decode_order_event_frame()` 自己的 Truncated 判断需要 27 字节。
   如果 peek 失败就直接返回 `Corrupt`，会把真正的断尾崩溃（残留字节 < 6）误判成损坏——round-6-P0
   torn-write-vs-corruption 区分本身要保护的场景。正确处理：peek 失败时用全零哑元 key 继续调用
   `decode_order_event_frame`，让它自己的 27 字节长度检查接管，正确分类成 `Truncated`；只有
   peek **成功**之后 `key_ring_.active_key()` 查找失败，才是真正的"签名 key 在当前环境解析不出
   来"，返回 `Corrupt`。
3. **tip-anchor 改为 peek 自身的 `key_id` 字段，而不是直接假设 active_key_id_**——新增
   `detail::peek_tip_anchor_key_id()`（同 `peek_frame_key_id` 的哲学：只读 key_id 字段，不验证
   MAC，返回 false 只代表长度不够）。MAC 验证通过之后，还新增一条一致性校验：**非空日志时，
   anchor 自己的 `key_id` 必须等于构造函数传入的 active_key_id_，不一致即 `Corrupt`**——防止
   "重启时把 active_key_id 参数悄悄换成别的值，却被静默当成有效接受"这种配置错误/未授权身份
   切换。用现有的 `RecoveryScanStatus::Corrupt`，不发明新枚举值。

其余改动：构造函数签名变为 `(const std::string& path, KeyRing& key_ring, std::uint32_t
active_key_id)`；移除原来持有原始 key 的私有成员和手工 HMAC block-derive 逻辑（职责移交
`KeyRing`，Phase 0 已建立）；`append_durable()` 每次实时 `key_ring_.active_key(active_key_id_, ...)` 查找（实现层
私有成员命名，不作为登记符号），失败 `Failed` 不 fence（可纠正的配置错误，同 Phase 1
ControlPlaneLogSink 内部 append_generic 辅助方法的先例，这是本轮**唯一新增**的 Failed-不-fence
路径——既有的"encode/I/O 失败一律 fence"行为不变，不是这一轮范围）；`ExportTuple.key_id` 从字面量
`0` 改成 active_key_id_；没有 `set_active_key_id()`（Phase 4 才做）。

**明确排除的范围**（外部评审提过、核实后判定不适用/超出这一轮边界，附理由，避免未来误以为遗漏）：

- **legacy `key_id=0` 数据迁移不做**：`git log` 确认这个仓库从未写过真实 durable 帧（`SubmitPort`
  仍是 mock-only），零真实部署数据=零迁移成本——同 Phase 0（v3→v4）、Phase 1（DurableLogStore
  重构）已经用过两次的理由。
- **不可变 key 快照架构不做**：Phase 1 那轮外部评审已经提过同一个问题，用户当时的拍板是"运行时
  轮换的并发安全设计整体留给 Phase 4"，这一轮不重新翻案。
- **`BinanceEnvironment` 环域绑定/独立密码学评审不做**：Phase 0 已经最大限度披露过 HY-KEKWRAP-v1
  "不是标准 AEAD"的边界；"上线前需要真正的密码学评审"是 `CLAUDE.md` 本身早就声明的、这整个仓库
  的常态背景，不是这一轮的缺口；环域绑定是超出"接入 KeyRing 做 key 选择"范围的架构改动。
- **recovery 改流式扫描不做**：和"接入 KeyRing"无关，且正面违反 Phase 1 刚定的"`DurableAuditSink`
  保持整份读入以维持行为不变"这一决定。

**测试范围**：`test_durable_audit_sink.cpp` 16 个既有用例，15 个只改构造机制（`DurableAuditSink
sink(base_path_, test_key())` → `DurableAuditSink sink(base_path_, *key_ring_, 1)`），断言不变；
原有的"错误 key 重启即 Corrupt"用例重新表述为"用一个从未加载过该 key_id 的 KeyRing 做恢复"，镜像
Phase 1 ControlPlaneLogSink 测试套件里对应的未知帧签名 key 场景（并补上原来漏掉的 `fenced()`
断言）。新增 3 个测试：MAC 验证成功但 anchor 的 key_id 与 active_key_id_ 不一致依然拒绝（直接
验证第 3 条设计要点）、断尾 < 6 字节场景（直接验证第 2 条设计要点的 torn-tail 修正）、跨真正独立
`KeyRing` 实例的持久化重建（用 `add_key()` 拿到的 wrapped-key 记录在一个全新 `KeyRing` 对象上
`load_wrapped_key()`，而不是像其余测试那样跨"重启前后"复用同一个内存 `KeyRing` 对象——这是外部
评审指出的一个真实测试质量问题，之前包括 Phase 1 的 ControlPlaneLogSink 测试套件在内都没有真正
测过这条路径）。

`spec_enum_diff.py`/`spec_xref_check.py` 均**零改动**——本轮零新增枚举。

### Phase 3：orchestrate_submit() 接入热提交路径（durable-before-send）

`DurableControlPlaneSink` 真实实现 / `orchestrate_submit` 热路径接入 / 密钥轮换 /
`ExternalAnchorClient` 总路线图的第四个执行单元。Phase 0-2（PR #18/#22/#23）建好了
KeyRing/DurableLogStore/`DurableAuditSink` 底座并接上了真实 key 选择，但 `orchestrate_submit()`
（`live_submit_orchestrator.hpp`）从未真正调用过 `DurableAuditSink`——热路径至今只写内存态的
`AuditRingSink`（fire-and-forget，进程崩溃即丢失）。这一轮实现
`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §3/§6.2/§6.4（**权威出处订正**：早期一轮 subagent 调研
把这部分内容误标成本文件的行号——本文件只有 780 行，不存在那些行号；真正的出处已直接读源码核实，
是 `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` 的 §3 行748-869、§6.2-6.4 行1546-1672）规定的
durable-before-send 纪律：网络发送前必须先拿到 `OrderIntentCreated`（CONFIRM 之前）和
`OrderSubmitPrepared`（发送前）两个真实落盘 Ack，响应结果本身也必须先落盘 Ack 才能转态/释放
in-flight 槽位。

**新增符号**：
- `AuditEventType::OrderSubmitPrepared = 20`——顺序取现有枚举末尾的下一个空位。两份 spec 都从未
  把 `AuditEventType` 转写成真正的 ` ```cpp ` 代码块（只在 prose 里提及这个记录类型），因此这个
  新增值**完全不会被 `spec_enum_diff.py` 感知**——它只 diff 从 spec 代码块里提取出的枚举定义；
  这里不能声称这条变更会被那个工具捕获，正确性靠 `spec_xref_check.py` + 人工审阅。
- `OrchestratorGate::AuditWriteNotAcked = 22`——spec 钉死的数值
  （`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md:845`），延续 `SubmitStaleRulesVersion = 21` 已经建立
  的先例：18-20（`SubmitPartialFill`/`SubmitFilled`/`SubmitRateLimited`）继续刻意留空，不重新编号
  占用。
- `DurableOrderAuditPort`（`live_submit_orchestrator.hpp` 新增，函数指针注入结构体，
  `AppendFn`/`FencedFn`/`user_data`，同文件里 `SubmitPort` 已经建立的风格）+
  `OrchestratorContext` 新增的 `durable_audit` 字段（值类型，未接线时 `is_valid()==false`，按 fenced
  处理）——外部评审 + 用户拍板的依赖注入方式：`durable_audit` 是真实文件 I/O，和 `SubmitPort`
  代表的真实网络 I/O 同一类依赖，这个文件"只有 `SubmitPort` 被注入 fake、其余依赖
  （`AuditRingSink`/`KillSwitch`/`InFlightRegistry`）直接用真实实例"的既有测试哲学对
  `durable_audit` 不适用。`make_durable_order_audit_port(DurableAuditSink&)` 提供薄 adapter，
  给少量真实文件 I/O 集成测试使用；其余既有 gate 逻辑测试改用一个轻量确定性 fake 后端，可以精确
  构造"仅 Intent 失败"/"仅 Prepared 失败"/"仅 Outcome 失败"，不再需要靠破坏磁盘文件这种粗粒度
  手法做故障注入。

**连带修复（必须和新枚举值同一提交，否则留下不安全中间态）**：`durable_frame_codec.hpp` 的
`is_legal_audit_event_type()` 硬编码上界是当前最后一个枚举值（`RateLimitApproaching`）——每次
`decode_order_event_frame` 都会跑这个范围检查。新增 `OrderSubmitPrepared` 后如果不同步把上界抬高
到它，`encode` 阶段不会报错，但任何后续 `run_recovery_scan()`（包括真实进程重启，也包括这一轮
新增的"重启验证 3 帧落盘"集成测试）会把合法的 `OrderSubmitPrepared` 帧误判成 `Corrupt`——这是
追踪 wire 路径才发现的连带缺口，两份 spec 的 prose 都没有明写。

**第二处连带修复（实现阶段实测发现，不是设计阶段能预见的）**：`live_submit_orchestrator.hpp` 新增
`#include <hengyuan/durable_audit_sink.hpp>` 后，MSVC 编译立即在 `order_tracker.hpp`/
`transport_policy.hpp` 里报出一批 `min`/`max` 相关的 C2589/C2059 语法错误——根因是
`durable_log_store.hpp`（经由 `durable_audit_sink.hpp` 传递 include）在 Windows 下 include
`<windows.h>` 时从未定义 `NOMINMAX`，而这次新增的 include 顺序恰好把它排到了
`order_tracker.hpp`/`transport_policy.hpp`（两者都用到 `std::numeric_limits<T>::max()` 一类的
标识符）前面，导致 windows.h 的 `min`/`max` 宏在同一个编译单元里污染了后续代码。这是一个此前
一直存在、但从未被两个 include 顺序同时触发过的潜伏缺陷，不是这一轮设计本身引入的新问题——修复
方式是给 `durable_log_store.hpp`、以及同样有这个隐患的 `kek_loader.hpp`/`env_loader.hpp`（都用
同一段 `WIN32_LEAN_AND_MEAN` + `#include <windows.h>` 写法）统一补上 `#ifndef NOMINMAX #define
NOMINMAX #endif`，随核心实现提交一起落地。

**行为改变**：`Submitting` 转态时机从"in-flight 注册成功后立即转"后移到"`OrderSubmitPrepared`
真正落盘 Ack 之后"——spec 明确指出这是修正一处历史 P0（`OrderState::AbortedPreSend` 当初被引入
就是为了绕开"转态和 in-flight 注册绑死、Gate 7/8 失败路径没有合法转态可撤销"这个坑，后来该状态
本身又被移除，见上面事件时间线第 5 条）。响应结果（Gate 13 的 5 分支 switch）同样先落盘 Ack、
再转态/释放，失败时 `result.order.state` 完全不碰，天然停在进入 switch 时的 `Submitting`，靠
代码顺序自然达成"失败时留在 Submitting"，不需要显式回滚。

**显眼披露（刻意不修，不是遗漏）**：outcome 阶段落盘失败时，订单静默停在 `Submitting`、
`InFlightRegistry` 槽位不释放、也不推入 `ctx.to_reconcile`（spec §6.4 原文："do NOT transition
to Ambiguous in the hope of reconciling in THIS process — that path is unreachable under a
fenced sink"）——如果进程本身不崩溃、不重启，这个订单会在进程剩余生命周期里一直卡在
`Submitting`，没有任何主动告警，直到下一次进程重启由 `recovery_scan()` 重新归类成 `Ambiguous`
才继续 reconciliation。这是 spec 自己的保守设计（fenced sink 下这个进程已经没有安全的方式继续），
这一轮刻意不修。

**明确排除的范围**（外部评审提过，核实后判定不适用/超出这一轮边界，附理由）：

- **不发明 `fence_and_stop_l5()`/`escalate_via_out_of_band_channel()`**：这两个名字只在 spec
  prose 里松散重复出现，全仓库 `native/` 从未真正定义过任一签名。`durable_control_plane.hpp`
  自己的治理原则是"TRANSCRIBE, NEVER INVENT"。"停止 L5"这个效果已经由 Phase 2 建立的
  `DurableAuditSink` 自我 fence 纪律自动保证——一旦真正 Failed，之后每次 `append_durable()` 都
  立即返回 `Failed`，下一次 `orchestrate_submit()` 会在最早的 Gate 5（Intent）就重新失败，到不了
  CONFIRM/in-flight/发送。`DurableOrderAuditPort::fenced()` 已经足够让调用方按需查询、自行决定是否
  告警。
- **不把 `AuditAppendResult::Status` 从 2 值扩成 4 值**：这个类型的 2 值定义（`Acked=0`/`Failed=1`）
  已经在 Phase 1 的 ledger 里明确记录过"不引入第 3 个值"，这一轮不翻案。规格自己对"Failed"的所有
  子情况都执行同样保守的动作（不转态、不释放、留给 recovery 判定真相），更细的状态区分对正确性
  没有实际增益。
- **不新增跨 context/sink 的共享 cacheline 对齐提交栅栏**：这个代码库从 Phase 0 起反复确认的架构
  前提是单一热线程调用 `orchestrate_submit()`，`DurableAuditSink` 自己的头注释也明写"单写者，
  无内部同步"——多线程并发提交本来就被架构排除，不是这一轮引入的新风险。
- **不给 `OrderSubmitPrepared` record 补新固定大小 wire 类型**：这一轮的安全属性只要求"recovery
  能正确判断订单曾经到达 Prepared 边界"，现有 `AuditRecord` 的 COID/symbol/price/qty/
  `resulting_state` 已经足够；更完整的取证记录是合理的未来增强，不是这一轮的范围。
- **`EscalatedToOperator`/`EscalatedLedger`/`OrderOperatorResolved`/§6.5 容量记账生命周期不做**：
  用户已明确确认这一轮不做，留待后续单独排期——这是 InFlightRegistry 容量耗尽的独立问题，和
  "接入 durable-before-send 门禁"是两件不同的事。
- **`is_exchange_final()`/`is_terminal()` 在释放点的用法维持现状不变**：已核实今天 Gate 13 逐分支
  硬编码"只有 `Rejected` 释放"，恰好和 `is_exchange_final()` 对这 5 个可达结果（`Accepted`/
  `Rejected`/`Timeout`/`NetworkError`/`StaleRulesVersion`）的判定完全一致；把它泛化成真正的
  `is_exchange_final()` 驱动释放是 `PartialFill`/`Filled` 那部分的事，这一轮不做。

**已核实为真、但判定为既有限制、这一轮不处理**：`AuditRingSink` 固定 1024 容量
（`audit_trail.hpp:127`），满了不自动 drain，`can_submit_order()` 报 `Unavailable` 而不是覆盖旧
记录——这是刻意设计（ADR-019 D10），从这个文件存在第一天起 Gate 1（`AuditUnavailable`）就依赖
这个容量上限，和 durable audit 的健康状况完全独立。这一轮没有新增任何 `ctx.audit->append()`
调用点（Prepared 阶段刻意没有内存态副本），不会让这个既有限制变得更差，也不会修它。

**测试范围**：既有 39 个 `orchestrate_submit`/`OrchestratorContext` 测试（`test_live_submit_
orchestrator.cpp` 32 + `test_reconcile_concurrency.cpp` 相关线程用例）改接一个默认"从不失败"的
fake `DurableOrderAuditPort` 后端，断言不变；新增 4 个用 fake 精确构造的故障场景用例（Intent 失败
/Prepared 失败/Outcome 失败/端口完全未接线），直接验证三处"失败时不转态/不释放"的安全关键行为；
新增至少 2-3 个真实文件 I/O 集成测试（真实 `KeyRing`+`DurableAuditSink`+
`make_durable_order_audit_port`），验证端口 adapter 接线正确、3 帧按正确顺序真实落盘且可恢复。
`native/src/live_submit_evidence_harness.cpp` 的 `gate_name()` 穷举 switch（无 `default:`）补
`AuditWriteNotAcked` 一个 case——`OrchestratorGate` 一旦加进这个值，这是硬编译约束，不是可选项；
该文件的测试 fixture 同步接上真实 `KeyRing`+`DurableAuditSink`，否则演示场景会全部先在新 Gate 5
处失败（fixture 是这个 harness 自己的本地结构体名，未登记进 `spec_xref_check.py` 的搜索文件列表，
不作为跨文件核对符号）。

`spec_enum_diff.py` 对本轮两个新枚举值分别确认：`OrderSubmitPrepared` 不参与 diff（两份 spec 都
未把 `AuditEventType` 转写成代码块）；`AuditWriteNotAcked` 走既有的仅追加数值模式，零冲突。
`spec_xref_check.py --quiet` 需重跑并通过。

### Phase 4：真正的运行时密钥轮换生命周期（DurableAuditSink + ControlPlaneLogSink）

`DurableControlPlaneSink` 真实实现总路线图的第五个执行单元（Phase 0-3 均已合并：PR #18/#22/#23/#24）。
`KeyRing`（Phase 0）本身早就支持持有多个 key_id、按 id 查找、按 id 退役，但 `DurableAuditSink`/
`ControlPlaneLogSink` 从 Phase 0 起就反复明确把"真正的运行时密钥轮换"排除在范围外：两个类的
`active_key_id_` 都是构造参数、`const` 成员，构造后不可变，没有 `set_active_key_id()` 或任何运行时
切换入口——每一轮的 ledger 都写着同一句话"运行时轮换的并发安全设计整体留给 Phase 4"。这一轮把这句
反复出现的欠账还上，并按用户明确要求，给轮换动作本身落一条专门的、可验证的 durable 记录，而不是
只靠"后续帧的 key_id 变了"这种间接证据。

**执行过程中的一次真实自我纠错，记在这里避免以后重复踩坑**：最初的设计把这条记录做成
`DurableRecordType::KeyRotated = 17`，调研当时错误地认为两份 spec 文档从未把 `DurableRecordType`
转写成真正的代码块（把它和 Phase 3 `AuditEventType` 的情况搞混了）。`tools/spec_enum_diff.py`
在实现落地后立刻抓到了这个错误——`DurableRecordType` **确实**是一个真正 spec-transcribed 的
代码块（`docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2511-2561`，止于 `SealJournalApplied=16`），该工具
对这类"持久化 wire discriminator"零容忍任何 spec 里没有的新枚举值，判定为 `NAME_CONFLICT`，
build-failing，且没有 delta/白名单机制可以绕过（跟 `OrchestratorGate` 那种 spec 自己标注"...
existing N values unchanged ..."的 delta 区块性质完全不同）。伪造一段"spec 提案"塞进 spec 文档来
让检查通过，比问题本身更违反 TRANSCRIBE 纪律。用户确认后的最终方案：**`KeyRotated` 完全不进
`DurableRecordType`，改成一个独立的、非 spec 治理的全新 wire 记录**（`KeyRotatedRecord`，
`control_plane_frame_codec.hpp`），用一个专属 marker 字节（`0xFE`，刻意选在 `kFrameFormatVersion`
(4) 和共享信封版本号任何可预见的未来增长范围之外）取代 `DurableRecordType` 在共享信封里的结构位置，
扫描时先 peek 这个 marker 字节，是就走独立解码路径，不是才走既有的 `read_control_plane_header`/
`DurableRecordType` 分发。

**关键设计事实**：
- `key_ring.hpp` **零改动**——`add_key()`/`load_wrapped_key()`/`active_key()`/`retire()`/
  `wipe_all()` 早就是完整底座，`kMaxLiveKeys=16` 的注释本身就说明了"key 轮换是罕见的、operator
  触发的事件，不是热路径容量限制"。
- **"writer-quiesce 协议"是免费的，不是这一轮要新设计的东西**：直接读过 `kill_switch.hpp`——
  它是这仓库唯一的另一个"operator 触发、热线程观察"的控制态类，完全没有原子量/跨线程同步原语，
  隐含约定就是"只从 owning 线程调用"。全仓库没有任何一处跨线程原子控制标志的先例；唯一的跨线程
  模式是 `ToReconcileRing`/`ReconcileEventRing` 这一对 SPSC 环，且专用于热线程↔对账线程这一个
  特定边界。`rotate_active_key()` 和 `append_*()`/`run_recovery_scan()` 一样只允许从 owning 线程
  调用，不需要发明任何新同步原语——没有并发写者需要被 quiesce。
- **`KeyRotated` 记录签名约定照抄 `durable_control_plane.hpp` 已有的 `GenerationBridgePayload`**
  （round-12 P0，压缩边界的密钥过渡记录）：帧本身签在 `new_key_id` 下（轮换发生时 `new_key_id`
  已经在 KeyRing 里验证过可解析），`old_key_id` 作为已认证的 payload 数据带在里面——理由原文是
  "绝不能允许验证方为了兼容一次轮换就尝试多个 key，那会把'这条消息是被这一个特定 key 签的'弱化成
  '被我们认识的任意一个 key 签的'"。不是发明新规则，是 TRANSCRIBE 已有先例。
- **`DurableAuditSink` 与 `ControlPlaneLogSink` 对 `KeyRotatedRecord` 的落盘位置不对称，是刻意的**：
  `DurableAuditSink` 的主日志格式从 Phase 0 起就明确写死"只写 `DurableRecordType::OrderEvent`"，
  它的 `run_recovery_scan()` 直接调用 `decode_order_event_frame`，完全没有先 peek 再分发的机制。
  真要把这条记录混进它的主日志，需要教会它的 recovery scan 支持变长帧/多类型分发——这是对一个已经
  稳定四轮的核心文件的真实结构改动，外部评审（用户）确认后采用的方案是：`DurableAuditSink` 用一个
  独立 sidecar 日志（`<path>.keyrotations`/`.keyrotations.lock`/`.keyrotations.tip`，复用 Phase 1
  的 `DurableLogStore`）专门承载这条记录，主日志格式完全不动。`ControlPlaneLogSink` 天生支持多种
  记录类型混在同一日志流（recovery scan 本来就已经是"先读通用 header 再分发"的结构），直接在扫描
  循环最前面加一段 marker 字节 peek 前置分支即可复用同一日志——两个类分别选了"独立 sidecar"和
  "内联主日志"，各自贴合自己已经建立四轮的架构边界。

**新增符号**：
- `KeyRotatedRecord`（`control_plane_frame_codec.hpp`，概念名，无对应 struct——它是一组自由函数 +
  常量）：`kKeyRotatedRecordMarker = 0xFE`（占据共享信封 `format_version` 字节的同一结构位置，
  但语义上跟 `DurableRecordType` 完全无关）、`kKeyRotatedHeaderSize`(22)、`kKeyRotatedFrameSize`
  (=22+16+32+32=102)、`encode_key_rotated_frame`/`decode_key_rotated_frame`/
  `DecodedKeyRotatedFrame`/`peek_is_key_rotated_record`/`peek_key_rotated_record_key_id`——**不复用**
  `write_control_plane_header`/`read_control_plane_header`/`ControlPlaneFrameHeader`（那三者硬编码
  `DurableRecordType` 类型），完全独立手写编解码，字段顺序为
  `[marker u8][key_id u32][sequence_number u64][time_kind u8][recorded_utc_ms i64][payload 16B]
  [prev_mac 32B][mac 32B]`。**`durable_control_plane.hpp` 的 `DurableRecordType` 枚举本身
  这一轮零改动**——`is_legal_durable_record_type()` 的上界也维持 `SealJournalApplied` 不变，
  两者都不需要任何连带修复，因为 `KeyRotated` 压根不经过这条校验路径。
- `KeyRotationPayload{old_key_id, new_key_id, log_sequence_at_rotation}`（16 字节固定大小，struct
  定义在 `durable_control_plane.hpp`，跟其余 payload struct 一致；`kNoPriorTipSequence` 同处）——
  内容不变，只是不再被塞进 `DurableRecordType` 家族的信封。`kNoPriorTipSequence`（`UINT64_MAX`）
  表示轮换发生在还从未写过任何帧的空日志上，没有 tip 可谈。
- **`peek_is_key_rotated_record()`/`peek_key_rotated_record_key_id()` 的"先 peek marker 字节再决定
  怎么解码"是两个消费者（sidecar 扫描、`ControlPlaneLogSink` 混合类型扫描）都必须遵守的前置步骤**——
  `key_id` 在这个格式里位于偏移 1（`[marker][key_id]...`），跟 `durable_frame_codec.hpp` 既有的
  `peek_frame_key_id()`（假设 `[format_version][record_type][key_id]...`，偏移 2）**不是同一个
  偏移量，两者不能混用**——`DurableAuditSink` 的 sidecar 扫描一开始误用了 `peek_frame_key_id()`，
  被 RotationSidecarSurvivesRestart 测试（测试方法名，同下方"测试范围"一节的说明，不加反引号）
  直接跑挂，修正为调用
  `peek_key_rotated_record_key_id()` 后通过；这是实现阶段发现的第二个自我纠错，记在这里防止
  以后又混用。
- `DurableAuditSink::rotate_active_key(new_key_id, now_ms) -> bool`：`active_key_id_` 从 `const`
  改为可变（这是本轮唯一必须打破的既有不变量）；`write_tip_anchor()` 私有方法新增显式 `key_id`
  参数（原来直接读成员），唯一现有调用点（`append_durable()` 内部）显式传 `active_key_id_`，
  零行为改变——`rotate_active_key()` 需要在还没翻转 `active_key_id_` 之前就用 `new_key_id` 签名
  主日志的重新锚定，显式参数是唯一干净的做法（先翻转成员再签名、失败再回滚，违反这仓库反复确立的
  "先落盘、后翻转，绝不翻转后回滚"纪律）。新增 sidecar 相关私有成员
  （`rotation_log_store_`/`rotation_log_open_`/`rotation_fenced_`/`rotation_next_sequence_`/
  `rotation_tip_mac_`）——sidecar 出问题（打不开/损坏）**不 fence 主日志、不影响 `append_durable()`
  正常工作**，只是让 `rotate_active_key()` 从此失败关闭。sidecar 自己的 recovery 大幅简化（只有
  `KeyRotated` 一种固定帧类型，不需要分发；不折叠任何跨记录状态，因为每条轮换记录是永久历史事实，
  不是"未解决、需要恢复"的状态；因此**不维护有界内存历史数组**，跟主日志的 `recovered_checkpoints()`
  语义不同，离线审计需要直接读原始 sidecar 文件）。新增只读访问器 `active_key_id()`/
  `rotation_log_open()`/`rotation_fenced()`。
- **`rotate_active_key()` 的关键正确性设计——重新锚定主日志 tip 关闭"崩溃窗口"**：如果轮换只是
  简单地翻转内存里的 `active_key_id_`、不重新签名主日志现有的 tip-anchor，那么"翻转成功返回"和
  "下一次真正 append 之前"之间存在一个崩溃窗口——此时重启若用新 key_id 构造，会发现 tip-anchor
  还是旧 key 签的，触发 Phase 2 那条"anchor.key_id 必须等于 active_key_id_"一致性检查，被误判成
  `Corrupt`，即使数据完全没问题。所以 `rotate_active_key()` 必须在真正翻转 `active_key_id_` 之前，
  先用 `new_key_id` 重新签名主日志现有 tip 的 anchor（`next_sequence_==0` 的空日志例外，此时没有
  tip 可谈，第一次 `append_durable()` 自然会用新 key 正确写出第一条帧+anchor）。这一步失败按
  `append_durable()` 同样的自我熔断纪律 fence 主日志（真实 I/O 失败，不是可纠正的配置错误）。
- **sidecar 落盘与主日志重新锚定之间没有跨文件原子性，是刻意的、如实披露的取舍**：两步之间崩溃，
  会让 sidecar 显示"一次被记录但未完成的轮换尝试"——这是诚实的取证信息，不是 bug，不需要两阶段
  提交去消除（这本来就是罕见的、operator 驱动的事件，不是需要跨进程协调的高频路径）。
- `ControlPlaneLogSink::rotate_active_key(new_key_id, now_ms) -> bool`：`active_key_id_` 同样从
  `const` 改为可变；`finish_append()` 新增显式 `key_id` 参数（原来直接读成员），两个现有调用点
  （`append_snapshot()`、`append_generic()`）都显式传 `active_key_id_`，零行为改变。
  `rotate_active_key()` **不走 `append_generic()`**——它内部按（还没翻转的）`active_key_id_`
  解析签名 key，这里恰恰需要显式用 `new_key_id` 签，理由同上；直接手写 encode + `finish_append`
  （显式 `key_id`）。`run_recovery_scan()` 的扫描循环在调用既有的 `read_control_plane_header`/
  `switch (hdr.record_type)` **之前**，新增一段 `peek_is_key_rotated_record()` 前置分支——命中就
  走独立的 `decode_key_rotated_frame` 路径（同样校验哈希链、不折叠跨记录状态，理由同 sidecar），
  未命中才继续走原有的 `DurableRecordType` 分发。`KeyRotated` **不是** `switch` 里的一个
  `case`（它压根不属于 `DurableRecordType`），所以这里不存在"漏加 case 会不会被 `-Wswitch`
  捕获"的问题——用不同的机制（marker 字节 peek）从根本上绕开了这个顾虑，不需要像 Phase 3 的
  `gate_name()` 那样依赖穷举 switch 的编译期强制。

**明确排除的范围（附理由）**：
- **不做旧 key 的自动退役**：`KeyRing::retire()` 早就存在，但 L4 spec 自己把"销毁 wrapped blob"
  绑定在压缩/归档密封之后——这个仓库目前没有一条真正在跑的压缩/归档管线。在还不知道旧帧是否已经
  不需要恢复的情况下自动 `retire()` 旧 key 是危险的。这一轮只提供轮换*机制*，退役依然是 operator
  的显式手动动作，同 Phase 0 早就写好的"`retire()` 只管退役本身，谁还在用哪个 key_id 是调用方的
  责任"。
- **不做跨线程的轮换触发通道**：见上方"writer-quiesce 协议是免费的"——真要支持一个独立的 operator
  线程/进程触发轮换，需要一个新的命令通道（例如仿照 `ToReconcileRing` 的 SPSC 环模式），是独立的、
  这一轮没有被要求的功能。
- **不做重启时从 KeyRotated 历史自动推导/校验 `active_key_id_`**：`active_key_id_` 完全来自外部
  构造参数（operator 配置）；一次成功的实时轮换之后，operator 必须记得在下一次重启时把外部配置
  更新成新 key_id，否则重启会被 Phase 2 那条一致性检查判成 `Corrupt`（即使数据完全没问题）。让
  recovery 转而信任日志自己的 `KeyRotated` 历史来推导"当前应该是哪个 key"，是一个更大的信任模型
  变化（把权威来源从外部配置移到日志内容本身），需要单独评估，不是这一轮顺带做的事。这一点作为
  已知的操作纪律显式披露（同 ADR-019 D4"轮换需要有责任人和文档化流程"的既有先例），不是 bug。

**测试范围**（测试方法名是测试文件自己的本地标识符，未登记进 `spec_xref_check.py` 的搜索文件列表，
同 Phase 3 "Fixture" 一条的先例，不作为跨文件核对符号，这里不加反引号）：`test_durable_audit_sink.cpp`
新增轮换用例，含关键回归测试 CrashBetweenRotateAndFirstAppendStillRecoversCleanly（append→rotate
含主日志重新锚定→销毁 sink 模拟崩溃→用新 key_id 重启，验证 `recovery_status()` 不是 `Corrupt`，
直接验证上面"崩溃窗口"设计的正确性）以及 sidecar 跨重启延续性测试。`test_control_plane_log_sink.cpp`
新增对称用例（RotateActiveKeyRequiresNewKeyAlreadyLoaded/
RotateActiveKeySucceedsAndSubsequentAppendUsesNewKey/CrashImmediatelyAfterRotateStillRecoversCleanly/
RotateActiveKeyFailsClosedWhenFenced）加 RecoveryScanAcceptsKeyRotatedFrameWithoutCorrupting
证明扫描循环里新增的 marker peek 前置分支真正生效、没有被现有的 `DurableRecordType` 分发路径
误判/吞掉。

`spec_xref_check.py --quiet`/`tools/spec_enum_diff.py` 均已重跑确认：前者 exit 0，后者
`0 value conflict(s), 0 name conflict(s)`（`KeyRotatedRecord` 完全不经过它的 spec-vs-code 枚举
比对，因为它压根不是任何枚举的成员）。`key_ring.hpp`/`kek_loader.hpp`/`durable_control_plane.hpp`
的 `DurableRecordType` 枚举本身，三者均零改动。

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
