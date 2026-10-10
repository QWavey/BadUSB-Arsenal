#!/usr/bin/env python3
"""
GhostLink packet sniffer + verdict stream (on-device, no tcpdump needed).

Design:
  * AF_PACKET SOCK_RAW on the configured iface(s).
  * Writes a classic libpcap file per rotation window.
  * Writes a parallel .jsonl verdict file where each line is one frame.
    Protocol-aware verdicts: DNS queries get {"rule":"DNS","q":"<host>"},
    matched REDIRECT_FROM entries get {"rule":"REDIRECT","v":"HIT","d":"<host>"}.
  * Rotates aggressively because /root is only 30 MB on this device.
"""

import json
import os
import socket
import struct
import sys
import time
from pathlib import Path

ROOT      = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))
CFG_PATH  = ROOT / "config.txt"
REC_DIR   = ROOT / "records"
VER_DIR   = ROOT / "verdicts"
LOG_PATH  = ROOT / "logs" / "ghostlink.log"


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
        v = v.split("#", 1)[0].strip()
        cfg[k.strip()] = v
    return cfg


def truthy(v: str) -> bool:
    return str(v).upper() in ("TRUE", "YES", "ON", "1")


# -------- rotation ------------------------------------------------------------

PCAP_MAGIC = 0xA1B2C3D4
PCAP_HDR   = struct.pack("<IHHIIII", PCAP_MAGIC, 2, 4, 0, 0, 65535, 1)


class Writer:
    def __init__(self, cfg):
        self.cfg          = cfg
        self.rotate_kb    = int(cfg.get("RECORD_ROTATE_KB", "1024"))
        self.pcap_ring    = int(cfg.get("RECORD_RING_FILES", "4"))
        self.verd_ring    = int(cfg.get("VERDICT_RING_FILES", "10"))
        self.want_pcap    = truthy(cfg.get("RECORD_PCAP", "TRUE"))
        self.want_verd    = truthy(cfg.get("VERDICT_STREAM", "TRUE"))
        self.pcap_f       = None
        self.verd_f       = None
        self.pcap_bytes   = 0
        self.frame        = 0
        self.idx          = self._next_idx()
        REC_DIR.mkdir(parents=True, exist_ok=True)
        VER_DIR.mkdir(parents=True, exist_ok=True)
        self._open()

    def _next_idx(self) -> int:
        existing = []
        for p in REC_DIR.glob("rec_*.pcap"):
            try:
                existing.append(int(p.stem.split("_")[1]))
            except Exception:
                pass
        return (max(existing) + 1) if existing else 0

    def _open(self):
        if self.want_pcap:
            # Line-buffered flush would be ideal; wb is fully buffered. We
            # flush after the header so Wireshark can open the file even
            # before the first frame.
            self.pcap_f = open(REC_DIR / f"rec_{self.idx:06d}.pcap", "wb")
            self.pcap_f.write(PCAP_HDR)
            self.pcap_f.flush()
            self.pcap_bytes = len(PCAP_HDR)
        if self.want_verd:
            self.verd_f = open(VER_DIR / f"rec_{self.idx:06d}.jsonl", "ab")
        self._prune()

    def _prune(self):
        if self.want_pcap:
            self._prune_dir(REC_DIR, "*.pcap", self.pcap_ring)
        if self.want_verd:
            self._prune_dir(VER_DIR, "*.jsonl", self.verd_ring)

    def _prune_dir(self, d: Path, pat: str, keep: int):
        files = sorted(d.glob(pat), key=lambda p: p.stat().st_mtime)
        while len(files) > keep:
            try:
                files[0].unlink()
            except Exception:
                pass
            files = files[1:]

    def write(self, ts_s: int, ts_us: int, frame_bytes: bytes, verdict: dict):
        self.frame += 1
        caplen = len(frame_bytes)
        verdict["f"] = self.frame
        verdict["t"] = f"{ts_s}.{ts_us:06d}"
        verdict["sz"] = caplen
        if self.want_verd and self.verd_f:
            self.verd_f.write((json.dumps(verdict, separators=(",", ":")) + "\n").encode())
            self.verd_f.flush()
        if self.want_pcap and self.pcap_f:
            rec = struct.pack("<IIII", ts_s, ts_us, caplen, caplen)
            self.pcap_f.write(rec)
            self.pcap_f.write(frame_bytes)
            self.pcap_bytes += 16 + caplen
            # flush each frame so an SSH session reading the file live sees
            # growth — cheap on a 1 MB rotation window and makes debugging
            # (and the WebUI live counter) honest.
            self.pcap_f.flush()
            if self.pcap_bytes >= self.rotate_kb * 1024:
                self._rotate()

    def _rotate(self):
        if self.pcap_f:
            self.pcap_f.close()
            self.pcap_f = None
        if self.verd_f:
            self.verd_f.close()
            self.verd_f = None
        self.idx += 1
        self.pcap_bytes = 0
        self._open()

    def close(self):
        if self.pcap_f:
            self.pcap_f.close()
        if self.verd_f:
            self.verd_f.close()


# -------- cheap protocol parse -----------------------------------------------

def flow_hash(buf: bytes) -> str:
    """5-tuple xor hash. Returns 8 hex chars."""
    if len(buf) < 34:
        return "00000000"
    etype = (buf[12] << 8) | buf[13]
    if etype == 0x0800:  # IPv4
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
    return "00000000"


def parse_dns_qname(buf: bytes, off: int) -> str:
    name = []
    pos = off
    while pos < len(buf) and buf[pos] != 0:
        n = buf[pos]
        if n & 0xC0:  # pointer, don't follow for safety
            return ".".join(name) + "?"
        pos += 1
        if pos + n > len(buf):
            return ".".join(name) + "!"
        name.append(buf[pos:pos + n].decode("ascii", "replace"))
        pos += n
    return ".".join(name)


def classify(buf: bytes, redirect_from_set) -> dict:
    """Returns a verdict dict (minimal if nothing matched)."""
    v = {"flow": flow_hash(buf)}
    if len(buf) < 42:
        return v
    etype = (buf[12] << 8) | buf[13]
    if etype != 0x0800:
        return v
    ihl = (buf[14] & 0x0F) * 4
    proto = buf[23]
    if proto == 17 and len(buf) >= 14 + ihl + 8:  # UDP
        udp_off = 14 + ihl
        sp = (buf[udp_off] << 8) | buf[udp_off + 1]
        dp = (buf[udp_off + 2] << 8) | buf[udp_off + 3]
        if dp == 53 or sp == 53:
            dns_off = udp_off + 8
            if len(buf) >= dns_off + 12:
                qname = parse_dns_qname(buf, dns_off + 12)
                v["rule"]  = "DNS"
                v["host"]  = qname
                low = qname.lower()
                for pat in redirect_from_set:
                    p = pat.lower()
                    if low == p or low.endswith("." + p):
                        v["rule"]    = "REDIRECT"
                        v["verdict"] = "HIT"
                        v["detail"]  = p
                        break
    return v


# -------- main loop -----------------------------------------------------------

def main():
    cfg = load_cfg(CFG_PATH)
    iface = cfg.get("SNIFF_IFACE", "eth0").strip()
    redirect_on = truthy(cfg.get("REDIRECT", "FALSE"))
    red_from    = []
    if redirect_on and cfg.get("REDIRECT_FROM"):
        red_from = [p.strip().lower() for p in cfg["REDIRECT_FROM"].split(",") if p.strip()]

    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    log = open(LOG_PATH, "a", buffering=1)

    def L(msg):
        line = f"{time.strftime('%H:%M:%S')} sniff {msg}\n"
        log.write(line)
        sys.stderr.write(line)

    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
    try:
        s.bind((iface, 0))
    except OSError as e:
        L(f"bind {iface} failed: {e}; retrying on 'lo' is useless. aborting")
        return

    L(f"attached to {iface}, redirect_from={red_from}")
    w = Writer(cfg)

    try:
        while True:
            try:
                buf, _ = s.recvfrom(65535)
            except Exception as e:
                L(f"recv err {e}")
                time.sleep(0.1)
                continue
            now = time.time()
            ts_s = int(now)
            ts_us = int((now - ts_s) * 1_000_000)
            v = classify(buf, red_from)
            w.write(ts_s, ts_us, buf, v)
    except KeyboardInterrupt:
        pass
    finally:
        w.close()
        L("sniff exiting")


if __name__ == "__main__":
    main()
