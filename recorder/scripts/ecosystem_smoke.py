"""Manual smoke test: can generic analysis tools read a sealed segment directly?

Builds a temporary segment from the real fixtures and reads it with whatever is installed:
``zstd -dc`` (if on PATH), pandas, Polars and DuckDB. Every reader must see every record.
If a tool cannot read the file directly, change the format - do not weaken the claim.

Run from the recorder directory:  python scripts/ecosystem_smoke.py
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "tests"))

from conftest import load_frames  # noqa: E402

from hy_recorder.clock import FakeClock  # noqa: E402
from hy_recorder.envelope import encode_record  # noqa: E402
from hy_recorder.manifest import Manifest  # noqa: E402
from hy_recorder.segment import SegmentWriter  # noqa: E402
from hy_recorder.state import StateFile  # noqa: E402

REPEAT = 20


SETS = {
    "single": ("spot", "depth", [("spot:btcusdt@depth@100ms", "spot_depth_bridge_frames.jsonl")]),
    # a class with several different payload schemas in one file: this is what broke nested payloads in Polars
    "mixed": (
        "usdm",
        "market",
        [
            ("usdm:btcusdt@aggTrade", "usdm_aggtrade_frames.jsonl"),
            ("usdm:btcusdt@markPrice@1s", "usdm_markprice_frames.jsonl"),
            ("usdm:!forceOrder@arr", "usdm_forceorder_frames.jsonl"),
            ("usdm:btcusdt@depth@100ms", "usdm_depth_bridge_frames.jsonl"),
        ],
    ),
}


def build_segment(root: Path, which: str) -> tuple[Path, int]:
    venue, cls, streams = SETS[which]
    state = StateFile(root)
    state.load()
    run, _ = state.begin_run()
    clock = FakeClock()
    writer = SegmentWriter(
        root, venue, cls, level=9, next_segseq=state.next_segseq, manifest=Manifest(root), clock=clock
    )
    seq = 0
    for _ in range(REPEAT):
        for stream, fixture in streams:
            for _, _, payload in load_frames(fixture):
                seq += 1
                line, _ = encode_record(
                    run=run,
                    seq=seq,
                    gen=1,
                    wall_us=clock.wall_us(),
                    mono_us=clock.mono_us(),
                    stream=stream,
                    payload=payload,
                )
                writer.write(line, run=run, seq=seq, wall_us=clock.wall_us(), stream=stream)
                clock.advance(0.001)
    ev = writer.seal("smoke")
    assert ev is not None
    return root / ev["name"], seq


def main() -> int:
    failures = 0
    for which in SETS:
        tmp = Path(tempfile.mkdtemp(prefix="hy_smoke_"))
        try:
            path, n = build_segment(tmp, which)
            print(
                "[%s] segment %s (%d bytes, %d records expected)" % (which, path.name, path.stat().st_size, n)
            )

            def report(name: str, got: int | None, extra: str = "", n: int = n) -> None:
                nonlocal failures
                ok = got == n
                failures += 0 if ok else 1
                print("  %-10s rows=%-6s %s %s" % (name, got, "OK" if ok else "MISMATCH", extra))

            if shutil.which("zstd"):
                out = subprocess.run(["zstd", "-dc", str(path)], capture_output=True, check=True).stdout
                report("zstd -dc", len(out.splitlines()))
            else:
                print("  zstd -dc   (zstd CLI not on PATH here; checked on the recorder host instead)")

            try:
                import pandas as pd

                df = pd.read_json(path, lines=True, compression="zstd")
                ok_cols = {"v", "k", "r", "q", "g", "t", "m", "s", "p"} <= set(df.columns)
                report("pandas", len(df), "columns ok" if ok_cols else "columns MISSING")
                failures += 0 if ok_cols else 1
            except Exception as exc:  # noqa: BLE001 - report every failure mode
                failures += 1
                print("  pandas     FAILED: %r" % exc)

            try:
                import polars as pl

                report("polars", pl.read_ndjson(path).height)
            except Exception as exc:  # noqa: BLE001
                failures += 1
                print("  polars     FAILED: %r" % exc)

            try:
                import duckdb

                rows = duckdb.sql(
                    "select count(*) from read_json('%s', format='newline_delimited')" % path.as_posix()
                ).fetchone()[0]
                report("duckdb", rows)
            except Exception as exc:  # noqa: BLE001
                failures += 1
                print("  duckdb     FAILED: %r" % exc)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print("RESULT:", "PASS" if failures == 0 else "FAIL (%d)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
