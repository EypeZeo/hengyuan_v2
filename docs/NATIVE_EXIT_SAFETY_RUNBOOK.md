# Native Exit-Safety Operator Runbook

This is a pre-live operational runbook for the native execution kernel's exit-safety path (`native/include/hengyuan/exit_safety.hpp`). It is not a live-ready stamp and does not authorize authenticated trading — see `CLAUDE.md`'s non-negotiable boundary section. Modeled on `hengyuan/docs/RUNBOOK_LIVE_PREP.md`'s structure (preconditions / stop conditions), adapted for the native kill-switch/exit-safety path specifically.

## 1. A correction to the original ask

This runbook was requested to make "manual Binance-app cancel/close" executable "in under 1ms with human-machine collaboration." That's not achievable and I'm not going to write a document that claims it is:

- The **software** side of kill-switch trigger (halting new order generation, entering `KillState` non-Normal, refusing `PostKillAction::NewOrder`/`Rearm`/`AutoResume`) is genuinely sub-millisecond — that part of `exit_safety.hpp`'s policy logic already is that fast.
- The **human** side — noticing an alert, unlocking a phone, opening the Binance app, navigating to open orders, confirming a cancel — realistically takes **15 seconds to a few minutes** under calm conditions, and can take considerably longer under 2FA prompts, poor network, or a genuinely stressful incident. No runbook changes that; treating "1ms" as an achievable target for a human phone action would give false confidence during a real incident, which is a real safety issue, not just an inaccurate document.

What the software *can* guarantee in sub-millisecond time is: no new orders get placed, no auto-rearm happens, and the system stops making things worse while the human responds. What the human provides is: the decision and the actual cancel/close action, on their own timescale.

## 2. What triggers exit-safety, and what happens automatically

When `KillSwitch` state moves out of `Normal` (see `kill_switch.hpp`), `exit_safety.hpp`'s `check_post_kill_permission()` immediately and automatically:

| Action | Permission after kill |
|---|---|
| `ReadOnlyQuery` (GET /account, GET /order) | Always allowed |
| `ExitOnlyCancel` (DELETE /order on a known open order) | Requires operator approval — **not automatic** |
| `NewOrder` (POST /order) | Always forbidden |
| `Rearm` | Always forbidden — requires the re-arm ceremony, a separate human action |
| `AutoResume` | Always forbidden (ADR-015) |

`PrivateStreamPipeline` can stage a single cancellation request after `Triggered` or `Latched`. Both staging and claim require an active approval bound to the environment, account, store, runtime activation and symbol. `determine_emergency_policy()` returns `OperatorManualOnly` for account-wide cancellation.

### Owner approval for a recovered order

The control owner provisions a trusted P-256 public key as DER SubjectPublicKeyInfo and sets the maximum grant duration. `OwnerCancelAuthority::create()` allocates a new activation ID. `signing_payload()` produces the binary grant for a symbol, increasing serial number and absolute expiry in exchange milliseconds. The grant is specific to spot cancellation. It encodes a NUL-terminated domain string, version and environment bytes, seven little-endian 64-bit fields (key ID, account UID, store UUID low and high words, activation ID, serial and expiry), and a NUL-padded 20-byte symbol.

An enrolled P-256 YubiKey PIV signing key can sign the exact binary file with PIN entry on the Owner's device. The signer returns the DER ECDSA signature. [YubiKey PIV Tool signing](https://docs.yubico.com/software/yubikey/tools/pivtool/piv-tool-command.html#sign-data)

```powershell
yubico-piv-tool -a verify-pin --sign -s 9c -A ECCP256 -H SHA256 -i owner-cancel.grant -o owner-cancel.sig
```

`OwnerCancelAuthority::install()` verifies the signature and expiry against the pinned key and a fresh exchange clock. The active grant is replaced only by a valid grant with a higher serial. Local revocation clears affected grants and prevents their reinstallation during this activation. Each new activation requires a new signature.

The native cancellation process retains the Trading lock while handing the log to the executor. Its control thread stops the private stream, joins its producer, drains the final events and transfers ownership of the sink. The executor scans the latest committed state under the same lock, checks the current kill switch revision, store, signed account UID and order identity, and writes the preparation before sending DELETE. An authorization refusal before HTTP write records `OrderCancelNotSent`. A prepared attempt with an uncertain result is queried by numeric order ID on restart. A working observation keeps it pending.

### Register and run native cancellation

Use an existing, healthy, bound order log and its KEK and wrapped-key file. The native entry covers the BTCUSDT store configuration with symbol ID 0. Keep the log directory and its public-key registration writable only by the trusted local operator. On POSIX, use owner-only permissions (`0600` or `0400`) for the KEK file. Enrollment publishes a new `.owner-cancel.pub` file beside that log and refuses an existing registration. The record binds the key to the verified environment, account and log UUID. The grant directory holds only unsigned payloads and signatures.

```powershell
& $exe --enroll-cancel-owner PRODUCTION $log $auditKeyId $kekFile $wrappedKeyFile $ownerKeyId $ownerPublicDer
& $exe --cancel-only PRODUCTION $log $auditKeyId $kekFile $wrappedKeyFile $credentialsFile $controlDir 600
```

For Spot Testnet use `TESTNET` in both commands and its independent account, log and key. The credentials file uses `HENGYUAN_BINANCE_LIVE_API_KEY` and `HENGYUAN_BINANCE_LIVE_SECRET` in Production, or `HENGYUAN_BINANCE_TESTNET_API_KEY` and `HENGYUAN_BINANCE_TESTNET_SECRET` in Testnet. For a working order the process prints the payload and corresponding signature paths. Sign that payload using the registered hardware key and write the signature to the printed path. The process installs the grant in memory and handles one claimed cancellation before exiting. To recover after an interruption, start the same cancellation command against the same log. Existing pending attempts are reconciled without a signature. New attempts require a signature for the current activation.

## 3. Operator manual takeover — step by step

**Precondition for this section to matter at all:** you must already have the Binance mobile app installed, logged in, with 2FA configured, on a phone you actually carry. If that's not true yet, that's a real precondition, not a checkbox — go set it up before treating this runbook as complete.

When you're notified (however that notification reaches you — this native kernel does not currently implement operator alerting; that's a separate gap, see §6) that the kill switch has triggered:

1. **Do not panic-act on the terminal/server.** The software has already stopped generating new order intents. There is no time-critical *software* action left for you to race against.
2. Open the Binance mobile app.
3. Navigate to **Orders → Open Orders** (exact menu path varies by app version — verify this against your actual installed app before relying on it; Binance has changed this navigation across releases).
4. For each open order shown: tap it, confirm the symbol/side/quantity match what you expect, then **Cancel**.
5. If a position remains (partial fill, or an order filled before you could cancel): navigate to **Wallet → Spot** (or **Positions**, depending on app version), locate the asset, and place a manual market/limit sell to flatten — using your own judgment on price, not a script's.
6. After you believe you're flat: use **Read-Only Query** (a `GET /account` — either via the app or, if wired up, via the native `sim_executor`/account-truth read path) to confirm actual exchange-side state matches what you expect. Don't trust your own memory of what you just did; check the account.
7. Do **not** attempt to re-arm the kill switch from panic or urgency. Re-arming is a separate, deliberate ceremony (see `docs/adr/ADR-019` D9) — doing it in the moment, under stress, defeats the reason it's a separate step.

## 4. Verification after manual action

Before considering the incident closed:

- Confirm via account query (not memory) that there are zero open orders and the position matches your intent (flat, or an intentionally-held residual).
- Write down (even informally) what triggered the kill switch, what you did, and when — this is the seed of a real audit trail even before persistent audit storage exists (see the D12-9 L5 review's F5 finding).
- Do not re-arm until you understand *why* the kill switch triggered. An unexplained trigger re-armed without investigation is how a second incident happens.

## 5. Stop conditions — do not proceed if

- You can't get the app to load or log in (network, outage, phone dead) — this is exactly the `WaitForOperator` case `determine_residual_action()` returns; the honest state is "unresolved," not "handled."
- The account state you observe doesn't match what you expected — stop and investigate before acting further; don't guess.
- You're not sure which order/position is the one that mattered — don't cancel/sell blind; confirm first.

## 6. Known gaps (not covered by this runbook)

- **No operator alerting exists yet.** This runbook assumes you somehow find out the kill switch triggered. There is no push notification, SMS, or equivalent wired up anywhere in this codebase. That's a real, separate gap — an exit-safety runbook is not useful if nothing tells you to read it.
- **No drill has ever been run.** ADR-018's actual D3-LIVE checklist (distinct from `preflight_gate.hpp`'s CODE-PREFLIGHT, see that file's updated header comment) lists "periodic manual out-of-band channel drills" as a precondition. Reading this document is not the same as having practiced it once on your actual phone, with your actual app, under a timer, to find out how long it *really* takes you.

## 7. R6 Testnet drill for an unresolved order

The owner runs this drill on Binance Spot Testnet. `hy_r6_prepare_drill` creates a separate rehearsal store with one synthetic BTCUSDT order at the `Prepared` boundary. It writes the symbol snapshot, audit log, Testnet account binding and keys, then exits. It takes no exchange credentials and sends no requests. The operator tool subsequently uses real Testnet reads to check and resolve that order.

1. Build both tools with `HY_BUILD_DEMO=ON`. Use a private parent directory on a local disk. With the system clock synchronized, create a fresh output directory with the command below and the UID from your dedicated Testnet account. Keep the directory and its sidecar files together. Use a dedicated read-only Testnet API key for resolution and keep it in a separate private file. Leave other senders for this account stopped during the drill.
2. Open the rehearsal store with `--list` and record the target client order ID. Keep the backup separate and use the same working store for listing, resolution and the final check. Check the printed environment, account UID, symbol and whether the order can be resolved. The command acquires the store locks and may perform recovery. It needs the local keys and makes no exchange requests.
3. Run the resolution against the same Testnet account. The program waits at least 270 seconds of exchange time after acquiring the locks and then queries the order and both order lists. The wait continues from the first valid exchange time after acquiring the locks. A time read makes up to three paced attempts on DNS, connection, write or read interruptions, transient TLS handshake failures, or a round trip longer than 2 seconds. Signed reads retry transient TLS handshake failures before sending their HTTP request. A certificate or hostname verification failure stops immediately. The tool allows 20 seconds to read an HTTP response and reports the failed stage or HTTP status. An account response with HTTP 200 and no valid UID receives its own diagnostic. HTTP 403, 418 and 429 stop any further requests in the current run. Review the result against the exchange's order and trade history for that account and time range. Enter the exact confirmation phrase shown by the tool only when the displayed account, symbol and order ID match and the exchange records show neither the order nor a fill.
4. Expect `result: Resolved (exit code 0)`. Run `--list` again with the same store and verify the target ID is gone from the unresolved list. If the tool returns 59 or 70, inspect `--list` before considering a retry because an append may already have reached storage. Record the result and the before-and-after list status in the drill notes. Keep the response attachment in the tool's local output directory.

On an interactive Windows console, type the displayed line at `Confirm>`. The console echoes the input as you type and Backspace removes the last character. Press Enter within five minutes. Exit code 57 is accompanied by the specific reason, such as a timeout, an incorrect line, or an input read error. Start a new run on the same store for another confirmation attempt and use the line shown in that run.

From the project root in PowerShell 7, use a new directory and replace the account UID. The two key IDs for a generated store are both `1`. If preparation exits with an error, keep the partial directory for inspection and use a new directory for another attempt.

```powershell
$uid = '<Testnet account UID>'
$drillDir = Join-Path $env:LOCALAPPDATA ('HengYuan-R6-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$prepare = '.\native\build-msvc\Release\hy_r6_prepare_drill.exe'
& $prepare --output-dir $drillDir --uid $uid
if ($LASTEXITCODE -ne 0) { throw 'Drill preparation failed.' }

$tool = '.\native\build-msvc\Release\hy_operator_resolve.exe'
$storeArgs = @(
    '--audit-log', (Join-Path $drillDir 'audit.log'),
    '--control-log', (Join-Path $drillDir 'control.log'),
    '--kek-file', (Join-Path $drillDir 'operator.kek'),
    '--key-file', (Join-Path $drillDir 'audit.keys'),
    '--audit-key-id', '1',
    '--control-key-id', '1'
)
& $tool --list @storeArgs
```

The first list should show one `Ambiguous` BTCUSDT order with the `coid` recorded in `r6-drill.txt`. After checking it, replace the client order ID below with that value.

If OpenSSL on Windows reports an empty trust store, set `SSL_CERT_FILE` in the current PowerShell session to the path of an installed CA bundle, such as the bundle supplied by Git for Windows at `C:\Program Files\Git\mingw64\etc\ssl\certs\ca-bundle.crt`. Run the resolution from that session.

```powershell
& $tool --env testnet --coid '<client order ID>' --uid $uid `
    --operator 'r6-drill' --credentials-file '.\.env.operator' @storeArgs
```

The credentials file contains `HENGYUAN_OPERATOR_TESTNET_API_KEY` and `HENGYUAN_OPERATOR_TESTNET_API_SECRET`. Record the HTTP status and exchange error code separately. A point lookup of HTTP 400 with `-2013` passes that one check. `-2026` means the exchange has an archived record and the workflow refuses absence. Leave the drill order unresolved whenever the queries or the account checks disagree. Share only the exit code and a summary with the account UID and balance details omitted.

For local verification, CTest's `R6PreparedDrill` runs both entry points against a fresh local directory and checks the resulting list. `RealToolTest.AnAbsentOrderIsResolvedWithTheOperatorKeyOverRealTls` exercises confirmation, append and recovery over a loopback exchange.

### Result on 2026-11-15

The operator completed the drill on Windows with a Spot Testnet account and a synthetic `Prepared` BTCUSDT order. The order generator had left one `Ambiguous` record in the rehearsal store.

| Check | Observed result |
| --- | --- |
| Environment and account | Testnet, with matching account bindings in the store and exchange response |
| Exchange-clock wait | 271 seconds from the first reading after acquiring the locks |
| Order queries | Point lookup returned `NoSuchOrder`, one `allOrders` window was `Clear`, and `openOrders` was `Clear` |
| Transport recovery | TLS handshake timeouts on the time, point and open-order requests recovered on retry |
| Operator confirmation | The Windows console echoed the current confirmation line and accepted it |
| Durable resolution and reopening | `Resolved (exit code 0)`. The subsequent `--list` reported `unresolved orders: 0` |

### Measuring order-list visibility

Use an order accepted by Spot Testnet and a separate read-only operator key for the same account. Record the exchange's order placement time as `t0` in milliseconds. Keep the symbol and order identifiers locally. Record the intended polling interval and observation deadline before collecting samples. Start polling promptly after acceptance and record the actual gap between observations. [Spot Testnet REST documentation](https://developers.binance.com/en/docs/products/spot/testnet/rest-api#all-orders-user_data)

Run the PowerShell entry point with its two companion Python files in the repository's `tools` directory. It requires PowerShell 7 and Python 3.8 or later. The default command lists up to ten recent orders for the symbol and asks which one to sample. Add `-ListOnly` to stop after listing.

To reuse a local credential file without passing `-CredentialsFile` each time, put its absolute path alone in `tools/.env.r6-visibility-path`. Git ignores this path setting. Without the setting, the launcher looks for `.env.operator` in the current directory. An explicit `-CredentialsFile` overrides either default.

```powershell
& '.\tools\r6_allorders_visibility.ps1' -CredentialsFile '.\.env.operator' -Symbol BTCUSDT
```

An order selected from the list has already been observed in `allOrders`. Completed follow-up runs with hits and zero misses report `already_visible` and set `interval_ms` to `null`. To sample from acceptance, use `-PlaceTestnetOrder` or pass the numeric `orderId` as soon as the exchange acknowledges the order.

```powershell
& '.\tools\r6_allorders_visibility.ps1' -CredentialsFile '.\.env.operator' -Symbol BTCUSDT -OrderId 12345
```

When sampling an existing order, the script reads the original placement time from `GET /api/v3/order` and verifies the symbol and order ID.

To submit and sample in one run, generate a separate HMAC key for the same Spot Testnet account. It needs `TRADE` to submit and `USER_DATA` to verify the account. Store it in a separate local UTF-8 file with the following entries. The existing operator key continues to handle the sampling reads. [Binance Spot Test Network](https://testnet.binance.vision/) [Testnet request security](https://developers.binance.com/en/docs/products/spot/testnet/rest-api#request-security)

```dotenv
HENGYUAN_R6_TESTNET_TRADE_API_KEY=<Testnet TRADE API key>
HENGYUAN_R6_TESTNET_TRADE_API_SECRET=<Testnet TRADE API secret>
```

```powershell
$order = @{
    Symbol = 'BTCUSDT'
    Side = Read-Host 'Side (BUY or SELL)'
    Price = Read-Host 'Limit price'
    Quantity = Read-Host 'Order quantity in BTC'
}
& '.\tools\r6_allorders_visibility.ps1' -PlaceTestnetOrder -TradeCredentialsFile 'D:\TestNet\.env.r6-trade' @order
```

The script compares the UIDs reported for the two keys before submitting a single `LIMIT_MAKER` order to Spot Testnet. An acknowledgment supplies `orderId` and the placement time `transactTime`. If the outcome is unknown, the script makes up to three read-only queries using the original client order ID. If it still cannot verify the outcome, check that ID in Testnet before running another placement. The script leaves management of the Testnet order to the operator. Keep terminal messages with order identifiers local.

The default sampling window is 20 seconds with a one-second polling interval and a five-second timeout per request. Use `-DurationSeconds`, `-PollIntervalMs` and `-TimeoutSeconds` to reproduce a sampling setup. If you need an explicit CA bundle, pass `-CaBundle`. The JSON report contains relative times, statuses and observation counts for sharing. Exit code 3 means a backoff response stopped the run. Exit code 4 means preparation failed. Exit code 5 means order submission was rejected or its outcome could not be verified.

1. For each observation, read `GET /api/v3/time`, call `GET /api/v3/allOrders` for the symbol and a time window covering the order, then read `GET /api/v3/time` again. Record both exchange times and match the returned order by symbol and order identifier. [Spot Testnet server time](https://developers.binance.com/en/docs/products/spot/testnet/rest-api#check-server-time)
2. Mark the order as missing only for a valid, complete HTTP 200 order list. Mark failed requests, saturated pages, invalid JSON, rows outside the requested time window, and unusable time brackets as inconclusive. JSON objects must have unique field names. The post-query time read must finish before the sampling deadline for an observation to count. Stop the run on HTTP 403, 418 or 429. If a later valid poll loses an order that was previously visible, report that sequence separately.
3. Report the interval from the **before-query** time of the last valid miss to the **after-query** time of the first valid hit, relative to `t0`. Use zero as the lower bound if the first valid poll already found the order. If the deadline passes with valid misses and zero hits, report the last lower bound with the upper bound open. Record inconclusive runs separately.

The following offline example uses elapsed milliseconds from `t0` and a synthetic sample label.

| Sample | Poll | Before query (ms) | After query (ms) | Observation |
| --- | ---: | ---: | ---: | --- |
| A | 1 | 210 | 324 | Missing |
| A | 2 | 730 | 841 | Found |

Sample A gives an observed transition interval of **210–841 ms**. Share the relative times, sample label, observation category, polling interval and sample counts. Retain the configured 120-second read allowance until the real samples have been reviewed. These intervals describe the observed reads. A finite set of samples cannot establish a service-wide upper bound.

### Read-only follow-up on 2026-11-15

The operator selected an existing order from the Spot Testnet `allOrders` list. The 20-second follow-up returned six hits, zero misses, and zero inconclusive observations. The exchange-time readings around the first timed query were **24.241 and 26.278 seconds** after placement. Gaps between the exchange-time readings taken before successive queries ranged from **3.365 to 4.029 seconds** with a configured one-second polling interval. The run verifies continued visibility for the selected order.

### Sampling from order acceptance on 2026-11-15

The operator submitted a Spot Testnet `LIMIT_MAKER` order and started `allOrders` sampling from the exchange-reported acceptance time. The saved report has `selection: new_order` and `status: bounded`.

| Measurement | Result |
| --- | --- |
| Sampling duration and configured polling interval | 20 seconds and 1,000 ms |
| First valid query | Found, bracketed by exchange times 1,566 and 4,088 ms after acceptance |
| Visibility interval for this order | 0–4,088 ms. The first valid query found the order, so there is no observed lower bound above zero. |
| Observation counts | 5 found, 0 missing, 1 inconclusive |
| Gaps between pre-query exchange-time readings | 3,165–3,935 ms |
| Final observation | The sampling deadline expired before the post-query exchange-time read at stage `after_time`. |

The read allowance is 120 seconds.

### Sampling from order acceptance on 2026-11-17

An automated run verified that the operator and `TRADE` keys belonged to the same Spot Testnet account, submitted one `BTCUSDT BUY LIMIT_MAKER` order, and sampled from the exchange-reported acceptance time. The saved report has `selection: new_order` and `status: bounded`.

| Measurement | Result |
| --- | --- |
| Sampling duration and configured polling interval | 20 seconds and 1,000 ms |
| First valid query | Found, bracketed by exchange times 1,760 and 3,477 ms after acceptance |
| Visibility interval for this order | 0–3,477 ms |
| Observation counts | 6 found, 0 missing, 1 inconclusive |
| Gaps between pre-query exchange-time readings | 2,551–5,995 ms, including the final inconclusive observation |
| Final observation | The sampling deadline expired before dispatching `allOrders` at stage `all_orders`. |
| Follow-up order status | `NEW` at the post-sampling point lookup |

The read allowance is 120 seconds. The measured interval applies to this order.

### Checking the archived-order response

Binance documents `-2026 ORDER_ARCHIVED` for unfilled orders canceled or expired more than 90 days ago that have been archived. Spot Testnet resets its order data approximately monthly, so a newly canceled Testnet order is unlikely to remain available long enough for this check. [Spot Testnet error codes](https://developers.binance.com/en/docs/products/spot/testnet/errors#-2026-order_archived) [Spot Test Network reset FAQ](https://testnet.binance.vision/)

If a suitable order from the operator's Testnet account is available, run the read-only sampler from the repository root with that order's symbol and numeric `orderId`.

```powershell
$symbol = Read-Host 'Spot Testnet symbol'
$orderId = [long](Read-Host 'Numeric orderId for the archived order')
& '.\tools\r6_allorders_visibility.ps1' -CredentialsFile '.\.env.operator' -Symbol $symbol -OrderId $orderId
```

Record the report's `problem.http_status` and `problem.exchange_code` from stage `order`. The sampler exits with code 4 on these point-query errors. Record `-2013` separately as a no-such-order response. The resolver classifies `-2026` as an archived order and refuses an `ABSENT` resolution. Simulated responses cover both codes. Direct Testnet observation of `-2026` is pending.
