"""Offline tests for the Testnet allOrders visibility sampler."""

from __future__ import annotations

import contextlib
import http.client
import io
import json
import sys
import tempfile
import unittest
from collections import deque
from pathlib import Path
from unittest import mock

TOOLS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_DIR))

import r6_allorders_visibility as visibility  # noqa: E402


SYMBOL = "BTCUSDT"
ORDER_ID = 1_314_159_265_358_979
RAW_CLIENT_ORDER_ID = "private-client-order-id-marker"
ACCEPTED_MS = 1_700_000_000_000


def _text(body: object) -> str:
    return body if isinstance(body, str) else json.dumps(body)


class FakeProbe:
    """Implements only the probe interface consumed by collect_visibility; no signer or network exists here."""

    def __init__(
        self,
        time_values: list[int] | None = None,
        pages: list[tuple[int, object]] | None = None,
        *,
        order_status: int = 200,
        order_body: object | None = None,
    ) -> None:
        self.offset_ms = 0
        self.calls: list[dict[str, object]] = []
        self._time_values = deque(time_values or [ACCEPTED_MS])
        self._last_time = self._time_values[-1]
        self._pages = deque(pages or [(200, [])])
        self._last_page = (200, [])
        self._order_status = order_status
        self._order_body = order_body if order_body is not None else {
            "symbol": SYMBOL,
            "orderId": ORDER_ID,
            "time": ACCEPTED_MS,
            "clientOrderId": RAW_CLIENT_ORDER_ID,
        }

    def public(self, path: str) -> tuple[int, str]:
        self.calls.append({"kind": "public", "path": path})
        if path != visibility.TIME_PATH:
            raise AssertionError(f"unexpected public path: {path}")
        if self._time_values:
            self._last_time = self._time_values.popleft()
        else:
            self._last_time += 1_000
        return 200, json.dumps({"serverTime": self._last_time})

    def signed(self, path: str, params: dict[str, object]) -> tuple[int, str | None]:
        self.calls.append({"kind": "signed", "path": path, "params": dict(params)})
        if path == visibility.ORDER_PATH:
            return self._order_status, _text(self._order_body)
        if path == visibility.ALL_ORDERS_PATH:
            if self._pages:
                self._last_page = self._pages.popleft()
            status, body = self._last_page
            return status, _text(body)
        raise AssertionError(f"unexpected signed path: {path}")


class FakeClock:
    def __init__(self) -> None:
        self.value = 0.0

    def monotonic(self) -> float:
        return self.value

    def sleep(self, seconds: float) -> None:
        self.value += seconds


def _missing_page() -> tuple[int, object]:
    return 200, []


def _found_page() -> tuple[int, object]:
    return 200, [{"symbol": SYMBOL, "orderId": ORDER_ID, "time": ACCEPTED_MS}]


def _probe_for(
    pages: list[tuple[int, object]],
    *,
    brackets: list[tuple[int, int]] | None = None,
    order_status: int = 200,
    order_body: object | None = None,
) -> FakeProbe:
    if brackets is None:
        brackets = [(200 + 1_000 * index, 300 + 1_000 * index) for index in range(max(len(pages), 5))]
    time_values = [ACCEPTED_MS]
    for before_offset, after_offset in brackets:
        time_values.extend((ACCEPTED_MS + before_offset, ACCEPTED_MS + after_offset))
    return FakeProbe(time_values, pages, order_status=order_status, order_body=order_body)


def _run_collection(
    probe: FakeProbe,
    *,
    duration_seconds: float = 3.5,
) -> dict[str, object]:
    clock = FakeClock()
    return visibility.collect_visibility(
        probe,
        SYMBOL,
        ORDER_ID,
        duration_seconds=duration_seconds,
        poll_interval_ms=1000,
        monotonic=clock.monotonic,
        sleep=clock.sleep,
    )


class CollectVisibilityTests(unittest.TestCase):
    def test_interval_uses_last_valid_miss_before_first_hit(self) -> None:
        probe = _probe_for([_missing_page(), (200, {"unexpected": "object"}), _missing_page(), _found_page()])

        report = _run_collection(probe)

        self.assertEqual(report["status"], "bounded")
        observations = report["observations"]
        self.assertEqual([item["result"] for item in observations], ["missing", "inconclusive", "missing", "found"])
        self.assertEqual(
            report["interval_ms"],
            {"lower": observations[2]["before_ms"], "upper": observations[3]["after_ms"]},
        )
        self.assertEqual(observations[1]["gap_ms"], observations[1]["before_ms"] - observations[0]["before_ms"])

        all_orders_calls = [call for call in probe.calls if call.get("path") == visibility.ALL_ORDERS_PATH]
        self.assertTrue(all_orders_calls)
        for call in all_orders_calls:
            params = call["params"]
            self.assertEqual(params["symbol"], SYMBOL)
            self.assertEqual(params["limit"], visibility.PAGE_LIMIT)
            self.assertLessEqual(params["startTime"], ACCEPTED_MS)
            self.assertGreaterEqual(params["endTime"], ACCEPTED_MS)

        order_call = next(call for call in probe.calls if call.get("path") == visibility.ORDER_PATH)
        self.assertEqual(order_call["params"], {"symbol": SYMBOL, "orderId": ORDER_ID})

    def test_first_sample_found_has_zero_lower_bound(self) -> None:
        probe = _probe_for([_found_page(), _found_page(), _found_page(), _found_page()])

        report = _run_collection(probe)

        self.assertEqual(report["status"], "bounded")
        first = report["observations"][0]
        self.assertEqual(report["interval_ms"], {"lower": 0, "upper": first["after_ms"]})

    def test_supplied_acceptance_time_skips_initial_order_lookup_and_keeps_relative_times(self) -> None:
        probe = FakeProbe([ACCEPTED_MS, ACCEPTED_MS + 200], [_found_page()])
        clock = FakeClock()

        report = visibility.collect_visibility(
            probe,
            SYMBOL,
            ORDER_ID,
            duration_seconds=0.5,
            poll_interval_ms=1000,
            accepted_ms=ACCEPTED_MS,
            monotonic=clock.monotonic,
            sleep=clock.sleep,
        )

        self.assertEqual(
            [call["path"] for call in probe.calls],
            [visibility.TIME_PATH, visibility.ALL_ORDERS_PATH, visibility.TIME_PATH],
        )
        self.assertEqual(report["status"], "bounded")
        self.assertEqual(report["observations"][0]["result"], "found")
        self.assertEqual(report["observations"][0]["before_ms"], 0)
        self.assertEqual(report["observations"][0]["after_ms"], 200)
        self.assertEqual(report["interval_ms"], {"lower": 0, "upper": 200})

    def test_misses_through_deadline_are_right_censored(self) -> None:
        probe = _probe_for([_missing_page()] * 5)

        report = _run_collection(probe)

        self.assertEqual(report["status"], "right_censored")
        self.assertEqual(report["interval_ms"], {"lower": report["observations"][-1]["before_ms"], "upper": None})

    def test_a_later_miss_after_visibility_is_nonmonotonic(self) -> None:
        probe = _probe_for([_found_page(), _missing_page(), _missing_page(), _found_page()])

        report = _run_collection(probe)

        self.assertEqual(report["status"], "nonmonotonic")
        self.assertIsNone(report["interval_ms"])
        self.assertEqual(report["observations"][0]["result"], "found")
        self.assertEqual(report["observations"][1]["result"], "missing")

    def test_full_or_malformed_pages_are_inconclusive_not_missing(self) -> None:
        saturated = (200, [{"symbol": SYMBOL, "orderId": index} for index in range(visibility.PAGE_LIMIT)])
        cases = {
            "full page": (saturated, "saturated_page"),
            "object instead of array": ((200, {"orders": []}), "invalid_page"),
            "invalid JSON": ((200, "not-json"), "invalid_page"),
            "invalid row": ((200, [{"symbol": SYMBOL}]), "invalid_page"),
            "missing order time": (
                (200, [{"symbol": SYMBOL, "orderId": 42}]),
                "invalid_order_time",
            ),
            "null order time": (
                (200, [{"symbol": SYMBOL, "orderId": 42, "time": None}]),
                "invalid_order_time",
            ),
            "string order time": (
                (200, [{"symbol": SYMBOL, "orderId": 42, "time": str(ACCEPTED_MS)}]),
                "invalid_order_time",
            ),
            "boolean order time": (
                (200, [{"symbol": SYMBOL, "orderId": 42, "time": True}]),
                "invalid_order_time",
            ),
            "negative order time": (
                (200, [{"symbol": SYMBOL, "orderId": 42, "time": -1}]),
                "invalid_order_time",
            ),
            "order time above signed maximum": (
                (200, [{"symbol": SYMBOL, "orderId": 42, "time": visibility.MAX_ORDER_ID + 1}]),
                "invalid_order_time",
            ),
        }
        for name, (page, expected_reason) in cases.items():
            with self.subTest(name=name):
                probe = _probe_for([page] * 5)

                report = _run_collection(probe)

                self.assertEqual(report["status"], "inconclusive")
                self.assertIsNone(report["interval_ms"])
                self.assertGreater(report["counts"]["inconclusive"], 0)
                self.assertEqual(report["counts"]["missing"], 0)
                self.assertEqual(report["observations"][0]["problem"]["reason"], expected_reason)

    def test_time_that_moves_backward_is_not_a_valid_absence(self) -> None:
        probe = _probe_for(
            [_missing_page(), _missing_page()],
            brackets=[(500, 700), (600, 800), (900, 1000)],
        )

        report = _run_collection(probe, duration_seconds=1.5)

        self.assertEqual(report["observations"][0]["result"], "missing")
        self.assertEqual(report["observations"][1]["result"], "inconclusive")
        self.assertEqual(report["counts"]["missing"], 1)
        self.assertNotEqual(report["observations"][1]["result"], "missing")

    def test_backoff_status_stops_before_following_time_read_or_poll(self) -> None:
        for status in (403, 418, 429):
            with self.subTest(status=status):
                probe = _probe_for([(status, {"code": -1003, "msg": "private response detail"})])

                report = _run_collection(probe)

                self.assertEqual(report["status"], "stopped")
                self.assertEqual(report["observations"][0]["result"], "inconclusive")
                self.assertEqual(
                    [call["path"] for call in probe.calls],
                    [visibility.TIME_PATH, visibility.ORDER_PATH, visibility.TIME_PATH, visibility.ALL_ORDERS_PATH],
                )
                self.assertNotIn("private response detail", json.dumps(report))

    def test_order_response_must_match_requested_symbol_and_id(self) -> None:
        invalid_orders = (
            {"symbol": "ETHUSDT", "orderId": ORDER_ID, "time": ACCEPTED_MS},
            {"symbol": SYMBOL, "orderId": ORDER_ID + 1, "time": ACCEPTED_MS},
            {"symbol": SYMBOL, "orderId": ORDER_ID, "time": 0},
        )
        for order in invalid_orders:
            with self.subTest(order=order):
                probe = _probe_for([_missing_page()], order_body=order)

                report = _run_collection(probe)

                self.assertEqual(report["status"], "setup_failed")
                self.assertEqual([call["path"] for call in probe.calls], [visibility.TIME_PATH, visibility.ORDER_PATH])

    def test_report_omits_uid_order_identifiers_and_request_details(self) -> None:
        probe = _probe_for([_missing_page(), _found_page(), _found_page(), _found_page()])

        report = _run_collection(probe)
        serialized = json.dumps(report, sort_keys=True)

        self.assertNotIn(str(ORDER_ID), serialized)
        self.assertNotIn(RAW_CLIENT_ORDER_ID, serialized)
        self.assertNotIn("uid", {key.casefold() for key in _all_dict_keys(report)})
        forbidden_keys = {"orderid", "clientorderid", "request", "params", "url", "signature", "body"}
        self.assertTrue(forbidden_keys.isdisjoint({key.casefold() for key in _all_dict_keys(report)}))
        self.assertNotIn("/api/v3/", serialized)


def _all_dict_keys(value: object):
    if isinstance(value, dict):
        for key, child in value.items():
            yield str(key)
            yield from _all_dict_keys(child)
    elif isinstance(value, list):
        for child in value:
            yield from _all_dict_keys(child)


class _FakeHTTPResponse:
    def __init__(self, status: int, body: bytes) -> None:
        self.status = status
        self._body = body
        self.length: int | None = len(body)
        self.read_calls = 0
        self.closed = False

    def read(self, amount: int = -1) -> bytes:
        self.read_calls += 1
        result = self._body if amount < 0 else self._body[:amount]
        if self.length is not None:
            self.length = max(0, self.length - len(result))
        return result

    def __enter__(self) -> _FakeHTTPResponse:
        return self

    def __exit__(self, *_args: object) -> bool:
        self.closed = True
        return False


class _FakeHTTPConnection:
    def __init__(self, response: _FakeHTTPResponse) -> None:
        self.response = response
        self.requests: list[tuple[str, str, dict[str, str]]] = []
        self.closed = False

    def request(self, method: str, target: str, headers: dict[str, str]) -> None:
        self.requests.append((method, target, dict(headers)))

    def getresponse(self) -> _FakeHTTPResponse:
        return self.response

    def close(self) -> None:
        self.closed = True


class TestnetTransportTests(unittest.TestCase):
    def test_transport_uses_fixed_testnet_host_and_does_not_follow_redirects(self) -> None:
        response = _FakeHTTPResponse(302, b"redirect target hidden")
        connection = _FakeHTTPConnection(response)
        created: list[tuple[str, float, _FakeHTTPConnection]] = []

        def make_connection(host: str, *, timeout: float) -> _FakeHTTPConnection:
            created.append((host, timeout, connection))
            return connection

        with mock.patch.object(visibility.http.client, "HTTPSConnection", side_effect=make_connection):
            result = visibility.testnet_get("/api/v3/time", {"Accept": "application/json"}, 4.0)

        self.assertEqual(result, (302, "redirect target hidden"))
        self.assertEqual(len(created), 1)
        self.assertEqual(created[0][0], "testnet.binance.vision")
        self.assertEqual(created[0][1], 4.0)
        self.assertEqual(connection.requests, [("GET", "/api/v3/time", {"Accept": "application/json"})])
        self.assertEqual(response.read_calls, 1)
        self.assertTrue(connection.closed)
        self.assertTrue(response.closed)

    def test_backoff_response_body_is_never_read(self) -> None:
        response = _FakeHTTPResponse(429, b"sensitive response body")
        connection = _FakeHTTPConnection(response)

        with mock.patch.object(visibility.http.client, "HTTPSConnection", return_value=connection) as constructor:
            result = visibility.testnet_get("/api/v3/allOrders", {}, 2.0)

        self.assertEqual(result, (429, None))
        constructor.assert_called_once_with("testnet.binance.vision", timeout=2.0)
        self.assertEqual(len(connection.requests), 1)
        self.assertEqual(response.read_calls, 0)
        self.assertTrue(connection.closed)


class VisibilityCliTests(unittest.TestCase):
    def _credentials_file(self, directory: str) -> Path:
        path = Path(directory) / "fake credentials with spaces.env"
        path.touch()
        return path

    def _call_main_with_fake_probe(self, probe: FakeProbe) -> tuple[int, dict[str, object], mock.Mock]:
        with tempfile.TemporaryDirectory(prefix="r6-visibility-cli-") as directory:
            credentials_path = self._credentials_file(directory)
            factory_calls: list[tuple[tuple[object, ...], dict[str, object]]] = []

            def fake_factory(*args: object, **kwargs: object) -> FakeProbe:
                factory_calls.append((args, kwargs))
                return probe

            fake_environment = {
                visibility.r6.KEY_VAR: "TEST_KEY_ONLY",
                visibility.r6.SECRET_VAR: "TEST_SECRET_ONLY",
            }
            stdout = io.StringIO()
            with mock.patch.object(visibility.r6, "read_env_file", return_value=fake_environment) as read_env_file:
                with mock.patch.object(visibility.r6, "Probe", side_effect=fake_factory) as probe_factory:
                    with contextlib.redirect_stdout(stdout):
                        exit_code = visibility.main(
                            ["--env-file", str(credentials_path), "--order-id", str(ORDER_ID), "--duration-seconds", "1"]
                        )
            read_env_file.assert_called_once_with(str(credentials_path))
            self.assertEqual(len(factory_calls), 1)
            args, kwargs = factory_calls[0]
            self.assertEqual(args, ("TEST_KEY_ONLY", "TEST_SECRET_ONLY", 5))
            self.assertIs(kwargs["request_get"], visibility.testnet_get)
            self.assertEqual(kwargs["attempts"], 1)
            return exit_code, json.loads(stdout.getvalue()), probe_factory

    def _run_cli_with_fakes(
        self,
        probe: FakeProbe,
        arguments: list[str],
        *,
        input_value: str = "",
        collection_report: dict[str, object] | None = None,
    ) -> tuple[int, str, str, mock.Mock, mock.Mock, mock.Mock]:
        with tempfile.TemporaryDirectory(prefix="r6-visibility-cli-") as directory:
            credentials_path = self._credentials_file(directory)
            stdout = io.StringIO()
            stderr = io.StringIO()
            fake_environment = {
                visibility.r6.KEY_VAR: "TEST_KEY_ONLY",
                visibility.r6.SECRET_VAR: "TEST_SECRET_ONLY",
            }
            with mock.patch.object(visibility.r6, "read_env_file", return_value=fake_environment) as read_env_file:
                with mock.patch.object(visibility.r6, "Probe", return_value=probe) as probe_factory:
                    with mock.patch.object(visibility, "collect_visibility", return_value=collection_report) as collect:
                        with mock.patch("builtins.input", return_value=input_value) as user_input:
                            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                                exit_code = visibility.main(["--env-file", str(credentials_path), *arguments])
            read_env_file.assert_called_once_with(str(credentials_path))
            return exit_code, stdout.getvalue(), stderr.getvalue(), collect, user_input, probe_factory

    def test_fake_probe_stopped_status_is_reported_as_exit_3_json(self) -> None:
        probe = _probe_for([(429, {"code": -1003, "msg": "hidden body"})])

        exit_code, report, _ = self._call_main_with_fake_probe(probe)

        self.assertEqual(exit_code, 3)
        self.assertEqual(report["status"], "stopped")
        self.assertNotIn("hidden body", json.dumps(report))
        self.assertNotIn(str(ORDER_ID), json.dumps(report))

    def test_fake_probe_setup_failure_is_reported_as_exit_4_json(self) -> None:
        invalid_order = {"symbol": "ETHUSDT", "orderId": ORDER_ID, "time": ACCEPTED_MS}
        probe = _probe_for([_missing_page()], order_body=invalid_order)

        exit_code, report, _ = self._call_main_with_fake_probe(probe)

        self.assertEqual(exit_code, 4)
        self.assertEqual(report["status"], "setup_failed")
        self.assertEqual(report["problem"]["stage"], "order")
        self.assertNotIn("/api/v3/", json.dumps(report))

    def test_archive_and_missing_order_errors_keep_code_without_leaking_response_details(self) -> None:
        private_message = "private order response message marker"
        for exchange_code in (-2026, -2013):
            with self.subTest(exchange_code=exchange_code):
                probe = _probe_for(
                    [],
                    order_status=400,
                    order_body={"code": exchange_code, "msg": private_message, "orderId": ORDER_ID},
                )

                exit_code, report, _ = self._call_main_with_fake_probe(probe)

                serialized = json.dumps(report)
                self.assertEqual(exit_code, 4)
                self.assertEqual(report["status"], "setup_failed")
                self.assertEqual(report["problem"]["stage"], "order")
                self.assertEqual(report["problem"]["http_status"], 400)
                self.assertEqual(report["problem"]["exchange_code"], exchange_code)
                self.assertNotIn(str(ORDER_ID), serialized)
                self.assertNotIn(private_message, serialized)
                self.assertEqual(
                    [call["path"] for call in probe.calls],
                    [visibility.TIME_PATH, visibility.ORDER_PATH],
                )

    def test_interactive_selection_lists_newest_first_and_samples_the_selected_order_id(self) -> None:
        rows = [
            {"symbol": SYMBOL, "orderId": 501, "time": ACCEPTED_MS + 1_000, "status": "FILLED"},
            {"symbol": SYMBOL, "orderId": 503, "time": ACCEPTED_MS + 3_000, "status": "NEW"},
            {"symbol": SYMBOL, "orderId": 502, "time": ACCEPTED_MS + 2_000, "status": "PARTIALLY_FILLED"},
        ]
        probe = _probe_for([(200, rows)])
        collection_report = {"status": "bounded", "interval_ms": {"lower": 0, "upper": 100},
                             "counts": {"missing": 0}, "observations": []}

        exit_code, stdout, stderr, collect, user_input, _ = self._run_cli_with_fakes(
            probe,
            ["--duration-seconds", "3", "--poll-interval-ms", "500"],
            input_value="2",
            collection_report=collection_report,
        )

        self.assertEqual(exit_code, 0)
        report = json.loads(stdout)
        self.assertEqual(report["selection"], "recent_orders")
        self.assertEqual(report["status"], "already_visible")
        self.assertIsNone(report["interval_ms"])
        self.assertNotIn("501", stdout)
        self.assertNotIn("502", stdout)
        self.assertNotIn("503", stdout)
        self.assertIn("Select an order by row number", stderr)
        positions = [stderr.index(str(order_id)) for order_id in (503, 502, 501)]
        self.assertEqual(positions, sorted(positions))
        user_input.assert_called_once_with()
        collect.assert_called_once_with(probe, SYMBOL, 502, 3, 500)
        self.assertEqual([call["path"] for call in probe.calls], [visibility.TIME_PATH, visibility.ALL_ORDERS_PATH])
        list_call = probe.calls[-1]
        self.assertEqual(list_call["params"], {"symbol": SYMBOL, "limit": 10})

    def test_list_only_prints_recent_orders_without_sampling_or_prompting(self) -> None:
        rows = [
            {"symbol": SYMBOL, "orderId": 601, "time": ACCEPTED_MS + 1_000, "status": "FILLED"},
            {"symbol": SYMBOL, "orderId": 603, "time": ACCEPTED_MS + 3_000, "status": "NEW"},
            {"symbol": SYMBOL, "orderId": 602, "time": ACCEPTED_MS + 2_000, "status": "PARTIALLY_FILLED"},
        ]
        probe = _probe_for([(200, rows)])

        exit_code, stdout, stderr, collect, user_input, _ = self._run_cli_with_fakes(
            probe, ["--list-only"], collection_report={"status": "bounded", "counts": {"missing": 0}}
        )

        self.assertEqual(exit_code, 0)
        positions = [stdout.index(str(order_id)) for order_id in (603, 602, 601)]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn("Select an order", stderr)
        collect.assert_not_called()
        user_input.assert_not_called()
        self.assertEqual([call["path"] for call in probe.calls], [visibility.TIME_PATH, visibility.ALL_ORDERS_PATH])
        self.assertEqual(probe.calls[-1]["params"], {"symbol": SYMBOL, "limit": 10})

    def test_empty_recent_order_list_does_not_prompt_or_start_sampling(self) -> None:
        for arguments, expected_exit in (([], 2), (["--list-only"], 0)):
            with self.subTest(arguments=arguments):
                probe = _probe_for([(200, [])])

                exit_code, stdout, stderr, collect, user_input, _ = self._run_cli_with_fakes(
                    probe, arguments, collection_report={"status": "bounded", "counts": {"missing": 0}}
                )

                self.assertEqual(exit_code, expected_exit)
                self.assertIn("No visible Testnet orders", stderr)
                self.assertEqual(stdout, "")
                collect.assert_not_called()
                user_input.assert_not_called()
                self.assertEqual([call["path"] for call in probe.calls], [visibility.TIME_PATH, visibility.ALL_ORDERS_PATH])

    def test_a_missing_sample_after_interactive_selection_is_nonmonotonic(self) -> None:
        rows = [{"symbol": SYMBOL, "orderId": 702, "time": ACCEPTED_MS + 2_000, "status": "NEW"}]
        probe = _probe_for([(200, rows)])
        collection_report = {
            "status": "right_censored",
            "interval_ms": {"lower": 1_000, "upper": None},
            "counts": {"missing": 1},
            "observations": [{"result": "missing", "before_ms": 1_000, "after_ms": 1_100, "gap_ms": None}],
        }

        exit_code, stdout, _stderr, collect, _user_input, _ = self._run_cli_with_fakes(
            probe, [], input_value="1", collection_report=collection_report
        )

        report = json.loads(stdout)
        self.assertEqual(exit_code, 0)
        self.assertEqual(report["selection"], "recent_orders")
        self.assertEqual(report["status"], "nonmonotonic")
        self.assertIsNone(report["interval_ms"])
        collect.assert_called_once_with(probe, SYMBOL, 702, 20, 1000)

    def test_explicit_order_id_skips_listing_and_preserves_direct_sampling(self) -> None:
        probe = _probe_for([])
        collection_report = {"status": "bounded", "interval_ms": {"lower": 0, "upper": 100},
                             "counts": {"missing": 0}, "observations": []}

        exit_code, stdout, stderr, collect, user_input, _ = self._run_cli_with_fakes(
            probe,
            ["--order-id", str(ORDER_ID), "--duration-seconds", "4", "--poll-interval-ms", "750"],
            collection_report=collection_report,
        )

        self.assertEqual(exit_code, 0)
        report = json.loads(stdout)
        self.assertNotIn("selection", report)
        self.assertEqual(report["status"], "bounded")
        self.assertEqual(stderr, "")
        user_input.assert_not_called()
        collect.assert_called_once_with(probe, SYMBOL, ORDER_ID, 4, 750)
        self.assertEqual(probe.calls, [])


if __name__ == "__main__":
    unittest.main()
