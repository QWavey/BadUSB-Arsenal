"""Regression test: forwarder.py must define SOL_PACKET even when the Python
socket module does not have the name. The GhostLink device runs Python 3.11
without it, and the forwarder then failed with
"module 'socket' has no attribute 'SOL_PACKET'"."""
import importlib.util
import socket
from pathlib import Path

FORWARDER = Path(__file__).resolve().parents[1] / "onboard" / "forwarder.py"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, FORWARDER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_sol_packet_falls_back_when_socket_module_lacks_it(monkeypatch):
    monkeypatch.delattr(socket, "SOL_PACKET", raising=False)
    forwarder = _load("forwarder_without_sol_packet")
    assert forwarder.SOL_PACKET == 263
