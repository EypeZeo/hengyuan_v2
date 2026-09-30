from __future__ import annotations

import re
from pathlib import Path

import pytest

from hy_recorder.config import ConfigError, default_config, load_config
from hy_recorder.guard import (
    ForbiddenEndpoint,
    build_rest_request,
    build_ws_url,
    depth_weight,
    endpoint_weight,
    validate_stream_name,
)

PKG = Path(__file__).resolve().parent.parent / "hy_recorder"


# -- guard ---------------------------------------------------------------------------------------
def test_allowlisted_rest_requests_are_built_exactly():
    assert build_rest_request("spot", "depth", {"symbol": "BTCUSDT", "limit": 1000}) == (
        "api.binance.com",
        "/api/v3/depth?limit=1000&symbol=BTCUSDT",
    )
    assert build_rest_request("usdm", "depth", {"symbol": "BTCUSDT", "limit": 100}) == (
        "fapi.binance.com",
        "/fapi/v1/depth?limit=100&symbol=BTCUSDT",
    )
    assert build_rest_request("spot", "time") == ("api.binance.com", "/api/v3/time")
    assert build_rest_request("usdm", "exchangeInfo") == ("fapi.binance.com", "/fapi/v1/exchangeInfo")


@pytest.mark.parametrize(
    ("venue", "endpoint"),
    [
        ("spot", "order"),
        ("spot", "account"),
        ("usdm", "listenKey"),
        ("usdm", "order"),
        ("coinm", "depth"),
        ("spot", "userDataStream"),
        ("spot", "../order"),
    ],
)
def test_private_and_trading_endpoints_cannot_be_built(venue, endpoint):
    with pytest.raises(ForbiddenEndpoint):
        build_rest_request(venue, endpoint, {})


@pytest.mark.parametrize("param", ["signature", "timestamp", "recvWindow", "orderId", "apiKey", "listenKey"])
def test_signature_style_parameters_are_rejected(param):
    with pytest.raises(ForbiddenEndpoint):
        build_rest_request("spot", "depth", {"symbol": "BTCUSDT", "limit": 100, param: "x"})


@pytest.mark.parametrize(
    "params",
    [
        {"symbol": "btcusdt", "limit": 100},  # lower case
        {"symbol": "BTC USDT", "limit": 100},
        {"symbol": "BTCUSDT", "limit": 999},  # not an allowed limit
        {"symbol": "BTCUSDT", "limit": "100"},
        {"symbol": "BTCUSDT"},  # depth needs a limit
        {"limit": 100},
    ],
)
def test_bad_depth_parameters_rejected(params):
    with pytest.raises(ForbiddenEndpoint):
        build_rest_request("spot", "depth", params)


def test_depth_weights_match_real_measurements():
    # measured on 2026-09-29: spot 1000 -> 50 and 100 -> 5; usdm 1000 -> 20 and 100 -> 5
    assert depth_weight("spot", 1000) == 50 and depth_weight("spot", 100) == 5
    assert depth_weight("usdm", 1000) == 20 and depth_weight("usdm", 100) == 5
    assert (
        endpoint_weight("spot", "exchangeInfo", {}) == 20 and endpoint_weight("usdm", "exchangeInfo", {}) == 1
    )
    assert endpoint_weight("spot", "time", {}) == 1
    with pytest.raises(ForbiddenEndpoint):
        depth_weight("spot", 5001)


def test_websocket_urls_use_the_routed_paths():
    assert (
        build_ws_url("spot", ["btcusdt@depth@100ms"])
        == "wss://stream.binance.com:9443/stream?streams=btcusdt@depth@100ms"
    )
    assert (
        build_ws_url("usdm_public", ["btcusdt@depth@100ms"])
        == "wss://fstream.binance.com/public/stream?streams=btcusdt@depth@100ms"
    )
    assert (
        build_ws_url("usdm_market", ["btcusdt@aggTrade", "btcusdt@markPrice@1s", "!forceOrder@arr"])
        == "wss://fstream.binance.com/market/stream?streams=btcusdt@aggTrade/btcusdt@markPrice@1s/!forceOrder@arr"
    )
    for legacy in ("usdm_legacy", "usdm", "private", "usdm_private"):
        with pytest.raises(ForbiddenEndpoint):
            build_ws_url(legacy, ["btcusdt@depth@100ms"])


@pytest.mark.parametrize(
    "name",
    [
        "BTCUSDT@depth",
        "btcusdt@depth@100ms;x",
        "btc usdt@trade",
        "btcusdt",
        "@trade",
        "",
        "x" * 80 + "@trade",
    ],
)
def test_bad_stream_names_rejected(name):
    with pytest.raises(ForbiddenEndpoint):
        validate_stream_name(name)


def test_ws_stream_count_limits():
    with pytest.raises(ForbiddenEndpoint):
        build_ws_url("spot", [])
    with pytest.raises(ForbiddenEndpoint):
        build_ws_url("spot", ["btcusdt@trade"] * 21)


def test_recorder_source_has_no_credential_or_trading_code_path():
    """Structural claim: nothing in the recorder reads credentials, signs, or names private endpoints."""
    forbidden = [
        r"apikey",
        r"api_key",
        r"secret",
        r"signature",
        r"hmac",
        r"listenkey",
        r"/order\b",
        r"userdatastream",
        r"os\.environ",
        r"getenv",
        r"\.env\b",
        r"private_key",
        r"password",
    ]
    pattern = re.compile("|".join(forbidden), re.IGNORECASE)
    offenders = []
    for path in sorted(PKG.rglob("*.py")):
        for i, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if pattern.search(line):
                offenders.append("%s:%d: %s" % (path.relative_to(PKG.parent), i, line.strip()))
    assert offenders == [], "\n".join(offenders)


# -- config --------------------------------------------------------------------------------------
def test_default_s1_layout(tmp_path):
    cfg = default_config(tmp_path)
    names = [c.name for c in cfg.connections]
    assert names == ["spot_depth_btcusdt", "usdm_depth_btcusdt", "spot_trade", "usdm_market"]
    by = {c.name: c for c in cfg.connections}
    assert by["spot_depth_btcusdt"].url == "wss://stream.binance.com:9443/stream?streams=btcusdt@depth@100ms"
    assert (
        by["usdm_depth_btcusdt"].url == "wss://fstream.binance.com/public/stream?streams=btcusdt@depth@100ms"
    )
    assert by["spot_trade"].url == "wss://stream.binance.com:9443/stream?streams=btcusdt@trade"
    assert (
        by["usdm_market"].url
        == "wss://fstream.binance.com/market/stream?streams=btcusdt@aggTrade/btcusdt@markPrice@1s/!forceOrder@arr"
    )
    assert cfg.depth_targets == (("spot", "BTCUSDT"), ("usdm", "BTCUSDT"))
    assert by["usdm_market"].stream_ids == (
        "usdm:btcusdt@aggTrade",
        "usdm:btcusdt@markPrice@1s",
        "usdm:!forceOrder@arr",
    )
    force = by["usdm_market"].streams[-1]
    assert force.kind == "forceOrder" and force.max_gap_s is None and force.first_frame_s is None
    assert all(c.cls in ("depth", "trade", "market") for c in cfg.connections)
    assert len({c.name for c in cfg.connections}) == len(cfg.connections)


def test_two_symbols_give_one_depth_connection_each(tmp_path):
    cfg = default_config(tmp_path, ("BTCUSDT", "ETHUSDT"))
    assert [c.name for c in cfg.connections if c.is_depth] == [
        "spot_depth_btcusdt",
        "usdm_depth_btcusdt",
        "spot_depth_ethusdt",
        "usdm_depth_ethusdt",
    ]
    assert (
        len(default_config(tmp_path, ("BTCUSDT", "ETHUSDT")).connections[-2].streams) == 2
    )  # trades of both


def test_validation_rejects_bad_values(tmp_path):
    for bad in (
        {"zstd_level": 0},
        {"zstd_level": 30},
        {"snapshot_limit": 50},
        {"snapshot_cooldown_s": 0.1},
        {"rest_budget_fraction": 0.9},
        {"retain_hours": 1},
        {"disk_floor_gb": 6.0},
        {"queue_capacity": 5},
        {"rotation_age_s": 100},
    ):
        with pytest.raises(ConfigError):
            default_config(tmp_path, **bad)
    for symbols in ((), ("btcusdt",), ("BTC/USDT",), tuple("A" * 6 for _ in range(11))):
        with pytest.raises(ConfigError):
            default_config(tmp_path, symbols)


def test_toml_loader(tmp_path):
    good = tmp_path / "good.toml"
    good.write_text(
        '[recorder]\nroot = "/var/lib/hy-recorder"\nsymbols = ["btcusdt"]\n'
        "zstd_level = 12\nretain_hours = 24\n"
    )
    cfg = load_config(good)
    assert cfg.zstd_level == 12 and cfg.retain_hours == 24 and cfg.symbols == ("BTCUSDT",)
    assert cfg.root == Path("/var/lib/hy-recorder") and len(cfg.connections) == 4
    for body in (
        '[recorder]\nroot = "x"\nhost = "evil.example"\n',  # unknown key
        '[recorder]\nroot = "x"\nzstd_level = "9"\n',  # wrong type
        "[recorder]\nzstd_level = 9\n",  # root missing
        "[other]\nx = 1\n",
        '[recorder]\nroot = "x"\nzstd_level = true\n',
    ):
        f = tmp_path / "bad.toml"
        f.write_text(body)
        with pytest.raises(ConfigError):
            load_config(f)


def test_fingerprint_changes_with_the_layout(tmp_path):
    a = default_config(tmp_path).fingerprint()
    assert a == default_config(tmp_path).fingerprint()
    assert a != default_config(tmp_path, ("BTCUSDT", "ETHUSDT")).fingerprint()
    assert a != default_config(tmp_path, zstd_level=12).fingerprint()
