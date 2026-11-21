"""Saving a network setting must not report success when dnsmasq did not take it."""
import http.client
import importlib.util
import os
import sys
import threading
import types
from pathlib import Path

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"

for _name in ("fcntl", "pty", "termios"):
    try:
        __import__(_name)
    except ImportError:
        sys.modules[_name] = types.ModuleType(_name)


def _load(root):
    os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location("webui_reload_under_test", ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _Failed:
    returncode = 1
    stderr = "bad DHCP range"
    stdout = ""


def test_a_failed_reload_keeps_the_old_config_and_reports_it(tmp_path, monkeypatch):
    web = _load(tmp_path)
    (tmp_path / "logs").mkdir()
    old = "DNSMASQ=TRUE\nDHCP_RANGE=172.32.0.100,172.32.0.120,255.255.0.0,12h\nREDIRECT=FALSE\n"
    (tmp_path / "config.txt").write_text(old, encoding="utf-8")
    monkeypatch.setattr(web.Handler, "_require_admin", lambda self: True)
    monkeypatch.setattr(web.subprocess, "run", lambda *a, **k: _Failed())
    body = old.replace("172.32.0.100,172.32.0.120", "bogus").encode("utf-8")
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=10)
        conn.request("POST", "/api/config", body=body)
        resp = conn.getresponse()
        status = resp.status
        resp.read()
    finally:
        srv.shutdown()
        srv.server_close()
    assert status == 500
    assert (tmp_path / "config.txt").read_text(encoding="utf-8") == old
