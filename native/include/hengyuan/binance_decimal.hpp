// SPDX-License-Identifier: proprietary
// binance_decimal.hpp — Binance L4 §4.2/§5.1 decimal-string -> fixed-point-ticks conversion,
// shared between the account-balance parser (binance_private_rest.hpp) and the exchangeInfo
// symbol-registry parser (symbol_registry.hpp / BinancePrivateRestClient::fetch_exchange_info()).
//
// Governance: L1 (pure logic, no network, no secret) — deliberately independent of
// binance_private_rest.hpp's Boost/Beast/OpenSSL dependency so symbol_registry.hpp (which needs
// none of that) never has to pull in the network stack just to reuse this parsing logic.
//
// Three functions, one responsibility split by "which scale governs this string":
//   - derive_scale_from_decimal_string(): the scale is UNKNOWN and must be derived FROM the
//     string itself (Binance's PRICE_FILTER.tickSize / LOT_SIZE.stepSize).
//   - parse_decimal_to_ticks_with_scale(): the scale is a per-symbol variable already known
//     (typically just derived by the function above) — minPrice/maxPrice/minQty/maxQty/etc.
//   - parse_balance_decimal_to_ticks(): the scale is always the fixed kBalanceScale(8) — §4.2
//     account balances, and §5's MIN_NOTIONAL/NOTIONAL filter (spec: same scale as balances).

#pragma once

#include <hengyuan/account_truth.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>

namespace hy {

// L4 §5.1: derives the number of significant fractional decimal digits a Binance
// PRICE_FILTER.tickSize / LOT_SIZE.stepSize decimal string implies, e.g.:
//   "0.00010000" -> strip trailing zeros -> "0.0001" -> scale=4
//   "1.00000000" -> strip trailing zeros -> "1"      -> scale=0
//   "0.00000001" ->                                     scale=8
// Not a plain "count digits after the decimal point": Binance formats these fields as a
// fixed-width 8-fractional-digit decimal regardless of the symbol's real precision, so the
// trailing zeros must be stripped, not counted.
//
// Fails closed (returns false, out_scale untouched) on: empty input, a leading '-' (tick/step
// sizes are never negative), any non-digit character, a bare '.' with no leading digit or no
// trailing digit, more than 18 fractional digits (defensive input-length bound; real Binance
// data is always <= 8), and — deliberately — an input whose numeric value is exactly zero
// ("0" or "0.00000000" alike). A tick size / step size is never legitimately zero in a real
// Binance symbol filter (a zero increment is meaningless); a "successfully parsed" zero scale
// derived from a zero-valued tickSize would silently produce a degenerate, incorrect scale
// downstream rather than surfacing the malformed input. This does not conflict with
// validate_pre_trade()'s existing `if (rules.tick_size_ticks > 0)` sentinel check
// (account_truth.hpp) — that one guards a SymbolRules never populated by this parser at all
// (a genuinely absent filter), not a filter present in the response but carrying an
// out-of-spec zero value.
inline bool derive_scale_from_decimal_string(std::string_view s, std::uint8_t& out_scale) noexcept {
    if (s.empty()) return false;
    if (s[0] == '-') return false;  // tick sizes / step sizes are never negative

    std::size_t pos = 0;
    bool any_integer_digit = false;
    bool integer_nonzero = false;
    while (pos < s.size() && s[pos] != '.') {
        const char c = s[pos];
        if (c < '0' || c > '9') return false;
        any_integer_digit = true;
        if (c != '0') integer_nonzero = true;
        ++pos;
    }
    if (!any_integer_digit) return false;  // e.g. ".5" -- no leading integer digit

    std::size_t frac_len = 0;
    std::size_t last_nonzero = 0;  // count of fractional digits up to and including the last nonzero one
    bool frac_nonzero = false;
    if (pos < s.size()) {
        if (s[pos] != '.') return false;  // unreachable given the loop above, kept explicit
        ++pos;
        if (pos == s.size()) return false;  // trailing '.' with no digits after it ("5.")
        while (pos < s.size()) {
            const char c = s[pos];
            if (c < '0' || c > '9') return false;
            ++frac_len;
            if (frac_len > 18) return false;  // fail closed on unreasonably long input
            if (c != '0') {
                frac_nonzero = true;
                last_nonzero = frac_len;
            }
            ++pos;
        }
    }

    if (!integer_nonzero && !frac_nonzero) return false;  // "0", "0.0", "0.00000000" -- reject

    out_scale = static_cast<std::uint8_t>(last_nonzero);  // trailing zeros stripped
    return true;
}

// L4 §5.1: converts minPrice/maxPrice/tickSize/minQty/maxQty/stepSize — decimal strings already
// known to be at a specific per-symbol `scale` (typically just derived by
// derive_scale_from_decimal_string() above from the sibling tickSize/stepSize field) — into
// integer ticks at that same scale. Unlike parse_balance_decimal_to_ticks() below, `scale` is a
// caller-supplied variable, not the fixed kBalanceScale.
//
// Decimal-digit handling deliberately does NOT simply reject a fractional part longer than
// `scale`: Binance formats these fields as fixed-width 8-fractional-digit decimals even when a
// symbol's real, derived scale is smaller. For example a symbol with stepSize="1.00000000"
// (derived qty_scale=0) will report minQty as "1.00000000" too, not "1" — rejecting on sight of
// a decimal point whenever scale==0 would incorrectly refuse Binance's own valid data. The rule
// actually needed, checked digit-by-digit past the decimal point:
//   - digits 1..scale: accumulate normally into the fractional value (exactly like
//     parse_balance_decimal_to_ticks's kBalanceScale-digit accumulation below).
//   - digits scale+1 and beyond: must be '0'. All-zero here is Binance's own padding and is
//     silently accepted; any nonzero digit past `scale` means this value's true precision
//     exceeds the symbol's derived scale, which fails closed (returns false) rather than
//     truncating that excess precision and reporting a smaller number than the field actually
//     specifies.
//
// `scale` itself must be in [0,18] (rescale_notional_ceil()'s own valid domain, account_truth.hpp)
// — pow10_i64() fails closed beyond that range, which this function fails through as `false`.
inline bool parse_decimal_to_ticks_with_scale(std::string_view s, std::uint8_t scale,
                                               std::int64_t& out_ticks) noexcept {
    if (s.empty()) return false;
    if (s[0] == '-') return false;  // price/qty ticks are never negative

    std::size_t pos = 0;
    std::int64_t integer_part = 0;
    while (pos < s.size() && s[pos] != '.') {
        const char c = s[pos];
        if (c < '0' || c > '9') return false;
        const int digit = c - '0';
        if (integer_part > (std::numeric_limits<std::int64_t>::max() - digit) / 10) return false;
        integer_part = integer_part * 10 + digit;
        ++pos;
    }

    std::int64_t frac_part = 0;
    std::uint8_t frac_digits = 0;
    if (pos < s.size()) {
        if (s[pos] != '.') return false;  // unreachable given the loop above
        ++pos;
        if (pos == s.size()) return false;  // trailing '.' with no digits after it ("5.")
        while (pos < s.size()) {
            const char c = s[pos];
            if (c < '0' || c > '9') return false;
            if (frac_digits < scale) {
                frac_part = frac_part * 10 + (c - '0');
                ++frac_digits;
            } else if (c != '0') {
                // Nonzero digit past the target scale: real excess precision, fail closed
                // rather than silently truncating it away. See the function comment above.
                return false;
            }
            ++pos;
        }
    }

    // Pad fewer-than-`scale` fractional digits up to `scale` (e.g. scale=4, "1.5" -> frac 5 at
    // 1 digit becomes 5000 at 4 digits) so the final combine below is a single fixed-width scale
    // throughout — the same technique parse_balance_decimal_to_ticks uses for kBalanceScale,
    // generalized here to a caller-supplied scale via pow10_i64() (account_truth.hpp).
    std::int64_t pow_scale = 0;
    if (!pow10_i64(scale, pow_scale)) return false;
    std::int64_t pow_frac = 0;
    if (!pow10_i64(frac_digits, pow_frac)) return false;
    const std::int64_t pad = pow_scale / pow_frac;
    if (frac_part > std::numeric_limits<std::int64_t>::max() / pad) return false;
    frac_part *= pad;

    if (integer_part > (std::numeric_limits<std::int64_t>::max() - frac_part) / pow_scale) {
        return false;
    }
    out_ticks = integer_part * pow_scale + frac_part;
    return true;
}

// §4.2: lossless decimal-string -> fixed-point ticks at account_truth.hpp's kBalanceScale (8).
// Deliberately NOT binance_json_parser.hpp::parse_decimal_to_fixed() reused as-is: that
// function silently TRUNCATES a fractional part longer than its target scale (verified by
// reading its implementation) -- correct for its own WS/L1 hot-path use (streaming price/qty,
// governed by a different spec section with its own truncation-is-fine precedent), but exactly
// the "never truncate a balance silently and proceed with a smaller number than the account
// actually holds" failure §4.2 explicitly forbids. This is a fresh, deliberately stricter
// parser: any of a non-digit character, more than 8 fractional digits, a negative sign
// (balances are never negative), an empty string, or std::int64_t overflow at scale 8 all
// REJECT (return false, `out_ticks` untouched) rather than truncating or clamping.
//
// Also reused by L4 §5's MIN_NOTIONAL/NOTIONAL filter parsing (symbol_registry.hpp /
// fetch_exchange_info()) — the spec fixes that filter's minNotional at kBalanceScale, the same
// scale as account balances, so no per-symbol scale derivation is needed there either.
inline bool parse_balance_decimal_to_ticks(std::string_view s, std::int64_t& out_ticks) noexcept {
    if (s.empty()) return false;
    if (s[0] == '-') return false;  // balances are never negative

    std::size_t pos = 0;
    std::int64_t integer_part = 0;
    while (pos < s.size() && s[pos] != '.') {
        const char c = s[pos];
        if (c < '0' || c > '9') return false;
        const int digit = c - '0';
        if (integer_part > (std::numeric_limits<std::int64_t>::max() - digit) / 10) return false;
        integer_part = integer_part * 10 + digit;
        ++pos;
    }

    std::int64_t frac_part = 0;
    int frac_digits = 0;
    if (pos < s.size()) {
        // Unreachable given the loop above (it only stops early on '.'), kept as an explicit
        // precondition rather than an assumption.
        if (s[pos] != '.') return false;
        ++pos;
        if (pos == s.size()) return false;  // trailing '.' with no digits after it ("5.")
        while (pos < s.size()) {
            const char c = s[pos];
            if (c < '0' || c > '9') return false;
            // The reject-not-truncate case §4.2 exists for.
            if (frac_digits >= kBalanceScale) return false;
            frac_part = frac_part * 10 + (c - '0');
            ++frac_digits;
            ++pos;
        }
    }

    // Pad fewer-than-kBalanceScale fractional digits up to kBalanceScale (e.g. "1.5" -> frac
    // 5 at 1 digit becomes 50000000 at 8 digits) so the final combine below is a single
    // fixed-width scale throughout.
    std::int64_t scale = 1;
    for (int i = 0; i < kBalanceScale; ++i) scale *= 10;  // 10^8 -- fits trivially in int64
    std::int64_t frac_scale = 1;
    for (int i = 0; i < frac_digits; ++i) frac_scale *= 10;
    const std::int64_t pad = scale / frac_scale;
    if (frac_part > std::numeric_limits<std::int64_t>::max() / pad) return false;
    frac_part *= pad;

    if (integer_part > (std::numeric_limits<std::int64_t>::max() - frac_part) / scale) {
        return false;
    }
    out_ticks = integer_part * scale + frac_part;
    return true;
}

// TODO 1A.3: the reverse direction of parse_decimal_to_ticks_with_scale() -- a real
// POST /api/v3/order request must send price/quantity as decimal strings ("50000.12",
// "0.001000"), and this codebase had no ticks->string formatter until now (only the
// string->ticks parse direction existed). Pure integer division/modulo, zero heap
// allocation, caller-owned stack buffer -- no double, no std::string concatenation,
// consistent with this file's own arithmetic discipline throughout.
//
// scale==0 formats as a bare integer (no decimal point) -- Binance's own documented
// examples do this for whole-number fields; `%0*lld` for the fractional part
// zero-pads to exactly `scale` digits (e.g. scale=6, frac=1 -> "000001", never "1"),
// matching the fixed-width fractional format Binance itself sends and this file's own
// parse functions expect back.
//
// Fails closed (false, out_len untouched) on: ticks<0 (price/qty ticks are never
// negative), scale>18 (pow10_i64()'s own domain), an out_buf too small for the
// formatted result (snprintf's truncation-detection convention: return value >=
// buffer size means truncated), or an internal pow10_i64()/snprintf failure.
inline bool format_ticks_to_decimal(std::int64_t ticks, std::uint8_t scale,
                                     std::span<char> out_buf, std::size_t& out_len) noexcept {
    if (ticks < 0 || out_buf.empty()) return false;

    if (scale == 0) {
        const int n = std::snprintf(out_buf.data(), out_buf.size(), "%lld",
                                     static_cast<long long>(ticks));
        if (n <= 0 || static_cast<std::size_t>(n) >= out_buf.size()) return false;
        out_len = static_cast<std::size_t>(n);
        return true;
    }

    std::int64_t divisor = 0;
    if (!pow10_i64(scale, divisor)) return false;  // scale > 18 -- pow10_i64's own domain

    const std::int64_t int_part = ticks / divisor;
    const std::int64_t frac_part = ticks % divisor;

    const int n = std::snprintf(out_buf.data(), out_buf.size(), "%lld.%0*lld",
                                 static_cast<long long>(int_part), static_cast<int>(scale),
                                 static_cast<long long>(frac_part));
    if (n <= 0 || static_cast<std::size_t>(n) >= out_buf.size()) return false;
    out_len = static_cast<std::size_t>(n);
    return true;
}

}  // namespace hy
