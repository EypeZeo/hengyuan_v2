from __future__ import annotations

import json

import pytest
from conftest import FIXTURES, load_frames

from hy_recorder.envelope import EnvelopeError, decode_record, encode_record

KW = dict(
    run=3,
    seq=17,
    gen=2,
    wall_us=1_790_691_993_915_736,
    mono_us=123_456_789,
    stream="spot:btcusdt@depth@100ms",
)


def test_real_frames_roundtrip_byte_exact():
    names = sorted(p.name for p in FIXTURES.glob("*_frames.jsonl"))
    assert len(names) >= 6
    total = 0
    for name in names:
        for _, _, payload in load_frames(name):
            line, reason = encode_record(payload=payload, **KW)
            assert reason is None, (name, reason)
            assert line.endswith(b"}\n") and line.count(b"\n") == 1
            rec = decode_record(line)
            assert rec.payload == payload  # exact bytes, not merely equal JSON
            assert json.loads(line)["p"].encode("utf-8") == payload  # generic tools recover the same text
            assert rec.data == json.loads(payload)
            assert (rec.run, rec.seq, rec.gen, rec.stream, rec.kind) == (3, 17, 2, KW["stream"], "m")
            total += 1
    assert total >= 140  # all real fixture frames (spot/usdm depth, trades, aggTrade, markPrice, forceOrder)


def test_str_payload_and_bytes_payload_give_the_same_line():
    text = '{"stream":"x","data":{"a":[1,2],"e":"depthUpdate","E":1}}'
    assert encode_record(payload=text, **KW) == encode_record(payload=text.encode("utf-8"), **KW)


def test_line_is_standard_json_for_generic_tools():
    payload = b'{"stream":"x","data":{"a":[1,2]}}'
    line, _ = encode_record(payload=payload, **KW)
    obj = json.loads(line)
    assert isinstance(obj["p"], str) and json.loads(obj["p"]) == {"stream": "x", "data": {"a": [1, 2]}}
    assert obj["t"] == KW["wall_us"] and obj["m"] == KW["mono_us"]
    assert obj["t"] < 2**53  # microsecond integers are exact in float64


def test_newlines_odd_whitespace_and_unicode_are_kept_exactly():
    cases = [
        b'{"a":1,\n"b":2}',  # pretty-printed message
        b'{ "z" : 1 ,  "a":  [ 1 , 2 ] }\r\n',  # odd whitespace and a trailing CRLF
        '{"s":"中文 \U0001f680"}'.encode(),  # non-ASCII text
        b"plain text, not json",  # a text frame that is not JSON is still recorded verbatim
        b'{"quote":"a\\"b","back":"c\\\\d"}',
    ]
    for payload in cases:
        line, reason = encode_record(payload=payload, **KW)
        assert reason is None and line.count(b"\n") == 1
        assert decode_record(line).payload == payload


def test_unicode_line_separators_are_escaped_so_splitlines_cannot_break_a_record():
    payload = '{"a":"x y z"}'
    line, _ = encode_record(payload=payload, **KW)
    assert len(line.decode("utf-8").splitlines()) == 1
    assert decode_record(line).payload == payload.encode("utf-8")


def test_non_json_text_is_recorded_but_has_no_parsed_data():
    rec = decode_record(encode_record(payload=b"not json", **KW)[0])
    assert rec.payload == b"not json" and rec.data is None and rec.fallback_reason is None


@pytest.mark.parametrize(
    ("payload", "binary", "reason"),
    [
        (b'{"a":1}', True, "binary"),  # a binary WebSocket message stays binary even if it looks like text
        (b'{"a":"\xff\xfe"}', False, "invalid_utf8"),
        ("\ud800 lone surrogate", False, "invalid_utf8"),
    ],
)
def test_binary_or_undecodable_payloads_use_explicit_base64(payload, binary, reason):
    line, got = encode_record(payload=payload, binary=binary, **KW)
    assert got == reason and b'"p":' not in line and f'"x":"{reason}"'.encode() in line
    rec = decode_record(line)
    assert rec.fallback_reason == reason and rec.data is None
    expected = payload if isinstance(payload, bytes) else payload.encode("utf-8", "surrogatepass")
    assert rec.payload == expected


@pytest.mark.parametrize("stream", ["", "a b", 'a"b', "a\nb", "x" * 97, "spot/depth"])
def test_bad_stream_ids_rejected(stream):
    with pytest.raises(EnvelopeError):
        encode_record(payload=b"{}", **{**KW, "stream": stream})


@pytest.mark.parametrize("field", ["run", "seq", "gen", "wall_us", "mono_us"])
def test_bad_numbers_rejected(field):
    for bad in (-1, 1.5, True, "3"):
        with pytest.raises(EnvelopeError):
            encode_record(payload=b"{}", **{**KW, field: bad})


def test_bad_kind_rejected():
    with pytest.raises(EnvelopeError):
        encode_record(payload=b"{}", kind="q", **KW)


def test_decode_rejects_malformed_lines():
    good, _ = encode_record(payload=b'{"a":1}', **KW)
    for bad in (
        b"not json\n",
        b"[]\n",
        good.replace(b'"v":1', b'"v":2'),
        b'{"v":1}\n',
        good.replace(b'"p":"{', b'"p":{'),  # payload must be a string
    ):
        with pytest.raises(EnvelopeError):
            decode_record(bad)


def test_snapshot_kind_roundtrip():
    body = (FIXTURES / "spot_depth_snapshot_l100.json").read_bytes().strip()
    line, reason = encode_record(payload=body, kind="s", **{**KW, "stream": "spot:snapshot:BTCUSDT"})
    assert reason is None
    rec = decode_record(line)
    assert rec.kind == "s" and rec.payload == body and "lastUpdateId" in rec.data
