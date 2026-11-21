"""Oversize frames must be dropped without ending the bridge (forwarder.py).

A frame longer than the outgoing interface's MTU makes send() fail with
EMSGSIZE. The forwarder drops such frames and counts them. They must not
move the 100-error exit counter, and the frames around them must still cross.

The sockets here are fakes with an MTU, so the test runs on any OS.
"""
import errno
import importlib.util
import socket
from pathlib import Path
from types import SimpleNamespace

import pytest

FORWARDER = Path(__file__).resolve().parents[1] / "onboard" / "forwarder.py"
ETH0_MAC = b"\x02\x00\x00\x00\x00\x01"
USB0_MAC = b"\x02\x00\x00\x00\x00\x02"
PEER_SRC = b"\x02\x11\x22\x33\x44\x55"   # a host on the far side, not one of our MACs
DST = b"\x02\xaa\xbb\xcc\xdd\xee"


def _load():
    spec = importlib.util.spec_from_file_location("forwarder_under_test", FORWARDER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def frame(size, src=PEER_SRC):
    return DST + src + b"\x08\x00" + bytes(size - 14)


class FakeSocket:
    def __init__(self, mtu):
        self.mtu = mtu
        self.inbox = []
        self.sent = []

    def recvfrom(self, _size, _flags=0):
        if not self.inbox:
            raise BlockingIOError()
        # addr[2] is the packet type; 0 is PACKET_HOST (not our own transmission)
        return self.inbox.pop(0), ("iface", 0x0800, 0)

    def send(self, buf):
        if len(buf) > self.mtu:
            # Linux errno 90, the value forwarder.py compares against; on
            # Windows errno.EMSGSIZE is 10040, so the constant is spelled out.
            raise OSError(90, "Message too long")
        self.sent.append(bytes(buf))
        return len(buf)

    def close(self):
        pass


@pytest.fixture
def run_bridge(monkeypatch):
    fw = _load()
    logs, socks = [], {}

    class AlwaysExists:
        def __init__(self, *_a, **_k):
            pass

        def exists(self):
            return True

    def fake_select(rlist, _w, _x, _timeout):
        ready = [s for s in rlist if s.inbox]
        if not ready:
            raise KeyboardInterrupt  # the bridge treats this as a normal stop
        return ready, [], []

    monkeypatch.setattr(fw, "Path", AlwaysExists)
    monkeypatch.setattr(fw, "_log", lambda m: logs.append(m))
    monkeypatch.setattr(fw, "_status", lambda m: None)
    monkeypatch.setattr(fw, "_get_mac", lambda ifn: ETH0_MAC if ifn == "eth0" else USB0_MAC)
    monkeypatch.setattr(fw, "_bind", lambda ifn: socks[ifn])
    monkeypatch.setattr(fw, "select", SimpleNamespace(select=fake_select))
    monkeypatch.setattr(socket, "MSG_DONTWAIT", 0x40, raising=False)

    def run(eth0, usb0):
        socks["eth0"], socks["usb0"] = eth0, usb0
        return fw._bridge("eth0", "usb0"), logs

    return run


def test_oversize_frames_toward_usb0_are_dropped_not_fatal(run_bridge):
    eth0 = FakeSocket(mtu=9000)   # eth0 hands us large frames
    usb0 = FakeSocket(mtu=1500)   # usb0 accepts up to 1500
    small_a = frame(100)
    small_b = frame(200)
    eth0.inbox = [small_a] + [frame(1600)] * 150 + [small_b]

    result, logs = run_bridge(eth0, usb0)

    assert result == "stopped", result                       # not "too many send errors"
    assert usb0.sent == [small_a, small_b]                   # the frames around the oversize ones still cross
    assert sum("dropped oversize frame for usb0" in m for m in logs) == 1
    assert not any("send error streak" in m for m in logs)


def test_oversize_frames_toward_eth0_are_dropped_not_fatal(run_bridge):
    eth0 = FakeSocket(mtu=1500)
    usb0 = FakeSocket(mtu=9000)
    small = frame(120)
    usb0.inbox = [small] + [frame(1600)] * 150

    result, logs = run_bridge(eth0, usb0)

    assert result == "stopped", result
    assert eth0.sent == [small]
    assert sum("dropped oversize frame for eth0" in m for m in logs) == 1
    assert not any("send error streak" in m for m in logs)
