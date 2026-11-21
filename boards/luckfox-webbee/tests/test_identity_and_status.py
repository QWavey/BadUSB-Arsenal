"""Identity override writes, bundle identity checks, and the honeypot status count."""
import http.client
import importlib.util
import io
import json
import os
import sys
import tarfile
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
    spec = importlib.util.spec_from_file_location("webui_identity_under_test", ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _request(web, method, path, body=b"", headers=None):
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=10)
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        srv.shutdown()
        srv.server_close()


def _bundle(members):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as t:
        for name, data in members:
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def test_identity_post_writes_the_override(tmp_path, monkeypatch):
    web = _load(tmp_path)
    monkeypatch.setattr(web.Handler, "_require_admin", lambda self: True)
    body = json.dumps({"VID": "0x1d6b", "PID": "0x0137"}).encode()
    status, _ = _request(web, "POST", "/api/clone/identity", body,
                         {"Content-Type": "application/json"})
    assert status == 200
    assert 'USB_VID="0x1d6b"' in (tmp_path / "identity.env").read_text()


def test_empty_identity_post_does_not_clear_the_override(tmp_path, monkeypatch):
    web = _load(tmp_path)
    monkeypatch.setattr(web.Handler, "_require_admin", lambda self: True)
    keep = 'USB_VID="0x1d6b"\n'
    (tmp_path / "identity.env").write_text(keep)
    status, _ = _request(web, "POST", "/api/clone/identity", b"")
    assert status == 400
    assert (tmp_path / "identity.env").read_text() == keep


def test_identity_check_accepts_plain_settings_and_refuses_broken_ones(tmp_path):
    web = _load(tmp_path)
    assert web._identity_ok(b'# comment\nUSB_VID="0x1d6b"\nUSB_PROD="Ghost Box"\n')
    assert not web._identity_ok(b'USB_MFR="Foo\n')                       # unbalanced quote
    assert not web._identity_ok(b'USB_MFR="a$(reboot)"\n')               # command substitution
    assert not web._identity_ok(b'EVIL=1\n')                             # not a USB_* setting


def test_bundle_with_a_broken_identity_keeps_the_rest_and_leaves_identity_out(tmp_path, monkeypatch):
    web = _load(tmp_path)
    monkeypatch.setattr(web.Handler, "_require_admin", lambda self: True)
    bundle = _bundle([("config.txt", b"DEVICE_NAME=ok\n"), ("identity.env", b'USB_MFR="Foo\n')])
    status, body = _request(web, "POST", "/api/bundle/import", bundle,
                            {"Content-Type": "application/gzip"})
    assert status == 200 and json.loads(body)["ok"] is True
    assert (tmp_path / "config.txt").read_text() == "DEVICE_NAME=ok\n"
    assert not (tmp_path / "identity.env").exists()


def test_honeypot_loot_count_follows_appends(tmp_path):
    web = _load(tmp_path)
    loot = web.HONEY_LOOT
    loot.parent.mkdir(parents=True, exist_ok=True)
    loot.write_text('{"a":1}\n{"a":2}\n{"a":3}\n')
    _, body = _request(web, "GET", "/api/honeypot/status")
    assert json.loads(body)["loot_count"] == 3
    with open(loot, "a") as f:
        f.write('{"a":4}\n')
    _, body = _request(web, "GET", "/api/honeypot/status")
    assert json.loads(body)["loot_count"] == 4
