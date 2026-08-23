# Seal-journal / compaction 子系统冻结说明

**状态**：已冻结（owner 决策，2026-08-23）。代码全部保留、继续参与 CI，但**停止扩展**，并从
「走向实盘」这条关键路径上移出。

**这不是废弃通知。** 被冻结的代码质量是这个仓库里最高的一档——完整的 TLA+ 模型加负控、TSan/ASan
全绿、每一轮都有对抗性审查记录。冻结的理由跟质量无关，跟**优先级**有关：它解决的问题不是当前
部署形态会遇到的问题，而在它身上继续投入的时间，是从「这个系统还完全不会做交易决策」这个更大的
缺口上挪走的。

---

## 1. 冻结范围（精确文件清单）

### 1.1 冻结：不再扩展

| 类别 | 文件 | 行数 |
|---|---|---|
| Header | `compaction_breadcrumb_io.hpp`、`compaction_intent_codec.hpp`、`compaction_intent_gc_authorized_loader.hpp`、`compaction_intent_store.hpp`、`compaction_lease.hpp`、`intent_phase_advancer.hpp`、`migrated_v2_started_publisher.hpp`、`seal_export_migration_cleanup_abandon_codec.hpp`、`seal_id_watermark_export_started_loader.hpp`、`seal_journal_breadcrumb_precondition_aggregate.hpp`、`seal_journal_commit_tombstone_codec.hpp`、`seal_journal_commit_tombstone_loader.hpp`、`seal_journal_precondition_codec.hpp`、`seal_journal_store_io.hpp`、`seal_journal_store_lease.hpp`、`seal_started_abandon_loader.hpp`、`seal_started_migration_cleanup_loader.hpp`（17 个） | 8,208 |
| 测试 | `native/tests/` 下 21 个对应测试文件 | 9,509 |
| 只读诊断工具 | `native/src/seal_journal_cross_file_audit.cpp`、`native/src/seal_journal_store_dump.cpp` | 2,661 |
| TLA+ 模型 | `formal/RoundDActual.tla`、`RoundEFDesign.tla`、`RoundEFDesignReceiptVerified.tla`、`RoundELockOrder.tla`、`RoundERetryIdempotency.tla`（各自的 `.cfg` 与 `_bug.cfg` 负控一并保留） | — |
| Spec | `docs/BINANCE_PRIVATE_REST_L4_SPEC.md` **§10 整节**（2,449–5,258 行，占该 spec 的 53%） | 2,810 |

**合计约 20,400 行 C++ 被冻结。**

### 1.2 明确不冻结：仍在使用，且是实盘审计的落地方案

| 文件 | 行数 | 为什么留着 |
|---|---|---|
| `durable_audit_sink.hpp`、`durable_log_store.hpp`、`durable_frame_codec.hpp` | 2,050 | **这三个是真能工作的**：真 fsync、真 OS 独占锁、真 MAC 链式帧、真 `recovery_scan()`。批次 4/6 的审计落盘直接用它们，不用 seal journal |
| `durable_control_plane.hpp` | 2,470 | 纯 ABI 转写头（`:35`：*"SCOPE: this is an ABI surface, not an implementation"*）。它同时装着 seal-journal 结构体族**和**订单/审计路径要用的结构体，整体保留；其中 seal-journal 那一族随本说明冻结 |
| `intent_channel.hpp` | 35 | **不属于本子系统**。它是 strategy→executor 的 SPSC 通道（`:2`），路书批次 5 要用 |

其余 TLA+ 模型（`durable_log_recovery`、`depth_snapshot_bootstrap`、`key_rotation`、
`inflight_lifecycle`、`freeze_episode_recovery`）覆盖的是**没被冻结**的子系统，照常维护。

---

## 2. 冻结时的实现进度

`docs/SPEC_INVARIANTS.md` 的 ledger 记录了每一轮，标 `[已实现]` 的共 14 轮：

- **Round A/B/C**——`.xgc` 家族三轮设计（结构体族 + 常量 + `is_legal_cleanup_auth_flags()`）
- **Round D**——`CompactionCandidateIntent` codec/genesis（六版方案演进）
- **Round E Slice 1 / 2a / 2b**——三组 durable-precondition codec
- **Round E breadcrumb L2 loaders**——7 个类型的真实只读文件加载器
- **Round E receipt + `raise_intent_phase()`**——Building→Reserved，含 `SealIdWatermark` 推进、
  `SealJournalCommitWatermark` CREATE_NEW、两把锁协调
- **Round E Reserved→StartedPublished**（`raise_started_published()`）
- **Round E StartedPublished→{PostSealFinalizing, AbandonFinalizing}**——两条终边
- **Round E `.xgc` decode + loader**
- **Round E MigratedV2Started**——V+M companion 写路径（PR #47，冻结前最后一轮）
- **Round F / G**——两把锁顺序 TLA+ 模型、重试幂等 TLA+ 模型、`.jhw` 审计规则扩展、
  store 级诊断工具

**状态机四条边全部实现完毕**（Building→Reserved→StartedPublished→{PostSeal|Abandon}）。

**从未实现、冻结后也不打算实现**：`.xgc` 的 CREATE/写路径（只有 decode+loader）、真实的
C/A unlink、journal drain-completeness 检查、`TipExportProducerResume`、外部锚定
（`ExternalAnchorClient` 永远是 mock）、代际目录切换与压缩。

---

## 3. 为什么冻结

### 3.1 它一行都没接进生产路径，而且这是代码自己说的

`intent_phase_advancer.hpp:49`：

> "THIS ROUND IS TEST-ONLY. This class is not called from any production call path. A candidate
> that reaches Reserved via `raise_intent_phase()` today has NO follow-on code path in this
> repository that can clean it up ... calling this in a real environment permanently strands the
> candidate directory and its allocated ids"

同一个文件的结尾还写着：*"Do not wire any of these four functions into any real
compaction-trigger flow until real `.clr`/`.abd` authorization, `.xgc` CREATE/GC, and
TipExportProducerResume all exist."* ——这三个前置条件**一个都不存在**，且都不在计划中。

### 3.2 它解决的问题跟当前部署形态不匹配

ADR-018 定下的形态是：**单台东京 VPS、Binance-only、散户自有资金、无多节点 HA**。
L4 §10 设计的保证是拜占庭回滚抵抗、外部锚定、跨代压缩与 GC 授权——那是多副本、多写者、
需要向第三方证明「日志没被回滚过」的场景才需要的东西。单机单进程独占目录的场景下，
「进程崩溃后能正确恢复 in-flight 订单状态」就是全部真实需求，而这一条
`durable_audit_sink.hpp` + `durable_log_store.hpp` 已经能做到。

### 3.3 投入产出比已经明显失衡

- L4 spec 的 §10 一节 2,810 行，比该 spec 其余全部章节加起来还长，也比 `docs/` 里其他所有
  文档加起来还长。
- L5 spec 经历了 **72 轮修订、其中 12 次连续被判定 rejected**。
- 同期，`backtest`/`策略`/`回测`/`机器学习` 这些词在 `docs/` 全部文档里的命中数是 **0**；
  门禁阶梯止步于 L5，**没有任何文档描述「决定要下这一单」之前发生什么**。

v1 的 ADR-016 当年诊断过同一种病：*"本项目的病不是'文档太多'，而是'**仪式轮次太多**'"*。
v2 换了个形式复现了它——不是审批仪式，是规格轮次。冻结是对这个诊断的执行。

---

## 4. 为什么保留代码而不是删除

1. **它是有测试覆盖的资产**：9,509 行测试 + 5 个 TLA+ 模型（每个都配了必须失败的负控）。
   删掉等于丢弃这部分验证工作，重建成本远高于留着的维护成本。
2. **维护成本接近零**：全部是 header-only + 测试，不参与任何运行时路径，不拖慢构建以外的
   任何东西。CI 继续跑它们，等于免费保有一份回归保护。
3. **`durable_control_plane.hpp` 的 ABI 转写有独立价值**：它是 L4 spec 结构体布局的唯一
   机器可校验副本，`tools/spec_xref_check.py` 依赖它做 spec↔code 交叉核对。
4. **解冻条件是真实可能发生的**（见下一节），不是理论上的。

---

## 5. 解冻条件

出现下列**任何一条**时，重新评估本决定：

| 条件 | 为什么这会改变结论 |
|---|---|
| **多节点部署**（不再是 ADR-018 的单台东京 VPS） | 多写者出现，单机独占目录的前提消失，跨节点日志一致性成为真问题 |
| **托管他人资金** | 需要向第三方证明日志完整性/未被回滚，外部锚定从「过度设计」变成「合规要求」 |
| **监管审计要求** | 同上，且举证责任要求的正是 §10 那套可验证的链式结构 |
| **`durable_audit_sink.hpp` 被实盘证明不够用** | 如果实盘中出现它无法正确恢复的崩溃场景，说明单机需求被低估了，这是最直接的解冻信号 |
| **日志无界增长成为实际问题** | `SPEC_INVARIANTS.md:223` 已记录这个已知缺口（*"日志无限增长，这是已知缺口"*）。压缩是 seal journal 的本职工作，届时应当解冻而不是另起炉灶 |

解冻时的起点：`docs/BINANCE_PRIVATE_REST_L4_SPEC.md` §10 + `docs/SPEC_INVARIANTS.md` 的 14 条
`[已实现]` ledger 条目，两者共同构成完整的续接上下文，不需要考古。

---

## 6. 冻结期间的规则

1. **不新增**该范围内的功能、状态机边、codec 或 loader。
2. **可以修 bug**——如果 CI 变红，或发现真实缺陷，正常修复，不受冻结限制。
3. **可以做机械性维护**——编译器升级、工具链适配、跟随其他模块的 API 变更。
4. **测试继续在 CI 全量跑**，包括 TSan/ASan 档位与全部 TLA+ 负控。冻结的是开发，不是验证。
5. 任何想恢复扩展的提议，先对照第 5 节的解冻条件，并在 `docs/SPEC_INVARIANTS.md` 记录理由。

---

## 7. 相关文档

- `docs/BINANCE_PRIVATE_REST_L4_SPEC.md` §10——被冻结的规格本体
- `docs/SPEC_INVARIANTS.md`——14 条 `[已实现]` ledger 条目，冻结时的完整实现记录
- `docs/NATIVE_ARCHITECTURE.md`——门禁表与实现状态
- `native/include/hengyuan/intent_phase_advancer.hpp` 头注释——test-only 边界的第一手声明
