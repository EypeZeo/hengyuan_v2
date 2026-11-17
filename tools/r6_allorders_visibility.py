#!/usr/bin/env python3
# SPDX-License-Identifier: proprietary
"""Sample order-list visibility on Binance Spot Testnet and print a redacted JSON report."""

import argparse
import http.client
import json
import re
import sys
import time
import uuid
from datetime import datetime, timedelta, timezone

import r6_testnet_probe as r6

TIME_PATH = "/api/v3/time"
ORDER_PATH = "/api/v3/order"
ALL_ORDERS_PATH = "/api/v3/allOrders"
ACCOUNT_PATH = "/api/v3/account"
TRADE_KEY_VAR = "HENGYUAN_R6_TESTNET_TRADE_API_KEY"
TRADE_SECRET_VAR = "HENGYUAN_R6_TESTNET_TRADE_API_SECRET"
BACKOFF_STATUSES = {403, 418, 429}
UNKNOWN_EXECUTION_CODES = {-1000, -1001, -1006, -1007}
RECOVERY_ATTEMPTS = 3
PAGE_LIMIT = 1000
RECENT_ORDER_LIMIT = 10
RESPONSE_CAP = 1 << 20
WINDOW_LIMIT_MS = 86_400_000
MAX_ORDER_ID = (1 << 63) - 1


def testnet_get(path_and_query, headers, timeout):
    """Send one bounded HTTPS GET to an allowlisted Testnet path."""
    if path_and_query.partition("?")[0] not in {TIME_PATH, ORDER_PATH, ALL_ORDERS_PATH, ACCOUNT_PATH}:
        raise ValueError("Unsupported sampling endpoint")
    return testnet_request("GET", path_and_query, headers, timeout)


def testnet_post_order(path_and_query, headers, timeout):
    path, separator, form = path_and_query.partition("?")
    if path != ORDER_PATH or not separator or not form:
        raise ValueError("Unsupported submission endpoint")
    return testnet_request(
        "POST", path, {**headers, "Content-Type": "application/x-www-form-urlencoded"},
        timeout, body=form,
    )


def testnet_request(method, path, headers, timeout, **request_options):
    connection = http.client.HTTPSConnection(r6.HOST, timeout=timeout)
    status = None
    try:
        connection.request(method, path, headers=headers, **request_options)
        with connection.getresponse() as response:
            status = response.status
            if status in BACKOFF_STATUSES:
                return status, None
            body = response.read(RESPONSE_CAP + 1)
            if len(body) > RESPONSE_CAP or response.length:
                return status, None
            return status, body.decode("utf-8")
    except (OSError, http.client.HTTPException, UnicodeError):
        return status, None
    finally:
        connection.close()


class ReadFailure(Exception):
    """A reportable error with no request URL or response content."""

    def __init__(self, stage, reason, http_status=None, exchange_code=None):
        super().__init__(reason)
        self.details = {"stage": stage, "reason": reason}
        if http_status is not None:
            self.details["http_status"] = http_status
        if exchange_code is not None:
            self.details["exchange_code"] = exchange_code
        self.stopped = http_status in BACKOFF_STATUSES


class SubmissionFailure(ReadFailure):
    def __init__(self, reason, http_status=None, exchange_code=None, *, client_order_id=None):
        super().__init__("submit_order", reason, http_status, exchange_code)
        self.client_order_id = client_order_id


def read_json(probe, stage, path, params=None):
    http_status, body = probe.public(path) if params is None else probe.signed(path, params)
    return response_json(stage, http_status, body)


def response_json(stage, http_status, body):
    if http_status is None:
        raise ReadFailure(stage, "transport_error")
    if http_status in BACKOFF_STATUSES:
        raise ReadFailure(stage, "backoff", http_status)
    document = r6.parse_json(body) if isinstance(body, str) else None
    if http_status != 200:
        code = document.get("code") if isinstance(document, dict) else None
        if type(code) is not int or not -99_999 <= code < 0:
            code = None
        raise ReadFailure(stage, "http_error", http_status, code)
    return document


def read_exchange_time(probe, stage):
    document = read_json(probe, stage, TIME_PATH)
    current_ms = document.get("serverTime") if isinstance(document, dict) else None
    if type(current_ms) is not int or not 0 < current_ms <= MAX_ORDER_ID:
        raise ReadFailure(stage, "invalid_exchange_time", 200)
    # Use the exchange time sampled before the response arrived as a conservative signing anchor.
    probe.offset_ms = current_ms - time.time_ns() // 1_000_000
    return current_ms


def read_order_time(probe, symbol, order_id):
    params = {"symbol": symbol, "orderId": order_id}
    order = read_json(probe, "order", ORDER_PATH, params)
    if (
        not isinstance(order, dict)
        or order.get("symbol") != symbol
        or type(order.get("orderId")) is not int
        or order["orderId"] != order_id
    ):
        raise ReadFailure("order", "order_mismatch", 200)
    accepted_ms = order.get("time")
    if type(accepted_ms) is not int or not 0 < accepted_ms <= MAX_ORDER_ID:
        raise ReadFailure("order", "invalid_order_time", 200)
    return accepted_ms


def account_uid(probe, stage):
    read_exchange_time(probe, stage + "_time")
    account = read_json(probe, stage, ACCOUNT_PATH, {})
    uid = account.get("uid") if isinstance(account, dict) else None
    if type(uid) is not int or uid <= 0:
        raise ReadFailure(stage, "account_uid_unavailable", 200)
    return uid


def accepted_order(document, symbol, client_order_id, time_field, stage):
    if (
        not isinstance(document, dict)
        or document.get("symbol") != symbol
        or document.get("clientOrderId") != client_order_id
    ):
        raise ReadFailure(stage, "order_mismatch", 200)
    order_id = document.get("orderId")
    accepted_ms = document.get(time_field)
    if type(order_id) is not int or not 0 <= order_id <= MAX_ORDER_ID:
        raise ReadFailure(stage, "invalid_order_id", 200)
    if type(accepted_ms) is not int or not 0 < accepted_ms <= MAX_ORDER_ID:
        raise ReadFailure(stage, "invalid_order_time", 200)
    return order_id, accepted_ms


def submit_testnet_order(read_probe, trade_probe, symbol, side, price, quantity, *, sleep=time.sleep):
    if read_probe.key == trade_probe.key:
        raise ReadFailure("account", "distinct_api_keys_required")
    if account_uid(read_probe, "read_account") != account_uid(trade_probe, "trade_account"):
        raise ReadFailure("account", "account_uid_mismatch")

    client_order_id = "r6-" + uuid.uuid4().hex
    params = {
        "symbol": symbol,
        "side": side,
        "type": "LIMIT_MAKER",
        "price": price,
        "quantity": quantity,
        "newClientOrderId": client_order_id,
        "newOrderRespType": "ACK",
    }
    status, body = trade_probe.signed_once(ORDER_PATH, params, transport=testnet_post_order)
    try:
        ack = response_json("submit_order", status, body)
        return accepted_order(ack, symbol, client_order_id, "transactTime", "submit_order")
    except ReadFailure as problem:
        if problem.stopped:
            raise SubmissionFailure("backoff", status) from None
        exchange_code = problem.details.get("exchange_code")
        if (
            status is not None
            and 400 <= status < 500
            and status != 408
            and exchange_code is not None
            and exchange_code not in UNKNOWN_EXECUTION_CODES
        ):
            raise SubmissionFailure("order_rejected", status, exchange_code) from None
        submission_problem = problem.details

    # Query the same client order ID. A missing order cannot establish that the POST failed.
    query = {"symbol": symbol, "origClientOrderId": client_order_id}
    for attempt in range(RECOVERY_ATTEMPTS):
        if attempt:
            sleep(0.5)
        try:
            read_exchange_time(read_probe, "recover_time")
            order = read_json(read_probe, "recover_order", ORDER_PATH, query)
            return accepted_order(order, symbol, client_order_id, "time", "recover_order")
        except ReadFailure as problem:
            recovery_problem = problem.details
            if problem.stopped:
                break

    failure = SubmissionFailure(
        "order_status_unknown", recovery_problem.get("http_status"),
        recovery_problem.get("exchange_code"), client_order_id=client_order_id,
    )
    failure.details["submission"] = submission_problem
    failure.details["recovery"] = recovery_problem
    raise failure


def page_order_ids(rows, symbol, *, time_window=None):
    if not isinstance(rows, list):
        raise ReadFailure("all_orders", "invalid_page", 200)
    if len(rows) >= PAGE_LIMIT:
        raise ReadFailure("all_orders", "saturated_page", 200)

    ids = set()
    for row in rows:
        if not isinstance(row, dict) or row.get("symbol") != symbol:
            raise ReadFailure("all_orders", "invalid_page", 200)
        value = row.get("orderId")
        if type(value) is not int or not 0 <= value <= MAX_ORDER_ID or value in ids:
            raise ReadFailure("all_orders", "invalid_page", 200)
        placed_ms = row.get("time")
        if type(placed_ms) is not int or not 0 <= placed_ms <= MAX_ORDER_ID:
            raise ReadFailure("all_orders", "invalid_order_time", 200)
        if time_window is not None and not time_window[0] <= placed_ms <= time_window[1]:
            raise ReadFailure("all_orders", "order_time_outside_window", 200)
        ids.add(value)
    return ids


def classify_page(rows, symbol, order_id, *, time_window=None):
    ids = page_order_ids(rows, symbol, time_window=time_window)
    return "found" if order_id in ids else "missing"


def summarize(report):
    last_missing_before = None
    first_found_after = None
    nonmonotonic = False
    for observation in report["observations"]:
        result = observation["result"]
        report["counts"][result] += 1
        if result == "found" and first_found_after is None:
            first_found_after = observation["after_ms"]
        elif result == "missing":
            if first_found_after is None:
                last_missing_before = observation["before_ms"]
            else:
                nonmonotonic = True

    if report["status"] == "stopped":
        return report
    if nonmonotonic:
        report["status"] = "nonmonotonic"
    elif first_found_after is not None:
        report["status"] = "bounded"
        report["interval_ms"] = {"lower": last_missing_before or 0, "upper": first_found_after}
    elif last_missing_before is not None:
        report["status"] = "right_censored"
        report["interval_ms"] = {"lower": last_missing_before, "upper": None}
    return report


def collect_visibility(
    probe, symbol, order_id, duration_seconds=20, poll_interval_ms=1000,
    *, accepted_ms=None, monotonic=time.monotonic, sleep=time.sleep,
):
    report = {
        "environment": "SPOT_TESTNET",
        "status": "inconclusive",
        "duration_seconds": duration_seconds,
        "poll_interval_ms": poll_interval_ms,
        "interval_ms": None,
        "counts": {"found": 0, "missing": 0, "inconclusive": 0},
        "observations": [],
    }
    if accepted_ms is None:
        try:
            last_exchange_ms = read_exchange_time(probe, "initial_time")
            accepted_ms = read_order_time(probe, symbol, order_id)
        except ReadFailure as problem:
            report["status"] = "stopped" if problem.stopped else "setup_failed"
            report["problem"] = problem.details
            return report
    else:
        last_exchange_ms = accepted_ms

    deadline = monotonic() + duration_seconds
    last_before_ms = None
    window_start = max(0, accepted_ms - 1000)
    while monotonic() < deadline:
        poll_start = monotonic()
        observation = {"result": "inconclusive", "before_ms": None, "after_ms": None, "gap_ms": None}
        try:
            before = read_exchange_time(probe, "before_time")
            if before < max(last_exchange_ms, accepted_ms):
                raise ReadFailure("before_time", "invalid_time_bracket")
            observation["before_ms"] = before - accepted_ms
            if last_before_ms is not None:
                observation["gap_ms"] = before - last_before_ms
            last_before_ms = before
            last_exchange_ms = before

            if monotonic() >= deadline:
                raise ReadFailure("all_orders", "observation_deadline")
            window_end = min(before, window_start + WINDOW_LIMIT_MS)
            params = {
                "symbol": symbol,
                "startTime": window_start,
                "endTime": window_end,
                "limit": PAGE_LIMIT,
            }
            rows = read_json(probe, "all_orders", ALL_ORDERS_PATH, params)

            if monotonic() >= deadline:
                raise ReadFailure("after_time", "observation_deadline")
            after = read_exchange_time(probe, "after_time")
            if monotonic() >= deadline:
                raise ReadFailure("after_time", "observation_deadline")
            if after < before:
                raise ReadFailure("after_time", "invalid_time_bracket")
            observation["after_ms"] = after - accepted_ms
            last_exchange_ms = after
            observation["result"] = classify_page(rows, symbol, order_id, time_window=(window_start, window_end))
        except ReadFailure as problem:
            observation["problem"] = problem.details
            if problem.stopped:
                report["status"] = "stopped"
                report["problem"] = problem.details

        report["observations"].append(observation)
        if report["status"] == "stopped":
            break
        next_poll = poll_start + poll_interval_ms / 1000
        delay = min(next_poll, deadline) - monotonic()
        if delay > 0:
            sleep(delay)

    return summarize(report)


def bounded_integer(minimum, maximum):
    def parse(value):
        if not re.fullmatch(r"[0-9]+", value) or len(value) > 19:
            raise argparse.ArgumentTypeError("expected a decimal integer")
        number = int(value)
        if not minimum <= number <= maximum:
            raise argparse.ArgumentTypeError(f"expected a value from {minimum} to {maximum}")
        return number

    return parse


def symbol_name(value):
    if not re.fullmatch(r"[A-Z0-9]{1,19}", value):
        raise argparse.ArgumentTypeError("expected an uppercase Spot symbol")
    return value


def positive_decimal(value):
    if not re.fullmatch(r"[0-9]{1,18}(?:\.[0-9]{1,18})?", value) or not any(c in "123456789" for c in value):
        raise argparse.ArgumentTypeError("expected a positive decimal without exponent notation")
    return value


def recent_orders(probe, symbol):
    read_exchange_time(probe, "initial_time")
    rows = read_json(probe, "all_orders", ALL_ORDERS_PATH, {"symbol": symbol, "limit": RECENT_ORDER_LIMIT})
    page_order_ids(rows, symbol)
    return sorted(rows, key=lambda row: row["time"], reverse=True)[:RECENT_ORDER_LIMIT]


def print_order_list(rows, stream):
    print(f"{'#':>2}  {'orderId':>19}  {'Symbol':<19}  {'Status':<18}  Placed at (SGT)", file=stream)
    sgt = timezone(timedelta(hours=8))
    for index, row in enumerate(rows, start=1):
        status = row.get("status")
        if not isinstance(status, str) or not re.fullmatch(r"[A-Z_]{1,18}", status):
            status = "-"
        try:
            placed = datetime.fromtimestamp(row["time"] / 1000, sgt).strftime("%Y-%m-%d %H:%M:%S")
        except (OverflowError, OSError, ValueError):
            placed = "-"
        print(f"{index:>2}  {row['orderId']:>19}  {row['symbol']:<19}  {status:<18}  {placed}", file=stream)


def select_order(rows):
    print_order_list(rows, sys.stderr)
    print("Select an order by row number to check continued visibility, or press Enter to cancel: ",
          end="", file=sys.stderr, flush=True)
    try:
        choice = input().strip()
    except (EOFError, KeyboardInterrupt):
        return None
    if not choice:
        return None
    if not re.fullmatch(r"[0-9]{1,2}", choice) or not 1 <= int(choice) <= len(rows):
        print("Choose a row number from the displayed list.", file=sys.stderr)
        return None
    return rows[int(choice) - 1]["orderId"]


def credentials_probe(path, key_name, secret_name, timeout):
    values = r6.read_env_file(path)
    key, secret = (values.get(name, "") for name in (key_name, secret_name))
    if not all(value and value.isascii() and value.isprintable() for value in (key, secret)):
        raise ValueError("Missing or invalid credentials")
    return r6.Probe(key, secret, timeout, request_get=testnet_get, attempts=1)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", required=True)
    parser.add_argument("--symbol", type=symbol_name)
    order_source = parser.add_mutually_exclusive_group()
    order_source.add_argument("--order-id", type=bounded_integer(0, MAX_ORDER_ID))
    order_source.add_argument("--list-only", action="store_true")
    order_source.add_argument("--place-testnet-order", action="store_true")
    parser.add_argument("--trade-env-file")
    parser.add_argument("--side", choices=("BUY", "SELL"))
    parser.add_argument("--price", type=positive_decimal)
    parser.add_argument("--quantity", type=positive_decimal)
    parser.add_argument("--duration-seconds", type=bounded_integer(1, 300), default=20)
    parser.add_argument("--poll-interval-ms", type=bounded_integer(500, 60_000), default=1000)
    parser.add_argument("--timeout-seconds", type=bounded_integer(1, 30), default=5)
    args = parser.parse_args(argv)
    placement_args = (args.trade_env_file, args.side, args.price, args.quantity)
    if args.place_testnet_order:
        if not args.symbol or not all(placement_args):
            parser.error("--place-testnet-order requires --symbol, --trade-env-file, --side, --price and --quantity")
    elif any(arg is not None for arg in placement_args):
        parser.error("--trade-env-file, --side, --price and --quantity require --place-testnet-order")
    symbol = args.symbol or "BTCUSDT"
    try:
        probe = credentials_probe(args.env_file, r6.KEY_VAR, r6.SECRET_VAR, args.timeout_seconds)
    except ValueError:
        print("The read-only credentials file must contain both Spot Testnet API credentials in UTF-8.", file=sys.stderr)
        return 2

    order_id = args.order_id
    accepted_ms = None
    if args.place_testnet_order:
        try:
            trade_probe = credentials_probe(args.trade_env_file, TRADE_KEY_VAR, TRADE_SECRET_VAR, args.timeout_seconds)
        except ValueError:
            print("The TRADE credentials file must contain both Spot Testnet API credentials in UTF-8.", file=sys.stderr)
            return 2
        try:
            order_id, accepted_ms = submit_testnet_order(
                probe, trade_probe, symbol, args.side, args.price, args.quantity,
            )
        except ReadFailure as problem:
            if isinstance(problem, SubmissionFailure):
                status, exit_code = problem.details["reason"], 5
                if problem.client_order_id is not None:
                    print(
                        f"Testnet order status is unknown for {symbol}, clientOrderId {problem.client_order_id}. "
                        "Check this order in Testnet before starting another placement.",
                        file=sys.stderr,
                    )
            else:
                status, exit_code = "setup_failed", 4
            if problem.stopped:
                status, exit_code = "stopped", 3
            print(json.dumps({"environment": "SPOT_TESTNET", "status": status, "problem": problem.details}, indent=2))
            return exit_code
        print(f"Spot Testnet accepted {symbol} orderId {order_id}.", file=sys.stderr)
    elif order_id is None:
        try:
            rows = recent_orders(probe, symbol)
        except ReadFailure as problem:
            status = "stopped" if problem.stopped else "setup_failed"
            print(json.dumps({"environment": "SPOT_TESTNET", "status": status, "problem": problem.details}, indent=2))
            return 3 if problem.stopped else 4
        if not rows:
            print(f"No visible Testnet orders for {symbol}.", file=sys.stderr)
            return 0 if args.list_only else 2
        if args.list_only:
            print_order_list(rows, sys.stdout)
            return 0
        order_id = select_order(rows)
        if order_id is None:
            return 2

    if args.place_testnet_order:
        report = collect_visibility(
            probe, symbol, order_id, args.duration_seconds, args.poll_interval_ms, accepted_ms=accepted_ms,
        )
        report["selection"] = "new_order"
    else:
        report = collect_visibility(probe, symbol, order_id, args.duration_seconds, args.poll_interval_ms)
    if not args.place_testnet_order and args.order_id is None:
        report["selection"] = "recent_orders"
        report["interval_ms"] = None
        # Selecting an order establishes that allOrders has already shown it once.
        if report["status"] != "stopped" and report["counts"]["missing"]:
            report["status"] = "nonmonotonic"
        elif report["status"] == "bounded":
            report["status"] = "already_visible"
    print(json.dumps(report, indent=2))
    return {"stopped": 3, "setup_failed": 4}.get(report["status"], 0)


if __name__ == "__main__":
    raise SystemExit(main())
