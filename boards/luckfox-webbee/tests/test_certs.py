"""Tests for local CA certificate generation."""
import os
import sys
import tempfile
import subprocess
from pathlib import Path

import pytest

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"
sys.path.insert(0, str(ONBOARD))

from certs import CAManager


def test_ca_initialization(tmp_path):
    """Test that CA can be initialized."""
    ca_mgr = CAManager(str(tmp_path / "certs"))
    ca_mgr.ensure_ca()

    assert ca_mgr.ca_key.exists()
    assert ca_mgr.ca_cert.exists()


def test_ca_cert_pem(tmp_path):
    """Test CA certificate can be exported as PEM."""
    ca_mgr = CAManager(str(tmp_path / "certs"))
    cert_pem = ca_mgr.get_ca_cert_pem()

    assert "-----BEGIN CERTIFICATE-----" in cert_pem
    assert "-----END CERTIFICATE-----" in cert_pem
    assert len(cert_pem) > 100


def test_host_cert_generation(tmp_path):
    """Test per-host certificate generation."""
    ca_mgr = CAManager(str(tmp_path / "certs"))

    cert_pem, key_pem = ca_mgr.get_host_cert("example.com")

    assert "-----BEGIN CERTIFICATE-----" in cert_pem
    assert "-----END CERTIFICATE-----" in cert_pem
    # Key can be in either PKCS#8 (PRIVATE KEY) or PKCS#1 (RSA PRIVATE KEY) format
    assert ("-----BEGIN PRIVATE KEY-----" in key_pem or "-----BEGIN RSA PRIVATE KEY-----" in key_pem)
    assert ("-----END PRIVATE KEY-----" in key_pem or "-----END RSA PRIVATE KEY-----" in key_pem)

    # Verify it was saved
    assert (tmp_path / "certs" / "hosts" / "example_com.crt").exists()
    assert (tmp_path / "certs" / "hosts" / "example_com.key").exists()


def test_host_cert_reuse(tmp_path):
    """Test that existing valid certs are reused."""
    ca_mgr = CAManager(str(tmp_path / "certs"))

    cert1, key1 = ca_mgr.get_host_cert("example.com")
    cert2, key2 = ca_mgr.get_host_cert("example.com")

    # Same cert should be returned (not regenerated)
    assert cert1 == cert2
    assert key1 == key2


def test_wildcard_cert(tmp_path):
    """Test wildcard domain certificate."""
    ca_mgr = CAManager(str(tmp_path / "certs"))

    cert_pem, key_pem = ca_mgr.get_host_cert("*.example.com")

    assert "-----BEGIN CERTIFICATE-----" in cert_pem

    # File should use a safe name
    assert (tmp_path / "certs" / "hosts" / "_wc_example_com.crt").exists()


def test_cert_cn_matches_hostname(tmp_path):
    """Test that certificate CN matches the requested hostname."""
    ca_mgr = CAManager(str(tmp_path / "certs"))

    cert_pem, key_pem = ca_mgr.get_host_cert("test.example.com")

    # Extract CN from certificate
    result = subprocess.run([
        "openssl", "x509", "-noout", "-subject"
    ], input=cert_pem, capture_output=True, text=True, check=True)

    assert "CN=test.example.com" in result.stdout or "CN = test.example.com" in result.stdout


def test_cert_is_signed_by_ca(tmp_path):
    """Test that host cert is signed by the CA."""
    ca_mgr = CAManager(str(tmp_path / "certs"))

    ca_cert_pem = ca_mgr.get_ca_cert_pem()
    host_cert_pem, _ = ca_mgr.get_host_cert("example.com")

    # Write certs to temp files for verification
    import tempfile
    with tempfile.NamedTemporaryFile(mode='w', suffix='.crt', delete=False) as f:
        f.write(ca_cert_pem)
        ca_cert_file = f.name

    with tempfile.NamedTemporaryFile(mode='w', suffix='.crt', delete=False) as f:
        f.write(host_cert_pem)
        host_cert_file = f.name

    try:
        # Verify the host cert with CA cert
        result = subprocess.run([
            "openssl", "verify", "-CAfile", ca_cert_file, host_cert_file
        ], capture_output=True, text=True)

        # openssl verify returns 0 on success
        assert result.returncode == 0, f"Cert verification failed: {result.stderr}"
    finally:
        Path(ca_cert_file).unlink(missing_ok=True)
        Path(host_cert_file).unlink(missing_ok=True)


def test_cleanup_old_certs(tmp_path):
    """Test that old certificates are cleaned up."""
    import time
    ca_mgr = CAManager(str(tmp_path / "certs"))

    # Generate a cert
    ca_mgr.get_host_cert("old.com")
    cert_file = tmp_path / "certs" / "hosts" / "old_com.crt"
    assert cert_file.exists()

    # Make it look old
    old_time = time.time() - (200 * 24 * 3600)  # 200 days ago
    os.utime(cert_file, (old_time, old_time))

    # Clean up old certs (older than 180 days)
    ca_mgr.cleanup_certs(older_than_days=180)

    # Old cert should be deleted
    assert not cert_file.exists()


def test_multiple_hosts(tmp_path):
    """Test generating certs for multiple hosts."""
    ca_mgr = CAManager(str(tmp_path / "certs"))

    hosts = ["example.com", "test.com", "another.org"]
    certs = {}

    for host in hosts:
        cert, key = ca_mgr.get_host_cert(host)
        certs[host] = cert
        assert "-----BEGIN CERTIFICATE-----" in cert

    # All certs should be different
    assert len(set(certs.values())) == len(hosts)
