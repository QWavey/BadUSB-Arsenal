#!/usr/bin/env python3
"""
GhostLink DuckyScript runner.

Writes HID boot-keyboard reports (8 bytes) to /dev/hidg0.
Supports Hak5-format layout files (XX,YY,ZZ hex triplets) and the legacy
[mod,key] array format. Covers DuckyScript 1.0 commands:
  REM / REM_BLOCK / END_REM, STRING, DELAY, DEFAULTDELAY,
  DEFAULTCHARDELAY, REPEAT, ENTER / RETURN, ESC / ESCAPE,
  TAB, SPACE, BACKSPACE, DELETE / DEL, UP / UPARROW, DOWN /
  DOWNARROW, LEFT / LEFTARROW, RIGHT / RIGHTARROW, HOME, END,
  PAGEUP, PAGEDOWN, INSERT, PRINTSCREEN, SCROLLLOCK, PAUSE /
  BREAK, CAPSLOCK, NUMLOCK, MENU / APP, F1..F12, and the
  modifiers CTRL/CONTROL, SHIFT, ALT, GUI/WINDOWS/WIN/SUPER/COMMAND.
"""
import json
import os
import sys
import time
from pathlib import Path

HID  = "/dev/hidg0"
ROOT = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))

NAMED = {
    "ENTER": 0x28, "RETURN": 0x28,
    "ESC": 0x29, "ESCAPE": 0x29,
    "TAB": 0x2B, "SPACE": 0x2C,
    "BACKSPACE": 0x2A, "DELETE": 0x4C, "DEL": 0x4C,
    "UP": 0x52, "UPARROW": 0x52,
    "DOWN": 0x51, "DOWNARROW": 0x51,
    "LEFT": 0x50, "LEFTARROW": 0x50,
    "RIGHT": 0x4F, "RIGHTARROW": 0x4F,
    "HOME": 0x4A, "END": 0x4D,
    "PAGEUP": 0x4B, "PAGEDOWN": 0x4E,
    "INSERT": 0x49, "PRINTSCREEN": 0x46,
    "SCROLLLOCK": 0x47, "PAUSE": 0x48, "BREAK": 0x48,
    "CAPSLOCK": 0x39, "NUMLOCK": 0x53,
    "MENU": 0x65, "APP": 0x65,
}
for i in range(1, 13):
    NAMED[f"F{i}"] = 0x39 + i

MOD = {
    "CTRL": 0x01, "CONTROL": 0x01,
    "SHIFT": 0x02,
    "ALT": 0x04,
    "GUI": 0x08, "WINDOWS": 0x08, "WIN": 0x08,
    "SUPER": 0x08, "COMMAND": 0x08, "CMD": 0x08,
}


def parse_entry(v):
    """Return (mod, keycode) from a layout entry.

    Supports two formats:
      Hak5:    "02,00,04"   (modifier,reserved,keycode as hex strings)
      legacy:  [2, 4]       (modifier, keycode)
    """
    if isinstance(v, str):
        parts = [p.strip() for p in v.split(",")]
        if len(parts) >= 3:
            try:
                return int(parts[0], 16), int(parts[2], 16)
            except ValueError:
                return 0, 0
        return 0, 0
    if isinstance(v, (list, tuple)) and len(v) >= 2:
        try: return int(v[0]), int(v[1])
        except (ValueError, TypeError): return 0, 0
    return 0, 0


def load_layout(name):
    p = ROOT / "layouts" / f"{name}.json"
    if not p.exists():
        p = ROOT / "layouts" / "us.json"
    if not p.exists():
        return {}
    try:
        raw = json.loads(p.read_text())
    except Exception:
        return {}
    # strip __comment and other non-key keys cleanly — any entry that is
    # not a legacy list or Hak5-style hex string is ignored.
    out = {}
    for k, v in raw.items():
        if k.startswith("__"): continue
        mod, key = parse_entry(v)
        if mod == 0 and key == 0 and not isinstance(v, (list, str)):
            continue
        out[k] = (mod, key)
    return out


def press(fd, mod, k1, k2=0):
    rep = bytes([mod, 0, k1, k2, 0, 0, 0, 0])
    try:
        os.write(fd, rep)
        os.write(fd, b"\x00" * 8)
    except OSError:
        pass


def key_of(name, lay):
    """name is a single char or a named key like ENTER/F5."""
    up = name.upper()
    if up in NAMED:
        return (0, NAMED[up])
    if name in lay:                # exact (case-sensitive) char lookup
        return lay[name]
    if up in lay:                  # some layouts use uppercase keys
        return lay[up]
    return (0, 0)


def _ms(txt, default=0):
    try: return int(str(txt).strip())
    except (TypeError, ValueError): return default


def run(path, stop_event=None):
    """Interpret path. Returns 0 on success, non-zero on open-HID failure."""
    lines = Path(path).read_text().splitlines()

    cfg_layout = "us"
    cfg_path = ROOT / "config.txt"
    if cfg_path.exists():
        for ln in cfg_path.read_text().splitlines():
            s = ln.strip()
            if s.startswith("HID_LAYOUT="):
                cfg_layout = s.split("=", 1)[1].split("#", 1)[0].strip() or "us"
    lay = load_layout(cfg_layout)

    try:
        fd = os.open(HID, os.O_RDWR)
    except OSError as e:
        sys.stderr.write(f"ducky: open {HID}: {e}\n")
        return 1

    default_delay  = 0
    char_delay_ms  = 2       # tiny inter-key gap so a slow host doesn't miss keys
    last_executed  = None
    in_block_rem   = False

    def exec_one(line):
        """Execute a single prepared command line. Returns True on repeatable action."""
        nonlocal default_delay, char_delay_ms
        if not line: return False
        up = line.upper()

        # REM_BLOCK and END_REM handled by caller via in_block_rem
        if up.startswith("REM "):   return False
        if up.startswith("REM"):    return False
        if up.startswith("#"):      return False

        if up.startswith("DEFAULTDELAY "):
            default_delay = _ms(line[13:], 0); return False
        if up.startswith("DEFAULT_DELAY "):
            default_delay = _ms(line[14:], 0); return False
        if up.startswith("DEFAULTCHARDELAY "):
            char_delay_ms = _ms(line[17:], 2); return False
        if up.startswith("DELAY "):
            time.sleep(max(0, _ms(line[6:], 0)) / 1000.0); return True

        if up.startswith("STRING "):
            miss = 0
            for ch in line[7:]:
                m, k = key_of(ch, lay)
                if k:
                    press(fd, m, k)
                    if char_delay_ms: time.sleep(char_delay_ms / 1000.0)
                else:
                    miss += 1
            if miss:
                sys.stderr.write(f"ducky: {miss} char(s) not in layout {cfg_layout}\n")
            if default_delay: time.sleep(default_delay / 1000.0)
            return True

        # modifier + key(s) line
        mod = 0
        keys = []
        for tok in line.split():
            up_t = tok.upper()
            if up_t in MOD:
                mod |= MOD[up_t]
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
        if stop_event is not None and stop_event.is_set():
            break
        s = raw.strip()

        # block comments
        if s.upper() == "REM_BLOCK":
            in_block_rem = True; continue
        if in_block_rem:
            if s.upper() == "END_REM":
                in_block_rem = False
            continue

        if s.upper().startswith("REPEAT "):
            n = _ms(s[7:], 0)
            if last_executed:
                for _ in range(n):
                    if stop_event is not None and stop_event.is_set():
                        break
                    exec_one(last_executed)
            continue

        if exec_one(s):
            last_executed = s

    os.close(fd)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: ducky.py <path>")
        sys.exit(2)
    sys.exit(run(sys.argv[1]))
