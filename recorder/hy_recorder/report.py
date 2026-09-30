"""Operations report over a data lake (the D0-4 shadow-run deliverable): rates, storage and runway, bridge
time, reconnects, gaps and reasons, rate-limit counters, clock offset and queue pressure.

Everything here is derived from the lake itself (raw records, ledger, manifest, ``status.json``); the offline
verifier stays the authority on correctness (``hy_recorder verify``). Clock statistics decode only every
``sample_every``-th record so a full day of data stays cheap to summarise.
"""

from __future__ import annotations

import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from .envelope import EnvelopeError, decode_record
from .ledger import Ledger
from .manifest import Manifest
from .verify.reader import SegmentReader

US = 1_000_000


def _pct(values: list[float], p: float) -> float | None:
    if not values:
        return None
    s = sorted(values)
    return round(s[min(len(s) - 1, int(p / 100.0 * len(s)))], 3)


def _dist(values: list[float]) -> dict[str, Any]:
    if not values:
        return {"n": 0}
    return {
        "n": len(values),
        "min": round(min(values), 3),
        "p50": _pct(values, 50),
        "p95": _pct(values, 95),
        "p99": _pct(values, 99),
        "max": round(max(values), 3),
    }


def _probe_view(items: list[dict[str, Any]]) -> dict[str, Any]:
    """REST ``/time`` probes: the offset estimate is only as good as half the round trip, so besides the
    plain distributions report the median offset of the fastest tenth with its uncertainty."""
    pairs = [
        (p["rtt_us"] / 1000.0, float(p["offset_ms"]))
        for p in items
        if isinstance(p.get("rtt_us"), (int, float)) and isinstance(p.get("offset_ms"), (int, float))
    ]
    best = sorted(pairs)[: max(1, len(pairs) // 10)] if pairs else []
    return {
        "n": len(items),
        "offset_ms": _dist([o for _, o in pairs]),
        "rtt_ms": _dist([r for r, _ in pairs]),
        "fastest_tenth": {
            "n": len(best),
            "offset_ms_median": _pct([o for _, o in best], 50),
            "uncertainty_ms": round(max(r for r, _ in best) / 2.0, 3) if best else None,
        },
    }


def _event_time_ms(payload: bytes) -> int | None:
    try:
        obj = json.loads(payload)
    except ValueError:
        return None
    data = obj.get("data") if isinstance(obj, dict) else None
    if isinstance(data, dict) and isinstance(data.get("E"), int):
        return data["E"]
    return None


def build_report(root: Path, *, sample_every: int = 20) -> dict[str, Any]:
    root = Path(root)
    ledger = Ledger.read_all(root)
    manifest = Manifest(root)
    live = manifest.live_segments()
    prunes = [e for e in manifest.read_all() if e.get("ev") == "prune"]

    # -- storage ------------------------------------------------------------------------------
    by_class: dict[str, int] = defaultdict(int)
    first_t: int | None = None
    last_t: int | None = None
    for ev in live.values():
        by_class["%s/%s" % (ev.get("venue"), ev.get("cls"))] += int(ev.get("bytes") or 0)
        f, la = (ev.get("first") or {}).get("t"), (ev.get("last") or {}).get("t")
        if isinstance(f, int):
            first_t = f if first_t is None else min(first_t, f)
        if isinstance(la, int):
            last_t = la if last_t is None else max(last_t, la)
    span_s = (
        (last_t - first_t) / US if first_t is not None and last_t is not None and last_t > first_t else 0.0
    )
    total = sum(by_class.values())
    storage = {
        "sealed_segments": len(live),
        "sealed_bytes": total,
        "by_class_bytes": dict(sorted(by_class.items())),
        "span_hours": round(span_s / 3600, 2),
        "mb_per_day": round(total / span_s * 86400 / 1e6, 1) if span_s > 3600 else None,
        "pruned_segments": len(prunes),
        "evicted_unacked": sum(1 for p in prunes if not p.get("acked")),
    }

    # -- per-stream rates and clock offsets (sampled) ---------------------------------------------
    counts: Counter[str] = Counter()
    t_range: dict[str, list[int]] = {}
    offsets: dict[str, list[float]] = defaultdict(list)
    for name, ev in sorted(live.items()):
        path = root / name
        if not path.exists() or ev.get("cls") in ("snapshot", "ref"):
            continue
        for line in SegmentReader(path).lines():
            try:
                rec = decode_record(line)
            except EnvelopeError:
                continue
            counts[rec.stream] += 1
            rng = t_range.setdefault(rec.stream, [rec.wall_us, rec.wall_us])
            rng[0], rng[1] = min(rng[0], rec.wall_us), max(rng[1], rec.wall_us)
            if counts[rec.stream] % sample_every == 0 and rec.kind == "m":
                e_ms = _event_time_ms(rec.payload)
                if e_ms is not None:
                    offsets[rec.stream].append(rec.wall_us / 1000.0 - e_ms)
    streams = {}
    for stream, n in sorted(counts.items()):
        lo, hi = t_range[stream]
        secs = (hi - lo) / US
        streams[stream] = {
            "records": n,
            "per_second": round(n / secs, 2) if secs > 1 else None,
            "recv_minus_event_ms": _dist(offsets.get(stream, [])),
        }

    # -- connections, bridging and rate limits (ledger) ---------------------------------------
    opens: dict[
        tuple[int, str, int], dict[str, Any]
    ] = {}  # (run, connection, generation): gens restart each run
    closes: Counter[str] = Counter()
    per_conn: Counter[str] = Counter()
    bridge_s: list[float] = []
    slow_bridges: list[dict[str, Any]] = []
    kinds: Counter[str] = Counter()
    gaps: list[dict[str, Any]] = []
    for e in ledger:
        k = e.get("k", "")
        kinds[k] += 1
        if k == "WS_OPEN":
            opens[(e["run"], e["conn"], e["gen"])] = e
            per_conn[e["conn"]] += 1
        elif k == "WS_CLOSE":
            closes[str(e.get("reason", "?")).split(":")[0]] += 1
        elif k == "BRIDGE_OK":
            stream = e.get("stream", "")
            venue, _, name = stream.partition(":")
            opened = opens.get((e.get("run"), "%s_depth_%s" % (venue, name.split("@")[0]), e.get("gen")))
            if opened is not None:
                secs = (e["t"] - opened["t"]) / US
                bridge_s.append(secs)
                if secs > 10:
                    slow_bridges.append({"stream": stream, "gen": e.get("gen"), "seconds": round(secs, 1)})
        elif k == "GAP_DETECTED":
            gaps.append(
                {"stream": e.get("stream"), "gen": e.get("gen"), "rule": e.get("rule"), "t": e.get("t")}
            )

    probes = [e for e in ledger if e.get("k") == "CLOCK_PROBE"]
    status: dict[str, Any] = {}
    try:
        status = json.loads((root / "status.json").read_bytes())
    except (OSError, ValueError):
        pass
    queues = status.get("queues", {})
    disk = status.get("disk", {})
    runs = sorted({e.get("run") for e in ledger if isinstance(e.get("run"), int)})
    return {
        "lake": str(root),
        "runs": runs,
        "unclean_runs": sorted(
            r for r in runs if not any(e.get("k") == "PROC_STOP" and e.get("run") == r for e in ledger)
        ),
        "storage": storage,
        "disk_now": {
            "free_gb": round(disk["free_bytes"] / 2**30, 2) if "free_bytes" in disk else None,
            "runway_hours": disk.get("runway_hours"),
            "stopped": disk.get("stopped"),
        },
        "streams": streams,
        "connections": {
            "opens_per_connection": dict(per_conn),
            "close_reasons": dict(closes),
            "stalls": kinds["STREAM_STALL"],
            "no_data": kinds["SUBSCRIBED_NO_DATA"],
            "connect_failures": kinds["WS_CONNECT_FAIL"],
            "unexpected_streams": kinds["UNEXPECTED_STREAM"],
            "bad_frames": kinds["BAD_FRAME"],
        },
        "bridge_seconds_from_ws_open": _dist(bridge_s),
        "slow_bridges_over_10s": slow_bridges,
        "gaps": gaps,
        "snapshots": {
            "ok": kinds["SNAPSHOT"],
            "failed": kinds["SNAPSHOT_FAIL"],
            "stale": kinds["SNAPSHOT_STALE"],
            "deferred": kinds["SNAPSHOT_DEFERRED"],
            "discarded": kinds["SNAPSHOT_DISCARDED"],
        },
        "rate_limits": {
            "http_429_403_451_events": kinds["RATE_LIMIT"],
            "http_418_bans": kinds["BAN"],
            "geo_block_suspect": kinds["GEO_BLOCK_SUSPECT"],
            "status_counters": status.get("rest", {}).get("counters"),
        },
        "clock_probe": {
            **_probe_view(probes),
            "by_venue": {
                v: _probe_view([p for p in probes if p.get("venue") == v])
                for v in sorted({p.get("venue") for p in probes if p.get("venue")})
            },
        },
        "queues": {
            "high_water": {k: v.get("high_water") for k, v in queues.items()},
            "dropped": {k: v.get("dropped") for k, v in queues.items() if v.get("dropped")},
            "overrun_events": kinds["OVERRUN"],
            "loop_max_lag_s": (status.get("loop") or {}).get("max_lag_s"),
            "write_errors": status.get("write_errors"),
        },
        "disk_events": {
            k: kinds[k]
            for k in (
                "DISK_WARN",
                "DISK_STOP",
                "DISK_RESUMED",
                "PRUNED_ACKED",
                "EVICTED_UNACKED",
                "RETENTION_STEPDOWN",
            )
            if kinds[k]
        },
        "task_crashes": kinds["TASK_CRASH"],
        "loop_lag_events": kinds["LOOP_LAG"],
    }


def render_text(rep: dict[str, Any]) -> str:
    st = rep["storage"]
    out = [
        "lake: %s" % rep["lake"],
        "runs: %s (unclean: %s)" % (rep["runs"], rep["unclean_runs"] or "none"),
        "storage: %d sealed segments, %.1f MB, span %.2f h, %s; pruned %d (evicted unacked %d)"
        % (
            st["sealed_segments"],
            st["sealed_bytes"] / 1e6,
            st["span_hours"],
            "%.1f MB/day (%.2f GB/day)" % (st["mb_per_day"], st["mb_per_day"] / 1000)
            if st["mb_per_day"] is not None
            else "MB/day n/a (span < 1 h)",
            st["pruned_segments"],
            st["evicted_unacked"],
        ),
        "disk now: free %s GB, runway %s h, stopped=%s"
        % (rep["disk_now"]["free_gb"], rep["disk_now"]["runway_hours"], rep["disk_now"]["stopped"]),
        "streams:",
    ]
    for name, s in rep["streams"].items():
        d = s["recv_minus_event_ms"]
        clock = (
            "recv-E ms p50 %s p99 %s (n=%d)" % (d.get("p50"), d.get("p99"), d["n"])
            if d["n"]
            else "recv-E n/a"
        )
        out.append("  %-34s %9d records  %6s /s  %s" % (name, s["records"], s["per_second"], clock))
    c = rep["connections"]
    out += [
        "connections: opens %s; close reasons %s" % (c["opens_per_connection"], c["close_reasons"]),
        "  stalls %d, no-data %d, connect failures %d, unexpected streams %d, bad frames %d"
        % (c["stalls"], c["no_data"], c["connect_failures"], c["unexpected_streams"], c["bad_frames"]),
        "bridge seconds from WS_OPEN: %s; slow (>10 s): %s"
        % (rep["bridge_seconds_from_ws_open"], rep["slow_bridges_over_10s"] or "none"),
        "gaps: %d %s" % (len(rep["gaps"]), [g["rule"] for g in rep["gaps"]][:10]),
        "snapshots: %s" % rep["snapshots"],
        "rate limits: %s" % rep["rate_limits"],
        "clock probes (server minus host, ms):",
        *[
            "  %-5s n=%d rtt p50 %s min %s | fastest tenth: offset %s +/- %s"
            % (
                venue,
                v["n"],
                v["rtt_ms"].get("p50"),
                v["rtt_ms"].get("min"),
                v["fastest_tenth"]["offset_ms_median"],
                v["fastest_tenth"]["uncertainty_ms"],
            )
            for venue, v in rep["clock_probe"]["by_venue"].items()
        ],
        "queues: high-water %s dropped %s overrun events %d loop max lag %s s write errors %s"
        % (
            rep["queues"]["high_water"],
            rep["queues"]["dropped"] or "none",
            rep["queues"]["overrun_events"],
            rep["queues"]["loop_max_lag_s"],
            rep["queues"]["write_errors"],
        ),
        "disk events: %s; task crashes %d; loop-lag events %d"
        % (rep["disk_events"] or "none", rep["task_crashes"], rep["loop_lag_events"]),
    ]
    return "\n".join(out)
