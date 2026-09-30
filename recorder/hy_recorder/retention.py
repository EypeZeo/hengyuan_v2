"""Rolling cleanup of sealed segments: never "stop without cleaning".

Order (:func:`plan_cleanup`, a pure function so the policy is testable without a disk):

1. always: segments that are **acked** (copied off the host and hash-verified by ``pull``) and older
   than the retention period (default 48 h);
2. if free space is still below the warning line: the oldest segments older than the retention
   period even if **not acked** (auditable ``EVICTED_UNACKED`` events with time range and hash);
3. if still below: step the retention period down 48 -> 24 -> 12 -> 6 hours, cleaning at each step;
4. only if free space is still below the floor after all that, ``hard_stop`` (something other than
   the recorder is filling the disk; writing must stop to protect the system disk).

Age is measured from the segment's last record time. Every deletion is written to the manifest
(``prune`` event carrying the hash and time range) and to the ledger, so a data hole is auditable.
"""

from __future__ import annotations

import os
import shutil
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from .clock import Clock
from .manifest import Manifest

_HOUR_US = 3_600_000_000
RECOVERED_KEEP_H = 72.0


def retention_steps(retain_hours: float) -> tuple[float, ...]:
    return (retain_hours,) + tuple(h for h in (24.0, 12.0, 6.0) if h < retain_hours)


@dataclass(frozen=True)
class SegInfo:
    name: str
    bytes: int
    last_t: int  # wall microseconds of the segment's last record (or its seal time)
    sha256: str
    acked: bool
    first_t: int | None = None
    first: dict[str, int] | None = None  # {"r","q","t"} of the first record (from the seal event)
    last: dict[str, int] | None = None


@dataclass(frozen=True)
class PruneAction:
    name: str
    reason: str  # acked_expired | evicted_unacked
    acked: bool
    bytes: int
    sha256: str
    first_t: int | None
    last_t: int
    level_h: float
    first: dict[str, int] | None = None
    last: dict[str, int] | None = None


@dataclass
class CleanupPlan:
    actions: list[PruneAction] = field(default_factory=list)
    stepdowns: list[float] = field(default_factory=list)
    free_after: int = 0
    hard_stop: bool = False


def plan_cleanup(
    segments: list[SegInfo],
    *,
    now_us: int,
    free_bytes: int,
    warn_bytes: int,
    floor_bytes: int,
    retain_hours: float = 48.0,
) -> CleanupPlan:
    plan = CleanupPlan()
    taken: set[str] = set()
    freed = 0

    def age_h(seg: SegInfo) -> float:
        return (now_us - seg.last_t) / _HOUR_US

    def take(seg: SegInfo, level: float) -> None:
        nonlocal freed
        reason = "acked_expired" if seg.acked else "evicted_unacked"
        plan.actions.append(
            PruneAction(
                seg.name,
                reason,
                seg.acked,
                seg.bytes,
                seg.sha256,
                seg.first_t,
                seg.last_t,
                level,
                seg.first,
                seg.last,
            )
        )
        taken.add(seg.name)
        freed += seg.bytes

    ordered = sorted(segments, key=lambda s: (s.last_t, s.name))
    for seg in ordered:  # 1. routine
        if seg.acked and age_h(seg) >= retain_hours:
            take(seg, retain_hours)
    for i, level in enumerate(retention_steps(retain_hours)):  # 2 and 3. pressure ladder
        if free_bytes + freed >= warn_bytes:
            break
        if i > 0:
            plan.stepdowns.append(level)
        for seg in ordered:
            if free_bytes + freed >= warn_bytes:
                break
            if seg.name not in taken and age_h(seg) >= level:
                take(seg, level)
    plan.free_after = free_bytes + freed
    plan.hard_stop = plan.free_after < floor_bytes  # 4.
    return plan


# -- disk helpers and the executor ---------------------------------------------------------------
def ack_dir(root: Path) -> Path:
    return Path(root) / "acks"


def segment_infos(root: Path, manifest: Manifest, clock: Clock) -> list[SegInfo]:
    acks = ack_dir(root)
    out: list[SegInfo] = []
    for name, ev in manifest.live_segments().items():
        last = (ev.get("last") or {}).get("t") or ev.get("wall_us") or clock.wall_us()
        first = (ev.get("first") or {}).get("t")
        out.append(
            SegInfo(
                name,
                int(ev.get("bytes") or 0),
                int(last),
                str(ev.get("sha256")),
                (acks / str(ev.get("sha256"))).exists(),
                first,
                ev.get("first"),
                ev.get("last"),
            )
        )
    return out


def run_cleanup(
    root: Path,
    manifest: Manifest,
    clock: Clock,
    emit: Callable[..., None],
    *,
    warn_bytes: int,
    floor_bytes: int,
    retain_hours: float,
    free_bytes: int | None = None,
) -> CleanupPlan:
    root = Path(root)
    free = shutil.disk_usage(root).free if free_bytes is None else free_bytes
    plan = plan_cleanup(
        segment_infos(root, manifest, clock),
        now_us=clock.wall_us(),
        free_bytes=free,
        warn_bytes=warn_bytes,
        floor_bytes=floor_bytes,
        retain_hours=retain_hours,
    )
    announced: set[float] = set()

    def announce(level: float) -> None:
        if level not in announced:
            announced.add(level)
            emit("RETENTION_STEPDOWN", retain_hours=level, free_bytes=free)

    for act in plan.actions:
        if (
            act.level_h in plan.stepdowns
        ):  # the step-down is announced right before the first removal it enables
            announce(act.level_h)
        try:
            os.remove(root / act.name)
        except FileNotFoundError:
            pass
        except OSError as exc:
            emit("PRUNE_FAILED", name=act.name, error=type(exc).__name__)
            continue
        manifest.append(
            {
                "ev": "prune",
                "name": act.name,
                "sha256": act.sha256,
                "reason": act.reason,
                "acked": act.acked,
                "first_t": act.first_t,
                "last_t": act.last_t,
                "bytes": act.bytes,
                "level_h": act.level_h,
                "first": act.first,
                "last": act.last,
                "wall_us": clock.wall_us(),
            }
        )
        emit(
            "PRUNED_ACKED" if act.acked else "EVICTED_UNACKED",
            name=act.name,
            sha256=act.sha256,
            bytes=act.bytes,
            first_t=act.first_t,
            last_t=act.last_t,
            level_h=act.level_h,
        )
    for level in plan.stepdowns:  # levels stepped down to that removed nothing (still worth an audit line)
        announce(level)
    _prune_recovered_originals(root, clock)
    return plan


def _prune_recovered_originals(root: Path, clock: Clock) -> None:
    rec = root / "recovered"
    if not rec.exists():
        return
    cutoff = clock.wall_us() / 1e6 - RECOVERED_KEEP_H * 3600
    for fp in rec.iterdir():
        try:
            if fp.is_file() and fp.stat().st_mtime < cutoff:
                fp.unlink()
        except OSError:
            continue


# -- emergency reserve -----------------------------------------------------------------------------
def reserve_path(root: Path) -> Path:
    return Path(root) / "reserve.bin"


def ensure_reserve(root: Path, size_bytes: int, free_bytes: int, warn_bytes: int) -> bool:
    """Create the emergency reserve file (space kept for sealing, manifest and ledger when the disk
    is nearly full). Returns True if it exists afterwards."""
    path = reserve_path(root)
    if path.exists():
        return True
    if free_bytes < warn_bytes + size_bytes:
        return False
    with open(path, "wb") as fh:
        if hasattr(os, "posix_fallocate"):
            os.posix_fallocate(fh.fileno(), 0, size_bytes)
        else:  # pragma: no cover - Windows / tests only
            fh.write(b"\0" * size_bytes)
        fh.flush()
        os.fsync(fh.fileno())
    return True


def release_reserve(root: Path) -> int:
    """Delete the reserve to make room for a graceful shutdown. Returns the bytes released."""
    path = reserve_path(root)
    try:
        size = path.stat().st_size
        path.unlink()
        return size
    except FileNotFoundError:
        return 0
