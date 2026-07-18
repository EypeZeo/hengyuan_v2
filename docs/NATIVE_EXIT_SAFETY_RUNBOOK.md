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
