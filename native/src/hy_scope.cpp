// SPDX-License-Identifier: proprietary
#include <hengyuan/owner_p256_signature.hpp>
#include <hengyuan/spot_owner_key_store.hpp>
#include <hengyuan/trade_scope_grant.hpp>
#include <hengyuan/trade_scope_owner_key.hpp>
#include <hengyuan/trade_scope_request_files.hpp>

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <thread>
#include <cinttypes>
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace files = hy::spot_owner_key_store;
using hy::TradeScopeGrant;
using hy::TradeScopeSides;

template <std::size_t Size>
bool read_identifier(std::string_view text, std::array<std::byte, Size>& identifier) noexcept {
    if (text.size() != 2 * Size) return false;
    for (std::size_t i = 0; i < Size; ++i) {
        const auto* begin = text.data() + 2 * i;
        unsigned int byte = 0;
        const auto word = std::from_chars(begin, begin + 2, byte, 16);
        if (word.ec != std::errc{} || word.ptr != begin + 2 || byte > 255) return false;
        identifier[i] = static_cast<std::byte>(byte);
    }
    return true;
}

std::optional<TradeScopeGrant> read_draft(const char* path) {
    std::vector<std::byte> input;
    if (hy::read_file_bounded(path, input, 8192) != hy::ReadFileStatus::Ok || input.empty()) return std::nullopt;
    simdjson::dom::parser parser;
    simdjson::dom::object object;
    const auto* data = reinterpret_cast<const char*>(input.data());
    if (parser.parse(data, input.size()).get(object) != simdjson::SUCCESS || object.size() != 18) return std::nullopt;

    TradeScopeGrant grant;
    auto& binding = grant.binding;
    std::string_view environment, activation, strategy, symbol, sides;
    std::uint64_t deviation = 0;
    if (object["environment"].get_string().get(environment) ||
        object["account_uid"].get_uint64().get(binding.store.account_uid) ||
        object["store_uuid_lo"].get_uint64().get(binding.store.store_uuid_lo) ||
        object["store_uuid_hi"].get_uint64().get(binding.store.store_uuid_hi) ||
        object["activation"].get_string().get(activation) ||
        object["scope_epoch"].get_uint64().get(binding.scope_epoch.value) ||
        object["policy_version"].get_uint64().get(binding.policy_version) ||
        object["strategy_version"].get_string().get(strategy) ||
        object["key_id"].get_uint64().get(binding.key_id) ||
        object["serial"].get_uint64().get(grant.serial) ||
        object["issued_at_ms"].get_int64().get(grant.issued_at_ms) ||
        object["not_after_ms"].get_int64().get(grant.not_after_ms) ||
        object["symbol"].get_string().get(symbol) ||
        object["sides"].get_string().get(sides) ||
        object["max_order_notional"].get_int64().get(grant.limits.max_order_notional) ||
        object["budget_notional"].get_int64().get(grant.limits.budget_notional) ||
        object["max_exposure_notional"].get_int64().get(grant.limits.max_exposure_notional) ||
        object["max_price_deviation_bps"].get_uint64().get(deviation)) return std::nullopt;

    if (environment == "TESTNET") binding.store.environment = hy::StoreEnvironment::Testnet;
    else if (environment == "PRODUCTION") binding.store.environment = hy::StoreEnvironment::Production;
    else return std::nullopt;

    if (sides == "BUY") grant.sides = TradeScopeSides::Buy;
    else if (sides == "SELL") grant.sides = TradeScopeSides::Sell;
    else if (sides == "BOTH") grant.sides = TradeScopeSides::Both;
    else return std::nullopt;

    if (!hy::cancel_prepared::valid_symbol(symbol) || !read_identifier(activation, binding.activation) ||
        !read_identifier(strategy, binding.strategy_version) || deviation > 10'000) return std::nullopt;
    std::copy(symbol.begin(), symbol.end(), grant.symbol.begin());
    grant.limits.max_price_deviation_bps = static_cast<std::uint32_t>(deviation);
    if (!hy::valid_trade_scope_grant(grant)) return std::nullopt;
    return grant;
}

void print_identifier(const char* label, std::span<const std::byte> identifier) {
    std::printf("%s=", label);
    for (const auto byte : identifier) std::printf("%02x", std::to_integer<unsigned int>(byte));
    std::putchar('\n');
}

void print_grant(const TradeScopeGrant& grant) {
    const auto& binding = grant.binding;
    const auto& limits = grant.limits;
    std::printf("format=1\nsignature_algorithm=P256\n"
                "environment=%s\naccount_uid=%" PRIu64 "\nstore_uuid_lo=%" PRIu64 "\nstore_uuid_hi=%" PRIu64 "\n",
                binding.store.environment == hy::StoreEnvironment::Production ? "PRODUCTION" : "TESTNET",
                binding.store.account_uid, binding.store.store_uuid_lo, binding.store.store_uuid_hi);
    print_identifier("activation", binding.activation);
    print_identifier("strategy_version", binding.strategy_version);
    const char* side = grant.sides == TradeScopeSides::Both ? "BOTH" : (grant.sides == TradeScopeSides::Buy ? "BUY" : "SELL");
    std::printf("scope_epoch=%" PRIu64 "\npolicy_version=%" PRIu64 "\nkey_id=%" PRIu64 "\nserial=%" PRIu64
                "\nissued_at_ms=%" PRId64 "\nnot_after_ms=%" PRId64 "\nsymbol=%s\nsides=%s\norder_type=LIMIT\n",
                binding.scope_epoch.value, binding.policy_version, binding.key_id, grant.serial,
                grant.issued_at_ms, grant.not_after_ms, grant.symbol.data(), side);
    std::printf("max_order_notional=%" PRId64 "\nbudget_notional=%" PRId64 "\nmax_exposure_notional=%" PRId64
                "\nmax_price_deviation_bps=%u\n",
                limits.max_order_notional, limits.budget_notional, limits.max_exposure_notional,
                static_cast<unsigned>(limits.max_price_deviation_bps));
}

int request_runtime(const std::filesystem::path& directory, std::string_view operation,
                    std::span<const std::byte> data) {
    auto request = hy::scope_files::publish_request(directory, operation, data);
    if (!request) {
        std::fputs("Could not publish request. Check the active control directory.\n", stderr);
        return 1;
    }
    request->replace_extension(".result");
    std::printf("Waiting for %s\n", request->string().c_str());
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < end) {
        if (files::is_regular_file(request->string())) {
            std::vector<std::byte> result;
            if (hy::read_file_bounded(request->string(), result, 4096) != hy::ReadFileStatus::Ok || result.empty()) break;
            const std::string_view reply(reinterpret_cast<const char*>(result.data()), result.size());
            std::printf("%.*s\n", static_cast<int>(reply.size()), reply.data());
            if (operation != "order") return reply.starts_with("Applied ") ? 0 : 1;
            return reply.starts_with("accepted ") ? 0 : 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::fputs("Result unavailable. Inspect this request's result before issuing another.\n", stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (command == "draft" && argc == 4) {
        const auto draft = read_draft(argv[2]);
        const auto payload = draft ? hy::encode_trade_scope_grant(*draft) : std::nullopt;
        if (!payload) {
            std::fputs("Invalid trade scope draft.\n", stderr);
            return 1;
        }
        const auto status = files::publish_file(argv[3], *payload);
        if (status != hy::ExportStatus::Ok) {
            std::fprintf(stderr, "Could not publish trade scope draft (%s).\n", hy::export_status_name(status));
            return 1;
        }
        print_grant(*draft);
        return 0;
    }

    if (command == "install" && argc == 5) {
        std::vector<std::byte> grant, signature;
        if (hy::read_file_bounded(argv[3], grant, hy::TradeScopeGrantBytes{}.size()) != hy::ReadFileStatus::Ok ||
            !hy::decode_trade_scope_grant(grant) ||
            hy::read_file_bounded(argv[4], signature, hy::owner_signature::kMaxSignatureBytes) != hy::ReadFileStatus::Ok ||
            signature.empty()) {
            std::fputs("Invalid scope grant or signature file.\n", stderr);
            return 1;
        }
        grant.insert(grant.end(), signature.begin(), signature.end());
        return request_runtime(argv[2], "install", grant);
    }

    if (command == "revoke" && argc == 4) {
        const std::string_view serial_text = argv[3];
        std::uint64_t serial = 0;
        if (!hy::scope_files::parse_number(serial_text, serial)) {
            std::fputs("A positive scope serial is required.\n", stderr);
            return 1;
        }
        return request_runtime(argv[2], "revoke", std::as_bytes(std::span(serial_text)));
    }

    if (command == "submit" && argc == 6) {
        const std::string_view side = argv[3];
        std::int64_t price = 0, quantity = 0;
        if ((side != "BUY" && side != "SELL") || !hy::scope_files::parse_number(argv[4], price) ||
            !hy::scope_files::parse_number(argv[5], quantity)) {
            std::fputs("Order needs BUY or SELL and positive price and quantity ticks.\n", stderr);
            return 1;
        }
        const auto text = std::string(side) + " " + std::to_string(price) + " " + std::to_string(quantity);
        return request_runtime(argv[2], "order", std::as_bytes(std::span(text)));
    }

    if (command == "first-order-draft" && argc == 7) {
        files::StoreFiles production{argv[2], 0, argv[4], argv[5]};
        if (!hy::scope_files::parse_number(argv[3], production.audit_key_id)) return 1;
        const auto result = hy::TradeScopeOwnerKey::publish_first_order_draft(production, argv[6]);
        if (result != hy::ExportStatus::Ok) {
            std::fprintf(stderr, "Could not publish first-order declaration (%s).\n", hy::export_status_name(result));
            return 1;
        }
        return 0;
    }

    if ((command == "inspect" && argc == 3) || (command == "verify" && argc == 5)) {
        std::vector<std::byte> payload;
        if (hy::read_file_bounded(argv[2], payload, hy::TradeScopeGrantBytes{}.size()) != hy::ReadFileStatus::Ok) {
            std::fputs("Could not read trade scope payload.\n", stderr);
            return 1;
        }
        const auto grant = hy::decode_trade_scope_grant(payload);
        if (!grant) {
            std::fputs("Invalid trade scope payload.\n", stderr);
            return 1;
        }
        if (command == "verify") {
            std::vector<std::byte> signature, public_key;
            if (hy::read_file_bounded(argv[3], signature, hy::owner_signature::kMaxSignatureBytes) != hy::ReadFileStatus::Ok ||
                hy::read_file_bounded(argv[4], public_key, hy::owner_signature::kMaxPublicKeyBytes) != hy::ReadFileStatus::Ok) {
                std::fputs("Could not read signature or public key.\n", stderr);
                return 1;
            }
            const auto key = hy::owner_signature::decode_p256(files::unsigned_bytes(public_key));
            if (!key || !hy::owner_signature::verify_p256(*key, payload, files::unsigned_bytes(signature))) {
                std::fputs("Trade scope signature refused.\n", stderr);
                return 1;
            }
            std::puts("signature=verified");
        }
        print_grant(*grant);
        return 0;
    }

    std::fputs("Usage\n  hy_scope draft <draft.json> <grant>\n  hy_scope inspect <grant>\n"
               "  hy_scope verify <grant> <signature.der> <public-key.der>\n"
               "  hy_scope first-order-draft <log> <audit-key-id> <kek> <wrapped-keys> <declaration>\n"
               "  hy_scope install <active-control-dir> <grant> <signature.der>\n"
               "  hy_scope revoke <active-control-dir> <serial>\n"
               "  hy_scope submit <active-control-dir> <BUY|SELL> <price-ticks> <quantity-ticks>\n", stderr);
    return 1;
}
