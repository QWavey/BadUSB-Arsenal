"""Tests for onboard/redirect302.py: the standalone :8080 302 redirector.

This module previously started a real ThreadingHTTPServer on the hardcoded port 8080
at import time (no `if __name__ == "__main__":` guard), so merely importing it for a
test -- or for any other reuse -- immediately bound a port and blocked forever in
serve_forever(). Fixed alongside adding this file, which was this project's only
onboard/*.py module with no test coverage at all. Importing it here now is itself a
regression test for that fix: if the guard regresses, this whole file hangs.
"""
import http.client
import importlib.util
import os
import threading
from pathlib import Path

import pytest

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"


def _load(root):
    os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location("redirect302_under_test", ONBOARD / "redirect302.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _request(mod, path="/", method="GET"):
    srv = mod.ThreadingHTTPServer(("127.0.0.1", 0), mod.H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
        conn.request(method, path)
        resp = conn.getresponse()
        resp.read()
        return resp.status, resp.getheader("Location")
    finally:
        srv.shutdown()
        srv.server_close()


def test_getcfg_reads_a_value(tmp_path):
    (tmp_path / "config.txt").write_text("REDIRECT_TO=https://amazon.de\n")
    mod = _load(tmp_path)
    assert mod.getcfg("REDIRECT_TO") == "https://amazon.de"


def test_getcfg_strips_inline_comment(tmp_path):
    (tmp_path / "config.txt").write_text("REDIRECT_TO=https://amazon.de   # the target\n")
    mod = _load(tmp_path)
    assert mod.getcfg("REDIRECT_TO") == "https://amazon.de"


def test_getcfg_empty_when_key_absent_or_no_config(tmp_path):
    mod = _load(tmp_path)
    assert mod.getcfg("REDIRECT_TO") == ""
    (tmp_path / "config.txt").write_text("OTHER_KEY=x\n")
    mod2 = _load(tmp_path)
    assert mod2.getcfg("REDIRECT_TO") == ""


def test_default_target_when_unconfigured(tmp_path):
    mod = _load(tmp_path)
    status, loc = _request(mod, "/")
    assert status == 302
    assert loc == "http://example.com"


def test_scheme_is_prepended_when_missing(tmp_path):
    (tmp_path / "config.txt").write_text("REDIRECT_TO=amazon.de\n")
    mod = _load(tmp_path)
    status, loc = _request(mod, "/")
    assert status == 302 and loc == "http://amazon.de"


def test_path_preserved_by_default(tmp_path):
    (tmp_path / "config.txt").write_text("REDIRECT_TO=https://amazon.de\n")
    mod = _load(tmp_path)
    status, loc = _request(mod, "/deals/today?x=1")
    assert status == 302 and loc == "https://amazon.de/deals/today?x=1"


def test_root_path_is_not_appended_even_with_preserve_on(tmp_path):
    # self.path == "/" is explicitly excluded, so the target's own path (if any) wins
    # instead of being suffixed with a redundant "/".
    (tmp_path / "config.txt").write_text("REDIRECT_TO=https://amazon.de/already/here\n")
    mod = _load(tmp_path)
    status, loc = _request(mod, "/")
    assert status == 302 and loc == "https://amazon.de/already/here"


def test_path_not_preserved_when_disabled(tmp_path):
    (tmp_path / "config.txt").write_text(
        "REDIRECT_TO=https://amazon.de\nREDIRECT_PRESERVE_PATH=FALSE\n")
    mod = _load(tmp_path)
    status, loc = _request(mod, "/deals/today")
    assert status == 302 and loc == "https://amazon.de"


def test_post_behaves_like_get(tmp_path):
    (tmp_path / "config.txt").write_text("REDIRECT_TO=https://amazon.de\n")
    mod = _load(tmp_path)
    status, loc = _request(mod, "/x", method="POST")
    assert status == 302 and loc == "https://amazon.de/x"


def test_response_has_no_body_and_closes(tmp_path):
    mod = _load(tmp_path)
    srv = mod.ThreadingHTTPServer(("127.0.0.1", 0), mod.H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
        conn.request("GET", "/")
        resp = conn.getresponse()
        body = resp.read()
        assert body == b""
        assert resp.getheader("Content-Length") == "0"
        assert resp.getheader("Connection") == "close"
    finally:
        srv.shutdown()
        srv.server_close()
