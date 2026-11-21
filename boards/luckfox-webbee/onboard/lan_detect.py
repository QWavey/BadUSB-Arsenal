#!/usr/bin/env python3
"""
Passive LAN detection for the GhostLink / WebBee device.

The device bridges the PC (usb0) and the real LAN (eth0) at layer 2 (forwarder.py).
When the PC's DHCP client runs, its DHCPDISCOVER/DHCPREQUEST leave through eth0 and
the real router's DHCPOFFER/DHCPACK come back through eth0. This module listens on
eth0 for those replies and derives the LAN's parameters from them. It sends nothing.

Usage: python3 lan_detect.py <iface> [timeout_seconds]
Exit:  0 detected (JSON on stdout, also written to /tmp/ghostlink.lan.json),
       1 nothing seen within the timeout, 2 bad arguments.

Limits (by design, not bugs):
- Nothing is detected unless a DHCP exchange happens inside the window. A PC that
  already holds a lease sends nothing until it renews (about half the lease time),
  so the caller may have to make the PC renew (e.g. `ipconfig /renew`) or accept a timeout.
- The subnet mask comes from option 1. If the server sends none, /24 is assumed.
- The gateway comes from option 3. If that is absent, the DHCP server identifier
  (option 54) is used, which on home routers is the router itself.
- dhcp_pool_hint is a sample: the offered addresses seen in the window. It is not
  the pool bounds, and a single-sample run gives only one address.
- suggested_ip is not probed with ARP. It only avoids the addresses seen in the DHCP
  replies, so a static address that falls inside the router's range can still collide.
- Only the first DHCP exchange of the window is observed. Options overloaded into
  the sname/file fields (option 52) are not parsed.
"""
from __future__ import annotations

import json
import os
import socket
import struct
import sys
import threading
import time
from pathlib import Path

LOG = Path("/tmp/ghostlink.log")
STATE_PATH = "/tmp/ghostlink.lan.json"

ETH_P_ALL = 0x0003
SOL_PACKET = getattr(socket, "SOL_PACKET", 263)  # Linux value; some Python builds leave the name out
PACKET_ADD_MEMBERSHIP = 1
PACKET_MR_PROMISC = 1

DHCP_PORTS = (67, 68)
MAGIC_COOKIE = b"\x63\x82\x53\x63"
MSG_OFFER = 2
MSG_ACK = 5
DEFAULT_TIMEOUT = 15.0
LINGER_AFTER_COMPLETE_S = 2.0  # after a full observation, keep listening this long for more offered addresses
DEFAULT_PREFIX = 24  # used only when the server sends no usable subnet mask


def _log(m):
    try:
        LOG.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG, "a") as f:
            f.write(f"{time.strftime('%H:%M:%S')} lan_detect {m}\n")
    except Exception: pass


def _ip_int(ip: str) -> int:
    return struct.unpack("!I", socket.inet_aton(ip))[0]


def _int_ip(n: int) -> str:
    return socket.inet_ntoa(struct.pack("!I", n & 0xFFFFFFFF))


def _prefix_mask(prefix: int) -> int:
    return (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF


def _mask_prefix(mask):
    """Prefix length of a dotted subnet mask, or None if it is absent, non-contiguous, or implausible."""
    if not mask: return None
    m = _ip_int(mask)
    prefix = bin(m).count("1")
    if not 8 <= prefix <= 30 or m != _prefix_mask(prefix): return None
    return prefix


def _dhcp_options(data: bytes) -> dict:
    """Code -> value of the first occurrence of each option. Stops at END or at a truncated option."""
    opts = {}
    i = 0
    while i < len(data):
        code = data[i]
        if code == 255: break       # END
        if code == 0:               # PAD
            i += 1
            continue
        if i + 1 >= len(data): break
        ln = data[i + 1]
        val = data[i + 2:i + 2 + ln]
        if len(val) < ln: break
        opts.setdefault(code, val)
        i += 2 + ln
    return opts


def _parse_dhcp_packet(raw: bytes) -> dict | None:
    """Parse one captured Ethernet frame. Returns the BOOTP/DHCP fields, or None if the frame
    is not a complete IPv4/UDP frame on ports 67/68 with a DHCP cookie. Never raises."""
    try:
        # Ethernet II, optionally one 802.1Q tag.
        if len(raw) < 14: return None
        etype = struct.unpack_from("!H", raw, 12)[0]
        off = 14
        if etype == 0x8100:
            if len(raw) < 18: return None
            etype = struct.unpack_from("!H", raw, 16)[0]
            off = 18
        if etype != 0x0800: return None

        # IPv4. Only a first fragment carries the UDP header.
        ip = raw[off:]
        if len(ip) < 20 or (ip[0] >> 4) != 4: return None
        ihl = (ip[0] & 0x0F) * 4
        if ihl < 20 or len(ip) < ihl: return None
        if ip[9] != 17: return None  # protocol UDP
        if struct.unpack_from("!H", ip, 6)[0] & 0x1FFF: return None
        src_ip = socket.inet_ntoa(ip[12:16])
        dst_ip = socket.inet_ntoa(ip[16:20])

        # UDP.
        udp = ip[ihl:]
        if len(udp) < 8: return None
        sport, dport, ulen = struct.unpack_from("!HHH", udp, 0)
        if sport not in DHCP_PORTS and dport not in DHCP_PORTS: return None
        body = udp[8:ulen] if 8 <= ulen <= len(udp) else udp[8:]

        # BOOTP fixed part is 236 bytes, then the 4-byte magic cookie, then options.
        if len(body) < 240 or body[236:240] != MAGIC_COOKIE: return None
        op, htype, hlen, hops = body[0], body[1], body[2], body[3]
        xid, secs, flags = struct.unpack_from("!IHH", body, 4)
        ciaddr = socket.inet_ntoa(body[12:16])
        yiaddr = socket.inet_ntoa(body[16:20])
        siaddr = socket.inet_ntoa(body[20:24])
        giaddr = socket.inet_ntoa(body[24:28])
        chaddr = ":".join(f"{b:02x}" for b in body[28:28 + min(hlen, 16)])

        opts = _dhcp_options(body[240:])
        mt = opts.get(53)
        msg_type = mt[0] if mt and len(mt) == 1 else None
        mask = socket.inet_ntoa(opts[1]) if len(opts.get(1, b"")) == 4 else None
        router = socket.inet_ntoa(opts[3][:4]) if len(opts.get(3, b"")) >= 4 else None
        if router == "0.0.0.0": router = None
        server_id = socket.inet_ntoa(opts[54]) if len(opts.get(54, b"")) == 4 else None

        return {
            "src_ip": src_ip, "dst_ip": dst_ip,
            "src_port": sport, "dst_port": dport,
            "op": op, "htype": htype, "hlen": hlen, "hops": hops,
            "xid": xid, "secs": secs, "flags": flags,
            "ciaddr": ciaddr, "yiaddr": yiaddr, "siaddr": siaddr, "giaddr": giaddr,
            "chaddr": chaddr,
            "msg_type": msg_type,
            "subnet_mask": mask,
            "router": router,
            "server_id": server_id,
        }
    except Exception:
        return None


def _is_complete(info: dict) -> bool:
    """An offer/ack with an address, a usable mask and a gateway (or server id): enough to stop early."""
    return (info.get("yiaddr") not in (None, "0.0.0.0")
            and _mask_prefix(info.get("subnet_mask")) is not None
            and bool(info.get("router") or info.get("server_id")))


def _pick_suggested(base24: int, net: int, mask: int, gateway, taken) -> str | None:
    """First host in base24 (the /24 of the gateway) that is not the gateway, not an observed
    address, and lies in the detected subnet. Prefers .250-.254, then counts down."""
    taken = set(taken)
    for last in [250, 251, 252, 253, 254] + list(range(249, 1, -1)):
        cand_int = base24 | last
        cand = _int_ip(cand_int)
        if cand == gateway or cand in taken: continue
        if cand_int & mask != net: continue
        return cand
    return None


def _summarize(observations: list, iface: str) -> dict | None:
    """Build the detect_lan result from parsed DHCP packets. Pure: no sockets, no clock.
    Uses only DHCPOFFER and DHCPACK with a non-zero yiaddr. Returns None if there are none."""
    replies = [o for o in observations
               if o.get("msg_type") in (MSG_OFFER, MSG_ACK)
               and o.get("yiaddr") not in (None, "0.0.0.0")]
    if not replies: return None

    prefix = None
    for o in replies:  # first reply that carries a usable mask
        prefix = _mask_prefix(o.get("subnet_mask"))
        if prefix is not None: break
    if prefix is None: prefix = DEFAULT_PREFIX

    gateway = (next((o["router"] for o in replies if o.get("router")), None)
               or next((o["server_id"] for o in replies if o.get("server_id")), None))

    pool = []  # addresses offered in the window, first-seen order; a sample, not the pool bounds
    for o in replies:
        if o["yiaddr"] not in pool: pool.append(o["yiaddr"])

    mask = _prefix_mask(prefix)
    net = _ip_int(pool[0]) & mask
    base = _ip_int(gateway) if gateway else _ip_int(pool[0])
    suggested = _pick_suggested(base & 0xFFFFFF00, net, mask, gateway, pool)

    return {
        "detected": True,
        "subnet": f"{_int_ip(net)}/{prefix}",
        "gateway": gateway,
        "dhcp_pool_hint": pool,
        "suggested_ip": suggested,
        "iface": iface,
    }


def _open_capture(iface: str):
    """Raw AF_PACKET socket bound to iface, with promiscuous membership so unicast replies to the PC's
    MAC reach us. forwarder.py and ghostlink.sh also set promisc on eth0; this does not depend on them.
    The membership ends with the socket. None if the bind fails; the error is logged."""
    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(ETH_P_ALL))
    except OSError as e:  # needs root (CAP_NET_RAW)
        _log(f"socket: {e}")
        return None
    try:
        s.bind((iface, 0))
    except OSError as e:
        s.close()
        _log(f"bind {iface}: {e}")
        return None
    try:
        mreq = struct.pack("iHH8s", socket.if_nametoindex(iface), PACKET_MR_PROMISC, 0, b"\x00" * 8)
        s.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)
    except OSError:
        pass  # still works without it, for broadcast replies
    s.settimeout(1.0)  # recv wakes at least once a second to check the deadline
    return s


def detect_lan(iface: str, timeout: float = DEFAULT_TIMEOUT) -> dict | None:
    """Listen on iface for DHCP offers/acks and describe the LAN they came from.
    Returns the result dict, or None if no reply arrived within timeout seconds."""
    if not hasattr(socket, "AF_PACKET"):
        _log("AF_PACKET not available on this platform")
        return None
    sock = _open_capture(iface)
    if sock is None:
        return None
    _log(f"listening on {iface} for up to {timeout:g}s")
    deadline = time.monotonic() + timeout
    complete_until = None
    seen = []
    try:
        while True:
            now = time.monotonic()
            if now >= deadline: break
            if complete_until is not None and now >= complete_until: break
            try:
                frame = sock.recv(65535)
            except socket.timeout:
                continue
            except OSError as e:
                _log(f"recv {iface}: {e}")
                break
            info = _parse_dhcp_packet(frame)
            if info is None or info.get("msg_type") not in (MSG_OFFER, MSG_ACK):
                continue
            seen.append(info)
            if complete_until is None and _is_complete(info):
                complete_until = time.monotonic() + LINGER_AFTER_COMPLETE_S
    finally:
        sock.close()

    result = _summarize(seen, iface)
    if result is None:
        _log(f"no DHCP offer/ack on {iface} within {timeout:g}s")
        return None
    result["detected_at"] = int(time.time())  # taken now, not at import time
    _log(f"detected subnet={result['subnet']} gateway={result['gateway']} "
         f"suggested={result['suggested_ip']} pool={result['dhcp_pool_hint']}")
    return result


def write_state(result: dict, path: str = STATE_PATH) -> None:
    """Atomically replace path with result as JSON. Same pattern as webui._atomic_write: a temp file
    unique to this call, then os.replace (which also overwrites on Windows, where os.rename would fail).
    A failure is logged, not raised."""
    p = Path(path)
    data = (json.dumps(result, indent=2) + "\n").encode("utf-8")
    tmp = p.with_name(f"{p.name}.{os.getpid()}.{threading.get_ident()}.new")
    try:
        tmp.write_bytes(data)
        os.replace(tmp, p)
    except OSError as e:
        _log(f"write_state {p}: {e}")
        try: tmp.unlink()
        except OSError: pass


def _main(argv: list) -> int:
    usage = "usage: python3 lan_detect.py <iface> [timeout_seconds]"
    if len(argv) not in (2, 3):
        print(usage, file=sys.stderr)
        return 2
    try:
        timeout = float(argv[2]) if len(argv) == 3 else DEFAULT_TIMEOUT
    except ValueError:
        print(usage, file=sys.stderr)
        return 2
    if timeout <= 0:
        print(usage, file=sys.stderr)
        return 2
    iface = argv[1]
    result = detect_lan(iface, timeout)
    if result is None:
        print(f"no DHCP offer or ack seen on {iface} within {timeout:g}s", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    write_state(result)
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
