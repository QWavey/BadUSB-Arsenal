"""Regression tests for the sniffer's verdict writer and flow hash.

The writer formats the verdict line of frames that match no rule by hand, and
buffers its writes. These tests check that the bytes on disk are the same JSON
json.dumps would give, and that flush() makes buffered lines visible.
"""
import importlib.util
import json
import random
from pathlib import Path

SNIFF = Path(__file__).resolve().parents[1] / "onboard" / "sniff.py"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, SNIFF)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _old_flow_hash(buf):
    """The flow hash as sniff.py computed it before the struct change."""
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


def _random_frame(rng):
    ihl = rng.choice([5, 5, 5, 6, 7, 15])
    ip_hdr = bytearray(rng.getrandbits(8) for _ in range(ihl * 4))
    ip_hdr[0] = 0x40 | ihl
    body = bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 40)))
    eth = bytes(rng.getrandbits(8) for _ in range(12)) + (b"\x08\x00" if rng.random() < 0.8 else b"\x86\xdd")
    return eth + bytes(ip_hdr) + body


def test_flow_hash_matches_previous_implementation():
    sn = _load("sniff_flow_under_test")
    rng = random.Random(11)
    for _ in range(3000):
        buf = _random_frame(rng)
        assert sn.flow_hash(buf) == _old_flow_hash(buf), buf.hex()


def test_verdict_lines_are_the_json_dumps_would_write(tmp_path, monkeypatch):
    sn = _load("sniff_writer_under_test")
    monkeypatch.setattr(sn, "REC_DIR", tmp_path / "rec")
    monkeypatch.setattr(sn, "VER_DIR", tmp_path / "ver")
    w = sn.Writer({"RECORD_PCAP": "FALSE", "VERDICT_STREAM": "TRUE"})
    rng = random.Random(5)
    expected = []
    for i in range(200):
        buf = _random_frame(rng)
        # Half the frames matched a rule; the writer keeps only those and drops the rest.
        v = {"rule": "HTTP", "host": "amazon.de"} if i % 2 == 0 else sn.classify(buf, ["amazon.de"])
        if v.get("rule"):
            expected_v = dict(v, f=i + 1, t=f"1700000000.{i:06d}", sz=len(buf))
            expected.append(json.dumps(expected_v, separators=(",", ":")))
        w.write(1700000000, i, buf, v)
    w.close()
    lines = (tmp_path / "ver" / "rec_000000.jsonl").read_text().splitlines()
    assert len(expected) == 100
    assert lines == expected


def test_flush_makes_buffered_lines_visible(tmp_path, monkeypatch):
    sn = _load("sniff_flush_under_test")
    monkeypatch.setattr(sn, "REC_DIR", tmp_path / "rec")
    monkeypatch.setattr(sn, "VER_DIR", tmp_path / "ver")
    w = sn.Writer({"RECORD_PCAP": "FALSE", "VERDICT_STREAM": "TRUE"})
    buf = _random_frame(random.Random(1))
    w.write(1700000000, 1, buf, {"rule": "DNS", "name": "amazon.de"})
    w.flush()
    data = (tmp_path / "ver" / "rec_000000.jsonl").read_bytes()
    assert data.endswith(b"\n")
    assert json.loads(data)["f"] == 1
    w.close()
