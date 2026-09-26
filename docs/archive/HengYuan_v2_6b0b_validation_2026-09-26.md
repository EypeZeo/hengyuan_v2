# HengYuan v2 6b-0b 验证记录（2026-09-26）

## 结论

当前工作树 `feat/batch6-6b0b-verified-dry-run-evidence` 的 6b-0b 变异套件已完整执行：

| 项目 | 结果 |
| :--- | :--- |
| 变异总数 | 162 |
| 被测试捕获 | 152 |
| 存活 | 10 |
| 编译失败 | 0 |
| 变异后原件恢复 | `identical=True` |
| 恢复后头文件 SHA-1 | `7687b43a4236e273db216b58e9b445fe46146baf` |
| `MUTATION-CHECK` 残留 | 0 |

原始逐项日志保存在本机临时验证目录：
`C:\Users\Shark Twizz\AppData\Local\Temp\opencode\mutations-6b0b-20260926T121113-d36db160e5e34adc84fd5b8ce5b12820\all_mutations.log`。

## 存活项处置

| 变异 | 结论 |
| :--- | :--- |
| O33 | `latch()` 后 `kill_switch_latched` 在合法 rig 中只有 `true`；判定函数已由篡改观察值测试覆盖 |
| O34 | `latch()` 后不会自动回到 `Normal`；判定函数已由篡改观察值测试覆盖 |
| O35 | `latch()` 后再次编排必为 `KillSwitchNotNormal` 且端口不调用；判定函数已由篡改观察值测试覆盖 |
| G1 | `AuditRingSink` 默认可用；该设置是显式语义标注，不改变当前 rig 的可观察行为 |
| G2 | `KillSwitch` 默认即为 `Normal`；显式 reset 是意图声明，不改变当前 rig 的可观察行为 |
| G22 | 本四路径 rig 显式调用 `poll_once()` 与 `drain_reconcile_events()`；该配置在本演练中冗余，不推导生产编排可删除该接线 |
| V2 | `ran_ok_` 在 `run_all()` 入口清零属于与链清零相伴的防御性复位；当前失败运行的链继承测试已覆盖核心行为 |
| V13 | `ready_for()` 保留 `ran_ok_` 与 `live_ready()` 双重保护；当前链语义使去掉前者不改变结果 |
| V14 | `ready_for()` 保留 `ran_ok_` 与 `live_ready()` 双重保护；当前链语义使去掉后者不改变结果 |
| V16 | `run_all()` 仅在 `chain_.live_ready()` 后设置 `ran_ok_`；链本身已提供相同结果的第二重保护 |

## 捕获性补充

前序变异记录中的 V11/V12 原先因 `/WX` 下未使用形参而编译失败。本轮通过保留形参并显式消费、仅移除环境或构建比较，构造了可编译变异；两项分别由环境/构建绑定测试捕获。前序 G3 原先被误判为等价；本轮新增限流预算耗尽负控后，删除配置会使第 73 笔请求被错误放行，G3 被捕获。

## 其他验证

- MSVC Release 全量：2036 通过、1 跳过（共 2037 项）。
- WSL/GCC-14 `none` 全量：2033/2033 通过。
- GCC-14 ASan+UBSan 定向：40/40 通过。
- GCC `none` 新测试随机顺序 30 轮、ASan+UBSan 新测试随机顺序 10 轮：每轮 40/40 通过，无 sanitizer 报告。
- 无 `.env` 预检进程：四项演练均 `PASSED`，随后在凭据读取处按预期退出，未进入网络路径。
- 远端 CI Native：run `36229428754`，`success`；作业 `native-build-test`，`success`。
- 远端 CI Spec Verification：run `36229428777`，`success`；L1 与 L3 作业均 `success`。
- 远端 CI Native Sanitizers：run `36229665532`，`success`；ASan+UBSan、TSan/负控、ARM64 hardware weak-memory 三个 job 均 `success`。
- follow-up head `c7270bc` 的最终复验：CI Native run `36238442566`、CI Spec Verification run `36238444607`、CI Native Sanitizers run `36238447723` 均 `success`；三个 Sanitizer jobs 均 `success`。
- 代码合并证据：PR #108，merge commit `8d81fda`；验证/文档收尾 PR #110，merge commit `5aa4242`；最终路线图元数据由 docs-only PR #111 合入；PR #109 为重复 head，已关闭，不作为合并证据。

## 限制

本记录不包含 PR 合并证据或真实凭据路径；模拟订单演练不构成真实订单写入授权。远端 CI 与 ARM64 结果仅证明代码验证作业通过，不替代 Owner 的 L5 授权和真实订单审查。
