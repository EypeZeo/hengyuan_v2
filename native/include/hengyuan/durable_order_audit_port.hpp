// SPDX-License-Identifier: proprietary
// durable_order_audit_port.hpp -- the injectable "append one order-audit record durably" port.
//
// Moved verbatim out of live_submit_orchestrator.hpp (SAFE-03 slice 1) so that order_tracker.hpp --
// which the orchestrator includes, and which therefore cannot include the orchestrator -- can name the
// same type for its reconcile publication (ReconcileDurablePublisher). The adapters that plug a real
// DurableAuditSink into the port (durable_audit_sink_append_adapter(), make_durable_order_audit_port())
// stay in live_submit_orchestrator.hpp: they need the sink, and this header must stay light.
//
// Governance: L1. No file I/O here; the port only forwards to whatever function pointer was injected.

#pragma once

#include <hengyuan/audit_trail.hpp>
#include <hengyuan/durable_control_plane.hpp>
#include <hengyuan/never_sent_rules.hpp>

#include <cstdint>

namespace hy {

// Q23 S6 (docs/NOTSENT_CLOSURE_DESIGN.md 4.3): what a closure append through the port did. The caller needs to tell four things apart:
// the order is closed (Appended), nothing was tried because there is nothing to try with (NotWired: a port without a closure function;
// NotEnabled: a sink that was not built to write closures -- the default, and then the order is exactly what it was before this
// existed: an Intent orphan), nothing was written because the writer refused (Refused), and the append failed (Failed: the frame MAY be
// on the disk, the sink is fenced if the failure was an I/O one). NotAttempted is the value of a result nobody filled in.
enum class ClosureAppendStatus : std::uint8_t {
    NotAttempted = 0,
    Appended = 1,
    NotEnabled = 2,
    NotWired = 3,
    Refused = 4,
    Failed = 5,
};

struct ClosureAppendResult {
    ClosureAppendStatus status{ClosureAppendStatus::NotAttempted};
    AuditAppendResult append{};  // set when status is Appended or Failed
};

// --- Durable order-audit port (dependency injection, mirrors SubmitPort) ---
//
// Real file I/O, the same class of dependency as SubmitPort's real network I/O --
// the orchestrator file's existing "only SubmitPort is mocked, everything else is a real
// instance" testing philosophy does not apply here: durable_audit is just as slow
// and just as side-effectful, so it earns the same injectable-fake treatment.
// make_durable_order_audit_port() (live_submit_orchestrator.hpp) plugs a real DurableAuditSink into
// this port unchanged, for the small number of tests that need real file persistence.
struct DurableOrderAuditPort {
    using AppendFn = AuditAppendResult(*)(void* user_data, const AuditRecord& rec, std::int64_t now_ms);
    using FencedFn = bool(*)(void* user_data);
    // Q23 S6: the closure writer. Optional and LAST, so that every existing aggregate initialisation {append, fenced, user_data} keeps
    // meaning what it meant; a port without it answers NotWired.
    using ClosureFn = ClosureAppendResult(*)(void* user_data, const never_sent::ClosureTarget& target, const AuditRecord& rec,
                                             std::int64_t now_ms);

    AppendFn append_fn{nullptr};
    FencedFn fenced_fn{nullptr};
    void* user_data{nullptr};
    ClosureFn closure_fn{nullptr};

    AuditAppendResult append_durable(const AuditRecord& rec, std::int64_t now_ms) const noexcept {
        if (!append_fn) {
            AuditAppendResult r{};
            r.status = AuditAppendResult::Status::Failed;
            return r;
        }
        return append_fn(user_data, rec, now_ms);
    }

    // Q23 S6: append the closing frame of an order that never crossed. `target` is the CALLER's account of the order (see
    // DurableAuditSink::append_never_sent(): the sink holds no live order and checks the frame against it, not the account itself).
    ClosureAppendResult append_never_sent(const never_sent::ClosureTarget& target, const AuditRecord& rec,
                                          std::int64_t now_ms) const noexcept {
        if (!closure_fn) {
            ClosureAppendResult r{};
            r.status = ClosureAppendStatus::NotWired;
            return r;
        }
        return closure_fn(user_data, target, rec, now_ms);
    }

    // Unwired reads as "already fenced" -- a caller that checks this before ever
    // calling append_durable() should see the conservative answer, not a
    // default that looks like "everything is fine".
    bool fenced() const noexcept { return fenced_fn ? fenced_fn(user_data) : true; }

    bool is_valid() const noexcept { return append_fn != nullptr; }
};

}  // namespace hy
