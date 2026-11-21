"""Regression tests for the bug-hunt fixes in onboard/webui.py.

Login: every attempt counts, a JSON list body is a failed login, and a negative Content-Length
does not hang the request. Shell: an exited shell is replaced, a runaway command is interrupted
and the next command still runs. The shell tests need a Linux pty and are skipped elsewhere.
"""
import http.client
import importlib.util
import json
import os
import sys
import threading
import time
import types
from pathlib import Path

import pytest

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"
LINUX_PTY = sys.platform.startswith("linux")

# webui imports these Linux-only modules at top level. Stub them only where they do not exist
# (Windows), so the real pty module is used on Linux.
for _name in ("fcntl", "pty", "termios"):
    try:
        __import__(_name)
    except ImportError:
        sys.modules[_name] = types.ModuleType(_name)


def _load(name, root):
    os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location(name, ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _serve(web):
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def _post(web, path, body, headers=None):
    srv = _serve(web)
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
        conn.request("POST", path, body=body, headers=headers or {})
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        srv.shutdown()
        srv.server_close()


# ---- login -----------------------------------------------------------------

def test_login_limit_counts_every_attempt(tmp_path):
    web = _load("webui_limit", tmp_path)
    results = [web._login_allowed("10.0.0.9") for _ in range(web.LOGIN_MAX + 1)]
    assert results[:web.LOGIN_MAX] == [True] * web.LOGIN_MAX
    assert results[-1] is False


def test_login_table_forgets_ips_whose_attempts_aged_out(tmp_path):
    web = _load("webui_forget", tmp_path)
    web.LOGIN_ATTEMPTS["10.0.0.7"] = [time.time() - 120]    # all older than the 60 s window
    web._login_allowed("10.0.0.8")
    assert "10.0.0.7" not in web.LOGIN_ATTEMPTS


def test_json_list_login_body_is_a_failed_login(tmp_path):
    web = _load("webui_list", tmp_path)
    (tmp_path / "config.txt").write_text("WEBUI_USER=root\nWEBUI_PASS=ghostlink\n", encoding="utf-8")
    status, _ = _post(web, "/api/auth/login", b"[]", {"Content-Type": "application/json"})
    assert status == 401


def test_negative_content_length_does_not_hang(tmp_path):
    web = _load("webui_len", tmp_path)
    # /api/auth/logout needs no session and reads the body before routing, so it exercises the
    # body read. Before the fix, rfile.read(-1) waited for the client to close, and this timed out.
    status, _ = _post(web, "/api/auth/logout", b"", {"Content-Length": "-1"})
    assert status == 200


# ---- shell (Linux pty only) -------------------------------------------------

@pytest.mark.skipif(not LINUX_PTY, reason="needs a Linux pty")
def test_exited_shell_is_replaced_by_a_fresh_one(tmp_path):
    web = _load("webui_exit", tmp_path)
    token = "tok-exit-test"
    try:
        s = web.pty_open(token)
        first = s["pid"]
        os.write(s["fd"], b"exit\n")
        deadline = time.time() + 3
        while time.time() < deadline and not web._pty_exited(first):
            time.sleep(0.05)
        s2 = web.pty_open(token)
        assert s2["pid"] != first
    finally:
        web.pty_close(token)


@pytest.mark.skipif(not LINUX_PTY, reason="needs a Linux pty")
def test_runaway_command_is_interrupted_and_the_next_one_runs(tmp_path):
    web = _load("webui_yes", tmp_path)
    token = "tok-yes-test"
    try:
        t0 = time.time()
        first = web.pty_run(token, "yes", timeout=1.0)
        assert first["timeout"] is True
        assert time.time() - t0 < 10             # bounded: the timeout plus one resync
        second = web.pty_run(token, "echo still-here", timeout=5.0)
        assert second["rc"] == 0
        assert "still-here" in second["out"]
    finally:
        web.pty_close(token)
