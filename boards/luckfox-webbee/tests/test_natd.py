"""Userspace NAT (onboard/natd.py): translation in both directions, with every checksum re-checked
from scratch, ICMP errors about translated packets, ARP for the NAT address, and what is dropped."""
import importlib.util
import random
import socket
import struct
from pathlib import Path

NATD = Path(__file__).resolve().parents[1] / "onboard" / "natd.py"


def _load():
    spec = importlib.util.spec_from_file_location("natd_under_test", NATD)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


natd = _load()
TCP, UDP, ICMP = natd.TCP, natd.UDP, natd.ICMP
NAT_IP = "192.168.178.250"
PC_IP = socket.inet_aton("172.32.0.114")
REMOTE = socket.inet_aton("93.184.216.34")
NAT_B = socket.inet_aton(NAT_IP)
USB_MAC = bytes.fromhex("eed43ccb3b8e")
WAN_MAC = bytes.fromhex("263b5bb70e7a")
GW_MAC = bytes.fromhex("0c7274f7340f")
PC_MAC = bytes.fromhex("ce6ed65a25d3")
checksum = natd.checksum


def ip_packet(src, dst, proto, payload, ttl=64, frag=0):
    hdr = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 7, frag, ttl, proto, 0, src, dst))
    hdr[10:12] = struct.pack("!H", checksum(bytes(hdr)))
    return bytes(hdr) + payload


def pseudo(src, dst, proto, seg):
    return src + dst + struct.pack("!BBH", 0, proto, len(seg))


def tcp_seg(src, dst, sport, dport, payload=b"", flags=0x18):
    seg = bytearray(struct.pack("!HHIIBBHHH", sport, dport, 1, 1, 5 << 4, flags, 65535, 0, 0) + payload)
    seg[16:18] = struct.pack("!H", checksum(pseudo(src, dst, TCP, bytes(seg)) + bytes(seg)))
    return bytes(seg)


def udp_seg(src, dst, sport, dport, payload=b"hello"):
    seg = bytearray(struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload)
    c = checksum(pseudo(src, dst, UDP, bytes(seg)) + bytes(seg))
    seg[6:8] = struct.pack("!H", c or 0xFFFF)
    return bytes(seg)


def icmp_msg(typ, code, rest, payload=b""):
    msg = bytearray(struct.pack("!BBHHH", typ, code, 0, *rest) + payload) if len(rest) == 2 else \
        bytearray(struct.pack("!BBHI", typ, code, 0, rest[0]) + payload)
    msg[2:4] = struct.pack("!H", checksum(bytes(msg)))
    return bytes(msg)


def eth(dst, src, pkt):
    return dst + src + b"\x08\x00" + pkt


def ip_ok(pkt):
    ihl = (pkt[0] & 0x0F) * 4
    return checksum(pkt[:ihl]) == 0


def l4_ok(pkt, proto):
    ihl = (pkt[0] & 0x0F) * 4
    total = struct.unpack("!H", pkt[2:4])[0]
    seg = pkt[ihl:total]
    if proto == ICMP:
        return checksum(seg) == 0
    if proto == UDP and seg[6:8] == b"\x00\x00":
        return True
    return checksum(pseudo(pkt[12:16], pkt[16:20], proto, seg) + seg) == 0


def new_nat():
    nat = natd.Nat(NAT_IP, "192.168.178.1", "172.32.0.0/16", USB_MAC, WAN_MAC)
    nat.set_gateway_mac(GW_MAC)
    return nat


def test_tcp_from_the_pc_is_rewritten_with_valid_checksums():
    nat = new_nat()
    pkt = ip_packet(PC_IP, REMOTE, TCP, tcp_seg(PC_IP, REMOTE, 51000, 443, flags=0x02))
    out, side = nat.handle(eth(USB_MAC, PC_MAC, pkt), "pc", 100.0)
    assert side == "wan"
    assert out[0:6] == GW_MAC and out[6:12] == WAN_MAC
    op = out[14:]
    assert op[12:16] == NAT_B and op[16:20] == REMOTE
    assert op[8] == 63                                       # TTL decremented
    assert ip_ok(op) and l4_ok(op, TCP)
    assert 40000 <= struct.unpack("!H", op[20:22])[0] < 60000


def test_tcp_reply_goes_back_to_the_pc():
    nat = new_nat()
    pkt = ip_packet(PC_IP, REMOTE, TCP, tcp_seg(PC_IP, REMOTE, 51000, 443, flags=0x02))
    out, _ = nat.handle(eth(USB_MAC, PC_MAC, pkt), "pc", 100.0)
    nat_port = struct.unpack("!H", out[14 + 20:14 + 22])[0]
    reply = ip_packet(REMOTE, NAT_B, TCP, tcp_seg(REMOTE, NAT_B, 443, nat_port, payload=b"HTTP/1.1 200"))
    back, side = nat.handle(eth(WAN_MAC, GW_MAC, reply), "wan", 101.0)
    assert side == "pc"
    assert back[0:6] == PC_MAC and back[6:12] == USB_MAC
    bp = back[14:]
    assert bp[16:20] == PC_IP and bp[8] == 63
    assert struct.unpack("!H", bp[22:24])[0] == 51000
    assert ip_ok(bp) and l4_ok(bp, TCP)


def test_udp_round_trip():
    nat = new_nat()
    pkt = ip_packet(PC_IP, REMOTE, UDP, udp_seg(PC_IP, REMOTE, 53000, 5353))
    out, _ = nat.handle(eth(USB_MAC, PC_MAC, pkt), "pc", 1.0)
    op = out[14:]
    assert ip_ok(op) and l4_ok(op, UDP)
    nat_port = struct.unpack("!H", op[20:22])[0]
    reply = ip_packet(REMOTE, NAT_B, UDP, udp_seg(REMOTE, NAT_B, 5353, nat_port, b"answer"))
    back, side = nat.handle(eth(WAN_MAC, GW_MAC, reply), "wan", 2.0)
    bp = back[14:]
    assert side == "pc" and bp[16:20] == PC_IP
    assert struct.unpack("!H", bp[22:24])[0] == 53000
    assert ip_ok(bp) and l4_ok(bp, UDP)


def test_icmp_echo_round_trip():
    nat = new_nat()
    echo = struct.pack("!BBHHH", 8, 0, 0, 0x1234, 1) + b"ping-data"
    echo = echo[:2] + struct.pack("!H", checksum(echo)) + echo[4:]
    out, _ = nat.handle(eth(USB_MAC, PC_MAC, ip_packet(PC_IP, REMOTE, ICMP, echo)), "pc", 1.0)
    op = out[14:]
    assert ip_ok(op) and l4_ok(op, ICMP)
    nat_id = struct.unpack("!H", op[24:26])[0]
    rep = struct.pack("!BBHHH", 0, 0, 0, nat_id, 1) + b"ping-data"
    rep = rep[:2] + struct.pack("!H", checksum(rep)) + rep[4:]
    back, side = nat.handle(eth(WAN_MAC, GW_MAC, ip_packet(REMOTE, NAT_B, ICMP, rep)), "wan", 2.0)
    bp = back[14:]
    assert side == "pc" and bp[16:20] == PC_IP
    assert struct.unpack("!H", bp[24:26])[0] == 0x1234
    assert ip_ok(bp) and l4_ok(bp, ICMP)


def test_icmp_error_about_a_translated_packet_is_rewritten():
    nat = new_nat()
    pkt = ip_packet(PC_IP, REMOTE, TCP, tcp_seg(PC_IP, REMOTE, 51000, 443, payload=b"x" * 900))
    out, _ = nat.handle(eth(USB_MAC, PC_MAC, pkt), "pc", 1.0)
    sent = out[14:]
    nat_port = struct.unpack("!H", sent[20:22])[0]
    quote = sent[:28]                                        # header + 8 bytes, as routers quote
    err = struct.pack("!BBHHH", 3, 4, 0, 0, 1400) + quote    # fragmentation needed, MTU 1400
    err = err[:2] + b"\x00\x00" + err[4:]
    err = err[:2] + struct.pack("!H", checksum(err)) + err[4:]
    outer = ip_packet(REMOTE, NAT_B, ICMP, err)
    back, side = nat.handle(eth(WAN_MAC, GW_MAC, outer), "wan", 2.0)
    assert side == "pc"
    bp = back[14:]
    assert bp[16:20] == PC_IP and ip_ok(bp)
    assert checksum(bp[20:]) == 0                            # the ICMP checksum is valid
    inner = bp[28:]
    assert inner[12:16] == PC_IP and ip_ok(inner)
    assert struct.unpack("!H", inner[20:22])[0] == 51000
    assert nat_port != 51000


def test_arp_for_the_nat_address_gets_an_answer():
    nat = new_nat()
    arp = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1) + GW_MAC + socket.inet_aton("192.168.178.1") \
        + b"\x00" * 6 + NAT_B
    reply, side = nat.handle(b"\xff" * 6 + GW_MAC + b"\x08\x06" + arp, "wan", 1.0)
    assert side == "wan"
    assert reply[0:6] == GW_MAC and reply[6:12] == WAN_MAC and reply[12:14] == b"\x08\x06"
    assert reply[14 + 6:14 + 8] == b"\x00\x02" and reply[14 + 14:14 + 18] == NAT_B


def test_the_gateway_mac_is_learned_from_its_arp_reply():
    nat = natd.Nat(NAT_IP, "192.168.178.1", "172.32.0.0/16", USB_MAC, WAN_MAC)
    assert nat.need_gateway()
    body = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2) + GW_MAC + socket.inet_aton("192.168.178.1") \
        + WAN_MAC + NAT_B
    nat.handle(WAN_MAC + GW_MAC + b"\x08\x06" + body, "wan", 1.0)
    assert not nat.need_gateway() and nat.gw_mac == GW_MAC


def test_things_not_to_translate_are_dropped():
    nat = new_nat()
    to_device = ip_packet(PC_IP, socket.inet_aton("172.32.0.93"), UDP,
                          udp_seg(PC_IP, socket.inet_aton("172.32.0.93"), 9000, 53))
    assert nat.handle(eth(USB_MAC, PC_MAC, to_device), "pc", 1.0) is None     # the device itself
    frag = ip_packet(PC_IP, REMOTE, UDP, udp_seg(PC_IP, REMOTE, 1, 2), frag=0x2000)
    assert nat.handle(eth(USB_MAC, PC_MAC, frag), "pc", 1.0) is None         # a fragment
    stray = ip_packet(REMOTE, NAT_B, TCP, tcp_seg(REMOTE, NAT_B, 443, 45000))
    assert nat.handle(eth(WAN_MAC, GW_MAC, stray), "wan", 1.0) is None       # no mapping
    gre = ip_packet(PC_IP, REMOTE, 47, b"\x00" * 8)
    assert nat.handle(eth(USB_MAC, PC_MAC, gre), "pc", 1.0) is None          # not TCP, UDP or ICMP


def test_truncated_tcp_reply_from_wan_is_dropped_not_crashed():
    # Bug-hunt finding: _from_wan only checked len(l4) < 4 before calling _rewrite(), which
    # reads the TCP checksum at a fixed offset (16) into the L4 header without its own length
    # check. A WAN-side TCP segment with 4-19 bytes of L4 data (past the sport/dport check, but
    # short of the full 20-byte TCP header) made struct.unpack() raise -- confirmed: this
    # crashed the whole natd.py process on a single malformed/short packet from the real LAN,
    # killing every PC's connectivity until the ~2s supervisor respawn, repeatably for as long
    # as the malformed packet kept arriving. Must now be dropped cleanly, and the mapping must
    # still work normally afterwards.
    nat = new_nat()
    pkt = ip_packet(PC_IP, REMOTE, TCP, tcp_seg(PC_IP, REMOTE, 51000, 443, flags=0x02))
    out, _ = nat.handle(eth(USB_MAC, PC_MAC, pkt), "pc", 100.0)
    nat_port = struct.unpack("!H", out[14 + 20:14 + 22])[0]

    truncated_l4 = struct.pack("!HH", 443, nat_port) + b"\x00" * 4   # 8 bytes: far short of a full TCP header
    truncated = ip_packet(REMOTE, NAT_B, TCP, truncated_l4)
    assert nat.handle(eth(WAN_MAC, GW_MAC, truncated), "wan", 101.0) is None

    # The mapping must survive the bad packet: a real reply through it still translates.
    reply = ip_packet(REMOTE, NAT_B, TCP, tcp_seg(REMOTE, NAT_B, 443, nat_port, payload=b"ok"))
    back, side = nat.handle(eth(WAN_MAC, GW_MAC, reply), "wan", 102.0)
    assert side == "pc"
    assert ip_ok(back[14:]) and l4_ok(back[14:], TCP)


def test_rewrite_never_raises_on_a_short_tcp_udp_buffer():
    # Defense-in-depth for the same bug class: _rewrite() itself must refuse a buffer too
    # short to hold the checksum field, rather than relying on every caller's pre-check being
    # exactly right.
    short_udp = ip_packet(PC_IP, REMOTE, UDP, struct.pack("!HH", 1, 2))          # 4 bytes, no checksum field
    assert natd._rewrite(short_udp, 20, len(short_udp), UDP, src=NAT_B, sport=100) is None
    short_tcp = ip_packet(PC_IP, REMOTE, TCP, struct.pack("!HH", 1, 2) + b"\x00" * 6)  # 10 bytes, short of offset 16
    assert natd._rewrite(short_tcp, 20, len(short_tcp), TCP, src=NAT_B, sport=100) is None


def test_idle_mappings_expire():
    nat = new_nat()
    pkt = ip_packet(PC_IP, REMOTE, UDP, udp_seg(PC_IP, REMOTE, 53000, 5353))
    out, _ = nat.handle(eth(USB_MAC, PC_MAC, pkt), "pc", 0.0)
    nat_port = struct.unpack("!H", out[14 + 20:14 + 22])[0]
    nat.expire(10_000.0)
    reply = ip_packet(REMOTE, NAT_B, UDP, udp_seg(REMOTE, NAT_B, 5353, nat_port))
    assert nat.handle(eth(WAN_MAC, GW_MAC, reply), "wan", 10_000.0) is None
    assert nat.ports[UDP] == set()


def test_many_random_flows_keep_valid_checksums():
    rng = random.Random(3)
    nat = new_nat()
    for _ in range(400):
        dst = bytes(rng.getrandbits(8) for _ in range(4))
        if dst[0] in (0, 10, 127) or dst[0] >= 224 or dst[:2] == b"\xc0\xa8" or dst[:2] == b"\xac\x1f":
            continue
        sport, dport = rng.randint(1024, 65535), rng.randint(1, 65535)
        if rng.random() < 0.5:
            seg = tcp_seg(PC_IP, dst, sport, dport, payload=bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 60))))
            pkt, proto = ip_packet(PC_IP, dst, TCP, seg), TCP
        else:
            seg = udp_seg(PC_IP, dst, sport, dport, payload=bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 60))))
            pkt, proto = ip_packet(PC_IP, dst, UDP, seg), UDP
        res = nat.handle(eth(USB_MAC, PC_MAC, pkt), "pc", 1.0)
        assert res is not None
        op = res[0][14:]
        assert ip_ok(op) and l4_ok(op, proto)
