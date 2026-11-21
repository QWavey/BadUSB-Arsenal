"""Regression tests for the 2026-10-29 fixes.

Forwarder: oversize frames are dropped, not counted toward the exit streak; a crash is
reported as "crashed". Timeline: every line stays valid JSON. Sniffer: an unreadable
filesystem deletes nothing, and a critical reading still clears captures. Shell and page
script: the address= loop skips junk tokens, and the REDIRECT_FROM editor keeps hostnames only.
"""
import http.client
import importlib.util
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
import types
from pathlib import Path

import pytest

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"
EMSGSIZE_LINUX = 90  # the device kernel's errno for "Message too long"

if not hasattr(socket, "MSG_DONTWAIT"):
    socket.MSG_DONTWAIT = 0x40  # Linux value; Windows Python does not define it
# webui imports these Linux-only modules at top level; nothing uses them at import.
for _name in ("fcntl", "pty", "termios"):
    sys.modules.setdefault(_name, types.ModuleType(_name))


def _load(name, path, root=None):
    if root is not None:
        os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---- forwarder -------------------------------------------------------------

class _FakeSock:
    def __init__(self, ifname, refuse_over=None):
        self.ifname = ifname
        self.refuse_over = refuse_over
        self.q = []
        self.sent = []

    def recv(self, n, flags=0):
        if not self.q:
            raise BlockingIOError()
        return self.q.pop(0)[0]

    def recvfrom(self, n, flags=0):
        if not self.q:
            raise BlockingIOError()
        return self.q.pop(0)

    def send(self, data):
        if self.refuse_over and len(data) > self.refuse_over:
            raise OSError(EMSGSIZE_LINUX, "Message too long")
        self.sent.append(data)

    def close(self):
        pass


class _FakePath:
    def __init__(self, p):
        pass

    def exists(self):
        return True


def _frame(n):
    return b"\x02\x00\x00\x00\x00\x99" + b"\x02\x00\x00\x00\x00\xcc" + b"\x08\x00" + b"x" * (n - 14)


def _run_bridge(fwd, sa, sb):
    def fake_select(r, w, x, timeout=None):
        ready = [s for s in r if s.q]
        if not ready:
            raise KeyboardInterrupt  # nothing queued: the bridge stops cleanly
        return ready, [], []

    fwd._bind = lambda ifname: {"eth0": sa, "usb0": sb}[ifname]
    fwd._get_mac = lambda ifname: b"\x02\x00\x00\x00\x00\xaa" if ifname == "eth0" else b"\x02\x00\x00\x00\x00\xbb"
    fwd._log = lambda m: None
    fwd._status = lambda m: None
    fwd.Path = _FakePath
    fwd.select = types.SimpleNamespace(select=fake_select)
    return fwd._bridge("eth0", "usb0")


def test_oversize_frames_are_dropped_and_do_not_end_the_bridge(tmp_path):
    fwd = _load("forwarder_oversize", ONBOARD / "forwarder.py", tmp_path)
    sa = _FakeSock("eth0")
    sb = _FakeSock("usb0", refuse_over=1514)
    for _ in range(150):  # more than the 100-error exit threshold, in a row
        sa.q.append((_frame(1518), ("eth0", 3, 0, 1, b"")))
    sa.q.append((_frame(200), ("eth0", 3, 0, 1, b"")))
    reason = _run_bridge(fwd, sa, sb)
    assert reason == "stopped"
    assert [len(d) for d in sb.sent] == [200]


def test_forwarder_reports_a_crash_in_its_status(tmp_path, monkeypatch):
    fwd = _load("forwarder_crash", ONBOARD / "forwarder.py", tmp_path)
    status = []

    def boom(a, b):
        raise RuntimeError("boom")

    monkeypatch.setattr(fwd, "_claim_pidfile", lambda: True)
    monkeypatch.setattr(fwd, "_release_pidfile", lambda: None)
    monkeypatch.setattr(fwd, "_status", lambda m: status.append(m))
    monkeypatch.setattr(fwd, "_log", lambda m: None)
    monkeypatch.setattr(fwd, "_bridge", boom)
    monkeypatch.setattr(os, "nice", lambda n: 0, raising=False)
    monkeypatch.setattr(sys, "argv", ["forwarder.py", "eth0", "usb0"])
    with pytest.raises(RuntimeError):
        fwd.main()
    assert status[-1] == "stopped: crashed: boom"


# ---- web UI timeline -------------------------------------------------------

def test_timeline_lines_stay_valid_json(tmp_path):
    import json
    web = _load("webui_timeline", ONBOARD / "webui.py", tmp_path)
    web._timeline("probe", big="x" * 3000)
    web._timeline("probe", small="ok")
    parsed = [json.loads(line) for line in Path(web.TIMELINE).read_text().splitlines()]
    assert len(parsed) == 2
    assert parsed[0].get("truncated") is True
    assert parsed[1].get("small") == "ok"


# ---- sniffer disk safety ---------------------------------------------------

def _captures(rec, n):
    rec.mkdir(parents=True, exist_ok=True)
    for old in rec.glob("rec_*.pcap"):
        old.unlink()
    for i in range(n):
        (rec / f"rec_{i:06d}.pcap").write_bytes(b"x" * 100)


def test_unreadable_filesystem_deletes_no_captures(tmp_path, monkeypatch):
    sniff = _load("sniff_unreadable", ONBOARD / "sniff.py", tmp_path)
    _captures(sniff.REC_DIR, 3)

    def broken(path):
        raise OSError("statvfs failed")

    monkeypatch.setattr(os, "statvfs", broken, raising=False)
    w = sniff.Writer({"RECORD_PCAP": "TRUE", "VERDICT_STREAM": "FALSE"})
    try:
        w._enforce_disk()
    finally:
        w.close()
    assert all((sniff.REC_DIR / f"rec_{i:06d}.pcap").exists() for i in range(3))


def test_critical_reading_still_clears_captures(tmp_path, monkeypatch):
    sniff = _load("sniff_critical", ONBOARD / "sniff.py", tmp_path)
    _captures(sniff.REC_DIR, 3)
    monkeypatch.setattr(sniff, "free_mb_or_none", lambda path="/root": 2)
    w = sniff.Writer({"RECORD_PCAP": "TRUE", "VERDICT_STREAM": "FALSE"})
    try:
        w._enforce_disk()
    finally:
        w.close()
    assert not any((sniff.REC_DIR / f"rec_{i:06d}.pcap").exists() for i in range(3))


@pytest.mark.skipif(os.name == "nt", reason="Windows cannot delete an open file; checked on the device")
def test_open_pcap_survives_an_age_prune(tmp_path):
    sniff = _load("sniff_open", ONBOARD / "sniff.py", tmp_path)
    _captures(sniff.REC_DIR, 2)
    w = sniff.Writer({"RECORD_PCAP": "TRUE", "VERDICT_STREAM": "FALSE", "RECORD_MAX_AGE_H": "1"})
    try:
        open_name = Path(w.pcap_f.name)
        old = time.time() - 3 * 3600
        os.utime(open_name, (old, old))
        w._prune()
        assert open_name.exists()
    finally:
        w.close()


# ---- shell and page script -------------------------------------------------

@pytest.mark.skipif(shutil.which("sh") is None, reason="needs a POSIX sh")
def test_address_lines_skip_junk_tokens():
    script = r'''
RED_FROM='#,example.com,www.example.com,(comma-sep),amazon.de'; TARGET_IP=172.32.0.93
for h in $(echo "$RED_FROM" | tr ',' ' '); do
  h=$(echo "$h" | sed -E 's#^https?://##; s#/.*##')
  case "$h" in ''|*[!A-Za-z0-9.-]*) continue ;; esac
  echo "address=/$h/$TARGET_IP"
done
'''
    out = subprocess.run(["sh", "-c", script], capture_output=True, text=True, check=True).stdout
    assert out.split() == [
        "address=/example.com/172.32.0.93",
        "address=/www.example.com/172.32.0.93",
        "address=/amazon.de/172.32.0.93",
    ]


@pytest.mark.skipif(shutil.which("node") is None, reason="needs node")
def test_redirect_editor_keeps_hostnames_only():
    runner = r'''
const fs = require('fs');
const src = fs.readFileSync(process.argv[1], 'utf8');
const start = src.indexOf('  if (/^REDIRECT_FROM=.*$/m.test(txt)) {');
const elseAt = src.indexOf('  } else { txt += `REDIRECT_FROM=', start);
const block = src.slice(start, src.indexOf('\n', elseAt));
const run = new Function('txt', 'h', block + '\nreturn txt;');
const out = run('REDIRECT_FROM=#,example.com,www.example.com,(comma-sep),amazon.de\n', 'bing.com');
console.log(out.split('\n').find(l => l.startsWith('REDIRECT_FROM=')));
'''
    out = subprocess.run(["node", "-e", runner, str(ONBOARD / "webui.py")],
                         capture_output=True, text=True, check=True).stdout.strip()
    assert out == "REDIRECT_FROM=example.com,www.example.com,amazon.de,bing.com"


# ---- web UI login ----------------------------------------------------------

def _login_status(web, user, password):
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
        conn.request("POST", "/api/auth/login", body=json.dumps({"user": user, "pass": password}),
                     headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        resp.read()
        return resp.status
    finally:
        srv.shutdown()
        srv.server_close()


def test_blank_webui_pass_never_logs_in(tmp_path):
    web = _load("webui_login_blank", ONBOARD / "webui.py", tmp_path)
    (tmp_path / "config.txt").write_text("WEBUI_USER=root\nWEBUI_PASS=\n", encoding="utf-8")
    assert _login_status(web, "root", "") == 401
    assert _login_status(web, "root", "anything") == 401


def test_non_ascii_webui_pass_logs_in(tmp_path):
    web = _load("webui_login_utf8", ONBOARD / "webui.py", tmp_path)
    (tmp_path / "config.txt").write_text("WEBUI_USER=root\nWEBUI_PASS=pässwort\n", encoding="utf-8")
    assert _login_status(web, "root", "pässwort") == 200
    assert _login_status(web, "root", "passwort") == 401
