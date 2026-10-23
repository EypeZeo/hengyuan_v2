# ADR-018: Binance-First 独压与东京单机拓扑规范 — P2-GOV-03

## Status

**Accepted with modifications**（Architect / GPT-5.5 于 2026-06-27 裁决；下列修订已落地）。

> 2026-10-23 状态补充：文末「v2.9.0 目标设计补充」记录 Owner 批准的后续单活冷备方向；**设计已选定，尚未实现验收**。正文保留 2026-06 的历史决策与当时证据，不代表当前代码、凭据或部署状态。新机制验收并获运行授权前，继续执行现行人工恢复流程。

本 ADR 是范围/部署/风险姿态决策。**不构成** live 交易授权、Private API 接入授权、native 实盘下单授权。执行面（Phase 3）仍须独立的 ADR-010 后继 + 完整 L5 gate chain + 先 dry-run。由 Sonnet 4.6 据 owner 指示起草。

### 修订记录（Architect 裁决落地）

1. **D1 修订**：冻结 Kraken 后须从构建/部署/运行依赖排除；完全隔离前仍须处理影响当前系统的安全漏洞。
2. **D2 修订**："全系统单机"明确排除外部心跳/手机 App/本地客户端;`~2ms` 与具体 VPS SKU 仅为测量记录,不作为架构保证。
3. **D3.3 修订**：外部心跳监控为进入任何 L5 live 阶段的**前置条件**;主机自发邮件仅为辅助。监控实现至少按 L4 审查,不得触发自动恢复/auto-rearm/auto-resume/自动交易。
4. **D4 修订**：book 无效/depth 溢出/snapshot 重建期间必须 fail-closed,禁止产生新执行意图;SPSC 严格单生产者/单消费者,不得多 io_context worker 并发写入。
5. **D4/D5 归级修正**：synthetic benchmark = L1/L2;真实 Binance market WS/REST = L4;authenticated user-data stream = 至少 L4;Private API/submit/cancel/HMAC/kill execution = L5。
6. **Control Plane 修订**：Python/AI 提纯写入共享内存的内容只能是 advisory context,不得直接成为买卖信号/风险放行/order intent/执行参数。
7. **增加单机单所 live 前置条件**：no-withdrawal key、IP allowlist、账户真值 reconciliation、行情失效禁新开仓、风险/敞口硬上限、人工带外通道定期演练、部署前重新验证地域和服务可用性。

## Context

### owner 最终裁决（2026-06-27）

- 彻底放弃 A 股（已由 ADR-017 冻结记账类开发）。
- **彻底放弃 Kraken**：Kraken 仅通过模拟盘测试、从未上线 LIVE，无 live 历史包袱；其行情/parser/CRC32/contracts 全部代码作为沉没成本整体冻结/退役。
- **全系统独压 Binance**：单台亚太东京 VPS（Vultr / Linode 东京）直连 Binance 撮合核心，~2ms 级行情与交易网络延迟，物理避开欧洲地缘 IP 封禁（实测 London VPS 已被 Binance 封）。
- 散户成本姿态：**不承担多机多活容灾**，单点风险用人工带外通道兜底。

### 数据背景（诚实记录）

P2-PERF-LP-01 + 币安实测：同步 Python 栈在币安峰值 1581 evt/s 下 p99=15μs，mock 处理能力 27 万 evt/s。**native 不是吞吐逼出来的**——owner 选择 native 是为**确定性 μs 尾延迟 + 执行安全（无 GC 停顿）**的主动工程选择，并已接受技术栈沉没成本。本 ADR 如实记录此前提，供未来读者判断。

## Decision

### D1. Kraken 退役（Architect 修订 1）

- Kraken 相关代码（`app/infrastructure/marketdata/kraken_*`、`app/infrastructure/exchanges/kraken_*`）**无限期冻结**，不再维护、不再扩展、不进新执行链。
- 既有代码暂不物理删除（保留 git 历史与审计链），但标注 deprecated/frozen。
- **冻结后须从构建、部署和运行依赖中排除**（不能让冻结代码的依赖影响 Binance 主线）。在完全隔离前,仍须处理影响当前系统的安全漏洞——不得以"冻结"为由保留不受控依赖。
- Kraken 从未上线 LIVE，退役不影响任何 live 真值或资金。

### D2. 东京单机拓扑（Architect 修订 2）

- 交易核心系统（C++ Data Plane + 风控 + 数据落盘 +（可选）Python 提纯）闭环运行于**单台东京 VPS**。
- "全系统单机"的边界**明确排除**外部心跳监控、手机 Binance App、本地 Windows 客户端——它们是独立于交易主机的带外设施（见 D3）。
- 退租 London VPS。
- 配置基线：≥2 vCPU / 8 GB / NVMe SSD。实测延迟（如 ~2ms 到 Binance）和具体 VPS SKU **仅为测量记录,不作为架构保证**——VPS 厂商/SKU 变更后须重新测量验证。

### D3. 散户轻量化带外 Kill 守则（Architect 修订 3 + 修订 7）

```text
单点故障兜底 = 人工带外介入，不做主机认知恢复：

1. 主机一旦挂机 / 失联 / 异常 → 不尝试自动恢复持仓认知。
2. 带外撤单平仓通道（独立于交易主机）：
   - 手机 Binance App
   - 本地 Windows 端 Binance 客户端
   人工一键撤单 / 平仓，清理战场。
3. 告警层级（Architect 修订 3）：
   - 主通道：外部心跳监控（独立于交易主机的免费监控服务或备用机 cron ping 心跳端点，
     主机停止响应时主动告警 owner）。
   - 辅助通道：主机自身检测到异常时尝试发邮件（best-effort,硬崩溃时发不出）。
   - 外部心跳监控是进入任何 L5 live 阶段的前置条件。
   - 监控实现涉及网络出站,至少按 L4 审查。
   - 监控不得触发自动恢复 / auto-rearm / auto-resume / 自动交易（ADR-015）。
4. 持仓真值以 Binance 交易所侧为唯一真相；主机状态丢失 ≠ 持仓认知丢失（去 App 看）。
```

### D3-LIVE. 单机单所 live 前置条件（Architect 修订 7，L5 进入前必须满足）

```text
1. API key 禁提币（no-withdrawal）+ IP allowlist
2. 账户真值与 reconciliation（可从 Binance 侧重建）
3. 行情失效时禁止新开仓（fail-closed）
4. 风险/敞口硬上限（单仓 + 总仓）
5. 人工带外通道（手机/App）定期演练
6. 部署前重新验证地域和服务可用性（geo-block/IP 封禁复核）
7. 仅密钥 SSH + 防火墙 + fail2ban
```

> **风险审计留档（Opus）**：单机无带外自动 kill = 主机挂机到人工发现之间持仓裸奔。owner 已知情接受散户成本姿态。以上前置条件不可省略——它们是单点架构下最低安全底线。

### D4. 非对称解耦架构（Architect 修订 4 强化）

```text
拒绝 asyncio 进热路径。并发用 C++ 原生解决：

┌─ I/O 线程（Boost.Asio io_context，普通核）──┐    ┌─ 热线程（pinned 核，isolcpus）─┐
│ async_read 币安 WS（market + user-data）    │SPSC│ try_pop → OrderBook/指标/风控    │
│ → simdjson 扁平解析 → BinanceMarketEvent    │───▶│ → 执行意图（独立通道下单）        │
│ → ring.try_push（零业务逻辑、零分配）        │ring│ 永不碰 socket、无锁、确定性       │
└──────────────────────────────────────────────┘    └──────────────────────────────────┘

核心纪律（Architect 修订 4）：
- SPSC 严格单生产者 / 单消费者。不得让多个 io_context worker 线程并发写入同一个环。
  多连接 → 每连接一环,或确保一个 io_context 单线程 run()。
- OrderBook 无效 / depth delta 溢出 / snapshot 重建期间 → 必须 fail-closed,
  禁止在此状态下产生新执行意图。
- 边界即 SPSC 无锁环：多路异步 I/O 在 I/O 线程拿到，热路径单核纹丝不动。
- 背压分流：可 conflate 流（ticker/aggTrade）满→丢+计数；不可 conflate 流（depth delta）满→
  禁止静默丢，触发 REST 重拉 snapshot 重建（DeepSeek P0-1 铁律）。
```

### D5. native 落地分级（Architect 修订 5 归级修正）

| 活动 | 闸门级别 |
|------|----------|
| synthetic parser / ring / order-book benchmark（纯本地,无网络） | L1 / L2 |
| 真实 Binance **market** WebSocket / REST（公开,无鉴权） | **L4** |
| authenticated **user-data stream**（需 API key listenKey） | **至少 L4** |
| Private API / submit / cancel / HMAC 签名 / kill execution | **L5** |

- Control Plane（Python 提纯 → seqlock 共享内存 → C++ 只读）= L1。
  Python/AI 提纯写入共享内存的内容**只能是 advisory context**,不得直接成为买卖信号、风险放行、order intent 或执行参数（Architect 修订 6）。
- **Execution Plane = L5**,须 ADR-010 后继 + 完整 gate chain + 先 dry-run + D3-LIVE 前置条件。本 ADR **不**授权执行面实现。

## Consequences

### 正面

- 单一交易所、单机，固定成本最低，运维面最小。
- 物理贴近 Binance 撮合，~2ms 延迟，享最深流动性。
- 工程产能 100% 集中，无多所/多机分散。

### 负面 / 残留风险（owner 已知情接受）

- **单点故障**：主机/区域/IP 任一出事即全系统停摆，靠人工带外兜底（见 D3 残留风险标注）。
- **单交易所暴露**：Binance 监管不确定性 + geo 封禁风险集中于一家、一区。
- **沉没成本**：Kraken 全套基础设施作废。
- **native 维护成本**：跨语言、内存安全、跨架构内存序（P2-CORE-05）等长期负担，换确定性尾延迟。

## Non-Authorization

本 ADR **不构成**：live 交易 / dry-run 授权、Binance Private API 接入授权、native 实盘下单授权、密钥读取授权、ADR-010 闸门削弱。Phase 3 执行面须独立 L5 ADR + dry-run。native 实现须 accepted packet（见 P2-CORE-01-IMPL）。

## v2.9.0 目标设计补充（Owner 决定，2026-10-23）

本补充面向后续执行面与运维实现者；由账户 Owner 决定恢复权限边界，工程验收负责证明边界成立。实现接线、部署及授权范围变化时须重新复核。本次仅修订文档，未修改代码、开通冷备或执行任何鉴权操作。

### A1. 从单机人工兜底演进为单活＋自动围栏冷备

- 目标保持 **Binance-first、至多一个可发送交易请求的活动执行实例**；新增冷备，不采用多活。冷备允许无交易权限的准备工作，但不得与旧主同时持有可用发送能力。
- 仅 Owner 事前明确授权的基础设施故障可进入自动接管；故障类型未知、授权范围不匹配、证据缺失或过期均转人工，不靠心跳超时推断旧主已停止。
- 东京主节点仍是目标基线，冷备位置、服务可用性、网络路径和独立故障域须在部署验收中给出证据。每月 5 美元的 R2 备份规划预算不包含冷备计算成本，也不是采购授权。

### A2. 接管顺序与交易围栏证明

接管必须遵守下列先后关系；恢复过程任何失败均保持新发送关闭，保留诊断和带外告警：

1. 识别在预授权范围内的基础设施故障，冻结新主的交易发送；绑定旧主身份、账户、故障及恢复授权记录。
2. **先实际撤销或隔离旧主的交易发送能力，并取得独立、可审计且有时效的生效回执**。该证明必须覆盖旧主已有会话、可用凭据及重启后恢复发送的路径；控制面请求已受理不等于围栏已生效。
3. 围栏生效后，恢复完整审计链并与交易所对账，纳入围栏前已经发出的在途请求、未决订单、成交、余额与风险占用；没有结清的不确定状态不得交给新主继续交易。
4. 在围栏和对账证明有效的前提下，持久提交新的单调所有权 epoch，再通过 ADR-019 本日补充的全部恢复门；只在既有有效授权范围内恢复运行。

对象存储的条件写入/CAS、租约过期、进程 PID 消失、失联或新 epoch **本身均不是交易围栏**：交易所并不因这些本地状态拒绝旧主请求。R2 备份也不自动成为仲裁或围栏服务。无法验证旧主已失去发送能力时，即使新主健康、对账暂时一致，也禁止自动接管。

### A3. 自动恢复的有限边界

- 全部门禁通过后，预授权基础设施故障允许自动恢复，无须为同一次已授权恢复再追加人工 re-arm；该能力只在实现与验收完成后生效。
- **首次实盘仍由 Owner 本人操作**。人工 stop、损失/风险 kill、凭据疑似泄露、审计损坏、未解决订单或账务不确定均禁止自动解锁；不能把这些原因重新归类为普通基础设施故障。原因混合时取更严格的人工边界。
- 故障处置仅允许已授权且通过安全合同的撤单与隔离，不自动平仓。撤单仍可能与成交竞争，撤单请求成功或旧主停止均不代表持仓已清零；剩余持仓继续可见、计入风险并按人工 runbook 处理。
- 独立外部监控、人工带外通道与 D3-LIVE 七项完整保留；冷备不替代地域可用性、密钥、账户真值及风险限制，也不保证零停机或零资金损失。

### A4. 覆盖范围与生效证据

本补充仅在目标设计层面有限替代 D2 的「单机」和 D3 的「所有故障仅人工恢复／一律不得 auto-rearm」约束，以及对应历史残留风险描述；同时受 [ADR-019 的 v2.9.0 补充](ADR-019-binance-native-execution-plane-l5-readiness-and-safety-gates.md)约束。除此之外的历史规则不因本补充撤销。旧的禁止自动恢复规则在新机制完成验收、部署和 Owner 运行授权前仍然有效。

验收必须绑定实际构建、配置与故障环境，并保留以下证据；目前均为后续待验收要求：

- [ ] 网络分区、旧主假死后复活、已有会话及旧凭据重放均不能形成双发送者；失败的围栏请求不得伪装成生效证明。
- [ ] 围栏前在途下单及撤单/成交竞态纳入恢复对账，未决状态阻止恢复。
- [ ] 所有权提交中崩溃、重复接管、陈旧 epoch/回执和回滚副本均不能解锁发送。
- [ ] 人工 stop、风险 kill 和凭据/审计异常在重启与接管后仍保持锁定；授权撤销或过期阻止自动恢复。
- [ ] 自动恢复有有限重试及超时边界，耗尽后保持隔离并告警，不形成递归重启/交易循环；具体阈值须在启用前随合同和测试冻结。
- [ ] 本文与 ADR-019 的安全门、人工退出流程及独立监控一起通过演练，且不以 mock、文档批准或绿色 CI 代替部署证据。
