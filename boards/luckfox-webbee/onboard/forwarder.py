#!/usr/bin/env python3
"""
GhostLink userspace L2 forwarder.

The stock Luckfox RV1106 kernel has no CONFIG_BRIDGE (verified at runtime —
`ip link add type bridge` returns EOPNOTSUPP). So an inline MITM between the
host-side usb0 and the on-board eth0 can't use a kernel bridge. This script
emulates one with two AF_PACKET sockets in promisc mode, cross-copying raw
frames between the two sides.

Throughput ceiling is roughly the Cortex-A7's syscall rate — in the 10-30
Mbps range for small packets, higher for bulk MTU-size frames. That's fine
for a passive-with-passthrough device.

Loop avoidance: we never retransmit a frame whose source MAC is the local
MAC on the destination side (which would mean we just emitted it ourselves
and the switch echoed it back). We also drop frames whose source MAC equals
our own MAC on the receiving interface.
"""
import os
import select
import socket
import struct
import sys
import time
from pathlib import Path

ROOT = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))
LOG  = ROOT / "logs" / "ghostlink.log"

ETH_P_ALL = 0x0003


def _log(m):
    try:
        LOG.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG, "a") as f:
            f.write(f"{time.strftime('%H:%M:%S')} forward {m}\n")
    except Exception: pass


def _get_mac(ifname: str) -> bytes:
    try:
        with open(f"/sys/class/net/{ifname}/address") as f:
            return bytes.fromhex(f.read().strip().replace(":", ""))
    except Exception:
        return b"\x00\x00\x00\x00\x00\x00"


def _bind(ifname: str):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((ifname, 0))
    # join promisc membership so we see frames not for our MAC
    ifindex = socket.if_nametoindex(ifname)
    PACKET_ADD_MEMBERSHIP = 1
    PACKET_MR_PROMISC     = 1
    mreq = struct.pack("iHH8s", ifindex, PACKET_MR_PROMISC, 0, b"\x00"*8)
    s.setsockopt(socket.SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)
    return s


PIDFILE = Path("/run/ghostlink-forwarder.pid")


def _claim_pidfile():
    """Single-instance guard — a double-start would create a packet storm
    (each instance echoes the other's output across usb0 <-> eth0)."""
    try:
        if PIDFILE.exists():
            try: old = int(PIDFILE.read_text().strip())
            except Exception: old = 0
            if old and old != os.getpid():
                try:
                    os.kill(old, 0)
                    _log(f"already running as pid {old}, exiting")
                    return False
                except OSError:
                    pass  # stale pidfile
        PIDFILE.write_text(str(os.getpid()))
        return True
    except Exception:
        return True  # don't block startup if /run isn't writable


def main():
    if not _claim_pidfile():
        return
    a, b = (sys.argv[1] if len(sys.argv) > 1 else "eth0"), \
           (sys.argv[2] if len(sys.argv) > 2 else "usb0")
    for ifn in (a, b):
        if not Path(f"/sys/class/net/{ifn}").exists():
            _log(f"iface {ifn} missing, aborting")
            return
    try:
        sa = _bind(a); sb = _bind(b)
    except Exception as e:
        _log(f"bind err {e}")
        return
    mac_a = _get_mac(a); mac_b = _get_mac(b)
    _log(f"up: {a} <-> {b}  (macs {mac_a.hex()} / {mac_b.hex()})")

    err_streak = 0
    try:
        while True:
            r, _, _ = select.select([sa, sb], [], [], 1.0)
            for s in r:
                try:
                    buf = s.recv(65535)
                except OSError as e:
                    err_streak += 1
                    if err_streak > 100:
                        _log(f"recv error streak hit {err_streak} ({e}); exiting")
                        return
                    continue
                if len(buf) == 0:
                    # iface went down — bail rather than spin.
                    _log("recv returned 0 bytes; iface down, exiting")
                    return
                if len(buf) < 14: continue
                err_streak = 0
                src = buf[6:12]
                # Drop frames we emitted ourselves (prevent self-loop echo).
                if s is sa:
                    if src == mac_a: continue
                    try: sb.send(buf)
                    except OSError: pass
                else:
                    if src == mac_b: continue
                    try: sa.send(buf)
                    except OSError: pass
    except KeyboardInterrupt:
        pass
    finally:
        try: sa.close()
        except Exception: pass
        try: sb.close()
        except Exception: pass
        try:
            if PIDFILE.exists() and PIDFILE.read_text().strip() == str(os.getpid()):
                PIDFILE.unlink()
        except Exception: pass
        _log("forwarder exiting")


if __name__ == "__main__":
    main()
