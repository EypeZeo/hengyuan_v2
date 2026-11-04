// SPDX-License-Identifier: proprietary
// private_stream_pipeline.hpp — private-stream migration M5a-2b (docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 5.2-5.6): the composition root of the private stream.
//
// Everything the migration built is a part with a narrow job and its own tests: the WS API session and its supervisor (M2), the reconnect barrier and the
// readiness decision (M3a), the sweep transport (M3b), the durable-first user-data path (M4), the operator-resolution late-fill alarm (R4), the store binding
// (R5), the account refresh channel (M5a-1). What nobody owned yet is the WIRING: which object is handed to which, in which order they run, which thread owns
// what, and how the facts StartupRecovery waits for are read off the live parts. This header is that, and nothing else -- it adds no state machine, no rule and
// no new cross-thread primitive. A process (the harness of M5b) builds ONE of these over its own durable store and shared order state and ticks it.
//
//   hot_tick(now)         the hot/submit thread, once per cycle:   supervisor.poll -> holder.poll -> StartupRecovery.update (recovery applied once, when it
//                         is due) -> drain_hot_inputs()  [sweep pump service, reconcile drain, user-data drain: the very function orchestrate_submit() runs
//                         ahead of its gates, so the two cannot drift]
//   reconcile_tick(now)   the reconcile thread, once per cycle:    account refresher (periodic) -> poll_once() with the sweep job, whose account step runs
//                         inside it after the last answer and before the marker
//   make_context()        an OrchestratorContext with every wiring field set; the caller fills in the order (symbol, side, price, quantity, ports) and calls
//                         orchestrate_submit()
//   bind_store(...)       the store binding at startup, with the uid the EXCHANGE reported (rest_account_fetcher.hpp::fetch_exchange_account_uid())
//
// WHAT THE PIPELINE OWNS: the rings between the halves (to-reconcile, reconcile events, sweep requests, account updates), the order tracker, the sweep job, the
// account refresher and holder, the supervisor, the startup-recovery state machine, the readiness decision (barrier included), the sweep pump, the late-fill
// alarm and the two durable publishers. WHAT IT DOES NOT: the durable store, the in-memory audit ring, the kill switch, PositionTruth, the in-flight registry,
// the fill context, the user-data event ring (the session factory is built over it, so it exists first), the clock publisher and the rate-limit tracker -- those
// are the process's and come in by reference (PipelineParts), and so does every function pointer that touches the network (query port, account fetch port,
// symbol rules). It creates no thread and opens no connection by itself: the supervisor connects on the hot thread's poll(), through the factory it was given.
//
// THREADS. The two halves are separate member groups on separate cache lines; the only things they share are the rings. Today the harnesses run both ticks from
// ONE loop thread -- which is also the only arrangement the real REST adapters allow (rest_account_fetcher.hpp: the credentials answer only on the thread that
// bound them) -- and that is a supported way to run this: one thread is just the case where both halves are the same thread. Two threads need the REST side
// to be solved first (own credentials, a shared rate-limit source); nothing here assumes either.
//
// THE FACTS STARTUPRECOVERY WAITS FOR are read in one function (observe_recovery()), used by both the tick and the readiness probe, so "ready" is judged on the
// same facts wherever it is asked: the clock publisher is fresh, exchangeInfo is loaded (set by the process), the durable store is open, unfenced and scanned,
// recovery was applied (once, from the sink's checkpoints, with the process's rules lookup), the recovered orders that are still unconfirmed (counted on the
// hot thread from the registry and the durable shadows, startup_recovery.hpp), the account is fresh AND the store is bound to this exchange account, and the
// user-data stream is connected and verified.
//
// THE STORE BINDING gates readiness: until bind_store() has returned a verdict under which the store may be used, the account does not count as fresh and the
// process cannot become ready. A refused binding (another environment or account, an unreadable binding, an unbound Production store with history) leaves it
// that way for good -- the caller is expected to stop the process, and the status says why.
//
// Governance: L4 composition (it calls the injected ports and the supervisor's factory, which perform network I/O; it signs nothing and holds no secret).

#pragma once

#include <hengyuan/account_refresh.hpp>
#include <hengyuan/barrier_sweep.hpp>
#include <hengyuan/binance_user_data_event.hpp>
#include <hengyuan/durable_audit_sink.hpp>
#include <hengyuan/durable_order_audit_port.hpp>
#include <hengyuan/feed_observation.hpp>
#include <hengyuan/kill_switch.hpp>
#include <hengyuan/late_fill_alarm.hpp>
#include <hengyuan/live_submit_orchestrator.hpp>
#include <hengyuan/order_fill_context.hpp>
#include <hengyuan/order_tracker.hpp>
#include <hengyuan/position_truth.hpp>
#include <hengyuan/public_feed_supervisor.hpp>
#include <hengyuan/spsc_ring.hpp>
#include <hengyuan/startup_recovery.hpp>
#include <hengyuan/store_binding_writer.hpp>
#include <hengyuan/submit_readiness.hpp>
#include <hengyuan/sweep_pump.hpp>

#include <concepts>
#include <cstdint>
#include <string>
#include <utility>

namespace hy {

// The session type the pipeline can be built over: what the barrier needs to observe it, plus the steady clock its activity stamps are in (the real session's
// steady_now_ms()). One clock for the feed observations, the sweep requests, the account stamps and the ticks.
template <typename S>
concept PipelineSession = ObservableFeedSession<S> && requires {
    { S::steady_now_ms() } -> std::convertible_to<std::int64_t>;
};

// The symbol rules of a recovered order's symbol (SymbolRegistry::current_rules() in a process). rules_version == 0 is "unknown symbol": recovery stops there
// and the state machine degrades -- an unwired lookup therefore fails closed.
struct PipelineRulesLookup {
    using Fn = SymbolRules (*)(void* user_data, std::uint32_t symbol_id) noexcept;

    Fn fn{nullptr};
    void* user_data{nullptr};

    [[nodiscard]] SymbolRules operator()(std::uint32_t symbol_id) const noexcept { return fn != nullptr ? fn(user_data, symbol_id) : SymbolRules{}; }
};

struct PipelineConfig {
    FeedSupervisorPolicy feed{};
    BarrierPolicy barrier{};
    ReconcilePollPolicy poll{};
    AccountRefreshPolicy refresh{};
    ExposureLimits exposure_limits{};          // the account freshness window of StartupRecovery is the one validate_pre_trade() applies
    std::int64_t reconcile_deadline_ms{60'000};  // how long recovered orders may stay unconfirmed before the state machine degrades
};

// What the process owns, by reference: every referent outlives the pipeline. The function-pointer ports are values.
struct PipelineParts {
    DurableAuditSink& sink;
    const std::string& log_path;  // the path and key ring the sink was built with: the store binding reads and writes beside it
    const KeyRing& key_ring;
    DurableOrderAuditPort durable_port;  // the same sink as the order path's port: one order's frames must not be split across two logs
    AuditRingSink& audit;
    KillSwitch& kill_switch;  // the ONE breaker: the late-fill alarm trips it, Gate 2 of every order reads it
    PositionTruth& position_truth;
    InFlightRegistry& in_flight;
    OrderFillContext& fill_context;
    UserDataWsEventRing& user_data_events;  // the ring the session factory's Deps::ring points at
    const ClockOffsetPublisher& clock;
    SpotRateLimitTracker* rate_limiter{nullptr};  // goes into the contexts it makes (the order path charges POST /order to it)
    QueryPort query_port{};
    AccountFetchPort account_port{};
    PipelineRulesLookup rules{};
};

// The hot thread's counters; read them on that thread. (The reconcile half has its own single counter, reconcile_ticks(), on its own cache line: two threads never
// store to one line.)
struct PipelineStats {
    std::uint64_t hot_ticks{0};
    std::uint64_t recoveries_applied{0};  // 0 or 1
    std::uint64_t binding_attempts{0};
};

template <PipelineSession Session>
class PrivateStreamPipeline {
public:
    using Supervisor = PublicFeedSupervisor<Session>;
    using Factory = typename Supervisor::Factory;

    // `factory` builds one WS API generation per call (WsApiSessionFactory in production); it is the supervisor's, called on the hot thread's poll().
    PrivateStreamPipeline(const PipelineParts& parts, const PipelineConfig& config, Factory factory)
        : parts_(parts),
          config_(config),
          hot_(parts, config, std::move(factory), *this, account_updates_, sweep_requests_),
          reconcile_(parts, config, account_updates_, sweep_requests_) {
        wire_context();
    }

    PrivateStreamPipeline(const PrivateStreamPipeline&) = delete;
    PrivateStreamPipeline& operator=(const PrivateStreamPipeline&) = delete;

    // ---- the hot/submit thread ---------------------------------------------------------------------------------------------------------------

    // `now_ms` is the steady clock (Session::steady_now_ms()); `wall_now_ms` the wall clock in ms, or OrderRecord::kClockUnset (R-10, startup recovery's wall
    // anchor for Ambiguous orders).
    void hot_tick(std::int64_t now_ms, std::int64_t wall_now_ms = OrderRecord::kClockUnset) noexcept {
        ++hot_.stats.hot_ticks;
        hot_.supervisor.poll(now_ms);
        hot_.holder.poll();
        advance_recovery(now_ms, wall_now_ms);
        context_.now_ms = now_ms;
        drain_hot_inputs(context_);
    }

    // The startup binding of the store to the exchange account this process trades as. `exchange_uid` is what the exchange reported for the key in use. The
    // verdict is remembered until the next call; readiness waits for one under which the store may be used. Call it on the hot thread before the loop. Every call
    // judges the store afresh (a bound store is re-verified, one that does not match is refused again), so the verdict is never stronger than the last look.
    EnsureBindingStatus bind_store(StoreEnvironment environment, std::uint64_t exchange_uid, std::int64_t now_ms) noexcept {
        ++hot_.stats.binding_attempts;
        binding_status_ = ensure_store_binding(parts_.sink, parts_.log_path, parts_.key_ring, environment, exchange_uid, now_ms);
        return binding_status_;
    }

    // exchangeInfo is loaded and the symbol rules lookup answers: recovery may scale the recovered orders.
    void set_exchange_info_loaded(bool loaded) noexcept { exchange_info_loaded_ = loaded; }

    // An OrchestratorContext with every wiring field of the private stream set; the caller fills in the order and the ports and calls orchestrate_submit().
    // Equal in every wiring field to the context the hot tick drains with.
    [[nodiscard]] auto make_context() const noexcept { return context_; }  // an OrchestratorContext, by value (`auto`: the guards count the type's constructions)

    // ---- the reconcile thread (or the same loop) ---------------------------------------------------------------------------------------------

    void reconcile_tick(std::int64_t now_ms, std::int64_t wall_now_ms = OrderRecord::kClockUnset) noexcept {
        ++reconcile_.ticks;
        reconcile_.refresher.tick(now_ms);
        poll_once(reconcile_.tracker, to_reconcile_, reconcile_events_, parts_.query_port, config_.poll, now_ms, wall_now_ms, &reconcile_.job);
    }

    // ---- what the process and the tests look at ------------------------------------------------------------------------------------------------

    [[nodiscard]] SubmitReadiness& readiness() noexcept { return hot_.readiness; }
    [[nodiscard]] const StartupRecovery& recovery() const noexcept { return hot_.recovery; }
    [[nodiscard]] Supervisor& supervisor() noexcept { return hot_.supervisor; }
    [[nodiscard]] const AccountHolder& holder() const noexcept { return hot_.holder; }
    [[nodiscard]] const SweepPump& sweep_pump() const noexcept { return hot_.pump; }
    [[nodiscard]] const LateFillAlarm& late_fill() const noexcept { return hot_.late_fill; }
    [[nodiscard]] const AccountRefresher& refresher() const noexcept { return reconcile_.refresher; }
    [[nodiscard]] const SweepJob& sweep_job() const noexcept { return reconcile_.job; }
    [[nodiscard]] const OrderTracker& tracker() const noexcept { return reconcile_.tracker; }
    [[nodiscard]] EnsureBindingStatus binding_status() const noexcept { return binding_status_; }
    [[nodiscard]] bool store_bound() const noexcept { return store_may_be_used(binding_status_); }
    [[nodiscard]] bool recovery_applied() const noexcept { return recovery_applied_; }
    [[nodiscard]] const RecoveryApplyResult& recovery_result() const noexcept { return apply_result_; }
    // The hot thread's counters, and the reconcile half's tick count: each is read on the thread that writes it.
    [[nodiscard]] const PipelineStats& stats() const noexcept { return hot_.stats; }
    [[nodiscard]] std::uint64_t reconcile_ticks() const noexcept { return reconcile_.ticks; }

    // The facts StartupRecovery waits for, read off the live parts right now. The readiness probe and the tick both use this.
    [[nodiscard]] RecoveryObservation observe_recovery(const FeedObservation& feed, std::int64_t now_ms) const noexcept {
        RecoveryObservation o{};
        o.clock_synced = clock_ready_now(parts_.clock);
        o.exchange_info_loaded = exchange_info_loaded_;
        o.sink_open = parts_.sink.is_open();
        o.sink_fenced = parts_.sink.fenced();
        o.scan_status = parts_.sink.recovery_status();
        o.recovery_applied = recovery_applied_;
        o.recovered_orders = apply_result_.recovered;
        o.applied_orders = apply_result_.applied;
        if (recovery_applied_ && hot_.recovery.state() != RunState::Ready) {
            o.unconfirmed_orders = tally_recovered_orders_by_shadow(parts_.in_flight, parts_.fill_context, parts_.sink.recovered_checkpoints()).unconfirmed;
        }
        o.account_fresh = store_bound() && hot_.holder.fresh(now_ms, config_.exposure_limits);
        o.user_data_connected = feed.connected;
        return o;
    }

private:
    // The hot thread's objects, on their own cache line(s): nothing a reconcile-thread write touches shares a line with them.
    struct alignas(kCacheLine) HotSide {
        HotSide(const PipelineParts& parts, const PipelineConfig& config, Factory factory, PrivateStreamPipeline& owner, AccountUpdateRing& account_updates,
                SweepRequestRing& sweep_requests)
            : supervisor(std::move(factory), config.feed),
              holder(account_updates),
              recovery(config.reconcile_deadline_ms),
              readiness(recovery, parts.kill_switch, ReadinessProbe{&read_feed, &read_recovery, &read_clock, &owner}, config.barrier),
              pump(readiness, sweep_requests, holder.sweep_probe()),
              reconcile_publisher(parts.durable_port),
              user_data_publisher(parts.durable_port),
              late_fill(parts.sink.recovered_operator_resolved(), parts.sink.recovered_operator_resolved_total(), parts.kill_switch) {}

        Supervisor supervisor;
        AccountHolder holder;
        StartupRecovery recovery;
        SubmitReadiness readiness;
        SweepPump pump;
        ReconcileDurablePublisher reconcile_publisher;
        UserDataDurablePublisher user_data_publisher;
        LateFillAlarm late_fill;
        PipelineStats stats{};
    };

    // The reconcile thread's objects.
    struct alignas(kCacheLine) ReconcileSide {
        ReconcileSide(const PipelineParts& parts, const PipelineConfig& config, AccountUpdateRing& account_updates, SweepRequestRing& sweep_requests)
            : refresher(account_updates, parts.account_port, config.refresh), job(sweep_requests, refresher.sweep_step()) {}

        OrderTracker tracker;
        AccountRefresher refresher;
        SweepJob job;
        std::uint64_t ticks{0};
    };

    // The probe functions run on the hot thread (authorize() and the pump's observe() are the hot thread's).
    static FeedObservation read_feed(void* self) noexcept {
        auto* p = static_cast<PrivateStreamPipeline*>(self);
        return make_feed_observation(p->hot_.supervisor, static_cast<std::int64_t>(Session::steady_now_ms()));
    }
    static RecoveryObservation read_recovery(void* self) noexcept {
        auto* p = static_cast<PrivateStreamPipeline*>(self);
        const std::int64_t now = static_cast<std::int64_t>(Session::steady_now_ms());
        return p->observe_recovery(make_feed_observation(p->hot_.supervisor, now), now);
    }
    static bool read_clock(void* self) noexcept { return clock_ready_now(static_cast<PrivateStreamPipeline*>(self)->parts_.clock); }

    void wire_context() noexcept {
        context_.audit = &parts_.audit;
        context_.kill_switch = &parts_.kill_switch;
        context_.rate_limiter = parts_.rate_limiter;
        context_.in_flight = &parts_.in_flight;
        context_.durable_audit = parts_.durable_port;
        context_.to_reconcile = &to_reconcile_;
        context_.reconcile_events = &reconcile_events_;
        context_.reconcile_durable = &hot_.reconcile_publisher;
        context_.user_data_events = &parts_.user_data_events;
        context_.user_data_durable = &hot_.user_data_publisher;
        context_.late_fill = &hot_.late_fill;
        context_.readiness = &hot_.readiness;
        context_.sweep = &hot_.pump;
        context_.position_truth = &parts_.position_truth;
        context_.fill_context = &parts_.fill_context;
        context_.account = &hot_.holder.view();
        context_.exposure_limits = config_.exposure_limits;
    }

    void advance_recovery(std::int64_t now_ms, std::int64_t wall_now_ms) noexcept {
        const FeedObservation feed = make_feed_observation(hot_.supervisor, now_ms);
        RecoveryObservation obs = observe_recovery(feed, now_ms);
        if (hot_.recovery.update(obs, now_ms) == RunState::Recovering && !recovery_applied_) {
            // Once: the durable log's unfinished orders are registered in flight, aligned with the fill context's baselines and queued for the reconcile
            // thread. The state machine is told what came of it right away.
            apply_result_ = apply_recovery(
                parts_.sink.recovered_checkpoints(), parts_.in_flight, to_reconcile_, parts_.position_truth, parts_.fill_context,
                [this](std::uint32_t symbol_id) noexcept { return parts_.rules(symbol_id); }, wall_now_ms);
            recovery_applied_ = true;
            ++hot_.stats.recoveries_applied;
            obs = observe_recovery(feed, now_ms);
            (void)hot_.recovery.update(obs, now_ms);
        }
    }

    PipelineParts parts_;
    PipelineConfig config_;

    // The rings between the halves. SpscRing aligns its own indices; each ring is its own object.
    ToReconcileRing to_reconcile_;
    ReconcileEventRing reconcile_events_;
    SweepRequestRing sweep_requests_;
    AccountUpdateRing account_updates_;

    HotSide hot_;
    ReconcileSide reconcile_;

    OrchestratorContext context_{};
    RecoveryApplyResult apply_result_{};
    EnsureBindingStatus binding_status_{EnsureBindingStatus::NotRun};
    bool recovery_applied_{false};
    bool exchange_info_loaded_{false};
};

}  // namespace hy
