# 四模型全仓只读审计（2026-11-09）：分诊与处置

> 状态：由主线会话逐条对照代码核实后写成；基点 `a61d100a2d84e00a7e0fc645d00ce4cdcd48d695`（PR #237）。审计报告是**未经核实的主张**，本表给的是核实后的裁定，不是报告原文的转述。

## 1. 输入与总览

| 报告 | 审计者（按 Owner 的说明） | 发现数 | 核实后 |
| :--- | :--- | :---: | :--- |
| A（`HengYuan_v2_Audit_A_GPT-6-Astra.md`） | GPT-6 Astra | 12 | 12 条属实（级别有调整），其中 8 条已修，4 条排入队列或待 Owner 决策 |
| B（`..._B_DeepSeek-V4.1-Flash.md`） | DeepSeek V4.1 Flash（Max） | 12 | 12 条属实（级别有调整），其中 4 条已修，其余为队列、Owner 决策或待做 |
| C（`..._C_Claude-Opus-5.5.md`） | **实际由 GPT-6 Astra（High）产出**，文件名沿用了旧的模型名 | 5 | 5 条属实（级别下调 4 条），其中 3 条已修或已改注释，2 条排入队列 |
| D（`..._D_Gemini-3.8-Flash.md`） | Gemini 3.8 Flash（High） | 14 | 6 条属实（其中 2 条只是部分属实），3 条是 Owner 已作出的设计或策略决定，**3 条不成立，2 条是增强项/信息项**；D-001 引用了仓库里不存在的函数、成员与工作流文件，应当整条作废 |

可靠性顺序（以本次核实为据）：C ≈ B > A > D。A、B、C 的证据可以逐行对上代码；D 的 P0（F-D-001）是编造的，D 的 F-D-011、F-D-013 的前提与仓库不符，D 的其余条目要自己再核一遍才可采信。

## 2. 逐条裁定

级别列：`报告 → 裁定`。处置列里的“已修”都在 PR #238 里（分支 `fix/audit-2026-11-09-native-tier-a`），见 §4；“队列”指 Tier B，须 Owner 看过计划说“开始”。

### 报告 A

| ID | 级别 | 判定 | 处置 |
| :--- | :--- | :--- | :--- |
| F-A-001 最小名义金额向上取整 | P2 → P3 | 属实：`price_scale + qty_scale > 8` 且乘积落在最小值下方不足 1e-8 时，本地放行交易所会拒的订单 | 已修：新增 `rescale_notional_floor()`，门与预检探针对下限都用向下取整；一条既有测试的期望值（3333 → 3334）因此改正 |
| F-A-002 格点起点 | P2 → P3 | 属实但性质是**规格与官方现行文档不一致**：官方 `filters.md` 现写 `price % tickSize == 0`、`quantity % stepSize == 0`（零起点），本地以 `minPrice` / `minQty` 为起点；二者在 min 是 step 整数倍时（所有常见品种）完全相同 | 队列 TB-5，且须 Owner 先定语义 |
| F-A-003 `maxNotional` 丢失 | P2 | 属实：解析器只存 `minNotional`，`MIN_NOTIONAL` 与 `NOTIONAL` 同时出现时以数组顺序为准 | 队列 TB-5（`SymbolRules` 的持久化格式要升版） |
| F-A-004 重叠 OOS 窗口 | P1 → P2 | 属实：默认 `step == test_window` 时无重叠，`step < test_window` 或手写 `Split` 时被重复复利 | 已修：拼接前的 `_require_stitchable()` |
| F-A-005 非有限输入 | P2 | 属实 | 已修：配置与价格列/信号的有限性校验，`ValidationReport` 增两个带默认值的字段 |
| F-A-006 模拟成交溢出 | P2 → P3 | 属实（模拟器，需要不合理的数量） | 已修：先在副本上算全部金额，任何一步放不下就整笔拒绝 |
| F-A-007 Armed 反向超量 | P2 → P3 | 属实（模拟 `RiskGate`，真实编排器不调用它） | 已修：减仓 = 反方向且数量不超过持仓 |
| F-A-008 EMA/RSI 窗口 65536 | P2 → P3 | 属实：`init()` 的 0xffff 检查漏了不属窗口类的 EMA/RSI | 已修 |
| F-A-009 EMA 中段 NaN 分叉 | P2 → P3 | 属实；**哪一端对是规格问题** | Owner 决策（见 §3） |
| F-A-010 两个常量操作数 | P2 → P3 | 属实：Python 的 `_align` 故意拒绝，C++ 接受 | 已修：Python 求值器为左常量补索引，与 C++ 一致 |
| F-A-011 刻度域检查太晚 | P2 → P3 | 属实但真实调用者的刻度都 ≤ 18，只有越界刻度才触发 | 已修：域检查提到第一行 |
| F-A-012 POST 回报校验 | P1 | 属实；**核实时发现比报告更严重的一条**（见 §5）：解析器接受全部八种状态，`EXPIRED_IN_MATCH` 等终态会被写成 `Submitting → Expired` 的非法帧，恢复扫描判整份日志 Corrupt | 队列 TB-1，须在任何真实 `SubmitPort` 调用者之前完成 |

### 报告 B

| ID | 级别 | 判定 | 处置 |
| :--- | :--- | :--- | :--- |
| F-B-001 守卫扫描器不认原始字符串 | P2 | 属实（读代码确认 `code_only()` 只有 `String` 状态、没有原始字符串；并在新的 `test_cxx_source_view` 里用报告的同一输入复现：旧扫描器在奇数引号的原始字符串之后丢掉全部代码） | 已修（独立 PR）：共享头 `test_helpers/cxx_source_view.hpp` + 原始字符串负控；与 B-010、D-007 同一件事 |
| F-B-002 HMAC 密钥栈副本不擦除 | P2 | 属实（`append_impl` 的 `key_block` 在所有返回路径上都没有擦除） | 队列（Tier B 级的安全卫生，需 Owner 同意热路径上的擦除成本） |
| F-B-003 `ExportOutboxRing` 契约矛盾 | P2 | 属实：环的注释说“硬滞后围栏”，`append_impl` 与导出 worker 写成静默丢弃；目前没有进程接线该环 | Owner 决策（围栏还是尽力而为） |
| F-B-004 恢复扫描 RSS | P2 | 潜在风险，属实的推演 | 登记为 L-63，不修 |
| F-B-005 trade/aggTrade 缺字段取默认 | P3 | 属实（`[[maybe_unused]]` 吞掉 `E`/`t`/`a`/`m` 的错误；缺 `m` 记为主动买入） | 待做（热路径解析，Tier A 小改） |
| F-B-006 klines 时间戳溢出 | P3 | 属实 | 已修：时间戳域 [0, INT64_MAX/4] |
| F-B-007 供应链固定不足 | P3 | 属实 | Owner 决策 |
| F-B-008 Windows 单次 `ReadFile` | P3 | 属实（失败关闭，所以只是不一致） | 已修：`read_exact()` |
| F-B-009 `SetFilePointer` 返回值 | P3 | 属实 | 已修：`SetFilePointerEx` |
| F-B-010 扫描器副本漂移 | P3 | 属实：六份逐字节相同、一份格式变体、一份两视图的 `strip()` | 已修（独立 PR，同 B-001） |
| F-B-011 容量注释 255 | INFO | 属实（`kOrderEventFrameSize == 256`） | 已修 |
| F-B-012 `u < U` 不校验 | P3 | 属实（防御性缺口，只会造成多余的重同步） | 待做 |

### 报告 C

| ID | 级别 | 判定 | 处置 |
| :--- | :--- | :--- | :--- |
| F-C-001 同状态成交增长 | P1 → P2 | 属实，但**重评**：`position_truth.hpp` 早已登记；活线接线时 `drain_reconcile_events()` 用共享基线 `consume_delta()` 算差分，所以增长在该订单的下一份报告里补回；生产代码里没有 `PositionTruth` 的读取方。残余：持久日志与审计没有这一帧，`SweepDone` 不等它 | 队列 TB-2；已把 `position_truth.hpp` 的过时注释改准 |
| F-C-002 marker 推迟后账户读取偏旧 | P1 → P2 | 属实：推迟期间实时轮询仍会推进订单，而账户步骤按 epoch 去重；影响被新鲜度 TTL 与交易所余额校验限制 | 队列 TB-3 |
| F-C-003 模拟 Armed | P2 → P3 | 同 F-A-007 | 已修 |
| F-C-004 现行说明与已运行的 Testnet 程序矛盾 | P2 → P3 | 属实（README、`CLAUDE.md`、`NATIVE_ARCHITECTURE.md` 三处） | 已修；路书一处随文档 PR |
| F-C-005 恒真的 TLA+ 谓词 | P3 | 属实（体内 `\/ TRUE`，没有配置启用） | 已修：删除并写明原因，TLC 重跑通过，负控仍违反 |

### 报告 D

| ID | 级别 | 判定 | 处置 |
| :--- | :--- | :--- | :--- |
| F-D-001 看门狗短路（P0） | P0 → 无 | **不成立**：引用的 `WatchdogDaemon::check_liveness`、`last_activity_us_`、`.github/workflows/ci-pr-gate.yml` 在仓库里都不存在；`watchdog_daemon.cpp` 的多维活性判定（`loop_counter` **或** `last_incoming_ns` 前进）是刻意设计，`hot_thread.hpp` 在热线程上写这两个字段 | 无 |
| F-D-002 seal 失败后的写者状态 | P1 → P2 | 属实（实验复现 `ZstdError`）；有进程级兜底（退出码 72 / 71、systemd 重启、`.part` 恢复） | 队列 TB-4 |
| F-D-003 Sanitizer 未列为必需检查 | P1 → 决策 | 是 Owner 已知的取舍（见 `CLAUDE.md`）；仓库公开后“私有仓库分钟预算”这个理由已经不存在 | Owner 决策（建议 TSan + ARM64 按 PR，ASan 保持周更） |
| F-D-004 自动 revert 受阻于规则集 | P1 → 决策 | `CLAUDE.md` 已写明：规则集下该 push 预期被拒，红色守卫是警报而不是自愈 | Owner 决策（可选：改为自动开 revert PR） |
| F-D-005 维保任务 3 小时时限 | P2 | 属实：仓库脚本 `PT3H`，已部署任务 `PT10H`（2026-11-02 起，实测 `PT10H`） | 已修：参数化，默认 10 |
| F-D-006 脚本未入 CI | P2 | 部分属实：`concurrency_targets_check.py` 只在 `wsl_verify.sh thread` 里跑，`closure_ledger_check.py` 不在任何 CI；报告点名的 `ci-pr-gate.yml` 不存在 | 已修：新作业 `repo-consistency` |
| F-D-007 守卫词法扫描器 | P2 | 同 F-B-001 | 已修（独立 PR，同 B-001） |
| F-D-008 供应链 | P2 | 同 F-B-007 | Owner 决策 |
| F-D-009 CODEOWNERS 未强制 | P2 → 决策 | 与“0 个批准、单人所有者”的设置一致；开启后他自己的 PR 无人可批 | 不做 |
| F-D-010 文档基线滞后 | P2 | 部分属实：README 仍提 `create_listen_key()`、仍说没用过真实凭据 | 已修（README 等）；路书头部表格的基线按其自述是历史基线，不动 |
| F-D-011 `.gitattributes` | P3 | **不成立**：文件里没有 `docs/*.md` 通配，是逐个列出受哈希校验的文档，刻意如此 | 无 |
| F-D-012 基准缺失 | P3 | 增强项，不是缺陷 | 不做 |
| F-D-013 脚本编码守卫 | P3 | 大体不成立：6 个脚本里 4 个已 `reconfigure`，报告点名的 `spec_xref_check.py` 就是其中之一 | 无 |
| F-D-014 | INFO | 无问题 | 无 |

## 3. 需要 Owner 决定的事

1. **EMA 中段 NaN 的语义**（F-A-009）：C++ 保持前值继续，Python 因递推永久为 NaN。建议以 C++（保持状态、只让这一根为 NaN）为准——前者对一根坏数据更宽容——但这是规格，不是我的决定。
2. **价格 / 数量格点的起点**（F-A-002）：官方现行文档是零起点。建议在规则入口拒绝 `min % step != 0` 的品种（此时两种解释不同），其余品种两种解释等价，什么都不用改。
3. **Sanitizer 是否列为必需检查**（F-D-003）：建议 TSan + ARM64 作为每个 PR 的必需检查，ASan+UBSan 保持周更与手动。
4. **供应链固定**（F-B-007、F-D-008）：`FetchContent` 改提交哈希、Actions 固定 SHA、Python 依赖加哈希——全是 Tier A，只是会让 Dependabot 的升级变成手工。
5. **`ExportOutboxRing` 满环时的语义**（F-B-003）：围栏还是尽力而为；至少先加一个不可见丢弃的计数。
6. **每次追加后擦除派生密钥的栈副本**（F-B-002）：同意热路径多一次 64 字节的擦除即可。
7. **对账演示的去留**、**重连屏障世代 0 的收紧**（M5b-2 遗留，与本次审计无关，仍开放）。

## 4. 代码与文档的处置

PR [#238](https://github.com/EypeZeo/hengyuan_v2/pull/238)（分支 `fix/audit-2026-11-09-native-tier-a`，每个发现一个提交，提交信息带发现编号；合并提交待填）。

## 5. 核实中新发现、四份报告都没有写到的一条

**POST 回报的状态集**。`parse_submit_order_response()` 校验了 COID、品种、方向、类型、有效期、价格和原始数量之后，用 `map_binance_order_status()` 映射状态——这个函数认全部八种状态（`NEW`、`PARTIALLY_FILLED`、`FILLED`、`PENDING_CANCEL`、`CANCELED`、`REJECTED`、`EXPIRED`、`EXPIRED_IN_MATCH`）。`orchestrate_submit()` 的 Accepted 分支把这个状态原样写成持久帧、忽略 `OrderRecord::transition_to()` 的返回值，并在 `is_exchange_final()` 时释放在途槽。而 `validate_transition(Submitting, …)` 只允许 `Accepted / Rejected / Ambiguous / Filled / PartialFill`。结果：一份带 `EXPIRED_IN_MATCH`（自成交防护让吃单方到期）的 POST 回报会被写成 `Submitting → Expired` 的帧；追加一侧不校验转移，下次启动时 `run_recovery_scan_impl()`（`durable_audit_sink.hpp` 第 1059 行）把整份日志判为 Corrupt，进程起不来。现货默认的自成交防护模式下这不常见，但这条路径一旦有真实调用者，它就是一个“一次异常回报让进程不能重启”的缺口。与 F-A-012 同一个修法（只认 `NEW / PARTIALLY_FILLED / FILLED`，其余按未知结果走 Ambiguous 与查询），放进 TB-1。
