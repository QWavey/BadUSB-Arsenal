#!/usr/bin/env python3
"""
GhostLink button daemon.

Watches the on-board button (adc-keys driver on /dev/input/event0) and fires
a configured DuckyScript or shell payload on press. Supports single tap,
double tap, and long press.

Config knobs (config.txt):
  BUTTON_ENABLED=TRUE
  BUTTON_DEV=/dev/input/event0
  BUTTON_PAYLOAD=name.ducky            fired on short single-tap
  BUTTON_DOUBLE_TAP_PAYLOAD=name.ducky fired when two taps happen under
                                       BUTTON_TAP_WINDOW_MS apart
  BUTTON_HOLD_PAYLOAD=name.ducky       fired when the button is held for
                                       >= BUTTON_HOLD_MS (and no payload
                                       is fired on release in that case)
  BUTTON_TAP_WINDOW_MS=600
  BUTTON_HOLD_MS=1200
"""
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))
CFG  = ROOT / "config.txt"
LOG  = ROOT / "logs" / "ghostlink.log"

# struct input_event — see linux/input.h.
# On arm 32-bit uclibc it's { struct timeval, __u16 type, __u16 code, __s32 value }
# which lays out as llHHi (two 'long's for sec/usec, two u16, one s32).
_FMT = "llHHi"
_SZ  = struct.calcsize(_FMT)

EV_KEY = 1


def _log(m):
    try:
        LOG.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG, "a") as f:
            f.write(f"{time.strftime('%H:%M:%S')} button {m}\n")
    except Exception: pass


def _cfg():
    d = {}
    if not CFG.exists(): return d
    try:
        for ln in CFG.read_text().splitlines():
            s = ln.strip()
            if not s or s.startswith("#") or "=" not in s: continue
            k, v = s.split("=", 1)
            import re as _re
            _m = _re.search(r"\s#", v)
            if _m: v = v[:_m.start()]
            d[k.strip()] = v.strip()
    except Exception: pass
    return d


def _truthy(v): return str(v).upper() in ("TRUE", "YES", "ON", "1")


def _resolve_payload(name):
    if not name: return None
    import re
    if not re.match(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\.(ducky|txt|sh|dd)$", name):
        _log(f"bad payload name '{name}'")
        return None
    p = ROOT / "payloads" / name
    return p if p.exists() else None


def _fire(name):
    p = _resolve_payload(name)
    if not p:
        _log(f"payload not found '{name}'")
        return
    try:
        if name.endswith(".sh"):
            subprocess.Popen(["sh", str(p)])
        else:
            subprocess.Popen(["python3", str(ROOT/"ducky.py"), str(p)])
        _log(f"fired {name}")
    except Exception as e:
        _log(f"fire fail {e}")


def main():
    cfg = _cfg()
    if not _truthy(cfg.get("BUTTON_ENABLED", "TRUE")):
        _log("disabled, exiting")
        return
    dev = cfg.get("BUTTON_DEV", "/dev/input/event0")
    try:
        tap_window = int(cfg.get("BUTTON_TAP_WINDOW_MS", "600"))
    except ValueError: tap_window = 600
    try:
        hold_ms = int(cfg.get("BUTTON_HOLD_MS", "1200"))
    except ValueError: hold_ms = 1200

    try:
        f = open(dev, "rb", buffering=0)
    except OSError as e:
        _log(f"open {dev}: {e}")
        return

    _log(f"watching {dev}")

    pending_single = 0.0     # when the last "release" happened; 0 if none pending
    press_time     = 0.0
    hold_fired     = False
    cfg_mtime      = 0.0

    while True:
        # cheap cfg reload — pick up BUTTON_* changes without a restart
        try:
            m = CFG.stat().st_mtime
            if m != cfg_mtime:
                cfg_mtime = m
                cfg = _cfg()
        except Exception: pass

        # if there's a pending single-tap, we have a budget before we fire it
        now = time.time()
        if pending_single > 0 and (now - pending_single) * 1000 >= tap_window:
            _fire(cfg.get("BUTTON_PAYLOAD", ""))
            pending_single = 0.0

        # wait for the next event with a short timeout so we can honor the
        # pending-tap window above
        try:
            import select
            r, _, _ = select.select([f], [], [], 0.1)
            if not r:
                continue
            data = os.read(f.fileno(), _SZ)
            if len(data) < _SZ: continue
            _, _, ty, code, val = struct.unpack(_FMT, data)
        except Exception as e:
            _log(f"read err {e}")
            time.sleep(0.2)
            continue

        if ty != EV_KEY: continue

        if val == 1:     # press
            press_time = time.time()
            hold_fired = False
        elif val == 0:   # release
            if hold_fired: continue
            dur = time.time() - press_time
            # If the release came from a hold that we already fired, skip. If
            # we hadn't fired hold yet but duration exceeds, treat as hold.
            if dur * 1000 >= hold_ms:
                _fire(cfg.get("BUTTON_HOLD_PAYLOAD", "") or cfg.get("BUTTON_PAYLOAD", ""))
                continue
            # short tap — decide between single and double
            if pending_single > 0 and (time.time() - pending_single) * 1000 <= tap_window:
                # second tap within window → double-tap
                dbl = cfg.get("BUTTON_DOUBLE_TAP_PAYLOAD", "")
                if dbl: _fire(dbl)
                else:   _fire(cfg.get("BUTTON_PAYLOAD", ""))
                pending_single = 0.0
            else:
                pending_single = time.time()
        elif val == 2:   # held
            if not hold_fired and (time.time() - press_time) * 1000 >= hold_ms:
                _fire(cfg.get("BUTTON_HOLD_PAYLOAD", ""))
                hold_fired = True


if __name__ == "__main__":
    main()
