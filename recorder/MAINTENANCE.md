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
2. 只读探测宿主机（`hostprobe.py` 经 ssh 标准输入送过去，不写任何东西）：服务状态与重启数、磁盘、保留与驱逐、未回执积压、最近 24 小时账本事件、时钟同步、内存、journal、同机服务。
3. 本地空间检查。
4. **每 7 天**对整个数据湖做一次完整 `verify`（含哈希）并保存 `report`。
5. 写 `logs\latest.txt`（给人看）、`logs\status.json`（给程序看）、当次完整日志；退出码 0 正常、1 WARN、2 CRIT（计划任务的“上次运行结果”就是它）。

日志只保留 60 天，维保不会删除任何数据。

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
| `CLOCK_UNSYNCED` / `CLOCK_STALE` | WARN | 时钟长时间未同步 / 超过 3 小时没有 NTP 交换 | 宿主机 `timedatectl`、`systemctl status systemd-timesyncd`；期间的 `t` 与 `CLOCK_PROBE` 偏移不可信 |
| `MEMORY_HIGH` / `OOM_KILL` | WARN ≥300 MB / CRIT | 录制器内存（单元 `MemoryHigh=400M`） | 看趋势：anon 在 138 至 163 MB 之间浮动属正常；持续上涨再查 |
| `JOURNAL_VOLATILE` / `JOURNAL_BIG` | WARN | journal 又不持久了 / 超过 250 MB（上限 200 MB） | 检查 `/etc/systemd/journald.conf.d/50-hy-persistent.conf` 与 `/var/log/journal` |
| `COTENANT_DOWN` | WARN | `x-ui`、`xray` 或 `fail2ban` 没在运行 | 不是录制器的服务，但说明宿主机出了问题；别动它们，告诉机主 |
| `VERIFY_FAIL` | CRIT | 周验证发现完整性或连续性失败 | 看 `verify-*.txt`；先判断是数据损坏还是已知空洞（轮换、重启）未被账本解释 |
| `LOCAL_DISK_LOW` | WARN <40 GB，CRIT <15 GB | 本机放数据湖的盘 | 数据湖每天约 +0.6 GB；腾空间或把整个数据湖搬到更大的盘（改 `maint.json`） |
| `NO_RECENT_SUCCESS` | CRIT | 48 小时没有成功的运行，且这次也失败 | 计划任务没在跑或一直失败：看任务的“上次运行结果”与 `logs\` |

## 例行维保清单

- **每天（自动）**：上面的任务。我（或你）每次回到工作时先读 `logs\latest.txt`；`status.json` 的 `generated_utc` 超过 36 小时就说明任务没在跑。
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
