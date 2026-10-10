# D0 录制器的定期维保

录制器（`tokyo-vps-8t` 上的 `hy-recorder`）只是一半：数据的**持久副本在操作员本机**，宿主机上只留约 48 小时已回执的数据。这份文档说明维保怎么自动做、读什么、出了告警怎么处置。

## 布局（操作员本机）

维保目录**不在任何 git 工作树里**（工作树会被清理，数据湖是唯一的持久副本）：

```
D:\My_Projects\hengyuan_ops\d0\
  venv\                  最小解释器环境（只有 zstandard 与 websockets）
  tool\                  recorder\ 的快照（git archive），计划任务从这里运行 `python -m hy_recorder maint`
  lake-tokyo-vps-8t\     数据湖（pull 的目的地；按 `verify` 的规则读写）
  logs\                  latest.txt  status.json  state.json  maint-*.log  verify-*.txt  report-*.txt
  maint.json             配置：主机别名、数据湖与日志路径、阈值覆盖
```

`maint.json` 示例（路径按实际填写）：

```json
{
  "host": "tokyo-vps-8t",
  "lake": "D:\\My_Projects\\hengyuan_ops\\d0\\lake-tokyo-vps-8t",
  "logs": "D:\\My_Projects\\hengyuan_ops\\d0\\logs",
  "ssh": "C:\\Windows\\System32\\OpenSSH\\ssh.exe",
  "thresholds": {"local_free_warn_gb": 40}
}
```

## 每天自动做什么

计划任务 `HengYuan-D0-Maintenance`（`tools/install_maint_task.ps1` 注册：每天 06:00，另在每次登录后 5 分钟补跑；只在用户已登录时运行；距上次成功不足 6 小时则什么都不做）：

1. `pull`：镜像宿主机上已封存的段，逐段校验大小与 SHA-256，**之后**才回执（宿主机凭回执才清理）。
2. 只读探测宿主机（`hostprobe.py` 经 ssh 标准输入送过去，不写任何东西）：服务状态与重启数、磁盘、保留与驱逐、未回执积压、最近 24 小时账本事件（`latest.txt` 里 `rotations …` 与 `unplanned closes …` 两行是计划轮换与非计划断连的次数和重连空洞范围，只是事实，超过阈值才成告警）、时钟同步、内存、journal、同机服务。
3. 本地空间检查。
4. **每 7 天**对整个数据湖做一次完整 `verify`（含哈希）并保存 `report`。
5. 写 `logs\latest.txt`（给人看）、`logs\status.json`（给程序看）、当次完整日志；退出码 0 正常、1 WARN、2 CRIT（计划任务的“上次运行结果”就是它）。

日志只保留 60 天，维保不会删除任何数据。

计划任务用 `pythonw.exe`（无控制台）运行。无控制台的进程每启动一个控制台子进程（`ssh.exe`），系统就给它新开一个黑窗口，一次 pull 逐段一个 `ssh` 就是几百个窗口闪现又关闭。所以本机所有 `ssh` 子进程都必须经 `hy_recorder/winproc.py` 的 `run_hidden` / `popen_hidden` 启动（`CREATE_NO_WINDOW`，非 Windows 上无副作用）；新增 `ssh` 调用点不要直接用 `subprocess.run`。

## 告警与处置

| 代码 | 级别 | 含义 | 该做什么 |
| :-- | :-- | :-- | :-- |
| `PULL_FAILED` | WARN，连续第二次起 CRIT | pull 整体失败（多半是 ssh 连不上） | 手动 `pull` 一次；宿主机被封/挂起时查提供商面板；pull 幂等，重跑安全 |
| `PULL_ERRORS` | CRIT | 有段没能镜像或校验失败（不回执、不落地） | 看 `maint-*.log`，重跑 `pull`；反复失败就别删宿主机上的段，先查原因 |
| `PULL_LOST` | CRIT | 有段在被拉取前已被宿主机清理：**数据已丢** | 记下时间范围（宿主机清单有 `prune` 事件），查为什么 pull 停了 |
| `HOST_UNREACHABLE` | WARN，连续第二次起 CRIT | 探测连不上宿主机 | 同上；录制本身可能还在跑，宿主机自己的保留规则兜底 |
| `RECORDER_INACTIVE` | CRIT | `hy-recorder` 不在运行 | `systemctl status hy-recorder`、`journalctl -u hy-recorder -b`（journal 已持久化）；`Restart=always` 仍没起来就是真故障 |
| `RECORDER_RESTARTED` | WARN | 同一次开机内重启数增加（崩溃或看门狗自杀） | 看 journal 与账本的 `PROC_STOP`/`TASK_CRASH`；每次重启都是一个数据空洞 |
| `HOST_REBOOTED` | INFO | 宿主机重启过 | 对照 `CLOCK_STATE` 与 `PROC_START.prev_clean`；周验证会列出空洞 |
| `HOST_DISK` | WARN ≥70%，CRIT ≥85% | 宿主机磁盘 | 通常是 pull 停了；先 pull 回执，保留规则才能清理 |
| `EVICTED_UNACKED` | CRIT | 24 小时内有**未回执**的段被驱逐：**数据已丢** | 同 `PULL_LOST`；说明磁盘紧而 pull 没跟上 |
| `MANIFEST_DANGLING` | CRIT | 清单登记的段在宿主机上不存在 | 不要手工动数据目录；保留证据，查谁删了 |
| `UNACKED_BACKLOG` | WARN ≥36 h，CRIT ≥96 h | 最老的未回执段太旧 | pull 没在跑：看计划任务与 `PULL_*` |
| `DISK_STOP` / `DISK_WARN` | CRIT / WARN | 录制因磁盘停写 / 磁盘告警 | 同 `HOST_DISK` |
| `BAN` / `RATE_LIMIT` | CRIT / WARN | 币安 418 封禁 / 429 限流 | 与同机代理共用出口：查 `RATE_LIMIT` 账本上下文，别手动加 REST 调用 |
| `TASK_CRASH` / `OVERRUN` | WARN | 录制器内部任务崩溃后重启 / 队列溢出丢记录 | 看账本事件的 `error`/区间；`verify` 会把丢失的序号对上 `OVERRUN` |
| `STREAM_STALL` / `WS_CONNECT_FAIL` | WARN | 24 小时内有流静默被重连（**数据空洞**）/ 连接尝试失败（会重试） | 看账本里这两类事件的时间与连接：多条连接同时停顿而宿主机日志干净，多半是出口网络的短暂抖动（2026-10-05 16:54 UTC 那次：四条连接同时静默被重连、重连握手超时，各连接约 31 至 53 秒无数据（含静默检测期），宿主机 journal 该窗口只有防火墙与 ssh 扫描噪声）；`verify` 会把空洞标出来，不用补救 |
| `GAP_DETECTED` / `SUBSCRIBED_NO_DATA` / `BAD_FRAME` | WARN | 录制器自己看到序号缺口 / 订阅后始终没数据（路径错？）/ 有帧走了回退记录 | 看账本事件的流名与区间；`SUBSCRIBED_NO_DATA` 不应该出现，出现就是订阅路径或交易所行为变了 |
| `LOOP_LAG` / `REST_FROZEN` / `RETENTION_STEPDOWN` | WARN | 事件循环停顿（那一刻的 `t_recv` 偏晚）/ REST 被冻结 / 保留期因磁盘紧而降档 | 看账本事件；降档说明磁盘紧而 pull 没跟上，同 `HOST_DISK` |
| `ROTATION_HOLE` | WARN | 正在运行的录制器某次**计划轮换**的 `WS_CLOSE` → `WS_OPEN` 超过 2 秒（修复后预期 0.3 至 0.6 秒；旧版是 5.3 秒） | 多半是关闭超时又被改回去了或服务端行为变了：对照 `session.py` 的 `close_timeout_s` 与账本里那次轮换 |
| `UNPLANNED_HOLE` | WARN | 正在运行的录制器某次**非计划**断连（原因不是 `rotation`/`stop`：对端或路径主动关闭 `closed`、异常 `error`、静默 `stall`、深度缺口 `gap` 等）的 `WS_CLOSE` → `WS_OPEN` 超过 2 秒（一次快速重连通常 0.3 至 0.9 秒；连接活不过 60 秒会指数退避，空洞可达数十秒） | 这是一段没人计划的数据空洞：看账本里那次关闭的原因、连接与时间；`stall` 同时会有 `STREAM_STALL`，多条连接同一时刻多半是出口网络；`verify` 会把空洞标出来，不用补救 |
| `UNPLANNED_CLOSES` | WARN | 24 小时内非计划断连累计 ≥ 24 次（约每小时一次；2026-10-09 实测 9 次，都落在整分钟边界上、原因 `closed:None`，对端或路径主动断开，重连只用了 0.25 至 0.63 秒） | 连接在抖：看 `latest.txt` 的 `unplanned closes` 行里的原因分布，再对照账本里的时间规律、出口网络、币安公告与同机 `xray` 的状态；阈值在 `maint.json` 的 `thresholds` 里改（`unplanned_closes_warn`、`unplanned_hole_warn_s`） |
| `CLOCK_UNSYNCED` / `CLOCK_STALE` | WARN | 时钟长时间未同步 / 超过 3 小时没有 NTP 交换 | 宿主机 `timedatectl`、`systemctl status systemd-timesyncd`；期间的 `t` 与 `CLOCK_PROBE` 偏移不可信 |
| `MEMORY_HIGH` / `OOM_KILL` | WARN ≥300 MB / CRIT | 录制器内存（单元 `MemoryHigh=400M`） | 看趋势：anon 在 138 至 163 MB 之间浮动属正常；持续上涨再查 |
| `JOURNAL_VOLATILE` / `JOURNAL_BIG` | WARN | journal 又不持久了 / 超过 250 MB（上限 200 MB） | 检查 `/etc/systemd/journald.conf.d/50-hy-persistent.conf` 与 `/var/log/journal` |
| `COTENANT_DOWN` | WARN | `x-ui`、`xray` 或 `fail2ban` 没在运行 | 不是录制器的服务，但说明宿主机出了问题；别动它们，告诉机主 |
| `VERIFY_FAIL` | CRIT | 周验证发现完整性或连续性失败 | 看 `verify-*.txt`；先判断是数据损坏还是已知空洞（轮换、重启）未被账本解释 |
| `LOCAL_DISK_LOW` | WARN <40 GB，CRIT <15 GB | 本机放数据湖的盘 | 数据湖每天约 +0.6 GB；腾空间或把整个数据湖搬到更大的盘（改 `maint.json`） |
| `NO_RECENT_SUCCESS` | CRIT | 48 小时没有成功的运行，且这次也失败 | 计划任务没在跑或一直失败：看任务的“上次运行结果”与 `logs\` |

## 例行维保清单

- **每天（自动）**：上面的任务。我（或你）每次回到工作时先读 `logs\latest.txt`；`status.json` 的 `generated_utc` 超过 36 小时就说明任务没在跑。账本事件只看最近 24 小时：电脑关机超过一天时，那段时间里的告警不会出现在 findings 里，用 `hy_recorder report --lake <数据湖>` 看账本汇总，周验证也会列出空洞。
- **新增账本事件种类时**：先决定它是告警（加进 `maint.EVENT_FINDINGS`）还是常规心跳（加进 `ROUTINE_EVENTS`），`test_maint.py` 会拦住“探针在数、没人判”的种类。
- **每周**：读 `report-*.txt`（速率、体量、空洞、时钟）和 `verify-*.txt`。
- **每月**：宿主机磁盘与 `journalctl --disk-usage`；录制器内存与磁盘增长有没有偏离上个月；本机数据湖大小与剩余空间；计划任务仍在（`Get-ScheduledTask HengYuan-D0-Maintenance`）。
- **录制器代码有改动时**：先部署（`deploy/install.sh`，重启一次），再**刷新 `tool\` 快照**：

  ```bash
  cd <ops>\d0 && rm -rf tool.new && mkdir tool.new && git -C <repo> archive --format=tar <提交>:recorder | tar -x -C tool.new && mv tool tool.old && mv tool.new tool && rm -rf tool.old
  ```

  然后 `venv\Scripts\python.exe -m hy_recorder maint --config maint.json --force` 手动跑一次确认。
- **宿主机 journal**：已持久化，`/etc/systemd/journald.conf.d/50-hy-persistent.conf`（`SystemMaxUse=200M`、`MaxRetentionSec=1month`）；回滚 = 删这个文件、`systemctl restart systemd-journald`，可选 `rm -rf /var/log/journal`。

## 卸载

```powershell
Unregister-ScheduledTask -TaskName HengYuan-D0-Maintenance -Confirm:$false
```

数据湖与日志不会被删除。
