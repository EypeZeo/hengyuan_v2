"""Command line: ``python -m hy_recorder <run|verify|report|status|config-check|pull|maint>``."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .config import ConfigError, load_config


def _cmd_run(args: argparse.Namespace) -> int:
    from .recorder import run_forever

    return run_forever(load_config(args.config))


def _cmd_verify(args: argparse.Namespace) -> int:
    from .verify import verify_lake

    report = verify_lake(Path(args.root), check_hashes=not args.no_hash)
    print(json.dumps(report.to_dict(), indent=1, sort_keys=True) if args.json else report.render_text())
    return report.exit_code


def _cmd_report(args: argparse.Namespace) -> int:
    from .report import build_report, render_text

    report = build_report(Path(args.root), sample_every=args.sample_every)
    print(json.dumps(report, indent=1, sort_keys=True) if args.json else render_text(report))
    return 0


def _cmd_status(args: argparse.Namespace) -> int:
    from .writer import dump_status

    try:
        print(dump_status(Path(args.root)))
    except FileNotFoundError:
        print(
            "no status.json under %s (recorder never ran there, or has not written it yet)" % args.root,
            file=sys.stderr,
        )
        return 2
    return 0


def _cmd_config_check(args: argparse.Namespace) -> int:
    cfg = load_config(args.config)
    print("root: %s\nsymbols: %s\nfingerprint: %s" % (cfg.root, ",".join(cfg.symbols), cfg.fingerprint()))
    for c in cfg.connections:
        print("%-22s %-5s %-7s %s" % (c.name, c.venue, c.cls, c.url))
    return 0


def _cmd_pull(args: argparse.Namespace) -> int:
    from .pull import SshTransport, pull

    transport = SshTransport(args.host, args.remote_root, ssh=args.ssh)
    result = pull(
        transport,
        Path(args.dest),
        ack=not args.no_ack,
        dry_run=args.dry_run,
        log=lambda m: print(m, flush=True),
    )
    print(
        "fetched %d (%d bytes), already had %d, acked %d, errors %d, evicted-before-pull %d"
        % (
            len(result.fetched),
            result.bytes_fetched,
            len(result.already_had),
            len(result.acked),
            len(result.errors),
            len(result.evicted_unfetched),
        )
    )
    for line in result.errors:
        print("ERROR: " + line, file=sys.stderr)
    for name in result.evicted_unfetched:
        print("LOST (reclaimed on the host before it was pulled): " + name, file=sys.stderr)
    return 0 if result.ok else 1


def _cmd_maint(args: argparse.Namespace) -> int:
    from .maint import load_config as load_maint_config
    from .maint import run_maintenance

    return run_maintenance(load_maint_config(args.config), force=args.force)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="hy_recorder", description="Read-only public market data recorder and verifier"
    )
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="record until SIGTERM/SIGINT")
    r.add_argument("--config", required=True)
    r.set_defaults(fn=_cmd_run)
    v = sub.add_parser("verify", help="verify a data lake offline; exit code 0 only if no FAIL")
    v.add_argument("--root", required=True)
    v.add_argument("--json", action="store_true")
    v.add_argument("--no-hash", action="store_true", help="skip SHA-256 of segments (size still checked)")
    v.set_defaults(fn=_cmd_verify)
    rp = sub.add_parser("report", help="operations report: rates, storage, bridge time, gaps, clock offset")
    rp.add_argument("--root", required=True)
    rp.add_argument("--json", action="store_true")
    rp.add_argument(
        "--sample-every", type=int, default=20, help="decode every Nth record for clock statistics"
    )
    rp.set_defaults(fn=_cmd_report)
    s = sub.add_parser("status", help="print the recorder's status.json")
    s.add_argument("--root", required=True)
    s.set_defaults(fn=_cmd_status)
    c = sub.add_parser("config-check", help="validate a config file and print the connections it derives")
    c.add_argument("--config", required=True)
    c.set_defaults(fn=_cmd_config_check)
    pl = sub.add_parser(
        "pull", help="operator side: mirror sealed segments over ssh, verify, then acknowledge"
    )
    pl.add_argument("--host", required=True, help="ssh destination (an alias from your ssh config)")
    pl.add_argument("--remote-root", default="/var/lib/hy-recorder")
    pl.add_argument("--dest", required=True)
    pl.add_argument("--ssh", default="ssh")
    pl.add_argument("--dry-run", action="store_true")
    pl.add_argument("--no-ack", action="store_true")
    pl.set_defaults(fn=_cmd_pull)
    mt = sub.add_parser(
        "maint",
        help="operator side: one maintenance run (pull, probe the host, check, write status);"
        " exit code 0/1/2 = ok/WARN/CRIT",
    )
    mt.add_argument("--config", required=True, help="maint.json (host, lake, logs, thresholds)")
    mt.add_argument(
        "--force", action="store_true", help="run even if the last success is more recent than min_interval_h"
    )
    mt.set_defaults(fn=_cmd_maint)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.fn(args))
    except ConfigError as exc:
        print("config error: %s" % exc, file=sys.stderr)
        return 2
