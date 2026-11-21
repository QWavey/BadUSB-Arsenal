"""Frame parser edge cases in sniff.classify: fragments, bad header lengths, truncated frames."""
import importlib.util
import random
from pathlib import Path

SNIFF = Path(__file__).resolve().parents[1] / "onboard" / "sniff.py"


def _load():
    spec = importlib.util.spec_from_file_location("sniff_parse_under_test", SNIFF)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _ipv4_tcp(frag_field=0x0000, ihl_byte=0x45, tcp_off_byte=0x50, payload=b""):
    """An Ethernet + IPv4 + TCP frame. The TCP header is 20 bytes; sport 12345, dport 80."""
    eth = b"\x00" * 12 + b"\x08\x00"
    ip = bytearray(20)
    ip[0] = ihl_byte
    ip[9] = 6                                   # protocol TCP
    ip[12:16] = bytes([10, 0, 0, 1])
    ip[16:20] = bytes([10, 0, 0, 2])
    ip[6] = (frag_field >> 8) & 0xFF
    ip[7] = frag_field & 0xFF
    tcp = bytearray(20)
    tcp[0:2] = (12345).to_bytes(2, "big")
    tcp[2:4] = (80).to_bytes(2, "big")
    tcp[12] = tcp_off_byte
    return eth + bytes(ip) + bytes(tcp) + payload


HTTP = b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"


def test_a_normal_tcp_request_gives_its_host():
    sn = _load()
    v = sn.classify(_ipv4_tcp(payload=HTTP), [])
    assert v.get("host") == "example.com"


def test_a_non_first_fragment_is_not_parsed_as_tcp():
    sn = _load()
    # Fragment offset 1 (in 8-byte units): the bytes here are data, not a TCP header.
    v = sn.classify(_ipv4_tcp(frag_field=0x0001, payload=HTTP), [])
    assert "host" not in v and "rule" not in v


def test_an_ihl_below_five_is_not_parsed():
    sn = _load()
    v = sn.classify(_ipv4_tcp(ihl_byte=0x40, payload=HTTP), [])
    assert v == {"flow": "00000000"}


def test_a_tcp_data_offset_below_five_is_not_parsed():
    sn = _load()
    v = sn.classify(_ipv4_tcp(tcp_off_byte=0x30, payload=HTTP), [])
    assert "host" not in v


def test_truncated_and_random_frames_never_raise():
    sn = _load()
    frame = _ipv4_tcp(payload=HTTP)
    for n in range(len(frame) + 1):
        sn.classify(frame[:n], [])                  # every prefix of a real frame
    rng = random.Random(7)
    for _ in range(2000):
        junk = bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 120)))
        sn.classify(junk, ["example.com"])
