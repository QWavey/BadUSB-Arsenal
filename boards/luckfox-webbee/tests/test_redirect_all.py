"""Tests for the "redirect all URLs" mode in onboard/webui.py.

REDIRECT_ALL sends every host that isn't the device itself, an exempt domain,
or the target to one target URL. Covered here: the default is off; the 302 for
a non-exempt host when the mode is on; no redirect for exempt hosts, the device
IP, or the target host; the save-time validation of the target URL and the
exempt list; and the config round trip. The per-URL REDIRECT feature is left
untouched and is not retested here.
"""
import http.client
import importlib.util
import os
import sys
import threading
import types
from pathlib import Path

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"

# webui imports these Linux-only modules at top level. Stub them only where they
# do not exist (Windows), so the real modules are used in WSL / on the device.
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


def _write_cfg(root, text):
    (root / "config.txt").write_text(text, encoding="utf-8")


def _serve(web):
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def _get(web, path, host, headers=None):
    """GET with an explicit Host header (passing Host in headers makes
    http.client skip its own auto Host), returning (status, Location)."""
    srv = _serve(web)
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
        h = dict(headers or {}); h["Host"] = host
        conn.request("GET", path, headers=h)
        resp = conn.getresponse()
        resp.read()
        return resp.status, resp.getheader("Location")
    finally:
        srv.shutdown(); srv.server_close()


def _post(web, path, body, headers=None):
    srv = _serve(web)
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
        conn.request("POST", path, body=body, headers=headers or {})
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        srv.shutdown(); srv.server_close()


_ON = ("REDIRECT_ALL=TRUE\n"
       "REDIRECT_ALL_TO=https://example.com\n"
       "REDIRECT_EXEMPT=anthropic.com,claude.ai\n")


# ---- default is off --------------------------------------------------------

def test_default_is_off_when_key_absent(tmp_path):
    web = _load("ra_default_absent", tmp_path)
    assert web.should_redirect_all("whatever.com", {}) is None


def test_default_config_has_mode_off(tmp_path):
    web = _load("ra_default_cfg", tmp_path)
    cfg = {}
    for ln in web.DEFAULT_CONFIG.splitlines():
        if "=" in ln and not ln.startswith("#"):
            k, v = ln.split("=", 1); cfg[k.strip()] = v.strip()
    assert cfg["REDIRECT_ALL"] == "FALSE"
    assert cfg["REDIRECT_ALL_TO"] == ""
    assert "anthropic.com" in cfg["REDIRECT_EXEMPT"]
    assert web.should_redirect_all("whatever.com", cfg) is None


def test_mode_off_never_redirects_even_with_target(tmp_path):
    web = _load("ra_off_target", tmp_path)
    cfg = {"REDIRECT_ALL": "FALSE", "REDIRECT_ALL_TO": "https://example.com"}
    assert web.should_redirect_all("whatever.com", cfg) is None


# ---- the 302 for a non-exempt host (real HTTP path) ------------------------

def test_http_302_for_non_exempt_host(tmp_path):
    web = _load("ra_http_302", tmp_path)
    _write_cfg(tmp_path, _ON)
    status, loc = _get(web, "/some/page", host="evil.example.net")
    assert status == 302
    assert loc == "https://example.com"


def test_http_no_redirect_for_exempt_host_serves_ui(tmp_path):
    web = _load("ra_http_exempt", tmp_path)
    _write_cfg(tmp_path, _ON)
    # An exempt host is not redirected; "/" falls through to the normal UI (200).
    status, loc = _get(web, "/", host="claude.ai")
    assert status == 200
    assert loc is None


def test_http_device_ui_never_redirected(tmp_path):
    web = _load("ra_http_device", tmp_path)
    _write_cfg(tmp_path, _ON)
    status, loc = _get(web, "/", host="172.32.0.93")
    assert status == 200
    assert loc is None


# ---- no redirect for exempt / device / target (unit) -----------------------

def test_unit_redirects_foreign_host(tmp_path):
    web = _load("ra_unit_foreign", tmp_path)
    cfg = {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "https://example.com",
           "REDIRECT_EXEMPT": "anthropic.com,claude.ai"}
    assert web.should_redirect_all("google.com", cfg) == "https://example.com"
    assert web.should_redirect_all("www.google.com:80", cfg) == "https://example.com"


def test_unit_exempt_host_and_subdomain(tmp_path):
    web = _load("ra_unit_exempt", tmp_path)
    cfg = {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "https://example.com",
           "REDIRECT_EXEMPT": "anthropic.com,claude.ai"}
    assert web.should_redirect_all("claude.ai", cfg) is None
    assert web.should_redirect_all("api.claude.ai", cfg) is None
    assert web.should_redirect_all("anthropic.com", cfg) is None


def test_unit_default_exempt_when_key_absent(tmp_path):
    web = _load("ra_unit_default_exempt", tmp_path)
    cfg = {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "https://t.example"}
    # REDIRECT_EXEMPT absent -> the built-in default list applies.
    assert web.should_redirect_all("claude.ai", cfg) is None
    assert web.should_redirect_all("msftconnecttest.com", cfg) is None
    assert web.should_redirect_all("foo.bar", cfg) == "https://t.example"


def test_unit_device_addresses_never_redirected(tmp_path):
    web = _load("ra_unit_device", tmp_path)
    cfg = {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "https://example.com"}
    for h in web.ADMIN_HOSTS:
        assert web.should_redirect_all(h, cfg) is None


def test_unit_target_host_is_exempt(tmp_path):
    web = _load("ra_unit_target", tmp_path)
    cfg = {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "https://example.com/login"}
    assert web.should_redirect_all("example.com", cfg) is None       # the target itself
    assert web.should_redirect_all("www.example.com", cfg) is None   # and its subdomains
    assert web.should_redirect_all("other.net", cfg) == "https://example.com/login"


def test_unit_invalid_target_means_no_redirect(tmp_path):
    web = _load("ra_unit_badtarget", tmp_path)
    cfg = {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "ftp://nope"}
    assert web.should_redirect_all("foo.bar", cfg) is None


# ---- exempt-list normalization ---------------------------------------------

def test_exempt_hosts_trimmed_lowered_deduped(tmp_path):
    web = _load("ra_exempt_norm", tmp_path)
    hosts, bad = web.redirect_exempt_hosts("  A.COM , b.com ,, a.com ")
    assert hosts == ["a.com", "b.com"]
    assert bad == []


def test_exempt_hosts_flags_space_and_slash(tmp_path):
    web = _load("ra_exempt_bad", tmp_path)
    hosts, bad = web.redirect_exempt_hosts("ok.com, bad domain.com, has/slash.com")
    assert hosts == ["ok.com"]
    assert "bad domain.com" in bad and "has/slash.com" in bad


# ---- save-time validation --------------------------------------------------

def test_validate_accepts_good_settings(tmp_path):
    web = _load("ra_val_ok", tmp_path)
    assert web.validate_redirect_all(
        {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "https://example.com",
         "REDIRECT_EXEMPT": "a.com,b.com"}) is None
    assert web.validate_redirect_all(
        {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "http://x.test/path"}) is None


def test_validate_rejects_bad_target_url(tmp_path):
    web = _load("ra_val_badurl", tmp_path)
    for to in ("", "ftp://x", "example.com", "not a url", "https://"):
        err = web.validate_redirect_all({"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": to})
        assert err and "REDIRECT_ALL_TO" in err


def test_validate_rejects_bad_exempt_entries(tmp_path):
    web = _load("ra_val_badexempt", tmp_path)
    err = web.validate_redirect_all(
        {"REDIRECT_ALL": "FALSE", "REDIRECT_EXEMPT": "good.com, bad entry.com"})
    assert err and "REDIRECT_EXEMPT" in err
    err2 = web.validate_redirect_all(
        {"REDIRECT_ALL": "FALSE", "REDIRECT_EXEMPT": "a.com,b.com/x"})
    assert err2 and "REDIRECT_EXEMPT" in err2


def test_validate_target_not_required_when_mode_off(tmp_path):
    web = _load("ra_val_off", tmp_path)
    assert web.validate_redirect_all({"REDIRECT_ALL": "FALSE", "REDIRECT_ALL_TO": ""}) is None


# ---- config round trip + POST reject keeps the file ------------------------

def test_config_round_trip(tmp_path):
    web = _load("ra_roundtrip", tmp_path)
    _write_cfg(tmp_path, _ON)
    cfg = web.load_cfg()
    assert cfg["REDIRECT_ALL"] == "TRUE"
    assert cfg["REDIRECT_ALL_TO"] == "https://example.com"
    assert cfg["REDIRECT_EXEMPT"] == "anthropic.com,claude.ai"


def test_post_bad_config_is_rejected_and_file_unchanged(tmp_path):
    web = _load("ra_post_reject", tmp_path)
    original = ("WEBUI_USER=root\nWEBUI_PASS=ghostlink\n"
                "REDIRECT_ALL=FALSE\nREDIRECT_ALL_TO=\n")
    _write_cfg(tmp_path, original)
    bad = "REDIRECT_ALL=TRUE\nREDIRECT_ALL_TO=ftp://nope\n"
    status, _ = _post(web, "/api/config", bad.encode(), {"Content-Type": "text/plain"})
    assert status == 400
    # The save was rejected, so config.txt still holds the original bytes.
    assert (tmp_path / "config.txt").read_text(encoding="utf-8") == original


def test_unit_host_case_is_ignored(tmp_path):
    web = _load("ra_unit_case", tmp_path)
    cfg = {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "https://example.com",
           "REDIRECT_EXEMPT": "anthropic.com"}
    assert web.should_redirect_all("API.Anthropic.COM", cfg) is None
    assert web.should_redirect_all("Google.COM", cfg) == "https://example.com"


def test_unit_target_with_port_and_path_keeps_full_url(tmp_path):
    # The 302 goes to the configured URL as written; only its host is exempt.
    web = _load("ra_unit_target", tmp_path)
    cfg = {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "https://example.com:8443/landing",
           "REDIRECT_EXEMPT": ""}
    assert web.should_redirect_all("other.test", cfg) == "https://example.com:8443/landing"
    assert web.should_redirect_all("example.com", cfg) is None


def test_unit_empty_exempt_key_uses_the_default_list(tmp_path):
    # The DNS side (ghostlink.sh) falls back to the default list when
    # REDIRECT_EXEMPT is empty. The HTTP side must match, or the chat domains
    # would resolve upstream but still be redirected.
    web = _load("ra_unit_empty_exempt", tmp_path)
    cfg = {"REDIRECT_ALL": "TRUE", "REDIRECT_ALL_TO": "https://example.com",
           "REDIRECT_EXEMPT": ""}
    assert web.should_redirect_all("api.anthropic.com", cfg) is None
    assert web.should_redirect_all("google.com", cfg) == "https://example.com"
