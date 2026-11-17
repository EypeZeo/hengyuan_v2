"""Offline order-submission tests; every exchange interaction uses FakeProbe."""

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
ACCOUNT_UID = 2_147_483_647
ORDER_ID = 8_765_432_109
LOOKUP_ORDER_ID = 8_765_432_110
SERVER_MS = 1_700_000_000_000
ACK_MS = SERVER_MS + 2_000
LOOKUP_MS = SERVER_MS + 3_000
PRICE = "64000.25"
QUANTITY = "0.0025"


class EnvFileParsingTests(unittest.TestCase):
    def test_utf8_bom_is_removed_from_a_noncredential_configuration_key(self) -> None:
        with tempfile.TemporaryDirectory(prefix="r6-env-file-") as directory:
            config_path = Path(directory) / "generic-config.env"
            config_path.write_text("APP_MODE=offline\n", encoding="utf-8-sig")

            self.assertEqual(visibility.r6.read_env_file(config_path), {"APP_MODE": "offline"})


def _text(body: object) -> str | None:
    return body if isinstance(body, str) else json.dumps(body)


class FakeProbe:
    """A protocol fake that never calls a signer or transport."""

    def __init__(
        self,
        key: str,
        *,
        uid: object = ACCOUNT_UID,
        time_status: int | None = 200,
        account_status: int | None = 200,
        account_body: object | None = None,
        submit_response: tuple[int | None, object] | None = None,
        order_responses: list[tuple[int | None, object]] | None = None,
    ) -> None:
        self.key = key
        self.offset_ms = 0
        self.uid = uid
        self.time_status = time_status
        self.account_status = account_status
        self.account_body = account_body if account_body is not None else {"uid": uid}
        self.submit_response = submit_response
        self.order_responses = deque(order_responses or [])
        self.calls: list[dict[str, object]] = []
        self.signed_once_calls: list[dict[str, object]] = []
        self._last_time = SERVER_MS
        self._submit_count = 0

    def public(self, path: str) -> tuple[int | None, str | None]:
        self.calls.append({"kind": "public", "path": path})
        if path != visibility.TIME_PATH:
            raise AssertionError(f"unexpected public path: {path}")
        self._last_time += 100
        if self.time_status is None:
            return None, None
        return self.time_status, json.dumps({"serverTime": self._last_time})

    def signed(self, path: str, params: dict[str, object]) -> tuple[int | None, str | None]:
        call = {"kind": "signed", "path": path, "params": dict(params)}
        self.calls.append(call)
        if path == visibility.ACCOUNT_PATH:
            return self.account_status, _text(self.account_body)
        if path == visibility.ORDER_PATH:
            if not self.order_responses:
                raise AssertionError("unexpected order lookup")
            response = self.order_responses.popleft()
            if callable(response[1]):
                return response[0], _text(response[1](params))
            return response[0], _text(response[1])
        raise AssertionError(f"unexpected signed path: {path}")

    def signed_once(self, path: str, params: dict[str, object], *, transport=None) -> tuple[int | None, str | None]:
        self._submit_count += 1
        self.signed_once_calls.append({"path": path, "params": dict(params), "transport": transport})
        if self._submit_count != 1:
            raise AssertionError("a single placement attempt must never be repeated")
        if self.submit_response is None:
            raise AssertionError("unexpected placement request")
        status, body = self.submit_response
        if callable(body):
            return status, _text(body(params))
        return status, _text(body)


def _success_ack(params: dict[str, object]) -> dict[str, object]:
    return {
        "symbol": SYMBOL,
        "clientOrderId": params["newClientOrderId"],
        "orderId": ORDER_ID,
        "transactTime": ACK_MS,
        "time": ACK_MS - 500,
    }


def _recovered_order(params: dict[str, object]) -> dict[str, object]:
    return {
        "symbol": SYMBOL,
        "clientOrderId": params["origClientOrderId"],
        "orderId": LOOKUP_ORDER_ID,
        "time": LOOKUP_MS,
    }


def _ready_probes(
    *,
    read_uid: object = ACCOUNT_UID,
    trade_uid: object = ACCOUNT_UID,
    submit_response: tuple[int | None, object] | None = None,
    order_responses: list[tuple[int | None, object]] | None = None,
) -> tuple[FakeProbe, FakeProbe]:
    if submit_response is None:
        submit_response = (200, _success_ack)
    read_probe = FakeProbe("READ_KEY_ONLY", uid=read_uid, order_responses=order_responses)
    trade_probe = FakeProbe("TRADE_KEY_ONLY", uid=trade_uid, submit_response=submit_response)
    return read_probe, trade_probe


def _submit(read_probe: FakeProbe, trade_probe: FakeProbe, *, sleep=lambda _seconds: None):
    return visibility.submit_testnet_order(
        read_probe, trade_probe, SYMBOL, "BUY", PRICE, QUANTITY, sleep=sleep
    )


def _lookup_response(status: int | None, body: object):
    return status, body


class SubmitTestnetOrderTests(unittest.TestCase):
    def test_matching_accounts_submit_once_and_use_ack_transact_time(self) -> None:
        read_probe, trade_probe = _ready_probes()

        order_id, accepted_ms = _submit(read_probe, trade_probe)

        self.assertEqual((order_id, accepted_ms), (ORDER_ID, ACK_MS))
        self.assertEqual(len(trade_probe.signed_once_calls), 1)
        request = trade_probe.signed_once_calls[0]
        self.assertEqual(request["path"], visibility.ORDER_PATH)
        self.assertIs(request["transport"], visibility.testnet_post_order)
        params = request["params"]
        self.assertEqual(params["symbol"], SYMBOL)
        self.assertEqual(params["side"], "BUY")
        self.assertEqual(params["type"], "LIMIT_MAKER")
        self.assertEqual(params["price"], PRICE)
        self.assertEqual(params["quantity"], QUANTITY)
        self.assertEqual(params["newOrderRespType"], "ACK")
        self.assertTrue(params["newClientOrderId"])
        self.assertEqual(read_probe.signed_once_calls, [])

        read_account_call = next(i for i, call in enumerate(read_probe.calls) if call.get("path") == visibility.ACCOUNT_PATH)
        trade_account_call = next(i for i, call in enumerate(trade_probe.calls) if call.get("path") == visibility.ACCOUNT_PATH)
        self.assertEqual(read_probe.calls[read_account_call - 1]["path"], visibility.TIME_PATH)
        self.assertEqual(trade_probe.calls[trade_account_call - 1]["path"], visibility.TIME_PATH)
        self.assertEqual([call["path"] for call in read_probe.calls], [visibility.TIME_PATH, visibility.ACCOUNT_PATH])
        self.assertEqual([call["path"] for call in trade_probe.calls], [visibility.TIME_PATH, visibility.ACCOUNT_PATH])

    def test_account_uid_mismatch_refuses_before_placement(self) -> None:
        read_probe, trade_probe = _ready_probes(trade_uid=ACCOUNT_UID + 1)

        with self.assertRaises(visibility.ReadFailure) as raised:
            _submit(read_probe, trade_probe)

        self.assertEqual(raised.exception.details["reason"], "account_uid_mismatch")
        self.assertEqual(trade_probe.signed_once_calls, [])
        self.assertFalse(any(call.get("path") == visibility.ORDER_PATH for call in read_probe.calls))

    def test_same_api_key_is_refused_before_exchange_reads(self) -> None:
        read_probe, trade_probe = _ready_probes()
        trade_probe.key = read_probe.key

        with self.assertRaises(visibility.ReadFailure) as raised:
            _submit(read_probe, trade_probe)

        self.assertEqual(raised.exception.details["reason"], "distinct_api_keys_required")
        self.assertEqual(read_probe.calls, [])
        self.assertEqual(trade_probe.calls, [])
        self.assertEqual(trade_probe.signed_once_calls, [])

    def test_account_uid_must_be_a_nonzero_integer(self) -> None:
        for invalid_uid in (0, -1, "2147483647", True, None):
            with self.subTest(uid=invalid_uid):
                read_probe, trade_probe = _ready_probes(read_uid=invalid_uid)

                with self.assertRaises(visibility.ReadFailure) as raised:
                    _submit(read_probe, trade_probe)

                self.assertEqual(raised.exception.details["reason"], "account_uid_unavailable")
                self.assertEqual(trade_probe.signed_once_calls, [])

    def test_definitive_rejection_does_not_query_or_repeat_the_order(self) -> None:
        read_probe, trade_probe = _ready_probes(
            submit_response=(400, {"code": -2010, "msg": "rejected order details"})
        )

        with self.assertRaises(visibility.SubmissionFailure) as raised:
            _submit(read_probe, trade_probe)

        self.assertEqual(raised.exception.details["reason"], "order_rejected")
        self.assertEqual(raised.exception.details["http_status"], 400)
        self.assertEqual(raised.exception.details["exchange_code"], -2010)
        self.assertEqual(len(trade_probe.signed_once_calls), 1)
        self.assertFalse(any(call.get("path") == visibility.ORDER_PATH for call in read_probe.calls))

    def test_rate_limit_and_forbidden_responses_stop_without_recovery(self) -> None:
        for status in (403, 418, 429):
            with self.subTest(status=status):
                read_probe, trade_probe = _ready_probes(submit_response=(status, None))
                sleeps: list[float] = []

                with self.assertRaises(visibility.SubmissionFailure) as raised:
                    _submit(read_probe, trade_probe, sleep=sleeps.append)

                self.assertTrue(raised.exception.stopped)
                self.assertEqual(raised.exception.details["reason"], "backoff")
                self.assertEqual(len(trade_probe.signed_once_calls), 1)
                self.assertFalse(any(call.get("path") == visibility.ORDER_PATH for call in read_probe.calls))
                self.assertEqual(sleeps, [])

    def test_ambiguous_ack_recovers_by_client_order_id_without_resubmitting(self) -> None:
        read_probe, trade_probe = _ready_probes(
            submit_response=(500, {"code": -1000, "msg": "hidden server response"}),
            order_responses=[_lookup_response(200, _recovered_order)],
        )
        sleeps: list[float] = []

        order_id, accepted_ms = _submit(read_probe, trade_probe, sleep=sleeps.append)

        self.assertEqual((order_id, accepted_ms), (LOOKUP_ORDER_ID, LOOKUP_MS))
        self.assertEqual(len(trade_probe.signed_once_calls), 1)
        submit_params = trade_probe.signed_once_calls[0]["params"]
        lookup_calls = [call for call in read_probe.calls if call.get("path") == visibility.ORDER_PATH]
        self.assertEqual(len(lookup_calls), 1)
        self.assertEqual(
            lookup_calls[0]["params"],
            {"symbol": SYMBOL, "origClientOrderId": submit_params["newClientOrderId"]},
        )
        self.assertEqual(sleeps, [])

    def test_duplicate_code_fields_in_http_400_trigger_read_only_recovery(self) -> None:
        duplicate_code_body = '{"code":-1007,"code":-1013,"msg":"execution status unknown"}'
        read_probe, trade_probe = _ready_probes(
            submit_response=(400, duplicate_code_body),
            order_responses=[_lookup_response(200, _recovered_order)],
        )

        order_id, accepted_ms = _submit(read_probe, trade_probe)

        self.assertEqual((order_id, accepted_ms), (LOOKUP_ORDER_ID, LOOKUP_MS))
        self.assertEqual(len(trade_probe.signed_once_calls), 1)
        submit_params = trade_probe.signed_once_calls[0]["params"]
        self.assertIsInstance(submit_params, dict)
        client_order_id = submit_params["newClientOrderId"]
        lookup_calls = [call for call in read_probe.calls if call.get("path") == visibility.ORDER_PATH]
        self.assertEqual(len(lookup_calls), 1)
        self.assertEqual(
            lookup_calls[0]["params"],
            {"symbol": SYMBOL, "origClientOrderId": client_order_id},
        )
        self.assertEqual(trade_probe.signed_once_calls[0]["path"], visibility.ORDER_PATH)

    def test_transport_timeout_and_unclear_ack_also_use_bounded_lookup(self) -> None:
        ambiguous_acks = (
            (None, None),
            (200, {"symbol": SYMBOL, "orderId": ORDER_ID, "clientOrderId": "wrong-id"}),
            (400, {"code": -1000, "msg": "execution status unknown"}),
            (400, {"code": -1001, "msg": "execution status unknown"}),
            (400, {"code": -1006, "msg": "execution status unknown"}),
            (400, {"code": -1007, "msg": "execution status unknown"}),
        )
        for ack in ambiguous_acks:
            with self.subTest(ack=ack):
                read_probe, trade_probe = _ready_probes(
                    submit_response=ack,
                    order_responses=[_lookup_response(200, _recovered_order)],
                )

                self.assertEqual(_submit(read_probe, trade_probe), (LOOKUP_ORDER_ID, LOOKUP_MS))
                self.assertEqual(len(trade_probe.signed_once_calls), 1)
                self.assertEqual(
                    len([call for call in read_probe.calls if call.get("path") == visibility.ORDER_PATH]), 1
                )

    def test_unknown_order_is_queried_a_finite_three_times_then_refused(self) -> None:
        not_found = (400, {"code": -2013, "msg": "hidden not-found detail"})
        read_probe, trade_probe = _ready_probes(
            submit_response=(None, None),
            order_responses=[not_found] * visibility.RECOVERY_ATTEMPTS,
        )
        sleeps: list[float] = []

        with self.assertRaises(visibility.SubmissionFailure) as raised:
            _submit(read_probe, trade_probe, sleep=sleeps.append)

        self.assertEqual(raised.exception.details["reason"], "order_status_unknown")
        lookup_calls = [call for call in read_probe.calls if call.get("path") == visibility.ORDER_PATH]
        self.assertEqual(len(lookup_calls), visibility.RECOVERY_ATTEMPTS)
        client_order_ids = {call["params"]["origClientOrderId"] for call in lookup_calls}
        self.assertEqual(len(client_order_ids), 1)
        self.assertEqual(len(trade_probe.signed_once_calls), 1)
        self.assertEqual(sleeps, [0.5, 0.5])
        self.assertNotIn("hidden not-found detail", json.dumps(raised.exception.details))

    def test_backoff_during_lookup_stops_further_queries(self) -> None:
        read_probe, trade_probe = _ready_probes(
            submit_response=(500, {"code": -1000}),
            order_responses=[_lookup_response(429, None)],
        )
        sleeps: list[float] = []

        with self.assertRaises(visibility.SubmissionFailure) as raised:
            _submit(read_probe, trade_probe, sleep=sleeps.append)

        self.assertTrue(raised.exception.stopped)
        lookup_calls = [call for call in read_probe.calls if call.get("path") == visibility.ORDER_PATH]
        self.assertEqual(len(lookup_calls), 1)
        self.assertEqual(len(trade_probe.signed_once_calls), 1)
        self.assertEqual(sleeps, [])


class SubmitTestnetOrderCliTests(unittest.TestCase):
    def test_place_flags_use_distinct_fake_probes_and_pass_ack_time_to_visibility(self) -> None:
        read_probe, trade_probe = _ready_probes()
        created: list[FakeProbe] = []
        expected_probes = {
            ("READ_KEY_ONLY", "READ_SECRET_ONLY"): read_probe,
            ("TRADE_KEY_ONLY", "TRADE_SECRET_ONLY"): trade_probe,
        }

        def fake_probe_factory(key: str, secret: str, timeout: int, **kwargs: object) -> FakeProbe:
            self.assertEqual(timeout, 5)
            self.assertIs(kwargs["request_get"], visibility.testnet_get)
            self.assertEqual(kwargs["attempts"], 1)
            self.assertIn((key, secret), expected_probes)
            probe = expected_probes[(key, secret)]
            created.append(probe)
            return probe

        visibility_report = {
            "status": "bounded",
            "interval_ms": {"lower": 0, "upper": 200},
            "counts": {"missing": 0, "found": 1, "inconclusive": 0},
            "observations": [],
        }
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory(prefix="r6-order-cli-") as directory:
            read_credentials = Path(directory) / "read fake credentials.env"
            trade_credentials = Path(directory) / "trade fake credentials.env"
            read_credentials.touch()
            trade_credentials.touch()

            def fake_env_file(path: str) -> dict[str, str]:
                if path == str(read_credentials):
                    return {
                        visibility.r6.KEY_VAR: "READ_KEY_ONLY",
                        visibility.r6.SECRET_VAR: "READ_SECRET_ONLY",
                    }
                if path == str(trade_credentials):
                    return {
                        visibility.TRADE_KEY_VAR: "TRADE_KEY_ONLY",
                        visibility.TRADE_SECRET_VAR: "TRADE_SECRET_ONLY",
                    }
                raise AssertionError(f"unexpected environment file path: {path}")

            arguments = [
                "--env-file",
                str(read_credentials),
                "--place-testnet-order",
                "--trade-env-file",
                str(trade_credentials),
                "--symbol",
                SYMBOL,
                "--side",
                "BUY",
                "--price",
                PRICE,
                "--quantity",
                QUANTITY,
                "--duration-seconds",
                "2",
                "--poll-interval-ms",
                "500",
            ]
            with mock.patch.object(visibility.r6, "read_env_file", side_effect=fake_env_file) as read_env_file:
                with mock.patch.object(visibility.r6, "Probe", side_effect=fake_probe_factory):
                    with mock.patch.object(visibility, "collect_visibility", return_value=visibility_report) as collect:
                        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                            exit_code = visibility.main(arguments)

        self.assertEqual(exit_code, 0)
        report = json.loads(stdout.getvalue())
        self.assertEqual(report["selection"], "new_order")
        self.assertIn(f"Spot Testnet accepted {SYMBOL} orderId {ORDER_ID}", stderr.getvalue())
        self.assertEqual(created, [read_probe, trade_probe])
        self.assertEqual(
            read_env_file.call_args_list,
            [mock.call(str(read_credentials)), mock.call(str(trade_credentials))],
        )
        collect.assert_called_once_with(
            read_probe, SYMBOL, ORDER_ID, 2, 500, accepted_ms=ACK_MS
        )
        self.assertEqual(len(trade_probe.signed_once_calls), 1)


class _FakeHTTPResponse:
    def __init__(self, status: int = 200, body: bytes = b"{}") -> None:
        self.status = status
        self.body = body
        self.length: int | None = len(body)

    def read(self, amount: int) -> bytes:
        result = self.body[:amount]
        if self.length is not None:
            self.length = max(0, self.length - len(result))
        return result

    def __enter__(self) -> _FakeHTTPResponse:
        return self

    def __exit__(self, *_args: object) -> bool:
        return False


class _FakeHTTPSConnection:
    def __init__(self, host: str, timeout: float, response: _FakeHTTPResponse) -> None:
        self.host = host
        self.timeout = timeout
        self.response = response
        self.requests: list[tuple[str, str, dict[str, str], dict[str, object]]] = []
        self.closed = False

    def request(self, method: str, path: str, *, headers: dict[str, str], **kwargs: object) -> None:
        self.requests.append((method, path, dict(headers), dict(kwargs)))

    def getresponse(self) -> _FakeHTTPResponse:
        return self.response

    def close(self) -> None:
        self.closed = True


class TestnetOrderTransportTests(unittest.TestCase):
    def test_post_transport_uses_fixed_host_and_places_form_in_body(self) -> None:
        response = _FakeHTTPResponse()
        connection: _FakeHTTPSConnection | None = None

        def make_connection(host: str, *, timeout: float) -> _FakeHTTPSConnection:
            nonlocal connection
            connection = _FakeHTTPSConnection(host, timeout, response)
            return connection

        form = "symbol=BTCUSDT&side=BUY&type=LIMIT_MAKER&price=64000.25&quantity=0.0025"
        with mock.patch.object(visibility.http.client, "HTTPSConnection", side_effect=make_connection):
            result = visibility.testnet_post_order(
                visibility.ORDER_PATH + "?" + form,
                {"X-MBX-APIKEY": "FAKE_TRADE_KEY"},
                6.0,
            )

        self.assertEqual(result, (200, "{}"))
        self.assertIsNotNone(connection)
        self.assertEqual(connection.host, "testnet.binance.vision")
        self.assertEqual(connection.timeout, 6.0)
        self.assertEqual(
            connection.requests,
            [
                (
                    "POST",
                    visibility.ORDER_PATH,
                    {
                        "X-MBX-APIKEY": "FAKE_TRADE_KEY",
                        "Content-Type": "application/x-www-form-urlencoded",
                    },
                    {"body": form},
                )
            ],
        )
        self.assertTrue(connection.closed)

    def test_post_transport_rejects_any_path_other_than_order(self) -> None:
        with mock.patch.object(visibility.http.client, "HTTPSConnection") as constructor:
            with self.assertRaises(ValueError):
                visibility.testnet_post_order(
                    visibility.ACCOUNT_PATH + "?symbol=BTCUSDT",
                    {"X-MBX-APIKEY": "FAKE_TRADE_KEY"},
                    6.0,
                )

        constructor.assert_not_called()


if __name__ == "__main__":
    unittest.main()
