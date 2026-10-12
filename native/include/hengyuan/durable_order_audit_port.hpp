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

#include <cstdint>

namespace hy {

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

    AppendFn append_fn{nullptr};
    FencedFn fenced_fn{nullptr};
    void* user_data{nullptr};

    AuditAppendResult append_durable(const AuditRecord& rec, std::int64_t now_ms) const noexcept {
        if (!append_fn) {
            AuditAppendResult r{};
            r.status = AuditAppendResult::Status::Failed;
            return r;
        }
        return append_fn(user_data, rec, now_ms);
    }

    // Unwired reads as "already fenced" -- a caller that checks this before ever
    // calling append_durable() should see the conservative answer, not a
    // default that looks like "everything is fine".
    bool fenced() const noexcept { return fenced_fn ? fenced_fn(user_data) : true; }

    bool is_valid() const noexcept { return append_fn != nullptr; }
};

}  // namespace hy
