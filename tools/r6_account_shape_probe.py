#!/usr/bin/env python3
"""Which check of the native account parser refuses the Binance Spot Testnet's GET /api/v3/account answer?

Why it exists: the preflight harness stopped with `PrivateRestError=MalformedResponse` while reading the account uid. That means HTTP 200 and a
well-formed JSON body that native/include/hengyuan/binance_private_rest.hpp::parse_account_response_impl() refused -- but the parser reports only THAT
it refused, not which check. This probe sends the same read-only request, re-applies the parser's checks in the same order (a Python mirror of
parse_account_response_impl, parse_balance_decimal_to_ticks and AccountSnapshotBuilder::add_asset) and prints the first one that fails.

What it never prints: the API key, the secret, a signature, the uid VALUE, any balance AMOUNT, any URL. What it prints: field names and JSON types,
counts, digit counts, the asset NAMES of the non-zero assets and of failing entries (public ticker symbols), and the verdict. It sends GET requests
only, to testnet.binance.vision only, and writes no file.

Credentials: either pair of names, from the environment or from the file named by --env-file (default ./.env):
    HENGYUAN_OPERATOR_TESTNET_API_KEY / HENGYUAN_OPERATOR_TESTNET_API_SECRET     (the R6 probe's names)
    HENGYUAN_BINANCE_TESTNET_API_KEY  / HENGYUAN_BINANCE_TESTNET_SECRET          (the preflight harness' names)
The key needs only USER_DATA for this request.

The C++ parser is the authority: this is a mirror of it and can drift (native/tests/test_binance_private_rest.cpp pins the C++ side, ParseAccountResponseScope and
FI-033). Since the account asset scope (L4 spec 4.3.1) the harness declares the assets it needs, so the refusal this probe explains no longer stops it; the probe stays
useful for the next unexpected answer.

Run (PowerShell, from the repository root):
    python tools/r6_account_shape_probe.py --env-file .env
The last block of the output is a small JSON object that is safe to paste into the chat. Python 3.8+, standard library only.
An AI session never runs this with real credentials (CLAUDE.md); the owner does.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r6_testnet_probe as r6  # noqa: E402  (same directory: tools/)

NAME_PAIRS = (
    ("HENGYUAN_OPERATOR_TESTNET_API_KEY", "HENGYUAN_OPERATOR_TESTNET_API_SECRET"),
    ("HENGYUAN_BINANCE_TESTNET_API_KEY", "HENGYUAN_BINANCE_TESTNET_SECRET"),
)

# The native limits this mirrors (account_truth.hpp, binance_decimal.hpp):
INT64_MAX = (1 << 63) - 1
UINT64_MAX = (1 << 64) - 1
MAX_ASSETS = 32         # kMaxAssets: more NON-ZERO assets than this -> CapacityExceeded
ASSET_NAME_LEN = 12     # kAssetNameLen: a name must be SHORTER than this (bytes)
SCALE_DIGITS = 8        # kBalanceScale


def credentials(env_file):
    file_values = r6.read_env_file(env_file)
    for key_name, secret_name in NAME_PAIRS:
        key = os.environ.get(key_name) or file_values.get(key_name)
        secret = os.environ.get(secret_name) or file_values.get(secret_name)
        if key and secret:
            return key, secret, key_name
    return None, None, None


def type_name(value):
    if isinstance(value, bool):
        return "bool"
    if value is None:
        return "null"
    return {"int": "int", "float": "float", "str": "string", "list": "array", "dict": "object"}.get(type(value).__name__, type(value).__name__)


def char_class(c):
    if c in "eE":
        return "exponent"
    if c == "+":
        return "plus"
    if c.isspace():
        return "space"
    if c.isalpha():
        return "letter"
    return "other"


def ticks_of(text):
    """Mirror of parse_balance_decimal_to_ticks(): (ticks, None), or (None, the first reason it refuses)."""
    if not text:
        return None, "empty"
    if text[0] == "-":
        return None, "negative_sign"
    pos = 0
    n = len(text)
    integer_part = 0
    while pos < n and text[pos] != ".":
        c = text[pos]
        if c < "0" or c > "9":
            return None, "non_digit_in_integer_part(" + char_class(c) + ")"
        digit = ord(c) - 48
        if integer_part > (INT64_MAX - digit) // 10:
            return None, "integer_part_overflow"
        integer_part = integer_part * 10 + digit
        pos += 1
    frac_part = 0
    frac_digits = 0
    if pos < n:
        pos += 1  # the '.'
        if pos == n:
            return None, "trailing_dot"
        while pos < n:
            c = text[pos]
            if c < "0" or c > "9":
                return None, "non_digit_in_fraction(" + char_class(c) + ")"
            if frac_digits >= SCALE_DIGITS:
                return None, "more_than_8_fraction_digits"
            frac_part = frac_part * 10 + (ord(c) - 48)
            frac_digits += 1
            pos += 1
    scale = 10 ** SCALE_DIGITS
    pad = scale // (10 ** frac_digits)
    if frac_part > INT64_MAX // pad:
        return None, "fraction_overflow"
    frac_part *= pad
    if integer_part > (INT64_MAX - frac_part) // scale:
        return None, "overflow_at_scale_8"
    return integer_part * scale + frac_part, None


def asset_name_problem(name):
    """Mirror of the name checks of AccountSnapshotBuilder::add_asset() (bytes, as in the native code)."""
    raw = name.encode("utf-8", "replace")
    if not raw:
        return "AssetNameEmpty"
    if len(raw) >= ASSET_NAME_LEN:
        return "AssetNameTooLong"
    for b in raw:
        if b < 0x21 or b > 0x7E:
            return "AssetNameInvalid"
    return None


def shown_name(name):
    """An asset name is a public ticker; anything odd is described, never echoed."""
    if isinstance(name, str) and 0 < len(name) <= 24 and all(0x21 <= ord(c) <= 0x7E for c in name):
        return name
    return "<%s, %d chars>" % (type_name(name), len(name) if isinstance(name, str) else 0)


def digits_of(text):
    head, _, tail = text.partition(".")
    return len(head), len(tail)


def analyze(text):
    """Apply parse_account_response_impl()'s checks, in its order, to a response body. Returns a dict that is safe to print."""
    out = {"bytes": len(text.encode("utf-8"))}
    try:
        doc = json.loads(text)
    except ValueError:
        out["json"] = "invalid"
        out["native_parser_verdict"] = {"outcome": "JsonParse"}
        return out
    out["json"] = "valid"

    def verdict(outcome, where, reason):
        return {"outcome": outcome, "first_failure": where + ": " + reason}

    if not isinstance(doc, dict):
        out["top_level"] = type_name(doc)
        out["native_parser_verdict"] = verdict("MalformedResponse", "top level", "not an object")
        return out

    keys = list(doc.keys())
    out["top_level_keys_in_order"] = ["%s:%s" % (k, type_name(doc[k])) for k in keys]
    first = None  # the native parser's verdict: the first failing check

    # 1. canTrade, a JSON bool
    can_trade = doc.get("canTrade", None)
    out["canTrade"] = type_name(can_trade) if "canTrade" in doc else "missing"
    if first is None and not isinstance(can_trade, bool):
        first = verdict("MalformedResponse", "canTrade", out["canTrade"] + " (a bool is required)")

    # 2. uid: an unsigned 64-bit JSON integer, not 0 (read BEFORE the balances; the field may come after them in the document)
    if "uid" not in doc:
        out["uid"] = {"present": False}
        if first is None:
            first = verdict("MalformedResponse", "uid", "missing")
    else:
        uid = doc["uid"]
        is_int = isinstance(uid, int) and not isinstance(uid, bool)
        info = {"present": True, "type": type_name(uid)}
        if is_int:
            info.update(digits=len(str(abs(uid))), nonzero=uid != 0, negative=uid < 0, fits_uint64=0 <= uid <= UINT64_MAX,
                        fits_int64=-(1 << 63) <= uid <= INT64_MAX)
        if "balances" in doc:
            info["position"] = "after balances" if keys.index("uid") > keys.index("balances") else "before balances"
        out["uid"] = info
        if first is None and not (is_int and 0 < uid <= UINT64_MAX):
            reason = "not an unsigned 64-bit integer" if not is_int or uid < 0 or uid > UINT64_MAX else "zero"
            first = verdict("MalformedResponse", "uid", reason + " (type " + type_name(uid) + ")")

    # 3. balances: an array
    balances = doc.get("balances", None)
    if not isinstance(balances, list):
        out["balances"] = {"present": "balances" in doc, "type": type_name(balances) if "balances" in doc else "missing"}
        if first is None:
            first = verdict("MalformedResponse", "balances", out["balances"]["type"] + " (an array is required)")
        out["native_parser_verdict"] = first or {"outcome": "None"}
        return out

    # 4. every entry, in order (the native parser stops at the first problem; this keeps counting to show every class)
    problems = {}
    first_problems = []
    key_sets = {}
    zero_entries = 0
    nonzero = []
    seen = set()
    capacity_hit = False
    max_int_digits = {"free": 0, "locked": 0}

    def note(index, asset, field, reason, outcome):
        nonlocal first
        problems[reason] = problems.get(reason, 0) + 1
        if len(first_problems) < 8:
            first_problems.append({"index": index, "asset": asset, "field": field, "reason": reason})
        if first is None:
            first = verdict(outcome, "balances[%d].%s (asset=%s)" % (index, field, asset), reason)

    for index, entry in enumerate(balances):
        if not isinstance(entry, dict):
            note(index, "<not an object>", "entry", "entry_not_an_object", "MalformedResponse")
            continue
        sig = ",".join(sorted(entry.keys()))
        key_sets[sig] = key_sets.get(sig, 0) + 1
        asset = entry.get("asset", None)
        shown = shown_name(asset)
        bad = False
        for field in ("asset", "free", "locked"):
            if not isinstance(entry.get(field, None), str):
                note(index, shown, field, "field_missing_or_not_a_string(" + type_name(entry.get(field, None)) + ")", "MalformedResponse")
                bad = True
                break
        if bad:
            continue
        free_ticks, free_reason = ticks_of(entry["free"])
        if free_reason is not None:
            note(index, shown, "free", free_reason, "MalformedResponse")
            continue
        locked_ticks, locked_reason = ticks_of(entry["locked"])
        if locked_reason is not None:
            note(index, shown, "locked", locked_reason, "MalformedResponse")
            continue
        if free_ticks == 0 and locked_ticks == 0:
            zero_entries += 1
            continue
        # a balance that would take a slot: AccountSnapshotBuilder::add_asset()
        free_digits = digits_of(entry["free"])
        locked_digits = digits_of(entry["locked"])
        max_int_digits["free"] = max(max_int_digits["free"], free_digits[0])
        max_int_digits["locked"] = max(max_int_digits["locked"], locked_digits[0])
        nonzero.append({"asset": shown, "free_int_digits": free_digits[0], "free_frac_digits": free_digits[1],
                        "locked_int_digits": locked_digits[0], "locked_frac_digits": locked_digits[1]})
        name_problem = asset_name_problem(asset)
        if name_problem is not None:
            note(index, shown, "asset", name_problem, "MalformedResponse")
            continue
        if free_ticks > INT64_MAX - locked_ticks:
            note(index, shown, "free+locked", "BalanceOverflow", "MalformedResponse")
            continue
        if asset in seen:
            note(index, shown, "asset", "DuplicateAsset", "MalformedResponse")
            continue
        if len(seen) >= MAX_ASSETS:
            capacity_hit = True
            note(index, shown, "asset", "CapacityExceeded", "CapacityExceeded")
            continue
        seen.add(asset)

    out["balances"] = {
        "entries": len(balances),
        "entry_key_sets": key_sets,
        "zero_entries": zero_entries,
        "nonzero_entries": len(nonzero),
        "native_capacity": MAX_ASSETS,
        "capacity_exceeded": capacity_hit,
        "max_integer_digits_of_nonzero": max_int_digits,
        "nonzero_assets": nonzero[:40],
        "problem_counts": problems,
        "first_problems": first_problems,
    }
    out["native_parser_verdict"] = first or {"outcome": "None"}
    return out


def main():
    parser = argparse.ArgumentParser(description="Which check of the native account parser refuses the Testnet's account answer (see the module docstring).")
    parser.add_argument("--env-file", default=".env", help="file with either pair of Testnet credential names (default ./.env)")
    parser.add_argument("--timeout", type=float, default=15.0, help="seconds per request (default 15)")
    args = parser.parse_args()

    key, secret, pair = credentials(args.env_file)
    if not key or not secret:
        print("error: neither credential pair is set (environment, or %s); names looked for: %s"
              % (args.env_file, " | ".join(k + " + " + s for k, s in NAME_PAIRS)), file=sys.stderr)
        return 2

    probe = r6.Probe(key, secret, args.timeout)
    print("== Testnet account shape probe (read-only, GET only; no key, secret, signature, uid value or amount is printed) ==")
    print("credential names used: %s" % pair)

    t0 = int(time.time() * 1000)
    status, text = probe.public("/api/v3/time")
    t1 = int(time.time() * 1000)
    body = r6.parse_json(text) if status == 200 else None
    if not isinstance(body, dict) or "serverTime" not in body:
        print("[1] GET /api/v3/time -> %s %s" % ("HTTP %d" % status if status else "transport error", text[:120] if status else text))
        return 1
    probe.offset_ms = int(body["serverTime"]) - (t0 + t1) // 2
    print("[1] GET /api/v3/time -> HTTP 200; clock offset %+d ms; round trip %d ms" % (probe.offset_ms, t1 - t0))

    status, text = probe.signed("/api/v3/account", {})
    if status != 200:
        code, msg = r6.error_fields(text) if status else (None, text)
        print("[2] GET /api/v3/account -> %s code=%s msg=%r" % ("HTTP %d" % status if status else "transport error", code, msg))
        print("    (a permission problem looks like this: HTTP 4xx with a Binance code; the native harness would say HttpStatus, not MalformedResponse)")
        return 1
    print("[2] GET /api/v3/account -> HTTP 200")

    result = {"host": r6.HOST, "http": 200, "analysis": analyze(text)}
    verdict = result["analysis"]["native_parser_verdict"]
    print()
    print("== result (safe to paste: no key, no secret, no signature, no uid value, no amounts) ==")
    print(json.dumps(result, indent=2, sort_keys=True))
    print()
    print("VERDICT: the native parser answers %s%s" % (verdict["outcome"], " -- " + verdict["first_failure"] if "first_failure" in verdict else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
