#!/usr/bin/env python3
"""Userspace NAT for the PC behind the WebBee's USB link (NET_MODE=NAT).

The PC takes the WebBee as its gateway and DNS server over usb0. Its IPv4 traffic leaves
through eth0 with the source rewritten to NAT_IP, and the replies are sent back to the PC.
The device's kernel has no netfilter and no TUN device, so the rewriting is done here, on
raw Ethernet frames.

Translated: TCP, UDP, ICMP echo, and the ICMP errors (unreachable, time exceeded) about them.
Anything else, and any fragment, is dropped.

NAT_IP is an address on the LAN that nothing else uses. The kernel does not own it, so this
program answers ARP for it, and the kernel drops anything sent to it.

usage: natd.py PC_IF WAN_IF NAT_IP WAN_GATEWAY PC_NET   (PC_NET like 172.32.0.0/16)

eth0 must not merge received segments (GRO): a merged frame is bigger than the USB MTU, the
USB send fails with EMSGSIZE, and every such frame is lost. GRO is switched off at startup.

Throughput note: the USB (NCM) gadget pays a large fixed cost per transmit, so sending frames
one at a time starves it. Each select wake now drains every readable socket (non-blocking, up
to DRAIN_CAP frames), translates the lot, and then sends each direction's frames back-to-back
in one burst, so the gadget can coalesce them into fewer NTB transfers. The NAT translation
itself (checksums, port allocation, idle expiry, ICMP handling) is unchanged; only the I/O
scheduling around it changed.
"""
import ctypes
import fcntl
import select
import socket
import struct
import sys
import time

SIOCETHTOOL = 0x8946
ETHTOOL_GGRO, ETHTOOL_SGRO = 0x2B, 0x2C
ETH_P_ALL = 0x0003
PACKET_OUTGOING = 4
ICMP, TCP, UDP = 1, 6, 17
ICMP_ECHO, ICMP_ECHO_REPLY = 8, 0
ICMP_ERRORS = (3, 11)                    # destination unreachable, time exceeded
PORT_LO, PORT_HI = 40000, 60000          # below the kernel's ephemeral range (32768-60999)
IDLE = {UDP: 120, ICMP: 60, TCP: 900}    # seconds without traffic before an entry is dropped
TCP_CLOSING = 60                         # idle seconds left after a FIN or RST

# -- throughput tuning (I/O only; no effect on translation) ---------------------
DRAIN_CAP = 64                           # frames read from one socket per select wake
RCVBUF = 180224                          # SO_RCVBUF target; the device caps it at rmem_max
SOL_PACKET = getattr(socket, "SOL_PACKET", 263)
PACKET_IGNORE_OUTGOING = 23              # kernel >= 4.20: do not loop our own sends back to us
USE_SENDMMSG = False                     # ctypes sendmmsg costs more Python time than the send() calls it saves


def _fold(s):
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s


def checksum(data):
    """The Internet checksum of data."""
    if len(data) % 2:
        data += b"\x00"
    return ~_fold(sum(struct.unpack("!%dH" % (len(data) // 2), data))) & 0xFFFF


def adjust(csum, changes):
    """RFC 1624: the checksum after some 16-bit words changed. changes: (old, new) byte pairs."""
    s = ~csum & 0xFFFF
    for old, new in changes:
        for i in range(0, len(old), 2):
            s += (~((old[i] << 8) | old[i + 1]) & 0xFFFF) + ((new[i] << 8) | new[i + 1])
    return ~_fold(s) & 0xFFFF


def _parse(pkt):
    """(ihl, total, proto, src, dst) of a whole IPv4 packet, or None for anything we do not handle."""
    if len(pkt) < 20 or pkt[0] >> 4 != 4:
        return None
    ihl = (pkt[0] & 0x0F) * 4
    if ihl < 20 or len(pkt) < ihl:
        return None
    total = struct.unpack("!H", pkt[2:4])[0]
    if total < ihl or total > len(pkt):
        return None
    if struct.unpack("!H", pkt[6:8])[0] & 0x3FFF:       # a fragment: no ports to read
        return None
    return ihl, total, pkt[9], pkt[12:16], pkt[16:20]


def _in_net(ip, net, plen):
    mask = (0xFFFFFFFF << (32 - plen)) & 0xFFFFFFFF
    return struct.unpack("!I", ip)[0] & mask == struct.unpack("!I", net)[0] & mask


def _rewrite(pkt, ihl, total, proto, *, src=None, dst=None, sport=None, dport=None,
             ident=None, dec_ttl=False):
    """A copy of the IPv4 packet with the given fields replaced and every checksum fixed.
    None when the TTL would reach zero."""
    b = bytearray(pkt[:total])
    if dec_ttl:
        if b[8] <= 1:
            return None
        b[8] -= 1
    pseudo = []                               # changes that also count in the TCP/UDP checksum
    if src is not None:
        pseudo.append((bytes(b[12:16]), src))
        b[12:16] = src
    if dst is not None:
        pseudo.append((bytes(b[16:20]), dst))
        b[16:20] = dst
    if proto in (TCP, UDP):
        off = ihl + (6 if proto == UDP else 16)
        # Belt-and-suspenders: every caller is expected to pre-check the full L4 header is
        # present, but a packet short enough to miss the checksum field here would otherwise
        # make struct.unpack() raise -- an unhandled exception that crashed the whole process
        # on a single malformed packet (see the _from_wan length-check fix). Dropping the
        # packet is always safe; crashing the translator for every flow never is.
        if len(b) < off + 2:
            return None
        if sport is not None:
            new = struct.pack("!H", sport)
            pseudo.append((bytes(b[ihl:ihl + 2]), new))
            b[ihl:ihl + 2] = new
        if dport is not None:
            new = struct.pack("!H", dport)
            pseudo.append((bytes(b[ihl + 2:ihl + 4]), new))
            b[ihl + 2:ihl + 4] = new
        old_c = struct.unpack("!H", b[off:off + 2])[0]
        if not (proto == UDP and old_c == 0):    # UDP 0 means "no checksum": keep it
            c = adjust(old_c, pseudo)
            if proto == UDP and c == 0:
                c = 0xFFFF
            b[off:off + 2] = struct.pack("!H", c)
    elif proto == ICMP and ident is not None:
        new = struct.pack("!H", ident)
        c = adjust(struct.unpack("!H", b[ihl + 2:ihl + 4])[0], [(bytes(b[ihl + 4:ihl + 6]), new)])
        b[ihl + 4:ihl + 6] = new
        b[ihl + 2:ihl + 4] = struct.pack("!H", c)
    b[10:12] = b"\x00\x00"
    b[10:12] = struct.pack("!H", checksum(bytes(b[:ihl])))
    return bytes(b)


def _inner_fix(inner, ihl, src, sport):
    """Rewrite the source of the packet quoted inside an ICMP error. Only the header and the
    ports are in the quote, so the transport checksum cannot be fixed and is left alone."""
    b = bytearray(inner)
    b[12:16] = src
    b[ihl:ihl + 2] = struct.pack("!H", sport)
    b[10:12] = b"\x00\x00"
    b[10:12] = struct.pack("!H", checksum(bytes(b[:ihl])))
    return bytes(b)


class Nat:
    def __init__(self, nat_ip, wan_gw, pc_net, usb_mac, wan_mac):
        self.nat_ip = socket.inet_aton(nat_ip)
        self.wan_gw = socket.inet_aton(wan_gw)
        net, plen = pc_net.split("/")
        self.pc_net = (socket.inet_aton(net), int(plen))
        self.usb_mac = usb_mac
        self.wan_mac = wan_mac
        self.gw_mac = None
        self.pc_mac = {}                 # PC address -> its MAC, learned from its frames
        self.out = {}                    # (proto, pc_ip, pc_port, remote_ip, remote_port) -> entry
        self.back = {}                   # (proto, remote_ip, remote_port, nat_port) -> entry
        self.ports = {TCP: set(), UDP: set(), ICMP: set()}
        self.next_port = PORT_LO
        self.counts = {"to_wan": 0, "to_pc": 0, "dropped": 0}
        self.drops = {}                  # "side:ethertype" -> frames dropped, to see what is ignored

    # -- state ----------------------------------------------------------------

    def set_gateway_mac(self, mac):
        if mac:
            self.gw_mac = mac

    def need_gateway(self):
        return self.gw_mac is None

    def arp_request_for_gateway(self):
        return (b"\xff" * 6 + self.wan_mac + b"\x08\x06"
                + struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1)
                + self.wan_mac + self.nat_ip + b"\x00" * 6 + self.wan_gw)

    def _alloc(self, proto):
        used = self.ports[proto]
        if len(used) >= PORT_HI - PORT_LO:
            return None
        p = self.next_port
        while p in used:
            p = PORT_LO if p >= PORT_HI else p + 1
        self.next_port = PORT_LO if p >= PORT_HI else p + 1
        used.add(p)
        return p

    def _entry(self, proto, out_key, now):
        ent = self.out.get(out_key)
        if ent is None:
            port = self._alloc(proto)
            if port is None:
                return None
            back_key = (proto, out_key[3], out_key[4], port)
            ent = {"port": port, "last": now, "out": out_key, "back": back_key,
                   "pc_ip": out_key[1], "pc_port": out_key[2]}
            self.out[out_key] = ent
            self.back[back_key] = ent
        ent["last"] = now
        return ent

    def expire(self, now):
        for key, ent in list(self.out.items()):
            if now - ent["last"] > IDLE[key[0]]:
                del self.out[key]
                self.back.pop(ent["back"], None)
                self.ports[key[0]].discard(ent["port"])

    # -- frames ---------------------------------------------------------------

    def handle(self, frame, side, now):
        """Process one Ethernet frame that came in on side ('pc' or 'wan').
        Returns (frame_to_send, side_to_send_on), or None to drop it."""
        if len(frame) < 14:
            return None
        etype = frame[12:14]
        try:
            if etype == b"\x08\x00":
                res = self._from_pc(frame, now) if side == "pc" else self._from_wan(frame, now)
            elif etype == b"\x08\x06" and side == "wan":
                res = self._arp_wan(frame)
            else:
                res = None
        except Exception as e:
            # A parsing bug on one malformed/unusual frame must never crash the whole
            # translator -- that drops every PC's connectivity at once, repeatably, for as
            # long as something keeps sending that frame (confirmed: a single truncated TCP
            # segment from the WAN side did exactly this before the length-check fix). Drop
            # the one frame and keep going; this is the last line of defense, not the fix --
            # the root cause should still be found and fixed like the one above was.
            self.counts["errors"] = self.counts.get("errors", 0) + 1
            self.drops[f"{side}:exc:{type(e).__name__}"] = self.drops.get(f"{side}:exc:{type(e).__name__}", 0) + 1
            return None
        if res is None:
            self.counts["dropped"] += 1
            key = "%s:%s" % (side, etype.hex())
            self.drops[key] = self.drops.get(key, 0) + 1
        return res

    def _from_pc(self, frame, now):
        pkt = frame[14:]
        h = _parse(pkt)
        if h is None:
            return None
        ihl, total, proto, src, dst = h
        if not _in_net(src, *self.pc_net) or _in_net(dst, *self.pc_net):
            return None                              # not from the PC, or the device's own subnet
        if dst[0] >= 224 or dst == b"\xff\xff\xff\xff" or src == self.nat_ip:
            return None
        l4 = pkt[ihl:total]
        if proto in (TCP, UDP):
            if len(l4) < (8 if proto == UDP else 20):
                return None
            sport, dport = struct.unpack("!HH", l4[:4])
            ent = self._entry(proto, (proto, src, sport, dst, dport), now)
            if ent is None:
                return None
            if proto == TCP and l4[13] & 0x05:       # FIN or RST: the flow is closing
                ent["last"] = now - (IDLE[TCP] - TCP_CLOSING)
            new = _rewrite(pkt, ihl, total, proto, src=self.nat_ip, sport=ent["port"], dec_ttl=True)
        elif proto == ICMP:
            if len(l4) < 8 or l4[0] != ICMP_ECHO:
                return None
            ident = struct.unpack("!H", l4[4:6])[0]
            ent = self._entry(ICMP, (ICMP, src, ident, dst, 0), now)
            if ent is None:
                return None
            new = _rewrite(pkt, ihl, total, ICMP, src=self.nat_ip, ident=ent["port"], dec_ttl=True)
        else:
            return None
        if new is None or self.gw_mac is None:
            return None
        self.pc_mac[src] = frame[6:12]
        self.counts["to_wan"] += 1
        return self.gw_mac + self.wan_mac + b"\x08\x00" + new, "wan"

    def _from_wan(self, frame, now):
        pkt = frame[14:]
        h = _parse(pkt)
        if h is None:
            return None
        ihl, total, proto, src, dst = h
        if dst != self.nat_ip:
            return None                              # the device's own traffic, or not ours
        l4 = pkt[ihl:total]
        if proto in (TCP, UDP):
            # Same minimum as _from_pc: _rewrite() reads the checksum field at a fixed offset
            # into the L4 header (6 for UDP, 16 for TCP) without re-checking length, so a
            # shorter-than-4-bytes check here let a truncated TCP segment (len(l4) in [4,19])
            # through to an out-of-bounds struct.unpack() -- an unhandled exception that
            # crashed the whole process on a single malformed packet from the real LAN.
            if len(l4) < (8 if proto == UDP else 20):
                return None
            sport, dport = struct.unpack("!HH", l4[:4])
            ent = self.back.get((proto, src, sport, dport))
            if ent is None:
                return None
            ent["last"] = now
            new = _rewrite(pkt, ihl, total, proto, dst=ent["pc_ip"], dport=ent["pc_port"], dec_ttl=True)
        elif proto == ICMP:
            if len(l4) < 8:
                return None
            if l4[0] == ICMP_ECHO_REPLY:
                ident = struct.unpack("!H", l4[4:6])[0]
                ent = self.back.get((ICMP, src, 0, ident))
                if ent is None:
                    return None
                ent["last"] = now
                new = _rewrite(pkt, ihl, total, ICMP, dst=ent["pc_ip"], ident=ent["pc_port"], dec_ttl=True)
            elif l4[0] in ICMP_ERRORS:
                new = self._icmp_error_back(pkt, ihl, total, l4, now)
            else:
                return None
        else:
            return None
        if new is None:
            return None
        pc_ip = struct.unpack("!4s", new[16:20])[0]
        mac = self.pc_mac.get(pc_ip)
        if mac is None:
            return None
        self.counts["to_pc"] += 1
        return mac + self.usb_mac + b"\x08\x00" + new, "pc"

    def _icmp_error_back(self, pkt, ihl, total, l4, now):
        """An ICMP error about one of our translated packets: rewrite the quoted packet too."""
        # The quoted packet is cut short (its header and 8 bytes of payload), so it is read
        # without the length check that a whole packet gets.
        inner = l4[8:]
        if len(inner) < 28 or inner[0] >> 4 != 4:
            return None
        i_ihl = (inner[0] & 0x0F) * 4
        i_proto = inner[9]
        i_src, i_dst = inner[12:16], inner[16:20]
        if i_ihl < 20 or i_src != self.nat_ip:
            return None
        il4 = inner[i_ihl:]
        if i_proto in (TCP, UDP) and len(il4) >= 4:
            i_sport, i_dport = struct.unpack("!HH", il4[:4])
            ent = self.back.get((i_proto, i_dst, i_dport, i_sport))
            if ent is None:
                return None
            new_inner = _inner_fix(inner, i_ihl, ent["pc_ip"], ent["pc_port"])
        elif i_proto == ICMP and len(il4) >= 8 and il4[0] == ICMP_ECHO:
            ident = struct.unpack("!H", il4[4:6])[0]
            ent = self.back.get((ICMP, i_dst, 0, ident))
            if ent is None:
                return None
            new_inner = bytearray(inner)
            new_inner[i_ihl + 4:i_ihl + 6] = struct.pack("!H", ent["pc_port"])
            new_inner[10:12] = b"\x00\x00"
            new_inner[10:12] = struct.pack("!H", checksum(bytes(new_inner[:i_ihl])))
            new_inner = bytes(new_inner)
        else:
            return None
        msg = bytearray(l4[:8]) + new_inner
        msg[2:4] = b"\x00\x00"
        msg[2:4] = struct.pack("!H", checksum(bytes(msg)))
        return _rewrite(pkt[:ihl] + bytes(msg), ihl, ihl + len(msg), ICMP,
                        dst=ent["pc_ip"], dec_ttl=True)

    def _arp_wan(self, frame):
        """Answer ARP for NAT_IP; learn the gateway's MAC from its replies."""
        a = frame[14:42]
        if len(a) < 28:
            return None
        oper = struct.unpack("!H", a[6:8])[0]
        sha, spa, tpa = a[8:14], a[14:18], a[24:28]
        if oper == 1 and tpa == self.nat_ip:
            reply = (sha + self.wan_mac + b"\x08\x06"
                     + struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2)
                     + self.wan_mac + self.nat_ip + sha + spa)
            return reply, "wan"
        if oper == 2 and spa == self.wan_gw:
            self.set_gateway_mac(sha)
        return None


# -- the device loop ------------------------------------------------------------

def _mac(ifname):
    with open("/sys/class/net/%s/address" % ifname) as f:
        return bytes.fromhex(f.read().strip().replace(":", ""))


def _gateway_mac_from_kernel(wan_if, gw_ip):
    """The MAC the kernel already has for the gateway, from /proc/net/arp."""
    try:
        with open("/proc/net/arp") as f:
            for ln in f.readlines()[1:]:
                p = ln.split()
                if len(p) >= 6 and p[0] == gw_ip and p[5] == wan_if and p[3] != "00:00:00:00:00:00":
                    return bytes.fromhex(p[3].replace(":", ""))
    except OSError:
        pass
    return None


def _ethtool_value(ifname, cmd, data=0):
    """One ETHTOOL_* call (struct ethtool_value {cmd, data}); returns the data field."""
    buf = ctypes.create_string_buffer(struct.pack("II", cmd, data), 8)
    ifr = bytearray(ifname.encode().ljust(16, b"\0") + struct.pack("I", ctypes.addressof(buf)) + b"\0" * 12)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        fcntl.ioctl(s.fileno(), SIOCETHTOOL, ifr)
    finally:
        s.close()
    return struct.unpack("II", buf.raw)[1]


def gro_off(ifname):
    """Switch generic receive offload off on ifname, so every received frame is one wire frame."""
    before = _ethtool_value(ifname, ETHTOOL_GGRO)
    if before:
        _ethtool_value(ifname, ETHTOOL_SGRO, 0)
    print("natd: GRO on %s was %d, now %d" % (ifname, before, _ethtool_value(ifname, ETHTOOL_GGRO)), flush=True)


# -- batched send over the raw sockets ------------------------------------------
#
# struct iovec   { void *iov_base; size_t iov_len; }
# struct msghdr  { void *msg_name; socklen_t msg_namelen; void *msg_iov; size_t msg_iovlen;
#                  void *msg_control; size_t msg_controllen; int msg_flags; }
# struct mmsghdr { struct msghdr msg_hdr; unsigned int msg_len; }
# ctypes lays these out for the running ABI (32-bit ARM here), so no hand-rolled offsets.

class _iovec(ctypes.Structure):
    _fields_ = [("iov_base", ctypes.c_void_p), ("iov_len", ctypes.c_size_t)]


class _msghdr(ctypes.Structure):
    _fields_ = [("msg_name", ctypes.c_void_p),
                ("msg_namelen", ctypes.c_uint32),
                ("msg_iov", ctypes.c_void_p),
                ("msg_iovlen", ctypes.c_size_t),
                ("msg_control", ctypes.c_void_p),
                ("msg_controllen", ctypes.c_size_t),
                ("msg_flags", ctypes.c_int)]


class _mmsghdr(ctypes.Structure):
    _fields_ = [("msg_hdr", _msghdr), ("msg_len", ctypes.c_uint)]


def _load_sendmmsg():
    """libc.sendmmsg prototyped, or None if this platform has no such symbol."""
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        fn = libc.sendmmsg
    except (OSError, AttributeError):
        return None
    fn.argtypes = [ctypes.c_int, ctypes.POINTER(_mmsghdr), ctypes.c_uint, ctypes.c_int]
    fn.restype = ctypes.c_int
    return fn


_SENDMMSG = _load_sendmmsg() if USE_SENDMMSG else None


def _sendmmsg_once(fn, fd, frames):
    """Send frames[0:] in one sendmmsg syscall. Returns how many were accepted (>=1), or
    raises OSError if even the first could not be sent. Buffers are kept alive for the call."""
    n = len(frames)
    bufs = [ctypes.create_string_buffer(f, len(f)) for f in frames]
    iovs = (_iovec * n)()
    msgs = (_mmsghdr * n)()
    for i in range(n):
        iovs[i].iov_base = ctypes.cast(bufs[i], ctypes.c_void_p)
        iovs[i].iov_len = len(frames[i])
        msgs[i].msg_hdr.msg_iov = ctypes.cast(ctypes.byref(iovs[i]), ctypes.c_void_p)
        msgs[i].msg_hdr.msg_iovlen = 1
    sent = fn(fd, msgs, n, 0)
    if sent < 0:
        raise OSError(ctypes.get_errno(), "sendmmsg")
    return sent


def send_batch(sock, frames):
    """Send every frame in frames on sock, as one burst. Uses sendmmsg when available and
    falls back to blocking send() for anything it did not take. Returns frames sent OK."""
    if not frames:
        return 0
    fn = _SENDMMSG
    done = 0
    if fn is not None:
        try:
            done = _sendmmsg_once(fn, sock.fileno(), frames)
        except OSError as e:
            print("natd: sendmmsg failed (%s); using send()" % e, flush=True)
            done = 0
    ok = done
    for f in frames[done:]:
        try:
            sock.send(f)
            ok += 1
        except OSError as e:
            print("natd: send failed: %s" % e, flush=True)
    return ok


def drain_socket(sock, cap):
    """Read up to cap incoming frames from sock without blocking (MSG_DONTWAIT). Our own sent
    copies (PACKET_OUTGOING) are skipped; they also stop appearing once PACKET_IGNORE_OUTGOING
    is set. Returns the list of received frames, oldest first."""
    frames = []
    recvfrom = sock.recvfrom
    for _ in range(cap):
        try:
            frame, addr = recvfrom(4096, socket.MSG_DONTWAIT)   # frames are <= 1514 bytes; a longer one is dropped by _parse
        except BlockingIOError:
            break
        except OSError:
            break
        if addr[2] == PACKET_OUTGOING:
            continue
        frames.append(frame)
    return frames


def _open_raw(ifname):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((ifname, 0))
    try:
        s.setsockopt(SOL_PACKET, PACKET_IGNORE_OUTGOING, 1)
    except OSError as e:
        print("natd: PACKET_IGNORE_OUTGOING unavailable on %s: %s" % (ifname, e), flush=True)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RCVBUF)
    except OSError as e:
        print("natd: could not raise SO_RCVBUF on %s: %s" % (ifname, e), flush=True)
    got = s.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
    print("natd: %s SO_RCVBUF=%d (kernel-reported, doubled)" % (ifname, got), flush=True)
    return s


def main(argv):
    pc_if, wan_if, nat_ip, wan_gw, pc_net = argv[1:6]
    nat = Nat(nat_ip, wan_gw, pc_net, _mac(pc_if), _mac(wan_if))
    try:
        gro_off(wan_if)
    except OSError as e:
        print("natd: could not switch GRO off on %s: %s" % (wan_if, e), flush=True)
    pc = _open_raw(pc_if)
    wan = _open_raw(wan_if)
    print("natd: %s -> %s as %s via %s (batch=%d, sendmmsg=%s)"
          % (pc_if, wan_if, nat_ip, wan_gw, DRAIN_CAP, _SENDMMSG is not None), flush=True)
    last_house = last_arp = last_stats = 0.0
    while True:
        r, _, _ = select.select([pc, wan], [], [], 1.0)
        now = time.monotonic()
        to_pc = []
        to_wan = []
        for s in r:
            side = "pc" if s is pc else "wan"
            for frame in drain_socket(s, DRAIN_CAP):
                res = nat.handle(frame, side, now)
                if res:
                    out, out_side = res
                    (to_pc if out_side == "pc" else to_wan).append(out)
        if to_wan:
            send_batch(wan, to_wan)
        if to_pc:
            send_batch(pc, to_pc)
        if now - last_house >= 5:
            last_house = now
            nat.expire(now)
            if nat.need_gateway():
                mac = _gateway_mac_from_kernel(wan_if, wan_gw)
                if mac:
                    nat.set_gateway_mac(mac)
                    print("natd: gateway %s is %s" % (wan_gw, mac.hex(":")), flush=True)
        if nat.need_gateway() and now - last_arp >= 1:
            last_arp = now
            wan.send(nat.arp_request_for_gateway())
        if now - last_stats >= 60:
            last_stats = now
            print("natd: %s drops %s" % (nat.counts, nat.drops), flush=True)


if __name__ == "__main__":
    main(sys.argv)
