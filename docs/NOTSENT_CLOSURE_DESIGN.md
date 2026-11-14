# NotSent 闭合与发送前最终授权 —— 语义冻结与协议模型（Q23 切片 1）

状态：S2/S3 的 v6 读取、状态机与恢复，以及 S4 的默认关闭写入接口已实现。S6 已接入类 A 拒绝流程及对应恢复、容量与成本测试。S5 已实现发送前最终授权与类 B 拒绝。实现命名为 `OrderNeverSent` 与 `OrderState::NeverSent`。各项验证结果见 §8 与 `docs/SPEC_INVARIANTS.md`。
依据：蓝图 v2.9.0 的 `5.9.2`、F.5、F.9、R-18 / R-22（Owner 已选统一 NotSent）、Q23、FI-055、FI-056、任务 SAFE-07 与 1A.6。
模型：`formal/notsent_closure.tla`（结果见 §7）。本文的“必须 / 不得”是对后续实现 PR 的要求；**数值取值不在本文分配**，随实现 PR 同步分配，并过 `tools/spec_enum_diff.py`、codec 与恢复扫描。

## 1. 目标

1. 在 `Prepared` 持久确认之后、`submit_port.call()` 之前重新检查发送条件；私有数据流断开、收到 `serverShutdown`、熔断或授权过期都能使原授权失效。
2. 为确定没有发送的订单写入 `OrderNeverSent`，使恢复能够关闭订单并释放资源。仅有 `Prepared` 的订单仍恢复为 `Ambiguous`；对账继续要求交易所响应与原订单正向匹配。
3. 对 `Intent` 之后、`Prepared` 之前的本地拒绝采用相同的关闭记录，并限制拒绝循环产生的日志规模。

蓝图的选择（R-18 / R-22）：统一的 NotSent 持久关闭同时服务以上两类拒绝。本文描述授权、写入、恢复和版本规则，并给出 TLA+ 模型与实现对应关系。

**不能退回的教训**：规格 round 13 的 `AbortedPreSend` 把“追加失败”读成“没有发生”，在 round 17 被整体删除（`docs/SPEC_INVARIANTS.md` 的 `OrderState::AbortedPreSend` 条目；`formal/durable_log_recovery_abortedpresend.cfg` 是它的必败反例）：一次 `Failed` 的追加可能已经落盘（迟到的 fsync）。NotSent 因此**只能是一个正向事实**：由一个**事先决定了不发送**的所有者写下，绝不能由失败、超时或“恢复后看起来没发”推出来。

## 2. 当前实现（2026-11-14）

| 事实 | 位置 |
| :-- | :-- |
| 执行顺序：`Intent` 持久确认、操作员确认、端口检查、限流预留、在途登记、`Prepared` 持久确认、`Submitting`、最终检查和一次性 claim、`submit_port.call()`、结果持久确认与状态更新 | `live_submit_orchestrator.hpp:orchestrate_submit` |
| `Prepared` 追加失败：状态留在 `Intent`，在途槽位**不**释放，返回 `AuditWriteNotAcked`（写入可能已落盘） | 同上，Gate 12d |
| 恢复扫描：每个 COID 的首帧 `resulting_state` 必须是 `Intent`；之后同状态重复或经 `validate_transition`；扫描结束时 `Submitting` 重映射为 `Ambiguous` 检查点，`Intent` 只计孤儿，`is_exchange_final` 的跳过 | `durable_audit_sink.hpp:run_recovery_scan_impl` |
| `Intent` 和 `Submitting` 均可转为 `NeverSent`；该状态没有后续出边。`Submitting` 的交易所响应与不确定结果仍按原状态机处理 | `order_lifecycle.hpp:validate_transition` |
| 恢复通过 `is_resolved()` 跳过 `NeverSent`；编排器在关闭写入确认后释放对应代次的在途槽位与成交上下文 | `durable_audit_sink.hpp`、`live_submit_orchestrator.hpp` |
| v6 增加 `AuditEventType::OrderNeverSent = 23`、`OrderState::NeverSent = 12`；v5 的合法范围保持原值 | `audit_trail.hpp`、`order_lifecycle.hpp`、`durable_frame_codec.hpp` |
| 机器字段 `detail_code`（int64，含义随事件类型：交易所错误码、事前检查码）；`detail_msg` 是人读短文 | `audit_trail.hpp:AuditRecord` |
| v5/v6 的订单载荷均为 165 字节，完整帧为 256 字节；未知版本返回 `UnknownVersion`，版本回退拒绝读取 | `durable_frame_codec.hpp` |
| 有日志而缺首个 anchor：`IoError`（失败关闭，不自动放行） | `durable_audit_sink.hpp:finalize_scan_with_anchor_check` |

## 3. 定义

- **跨越（crossing）**：订单请求的第一个字节可能离开进程的那一刻。**只有一个方向的义务**：跨越 ⇒ 之前已有持久 `Prepared` 且决定字为 `SendClaimed`；反过来不成立（claim 之后、真正写出之前崩溃，订单也读作 `Ambiguous`，这是保守方向）。
- **决定字（decision word）**：模型中每个在途订单的抽象状态，值 `None → Undecided → {SendClaimed → Sent | Rejecting}`。`Undecided` 在 `Prepared` ACK 时建立。S5 的实现对应见 §4.2。
- **NotSent**：一个持久帧（新事件 + 新终态），语义是“这个订单**从未**跨越”。终态；不是交易所的 `Rejected`。
- **类 A（发送前拒绝）**：`Intent` 之后、`Prepared` 之前被本地拒绝（Gate 11 / 12 / 12b / 12c）。前驱帧是 `Intent`。
- **类 B（最终门拒绝）**：`Prepared` 之后、决定字取到 `Rejecting`。前驱帧是 `Prepared`。
- **单次尝试 COID**：一个 client-order-id 只对应一次尝试（Gate 9 每次生成新的）。被 NotSent 关闭的 COID 永不再启动。

## 4. 协议

### 4.1 最终门的位置

`Prepared` 持久确认与 `transition_to(Submitting)` 之后执行最终检查，并在 `submit_port.call()` 之前取得 claim。claim 成功后，端口构造、签名与写出请求的时间均按可能已经发送处理。异步传输或写出回调采用同样的授权规则，并在实际发送者中消费授权。

### 4.2 决定字与线性化

1. 模型中，每订单决定字通过原子动作从 `Undecided` 进入 `SendClaimed` 或 `Rejecting`。只有全部检查通过且没有生效的撤销时才允许 `SendClaimed`。两种结果互斥，决定后不能重新开放同一次尝试。
2. **对每一次撤销 R 与每个订单 o：要么 o 的 claim 线性化在 R 之前（o 可能已在途，撤销者必须按“可能已发出”处理，不得承诺撤回字节），要么 o 被拒绝。**“检查撤销”与“claim”必须是同一个线性化点；先查一遍再 claim 不构成证明（`5.9.2` 的“七次独立 load 后普通 call”）。
3. S5 的同步实现由同一热线程持有订单、槽位与限流器。跨线程撤销与发送 claim 共同修改 `SubmitAuthority` 的一个原子 epoch：`claim(expected)` 通过 CAS 消费签发时的 epoch，`revoke()` 递增同一个值。撤销先发生时，原 claim 失败；claim 先发生时，订单进入可能已经发送的处理范围。计数达到上限后拒绝新授权。模型中的每订单决定状态由该 CAS 结果和编排器的单一分支表示。
4. 授权同时绑定签发者、订阅代次、屏障 epoch 和熔断 revision；最终检查绑定原在途句柄的槽位代次与订单号。释放操作验证原句柄，迟到的结果不能释放复用后的槽位。当前授权由热线程直接消费；异步发送者须在实际消费线程重新实现相同的一次性判定与句柄校验。
5. 取到 `Rejecting` 之后，同一个 COID **不得**再发送；取到 `SendClaimed` 之后，**不得**写 NotSent。

### 4.2.1 S5 的最终检查

`SubmitReadiness::claim()` 先读取当前订阅、恢复、屏障、熔断和签名时钟状态，再检查令牌绑定，随后执行编排器的局部检查，最后执行原子 CAS。局部检查包括：默认 5000 ms 的单调时钟授权期限、原操作员确认的有效期、原槽位所有权、限流预留、账户新鲜度与事前检查、审计可写状态、签名与深度状态，以及规则版本。时间检查发生在读取就绪状态之后，覆盖读取期间消耗的时间。成功 CAS 后直接进入内存审计与提交端口，期间没有持久写入。

`SpotRateLimitTracker::OrderReservation` 是不可复制的票据，记录所属限流器、配置代次及三个维度中最早的窗口截止。过期或重新配置后票据失效。确认关闭后仅退回仍属于原窗口的额度；旧窗口票据不会减少新窗口的使用量。

WebSocket 会话在停止请求、连接错误、`serverShutdown` 或 `eventStreamTerminated` 时先公布停止状态，再撤销共享 epoch。会话和 `PrivateStreamPipeline` 由调用者传入同一个 `SubmitAuthority`；预检程序已连接该对象。`KillSwitch` 由热线程管理，其 revision 使触发后立即重置的操作也会使旧令牌失效。

### 4.3 NotSent 何时写、写什么

- 写者：只有**取得 `Rejecting` 的那个进程实例**（类 B），或经过类 A 拒绝路径的实例。**恢复不得为 `Prepared` 之后没有终局帧的订单写 NotSent**——那是 `Ambiguous`，交给对账（模型的 `RecoveryNeverClosesPrepared` 开关）。
- 内容：`AuditRecord`，新事件类型、`resulting_state` = 新终态、COID、价格数量原样、成交为 0、`side` 保持；**拒绝原因放 `detail_code`**（不放 `detail_msg`，不占用其它字段）。原因集合（至少）：类 A —— 确认缺失、确认不符、端口无效、在途表不可用、限流耗尽、重复在途；类 B —— 就绪纪元变化、屏障未处于签发代次的 `Reconciled`、订阅失效或收到 `serverShutdown`、熔断锁存、时钟令牌过期、授权截止、槽位或代次不符、预留丢失（R-25 的范围批准另增一个：范围不再存活——撤销、被替换、过期、熔断终结，见 `docs/SCOPE_APPROVAL_DESIGN.md` §6）。类 A / 类 B 由前驱帧区分，不另设字段。
- 一个 COID 至多一个 NotSent 帧，且是它的最后一帧。

### 4.4 释放

`Released(o) ⇒ 持久 NotSent(o) ∨ (持久交易所终局(o) ∧ 已投影(o))`。在途槽位、`OrderFillContext` 登记、限流预留和任何仓位或名义预留，**只在**闭合帧的持久 ACK 之后（或恢复读到该帧时）释放，顺序与现有 `Rejected` 分支一致：持久 ACK → `transition_to` → `mark_resolved` → `fill_context->remove`。NotSent 追加 `Failed` 或超时：**写者围栏，订单与全部资源原地不动**，由下一次启动按持久帧决定。限流预留在 NotSent 之后可以归还给本地限流器（请求没有离开；见 §9 D3）。

### 4.5 失败与迟到

- `Failed` 不证明没写入。NotSent 追加失败后：帧若迟到落盘 → 下次启动读到 NotSent，闭合（仍然正确，因为它从未跨越）；没落盘 → 读到 `Prepared` → `Ambiguous`。**这不是“多一段 UNKNOWN”**：今天的对账不能把“交易所没有这个订单”当结论（§1 第 2 点），这样的订单只会走 R-10 隔离升级给操作员。所以 NotSent 追加失败不会不安全（安全性质不依赖对账能否下结论），但也不便宜：一次丢失的 NotSent 帧等于一次操作员升级。模型用 `ReconcileConcludesAbsent` 精确显示了这一点（§6 A6）。
- **迟到的完成（ACK）只是审计证据**：调用方已经放弃（超过截止、已围栏）之后到达的完成，不得改变该进程的任何状态，不得释放槽位，不得让订单变成可发送。同步写入器下这条恒真；异步或组提交写入器（PERF-01）必须让每个请求携带 `(boot_uuid, request_id, slot_index, slot_generation)`，完成按它匹配，不匹配就丢弃并计数；请求未结清之前槽位不得复用。
- 前任进程的迟到落盘**不得**与继任者的恢复扫描交错：见 §6 假设 A1。

### 4.6 恢复与重放

| 某 COID 的帧序列 | 恢复结果 |
| :-- | :-- |
| `Intent` | 孤儿（计数，行为不变） |
| `Intent, NotSent` | 已闭合：既不是检查点也不是孤儿 |
| `Intent, Prepared`（其后没有终局帧） | `Ambiguous` 检查点（行为不变） |
| `Intent, Prepared, NotSent` | 已闭合，无检查点 |
| `Intent, Prepared, Outcome…` | 按 Outcome（行为不变） |
| NotSent 之后的**任何**帧（含再来一个 `Prepared`、一个 `Outcome`） | `Corrupt`，失败关闭 |
| NotSent 作为某 COID 的首帧 | `Corrupt`（首帧必须是 `Intent`，规则不变） |

对状态机的要求：新终态 `NotSent`；边只有 `Intent → NotSent` 与 `Submitting → NotSent`，**不得**有 `Ambiguous → NotSent`（恢复不能闭合 Ambiguous）；`is_terminal(NotSent)` 为真。恢复扫描、压实保留和槽位释放用一个**新的谓词** `is_resolved = is_exchange_final ∨ NotSent`，不改 `is_exchange_final` 的含义（它的注释明确“只有交易所终局”）。对账目标集合（`order_tracker.hpp` 的 `is_legal_ambiguous_target`、`is_legal_query_target`）**不得**包含 NotSent。

**读者规则（已实现，S2/S3）**：除上表之外，恢复扫描对一个闭合帧还要求（任何一条不满足 = 整本日志 `Corrupt`；规则在 `never_sent_rules.hpp`，写者以后调用同一个函数）：① 订单此前没有出现过交易所订单号或成交；② 事件类型与结果状态是同一个事实说两遍，只说一半的帧在任何位置都是 `Corrupt`（作为某订单号的首帧也是）；③ 帧不带交易所订单号、成交与均价，品种、方向、价格、数量、模式与 Intent 帧一致；④ 闭合帧是该订单号的最后一帧，其后的任何帧（含操作员的闭合）都是 `Corrupt`；⑤ 只在 v6 帧里合法。`detail_code`（拒绝原因）**不检查**：未知的值不是损坏。读者无法核实“它真的没有越过边界”，这条保证在协议（模型 `NotSentImpliesNeverCrossed`）与写者里。命名：代码里的事件与状态叫 `OrderNeverSent` / `NeverSent`，以区别 `QueryOutcome::NotSent`。

### 4.7 不在本设计内

- 跨越之后的 NotSent（“端口证明在第一个字节之前失败”）。claim 之后的失败一律沿用现有的 `Ambiguous` 路径；以后若要，端口必须返回一个定义极窄的 `NeverWritten` 结论，另开设计。
- 撤单、看门狗（1A.7 / 1A.8）、主机围栏（6.7）、日志层 / anchor / 目录同步的分层 ACK（F.6，PERF-01）。

## 5. 格式与版本

1. v5 历史段**不改写**。
2. 新语义用 **v6**：布局与 v5 完全相同（256 字节，`detail_code` 承载原因，**不**压缩或偷占 `detail_msg`）；v5 帧的合法集合保持“事件 ≤ 21、状态 ≤ 11”，v6 帧再加上新事件与新终态。若以后需要更多恢复字段，是新的**载荷版本**，不是在 256 字节里挤。
3. 单个存储内 `format_version` **单调不减**：第一个 v6 帧必须接在 v5 的尖端之后，此后出现 v5 帧 = `Corrupt`。
4. **读者先行**：能严格读 v5 与 v6 的构建先于任何会写 v6 的构建部署；写 v6 由一个默认关闭的开关控制，没有读者的构建不能打开它。
5. 回退：一旦存在 v6 帧，只认 v5 的旧构建在 `UnknownVersion` 上失败关闭。“回退前先停机”是运维流程，不是代码。
6. 迁移清单（旁文件）绑定存储 UUID、旧尖端（序号与 MAC）、工具版本与日期，在第一个 v6 追加**之前**原子写入；写者在清单与当前尖端不符时拒绝启用 v6。
7. **直到出现第一份我们要保留的真实日志之前，4 到 6 不需要**：仓库里没有任何已部署数据（`CLAUDE.md`：没有真实或 Testnet 凭据用过这些代码），v4 与 v5 就是直接升版、更新夹具、零迁移成本。规则 1 到 6 从**第一次会写下我们要保留的日志的运行**（M5 的 Testnet 联调）起，对每一次格式变化生效（§9 D2）。
8. 缺 anchor 仍是 `IoError`，本设计不改；“认证的创世”是另一个设计。

## 6. 协议依赖的假设

- **A1 独占日志锁。**继任者的恢复扫描在前任的最后一次写入落盘或丢失**之后**才开始（独占打开保证：Windows 是无共享的 `CreateFileA`，POSIX 是 `flock(LOCK_EX)`，均见 `durable_log_store.hpp`——前任的句柄关闭之后继任者才能打开）。去掉它，安全性仍然成立，但 NotSent 与对账的 Outcome 可能在日志里交错成重放不接受的序列——模型用 `RecoveryWaitsForWriter` 开关精确显示了这一点。
- **A2 COID 单次尝试。**见 §3；没有它，NotSent 之后的再次发送会得到“已闭合又已发出”（开关 `SingleAttemptCoid`）。
- **A3 claim 是传输边界之前的最后一步。**claim 与第一个可能写出的字节之间，只允许不能阻塞的工作。
- **A4 “可以发送”只有一个来源**：决定字。没有第二个局部标志。
- **A5 时间域。**截止、退避用单调时钟；这些是门的输入，不在模型里。
- **A6 对账能否结论“交易所没有它”。**设计**不**依赖这一条为真：模型的安全性质在 `ReconcileConcludesAbsent = FALSE`（今天的代码）下全部成立（`notsent_closure_no_absent_safety.cfg`）。依赖它的只有进展：为真时，`Prepared` 之后没有闭合帧的订单最终能由对账收敛；为假时，它们永远停在 `Ambiguous`（`notsent_closure_no_absent_liveness.cfg` 必须失败，反例是一个 `Prepared` 迟到落盘、从未发出、被恢复成 `Ambiguous` 的订单）。让它为真需要另一个设计（例如在 `recvWindow` 过期后、带可审计证据地把“查无此单”认作终局，或操作员的证明路径），不在本文。**在那之前，缩小 `Ambiguous` 窗口（claim 越晚越好）和让 NotSent 可靠落盘，是仅有的两把手段。**

## 7. 形式化模型 `formal/notsent_closure.tla`

对 `durable_log_recovery.tla` 的惯例（非原子追加、迟到 fsync、写者围栏、WF 的写法）原样沿用，另起一个模块——旧模型与它的两个必败反例**一字未动**，仍是 round 13 / round 14 的回归保护。

**检查的性质**（设计配置必须全部成立）：`NoDoubleSend`、`SendImpliesDurablePrepared`、`NotSentImpliesNeverCrossed`（持久 NotSent ⇒ 从未发送）、`ClosedNotSentIsTrue` 与 `ClosedNotSentIsDurable`（内存里的 NotSent 为真且有持久帧，含恢复产生的）、`ReleaseNeedsProof`、`NoClaimUnderRevoke`、`SendRequiresClaim`（只有 claim 才能跨越）、`ClaimRejectExclusive`（决定互斥）、`ReplayLegal`、`TerminalIsStable`、`FencedWriterMakesNoNewWrites`，外加三个模型自检不变量与活性 `EventualSettlement`。

**八个开关**（`TRUE` = 设计；每个关掉都必须失败，这是模型不至于空转的保证）：

| 开关 | 关掉意味着 | 必败配置报告 |
| :-- | :-- | :-- |
| `AtomicClaim` | “有无撤销”与 claim 是两步，claim 信任旧答案 | `NoClaimUnderRevoke` |
| `ExclusiveDecision` | 撤销者可以用普通写覆盖已取得的决定 | `NotSentImpliesNeverCrossed` |
| `FinalGateAfterPrepared` | claim 可以在 `Prepared` ACK 之前取 | `SendImpliesDurablePrepared` |
| `NoCompensationOnFailure` | `Failed` 读作“没发生”：释放并回卷（round 13 的 `AbortedPreSend`） | `NoDoubleSend` |
| `ReleaseAfterAck` | 在 NotSent 追加发出时就释放 | `ReleaseNeedsProof` |
| `SingleAttemptCoid` | 被 NotSent 关闭的 COID 回到 `Intent` 并再次发送 | `NotSentImpliesNeverCrossed` |
| `RecoveryNeverClosesPrepared` | 恢复把“`Prepared` 之后没有任何帧”读成 NotSent | `ClosedNotSentIsTrue` |
| `RecoveryWaitsForWriter` | 继任者不等前任的最后一次写入就扫描（假设 A1） | `ReplayLegal`（`notsent_closure_no_lock_safety.cfg` 证明此时其余性质仍成立） |

**两个环境假设各有一对配置**：`RecoveryWaitsForWriter`（A1）——`notsent_closure_no_lock_bug.cfg` 必败于 `ReplayLegal`，`notsent_closure_no_lock_safety.cfg` 证明其余性质仍成立；`ReconcileConcludesAbsent`（A6，今天为假）——`notsent_closure_no_absent_safety.cfg` 证明安全性质仍成立，`notsent_closure_no_absent_liveness.cfg` 必败于活性 `EventualSettlement`。

另有三个可达性探针（必败）：`NotSentNeverDurable`、`LateNotSentNeverLands`（NotSent 在调用方被告知失败或崩溃**之后**落盘）、`SentWhileRevokedNeverHappens`（先 claim 后撤销：订单在途，设计只承诺“先到的撤销会赢”，不承诺撤回）。结果表与运行方法见 `formal/README.md`。

**不在模型里的东西**（不是遗漏，各有归属）：帧布局与版本规则（§5，实现时用测试验证）；日志 / anchor / 目录分层（一个 `landed` 布尔，与旧模型一致；F.5 的 `Acked ⇒ LogCovers ∧ AnchorCovers` 与 F.6 属 PERF-01）；多节点与主机围栏（F.5 的 `count(CanSubmit) ≤ 1`，任务 6.7）；异步写入器的请求号与槽位代次（§4.5，PERF-01）；`Intent` 帧与孤儿计数（SAFE-07）；时间与截止；一个撤销闩锁代表所有拒绝原因。

**模型不证明实现。**它证明的是：这套协议的**每个设计决定**都是承重的（关掉就出反例），而设计本身在它的状态空间里成立。代码是否照此实现，由后续切片的测试、TSan 与变异检查证明。

## 8. 后续切片

每一片都是 Tier B，需要 Owner 选定、计划与“开始”；这里只定内容与要钉的负控。

| 片 | 内容 | 验证与负控 |
| :-- | :-- | :-- |
| S2 codec 读者 | **已实现（与 S3 合为一片，2026-11-12）**：`format_version` 6 与两套合法集合（事件 ≤ 23、状态 ≤ 12），只读不写；未知版本、v5 帧里的 v6 值、版本回退各有明确状态（`format_version` 6 本身与事件 22 已由 D5 R1 落地） | MSVC + GCC `none`；codec 表驱动用例 |
| S3 状态机与恢复 | **已实现（与 S2 合为一片，2026-11-12；命名已定：事件 `OrderNeverSent` = 23、状态 `NeverSent` = 12，规则在 `never_sent_rules.hpp`，读者与写者共用）**：`NotSent` 状态与边、`is_resolved`、恢复表（§4.6）、重放语料；**命名**：`order_tracker.hpp` 已有 `QueryOutcome::NotSent`（R-10，意思是对账**查询**没有离开进程），订单状态与事件的新名字要与它区分（实现 PR 里定），否则 grep 和读者都会混 | `none` + `address`；表中每一行与每个 `Corrupt` 一个用例；枚举 / 规格对照 |
| S4 写入 | **已实现（2026-11-13）**：`append_never_sent()` 沿用 `append_impl`，与读者共用 `check_closure()`，构造开关 `NeverSentWrites` 默认关闭；S6 和 S5 通过 `DurableOrderAuditPort` 写入类 A 和类 B 关闭记录 | `address`；追加失败、迟到完成、尾部撕裂及非法重复发送 |
| S5 最终授权（1A.6、FI-055） | **已实现（2026-11-14）**：`SubmitAuthority` 原子 epoch 与 `SubmitReadiness::claim()`；令牌绑定签发者、订阅、屏障和熔断代次；最终检查后一次性消费；类 B 拒绝确认写入后释放对应资源 | MSVC、GCC 普通构建、ASan/UBSan、TSan 并发测试和 relaxed 负向控制通过；覆盖实际文件恢复与时限边界；ARM64 重复测试已接入 CI，本次未运行 |
| S6 类 A 闭合（SAFE-07、FI-056） | **已实现（2026-11-14）**：六种本地拒绝写入 NotSent，确认成功后返回 NeverSent；存储维护已尝试关闭的生成订单号的时间与序号最高值，重启恢复该值，拒绝重复或倒退的尝试；两个测试程序显式启用写入 | MSVC、GCC 普通构建与 ASan/UBSan 回归通过；覆盖孤儿预算、日志容量与恢复；Q19 持久化成本测量已完成 |
| S7 迁移 | 清单、读者先行开关、回退前停机；仅当存在真实日志 | 迁移中崩溃、清单撕裂、旧构建遇 v6 |
| S8 操作员闭合（D5，L-46） | 设计与模型已有（`docs/OPERATOR_RESOLUTION_DESIGN.md`、`formal/operator_resolution.tla`）；实现切片 R1 至 R6 在那份文档里 | 见该文档 §10；R1 与 S2 合并为同一次 v6 |

## 9. 请 Owner 确认的点

不阻塞本切片；给出推荐，不同意就说。D5 是这次建模意外发现的，分量最重。

**2026-10-25 记录**：Owner 回复“合并，同意，继续自主推进，先做D5吧”——CI 步骤已按同意接入（PR #200）；D1 至 D4 按推荐方案采纳（这是我对“同意”的解读，若有异议请指出，实现切片开工前都可以改）；D5 先做，设计与模型见 `docs/OPERATOR_RESOLUTION_DESIGN.md`。

- **D1 类 A 是否一律写 NotSent。**推荐一律写（R-18 的统一路线）；若 Q19 量出拒绝循环里每次多一条持久链太贵，先把纯静态检查前移到 `Intent` 帧之前减少拒绝次数——那是优化，不改语义。
- **D2 第一份真实日志之前的版本策略。**推荐：现在直接升 v6，不做迁移工具（v4、v5 的先例）；第一次会写下我们要保留的日志的运行（M5 的 Testnet 联调）起，§5 的 1 到 6 全部生效。**2026-11-13 记录**：Owner 对 S4 的计划答“不保留审计日志”——2026-11-07 Testnet 联调留下的审计日志不保留，所以“直接升 v6”适用，S4 不做迁移清单（S7 仍只在存在要保留的真实日志时才需要）；写 v6 的开关选“显式的构造配置”（`NeverSentWrites`，默认关）。
- **D3 NotSent 之后是否归还本地限流预留。**推荐归还（请求没有离开；只动本地限流器，不对交易所状态做任何断言）。
- **D4 claim 取在 `call` 之前还是端口的写出回调里。**推荐 S5 先取在 `call` 之前；写出回调作为后续优化，目的只是缩小 `Ambiguous` 窗口。
- **D5 操作员的回头路（已于 2026-10-25 有设计：`docs/OPERATOR_RESOLUTION_DESIGN.md`）。**NotSent 管不到“claim 之后、写出之前崩溃”的订单，也管不到已经升级给操作员的订单；今天它们没有任何出口（§1 第 2 点，L-46）。推荐另开一个设计：操作员在交易所核实“这个 COID 没有订单”之后，经一个带认证的工具写一个**操作员出具的闭合帧**（与 NotSent 不同的事件，证据来源是人而不是进程内决定字），并让清扫的未决数随之减少。它独立于本文的协议，应排在 S5 之后、M5 联调之前。

## 10. 与蓝图 F.5 的对照

F.5 里“Required future model invariants”五条，本切片后：前三条（`DurableNotSent ⇒ ¬everCrossed`、`everCrossed ⇒ DurableIntent ∧ DurablePrepared`、`Released ⇒ …`）在**协议层**由 `notsent_closure.tla` 检查（`everCrossed ⇒ DurablePrepared` 是 `SendImpliesDurablePrepared`；Intent 帧未建模）；第四条（日志 / anchor / 目录覆盖）与第五条（单发送者）**没有**检查。F.9 要求的负控里，这里覆盖：旧 `AbortedPreSend` 双发、claim / revoke 两种次序、失败仍落盘、迟到落盘；**没有**覆盖：暂停复活与网络在途（6.7）、槽位跨代（PERF-01）、升级中崩溃与回绕前停机（S7）。S2 至 S6 的实现覆盖读取、恢复、默认关闭写入、两类本地拒绝和最终授权；实际文件测试覆盖关闭成功及失败恢复，TSan 负向控制覆盖授权发布内存序。S7 迁移只在存在需要保留的真实日志时启用；异步传输和进程暂停复活的验证继续按各自任务实施。
