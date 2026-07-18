# ADR-018: Binance-First 独压与东京单机拓扑规范 — P2-GOV-03

## Status

**Accepted with modifications**（Architect / GPT-5.5 于 2026-06-27 裁决；下列修订已落地）。

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
