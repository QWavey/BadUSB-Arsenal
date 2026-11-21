"""Regression tests: HID/payload-execution and delete endpoints must require auth.

Bug-hunt finding: /api/hid, /api/live/key, /api/scripts/<name>/run, /rename,
/api/scripts/_runtemp, /api/hid/stop, and every do_DELETE branch ran with no
_require_admin()/_require_auth() gate at all, so once a LAN IP is bound
(admin_hosts() grows beyond the isolated 172.32.0.93) any LAN host could
run or delete payloads, inject live keystrokes, or wipe the cloned identity
with zero authentication -- regardless of WEBUI_REQUIRE_AUTH. These tests
pin WEBUI_REQUIRE_AUTH=TRUE and confirm every one of those paths now
refuses an unauthenticated caller with 401, instead of performing the
action.
"""
import http.client
import importlib.util
import os
import sys
import threading
import types
from pathlib import Path

import pytest

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"

for _name in ("fcntl", "pty", "termios"):
    try:
        __import__(_name)
    except ImportError:
        sys.modules[_name] = types.ModuleType(_name)


def _load(name, root):
    (root / "config.txt").write_text("WEBUI_REQUIRE_AUTH=TRUE\n")
    (root / "payloads").mkdir(exist_ok=True)
    os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location(name, ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _serve(web):
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def _request(web, method, path, body=b""):
    srv = _serve(web)
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
        headers = {"Content-Length": str(len(body))} if body else {}
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        srv.shutdown()
        srv.server_close()


@pytest.mark.parametrize("path,body", [
    ("/api/hid", b"nonexistent.ducky"),
    ("/api/hid/stop", b""),
    ("/api/live/key", b'{"key": 4}'),
    ("/api/scripts/_runtemp", b"REM hi\n"),
    ("/api/scripts/nope.ducky/run", b""),
    ("/api/scripts/nope.ducky/rename", b'{"new": "other.ducky"}'),
])
def test_hid_and_script_run_endpoints_require_auth(tmp_path, path, body):
    web = _load(f"webui_auth_{path.replace('/', '_')}", tmp_path)
    status, resp = _request(web, "POST", path, body)
    assert status == 401, f"{path} did not require auth (got {status}: {resp[:200]!r})"


def test_delete_requires_auth_for_every_branch(tmp_path):
    web = _load("webui_delete_auth", tmp_path)
    for path in ("/api/scripts/nope.ducky", "/api/extensions/nope", "/api/clone/identity"):
        status, resp = _request(web, "DELETE", path)
        assert status == 401, f"DELETE {path} did not require auth (got {status}: {resp[:200]!r})"


def test_unauthed_runtemp_does_not_execute(tmp_path):
    """The gate must reject before touching the filesystem/subprocess, not after.
    GL_SET is ducky.py's own extension command for writing config.txt (see its
    module docstring) -- if the gate were bypassed, this would add MARKER_RAN=1
    to config.txt. It must not."""
    web = _load("webui_runtemp_noexec", tmp_path)
    status, _ = _request(web, "POST", "/api/scripts/_runtemp", b"GL_SET MARKER_RAN=1\n")
    assert status == 401
    assert "MARKER_RAN" not in (tmp_path / "config.txt").read_text(), \
        "payload executed despite missing auth"
