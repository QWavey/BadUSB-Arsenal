#!/usr/bin/env python3
"""
GhostLink sniffer + verdict stream (on-device).

AF_PACKET raw socket on the configured iface. One libpcap file + one verdict
JSONL per rotation window. Prunes aggressively because the stock Luckfox
rootfs has ~30 MB free.

Pruning triggers (all applied on every rotate and on startup):
  - count ring:  keep at most RECORD_RING_FILES / VERDICT_RING_FILES
  - age:         delete files older than RECORD_MAX_AGE_H hours
  - disk-free:   while free /root < RECORD_MIN_FREE_MB, delete oldest
  - config-off:  RECORD_PCAP=FALSE wipes ALL existing .pcap files
                 VERDICT_STREAM=FALSE wipes ALL existing .jsonl files

The watcher thread re-reads config.txt and runs the disk guard every 1.5 s, so
the user toggling RECORD_PCAP via the Web UI takes effect without restarting
the firmware. Verdict and pcap writes are buffered; the same thread flushes them.
"""

import ipaddress
import json
import os
import re
import socket
import stat
import struct
import sys
import threading
import time
from pathlib import Path

ROOT      = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))
CFG_PATH  = ROOT / "config.txt"
REC_DIR   = ROOT / "records"
VER_DIR   = ROOT / "verdicts"
LOG_DIR   = ROOT / "logs"
LOG_PATH  = LOG_DIR / "ghostlink.log"

WATCH_SECONDS = 1.5    # watcher thread: config reload, disk guard, flush
OUT_BUF = 64 * 1024    # buffer for the pcap and verdict files, in bytes

# Disk-pressure thresholds (MB free on /root).
# Overridable via config: WARN_FREE_MB / CRIT_FREE_MB / PANIC_FREE_MB.
# WARN  — start proactively pruning oldest records + log tails.
# CRIT  — nuke ALL records and verdicts now, log loud warning, pause.
# PANIC — stop ALL writes (even verdicts). Protect the OS.
WARN_FREE_MB  = 15
CRIT_FREE_MB  = 8
PANIC_FREE_MB = 5

# Independent log-rotation cap. Even without disk pressure, don't let any
# single log file grow past this.
LOG_MAX_KB = 256

# Rotate verdicts if a single jsonl file exceeds this. Important because
# a chatty Windows host can push verdicts to MBs quickly.
VERDICT_MAX_KB = 512


# -------- config --------------------------------------------------------------

def load_cfg(p: Path) -> dict:
    cfg = {}
    if not p.exists():
        return cfg
    for ln in p.read_text().splitlines():
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        if "=" not in s:
            continue
        k, v = s.split("=", 1)
        # strip inline comment only when preceded by whitespace, so values
        # containing a '#' (e.g. passwords) are preserved
        import re as _re
        _m = _re.search(r"\s#", v)
        if _m: v = v[:_m.start()]
        cfg[k.strip()] = v.strip()
    return cfg


def truthy(v) -> bool:
    return str(v).upper() in ("TRUE", "YES", "ON", "1")


# -------- pruning helpers -----------------------------------------------------

def free_mb(path: str = "/root") -> int:
    try:
        st = os.statvfs(path)
        return int(st.f_bavail * st.f_bsize / 1024 / 1024)
    except Exception:
        # statvfs() failed (permission denied, /root unmounted, etc.). Assume
        # emergency rather than plenty of space — prevents silent data loss if
        # filesystem becomes inaccessible. Return value < PANIC_FREE_MB (5).
        return 2


def free_mb_or_none(path: str = "/root"):
    """Free MB on the filesystem, or None when it cannot be read right now.
    Callers keep their last decision on None; they must not treat it as a level."""
    try:
        st = os.statvfs(path)
        return int(st.f_bavail * st.f_bsize / 1024 / 1024)
    except Exception:
        return None


def trim_log(p: Path, keep_kb: int = LOG_MAX_KB):
    """Keep only the last keep_kb KB of a log file, rewritten in place. The daemons append to
    these files through shell redirections; a replaced file would leave them writing to the old,
    deleted copy, and their output would vanish."""
    try:
        if not p.exists(): return
        sz = p.stat().st_size
        if sz < keep_kb * 1024 * 2: return  # headroom before trim
        with open(p, "r+b") as f:
            f.seek(max(0, sz - keep_kb * 1024))
            tail = f.read()
            # drop partial first line
            nl = tail.find(b"\n")
            if nl >= 0: tail = tail[nl+1:]
            f.seek(0)
            f.write(tail)
            f.truncate()
    except Exception:
        pass


def pressure_level() -> int:
    f = free_mb()
    if f < PANIC_FREE_MB: return 2  # PANIC
    if f < CRIT_FREE_MB:  return 2  # treat crit as panic for writes
    if f < WARN_FREE_MB:  return 1
    return 0


# Shared state so stats + log can report it
PRESSURE_STATE = {"level": 0, "free_mb": 100, "last_change": 0.0}


def nuke(d: Path, pattern: str):
    for p in d.glob(pattern):
        try: p.unlink()
        except Exception: pass


# A device with no clock writes 1970 timestamps. Retention therefore orders capture files by
# their number, and only ages out files whose mtime is a real date.
SANE_MTIME = 1577836800   # 2020-01-01


_WRITE_ERRORS = {"n": 0}


def _log_write_error(msg: str):
    """A failed record write repeats for every frame while the disk is full. Log the first one,
    then one in a thousand, and nothing during PANIC (the writes are paused then)."""
    if PRESSURE_STATE["level"] >= 2: return
    _WRITE_ERRORS["n"] += 1
    if _WRITE_ERRORS["n"] != 1 and _WRITE_ERRORS["n"] % 1000: return
    try:
        with open(LOG_PATH, "a") as lf:
            lf.write(f"{time.strftime('%H:%M:%S')} sniff {msg}\n")
    except Exception: pass


def _rec_index(p: Path) -> int:
    """Capture number from rec_NNNNNN.*: the order files were made in."""
    try: return int(p.stem.split("_")[1])
    except (IndexError, ValueError): return -1


# prune_count/prune_age/prune_disk used to live here. They sorted purely by mtime, which the
# SANE_MTIME guard above exists specifically to distrust (a device with no clock writes 1970
# timestamps for every file, so mtime-only ordering can delete newest-first instead of
# oldest-first), and prune_age had no SANE_MTIME check at all -- a clockless device would see
# every file as decades old and eligible for deletion on its first run. They were replaced by
# Writer._prune/_ring_age below, which is index-ordered and SANE_MTIME-guarded, and are gone
# now: confirmed unreachable from anywhere in onboard/ or tests/ (bug-hunt, 2026-10-31) --
# dead code sitting next to the real retention path is a landmine for whoever calls it next
# assuming it's still the live implementation.


# -------- rotation ------------------------------------------------------------

PCAP_MAGIC = 0xA1B2C3D4
PCAP_HDR   = struct.pack("<IHHIIII", PCAP_MAGIC, 2, 4, 0, 0, 65535, 1)


class Writer:
    def __init__(self, cfg):
        REC_DIR.mkdir(parents=True, exist_ok=True)
        VER_DIR.mkdir(parents=True, exist_ok=True)

        self._lock      = threading.Lock()  # guards file handle access (main + watcher threads)
        self.cfg_mtime  = 0.0
        self.pcap_f     = None
        self.verd_f     = None
        self.pcap_bytes = 0
        self.frame      = 0
        self.idx        = self._next_idx()
        self._apply(cfg)
        self._rec_target = os.path.realpath(REC_DIR)   # where REC_DIR points: flash or the SD card
        self._prune()
        self._open()

    def _apply(self, cfg: dict):
        def _int(key, default):
            try: return int(cfg.get(key, default))
            except (TypeError, ValueError):
                # bad value in config shouldn't crash the sniffer
                sys.stderr.write(f"sniff: bad int for {key}, using default {default}\n")
                return int(default)
        self.cfg          = cfg
        self.rotate_kb    = _int("RECORD_ROTATE_KB",   1024)
        self.pcap_ring    = _int("RECORD_RING_FILES",     4)
        self.verd_ring    = _int("VERDICT_RING_FILES",   10)
        self.max_age_h    = _int("RECORD_MAX_AGE_H",     24)
        self.min_free_mb  = _int("RECORD_MIN_FREE_MB",   10)
        self.want_pcap    = truthy(cfg.get("RECORD_PCAP", "TRUE"))
        self.want_verd    = truthy(cfg.get("VERDICT_STREAM", "TRUE"))
        # sanity: refuse zero/negative rotation sizes that would spin forever
        if self.rotate_kb < 16:   self.rotate_kb = 1024
        if self.pcap_ring < 1:    self.pcap_ring = 4
        if self.verd_ring < 1:    self.verd_ring = 10
        if self.max_age_h < 1:    self.max_age_h = 24
        if self.min_free_mb < 1:  self.min_free_mb = 10
        # per-session verdict byte counter for size-based rotation
        self.verd_bytes   = 0

    def _next_idx(self) -> int:
        used = []
        for p in list(REC_DIR.glob("rec_*.pcap")) + list(VER_DIR.glob("rec_*.jsonl")):
            try: used.append(int(p.stem.split("_")[1]))
            except Exception: pass
        return (max(used) + 1) if used else 0

    def _open(self):
        if self.want_pcap:
            self.pcap_f = open(REC_DIR / f"rec_{self.idx:06d}.pcap", "wb", buffering=OUT_BUF)
            self.pcap_f.write(PCAP_HDR)
            self.pcap_f.flush()
            self.pcap_bytes = len(PCAP_HDR)
        if self.want_verd:
            self.verd_f = open(VER_DIR / f"rec_{self.idx:06d}.jsonl", "ab", buffering=OUT_BUF)

    def _prune(self):
        # 1. config says off → wipe entirely
        if not self.want_pcap:
            nuke(REC_DIR, "rec_*.pcap")
        if not self.want_verd:
            nuke(VER_DIR, "rec_*.jsonl")
        # 2. ring count + age, in one scan per dir. The old path globbed+stat'd
        #    each dir twice (prune_count, then prune_age); the kept/deleted set
        #    is identical done in a single pass, so scan once and stat once.
        now = time.time()
        max_age_s = self.max_age_h * 3600
        def _ring_age(d, pattern, keep):
            files = []
            for p in d.glob(pattern):
                try: files.append((_rec_index(p), p.stat().st_mtime, p))
                except Exception: pass
            files.sort(key=lambda t: t[0])   # oldest first, by capture number
            drop = len(files) - keep         # oldest `drop` exceed the ring
            open_now = {f.name for f in (self.pcap_f, self.verd_f) if f}
            for i, (_, mtime, p) in enumerate(files):
                if str(p) in open_now: continue  # never delete the file being written
                too_old = mtime > SANE_MTIME and now - mtime > max_age_s
                if i < drop or too_old:
                    try: p.unlink()
                    except Exception: pass
        if self.want_pcap:
            _ring_age(REC_DIR, "rec_*.pcap",  self.pcap_ring)
        if self.want_verd:
            _ring_age(VER_DIR, "rec_*.jsonl", self.verd_ring)
        # 3. disk free (last resort — eats whatever is oldest in BOTH dirs)
        fm = free_mb_or_none()
        if fm is not None and fm < self.min_free_mb:
            # delete oldest across both until free is OK, never the files being written
            open_now = {f.name for f in (self.pcap_f, self.verd_f) if f}
            all_files = sorted(
                [p for p in list(REC_DIR.glob("rec_*.pcap")) + list(VER_DIR.glob("rec_*.jsonl")) if str(p) not in open_now],
                key=_rec_index,
            )
            while all_files:
                fm = free_mb_or_none()
                if fm is None or fm >= self.min_free_mb + 2: break
                try: all_files[0].unlink()
                except Exception: pass
                all_files = all_files[1:]

    def _check_record_target(self):
        """REC_DIR is a symlink that the shell points at the flash or the SD card (RECORD_SD). When
        it moves, rotate, so the new captures are written to the new place."""
        now = os.path.realpath(REC_DIR)
        if now != self._rec_target:
            self._rec_target = now
            self._rotate()

    def _check_config(self):
        try:
            m = CFG_PATH.stat().st_mtime
        except Exception:
            return
        if m == self.cfg_mtime:
            return
        self.cfg_mtime = m
        old_pcap, old_verd = self.want_pcap, self.want_verd
        self._apply(load_cfg(CFG_PATH))

        # pcap flipped off → close current + nuke
        with self._lock:   # the main thread swaps these handles in _rotate
            if old_pcap and not self.want_pcap:
                if self.pcap_f:
                    self.pcap_f.close()
                    self.pcap_f = None
                nuke(REC_DIR, "rec_*.pcap")
                sys.stderr.write("sniff: RECORD_PCAP=FALSE, pcaps wiped\n")
            # pcap flipped on → open fresh
            if not old_pcap and self.want_pcap and not self.pcap_f:
                self.idx = self._next_idx()
                self._open()

            if old_verd and not self.want_verd:
                if self.verd_f:
                    self.verd_f.close()
                    self.verd_f = None
                nuke(VER_DIR, "rec_*.jsonl")
            if not old_verd and self.want_verd and not self.verd_f:
                self.verd_f = open(VER_DIR / f"rec_{self.idx:06d}.jsonl", "ab", buffering=OUT_BUF)

    def write(self, ts_s: int, ts_us: int, frame_bytes: bytes, verdict: dict):
        self.frame += 1
        # Config reload and the disk guard run on the watcher thread, not per frame.

        # PANIC: refuse to write anything that touches the FS until free recovers
        if PRESSURE_STATE["level"] >= 2:
            return

        caplen = len(frame_bytes)

        # Capture file-handle refs under the GIL — the watcher thread may null
        # them between our check and the write. Using the local ref makes the
        # worst case a write to a closed handle (ValueError) which we swallow.
        with self._lock: vf = self.verd_f
        if self.want_verd and vf is not None and verdict.get("rule"):
            try:
                verdict.update(f=self.frame, t=f"{ts_s}.{ts_us:06d}", sz=caplen)
                line = (json.dumps(verdict, separators=(",", ":")) + "\n").encode()
                vf.write(line)
                self.verd_bytes += len(line)
                if self.verd_bytes >= VERDICT_MAX_KB * 1024:
                    self._rotate()
            except (ValueError, OSError) as e:
                _log_write_error(f"verdict write failed: {e}")

        with self._lock: pf = self.pcap_f
        if self.want_pcap and pf is not None:
            try:
                rec = struct.pack("<IIII", ts_s, ts_us, caplen, caplen)
                pf.write(rec)
                pf.write(frame_bytes)
                self.pcap_bytes += 16 + caplen
                if self.pcap_bytes >= self.rotate_kb * 1024:
                    self._rotate()
            except (ValueError, OSError) as e:
                _log_write_error(f"pcap write failed: {e}")

    def _enforce_disk(self):
        """Called every ~1.5s. Decide pressure level and act."""
        f = free_mb_or_none()
        if f is None:
            return  # unreadable right now: keep the current level and delete nothing
        PRESSURE_STATE["free_mb"] = f
        prev = PRESSURE_STATE["level"]
        if   f < CRIT_FREE_MB:  level = 2
        elif f < WARN_FREE_MB:  level = 1
        else:                   level = 0

        if level != prev:
            PRESSURE_STATE["level"] = level
            PRESSURE_STATE["last_change"] = time.time()
            try:
                with open(LOG_PATH, "a") as lf:
                    lf.write(f"{time.strftime('%H:%M:%S')} sniff "
                             f"PRESSURE level {prev} -> {level} (free={f}MB)\n")
            except Exception: pass

        if level == 2:
            # EMERGENCY: close everything, nuke all records + verdicts, trim logs.
            with self._lock:
                if self.pcap_f: self.pcap_f.close(); self.pcap_f = None
                if self.verd_f: self.verd_f.close(); self.verd_f = None
            nuke(REC_DIR, "rec_*.pcap")
            nuke(VER_DIR, "rec_*.jsonl")
            for lp in LOG_DIR.glob("*.log"):
                trim_log(lp, 64)
            self.verd_bytes = 0
            # keep writer stopped; on next recovery we'll re-open
            return

        if level == 1:
            # WARN: proactive prune — oldest files first.
            self._prune()
            for lp in LOG_DIR.glob("*.log"):
                trim_log(lp, LOG_MAX_KB)
            return

        # level 0: if files were closed during emergency, reopen whichever
        # stream is still enabled. Previously only triggered when want_pcap,
        # so verdict-only configs stayed dead after a disk crisis.
        need_pcap_reopen = self.want_pcap and self.pcap_f is None
        need_verd_reopen = self.want_verd and self.verd_f is None
        if (need_pcap_reopen or need_verd_reopen) and f > WARN_FREE_MB + 3:
            with self._lock:
                self.idx = self._next_idx()
                self._open()
            self.verd_bytes = 0

    def _rotate(self):
        # The handles are swapped under the lock: the watcher thread closes them too.
        with self._lock:
            if self.pcap_f:
                self.pcap_f.close()
                self.pcap_f = None
            if self.verd_f:
                self.verd_f.close()
                self.verd_f = None
            self.idx += 1
            self.pcap_bytes = 0
            self.verd_bytes = 0
            self._open()
        self._prune()
        # also trim logs on every rotation (cheap)
        for lp in LOG_DIR.glob("*.log"):
            trim_log(lp, LOG_MAX_KB)

    def flush(self):
        """Push buffered lines to flash. The watcher calls this every 1.5 s,
        so the Live tab lags by at most that long."""
        for fh in (self.pcap_f, self.verd_f):
            if fh is None:
                continue
            try:
                fh.flush()
            except (ValueError, OSError):
                pass

    def close(self):
        if self.pcap_f: self.pcap_f.close()
        if self.verd_f: self.verd_f.close()


# -------- cheap protocol parse -----------------------------------------------

_IPV4_ADDRS = struct.Struct(">II")  # source and destination address, IPv4 offset 26
_PORTS = struct.Struct(">HH")       # source and destination port after the IPv4 header


def _flow_id(src: int, dst: int, sp: int, dp: int) -> str:
    h = src ^ dst ^ (sp << 16) ^ dp
    return f"{h & 0xFFFFFFFF:08x}"


def flow_hash(buf: bytes) -> str:
    if len(buf) < 34:
        return "00000000"
    etype = (buf[12] << 8) | buf[13]
    if etype != 0x0800:
        return "00000000"
    src, dst = _IPV4_ADDRS.unpack_from(buf, 26)
    proto = buf[23]
    ihl = (buf[14] & 0x0F) * 4
    sp = dp = 0
    if (proto == 6 or proto == 17) and len(buf) >= 14 + ihl + 4:
        sp, dp = _PORTS.unpack_from(buf, 14 + ihl)
    return _flow_id(src, dst, sp, dp)


def parse_dns_qname(buf: bytes, off: int) -> str:
    name, pos = [], off
    while pos < len(buf) and buf[pos]:
        n = buf[pos]
        if n & 0xC0:
            return ".".join(name) + "?"
        pos += 1
        if pos + n > len(buf):
            return ".".join(name) + "!"
        name.append(buf[pos:pos + n].decode("ascii", "replace"))
        pos += n
    return ".".join(name)


_CRED_RE_CACHE = {}


def _re(pat, flags=0):
    k = (pat, flags)
    r = _CRED_RE_CACHE.get(k)
    if r is None:
        import re as _r
        r = _r.compile(pat, flags)
        _CRED_RE_CACHE[k] = r
    return r


def _redact(s: str, keep: int = 2) -> str:
    """Return "ab****" so logs show the shape of a secret without leaking it."""
    if not s: return ""
    if len(s) <= keep: return "*" * len(s)
    return s[:keep] + "*" * min(6, len(s) - keep)


def sniff_creds(payload: bytes, dport: int, sport: int) -> dict:
    """Look inside a TCP payload for leaked credentials in cleartext.

    Returns {} if nothing found, otherwise a dict with:
      kind:  HTTP_BASIC | HTTP_FORM | FTP_USER | FTP_PASS | TELNET_LOGIN
      host:  the Host: header if we parsed one, else ''
      value: redacted snippet so the verdict is actionable but not a
             logging landmine

    Keep this cheap — runs on the hot path for every packet with a payload.
    """
    if not payload: return {}

    # HTTP: look for the Host + Authorization or form password=.
    # Trigger only if first line looks HTTP-ish OR if dport/sport is 80/8080.
    is_http = dport in (80, 8080) or sport in (80, 8080) or payload[:4] in (
        b"GET ", b"POST", b"PUT ", b"HEAD", b"DELE", b"OPTI", b"PATC") or \
        payload[:7].startswith(b"HTTP/1.")
    if is_http and len(payload) >= 16:
        try:
            text = payload.decode("latin-1", "replace")
        except Exception:
            text = ""
        host = ""
        m = _re(r"(?mi)^host:\s*([^\r\n]+)").search(text)
        if m: host = m.group(1).strip()[:120]

        # HTTP Basic
        m = _re(r"(?mi)^authorization:\s*basic\s+([A-Za-z0-9+/=]+)").search(text)
        if m:
            try:
                import base64 as _b64
                dec = _b64.b64decode(m.group(1) + "==", validate=False).decode("latin-1", "replace")
                user, _, pw = dec.partition(":")
            except Exception:
                user, pw = "?", "?"
            return {"rule": "CRED", "kind": "HTTP_BASIC",
                    "host": host, "user": user[:40],
                    "value": _redact(pw)}

        # HTTP form-encoded password / login / credential field
        m = _re(r"(?i)(?:^|[&?])(password|passwd|pwd|pass|login|user|username|email)=([^&\r\n ]{1,256})").search(text)
        if m:
            return {"rule": "CRED", "kind": "HTTP_FORM",
                    "host": host, "field": m.group(1).lower(),
                    "value": _redact(m.group(2))}

        # HTTP request without credential: still capture the host
        if host:
            return {"rule": "HTTP", "host": host}

    # FTP cleartext
    if dport == 21 or sport == 21:
        try:
            text = payload.decode("latin-1", "replace").strip()
        except Exception:
            text = ""
        m = _re(r"(?i)^USER\s+(\S+)").match(text)
        if m: return {"rule": "CRED", "kind": "FTP_USER", "user": m.group(1)[:60]}
        m = _re(r"(?i)^PASS\s+(\S+)").match(text)
        if m: return {"rule": "CRED", "kind": "FTP_PASS", "value": _redact(m.group(1))}

    # Telnet is just raw bytes — look for "login:" / "password:" prompts
    # followed by typed text. Very noisy, keep bar high.
    if dport == 23 or sport == 23:
        try:
            text = payload.decode("latin-1", "replace")
        except Exception:
            text = ""
        if "password:" in text.lower() or "login:" in text.lower():
            return {"rule": "CRED", "kind": "TELNET_LOGIN"}

    return {}


_HOST_OK = re.compile(r"^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+$")


def _sni_from_clienthello(payload: bytes) -> str:
    """Server name (SNI) from a TLS ClientHello at the start of payload, or ''."""
    try:
        # Record: type 0x16 (handshake). Handshake: type 0x01 (ClientHello), 3-byte length.
        if len(payload) < 9 or payload[0] != 0x16 or payload[5] != 0x01:
            return ""
        pos = 9 + 2 + 32                                   # client version, random
        pos += 1 + payload[pos]                            # session id
        pos += 2 + int.from_bytes(payload[pos:pos + 2], "big")   # cipher suites
        pos += 1 + payload[pos]                            # compression methods
        end = min(len(payload), pos + 2 + int.from_bytes(payload[pos:pos + 2], "big"))
        pos += 2                                           # extensions block
        while pos + 4 <= end:
            etype = int.from_bytes(payload[pos:pos + 2], "big")
            elen = int.from_bytes(payload[pos + 2:pos + 4], "big")
            body = pos + 4
            if etype == 0 and elen >= 5:                   # server_name
                # list length (2), name type (1, 0 = host_name), name length (2), name
                nlen = int.from_bytes(payload[body + 3:body + 5], "big")
                if body + 5 + nlen > len(payload):          # name cut off by the packet end
                    return ""
                host = payload[body + 5:body + 5 + nlen].decode("ascii").lower()
                return host if _HOST_OK.match(host) else ""
            pos = body + elen
    except (IndexError, ValueError, UnicodeDecodeError):
        pass
    return ""


def _extract_hostname(payload: bytes, dport: int, sport: int) -> str:
    """Hostname from an HTTP Host header (port 80) or a TLS ClientHello SNI (port 443)."""
    if dport == 443:
        return _sni_from_clienthello(payload)
    try:
        # HTTP: look for Host header in request line
        if dport == 80 or sport == 80:
            try:
                text = payload.decode("latin-1")
                # Look for Host header (case-insensitive, may have spaces)
                m = re.search(r"(?i)\nhost:\s*([^\r\n:]+)", text)
                if m:
                    host = m.group(1).strip().lower().split(":")[0]   # host without the port
                    if host and host != "localhost" and _HOST_OK.match(host):
                        return host
            except Exception:
                pass
    except Exception:
        pass
    return ""


# -------- hostnames seen in traffic -------------------------------------------
# Every hostname read from HTTP Host or TLS SNI is remembered here. The WebUI
# reads SEEN_FILE (in /run, RAM only) to show the list and to auto-add names.
SEEN_FILE = "/run/ghostlink-seen.json"
SEEN_MAX = 500
SEEN = {}                 # host -> [count, first_ts, last_ts]
_seen_flushed = 0.0
_seen_dirty = False       # SEEN changed since the last write


def flush_seen():
    global _seen_flushed, _seen_dirty
    _seen_flushed = time.time()
    _seen_dirty = False
    try:
        data = [{"host": h, "count": r[0], "first": int(r[1]), "last": int(r[2])} for h, r in SEEN.items()]
        tmp = SEEN_FILE + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f)
        os.replace(tmp, SEEN_FILE)
    except OSError:
        pass


def note_seen(host):
    global _seen_dirty
    _seen_dirty = True
    now = time.time()
    rec = SEEN.get(host)
    if rec is None:
        if len(SEEN) >= SEEN_MAX:
            del SEEN[min(SEEN, key=lambda h: SEEN[h][2])]   # forget the least recent name
        SEEN[host] = [1, now, now]
    else:
        rec[0] += 1
        rec[2] = now
    if now - _seen_flushed >= 5:
        flush_seen()


# The device's own names (the admin UI). Requests for these are the admin's
# traffic, not a victim's, so they are never recorded.
ADMIN_HOSTS_FIXED = frozenset(("172.32.0.93", "ghostlink.local", "localhost", "127.0.0.1"))

# Mirrors webui.py's admin_hosts()/lan_admin_ips(): once the LAN detector verifies a real
# LAN IP, the admin's own requests arrive over THAT address, not just 172.32.0.93. Without
# this, every admin page load/API poll over the LAN IP (today's NET_MODE=NAT default) gets
# recorded as victim traffic in the Live Monitor and pcap/verdict files, mixed in with what
# was actually captured from the PC. Same validation as webui.py's _read_lan_ip(): only a
# regular, non-symlink, root-owned file with no group/other write bits is trusted, so a
# planted file under the world-writable /tmp cannot add an attacker's address to this set.
LAN_DETECT_PATH = "/tmp/ghostlink.lan.json"
_admin_hosts_cache = (ADMIN_HOSTS_FIXED, 0.0)


def admin_hosts():
    global _admin_hosts_cache
    hosts, read_at = _admin_hosts_cache
    now = time.time()
    if 0 <= now - read_at < 5.0:
        return hosts
    ip = None
    try:
        fd = os.open(LAN_DETECT_PATH, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(fd, "rb") as f:
            st = os.fstat(f.fileno())
            if (stat.S_ISREG(st.st_mode)
                    and not (os.name == "posix" and (st.st_uid != 0 or (st.st_mode & 0o022)))):
                data = json.loads(f.read(4096).decode("utf-8"))
                if isinstance(data, dict) and data.get("detected") is True:
                    cand = data.get("suggested_ip")
                    if isinstance(cand, str):
                        addr = ipaddress.IPv4Address(cand)
                        if not (addr.is_unspecified or addr.is_loopback or addr.is_multicast
                                or addr.is_reserved or addr.is_link_local):
                            ip = str(addr)
    except Exception:
        pass
    hosts = ADMIN_HOSTS_FIXED | ({ip} if ip and ip not in ADMIN_HOSTS_FIXED else set())
    _admin_hosts_cache = (hosts, now)
    return hosts


def classify(buf: bytes, redirect_from_set) -> dict:
    # Parse the Ethernet/IPv4 header once. The flow id and the protocol
    # dispatch below share src/dst/proto/ihl/ports instead of each re-reading
    # (and flow_hash() re-parsing) the same header bytes on every frame.
    n = len(buf)
    if n < 34 or ((buf[12] << 8) | buf[13]) != 0x0800:
        return {"flow": "00000000"}
    ihl    = (buf[14] & 0x0F) * 4
    if ihl < 20:                              # an IPv4 header is at least 20 bytes
        return {"flow": "00000000"}
    proto  = buf[23]
    # A non-first fragment carries no L4 header, so its bytes must not be read as ports.
    frag_off = ((buf[20] << 8) | buf[21]) & 0x1FFF
    src, dst = _IPV4_ADDRS.unpack_from(buf, 26)
    l4_off = 14 + ihl
    sp = dp = 0
    if (proto == 6 or proto == 17) and frag_off == 0 and n >= l4_off + 4:
        sp, dp = _PORTS.unpack_from(buf, l4_off)
    v = {"flow": _flow_id(src, dst, sp, dp)}
    if n < 42 or frag_off: return v
    if proto == 17 and n >= l4_off + 8:
        if dp == 53 or sp == 53:
            dns_off = l4_off + 8
            if n >= dns_off + 12:
                q = parse_dns_qname(buf, dns_off + 12)
                v["rule"] = "DNS"
                v["host"] = q
                low = q.lower()
                for pat in redirect_from_set:
                    p = pat.lower()
                    if low == p or low.endswith("." + p):
                        v["rule"]    = "REDIRECT"
                        v["verdict"] = "HIT"
                        v["detail"]  = p
                        break
        return v
    if proto == 6 and n >= l4_off + 20:    # TCP
        tcp_off = l4_off
        tcp_hdr = ((buf[tcp_off + 12] >> 4) & 0x0F) * 4
        if tcp_hdr < 20: return v   # a TCP header is at least 20 bytes; less is not a TCP header
        pay_off = tcp_off + tcp_hdr
        # Responses from port 80 never carry a Host header or a request line,
        # so skip them before any parsing. Most bytes of a download are these.
        if sp == 80: return v
        if pay_off < n:
            payload = buf[pay_off:]
            # Extract hostname from HTTP/HTTPS for better verdict display
            hostname = _extract_hostname(payload, dp, sp)
            if len(hostname) > 253: hostname = ""   # longer than any DNS name can be
            # The admin UI's own requests to this device are not victim traffic.
            if hostname in admin_hosts(): return v
            if hostname: note_seen(hostname)
            if hostname and "host" not in v:
                v["host"] = hostname
            cred = sniff_creds(payload, dp, sp)
            if cred:
                v.update(cred)
    return v


# -------- stats sidecar -------------------------------------------------------
# Writes /tmp/ghostlink.stats.json every 2s so the WebUI can poll it.

STATS_PATH = Path("/tmp/ghostlink.stats.json")


_RATE = {"t": None, "frames": 0, "matches": 0, "pps": 0.0, "mps": 0.0}


def dump_stats(frame_count: int, match_count: int, start: float, iface: str):
    # The seen-hosts list is written here too, so the last names reach the file
    # even when no new name follows them.
    if _seen_dirty and time.time() - _seen_flushed >= 2:
        flush_seen()
    try:
        now = time.time()
        # Rates are measured since the previous stats write. Measuring from process start would
        # make a long quiet spell leave the figure near zero for a long time afterwards.
        if _RATE["t"] is None:
            _RATE.update(t=now, frames=frame_count, matches=match_count)
        elif now - _RATE["t"] >= 1.0:
            span = now - _RATE["t"]
            _RATE.update(pps=round((frame_count - _RATE["frames"]) / span, 2),
                         mps=round((match_count - _RATE["matches"]) / span, 2),
                         t=now, frames=frame_count, matches=match_count)
        rx_b = tx_b = rx_p = tx_p = 0
        p = Path(f"/sys/class/net/{iface}/statistics")
        if p.exists():
            try: rx_b = int((p / "rx_bytes").read_text())
            except Exception: pass
            try: tx_b = int((p / "tx_bytes").read_text())
            except Exception: pass
            try: rx_p = int((p / "rx_packets").read_text())
            except Exception: pass
            try: tx_p = int((p / "tx_packets").read_text())
            except Exception: pass
        data = {
            "ts":      int(now),
            "iface":   iface,
            "frames":  frame_count,
            "matches": match_count,
            "pps":     _RATE["pps"],
            "mps":     _RATE["mps"],
            "if_rx_b": rx_b, "if_tx_b": tx_b,
            "if_rx_p": rx_p, "if_tx_p": tx_p,
            "free_mb":    PRESSURE_STATE["free_mb"],
            "warn_level": PRESSURE_STATE["level"],
            "warn_text":  ("ok", "pruning (low disk)", "EMERGENCY: writes paused")[PRESSURE_STATE["level"]],
            "warn_free_mb":  WARN_FREE_MB,
            "crit_free_mb":  CRIT_FREE_MB,
        }
        # Atomic write: webui polls this file ~every 2 s and json.loads would
        # fail on a half-written file. Write to .tmp, rename over the target.
        tmp = STATS_PATH.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(data))
        tmp.replace(STATS_PATH)
    except Exception:
        pass


# -------- main ----------------------------------------------------------------

def _ifindex(name):
    """Kernel index of a network interface, or None while it does not exist."""
    try:
        with open(f"/sys/class/net/{name}/ifindex") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def _open_capture(iface, log):
    """Raw AF_PACKET socket bound to iface. The 2 s timeout lets the loop do its periodic work on
    a quiet link. None if the bind fails; the error is logged."""
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
    try:
        s.bind((iface, 0))
    except OSError as e:
        s.close()
        log(f"bind {iface}: {e}")
        return None
    s.settimeout(2.0)
    return s


def main():
    cfg       = load_cfg(CFG_PATH)
    iface     = cfg.get("SNIFF_IFACE", "usb0").strip()
    red_on    = truthy(cfg.get("REDIRECT", "FALSE"))
    red_from  = []
    if red_on and cfg.get("REDIRECT_FROM"):
        red_from = [p.strip().lower() for p in cfg["REDIRECT_FROM"].split(",") if p.strip()]

    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    # No long-lived append handle: trim_log replaces the inode, and an open
    # fd would keep the old (deleted) inode alive so trim never frees space.
    # Reopen per write — syscall cost is trivial at this log rate.
    def L(m):
        line = f"{time.strftime('%H:%M:%S')} sniff {m}\n"
        try:
            with open(LOG_PATH, "a") as f: f.write(line)
        except Exception: pass
        # Not echoed to stderr: ghostlink.sh points stderr at this same log file,
        # so the line would be written twice.

    s = _open_capture(iface, L)
    if s is None:
        L("aborting"); return
    bound_idx = _ifindex(iface)   # to notice when usb0 is removed or recreated

    L(f"attached iface={iface} redirect_from={red_from}")
    w = Writer(cfg)
    start = time.time()
    match_count = 0
    last_stats = 0.0

    # Even when the network is quiet, keep the config poll + disk guard alive.
    def _watcher():
        ticks = 0
        while True:
            try:
                w._check_config()
                w._check_record_target()
                w._enforce_disk()
                w.flush()
                ticks += 1
                if ticks % 40 == 0:   # about once a minute: the logs also grow on a quiet link
                    for lp in LOG_DIR.glob("*.log"): trim_log(lp, LOG_MAX_KB)
            except Exception: pass
            time.sleep(WATCH_SECONDS)
    threading.Thread(target=_watcher, daemon=True).start()

    cfg_mt = 0.0
    cfg_check_count = 0
    # Hot path: bind time.time/int to locals so each frame is a LOAD_FAST
    # instead of a global + builtin dict lookup on every packet.
    _time = time.time
    _int  = int
    try:
        while True:
            try:
                buf, _ = s.recvfrom(65535)
            except Exception as e:
                # A timeout is a quiet link. Any other error (ENETDOWN while usb0 is down) takes the
                # same path, so the stats and the reattach check keep running either way.
                if not isinstance(e, socket.timeout):
                    L(f"recv err {e}"); time.sleep(0.1)
                now = _time()
                if now - last_stats > 2:
                    dump_stats(w.frame, match_count, start, iface)
                    last_stats = now
                cur_idx = _ifindex(iface)
                if cur_idx != bound_idx:
                    ns = _open_capture(iface, L) if cur_idx is not None else None
                    if cur_idx is None or ns is not None:
                        bound_idx = cur_idx
                    if ns is not None:
                        s.close(); s = ns
                        L(f"reattached iface={iface} ifindex={cur_idx}")
                continue
            now = _time()
            ts_s = _int(now); ts_us = _int((now - ts_s) * 1_000_000)
            # pick up live config changes for REDIRECT_FROM, rate-limited
            # (used to stat() on every packet — 50+ syscalls/sec wasted)
            cfg_check_count += 1
            if cfg_check_count % 20 == 0:
                try:
                    m = CFG_PATH.stat().st_mtime
                    if m != cfg_mt:
                        cfg_mt = m
                        cur = load_cfg(CFG_PATH)
                        if truthy(cur.get("REDIRECT", "FALSE")) and cur.get("REDIRECT_FROM"):
                            red_from = [p.strip().lower() for p in cur["REDIRECT_FROM"].split(",") if p.strip()]
                        else:
                            red_from = []
                        L(f"REDIRECT_FROM reloaded: {red_from}")
                except Exception: pass
            try:
                v = classify(buf, red_from)
            except Exception:
                v = {}   # a frame the parser does not handle must not end the sniffer
            if v.get("rule"): match_count += 1
            w.write(ts_s, ts_us, buf, v)
            if now - last_stats > 2:
                dump_stats(w.frame, match_count, start, iface)
                last_stats = now
    except KeyboardInterrupt:
        pass
    finally:
        w.close()
        L("exiting")


if __name__ == "__main__":
    main()
