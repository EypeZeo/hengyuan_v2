# 操作员解决（Operator Resolution）：被困订单的回头路（D5）

状态：**设计，语义已冻结；切片 R1（恢复读者）已合入（PR #203，`a4372ba`，2026-10-25）；切片 R2（证据原语：栅栏算术、查询结果分类、`allOrders` 窗口、结论函数）已合入（PR #205，`99d8cb4`，2026-10-26）；切片 R3-0（工具的持久层数据源：检查点的模式与发送开始时间、写者 `append_operator_resolution()`、读写共用的 `check_resolution()`、控制面日志的品种名折叠）已合入（PR #207，`f7b964c`，2026-10-27）；切片 R3a（离线工具的流程：四个端口、确认短语与读行器、证据文件、`resolve_order()`，对着回环假交易所证明；没有真实传输与命令行）已合入（PR #210，`7b66a62`，2026-10-29）；生产代码里没有任何一方调用写者，调用方只有离线工具的流程头（命令行是 R3b）**。起草 2026-10-25；Owner 于 2026-10-25 对 D5 说“先做D5吧”，对 R1 与 R2 说“开始”，对 PR #203 与 #205 说“合并”，2026-10-26 对 R3 的切法（R3-0 → R3a → R3b）说“开始，按这个切法做R3-0”，2026-10-27 对 R3a 说“同意，开始R3a”。
依据：SUBMITPORT 规格 §6.5 第 4、5 条（已被接受的 `OrderOperatorResolved`）；`docs/NOTSENT_CLOSURE_DESIGN.md` §1、A6、D5；蓝图 L-43、L-46、`5.9.4`。
模型：`formal/operator_resolution.tla`（§8）。本文的“必须 / 不得”是对后续实现 PR 的要求；**数值取值（事件号、位布局的最终形式）随实现 PR 分配**，并过 `tools/spec_enum_diff.py`、codec 与恢复扫描。R1 已分配：事件号 22，`detail_code` 的位布局冻结在 `native/include/hengyuan/operator_resolution_frame.hpp`。

## 1. 问题：一个被困订单就是一块砖

| # | 事实 | 位置 |
| :-- | :-- | :-- |
| 1 | 对账**不能**下“交易所没有这个订单”的结论：`query_order()` 把一切不是正向匹配的回答（包括“查无此单”的错误）折叠成 `QueryOutcome::Inconclusive` | `binance_private_rest.hpp:query_order`、`parse_order_query_response` |
| 2 | R-10 隔离：默认“已发出的查询 5 次且过 5 秒，或绝对上限 15 秒”即升级给操作员；升级写 `OrderEscalated`（必要时先补一条 `OrderAmbiguous` 保证重放合法），升级订单不再被轮询 | `order_lifecycle.hpp:UnknownQuarantinePolicy`、`order_tracker.hpp:poll_once`、`ReconcileDurablePublisher` |
| 3 | 同一进程里，清扫把升级订单**永久**计入未决数，屏障不再完成（L-46） | `order_tracker.hpp:OrderTracker::count_unknown_fate` |
| 4 | 重启后恢复扫描**不**跳过非终局订单：检查点被登记为在途并送去对账；`StartupRecovery` 在 `Reconciling` 里等所有恢复订单“已知”，超过期限即 `Degraded`——粘性，只有重启能清，而重启又从同一份日志恢复 | `durable_audit_sink.hpp:run_recovery_scan_impl`、`startup_recovery.hpp:apply_recovery`、`tally_recovered_orders`、`StartupRecovery::update` |
| 5 | 日志不可编辑：MAC 链、tip anchor、存储身份。唯一的“出口”是弃用整个存储（新的存储身份），代价是丢掉全部审计连续性与其它订单的记忆 | `durable_audit_sink.hpp`、`durable_log_store.hpp` |
| 6 | 规格 §6.5 第 4 条设计了出口（`OrderOperatorResolved`）；`SPEC_INVARIANTS.md` 记录“用户已明确确认这一轮不做，留待后续单独排期”；树内没有任何实现 | `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.5 |

合起来：**任何一个 `Prepared` 之后没有闭合帧、又不被交易所查到的订单，都会让按设计的启动路径永远到不了 `Ready`。**这样的订单有三个来源：崩溃在 `Prepared` ACK 与发送之间（从未发出）；请求发出但被交易所在到达撮合引擎之前拒绝、而响应丢失（官方对 `-1013` 的说明是“请求没到撮合引擎”，其它不产生订单的拒绝同理）；NotSent 帧丢失（`NOTSENT_CLOSURE_DESIGN.md` §4.5）。

注：`StartupRecovery` 至今没有进程接线（L-43），所以第 4 条是**已设计的启动路径**的性质，不是已观察到的事故。

## 2. 范围与非目标

范围：一个**离线工具**、一个持久帧、恢复读者的规则、证据与栅栏的合同，使操作员能在**可证明交易所没有该订单**时结清它。

**v1 只做一种结论：ABSENT**（交易所没有此订单，也永远不会有），落成 `Rejected`、零成交、`exchange_order_id = 0`。理由：它没有仓位影响，所以不需要先有账务 / `PositionTruth` 的恢复路径；而被困订单的主体正是这一类。

非目标：在线命令（离线工具已够，见 O1）；带成交的解决（`Filled`，或部分成交后取消，K3）；交易所有、零成交、已终局的订单（K2，含 `-2026` 已归档的）；让进程自己下 ABSENT 结论（O5）；`EscalatedLedger` 的容量记账（规格 §6.5 第 1 至 3、6 条，独立问题）；比“能在这台主机上运行工具”更强的操作员身份认证；多存储。

## 3. 与规格 §6.5 的关系

| §6.5 | 本文 |
| :-- | :-- |
| 事件 `AuditEventType::OrderOperatorResolved`（新值） | 沿用名字，值不在此分配；与 NotSent 同在 v6，一次升版（`NOTSENT_CLOSURE_DESIGN.md` §5） |
| 承载终态 `{Filled, Cancelled, Rejected, Expired}` 与成交字段 | v1 读者**只接受** ABSENT（`Rejected`、零成交）。K2 / K3 另起切片，读者与写者一起改：带成交的帧会改变 `PositionTruth` 的恢复语义 |
| `EscalatedToOperator → {…}` 加进 `validate_transition()` | **不加。**`validate_transition` 被活进程的 `OrderRecord::transition_to` 共用，加边等于允许任何自动路径离开 `EscalatedToOperator`。边放进恢复读者专用的 `validate_operator_resolution(from, to)`，且只对新事件类型有效；`validate_transition(EscalatedToOperator, *)` 必须保持 `AlreadyTerminal`，由守卫测试钉住。“replay-legal for exactly this event type”靠这个结构实现，而不是靠一条状态边 |
| 只有 durable `Acked` 才释放台账条目 | 沿用。离线工具下“释放”= 下次启动的恢复不再为该订单产生检查点 |
| 恢复时无 `OrderOperatorResolved` 的升级订单进 `out_escalated`，已解决的是终态 | 与 `EscalatedLedger` 一起（未实现）；D5 不依赖它 |
| 水位线 32、第 65 个升级阻断提交 | 不变，D5 不碰 |

规格没有、本文新增：离线工具；证据与栅栏；容忍 `CapacityExceeded` 的打开模式；守卫测试；迟到成交告警；环境 / 账户绑定的前置条件。

## 4. 哪些订单可以被解决

| 日志里该 COID 的最后状态 | 怎么来的 | v1 |
| :-- | :-- | :-- |
| `Submitting`（`Prepared` 之后无闭合帧） | 崩溃在 ACK 与结果之间：从未发出，或发出但丢失，或发出且被接受（三种读起来一样） | 工具读交易所：只有“无此单”才可结清；有则留给对账 |
| `Ambiguous` | POST 超时 / 响应丢失后写下 | 同上 |
| `EscalatedToOperator` | R-10 升级 | 同上，经操作员边（§3） |
| `Accepted`、`PartialFill` | 活单 | **不是被困**：对账按固定节奏轮询。操作员要么在交易所取消它、让对账看到终局，要么留着 |
| 终局状态 | | 不处理 |
| 只有 `Intent` | 孤儿 | 从未发出，**不是 D5**（SAFE-07、NotSent 类 A） |
| `mode == DryRun` | | 没有交易所，拒绝 |

## 5. 核心论证：何时可以说“交易所没有它，也永远不会有”

### 5.1 官方文档给了什么（2026-10-25 核对）

- 请求被接受的规则是**两次检查**：到达时 `serverTime - timestamp <= recvWindow`，开始处理后、转给撮合引擎之前再查一次。`recvWindow` 默认 5000 ms，上限 60000 ms（Spot REST，“Timing security”）。
- 查询：`GET /api/v3/order` 权重 4（数据源 Memory => Database）；`GET /api/v3/allOrders` 权重 20，数据源 **Database**，`startTime` 到 `endTime` 不得超过 **24 小时**；`GET /api/v3/openOrders` 不带 symbol 权重 80；`GET /api/v3/time` 权重 1。
- 错误：`-2013 NO_SUCH_ORDER` 只有一句 “Order does not exist.”；**`-2026 ORDER_ARCHIVED`**（零成交且 90 天前取消 / 过期的订单）是另一个码——归档订单**存在过**，不是“从未存在”；`-1013` 表示请求没到撮合引擎；`-1007` 表示发送与执行状态都未知。
- 文档**没有**承诺 `-2013` 表示“从未发出”，也**没有**给出读取延迟的上界。所以证据不能建立在“`-2013` 就是不存在”上，而要建立在栅栏上。

### 5.2 推导

设 W 为发单请求的 `recvWindow`（≤ 60 s），B 为发送进程时钟偏移的界（SAFE-01 切片 4 起 `sync_clock` 拒绝超过 `recv_window` 的偏移，故 B ≤ 60 s）。发送进程最后一次签名发生在它死亡（日志句柄关闭）**之前**；请求的 `timestamp` ≤ 真实签名时刻 + B；被接受要求 `serverTime ≤ timestamp + W`。于是：

> 工具取得独占锁之后，再经过交易所时间 W + B，**该进程的任何请求都不可能再被接受**。

读取可能落后于交易所的真实状态（`allOrders` 来自数据库）：一次读只反映“s 之前的状态”，0 ≤ s ≤ L。所以证据读必须发生在栅栏之后**再过 L**：

> t_读 − L ≥ t_锁 + W + B　⇒　读到“无此单”　⇒　永远无此单。

所有时刻都用**交易所时钟**度量（`GET /api/v3/time`），不信本机墙钟：栅栏起点取取得锁之后读到的第一个 `serverTime`（保守：不早于真实的锁定时刻）；证据查询之前再读一次 `serverTime` 作为 t_读 的下界。

参数取值：W = B = 60 s（交易所上限，不依赖已丢失的配置历史），再加 30 s 余量覆盖两次同步之间的漂移与调度，栅栏 150 s；L = 120 s（策略值，`NEEDS_BASELINE`，等 Testnet 演练测量，只许调大）。**总等待约 4.5 分钟**，相对于人工核对步骤可以忽略。

### 5.3 工具无法检验、必须写进操作员确认的前提

- **A-D5-1** 日志句柄的生命周期等于进程的生命周期：句柄关闭之后，该进程不能再发出请求。
- **A-D5-2** 没有第二个持有同一 API 凭据的发送者（主机接管、旧实例复活：任务 6.7、FI-047）。
- **A-D5-3** 证据查询打到订单当初所发往的**同一交易所环境和同一账户**。日志**不记录**这一点（§9 R-env）。
- **A-D5-4** 订单的 symbol 取自日志（MAC 保护），不是操作员输入。

### 5.4 模型里的对应

模型的 `Window` = W + B，`Lag` = L；五个开关分别是：独占打开（`ExclusiveOpen`）、栅栏从工具自己的锁起算而不是操作员口述的崩溃时间（`FenceFromLock`）、栅栏本身（`FenceChecked`）、读延迟余量（`LagAllowance`）、真的读交易所（`ChecksExchange`）。每个关掉都出反例（§8）。

## 6. 工具与流程（离线）

工具名暂定 `hy_operator_resolve`，沿用 `durable_audit_log_export` 的离线工具先例（命令行、退出码、不进生产路径）。

1. **前置**：交易进程已停止，且没有别的主机持有该凭据。
2. **打开存储**：专用的 `OperatorOpen` 模式——与交易进程同样的独占打开和恢复扫描，只接受 `Clean` / `Recovered`，以及 `CapacityExceeded`（仅为了能追加：超过 64 个未结清订单时，普通打开会让 sink 围栏，操作员就无路可走）。其它任何状态拒绝。这个模式**不得**被交易进程使用（守卫，§9）。
3. **列表**：§4 表中前三类订单与日志事实（symbol、方向、数量、价格、各帧时间、查询次数）。逐个处理。
4. **栅栏**：从取得锁起计；到点之前不做证据读。
5. **证据**（只读、签名 GET）：
   - (a) 点查 `origClientOrderId`：必须是 `-2013`。`-2026`、其它错误码、任何成功响应一律拒绝并说明原因；
   - (b) `allOrders`：该 symbol，窗口 [`Prepared` 时刻 − 2W，t_读]，按 ≤ 24 小时分页，页数上限 14（超过则拒绝，请操作员人工处理）——其中不得有此 COID；
   - (c) `openOrders`（不带 symbol）：不得有此 COID；
   - (d) 账户余额快照：仅显示，供操作员核对。
   时间窗起点用 `Prepared` 帧的 `recorded_utc_ms`（仅当 `time_kind` 为 `ServerCorrectedUtc`），否则用 COID 里的时间戳（`HY-{timestamp_ms}-…`，需是合理墙钟）；两者都不可用则拒绝。
   **R2 的实现取舍**（`operator_resolution_evidence.hpp` / `operator_resolution_response.hpp`，都是纯函数，由 `conclude_absent()` 一处汇总，工具递进去的只有每次读的判定）：
   (a) 只有 HTTP **400** 且错误体 `code` 恰为 -2013 才算“无此单”（状态是声明的一部分，真实表现由 R6 测量）；`-2026` 是 `Archived`，意思是订单存在过；
   (b) 每页窗口不超过 24 小时少 1 秒，请求 `limit` = 1000；官方文档对截断没有任何标记，所以满页（1000 条）一律当作可能被截断而拒绝；窗口首尾相接（下一页起点 = 上一页终点），无论交易所把终点算开区间还是闭区间都不留缝；`allOrders` 的每个元素的 symbol 必须等于所请求的，错品种的页什么也证明不了；
   (c) 时间窗起点取两个锚点（`Prepared` / `Submitting` 帧的服务器校正时间、COID 里的时间戳）中**较早的**再减 2W——比上面的“优先帧时间，否则 COID”只会更早（更多页，绝不留缝）；
   (d) 所有时间必须落在 2020-01-01 至 2100-01-01 之内，之外的读数（例如解析失败而默认成 0 的 serverTime）永远满足不了栅栏。
6. **展示与确认**：显示全部证据与清单（操作员须在交易所 App 的订单历史和成交历史里自行核对该时间窗内没有此订单 / 成交、余额与预期一致、API 权限未变），然后要求键入确认短语，绑定 COID、结论（`ABSENT`）、品种名、环境名、账户 UID 和证据摘要前 8 位（品种名与账户 UID 是 R3a 实现时加的：日志给不了它们，见 L-50 与 L-48，让操作员把它们亮出来再键入）。输入方式：字节来源与原生标准输入适配器沿用 `OperatorInputReader` 的，读行器新写（`OperatorInputResult` 只有 7 字节、`OperatorConfirmationGate` 是下单门，装不下短语）。
7. **追加** `OrderOperatorResolved`，必须 durable `Acked`。失败：报告并退出，不重试——重新运行是幂等的（恢复扫描已把写进去的订单当终局，不再列出）。
8. **证据附件**：`<store>.operator-evidence/<coid>-<utc>.json`——原始响应（去掉签名与密钥）、工具版本、栅栏与读延迟参数、各次 `serverTime` 读数；帧里存它的 SHA-256 前 32 位十六进制。
9. **退出**。操作员重启交易进程；恢复读到该帧，订单是 `Rejected`（终局），不再产生检查点。

## 7. 帧与读者

### 7.1 帧（`AuditRecord` 映射，256 字节完整帧布局不变）

| 字段 | 取值 |
| :-- | :-- |
| `event_type` | `OrderOperatorResolved`（值随实现分配） |
| `resulting_state` | `Rejected` |
| `client_order_id`、`symbol_id`、`side`、`price_ticks`、`qty_ticks`、`mode` | 原样取自该订单恢复出的意图 |
| `exchange_order_id`、`filled_qty_ticks`、`avg_fill_price_ticks` | 0 |
| `timestamp_ms`、帧头时间 | 工具的时钟；帧头时间类型 `ServerCorrectedUtc`（取交易所 `serverTime`） |
| `detail_code`（R1 已冻结，`operator_resolution_frame.hpp`） | 位 0–3 结论种类（1 = ABSENT）；位 4 栅栏已满足；位 5 点查为 `-2013`；位 6 `allOrders` 无此单；位 7 `openOrders` 无此单；位 8 操作员已确认；位 9 独占锁已持有；位 16–31 实际等待秒数（封顶）；位 32–47 工具版本；其余位（10–15 与 48–63，含符号位）必须为 0 |
| `detail_msg`（≤ 64 字节 ASCII） | `op=<操作员标签>;ev=<证据附件 SHA-256 前 32 位十六进制>` |

### 7.2 读者规则（违反任何一条 = `Corrupt`，失败关闭）

1. 前一状态必须是 `Submitting`（`Prepared` 之后）、`Ambiguous` 或 `EscalatedToOperator`；前两者走现有的 `validate_transition`（二者到 `Rejected` 本来就合法），第三者走 `validate_operator_resolution`。
2. `resulting_state == Rejected`，成交与 `exchange_order_id` 为 0，symbol、方向、数量、价格、`mode` 与该 COID 的意图一致。
3. `detail_code`：种类 = ABSENT 时位 4 至 9 必须全部置位，保留位必须为 0。（置位只是声明，读者无法核实它为真——真正的保证在 §5 的协议和工具，读者的这一条只是让一个不声称证据的写者无法混进来。）
4. 该帧必须是该 COID 的最后一帧。
5. 本事件只在 v6 帧里合法；v5 帧里出现 = `Corrupt`。
6. （R1 补充）订单先前出现过交易所订单号或成交的，不得被结清：那是一个存在过的订单，ABSENT 的结论与日志自己的记录矛盾。
7. （R1 补充）本事件永远不能是一个订单号的首帧，即使它自称 `Intent`。
8. （R1 补充）一个存储里帧版本单调不减：v5 帧出现在 v6 帧之后 = `Corrupt`（存储可以从 v6 起头）。因此写者随存储尖端的版本走：操作员帧写下之后，交易进程接着写的也是 v6，否则下一次启动就会撞上“v6 之后的 v5”。

### 7.3 产物

该订单不进检查点；恢复提供一个有界的列表 `recovered_operator_resolved()`（供 §9 的迟到成交告警）。R1 的容量规则：`kMaxOperatorResolved` = 1024；这个表必须完整才有用（被错误结清的挂单可能几周后才成交），所以交易进程超过就 `CapacityExceeded`、不启动；`OperatorOpen` 打开模式（只给离线工具）改为计数，`recovery_overflow()` 说明没装下什么，其余失败（`Corrupt`、`IoError`）照旧围栏。

### 7.4 与 NotSent 的区别

| | `NotSent`（`NOTSENT_CLOSURE_DESIGN.md`） | `OrderOperatorResolved`（ABSENT） |
| :-- | :-- | :-- |
| 命题 | 这个订单**从未跨越**发送边界 | 交易所**没有**这个订单，也永远不会有 |
| 证明来源 | 进程内的决定字（先到先得） | 栅栏 + 交易所读 + 操作员确认 |
| 写者 | 做出“不发送”决定的那个进程 | 离线工具 |
| 适用 | 被最终门拒绝、发送前被拒绝 | 崩溃在 ACK 与发送之间、响应丢失的被拒订单、NotSent 帧丢失 |
| 终态 | `NotSent`（新状态） | `Rejected`（现有状态，出处由事件类型区分） |

两者在效果上都是“没有订单、没有仓位”，在来源上不同，不能互相冒充。

## 8. 形式化模型 `formal/operator_resolution.tla`

沿用本目录的惯例（相对时间、饱和计数器、预算而非约束、每个设计决定一个开关且关掉必须出反例），另起一个模块，`durable_log_recovery.tla` 与 `notsent_closure.tla` 一字未动。时间是**相对且饱和**的（请求的年龄、一次接受之后过去的时间、工具持锁的时间），所以图有限而不需要时间上限，活性运行也不会“时间用完”。

**安全性质**：`ClosedAbsentIsTrue`（持久闭合帧 ⇒ 交易所从未接受该请求，闭合之前和**之后**都一样）、`ReplayLegal`、`ExclusiveHolds`，以及模型自检 `DeliveredOnlyIfSent`、`SentOnlyIfPrepared`、`OutcomeOnlyIfDelivered`（对账只关闭交易所有的订单）。**进展**：`EventualSettlement`。

**五个设计开关**（`TRUE` = 设计；每个关掉都必须失败）：

| 开关 | 关掉意味着 | 必败配置报告 |
| :-- | :-- | :-- |
| `ExclusiveOpen` | 工具与活进程可以同时持有日志 | `ClosedAbsentIsTrue` |
| `FenceFromLock` | 栅栏起点是操作员口述的崩溃时间，而不是工具自己的锁 | `ClosedAbsentIsTrue` |
| `FenceChecked` | 没有栅栏 | `ClosedAbsentIsTrue` |
| `LagAllowance` | 证据读不要求落后于栅栏再过 L | `ClosedAbsentIsTrue` |
| `ChecksExchange` | 工具不读交易所，凭操作员的话 | `ClosedAbsentIsTrue` |

**两个环境假设**（各一对配置，一个证明安全性仍成立，一个显示进展何处断）：

| 假设 | 为假时 | 配置 |
| :-- | :-- | :-- |
| `OperatorPathExists`（今天为假） | 没有任何东西能关闭交易所从未见过的订单：`EventualSettlement` 失败 | `operator_resolution_no_path_liveness.cfg`（必须失败） |
| `ReconcileResolvesExisting` | 对账无法匹配交易所有的订单（响应校验失败）：工具也拒绝它（v1 的范围），订单留着 | `operator_resolution_no_match_safety.cfg`（无反例）、`operator_resolution_no_match_liveness.cfg`（必须失败，v1 的边界） |

另有三个可达性探针（必败）：`OpClosedNeverDurable`、`ClosedSentUndeliveredNeverHappens`（关闭一个请求**确实发出过**但从未被接受的订单——栅栏存在的理由）、`PresentVerdictNeverHappens`（工具遇到交易所有的订单并如实说出）。

**进展依赖的公平性假设，不多不少**：时间前进；进程会发出它已 `Prepared` 的订单（或崩溃）；对账能匹配的就匹配；被降级的进程会被停下；工具一旦锁定就读、关、解锁；操作员放弃一次运行的次数有限（`MaxAborts`）；一个没被操作员按住的死进程会被重启（**强**公平：他不能无限地停了又按住）。模型的第一个活性反例恰好说明：没有后两条，对任何设计进展都不成立。

**不在模型里**：帧布局与版本（本文 §7、`NOTSENT_CLOSURE_DESIGN.md` §5）；非原子追加与迟到落盘（`notsent_closure.tla`；这里追加是一步，丢失的闭合追加只是让订单继续未结清、由工具再跑一次）；点查 / 列表 / 未结订单三种读（模型里是一次读，唯一的不完美是延迟）；成交（v1 不关闭有成交的订单）；订单的 symbol；第二个发送者（6.7）。**模型不证明实现。**

## 9. 防线与残余风险

- **R-late 迟到成交被静默丢弃（L-47）。**`drain_user_data_events` 对不在途的 COID 直接 `continue`：没有审计记录、没有计数、没有仓位效果（`binance_user_data_event.hpp`）。如果一次错误的 ABSENT 结论让一个真实存在的订单后来成交，系统看不见。**要求**：对 `recovered_operator_resolved()` 里的 COID，收到 `executedQty > 0` 的执行报告必须触发熔断并写审计。告警**只限**这些 COID——更宽的“任何自己的 COID 的未知成交”会把合法的迟到重复事件误报，属于另一个设计。
- **R-env 环境与账户没有持久绑定（L-48）。**`AuditRecord::mode` 只有 `DryRun` / `Live`；存储身份文件只有 UUID、密钥号和 MAC。工具无法验证它查的交易所和账户就是订单所发往的那个——在 Testnet 上查一个 Production 订单会得到干净的“无此单”。**要求**：生产存储启用本工具之前，存储必须持久记录环境，工具必须解析账户 UID（`OperatorOrderChallenge::account_uid` 的注释写着 `fetch_account()` 当前丢弃 uid）并拒绝不一致。Testnet 演练不需要。过渡期靠操作员确认短语里的环境名与账户 UID。
- **R-guard 守卫测试。**只有工具的翻译单元可以引用写 `OrderOperatorResolved` 的代码；活进程的头文件里不得有任何生产者；`OperatorOpen` 不得被交易进程使用；`validate_transition(EscalatedToOperator, *)` 必须保持 `AlreadyTerminal`。
- **R-wrong 错误证明的后果。**订单实际存在却被结清：无人跟踪的挂单，以及之后的成交与仓位漂移。兜底：R-late 的告警、账户余额对账（SAFE-02、LEDGER-01 的三方收敛，今天只有快照）。这也是 O5 推荐暂不自动化的原因。
- **R-key 只读凭据。**工具只发 GET，应使用只读 API 密钥（O3）；不得使用能下单的密钥做这件事。
- **R-v6 一次升版。**NotSent 与 `OrderOperatorResolved` 同在 v6；读者先行；没有真实日志之前直接升版（`NOTSENT_CLOSURE_DESIGN.md` §5、D2）。

## 10. 实现切片

每一片都是 Tier B：需要 Owner 选定、计划与“开始”。本文只定内容与要钉的负控。

| 片 | 内容 | 验证与负控 |
| :-- | :-- | :-- |
| R1 读者（已合入：PR #203，`a4372ba`，2026-10-25） | 事件类型、`validate_operator_resolution`、§7.2 的接受 / 拒绝表、`recovered_operator_resolved()`、`OperatorOpen` 打开模式、守卫；与 NotSent 的 codec 读者（`NOTSENT_CLOSURE_DESIGN.md` S2）同一次 v6 | MSVC + GCC `none`、`address`；表中每一行与每个 `Corrupt` 一个用例；`validate_transition(EscalatedToOperator, *)` 仍为 `AlreadyTerminal`；变异检查 |
| R2 证据原语（纯逻辑；已合入：PR #205，`99d8cb4`，2026-10-26，见 `docs/SPEC_INVARIANTS.md`“D5 切片 R2”） | 栅栏计算（`constexpr`、用交易所时间）、证据位的编解码、查询结果分类（`-2013` / `-2026` / 其它错误 / 成功，**对账路径不变**）、`allOrders` 分页规划 | 表驱动用例；变异检查；`closure_ledger_check` 若新增 FI 则同步 |
| R3-0 持久层数据源（已合入：PR #207，`f7b964c`，2026-10-27；见 `docs/SPEC_INVARIANTS.md`“D5 切片 R3-0”） | 工具先要有的三样东西：检查点带上 `mode` 与 `send_started_utc_ms`（R2 留下的缺口）；`DurableAuditSink::append_operator_resolution()`——`OperatorOpen` 专用、只写事件 22、用与读者同一个 `check_resolution()`（`operator_resolution_rules.hpp`）预检、成功后写版本变 6；控制面日志的 `recovered_symbol_name()`——只有每个快照都同意才 `Known` | MSVC + GCC `none`、`address`；写者与读者的差分用例（写者放行 ⇔ 读者接受）；每条拒绝都不碰日志、不围栏；变异检查 |
| R3a 工具的流程（仅逻辑与回环；已合入：PR #210，`7b66a62`，2026-10-29；见 `docs/SPEC_INVARIANTS.md`“D5 切片 R3a”） | 请求（订单号、环境、账户 UID、操作员标签、确认等待）、取锁后的栅栏等待、确认短语与读行（环境名、账户 UID、**品种名**亮给操作员）、证据附件；对着注入的端口与回环假交易所（含**落后于真实状态**的读、错误码、翻页）走完整条流程；**没有真实传输**。现有的 `OperatorInputResult` 只有 7 字节、`OperatorConfirmationGate` 是下单门，都不适用，所以读行器新写（循环复制而非模板化，不碰下单确认的路径）；短语比设计多绑了品种名与账户 UID（L-50、L-48） | MSVC + GCC；`address`；栅栏与读延迟的负控；全部拒绝路径各一个用例；变异检查 |
| R3b CLI 与真实传输 | 命令行；KEK 的来源（密钥文件还是参数，Owner 待定）；`PrivateRestEndpoint` / `ResponseKind` / 许可 / 配速 / 冻结守卫的新项（`PrivateRestEndpoint` 是 spec 转录、封闭的，`SignedJson` 上限 256 KiB）；真实 HTTPS 传输 | 同上，加守卫：只有工具的翻译单元能引用写帧的代码 |
| R4 迟到成交告警 | R-late：对 operator-resolved COID 的成交触发熔断 | 单元 + 恢复往返；变异检查 |
| R5 环境与账户绑定 | R-env：存储记环境、解析账户 UID、工具拒绝不一致 | 生产启用的前置 |
| R6 运行手册与 Testnet 演练 | `NATIVE_EXIT_SAFETY_RUNBOOK.md` 新增一节；**Owner** 在 Testnet 上制造一个崩溃窗口订单并用工具结清；测量 `-2013` / `-2026` 的实际表现和读延迟 L | 演练记录；参数 L 的基线 |

K2、K3（存在的订单、带成交的解决）排在账务 / `PositionTruth` 恢复路径能吸收操作员解决的成交之后，另行设计。

## 11. 请 Owner 确认的点（不阻塞合并，给出推荐）

- **O1 离线工具还是在线命令。**推荐离线：停进程、取锁即证明前任已死，栅栏的论证最简单；在线命令要新增控制通道、新的并发与攻击面。
- **O2 v1 范围只做 ABSENT。**推荐是：无仓位影响，不依赖账务恢复；带成交的解决另起。
- **O3 工具使用只读 API 密钥。**推荐是（密钥由你创建，我不接触）。
- **O4 栅栏参数。**推荐 W = B = 60 s、余量 30 s、L = 120 s，总等待约 4.5 分钟，只许调大；L 等 Testnet 演练测量。
- **O5 暂不让进程自己下 ABSENT 结论。**文档没有保证 `-2013` 的含义，也没有读延迟的数；先让人在环里，演练测出实际表现后再评估（帧里已有“操作员确认”位，以后可以加一个“自动”种类而不改格式）。
- **O6 R-env 是生产启用的前置。**推荐是。

## 12. 与 `NOTSENT_CLOSURE_DESIGN.md` 的关系

互相独立：本文不依赖 NotSent 协议，NotSent 也不依赖本文。共享的只有 v6 的升版和读者（R1 与 S2 合并）。NotSent 覆盖“被最终门拒绝的订单”这个自动出口；本文覆盖其余所有不得不靠人的出口。
