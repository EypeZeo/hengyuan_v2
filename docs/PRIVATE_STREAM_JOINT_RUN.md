# 私网流联调手册（M5b-1）

给 Owner 的一页纸：怎么跑预检线束对 Testnet 的联调、日志里看什么、哪里坏了该怎么读。AI 会话**不运行**这一步（`CLAUDE.md`：AI 会话不处理真实凭据）；本手册只写你需要的命令和判据。

## 这次运行证明什么，不证明什么

- **证明**（如果 `joint-run criteria` 全是 yes）：用你的 Testnet HMAC 密钥，WebSocket API 的签名订阅（`userDataStream.subscribe.signature`）被交易所接受，`session.subscriptions` 恰好列出这一个订阅；屏障对账过（一次真实的清扫，带一次真实的 `GET /api/v3/account`）；
  启动恢复到 `Ready`；存储绑定到交易所报告的账户；订单路径的 Gate 2b 拿到真实令牌，一直走到 `OperatorNotConfirmed`。
- **不证明**：真实下单（这个进程不可能下单：提交端口是 mock，确认永不绑定，见 `preflight_probe.hpp`）；订单查询与成交回报的真实形状（运行里没有订单，清扫查询的是空集）；生产环境（线束只认 Testnet）。
- 回环测试证明“我们的客户端按我们理解的协议行事”；这次运行才有交易所一侧的证据。**首次通过：2026-11-07**（第 4 次尝试，见文末“首次联调记录”）。当时没有交易所证据的两个点，答案是：
  **L-39**（从未 `logon` 的连接上调 `session.subscriptions` 是否返回订阅列表）：返回，`verify_status=200`、`subscriptions_listed=1`——Testnet 上保留“恰好一个”的严格判定；
  **签名订阅需要什么权限**：带 USER_DATA 与 USER_STREAM 的密钥被接受（`subscribe_status=200`）；只有 USER_DATA 的密钥没有试过，官方文档说这个方法需要 USER_STREAM，所以按 USER_DATA + USER_STREAM 准备。
  生产端点没有证据。之后每次运行日志里仍然有这两个字段。

## 准备（一次）

1. 一把 **Testnet 的 HMAC 密钥**，权限开 **USER_DATA 与 USER_STREAM**（读取账户、订阅用户数据流）；不要给交易权限（TRADE）——进程不下单，给了也没用，给了就多一分风险。签名订阅是 USER_STREAM 方法（官方文档），只有 USER_DATA 的密钥应该会在订阅这一步被拒（`subscribe_status` / `subscribe_error`，见下表；这个情形没有试过，只来自文档）。2026-11-07 通过的那次联调用的就是带这两个权限的密钥。Ed25519 密钥不行（这条路径是 HMAC 签名）。**别用 R6 探针那把只读密钥**：它没有 USER_STREAM。
2. 在**仓库根目录**放 `.env`（`./.env` 相对当前目录读取），两个变量名固定：

   ```
   HENGYUAN_BINANCE_TESTNET_API_KEY=<你的 Testnet API key>
   HENGYUAN_BINANCE_TESTNET_SECRET=<你的 Testnet secret>
   ```

   载入器只认这两个名字；文件权限过宽会被拒绝（`PermissionTooWide`，Linux 下 `chmod 600 .env`）。**不要把 `.env` 的内容贴给任何人**，包括 AI 会话。
   **两套变量名，别混**：R6 探针（`tools/r6_testnet_probe.py --env-file`）与 `hy_operator_resolve --credentials-file` 读的文件用 `HENGYUAN_OPERATOR_TESTNET_API_KEY` / `HENGYUAN_OPERATOR_TESTNET_API_SECRET`；这个线束只认上面两个名字。
   载入器整份文件拒绝任何其他变量名（包括那一对）、没有 `=` 的行和重复的键，日志里是 `ERROR: failed to load .env (status=6)`（`ParseError`）。所以线束用单独的一份 `.env`；R6 的那份改名为 `.env.operator` 留着给探针，两份不要合并。
3. 构建（`HY_BUILD_DEMO=ON`，见 `CLAUDE.md` 的 MSVC 运行手册）。目标 `live_submit_preflight_harness`，Windows 下在 `native/build-msvc/Release/`。

## 运行

**Windows 上先做这一步**（同一个 PowerShell 窗口里，每开一个新窗口都要再做一次）：这台机器上 vcpkg 的 OpenSSL 编进去的默认 CA 目录 `C:\Program Files\Common Files\SSL` 不存在，不设的话每次 TLS 握手都失败（日志里是 `ERROR: sync_clock() failed.` 一类）。
把 `SSL_CERT_FILE` 指向 Git for Windows 自带的 Mozilla 根证书包（`cmd /c` 会继承环境变量）：

```
$env:SSL_CERT_FILE = 'C:\Program Files\Git\mingw64\etc\ssl\certs\ca-bundle.crt'
```

在仓库根目录，一条命令（Windows 用 `cmd` 重定向得到纯文本日志；PowerShell 5.1 的 `>` 会写成 UTF-16）：

```
cmd /c "native\build-msvc\Release\live_submit_preflight_harness.exe native\tests\fixtures\strategy_spec\sma_crossover_btc_1h.spec.toml 240 1h > joint_run.log 2>&1"
```

参数是 `<spec.toml> [秒数=60] [周期=1h]`；spec 必须是 BTCUSDT/1h（线束的订阅固定如此）。240 秒够走完：连接、订阅、校验、清扫、恢复、深度同步、K 线暖机、启动探针。
`Ctrl+C` 可以提前结束，报告照样打印；日志是逐行写出的（stdout 不缓冲），进程异常终止时已经打印的行都在文件里。第一次只要 240 秒；**第二次**可以跑得更久（几分钟到半小时），中途断一次网看重连（每次重连的固定权重成本只有 6）。

## 日志里看什么

按出现顺序：

| 行 | 期望 |
| :-- | :-- |
| `Dry-run drill ... PASSED`（四行）与 `Dry-run evidence earned: ... live_ready=true` | 四个演练都通过 |
| `Exchange account uid read (N digits; ...)` | N 约 19（R6 探针量过：Testnet 的 uid 是 19 位）；**值不打印** |
| `Exchange account read: entries=… represented=… zero_in_scope=… out_of_scope=… (scope: BTC, USDT; ...)` | `represented=2`（BTC 与 USDT 都在账户里）。`entries` 是账户答复的条目总数：Testnet 给每个账户发了所有资产（首次联调是 517 个，全部非零），线束只声明 BTC 与 USDT，其余条目只计数、不转换（L4 规范 §4.3.1）；`entries = represented + zero_in_scope + out_of_scope` |
| `Symbol rules (as the exchange reports them): price_scale=… qty_scale=… \| tick=… min_price=… max_price=… \| step=… min_qty=… max_qty=… \| min_notional=… (balance scale 8)` | 交易所对 BTCUSDT 的过滤器，已换成整数 tick（公开数据，没有凭据）。价格 = `price_ticks / 10^price_scale`，数量 = `qty_ticks / 10^qty_scale`，`min_notional` 在余额刻度（8 位小数）上。首次联调的 Testnet：`price_scale=2 qty_scale=5`，`min_qty=1`（0.00001 BTC），`min_notional=500000000`（5.0 USDT） |
| `Store binding: BoundNew` | 新的临时存储第一次绑定 |
| `[t=... ms] private stream: supervisor=... generation=... verified=... \| barrier=... \| recovery=...` | 每次状态变化一行。顺序应是：`supervisor=Connecting` → `Connected ... verified=yes` → `barrier=Sweeping` → `barrier=Reconciled`，`recovery=Reconciling` → `Ready` |
| `Private stream generation N (ended): ...` | 只在一代结束时出现。一代都没有结束过是好事 |
| `[startup probe] ...` 随后 `[preflight probe #1] (startup) OPEN BUY price=... qty=... -> gate=OperatorNotConfirmed` | 订单路径走到确认门。探针的数量按**交易所的最小名义金额**定：满足 LOT_SIZE 的最小数量里，第一个名义金额（价格 × 数量）够 `min_notional` 的（首次联调的价格 75 920.32 上是 `qty=7`，即 0.00007 BTC ≈ 5.31 USDT；最小单位 0.00001 BTC 只有 0.76 USDT，会被拒） |
| `=== Private stream joint-run report ===` 以及最后一行 `joint-run criteria: reconciled_once=yes account_refreshed_by_sweep=yes recovery_ready=yes probe_reached_OperatorNotConfirmed=yes all_met=yes` | 全 yes |

报告里还有：`barrier`（各计数）、`sweep`（作业与泵）、`account`（持有者 / 刷新器 / REST 的计数与 `last_error`）、`supervisor`、`factory`、最后一代的 `Private stream generation N (last): ...`（订阅应答与列表的状态码、错误码、`retry_after_ms`）、
`readiness`（令牌签发数与按原因的拒绝数）、`durable`、`late_fill`、`pipeline`、`probes`。

## 哪里坏了怎么读

| 看到 | 意思 | 怎么办 |
| :-- | :-- | :-- |
| 启动就退出：`ERROR: failed to load .env (status=6)` | `ParseError`：`.env` 里有载入器不认的变量名（最常见：R6 探针的 `HENGYUAN_OPERATOR_TESTNET_*`）、没有 `=` 的行或重复的键。注释行没关系 | 只留 `HENGYUAN_BINANCE_TESTNET_API_KEY` / `HENGYUAN_BINANCE_TESTNET_SECRET` 两个变量；不要把内容贴出来，只贴名字（不含值）和这一行 |
| 启动就退出：`ERROR: sync_clock() failed.` 或每次握手都失败（Windows） | 没有受信任的根证书（OpenSSL 的默认 CA 目录不存在） | 见上面“运行”里的 `SSL_CERT_FILE` |
| 启动就退出：`could not read the account uid ... PrivateRestError=MalformedResponse` | 账户答复被解析器整份拒绝。首次联调是 Testnet 账户有 517 个非零资产（含多字节名字），快照放不下；#233 之后线束只声明 BTC 与 USDT，其余只计数 | 如果 #233 之后还出现：运行 `python tools/r6_account_shape_probe.py --env-file .env.operator`（只读，只打印字段名 / 类型 / 计数），把输出贴回来 |
| `WARN: periodic clock resync failed, retrying sooner`（偶发） | 周期性时钟同步（每 60 秒）失败一次，10 秒后重试；这条线路上 REST 的 TLS 握手大约每 25 次失败 1 次 | 偶发正常；每次都出现才是问题（线束还不打印错误名） |
| `account ... failed=1 last_error=TlsHandshake`（偶发） | 同上，账户刷新器吸收了一次失败（首次联调两次运行都是 1 / 26） | 偶发正常；`stale` 增长或 `failed` 占多数才是问题 |
| 启动就退出：`ERROR: could not read the account uid ... PrivateRestError=HttpStatus` | 交易所拒绝了 `GET /api/v3/account`：密钥不对、不是 Testnet 的、没有读取权限，或 IP 受限 | 检查密钥；报告 `last_error` |
| `... PrivateRestError=ClockNotFresh` / `ClockSkewTooLarge` | 时钟同步失败或本机时钟偏得太远 | 校时后重跑 |
| `stage=subscribe_rejected`，`subscribe_status=4xx`，`subscribe_error=-2015` 一类 | 交易所拒绝了签名订阅：密钥类型 / 权限 / IP / 签名 | **把整行贴回来**，这是权限问题的证据 |
| `subscribe_status=200` 但 `stage=subscriptions_rejected`，或 `subscriptions_mismatch`（`subscriptions_listed` 不是 1） | **L-39**：未登录的连接上 `session.subscriptions` 与我们的假设不符 | **贴回来**；进程失败关闭（永远不就绪），设计要改 |
| `stage=stale_subscribe_request` 偶尔出现 | 握手比 4 秒慢，签名过期，下一代重新签 | 偶尔正常；反复出现是网络慢 |
| `stage=resolve` / `connect` / `ssl_handshake` / `ws_handshake` | 网络、证书或主机 | 检查到 `ws-api.testnet.binance.vision:443` 的连通 |
| `retry_after_ms` 非 0，或 `factory ... refused_ban` 增长 | 交易所（HTTP 418 / 429）让这个 IP 等；工厂在那个时刻之前不再连 | 别再重跑，等过那个时刻；贴回来 |
| `ALERT: the private stream gave up (TooManyFailures)` | 连续十次失败 | 看它前面的 `Private stream generation N (ended)` 各行 |
| `account ... not_sent` 很多，`last_error=SigningFailed` | 取账户的线程没有凭据（线程所有权被破坏） | 贴回来：这不该发生 |
| `barrier=Sweeping` 一直不变，`sweep ... reports_account_stale` 增长 | 清扫的账户步骤没有送到 | 看 `account` 行里 `rest ... failed` / `not_sent` |
| `readiness: ... refused_StartupNotReady=...` 一直增长 | 启动恢复没有 Ready，或流不在线 | 看 `recovery:` 行的 `state` 与 `degraded_reason`，`store binding` 行，`supervisor:` 行 |
| 探针行 `-> gate=PreTradeFailed`，随后 `WARN: expected gate=OperatorNotConfirmed, got PreTradeFailed / reason=<名字>` | 订单路径在预交易检查被拒，`reason` 就是检查的名字：`BelowMinNotional`（价格 × 数量低于交易所的最小名义金额；探针本该按它定量，对照 `Symbol rules` 行）、`InsufficientBalance`（买入的计价资产可用余额不够）、`AccountCannotTrade`（账户 `canTrade` 为 false）、`BalanceStale` / `BalanceFromFuture`（账户快照的时间戳与线束的时钟不符）、`ExceedsSingleOrderCap` / `ExceedsTotalExposureCap`（线束给的 10 USDT 上限）、`Ok`（链在检查之前就停了：规则或账户没接上） | 贴回这一行和 `Symbol rules` 行 |
| `FATAL: the store binding was refused (...)` | 新建的临时存储不该被拒绝 | 贴回来 |

## 首次联调记录（2026-11-07 通过）

| 次 | 日期 | 现象 | 原因 | 修复 |
| :-- | :-- | :-- | :-- | :-- |
| 1 | 2026-11-06 | `ERROR: failed to load .env (status=6)` | `.env` 里是 R6 探针的变量名（`HENGYUAN_OPERATOR_TESTNET_*`），线束只认 `HENGYUAN_BINANCE_TESTNET_API_KEY` / `_SECRET`，载入器整份文件拒绝其他名字 | 线束用单独的 `.env`；R6 的那份改名为 `.env.operator` |
| 2 | 2026-11-06 | `ERROR: could not read the account uid ... PrivateRestError=MalformedResponse` | Testnet 账户有 517 个非零资产、含多字节名字，快照放不下，规范 §4.3 要求整份失败 | PR #233：线束声明 BTC 与 USDT，其余条目只计数（L4 规范 §4.3.1） |
| 3 | 2026-11-07 | 三次启动探针 `gate=PreTradeFailed`（四个判据三个满足） | 探针数量是最小单位 0.00001 BTC = 0.76 USDT，低于 BTCUSDT 的最小名义金额 5 USDT | PR #234：数量按最小名义金额定；日志打印 `reason=` 与 `Symbol rules` |
| 4 | 2026-11-07 | `joint-run criteria: ... all_met=yes`；`OPEN BUY price=7602001 qty=7 -> gate=OperatorNotConfirmed` | — | — |

Windows 的 CA 包（`SSL_CERT_FILE`）是第 1 次之后预先查出来的，没有成为一次失败。第 4 次的密钥带 USER_DATA 与 USER_STREAM 权限。第 4 次大约 150 秒：账户 25 次送达（26 次尝试中 1 次 `TlsHandshake` 失败），清扫 1 次完成，深度 1116 个事件，K 线回填 52 根。

## 贴回什么

`joint_run.log` 里从 `=== Live Submit Preflight Harness (testnet) ===` 到最后的全部内容。日志里没有密钥、没有 secret；账户 uid 只有位数。`.env` 不要贴。

## 安全边界（再说一遍）

- 提交端口永远是 mock；`ctx.confirmation` 永不绑定；`test_submit_readiness_guard.cpp` 把这两条钉成源码级守卫。
- 进程对交易所做的写操作只有 WebSocket 的订阅（`userDataStream.subscribe.signature` 与无签名的 `session.subscriptions`）；REST 全是 GET（时钟、exchangeInfo、账户；清扫的订单查询在空存储上没有对象）。
- 每个探针在确认门之前往**临时**存储写一帧 Intent；存储在退出时删除。
