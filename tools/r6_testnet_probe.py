#!/usr/bin/env python3
"""R6 read-only probe of the Binance Spot Testnet (D5 slice R6 prerequisite, docs/OPERATOR_RESOLUTION_DESIGN.md section 9 and 10).

What it answers -- three facts the offline operator tool and the store binding assume and nobody has measured yet:
  1. Does GET /api/v3/account carry a `uid` on the Testnet?  The operator tool's account check (R3b-1) and the trading side's
     ensure_store_binding() (R5, wired in M5) both need it.
  2. What exactly does a query for an order that does not exist return?  The design assumes HTTP 400 with code -2013.
  3. Do the read endpoints the operator tool uses (order, allOrders, openOrders) answer for this key?

What it does NOT do: place, cancel or change anything. It sends GET requests only, to testnet.binance.vision only (there is no flag for any other
host). It never prints the API key, the secret or a signature, and it prints the uid only as "present, N digits". It writes no file.
The read lag L of allOrders (design section 5) cannot be measured without placing orders and is NOT part of this probe.

Credentials: HENGYUAN_OPERATOR_TESTNET_API_KEY / HENGYUAN_OPERATOR_TESTNET_API_SECRET -- the READ-ONLY operator key the offline tool uses, not the
trading key -- from the environment, or from the .env file named by --env-file (default ./.env), the same variable names as the tool.
The probe signs with HMAC-SHA256; a code -1022 ("Signature for this request is not valid") usually means the key is an Ed25519 key.

Run (PowerShell, Linux or WSL), from the repository root:
    python tools/r6_testnet_probe.py --env-file .env
The last block of the output is a small JSON object that is safe to paste into the chat. Python 3.8+, standard library only.
An AI session never runs this with real credentials (CLAUDE.md); the owner does.
"""
import argparse
import hashlib
import hmac
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HOST = "testnet.binance.vision"
BASE = "https://" + HOST
KEY_VAR = "HENGYUAN_OPERATOR_TESTNET_API_KEY"
SECRET_VAR = "HENGYUAN_OPERATOR_TESTNET_API_SECRET"
RECV_WINDOW_MS = 10_000


def read_env_file(path):
    """KEY=VALUE lines (an optional `export `, optional quotes, # comments); a missing file is an empty mapping."""
    values = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                if line.startswith("export "):
                    line = line[len("export "):].lstrip()
                name, _, value = line.partition("=")
                value = value.strip()
                if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                    value = value[1:-1]
                values[name.strip()] = value
    except OSError:
        pass
    return values


def credentials(env_file):
    file_values = read_env_file(env_file)
    key = os.environ.get(KEY_VAR) or file_values.get(KEY_VAR)
    secret = os.environ.get(SECRET_VAR) or file_values.get(SECRET_VAR)
    return key, secret


def http_get(path_and_query, headers, timeout):
    """Returns (status, body_text); (None, reason) for a transport failure. The URL (which carries the signature) is never printed."""
    request = urllib.request.Request(BASE + path_and_query, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:  # noqa: S310 -- the scheme and host are the constants above
            return response.status, response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as err:
        return err.code, err.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError, TimeoutError) as err:
        return None, type(err).__name__


def parse_json(text):
    try:
        return json.loads(text)
    except ValueError:
        return None


class Probe:
    def __init__(self, key, secret, timeout):
        self.key = key
        self.secret = secret
        self.timeout = timeout
        self.offset_ms = 0

    def now_ms(self):
        return int(time.time() * 1000) + self.offset_ms

    def public(self, path):
        return http_get(path, {}, self.timeout)

    def signed(self, path, params):
        params = dict(params)
        params["timestamp"] = self.now_ms()
        params["recvWindow"] = RECV_WINDOW_MS
        query = urllib.parse.urlencode(params)
        signature = hmac.new(self.secret.encode("ascii"), query.encode("ascii"), hashlib.sha256).hexdigest()
        return http_get(path + "?" + query + "&signature=" + signature, {"X-MBX-APIKEY": self.key}, self.timeout)


def error_fields(text):
    body = parse_json(text)
    if isinstance(body, dict):
        return body.get("code"), body.get("msg")
    return None, None


def describe_uid(account):
    if "uid" not in account:
        return {"present": False}
    uid = account["uid"]
    digits = len(str(uid)) if isinstance(uid, int) and not isinstance(uid, bool) else None
    return {"present": True, "type": type(uid).__name__, "digits": digits, "nonzero": bool(uid)}


def main():
    parser = argparse.ArgumentParser(description="Read-only probe of the Binance Spot Testnet (see the module docstring).")
    parser.add_argument("--env-file", default=".env", help="file with the two HENGYUAN_OPERATOR_TESTNET_* variables (default ./.env)")
    parser.add_argument("--symbol", default="BTCUSDT", help="symbol for the order and allOrders reads (default BTCUSDT)")
    parser.add_argument("--timeout", type=float, default=15.0, help="seconds per request (default 15)")
    args = parser.parse_args()

    key, secret = credentials(args.env_file)
    if not key or not secret:
        print("error: %s and %s are not set (environment, or %s)" % (KEY_VAR, SECRET_VAR, args.env_file), file=sys.stderr)
        return 2

    probe = Probe(key, secret, args.timeout)
    result = {"host": HOST, "symbol": args.symbol}
    print("== R6 Testnet probe (read-only, GET only) ==")
    print("host: %s" % HOST)

    # 1. the clock: signed requests are judged against the server's time
    t0 = int(time.time() * 1000)
    status, text = probe.public("/api/v3/time")
    t1 = int(time.time() * 1000)
    body = parse_json(text) if status == 200 else None
    if not isinstance(body, dict) or "serverTime" not in body:
        print("[1] GET /api/v3/time -> %s %s" % ("HTTP %d" % status if status else "transport error", text[:120] if status else text))
        return 1
    server_ms = int(body["serverTime"])
    probe.offset_ms = server_ms - (t0 + t1) // 2
    print("[1] GET /api/v3/time -> HTTP 200; clock offset (server - local) %+d ms; round trip %d ms" % (probe.offset_ms, t1 - t0))
    result["time"] = {"http": 200, "offset_ms": probe.offset_ms, "round_trip_ms": t1 - t0}

    # 2. the account: is there a uid?
    status, text = probe.signed("/api/v3/account", {})
    result["account"] = {"http": status}
    if status == 200 and isinstance(parse_json(text), dict):
        account = parse_json(text)
        uid = describe_uid(account)
        result["account"].update(uid=uid, accountType=account.get("accountType"), canTrade=account.get("canTrade"),
                                 permissions=account.get("permissions"))
        shown = "present, %s digits" % uid.get("digits") if uid["present"] else "ABSENT"
        print("[2] GET /api/v3/account -> HTTP 200; uid: %s; accountType=%s canTrade=%s permissions=%s"
              % (shown, account.get("accountType"), account.get("canTrade"), account.get("permissions")))
    else:
        code, msg = error_fields(text) if status else (None, text)
        result["account"].update(code=code, msg=msg)
        print("[2] GET /api/v3/account -> %s code=%s msg=%r" % ("HTTP %d" % status if status else "transport error", code, msg))
        if code == -1022:
            print("      (-1022: the signature was refused; this probe signs with HMAC-SHA256, an Ed25519 key will not work here)")

    # 3. an order that does not exist
    missing = "HYPROBE" + secrets.token_hex(8)
    status, text = probe.signed("/api/v3/order", {"symbol": args.symbol, "origClientOrderId": missing})
    code, msg = error_fields(text) if status else (None, text)
    result["order_not_found"] = {"http": status, "code": code, "msg": msg}
    print("[3] GET /api/v3/order (an order that does not exist) -> %s code=%s msg=%r"
          % ("HTTP %d" % status if status else "transport error", code, msg))

    # 4. allOrders over the last hour, one row
    end_ms = probe.now_ms()
    status, text = probe.signed("/api/v3/allOrders", {"symbol": args.symbol, "startTime": end_ms - 3_600_000, "endTime": end_ms, "limit": 1})
    rows = parse_json(text) if status == 200 else None
    result["all_orders"] = {"http": status, "rows": len(rows) if isinstance(rows, list) else None}
    if status == 200 and isinstance(rows, list):
        print("[4] GET /api/v3/allOrders (last hour, limit 1) -> HTTP 200; %d row(s)" % len(rows))
    else:
        code, msg = error_fields(text) if status else (None, text)
        result["all_orders"].update(code=code, msg=msg)
        print("[4] GET /api/v3/allOrders -> %s code=%s msg=%r" % ("HTTP %d" % status if status else "transport error", code, msg))

    # 5. openOrders (all symbols, as the operator tool asks)
    status, text = probe.signed("/api/v3/openOrders", {})
    rows = parse_json(text) if status == 200 else None
    result["open_orders"] = {"http": status, "rows": len(rows) if isinstance(rows, list) else None}
    if status == 200 and isinstance(rows, list):
        print("[5] GET /api/v3/openOrders -> HTTP 200; %d open order(s)" % len(rows))
    else:
        code, msg = error_fields(text) if status else (None, text)
        result["open_orders"].update(code=code, msg=msg)
        print("[5] GET /api/v3/openOrders -> %s code=%s msg=%r" % ("HTTP %d" % status if status else "transport error", code, msg))

    print()
    print("== result (safe to paste: no key, no secret, no signature, no uid value) ==")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
