# master 必需检查 runbook

本文说明 `master` 的必需检查（GitHub 规则集）应当怎么配、怎么验证、怎么日常合并、怎么回滚。规则集属于仓库安全设置，**只由 Owner 配置**；AI 会话不改它（见 `CLAUDE.md` 的边界）。

状态：2026-10-07，规则集 `master-gate`（id 24635472）已由 Owner 创建并启用，`master` 受保护；启用后的实测见 4.1。仓库自 2026-09-30 公开，平台允许配置。

## 1. 为什么需要 `gate` 检查

`ci-native`、`ci-python`、`ci-recorder`、`ci-spec-verification` 过去的 `pull_request` 触发带工作流级 `paths:` 过滤。GitHub 的规则是：因路径过滤而没有运行的 workflow，其检查永远停在 Pending，并**阻止合并**；被 `if:` 条件跳过的 job 则记为 Success。所以不能把 `native-build-test` 这类检查直接设为必需。

现在每个 workflow 在每个 PR 上都会运行，路径判断挪到 job 级（`changes` job），再由一个总是运行的 `gate` job 汇总：

| 检查名 | 来自 | 通过的条件 |
| :--- | :--- | :--- |
| `native: gate` | `ci-native.yml` | `changes` 成功，且（改了 `native/**` 或本 workflow ⇒ `native-build-test` 成功；否则 ⇒ 它被跳过） |
| `py_core: gate` | `ci-python.yml` | 同上，守护 `pytest` |
| `recorder: gate` | `ci-recorder.yml` | 同上，守护 `lock-311`、`latest`、`bundle` |
| `spec: gate` | `ci-spec-verification.yml` | 同上，守护 `spec-invariant-xref`、`tla-model-check-all` |

任何其他组合（`changes` 失败或取消、该跑的被跳过、该跳过的却跑了）都使 gate 失败：失败即关闭。路径正则各写在 workflow 的 `changes` job 里，与 `push.paths` 要保持一致。

## 2. 规则集设置（一个 ruleset，名 `master-gate`，目标 = 默认分支）

| 规则 | 设置 |
| :--- | :--- |
| Restrict deletions、Block force pushes | 开 |
| Require a pull request before merging | 开；Required approvals = **0**；Allowed merge methods = **只勾 Merge**；不要求 code owner 评审；`require_extra_approval_for_unattributed_changes` 显式写 **false**（见下） |
| Require status checks to pass | 开；**Require branches to be up to date before merging** 勾上；检查见下表；“Do not require status checks on creation” 不勾 |
| Bypass list | **空** |
| 不要开 | Require linear history、Require signed commits、Require code owner review、Require deployments |

为什么批准数是 0、不要求 code owner：这是单人仓库，PR 作者不能批准自己的 PR；`.github/CODEOWNERS` 是 `* @EypeZeo`，要求 code owner 评审等于永远合不了。为什么只允许 merge commit：全仓一直用 merge commit，合并守卫也按它核对 `merge_commit_sha`。

**GitHub 自己补上的一个默认值**：`pull_request` 规则多了一个公开预览参数 `require_extra_approval_for_unattributed_changes`，JSON 里省略它时 GitHub 按 `true` 创建——2026-10-07 创建本规则集时读回就是 `true`。按第三方资料的说法（官方 REST 文档页没有列出该字段，下面的含义无法从官方文档核实），它要求由 AI 代理或无法归属到人类账号的提交开出的 PR 多一个审批；单人仓库里没人能给这个审批，又没有绕过名单，一旦触发只能临时把规则集改成 Disabled。本仓库最近 100 个主干提交与最近 30 个合并 PR 都归属到 Owner 账号，预计不会触发，但设计意图是“批准数 0”，所以 JSON 里显式写 `false`。创建或更新后用 `gh api repos/EypeZeo/hengyuan_v2/rulesets/<id>` 读回确认。

**9 个必需检查**（名字与来源取自真实 check-run）：

| 检查名 | 来源（integration_id） |
| :--- | :--- |
| `native: gate`、`spec: gate`、`recorder: gate`、`py_core: gate` | GitHub Actions（15368） |
| `Analyze (actions)`、`Analyze (c-cpp)`、`Analyze (javascript-typescript)`、`Analyze (python)` | GitHub Actions（15368），CodeQL 默认配置，每个 PR 都有 |
| `CodeQL` | GitHub Advanced Security（57789），代码扫描结果 |

不要把 `changes` job 设为必需：它的失败会让对应的 gate 失败，已经被覆盖。`CI Native Sanitizers` 只在每周定时与手动派发时运行，不在 PR 上，不能设为必需。

`CodeQL`（代码扫描结果）检查在 `Analyze (…)` 还在运行时先显示 neutral，四个 Analyze 全部完成后才变 success（#139、#140 实测）。受保护分支文档写明必需检查把 success、skipped、neutral 都算通过，所以真正等待分析完成的是四个 `Analyze (…)`；两类都要设为必需。

## 3. 配置步骤

前置：
- 上面四个 `*: gate` 已在 `master` 上跑过（规则集界面只能选 7 天内成功运行过的检查）。
- 在途 PR：规则集生效后，最近一次运行不含 gate 的 PR（在四个 workflow 加 gate 之前开出、之后没同步过的）会一直“等待状态”。启用前先 `gh pr update-branch <N>` 让它们同步并跑一遍 gate，或者把它们合并/关闭。

### 3.1 用界面

Settings → Rules → Rulesets → New ruleset → New branch ruleset：
1. Ruleset Name：`master-gate`；Enforcement status：先选 **Disabled**。
2. Bypass list：不加任何人。
3. Target branches：Add target → Include default branch。
4. 勾上第 2 节表里的规则并填写参数；Add checks 里逐个添加 9 个检查（来源选“任何来源”也可；想防同名伪造就按上表选来源）。
5. Create。检查无误后把 Enforcement status 改成 **Active**。

### 3.2 用命令

把下面的 JSON 存成 `ruleset.json`：

```json
{
  "name": "master-gate",
  "target": "branch",
  "enforcement": "disabled",
  "conditions": { "ref_name": { "include": ["~DEFAULT_BRANCH"], "exclude": [] } },
  "bypass_actors": [],
  "rules": [
    { "type": "deletion" },
    { "type": "non_fast_forward" },
    { "type": "pull_request", "parameters": {
        "required_approving_review_count": 0,
        "dismiss_stale_reviews_on_push": false,
        "require_code_owner_review": false,
        "require_last_push_approval": false,
        "required_review_thread_resolution": false,
        "require_extra_approval_for_unattributed_changes": false,
        "allowed_merge_methods": ["merge"] } },
    { "type": "required_status_checks", "parameters": {
        "strict_required_status_checks_policy": true,
        "do_not_enforce_on_create": false,
        "required_status_checks": [
          { "context": "native: gate",   "integration_id": 15368 },
          { "context": "spec: gate",     "integration_id": 15368 },
          { "context": "recorder: gate", "integration_id": 15368 },
          { "context": "py_core: gate",  "integration_id": 15368 },
          { "context": "Analyze (actions)", "integration_id": 15368 },
          { "context": "Analyze (c-cpp)", "integration_id": 15368 },
          { "context": "Analyze (javascript-typescript)", "integration_id": 15368 },
          { "context": "Analyze (python)", "integration_id": 15368 },
          { "context": "CodeQL", "integration_id": 57789 } ] } }
  ]
}
```

```bash
gh api -X POST repos/EypeZeo/hengyuan_v2/rulesets --input ruleset.json
gh api repos/EypeZeo/hengyuan_v2/rulesets --jq '.[] | [.id, .name, .enforcement] | @tsv'
```

启用：把 `ruleset.json` 里的 `enforcement` 改成 `"active"`，保存为 `ruleset-active.json`，用整份文件更新（整份替换，不依赖“部分更新”的语义）：

```bash
gh api -X PUT repos/EypeZeo/hengyuan_v2/rulesets/<id> --input ruleset-active.json
```

`<id>` 换成上一条列表输出里的数字（本仓库是 `24635472`）。在 Windows PowerShell 5.1 里不要在命令里用 `$(...)` 嵌套另一条带 jq 字符串字面量的 `gh api`：5.1 把参数传给原生程序时会吃掉内层双引号，jq 报 `function not defined`，替换结果为空，请求落到 `/rulesets/` 得到 404（2026-10-07 实际发生过，没有副作用）。先列出 id，再手写进命令。

## 4. 启用后的核验

只读：`gh api repos/EypeZeo/hengyuan_v2/rules/branches/master` 应列出 4 条规则（`deletion`、`non_fast_forward`、`pull_request`、`required_status_checks`）；规则集处于 Disabled 或 Evaluate 时这个列表是空的。

| 场景 | 预期 |
| :--- | :--- |
| 纯文档 PR | 四个 gate 绿（被守护的 job 显示 skipped），CodeQL 五项绿，可合并 |
| 改了 `native/` 的 PR | `native-build-test` 真跑，`native: gate` 在它成功后才绿 |
| 故意让一个测试失败的 PR | 对应 gate 红，`gh pr merge` 被拒 |
| master 前进后，在途 PR 没同步 | 合并被拒，提示分支落后；`gh pr update-branch <N>` 后重跑检查 |

不要用真实推送去试“直接推 master 会不会被拒”：规则集若还没生效，那一下就是真推送。用上面只读的规则列表确认规则已生效即可。

### 4.1 首次启用的实测（2026-10-07）

| 场景 | 探针 | 结果 |
| :--- | :--- | :--- |
| 规则生效（只读） | — | `rules/branches/master` 列出 4 条规则，来源都是规则集 24635472；规则集读回与 `ruleset-active.json` 逐项一致（`bypass_actors` 为空，`current_user_can_bypass` 为 `never`，`require_extra_approval_for_unattributed_changes` 为 `false`） |
| 红色 PR | #140（故意失败的 `py_core` 测试，不合并，已关闭） | `py_core: pytest` 失败，`py_core: gate` 失败，`mergeStateStatus` = `BLOCKED`；`gh pr merge` 被拒：`the base branch policy prohibits the merge`；`master` 不变。没有试 `--admin` |
| 绿色纯文档 PR | #141（只加 `docs/PROBE_REQUIRED_CHECKS.md`，不合并） | 四个 gate 绿（被守护的 7 个 job 为 skipped），`CodeQL` 与四个 `Analyze` 绿，`mergeStateStatus` = `CLEAN` |
| 落后于 master 的 PR | #141（#142 合并后，探针，不合并） | `mergeStateStatus` = `BEHIND`；`gh pr merge` 被拒：`the head branch is not up to date with the base branch`；#142（纯文档，同样的检查形态）本身在规则集下合并成功（合并提交 `ff55fd6`，合并守卫判 ok） |

## 5. 日常合并流程（strict 模式）

```bash
gh pr checks <N>                         # 看各检查与 gate
gh pr update-branch <N>                  # 分支落后于 master 时：把 master 合进 PR 分支，然后等检查重跑
gh pr merge <N> --merge --match-head-commit <完整 head SHA>
```

纯文档 PR 同步后只重跑几分钟的 CodeQL 与四个 gate；碰了 native 或 recorder 的 PR 会重跑相应的 CI。可选的仓库设置（均由 Owner 决定，默认关）：Settings → General → “Always suggest updating pull request branches”（在界面里显示同步按钮）、“Allow auto-merge”（`gh pr merge --auto`，检查全绿后自动合并）。

## 6. 合并守卫（`main-merge-guard.yml`）的定位

守卫仍保留，作为纵深防御：它在每次推送到 `master` 后核对“这次推送是否对应一个已合并且检查全绿的 PR”。规则集生效后，直接推送与红色合并都被平台拦下，守卫的 revert 分支基本不会触发；万一触发，它用 `GITHUB_TOKEN` 直接推 `master` 的 revert 也会被“必须经 PR”拦下（规则集没有绕过名单）。所以守卫变红就是警报：用 PR 手动回退（这一点按规则语义推断，没有为实测而故意触发守卫回滚）。守卫读的是 PR 头提交的 GraphQL `statusCheckRollup.state`，只认 `SUCCESS`（汇总为 null 时也放行）；含跳过 job 的汇总实测为 `SUCCESS`——#139 合并时守卫日志是 `Attempt 1: PR #139 rollup=SUCCESS`，所以纯文档 PR 不会被误判。守卫本身不需要改。

## 7. 紧急绕过与回滚

规则集没有绕过名单。紧急情况：Settings → Rules → Rulesets → `master-gate` → Enforcement status 改成 **Disabled**，合并，再改回 **Active**（设置页留有记录）。整体回滚同样是 Disabled，或 Delete。

## 8. 维护

- 给 gate 守护的 workflow 新增、改名 job：同步更新该 workflow 的 `gate` job 的 `needs` 与环境变量；规则集只认 gate 的名字，job 改名不影响规则集。
- 改路径过滤：同时改该 workflow 的 `changes` 正则与 `push.paths`。
- 新增一个会在 PR 上运行的 workflow：照同样的模式加 `changes` 与 `gate`，再把新 gate 加进规则集的必需检查（先让它在 `master` 上跑过一次）。
- 检查名、来源（integration_id）不一致会让检查永远“等待状态”：改名后到 PR 页面核对实际名字与来源。
