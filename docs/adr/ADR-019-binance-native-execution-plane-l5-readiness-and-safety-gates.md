# ADR-019：Binance Native Execution Plane L5 Readiness and Safety Gates — P2-GOV-04

## Status

**Accepted with modifications**（Architect / GPT-5.5 于 2026-06-28 裁决；下列 9 项修订已落地）。

本 ADR 是 ADR-010 的**执行面后继**，专门冻结 native（C++ / 东京单机 / Binance 直连）执行面进入 L5 实盘下单前的安全闸门。ADR-018 D5 与 Non-Authorization 明确点名"Execution Plane = L5，须 ADR-010 后继 + 完整 gate chain + 先 dry-run + D3-LIVE 前置条件"——本 ADR 即为该缺失的后继。

本 ADR **不构成**：live 交易授权、Binance Private API 接入授权、native 实盘下单授权、`.env` 密钥读取运行授权、任何既有闸门削弱。它只冻结"在什么前置条件全部满足之前，不得进入实盘"的边界，并把后续工作拆成更窄的 future packet。

### Architect 修订记录（GPT-5.5 裁决落地）

1. **M1 修正活动归级**：通用 key-value 解析器配合合成数据 = L1；读取真实 `.env` 凭据 = L3。签名器使用合成密钥 = L1，加载真实密钥 = L3，签名只读请求 = L4，签名下单 = L5。按 ADR-016 实际能力归级。
2. **M2 拆分两套清单**：ADR-018 D3-LIVE 七项与 native preflight 七项是不同清单，必须分别命名、逐项满足，不能相互替代。
3. **M3 禁止 preflight 自报**：外部监控须提供带时效的独立健康证据；regression 须绑定受审构建制品和版本；CONFIRM 须绑定不可变订单摘要、账户标识、最大名义金额和有效期；signer readiness 不得只检查"已初始化"。
4. **M4 补齐 secret lifecycle gate**：严格变量名 allowlist、文件权限、禁 symlink/路径漂移、内存清零、日志与错误脱敏、轮换、撤销和责任人。真实 secret 不得进入参数、core dump、共享内存或子进程环境。
5. **M5 补齐 L4 transport gate**：TLS/证书/hostname 验证、endpoint allowlist、禁重定向、超时、rate limit、时钟偏差、响应大小和 schema 校验、错误信息脱敏。
6. **M6 修正网络超时语义**：`POST /order` 超时不能只"立即停止"——可能已被交易所接受。须进入 `ambiguous` 态，禁止盲目重试，通过同一 `newClientOrderId` 查询和 reconciliation 收敛。
7. **M7 补齐订单合法性与风险输入**：首次提交前须验证 symbol 状态、价格/数量步长、最小名义金额、订单类型、余额 freshness、单笔及总敞口上限。首条路径只支持一个明确冻结的订单类型。
8. **M8 增加 Binance exit-safety 独立 packet**：D12 须含独立 exit-safety / kill-switch 合同 packet（emergency cancel、人工接管、kill 后允许哪些只读/退出动作），不可用 Kraken 或 simulation skeleton 替代。
9. **M9 重排 D12**：顺序为 secret/config boundary → L3 本地加载 → L4 transport contract → account-truth contract 及探针 → lifecycle/idempotency/reconciliation → exit safety/kill → audit/DB → dry-run evidence → L5 submit。

- 日期：2026-06-28
- 关联任务：`P2-GOV-04`；落地后继 spec packet `docs/task-packets/P2-EXEC-LIVE-01-binance-native-first-live-order-path-spec-packet.md`
- 关联 ADR：`ADR-010`（crypto spot live readiness 母 ADR）、`ADR-018`（Binance-first 东京单机拓扑 + D3-LIVE 前置条件）、`ADR-003`（Strategy/Risk/Execution 分离）、`ADR-016`（L0–L5 风险闸门矩阵）、`ADR-015`（禁 auto-rearm/递归硬顶）

## Context

### 既有事实（已核实，截至 2026-06-28）

native 目录已落地一批 **SIMULATION ONLY** 的执行面基础设施，但全部明确标注"不授权实盘提交"：

- `native/include/hengyuan/binance_signer.hpp`：HMAC-SHA256 签名器，头注释原文 *"SIMULATION INFRASTRUCTURE — NOT LIVE READY... Actual private API submission requires explicit L5 authorization + D3-LIVE completion."* 只产出签名，**不提交**。
- `native/include/hengyuan/preflight_gate.hpp`：D3-LIVE 7 项 preflight checklist，注释原文 *"Does NOT enable live by itself... SIMULATION ONLY until D3-LIVE is fully satisfied."*
- `sim_executor.hpp` / `kill_switch.hpp` / `risk_gate.hpp` / `shm_heartbeat.hpp` / `intent_channel.hpp`：模拟撮合、Kill Switch 状态机、pre-trade 风控、SHM 看门狗心跳、SPSC 意图通道。
- `.env` 已在 VPS 配置（API Key + Secret，IP allowlist，只读+现货，禁提现），API Key 验证通过（Account type=SPOT, Can trade=True）。

### 关键缺口

所有已落地的**订单生命周期治理基础设施**（幂等、reconciliation、ambiguous submit 恢复、kill-switch re-arm 仪式、audit 写面、post-submit 对账）都是为 **Kraken** 建的（P2-113~182 族系），且 Kraken 已由 ADR-018 D1 整体冻结退役。**Binance native 执行面目前没有**：

- Binance 侧订单生命周期 / client order id 幂等规则的落地实现；
- Binance 侧 post-submit query / reconciliation；
- Binance 实盘尝试 / 拒单 / kill-switch 触发的 append-only audit 写面；
- 实盘 account truth（余额/持仓/冻结量/freshness）真值源的 native 落地；
- dry-run-before-live 的回放/对账证据链；
- secret lifecycle 管理（轮换 / 撤销 / 脱敏 / 权限控制）；
- L4 transport 安全合同（TLS / endpoint allowlist / 超时 / schema 校验）；
- 订单合法性 pre-trade 校验（symbol 状态 / 步长 / 最小名义 / 订单类型冻结）；
- Binance 侧独立的 exit-safety / emergency cancel / 人工接管合同。

因此，"基础设施大体齐了"指的是**密码学层 + 模拟层 + 心跳/风控/Kill 骨架**齐了，而**订单真值闭环未建**。在缺口未补齐前直接接 `POST /api/v3/order` 实盘，会一次性把 secret 运行时、账户真值、订单状态机、对账与审计糊进一个过宽的实现，正是 ADR-010 第 7 条与 ADR-018 反复拒绝的 omnibus live。

### owner 治理约定（已核实记忆事实）

> owner 直接授权落 L1/Proposed，**但 L5 live 仍须先 dry-run**。

本 ADR 据此把 dry-run-before-live 冻结为 native 执行面的硬前置条件，不因 owner 已配置真实 key 而豁免。

## Decision

### D1. Binance native 执行面进入 L5 是 staged gate chain，不是 `--live` 翻转

继承 ADR-010 第 1 条。`--live` 命令行参数 + 手动 `CONFIRM` 输入 + preflight 7/7 PASS **三者合并仍不等于** live-ready。进入实盘提交必须额外满足本 ADR D3–D11 的全部前置条件，且每个高风险子问题须单独 packet、单独审查、单独留痕。

> `--live` 可被接受 ≠ 系统已 live-ready。`ENABLE_LIVE_TRADING` / preflight 7/7 / API Key `Can trade=True` 均非 live 授权证明。

### D2. native 活动分级（接 ADR-018 D5 + Architect M1 归级修正）

按 ADR-016 实际能力归级。同一组件在不同使用姿态下级别不同：

| 活动 | 闸门级别 | 现状 |
|------|----------|------|
| 通用 key=value 解析器 + 合成测试数据（无真实 secret） | **L1** | 可在 spec 接受后落地 |
| 读取真实 `.env` 凭据（加载 API key / secret 到进程内存） | **L3** | 须 secret lifecycle gate (D4) 通过 + L3 审查 |
| HMAC 签名器 + 合成密钥（无真实 secret，无出站） | **L1** | 已存在（合成测试姿态），仅密码学层 |
| HMAC 签名器 + 真实密钥加载 | **L3** | 须 D4 secret lifecycle gate |
| 签名只读请求 `GET /api/v3/account`（鉴权出站，**无下单副作用**） | **L4** | 须 D4 + D5 transport gate + L4 审查 |
| 签名下单请求 `POST /api/v3/order`（实盘写副作用） | **L5** | **本 ADR 不授权**；须 D3–D11 全满足 + 先 dry-run + L5 审 |
| `--live` 激活 + 手动 CONFIRM 下单路径 | **L5** | 同上 |

低级别件的存在**不解锁**高级别写通道。L1 解析器 + 合成测试不解锁 L3 真实加载；L3 加载不解锁 L4 出站；L4 只读不解锁 L5 下单。

### D3. 双清单制度 + 禁自报（Architect M2 + M3）

ADR-018 D3-LIVE 七项与 `preflight_gate.hpp` native preflight 七项是**两套不同清单**，必须分别命名、逐项满足，**不能相互替代**。

**清单 A — ADR-018 D3-LIVE 部署前置条件**（运维/合规层面，L5 进入前必须满足）：

1. API key 禁提币（no-withdrawal）+ IP allowlist
2. 账户真值与 reconciliation（可从 Binance 侧重建）
3. 行情失效时禁止新开仓（fail-closed）
4. 风险/敞口硬上限（单仓 + 总仓）
5. 人工带外通道（手机/App）定期演练
6. 部署前重新验证地域和服务可用性（geo-block/IP 封禁复核）
7. 仅密钥 SSH + 防火墙 + fail2ban

**清单 B — native preflight 运行时检查**（每次启动/每次下单前实时校验）：

1. Kill switch 状态机处于 Normal
2. Pre-trade risk gate 已配置（单仓 + 总仓硬上限，接 ADR-016）
3. Depth snapshot 已同步（Tracking 态）
4. 外部心跳（watchdog，独立于交易主机）活跃
5. HMAC 签名层就绪
6. Operator 手动确认（typed CONFIRM）
7. Regression test 认证通过

两套清单必须**各自 7/7 全 PASS**，任一项 FAIL → fail-closed，停留模拟态。

**禁止 preflight 自报 PASS**（Architect M3）：

- **外部心跳（B4）**：不得仅以 SHM 指针存在判断。须由外部监控服务提供**带时效**的独立健康证据（含时间戳、过期阈值），过期或不可达 → FAIL。
- **Regression（B7）**：不得由外部布尔值直接置位。须绑定**受审构建制品和版本**（commit hash / build artifact / test report 引用），与当前运行二进制一致性校验。
- **Operator CONFIRM（B6）**：不得为空白确认。须绑定**不可变订单摘要**（symbol、side、order type、quantity、price）、**账户标识**、**最大名义金额**和**有效期**，超期或摘要不匹配 → 作废。
- **Signer readiness（B5）**：不得仅检查 `is_initialized()` 布尔。须以实际合成 payload 签名验证密码学路径完整可用。

### D4. Secret lifecycle gate（Architect M4，新增前置条件）

接 ADR-010 第 2 条 + SECURITY_BASELINE。真实 `.env` 凭据加载（L3）前须满足：

- **变量名 allowlist**：只允许声明的 key 被读取（如 `BINANCE_API_KEY`、`BINANCE_API_SECRET`），拒绝未知变量名。
- **文件权限**：`.env` 文件仅 owner 可读（`chmod 600` / POSIX ACL 等效），启动时校验权限，权限过宽 → 拒绝加载。
- **禁 symlink / 路径漂移**：`.env` 路径必须为预设绝对路径，不得跟随符号链接，不得从命令行参数/环境变量传入路径。
- **内存清零**：secret 使用完毕后必须 `memset` / `explicit_bzero` 清零（signer 已有 `destroy()` 清零，读取器同理）。
- **日志与错误脱敏**：secret 不得出现在 `stdout` / `stderr` / 日志文件 / error message / stack trace / core dump 中。签名失败的错误信息只报"签名失败"，不泄露密钥片段。
- **禁入共享内存 / 子进程环境**：secret 不得写入 SHM、不得通过 `setenv` / `fork+exec` 传播到子进程，不得作为命令行参数传递。
- **轮换与撤销**：secret lifecycle 必须有责任人（owner），轮换/撤销流程须有文档化的 runbook（可在 future packet 中建立，但 L3 加载前至少须有责任人指定）。

未通过 D4 gate 时，不得从 `.env` 加载真实 API key / secret。

### D5. L4 transport gate（Architect M5，新增前置条件）

签名只读请求（`GET /api/v3/account`，L4）及后续所有鉴权出站前须冻结：

- **TLS / 证书 / hostname 验证**：必须验证 Binance endpoint 的 TLS 证书链和 hostname，禁止 `verify=false` 或自签证书。
- **Endpoint allowlist**：仅允许连接预设的 Binance API endpoint（如 `https://api.binance.com`），拒绝重定向到非 allowlist 目标。
- **禁止 HTTP 重定向跟随**：鉴权请求不得自动跟随 3xx 重定向，重定向 → 报错。
- **超时**：连接超时 + 读取超时必须明确设置且合理（秒级），不得无限等待。
- **Rate limit**：必须尊重 Binance 的 request weight / order rate limit，本地计数，接近限额 → 主动降速或拒绝。
- **时钟偏差**：签名中的 `timestamp` + `recvWindow` 须基于可靠时钟源，偏差过大 → 报错而非静默重试。
- **响应大小与 schema 校验**：限制最大响应体大小，校验响应 JSON 结构符合预期 schema，异常结构 → 报错。
- **错误信息脱敏**：Binance 返回的错误响应不得被完整写入对外日志（可能含 IP / key 片段），须脱敏后记录。

未通过 D5 gate 时，不得发起任何 Binance 鉴权 HTTP 请求。

### D6. 实盘提交必须依赖明确的 Binance account truth（接 ADR-010 第 3 条）

实盘下单前必须有明确的账户真值源：余额、可用资金、持仓、冻结量、资产映射、freshness 规则须先定义并落地。simulated balance / operator 手输 / 过时快照不得替代实盘真值。account sync 缺失/过期/失败/不可信时 → fail-closed。ADR-018 D3 第 4 条：持仓真值以 Binance 交易所侧为唯一真相。

`GET /api/v3/account` 是 account-truth gate 的**一个输入**，但一次成功响应不能直接解锁下单——须经 freshness 校验、交易规则校验（D7）和 reconciliation 约束后方可作为下单决策依据。

### D7. 订单合法性与 pre-trade 风险输入（Architect M7，新增前置条件）

首次实盘提交前，必须建立 pre-trade 合法性验证层：

- **Symbol 状态**：验证目标 symbol 当前在 Binance 处于 TRADING 状态，非 HALT / BREAK。
- **价格 / 数量步长**：验证 price 与 quantity 符合 Binance `exchangeInfo` 中的 `tickSize` / `stepSize` / `LOT_SIZE` / `PRICE_FILTER`。
- **最小名义金额**：验证订单名义金额 ≥ `MIN_NOTIONAL` filter。
- **订单类型冻结**：首条路径**只支持一个明确冻结的订单类型**（如 LIMIT），不得在首条路径中支持 MARKET / STOP / OCO 等多类型。
- **余额 freshness**：下单前 account truth 必须在定义的 freshness 窗口内（如最近 N 秒），过期 → 重拉或 fail-closed。
- **单笔敞口上限**：单笔订单名义金额不得超过预设硬上限。
- **总敞口上限**：当前持仓 + 待成交订单 + 本次意图的总名义敞口不得超过预设硬上限。

任一校验失败 → 拒绝生成 order intent，fail-closed。

### D8. Order lifecycle / idempotency / reconciliation 必须先于 live submit（接 ADR-010 第 4 条 + Architect M6）

Binance native 实盘提交不得只定义成功路径。须先落地：

- order intent → submit attempt → accepted → rejected → partial fill → filled → cancel requested → cancelled → expired → reconciled 的生命周期边界；
- `newClientOrderId` 幂等键规则（防重提交 / 重复 / 乱序事件 / 断线恢复 / reconciliation 语义）；
- operator 可见的状态解释与失败恢复路径。

**网络超时语义修正（Architect M6）**：`POST /api/v3/order` 超时**不能**只"立即停止并丢弃"。超时时订单可能已被交易所接受。超时后必须：

1. 进入 **ambiguous** 状态（既非成功也非失败）；
2. **禁止盲目重试**（可能导致重复下单）；
3. 通过同一 `newClientOrderId` 调用 `GET /api/v3/order` 查询实际状态；
4. 通过 reconciliation 流程收敛到确定状态（accepted / rejected / unknown-escalate）；
5. unknown-escalate → kill-switch 触发或 operator 人工接管，不得自动化处理。

这些目前 Binance 侧**不存在**，须作为本 ADR 之后的独立窄 packet 建立。未建立前不得开 `POST /order` 实现。

### D9. Kill switch、exit safety 与 fail-closed 是前置而非后补（接 ADR-010 第 5 条 + ADR-015 + Architect M8）

- Kill switch 是 operator-visible、可审计、可验证的第一类控制面；
- 签名失败 / account sync 失效 / 行情失效 / order-state 不一致 → 立即 fail-closed，禁止 best-effort 续跑；
- **kill switch 触发后禁止自动恢复 / auto-rearm / auto-resume / auto-loop**（ADR-015 + ADR-018 D3 修订 3）；re-arm 须独立人工仪式 packet；
- 行情失效（book 无效 / depth 溢出 / snapshot 重建）期间禁止产生新执行意图（ADR-018 D4）；
- `POST /order` 超时后须允许只读 reconciliation 与人工接管，不得简单退出进程遗失订单真值（接 D8 M6 ambiguous 语义）。

**Binance exit-safety 须独立 packet（Architect M8）**：D12 future packet 列表必须包含独立的 exit-safety / kill-switch 合同 packet，明确定义：

- emergency cancel 机制（在 kill-switch 触发后如何撤单）；
- 人工接管边界（kill 后 operator 允许哪些只读查询和退出动作，禁止哪些操作）；
- kill 后残留持仓处理流程（不能由程序自动平仓，但须有 operator runbook）。

不可用 Kraken 已退役的 lifecycle 或 simulation skeleton 替代。

### D10. Audit / DB separation / rollback 必须提前冻结（接 ADR-010 第 6 条）

- 实盘尝试、风控审批/拒绝、kill switch 触发、关键状态转换必须可审计、append-only；
- dry-run 与 live 的业务状态/事件/配置/运行痕迹不得糊成一条模糊数据流；
- 若需新增 live 业务表 / 状态表 / reconciliation 表 / ledger，须独立 migration + DB review packet；
- secret 不得进入仓库、文档、run note、review note、日志、截图、`system_settings`、测试 fixture（接 ADR-010 第 2 条 + SECURITY_BASELINE）；
- **审计写入失败时不得提交新订单**：如果 audit 写面不可用（DB 不可达、写失败），系统必须 fail-closed，禁止在审计盲区内继续提交。

### D11. dry-run-before-live 是硬前置条件，不可因已配真实 key 豁免

接 owner 治理约定。Binance native 执行面在首次实盘提交前，必须先有 dry-run 回放/对账证据：模拟提交路径、拒单路径、ambiguity 路径、kill-switch 触发路径均须有可审证据。`sim_executor` 的模拟撮合不等于 dry-run-before-live 证据链——后者须针对实盘 endpoint 形态做回放验证。

### D12. Future work 必须拆为更窄 packet，禁止 omnibus（接 ADR-010 第 7 条 + Architect M8 + M9）

本 ADR 接受后，Binance native 执行面工作至少拆为以下有序 packet，**不得跳 gate**：

1. **Secret / config boundary**（L1 合成解析器 + secret lifecycle 合同冻结，接 D4）
2. **L3 本地真实凭据加载**（真实 `.env` 加载 + D4 gate 全满足 + L3 审查）
3. **L4 transport contract**（TLS / endpoint allowlist / 超时 / schema / 脱敏冻结，接 D5）
4. **Account-truth contract 及探针**（`GET /api/v3/account` 签名只读 + freshness + 交易规则校验，接 D6 + D7；一次成功不解锁下单）
5. **Order lifecycle / idempotency / reconciliation**（含 ambiguous 超时语义，接 D8）
6. **Exit-safety / kill-switch 合同**（independent packet：emergency cancel、人工接管、kill 后只读/退出边界、残留持仓 runbook，接 D9 M8；**不可用 Kraken 或 simulation skeleton 替代**）
7. **Audit / DB separation**（实盘审计写面 + 审计不可用 → fail-closed，接 D10；如需 live 表则独立 migration + DB review）
8. **dry-run-before-live 回放/对账证据链**（接 D11）
9. **最小 live submit path**（仅在 1–8 全部接受后，`POST /order` 单路径、fail-closed、CONFIRM-gated、单一冻结订单类型、单笔+总敞口硬上限）

每个 packet 可独立审查、独立验证。接受本 ADR 仅冻结规格，**不自动授权任何 code-bearing L3/L4/L5 工作**——每个 code-bearing packet 须单独取得对应级别的明示授权。

## Consequences

### 正面

- 把"是否想做 Binance 实盘"转成"是否满足 gate chain"；
- 补齐 ADR-018 D5 点名缺失的 ADR-010 执行面后继，治理链闭合；
- 降低 secret 运行时 / 账户真值 / 订单状态机 / 审计被一次性糊成一团的风险；
- 保留 dry-run-before-live、kill switch、append-only audit、Strategy/Risk/Execution 分层；
- 按实际能力归级（L1/L3/L4/L5），避免把合成测试与真实凭据混为同一级别；
- 双清单制度 + 禁自报，堵住 preflight 形式化通过的风险。

### 负面 / 残留风险（owner 已知情接受散户成本姿态）

- Binance 实盘推进被拆成更多窄 packet（9 个），速度更慢；
- 已配真实 key + `Can trade=True` 不能直接进 live submit，须先补订单真值闭环；
- 单机无带外自动 kill：主机挂机到人工发现之间持仓裸奔（ADR-018 D3 已留档），本 ADR 不改变该残留风险，仅要求外部心跳为 L5 前置；
- 提交超时后系统处于 ambiguous 态，须人工或 reconciliation 收敛——散户场景下 reconciliation 延迟可能较长。

## Non-Authorization

本 ADR **不构成**：live 交易 / dry-run 授权、Binance Private API 接入授权、native 实盘下单 / 撤单授权、`.env` 真实密钥读取运行授权、ADR-010 / ADR-015 / ADR-016 / ADR-018 任何闸门削弱。`POST /api/v3/order` 实现须本 ADR 被 Architect 接受 + D3–D11 全满足 + 先 dry-run + L5 审查（Architect + Opus 4.8 Senior Reviewer）。任何分支 / commit / PR / preflight 7/7 / `Can trade=True` 均非实盘授权或 rearm/resume 授权。接受本 ADR 仅冻结规格，不自动授权任何 code-bearing L3/L4/L5 工作。

## 与既有 ADR 的关系

- **后继 `ADR-010`**：把母 ADR 的 7 条 live-readiness gate 落到 native / Binance / 东京单机的具体执行面，不替代、不削弱。一致性核实：Gate 1 staged readiness → D1；Gate 2 secret isolation → D4；Gate 3 account truth → D6+D7；Gate 4 lifecycle/reconciliation → D8；Gate 5 kill/exit safety → D9；Gate 6 audit/DB/rollback → D10；Gate 7 narrow sequencing → D12。
- **后继 `ADR-018`**：填补其 D5 与 Non-Authorization 明确要求的"ADR-010 后继"。D3-LIVE 七项作为清单 A 独立保留，不与 native preflight（清单 B）混同。
- 补充 `ADR-003`：Strategy/Risk/Execution 分离在 native 实盘路径上依然成立，提纯写入 SHM 只能是 advisory context。
- 补充 `ADR-016`：活动按实际能力归级（M1），L1/L3/L4/L5 分线清晰。
- 沿用 `ADR-015`：禁 auto-rearm / auto-resume / auto-loop / 递归无界。
