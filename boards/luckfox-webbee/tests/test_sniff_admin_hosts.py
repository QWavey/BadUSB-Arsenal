"""sniff.py's admin_hosts() must track the LAN-detected IP like webui.py's does.

Bug-hunt finding: sniff.py had its own ADMIN_HOSTS frozenset, fixed at the isolated
172.32.0.93/ghostlink.local/localhost/127.0.0.1 set, with no knowledge of the LAN IP the
auto-detector binds once NET_MODE=NAT is reachable from the real LAN. Every admin page
load / API poll made over that LAN IP was being recorded as victim traffic in the Live
Monitor and pcap/verdict files, indistinguishable from what was actually captured from the
PC. Mirrors webui.py's lan_admin_ips()/_read_lan_ip(): same state file, same validation
(regular, non-symlink, root-owned file only), same 5s cache.
"""
import importlib.util
import json
import os
import time
from pathlib import Path

SNIFF = Path(__file__).resolve().parents[1] / "onboard" / "sniff.py"


def _load():
    spec = importlib.util.spec_from_file_location(f"sniff_admin_{time.time_ns()}", SNIFF)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_fixed_set_when_no_state_file(tmp_path, monkeypatch):
    sniff = _load()
    monkeypatch.setattr(sniff, "LAN_DETECT_PATH", str(tmp_path / "missing.json"))
    assert sniff.admin_hosts() == sniff.ADMIN_HOSTS_FIXED


def test_adds_the_lan_ip_once_detected(tmp_path, monkeypatch):
    sniff = _load()
    state = tmp_path / "lan.json"
    state.write_text(json.dumps({"detected": True, "suggested_ip": "192.168.178.250"}))
    monkeypatch.setattr(sniff, "LAN_DETECT_PATH", str(state))
    monkeypatch.setattr(sniff, "_admin_hosts_cache", (sniff.ADMIN_HOSTS_FIXED, 0.0))
    hosts = sniff.admin_hosts()
    assert "192.168.178.250" in hosts
    assert hosts >= sniff.ADMIN_HOSTS_FIXED


def test_ignores_malformed_state_file(tmp_path, monkeypatch):
    sniff = _load()
    state = tmp_path / "lan.json"
    state.write_text("not json at all {{{")
    monkeypatch.setattr(sniff, "LAN_DETECT_PATH", str(state))
    monkeypatch.setattr(sniff, "_admin_hosts_cache", (sniff.ADMIN_HOSTS_FIXED, 0.0))
    assert sniff.admin_hosts() == sniff.ADMIN_HOSTS_FIXED


def test_ignores_undetected_state(tmp_path, monkeypatch):
    sniff = _load()
    state = tmp_path / "lan.json"
    state.write_text(json.dumps({"detected": False, "suggested_ip": "192.168.178.250"}))
    monkeypatch.setattr(sniff, "LAN_DETECT_PATH", str(state))
    monkeypatch.setattr(sniff, "_admin_hosts_cache", (sniff.ADMIN_HOSTS_FIXED, 0.0))
    assert sniff.admin_hosts() == sniff.ADMIN_HOSTS_FIXED
