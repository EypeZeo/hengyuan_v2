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

`determine_emergency_policy()` always returns `OperatorManualOnly` in the current implementation — there is no automated cancel-all-open-orders path. This is deliberate: `EmergencyCancelPolicy::CancelAllKnownOpen` exists as an enum value but is not wired to anything, precisely because automated cancellation during a kill event is exactly the kind of action that should require a human decision, not a background process making it for you.

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

Measure order-list visibility separately using known Testnet orders. For each order, retain the last `allOrders` observation where it was absent and the first observation where it appeared. Record the exchange times around those queries, along with the order acceptance time and polling interval. Report the observed visibility intervals and sample count. Retain the configured 120-second read allowance until the measurements have been reviewed. One successful resolution does not measure visibility, and a finite sample does not establish a service-wide upper bound.

For local verification, CTest's `R6PreparedDrill` runs both entry points against a fresh local directory and checks the resulting list. `RealToolTest.AnAbsentOrderIsResolvedWithTheOperatorKeyOverRealTls` exercises confirmation, append and recovery over a loopback exchange. The Testnet drill exercises the real read and resolution flow for the synthetic order. Measuring order-list visibility requires a separate run with known Testnet orders.

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

An order that the exchange received is required to measure `allOrders` visibility. The visibility interval and the archived-order response `-2026` remain unmeasured.
