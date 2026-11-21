#!/usr/bin/env python3
"""
GhostLink DuckyScript runner.

Supports a usable subset of DuckyScript 3.0:
  * DS1 primitives: STRING, DELAY, DEFAULTDELAY, DEFAULTCHARDELAY, REPEAT,
    modifier combos (CTRL/ALT/SHIFT/GUI), named keys (ENTER/TAB/ESC/arrows
    /F1-F12/HOME/END/PAGE*/INSERT/CAPSLOCK/NUMLOCK/SCROLLLOCK/MENU/PAUSE),
    REM / REM_BLOCK / END_REM.
  * DS3 preprocessor:
      - EXTENSION NAME ... END_EXTENSION       in-script extension block
      - DEFINE #CONST value                     literal-substitution const
  * DS3 runtime:
      - VAR $NAME = <expr>                      declare + assign
      - $NAME = <expr>                          reassign
      - IF (<expr>) ... [ELSE] ... END_IF
      - WHILE (<expr>) ... END_WHILE
      - CALLs: a bare NAME line OR NAME() invokes a defined extension
  * Built-in variables ($_CAPSLOCK_ON, $_NUMLOCK_ON, $_SCROLLLOCK_ON,
    $_RANDOM_INT, $_RANDOM_SHORT, $_RANDOM_BYTE, $_RANDOM_LOWERCASE_LETTER,
    $_RANDOM_UPPERCASE_LETTER, $_RANDOM_LETTER, $_RANDOM_NUMBER, $_OS).
    $_CAPSLOCK_ON / $_NUMLOCK_ON / $_SCROLLLOCK_ON are tracked from the
    HID output report the host sends back (LED-set-report), read in a
    background thread off /dev/hidg0.
  * STRING / variable interpolation: STRING hello $USER prints the current
    value of $USER (names strip to [A-Za-z_][A-Za-z0-9_]*).
  * GhostLink extensions: GL_SET, GL_RELOAD, GL_RUN, GL_LOG, GL_WAIT,
    GL_SPOOF_ADD, GL_REBOOT.
  * ATTACKMODE / LED_* / LCD_* / EXFIL are parsed and logged as no-ops
    (this is a Luckfox WebBee, not a Rubber Ducky — gadget rebind at
    runtime bricks the device, see docs/HID.md).

Hak5 extension library ships under /root/ghostlink/extensions/hak5/.
"""
import json
import os
import random
import re
import string
import struct
import subprocess
import sys
import threading
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


# ============================================================================
#  Low-level utilities
# ============================================================================

def parse_entry(v):
    """Return (mod, keycode) from a layout entry (Hak5 "XX,YY,ZZ" or legacy [m,k])."""
    if isinstance(v, str):
        parts = [p.strip() for p in v.split(",")]
        if len(parts) >= 3:
            try: return int(parts[0], 16), int(parts[2], 16)
            except ValueError: return 0, 0
        return 0, 0
    if isinstance(v, (list, tuple)) and len(v) >= 2:
        try: return int(v[0]), int(v[1])
        except (ValueError, TypeError): return 0, 0
    return 0, 0


def load_layout(name):
    p = ROOT / "layouts" / f"{name}.json"
    if not p.exists(): p = ROOT / "layouts" / "us.json"
    if not p.exists(): return {}
    try:
        raw = json.loads(p.read_text())
    except Exception:
        return {}
    out = {}
    for k, v in raw.items():
        if k.startswith("__"): continue
        m, kc = parse_entry(v)
        if m == 0 and kc == 0 and not isinstance(v, (list, str)): continue
        if not (0 <= m <= 255 and 0 <= kc <= 255):
            _cfg_log(f"warning: layout {name}: {k}={v!r} outside 0-255, skipped"); continue
        out[k] = (m, kc)
    return out


def _cfg_log(msg):
    try:
        logp = ROOT / "logs" / "ghostlink.log"
        logp.parent.mkdir(parents=True, exist_ok=True)
        with open(logp, "a") as f:
            f.write(f"{time.strftime('%H:%M:%S')} ducky {msg}\n")
    except Exception: pass


_PRESS_FAIL_LOGGED = False
def press(fd, mod, k1, k2=0, dwell_ms: int = 3):
    """Send a boot-keyboard press then release report. A short dwell between
    the two reports is required — some hosts (notably Windows at 125 Hz USB
    poll) will coalesce back-to-back reports and miss the key, which looked
    on the German layout like the comma "hanging" (the . → , transition
    appeared to drop keys)."""
    global _PRESS_FAIL_LOGGED
    try: rep = bytes([mod, 0, k1, k2, 0, 0, 0, 0])
    except ValueError: _cfg_log(f"press() key out of range mod={mod} k1={k1} k2={k2}"); return
    try:
        os.write(fd, rep)
        if dwell_ms: time.sleep(dwell_ms / 1000.0)
        os.write(fd, b"\x00" * 8)
    except OSError as e:
        # Log the first write-failure per process so a silent "payload
        # ran but nothing typed" (no /dev/hidg0, HID not enabled, etc.)
        # shows up in ghostlink.log instead of appearing successful.
        if not _PRESS_FAIL_LOGGED:
            _PRESS_FAIL_LOGGED = True
            _cfg_log(f"press() OSError: {e} — HID likely disabled or /dev/hidg0 missing")


def key_of(name, lay):
    up = name.upper()
    if up in NAMED: return (0, NAMED[up])
    if name in lay: return lay[name]
    if up in lay:   return lay[up]
    return (0, 0)


def _tl_emit(kind, **fields):
    """Append an Attack Timeline event from ducky.py. Writes to the same
    jsonl file webui.py uses (/root/ghostlink/logs/timeline.jsonl) so
    both producers converge into one feed."""
    try:
        tl = ROOT / "logs" / "timeline.jsonl"
        tl.parent.mkdir(parents=True, exist_ok=True)
        rec = {"t": int(time.time()), "ts": time.strftime("%H:%M:%S"), "kind": kind}
        rec.update(fields)
        if "cmd" in rec: rec["cmd"] = str(rec["cmd"])[:40]   # cap the cmd field
        # same 2MB cap behavior as webui's _timeline
        if tl.exists() and tl.stat().st_size > 2 * 1024 * 1024:
            try:
                kept = tl.read_bytes()[-512*1024:]
                nl = kept.find(b"\n")   # first newline: everything before it is a cut-off record
                tl.write_bytes(kept[nl + 1:] if nl >= 0 else b"")   # file starts at a record boundary
            except Exception: pass
        import json as _json
        with open(tl, "a") as f:
            f.write(_json.dumps(rec) + "\n")   # whole JSON line: never sliced mid-record
    except Exception: pass


# Limits that stop a runaway script. A payload that loops forever types forever, and the
# device has no other way to stop it short of a reboot.
MAX_STEPS = 100_000   # statements executed in one run
MAX_DEPTH = 8         # payloads started by payloads (RUN_PAYLOAD, GL_RUN), in a chain


class _Budget(BaseException):
    """The step limit was reached. BaseException, so the line handlers do not swallow it."""


class _BadScript(Exception):
    """The script cannot run as written (unclosed block, runaway expansion)."""


def _child_env():
    """Environment for a payload started by a payload: one level deeper."""
    env = dict(os.environ)
    env["GL_DUCKY_DEPTH"] = str(int(os.environ.get("GL_DUCKY_DEPTH", "0") or 0) + 1)
    return env


def _ms(txt, default=0):
    try: return min(int(float(str(txt).strip())), 3_600_000)   # an hour at most: DELAY, REPEAT
    except (TypeError, ValueError, OverflowError): return default   # OverflowError: int(float("inf"))


# ============================================================================
#  GhostLink extensions (GL_)
# ============================================================================

def gl_set(key_value: str):
    key, _, val = key_value.partition("=")
    key = key.strip(); val = val.strip()
    if not key: return False
    cfg_path = ROOT / "config.txt"
    try: txt = cfg_path.read_text() if cfg_path.exists() else ""
    except Exception: txt = ""
    pat = re.compile(r"^" + re.escape(key) + r"=.*$", re.MULTILINE)
    new_line = f"{key}={val}"
    if pat.search(txt): txt = pat.sub(lambda _m: new_line, txt)   # function replacement: backslashes in val stay literal
    else:
        if txt and not txt.endswith("\n"): txt += "\n"
        txt += new_line + "\n"
    tmp = cfg_path.with_suffix(".new")
    try:
        tmp.write_text(txt); tmp.replace(cfg_path)
        _cfg_log(f"GL_SET {key}={val[:40]}")
        return True
    except Exception as e:
        _cfg_log(f"GL_SET fail {e}"); return False


def gl_reload_dnsmasq():
    try:
        r = subprocess.run(["sh", str(ROOT/"ghostlink.sh"), "reload-dnsmasq"],
                           timeout=5, capture_output=True)
        if r.returncode != 0:
            _cfg_log(f"GL_RELOAD_DNSMASQ rc={r.returncode}"); return False
        _cfg_log("GL_RELOAD_DNSMASQ"); return True
    except Exception as e:
        _cfg_log(f"GL_RELOAD_DNSMASQ fail {e}"); return False


def gl_run_script(name: str):
    name = name.strip()
    if not re.match(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\.(sh|dd|txt|ducky)$", name):
        _cfg_log(f"GL_RUN bad name '{name}'"); return False
    p = ROOT / "payloads" / name
    if not p.exists():
        _cfg_log(f"GL_RUN not found '{name}'"); return False
    try:
        if name.endswith(".sh"): subprocess.Popen(["sh", str(p)])
        else:                     subprocess.Popen(["python3", str(ROOT/"ducky.py"), str(p)], env=_child_env())
        _cfg_log(f"GL_RUN {name}"); return True
    except Exception as e:
        _cfg_log(f"GL_RUN fail {e}"); return False


# ============================================================================
#  HID LED feedback (host → device) for $_CAPSLOCK_ON etc.
# ============================================================================
# Writing to /dev/hidg0 sends input reports (device → host). Reading from it
# gives us output reports — the host writing LED state back. We background-
# thread that read so built-in LED variables always have a fresh value.

HID_LED_STATE = {"NUMLOCK": False, "CAPSLOCK": False, "SCROLLLOCK": False}
_LED_THREAD_STARTED = False
_LED_LOCK = threading.Lock()


def _led_reader(fd):
    while True:
        try:
            data = os.read(fd, 1)
        except OSError:
            return
        if not data: return
        b = data[0]
        with _LED_LOCK:
            HID_LED_STATE["NUMLOCK"]    = bool(b & 0x01)
            HID_LED_STATE["CAPSLOCK"]   = bool(b & 0x02)
            HID_LED_STATE["SCROLLLOCK"] = bool(b & 0x04)


def _ensure_led_reader(fd):
    global _LED_THREAD_STARTED
    with _LED_LOCK:
        if _LED_THREAD_STARTED: return
        _LED_THREAD_STARTED = True
    t = threading.Thread(target=_led_reader, args=(fd,), daemon=True)
    t.start()


# ============================================================================
#  Expression evaluator — safe subset
# ============================================================================
# Transforms a DuckyScript expression into a Python expression, substitutes
# vars and defines, then evaluates in a locked-down namespace. Supports:
#   int, string literal "..." or '...', TRUE/FALSE
#   + - * / % & | ^ ~ << >>
#   == != < > <= >=
#   && || !
#   parens

_EXPR_TRANSLATE = [
    (r"\bTRUE\b",  "True"),
    (r"\bFALSE\b", "False"),
    (r"&&",        " and "),
    (r"\|\|",      " or "),
    (r"!(?!=)",    " not "),
]
_SAFE_EVAL_NAMES = {"__builtins__": {}, "True": True, "False": False}
_NOT_BUILTIN = object()   # _builtin() result for a name that is not a generated built-in

# Quoted string literals, "..." or '...', with backslash escapes. What is inside
# them is data, so the _EXPR_TRANSLATE rules must not rewrite it.
_EXPR_STRING = re.compile(r"""("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')""")


def _translate_expr(src):
    """Apply _EXPR_TRANSLATE to the unquoted pieces of src only. Quoted pieces
    are joined back unchanged, so "TRUE" or "a && b" keep their exact text."""
    parts = _EXPR_STRING.split(src)      # even indices unquoted, odd indices quoted
    for i in range(0, len(parts), 2):
        for pat, rep in _EXPR_TRANSLATE:
            parts[i] = re.sub(pat, rep, parts[i])
    return "".join(parts)


def _coerce(x):
    """Convert a lexed token to Python value — int if it looks numeric,
    else string stripped of quotes if quoted, else bare word kept as str."""
    if isinstance(x, (int, float, bool)): return x
    s = str(x).strip()
    if s.upper() == "TRUE":  return True
    if s.upper() == "FALSE": return False
    try: return int(s)
    except ValueError: pass
    if len(s) >= 2 and ((s[0] == '"' and s[-1] == '"') or (s[0] == "'" and s[-1] == "'")):
        return s[1:-1]
    return s


class Interp:
    def __init__(self, path, stop_event=None):
        self.path = Path(path)
        self.stop_event = stop_event
        self.vars = {}         # $NAME -> value
        self.defines = {}      # #NAME -> literal str
        self.extensions = {}   # NAME -> list of body lines (uppercase name)
        self.fd = None
        self.lay = {}
        self.default_delay = 0
        self.char_delay_ms = 8      # was 2 — too aggressive, caused host-side
                                    # key drops on some layouts (German . → ,)
        self.press_dwell_ms = 3     # time key is "held" before release report
        self.in_rem_block = False
        # built-ins that are random each access (regenerated per read)
        self._rand = random.Random()

    # -------- preprocessing (file includes + extension defs + defines) ------
    def _load_lines(self, path):
        try: return Path(path).read_text().splitlines()
        except Exception as e:
            _cfg_log(f"load '{path}' fail {e}"); return []

    def _preprocess(self):
        """Walk the top-level script and nested extensions/*.ducky files.
        Collect EXTENSION ... END_EXTENSION blocks and DEFINE constants,
        and strip REM_BLOCK/END_REM. Return the executable body lines."""
        body = []
        self._absorb(self._load_lines(self.path), body, depth=0)
        return body

    def _absorb(self, lines, out, depth=0):
        if depth > 32:
            _cfg_log("extension include depth cap hit"); return
        i = 0
        while i < len(lines):
            # A self-referencing EXTENSION doubles the output at every level, so bound the size.
            if len(out) > MAX_STEPS: raise _BadScript("extensions expand past the step limit")
            raw = lines[i]; i += 1
            s = raw.strip()
            u = s.upper()

            # multi-line REM_BLOCK filter
            if u == "REM_BLOCK" or u.startswith("REM_BLOCK "):
                while i < len(lines) and lines[i].strip().upper() != "END_REM":
                    i += 1
                i += 1
                continue
            if u == "END_REM":
                continue

            # Single-line `EXTENSION NAME` — could be a block definition or a
            # reference. Distinguish by peeking ahead: if we hit END_EXTENSION
            # before another EXTENSION line (or EOF), it's a definition.
            m = re.match(r"^\s*EXTENSION\s+([A-Za-z_][A-Za-z0-9_.-]*)\s*$", s)
            if m:
                nm = m.group(1)
                is_def = False
                j = i
                while j < len(lines):
                    t = lines[j].strip().upper()
                    if t == "END_EXTENSION":
                        is_def = True; break
                    if re.match(r"^EXTENSION\s+[A-Za-z_]", t):
                        break
                    j += 1
                if is_def:
                    # block definition: capture until END_EXTENSION, register
                    # under NAME, and ALSO inline at the definition point so
                    # the body runs where it was written (Hak5 semantics).
                    name = nm.upper()
                    buf = []
                    while i < len(lines):
                        t = lines[i].strip().upper()
                        if t == "END_EXTENSION":
                            i += 1; break
                        buf.append(lines[i]); i += 1
                    nested = []
                    self._absorb(buf, nested, depth + 1)
                    self.extensions[name] = nested
                    for ln2 in nested: out.append(ln2)
                    continue
                # reference: already registered → inline the body
                if nm.upper() in self.extensions:
                    for ln2 in self.extensions[nm.upper()]: out.append(ln2)
                    continue
                # file-based lookup — case-insensitive since Hak5 ships
                # lowercase filenames but docs use uppercase names
                found = False
                for sub in ("", "hak5/"):
                    for cand in (nm, nm.lower(), nm.upper()):
                        for ext in (".ducky", ".txt"):
                            p = ROOT / "extensions" / f"{sub}{cand}{ext}"
                            if p.exists():
                                self._absorb(self._load_lines(p), out, depth + 1)
                                found = True; break
                        if found: break
                    if found: break
                if not found:
                    _cfg_log(f"EXTENSION '{nm}' not found")
                continue

            # DEFINE #NAME value  (constant literal substitution)
            m = re.match(r"^\s*DEFINE\s+#?([A-Za-z_][A-Za-z0-9_]*)\s+(.+)$", s)
            if m:
                self.defines[m.group(1).upper()] = m.group(2).strip()
                continue

            out.append(raw)

    # -------- expression evaluation --------------------------------------
    def _builtin(self, name):
        """Value of a generated built-in ($_CAPSLOCK_ON, $_RANDOM_*, $_OS) as a
        Python value, or _NOT_BUILTIN for any other name. _subst_vars and
        _subst_string both resolve built-ins here, so the two cannot drift."""
        if name == "_CAPSLOCK_ON":
            with _LED_LOCK: return HID_LED_STATE["CAPSLOCK"]
        if name == "_NUMLOCK_ON":
            with _LED_LOCK: return HID_LED_STATE["NUMLOCK"]
        if name == "_SCROLLLOCK_ON":
            with _LED_LOCK: return HID_LED_STATE["SCROLLLOCK"]
        if name == "_RANDOM_INT":              return self._rand.randint(0, 65535)
        if name == "_RANDOM_SHORT":            return self._rand.randint(0, 65535)
        if name == "_RANDOM_BYTE":             return self._rand.randint(0, 255)
        if name == "_RANDOM_NUMBER":           return self._rand.randint(0, 9)
        if name == "_RANDOM_LOWERCASE_LETTER": return self._rand.choice(string.ascii_lowercase)
        if name == "_RANDOM_UPPERCASE_LETTER": return self._rand.choice(string.ascii_uppercase)
        if name == "_RANDOM_LETTER":           return self._rand.choice(string.ascii_letters)
        if name == "_OS":                      return self.vars.get("_OS", "UNKNOWN")
        return _NOT_BUILTIN

    def _subst_vars(self, text):
        """Replace $NAME tokens with their values (int or quoted string).
        Supports $_RANDOM_* generators."""
        def repl(m):
            name = m.group(1).upper()
            v = self._builtin(name)
            if v is _NOT_BUILTIN:
                v = self.vars.get(name)
                if v is None: return "0"
            if isinstance(v, bool): return "True" if v else "False"
            if isinstance(v, (int, float)): return str(v)
            return repr(str(v))
        return re.sub(r"\$([A-Za-z_][A-Za-z0-9_]*)", repl, text)

    def _subst_defines(self, text):
        """Replace #NAME with the defined literal."""
        def repl(m):
            k = m.group(1).upper()
            return self.defines.get(k, m.group(0))
        return re.sub(r"#([A-Za-z_][A-Za-z0-9_]*)", repl, text)

    def _eval(self, expr: str):
        src = self._subst_defines(expr)
        src = self._subst_vars(src)
        src = _translate_expr(src)
        try:
            return eval(src, _SAFE_EVAL_NAMES, {})
        except Exception as e:
            _cfg_log(f"expr fail: {expr!r} -> {src!r} ({e})")
            return 0

    def _subst_string(self, text):
        """STRING $var substitution — same as _subst_vars but return raw
        values (not Python-literal reprs) so the typed text matches what the
        user wrote."""
        def repl(m):
            name = m.group(1).upper()
            v = self._builtin(name)
            if v is _NOT_BUILTIN:
                v = self.vars.get(name)
                if v is None: return ""
            return str(v)
        return re.sub(r"\$([A-Za-z_][A-Za-z0-9_]*)", repl, text)

    # -------- primitive HID actions --------------------------------------
    def _press(self, mod, k1, k2=0):   press(self.fd, mod, k1, k2, self.press_dwell_ms)
    def _press_mod_only(self, mod):    press(self.fd, mod, 0, 0, self.press_dwell_ms)

    def _type_string(self, s):
        s = self._subst_string(s)
        miss = 0
        for ch in s:
            m, k = key_of(ch, self.lay)
            if k:
                self._press(m, k)
                if self.char_delay_ms: time.sleep(self.char_delay_ms / 1000.0)
            else:
                miss += 1
        if miss: sys.stderr.write(f"ducky: {miss} char(s) not in layout\n")
        if self.default_delay: time.sleep(self.default_delay / 1000.0)

    # -------- interpret a single line -----------------------------------
    def _run_line(self, line):
        if not line: return
        line = line.lstrip()   # leading blanks go, so keyword slices (line[7:] etc.) start at column 0; trailing blanks stay for STRING
        s = line.strip()
        if not s: return
        u = s.upper()

        # REM / # comments
        if u == "REM" or u.startswith("REM "): return
        if s.startswith("#"): return

        # Block comments are stripped in preprocess; defensive check anyway
        if u == "REM_BLOCK" or u.startswith("REM_BLOCK "):
            self.in_rem_block = True; return
        if self.in_rem_block:
            if u == "END_REM": self.in_rem_block = False
            return

        # ATTACKMODE / LED_* / LCD_* / EXFIL are advisory on this device
        if u.startswith(("ATTACKMODE ", "LED_", "LCD_", "EXFIL ", "RESTART_PAYLOAD ",
                         "RESET ", "DISABLE_BUTTON", "ENABLE_BUTTON",
                         "WAIT_FOR_HOST_LOCK_STATE_CHANGE")) \
           or u in ("END", "FINISH", "PAYLOAD_FINISHED", "ATTACKMODE", "RESET",
                    "RESTART_PAYLOAD",
                    "SAVE_HOST_KEYBOARD_LOCK_STATE",
                    "RESTORE_HOST_KEYBOARD_LOCK_STATE"):
            _cfg_log(f"advisory: {s[:80]}")
            return

        # ====================================================================
        # ESP-S3-Key-BadUSB parity — commands from QWavey/ESP-S3-Key-BadUSB's
        # DuckyInterpreter.cpp. The ones that need hardware we don't have
        # (WiFi/BT radio, SD slot, MSC/thumbdrive mode, LED array) are
        # logged as advisory + emitted to the Attack Timeline so the user
        # can see the script's intent. The rest fire for real.
        # ====================================================================

        # ---- Strings with per-flavor timing nudges --------------------------
        if u.startswith("STRING_BASH "):
            # Bash tends to accept full speed; keep the default.
            self._type_string(line[12:]); self._press(0, NAMED["ENTER"]); return
        if u.startswith("STRING_POWERSHELL "):
            # PowerShell's history-highlighter can swallow fast input — bump
            # the dwell for just this one line, then restore.
            saved = self.press_dwell_ms
            self.press_dwell_ms = max(6, saved)
            self._type_string(line[18:]); self._press(0, NAMED["ENTER"])
            self.press_dwell_ms = saved
            return

        # ---- Raw HID: KEYCODE <mod> <k1> [k2] -------------------------------
        if u.startswith("KEYCODE "):
            toks = s.split()[1:]
            try: parts = [int(t, 0) & 0xFF for t in toks]
            except ValueError: _cfg_log(f"KEYCODE bad int: {s[:60]}"); return
            if len(parts) == 1:   self._press(0, parts[0])
            elif len(parts) == 2: self._press(parts[0], parts[1])
            else:                 self._press(parts[0], parts[1], parts[2])
            return

        # ---- INJECT_MOD <mod> <key> — explicit modifier+key -----------------
        if u.startswith("INJECT_MOD "):
            toks = s.split()[1:]
            mod_bits = 0; keys_bits = []
            for t in toks:
                if t.upper() in MOD: mod_bits |= MOD[t.upper()]
                else:
                    _, k = key_of(t, self.lay)
                    if k: keys_bits.append(k)
            if keys_bits: self._press(mod_bits, keys_bits[0])
            elif mod_bits: self._press_mod_only(mod_bits)
            return

        # ---- HOLD / RELEASE / STOPHOLD --------------------------------------
        if u.startswith("HOLD ") and not u.startswith("HOLD_TILL"):
            # Press without release. _held tracks the sticky modifier bits
            # and keycodes so RELEASE can send a matching 0-report.
            if not hasattr(self, "_held"):
                self._held = {"mod": 0, "keys": []}
            toks = s.split()[1:]
            for t in toks:
                up_t = t.upper()
                if up_t in MOD: self._held["mod"] |= MOD[up_t]
                else:
                    _, k = key_of(t, self.lay)
                    if k and len(self._held["keys"]) < 6:
                        self._held["keys"].append(k)
            rep = bytes([self._held["mod"], 0] + self._held["keys"]
                        + [0] * (6 - len(self._held["keys"])))
            try: os.write(self.fd, rep)
            except OSError: pass
            return
        if u in ("RELEASE", "STOPHOLD") or u.startswith("RELEASE "):
            try: os.write(self.fd, b"\x00" * 8)
            except OSError: pass
            self._held = {"mod": 0, "keys": []}
            return

        # ---- CONSUME <key> — press+release once -----------------------------
        if u == "CONSUME" or u.startswith("CONSUME "):
            if u == "CONSUME": return
            toks = s.split()[1:]
            for t in toks:
                m2, k = key_of(t, self.lay)
                if k: self._press(m2, k)
            return

        # ---- Boot script control --------------------------------------------
        if u.startswith("SET_BOOT_SCRIPT "):
            name = s[len("SET_BOOT_SCRIPT "):].strip()
            boot = ROOT / "boot_payload"
            try: boot.write_text(name + "\n"); _cfg_log(f"SET_BOOT_SCRIPT {name}")
            except Exception as e: _cfg_log(f"SET_BOOT_SCRIPT fail {e}")
            return
        if u.startswith("SET_BUTTON_PIN "):
            # Map to our BUTTON_DEV instead of a GPIO pin (WebBee uses evdev).
            v = s[len("SET_BUTTON_PIN "):].strip()
            gl_set(f"BUTTON_DEV={v}")
            return

        # ---- RUN_PAYLOAD <name> — chain-run a sibling script ----------------
        if u.startswith("RUN_PAYLOAD "):
            name = s[len("RUN_PAYLOAD "):].strip()
            # Same name whitelist gl_run_script() (GL_RUN) already uses -- this command
            # had none, so "RUN_PAYLOAD ../../../etc/passwd" would resolve outside
            # payloads/ (Path joining does not strip ".."). Same operator trust level as
            # GL_RUN either way (both only run from inside an already-authorized script),
            # but there is no reason for one DuckyScript command to be stricter than the
            # sibling command that does the same thing.
            if not re.match(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\.(sh|dd|txt|ducky)$", name):
                _cfg_log(f"RUN_PAYLOAD bad name: {name!r}"); return
            p = ROOT / "payloads" / name
            if p.exists() and p.is_file():
                import subprocess as _sp
                _sp.Popen(["python3", str(ROOT/"ducky.py"), str(p)], env=_child_env())
            else:
                _cfg_log(f"RUN_PAYLOAD not found: {name}")
            return

        # ---- IMPORT <file> — include another script inline ------------------
        if u.startswith("IMPORT "):
            name = s[len("IMPORT "):].strip()
            for base in (ROOT/"extensions", ROOT/"payloads"):
                p = base / name
                if p.is_file():
                    try:
                        inner = p.read_text().splitlines()
                        self.exec_body(inner)
                    except Exception as e: _cfg_log(f"IMPORT err: {e}")
                    return
            _cfg_log(f"IMPORT not found: {name}")
            return

        # ---- Random generators — type a random character --------------------
        if u == "RANDOM_LOWERCASE_LETTER":
            import random as _r
            self._type_string(_r.choice("abcdefghijklmnopqrstuvwxyz")); return
        if u == "RANDOM_UPPERCASE_LETTER":
            import random as _r
            self._type_string(_r.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ")); return
        if u == "RANDOM_LETTER":
            import random as _r
            self._type_string(_r.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")); return
        if u == "RANDOM_NUMBER":
            import random as _r
            self._type_string(_r.choice("0123456789")); return
        if u == "RANDOM_SPECIAL":
            import random as _r
            self._type_string(_r.choice("!@#$%^&*()-_=+[]{};:,.<>?/")); return
        if u == "RANDOM_CHAR":
            import random as _r
            self._type_string(_r.choice(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*")); return

        # ---- Time / Day → vars $_TIME / $_DAY -------------------------------
        if u == "GET_TIME":
            self.vars["_TIME"] = time.strftime("%H:%M:%S"); return
        if u == "GET_DAY":
            self.vars["_DAY"]  = time.strftime("%A"); return

        # ---- Internet / mode toggles ----------------------------------------
        if u == "JOIN_INTERNET":
            gl_set("NET_MODE=FORWARD"); _cfg_log("JOIN_INTERNET -> NET_MODE=FORWARD"); return
        if u == "LEAVE_INTERNET":
            gl_set("NET_MODE=PASSIVE"); _cfg_log("LEAVE_INTERNET -> NET_MODE=PASSIVE"); return

        # ---- IF_ONLINE / IF_OFFLINE -----------------------------------------
        if u in ("IF_ONLINE", "IF_OFFLINE"):
            # Flag the condition. The IF/END_IF collector in exec_body
            # evaluates $_ONLINE if the user writes it; here we just set it.
            import socket as _s
            try:
                s2 = _s.socket(_s.AF_INET, _s.SOCK_STREAM); s2.settimeout(1.5)
                s2.connect(("1.1.1.1", 53)); s2.close()
                self.vars["_ONLINE"] = "1"
            except Exception:
                self.vars["_ONLINE"] = "0"
            return

        # ---- IF_OS <linux|windows|macos|android|ios> → sets $_OS_MATCH ------
        if u.startswith("IF_OS "):
            want = s[6:].strip().strip('"').lower()
            # WebBee itself runs linux; this is only useful when the admin
            # has set $_OS externally. We expose both.
            cur = (self.vars.get("_OS") or "linux").lower()
            self.vars["_OS_MATCH"] = "1" if cur == want else "0"
            return

        # ---- System actions -------------------------------------------------
        if u == "REBOOT":
            _cfg_log("REBOOT requested"); os.system("sync; reboot &"); return
        if u == "SHUTDOWN":
            _cfg_log("SHUTDOWN requested"); os.system("sync; poweroff &"); return
        if u in ("FACTORYRESET", "FACTORY_RESET"):
            _cfg_log("FACTORY_RESET requested (restoring default config.txt)")
            try: (ROOT/"config.txt").unlink()
            except Exception: pass
            os.system("sync; reboot &"); return
        if u.startswith(("SELFDESTRUCT", "SELF_DESTRUCT")):
            _cfg_log("SELF_DESTRUCT advisory — not actually bricking the device")
            return

        # ---- Advisory family: WIFI_*, BT_*, LED_*, SD_*, STORAGE_*, HID_* ---
        # All logged + emitted to Attack Timeline for intent-tracking.
        ADVISORY_PREFIXES = (
            "WIFI_", "BLUETOOTH_", "BT_", "BLE_", "SD_", "STORAGE_", "LCD_",
            "RGB ", "LED ", "BLINK_LED_", "BLINK_STOP",
            "HID_ATTACH", "HID_DETACH", "USB_ATTACHED", "USB_DETACHED",
            "USB_MOUNTED", "USB_UNMOUNTED", "USB_CONNECTED", "USB_DISCONNECTED",
            "UPLOAD_FILE ", "DOWNLOAD_FILE ", "COPY_FILE ", "CUT_FILE ",
            "PASTE_FILE", "USE_FILE ", "CD ",
            "SAVE_CREDENTIALS",
            "HTTP_REQUEST", "HTTPS_REQUEST",
            "PING ", "WAIT_FOR_SD", "WAIT_FOR_EOF", "WAIT_FOR_EVENT",
            "RUN_AT_TIME", "RUN_AT_DAY", "RUN_WHEN_WIFI", "RUN_WHEN_BLUETOOTH",
            "BT_FOUND", "RUN_WHEN_BT_FOUND",
            "WIFI_OFF_WHEN_WIFI", "WIFI_ON_WHEN_WIFI",
            "BLUETOOTH_OFF_WHEN_WIFI", "BLUETOOTH_ON_WHEN_WIFI",
            "BLUETOOTH_DISCOVERY",
            "VID_", "PID_", "MAN_", "PRODUCT_",
            "HIDE_PAYLOAD", "REVERT_TO_THUMBDRIVE", "SOFT_BRICK",
            "BEHAVE_BROKEN", "BEHAVEBROKEN",
            "BEGIN_ROWER", "END_ROWER", "RUN_ON_REBOOT", "END_RUN_ON_REBOOT",
            "DETECT_OS", "DAY_TRIGGER", "TIME_TRIGGER", "WIFI_TRIGGER",
            "SD_REMOVAL_TRIGGER",
            "IF_PRESENT ", "IF_NOTPRESENT ", "IF_NOT_PRESENT ",
            "IF_BT_PRESENT", "IF_CLIENT_",
            "WAIT_FOR_SCROLL_CHANGE",
        )
        if u.startswith(ADVISORY_PREFIXES) or u in (
            "WIFI_OFF", "WIFI_ON", "WIFI_CONNECTED", "WIFI_DISCONNECTED",
            "WIFI_JOINING", "BLUETOOTH_OFF", "BLUETOOTH_ON",
            "CLIENT_CONNECTED", "CLIENT_DISCONNECTED",
            "STORAGE",
        ):
            _cfg_log(f"advisory (ESP-S3): {s[:80]}")
            try: _tl_emit("ducky_advisory", cmd=u.split()[0] if u.split() else u, line=s[:120])
            except Exception: pass
            return

        # delays
        if u.startswith("DELAY "):
            time.sleep(max(0, _ms(self._eval(line[6:]), 0)) / 1000.0); return
        if u.startswith("WAIT "):
            time.sleep(max(0, _ms(self._eval(line[5:]), 0))); return
        if u.startswith("DEFAULTDELAY "):
            self.default_delay = _ms(self._eval(line[13:]), 0); return
        if u.startswith("DEFAULT_DELAY "):
            self.default_delay = _ms(self._eval(line[14:]), 0); return
        if u.startswith("DEFAULTCHARDELAY "):
            self.char_delay_ms = _ms(self._eval(line[17:]), 2); return

        # button wait
        if u in ("WAIT_FOR_BUTTON_PRESS", "BUTTON_WAIT"):
            self._wait_for_button(); return

        # STRING
        if u.startswith("STRING "):
            self._type_string(line[7:]); return
        if u.startswith("STRINGLN "):
            self._type_string(line[9:])
            self._press(0, NAMED["ENTER"])
            return

        # GhostLink
        if u.startswith("GL_SET "):            gl_set(self._subst_string(line[7:])); return
        if u in ("GL_RELOAD", "GL_RELOAD_DNSMASQ"): gl_reload_dnsmasq(); return
        if u.startswith("GL_RUN "):            gl_run_script(self._subst_string(line[7:])); return
        if u.startswith("GL_LOG "):            _cfg_log(self._subst_string(line[7:])); return
        if u.startswith("GL_WAIT "):           time.sleep(max(0, _ms(self._eval(line[8:]), 0))); return
        if u == "GL_REBOOT":
            _cfg_log("GL_REBOOT requested"); os.system("sync; reboot &"); return

        # VAR / assignment
        m = re.match(r"^\s*VAR\s+\$([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", s)
        if m:
            self.vars[m.group(1).upper()] = self._eval(m.group(2)); return
        m = re.match(r"^\s*\$([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", s)
        if m:
            self.vars[m.group(1).upper()] = self._eval(m.group(2)); return

        # REPEAT handled by caller (needs last-executed state)
        if u.startswith("REPEAT "):
            return "REPEAT:" + line[7:].strip()

        # Modifier / key combo line
        mod = 0; keys = []; all_mods = True
        toks = s.split()
        unknown = False
        for tok in toks:
            up_t = tok.upper()
            if up_t in MOD: mod |= MOD[up_t]
            else:
                all_mods = False
                m2, k = key_of(tok, self.lay)
                if k: mod |= m2; keys.append(k)
                else: unknown = True
        if keys:
            self._press(mod, keys[0], keys[1] if len(keys) > 1 else 0); return
        if mod and all_mods:
            self._press_mod_only(mod); return

        # Bare NAME / NAME() → extension call
        m = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)?\s*$", s)
        if m:
            name = m.group(1).upper()
            if name in self.extensions:
                self.exec_body(self.extensions[name])
                return
        m = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*$", s)
        if m:
            name = m.group(1).upper()
            if name in self.extensions:
                self.exec_body(self.extensions[name])
                return
            # If it's a single named key (ENTER / F5 / …) it was handled above.
            # Otherwise log as unknown — a loud miss is better than silent eat.
            if unknown:
                _cfg_log(f"unknown line: {s[:80]}")
            return

        if unknown:
            _cfg_log(f"unknown line: {s[:80]}")

    # -------- block executor with IF / WHILE / nested EXTENSION calls -----
    def exec_body(self, lines):
        i = 0
        last_exec = None
        while i < len(lines):
            if self.stop_event is not None and self.stop_event.is_set(): return
            self.steps = getattr(self, "steps", 0) + 1
            if self.steps > MAX_STEPS: raise _Budget()
            raw = lines[i]
            s = raw.strip()
            u = s.upper()

            # IF / WHILE control flow
            m = re.match(r"^\s*IF\s*\((.+)\)\s*$", s, re.IGNORECASE)
            if m:
                cond = bool(self._eval(m.group(1)))
                try: then_block, else_block, consumed = self._collect_if(lines, i + 1)
                except _BadScript as e:
                    _cfg_log(f"line {i+1}: {e}"); sys.stderr.write(f"ducky: line {i+1}: {e}\n"); return
                i += 1 + consumed
                self.exec_body(then_block if cond else else_block)
                continue

            m = re.match(r"^\s*WHILE\s*\((.+)\)\s*$", s, re.IGNORECASE)
            if m:
                try: body, consumed = self._collect_while(lines, i + 1)
                except _BadScript as e:
                    _cfg_log(f"line {i+1}: {e}"); sys.stderr.write(f"ducky: line {i+1}: {e}\n"); return
                i += 1 + consumed
                guard = 50_000
                while guard > 0 and bool(self._eval(m.group(1))):
                    if self.stop_event is not None and self.stop_event.is_set(): return
                    self.exec_body(body)
                    guard -= 1
                continue

            # A malformed IF or WHILE header used to run as a plain line, so its body ran every
            # time. Stop the block instead.
            if re.match(r"^\s*(IF|WHILE|END_IF|END_WHILE)\b", s, re.IGNORECASE):
                _cfg_log(f"line {i+1}: malformed {u.split()[0]} header, stopped")
                sys.stderr.write(f"ducky: line {i+1}: malformed {u.split()[0]} header\n")
                return
            try:
                r = self._run_line(raw)
            except Exception as e:
                # Report with the source line number so a syntax error in
                # a long script says WHERE instead of just dying silently.
                sys.stderr.write(f"ducky: line {i+1}: {type(e).__name__}: {e}\n")
                sys.stderr.write(f"       -> {raw.rstrip()[:120]}\n")
                _cfg_log(f"line {i+1} {type(e).__name__}: {e}")
                return
            i += 1
            if r is None:
                # record repeatable commands for REPEAT N
                if s and not u.startswith(("REM", "VAR ", "DEFINE ", "DEFAULTDELAY",
                                           "DEFAULT_DELAY", "DEFAULTCHARDELAY",
                                           "ATTACKMODE", "GL_")):
                    last_exec = raw
            elif isinstance(r, str) and r.startswith("REPEAT:"):
                n = _ms(self._eval(r[7:]), 0)
                if last_exec:
                    try:
                        for _ in range(n):
                            if self.stop_event is not None and self.stop_event.is_set(): return
                            self._run_line(last_exec)
                    except Exception as e:
                        sys.stderr.write(f"ducky: REPEAT line {i}: {type(e).__name__}: {e}\n")
                        sys.stderr.write(f"       -> {last_exec.rstrip()[:120]}\n")
                        _cfg_log(f"REPEAT line {i} {type(e).__name__}: {e}")
                        return

    def _collect_if(self, lines, start):
        """Return (then_body, else_body, consumed) up to matching END_IF."""
        then_body, else_body = [], []
        cur = then_body
        depth = 1
        i = start
        while i < len(lines):
            s = lines[i].strip().upper()
            if s.startswith("IF ") or s.startswith("IF("):
                depth += 1
                cur.append(lines[i])
            elif s == "END_IF":
                depth -= 1
                if depth == 0:
                    return then_body, else_body, (i - start + 1)
                cur.append(lines[i])
            elif s == "ELSE" and depth == 1:
                cur = else_body
            else:
                cur.append(lines[i])
            i += 1
        raise _BadScript("IF without END_IF")

    def _collect_while(self, lines, start):
        body = []; depth = 1; i = start
        while i < len(lines):
            s = lines[i].strip().upper()
            if s.startswith("WHILE ") or s.startswith("WHILE("):
                depth += 1
                body.append(lines[i])
            elif s == "END_WHILE":
                depth -= 1
                if depth == 0:
                    return body, (i - start + 1)
                body.append(lines[i])
            else:
                body.append(lines[i])
            i += 1
        raise _BadScript("WHILE without END_WHILE")

    def _wait_for_button(self):
        try:
            dev = "/dev/input/event0"
            cfg = ROOT / "config.txt"
            if cfg.exists():
                for ln in cfg.read_text().splitlines():
                    ln = ln.strip()
                    if ln.startswith("BUTTON_DEV="):
                        v = ln.split("=", 1)[1]
                        m = re.search(r"\s#", v)
                        if m: v = v[:m.start()]
                        dev = v.strip() or dev
            fmt = "llHHi"; sz = struct.calcsize(fmt)
            with open(dev, "rb", buffering=0) as f:
                while True:
                    if self.stop_event is not None and self.stop_event.is_set(): return
                    data = os.read(f.fileno(), sz)
                    if len(data) < sz: continue
                    _, _, ty, _, val = struct.unpack(fmt, data)
                    if ty == 1 and val == 1: return
        except Exception as e:
            sys.stderr.write(f"ducky: WAIT_FOR_BUTTON_PRESS err {e}\n")

    def run(self):
        if int(os.environ.get("GL_DUCKY_DEPTH", "0") or 0) > MAX_DEPTH:
            _cfg_log(f"payload chain deeper than {MAX_DEPTH}; not started"); return 1
        # Write PID so button.py can check if a payload is running (avoids ps overhead).
        pidfile = Path("/run/ghostlink-ducky.pid")
        pidfile.write_text(str(os.getpid()))
        try:
            cfg_layout = "us"
            cfg_path = ROOT / "config.txt"
            if cfg_path.exists():
                for ln in cfg_path.read_text().splitlines():
                    s = ln.strip()
                    if s.startswith("HID_LAYOUT="):
                        v = s.split("=", 1)[1]
                        m = re.search(r"\s#", v)
                        if m: v = v[:m.start()]
                        cfg_layout = v.strip() or "us"
            self.lay = load_layout(cfg_layout)

            try:
                self.fd = os.open(HID, os.O_RDWR)
            except OSError as e:
                sys.stderr.write(f"ducky: open {HID}: {e}\n"); return 1
            _ensure_led_reader(self.fd)

            try:
                body = self._preprocess()
                _tl_emit("ducky_start", path=str(getattr(self, "path", "?")).split("/")[-1][:60])
                self.exec_body(body)
            except _BadScript as e:
                _cfg_log(f"script refused: {e}"); sys.stderr.write(f"ducky: {e}\n")
                os.close(self.fd); return 1
            except _Budget:
                _cfg_log(f"step limit {MAX_STEPS} reached; stopped"); sys.stderr.write("ducky: step limit reached\n")
                os.close(self.fd); return 1
            _tl_emit("ducky_done",  path=str(getattr(self, "path", "?")).split("/")[-1][:60])
            os.close(self.fd); return 0
        finally:
            # Remove the pidfile only if it still names this run. A payload started meanwhile
            # writes its own pid, and removing that would reopen the button gate under it.
            try:
                if pidfile.read_text().strip() == str(os.getpid()): pidfile.unlink()
            except Exception: pass


def run(path, stop_event=None):
    """Public entry — compatibility with the old ducky.py API."""
    return Interp(path, stop_event).run()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: ducky.py <path>"); sys.exit(2)
    sys.exit(run(sys.argv[1]))
