# D0 数据字典（S1：BTCUSDT）

D0-0 边界冻结产物。每条协议断言都给出官方来源与核验日期，并有真实样本：`tests/fixtures/`（2026-09-29 于录制宿主抓取，UTC 14:25 至 14:31，每流约 5 分钟）。
状态取值：`VERIFIED_BY_FIXTURE`（官方文档与真实样本一致）、`EXTERNAL_PROTOCOL_VERIFICATION_REQUIRED`（无真实样本或与文档不一致，不得据此验证）。

## 1. 记录封装与文件

- 路径：`<root>/raw/<场所>/<类别>/<YYYYMMDD>/<HH>-<段序号>.jsonl.zst`（封存前带 `.part` 后缀）。类别：`depth`、`trade`、`market`（WebSocket 数据），`snapshot`（深度快照响应），`ref`（参考数据，每日 `exchangeInfo`；流名 `场所:exchangeInfo[:标的]`，`g` 恒为 0，不属任何连接代次）。
- **一段一帧**：每个封存段恰为一个 zstd 帧（带帧校验和）。python-zstandard 的流式读取默认只读第一帧，多帧文件会被读半截，故不允许多帧。
- 每行一个标准 JSON 对象，键序固定：
  `{"v":1,"k":"m","r":运行号,"q":序号,"g":代次,"t":墙钟微秒,"m":单调微秒,"s":"场所:流名","p":"消息文本"}`
  - `k`：`m` 为 WebSocket 消息，`s` 为 REST 快照响应体。
  - `r`、`q`：`(r, q)` 是全序。`r` 每次进程启动加一且永不回退，`q` 与账本事件共用同一计数器。
  - `t`、`m`：整数微秒（避免 float64 精度陷阱）。
  - `p`：库交付的**消息文本，作为 JSON 字符串**；`json.loads(行)["p"]` 得到与收到时完全相同的文本。二进制消息改用 `"b"`（base64）与 `"x":"binary"`。
- **保真语义**：应用层 WebSocket 消息载荷保真。保存的是 WebSocket 库交付的已重组消息，不是线路帧，不含控制帧。
- **为何是字符串而不是嵌套对象**：嵌套对象使 Polars 在混合流文件上报结构超类型错误、使 DuckDB 因载荷键 `e` 与 `E` 大小写重名而失败；改为字符串后 pandas、Polars、DuckDB 均可直接读取，实测压缩体积仅多约 1.4%（真实帧，zstd 9 级）。
- 读取：pandas `read_json(path, lines=True, compression="zstd")`；Polars `read_ndjson(path)`；DuckDB `read_json(path, format='newline_delimited')`；`zstd -dc path`。载荷再用各自的 JSON 函数解析。
- 完整性：清单 `manifest/<日期>.jsonl` 追加式登记每个封存段的字节数与 SHA-256；SHA-256 只防偶发损坏与传输错误，本机对抗性篡改不在威胁模型内。

## 2. 流清单

### 2.1 现货深度 `spot:btcusdt@depth@100ms`

| 项 | 内容 |
| :--- | :--- |
| 端点 | `wss://stream.binance.com:9443/stream?streams=btcusdt@depth@100ms`（连接 A） |
| 官方来源 | 现货仓库 `web-socket-streams.md` 的增量深度流与“How to manage a local order book correctly”，2026-09-29 拉取 |
| 字段 | `e` `E` `s` `U` `u` `b` `a`；`b`、`a` 为“价格、数量”字符串对，数量为 0 表示删除该档 |
| 序号语义 | 丢弃 `u` 不大于快照 `lastUpdateId` 的事件；首个保留事件须 `U ≤ lastUpdateId+1 ≤ u`；此后 `U` 等于前一 `u+1`，大于即漏事件 |
| 实测（300 秒） | 3000 帧，10.0 帧/秒；序号链 0 处违规；载荷 p50 682 B、p99 9.5 KB、最大 22 KB；`t_recv − E` 最小 1.4 ms、p50 1.9 ms、p99 14.1 ms |
| 不可推导 | 不含订单级标识（L2，不是 L3）；快照价档有上限，上限之外的档位在变化前不可知 |
| 存活预期 | 最长静默 5 s（否则重连该连接）；订阅后首帧期限 20 s |
| 样本 | `spot_depth_bridge_frames.jsonl`、`spot_depth_snapshot_l100.json` |
| 状态 | `VERIFIED_BY_FIXTURE` |

### 2.2 现货成交 `spot:btcusdt@trade`

| 项 | 内容 |
| :--- | :--- |
| 端点 | `wss://stream.binance.com:9443/stream?streams=btcusdt@trade`（连接 B） |
| 字段 | `e` `E` `s` `t`（成交号）`p` `q` `T` `m`（买方是否做市方）`M` |
| 序号语义 | 成交号 `t` 逐代次严格加 1 |
| 实测（300 秒） | 10,699 笔，35.7 笔/秒；成交号 0 处断裂、0 处非递增；载荷 168 B；`t_recv − E` p50 2.7 ms、p99 115 ms |
| 存活预期 | 最长静默 30 s；订阅后首帧期限 30 s |
| 样本 | `spot_trade_frames.jsonl` |
| 状态 | `VERIFIED_BY_FIXTURE` |

### 2.3 USD-M 深度 `usdm:btcusdt@depth@100ms`

| 项 | 内容 |
| :--- | :--- |
| 端点 | `wss://fstream.binance.com/public/stream?streams=btcusdt@depth@100ms`（连接 C，**须走 `/public`**） |
| 官方来源 | 衍生品全文变更日志 2026-03-05、《Important WebSocket Change Notice》《Connect》《How to manage a local order book correctly》页，2026-09-29 拉取 |
| 字段 | `e` `E` `T` `s` `ps` `U` `u` `pu` `b` `a` |
| 序号语义 | 丢弃 `u` 小于快照 `lastUpdateId` 的事件；首个保留事件须 `U ≤ lastUpdateId` 且 `u ≥ lastUpdateId`；此后 `pu` 等于前一 `u`。**与现货规则不同，不得互相外推** |
| 实测（300 秒） | 2942 帧，9.8 帧/秒；`pu` 链 0 处违规；载荷 p50 2.1 KB、p99 21 KB、最大 35 KB；`t_recv − E` p50 1.9 ms |
| 不可推导 | 不含订单级标识；不含 RPI 订单（RPI 有独立流，首轮不录） |
| 存活预期 | 最长静默 5 s；订阅后首帧期限 20 s |
| 样本 | `usdm_depth_bridge_frames.jsonl`、`usdm_depth_snapshot_l100.json` |
| 状态 | `VERIFIED_BY_FIXTURE` |

### 2.4 USD-M 聚合成交 `usdm:btcusdt@aggTrade`

| 项 | 内容 |
| :--- | :--- |
| 端点 | `wss://fstream.binance.com/market/stream?streams=btcusdt@aggTrade/btcusdt@markPrice@1s/!forceOrder@arr`（连接 D，**须走 `/market`**） |
| 字段 | `e` `E` `a`（聚合成交号）`s` `p` `q` `nq` `f` `l` `T` `m` `st` |
| 序号语义 | 聚合成交号 `a` 逐代次严格加 1。**`f`/`l` 区间首尾相接不是判据**：4,550 帧中有 32 处 `f` 不等于前一 `l+1`，故仅作信息计数 |
| 实测（300 秒） | 4,550 帧，15.2 帧/秒；`a` 0 处断裂；载荷约 205 B |
| 存活预期 | 最长静默 30 s；订阅后首帧期限 30 s |
| 样本 | `usdm_aggtrade_frames.jsonl` |
| 状态 | `VERIFIED_BY_FIXTURE` |

### 2.5 USD-M 标记价 `usdm:btcusdt@markPrice@1s`

| 项 | 内容 |
| :--- | :--- |
| 端点 | 同连接 D（`/market`） |
| 字段 | `e` `E` `s` `p` `ap` `P` `i` `r`（资金费率）`T`（下次资金费时间）`st`；字段含义以官方文档为准，此处只确认存在 |
| 节拍 | `E` 间隔 995 至 1005 ms（300 帧）；`E` 为整秒量化，故 `t_recv − E` 不适合作时延（实测 p50 57 ms） |
| 判据 | 相邻 `E` 间隔大于 3.5 s 判缺秒（FAIL，可被账本解释），1.5 至 3.5 s 只计数 |
| 存活预期 | 最长静默 5 s；订阅后首帧期限 20 s |
| 样本 | `usdm_markprice_frames.jsonl` |
| 状态 | `VERIFIED_BY_FIXTURE` |

### 2.6 USD-M 强平快照 `usdm:!forceOrder@arr`

| 项 | 内容 |
| :--- | :--- |
| 端点 | 同连接 D（`/market`） |
| 字段 | `e:"forceOrder"` `E` `o:{s S o f q p ap X l z T ps st}` |
| 语义 | **通知流，不是全量强平真值**：每 1000 ms 窗口内每个标的至多推送一单，自 2026-04-14 起由“最新一单”改为“最大一单”（`5.7` 的 P-11）。无法据此恢复清算总量 |
| 实测（300 秒） | 66 笔（全市场），事件时间单调 |
| 判据 | 仅校验字段齐备与事件时间不倒退；录制数据均在语义生效日之后 |
| 存活预期 | 无（可长时间静默） |
| 样本 | `usdm_forceorder_frames.jsonl` |
| 状态 | `VERIFIED_BY_FIXTURE` |

### 2.7 未录制或未证实

| 流 | 状态 |
| :--- | :--- |
| USD-M RPI 深度 `@rpiDepth@500ms`（属 `/public`） | `EXTERNAL_PROTOCOL_VERIFICATION_REQUIRED`：官方接口目录只给出流名与“含 RPI 订单、500 ms”，无序号语义与真实样本，首轮不录 |
| ETH 与更多标的、bookTicker、COIN-M | 不在首轮范围 |

## 3. REST（仅公开、仅 GET）

| 用途 | 现货 | USD-M | 权重（实测） |
| :--- | :--- | :--- | :--- |
| 深度快照 | `GET /api/v3/depth?symbol&limit` | `GET /fapi/v1/depth?symbol&limit` | 现货 limit 100 为 5、1000 为 50；USD-M limit 100 为 5、1000 为 20 |
| 交易所信息 | `GET /api/v3/exchangeInfo?symbol` | `GET /fapi/v1/exchangeInfo` | 现货 20；USD-M 1（响应约 1.1 MB） |
| 服务器时间 | `GET /api/v3/time` | `GET /fapi/v1/time` | 1 |

- 快照响应：现货 `lastUpdateId` `bids` `asks`；USD-M 另有 `E` `T`。权重头 `x-mbx-used-weight-1m` 为该出口地址的已用权重。
- 限额：现货 6000/分钟，USD-M 2400/分钟（按出口地址计）。采集器自限不超过 5%，并按 429 深度休眠规则处理限流（见 README）。
- 不带任何密钥头，不访问签名或订单端点；采集器代码中不存在读取凭据与签名的路径。

## 4. 连接与保活

| 场所 | 有效期 | 服务端心跳 | 入站消息上限 |
| :--- | :--- | :--- | :--- |
| 现货 | 24 小时 | 每 20 s ping，1 分钟内须回 pong | 5 条/秒 |
| USD-M | 24 小时 | 每 3 分钟 ping，10 分钟内须回 pong | 10 条/秒 |

- 库自动回复 pong；采集器不发客户端 ping，也不发订阅消息（用 URL 订阅）。
- 24 小时前主动轮换连接（约 23.5 小时），新代次重新桥接。

## 5. 时间口径

- `t` 为录制宿主接收时间，仅代表宿主视角，不代表交易节点。录制宿主到流服务器约 3 ms，`t_recv − E` p50 约 2 ms；`E` 为交易所事件时间。
- 时钟偏移以流的 `t_recv − E` 分钟级最小值与分位数为主证据；REST `/time` 每分钟一次为辅（REST 路径往返约 140 ms，不足以单独证明偏移）。
- 全序由 `(r, q)` 给出，不依赖墙钟。

## 6. 校验规则（`hy_recorder verify`）

- 完整性：清单登记的段必须存在、大小与 SHA-256 一致、恰为一个完整 zstd 帧；未登记的封存段判失败；`.part` 只作信息。
- 序号：`(r, q)` 在每个类别内严格递增；每个运行内 `q` 的空洞必须被账本的 `OVERRUN` 区间解释，或位于未正常结束运行的末尾 10 秒内（丢失的 fsync 窗口）。
- 深度：桥接状态机 `UNVERIFIED → BUFFERING → BRIDGING → VERIFIED → GAP`；缺口终止该代次的可用性，不得由后续快照回填；首个桥接点之前仅是原始观察。
- 成交、聚合成交、标记价、强平：见各流“判据”。
- 账本互验：离线推得的缺口必须有对应账本事件（深度缺口 `GAP_DETECTED` 的 `bad_q`、丢弃 `OVERRUN` 的序号区间），否则判失败；每个连接代次必须有 `WS_OPEN`。
- 退出码：0 无失败，1 有失败，2 无法读取。

## 7. 账本事件

每个事件含 `run`、`q`、`t`、`m`、`k`（信封，字段不得覆盖；若调用方误传同名字段，写入时改存为 `x_<名>` 并加 `schema_clash`，结构性测试保证包内没有此类调用）。`q` 与记录共用同一计数器，是全序的一部分：**每个事件必须用自己的 `q`**，事件描述某条记录时用 `rec_q` 指向它。

| 类别 | 事件与主要字段 |
| :--- | :--- |
| 进程 | `PROC_START`（`version` `pid` `prev_clean` `state_recovered` `fingerprint` `config`）、`HOST_PROFILE`（内核、CPU 数、内存、库版本；不含主机名与地址）、`PROC_STOP`（`reason` `write_errors` `dropped`）、`SEGMENT_RECOVERED`、`RECOVERY_FAILED`、`STATE_RECOVERED`、`TASK_CRASH`（`task` `error`） |
| 连接 | `WS_OPEN`（`conn` `gen` `streams` `url_path` `connect_ms` `dns_ms`）、`WS_CLOSE`（`conn` `gen` `reason` `frames` `duration_s`）、`WS_CONNECT_FAIL`（`conn` `error` `status`）、`DNS_SLOW`、`SUBSCRIBED_NO_DATA`（订阅后首帧期限内无数据）、`STREAM_STALL`（有过数据后静默超限）、`UNEXPECTED_STREAM`、`BAD_FRAME`（`reason` `bad_q`，限频） |
| 深度桥接 | `SNAPSHOT`（`stream` `gen` `trigger` `status` `weight` `last_update_id` `rec_q` `sha256` 及请求起止单调时钟）、`SNAPSHOT_FAIL`（`backoff_s` `reason`）、`SNAPSHOT_STALE`、`SNAPSHOT_DEFERRED`、`SNAPSHOT_DISCARDED`（代次已变）、`BRIDGE_OK`（`gen` `bridge_q` `L` `U` `u`）、`GAP_DETECTED`（`stream` `gen` `bad_q` `rule` `expected` `got`） |
| REST 与限流 | `CLOCK_PROBE`（`venue` `server_ms` `rtt_us` `offset_ms`）、`REFDATA`（`stream` `bytes` `rec_q`）、`REST_FAIL`（`endpoint` `reason` `status`）、`RATE_LIMIT`（`status` `sleep_s` `level` `during_sleep`）、`BAN`（418）、`GEO_BLOCK_SUSPECT`（451） |
| 队列与磁盘 | `OVERRUN`（`cls` `dropped` `first_q` `last_q`，合并区间）、`WRITE_ERROR`、`DISK_WARN`、`DISK_STOP`、`DISK_RESUMED`、`RESERVE_RELEASED`、`PRUNED_ACKED`、`EVICTED_UNACKED`（未回执即被驱逐，附时间范围与哈希）、`RETENTION_STEPDOWN`、`RETENTION_EXHAUSTED`、`RETENTION_FAILED`、`PRUNE_FAILED`、`LOOP_LAG` |

## 8. 体量实测（S1，300 秒样本外推，UTC 14:25 至 14:31）

| 流 | 帧/秒 | 原始 KB/秒 | zstd 9 级压缩比 | 压缩后 MB/天 |
| :--- | ---: | ---: | ---: | ---: |
| 现货深度 | 10.0 | 13.2 | 8.2 | 约 140 |
| 现货成交 | 35.7 | 7.6 | 28.7 | 约 23 |
| USD-M 深度 | 9.8 | 33.9 | 5.8 | 约 510 |
| USD-M 市场类（聚合成交、标记价、强平） | 16.4 | 4.1 | 13.7 | 约 26 |
| 合计 | 72 | 58.8 | — | **约 700 MB/天** |

- 这是单个时段的样本，行情活跃时会更高；D0-4 的 24 小时实测取代它。快照与每日 `exchangeInfo` 相对上表可忽略。
- 12 GB 可用空间、48 小时保留下限可容纳约 6 GB/天。
