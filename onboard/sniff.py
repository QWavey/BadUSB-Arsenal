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

The Writer re-reads config.txt every WATCH_FRAMES frames so the user toggling
RECORD_PCAP via the Web UI takes effect without restarting the firmware.
"""

import json
import os
import socket
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

WATCH_FRAMES = 10      # re-read config.txt every N frames
WATCH_SECONDS = 1.5    # or every N seconds, whichever comes first

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
        cfg[k.strip()] = v.split("#", 1)[0].strip()
    return cfg


def truthy(v) -> bool:
    return str(v).upper() in ("TRUE", "YES", "ON", "1")


# -------- pruning helpers -----------------------------------------------------

def free_mb(path: str = "/root") -> int:
    try:
        st = os.statvfs(path)
        return int(st.f_bavail * st.f_bsize / 1024 / 1024)
    except Exception:
        return 10_000


def trim_log(p: Path, keep_kb: int = LOG_MAX_KB):
    """Keep only the last keep_kb KB of a log file, atomically."""
    try:
        if not p.exists(): return
        sz = p.stat().st_size
        if sz < keep_kb * 1024 * 2: return  # headroom before trim
        with open(p, "rb") as f:
            f.seek(max(0, sz - keep_kb * 1024))
            tail = f.read()
        # drop partial first line
        nl = tail.find(b"\n")
        if nl >= 0: tail = tail[nl+1:]
        tmp = p.with_suffix(p.suffix + ".tmp")
        tmp.write_bytes(tail)
        tmp.replace(p)
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


def prune_count(d: Path, pattern: str, keep: int):
    files = sorted(d.glob(pattern), key=lambda p: p.stat().st_mtime)
    while len(files) > keep:
        try: files[0].unlink()
        except Exception: pass
        files = files[1:]


def prune_age(d: Path, pattern: str, max_age_s: int):
    now = time.time()
    for p in list(d.glob(pattern)):
        try:
            if now - p.stat().st_mtime > max_age_s:
                p.unlink()
        except Exception:
            pass


def prune_disk(d: Path, pattern: str, min_free: int):
    # keep deleting oldest while free is below threshold
    guard = 0
    while free_mb() < min_free and guard < 50:
        files = sorted(d.glob(pattern), key=lambda p: p.stat().st_mtime)
        if not files:
            return
        try: files[0].unlink()
        except Exception: pass
        guard += 1


# -------- rotation ------------------------------------------------------------

PCAP_MAGIC = 0xA1B2C3D4
PCAP_HDR   = struct.pack("<IHHIIII", PCAP_MAGIC, 2, 4, 0, 0, 65535, 1)


class Writer:
    def __init__(self, cfg):
        REC_DIR.mkdir(parents=True, exist_ok=True)
        VER_DIR.mkdir(parents=True, exist_ok=True)

        self.cfg_mtime  = 0.0
        self.last_check = 0.0
        self.pcap_f     = None
        self.verd_f     = None
        self.pcap_bytes = 0
        self.frame      = 0
        self.idx        = self._next_idx()
        self._apply(cfg)
        self._prune()
        self._open()

    def _apply(self, cfg: dict):
        self.cfg          = cfg
        self.rotate_kb    = int(cfg.get("RECORD_ROTATE_KB", "1024"))
        self.pcap_ring    = int(cfg.get("RECORD_RING_FILES", "4"))
        self.verd_ring    = int(cfg.get("VERDICT_RING_FILES", "10"))
        self.max_age_h    = int(cfg.get("RECORD_MAX_AGE_H", "24"))
        self.min_free_mb  = int(cfg.get("RECORD_MIN_FREE_MB", "10"))
        self.want_pcap    = truthy(cfg.get("RECORD_PCAP", "TRUE"))
        self.want_verd    = truthy(cfg.get("VERDICT_STREAM", "TRUE"))
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
            self.pcap_f = open(REC_DIR / f"rec_{self.idx:06d}.pcap", "wb")
            self.pcap_f.write(PCAP_HDR)
            self.pcap_f.flush()
            self.pcap_bytes = len(PCAP_HDR)
        if self.want_verd:
            self.verd_f = open(VER_DIR / f"rec_{self.idx:06d}.jsonl", "ab")

    def _prune(self):
        # 1. config says off → wipe entirely
        if not self.want_pcap:
            nuke(REC_DIR, "rec_*.pcap")
        if not self.want_verd:
            nuke(VER_DIR, "rec_*.jsonl")
        # 2. ring counts
        if self.want_pcap:
            prune_count(REC_DIR, "rec_*.pcap", self.pcap_ring)
        if self.want_verd:
            prune_count(VER_DIR, "rec_*.jsonl", self.verd_ring)
        # 3. age
        max_age_s = self.max_age_h * 3600
        prune_age(REC_DIR, "rec_*.pcap",  max_age_s)
        prune_age(VER_DIR, "rec_*.jsonl", max_age_s)
        # 4. disk free (last resort — eats whatever is oldest in BOTH dirs)
        if free_mb() < self.min_free_mb:
            # delete oldest across both until free is OK
            all_files = sorted(
                list(REC_DIR.glob("rec_*.pcap")) + list(VER_DIR.glob("rec_*.jsonl")),
                key=lambda p: p.stat().st_mtime,
            )
            while all_files and free_mb() < self.min_free_mb + 2:
                try: all_files[0].unlink()
                except Exception: pass
                all_files = all_files[1:]

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
            self.verd_f = open(VER_DIR / f"rec_{self.idx:06d}.jsonl", "ab")

    def write(self, ts_s: int, ts_us: int, frame_bytes: bytes, verdict: dict):
        self.frame += 1
        now = time.time()
        if self.frame % WATCH_FRAMES == 0 or (now - self.last_check) > WATCH_SECONDS:
            self.last_check = now
            self._check_config()
            self._enforce_disk()

        # PANIC: refuse to write anything that touches the FS until free recovers
        if PRESSURE_STATE["level"] >= 2:
            return

        caplen = len(frame_bytes)
        verdict["f"]  = self.frame
        verdict["t"]  = f"{ts_s}.{ts_us:06d}"
        verdict["sz"] = caplen

        if self.want_verd and self.verd_f:
            line = (json.dumps(verdict, separators=(",", ":")) + "\n").encode()
            self.verd_f.write(line)
            self.verd_f.flush()
            self.verd_bytes += len(line)
            if self.verd_bytes >= VERDICT_MAX_KB * 1024:
                self._rotate()

        if self.want_pcap and self.pcap_f:
            rec = struct.pack("<IIII", ts_s, ts_us, caplen, caplen)
            self.pcap_f.write(rec)
            self.pcap_f.write(frame_bytes)
            self.pcap_f.flush()
            self.pcap_bytes += 16 + caplen
            if self.pcap_bytes >= self.rotate_kb * 1024:
                self._rotate()

    def _enforce_disk(self):
        """Called every ~1.5s. Decide pressure level and act."""
        f = free_mb()
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

        # level 0: if files were closed during emergency, reopen for the active modes
        if self.want_pcap and not self.pcap_f and f > WARN_FREE_MB + 3:
            self.idx = self._next_idx()
            self._open()
            self.verd_bytes = 0

    def _rotate(self):
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

    def close(self):
        if self.pcap_f: self.pcap_f.close()
        if self.verd_f: self.verd_f.close()


# -------- cheap protocol parse -----------------------------------------------

def flow_hash(buf: bytes) -> str:
    if len(buf) < 34:
        return "00000000"
    etype = (buf[12] << 8) | buf[13]
    if etype != 0x0800:
        return "00000000"
    src = int.from_bytes(buf[26:30], "big")
    dst = int.from_bytes(buf[30:34], "big")
    proto = buf[23]
    ihl = (buf[14] & 0x0F) * 4
    sp = dp = 0
    if (proto == 6 or proto == 17) and len(buf) >= 14 + ihl + 4:
        sp = (buf[14 + ihl] << 8) | buf[14 + ihl + 1]
        dp = (buf[14 + ihl + 2] << 8) | buf[14 + ihl + 3]
    h = src ^ dst ^ (sp << 16) ^ dp
    return f"{h & 0xFFFFFFFF:08x}"


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


def classify(buf: bytes, redirect_from_set) -> dict:
    v = {"flow": flow_hash(buf)}
    if len(buf) < 42: return v
    if ((buf[12] << 8) | buf[13]) != 0x0800: return v
    ihl   = (buf[14] & 0x0F) * 4
    proto = buf[23]
    if proto == 17 and len(buf) >= 14 + ihl + 8:
        udp_off = 14 + ihl
        sp = (buf[udp_off] << 8) | buf[udp_off + 1]
        dp = (buf[udp_off + 2] << 8) | buf[udp_off + 3]
        if dp == 53 or sp == 53:
            dns_off = udp_off + 8
            if len(buf) >= dns_off + 12:
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


# -------- stats sidecar -------------------------------------------------------
# Writes /tmp/ghostlink.stats.json every 2s so the WebUI can poll it.

STATS_PATH = Path("/tmp/ghostlink.stats.json")


def dump_stats(frame_count: int, match_count: int, start: float, iface: str):
    try:
        now = time.time()
        elapsed = max(0.01, now - start)
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
            "pps":     round(frame_count / elapsed, 2),
            "mps":     round(match_count / elapsed, 2),
            "if_rx_b": rx_b, "if_tx_b": tx_b,
            "if_rx_p": rx_p, "if_tx_p": tx_p,
            "free_mb":    free_mb(),
            "warn_level": PRESSURE_STATE["level"],
            "warn_text":  ("ok", "pruning (low disk)", "EMERGENCY: writes paused")[PRESSURE_STATE["level"]],
            "warn_free_mb":  WARN_FREE_MB,
            "crit_free_mb":  CRIT_FREE_MB,
        }
        STATS_PATH.write_text(json.dumps(data))
    except Exception:
        pass


# -------- main ----------------------------------------------------------------

def main():
    cfg       = load_cfg(CFG_PATH)
    iface     = cfg.get("SNIFF_IFACE", "usb0").strip()
    red_on    = truthy(cfg.get("REDIRECT", "FALSE"))
    red_from  = []
    if red_on and cfg.get("REDIRECT_FROM"):
        red_from = [p.strip().lower() for p in cfg["REDIRECT_FROM"].split(",") if p.strip()]

    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    log = open(LOG_PATH, "a", buffering=1)
    def L(m): log.write(f"{time.strftime('%H:%M:%S')} sniff {m}\n"); sys.stderr.write(m + "\n")

    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
    try:
        s.bind((iface, 0))
    except OSError as e:
        L(f"bind {iface}: {e}; aborting"); return

    L(f"attached iface={iface} redirect_from={red_from}")
    w = Writer(cfg)
    start = time.time()
    match_count = 0
    last_stats = 0.0

    # Even when the network is quiet, keep the config poll + disk guard alive.
    def _watcher():
        while True:
            try:
                w._check_config()
                w._enforce_disk()
            except Exception: pass
            time.sleep(1.5)
    threading.Thread(target=_watcher, daemon=True).start()

    cfg_mt = 0.0
    try:
        while True:
            try:
                buf, _ = s.recvfrom(65535)
            except Exception as e:
                L(f"recv err {e}"); time.sleep(0.1); continue
            now = time.time()
            ts_s = int(now); ts_us = int((now - ts_s) * 1_000_000)
            # pick up live config changes for REDIRECT_FROM (mtime-triggered)
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
            v = classify(buf, red_from)
            if v.get("rule"): match_count += 1
            w.write(ts_s, ts_us, buf, v)
            if now - last_stats > 2:
                dump_stats(w.frame, match_count, start, iface)
                last_stats = now
    except KeyboardInterrupt:
        pass
    finally:
        w.close(); L("exiting")


if __name__ == "__main__":
    main()
