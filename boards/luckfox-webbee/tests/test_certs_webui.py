"""Tests for certificate endpoints in webui.py."""
import http.client
import importlib.util
import io
import json
import os
import sys
import threading
import types
from pathlib import Path

import pytest

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"

# Stub Linux-only modules for Windows testing
for _name in ("fcntl", "pty", "termios"):
    try:
        importlib.import_module(_name)
    except ImportError:
        sys.modules[_name] = types.ModuleType(_name)


def _load(root):
    os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location("webui_certs_test", ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _request(web, method, path, body=None, admin=True):
    """Helper to make HTTP requests to the test server."""
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=30)
        headers = {}
        if body:
            headers["Content-Length"] = str(len(body))
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        resp_body = resp.read()
        try:
            return resp.status, json.loads(resp_body) if resp_body else {}
        except json.JSONDecodeError:
            return resp.status, resp_body.decode("utf-8", "replace")
    finally:
        srv.shutdown()
        srv.server_close()


@pytest.fixture
def web(tmp_path, monkeypatch):
    """Load webui module with test root directory."""
    mod = _load(tmp_path)
    # Override admin requirement for testing
    monkeypatch.setattr(mod.Handler, "_require_admin", lambda self: True)
    return mod


def test_ca_cert_download(web, tmp_path):
    """Test downloading the CA certificate."""
    status, body = _request(web, "GET", "/api/certs/ca")

    assert status == 200
    assert isinstance(body, str)
    assert "-----BEGIN CERTIFICATE-----" in body
    assert "-----END CERTIFICATE-----" in body


def test_host_cert_generation(web, tmp_path):
    """Test generating a per-host certificate."""
    req_body = json.dumps({"hostname": "example.com"})
    status, resp = _request(web, "POST", "/api/certs/gen", body=req_body)

    assert status == 200
    assert resp["ok"] is True
    assert resp["hostname"] == "example.com"
    assert "-----BEGIN CERTIFICATE-----" in resp["cert"]
    assert "-----BEGIN PRIVATE KEY-----" in resp["key"] or "-----BEGIN RSA PRIVATE KEY-----" in resp["key"]


def test_host_cert_no_hostname(web, tmp_path):
    """Test cert generation without hostname fails."""
    req_body = json.dumps({"hostname": ""})
    status, resp = _request(web, "POST", "/api/certs/gen", body=req_body)

    assert status == 400
    assert resp["ok"] is False
    assert "required" in resp["err"].lower()


def test_host_cert_invalid_hostname(web, tmp_path):
    """Test cert generation with invalid hostname fails."""
    req_body = json.dumps({"hostname": "example\x00com"})
    status, resp = _request(web, "POST", "/api/certs/gen", body=req_body)

    assert status == 400
    assert resp["ok"] is False


def test_ca_cert_persists(web, tmp_path):
    """Test that CA certificate persists across requests."""
    # First download
    status1, cert1 = _request(web, "GET", "/api/certs/ca")

    # Second download
    status2, cert2 = _request(web, "GET", "/api/certs/ca")

    assert status1 == 200
    assert status2 == 200
    assert cert1 == cert2


def test_host_cert_caching(web, tmp_path):
    """Test that host certificates are cached."""
    # Generate first
    req_body = json.dumps({"hostname": "cache-test.com"})
    status1, resp1 = _request(web, "POST", "/api/certs/gen", body=req_body)

    # Generate again
    status2, resp2 = _request(web, "POST", "/api/certs/gen", body=req_body)

    assert status1 == 200
    assert status2 == 200
    assert resp1["cert"] == resp2["cert"]
    assert resp1["key"] == resp2["key"]


def test_wildcard_cert(web, tmp_path):
    """Test generating wildcard certificate."""
    req_body = json.dumps({"hostname": "*.example.com"})
    status, resp = _request(web, "POST", "/api/certs/gen", body=req_body)

    assert status == 200
    assert resp["ok"] is True
    assert "-----BEGIN CERTIFICATE-----" in resp["cert"]


def test_multiple_hosts(web, tmp_path):
    """Test generating certs for different hosts."""
    hosts = ["host1.com", "host2.com", "host3.com"]
    certs = {}

    for host in hosts:
        req_body = json.dumps({"hostname": host})
        status, resp = _request(web, "POST", "/api/certs/gen", body=req_body)
        assert status == 200
        certs[host] = resp["cert"]

    # All certs should be different
    assert len(set(certs.values())) == len(hosts)
