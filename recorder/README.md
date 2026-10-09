# hy-recorder：D0 只读公开行情录制器

只录制币安**公开**行情（S1：BTCUSDT 与 ETHUSDT 的现货深度与成交、USD-M 深度、聚合成交、标记价、强平通知；ETHUSDT 自 2026-10-07 起，D0-5a），并提供离线校验器。它不是"L2 已可用"的声明：只有校验器判为 `Verified` 的区间才可称 L2 可用，其余仅是原始观察。

## 它做什么、不做什么

- **只读**：唯一能构造出站请求的模块是 `hy_recorder/guard.py`，请求来自枚举出的 `(场所, 端点)` 与固定参数集；不存在读取凭据、签名、监听密钥、订单或用户数据端点的代码路径（`tests/test_guard_config.py` 扫描源码禁词，`tests/test_ledger_schema.py` 扫描事件字段）。进程无密钥、无监听端口、不读环境文件。
- **不做**：BTCUSDT 与 ETHUSDT 之外的标的、RPI 深度、COIN-M、bookTicker、Parquet 转换、历史回补、任何交易或账户操作。这些属于后续阶段（D0-5 的其余项）。
- **不得与交易节点共用机器与出口**：录制宿主是可牺牲负载；批量公开行情采集不得放在将来的交易节点上（蓝图 V-12）。

## 数据布局（详见 `DATA_DICTIONARY.md`）

```
<root>/raw/<场所>/<类别>/<YYYYMMDD>/<HH>-<段序号>.jsonl.zst   一段一个 zstd 帧，封存后不再改动
<root>/manifest/<YYYYMMDD>.jsonl   追加式：每个封存段的大小与 SHA-256；剪枝与驱逐也在此登记
<root>/ledger/<YYYYMMDD>.jsonl     控制面事件（连接、桥接、缺口、限流、磁盘…），与记录共用 (run, q) 全序
<root>/state.json  status.json  acks/  recovered/  reserve.bin
```

pandas、Polars、DuckDB 与 `zstd -dc` 都能直接读取封存段（`scripts/ecosystem_smoke.py` 覆盖）。

## 命令

```bash
python3 -m hy_recorder run --config /etc/hy-recorder/recorder.toml   # 录制，直到 SIGTERM/SIGINT，然后优雅封存
python3 -m hy_recorder verify --root <数据目录> [--json] [--no-hash]  # 离线校验；退出码 0 无失败，1 有失败
python3 -m hy_recorder report --root <数据目录> [--json]               # 运行报告：速率、体量与 MB/天、桥接耗时、缺口、时钟偏差
python3 -m hy_recorder status --root <数据目录>                        # 读 status.json
python3 -m hy_recorder config-check --config <配置>                    # 校验配置并列出推导出的连接
python3 -m hy_recorder pull --host <ssh 别名> --dest <本机目录>        # 操作员侧：拉取、校验、再回执
python3 -m hy_recorder maint --config <maint.json> [--force]           # 操作员侧：一次维保（pull、探测宿主机、检查、写状态；退出码 0/1/2 = 正常/WARN/CRIT）
```

## 不可协商的运行规则

1. **限流**：任何 REST 响应 429（403 WAF 同样处理）立即进入**至少 5 分钟深度休眠**（不短于 `Retry-After`）：期间零 REST（快照、时间探针、`exchangeInfo` 全停），已建立的 WebSocket 继续录制，重连间隔不小于 30 秒；一小时内再次触发则翻倍，封顶 2 小时；休眠截止时刻持久化，重启不会提前结束。HTTP 418 冻结所有面向币安的流量（REST 与 WebSocket 重连）直到封禁期满。录制宿主与代理共用出口地址，撞上 418 会连带影响同机业务，所以这条规则是防线，不是建议。
2. **桥接**：新代次缓冲到首个事件后立即请求一次代次起点快照（每标的 5 秒硬冷却，受权重预算约束），失败按 5 秒起、翻倍、封顶 5 分钟退避重试，**无次数上限**，直到桥接成功或该代次结束。不得跨越已判定的缺口宣称连续。
3. **磁盘自愈，严禁只停机不清理**：已回执且超过保留期（默认 48 小时）的段先清；仍低于告警线时清未回执的最老段（账本记 `EVICTED_UNACKED`，缺口可审计）；仍不足则保留期阶梯下调 48→24→12→6 小时；仅当 6 小时窗口下仍低于底线才停止写入。`pull` 是优先保全数据的通道，不是清理的前提。
4. **写入所有权**：接收路径只打时间戳并 `put_nowait`；唯一写入线程持有压缩、文件、清单与账本；队列满则丢新，丢弃以合并区间 `OVERRUN` 入账，任何序号空洞要么被账本解释、要么校验失败。
5. **保活**：`Type=simple`，进程内监控线程在事件循环停滞 30 秒（退出码 70）或写入线程停滞 60 秒（退出码 71）时自杀，由 systemd 拉起，下次启动恢复遗留 `.part`。

## 部署（Debian 12 / systemd 252 / Python 3.11，无 pip）

```bash
python scripts/build_bundle.py --out dist          # 可信构建机：取 requirements.lock 中的 cp311 manylinux 轮子并逐个核哈希
# 把 dist/hy-recorder-<摘要>.tar.gz{,.sha256} 与 deploy/ 复制到目标机，然后（root）：
bash deploy/install.sh hy-recorder-<摘要>.tar.gz [--enable]
```

- 发布目录 `/opt/hy-recorder/releases/<摘要>/` 是扁平目录（应用包与依赖并列），`current` 为符号链接；unit 用 `PYTHONPATH` 指向它，目标机无需 venv。摘要由内容决定（`hy_recorder/bundle.py`），构建可复现，CI 会构建两次并要求字节一致。
- `install.sh` 只创建：系统用户 `hyrec`、`/opt/hy-recorder`、`/etc/hy-recorder`、`/var/lib/hy-recorder`、`/etc/systemd/system/hy-recorder.service`；不碰其他服务、防火墙、DNS、网络配置，不重启。不带 `--enable` 时服务保持停止。
- 升级 = 装新包（自动切换 `current`，旧目标记入 `previous`）后 `systemctl restart hy-recorder`；回退 = 把 `current` 指回 `previous` 再重启。
- 回滚全部：`bash deploy/uninstall.sh [--purge-data]`（默认保留数据目录）。
- unit 的要点与理由见 `deploy/hy-recorder.service` 内注释：无 `CPUQuota`（硬限流会给接收时间戳加最长 40 ms 抖动）、无 `SocketBindDeny`（"无监听端口"用 `ss -ltnup` 验收）、资源只用 `CPUWeight=20`、`Nice=10`、`IOWeight=20`、`OOMScoreAdjust=500`、`MemoryHigh=400M`、`MemoryMax=600M`。

## 转存与回执

```bash
python -m hy_recorder pull --host tokyo-vps-8t --dest D:\data\hy-lake      # 本机运行，走你自己的 ssh 配置
python -m hy_recorder verify --root D:\data\hy-lake
```

`pull` 逐段流式取回、核对大小与 SHA-256、fsync 后原子落地，**之后**才在主机写 `acks/<sha256>`；校验失败的段不回执、不落地。本地清单描述"本地归档持有什么"：主机事后清理的段，本地照常保留。

定期维保（每日 pull、只读探测宿主机、阈值检查、每周完整 `verify`）由 `python -m hy_recorder maint --config maint.json` 完成，用 `tools/install_maint_task.ps1` 注册成 Windows 计划任务；布局、告警含义与处置见 [MAINTENANCE.md](MAINTENANCE.md)。

## 已知限制与残留风险

- **24 小时轮换**：约每 23.5 小时先断后连，以新代次显式登记（不回填）。关闭握手最多等 0.25 秒、重连不再随机睡眠，预期每条连接的数据缺口约 0.3 至 0.6 秒（待下一次自然轮换在账本里实测）；此前实测是 5.15 至 5.53 秒，其中 5.0 秒是旧的 5 秒关闭超时（Binance 不及时关闭连接）。真正无缝（先连后断）需要按 id 去重（成交 `t`、聚合成交 `a`、深度 `u`/`pu`）和验证器改动，暂不做。
- **开机后的时钟**：到 timesyncd 首次同步前（D0 宿主约 36 秒）宿主时钟可能偏数百毫秒。采集器不等同步，照常录制，`CLOCK_STATE` 标记该区间，`report` 列出。
- **解释器整体冻结**（C 扩展持有 GIL）时进程内监控线程也失效，只能靠外部检查或后续的 notify + 看门狗。
- **时间视角**：`t` 是宿主接收时间，不代表交易节点；时钟口径以流的 `t_recv − E` 分钟级最小值为主证据。
- **接收时间戳抖动**：unit 有意低优先级（`CPUWeight=20`、`Nice=10`），同机其它进程（包括你的 SSH 登录）占用单核时录制循环会被让路，实测最长约 0.17 秒，数据不丢（内核缓冲）但 `t` 会晚；`t_recv − E` 的高分位反映的正是这个。要更准的接收时间请用更高优先级或独占机器。
- **出口无配额隔离**：与同机代理共用地址限额，限流规则是防线而非隔离。
- **SHA-256 只防偶发损坏与传输错误**，本机对抗性篡改不在威胁模型内。
- **未回执数据可能被驱逐**：这是"严禁只停机不清理"的代价，每次驱逐都有清单与账本事件。

## 开发

```bash
cd recorder
python -m pip install -e ".[dev]"
python -m pytest -q        # 含本地假交易所的端到端测试（真实 socket 与写入线程）
ruff check .
```

压测与故障注入（在录制宿主上做，数字以实测为准，不要用开发机的数字）：

```bash
python tests/loadgen.py --mult 3 --seconds 120 --level 9        # 真实接收路径与写入线程，进程内合成交易所
mount -t tmpfs -o size=64m tmpfs /mnt/hy-tmpfs                   # 真实小文件系统上的保留阶梯与真 ENOSPC
HY_TEST_TMPFS=/mnt/hy-tmpfs python3 -m pytest tests/test_disk_pressure_tmpfs.py
```

`loadgen.py` 的合成交易所与录制器同进程，CPU 数字是录制器自身的上界；价格数量为随机数，压缩比是真实（相关性更强的）数据的下界。

CI：`.github/workflows/ci-recorder.yml`（py3.11 加锁定轮子；py3.13 加最新依赖；部署包可复现性）。
