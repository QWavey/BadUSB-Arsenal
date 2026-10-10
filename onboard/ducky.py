#!/usr/bin/env python3
"""
Tiny DuckyScript runner that writes HID boot-keyboard reports to /dev/hidg0.
Only runs after the gadget has been rebound with hid.usb0 symlinked into the
config. Supports STRING, DELAY, DEFAULTDELAY, ENTER, TAB, ESC, BACKSPACE, GUI,
CTRL, ALT, SHIFT combos, arrows, and F1-F12.
"""
import json
import os
import sys
import time
from pathlib import Path

HID = "/dev/hidg0"
ROOT = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))

NAMED = {
    "ENTER": (0, 0x28), "RETURN": (0, 0x28),
    "ESC": (0, 0x29), "ESCAPE": (0, 0x29),
    "TAB": (0, 0x2B), "SPACE": (0, 0x2C),
    "BACKSPACE": (0, 0x2A), "DELETE": (0, 0x4C), "DEL": (0, 0x4C),
    "UP": (0, 0x52), "DOWN": (0, 0x51), "LEFT": (0, 0x50), "RIGHT": (0, 0x4F),
    "HOME": (0, 0x4A), "END": (0, 0x4D),
    "PAGEUP": (0, 0x4B), "PAGEDOWN": (0, 0x4E), "INSERT": (0, 0x49),
    "PRINTSCREEN": (0, 0x46), "CAPSLOCK": (0, 0x39),
}
for i in range(1, 13):
    NAMED[f"F{i}"] = (0, 0x39 + i)

MOD = {"CTRL": 0x01, "CONTROL": 0x01, "SHIFT": 0x02, "ALT": 0x04,
       "GUI": 0x08, "WINDOWS": 0x08, "WIN": 0x08, "SUPER": 0x08, "COMMAND": 0x08}


def layout(name):
    p = ROOT / "layouts" / f"{name}.json"
    if not p.exists():
        p = ROOT / "layouts" / "us.json"
    if not p.exists():
        return {}
    return json.loads(p.read_text())


def press(fd, mod, k1, k2=0):
    rep = bytes([mod, 0, k1, k2, 0, 0, 0, 0])
    os.write(fd, rep)
    os.write(fd, b"\x00" * 8)


def key_of(name, lay):
    if name in NAMED:
        return NAMED[name]
    if len(name) == 1 and name in lay:
        v = lay[name]
        return (v[0] if len(v) > 0 else 0, v[1] if len(v) > 1 else 0)
    return (0, 0)


def main(path):
    lines = Path(path).read_text().splitlines()
    cfg_layout = "us"
    cfg_path = ROOT / "config.txt"
    if cfg_path.exists():
        for ln in cfg_path.read_text().splitlines():
            if ln.strip().startswith("HID_LAYOUT="):
                cfg_layout = ln.strip().split("=", 1)[1].split("#", 1)[0].strip() or "us"
    lay = layout(cfg_layout)
    try:
        fd = os.open(HID, os.O_RDWR)
    except OSError as e:
        sys.stderr.write(f"open {HID}: {e}\n")
        return 1

    def _ms(txt, default=0):
        try: return int(txt)
        except (TypeError, ValueError): return default

    default_delay = 0
    last_executed = None   # for REPEAT
    inter_key_ms = 2       # tiny delay between consecutive HID reports so
                           # a slow or busy host doesn't drop keys

    def _exec_cmd(line):
        """Execute one DuckyScript line. Returns True if it was a real action
        (eligible for REPEAT), False for comments/empty/REPEAT itself."""
        nonlocal default_delay
        if not line or line.startswith("REM") or line.startswith("#"):
            return False
        if line.startswith("DELAY "):
            time.sleep(_ms(line[6:], 0) / 1000.0)
            return True
        if line.startswith("DEFAULTDELAY "):
            default_delay = _ms(line[13:], 0)
            return False
        if line.startswith("STRING "):
            missed = 0
            for ch in line[7:]:
                if ch in lay:
                    m, k = (lay[ch] + [0, 0])[:2]
                    press(fd, m, k)
                    if inter_key_ms: time.sleep(inter_key_ms / 1000.0)
                else:
                    missed += 1
            if missed:
                sys.stderr.write(f"ducky: {missed} char(s) not in layout\n")
            if default_delay: time.sleep(default_delay / 1000.0)
            return True
        mod = 0
        keys = []
        for tok in line.split():
            if tok in MOD:
                mod |= MOD[tok]
            else:
                m, k = key_of(tok, lay)
                mod |= m
                if k: keys.append(k)
        if keys:
            press(fd, mod, keys[0], keys[1] if len(keys) > 1 else 0)
            if default_delay: time.sleep(default_delay / 1000.0)
            return True
        return False

    for raw in lines:
        s = raw.strip()
        if s.startswith("REPEAT "):
            n = _ms(s[7:], 0)
            if last_executed:
                for _ in range(n):
                    _exec_cmd(last_executed)
            continue
        if _exec_cmd(s):
            last_executed = s
    os.close(fd)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: ducky.py <path>")
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
