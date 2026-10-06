"""CLOCK_STATE: the recorder records through an unsynchronised clock and says so; it never waits."""

from __future__ import annotations

import asyncio

from hy_recorder.clocksync import MAX_MARKER_AGE_S, ClockStateWatcher, SyncState, read_sync_state


def test_a_marker_means_synced_and_reports_its_age(tmp_path):
    marker = tmp_path / "synchronized"
    marker.write_bytes(b"")
    st = read_sync_state(marker, now_s=marker.stat().st_mtime + 12.5)
    assert st == SyncState(True, "timesyncd", 12.5, None)


def test_a_runtime_directory_without_a_marker_means_unsynced(tmp_path):
    st = read_sync_state(tmp_path / "synchronized")
    assert st == SyncState(False, "timesyncd", None, "no_marker")


def test_no_runtime_directory_means_unknown_not_unsynced(tmp_path):
    st = read_sync_state(tmp_path / "missing" / "synchronized")
    assert st == SyncState(None, "unknown")


def test_a_marker_untouched_for_hours_means_the_exchanges_stopped(tmp_path):
    marker = tmp_path / "synchronized"
    marker.write_bytes(b"")
    mtime = marker.stat().st_mtime
    assert read_sync_state(marker, now_s=mtime + MAX_MARKER_AGE_S - 1).synced is True
    stale = read_sync_state(marker, now_s=mtime + MAX_MARKER_AGE_S + 1)
    assert stale.synced is False and stale.reason == "marker_stale" and stale.source == "timesyncd"


class Script:
    """A ``read`` that yields the given states in order and then repeats the last one."""

    def __init__(self, *states):
        self.states, self.calls = list(states), 0

    def __call__(self):
        st = self.states[min(self.calls, len(self.states) - 1)]
        self.calls += 1
        return st


class Mono:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now


UNSYNCED = SyncState(False, "timesyncd", None, "no_marker")
SYNCED = SyncState(True, "timesyncd", 1.0)
STALE = SyncState(False, "timesyncd", 4 * 3600.0, "marker_stale")


def watcher(events, *states, mono=None):
    return ClockStateWatcher(
        lambda kind, **f: events.append((kind, f)), read=Script(*states), mono=mono or Mono()
    )


def test_start_records_the_initial_state_before_anything_else():
    events = []
    watcher(events, UNSYNCED).start()
    assert events == [
        (
            "CLOCK_STATE",
            {
                "initial": True,
                "synced": False,
                "source": "timesyncd",
                "marker_age_s": None,
                "reason": "no_marker",
            },
        )
    ]


def test_an_unsynchronised_start_is_followed_by_one_event_when_the_sync_arrives_with_its_duration():
    events, mono = [], Mono()
    w = watcher(events, UNSYNCED, UNSYNCED, UNSYNCED, SYNCED, SYNCED, mono=mono)
    w.start()
    for _ in range(2):  # still unsynced: nothing new is written
        mono.now += 1.0
        w.poll_once()
    assert len(events) == 1
    mono.now += 33.4
    w.poll_once()  # the marker appeared
    mono.now += 30.0
    w.poll_once()  # synced and unchanged: still nothing new
    assert [e[1]["synced"] for e in events] == [False, True]
    assert events[1][1]["initial"] is False and events[1][1]["unsynced_s"] == 35.4
    assert events[1][1]["marker_age_s"] == 1.0


def test_a_stale_marker_reopens_an_unsynced_interval_and_the_next_sync_closes_it():
    events, mono = [], Mono()
    w = watcher(events, SYNCED, STALE, STALE, SYNCED, mono=mono)
    w.start()
    mono.now += 5.0
    w.poll_once()
    assert events[1][1]["synced"] is False and events[1][1]["reason"] == "marker_stale"
    assert "unsynced_s" not in events[1][1]
    mono.now += 60.0
    w.poll_once()  # still stale
    mono.now += 10.0
    w.poll_once()
    assert [e[1]["synced"] for e in events] == [True, False, True]
    assert events[2][1]["unsynced_s"] == 70.0


def test_an_unknown_state_is_recorded_once_and_never_flaps():
    events = []
    w = watcher(events, SyncState(None, "unknown"))
    w.start()
    for _ in range(5):
        w.poll_once()
    assert len(events) == 1 and events[0][1]["synced"] is None


def test_the_background_loop_polls_until_stopped_and_records_the_transition():
    events = []
    w = ClockStateWatcher(
        lambda kind, **f: events.append((kind, f)),
        read=Script(UNSYNCED, UNSYNCED, SYNCED),
        unsynced_poll_s=0.01,
        synced_poll_s=0.01,
    )

    async def main():
        stop = asyncio.Event()
        w.start()
        task = asyncio.create_task(w.run(stop))
        for _ in range(300):
            if len(events) >= 2:
                break
            await asyncio.sleep(0.01)
        stop.set()
        await asyncio.wait_for(task, 5)

    asyncio.run(main())
    assert [e[1]["synced"] for e in events] == [False, True]
