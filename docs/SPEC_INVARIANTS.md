# SPEC_INVARIANTS.md — 机械可核查的规格不变量清单

来源：`docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md`（当前 Revision 73 / Architect review round 72）+
`docs/BINANCE_PRIVATE_REST_L4_SPEC.md`（当前 Revision 19 / round 19；注意 L4 文件内部的 round 计数与
SUBMITPORT 文件不是同一套编号，两者靠 changelog 里的 "see L4 revision N" 互相交叉引用）。

> **方向更新（2026-08，owner 决策）**：真实自动下单已成为明确开发目标——L4 spec（rev 72）是下一
> 实现里程碑，L5 spec（rev 73）紧随其后，两份均为已接受的实现蓝图（见 `CLAUDE.md`）。本清单历史条目
> 中出现的"`SubmitPort` 永久 mock-only"等表述记录的是**当时该轮的范围裁决**，已被本方向更新取代；
> 字段 / ABI / 不变量级别的权威事实不受影响。

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
  - **[R-10 进程内部分已落地，持久锚点未做]**：未知隔离判据（Owner 决策 2026-09-29）取代了原先的
    "第 3 次不可判定即升级"——常量 kMaxQueryAttempts 与成员函数 should_escalate 已从 `OrderRecord`
    删除（此处刻意不加反引号：账本对反引号内的符号要求在被搜索文件里至少出现一次，已删除的符号会让
    检查器误报账本过期）。`determine_reconcile_action(rec, now_mono_ms, now_wall_ms, UnknownQuarantinePolicy)` 在
    ① 实发查询不少于 5 次且自首次进入 `Ambiguous` 起不少于 5000 ms，或 ② 该时长不少于 15000 ms
    （绝对上限，与查询次数无关）时返回 `EscalateToOperator`；三个参数在 `ReconcilePollPolicy` 的
    `quarantine` 成员中可配置，退化配置（`hard_cap_ms <= 0`）失败关闭为升级。经过时间
    取单调时钟与挂钟两个读数中较大者：挂钟只提前、不延后（蓝图 V-07 的唯一例外）。"实发"用
    `QueryOutcome::NotSent` 区分——本地限流拒绝、签名时钟不新鲜、凭据或参数缺失、未接线的
    `QueryPort` 都不计入 `OrderRecord::query_attempts`（原先它们与真实的不可判定同等计数，是
    `binance_private_rest.hpp` 里登记过的 KNOWN COUPLING），但耗掉的时间仍走 ② 的上限。锚点
    `OrderRecord::unknown_since_mono_ms/_wall_ms` 由 `OrderTracker::track()`（`poll_once()` 兜底）在
    对账线程上首次见到 `Ambiguous` 时盖一次戳，此后不再移动。`query_attempts` 饱和于 255，不回绕。
    **未做**：锚点不持久，重启或崩溃循环会重新盖戳，未决累计时间因此可被重置（已知限制 L-30；FI-042
    的崩溃循环子判据仍为 `NOT_TESTABLE_YET`；持久载体归 R-06、R-14）。因此上一条"纯内存态只是效率损失"
    对**升级时点**不再成立：它现在是与正确性相关的已登记限制。

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

---

> **⚠ 冻结通知（2026-08-23，owner 决策）**
>
> 从这里开始的全部 Seal-journal / compaction 条目（Round A–G，共 14 条 `[已实现]`）所描述的
> 子系统**已冻结**：代码全部保留、CI 继续全量跑（含 TSan/ASan 与全部 TLA+ 负控），但**停止
> 扩展**，并已从「走向实盘」的关键路径上移出。理由、精确文件清单、以及**解冻条件**见
> `docs/SEAL_JOURNAL_FREEZE_NOTE.md`。
>
> 下面这些条目**仍然是准确的实现记录**，不因冻结而失效——它们是将来一旦解冻时的续接上下文。
> 冻结的是继续开发，不是既有事实，也不是验证。
>
> 冻结**不影响** `durable_audit_sink.hpp` / `durable_log_store.hpp` / `durable_frame_codec.hpp`
> 这三个真能工作的审计落盘组件（真 fsync、真 MAC 链、真 `recovery_scan()`）——它们不在冻结
> 范围内，且是后续实盘审计的落地方案。

---

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

### Seal-journal Round D（CompactionCandidateIntent codec/genesis 落地，六版方案演进记录）[已实现]

上面记录的 codec/recovery 设计输入的第一个真实实现轮，已落地并通过 MSVC + WSL2/GCC-14 双工具链
验证（见本节末尾"实测证据"）。方案在提交实现代码之前经过六轮外部（GPT）Architect 审查，前五版
均被 REJECT——逐条记录被拒理由，避免以后重新踩同样的坑：

- **第一版**：设想中的 CompactionIntentManager 的 `.xgc` CREATE 依赖调用方传入未经验证的
  GateCaptureEvidence（这两个名字从未落地实现，仅存在于第一版方案文字里）——HMAC 只证明"持
  key 的写入者"，不证明"gate 事实为真"；真实 C/A
  （`.clr`/`.abd`）codec 在这个仓库里完全不存在，任何调用方都能构造一个"看起来合法"的证据签发
  永久性的 GC 授权。
- **第二版**：收回了 `.xgc`-from-evidence，但 `authorize_preseal_building_clear()`（Building
  现状不能证明"没有遗留 Started/journal/C/A/generation 文件"）和通用 `raise_intent_phase()`
  （Intent/`.x1` 自洽不能证明 watermark/started/C-A-bridge-tip-journal 这些本轮不存在的 durable
  事实真的发生过）是同一个问题换皮；另外发现 ID 生命周期规则自相矛盾（Building 要求 id=0、
  Reserved+ 要求 id≠0、immutable 比较又要求 REPLACE 前后逐字节相等，连
  `Building→Reserved` 第一次合法转移都构造不出来），以及 `KeyRing::pin_active_key()` 无参数——
  `KeyRing` 本身允许多个 live key，无参数版本在 rotation 后会选错 key。
- **第三版**：收窄到"codec + genesis-only"（这个方向本身被审查认可，此后未再被推翻），但
  `CandidateLease` 按路径检查身份之后仍按路径打开 artifact——检查和打开之间存在 TOCTOU 窗口，
  candidate 目录可以被 rename/recreate（不需要 symlink），锁保护的是旧 handle，写入却落到新目录。
- **第四版**：修了 directory-handle TOCTOU（POSIX `openat`/`linkat`/`renameat2`，Windows NT
  handle-relative），但 `CandidateLease::dir_handle()` 公开返回 `const CandidateDirHandle&`——
  `const` 只限制 C++ 成员访问，不限制底层文件系统副作用，调用方能保存引用跨线程用、在
  release/move 之后继续用，绕过"同一 candidate 单写者"约束；`PinnedKeyHandle` 只在 **debug
  build** 断言 `KeyRing` 析构时 pin 计数为零，**Release 下是真实的 use-after-free**。
- **第五版**：收回了公开 handle、`KeyRing` 全 build type fail-closed 析构，这两个 P0 被认可
  关闭。但 `CandidateLease` 仍公开接受任意 `std::string_view` 文件名的
  `publish_no_replace`/publish_replace/read_exact/unlink_file（后三个同样从未落地，仅存在于
  第五版方案文字里）——目录 handle 只固定了
  pathname resolution 的起点，**不会自动挡住 `..`**，任何拿到 `CandidateLease` 的代码理论上都能
  用 `../sibling` 之类的名字逃出目录写/删任意 sibling 文件，让"Round D 只有 genesis 一个写操作"
  这句范围声明变成假的；另外 `release()` 是一个没有 owner-thread 检查的 `void` 方法，可以被非
  owner 线程在 owner 正在发起 I/O 时并发调用，关掉一个还在用的 handle。
- **第六版（本轮采用）**：`CandidateLease` 公开面收窄到只剩生命周期状态查询
  （`acquire`/`release`/`held`/`fenced`/`last_identity_diagnostic`），不导出任何 handle 类型；
  真正的读写是唯一 friend `IntentStore` 才能调用的私有 typed 方法
  （`create_intent_genesis_no_replace`/`read_intent_genesis`/`read_x1_frame`），文件名不是
  参数——编译期固定字面量或受限 hex 格式化，物理上没有字符串输入面可以被用来路径穿越；
  `owner_thread_hash_`/`held_`/`fenced_` 全部改成 `std::atomic`，`release()` 返回
  `ReleaseStatus{Released,WrongOwner,NotHeld}`，非 owner 调用在触碰任何 handle 之前就已经
  返回；identity 不匹配变成 sticky fence，附带固定大小、不含堆分配字符串的
  `CandidateIdentityDiagnostic`。

**本轮最终范围**：
1. 三个类型（`CompactionCandidateIntentWire`/`CompactionIntentTransitionWire`/
   `CompactionIntentGcAuthorizedWire`）从 `mac[32]`-only 升格为真实具名字段，加 offset/size
   `static_assert`；真实 encode/decode（固定 `std::array` 缓冲，MAC 域字符串复用既有注释里的
   `HY-COMPINTENT-v1`/`HY-COMPINTENT-X-v1`/`HY-COMPINTENT-GC-v2`，`constant_time_equal` 比较）；
   `.xgc` 204B legacy fail-closed；decode 成功返回不可绕过的
   `VerifiedCompactionCandidateIntent`/`VerifiedTransition` 类型，MAC 已验证是类型层面的前置
   条件而不是运行时的希望。
2. 完整语义校验规则（ID 生命周期两层拆分——永久不可变字段 vs. `Building→Reserved` 唯一一次
   `0/0→nonzero/nonzero`；完整 `.x1` 链校验；`validate_intent_transition()` 纯函数）——为
   Round E/F 固化规则，**本轮生产代码不调用它来产生真实转移**，只在 codec/property 测试里覆盖。
3. `IntentStore`：唯一写操作是 `create_building_intent()`（Intent genesis，phase=Building，
   candidate_id/request_id 均为 0，CREATE_NEW/no-replace）；两个只读诊断
   （`load_and_validate_intent`/`inspect_x1_chain`）+ `load_and_match_building_genesis()`（复用
   codec 的永久不可变字段比较，不重写比较逻辑）。**没有 `raise_intent_phase()`，没有任何 `.x1`
   写入方法，没有任何 `.xgc` 写入方法**——这是本轮反复被审查纠正之后收敛出的边界：genesis
   不依赖任何本轮无法验证的 durable 事实（不需要 watermark/started/gate 证据），是唯一被证明
   安全的写操作。
4. `CandidateLease`：candidate-directory-scoped，文件身份绑定（POSIX `openat`/`linkat`/
   `renameat2`，Windows NT handle-relative，`windows_native_io.hpp` 承载），owner-thread
   confined（atomic 字段，无 mutex），identity mismatch 触发 sticky fence。
5. `KeyRing::pin_key(key_id)`（按 wire 里解出来的 `kek_key_id` 键控，不是无参数版本）+
   `retire()` 迁移到 `RetireStatus`；`~KeyRing()` 全 build type fail-closed（活跃 pin 非零直接
   `std::terminate()`）。
6. `RoundDActual.tla`（只建模本轮真实公开的 API，三个 model switch 对应六轮审查里三个有真实
   反例的安全性发现——HandleRelativeIo=v3 TOCTOU、HandleEncapsulated=v4 handle 逃逸、
   OwnerThreadCheckedOnRelease=v5 release() 无 owner 检查——每个切到 FALSE 都必须让对应
   invariant 违反，否则说明模型已经不再能区分它本该证明的那个历史发现）+ `RoundEFDesign.tla`
   （未来完整设计的独立模型，不接入 Round D 的 production traceability，没有回归控制——本轮
   没有真实事故可以拿来回归，防止被误读为"已实现"）。
7. Clang-only fuzz target（独立 `LABELS fuzz`，带 timeout/seed/RSS 上限，不进默认 CTest）+
   GCC/MSVC 都能跑的确定性 corpus runner（两者调用同一个 fuzz target 函数，不会各测各的）。

**本轮明确不做**（不是疏漏，是六轮审查反复确认过的范围切割）：真实 C/A（`.clr`/`.abd`）codec；
`TipExportProducerResume`（本仓库目前不存在这个符号）；任何 `.x1`/`.xgc` 写入/GC/物理清理；
完整 §10.3 CURRENT-flip 世代切换集成；主 `DurableLogStore` 文件的真实物理 GC/truncate；
publish_replace/unlink_file（Round D 没有真实调用点，留给 Round E/F 按各自需求重新设计）；
TLA+ 模型继续排除 compaction 的完整 fault-injection 矩阵（`formal/README.md` 已声明）。

**Round D/E/F 边界**：
- Round D（本轮）：真实 codec + Intent genesis-only + 只读诊断 + candidate-scoped 租约 + keyed
  key pin。不推进 Intent phase，不写 `.x1`，不写 `.xgc`。
- Round E（未来）：真实 C/A codec；`SealIdWatermark`/`SealExportStarted` 等真实 durable 前置
  事实的 codec/验证（Slice 1 已启动，见下面"Seal-journal Round E Slice 1"小节）；只有这些都存在
  之后，phase 推进能力才能被安全地暴露为 production API（且必须要求调用方持有由这些真实 codec
  产出的不可伪造 receipt，manager 重新验证 receipt+Intent+当前文件状态，不接受裸的 to_phase/ID
  参数）。
- Round F（未来，依赖 D+E）：真实 `.xgc` CREATE/reconstruction；`TipExportProducerResume`；
  `.x1`/Intent/`.xgc` 物理 GC 尾段；主日志真实 compaction/retention；`KeyRing::retire()` 真正
  可用（前提条件解除）。**本轮完成不等于日志无限增长或 `KeyRing::retire()` 可用性问题已解决——
  这两个仍然属于 Round F。**

`spec_enum_diff.py`/`spec_xref_check.py` 需要重跑——三个类型从 `mac[32]`-only 升格为真实字段
（不是新增枚举），新增头文件（`compaction_intent_codec.hpp`/`compaction_breadcrumb_io.hpp`/
`windows_native_io.hpp`/`compaction_lease.hpp`/`compaction_intent_store.hpp`）需要加入 xref
搜索列表。

**实测证据（2026-08-09）**：

- 真实 filesystem bug（不是审查发现，是让代码在真实 NTFS/junction/sharing-violation 上跑起来
  之后才发现的）：`open_directory()` 打开目录 handle 时缺 `FILE_WRITE_DATA`，导致
  `FlushFileBuffers(dir_handle)` 在 Windows 上永远失败（`ERROR_ACCESS_DENIED`）——genesis 写入
  永远只能报告 `PublishedNamespaceUncertain`，从未报告过 `DurablyPublished`，"durable" 这条
  路径事实上不可达。修复后用真实临时目录端到端验证过。
- `CandidateLease::release()`/`check_can_operate()` 的检查顺序 bug：owner-thread 检查排在
  `held_` 检查之前，导致从未 `acquire()` 过的 lease 调用 `release()` 被错误分类成 `WrongOwner`
  而不是 `NotHeld`——同样是跑真实测试才发现，不是代码审查发现。
- 一个平台层面的真实发现（不是 bug，是这一版设计的额外收益）：只要 `CandidateLease` 在某个
  子目录内持有任何缺 share-delete 权限的 handle，Windows 会拒绝重命名/删除该子树内**任何**
  对象（不只是被持有的那一个，包括所有祖先目录），对任何进程都一样——用一次性探测程序验证过
  （remove_all/rename 都失败）。这意味着 Round D 设计要防的那个"外部重命名目录"TOCTOU，在
  Windows 上已经被操作系统自身的 handle 语义结构性挡住了；`RoundDActual.tla` 对应的目录身份
  fencing 测试因此在 test_compaction_intent_store.cpp 里改成 POSIX-only（POSIX `rename()`
  从不关心 open fd，这个场景在 POSIX 上真实可达，由 WSL2/GCC-14 那条验证腿实际执行）。
- MSVC Release 全量 ctest：875/875（compaction intent codec、compaction breadcrumb I/O
  detail、CandidateLease（含真实跨进程互斥测试，独立子进程 helper）、IntentStore（含目录身份
  fencing）、windows_native_io（含真实 NTFS junction 创建验证 reparse-point 拒绝）、fuzz 语料库
  runner 各自的测试套件）。
- WSL2 GCC-14（镜像 ci-native.yml）：900/900，含两个 POSIX-only 的目录身份 fencing 测试与
  跨进程互斥测试。
- `RoundDActual.tla`：主配置 22 states 无违反；三个回归控制（v3bug/v4bug/v5bug）均按预期
  违反各自对应的 invariant；liveness 配置 22 states 无违反（EventualResolution 成立）。
  整个状态空间在构造上就是有限的（8 个布尔量 + 有界计数器），不需要 constraint。
- `RoundEFDesign.tla`：主配置 + liveness 配置各 7 states，均无违反。
- Clang-only fuzz target 本身未在本地环境验证——两个本地验证环境（Windows/WSL2 Ubuntu-24.04）
  都没有 Clang 工具链；GCC/MSVC 均可跑的确定性语料库 runner 已验证，8 个种子文件（含两个针对
  fuzz target 固定 key 的真实 MAC 有效编码）跑通。

### Seal-journal Round E Slice 1（SealIdWatermark/SealExportStartedWire durable-precondition codec）[已实现]

Round D 自己的"Round D/E/F 边界"（上面）声明 Round E 需要"`SealIdWatermark`/`SealExportStarted` 等
真实 durable 前置事实的 codec/验证"。这是 Round E 的第一个切片——只做这两个类型的 codec，不做真实 C/A
（`.clr`/`.abd`）codec，不做 receipt 概念，不做 `raise_intent_phase()` 或任何 phase 推进 API，不接入
`IntentStore`/`CandidateLease`/任何 manager，不写任何文件。方案本身经过一轮外部（GPT）Architect
审查——第一版被 REJECT（把 `SealExportStartedWire` 的拓扑校验错误降级成可选项，会放行 MAC 正确但拓扑
伪造的输入；`SealIdWatermark` 没有 allocator 安全语义），本节是修订后的范围声明。

**`SealIdWatermark`**（`durable_control_plane.hpp:726-736`，字段已经真实、只缺 codec）：
- Wire 顺序：`store_uuid_lo`(u64)/`store_uuid_hi`(u64)/`next_candidate_id`(u64)/`next_request_id`(u64)/
  `mac`(u8[32])，共 64 字节，新增 `kSealIdWatermarkWireBytes = 64`。
- MAC 域：`HMAC(KEK, "HY-SEALIDWM-v1" || store_uuid_lo || store_uuid_hi || next_candidate_id ||
  next_request_id)`（照抄结构体自己的注释）。没有 `format_version`/`total_bytes`/`kek_key_id` 字段，
  也没有 peek-kek-key-id 函数。
- **allocator 安全语义**：decode 拒绝 `next_candidate_id==0` 或 `next_request_id==0`（0 从来不是
  合法的 candidate_id/request_id，一个持久化的 `next_*==0` watermark 本身就是矛盾/已损坏状态）。
  `UINT64_MAX` **不**在 decode 阶段拒绝——一个完整 MAC 校验通过的 `UINT64_MAX` 允许被读出（用于
  recovery 诊断）；真正的上界 fence 是**未来**"reservation API"（推进 watermark 的写路径）递增前的
  职责，Slice 1 没有任何写路径/reservation API，所以这条约束在这一轮没有代码要写。
- **明确不做**：跨文件单调性（同一个 store 连续两次读到的 watermark，后一次的 `next_*` 必须
  >= 前一次）不是单文件 decode 能证明的性质，属于以后 breadcrumb store/recovery 那一层。

**`SealExportStartedWire`**（`durable_control_plane.hpp:910-967`，别名 `SealExportStarted`，本轮从
`mac[32]`-only 升格为真实字段）：
- Wire 顺序照抄 `:917-936` 的偏移注释：`format_version`(u32)/`total_bytes`(u32)/`store_uuid_lo`(u64)/
  `store_uuid_hi`(u64)/`candidate_id`(u64)/`source_generation`(u32)/`baseline_tip_seq`(u64)/
  `baseline_tip_mac`(u8[32])/`baseline_key_id`(u32)/`new_generation`(u32)/`new_final_seq`(u64)/
  `new_final_tip_mac`(u8[32])/`new_key_id`(u32)/`request_id`(u64)/`content_root`(u8[32])/
  `kek_key_id`(u32)/`registered_producer_mask`(u8)/`producer_count`(u8)/`ring_id`(u32[8])/`mac`(u8[32])，
  共 238 字节（`kSealExportStartedWireBytes`，已存在）。MAC 域：`"HY-SEALSTART-v2"`（照抄 `:952-957`）。
  `peek_seal_export_started_kek_key_id` 偏移 168。
- **host struct 不是 wire layout**：238 字节里 u32 后接 u64 会有 C++ 对齐 padding，host `sizeof`
  几乎肯定不等于 238。禁止 `#pragma pack`/`reinterpret_cast` 序列化/`memcpy(sizeof)`/
  offsetof==disk offset/`static_assert(sizeof==238)`；唯一的 wire 权威是 encoder/decoder 里
  显式的逐字段 LE 读写（跟 `CompactionCandidateIntentWire` 一个套路）。ABI 测试删掉
  `sizeof==32u` 之后只断言 `is_trivially_copyable_v`/`is_standard_layout_v` + wire-byte-constant，
  不对 host sizeof/offsetof 下注。
- **拓扑语义校验（`validate_seal_export_started_shape()`，MAC 比较之前运行，fail-fast，跟
  `compaction_intent_codec.hpp` 已有的 `MalformedField` 检查顺序一致）**：
  1. `registered_producer_mask` 只含 `[0, kMaxSealHandoffProducers)` 范围内的 bit 且非零；
  2. `producer_count == popcount(mask)` 且落在 `[1, kMaxSealHandoffProducers]`；
  3. mask 置位的每个 `ring_id[i]` 非零，未置位的每个 `ring_id[i]` 必须为零；
  4. 所有置位槽的 `ring_id` 两两不同；
  5. `candidate_id != 0`、`request_id != 0`、`new_generation == source_generation + 1`（防
     `source_generation==UINT32_MAX` 溢出）；
  6. `kek_key_id`/`baseline_key_id`/`new_key_id` 的 0 值——**不做**非零校验：grep 过
     `key_ring.hpp` 全文，没有找到任何"key_id==0 是保留值/非法值"的既有契约（`Entry.key_id{0}`
     只是内部空槽位哨兵，不是 wire 层面的禁止值；`CompactionCandidateIntentWire.kek_key_id` 本身
     也从未拒绝 0）。既然没有既有契约，就不发明一条没有依据的规则——这是显式记录的开放问题，不是
     遗漏，留给以后如果出现真实契约时再收紧。
  失败一律返回 `MalformedField`（这个 codec 文件自己定义 `SealJournalPreconditionDecodeStatus`，
  不复用 `CompactionWireDecodeStatus`——两个类型族没有实际耦合，复用会造成误导）。
- **decoder 输入长度契约**：跟 `decode_compaction_candidate_intent_wire` 一致，只要求
  `in.size() >= kSealExportStartedWireBytes`（不够 `Truncated`），只读取/认证前 N 字节，不关心
  span 后面是否有多余字节——这不是安全漏洞（MAC 只覆盖前 N 字节内容，附加垃圾字节不改变认证结果），
  "必须恰好 N 字节"这条更严格的契约属于**未来**的 I/O 读取层（照 `read_validated_exact` 的模式），
  这一轮没有 I/O 层代码。

**落地位置**：新建 `native/include/hengyuan/seal_journal_precondition_codec.hpp`，一个文件同时装
两个类型的 codec（spec 自己的"Round D/E/F 边界"原文就是把这两个类型并列成一组"durable 前置事实"）。
Governance: L1（纯内存计算，无文件 I/O）。`VerifiedSealIdWatermark`/`VerifiedSealExportStarted`
私有构造、按值持有（不保存输入/MAC/key span 的引用），跟 `VerifiedCompactionCandidateIntent` 一个
套路；所有函数 `noexcept`；`decode_*` 的失败路径先 `out.reset()`，不允许部分填充。

**本轮明确不做**：真实 C/A（`.clr`/`.abd`）codec；receipt 概念；`raise_intent_phase()`；接入
`IntentStore`/`CandidateLease`/任何 manager；任何文件 I/O；`SealJournalCommitWatermark`/
`SealJournalTombstoneWire`/`SealExportStartedMigrationWire`/`SealStartedCleanupTombstoneWire`/
`SealStartedAbandonWire` 的升格（全部保持现状不动）；`SealIdWatermark` 的跨文件单调性证明；
`SealExportStartedWire` 的 kek_key_id/baseline_key_id/new_key_id 非零校验（开放问题，见上）。

`tools/spec_xref_check.py` 需要把新头文件加入其 SEARCH_FILES 列表。`spec_enum_diff.py` 这轮不新增
`enum class`，预期零新发现。

**实测证据（2026-08-09）**：

- `test_seal_journal_precondition_codec.cpp`（新建，28 个测试）：SealIdWatermarkCodec 测试套件 7 个
  （round-trip、单字节 MAC 篡改检测、错 key、截断、`next_candidate_id`/`next_request_id` 为 0
  拒绝各 1 个、`UINT64_MAX` 且 MAC 正确必须解码成功）；SealExportStartedCodec 测试套件 18 个（round-trip
  含 `ring_id[8]` 逐元素比较、未知 format_version、错误 total_bytes、MAC 篡改、错 key、截断、
  legacy-192B/draft-234B 均按 `Truncated` 拒绝、以及 P0-1 要求的 10 条"MAC 正确但字段非法"语义
  拒绝测试——空 mask、mask 满容量下正确接受、`producer_count`/`popcount` 不匹配、置位槽 `ring_id`
  为零、未置位槽 `ring_id` 非零、重复 `ring_id`、`candidate_id`/`request_id` 为零各 1 个、
  `new_generation` 不等于 `source_generation+1`、`source_generation==UINT32_MAX` 溢出防护）；
  `peek_seal_export_started_kek_key_id` 测试 2 个（MAC 篡改后仍可 peek、缓冲区过短拒绝）；一个 2000
  次迭代的属性测试确认两个 decode 函数对任意随机字节都不崩溃。
- MSVC Release 全量 ctest：904/904。
- WSL2 GCC-14 Release（镜像 ci-native.yml）：931/931。
- WSL2 ASan+UBSan（镜像 ci-native-sanitizers.yml 的 asan-ubsan-full job，含 `HY_BUILD_DEMO=ON`
  的全部二进制）：931/931，无 ASan/UBSan 报告。
- WSL2 TSan 并发测试套件：47/47；两个负控 tsan_control_relaxed_ring/
  tsan_control_export_worker_dual_consumer 均按预期以非零退出码报出
  `WARNING: ThreadSanitizer: data race`（这两个控制跟本轮改动无关——Round E Slice 1 是纯内存
  codec，不碰任何并发/线程代码——之所以仍然重跑，是因为 `tools/wsl_verify.sh all` 的 `thread`
  模式本身就会跑这两个控制，用来证明 TSan 在本机确实还能检测到注入的数据竞争，不是这条验证腿本身
  失效了）。
- `spec_xref_check.py --quiet`：398 个符号、33045 个匹配点，全部可定位。
- `spec_enum_diff.py`：0 个新发现（本轮无新增 `enum class`，符合预期）。

### Seal-journal Round E Slice 2a（SealJournalCommitWatermark/SealJournalTombstoneWire commit/tombstone codec）[已实现]

Round E Slice 1（上面）做了 `SealIdWatermark`/`SealExportStartedWire` 两个 durable-precondition 类型的
codec，并明确把 `SealJournalCommitWatermark`/`SealJournalTombstoneWire` 排除在那一轮之外。这是 Round E
的第二个切片——只做这两个类型的 codec（编码/解码/MAC 验证 + 字段语义校验），`SealJournalTombstoneWire`
从 `mac[32]`-only 升格为真实具名字段；不写任何文件 I/O，不接入 `IntentStore`/`CandidateLease`/任何
manager，不做 receipt 概念，不做 `raise_intent_phase()`，不碰真实 C/A（`.clr`/`.abd`——那是并行轨道
Slice 2b 的范围）。方案本身经过一轮外部 Architect 审查，发现两个真实的规则遗漏并已修订进本条目：
P0-3（两条非零校验规则，见下）与 P1-2（`UINT64_MAX` 上界 fence 的 reader/writer 职责边界，见下）；
另确认一个并行安全要求（独立 worktree 开发，不共享 checkout）。

**`SealJournalCommitWatermark`**（`durable_control_plane.hpp:754-764`，字段已经真实、只缺 codec）：
- Wire 顺序照抄字段声明顺序：`store_uuid_lo`(u64)/`store_uuid_hi`(u64)/`candidate_id`(u64)/
  `highest_committed_journal_seq`(u64)/`kek_key_id`(u32)/`mac`(u8[32])，共 68 字节，新增
  `kSealJournalCommitWatermarkWireBytes = 68` + `static_assert(8+8+8+8+4+32 == ...)`。没有
  `format_version`/`total_bytes` 字段（不要照抄 Compaction 模板硬凑这两个字段，`SealIdWatermark`
  已是先例）。
- MAC 域（LE，无 padding）：`HMAC(KEK[kek_key_id], "HY-SEALJRNHW-v1" || store_uuid_lo ||
  store_uuid_hi || candidate_id || highest_committed_journal_seq || kek_key_id)`（照抄结构体自己的注释）。
- `peek_seal_journal_commit_watermark_kek_key_id`：偏移 `8+8+8+8=32`（有 `kek_key_id` 字段，decode
  前需要先按 key_id 解析 KEK）。
- **语义校验（P0-3 修订）**：decode 拒绝 `candidate_id == 0`——这个 id 由 `SealIdWatermark` 分配
  （`next_candidate_id` 起始于 1），0 从来不是合法的已保留 id。`highest_committed_journal_seq` **允许**
  为 0——结构体注释明确写"0 = none yet"，这是合法的初始状态（"这个 candidate 还没有任何 commit 过的
  journal entry"），**不要**对它加非零校验。
- **`UINT64_MAX` 边界（P1-2 修订，这版提示词新加）**：`highest_committed_journal_seq == UINT64_MAX`
  时 decode **必须允许**读出（不要拒绝）——与 `SealIdWatermark` 的 `next_candidate_id`/`next_request_id`
  对 `UINT64_MAX` 的处理方式同一个先例：一个 MAC 校验完全通过的 `UINT64_MAX` 状态代表这个 counter
  已经用尽，运维需要能诊断出这个状态（比如判断"这个 candidate 的 journal 已经不能再往前推进了"），
  不能因为数值很大就当成损坏拒绝。但这**不代表**未来允许有代码直接对一个 `UINT64_MAX` 的值做未检查的
  `+1`——那会无符号回绕到 0，是真正的 bug 来源。这轮的 codec 不实现任何写路径（没有
  reservation/推进 watermark 的函数），所以这条 fence 规则本身没有代码可写，边界如下：
  1. **reader（本 codec 文件）**：允许读出 `UINT64_MAX`，不允许在 decode 阶段以"数值太大"为由拒绝；
  2. **writer（未来某个负责推进 `highest_committed_journal_seq` 的函数）**：必须在 `+1` 之前
     fail-closed（推进前检查 `== UINT64_MAX` 即拒绝，不许回绕到 0）。
  这条规则在 codec 文件头部注释里明确写清楚，并列为**未来任何实现 journal watermark 推进逻辑的
  Slice 必须验收的一项**，不能被默认忽略。

**`SealJournalTombstoneWire`**（`durable_control_plane.hpp:858-879`，本轮从 `mac[32]`-only 升格为
真实字段）：
- Wire 顺序照抄偏移注释：`format_version`(u32，=1)/`total_bytes`(u32，=108)/`store_uuid_lo`(u64)/
  `store_uuid_hi`(u64)/`kek_key_id`(u32)/`candidate_id`(u64)/`journal_seq`(u64)/`entry_mac`(u8[32])/
  `mac`(u8[32])，共 108 字节（`kSealJournalTombstoneFormatVersion`/`kSealJournalTombstoneBytes` 已存在，
  不重新定义）。MAC 域：`HMAC(KEK[kek_key_id], "HY-SEALJRNTS-v1" || format_version || total_bytes ||
  store_uuid_lo || store_uuid_hi || kek_key_id || candidate_id || journal_seq || entry_mac)`
  （照抄 `:868-870`）。`peek_seal_journal_tombstone_kek_key_id`：偏移 `4+4+8+8=24`。
- **语义校验（P0-3 修订）**：decode 拒绝 `candidate_id == 0` 或 `journal_seq == 0`——`journal_seq`
  从 1 开始严格递增分配（`docs/BINANCE_PRIVATE_REST_L4_SPEC.md:5059`），0 不可能是任何真实 entry 的
  编号。注意与 `SealJournalCommitWatermark.highest_committed_journal_seq` 的规则**不同**——watermark
  的 0 是合法初始值，tombstone 的 0 永远非法，两者含义不同，不要类比错。
- **host struct 不是 wire layout**：108 字节里 u32 后接 u64 会有 C++ 对齐 padding，host `sizeof`
  几乎肯定不等于 108。禁止 `#pragma pack`/`reinterpret_cast` 序列化/`memcpy(sizeof)`/
  offsetof==disk offset/`static_assert(sizeof==108)`；唯一的 wire 权威是 encoder/decoder 里显式的
  逐字段 LE 读写（跟 `SealExportStartedWire` 升格一个套路）。ABI 测试删掉 `sizeof==32u` 那一行之后
  只断言 `is_trivially_copyable_v`/`is_standard_layout_v` + wire-byte-constant，不对 host sizeof
  /offsetof 下注。
- **`entry_mac` 的"明确不做"**：字段注释写"MUST equal the Applied / journal entry_mac"——这是
  **跨文件**一致性要求（要跟另一份 journal entry 记录的 mac 比对），单文件 decode 函数证明不了这件事，
  本 codec 只把它当作一个普通的 32 字节字段编解码，**不**在 decode 里验证这条规则。

**两个类型共同的 codec 规则**（都实现于新文件 `native/include/hengyuan/seal_journal_commit_tombstone_codec.hpp`，
一个文件装两个类型的 codec。Governance: L1，纯内存计算，无文件 I/O；结构参考
`seal_journal_precondition_codec.hpp`，同一套 `detail::write_u32_le`/`read_u32_le`/
`write_u64_le`/`read_u64_le`/`write_u8`/`read_u8`/`write_bytes`/`read_bytes` 原语来自
`durable_frame_codec.hpp`，`crypto::HmacSha256`/`crypto::constant_time_equal` 来自 `sha256.hpp`）：
- 自己定义 `SealJournalCommitTombstoneDecodeStatus` 枚举（`Ok`/`Truncated`/`UnknownVersion`/
  `TotalBytesInvalid`/`MalformedField`/`ChecksumMismatch`，不复用其它 codec 文件的枚举——这两个类型族
  没有实际耦合）。`SealJournalCommitWatermark` 没有 `format_version`/`total_bytes` 字段，其 decode
  只会用到 `Ok`/`Truncated`/`MalformedField`/`ChecksumMismatch`。
- `VerifiedSealJournalCommitWatermark`/`VerifiedSealJournalTombstoneWire`：私有构造函数，成员按值持有
  完整 struct（不保存输入/MAC/key span 的引用），所有函数 `noexcept`；decode 函数的
  `std::optional<VerifiedX>&` out 参数在任何失败路径都先 `out.reset()`，不允许部分填充。
- **MAC 比较之前先做语义校验**（`candidate_id`/`journal_seq` 非零检查），跟
  `compaction_intent_codec.hpp`/`seal_journal_precondition_codec.hpp` 已有的检查顺序一致——fail-fast
  的字段形状检查先于 MAC，MAC 检查是最后、唯一决定"是否可信"的门。
- **decoder 输入长度契约**：接受 `std::span<const std::byte>`，只要求 `in.size() >= kXxxWireBytes`
  （不够就 `Truncated`），只读取/认证前 N 字节，不关心调用者传入的 span 后面是否还有多余字节——这
  不是安全漏洞（MAC 只覆盖前 N 字节内容，附加垃圾字节不改变认证结果），"必须恰好 N 字节"这条更严格
  的契约属于未来 I/O 读取层。
- **写侧 shape gate（这版提示词新加的一条）**：两个 `encode_*` 函数**必须**在写入任何字节之前先调用
  对应的 `validate_*_shape()`（watermark 检查 `candidate_id != 0`；tombstone 检查 `candidate_id != 0
  && journal_seq != 0`）——语义非法直接返回 `0`（不写任何输出字节，不计算 MAC），成功才继续正常的
  逐字段 LE 写入 + HMAC。这是防止未来某个写路径不小心把语义非法的值签成"MAC 正确"的 durable record。

**明确不做**：真实 C/A（`.clr`/`.abd`）codec；receipt 概念；`raise_intent_phase()`；接入
`IntentStore`/`CandidateLease`/任何 manager；任何文件 I/O；`SealJournalOriginKey` 的 codec（该类型
是纯内存去重索引 key，两个字段没有 `mac[32]`，没有 wire 格式，不需要 codec）；`entry_mac` 的跨文件
一致性验证（见上）；`docs/SPEC_INVARIANTS.md` 的 ledger 状态翻 `[已实现]`（留给协调者合并两边工作
之后统一处理）；`tools/spec_xref_check.py` 的改动（同样是协调者统一处理的部分）。

**验收要求（未来 Slice 必须遵守，不能被默认忽略）**：任何实现 journal watermark 推进逻辑（对
`highest_committed_journal_seq` 做 `+1`）的 Slice，必须在 `+1` 之前对 `UINT64_MAX` fail-closed
（见上 P1-2 的 writer 边界），并以测试验收。

**实测证据（2026-08-11，协调者合并 Slice 2a+2b 后统一补入）**：
- test_seal_journal_commit_tombstone_codec.cpp（新建，28 个测试）：SealJournalCommitWatermarkCodec / 
  SealJournalTombstoneWireCodec / Peek 两个 / RefusesToEncodeSemanticallyInvalidInput 两个 / 合并
  属性测试（固定种子 2000 次迭代、长度 `trial % 200`、两个 decode 都跑、断言不崩溃 + 状态闭合 +
  失败时 out 为空 + 成功时重复调用结果一致）——负测试（MAC 正确但 candidate_id/journal_seq 为 0）
  用测试文件内部的 raw signer 辅助函数构造（生产 `encode_*` 现在会拒绝语义非法的输入）。
- `test_durable_control_plane_seal_journal_abi.cpp`：新增 `SealJournalCommitWatermark.WireByteConstant
  MatchesSpec`（`kSealJournalCommitWatermarkWireBytes == 68`）；原计划的 ShapeIsJustTheTrailerMac
  测试因 sizeof==32u 断言不再成立而改名为 WireByteConstantsMatchSpec（删掉 sizeof 断言、保留常量
  断言）；两个类型的 IsTriviallyCopyableStandardLayout 测试原样保留。
- native/CMakeLists.txt 注册新的 test_seal_journal_commit_tombstone_codec 目标（照
  test_seal_journal_precondition_codec 的四件套形状）。
- MSVC Release 全量 ctest（Slice 2a+2b 合并后的集成分支）：983/983。
- WSL2 GCC-14 Release（镜像 `ci-native.yml`，含 `HY_BUILD_DEMO=ON` 的全部二进制）：1010/1010。
- WSL2 ASan+UBSan（定向构建本轮四个相关 test 目标：
  test_seal_journal_commit_tombstone_codec/test_seal_export_migration_cleanup_abandon_codec/
  test_durable_control_plane_seal_journal_abi/test_durable_control_plane_seal_journal_started_abi，
  避开 WSL VM 全量 `HY_BUILD_DEMO=ON` ASan 构建已知的 OOM 问题）：102/102，无 ASan/UBSan 报告。
- TSan：本轮未重跑——两个切片都是纯内存 codec，不碰任何并发/线程代码，Slice 1 的先例（上面）已经
  确认本机 TSan 负控仍能检测注入的数据竞争，不为跟并发无关的改动重复这条验证腿。
- `spec_xref_check.py --quiet`：440 个符号、36088 个匹配点，全部可定位（新增两个 codec 头文件的
  SEARCH_FILES 注册；ledger 正文里若干处误用反引号包裹的非符号 prose——测试名 prose 引用、worktree
  目录名、记忆条目名——改成纯文本，不是真实的 spec/code drift）。
- `spec_enum_diff.py`：0 个新发现（`SealJournalCommitTombstoneDecodeStatus`/`SealStartedWireDecodeStatus`
  均已在代码中定义，跟 spec 无冲突）。

### Seal-journal Round E Slice 2b（SealExportStartedMigrationWire/SealStartedCleanupTombstoneWire/SealStartedAbandonWire durable-precondition codec）[已实现]

Round E Slice 1（上面）给"Round A"/"Round B"两个 Wire 类型集群里的 2 个类型
（`SealIdWatermark`/`SealExportStartedWire`）做了真实 codec。"Round B" 集群（`durable_control_
plane.hpp` 里 `SealExportStartedWire` 紧邻的三个姊妹类型，对应
`test_durable_control_plane_seal_journal_started_abi.cpp` 覆盖的测试文件边界）还有三个仍是
`mac[32]`-only：`SealExportStartedMigrationWire`（`.mig`）、`SealStartedCleanupTombstoneWire`
（`.clr`）、`SealStartedAbandonWire`（`.abd`）。这一轮（Slice 2b）给这三个类型做同样的
encode/decode/MAC 验证 codec，跟 Slice 1 一样的governance（L1，纯内存计算，无文件 I/O，不接入
`IntentStore`/`CandidateLease`/任何 manager，不做 receipt，不做 `raise_intent_phase()`）。

**并行开发说明**：这一轮跟另一个"Round A"集群（`SealJournalCommitWatermark`/
`SealJournalTombstoneWire`，Slice 2a）并行开发，各自在独立的 git worktree
（hengyuan_v2_slice2b/hengyuan_v2_slice2a）里进行，避免共享 checkout 导致互相打断工作目录
（上一轮 Round E Slice 1 + 形式化建模那次并行工作真实发生过两次分支切换互相干扰，详见记忆
project_shared_workdir_branch_collision）。`tools/spec_xref_check.py` 的注册和这个 ledger
条目从 `[计划中]` 翻成 `[已实现]`，都统一放到两边合并之后由协调者做，不在各自轨道自己的 commit
里做。

**字段表**（已从 `durable_control_plane.hpp:1007-1159` 核实，不是猜的）：

- `SealExportStartedMigrationWire`（208 字节）：`format_version`(u32)/`total_bytes`(u32)/
  `store_uuid_lo`(u64)/`store_uuid_hi`(u64)/`candidate_id`(u64)/`request_id`(u64)/
  `legacy_kek_key_id`(u32)/`v2_kek_key_id`(u32)/`legacy_file_digest`(u8[32])/
  `v2_file_digest`(u8[32])/`legacy_mac`(u8[32])/`v2_mac`(u8[32])/`mac`(u8[32])。MAC 域用
  `v2_kek_key_id`：`HMAC(KEK[v2_kek_key_id], "HY-SEALSTARTMIG-v2" || format_version ||
  total_bytes || store_uuid_lo || store_uuid_hi || candidate_id || request_id ||
  legacy_kek_key_id || v2_kek_key_id || legacy_file_digest || v2_file_digest || legacy_mac ||
  v2_mac)`。peek 对 `v2_kek_key_id`，偏移 `4+4+8+8+8+8+4=44`。
- `SealStartedCleanupTombstoneWire`（304 字节，23 个字段）：`format_version`/`total_bytes`/
  `store_uuid_lo`/`store_uuid_hi`/`candidate_id`/`request_id`/`kek_key_id`/`started_kind`(u8)/
  `present_mask`(u8)/`phase`(u8)/`reserved0`(u8)/`source_generation`/`baseline_tip_seq`/
  `baseline_tip_mac`/`baseline_key_id`/`new_generation`/`new_final_seq`/`new_final_tip_mac`/
  `new_key_id`/`content_root`/`digest_L`/`digest_V`/`digest_M`/`mac`。MAC 域见
  `durable_control_plane.hpp:1095-1101`。
- `SealStartedAbandonWire`（192 字节，17 个字段）：`format_version`/`total_bytes`/
  `store_uuid_lo`/`store_uuid_hi`/`candidate_id`/`request_id`/`kek_key_id`/`started_kind`(u8)/
  `abandon_reason`(u8)/`present_mask`(u8)/`phase`(u8)/`source_generation`/`baseline_tip_seq`/
  `baseline_tip_mac`/`baseline_key_id`/`content_root`/`digest_C`/`mac`。MAC 域见
  `durable_control_plane.hpp:1149-1153`。

**落地位置**：新建 `native/include/hengyuan/seal_export_migration_cleanup_abandon_codec.hpp`。
自己的 `SealStartedWireDecodeStatus` 枚举（`Ok`/`Truncated`/`UnknownVersion`/
`TotalBytesInvalid`/`MalformedField`/`ReservedNonzero`/`ChecksumMismatch`，不复用 Slice 1 的
`SealJournalPreconditionDecodeStatus`——两个类型族没有实际耦合）。

**Decode gate 顺序**：长度 gate（`in.size() >= kXxxWireBytes`，不够 → `Truncated`）→
`format_version`/`total_bytes` 校验（不等于活动版本 → `UnknownVersion`；`total_bytes` 不等于
对应常量 → `TotalBytesInvalid`）→ 其余字段 LE 解码 → `validate_*_shape()` 语义校验（失败 →
`MalformedField`/`ReservedNonzero`）→ HMAC 比较（失败 → `ChecksumMismatch`）→ 构造
`VerifiedX`。这不是"MAC 已经覆盖所以能省略"的检查——同一把 KEK 签出的、来自未来 ABI 版本或
写错的 producer 的字节，MAC 仍然可能正确；decoder 如果只按当前固定偏移硬解，会把不同版本的
字节错误解释成当前字段的值，破坏"升级 fail-closed"这条边界。

**`validate_*_shape()` 规则（MAC 比较之前跑，fail-fast，跟 `compaction_intent_codec.hpp`/
`seal_journal_precondition_codec.hpp` 已有的检查顺序一致）**：

- 三个类型均要求 `candidate_id != 0 && request_id != 0`——这些 id 全部由 `SealIdWatermark`
  分配（`next_candidate_id{1}`/`next_request_id{1}` 起始于 1），0 从来不是合法的已保留 id。
- `SealExportStartedMigrationWire`：无额外语义校验（没有 topology/phase 结构）。
  `legacy_file_digest`/`v2_file_digest`/`legacy_mac`/`v2_mac` 是密码学输出，不加"不能全零"的
  通用拒绝——密码学输出理论上可以是零，只有协议明确规定某个 bit 情况下必须全零时才检查，这几
  个字段没有这种规定。
- `SealStartedCleanupTombstoneWire`：
  - `started_kind ∈ {kSealStartedKindNativeV2, kSealStartedKindMigratedV2}`；
  - `present_mask` 必须严格等于 `started_kind` 对应的闭合集合——`durable_control_plane.hpp:
    1058` 自己的注释"bit0=L, bit1=V, bit2=M at authorize time"，
    `docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3985-3989`"L is v2...V/M/C absent →
    NativeV2Started"确认 NativeV2 的文件集合就是 {L}——present_mask 是"at authorize time"的
    快照，CREATE_NEW 只发生一次、发生在任何 unlink 之前：`started_kind==NativeV2` 时
    `present_mask==0b001`；`MigratedV2` 时 `==0b111`；其它组合 `MalformedField`；
  - `phase ∈ [kSealStartedCleanupPhaseAuthorized, kSealStartedCleanupPhaseClrPending]`
    （0..4，范围检查跟 `is_legal_generation_transition` 同一个套路）；
  - `reserved0 == 0`（`ReservedNonzero`）；
  - `source_generation != UINT32_MAX && new_generation == source_generation + 1`——这是同一个
    wire 内部两个字段的代数关系（不是跨文件比对，`docs/BINANCE_PRIVATE_REST_L4_SPEC.md:5084`
    "new_generation = Started.new_generation (== source_generation+1 for legal compaction;
    UINT32_MAX refused earlier)"确认这条关系式），溢出防护用无符号显式 guard；
  - **digest_L/digest_V/digest_M 的 canonical-zero 规则**：`durable_control_plane.hpp:1072`
    自己的注释只给 `digest_L` 写了"or zeros if bit0 clear"，`digest_V`/`digest_M` 没有单独的
    同款注释——但三者结构上是同一类字段（`.clr` 的这三个 digest 都是"CREATE_NEW 时从已认证的
    Started 记录拷贝的摘要"，`docs/BINANCE_PRIVATE_REST_L4_SPEC.md:4025`"copy proof fields +
    digests + kind/mask from admitted Started"；NativeV2Started 本身只有 L，V/M 在磁盘上不
    存在——既然 Started 侧根本没有 V/M 可供拷贝，`.clr` 里对应的 digest_V/digest_M 唯一合理的
    canonical 值就是全零，是把已经写明的模式套用到结构相同的另外两个字段，不是发明新规则）：
    `started_kind==NativeV2` 时 `digest_V`/`digest_M` 必须全零；`MigratedV2` 时不检查（三个
    digest 都对应"文件存在"，密码学输出理论上仍可以是零）。
  - **明确不做**：所有"`== Started.X`"标注的字段（`baseline_tip_seq`/`baseline_tip_mac`/
    `baseline_key_id`/`new_final_seq`/`new_final_tip_mac`/`new_key_id`/`content_root`）——
    需要读另一份 `SealExportStartedWire`/bridge 记录才能验证，单文件 decode 证明不了，明确
    排除；`phase` 单调递增，同样排除。
- `SealStartedAbandonWire`：
  - `started_kind ∈ {1,2}`；
  - `present_mask` 低三位（L/V/M）必须严格等于 `started_kind` 对应集合（同上推导，
    `docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3303`同款注释）；bit3（C）可以是 0 或 1（两者都
    合法——`docs/BINANCE_PRIVATE_REST_L4_SPEC.md:436`/`4092-4093`明确 A0 时 no-C:
    `present_mask.C=0`+`digest_C=0`，with-C: `bit3=1`+真实摘要，两条路径都合法）；高 4 位
    必须是 0；
  - `bit3(C)==0` 时 `digest_C` 必须全零——唯一一条"必须全零"的规则，直接来自
    `docs/BINANCE_PRIVATE_REST_L4_SPEC.md:436`/`4092-4093`的显式规定；`bit3==1` 时
    digest_C 是否等于真实 C 文件内容摘要是跨文件比对，明确排除；
  - `abandon_reason == kSealStartedAbandonReasonNotFound`（代码库目前只定义这一个值，注释
    写法读起来像封闭枚举）；
  - `phase ∈ [kSealStartedAbandonPhaseAuthorized, kSealStartedAbandonPhaseAbdPending]`
    （0..7）；
  - **明确不做**：`baseline_tip_seq`/`baseline_tip_mac`/`baseline_key_id`/`content_root` 的
    跨文件一致性；phase 单调性；bit3=1 时 digest_C 与真实文件的比对。

**写侧 shape gate**：三个新 `encode_*` 函数在写入任何字节之前先调用对应的
`validate_*_shape()`——语义非法直接返回 `0`（不写任何输出字节，不计算 MAC）。**已知遗留**：
Slice 1 已经合并的 `encode_seal_id_watermark_wire`/`encode_seal_export_started_wire` 目前
仍是"无条件写入并签 MAC，shape 只在 decode 时验证"，没有这个 gate——本轮不回头改 Slice 1
代码（避免范围蔓延），只在这里记一笔：未来如果要接入真实写路径，需要先给这两个 Slice 1
encoder 也补上同款 gate。

**本轮明确不做**：真实文件 I/O；`SealJournalOriginKey` 的 codec（内存去重 key，没有
`mac[32]`，不需要）；`SealJournalIntakeCloseControl`/`SealJournalIntakeCloseProducerSlot` 的
codec（RAM-only atomic 控制块，非 durable）；Fuzz 基础设施（2000 次随机字节属性测试保留，但
不替代真正的 fuzz——这轮的 parser 处理固定、可信的本地 sidecar 格式，fuzz 标记
NEEDS_EVIDENCE，不是这轮发布阻断项）；TLA+（纯 encode/decode，没有新的并发状态机或持久化
写路径，不为了"覆盖率"造模型）。

**实测证据**：Slice 2a 与 Slice 2b 由协调者在同一次合并（`codex/round-e-slice2-integration`）后
统一验证，实测数字记在上面"Seal-journal Round E Slice 2a"条目的"实测证据"小节——两边共用同一套
MSVC/WSL2/ASan+UBSan 验证腿，不重复记录。

### Seal-journal Round E breadcrumb L2 loaders（7 个 durable-precondition 类型的真实只读文件
I/O）[已实现]

Slice 1/2a/2b（上面）给 `SealIdWatermark`/`SealExportStartedWire`/`SealJournalCommitWatermark`/
`SealJournalTombstoneWire`/`SealExportStartedMigrationWire`/`SealStartedCleanupTombstoneWire`/
`SealStartedAbandonWire` 七个类型都做了真实 L1（纯内存，MAC 验证）codec。Round D 的"边界"小节
（本文档"Seal-journal Round D"条目）早就写明：receipt 概念 / `raise_intent_phase()` 生产 API /
manager 接入，必须等这些真实 codec **都**存在**之后**才能安全开放——现在满足了这个前提，但下一步
不是直接跳去做 phase-advancement API。

**为什么不直接做 receipt/raise_intent_phase()/manager**：`CandidateLease`/`IntentStore`
（Round D，唯一已经落地的 L2 写路径）自己的 ledger 记录了 6 轮外部 Architect 审查、前 5 版全部被
拒的真实历史（本文档"Seal-journal Round D"条目"第一版"到"第六版"）。拒绝理由包括：可伪造的
evidence 对象、`raise_intent_phase()` 本身就是同一个问题换皮、ID 生命周期规则自相矛盾、TOCTOU、
handle 通过 public accessor 泄漏导致 UAF、写方法接受任意字符串文件名导致路径穿越、`release()`
没有 owner-thread 检查。这些全部是 phase-advancement/manager 级别的设计问题，需要单一权威反复
审查才收敛，不适合分给互不通气的多方并行做。

**这一轮做什么**：receipt/manager 逻辑要用的"读取一份已经在磁盘上的 durable precondition 记录"
这个能力本身还不存在（之前全是 L1 codec，没有真正的文件 I/O）。这一轮只做**只读** L2 I/O——不涉及
Round D 那 5 次被拒设计里任何一个写路径特有的问题（可伪造 evidence / ID 生命周期 / 路径穿越写 /
release 无 owner 检查全部是写语义）。仍然**明确不做**：receipt 概念、`raise_intent_phase()`、
`IntentStore`/`CandidateLease`/任何 manager 的 phase-advancement 接入、任何写方法。

**关键架构发现（阻塞性，已解决）**：起初设想给 `SealIdWatermark`/`SealExportStartedWire`/M/C/A
五个类型新建一个独立的 SealBreadcrumbLease 类（这个类从未落地，只是被否决的设计方案名字）。
这是错的——
`docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3152-3163` 的文件名列表（"Filenames (breadcrumb dir) —
round-56/57/64"）把 `seal-export-started`/`.v2`/`.mig`/`.clr`/`.abd` 和
`compaction-candidate-intent`/`.x1`/`.xgc`（Round D 已经在管的三个文件）列在**同一个目录**里，
逐字面确认无疑。如果新建一个独立的 lease 类去开同一个物理目录，会在 Round D 已有的锁文件
（`compaction-candidate.lock`）之外产生一套完全不协调的第二把锁——两个类各自的 owner-thread/
fenced 状态互相看不见对方，等于在类间层面重新制造 Round D 六轮审查专门要消灭的那类协调漏洞。
**决定**：不新建 breadcrumb 类，直接扩展 `CandidateLease`（`compaction_lease.hpp`）本体，复用它
已经审查过、已经在生产可用的 handle/锁/atomic 机制，新增 6 个 friend-only 定长只读方法（类/文件
名不改，保留六轮审查历史的最小 diff）。`SealJournalCommitWatermark`/`SealJournalTombstoneWire`
所在的 `seal-journal/<store_uuid_lo_hex16><store_uuid_hi_hex16>/`
（`durable_control_plane.hpp:750`/`871`、`BINANCE_PRIVATE_REST_L4_SPEC.md:4392`）是另一个、
名字明确不同的目录，这个确实需要一个新的独立类：`SealJournalStoreLease`（新文件
`seal_journal_store_lease.hpp` + `seal_journal_store_io.hpp`，只读子集，不
`#include compaction_breadcrumb_io.hpp`——那个文件头注释明确写死"DO NOT include this file from
anywhere except compaction_lease.hpp"，这个边界不动，`seal_journal_store_io.hpp`是独立、只读的
重复实现，不是耦合）。

**`SealIdWatermark` 文件名（新增决定，不是 transcribe）**：跟 L/V/M/C/A 五个类型不同，
`SealIdWatermark` 在整个仓库/spec 里之前**没有任何字面量文件名**。这一轮把它定为
`seal-id-watermark`（`ValidatedArtifactName::for_seal_id_watermark()`），跟其余 kebab-case
命名一致，作为这一轮的设计决定明确记录，不是从既有契约里抄出来的。

**`kek_key_id` 契约**：`SealIdWatermark` 的 wire 里没有 `kek_key_id` 字段、没有 peek 函数
（跟本仓库其它六个类型都不同）——调用方必须显式传入 `kek_key_id`，绝不能加"当前 active key"
兜底（那正是 Round D 第二版被拒的漏洞在 `KeyRing` 层面的翻版；`KeyRing::pin_active_key()` 已经
因为这个确切原因被移除）。Module 1（下面）的 `SealIdWatermarkLoader::load()` 必须把 `kek_key_id`
作为显式参数。

**5 模块划分（协调者 + 4 个独立外包，完整拆解见协调者本地计划文档，未入库）**：
- **模块 0（协调者，本条目状态为 [进行中] 期间已落地的部分）**：`CandidateLease` 新增 6 个
  friend-only 方法（`read_seal_id_watermark`/`read_seal_export_started`（L/V 用
  `legacy_or_greenfield` 布尔选择文件名，同一个方法，不是两个）/`read_seal_export_started_migration`/
  `read_seal_started_cleanup_tombstone`/`read_seal_started_abandon`）；`ValidatedArtifactName`
  新增 5 个对应 factory；新建 `SealJournalStoreLease`（`read_seal_journal_commit_watermark`/
  `read_seal_journal_tombstone` 两个 friend-only 方法）。Friend 类名冻结（其余模块必须原样使用，
  不得自行更名）：`SealIdWatermarkLoader`/`SealExportStartedLoader`/
  `SealExportStartedMigrationLoader`/`SealStartedCleanupTombstoneLoader`/
  `SealStartedAbandonLoader`（`CandidateLease` 的友元）、`SealJournalCommitWatermarkLoader`/
  `SealJournalTombstoneLoader`（`SealJournalStoreLease` 的友元）。测试通过
  `hy::test_only::CandidateLeaseSealBreadcrumbTestAccess`/
  `hy::test_only::SealJournalStoreLeaseTestAccess` 两个协调者自用的 test-only friend 驱动
  （生产代码不 include，只有对应测试文件用）——因为下面 4 个模块的真实 loader 类在这一步落地时
  还不存在。
- **模块 1-4（外包，尚未落地，本条目 [进行中] 状态直到全部并入才翻 [已实现]）**：模块 1 =
  `SealIdWatermarkLoader` + `SealExportStartedLoader`；模块 2 = `SealJournalCommitWatermarkLoader`
  + `SealJournalTombstoneLoader`（后者依赖前者读出的 `highest_committed_journal_seq` 才知道该
  扫哪些 `journal_seq`，不是互相独立）；模块 3 = `SealExportStartedMigrationLoader` +
  `SealStartedCleanupTombstoneLoader`；模块 4 = `SealStartedAbandonLoader`。四个模块互相之间
  零依赖，只依赖模块 0 已经冻结的方法签名 + 已经合并的 L1 `decode_*` 函数；聚合函数
  `load_all_breadcrumb_preconditions()` 由协调者在四个模块都落地后自己写（需要同时知道 1/3/4
  三个模块各自的返回类型，故意不下放，避免破坏"互相零上下文"）。

**验证（模块 0-4 + 协调者聚合函数全部并入后的最终实测，2026-08-12）**：
- `test_compaction_lease.cpp` 新增 SealBreadcrumbLoaders/SealBreadcrumbLoadersDirectoryFencing
  测试套件（round-trip、NotFound、WrongSize、NotHeld、WrongOwner，以及证明"扩展 CandidateLease
  而非新建"这个决定本身成立的关键测试——目录 rename/recreate 触发的 sticky fence 对新方法和
  Round D 原有方法一视同仁，POSIX-only，Windows 上该场景结构性不可达的理由跟
  `test_compaction_intent_store.cpp` 的 IntentStoreDirectoryFencing 测试套件完全一致）；新建
  `test_seal_journal_store_lease.cpp`，同款生命周期 + fencing 覆盖，外加多 candidate/多 seq
  共享同一目录的场景（证明"per-store not per-candidate"这个容易被误读的性质）。
- Modules 1-4（`test_seal_id_watermark_export_started_loader.cpp`/
  `test_seal_journal_commit_tombstone_loader.cpp`/`test_seal_started_migration_cleanup_loader.cpp`/
  `test_seal_started_abandon_loader.cpp`）各自独立交付，复核后发现两处需要修的问题：
  (1) Module 2 的 `SealJournalCommitWatermarkLoader`/`SealJournalTombstoneLoader` 头文件
  在 `hy` 命名空间里定义了自己的 `enum class LoadStatus`，跟 Module 1/3/4 复用的
  `hy::LoadStatus`（来自 `compaction_intent_store.hpp`）同名——两者字段集合确实不同
  （`StoreDirFenced` vs `CandidateFenced`，语义上不该合并），但同名会在任何同时
  `#include` 两边的翻译单元里造成重定义编译错误（协调者聚合函数正是这样的翻译单元）。
  修复：重命名为 `SealJournalStoreLoadStatus`，不复用 `hy::LoadStatus`。
  (2) Module 1 的 `SealExportStartedLoader` round-trip 测试对 `std::uint8_t[32]` 字段
  （`baseline_tip_mac`/`new_final_tip_mac`/`content_root`）用 EXPECT_EQ 直接比较——C
  数组退化成指针比较地址而非内容，测试恒定失败（无论产品代码是否正确）。修复：改用
  `std::memcmp`，跟本仓库其它 mac[32] 字段的既有测试写法一致。
- 协调者新建 `seal_journal_breadcrumb_precondition_aggregate.hpp`
  （`load_all_breadcrumb_preconditions()`），只组合 Modules 1/3/4（breadcrumb 目录，同一个
  `CandidateLease`），不含 Module 2（`seal-journal/<store_uuid>/` 是不同目录，用
  `SealJournalStoreLease`，混进同一个函数会模糊这轮 ledger 条目特意保留的目录边界区分）；
  新建 `test_seal_journal_breadcrumb_precondition_aggregate.cpp` 验证组合本身（每个字段的
  状态互相独立、没有提前 return）。
- MSVC Release 全量 ctest：1084/1084（`SealIdWatermarkLoader.DirectoryIdentityChangeThen
  StickyFenceAreDistinct` 之外的两个 fencing 测试都标了 `#ifndef _WIN32`，1 个 skip 是
  预期行为，理由跟 IntentStoreDirectoryFencing 的既有 POSIX-only 说明完全一致）。
- WSL2 GCC-14 Release（镜像 `ci-native.yml`，含 `HY_BUILD_DEMO=ON` 全部二进制，两个
  POSIX-only fencing 测试这里真的会跑）：1111/1111。
- WSL2 ASan+UBSan（定向构建本轮全部相关 test 目标：test_compaction_lease/
  test_compaction_intent_store/test_seal_journal_store_lease/
  test_seal_id_watermark_export_started_loader/test_seal_journal_commit_tombstone_loader/
  test_seal_started_migration_cleanup_loader/test_seal_started_abandon_loader/
  test_seal_journal_breadcrumb_precondition_aggregate，避开 WSL VM 全量
  `HY_BUILD_DEMO=ON` ASan 构建已知的 OOM 问题）：123/123，无 ASan/UBSan 报告。
- WSL2 TSan：`CandidateLease` 本体被改了，负控 tsan_control_relaxed_ring 重新确认仍然以
  非零退出码报出 `WARNING: ThreadSanitizer: data race`（本机 TSan 检测能力未失效）；
  test_compaction_lease（含新的 SealBreadcrumbLoaders/跨进程互斥测试）在 TSan 下全部
  通过，未发现新的数据竞争。
- `spec_xref_check.py --quiet`：460 个符号、38155 个匹配点，全部可定位（新注册
  `seal_journal_store_lease.hpp`/`seal_journal_store_io.hpp`/四个模块的 loader 头文件/
  聚合头文件；ledger 正文修了几处误用反引号包裹非符号 prose 的地方，同 Slice 2a/2b 的
  同类修复）。
- `spec_enum_diff.py`：0 个新发现。

### Seal-journal Round E receipt + raise_intent_phase()（Building->Reserved）[已实现]

Round E breadcrumb L2 loaders（上面）落地后，receipt 概念 / `raise_intent_phase()` 的前提（7 个
durable-precondition 类型都有真实 L1 codec + L2 只读 I/O）第一次全部满足。这一轮做 Round D 六轮
审查刻意排除的那类写路径——Intent phase 推进——但只做 Building→Reserved 一条边，其余三条边
（Reserved→StartedPublished / StartedPublished→{PostSealFinalizing,AbandonFinalizing}）推迟到
这一条边真正落地、复核通过之后再逐条设计，不比照 Round D 五版被拒历史里"一次性设计全部写路径"
那类失败模式。

**三轮对抗性审查收敛出的设计（协调者本地 scratchpad，未入库，`receipt_raise_intent_phase_design_
{v1,v2,v3}.md`）**：
- v1（四条边全设计）被查出三处真实 bug：`StartedPublished→PostSealFinalizing`/`AbandonFinalizing`
  两条边的字段映射表比对了 `CompactionCandidateIntentWire` 根本没有的字段、`phase >=
  kSealStartedCleanupPhaseAuthorized`（== 0）恒真死代码、`AbandonFinalizing` 边接受 `GenGone`
  过宽（`GenGone` 不代表 tip 已重新核对，只有 `ResumeAuthorized` 代表）。
- v2 收窄到只做 Building→Reserved，修正上述三处（推迟另外三条边，不是修复后仍然一次性做完）；
  同时诚实记录 `friend class hy::IntentPhaseAdvancer` 授予的是对 `CandidateLease` **整个类**
  私有面的编译期访问权（C++ friend 是类粒度不是方法粒度），不是这版设计能消除的风险。
- v3 修 v2 唯一未过关的一项：`.x1` 写入的幂等重试直接复用 `write_validated_no_replace`/`_win`
  本身的语义，但没配上 `IntentStore::genesis_provenance_memory_*` 那套同进程 provenance-memory
  防护——Windows 上首次写入可能返回 `PublishedNamespaceUncertain`，崩溃重试会看到"文件已存在且
  字节相同"就误判 `DurablyPublished`，违反 L4 spec"Forbidden: Intent REPLACE without prior
  durable matching `.x1`"。修复：`IntentPhaseAdvancer` 新增跟 `genesis_provenance_memory_*`
  同款的 `X1WriteProvenanceMemory`（键为 build_nonce+transition_seq+encoded bytes，而非
  GenesisRequest）。v3 同时钉死两个开放问题：`raise_intent_phase(build_nonce)` 只接受
  `build_nonce`（不接受裸 `to_phase`，不接受 candidate_id/request_id 参数——两个 id 完全由这次
  调用内部从新鲜读取的 `SealIdWatermark`（`next_candidate_id - 1`/`next_request_id - 1`）派生，
  见下面"实现层面对 watermark 严格 == 判断的落地方式"）；`SealIdWatermarkLoader::load()` 的
  `kek_key_id` 从当前 Intent 自己的 `kek_key_id` 字段读，不另外传参。

**实现层面对 watermark 严格 `==` 判断的落地方式（v3 之后、写代码时才发现需要明确记录的一点）**：
设计文档的字段映射表写的是"`next_candidate_id == candidate_id + 1`"，隐含 candidate_id 是一个
独立获得、可能过期的值。但 v3 把 `raise_intent_phase()` 的参数收窄到只有 `build_nonce` 之后，
candidate_id/request_id 不再是调用方传入的独立值——`IntentPhaseAdvancer` 直接把这次读到的
watermark 的 `next_candidate_id - 1`/`next_request_id - 1` 当成要绑定的 id。这让"严格 ==
判断"在实现里体现为"永远用最新一次读取派生"，不是"比较两个独立获得的值"——原设计文档里
"严格 == 不是 >"这条分析结论仍然成立（排除了"watermark 已经继续往前推了好几个"这种场景被放行
的风险），只是承载它的机制从"一次显式比较"变成了"派生方式本身"。诚实记录这个差异，不假装
按字面实现了一次独立比较。

**明确不做（跟 v1/v2/v3 一致）**：`.xgc`/GC 授权（Round F）；journal drain-completeness 前置
检查；Reserved→StartedPublished 及之后任何边；`SealIdWatermark` 本身的**写入**（`raise_intent_
phase()` 只读取、绑定已经 durable 的 watermark 状态，不负责让它变成 durable——本仓库目前没有任何
代码路径写 `SealIdWatermark`，这是这一轮明确划定在范围外的前置依赖，真实部署前必须先有）。

**运营范围（用户已确认，2026-08-15）**：这一轮**只做到"类存在、测试证明逻辑对"**，不接入任何
真实会被调用的生产代码路径——`Reserved→StartedPublished`/PreSeal-abandon 清理路径都还不存在，
一旦真实调用会永久卡住 candidate 目录和已分配的 id（跟 spec 自己写明的"crash 后 id 永久消耗"
同一类不可逆），要清理只能靠仓外人工手段。

**交付物**：
- `compaction_lease.hpp`：新增 `write_x1_frame_no_replace()`/`replace_intent_phase()` 两个
  friend-only 写方法（`CandidateLease` 有史以来第 2/3 个写方法，第 1 个是 Round D 的
  `create_intent_genesis_no_replace()`），新增 `friend class hy::IntentPhaseAdvancer`
  （第 8 个 friend）。`replace_intent_phase()` 需要 REPLACE 语义（Round D 的 genesis 写入
  一直是 CREATE_NEW-only，这个仓库历史上第一次需要"覆盖已存在文件"这个原语）：新增
  `windows_native_io.hpp::rename_with_replace()`（`ReplaceIfExists=TRUE` 版
  `rename_no_replace()`）、`compaction_breadcrumb_io.hpp::write_validated_replace()`（POSIX，
  普通 `renameat()` 本身已经是原子 replace，不需要 no-replace 那套 renameat2/linkat 回退，
  也没有 byte-compare-on-collision 分支——REPLACE 没有"already exists"这个失败态可以区分）、
  `compaction_lease.hpp` 内 `write_validated_replace_win()`（组合 helper，镜像
  `write_validated_no_replace_win()` 结构）。
- 新文件 `intent_phase_advancer.hpp`：`PhaseAdvanceStatus` 枚举、`SealIdWatermarkAdvanceReceipt`
  （私有构造 + `IntentPhaseAdvancer` 唯一 friend，不作为跨调用对象暴露——receipt 生产+消费在
  同一次 `raise_intent_phase()` 调用内完成，关闭 TOCTOU 窗口）、`IntentPhaseAdvancer` 本体
  （构造接收 `CandidateLease&`/`KeyRing&`，复用同一把已审查过的锁，不新开）。`raise_intent_
  phase(build_nonce)`：读取现有 Intent（必须 Building 且 build_nonce 匹配；已经 Reserved 及以后
  且 build_nonce 匹配则返回 `AlreadyAtOrPastTargetPhase`，幂等安全）→ 读取+MAC 验证
  `SealIdWatermark` receipt → 派生 candidate_id/request_id → 复用既有
  `validate_intent_transition()` 做语义校验（不重新实现 ID 生命周期/immutable 字段/chain 规则）
  → `write_x1_frame_no_replace()`（带 `X1WriteProvenanceMemory` 幂等重试防护）→ 确认 durable
  后才 `replace_intent_phase()`。
- 新测试 `test_intent_phase_advancer.cpp`：成功路径（watermark 派生 id 正确绑定）、`.x1` 先于
  Intent REPLACE durable 的顺序验证、同实例重试返回 `AlreadyAtOrPastTargetPhase`、
  ForeignBuildNonce/IntentNotFound/WatermarkNotFound/WatermarkCorrupt（含 watermark 全零场景，
  见下面"验证"小节的说明）/WatermarkForeignStore/LeaseNotHeld 各类拒绝路径。`SealIdWatermark`
  无生产写入路径，测试用裸文件 I/O 直接种一份 watermark（不经过 lease，因为只读方法才是
  friend-gated，写字节本身不需要）。

**验证（2026-08-15）**：
- MSVC Release 全量构建：0 warning（`/W4`）。全量 ctest：1093/1093（1 个既有 POSIX-only fencing
  skip，跟既有说明一致）。过程中发现并修了两处自己的 bug：测试 fixture 把 watermark 写到了
  错误文件名（`compaction-id-watermark`，应为 `ValidatedArtifactName::for_seal_id_watermark()`
  实际返回的 `seal-id-watermark`）；`IntentPhaseAdvancer` 里一段
  `next_candidate_id/next_request_id == 0` 检查是死代码——`decode_seal_id_watermark_wire()`
  自己已经在解码阶段拒绝零值（Round E Slice 1 的 allocator-safety 检查），这一步永远不可达，
  删除并把 watermark store_uuid 不匹配的状态从原先设想的"WatermarkNotYetAdvanced"改名为更准确的
  `WatermarkForeignStore`（不再跟一个已经不可能出现的场景共用同一个名字）。
- WSL2 GCC-14 Release（镜像 `ci-native.yml`，`HY_BUILD_DEMO=ON`）：1122/1122。
- WSL2 ASan+UBSan：全量 `HY_BUILD_DEMO=ON` ASan 构建在这台 WSL VM 上有已知 OOM 问题（跟
  breadcrumb loaders 那轮完全一致的限制），改用同一个规避方式——只构建本轮相关的 9 个测试目标
  （test_compaction_lease/test_compaction_intent_store/test_intent_phase_advancer/
  test_seal_journal_store_lease/四个 loader 测试/test_seal_journal_breadcrumb_precondition_
  aggregate），`-DHY_BUILD_DEMO=OFF`：92/92，无 ASan/UBSan 报告。
- WSL2 TSan：`ctest -L concurrency`：58/58。两个负控
  （tsan_control_relaxed_ring/tsan_control_export_worker_dual_consumer）均以非零退出码正确
  报出 `WARNING: ThreadSanitizer: data race`（本机 TSan 检测能力未失效）。验证过程中一次因
  WSL VM 瞬时中断导致的构建被杀掉，留下一个 0 字节的 tsan_control_export_worker_dual_consumer
  可执行文件（增量构建系统没能感知到这是被杀掉的半成品，误判"已经是最新"）——删除后强制重新
  link 即恢复正常；记录下来是因为这跟真正的代码 bug表现相似（负控没报错），必须先排除环境因素
  再下结论，不能直接假设"负控没触发 = 代码有问题"或反过来"退出码 0 = 一切正常"（这次 WSL
  外层脚本本身的退出码在这次环境中断后也不可靠，靠读实际 ctest/日志内容而非退出码本身确认结果，
  跟本轮更早发现的"exit code 经过 `| tail` 管道会被掩盖"是同一类教训）。

**同轮并行派发的两份独立产出（跟上面的 `IntentPhaseAdvancer` 实现互相零文件交集，按"下一轮
2-模型并行派发协议"隔离 worktree 完成，协调者复核后一并整合进本条目，不单开 ledger 条目）**：
- **Grok 4.6**：新增 `formal/RoundEFDesignReceiptVerified.tla`（+ .cfg / _liveness.cfg 两个模型
  检查配置）——`RoundEFDesign.tla`（未来设计参考模型，跟生产代码无可追溯关系）的独立姊妹模型，
  不修改原文件。新增 receiptVerified BOOLEAN 变量，按"消费型"（不是单调）建模：VerifyReceipt
  置真，`Transition` 要求为真后立即消费回假——跟 `IntentPhaseAdvancer` 真实实现里"receipt 只在
  `raise_intent_phase()` 一次调用内生产+消费，不跨调用持久化"这条设计决定同构。协调者用 TLC
  独立重跑全部结果，与 Grok 报告完全一致；并额外做了一次这个仓库一贯的"控制必须失败"验证
  （去掉 receiptVerified 守卫，确认 TLC 报出预期的不变量违反）——未发现问题，无需修复。
- **DeepSeek V4 Flash**：新增 `native/src/seal_journal_cross_file_audit.cpp`（只读诊断工具，
  非生产代码路径，从不获取 lease，只用 plain std::ifstream）——扫描一个构造出的 fixture breadcrumb
  目录，报告 92 条跨文件 MUST-equal 规则（entry_mac 等字段必须在多个文件间一致）的
  Verified/Violated/N/A 状态，喂给未来字段映射表的实证基础；`--self-check` 模式跑满 97 个
  单点注入 + 4 个 clean 场景的完整矩阵。协调者复核了 key material 处理、7 个 `decode_*`
  wrapper 的 peek→pin→decode 模式、`InjectionSpec.field` 的生命周期修复（`std::string` 而非
  悬空 `string_view`）、3 处 `KeyRing` 构造点的双 key 修复，并独立在 MSVC + WSL2 GCC-14 双工具链
  各跑一次完整构建 + 全量 ctest（MSVC 1085/1085，1 个既有 skip；WSL2 1114/1114），外加直接运行
  `--self-check` 本身确认 97+4 项全部符合预期——未发现问题，无需修复。

**最终整合**：三份互相零文件交集的产出（`IntentPhaseAdvancer` 本体、Grok 的 TLA+ 模型、
DeepSeek 的审计工具）合并进同一个集成分支，仅 `native/CMakeLists.txt` 有两处独立、不重叠的插入点
需要协调者手工按顺序应用（`add_executable(seal_journal_cross_file_audit ...)` +
`add_test(seal_journal_cross_file_audit_selfcheck)` 来自 DeepSeek，
`add_executable(test_intent_phase_advancer ...)` 来自协调者自己），应用后 `grep -c
add_executable` 确认无重复/无遗漏行。

### Seal-journal Round E SealIdWatermark advance（`raise_intent_phase()` 折进 watermark 推进）[已实现]

上一轮（"Seal-journal Round E receipt + raise_intent_phase()"，见上）明确划定"SealIdWatermark
本身的写入"在范围外，假设有某个外部 actor 已经先把 watermark 推进到 durable 状态——但复核发现
这个假设从未被本仓库任何代码路径满足过（"明确不做"清单里那句"本仓库目前没有任何代码路径写
SealIdWatermark"本身就已经暗示了这一点），而 L4 spec 把"durable watermark advance → `.x1` →
Intent REPLACE"写成同一个 id-reserve 步骤，不是一个解耦的前置条件。这一轮把 watermark advance
折进 `raise_intent_phase()` 本身，修正这个从未真正成立的假设。

**两轮对抗性审查**（协调者本地 scratchpad，未入库，`receipt_raise_intent_phase_design_
{v4,v5}.md`）：
- v4（提议同时折入 `SealIdWatermark` advance + `SealJournalCommitWatermark` CREATE_NEW）第一轮
  审查给出 4 项发现。协调者独立核实后推翻其中头号发现——审查 agent 认为每个 candidate 各自一份
  独立目录、store 内多个 candidate 会各自 bootstrap 出冲突的 `next_candidate_id=1`；但
  `compaction_intent_store.hpp:109`（"Refuses AlreadyExists if a genesis already exists on
  disk"）+ L4 spec"refuse if a prior Intent still exists"只有在同一个目录被多个连续 candidate
  复用时才成立（如果每个 candidate 各自新目录，物理上不可能"已存在"）——`CandidateLease` 管的
  目录是 store 级别、跨多个 candidate 生命周期复用的单一目录，`seal-id-watermark` 放在这里没有
  跨 candidate 冲突。另外三项发现确认成立：`next_candidate_id` 方向需要从"减一"改成直接用
  `next_*` 本身（旧假设下"减一"是对的，但既然 advance 本身现在就在这次调用里发生，"减一"会
  重复消耗）；`CandidateLease`/`SealJournalStoreLease` 两把锁协调是真实开放问题、本仓库没有
  先例；同进程重试在 advance 成功但下游（`.x1`/Intent REPLACE）失败后，如果不加防护会每次重试
  多烧一个 id。
- v5 收窄范围：只折入 `SealIdWatermark` advance，`SealJournalCommitWatermark` CREATE_NEW 整体
  推迟到专门设计两把锁协调方案的下一轮（不在这轮内尝试）；修正 id 派生方向；新增
  `WatermarkAdvanceProvenanceMemory` 同进程幂等重试防护（跟 `X1WriteProvenanceMemory` 形状不同：
  X1 只在"不确定"结果时才需要记忆，但 watermark advance 一旦 `DurablyPublished` 就已经消耗
  完毕，不管下游成功与否，所以要在 `DurablyPublished` 观测到的当下就记，不是只在"不确定"分支
  记）。第二轮审查发现一项"新"问题（watermark advance 排在 `validate_intent_transition()`
  的 before-state 检查之前，一个注定失败的 transition 也会先烧掉一个 id）——协调者核实后确认
  不成立：`intent_permanently_immutable_fields_match(before, after)` 因为 `after` 是从
  `before` 拷贝构造、只改 phase/candidate_id/request_id，按构造方式恒真；
  `is_legal_candidate_ids_for_phase(before.phase, ...)` 已经在 `raise_intent_phase()` 现有
  Step 0（`before.phase != Building` 检查，watermark 读取之前）覆盖，不会走到 advance 之后才
  发现失败。其余核实项（目录复用读法、`X1WriteProvenanceMemory` 类比是否连贯、bootstrap 遇到
  `PublishedNamespaceUncertain` 的处理、fence 检查顺序、跟 `genesis_provenance_memory_` 的类比
  是否准确）全部确认成立，v5 无需 v6。

**这一轮做的事**：
- `compaction_lease.hpp`：新增 `create_seal_id_watermark_no_replace()`/
  `replace_seal_id_watermark()` 两个 friend-only 写方法（`CandidateLease` 有史以来第 4/5 个写
  方法），复用已有的 `write_validated_no_replace`/`write_validated_replace`（及各自 `_win`
  变体）原语，不新增底层 I/O 原语。
- `intent_phase_advancer.hpp`：`raise_intent_phase()` 的 Step 1 从"只读 watermark、`next_* - 1`
  派生 id"改成"读取或（NotFound 时）bootstrap watermark → `candidate_id = next_candidate_id`/
  `request_id = next_request_id`（不再减一）→ UINT64_MAX fence 检查 → CREATE_NEW（bootstrap）
  或 REPLACE（advance）写回 `next_*+1`"，全部在同一次调用内完成，advance 成功后才继续走
  `.x1`/Intent REPLACE（未改动）。新增 `WatermarkAdvanceProvenanceMemory`（同进程记忆
  `(build_nonce, bound_candidate_id, bound_request_id, advanced)`，`DurablyPublished` 一出现
  就记录，同 build_nonce 的后续调用直接复用绑定的 id，不重新派生）。新增
  `PhaseAdvanceStatus::WatermarkExhausted`（`next_candidate_id`/`next_request_id` 已经是
  UINT64_MAX）、`WatermarkAdvanceFailed`（watermark CREATE_NEW/REPLACE 返回 `NotPublished`，
  含"bootstrap 时跟另一个进程的真实 watermark 写入撞车"这个窄场景——调用方直接重试整个
  `raise_intent_phase()` 调用即可，重试的新鲜读取会看到真实已存在的 watermark，走正常
  非-bootstrap 路径）。原有的 `WatermarkNotFound` 状态因为 NotFound 现在触发 bootstrap 而不再
  可达，从枚举里移除。
- `test_intent_phase_advancer.cpp`：`plant_watermark_reserving()` 测试 helper 修正方向（不再
  `+1`，直接存 `candidate_id`/`request_id` 本身）；原 RefusesWithoutWatermark 测试改名
  BootstrapsWatermarkWhenAbsentAndBindsOneOne，断言 bootstrap 成功绑定 1/1 且 watermark 文件
  推进到 `next_*=2`；新增 RefusesWhenWatermarkAtUint64Max（fence 检查）；新增
  RetryAfterX1FailureReusesSameWatermarkBoundIdsInsteadOfBurningAnother（预置一个字节不同的
  `.x1` seq=1 占位文件制造下游写入失败，验证 watermark 已经推进但同一个 advancer 实例重试时
  绑定同一对 id，不是新的一对）。

**明确不做（跟 v4/v5 一致）**：`SealJournalCommitWatermark` CREATE_NEW（推迟到下一轮，届时设计
`CandidateLease`/`SealJournalStoreLease` 两把锁的协调方案）；`.xgc`/GC 授权；journal
drain-completeness 前置检查；Reserved→StartedPublished 及之后任何边。运营范围延续上一轮"只做到
类存在、测试证明逻辑对，不接入生产路径"（未变，未重新征求用户确认，因为范围本身没有扩大）。

**验证（2026-08-15）**：
- MSVC Release 全量构建：0 warning。全量 ctest：1096/1096（1 个既有 POSIX-only skip）。
- WSL2 GCC-14 Release（`HY_BUILD_DEMO=ON`）：1125/1125（1123 基线 + 本轮新增 2 个测试）。
- WSL2 ASan+UBSan（同前几轮的 OOM 规避：只构建本轮相关的 9 个测试目标 + compaction_lease_holder
  跨进程测试辅助可执行文件——第一次目标列表遗漏了这个辅助程序，导致
  `CandidateLease.CrossProcessMutualExclusion` 因"找不到可执行文件"而非真实 bug 失败，补上目标
  后确认是构建范围问题不是代码问题）：94/94，无 ASan/UBSan 报告。
- WSL2 TSan：`ctest -L concurrency`：58/58。两个负控均正确报出 `WARNING: ThreadSanitizer: data
  race`。
- `tools/spec_xref_check.py --quiet`、`tools/spec_enum_diff.py`：均 clean（跟之前几轮相同的
  既有、无关警告）。

### Seal-journal Round E SealJournalCommitWatermark CREATE_NEW + 两把锁协调[已实现]

上一轮（"Seal-journal Round E SealIdWatermark advance"，见上）明确把 `SealJournalCommitWatermark`
CREATE_NEW 推迟到"专门设计两把锁协调方案的下一轮"。L4 spec 原文
（`docs/BINANCE_PRIVATE_REST_L4_SPEC.md:3839-3843`）："Reserved: after durable SealIdWatermark
advance + CREATE_NEW SealJournalCommitWatermark for this candidate_id (same id-reserve step as
§10.1): publish `.x1` seq=1 ... THEN REPLACE Intent."——这一轮做后半句：把 `.jhw`
（`seal-journal/<store_uuid>/<candidate_id_hex16>.jhw`）的 CREATE_NEW 折进
`raise_intent_phase()`，插在 watermark advance 产出 candidate_id 之后、构造 `.x1` 之前（`.jhw`
的 `candidate_id` 字段本身就依赖 watermark advance 的输出，两步顺序不能颠倒）。这是
`IntentPhaseAdvancer` 第一次需要同时持有两把完全独立的锁（`CandidateLease` +
`SealJournalStoreLease`，互不共享任何状态）。

**一轮对抗性审查**（协调者本地 scratchpad，未入库，`jhw_create_new_two_lock_design_v1.md`）：
核实全部 8 条声明，草稿的核心结论（两把锁确实是这个仓库第一次出现、`.jhw` CREATE_NEW 必须晚于
watermark advance、`.jhw` 需要跟 `X1WriteProvenanceMemory` 同款（不是跟
`WatermarkAdvanceProvenanceMemory` 同款）的幂等重试防护——因为 `.jhw` 是 no-replace CREATE_NEW,
跟 `.x1` 是同一类原语，不是 watermark 的 REPLACE-advance）全部确认成立。审查同时指出草稿三处
可执行层面的缺口（草稿本身没有编译期/运行期可验证性,是写代码时才会暴露的那类问题，不是设计
思路错误）：
1. 草稿提议的写方法签名遗漏了 `candidate_id` 参数——`SealJournalArtifactName::
   for_commit_watermark(candidate_id)` 需要它才能构造文件名，跟已有的读方法
   `read_seal_journal_commit_watermark(candidate_id, out)` 签名一致，草稿的写方法却只有
   encoded bytes 一个参数，物理上无法确定要写哪个文件。
2. 草稿把 Windows 写原语放错了文件——`seal_journal_store_io.hpp` 只有平台无关部分 + POSIX-only
   部分（`#ifndef _WIN32`），Windows 版本应该跟既有的 `read_validated_exact_win` 一样放在
   `seal_journal_store_lease.hpp` 自己的 `#ifdef _WIN32` 块里。
3. 草稿没有设计新的写结果类型——`SealJournalStoreLease` 目前只有 `SealJournalLeaseReadResult`，
   新写方法需要一个类比 `CandidateLease` 的 `LeaseWriteResult` 的 `SealJournalLeaseWriteResult`。
审查同时核实草稿"未决问题"里的第 3 条本可以不必悬而未决——L4 spec 3024 行明确写"Lifetime:
CREATE_NEW at durable id-reserve (highest=0)"，`highest_committed_journal_seq` 在 CREATE_NEW
时必须是 0，不是草稿以为的"看起来合理但没找到明文依据"。

**这一轮做的事**：
- `seal_journal_store_io.hpp`：新增 `PublishCommitState`/`PublishProvenance`/`PublishResult`
  （跟 `compaction_breadcrumb_io.hpp` 同款词汇表，复制不共享，同一条信任边界理由）+ POSIX
  `write_validated_no_replace()`（CREATE_NEW only，这个文件依然没有 REPLACE/delete 原语）。
- `seal_journal_store_lease.hpp`：新增 Windows `write_validated_no_replace_win()`（镜像
  `compaction_lease.hpp` 同名函数），新增 `SealJournalLeaseWriteResult`，新增
  `create_seal_journal_commit_watermark_no_replace(candidate_id, encoded_watermark)`
  friend-only 写方法（这个类有史以来第一个写方法），新增
  `friend class hy::IntentPhaseAdvancer`（第 3 个 friend）。新增
  **LOCK ORDER 文档**：`CandidateLease` 必须先于 `SealJournalStoreLease` 获取，释放顺序相反——
  这个仓库第一次需要跨两把锁的调用路径，没有先例可循，这条规则只能靠文档纪律，编译器不强制。
- `intent_phase_advancer.hpp`：构造函数改成同时接收 `CandidateLease&` +
  `SealJournalStoreLease&`（调用方负责按 LOCK ORDER 顺序各自 acquire 好、持有整个调用期间，
  这个类本身不 acquire/release 任何一把锁）。`raise_intent_phase()` 在 watermark advance
  之后、构造 `.x1` 之前插入 `.jhw` CREATE_NEW，新增 `JhwWriteProvenanceMemory`（跟
  `X1WriteProvenanceMemory` 同形状，键为 `(build_nonce, candidate_id, encoded .jhw bytes)`,
  只在 `PublishedNamespaceUncertain` 时才记忆——因为 candidate_id 到这一步已经被
  `WatermarkAdvanceProvenanceMemory` 钉死，`.jhw` 内容对同一个 build_nonce 永远确定，定成功/
  定失败都可以安全地在重试时重新推导，不需要跟 watermark advance 那种"一旦 Durable 就已经
  消耗、必须记忆"的逻辑）。新增 `PhaseAdvanceStatus::SealJournalStoreLeaseNotHeld/
  SealJournalStoreLeaseFenced/SealJournalStoreLeaseDirectoryIdentityChanged/JhwWriteFailed`。
- `test_intent_phase_advancer.cpp`：全部改用 TwoLeases 测试 helper（同时 acquire 两把锁，按
  LOCK ORDER 顺序 release），新增 WritesDurableJhwAndX1BeforeReplacingIntent（验证 `.jhw`
  真实落盘、字段正确）、RefusesWithoutSealJournalStoreLease（对称于既有的
  RefusesWithoutCandidateLease，原 RefusesWithoutLease 改名）；既有的
  RetryAfterX1FailureReusesSameWatermarkBoundIdsInsteadOfBurningAnother 顺带验证了 `.jhw`
  CREATE_NEW 在重试路径上的 byte-compare-on-collision 自愈行为（不需要额外测试，这个场景
  天然被这条既有用例的重试步骤覆盖到）。

**明确不做（这一轮之后 Building→Reserved 的两个 id-reserve 写终于都做完了，但仍然不做）**：
`.xgc`/GC 授权；journal drain-completeness 前置检查；Reserved→StartedPublished 及之后任何边；
`highest_committed_journal_seq` 后续的 REPLACE-with-monotonic-CAS（`.jhw` 生命周期里 CREATE_NEW
之后的部分，属于 StartedPublished 及之后，本仓库目前完全没有）。运营范围延续"只做到类存在、
测试证明逻辑对，不接入生产路径"（未变）。

**验证（2026-08-15）**：
- MSVC Release 全量构建：0 warning。全量 ctest：1097/1097（1096 基线 + 本轮新增 1 个测试，1 个
  既有 POSIX-only skip）。
- WSL2 GCC-14 Release（`HY_BUILD_DEMO=ON`）：1126/1126（1125 基线 + 本轮新增 1 个测试）。
- WSL2 ASan+UBSan（同前几轮的 OOM 规避，这次目标列表一次性包含了
  compaction_lease_holder，没有重复上一轮"漏掉辅助程序"那个失误）：95/95，无 ASan/UBSan 报告。
- WSL2 TSan：`ctest -L concurrency`：58/58。两个负控均正确报出 `WARNING: ThreadSanitizer: data
  race`。
- `tools/spec_xref_check.py --quiet`、`tools/spec_enum_diff.py`：均 clean（跟之前几轮相同的
  既有、无关警告）。

### Round F：两把锁顺序 TLA+ 模型 + `.jhw` 审计工具扩展[已实现]

PR #40 引入的 `CandidateLease`/`SealJournalStoreLease` 两把锁顺序约束（只靠
`seal_journal_store_lease.hpp` 的文档纪律，编译器不强制）此前没有任何形式化模型或跨文件审计
工具覆盖。这一轮按已建立的"2-模型并行派发协议"（见本文件历史，PR #38 的 Grok 4.6/DeepSeek V4
Flash 分工先例）派发两块零文件交集、只读/纯新增的独立准备工作：**真正写 Reserved→
StartedPublished 这条边（需要真实的 "publish NativeV2Started" 写路径，跟 Round D 五版被拒
历史同一量级的设计风险）继续由协调者自己做，不外包，这一轮两块产出都不触碰这条边**。

**Grok 4.6：`formal/RoundELockOrder.tla`（+ .cfg 正确配置 + _bug.cfg 负控）**——单进程状态机
（不是两个并发调用者的死锁模型）：布尔量 candidateLeaseHeld/sealJournalLeaseHeld/
jhwWritten；AcquireSealJournalLease 的正确 Next 要求 candidateLeaseHeld 已经为真才能
获取内锁；WriteJhw 要求两把锁都已持有（对应 `raise_intent_phase()` Step 1b）；
ReleaseCandidateLease 要求内锁已经为假才能放外锁。安全不变式 `LockOrderRespected ==
sealJournalLeaseHeld => candidateLeaseHeld`（guard-implies-invariant）。_bug.cfg 用常量
RequireCandidateHeldForSealJournal = FALSE 换掉 AcquireSealJournalLease 的
candidateLeaseHeld guard，TLC 必须报出 `Invariant LockOrderRespected is violated`——这个
模型刻意带负控，不沿用 `RoundEFDesignReceiptVerified.tla`（上一轮 Grok 自己的产出）"没有历史
事故可以回归、所以没有负控"的先例，因为这次的锁顺序是一条真实、当前只靠文档约束的规则。
协调者独立用 TLC 重跑两个配置：`RoundELockOrder.cfg`（10 states generated / 6 distinct /
depth 6，无违反）、`RoundELockOrder_bug.cfg`（12 states generated / 8 distinct / depth 7，
`Error: Invariant LockOrderRespected is violated`，反例状态序列跟设计预期完全一致：
AcquireSealJournalLease 在 candidateLeaseHeld=FALSE 时直接把 sealJournalLeaseHeld 置真）
——结果跟 Grok 自己报告的完全一致，未发现问题，无需修复。

**DeepSeek V4 Flash：扩展 `native/src/seal_journal_cross_file_audit.cpp`（对已有文件的 diff，
不是新文件）**——新增第 8 种 artifact 类型 `SealJournalCommitWatermark`（`.jhw`，
`<candidate_id_hex16>.jhw`，位于 `seal-journal/<store_uuid>/`，一个跟其余 7 种完全不同的目录
scope）：新增 `scan_seal_journal_store()`（按 Intent 的 `candidate_id` 选取要审计的那一份
`.jhw`，跟 breadcrumb 目录扫描分开调用后合并进同一个 BreadcrumbSet）；CLI 新增
`--seal-journal-dir`（默认 `<dir>/seal-journal`，跟 `--gen` 写出的布局一致）；规则表
92→97 条，新增 5 条（依据 `intent_phase_advancer.hpp` Step 1b 的真实写入代码）：
`JHW.store_uuid_lo/hi==Intent.store_uuid_lo/hi`、`JHW.candidate_id==Intent.candidate_id`
（`Intent.phase < Reserved` 时 Unverifiable）、`JHW.kek_key_id==Intent.kek_key_id`（用的是
Intent 自己的 key，不是 L 的——这条边 L 还不存在）、`JHW.candidate_id+1==WM.next_candidate_id`
（**严格相等，不是现有 L/Intent-vs-WM 规则用的 `<` 区间不等式**，因为 `.jhw` 绑定的是
advance 之前那个精确的 `next_candidate_id` 值）。刻意排除
`highest_committed_journal_seq==0` 进规则表——那是新建 watermark 的 decode-time/shape 属性
（`validate_seal_journal_commit_watermark_shape()` 已经在编码层面保证），不是跨文件
MUST-equal 规则。首轮 `--self-check` 抓到 2 处真实级联（往 `Intent.store_uuid_lo/hi` 注入错误
值时，除了原有的 L/X1[1-3] 也会连带违反新的 `JHW.store_uuid_lo/hi==Intent.store_uuid_lo/hi`
规则；往 `Intent.candidate_id`/`WM.next_candidate_id` 注入时同理连带触发
`JHW.candidate_id+1==WM.next_candidate_id`）——这正是这个工具"control must still fail"纪律
在起作用，期望违反集已经据此补全。协调者复核：读完整 diff（509 行）、独立在 MSVC 上构建 0
警告 + 直接运行 `--self-check`（102 injections + 3 clean scenarios + empty dir，全部 PASS，
跟 DeepSeek 报告的数字完全一致）+ 手工冒烟测试（`--gen`→`--audit` 干净往返 0 violated；单独
注入 `JHW.kek_key_id==Intent.kek_key_id` 后 `--audit` 精确报出这一条 VIOLATED，其余不受影响）+
独立在 WSL2 GCC-14 上构建 + 跑 `--self-check`（结果一致）——未发现问题，无需修复。

**这一轮之后仍然明确不做**：Reserved→StartedPublished 及之后任何边；`.xgc`/GC 授权；journal
drain-completeness 前置检查；`highest_committed_journal_seq` 的 REPLACE-with-monotonic-CAS。

**CI 集成**：RoundELockOrder.tla/.cfg/_bug.cfg 接入 PR #41 刚合并的 `ci-spec-verification.yml`
`tla-model-check-all` 合并 job，作为第 8 个 model step（m_lockorder），跟其余 7 个模型同样的
`continue-on-error: true` + Aggregate 汇总模式；`.jhw` 审计工具扩展不需要任何 CMakeLists.txt
改动（`add_executable(seal_journal_cross_file_audit ...)` 早已注册，`--self-check` 走的还是
同一个 seal_journal_cross_file_audit_selfcheck ctest）。

**验证（2026-08-16）**：
- MSVC Release 全量构建：0 warning。全量 ctest：1097/1097（跟上一轮相同——这一轮没有新增 C++
  测试，audit 工具的自检内容本身在既有的 seal_journal_cross_file_audit_selfcheck 里扩展，
  1 个既有 POSIX-only skip）。
- WSL2 GCC-14 Release（`HY_BUILD_DEMO=ON`）：1126/1126（跟上一轮相同）。
- WSL2 ASan+UBSan（同前几轮的 OOM 规避，目标列表含 compaction_lease_holder +
  seal_journal_cross_file_audit）：95/95，无 ASan/UBSan 报告；audit 工具 `--self-check` 在
  ASan/UBSan 下同样 102+3+1 全过。
- WSL2 TSan：`ctest -L concurrency`：58/58。两个负控均正确报出 `WARNING: ThreadSanitizer: data
  race`。
- `tools/spec_xref_check.py --quiet`、`tools/spec_enum_diff.py`：均 clean（跟之前几轮相同的
  既有、无关警告）。
- `formal/RoundELockOrder.tla`：协调者独立用 TLC 重跑正确配置 + 负控配置，结果与 Grok 报告完全
  一致（见上）。

### Round G：raise_intent_phase() 幂等重试 TLA+ 模型 + store 级别 `.jhw`/`.jts` 诊断工具[已实现]

延续 Round F 的 2-模型并行派发协议，这一轮的两块候选（同样零文件交集、只读/纯新增）分别补上：
`raise_intent_phase()` 内部三个 provenance-memory 结构（`WatermarkAdvanceProvenanceMemory`/
`JhwWriteProvenanceMemory`/`X1WriteProvenanceMemory`）各自不同的 armed 规则从未被形式化验证过；
`seal_journal_cross_file_audit.cpp` 只审计一个 Intent 选中的那一份 `.jhw`，没有任何工具能一次性
列出一个 store 目录里全部 candidate 各自的 `.jhw`/`.jts`。**Reserved→StartedPublished 这条真正
的下一个实现边继续留给协调者自己做，不外包，这一轮两块产出都不触碰它。**

**Grok 4.6：`formal/RoundERetryIdempotency.tla`（+ .cfg 正确配置 + _bug.cfg 负控）**——单进程
状态机（同 RoundELockOrder.tla 先例）：变量 watermarkAdvanced/boundCandidateId/
firstBoundCandidateId（history/ghost，记住第一次绑定的 id，状态不变式需要"看见上一态"时必须
靠它，因为 CI 抓的是 INVARIANT 行不是 temporal property）/jhwWritten/x1Written。正确 Next 的
AdvanceWatermark(id) 要求 `~watermarkAdvanced`（对应真实代码 `watermark_provenance_memory_.
has_value() && advanced` 就直接复用绑定值、不重新推导）；`.jhw`/`.x1` 两步（选项 (a)，未
建模各自的 provenance-memory/Uncertain 状态，只要求它们必须晚于 watermarkAdvanced）。安全
不变式 `NeverRebindsCandidateId == watermarkAdvanced => boundCandidateId = firstBoundCandidateId`。
_bug.cfg 用常量 ReuseBoundIdsOnRetry=FALSE 去掉 `~watermarkAdvanced` guard，TLC 必须报出
`Invariant NeverRebindsCandidateId is violated`。设计偏离（均已在报告里说明理由，核实后认为
合理，不需要改）：id 空间不含单独的 request_id 变量（`(candidate_id, request_id)` 是同一次
advance 一起 spend 的一对，拆开只会放大状态空间，不改变负控本身）；MaxCandidateId 钉死为 2
（负控必须能选到一个不同的 id，否则两次 Advance 仍可能巧合绑同一个值，负控会空转）；没有单独
建模"重试"动作（重试第 1 步本身就是再次调用 AdvanceWatermark，正确版本已经被 guard 挡住）。
协调者独立用 TLC 重跑：正确配置 11 states generated / 9 distinct / depth 4，无违反；负控配置
`Error: Invariant NeverRebindsCandidateId is violated`，反例状态序列（Init → 第一次
AdvanceWatermark 绑定一个 id → 第二次 AdvanceWatermark 在 watermarkAdvanced 已经为真时
绑定另一个 id）跟设计预期完全一致——具体 states generated/distinct 数字（6/5 vs Grok 报告的
5/4）因 TLC 多 worker 并行搜索在找到第一个反例后提前退出、探索顺序不确定而有微小出入，属于
TLC 已知的良性非确定性，不是真实差异（错误字符串、反例结构完全一致）。未发现问题，无需修复。

**DeepSeek V4 Flash：新文件 `native/src/seal_journal_store_dump.cpp`**——纯只读诊断工具（不
获取任何 lease，跟 `seal_journal_cross_file_audit.cpp` 同一套纪律）：`seal_journal_store_dump
--dir <store 目录> [--kek-hex <64hex>]`，枚举目录、按文件名模式识别 `.jhw`（`<16hex>.jhw`）/
`.jts`（`<16hex>-<16hex>.jts`），复用已有的 `encode/decode_seal_journal_commit_watermark_wire`/
`decode_seal_journal_tombstone_wire`，MAC/解码结果三态分类（valid/invalid/decode failed）,
按 candidate_id 分组展示、`.jts` 按 journal_seq 排序，文件名跟解码字段矛盾（candidate_id 或
journal_seq 不一致）单独报 warning 不中断，无法识别的文件名/非常规文件（目录等）同样只警告
跳过。没有 `--self-check`/`--gen` 模式（那是审计工具的自证方式，这个是纯展示型诊断工具），
不需要 ctest 注册。协调者复核：读完整文件（470 行）、独立在 MSVC 上构建 0 警告 + 用
`seal_journal_cross_file_audit --gen --seal-journal-dir` 生成的真实 fixture 加上手工构造的
损坏/不完整 `.jts`（复制 `.jhw` 字节冒充 `.jts`，触发"decode failed: truncated"）、未知文件、
非常规目录条目做端到端冒烟测试，输出完全符合预期（正确分组、正确报告字段、正确警告、不中断、
exit 0）——在 MSVC 和 WSL2 GCC-14 上各自独立复现同一测试，结果一致；未发现问题，无需修复。

**CI 集成**：RoundERetryIdempotency.tla/.cfg/_bug.cfg 接入 `ci-spec-verification.yml`
`tla-model-check-all` 合并 job，作为第 9 个 model step（m_retryidempotency）；
seal_journal_store_dump 新增 CMakeLists.txt 注册（add_executable/
target_link_libraries/hengyuan_set_warnings，紧跟在 seal_journal_cross_file_audit 后面），
无 ctest 条目。

**这一轮之后仍然明确不做**：Reserved→StartedPublished 及之后任何边；`.xgc`/GC 授权；journal
drain-completeness 前置检查；Intent REPLACE 本身的幂等重试建模（这次只建模前三步）；
`.jhw`/`.x1` 的 `PublishedNamespaceUncertain` 误判场景的形式化验证（Grok 报告里明确点出这是
另一条性质，这次选项 (a) 没做）。

**验证（2026-08-16）**：
- MSVC Release 全量构建：0 warning。全量 ctest：1097/1097（跟上一轮相同——这一轮没有新增
  ctest 条目，1 个既有 POSIX-only skip）。
- WSL2 GCC-14 Release（`HY_BUILD_DEMO=ON`）：1126/1126（跟上一轮相同）。
- WSL2 ASan+UBSan（同前几轮的 OOM 规避，目标列表含 compaction_lease_holder +
  seal_journal_cross_file_audit + seal_journal_store_dump）：95/95，无 ASan/UBSan 报告；
  audit 工具 `--self-check` 同样 102+3+1 全过；store_dump 端到端冒烟测试在 ASan/UBSan 下同样
  干净。
- WSL2 TSan：`ctest -L concurrency`：58/58。两个负控均正确报出 `WARNING: ThreadSanitizer: data
  race`。
- `tools/spec_xref_check.py --quiet`、`tools/spec_enum_diff.py`：均 clean（跟之前几轮相同的
  既有、无关警告）。
- `formal/RoundERetryIdempotency.tla`：协调者独立用 TLC 重跑正确配置 + 负控配置，结果与 Grok
  报告一致（见上，状态计数的微小出入已说明原因）。

### Seal-journal Round E Reserved->StartedPublished（NativeV2Started）[已实现]

Round G 之后，用户在"继续外包派发 2 个任务"和"开始设计真正的下一条实现边"之间选择了后者——这一轮
不再外包，协调者自己设计并实现 `raise_started_published()`：`IntentPhaseAdvancer`（
`native/include/hengyuan/intent_phase_advancer.hpp`）新增的第二个写方法，Reserved->
StartedPublished 边（`.x1` seq=2），仅覆盖 NativeV2Started（greenfield）路径。同一个类现在实现
两条边；MigratedV2Started（V+M legacy-companion 写路径）与 StartedPublished->{PostSealFinalizing,
AbandonFinalizing} 两条终边不在这一轮范围内。**跟 raise_intent_phase() 一样，这一整轮是
test-only**——本仓库没有任何生产调用路径会走到这个函数，也没有任何 PostSeal 恢复实现能正确处理
它写出的 L 文件（见下面"操作性风险"）。

**设计经过 3 轮对抗性审查才定稿（v1→v2→v3），过程中两轮各自发现真实问题，均已独立核实并修正**：

- **Finding 0（第一轮发现，最终定性为设计纪律而非需要修复的 bug）**：`walk_x1_chain_raw()`
  （`compaction_intent_codec.hpp`）的 `TerminalBranchConflict` 检查（校验 `after.phase` 等于
  链条终点的 `to_phase`）只在传入的链条恰好是 3 帧时才生效——对 1 帧（Building->Reserved）和
  2 帧（Reserved->StartedPublished）链条都会被静默跳过。v1 曾提议删掉这个 `size()==3` 门槛来
  堵住这个校验缺口，但这会破坏 `IntentStore::inspect_x1_chain()`（`compaction_intent_store.hpp`）
  在文档化的崩溃恢复窗口期（`.x1` seq=1 已落盘但 Intent REPLACE 还没到达，磁盘上的
  `Intent.phase` 因此合法地落后于链条）对这种宽松行为的合法依赖——协调者独立读
  `inspect_x1_chain()` 源码核实后确认 v1 的修复方案本身是错的，改为 v2 的正确做法：
  `raise_intent_phase()`（既有）与 `raise_started_published()`（新增）各自在调用未改动的
  `walk_x1_chain_raw` 之后，自己额外加一条 `after.phase == 本边目标 phase` 的显式检查——编译器
  不强制，但对整个状态机是闭合的（合法链条拓扑只有 1/2/3 帧三种，两条尚未实现的 3 帧终边落地时
  会自动被共享函数自身的 `size()==3` 检查覆盖）。
- **字段绑定表遗漏（第一轮发现）**：v1/v2 的 Step 2 绑定字段列表漏了
  `started.new_generation == before.target_generation`——通过 L4 spec 的
  `CURRENT.generation == C.new_generation`（约 3882 行）和 `CURRENT == I.target_generation`
  （约 4257-4258 行）传递约束同一个 `CURRENT`，v2 已补上。
- **引用错误（第二轮发现）**：v2 的说明文字里有一处引用张冠李戴（"recovery one-step lag"那句话
  误标为出自 `intent_phase_advancer.hpp`，实际出自 L4 spec 文档）和一处不精确的先例引用
  （`create_seal_journal_commit_watermark_no_replace` 被引作 `CandidateLease` 同类先例，实际
  定义在 `SealJournalStoreLease` 上）——v3 已修正，协调者独立复核（`sed`/`grep` 核对原文）确认
  修正无误。
- 第三轮审查结论："design is ready for implementation... No v4 needed."

**跟 raise_intent_phase() 一个根本不同的信任模型**：`SealIdWatermark`/`SealJournalCommitWatermark`
的内容完全由 `raise_intent_phase()` 内部从已有的 Intent 状态推导出来，调用方没有任何东西需要提供。
`SealExportStartedWire` 描述的是真实的、已经完成的 seal 输出（`new_generation`/`new_final_seq`/
`new_final_tip_mac`/`content_root`），本仓库没有真实的 compaction 执行引擎能算出这些字段——
`raise_started_published()` 无法自己推导，因此 `started` 是调用方提供的参数，在 Step 2 里做校验
（不是信任）：先逐字段核对跟这个 Intent 自己的 store_uuid/candidate_id/request_id/
source_generation/target_generation/baseline 四元组/kek_key_id 是否绑定一致（不一致返回
`StartedForeignBinding`），再调用既有的 `validate_seal_export_started_shape()` 校验拓扑自洽性
（不通过返回 `StartedShapeInvalid`）。

**操作性风险，比 raise_intent_phase() 更尖锐**：`SealExportStartedWire` 自己的文档注释
（`durable_control_plane.hpp:951-952`）写着"Presence forces PostSeal recovery even when no local
seal Ack was observed"——未来一个真实的 PostSeal 恢复实现（本仓库目前不存在）一旦看到这个
test-only 调用写出的 L 文件，会被强制当成"真的发生过一次 seal 尝试"的证据，即便实际上什么都没
发生。在 StartedPublished->{PostSealFinalizing, AbandonFinalizing} 及至少 PreSeal-abandon 清理
的雏形落地之前，不能把这两个函数接进任何真实的 compaction 触发流程。

**实现内容**：
- `CandidateLease::create_seal_export_started_no_replace()`（`compaction_lease.hpp`）——第 6 个
  写方法，CREATE_NEW-only（这个 L 文件只在 StartedPublished 写一次；PostSeal/Abandon 清理会
  最终 unlink 它，不在这一轮范围内），跟 `create_seal_id_watermark_no_replace` 完全同一套
  CREATE_NEW 模式。
- `IntentPhaseAdvancer::raise_started_published(build_nonce, started)`——Step 0 读 Intent（要求
  phase==Reserved，past 且 ids 合法返回 `AlreadyAtOrPastTargetPhase` 幂等重试）；Step 1 用既有
  的 `CandidateLease::read_x1_frame()` 读回 durable 的 seq=1 `.x1` 帧（缺失/损坏分别返回
  `SeqOneX1NotFound`/`SeqOneX1Corrupt`）；Step 2 校验 `started`（见上）；Step 3 构造 `after`
  （ids 不变）+ seq=2 `CompactionIntentTransitionWire`；Step 4 直接调用
  `compaction_codec_detail::walk_x1_chain_raw`（不经过 `validate_intent_transition()`——那个
  函数硬编码 `before.phase == Building`，是 Building->Reserved 专用，这条边不能复用）+ 显式的
  `after.phase == StartedPublished` 检查（Finding 0 的纪律）；Step 5 CREATE_NEW L 文件，配
  `StartedWriteProvenanceMemory`（跟 `JhwWriteProvenanceMemory` 同一个形状——只在 `Uncertain`
  时需要同进程记忆，因为内容一旦 `started` 参数确定就是确定性的，Definite 成功/失败都可以在
  重试时安全地重新推导）；Step 6 复用既有的 `write_x1_frame_no_replace`/
  `X1WriteProvenanceMemory`（本来就按 `transition_seq` 泛化，不需要改动）写 `.x1` seq=2；
  Step 7 复用既有的 `replace_intent_phase` REPLACE Intent 到 StartedPublished。
- 新增 5 个 `PhaseAdvanceStatus` 枚举值：`SeqOneX1NotFound`/`SeqOneX1Corrupt`/
  `StartedForeignBinding`/`StartedShapeInvalid`/`StartedWriteFailed`。
- `test_intent_phase_advancer.cpp` 新增 8 个测试：成功路径（验证 Intent.phase、L 文件内容、`.x1`
  seq=2 落盘）、幂等重试、仍在 Building 时拒绝、外部绑定不一致拒绝、拓扑不自洽拒绝、外部
  build_nonce 拒绝、seq=1 `.x1` 缺失拒绝、seq=1 `.x1` 损坏拒绝。

**验证（2026-08-16）**：
- MSVC Release 全量构建：0 warning。全量 ctest：1105/1105（含新增 8 个测试；1 个既有
  POSIX-only skip）。
- WSL2 GCC-14 Release（`HY_BUILD_DEMO=ON`）：1134/1134。
- WSL2 ASan+UBSan（同前几轮的 OOM 规避手法，`HY_BUILD_DEMO=OFF` 目标构建：
  test_intent_phase_advancer + compaction_lease_holder + seal_journal_cross_file_audit）：
  IntentPhaseAdvancer 测试 20/20（含新增 8 个），audit 工具 `--self-check` 1/1，均无 ASan/UBSan
  报告。
- WSL2 TSan：`ctest -L concurrency`：58/58（含 `CandidateLease` 系列，覆盖新增写方法所在的类）。
  两个负控均正确报出 `WARNING: ThreadSanitizer: data race`。
- `tools/spec_xref_check.py --quiet`：500 个符号、31 个文件、43037 个引用点，OK。
  `tools/spec_enum_diff.py`：OK（跟之前几轮相同的既有、无关警告——`SubmitOutcome::RateLimited`
  尚未在代码中实现，跟本轮无关）。

**这一轮之后仍然明确不做**：MigratedV2Started 写路径；StartedPublished->{PostSealFinalizing,
AbandonFinalizing} 两条终边；`.xgc`/GC 授权；任何真实网络 seal 调用；把这两个函数接进任何真实的
compaction 触发流程（见上"操作性风险"）。

### Seal-journal Round E StartedPublished->{PostSealFinalizing, AbandonFinalizing}[已实现]

PR #44 合并后，用户明确要求继续做 `IntentPhaseAdvancer` 剩下的两条边——`StartedPublished->
PostSealFinalizing` 和 `StartedPublished->AbandonFinalizing`（`.x1` seq=3，二选一，互斥）。这两条
边正是上一轮头注释里点名"字段映射表在早期草稿（v1）里被查出真实 bug、推迟到未来一轮"的那两条：

1. 字段映射表比对了 `CompactionCandidateIntentWire` 根本没有的字段（写死在一个想象中的/过时的
   struct 形状上）。
2. 死代码前置检查 `phase >= kSealStartedCleanupPhaseAuthorized`，而
   `kSealStartedCleanupPhaseAuthorized == 0`——恒真，等于没检查。
3. `AbandonFinalizing` 边把 `GenGone`（phase 5）当作足够的授权证据，但 `GenGone` 只证明 gen-N+1
   目录已经改名挪走，不证明 tip 已经针对 abandon baseline 重新核对过——只有 `ResumeAuthorized`
   （phase 6）才证明这个。

本仓库没有任何代码路径能真正走完 `.clr`/`.abd` 的真实授权流程（完整状态机、
`TipExportProducerResume`、journal drain-completeness 检查、`.xgc` 全都不存在）。用户确认采用
方案 (b)：`raise_post_seal_finalizing()`/`raise_abandon_finalizing()` 本身对 `.clr`/`.abd`
**只读**（复用已经落地、在 `seal_journal_breadcrumb_precondition_aggregate.hpp` 里已经使用的
`SealStartedCleanupTombstoneLoader`/`SealStartedAbandonLoader`），从不写入——测试直接用
std::ofstream 种文件（跟本文件已有的 `plant_watermark()`/`test_compaction_lease.cpp` 的
`write_raw_file()` 同一先例），不需要给 `CandidateLease` 新增任何写方法或 friend。两条边合并成
一轮/一个 PR：结构上近乎镜像对称（都是"读一份已 durable 的终态证据 + 3-帧 `.x1` 链校验 + Intent
REPLACE"），且 `AlreadyAtOppositeTerminalPhase` 这类跨分支保护（PostSeal 看到 Intent 已经在
AbandonFinalizing，反之亦然）只有两条边都实现了才能被真实测试到。

**设计经过 1 轮 Explore 双 agent 研究 + 1 轮 Plan agent 设计 + 1 轮 DeepSeek 对抗性审查，9 条发现
逐条对照实际代码验证后确认为真实问题，全部采纳**：

1. **最大简化**：原计划打算新增一个 test-only planter 类 + `CandidateLease` 两个新 CREATE_NEW
   写方法 + 新 friend，理由是"这样两个新函数只读的属性能在 diff 里被肉眼核实"——审查指出这个属性
   不依赖 planter 存在，函数体是否调用 lease 写方法是函数体自己的事，且本仓库已有
   `plant_watermark()`/`write_raw_file()` 直接 ofstream 种文件的先例。采纳后这一轮完全不改
   `compaction_lease.hpp`。
2. **Step 0 三分支不对称**：跨分支判定（`AlreadyAtOppositeTerminalPhase`）漏了 ids 合法性核对，
   一条 `candidate_id==0` 的损坏记录会被误判成"对面已完成"而不是 `IllegalTransition`——已用
   `is_legal_candidate_ids_for_phase()` 的真实签名核实并修正，三个分支（本函数目标终态/对面
   终态/其余）现在统一核对 ids 合法性。
3. **`walk_x1_chain_raw()` 不校验帧的 candidate_id/request_id**（`compaction_intent_codec.hpp`
   的 ForeignBinding 检查只比对 store_uuid/kek_key_id/generations/baseline/build_nonce）——两个
   新函数读回 seq=1/seq=2 帧后补一条显式 ids 核对；**这个防御性缺口在已合并的
   `raise_started_published()` 里同样存在**，这一轮顺手给它的 Step 1 也补上同样两行（新增回归
   测试 RefusesStartedPublishedWhenSeqOneX1IdsMismatch）。
4. `LoadStatus::IoError` 必须显式映射到 `ClrCorrupt`/`AbdCorrupt`（已用
   `seal_started_abandon_loader.hpp` 的真实分支核实是可达分支，不是理论上的）。
5/6. "明确不做"清单本来遗漏了两条真实省略——`.clr` 跟已落盘的 L 文件之间的跨文件字段核对完全不做
   （`seal_export_migration_cleanup_abandon_codec.hpp` 自己的注释已经写明"explicitly out of
   scope"）；Abandon 边 spec 的 Forbidden 从句是"`ResumeAuthorized` 且 A 仍在磁盘"的合取，这一轮
   只落地 phase 精确相等这一半（`.abd` 文件不存在时 loader 已经返回 `NotFound`，隐式满足但不是
   独立显式检查）——均已补进头注释 SCOPE 段落。
7. 事实性用词修正：`SealStartedCleanupTombstoneLoader`/`SealStartedAbandonLoader` 不是"落地未
   使用"，已经在 `seal_journal_breadcrumb_precondition_aggregate.hpp` 里被使用。
8. Planter 改直接种文件后新增的提醒：`encode_seal_started_cleanup_tombstone_wire`/
   `encode_seal_started_abandon_wire` 在形状不合法时**静默返回 0**（不是 assert）——测试 helper
   （plant_clr/plant_abd）的 `ASSERT_EQ(编码返回值, 期望字节数)` 是防止这类 bug 静默写出空
   文件的关键一环，不是可省略的装饰。
9. 正面确认（不用改）：新终态落地后，既有 `raise_started_published()` 对 phase∈{3,4} 已正确返回
   `AlreadyAtOrPastTargetPhase`（幂等重试语义恰好正确），未改动。

**实现内容**（`native/include/hengyuan/intent_phase_advancer.hpp`）：
- 两个新公开方法，均只接受 `build_nonce`（证据文件不是这次调用产生的，是读一份已经存在的）：
  `raise_post_seal_finalizing()`（要求 `.clr` 已 `phase==kSealStartedCleanupPhaseAuthorized`）、
  `raise_abandon_finalizing()`（要求 `.abd` 已
  `phase==kSealStartedAbandonPhaseResumeAuthorized(6)`，`.abd` 本身从不被 unlink——spec:
  "while A is still on disk"）。
- 新私有 helper `read_verified_seq1_and_seq2()`：两个新函数共用，读回 durable 的 seq=1/seq=2
  `.x1` 帧、MAC 验证、核对 ids（Finding 3）。返回 `bool` + out-param 失败状态（不复用
  `PhaseAdvanceStatus` 里的某个真实枚举值当"成功"哨兵——那本身是个隐患，被在实现阶段自己发现并
  改掉了，改用 `bool` + `out_failure_status` 更明确）。`raise_started_published()` 自己的单帧
  Step 1 保持原样、不重构成调用这个 helper（尽量不动已发布代码的结构），只加两行 ids 核对
  （Finding 3）。
- 两个函数各自 Step 0-6：读 Intent（三分支前置检查，Finding 2）→ 读 seq1/seq2 → 读 `.clr`/`.abd`
  并核对绑定字段+phase 门槛（Finding 2 的 bug #2/#3 修法：`.clr` 精确等于 `Authorized(0)`；`.abd`
  精确等于 `ResumeAuthorized(6)`，不是 `>=`）→ 构造 `after`+seq=3 `CompactionIntentTransitionWire`
  → `walk_x1_chain_raw`（3 帧链条，`TerminalBranchConflict` 这次真的会触发,显式检查仍保留做防御性
  冗余）→ CREATE_NEW `.x1` seq=3（复用既有 `X1WriteProvenanceMemory`）→ `replace_intent_phase`。
- 新增 8 个 `PhaseAdvanceStatus` 枚举值：`SeqTwoX1NotFound`/`SeqTwoX1Corrupt`（两个新函数共用）、
  `AlreadyAtOppositeTerminalPhase`、`ClrNotFound`/`ClrCorrupt`/`ClrKeyNotFound`/
  `ClrForeignBinding`/`ClrNotAuthorized`（PostSeal 专用）、`AbdNotFound`/`AbdCorrupt`/
  `AbdKeyNotFound`/`AbdForeignBinding`/`AbdNotResumeAuthorized`（Abandon 专用）。
- `test_intent_phase_advancer.cpp` 新增 32 个测试：1 个 `raise_started_published()` 的 Finding 3
  回归测试，PostSeal 15 个，Abandon 16 个（成功路径、幂等重试、Reserved/Building 时拒绝、跨分支
  拒绝、外部 build_nonce、证据缺失/损坏、phase 门槛不满足——含最重要的
  RefusesAbandonAtGenGoneNotResumeAuthorized 直接对应 bug #3，以及证明门槛精确相等而非 `>=`
  的 RefusesAbandonFinalizingWhenAbdAtAbdPending——绑定字段不一致、seq=1/seq=2 `.x1` 缺失/
  损坏）。

**验证（2026-08-16）**：
- MSVC Release 全量构建：0 warning。全量 ctest：1137/1137（含新增 32 个 + 1 个回填测试；1 个
  既有 POSIX-only skip）。
- WSL2 GCC-14 Release（`HY_BUILD_DEMO=ON`）：1166/1166。
- WSL2 ASan+UBSan（`HY_BUILD_DEMO=OFF` 目标构建：test_intent_phase_advancer +
  compaction_lease_holder + seal_journal_cross_file_audit）：53/53，均无 ASan/UBSan 报告。
- WSL2 TSan：**这一轮未单独重跑**——`test_intent_phase_advancer` 本来就不在
  `wsl_verify.sh` 的 run_thread 目标列表里（`raise_intent_phase()`/`raise_started_published()`
  落地时就是如此），这一轮不改 `compaction_lease.hpp`，没有新的并发/共享状态改动理由改变这一点。
- `tools/spec_xref_check.py --quiet` / `tools/spec_enum_diff.py`：OK，同前几轮。

**这一轮之后仍然明确不做**：`.clr` 跟 L 文件的跨文件字段核对；Abandon 边"A present"独立显式检查；
`.xgc`/GC 授权；`TipExportProducerResume`（本仓库不存在这个符号）；真实 unlink M/V/L 或 A；
`PostSealCommittedProof` 完整 4 项门槛（仅实现"`.clr` 已 Authorized"/"`.abd` 已
ResumeAuthorized"这一半，`CURRENT.generation` 核对/`LastRemoteAckedTip` 覆盖检查/
`GenerationBridge` 绑定/journal drain-completeness 继续不做）；MigratedV2Started；生产环境接线
（继续 test-only，风险比 `raise_started_published()` 更尖锐——test-only 调用现在能把 `Intent.
phase` 推到终态，未来真实的 PostSeal/Abandon 恢复实现会被迫当成"真实清理流程已完成"的证据）。

### Seal-journal Round E `.xgc`（CompactionIntentGcAuthorizedWire）decode + loader[已实现]

PR #45 合并后，用户要求继续做 `.xgc`/GC 授权那一轮——本仓库自 Round D 起每一轮"明确不做"清单里
都挂着、从未真正开工的最后一块拼图。

**范围确认经过（研究阶段派了 2 个 Explore agent，结果一处被证伪，记进本条目作为经验）**：第一个
agent 核实"现有代码到底有多少"，第二个核实"spec 到底要求什么"。第二个 agent 的 spec 结论是真实
且关键的：真正合法的 `.xgc` CREATE，spec（`BINANCE_PRIVATE_REST_L4_SPEC.md:3624-3671`）要求
`.clr`/`.abd` 必须先被真实 unlink 掉才能写 `.xgc`（"Forbidden: CREATE while L/V/M/Started still
present"），且 `PostSealFinalizingClear`/`AbandonFinalizingClear` 两个 disposition 都硬性要求
`cleanup_auth_flags` bit0（journal drain-complete）——这两件事本仓库都做不到，用户据此确认这一轮
范围收窄为"只读 decode + loader"，跟 `CompactionIntentGcAuthorizedWire` struct 自己的文档注释
（`durable_control_plane.hpp:1710-1715`）"can decode/verify... but no production code path ever
constructs/publishes one"完全一致。

**但第一个 agent 的结论是假的**：两个 agent 都报告"`encode_compaction_intent_gc_authorized_wire`/
`decode_compaction_intent_gc_authorized_wire`/`VerifiedX` 包装类完全不存在，grep 零匹配"。真正
打开 `compaction_intent_codec.hpp` 准备写这部分代码时才发现，这三样东西——外加
`native/tests/test_compaction_intent_codec.cpp` 里完整的 CompactionIntentGcAuthorizedCodec
测试组（round-trip、legacy v1 拒绝、total_bytes 不符、undefined flag bits、disposition 越界、
MAC 篡改检测、截断 buffer，8 个测试）——**从 PR #34（"Round D — CompactionCandidateIntent
codec/genesis"，2026-08-09，本仓库最早的一轮）就已经存在**，`git blame` 直接确认，不是任何并发
写入或时序问题。类名是 `VerifiedGcAuthorized`（不是猜测中的 VerifiedCompactionIntentGcAuthorized）。
**教训，记进这一轮**：任何 agent 报告"某某代码完全不存在"这种否定性结论，在真正开始写代码填补那个
"空白"之前，必须自己用 Grep 工具原地复核一遍——这条纪律此前主要用于"审查类 agent 的发现是否
真实"，这次证明同样适用于"探索类 agent 的'找不到'结论"，且后果更严重（如果没有在写代码前发现，
会直接产出跟已有代码重复定义、编译失败的废弃工作）。

**重新核实后的真实缺口**（这才是这一轮实际做的）：`peek_compaction_intent_gc_authorized_kek_
key_id()` 不存在；`ValidatedArtifactName::for_xgc(build_nonce)` 只是注释占位
（`compaction_breadcrumb_io.hpp:174-176`）；`CandidateLease` 没有任何 `.xgc` 读方法或 friend；
没有任何 loader 类。

**已确认 `decode_compaction_intent_gc_authorized_wire()` 自己的既有分层设计（不是这一轮的产出，
只是核实清楚）**：跟 `CompactionCandidateIntentWire`/`CompactionIntentTransitionWire` 两个兄弟
类型同一种"inline 逐字段检查"风格（不是 `seal_export_migration_cleanup_abandon_codec.hpp` 那种
独立 `validate_*_shape()` 函数风格）——`decode_*` 只做结构+MAC 校验，**不**调用
`is_legal_cleanup_auth_flags()` 那套 disposition/flag 组合业务规则，是有意的分层（HMAC 只证明
"持有 key 的人写的"，业务合法性是独立纯函数的职责，见 `compaction_intent_codec.hpp:482-487` 的
既有注释），这一轮的 loader 沿用同一条边界，不额外调用 `is_legal_cleanup_auth_flags()`。

**实现内容**：
- `peek_compaction_intent_gc_authorized_kek_key_id()`（`compaction_intent_codec.hpp`）——固定
  偏移量 +24 读 4 字节 `kek_key_id`，不认证，照抄 `peek_compaction_candidate_intent_kek_key_id`
  的模式。
- `CompactionIntentGcAuthorizedLoader`（新文件 `compaction_intent_gc_authorized_loader.hpp`）
  ——照抄 `seal_started_abandon_loader.hpp` 的 peek-then-pin-then-decode 模式，唯一的结构性
  差异：`.xgc` 文件名带 `build_nonce`（不是固定字面量），所以 `load()`/`load_from()` 比既有六个
  breadcrumb loader 多接一个 `build_nonce` 参数。
- `CandidateLease::read_compaction_intent_gc_authorized(build_nonce, out)` + `friend class
  hy::CompactionIntentGcAuthorizedLoader;`（`compaction_lease.hpp`）——只读，没有加任何写/
  CREATE_NEW 方法。
- `ValidatedArtifactName::for_xgc(build_nonce)`（`compaction_breadcrumb_io.hpp`）——把已经写好
  10 天的注释占位变成真实实现，跟 `for_x1()` 同一个 `format_hex16` 模式，去掉 `transition_seq`
  那一段（`.xgc` 是每个 candidate 一份，不是每个 transition 一份）。
- 新测试文件 `test_compaction_intent_gc_authorized_loader.cpp`（18 个测试，照抄
  `test_seal_started_abandon_loader.cpp` 的 mock-lease 单测结构：round-trip、5 类 outcome/
  read-status 映射、format_version/total_bytes 非法、MAC 三处篡改点、未知 key、4000 次随机字节
  property 测试、`static_assert` 构造性检查——外加一个既有六个 loader 测试文件都没有的第 13 项：
  用真实 `CandidateLease` + std::ofstream 直接种 `.xgc` 文件（跟 `plant_watermark()` /
  plant_clr / plant_abd 同一先例），端到端验证 `for_xgc(build_nonce)` 命名 + 新读方法真的接得上，并验证
  `build_nonce` 不同时正确返回 `NotFound`，证明 `.xgc` 身份真的按 `build_nonce` 区分）。
- `test_compaction_intent_codec.cpp` 给新 peek 函数补 2 个测试（成功读取；buffer 太短返回
  false）。

**实现过程中真实抓到的一个 UBSan bug（不是这一轮引入的新设计问题，是复制既有测试模板时带出来的
既有缺陷，被新的随机种子/参数第一次触发）**：新 loader 测试文件的 mock lease
（MockCandidateLeaseForGcAuthorized::read_compaction_intent_gc_authorized）里
`std::memcpy(out.data(), canned_bytes.data(), n)` 在 canned_bytes 为空、`n==0` 时，
canned_bytes.data() 可能返回 nullptr——`memcpy` 第二个参数在 glibc/GCC 下带 nonnull 标注，
即使长度是 0，传 nullptr 仍然是 UB，WSL2 UBSan 4000 次随机字节 property 测试真的踩中并报出
"null pointer passed as argument 2, which is declared to never be null"。这一行代码是逐字照抄
已经合并的 `test_seal_started_abandon_loader.cpp` 的 mock 结构写的——核实后确认
`test_seal_started_abandon_loader.cpp` 和 `test_seal_started_migration_cleanup_loader.cpp`
两个已合并文件里有完全相同的未加保护写法，只是这两个文件里 property 测试用的固定 RNG 种子（跟这
一轮相同的 `0xC0FFEE`）配合各自不同的 wire-字节数分布上界，此前的具体随机序列凑巧没有同时踩中
"len==0 且 read_status==Ok"这个组合，不代表那两个文件的写法本身是安全的。这一轮把自己新增的
mock 加了 `if (n > 0)` 保护并确认修复后 4000 次迭代全过；另外两个已合并文件的同一个 bug 已经
spawn 一个独立后续任务去修，不在这个 PR 里顺手改（保持这个 PR 的改动范围只覆盖 `.xgc` 相关文件）。

**明确不做**：`.xgc` CREATE/写路径；真实 C/A unlink（需要单写者锁协议 + fault-injection 崩溃
安全矩阵，本条目上面"Seal-journal Round E `.xgc`"研究阶段记录的要求）；journal
drain-completeness 检查；`TipExportProducerResume`（本仓库不存在这个符号）；调用
`is_legal_cleanup_auth_flags()`（loader 沿用 decode 自己的既有分层，不在这一轮新增调用点）；
任何生产环境接线。

**验证（2026-08-18）**：
- MSVC Release 全量构建：0 warning。全量 ctest：1157/1157（含新增 20 个测试；1 个既有
  POSIX-only skip）。
- WSL2 GCC-14 Release（`HY_BUILD_DEMO=ON`）：1186/1186。
- WSL2 ASan+UBSan（`HY_BUILD_DEMO=OFF` 定向构建：test_compaction_intent_gc_authorized_loader +
  test_compaction_intent_codec + test_compaction_lease + compaction_lease_holder）：真实抓到
  上述 UBSan bug 并修复后，55/55（`.xgc` 相关）全过，无 ASan/UBSan 报告。
- WSL2 TSan：这一轮改了 `compaction_lease.hpp`（新 friend + 新读方法），`test_compaction_lease`
  在 `wsl_verify.sh` 的 run_thread 目标列表里，这次没有跳过——`ctest -L concurrency`
  58/58，两个负控均正确报出 `WARNING: ThreadSanitizer: data race`。
- `tools/spec_xref_check.py --quiet` / `tools/spec_enum_diff.py`：OK，同前几轮。

### Seal-journal Round E MigratedV2Started（V+M companion 写路径，重新诠释为"L 已是 238B v2"场景）[已实现]

PR #46（`.xgc` decode + loader）合并后，用户要求"下一步该怎么做？请你自主推进"。派了 2 轮
Explore agent 研究才收敛到这一轮的真实范围。

**第一轮排查（journal drain-completeness，已否决）**：本仓库没有任何生产 lease 类支持目录枚举
（`SealJournalStoreLease`/`CandidateLease` 全部只有固定文件名或参数化的单文件读写，这是六轮
review 定下的安全纪律），且 `.jts` 在 `.jhw` 已经被删除后（drain-complete 的目标状态本身）结构性
地无法用现有的 `SealJournalTombstoneLoader::scan_up_to_watermark`（依赖存活的 `.jhw` 才能算出
探测范围）枚举到。即使只做 spec 里"journal side"那半句窄定义，也需要先给 `SealJournalStoreLease`
发明一个前所未有的目录枚举原语——用户确认放弃这个方向。

**第二轮排查（MigratedV2Started，范围收窄）**：最初假设跟 `raise_started_published()` 同构。
核实后发现 spec 里唯一具体的 V+M 写序程序（`BINANCE_PRIVATE_REST_L4_SPEC.md:4002-4016`）明确
标注"Offline migration (operator tool only — never hot-path auto-upgrade)"——前置条件是"L 已经
是一份 legacy 192 字节格式的预先存在文件"，本仓库从未 decode/encode 过这个格式（只有一个字节数
常量 `kSealExportStartedLegacyV1Bytes = 192`，没有对应的字段布局/codec）。用户确认范围：只做
V+M 的写步骤（spec 步骤 3-5），把"L"重新诠释成本仓库真正会写的 238 字节 v2 L（即
`raise_started_published()` 已经写过的那份）——不碰 legacy 192B 格式，在头注释里显式声明这是
对 spec offline-migration 步骤的诚实改编，不是字面场景的完整实现。用户随后对第一版计划提出
ExitPlanMode 拒绝（无附言），紧接着转发一份 GPT Tier B 评审（10 条发现），要求据此优化调整
方案再执行。

**GPT Tier B 评审 10 条发现，逐条核实后采纳情况**：
1. **（已用 Grep 核实为真）`encode_seal_export_started_wire()`（`seal_journal_precondition_
   codec.hpp`）不会先调用 `validate_seal_export_started_shape()`**——只有对应的 `decode_*` 才在
   MAC 校验前调用；这跟本文件（`seal_export_migration_cleanup_abandon_codec.hpp`）自己的三个
   `encode_*` 都先 shape-validate、不合法返回 0 的纪律不一样，是该文件自己头注释里已经承认的
   已知未回填缺口。**采纳**：新增 Step 3.5，构造 V 后、编码前，自己调用一次
   `validate_seal_export_started_shape(v)`，不合法 → `VShapeInvalid`，不写任何文件。
2. V 应该整体复制 L 再覆盖可变字段（`SealExportStartedWire v = l;` 再覆盖
   `kek_key_id`/`registered_producer_mask`/`producer_count`/`ring_id`），不是逐字段列出 closed
   set——更不容易漏字段（`new_key_id` 是最容易漏的一个，测试
   PublishesVWithClosedFieldsCopiedFromL 专门断言这个字段）。**采纳**。
3. `VKeyNotFound` 单独存在；M 不需要独立的 key-not-found——复用 V 那次已经 pin 成功的同一把
   `v_kek_key_id`。**采纳**（`MigratedV2StartedPublishStatus` 枚举没有为 M 单开 key-not-found
   分支，`publish()` 的 Step 4 直接复用 Step 3 的 `v_pin`）。
4. 不额外做存在性预读——直接走既有 CREATE_NEW + collision byte-compare，不加一次 syscall、不开
   TOCTOU 窗口。**确认，非改动**（本来就这么设计）。
5. 不合并/削减 V 和 M 各自的 fsync 边界——V 必须先确认 `DurablyPublished` 才能进入 Step 4。
   **确认，非改动**。
6. `PublishedNamespaceUncertain` 必须逐文件保守处理，V 和 M 完全独立、各自立即 `return`，同一
   实例同样入参重试即使命中 byte-equal 碰撞也必须仍然返回 `PublishUncertain`——跟
   `X1WriteProvenanceMemory` 等既有 provenance-memory 同一纪律。**采纳**：`VWriteProvenanceMemory`/
   `MWriteProvenanceMemory` 两个独立结构体，`Uncertain` 分支各自立即 `return`，`DurablyPublished`
   才 `reset()`。
7. 线程安全边界写进类头注释——单 owner 线程，不为这条冷路径加锁。**采纳**（头注释点 4）。
8. 既有的 null/UB 防护纪律显式保持——构造函数用引用、磁盘字节全部走固定长度
   `std::array<std::byte,N>` + fixed-extent `std::span`、decode 前严格 read-exact-size →
   peek → pin → decode 的顺序。**确认，非改动**。
9. MAC 比较继续只经由既有 `decode_*` 内部的 `constant_time_equal()` 发生，本类自己不用
   memcmp/== 直接比对任何 MAC 字节。**采纳**（头注释点 8；`m.legacy_mac`/`m.v2_mac` 只是复制
   L/V 各自已验证的 trailer，不参与判定）。
10. 测试列表补全（幂等重试、M 的 MAC 只能用 v_kek_key_id 验证、VShapeInvalid 时验证 V/M 都不
    存在、legacy-192-byte-sized L 的显式回归等）。**采纳**，见下方测试小节。

**实现内容**：
- 新文件 `native/include/hengyuan/migrated_v2_started_publisher.hpp`——
  `MigratedV2StartedPublisher` 类，非 phase-advancer（不属于 `IntentPhaseAdvancer`，从不推进
  `Intent.phase`）。`publish(build_nonce, v_kek_key_id, registered_producer_mask,
  producer_count, ring_id)` 实现 Step 0（读+验证 Intent，精确要求
  `phase == StartedPublished`）→ Step 1（读 L 的原始字节+decode+MAC 验证，`.mac` 供 Step 4
  的 `legacy_file_digest` 输入用真实磁盘字节而非重建）→ Step 2（核对 L 绑定字段真的匹配这个
  Intent，含 `L.new_generation == Intent.target_generation`）→ Step 3（整体复制 L 构造 V + 
  shape-validate + pin `v_kek_key_id` + 编码 + CREATE_NEW，`Uncertain` 立即返回）→ Step 4
  （只在 V 确认 `DurablyPublished` 后执行：SHA-256(L 原始字节)/SHA-256(V 编码字节) 作为
  `legacy_file_digest`/`v2_file_digest`；`legacy_mac`/`v2_mac` 直接复制 L/V 各自已验证的
  trailer；复用 Step 3 的 pin 编码 M + CREATE_NEW，`Uncertain` 立即返回）。
- `compaction_lease.hpp` 新增：`friend class hy::MigratedV2StartedPublisher;` +
  `CandidateLease::create_seal_export_started_migration_no_replace()`（CREATE_NEW，方法体
  照抄 `create_seal_export_started_no_replace` 的模式，文件名用既有的
  `ValidatedArtifactName::for_seal_export_started_migration()`）。V 复用既有的
  `create_seal_export_started_no_replace(/*legacy_or_greenfield=*/false, ...)`——这个方法本来
  就支持写两个文件名中的任意一个，这一轮只是第一次拿 `false` 调它，不需要新增方法。
- 新测试文件 `native/tests/test_migrated_v2_started_publisher.cpp`（15 个测试，真实文件 I/O，
  两把不同字节内容的 KeyRing key 分别签 L 和 V/M，使"M 的 MAC 只能用 v_kek_key_id 验证"这条
  断言真正有意义而非平凡为真）：成功路径（PublishesVWithClosedFieldsCopiedFromL 逐字段断言 V
  跟 L 完全一致，尤其 `new_key_id`；PublishesMWithCorrectDigestsAndMacs 用重新编码的 L/V 字节
  独立核算 digest/mac 是否吻合）；MacOnlyVerifiesUnderVKeyNotLegacyKey（M 的字节用 L 的 key
  decode 必须返回 `ChecksumMismatch`）；IdempotentRetrySameInputsReturnsPublished；
  RefusesWhileReserved/RefusesWhilePostSealFinalizing（后者真实调用
  `raise_post_seal_finalizing()` 把 Intent 推过 StartedPublished，证明门槛是精确相等不是
  `>=`）；RefusesForeignBuildNonce；L 相关三个（`LNotFound`、MAC 篡改 `LCorrupt`、**legacy
  192 字节尺寸的 L 显式回归 `LCorrupt`**——证明这个类复用的 `decode_seal_export_started_wire`
  只认 238 字节 v2）；RefusesWhenLForeignBinding；RefusesIllegalTopologyAndWritesNeitherVNorM
  （`registered_producer_mask==0`，并验证 V/M 事后都不存在）；RefusesWhenVKeyNotFound；
  VWriteConflictReturnsVWriteFailed/MWriteConflictReturnsMWriteFailed。
- 源码走查（非跑起来的测试，`Uncertain` 路径本身是既有四个 `raise_*` 函数的测试套件都没有强行
  构造过的平台级不确定性场景，这一轮同样不专门造 mock 触发）：确认 `VWriteProvenanceMemory`/
  `MWriteProvenanceMemory` 相互独立、`Uncertain` 分支在源码里确实立即 `return` 不下坠到下一步
  （`migrated_v2_started_publisher.hpp:313`/`:383`）。

**明确不做**（头注释显式声明）：legacy 192 字节 L 格式的 decode/校验；真实 operator 交互/CLI；
接入任何真实触发流程；`.clr`/`.abd`/`.xgc` 相关的任何东西。

**验证（2026-08-22）**：
- MSVC Release 全量构建：0 warning（`/W4`）。全量 ctest：1172/1172（含新增 15 个测试；1 个既有
  POSIX-only skip）。
- WSL2 GCC-14 Release（`none` 档，`HY_BUILD_DEMO=ON`）：1201/1201。
- WSL2 ASan+UBSan（`address` 档，`HY_BUILD_DEMO=OFF` 定向构建：
  test_migrated_v2_started_publisher + test_compaction_lease + compaction_lease_holder +
  test_compaction_intent_store，规避这台 WSL2 VM 在全量 demo ASan 构建下的既知 OOM）：48/48，
  无 ASan/UBSan 报告。
- WSL2 TSan（`thread` 档）：这一轮改了 `compaction_lease.hpp`（新 friend + 新写方法），从设计
  阶段起就规划了 address/thread 两档，不是先跳过再回头补——`tools/wsl_verify.sh` 的
  `run_thread()` 目标列表新增 test_migrated_v2_started_publisher（跟已有的
  test_compaction_lease/compaction_lease_holder/test_compaction_intent_store 同一条
  "本地覆盖比 CI 的 tsan-concurrency job 更宽"的既有先例，CI 那个 job 本身只构建纯并发原语，
  不构建这几个）。`ctest -L concurrency` 73/73（含新增 15 个测试），两个负控
  tsan_control_relaxed_ring/`tsan_control_export_worker_dual_consumer` 均正确报出
  `WARNING: ThreadSanitizer: data race`。
- `tools/spec_xref_check.py --quiet`：OK（`migrated_v2_started_publisher.hpp` 已加入该工具的
  搜索文件列表；测试名/ExitPlanMode 工具名按既有惯例去掉反引号）。
  `tools/spec_enum_diff.py`：OK（唯一警告是既有的 `SubmitOutcome::RateLimited` spec-ahead-of-code，
  跟这一轮无关）。

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

### Phase 5：ExportOutboxRing 消费者（best-effort telemetry）—— export-worker 排水循环 +
### LastRemoteAckedTip 面包屑持久化 + Store 身份

`DurableControlPlaneSink` 真实实现总路线图的第六个、也是当前排期的最后一个执行单元（Phase 0-4 均已
合并：PR #18/#22/#23/#24/#25）。用户最初要求的措辞是"`ExternalAnchorClient` 真实网络实现"，撞到
一堵硬墙：两份 spec 都从未规定这个接口该讲什么协议、连接对端是什么、用什么认证方案——`docs/BINANCE_PRIVATE_REST_L4_SPEC.md`
原文明确说这是"这份 spec 不发明的真实基础设施"，这条判断本身早就记在这份清单更早的位置（见本节
之前"比照 `SubmitPort`/`QueryPort` 先例，真实网络实现不做"的既有记录）。用户确认后的方向：
`ExternalAnchorClient` 接口继续保持可注入（同 `SubmitPort` 先例，永久 mock-only），这一轮改做
它旁边真正缺失、真正可独立验证、不需要发明协议或依赖外部基础设施的东西——`ExportOutboxRing`
（`durable_control_plane.hpp`，早已完整实现的 256 容量 SPSC 环）至今没有任何消费者，
`LastRemoteAckedTip`（导出基线面包屑，ABI 早已定义）也从未有代码真正读写过。用户随后把范围收窄到
只做"export-worker 排水循环 + `LastRemoteAckedTip` 持久化"，明确排除启动时重建程序（§10.2.1）、
`OperatorOverrideSidecar` 真实实现+准入算法、append 路径的背压式围栏——三者均需要这一轮之外的
独立基础设施（离线 operator 工具、实时 `/time` 探测、对一个已稳定四轮的核心 `append_durable()`
的真实行为改动），不是"给已经在跑的 ring 建一个消费者"这件事本身的自然延伸。

**两轮外部 GPT 审查触发的自我纠错，记在这里防止以后重复踩坑**：

第一轮审查核实为真、已采纳的点：`LastRemoteAckedTipStore` 原设计完全没有对新写入值做单调性/身份
校验，直接违反了研究阶段已经引用过的 spec 原文"under monotonic (generation, sequence) CAS"；
`read()` 原设计只返回 `bool`，把"从未导出过"和"文件损坏"混成一种失败，跟这个仓库
`RecoveryScanStatus` 的多值区分传统不一致；`LastRemoteAckedTipStore` 原设计持有活的 `KekLoader&`
并逐次调用 `is_loaded()`/`get()`，有 TOCTOU 风险，且偏离了 `KeyRing::KeyRing()` 自己"构造时拷贝
一次 KEK，不持有外部活引用"的既有先例；breadcrumb wire 格式原设计漏掉了这个仓库每一处 wire 格式
都有的 format-version 字节。同一轮还核实了 `DurableAuditSink::append_durable()` 产出的
`ExportTuple` 现在 `store_uuid_lo/hi` 永远是 0——这是一个真实存在、早于本轮就有的语义空洞（外部
锚定服务分不清两个不同的 store），用户单独确认后同意"顺便加上，小而纯增量的 store UUID 机制"
（`generation` 字段依然保持硬编码为 0——那是压缩/generation 切换子系统 round-12 的范围，是一个
不同大小的问题，这一轮不碰）。

第二轮审查抓到了第一轮实现里一个真正的自我矛盾，是本轮最严重的一处修正：store-identity"降级"路径
原设计在身份文件损坏或首次生成后落盘失败时，仍然继续用 `(0,0)` 或未持久化的内存态 UUID 往
`export_outbox_` 里塞 tuple——这恰好复现了引入这套机制本来要修的那个语义空洞，只是从"永远是 0"
变成"有条件地是 0 或不稳定"，反而更隐蔽。修正为：`store_identity_degraded_ == true` 时，
`append_durable()` 整个跳过 `export_outbox_` 的 push，效果等同于压根没接导出通道，绝不发送零身份
或跨重启会变化的身份。第二轮审查还指出两处会在 `noexcept` 路径下触发 `std::terminate()` 的真实
风险：`std::filesystem::exists()` 的裸重载和 `std::random_device` 的构造函数都可能抛异常，而
这两个类的每一个公开方法都声明为 `noexcept`。修正为：`std::filesystem::exists(path, error_code&)`
的不抛异常重载，以及用 Windows `BCryptGenRandom`/Linux `getrandom()` 的薄封装取代
`std::random_device`——这直接照抄 `kek_loader.hpp` 自 Phase 0 起就确立的"裸平台 API、不借助任何
会抛异常的 C++ 标准库设施"纪律，`std::random_device` 是这一版计划里唯一偏离这条纪律的地方。

两轮审查里被判定为"已经是既有范围决定，重复提出但没有新论据"而拒绝的点：完整启动重建程序、
`OperatorOverrideSidecar`、append 路径背压围栏、真正的生产线程调度器——均为用户在两轮
AskUserQuestion 里已经明确排除的范围。被判定为"改动面明显超出'小而纯增量'"而拒绝、改为纯披露
的点：`set_export_outbox(ExportOutboxRing*)` 改成构造期非空引用注入（会改变一个已稳定三轮、被
大量既有测试依赖的公开 API 默认行为，只做既有注释加固，不改签名）；store_uuid 与主日志创世帧内容
做密码学绑定（能防"整个目录树被复制导致身份碰撞"，但是比"给 store 一个持久身份"更大的一块独立
设计，这一轮只做披露不实现）。

**关键设计事实**：
- `ExportOutboxRing`/`ExportTuple` 早已完整实现（256 容量、two-phase `peek_oldest()`/
  `pop_after_remote_ack()`、SPSC、零堆分配），只有 `DurableAuditSink` 接了 producer 端
  （`set_export_outbox()`）；`ControlPlaneLogSink` 这一轮维持"完全没接"的现状不变。
- `LastRemoteAckedTip` 的 MAC 原本设计在裸 KEK 下计算，不走 `KeyRing` 按 key_id 派生——这是这个
  仓库里唯一一个信任边界不经过 `KeyRing` 的 durable 结构，是刻意的、独立的设计点。但
  `DurableAuditSink` 的构造函数签名（`(path, KeyRing&, active_key_id)`）根本没有 `KekLoader&`——
  给新的 store-identity 机制引入裸 KEK 签名会强迫改这个已稳定四轮的构造函数签名，牵连所有既有
  调用点，不是"纯增量"。**store-identity 因此改用已经可用的 `key_ring_`/`active_key_id_` 签名，
  跟 tip-anchor 自己的签名方式完全一致**——`LastRemoteAckedTip` 自己的裸 KEK 签名维持不变（它是
  `export_worker.hpp` 里独立的 `LastRemoteAckedTipStore` 概念，不依赖 `DurableAuditSink` 的
  `KeyRing`）。
- `DurableLogStore::write_tip_anchor()`/`read_tip_anchor()`（Phase 1 共享基础设施）完全通用，
  这一轮第三次复用它（`LastRemoteAckedTipStore` 一次、`DurableAuditSink` 新增的 store-identity
  sidecar 一次）——`read_tip_anchor()` 把"文件不存在"和"I/O 失败"合并成同一个 `false`，这个仓库
  不改这个共享原语（已被两个生产 sink 共用四轮，改动面不成比例），Absent vs IoError 的区分改在
  调用方这一层用 `std::filesystem::exists(path, error_code&)` 做。
- `order_tracker.hpp` 的 `poll_once()`/`ReconcilePollPolicy`/`reconcile_backoff_delay_ms()` 是
  直接仿照的既有先例，包括"至今没有真正的生产线程驱动"这一点——`run_export_worker_once()` 同样是
  纯 `noexcept` 自由函数，不内建线程/sleep，调用方决定驱动节奏。
- `export_tip_and_wait_bounded()` 返回 `AuditAppendResult`（不是 `bool`）——真正的"远端拒绝"和
  "传输层失败"在这一层共用同一个 `Failed`/未 `acked()` 语义，spec 原文没有在这一层进一步区分，
  这一轮不发明区分。
- **最小状态链**（本轮实际交付的因果顺序，任一环节失败就停在那一步，不静默推进）：
  `main-log Ack` → 已持久化且非降级的 store identity → `try_push()`（满则丢弃，
  既有先例不改）→ `ExternalAnchorClient::export_tip_and_wait_bounded` 远端 Ack → `LastRemoteAckedTip`
  原子落盘（内建单调性/身份校验）→ `pop_after_remote_ack`（落盘成功后才做，pop 前防御性
  re-peek 核对 head 未变）。这条链本身就是"best-effort telemetry"的定义——不保证每一条 tuple
  最终都被导出，只保证凡是被导出/落盘的都真实、完整、单调。

**新增符号**：
- `export_worker.hpp`（新文件）：`kLastRemoteAckedTipFormatVersion`(1)、
  `kLastRemoteAckedTipContentSize`(65)、`kLastRemoteAckedTipWireSize`(97)、
  `encode_last_remote_acked_tip`/`decode_last_remote_acked_tip`（MAC 覆盖全部字段，含
  format-version 字节，签名 key 是调用方传入的裸 KEK span，不是 `KeyRing` 查找）。
- `LastRemoteAckedTipReadStatus{Absent, Valid, Corrupt, IoError}`/
  `LastRemoteAckedTipWriteStatus{Ok, Regressed, Conflicting, IoError}`——全新的、非 spec 治理的
  本地小枚举，不涉及任何已转写的 spec 枚举，不会重演 Phase 4 那次 `DurableRecordType` 的教训。
  `Regressed`：新 `(generation, sequence)` 落后于磁盘已有值。`Conflicting`：同一位置（或同一
  `store_uuid`）但 `tip_mac`/`key_id` 不同——跟 `Corrupt`（读时 MAC/尺寸校验失败）是两个独立的
  判定层，`Corrupt` 是"文件本身读不出一个自洽的值"，`Conflicting` 是"文件给出的自洽值跟即将写入
  的新值语义冲突"，概念上不合并。
- `LastRemoteAckedTipStore`：构造时拷贝一次 KEK（`kek_copy_`，析构 `secure_wipe()`，同
  `KeyRing::KeyRing()` 先例，不持有 `KekLoader&`）；`open()` 把 `acquire_lock()` 的结果记进
  `lock_held_`，`write()`/`read()` 内部强制检查 `lock_held_`，不再只信任调用方自己检查过
  `open()` 的返回值——两个独立实例意外指向同一路径时，没拿到锁的那个会被内部拒绝，不会绕过
  "单调 CAS" 直接写穿。
- `ExportWorkerPolicy{base_retry_interval_ms, backoff_multiplier, max_retry_interval_ms}` +
  `export_worker_backoff_delay_ms()`——无状态公式，`consecutive_failures` 由调用方传入/维护，
  同 `ReconcilePollPolicy` 先例；真正的生产调度器（GPT 第二轮审查建议的"入队时一次唤醒 + 有界
  定时器退避"混合等待，避免每笔 append 都做内核唤醒）不在本轮范围内，这个具体形状记在这里作为
  未来独立回合的参考起点。
- `ExportRunStatus{Empty, Exported, RemoteRejected, BaselineWriteFailed, BaselineConflict,
  InternalInconsistency}` + `run_export_worker_once(ExportOutboxRing&, ExternalAnchorClient&,
  LastRemoteAckedTipStore&) noexcept`——必须只从唯一一个专属 export-worker 线程调用（单消费者
  契约，tsan_control_export_worker_dual_consumer 提供真实的 TSan 负控制）；pop 前重新
  `peek_oldest()` 核对 head 未变（`InternalInconsistency` 覆盖这个正常场景下不该发生、出现即
  说明契约被违反的情形）；`ExternalAnchorClient` 的纯虚方法在既有 ABI 里已经声明 `noexcept`
  （Round A/B，非本轮新增）——实现若抛出会在自身栈展开时直接 `std::terminate()`，发生在控制权
  返回本函数之前，worker 侧代码拦截不到也不需要尝试拦截，这跟这仓库其余每一个 `noexcept` 虚接口
  （例如 `SubmitPort`）的既有契约一致，不是本轮引入的新风险。
- `durable_audit_sink.hpp` 新增 store-identity 小节（仿照文件里已有的 tip-anchor 本地 wire-format
  小节写法）：`kStoreIdentityFormatVersion`(1)、`kStoreIdentitySize`(53)。新增私有成员：
  第三个 `DurableLogStore` sidecar `store_identity_store_`（`<path>.storeid`/`.lock`/`.tip`，
  同 `rotation_log_store_` 的既有接线模式）、`store_uuid_lo_`/`store_uuid_hi_`（默认 0）、
  `store_identity_degraded_`（默认 false）。新增只读访问器：`store_uuid_lo()`/`store_uuid_hi()`/
  `store_identity_degraded()`。**构造函数签名不变**，只在函数体里追加：首次运行用平台原生 CSPRNG
  （`BCryptGenRandom`/`getrandom()`，不用会抛异常的 `std::random_device`）生成新身份，用当前
  `active_key_id_` 对应的 `key_ring_` 密钥签名并落盘；已有身份文件则用文件自带的 `key_id`（不是
  当前 `active_key_id_`）做一次 `key_ring_` 查找校验 MAC，同 tip-anchor 自己"用帧自带 key_id
  校验"的既有模式一致，允许身份文件是在更早的 key 下签的。**身份 sidecar 出问题（打不开/损坏/
  首次落盘失败）不 fence 主日志、不影响 `append_durable()` 正常工作**，只设置
  `store_identity_degraded_ = true`——同 rotation sidecar 自己"绝不能 fence 主 sink"的既有原则
  一致。**关键修正**：`append_durable()` 里 push 进 `export_outbox_` 的条件从
  `if (export_outbox_)` 改成 `if (export_outbox_ && !store_identity_degraded_)`——身份不可信
  （无论是"文件损坏"还是"首次生成后落盘失败，内存里有值但不稳定"）时，整个导出通道跟"没接
  `export_outbox_`"完全一样地静默跳过，绝不发送零身份或跨重启会变化的身份。
  `ExportTuple.store_uuid_lo/hi` 从永远是 0 改成填 `store_uuid_lo_/hi_`；`generation` 维持
  硬编码为 0 不变。
- `set_export_outbox(ExportOutboxRing*)`（Phase 2 既有 API，可空、默认不设置、不拥有所有权）——
  这一轮**签名和行为均不改**，只加固既有注释，明确此前隐含但没写清楚的生命周期契约：只应在
  worker/提交线程开始排水之前完成一次性配置，运行期间不得 `reset` 或让所指向的 `ExportOutboxRing`
  提前析构。

**明确排除/降级为"记录但不实现"的范围（附理由）**：
- 不做启动时重建程序（§10.2.1 正常路径+降级路径）+ 不做 append 路径背压式围栏——用户两轮
  AskUserQuestion 已明确排除。已知后果，本轮如实披露而非新发现：`ExportOutboxRing` 纯内存，
  进程重启会丢失尚未导出的 tuple，两个具体崩溃窗口——(a) 本地日志已落盘、tuple 还没 push 进 ring
  之前崩溃；(b) baseline 已落盘、ring 还没真正 pop 之前崩溃。两者都不会腐化本地审计日志本身的
  正确性，只是那条 tuple 的导出/baseline 记录可能重复或延后。**这个通道的输出不得被未来任何代码
  当作 rollback 检测或 hard-lag 判定的输入，除非启动重建和背压围栏先被建出来**——这是本轮对
  "best-effort telemetry, not a safety mechanism"这个定性的直接可执行约束，不是免责声明。
- 不做 `OperatorOverrideSidecar` 真实实现 + 准入算法——需要离线 operator 工具 + 实时 `/time`
  探测，这两样都不存在，属于独立基础设施工作。
- `ExternalAnchorUnavailable` 依然没有任何代码设置/检查它——设置它需要上面
  第一条排除的启动重建路径存在。
- `ExternalAnchorClient` 接口零改动，不新增真实网络实现子类，继续可注入（同 `SubmitPort` 先例）；
  `ControlPlaneLogSink` 零改动。
- store_uuid 不与主日志内容做密码学绑定——当前设计只保证"同一个 store 目录跨重启身份稳定"，不
  保证"这个目录树没有被整个复制/克隆到别处"；真正堵上这个洞需要把身份 MAC 纳入主日志创世帧的
  哈希，是比"给 store 一个持久身份"更大的一块独立设计，这里只做披露。

**测试范围**：`test_export_worker.cpp`（新文件）覆盖 `ExportRunStatus` 每个分支、
`LastRemoteAckedTipReadStatus`/`LastRemoteAckedTipWriteStatus` 每个分支（含
WriteWithoutOpenFailsClosed/SecondStoreOnSameLockedPathFailsClosed 两个专门验证锁生命周期强制
校验真的生效的用例，测试方法名同上方"测试范围"惯例不加反引号）、`export_worker_backoff_delay_ms`
独立单元测试。tsan_control_export_worker_dual_consumer.cpp（新文件，不进常规 ctest，同
tsan_control_relaxed_ring 角色，手工 TSan 验证）证明违反单消费者契约会被真实抓到。
`test_durable_audit_sink.cpp` 新增 store-identity 用例，含
TamperedStoreIdentityFileDisablesExportWithoutFencingMainLog（核心断言：篡改后新追加的 tuple
完全不出现在 `export_outbox_` 里，不是出现但带零身份）和
StoreIdentityWriteFailureDisablesExportEvenWithInMemoryUuid（验证首次落盘失败时即使内存里已经
生成了随机 UUID，导出仍然被禁用）。

`spec_xref_check.py --quiet`/`tools/spec_enum_diff.py` 均已重跑确认：前者 exit 0，后者
`0 value conflict(s), 0 name conflict(s)`（本轮三个新枚举都是全新本地小枚举，不涉及任何已转写的
spec 枚举）。`control_plane_log_sink.hpp`/`durable_control_plane.hpp`/`durable_log_store.hpp`
三者均零改动。

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
