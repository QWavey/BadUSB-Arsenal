"""Local CA for HTTPS interception with device-signed certs.

Generate a CA on the device (once), export it for manual install on the client PC,
then mint per-host certs for any domain listed in config.
"""
import subprocess
import os
from pathlib import Path
from datetime import datetime, timedelta


class CAManager:
    """Manage a local CA and per-host certificates."""

    def __init__(self, ca_root="/root/ghostlink/certs"):
        self.ca_root = Path(ca_root)
        self.ca_key = self.ca_root / "ca.key"
        self.ca_cert = self.ca_root / "ca.crt"
        self.hosts_dir = self.ca_root / "hosts"

    def ensure_ca(self):
        """Create CA key/cert if not exists. Idempotent."""
        if self.ca_key.exists() and self.ca_cert.exists():
            return

        self.ca_root.mkdir(parents=True, exist_ok=True)
        self.hosts_dir.mkdir(exist_ok=True)

        # Generate CA private key (2048-bit RSA)
        subprocess.run([
            "openssl", "genrsa", "-out", str(self.ca_key), "2048"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # Generate self-signed CA certificate (10-year validity)
        subprocess.run([
            "openssl", "req", "-new", "-x509", "-days", "3650",
            "-key", str(self.ca_key), "-out", str(self.ca_cert),
            "-subj", "/C=DE/ST=Hessen/L=Remote/O=GhostLink/CN=GhostLink Root CA"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def get_ca_cert_pem(self):
        """Return CA certificate in PEM format."""
        self.ensure_ca()
        return self.ca_cert.read_text()

    def _cert_exists_and_valid(self, cert_file, days_min=7):
        """Check if cert exists and is valid for at least days_min more days."""
        if not cert_file.exists():
            return False

        try:
            # Parse expiry with openssl
            result = subprocess.run([
                "openssl", "x509", "-enddate", "-noout", "-in", str(cert_file)
            ], capture_output=True, text=True, check=True)

            # Format: "notAfter=Nov 15 12:34:56 2025 GMT"
            datestr = result.stdout.split("=")[1].strip()
            expiry = datetime.strptime(datestr, "%b %d %H:%M:%S %Y %Z")
            remaining = (expiry - datetime.utcnow()).days
            return remaining > days_min
        except Exception:
            return False

    def get_host_cert(self, hostname, days=30):
        """
        Generate or return existing cert for hostname.

        Returns: (cert_pem, key_pem) tuple, both as strings.
        Reuses existing cert if valid for > 7 more days.
        """
        self.ensure_ca()

        # Sanitize hostname to filesystem-safe name (replace * with wildcard marker)
        safe_name = hostname.replace("*.", "_wc_").replace(".", "_")
        key_file = self.hosts_dir / f"{safe_name}.key"
        cert_file = self.hosts_dir / f"{safe_name}.crt"

        # Reuse valid existing cert
        if self._cert_exists_and_valid(cert_file, days_min=7):
            return cert_file.read_text(), key_file.read_text()

        # Generate new key for this host
        subprocess.run([
            "openssl", "genrsa", "-out", str(key_file), "2048"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # Generate CSR (Certificate Signing Request)
        csr_file = self.hosts_dir / f"{safe_name}.csr"
        subprocess.run([
            "openssl", "req", "-new",
            "-key", str(key_file), "-out", str(csr_file),
            "-subj", f"/C=DE/ST=Hessen/L=Remote/O=GhostLink/CN={hostname}"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # Sign CSR with CA key
        subprocess.run([
            "openssl", "x509", "-req", "-days", str(days),
            "-in", str(csr_file),
            "-CA", str(self.ca_cert), "-CAkey", str(self.ca_key),
            "-CAcreateserial", "-out", str(cert_file),
            "-sha256"
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        csr_file.unlink()

        return cert_file.read_text(), key_file.read_text()

    def cleanup_certs(self, older_than_days=180):
        """Delete host certs older than N days (maintenance)."""
        cutoff = datetime.utcnow() - timedelta(days=older_than_days)

        for cert_file in self.hosts_dir.glob("*.crt"):
            try:
                mtime = datetime.utcfromtimestamp(cert_file.stat().st_mtime)
                if mtime < cutoff:
                    key_file = cert_file.with_suffix(".key")
                    cert_file.unlink(missing_ok=True)
                    key_file.unlink(missing_ok=True)
            except Exception:
                pass
