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

Loop avoidance: every frame we transmit is handed back to our own sockets
as PACKET_OUTGOING. Forwarding those would loop forever, so they are dropped
(the kernel is asked not to queue them; _bridge also checks the packet type).
We also drop frames whose source MAC equals our own MAC on the receiving
interface.

Status: the last state is written to STATUS (a file in /run) so the web UI
can say why the forwarder is not running, instead of only that it isn't.
"""
import errno
import os
import select
import signal
import socket
import struct
import sys
import time
from pathlib import Path

ROOT = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))
LOG  = ROOT / "logs" / "ghostlink.log"

ETH_P_ALL = 0x0003
DRAIN_FRAMES = 64  # most frames taken from one socket per select(); keeps the other direction from starving
SOL_PACKET = getattr(socket, "SOL_PACKET", 263)  # Linux value; some Python builds leave the name out
PACKET_OUTGOING        = 4   # packet type of a frame this host transmitted
PACKET_IGNORE_OUTGOING = 23  # setsockopt: do not queue our own transmissions
EMSGSIZE = 90  # Linux errno for "Message too long": a frame longer than the outgoing MTU


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
    try:
        s.bind((ifname, 0))
        # Tune socket buffers to prevent overflow during heavy traffic.
        # Default (~128KB) overflows when forwarding large website assets.
        # Increase to 1MB to handle bursts of multiple large frames.
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024*1024)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024*1024)
        except OSError:
            pass  # Some systems may reject this; continue anyway
        # join promisc membership so we see frames not for our MAC
        ifindex = socket.if_nametoindex(ifname)
        PACKET_ADD_MEMBERSHIP = 1
        PACKET_MR_PROMISC     = 1
        mreq = struct.pack("iHH8s", ifindex, PACKET_MR_PROMISC, 0, b"\x00"*8)
        s.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)
    except Exception:
        s.close()
        raise
    try:
        s.setsockopt(SOL_PACKET, PACKET_IGNORE_OUTGOING, 1)
    except OSError:
        pass  # older kernel: _bridge() drops these by packet type instead
    return s


PIDFILE = Path("/run/ghostlink-forwarder.pid")
STATUS  = Path("/run/ghostlink-forwarder.status")


def _status(msg: str) -> None:
    try:
        STATUS.write_text(msg + "\n")
    except Exception:
        pass  # /run not writable: the log still has the reason


def _pid_is_forwarder(pid: int) -> bool:
    """True only if *pid* is a live forwarder.py process (not a reused pid)."""
    try:
        return b"forwarder.py" in Path(f"/proc/{pid}/cmdline").read_bytes()
    except OSError:
        pass
    # No /proc entry: the process is gone, or /proc isn't available here.
    try:
        os.kill(pid, 0)
        return True   # alive, and we can't tell what it is: be conservative
    except OSError:
        return False


def _claim_pidfile():
    """Single-instance guard — a double-start would create a packet storm
    (each instance echoes the other's output across usb0 <-> eth0)."""
    # O_EXCL makes the claim atomic: two starts at the same moment (the watcher
    # poll and the web UI button) cannot both pass the check.
    for _ in range(3):
        try:
            fd = os.open(PIDFILE, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except FileExistsError:
            try: old = int(PIDFILE.read_text().strip())
            except Exception: old = 0
            if old and old != os.getpid() and _pid_is_forwarder(old):
                _log(f"already running as pid {old}, exiting")
                return False
            try: PIDFILE.unlink()  # stale: its owner is gone
            except OSError: pass
            continue
        except Exception:
            return True  # don't block startup if /run isn't writable
        with os.fdopen(fd, "w") as f:
            f.write(str(os.getpid()))
        return True
    return True


def _release_pidfile():
    try:
        if PIDFILE.exists() and PIDFILE.read_text().strip() == str(os.getpid()):
            PIDFILE.unlink()
    except Exception:
        pass


def _bridge(a: str, b: str) -> str:
    """Cross-copy frames between a and b until one fails. Returns why it stopped."""
    for ifn in (a, b):
        if not Path(f"/sys/class/net/{ifn}").exists():
            _log(f"iface {ifn} missing, aborting")
            return f"{ifn} is not up" + (" (no USB host connected?)" if ifn == "usb0" else "")
    try:
        sa = _bind(a); sb = _bind(b)
    except Exception as e:
        _log(f"bind err {e}")
        return f"could not open {a} and {b}: {e}"
    try:
        mac_a = _get_mac(a); mac_b = _get_mac(b)
        _log(f"up: {a} <-> {b}  (macs {mac_a.hex()} / {mac_b.hex()})")
        _status(f"running: {a} <-> {b}")
        err_streak = 0
        send_err_streak = 0
        oversize = {"usb0": 0, "eth0": 0}
        while True:
            r, _, _ = select.select([sa, sb], [], [], 1.0)
            for s in r:
                # Take the frames already queued on this socket, up to DRAIN_FRAMES,
                # before the next select(). One select() per burst instead of per frame.
                for _ in range(DRAIN_FRAMES):
                    try:
                        buf, addr = s.recvfrom(65535, socket.MSG_DONTWAIT)
                    except BlockingIOError:
                        break
                    except OSError as e:
                        if e.errno == errno.ENETDOWN:
                            _log(f"receive failed, interface went down ({e})")
                            return "an interface went down"
                        err_streak += 1
                        if err_streak > 100:
                            _log(f"recv error streak hit {err_streak} ({e}); exiting")
                            return "too many receive errors"
                        break
                    if len(buf) == 0:
                        # iface went down — bail rather than spin.
                        _log("recv returned 0 bytes; iface down, exiting")
                        return "an interface went down"
                    if len(buf) < 14: continue
                    if addr[2] == PACKET_OUTGOING: continue  # our own transmission, looped back
                    err_streak = 0
                    src = buf[6:12]
                    # Frames sourced from our own MAC are not from a host on this side.
                    if s is sa:
                        if src == mac_a: continue
                        try:
                            sb.send(buf)
                            send_err_streak = 0
                        except OSError as e:
                            if e.errno == EMSGSIZE:
                                # Longer than usb0 accepts: a dropped frame, not a link fault.
                                oversize["usb0"] += 1
                                if oversize["usb0"] == 1 or oversize["usb0"] % 1000 == 0:
                                    _log(f"dropped oversize frame for usb0: {len(buf)} bytes ({oversize['usb0']} so far)")
                                continue
                            send_err_streak += 1
                            if send_err_streak == 1:
                                _log(f"send to usb0 failed: {e}")
                            if send_err_streak > 100:
                                _log(f"send error streak hit {send_err_streak}; exiting")
                                return "too many send errors"
                    else:
                        if src == mac_b: continue
                        try:
                            sa.send(buf)
                            send_err_streak = 0
                        except OSError as e:
                            if e.errno == EMSGSIZE:
                                # Longer than eth0 accepts: a dropped frame, not a link fault.
                                oversize["eth0"] += 1
                                if oversize["eth0"] == 1 or oversize["eth0"] % 1000 == 0:
                                    _log(f"dropped oversize frame for eth0: {len(buf)} bytes ({oversize['eth0']} so far)")
                                continue
                            send_err_streak += 1
                            if send_err_streak == 1:
                                _log(f"send to eth0 failed: {e}")
                            if send_err_streak > 100:
                                _log(f"send error streak hit {send_err_streak}; exiting")
                                return "too many send errors"
    except KeyboardInterrupt:
        return "stopped"
    finally:
        for s in (sa, sb):
            try: s.close()
            except Exception: pass


def main():
    if not _claim_pidfile():
        return
    # A SIGTERM (from the web UI or the watcher) should still run the cleanup.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    # Forwarding is the latency-sensitive path. Under CPU contention the
    # sniffer and the WebUI should wait for it, not the other way round.
    try: os.nice(-5)
    except OSError: pass
    reason = "stopped"
    try:
        a = sys.argv[1] if len(sys.argv) > 1 else "eth0"
        b = sys.argv[2] if len(sys.argv) > 2 else "usb0"
        reason = _bridge(a, b)
    except Exception as e:
        reason = f"crashed: {e}"  # the web UI shows this; the traceback still goes to the log
        raise
    finally:
        _release_pidfile()
        _status(f"stopped: {reason}")
        _log(f"forwarder exiting: {reason}")


if __name__ == "__main__":
    main()
