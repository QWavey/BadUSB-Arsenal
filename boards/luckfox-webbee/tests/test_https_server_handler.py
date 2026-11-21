"""Tests for HTTPSHandler.do_GET/_is_exempt and load_config in onboard/https_server.py.

These are the live code path for REDIRECT_ALL's HTTPS interception (manually verified
working on the real device during this session), but had zero test coverage -- only
fetch_certificate (the currently-unused real-cert-forwarding path) was tested. The TLS
wrapping happens at the socket level in _setup_ssl, separate from this request-handling
logic, so a plain (non-TLS) HTTPServer exercises the same do_GET/_is_exempt code the real
server runs once a TLS connection has already been accepted.
"""
import http.client
import http.server
import importlib.util
import sys
import threading
import types
from pathlib import Path

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"


def _load():
    for name in ("fcntl", "termios"):
        if name not in sys.modules:
            sys.modules[name] = types.ModuleType(name)
    spec = importlib.util.spec_from_file_location("https_server_handler_under_test", ONBOARD / "https_server.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _serve(mod, redirect_url, exempt=()):
    mod.HTTPSHandler.redirect_url = redirect_url
    mod.HTTPSHandler.redirect_exempt = set(exempt)
    srv = http.server.HTTPServer(("127.0.0.1", 0), mod.HTTPSHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def _get(srv, path="/", host=None, method="GET"):
    conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
    headers = {"Host": host} if host else {}
    conn.request(method, path, headers=headers)
    resp = conn.getresponse()
    resp.read()
    return resp.status, resp.getheader("Location")


def test_redirects_to_the_configured_target(tmp_path):
    mod = _load()
    srv = _serve(mod, "https://amazon.de/")
    try:
        status, loc = _get(srv, "/", host="bing.com")
        assert status == 302 and loc == "https://amazon.de/"
    finally:
        srv.shutdown(); srv.server_close()


def test_path_is_appended_to_the_target(tmp_path):
    mod = _load()
    srv = _serve(mod, "https://amazon.de/")
    try:
        status, loc = _get(srv, "/deals/today", host="bing.com")
        assert status == 302 and loc == "https://amazon.de/deals/today"
    finally:
        srv.shutdown(); srv.server_close()


def test_target_with_an_existing_query_string_is_not_rewritten(tmp_path):
    # "if target and '?' not in target" -- a target that already has a query string keeps
    # it as-is rather than being rebuilt from the request path.
    mod = _load()
    srv = _serve(mod, "https://amazon.de/search?q=preset")
    try:
        status, loc = _get(srv, "/other/path", host="bing.com")
        assert status == 302 and loc == "https://amazon.de/search?q=preset"
    finally:
        srv.shutdown(); srv.server_close()


def test_exempt_exact_host_gets_204_not_redirected(tmp_path):
    mod = _load()
    srv = _serve(mod, "https://amazon.de/", exempt={"anthropic.com"})
    try:
        status, loc = _get(srv, "/", host="anthropic.com")
        assert status == 204 and loc is None
    finally:
        srv.shutdown(); srv.server_close()


def test_exempt_subdomain_is_also_exempt(tmp_path):
    mod = _load()
    srv = _serve(mod, "https://amazon.de/", exempt={"anthropic.com"})
    try:
        status, _ = _get(srv, "/", host="api.anthropic.com")
        assert status == 204
    finally:
        srv.shutdown(); srv.server_close()


def test_lookalike_host_is_not_exempt(tmp_path):
    # "notanthropic.com" must not match "anthropic.com" via a naive substring/suffix check
    # without the dot -- _is_exempt requires an exact match or a "." + exempt suffix.
    mod = _load()
    srv = _serve(mod, "https://amazon.de/", exempt={"anthropic.com"})
    try:
        status, loc = _get(srv, "/", host="notanthropic.com")
        assert status == 302 and loc == "https://amazon.de/"
    finally:
        srv.shutdown(); srv.server_close()


def test_host_header_port_suffix_is_stripped_before_exempt_check(tmp_path):
    mod = _load()
    srv = _serve(mod, "https://amazon.de/", exempt={"anthropic.com"})
    try:
        status, _ = _get(srv, "/", host="anthropic.com:8443")
        assert status == 204
    finally:
        srv.shutdown(); srv.server_close()


def test_post_and_head_behave_like_get(tmp_path):
    mod = _load()
    srv = _serve(mod, "https://amazon.de/")
    try:
        for method in ("POST", "HEAD"):
            status, loc = _get(srv, "/", host="bing.com", method=method)
            assert status == 302 and loc == "https://amazon.de/"
    finally:
        srv.shutdown(); srv.server_close()


def test_load_config_parses_keys_and_skips_comments(tmp_path):
    mod = _load()
    (tmp_path / "config.txt").write_text(
        "# a comment\nREDIRECT_ALL_TO=https://amazon.de\n\nREDIRECT_EXEMPT=a.com,b.com\n")
    cfg = mod.load_config(tmp_path)
    assert cfg["REDIRECT_ALL_TO"] == "https://amazon.de"
    assert cfg["REDIRECT_EXEMPT"] == "a.com,b.com"
    assert "# a comment" not in cfg


def test_load_config_missing_file_returns_empty(tmp_path):
    mod = _load()
    assert mod.load_config(tmp_path / "does_not_exist") == {}
