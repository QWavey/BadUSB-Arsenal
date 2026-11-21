"""DHCP parsing and LAN summary in onboard/lan_detect.py, driven by hand-built frames.

No raw socket is opened, so this runs without root and without a real interface. The
detect_lan loop is exercised with a fake socket that replays captured frames."""
import importlib.util
import json
import random
import socket
import struct
import time
from pathlib import Path

import pytest

MODULE = Path(__file__).resolve().parents[1] / "onboard" / "lan_detect.py"


def _load():
    spec = importlib.util.spec_from_file_location("lan_detect_under_test", MODULE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lan = _load()


def _options(*pairs):
    """(code, value) pairs as a DHCP options area, ended by END (255)."""
    out = b""
    for code, val in pairs:
        out += bytes([code, len(val)]) + val
    return out + b"\xff"


def _bootp(yiaddr, options, op=2, xid=0xDEADBEEF, chaddr=bytes.fromhex("02000000aa01")):
    """BOOTP fixed header (236 bytes), magic cookie, then the options."""
    fixed = struct.pack("!BBBBIHH", op, 1, len(chaddr), 0, xid, 0, 0x8000)
    fixed += b"\x00" * 4                          # ciaddr
    fixed += socket.inet_aton(yiaddr)             # yiaddr: the address offered
    fixed += socket.inet_aton("192.168.178.1")    # siaddr
    fixed += b"\x00" * 4                          # giaddr
    fixed += chaddr.ljust(16, b"\x00")            # chaddr
    fixed += b"\x00" * 64 + b"\x00" * 128         # sname, file
    return fixed + b"\x63\x82\x53\x63" + options  # magic cookie 63.82.53.63


def _frame(payload, sport=67, dport=68, proto=17, ethertype=0x0800):
    """Ethernet II + IPv4 (20 bytes, no options) + UDP + payload."""
    udp = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload
    ip = bytearray(20)
    ip[0] = 0x45
    struct.pack_into("!H", ip, 2, 20 + len(udp))
    ip[8] = 64
    ip[9] = proto
    ip[12:16] = socket.inet_aton("192.168.178.1")
    ip[16:20] = socket.inet_aton("255.255.255.255")
    eth = bytes.fromhex("0200000000aa") + bytes.fromhex("001122334455") + struct.pack("!H", ethertype)
    return eth + bytes(ip) + udp


def _offer(yiaddr="192.168.178.87", mtype=2):
    """A DHCPOFFER (type 2) by default: mask 255.255.255.0, router and server 192.168.178.1."""
    opts = _options(
        (53, bytes([mtype])),
        (1, socket.inet_aton("255.255.255.0")),
        (3, socket.inet_aton("192.168.178.1")),
        (51, struct.pack("!I", 43200)),
        (54, socket.inet_aton("192.168.178.1")),
    )
    return _frame(_bootp(yiaddr, opts))


class _FakeSock:
    """Replays frames, then times out like a socket with settimeout() would."""

    def __init__(self, frames):
        self.frames = list(frames)
        self.closed = False

    def recv(self, n):
        if self.frames:
            return self.frames.pop(0)
        time.sleep(0.005)
        raise socket.timeout("timed out")

    def close(self):
        self.closed = True


def test_offer_frame_parses_to_expected_fields():
    info = lan._parse_dhcp_packet(_offer())
    assert info is not None
    assert info["op"] == 2
    assert info["xid"] == 0xDEADBEEF
    assert info["src_port"] == 67 and info["dst_port"] == 68
    assert info["msg_type"] == 2
    assert info["yiaddr"] == "192.168.178.87"
    assert info["subnet_mask"] == "255.255.255.0"
    assert info["router"] == "192.168.178.1"
    assert info["server_id"] == "192.168.178.1"
    assert info["chaddr"] == "02:00:00:00:aa:01"


def test_offer_summarizes_to_expected_lan():
    info = lan._parse_dhcp_packet(_offer())
    assert lan._summarize([info], "eth0") == {
        "detected": True,
        "subnet": "192.168.178.0/24",
        "gateway": "192.168.178.1",
        "dhcp_pool_hint": ["192.168.178.87"],
        "suggested_ip": "192.168.178.250",
        "iface": "eth0",
    }


def test_suggestion_skips_observed_addresses():
    obs = [lan._parse_dhcp_packet(_offer(yi)) for yi in ("192.168.178.250", "192.168.178.251")]
    result = lan._summarize(obs, "eth0")
    assert result["suggested_ip"] == "192.168.178.252"
    assert result["dhcp_pool_hint"] == ["192.168.178.250", "192.168.178.251"]


def test_ack_counts_nak_does_not():
    ack = lan._parse_dhcp_packet(_offer("192.168.178.87", mtype=5))
    nak = lan._parse_dhcp_packet(_offer("0.0.0.0", mtype=6))
    assert lan._summarize([nak], "eth0") is None
    assert lan._summarize([nak, ack], "eth0")["dhcp_pool_hint"] == ["192.168.178.87"]


def test_missing_mask_and_router_fall_back():
    opts = _options((53, b"\x02"), (54, socket.inet_aton("10.0.0.1")))
    info = lan._parse_dhcp_packet(_frame(_bootp("10.0.0.50", opts)))
    result = lan._summarize([info], "eth0")
    assert result["subnet"] == "10.0.0.0/24"
    assert result["gateway"] == "10.0.0.1"
    assert result["suggested_ip"] == "10.0.0.250"


def test_non_dhcp_frames_are_rejected():
    payload = _bootp("192.168.178.87", _options((53, b"\x02")))
    assert lan._parse_dhcp_packet(_frame(payload, sport=53, dport=5353)) is None   # not ports 67/68
    assert lan._parse_dhcp_packet(_frame(payload, proto=6)) is None                # TCP, not UDP
    assert lan._parse_dhcp_packet(_frame(payload, ethertype=0x86DD)) is None       # IPv6
    bad_cookie = bytearray(payload)
    bad_cookie[236:240] = b"\x00" * 4
    assert lan._parse_dhcp_packet(_frame(bytes(bad_cookie))) is None
    assert lan._parse_dhcp_packet(b"\x00" * 30) is None


def test_truncated_and_mutated_frames_never_raise():
    frame = _offer()
    for n in range(len(frame)):
        lan._parse_dhcp_packet(frame[:n])
    rng = random.Random(1234)
    for _ in range(2000):
        mutated = bytearray(frame)
        for _ in range(rng.randrange(1, 6)):
            mutated[rng.randrange(len(mutated))] = rng.getrandbits(8)
        info = lan._parse_dhcp_packet(bytes(mutated))
        assert info is None or isinstance(info["yiaddr"], str)


def test_detect_lan_loop_returns_schema_with_detected_at(monkeypatch, tmp_path):
    monkeypatch.setattr(lan, "LOG", tmp_path / "log.txt")
    monkeypatch.setattr(lan, "LINGER_AFTER_COMPLETE_S", 0.0)
    monkeypatch.setattr(lan.socket, "AF_PACKET", 17, raising=False)  # so the check passes off Linux
    frames = [b"\x00" * 30, _frame(b"x" * 300, sport=53, dport=5353), _offer("192.168.178.87")]
    monkeypatch.setattr(lan, "_open_capture", lambda iface: _FakeSock(frames))
    result = lan.detect_lan("eth0", timeout=5)
    assert list(result) == ["detected", "subnet", "gateway", "dhcp_pool_hint",
                            "suggested_ip", "iface", "detected_at"]
    assert result["subnet"] == "192.168.178.0/24"
    assert result["gateway"] == "192.168.178.1"
    assert isinstance(result["detected_at"], int)


def test_detect_lan_returns_none_when_nothing_arrives(monkeypatch, tmp_path):
    monkeypatch.setattr(lan, "LOG", tmp_path / "log.txt")
    monkeypatch.setattr(lan.socket, "AF_PACKET", 17, raising=False)
    monkeypatch.setattr(lan, "_open_capture", lambda iface: _FakeSock([]))
    assert lan.detect_lan("eth0", timeout=0.2) is None


def test_open_capture_without_root_returns_none(monkeypatch, tmp_path):
    monkeypatch.setattr(lan, "LOG", tmp_path / "log.txt")
    monkeypatch.setattr(lan.socket, "AF_PACKET", 17, raising=False)

    def refuse(*args, **kwargs):
        raise PermissionError(1, "Operation not permitted")

    monkeypatch.setattr(lan.socket, "socket", refuse)
    assert lan.detect_lan("eth0", timeout=0.1) is None
    assert "socket" in (tmp_path / "log.txt").read_text()


def test_write_state_round_trips_and_leaves_no_temp_file(tmp_path):
    result = lan._summarize([lan._parse_dhcp_packet(_offer())], "eth0")
    result["detected_at"] = 1234567890
    target = tmp_path / "ghostlink.lan.json"
    lan.write_state(result, str(target))
    assert json.loads(target.read_text()) == result
    assert [p.name for p in tmp_path.iterdir()] == ["ghostlink.lan.json"]


def test_write_state_failure_is_logged_not_raised(tmp_path, monkeypatch):
    log = tmp_path / "log.txt"
    monkeypatch.setattr(lan, "LOG", log)
    lan.write_state({"detected": True}, str(tmp_path / "missing" / "state.json"))
    assert "write_state" in log.read_text()
