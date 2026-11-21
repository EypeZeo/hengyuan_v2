// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/preflight_probe.hpp>
#include <hengyuan/trade_scope_owner_key.hpp>
#include <hengyuan/trade_scope_request_files.hpp>

#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace hy {

struct ScopeRuntimePolicy {
    std::uint64_t version{0};
    TradeScopePolicy approval{};
};

inline std::optional<ScopeRuntimePolicy> read_scope_runtime_policy(const std::string& path) {
    std::vector<std::byte> bytes;
    if (read_file_bounded(path, bytes, 512) != ReadFileStatus::Ok || bytes.empty()) return std::nullopt;
    std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    std::array<std::string, 6> fields;
    for (auto& field : fields) if (!(input >> field)) return std::nullopt;
    ScopeRuntimePolicy result;
    auto& approval = result.approval;
    auto& limits = approval.limits;
    std::string extra;
    if (input >> extra || !scope_files::parse_number(fields[0], result.version) ||
        !scope_files::parse_number(fields[1], approval.max_validity_ms) ||
        !scope_files::parse_number(fields[2], limits.max_order_notional) ||
        !scope_files::parse_number(fields[3], limits.budget_notional) ||
        !scope_files::parse_number(fields[4], limits.max_exposure_notional) ||
        (fields[5] != "0" && !scope_files::parse_number(fields[5], limits.max_price_deviation_bps)) ||
        limits.budget_notional < limits.max_order_notional || limits.max_exposure_notional < limits.max_order_notional ||
        limits.max_price_deviation_bps > 10'000) return std::nullopt;
    return result;
}

inline std::optional<TradeScopeBinding> new_scope_binding(const TradeScopeOwnerKey& key,
                                                            const ScopeRuntimePolicy& policy) noexcept {
    TradeScopeBinding binding;
    binding.store = key.subject();
    binding.key_id = key.key_id();
    binding.policy_version = policy.version;
    constexpr std::string_view strategy = "local-spot-limit-request-v1";
    std::memcpy(binding.strategy_version.data(), strategy.data(), strategy.size());
    if (!detail::fill_random_bytes(binding.activation)) return std::nullopt;
    return binding;
}

inline std::optional<std::filesystem::path> create_scope_request_directory(
    const std::filesystem::path& parent, const TradeScopeBinding& binding) {
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(parent, ec)) || ec) return std::nullopt;
    auto path = parent / ("scope-" + scope_files::hex_id(binding.activation));
    if (!std::filesystem::create_directory(path, ec) || ec) return std::nullopt;
#ifndef _WIN32
    std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
    if (ec) return std::nullopt;
#endif
    return path;
}

inline std::string_view scope_change_name(TradeScopeChange result) noexcept {
    switch (result) {
        case TradeScopeChange::Applied: return "Applied";
        case TradeScopeChange::Refused: return "Refused";
        case TradeScopeChange::WriteUnconfirmed: return "WriteUnconfirmed";
    }
    return "Refused";
}

class ScopeTradingRuntime {
public:
    ScopeTradingRuntime(TradeScopeExecution& execution, const DurableAuditSink& sink, const TradeScopeOwnerKey& owner,
                        const ScopeRuntimePolicy& policy, std::filesystem::path directory)
        : execution_(execution), sink_(sink), owner_(owner), policy_(policy), directory_(std::move(directory)) {}

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

    bool service_control(const SymbolRules& rules, std::uint32_t symbol_id, std::int64_t wall_ms,
                         const InFlightRegistry& in_flight) {
        if (const auto request = scope_files::claim_request(directory_, "revoke")) {
            std::vector<std::byte> contents;
            std::uint64_t serial = 0;
            const auto result = read_request(*request, contents, 20) && scope_files::parse_number(view(contents), serial)
                              ? execution_.revoke(serial, wall_ms) : TradeScopeChange::Refused;
            std::string reply(scope_change_name(result));
            reply += " high_water=" + std::to_string(sink_.trade_scope_high_water());
            if (result == TradeScopeChange::Applied) {
                std::array<ClientOrderId, kMaxInFlight> active{};
                std::size_t count = 0;
                in_flight.visit_active([&](InFlightHandle, std::string_view id) noexcept {
                    std::memcpy(active[count++].id, id.data(), id.size());
                    return true;
                });
                for (const auto& id : std::span(active).first(count)) {
                    reply += " in_flight=";
                    reply += id.view();
                }
            }
            if (!reply_to(*request, reply) || result == TradeScopeChange::WriteUnconfirmed) return false;
        }
        if (const auto request = scope_files::claim_request(directory_, "install")) {
            const auto result = install(*request, rules, symbol_id, wall_ms);
            const auto reply = std::string(scope_change_name(result)) + " high_water=" +
                               std::to_string(sink_.trade_scope_high_water());
            if (!reply_to(*request, reply) || result == TradeScopeChange::WriteUnconfirmed) return false;
        }
        return publish_draft(rules);
    }

    template <typename Pipeline>
    bool service_order(Pipeline& pipeline, const PreflightEnvironment& environment,
                       std::pair<std::int64_t, std::int64_t> book, std::int64_t wall_ms) {
        auto ctx = pipeline.make_context();
        if (!ctx.trade_scope_required || ctx.trade_scope != &execution_) return false;
        // Wait for an account sample obtained after any previous submission.
        if (!ctx.account || ctx.account->timestamp_ms() <= last_order_ms_) return true;
        if (!ctx.in_flight || !ctx.fill_context) return false;
        bool unresolved = false;
        ctx.in_flight->visit_active([&](InFlightHandle, std::string_view id) noexcept {
            const auto* tracked = ctx.fill_context->find(id);
            unresolved = !tracked || tracked->shadow.state == OrderState::Submitting ||
                         tracked->shadow.state == OrderState::Ambiguous ||
                         tracked->shadow.state == OrderState::EscalatedToOperator;
            return !unresolved;
        });
        if (unresolved) return true;
        const auto request = scope_files::claim_request(directory_, "order");
        if (!request) return true;

        std::vector<std::byte> contents;
        if (!read_request(*request, contents, 96)) return reply_to(*request, "InvalidOrder");
        const auto probe = parse_order(view(contents), book, *environment.rules);
        if (!probe) return reply_to(*request, "InvalidOrder");
        const auto reference = probe->side == OrderSide::Buy ? book.second : book.first;
        const auto risk_reference = std::max(reference, probe->price_ticks);
        const auto exposure = marked_exposure(*ctx.account, environment, risk_reference);
        if (!exposure) return reply_to(*request, "AccountExposureUnavailable");
        fill_preflight_order(ctx, *probe, environment);
        ctx.order_wall_ms = wall_ms;
        ctx.scope_reference_price_ticks = reference;
        ctx.exposure_limits.current_exposure_notional = *exposure;

        const auto* scope = execution_.active();
        const auto serial = scope ? scope->serial : 0;
        const auto result = orchestrate_submit(ctx);
        last_order_ms_ = environment.now_ms;
        const auto* balance = sink_.trade_scope_balance(serial);
        const bool accepted = result.gate == OrchestratorGate::SubmitAccepted ||
                              result.gate == OrchestratorGate::SubmitFilled ||
                              result.gate == OrchestratorGate::SubmitPartialFill;
        const std::string reply = std::string(accepted ? "accepted gate=" : "order_result gate=") +
                                  std::to_string(static_cast<unsigned>(result.gate)) +
                                  " state=" + std::to_string(static_cast<unsigned>(result.order.state)) +
                                  " scope=" + std::to_string(serial) +
                                  " used=" + std::to_string(balance ? balance->usage.used_notional : 0) +
                                  " order_id=" + std::to_string(result.order.exchange_order_id);
        return reply_to(*request, reply);
    }

private:
    static std::string_view view(std::span<const std::byte> bytes) noexcept {
        if (bytes.empty()) return {};
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    static bool read_request(const scope_files::Request& request, std::vector<std::byte>& bytes, std::size_t maximum) {
        return read_file_bounded(request.claimed_file.string(), bytes, maximum) == ReadFileStatus::Ok;
    }

    static bool reply_to(const scope_files::Request& request, std::string_view reply) {
        return scope_files::publish_text(request.result_file, reply) == ExportStatus::Ok;
    }

    TradeScopeChange install(const scope_files::Request& request, const SymbolRules& rules,
                             std::uint32_t symbol_id, std::int64_t wall_ms) {
        constexpr auto payload_size = TradeScopeGrantBytes{}.size();
        std::vector<std::byte> contents;
        if (!read_request(request, contents, payload_size + owner_signature::kMaxSignatureBytes) ||
            contents.size() <= payload_size) return TradeScopeChange::Refused;
        const auto binding = execution_.current_binding();
        if (!binding) return TradeScopeChange::Refused;
        const std::span<const std::byte> bytes(contents);
        const auto verified = VerifiedTradeScope::verify(bytes.first(payload_size),
            spot_owner_key_store::unsigned_bytes(bytes.subspan(payload_size)),
            owner_.public_key(), *binding, policy_.approval, execution_.exchange_upper());
        if (!verified) return TradeScopeChange::Refused;
        return execution_.install(*verified, rules, symbol_id, wall_ms);
    }

    bool publish_draft(const SymbolRules& rules) {
        const auto binding = execution_.current_binding();
        const auto upper = execution_.exchange_upper();
        const auto high_water = sink_.trade_scope_high_water();
        if (!binding || !upper || high_water == std::numeric_limits<std::uint64_t>::max()) return true;
        const auto serial = high_water + 1;
        if (serial == last_draft_serial_ && binding->scope_epoch == last_draft_epoch_ && *upper < last_draft_expiry_) {
            return true;
        }

        TradeScopeGrant draft;
        draft.binding = *binding;
        draft.serial = serial;
        draft.issued_at_ms = *upper;
        if (!checked_add_i64(*upper, policy_.approval.max_validity_ms, draft.not_after_ms)) return false;
        std::memcpy(draft.symbol.data(), rules.symbol, draft.symbol.size());
        draft.sides = TradeScopeSides::Both;
        draft.limits = policy_.approval.limits;
        const auto bytes = encode_trade_scope_grant(draft);
        const auto name = "draft-" + std::to_string(serial) + "-" + std::to_string(*upper) + ".grant";
        if (!bytes || spot_owner_key_store::publish_file((directory_ / name).string(), *bytes) != ExportStatus::Ok) return false;
        last_draft_serial_ = serial;
        last_draft_epoch_ = binding->scope_epoch;
        last_draft_expiry_ = draft.not_after_ms;
        return true;
    }

    std::optional<PreflightProbe> parse_order(std::string_view payload, std::pair<std::int64_t, std::int64_t> book,
                                               const SymbolRules& rules) const {
        if (payload == "MIN_BUY" && owner_.subject().environment == StoreEnvironment::Testnet) {
            return plan_preflight_probe(SuggestedAction::Open, book, rules);
        }
        std::istringstream input{std::string(payload)};
        std::string side, price, quantity, extra;
        PreflightProbe probe;
        if (!(input >> side >> price >> quantity) || input >> extra ||
            !scope_files::parse_number(price, probe.price_ticks) ||
            !scope_files::parse_number(quantity, probe.qty_ticks)) return std::nullopt;
        if (side == "BUY") probe.side = OrderSide::Buy;
        else if (side == "SELL") probe.side = OrderSide::Sell;
        else return std::nullopt;
        return probe;
    }

    static std::optional<std::int64_t> marked_exposure(const AccountSnapshot& account,
                                                       const PreflightEnvironment& env, std::int64_t reference) noexcept {
        const auto* base = account.find(env.base_asset);
        const auto* quote = account.find(env.quote_asset);
        std::int64_t base_amount = 0;
        if (reference <= 0 || !quote || (base && !checked_add(base->free_ticks(), base->locked_ticks(), base_amount))) {
            return std::nullopt;
        }
        // Split the product at the balance scale. The product of the two
        // remainders fits in int64_t even when the full scaled product would overflow.
        constexpr std::int64_t unit = 100'000'000;
        static_assert(kBalanceScale == 8);
        std::int64_t price = 0;
        std::int64_t whole = 0;
        std::int64_t fraction = 0;
        std::int64_t remainder = 0;
        std::int64_t marked = 0;
        if (!rescale_notional_ceil(reference, env.rules->price_scale, kBalanceScale, price) ||
            !checked_notional(base_amount / unit, price, whole) ||
            !checked_notional(base_amount % unit, price / unit, fraction) ||
            !rescale_notional_ceil((base_amount % unit) * (price % unit), 2 * kBalanceScale, kBalanceScale, remainder) ||
            !checked_add(whole, fraction, marked) || !checked_add(marked, remainder, marked) ||
            !checked_add(marked, quote->locked_ticks(), marked)) return std::nullopt;
        return marked;
    }

    TradeScopeExecution& execution_;
    const DurableAuditSink& sink_;
    const TradeScopeOwnerKey& owner_;
    const ScopeRuntimePolicy& policy_;
    std::filesystem::path directory_;
    std::uint64_t last_draft_serial_{0};
    TradeScopeEpoch last_draft_epoch_{};
    std::int64_t last_draft_expiry_{0};
    std::int64_t last_order_ms_{0};
};

}  // namespace hy
