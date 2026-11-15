#!/usr/bin/env python3
# SPDX-License-Identifier: proprietary
"""Sample order-list visibility on Binance Spot Testnet and print a redacted JSON report."""

import argparse
import http.client
import json
import re
import sys
import time

import r6_testnet_probe as r6

TIME_PATH = "/api/v3/time"
ORDER_PATH = "/api/v3/order"
ALL_ORDERS_PATH = "/api/v3/allOrders"
BACKOFF_STATUSES = {403, 418, 429}
PAGE_LIMIT = 1000
RESPONSE_CAP = 1 << 20
WINDOW_LIMIT_MS = 86_400_000
MAX_ORDER_ID = (1 << 63) - 1


def testnet_get(path_and_query, headers, timeout):
    """Send one bounded HTTPS GET to an allowlisted Testnet path."""
    if path_and_query.partition("?")[0] not in {TIME_PATH, ORDER_PATH, ALL_ORDERS_PATH}:
        raise ValueError("Unsupported sampling endpoint")

    connection = http.client.HTTPSConnection(r6.HOST, timeout=timeout)
    status = None
    try:
        connection.request("GET", path_and_query, headers=headers)
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


def read_json(probe, stage, path, params=None):
    http_status, body = probe.public(path) if params is None else probe.signed(path, params)
    if http_status is None:
        raise ReadFailure(stage, "transport_error")
    if http_status in BACKOFF_STATUSES:
        raise ReadFailure(stage, "backoff", http_status)
    try:
        document = r6.parse_json(body) if isinstance(body, str) else None
    except RecursionError:
        document = None
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


def classify_page(rows, symbol, order_id):
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
        ids.add(value)
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
    *, monotonic=time.monotonic, sleep=time.sleep,
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
    try:
        last_exchange_ms = read_exchange_time(probe, "initial_time")
        accepted_ms = read_order_time(probe, symbol, order_id)
    except ReadFailure as problem:
        report["status"] = "stopped" if problem.stopped else "setup_failed"
        report["problem"] = problem.details
        return report

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
            params = {
                "symbol": symbol,
                "startTime": window_start,
                "endTime": min(before, window_start + WINDOW_LIMIT_MS),
                "limit": PAGE_LIMIT,
            }
            rows = read_json(probe, "all_orders", ALL_ORDERS_PATH, params)

            if monotonic() >= deadline:
                raise ReadFailure("after_time", "observation_deadline")
            after = read_exchange_time(probe, "after_time")
            if after < before:
                raise ReadFailure("after_time", "invalid_time_bracket")
            observation["after_ms"] = after - accepted_ms
            last_exchange_ms = after
            observation["result"] = classify_page(rows, symbol, order_id)
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", required=True)
    parser.add_argument("--symbol", type=symbol_name, default="BTCUSDT")
    parser.add_argument("--order-id", required=True, type=bounded_integer(0, MAX_ORDER_ID))
    parser.add_argument("--duration-seconds", type=bounded_integer(1, 300), default=20)
    parser.add_argument("--poll-interval-ms", type=bounded_integer(500, 60_000), default=1000)
    parser.add_argument("--timeout-seconds", type=bounded_integer(1, 30), default=5)
    args = parser.parse_args(argv)

    try:
        values = r6.read_env_file(args.env_file)
    except UnicodeError:
        print("Could not read the credentials file as UTF-8.", file=sys.stderr)
        return 2
    key, secret = (values.get(name, "") for name in (r6.KEY_VAR, r6.SECRET_VAR))
    if not all(value and value.isascii() and value.isprintable() for value in (key, secret)):
        print("The credentials file must contain both read-only Spot Testnet API credentials.", file=sys.stderr)
        return 2

    probe = r6.Probe(key, secret, args.timeout_seconds, request_get=testnet_get, attempts=1)
    report = collect_visibility(probe, args.symbol, args.order_id, args.duration_seconds, args.poll_interval_ms)
    print(json.dumps(report, indent=2))
    return {"stopped": 3, "setup_failed": 4}.get(report["status"], 0)


if __name__ == "__main__":
    raise SystemExit(main())
