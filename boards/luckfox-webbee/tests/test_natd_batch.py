"""Batching-logic tests for natd-batch.py. No AF_PACKET needed: drain/grouping use fake
sockets, and the sendmmsg path is proved over a loopback UDP socket (host ABI), which
exercises the exact mmsghdr/iovec ctypes layout the daemon uses."""
import importlib.util
import socket
import struct
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent


def _load():
    spec = importlib.util.spec_from_file_location("natd_batch", HERE.parent / "onboard" / "natd.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


natd = _load()
TCP, UDP = natd.TCP, natd.UDP
checksum = natd.checksum
NAT_IP = "192.168.178.250"
PC_IP = socket.inet_aton("172.32.0.114")
REMOTE = socket.inet_aton("93.184.216.34")
NAT_B = socket.inet_aton(NAT_IP)
USB_MAC = bytes.fromhex("eed43ccb3b8e")
WAN_MAC = bytes.fromhex("263b5bb70e7a")
GW_MAC = bytes.fromhex("0c7274f7340f")
PC_MAC = bytes.fromhex("ce6ed65a25d3")


# -- frame builders (same wire format the repo test uses) -----------------------

def ip_packet(src, dst, proto, payload, ttl=64):
    hdr = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 7, 0, ttl, proto, 0, src, dst))
    hdr[10:12] = struct.pack("!H", checksum(bytes(hdr)))
    return bytes(hdr) + payload


def pseudo(src, dst, proto, seg):
    return src + dst + struct.pack("!BBH", 0, proto, len(seg))


def tcp_seg(src, dst, sport, dport, payload=b"", flags=0x18):
    seg = bytearray(struct.pack("!HHIIBBHHH", sport, dport, 1, 1, 5 << 4, flags, 65535, 0, 0) + payload)
    seg[16:18] = struct.pack("!H", checksum(pseudo(src, dst, TCP, bytes(seg)) + bytes(seg)))
    return bytes(seg)


def eth(dst, src, pkt):
    return dst + src + b"\x08\x00" + pkt


def new_nat():
    nat = natd.Nat(NAT_IP, "192.168.178.1", "172.32.0.0/16", USB_MAC, WAN_MAC)
    nat.set_gateway_mac(GW_MAC)
    return nat


def ip_ok(pkt):
    ihl = (pkt[0] & 0x0F) * 4
    return checksum(pkt[:ihl]) == 0


# -- fakes ----------------------------------------------------------------------

class FakeRecvSock:
    """Yields queued (frame, pkttype) then raises BlockingIOError, like MSG_DONTWAIT."""
    def __init__(self, items):
        self.q = list(items)
        self.calls = 0

    def recvfrom(self, bufsize, flags=0):
        self.calls += 1
        if not self.q:
            raise BlockingIOError(11, "EAGAIN")
        frame, pkttype = self.q.pop(0)
        return frame, ("iface", 0, pkttype, 0, b"")


class FakeErrSock:
    def __init__(self, first):
        self.first = first
        self.done = False

    def recvfrom(self, bufsize, flags=0):
        if not self.done:
            self.done = True
            return self.first, ("iface", 0, 0, 0, b"")
        raise OSError(22, "boom")


class FakeSendSock:
    def __init__(self, fail_indices=()):
        self.sent = []
        self.fail = set(fail_indices)
        self._n = 0

    def send(self, data):
        if self._n in self.fail:
            self._n += 1
            raise OSError("send fail")
        self._n += 1
        self.sent.append(bytes(data))
        return len(data)


# -- drain_socket ---------------------------------------------------------------

def test_drain_skips_outgoing_and_stops_on_eagain():
    sock = FakeRecvSock([(b"in1", 0), (b"mine", natd.PACKET_OUTGOING), (b"in2", 0)])
    frames = natd.drain_socket(sock, natd.DRAIN_CAP)
    assert frames == [b"in1", b"in2"]            # our own outgoing copy skipped, order kept


def test_drain_respects_cap_and_leaves_the_rest():
    sock = FakeRecvSock([(bytes([i]), 0) for i in range(100)])
    frames = natd.drain_socket(sock, 10)
    assert len(frames) == 10                     # bounded by the cap
    assert sock.calls == 10                      # no over-reading past the cap
    assert len(sock.q) == 90                      # the rest stays buffered for the next pass


def test_drain_stops_on_oserror():
    sock = FakeErrSock(b"only")
    assert natd.drain_socket(sock, natd.DRAIN_CAP) == [b"only"]


# -- send_batch (fallback path) -------------------------------------------------

def test_send_batch_fallback_sends_all_in_order(monkeypatch):
    monkeypatch.setattr(natd, "_SENDMMSG", None)
    sock = FakeSendSock()
    frames = [b"a", b"bb", b"ccc"]
    assert natd.send_batch(sock, frames) == 3
    assert sock.sent == frames


def test_send_batch_empty_is_a_noop():
    sock = FakeSendSock()
    assert natd.send_batch(sock, []) == 0
    assert sock.sent == []


def test_send_batch_counts_only_what_went_out(monkeypatch):
    monkeypatch.setattr(natd, "_SENDMMSG", None)
    sock = FakeSendSock(fail_indices={1})        # second frame fails
    assert natd.send_batch(sock, [b"a", b"b", b"c"]) == 2
    assert sock.sent == [b"a", b"c"]


# -- sendmmsg path over real loopback UDP (proves the ctypes struct layout) ------

def test_sendmmsg_once_delivers_all_in_order():
    if natd._SENDMMSG is None:
        pytest.skip("no sendmmsg on this platform")
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    rx.settimeout(2.0)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.connect(rx.getsockname())
    try:
        frames = [b"one", b"two", b"three", b"x" * 1400]
        sent = natd._sendmmsg_once(natd._SENDMMSG, tx.fileno(), frames)
        assert sent == len(frames)
        got = [rx.recv(2000) for _ in frames]
        assert got == frames
    finally:
        rx.close()
        tx.close()


def test_send_batch_over_real_socket_delivers_every_frame():
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    rx.settimeout(2.0)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.connect(rx.getsockname())
    try:
        frames = [bytes([i]) * (i + 1) for i in range(1, 20)]
        assert natd.send_batch(tx, frames) == len(frames)
        got = [rx.recv(2000) for _ in frames]
        assert got == frames                     # all arrive, in order, via sendmmsg or fallback
    finally:
        rx.close()
        tx.close()


# -- loop-body grouping with the real Nat (batching + NAT correctness together) --

def route_once(nat, readable, now):
    """The candidate's loop body, extracted: drain each readable (sock, side) and group."""
    to_pc, to_wan = [], []
    for sock, side in readable:
        for frame in natd.drain_socket(sock, natd.DRAIN_CAP):
            res = nat.handle(frame, side, now)
            if res:
                out, out_side = res
                (to_pc if out_side == "pc" else to_wan).append(out)
    return to_pc, to_wan


def test_grouping_routes_pc_frames_to_the_wan_batch():
    nat = new_nat()
    f1 = eth(USB_MAC, PC_MAC, ip_packet(PC_IP, REMOTE, TCP, tcp_seg(PC_IP, REMOTE, 51000, 443, flags=0x02)))
    f2 = eth(USB_MAC, PC_MAC, ip_packet(PC_IP, REMOTE, TCP, tcp_seg(PC_IP, REMOTE, 51001, 443, flags=0x02)))
    pc = FakeRecvSock([(f1, 0), (f2, 0)])
    to_pc, to_wan = route_once(nat, [(pc, "pc")], 100.0)
    assert to_pc == [] and len(to_wan) == 2
    for out in to_wan:
        assert out[0:6] == GW_MAC and out[6:12] == WAN_MAC
        op = out[14:]
        assert op[12:16] == NAT_B and ip_ok(op)


def test_grouping_routes_wan_reply_to_the_pc_batch():
    nat = new_nat()
    syn = eth(USB_MAC, PC_MAC, ip_packet(PC_IP, REMOTE, TCP, tcp_seg(PC_IP, REMOTE, 51000, 443, flags=0x02)))
    out = nat.handle(syn, "pc", 100.0)[0]
    nat_port = struct.unpack("!H", out[14 + 20:14 + 22])[0]
    reply = eth(WAN_MAC, GW_MAC, ip_packet(REMOTE, NAT_B, TCP, tcp_seg(REMOTE, NAT_B, 443, nat_port, payload=b"ok")))
    wan = FakeRecvSock([(reply, 0)])
    to_pc, to_wan = route_once(nat, [(wan, "wan")], 101.0)
    assert to_wan == [] and len(to_pc) == 1
    bp = to_pc[0][14:]
    assert to_pc[0][0:6] == PC_MAC and bp[16:20] == PC_IP and ip_ok(bp)


def test_grouping_drops_are_not_queued():
    nat = new_nat()
    gre = eth(USB_MAC, PC_MAC, ip_packet(PC_IP, REMOTE, 47, b"\x00" * 8))   # not TCP/UDP/ICMP
    pc = FakeRecvSock([(gre, 0)])
    to_pc, to_wan = route_once(nat, [(pc, "pc")], 1.0)
    assert to_pc == [] and to_wan == []
