# NotSent 闭合与发送前最终授权 —— 语义冻结与协议模型（Q23 切片 1）

状态：**设计，语义已冻结；没有任何实现**。无代码、无枚举取值、无日志字节变化。起草 2026-10-25；Owner 在 2026-10-24 对计划说“开始”。
依据：蓝图 v2.9.0 的 `5.9.2`、F.5、F.9、R-18 / R-22（Owner 已选统一 NotSent）、Q23、FI-055、FI-056、任务 SAFE-07 与 1A.6。
模型：`formal/notsent_closure.tla`（结果见 §7）。本文的“必须 / 不得”是对后续实现 PR 的要求；**数值取值不在本文分配**，随实现 PR 同步分配，并过 `tools/spec_enum_diff.py`、codec 与恢复扫描。

## 1. 问题

1. 发送前最后一道复核不存在：`Prepared` 帧 ACK 之后到 `submit_port.call()` 之间没有任何检查（蓝图 L-40）。令牌是 Gate 2b 签发的快照，之后约十个门与两次持久追加，私网流可能掉线、`serverShutdown` 可能到达、熔断可能锁存，订单照发。
2. 即使加了最后一门，**拒绝之后订单怎么办**：`Prepared` 已落盘，恢复只会把它读成 `Ambiguous`（可能已发出），占用在途槽位与资金预留。而且**对账关不掉它**：`query_order()` 把一切不是“正向匹配”的回答——包括“交易所没有这个订单”的错误——折叠成 `QueryOutcome::Inconclusive`（`binance_private_rest.hpp:query_order`、`parse_order_query_response`；币安的 -2013 也没有“从未发送”的语义），所以一个从未发出的 `Prepared` 订单永远得不到结论：R-10 隔离判据到点后升级给操作员，并在 L-46 的意义上永久计入清扫的未决数，屏障不再完成。这一条同样适用于“`Prepared` ACK 之后、真正发出之前崩溃”的订单。要么接受这个代价，要么有一个**能证明“没有发出”的持久记录**。
3. 发送前被拒的订单（Gate 11 到 12c）只留一个 `Intent` 孤儿，预算 `kMaxIntentOrphans` 是唯一的界（L-37）。

蓝图的选择（R-18 / R-22）：**一个统一的 NotSent 持久闭合**，同时服务以上两类拒绝。本文冻结它的证明条件、决定字、帧、恢复与版本规则，并用 TLA+ 证明协议；不写实现。

**不能退回的教训**：规格 round 13 的 `AbortedPreSend` 把“追加失败”读成“没有发生”，在 round 17 被整体删除（`docs/SPEC_INVARIANTS.md` 的 `OrderState::AbortedPreSend` 条目；`formal/durable_log_recovery_abortedpresend.cfg` 是它的必败反例）：一次 `Failed` 的追加可能已经落盘（迟到的 fsync）。NotSent 因此**只能是一个正向事实**：由一个**事先决定了不发送**的所有者写下，绝不能由失败、超时或“恢复后看起来没发”推出来。

## 2. 树内现状（2026-10-25，master `a3ddea8`）

| 事实 | 位置 |
| :-- | :-- |
| Gate 顺序：10b `Intent` 持久 ACK → 11 操作员确认 → 12 端口有效 → 12b 在途守卫 → 12c 限流扣费 → 在途登记 → **12d `Prepared` 持久 ACK（ACK 之后才 `transition_to(Submitting)`）** → `submit_port.call()` → 13 结果路由（每个分支先持久 ACK，再转状态、释放槽位） | `live_submit_orchestrator.hpp:orchestrate_submit` |
| `Prepared` 追加失败：状态留在 `Intent`，在途槽位**不**释放，返回 `AuditWriteNotAcked`（写入可能已落盘） | 同上，Gate 12d |
| 恢复扫描：每个 COID 的首帧 `resulting_state` 必须是 `Intent`；之后同状态重复或经 `validate_transition`；扫描结束时 `Submitting` 重映射为 `Ambiguous` 检查点，`Intent` 只计孤儿，`is_exchange_final` 的跳过 | `durable_audit_sink.hpp:run_recovery_scan_impl` |
| 状态机：`Intent → Submitting` 是 `Intent` 的唯一出边；`Submitting → {Accepted, Rejected, Ambiguous, Filled, PartialFill}`；`is_terminal` 之后一律 `AlreadyTerminal` | `order_lifecycle.hpp:validate_transition` |
| `is_exchange_final` 已有生产调用点（恢复扫描跳过、编排器 Gate 13、`OrderTracker` 槽位释放与 `drain_reconcile_events`）——`docs/SPEC_INVARIANTS.md` 里“已定义，未启用”的措辞已过期 | `order_tracker.hpp`、`live_submit_orchestrator.hpp` |
| 事件与状态枚举只追加：`AuditEventType` 0–21、`OrderState` 0–11；codec 的合法性上界写死在最后一个值上 | `audit_trail.hpp`、`order_lifecycle.hpp`、`durable_frame_codec.hpp:is_legal_audit_event_type / is_legal_order_state` |
| 机器字段 `detail_code`（int64，含义随事件类型：交易所错误码、事前检查码）；`detail_msg` 是人读短文 | `audit_trail.hpp:AuditRecord` |
| 帧格式 v5：载荷 165 字节（`kAuditRecordWireSize`），完整帧 256 字节，`format_version` 不符 → `UnknownVersion`；v3 → v4 → v5 每次升版都因“仓库里没有任何已部署数据”而零迁移成本 | `durable_frame_codec.hpp` |
| 有日志而缺首个 anchor：`IoError`（失败关闭，不自动放行） | `durable_audit_sink.hpp:finalize_scan_with_anchor_check` |

## 3. 定义

- **跨越（crossing）**：订单请求的第一个字节可能离开进程的那一刻。**只有一个方向的义务**：跨越 ⇒ 之前已有持久 `Prepared` 且决定字为 `SendClaimed`；反过来不成立（claim 之后、真正写出之前崩溃，订单也读作 `Ambiguous`，这是保守方向）。
- **决定字（decision word）**：每个在途订单一个，值 `None → Undecided → {SendClaimed → Sent | Rejecting}`。`Undecided` 在 `Prepared` ACK 时建立。
- **NotSent**：一个持久帧（新事件 + 新终态），语义是“这个订单**从未**跨越”。终态；不是交易所的 `Rejected`。
- **类 A（发送前拒绝）**：`Intent` 之后、`Prepared` 之前被本地拒绝（Gate 11 / 12 / 12b / 12c）。前驱帧是 `Intent`。
- **类 B（最终门拒绝）**：`Prepared` 之后、决定字取到 `Rejecting`。前驱帧是 `Prepared`。
- **单次尝试 COID**：一个 client-order-id 只对应一次尝试（Gate 9 每次生成新的）。被 NotSent 关闭的 COID 永不再启动。

## 4. 协议

### 4.1 最终门的位置

紧接 `Prepared` 持久 ACK 与 `transition_to(Submitting)`，**在 `submit_port.call()` 之前**，取 claim。这是任务 1A.6（`5.9.2`）要落地的门；它复核的七项见蓝图，本文不改。claim 越靠近传输边界越好：v1 取在 `call` 之前（保守——`call` 里构造与签名请求的时间都算“可能已跨越”）；端口以后若提供“写出前一刻”的回调，claim 应移到那里以缩小 `Ambiguous` 窗口（待定，见 §9 D4）。

### 4.2 决定字与线性化

1. 决定字**只能**由比较并交换（CAS）从 `Undecided` 迁出：所有者 `Undecided → SendClaimed`（当且仅当全部检查通过且没有已锁存的撤销），所有者或撤销者 `Undecided → Rejecting`。`SendClaimed`、`Sent`、`Rejecting` 之后**任何东西都不得覆盖**。
2. **对每一次撤销 R 与每个订单 o：要么 o 的 claim 线性化在 R 之前（o 可能已在途，撤销者必须按“可能已发出”处理，不得承诺撤回字节），要么 o 被拒绝。**“检查撤销”与“claim”必须是同一个线性化点；先查一遍再 claim 不构成证明（`5.9.2` 的“七次独立 load 后普通 call”）。
3. 今天发送与撤销事件的消费都在热线程上，决定字退化为局部变量，线性化点就是门的那次读取；**一旦发送由另一线程执行（异步传输），或撤销由另一线程发起（熔断、对账），决定字必须是在途槽位里的原子字，绑定槽位代次**（持有旧句柄的迟到撤销者不得影响被复用的槽位），消费点必须再校验并一次性消费授权。选 CAS 单字还是撤销闩锁加扫描的 Dekker 配对，是切片 S5 的并发设计，带自己的 TSan 负控。
4. 取到 `Rejecting` 之后，同一个 COID **不得**再发送；取到 `SendClaimed` 之后，**不得**写 NotSent。

### 4.3 NotSent 何时写、写什么

- 写者：只有**取得 `Rejecting` 的那个进程实例**（类 B），或经过类 A 拒绝路径的实例。**恢复不得为 `Prepared` 之后没有终局帧的订单写 NotSent**——那是 `Ambiguous`，交给对账（模型的 `RecoveryNeverClosesPrepared` 开关）。
- 内容：`AuditRecord`，新事件类型、`resulting_state` = 新终态、COID、价格数量原样、成交为 0、`side` 保持；**拒绝原因放 `detail_code`**（不放 `detail_msg`，不占用其它字段）。原因集合（至少）：类 A —— 确认缺失、确认不符、端口无效、在途表不可用、限流耗尽、重复在途；类 B —— 就绪纪元变化、屏障未处于签发代次的 `Reconciled`、订阅失效或收到 `serverShutdown`、熔断锁存、时钟令牌过期、授权截止、槽位或代次不符、预留丢失。类 A / 类 B 由前驱帧区分，不另设字段。
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
| S2 codec 读者 | `format_version` 6 与两套合法集合，只读不写；未知版本、v5 帧里的 v6 值、版本回退各有明确状态 | MSVC + GCC `none`；codec 表驱动用例 |
| S3 状态机与恢复 | `NotSent` 状态与边、`is_resolved`、恢复表（§4.6）、重放语料；**命名**：`order_tracker.hpp` 已有 `QueryOutcome::NotSent`（R-10，意思是对账**查询**没有离开进程），订单状态与事件的新名字要与它区分（实现 PR 里定），否则 grep 和读者都会混 | `none` + `address`；表中每一行与每个 `Corrupt` 一个用例；枚举 / 规格对照 |
| S4 写入 | sink 追加 NotSent（沿用 `append_durable`）、故障注入：Failed、迟到、撕裂尾 | `address`；旧 `AbortedPreSend` 双发场景作为必败 / 必拒用例保留 |
| S5 最终门（1A.6、FI-055） | 决定字与门；撤销者与所有者的两种次序；`ServerShutdown`、代次变化、熔断、授权截止注入 | `thread` + arm64 重复；**新增一个 TSan 负控**（把决定字的配对降成 relaxed 必须被抓到）；变异检查 |
| S6 类 A 闭合（SAFE-07、FI-056） | 各拒绝路径写 NotSent；孤儿计数；纯静态预检前移只作减量优化 | 孤儿预算与告警；拒绝循环的日志规模有界；Q19 先量出每次拒绝多一条持久链的成本 |
| S7 迁移 | 清单、读者先行开关、回退前停机；仅当存在真实日志 | 迁移中崩溃、清单撕裂、旧构建遇 v6 |
| S8 操作员闭合（D5，L-46） | 另开设计：操作员出具的闭合帧与清扫未决数的减少 | 另定；先有设计与模型，再有实现 |

## 9. 请 Owner 确认的点

不阻塞本切片；给出推荐，不同意就说。D5 是这次建模意外发现的，分量最重。

- **D1 类 A 是否一律写 NotSent。**推荐一律写（R-18 的统一路线）；若 Q19 量出拒绝循环里每次多一条持久链太贵，先把纯静态检查前移到 `Intent` 帧之前减少拒绝次数——那是优化，不改语义。
- **D2 第一份真实日志之前的版本策略。**推荐：现在直接升 v6，不做迁移工具（v4、v5 的先例）；第一次会写下我们要保留的日志的运行（M5 的 Testnet 联调）起，§5 的 1 到 6 全部生效。
- **D3 NotSent 之后是否归还本地限流预留。**推荐归还（请求没有离开；只动本地限流器，不对交易所状态做任何断言）。
- **D4 claim 取在 `call` 之前还是端口的写出回调里。**推荐 S5 先取在 `call` 之前；写出回调作为后续优化，目的只是缩小 `Ambiguous` 窗口。
- **D5 操作员的回头路。**NotSent 管不到“claim 之后、写出之前崩溃”的订单，也管不到已经升级给操作员的订单；今天它们没有任何出口（§1 第 2 点，L-46）。推荐另开一个设计：操作员在交易所核实“这个 COID 没有订单”之后，经一个带认证的工具写一个**操作员出具的闭合帧**（与 NotSent 不同的事件，证据来源是人而不是进程内决定字），并让清扫的未决数随之减少。它独立于本文的协议，应排在 S5 之后、M5 联调之前。

## 10. 与蓝图 F.5 的对照

F.5 里“Required future model invariants”五条，本切片后：前三条（`DurableNotSent ⇒ ¬everCrossed`、`everCrossed ⇒ DurableIntent ∧ DurablePrepared`、`Released ⇒ …`）在**协议层**由 `notsent_closure.tla` 检查（`everCrossed ⇒ DurablePrepared` 是 `SendImpliesDurablePrepared`；Intent 帧未建模）；第四条（日志 / anchor / 目录覆盖）与第五条（单发送者）**没有**检查。F.9 要求的负控里，这里覆盖：旧 `AbortedPreSend` 双发、claim / revoke 两种次序、失败仍落盘、迟到落盘；**没有**覆盖：暂停复活与网络在途（6.7）、槽位跨代（PERF-01）、升级中崩溃与回绕前停机（S7）。Q23 仍然开放：语义已冻结且有模型，实现、迁移、负控、恢复验收都还没做。
