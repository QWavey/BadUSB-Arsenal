"""Button logic: tap, double tap and hold, driven by scripted evdev events.

The loop reads fixed-size input events from a file. The test feeds a file of events and
advances a fake monotonic clock to each event's time. The wall clock is patched to a constant,
so the timing must come from the monotonic clock alone.
"""
import importlib.util
import struct
from pathlib import Path

BUTTON = Path(__file__).resolve().parents[1] / "onboard" / "button.py"
EV_SYN = 0
EV_KEY = 1
PRESS, RELEASE, HELD = 1, 0, 2


def _load():
    spec = importlib.util.spec_from_file_location("button_under_test", BUTTON)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _Done(BaseException):
    """Ends the loop once the scripted events run out (BaseException: the loop catches Exception)."""


def _run(tmp_path, monkeypatch, events, cfg):
    """events: (time_s, type, value) tuples. Returns the payload names that were fired."""
    import select as select_mod
    mod = _load()
    dev = tmp_path / "event0"
    dev.write_bytes(b"".join(struct.pack(mod._FMT, 0, 0, ty, 30, val) for _, ty, val in events))
    # Offset the clock: the loop uses 0.0 to mean "no press yet", and a real monotonic clock
    # (time since boot) never reads 0 at a press.
    times = [100.0 + t for t, _, _ in events]
    clock = [0.0]
    fired = []

    def fake_select(rlist, wlist, xlist, timeout=None):
        idx = rlist[0].tell() // mod._SZ
        if idx >= len(times):
            raise _Done()
        clock[0] = times[idx]
        return (rlist, [], [])

    monkeypatch.setattr(mod, "_cfg", lambda: dict(cfg, BUTTON_DEV=str(dev)))
    monkeypatch.setattr(mod, "_fire", lambda name: fired.append(name))
    monkeypatch.setattr(mod, "_log", lambda m: None)
    monkeypatch.setattr(mod.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(mod.time, "time", lambda: 0.0)   # wall clock must not matter
    monkeypatch.setattr(select_mod, "select", fake_select)
    try:
        mod.main()
    except _Done:
        pass
    return fired


TAPS = {"BUTTON_PAYLOAD": "single", "BUTTON_DOUBLE_TAP_PAYLOAD": "double",
        "BUTTON_TAP_WINDOW_MS": "600", "BUTTON_HOLD_MS": "1200", "BUTTON_HOLD_PAYLOAD": "hold"}


def test_two_quick_taps_are_one_double_tap(tmp_path, monkeypatch):
    events = [(0.00, EV_KEY, PRESS), (0.10, EV_KEY, RELEASE),
              (0.30, EV_KEY, PRESS), (0.40, EV_KEY, RELEASE),
              (2.00, EV_SYN, 0)]                       # a later event lets the loop run its checks
    assert _run(tmp_path, monkeypatch, events, TAPS) == ["double"]


def test_one_tap_fires_the_single_after_the_window(tmp_path, monkeypatch):
    events = [(0.00, EV_KEY, PRESS), (0.10, EV_KEY, RELEASE),
              (1.50, EV_SYN, 0), (1.60, EV_SYN, 0)]
    assert _run(tmp_path, monkeypatch, events, TAPS) == ["single"]


def test_a_long_press_fires_the_hold_payload(tmp_path, monkeypatch):
    events = [(0.00, EV_KEY, PRESS), (0.10, EV_KEY, HELD), (1.50, EV_KEY, RELEASE)]
    assert _run(tmp_path, monkeypatch, events, TAPS) == ["hold"]


def test_hold_falls_back_to_the_single_payload_on_autorepeat_too(tmp_path, monkeypatch):
    cfg = dict(TAPS, BUTTON_HOLD_PAYLOAD="")
    events = [(0.00, EV_KEY, PRESS), (1.30, EV_KEY, HELD), (1.40, EV_KEY, RELEASE)]
    assert _run(tmp_path, monkeypatch, events, cfg) == ["single"]


def test_a_tap_before_a_hold_does_not_fire_later(tmp_path, monkeypatch):
    events = [(0.00, EV_KEY, PRESS), (0.10, EV_KEY, RELEASE),     # tap: a single is pending
              (0.20, EV_KEY, PRESS), (1.40, EV_KEY, HELD),       # then a hold
              (1.50, EV_KEY, RELEASE),
              (5.00, EV_SYN, 0), (5.10, EV_SYN, 0)]             # the pending single must not fire
    assert _run(tmp_path, monkeypatch, events, TAPS) == ["hold"]
