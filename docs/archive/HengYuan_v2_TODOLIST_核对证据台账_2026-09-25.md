# HengYuan v2 规划文档（v2.5.6 → v2.6.0）核对证据台账

> **用途**：本文档是 [HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.md](../HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.md) v2.6.0 全部更正的**可复核底账**。每个更正经条目在本台账中给出：核验方法、命令/文件位置、原始输出要点。凡本台账未列证据的断言，v2.6.0 正文均按 `UNVERIFIED` 处理。
> **核对时间**：2026-09-25
> **核对对象**：本地 `D:\My_Projects\hengyuan_v2`（worktree HEAD `f4ee620`）+ 远端 `origin`（GitHub，private）+ 兄弟 worktree `hengyuan_v2_6b0e` / `hengyuan_v2_6c`
> **原件备份**：`docs/archive/HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.orig-9d78643d.md`（SHA256 `9d78643df8e32a7f3efa3a7d79de4f0eac17ba8a5c0d37c1eaa9283d16c93965`，86044 字节，与用户提供的附件逐字节一致）

---

## 0. 核对方法与工具边界

| 证据类型 | 获取方式 | 说明 |
| :--- | :--- | :--- |
| 代码事实 | `read` / `grep` 工具逐文件读取；`git grep` 统计调用点 | 全部给出 `file:line` |
| 版本历史 | `git log origin/master`、`git show --stat <sha>`、`git rev-list --count` | 本地镜像 + 远端已抓取引用 |
| PR 元数据 | `gh pr list --state all`、`gh api repos/.../pulls/<n>`、`gh api .../pulls/<n>/files` | 需 `GH_TOKEN`（本机已存于**用户级**环境变量；harness 进程未继承，需显式 `$env:GH_TOKEN = [Environment]::GetEnvironmentVariable("GH_TOKEN","User")`） |
| CI 运行 | `gh api repos/.../actions/runs?head_sha=<sha>`、`.../runs/<id>/jobs` | 含每步状态，可区分“真跑完”与“被取消” |
| 平台能力 | `gh api repos/.../branches/master/protection`、`.../rulesets`、`.../code-scanning/default-setup` | 返回 403 亦为证据（记录其 message） |
| 不可核验项 | 截图、口头进度、外部厂商性能数字 | 一律标注，不作状态依据 |

**环境已知限制**：
- 仓库为 **private**，未认证的 `api.github.com` / `github.com` 请求返回 404（这是私有仓库的正常表现，不是仓库不存在）。
- `gh` 默认未登录；本机 `GH_TOKEN` 存于用户级环境变量，需显式注入命令环境。

---

## 1. 基线与版本事实

| 编号 | 结论 | 证据 |
| :--- | :--- | :--- |
| B-01 | `origin/master` = `f4ee620`，提交信息 `feat(l4): batch6-6b0f9 … (#107)`，时间 `2026-09-25 12:20:38 +0800` | `git log -1 origin/master` |
| B-02 | `f4ee620` 就是 PR #107 的合并提交（squash 合并，提交信息尾带 `(#107)`） | `gh pr list --state all` → `107 MERGED 2026-09-25T04:20:38Z f4ee620` |
| B-03 | 本地 `master` = `3278180`（#92），落后 `origin/master` **15** 个提交 | `git rev-list --count 3278180..origin/master` → `15` |
| B-04 | 恰好存在 **3 个 git worktree**，基点均为 `f4ee620`：`hengyuan_v2`(6b-0b) / `hengyuan_v2_6b0e` / `hengyuan_v2_6c` | `git worktree list` |
| B-05 | 三个 worktree 分支**均无远端分支、均无 PR（任意状态）** | `git ls-remote --heads origin <branch>` 为空；`gh pr list --state all --head <branch>` 为空 |
| B-06 | 当前开放 PR 数 = **0** | `gh pr list --state open` 输出为空 |
| B-07 | 合并区间为 #70–#107（含 #73/#74/#78/#80–#91 等原文未列项）；PR #13–#37 时代为真 merge commit，其后改为 squash | `git log --oneline -45 origin/master`；`git log --merges --oneline -10` |
| B-08 | 仓库为 **private**，`allow_merge_commit` 与 `allow_squash_merge` 均开启，默认分支 `master` | `gh repo view … --json visibility`；`gh api repos/...` |

---

## 2. GitHub 侧核验（对应正文 E2/E4/…）

### 2.1 f4ee620 上的 CI

```
gh api "repos/EypeZeo/hengyuan_v2/actions/runs?head_sha=f4ee6208005d7cedc39f6600ab8ce4fbf81868e1"
→ total_count: 3
   36094061073  Main Merge Guard        push  completed/success  09/25/2026 04:20:41
   36094061053  CI Spec Verification    push  completed/success  09/25/2026 04:20:41
   36094061061  CI Native               push  completed/success  09/25/2026 04:20:41
```

- **正文三个 run id 与 workflow 名对应正确**（原文写作“CI Native / CI Spec Verification / Main Merge Guard”，无误）。
- **补充事实（E21）**：当次 push **只跑了 3 个 workflow**。`ci-python`（路径过滤未命中）、`ci-native-sanitizers`（仅 schedule/dispatch）、`codeql`（仅 schedule/dispatch）**均未运行**。故“该 SHA CI 全绿”只能覆盖这三条通道。

### 2.2 分支保护 / 必需检查（E2）

```
gh api repos/EypeZeo/hengyuan_v2/branches/master/protection
→ 403 {"message":"Upgrade to GitHub Pro or make this repository public to enable this feature."}
gh api repos/EypeZeo/hengyuan_v2/rulesets
→ 403 同上
```

**结论**：这是**平台能力缺失**，不是“配置不可见”。原文 `REM-0.1①③`、`GATE-01` 中把 branch-protection 必需检查当作可获得证据的写法必须改写；`GATE-01` 不应把该项记为 `UNVERIFIED`（暗示“还没查”），而应记为“平台不支持，需 Owner 决策”。

### 2.3 Sanitizer 周更实际状态（E3）——本次最重要的发现之一

```
gh run list --workflow=ci-native-sanitizers.yml --limit 8
35556436339  schedule  completed/cancelled  master  2026-09-21
34801501343  schedule  completed/cancelled  master  2026-09-14
34078573391  schedule  completed/cancelled  master  2026-09-07
33352827054  schedule  completed/success    master  2026-08-31

gh api "repos/EypeZeo/hengyuan_v2/actions/runs/35556436339/jobs"
  ASan+UBSan: full suite    completed/cancelled  03:07:37 → 03:52:59   (45m22s)
  TSan: …negative control   completed/success    03:07:37 → 03:09:11
  ARM64 Release: …          completed/success    03:07:40 → 03:08:59
  步骤级：Configure(ASan+UBSan) completed/success → Build completed/cancelled → Test completed/skipped
```

- `.github/workflows/ci-native-sanitizers.yml:41` → `timeout-minutes: 45`（ASan 作业）。
- 连续三周的取消点完全一致（≈45 分钟、卡在 `Build`），且 `Test` 步 `skipped` ⇒ **ASan+UBSan 全量套件自 2026-08-31 之后一次都没有真正执行**。
- 对照：08-31 次 `Build+Test` 合计约 21 分钟（03:07:56 → 03:28:53），说明是构建规模增长撞上了写死的 45 分钟上限，而非偶发。
- TSan 与 ARM64 两个作业在三次被取消的 run 中**均为 success**（各耗时 1–2 分钟）——门禁不是全废，但最耗时的那条腿断了。

### 2.4 CodeQL 空绿（E22）

- `gh api repos/EypeZeo/hengyuan_v2/code-scanning/default-setup` → `403 Code scanning is not enabled for this repository.`
- `gh run list --workflow=codeql.yml` → 每周 schedule `completed/success`。
- `.github/workflows/codeql.yml`：`:23-27` 为 `if: ${{ github.event.repository.private }}` 的“跳过”步骤；`:30/:36/:45/:52/:59` 全部实质步骤都带 `if: ${{ !github.event.repository.private }}`。仓库确为 private ⇒ 每周作业只执行了一条 echo，却报 success。

### 2.5 merge-guard 语义（E6/E23）

- 读全文 `.github/workflows/main-merge-guard.yml`（235 行）：
  - `:145-156` 注释明确：“rollup 为 null = 该 PR 变更路径没命中任何 workflow 的 `paths:` 过滤，这是**预期且有意**的状态，不是还在跑”。
  - `:169-171`：`if (finalState === "SUCCESS" || rollupIsNull) { mode = "ok"; reason = rollupIsNull ? "pr_no_checks_required" : "pr_checks_success"; }` ⇒ **零检查 = 通过（fail-open）**。
  - `:221-229`：处置为 `git revert [-m 1]` + `git commit --amend` + **`git push origin HEAD:master`**（普通前向推送，**不是 force-push**）。
- ⇒ 原文 `REM-0.1③` “验证 merge-guard 在零匹配检查时 fail closed”与代码相反；消除盲区的正确手段是新增无 `paths:` 的 catch-all workflow，让 rollup 非 null。

### 2.6 路径过滤与 catch-all 现状（E20/E41 相关）

`grep -n "^\s+paths:" .github/workflows/*.yml` ⇒ 仅 6 处：

```
ci-native.yml:9,  ci-native.yml:12      (push / pull_request)
ci-python.yml:23, ci-python.yml:28      (push / pull_request)
ci-spec-verification.yml:45, :68         (push / pull_request)
```

- 触发方式（本地读取 + `gh` 佐证）：`ci-native` = push/pull_request(**有** paths)+dispatch；`ci-python` = push/pull_request(**有** paths)+dispatch；`ci-spec-verification` = push/pull_request(**有** paths)；`ci-native-sanitizers` = schedule+dispatch；`codeql` = schedule+dispatch；`main-merge-guard` = push→master。
- ⇒ **在 `pull_request` 上无路径过滤运行的 workflow 数量为 0**。改动 `docs/**`、`formal/**`、`tools/**`、`.github/**`（除被显式列出的 workflow 文件）的 PR 不会产生任何检查 ⇒ FI-041 描述的盲区属实。

### 2.7 关键 PR 内容抽验

| PR | 标题要点 | 文件级证据 | 用途 |
| :--- | :--- | :--- | :--- |
| #72 | `feat(l5): TODO 1A.3 real POST /api/v3/order submission` | 10 文件 `+843/-39`；`binance_private_rest.hpp +287`、`live_submit_orchestrator.hpp +80`、`test_binance_private_rest.cpp +349` | 证实真实 POST 落地（正文 E1/§A） |
| **#79** | `Batch G: close ExecutionJournal (TODO 1A.5) + fix stale is_legal_audit_event_type() bound` | **仅 4 文件 `+57/-1`**：`docs/NATIVE_ARCHITECTURE.md +17`、`docs/adr/ADR-019 +3`、`durable_frame_codec.hpp +10`、`test_durable_frame_codec.cpp +27`。**无任何日志/台账实现** | 证实 E1（#79 是文档收口，不是交付 `ExecutionJournal`） |
| #85 | `feat(l4): H6 -- cold-start harness wiring real testnet credentials, H1-H5` | `+419/-0`，新增 `native/src/live_submit_real_credentials_demo.cpp +402` | 证实 E1/§七（仓库有可加载真实凭据的演示路径） |
| #91 | `… preflight harness reaches OperatorNotConfirmed, zero real network writes` | `+992/-0`，新增 `live_submit_preflight_harness.cpp +688` + 测试 `+277` | 对应 E9/E10 的复核对象 |
| #106 / #107 | 预检 harness 改跑 `PublicFeedPipeline`；数据级 staleness | `#106` 4 文件 `+259/-156`（含 `binance_clock_sync.hpp +15`）；`#107` 14 文件 `+680/-24` | 证实 §A “#95–#107 公共行情链” |

---

## 3. 代码侧核验（逐条 `file:line`）

### 3.1 ExecutionJournal 不存在（E1）——影响面最大的一条

```
git grep -n "ExecutionJournal" -- .        → 4 处，全部是散文/注释：
  docs/NATIVE_ARCHITECTURE.md:29-42   「ExecutionJournal — evaluated and dropped (2026-09).」
  docs/adr/ADR-019-…md:205            评估记录
  native/include/hengyuan/durable_frame_codec.hpp:171   注释
  native/tests/test_durable_frame_codec.cpp:92          注释
```

`docs/NATIVE_ARCHITECTURE.md:29-42` 原文要点（逐字核对）：PR #72 原始的 TODO 1A.5 排除清单把 “Minimal PositionTruth/ExecutionJournal” 并列，**只有 PositionTruth 被实现**；ExecutionJournal 从未在任何 spec/ADR 中定义，其疑似目标（跨机制去重）已由 `OrderFillContext::consume_delta()` 关闭；仓库内没有任何 ADR/spec/`py_core` 代码要求它；并明确 **新增 `DurableRecordType` 枚举值不是可选项**（封闭 + `tools/spec_enum_diff.py` CI 强制）。

全树 glob 亦确认 `native/include/hengyuan/execution_journal.hpp` **不存在**，`spot_query_adapter.hpp` 同样不存在（E26）。

### 3.2 终态持久化缺口（E13 / AUDIT-P1-002 / REM-0.4）

`native/include/hengyuan/order_tracker.hpp:584-627`（本人逐行读过）：
- `:584-589` 签名参数只有 `InFlightRegistry& / AuditRingSink* / ReconcileEventRing& / now_ms / PositionTruth* / OrderFillContext*` —— **没有任何 durable 端口**。
- `:613` `audit->append(ar);`（内存 ring）
- `:616-618` `fill_context->consume_delta(...)`
- `:619-621` `position_truth->apply_fill(...)`
- `:622-625` `in_flight.mark_resolved_handle(...)` + `fill_context->remove(...)`
- `:595-597` `OrderReconciled` / `OrderEscalated` 的构造点；`:598-605` 自带注释写明 “this is the only place either is ever emitted”。

`native/include/hengyuan/binance_user_data_event.hpp:163`（子代理核验）同形：`:237` 仅 `audit->append(ar)`、`:191` `consume_delta`、`:195` `apply_fill`，全文无 `append_durable`。

对照同步 POST 路径确有 durable ACK：`live_submit_orchestrator.hpp` Gate 10b `:540`、Gate 12d `:666`、Gate 13 分支 `:740/:819/:852/:884/:920`（均为 `!ctx.durable_audit.append_durable(...).acked()` → `OrchestratorGate::AuditWriteNotAcked` + return）。

### 3.3 TransportPolicy / 时钟幅度（E11/E12 / AUDIT-P2-001）

```
git grep -n "check_endpoint" -- native
  transport_policy.hpp:106                (定义)
  tests/test_transport_policy.cpp:13,89,94 (仅测试)
git grep -n "check_clock_skew" -- native
  transport_policy.hpp:290                (定义)
  binance_clock_sync.hpp:166              (仅注释引用)
  tests/test_transport_policy.cpp:12,294-381 (仅测试)
git grep -c "TransportPolicy" -- native/include/hengyuan/binance_private_rest.hpp  → 0 命中
```

`transport_policy.hpp` 关键片段（本人读取）：
- `:31` `kMaxEndpoints = 4`；`:33-35` `struct EndpointAllowlist { std::array<std::string_view, kMaxEndpoints> hosts{}; std::size_t count{0}; }`
- `:37-42` `contains()` 用 `for (i = 0; i < count; ++i) hosts[i]`，`operator[]` 无边界检查 ⇒ `count > 4` 时越界读。
- `:94-103` `validate_policy()` 只有 `count == 0` 检查，**没有上界检查**。
- `:45-53` `binance_default_endpoints()` 设 `count = 4`；`binance_environment.hpp:159` 设 `count = 1` ⇒ 当前树内不可达，属**潜在**缺陷。

### 3.4 私有 REST 客户端（E16/E11 / §A）

`native/include/hengyuan/binance_private_rest.hpp`（本人读取）：
- `:121-122` `std::string extra_trusted_ca_pem_path;` / `std::string connect_host_override;`（注释 `:119-120` 自述 “Test-only escape hatches … production callers never set either”）。
- `:1217` `fetch_account(...)` 入口**无** `if (!creds_)`；`:1235` 首次解引用 `build_signed_query(*creds_, …)`、`:1240` `creds_->copy_api_key`。
- 对照兄弟方法**都有**守卫：`:1552` `submit_order` 的 `if (!creds_) return {SubmitOutcome::NetworkError, 0, -1};`（另有空 coid/symbol/价格数量的前置校验 `:1553-1555`），以及 `:1292`/`:1423`/`:1637`。
- `:1543` `SubmitResponse submit_order(...) noexcept`；`:1605-1611` 真实 `net::co_spawn(..., http::verb::post)`。
- 同名 override 字段另见 `binance_rest_snapshot.hpp:70,80`、`binance_klines_rest.hpp:59,60`（E-ADD：收口范围是 3 个结构体，不是 1 个）。

### 3.5 KillSwitch（E14）

`native/include/hengyuan/kill_switch.hpp`（本人读取 `:14-61`）：
- `:14-19` `KillState { Normal, Armed, Triggered, Latched }`，`:18` 注释 “terminal lockout — operator-only reset”。
- `:28-33` `arm()` / `:36-41` `trigger()`（单调）/ `:44-46` `latch()` / `:51-53` `operator_reset()`（`:48-50` 注释：“models a deliberate process restart … NO automatic code path calls this”）。
- `:59` `KillState state_{KillState::Normal};` —— 普通字段，无 `atomic`、无 `alignas`。
- 全仓无任何序列化/载入 `KillState` 的代码 ⇒ **内存锁存有、持久化无**；且现有设计把“重启”等同于 `operator_reset()`，正说明 REM-0.5 的“持久锁存”是新增要求而非修复既有实现。

### 3.6 预检 harness（E9/E10）

`native/src/live_submit_preflight_harness.cpp`：
- `:382` `if (client.create_listen_key(listen_key_buf, listen_key_len) != PrivateRestError::None) {` —— 落到 `binance_private_rest.hpp:1305-1307` 的 `POST /api/v3/userDataStream`（服务端建状态）⇒ “零真实网络写入”不成立，准确说法是**零真实订单写入**。
- `:679` `ctx.submit_port = {mock_submit_must_never_be_called, …}`（mock 被调用即断言，`:170-174`）。
- `:687-693` 对 `OrchestratorGate::OperatorNotConfirmed` 只做告警打印，不是停止点。
- `:532-536` 自述该 harness “**不是**恢复路径”，真实恢复须先跑 `startup_recovery.hpp` 的序列。

### 3.7 其它逐条

| 编号 | 结论 | 证据 |
| :--- | :--- | :--- |
| E4 | `durable_control_plane.hpp` 无构造函数（只有 POD 与抽象接口，`:2051/:2123/:2455` 为虚析构）；真实 `noexcept` 构造分配隐患在 `durable_audit_sink.hpp:300-303`，另见 `control_plane_log_sink.hpp:110`、`export_worker.hpp:169` | 子代理逐文件核验 + 文件结构确认 |
| E7 | `ci-native.yml` 唯一 job `native-build-test`，`runs-on: ubuntu-24.04`；**无 MSVC job**。`CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_TIMEOUT 120` 位于 `native/CMakeLists.txt:303`，不在 workflow 内 | 读 `ci-native.yml` 全文 + `git grep DISCOVERY_TIMEOUT` |
| E15 | `live_submit_orchestrator.hpp:712` `switch (resp.outcome)` 覆盖 `Accepted/Rejected/Timeout/NetworkError/StaleRulesVersion` 五个值、无 `default`；spec 另有 `RateLimited = 4`（`:60-65`）⇒ 当前无 UB，新增值会静默落空 | 子代理 + 文件核实 |
| E16 | `risk_gate.hpp:4` “`__int128` for notional overflow safety” vs `:97` “Portable overflow-safe check (**no `__int128`**)” | 子代理引用 |
| E17 | `binance_signer.hpp:191/:216/:224` 仅 HMAC-SHA256；`SigningPolicy` 类型全仓零命中；RSA/Ed25519 仅在 spec 中标注 NOT supported | 子代理 grep |
| E18 | `binance_decimal.hpp` / `fixed_point.hpp` 为编解码 + int64 溢出安全助手；全仓唯一舍入助手是 `account_truth.hpp:310 rescale_notional_ceil()` | 子代理 + 本人复核 |
| E19 | `AccountModeGate`/`ConstraintEngine`/`Decimal64`/`RoundingMode`/`LegRiskController`/`FundingOpportunityGate`/`KillPublicationPoint` 等在本仓库**零命中**（只出现在本规划文档自身） | 子代理全仓 grep |
| E8 | `order_tracker.hpp:398-549` `poll_once()` 已实现且在 3 个 harness 中运行（`live_submit_preflight_harness.cpp:607`、`live_submit_real_credentials_demo.cpp:338`、`live_submit_reconcile_harness_demo.cpp:140`）⇒ 原文把“轮询驱动器”列为待实现已过时 | 子代理 + `formal/inflight_lifecycle.tla` 佐证 |
| E-ADD | InFlight 容量 64（`order_lifecycle.hpp:353`）；满/重复同返 `{}`（`:401`），orchestrator 统一记 `DuplicateInFlight`（`live_submit_orchestrator.hpp:617-627`）；`poll_once()` 对无法入队的 ingress 静默丢弃（`order_tracker.hpp:406-407`） | 子代理引用 |
| E-ADD | `startup_recovery.hpp`（#94）存在且仅被测试调用；harness 明确拒绝充当恢复路径 | 子代理引用 |
| E5 | `docs/NATIVE_ARCHITECTURE.md:10` 已写 “A real, signed, authenticated REST client exists … simply not yet wired into any running `native/src/` process”；`:24-25` 亦已口径正确。仍过时的是 `CLAUDE.md:237-239`（“no authenticated REST client exists yet; no real testnet or production credentials have ever been used”） | 本人读 `NATIVE_ARCHITECTURE.md` 全文 + grep `CLAUDE.md` |
| E27 | `formal/*.tla` = **10** 个；`ci-spec-verification.yml` 只跑 **9** 个 `Model:` 步骤（`:164,211,249,279,315,331,361,399,429`），`RoundEFDesignReceiptVerified.tla` 未纳入；workflow 自身注释 `:15` 仍写 “the 7 independent TLA+ models”；`formal/README.md` 只为 7 个模型写了章节 | 本人 grep + 计数 |
| E28 | `SPEC_INVARIANTS.md:4` 称 L4 为 “Revision 19 / round 19”，而 `BINANCE_PRIVATE_REST_L4_SPEC.md:1` 自报 rev 72；该行自带“两套编号不同”的说明 ⇒ 记为**口径歧义**，不断言漂移 | 本人读取两个文件头部 |
| E-ADD | `docs/adr/` 恰有 ADR-016 / ADR-018 / ADR-019 三个文件 | 本人列目录 |
| E-ADD | 6c worktree：`durable_audit_export.hpp` 未跟踪、`CMakeLists.txt` 未改、无测试、无 CLI（树内无任何文件引用它） | `git -C …_6c status` + 递归 grep |
| E-ADD | 6b-0e worktree：新增 3 个 `operator_*` 头 + `test_helpers/blocking_istream.hpp` + 2 个测试；改 `ci-native-sanitizers.yml`/`CMakeLists.txt`/`wsl_verify.sh`；全部未跟踪/未提交 | `git -C …_6b0e status --short` + `diff --stat` |

---

## 4. py_core 侧核验（本人直接执行，未走子代理）

| 编号 | 结论 | 证据 |
| :--- | :--- | :--- |
| P-01 | `assert_no_lookahead` 定义于 `py_core/strategies/base.py:97`；`git grep` 全仓 14 处命中中，**生产调用点为 0**（其余为定义、`__init__` 导出、`base.py:54` 自述注释、以及 `tests/` 内 4 个用例） | `git grep -n assert_no_lookahead -- py_core` |
| P-02 | `base.py:54` 明写 “本模块的 `assert_no_lookahead()` 是一个**可选的事后核验工具**，不是…” ⇒ 原文“可选工具”判断准确 | 同上 |
| P-03 | 恒真自检在 **`py_core/backtests/vectorized_engine.py`**（原文写作 `validation/vectorized_engine.py`，**路径有误**）：`:105` `df.index.equals(...)`、`:195` 先 `reindex()`、`:198` 调 `validate_inputs(...)` ⇒ 比对的是已对齐 index，恒真。另 `py_core/risk/risk_integration.py:161` 也硬编码 `no_future_shift_detected=True` | `git grep -n no_future_shift_detected -- py_core` |
| P-04 | 算子注册表存在但仅覆盖 `RAW_FIELDS = {open,high,low,close,volume}` 及窗函数 `sma/ema/stddev/rolling_max/rolling_min/roc/rsi`、`lag`、`not`、`if_then_else`、二元运算、`crosses_*` ⇒ **无资金费率/订单簿数据源与算子**（原文判断正确，且问题比“缺两个算子”更深：DSL 无此二类输入） | `py_core/strategy_spec/schema.py:26`(`RAW_FIELDS`)、`:31`(`_WINDOW_OPS`)、`:39`(`KNOWN_OPS`)；`py_core/strategy_spec/evaluator.py:21`(`_WINDOWED_FUNCS`)。**注：该目录下不存在 `operators.py` / `validation.py`；实际文件为 `__init__.py` / `schema.py` / `evaluator.py` / `validation_sweep.py`** |
| P-05 | `binance_public_rest.py:394-398` 仅在 `last_accumulated_open_time_ms is not None`（`:353` 初值 `None`）时做跨页连续性校验；`:404`/`:408` 两处 `continue` 是仅有的防御分支，但 `:411` 之前**没有按 `start_ms` 过滤** ⇒ 首页（或单页）早于游标的数据会被整体接受。原文 AUDIT-P1-005 描述方向正确，但**未指出“没有按请求游标过滤/没有首页断言”这一根因**；游标变量为 `:347 start_ms` / `:355 cursor_ms`，续拉起点由 `:423-424` 依据 `last_accumulated_open_time_ms` 计算 | 本人读 `py_core/market_data/binance_public_rest.py:347,353,355,394-398,404-411,423-424` |
| P-06 | `is_contiguous` **已存在**于 `py_core/market_data/warehouse.py:875-876`（`CoverageReport` 的字段，由 `:919-920` 填充），但 `py_core/market_data/warehouse_backfill.py:45` 的 `BackfillReport` **没有**该字段（该文件的 `:66/:101` 出现 `covered_end_utc` 是读取 `existing` 这个 **`CoverageReport`**，不是 `BackfillReport` 自身字段） | 本人读 `warehouse.py` / `warehouse_backfill.py` 及 `market_data/cli.py:319-320` |
| P-07 | `py_core` 规模：**71 个受跟踪文件 / ~16.2k 行**（不含 `.venv`），原文“~66 文件 / ~15,300 行”已过时（原文亦自述该数字会漂移，态度正确，本次更新为实测值） | `git ls-files py_core` 过滤 `.venv` 后计数与逐文件行数累加 |
| P-08 | `ci-python.yml` 触发为 push/pull_request（`paths` 过滤）+ dispatch；ruff 为**硬门**（无 `continue-on-error`） | 本人 grep workflow |
| P-09 | py_core 内**不存在** ExperimentRegistry / Frozen Holdout / DataQualityLedger / BacktestFeeProvider / HistoricalFeeModel（对应 Stage 3/4 各项确为待建） | 子代理检索 + 本人抽样 |

---

## 5. 未能核验 / 刻意不下结论的项

| 项 | 原因 | v2.6.0 的处理 |
| :--- | :--- | :--- |
| 原文引用的“开发者截图”中的构建、测试、变异进度 | 截图不在本机，且属开发者自述 | 保留“不能替代终态日志”的定性，并注明合入前须以 PR diff + CI 终态替换（W6） |
| Jev / Laya 的性能与价格数字 | 全部来自厂商/作者自述页面，无法在本环境复现，且原文已标注测量环境差异 | 保留为“厂商报告”，并维持“不得写作 HengYuan p99 或收益”的约束 |
| §八/§九 第三方数据源与开源库的可用性、延迟、免费额度 | 属外部服务现状，非本仓库事实 | 保留为调研建议，删去会漂移的 Star 数（W4） |
| `SPEC_INVARIANTS.md:4` 的 L4 revision 是否为真漂移 | 该行自带“两套编号体系不同”的说明，无法单方面判定 | 记为**编号口径歧义**，要求明确二者关系（E28），不断言缺陷 |
| `round-h`/`round-i`/`round-j` 等历史分支的细节 | 与当前版本计划无关 | 不纳入 |

---

## 6. 复现命令清单

```powershell
# 0) 注入 gh 凭据（本机 GH_TOKEN 存于用户级环境变量，harness 进程不自动继承）
$env:GH_TOKEN = [Environment]::GetEnvironmentVariable("GH_TOKEN","User")

# 1) 基线与 worktree
cd D:\My_Projects\hengyuan_v2
git fetch origin --prune
git log -1 --format="%H %ci %s" origin/master
git rev-list --count 3278180..origin/master
git worktree list
git status --short

# 2) PR 与合并提交
gh pr list --state all --limit 40 --json number,state,title,mergedAt,mergeCommit
gh pr list --state open
gh api "repos/EypeZeo/hengyuan_v2/pulls/79/files?per_page=100"

# 3) CI 事实
gh api "repos/EypeZeo/hengyuan_v2/actions/runs?head_sha=f4ee6208005d7cedc39f6600ab8ce4fbf81868e1"
gh api "repos/EypeZeo/hengyuan_v2/actions/runs/35556436339/jobs?per_page=20"
gh run list --workflow=ci-native-sanitizers.yml --limit 8 --json databaseId,event,status,conclusion,createdAt
gh run list --workflow=codeql.yml --limit 6 --json databaseId,event,status,conclusion

# 4) 平台能力
gh api repos/EypeZeo/hengyuan_v2/branches/master/protection
gh api repos/EypeZeo/hengyuan_v2/rulesets
gh api repos/EypeZeo/hengyuan_v2/code-scanning/default-setup

# 5) 代码事实（节选）
git grep -n "ExecutionJournal" -- .
git grep -n "check_endpoint\|check_clock_skew" -- native
git grep -c "TransportPolicy" -- native/include/hengyuan/binance_private_rest.hpp
git grep -n "assert_no_lookahead" -- py_core
git grep -n "DISCOVERY_TIMEOUT" -- .
(Get-ChildItem formal\*.tla | Measure-Object).Count
```

---

## 7. 交付物清单

| 文件 | 说明 |
| :--- | :--- |
| `docs/HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.md` | **v2.6.0 修订版正文**（本次交付主体） |
| `docs/archive/HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.orig-9d78643d.md` | v2.5.6 原件**逐字节备份**（SHA256 已校验一致） |
| `docs/archive/HengYuan_v2_TODOLIST_核对证据台账_2026-09-25.md` | 本台账（更正的证据底账） |

> 本次交付**未**修改任何仓库源码、未提交 commit、未推送远端；正文与台账均为 `docs/` 下的新增/替换文件。6b-0b / 6b-0e / 6c 三个 worktree 的在制改动未被触碰。
