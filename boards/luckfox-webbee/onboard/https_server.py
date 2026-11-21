#!/usr/bin/env python3
"""
HTTPS interception server for REDIRECT_ALL feature.

Fetches real certificates from target websites and forwards them.
No CA trust needed - client sees real website certificate.
"""
import os
import re
import sys
import ssl
import http.server
import json
import socket
import logging
import tempfile
from pathlib import Path
from urllib.parse import urlparse
from subprocess import run, PIPE


logger = logging.getLogger("https_server")
logger.setLevel(logging.DEBUG)
handler = logging.StreamHandler(sys.stderr)
handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s"))
logger.addHandler(handler)


class HTTPSHandler(http.server.BaseHTTPRequestHandler):
    """Handle HTTPS requests and redirect to REDIRECT_ALL_TO."""

    redirect_url = None
    redirect_exempt = set()
    cert_cache = {}  # hostname -> (cert_path, key_path)

    def do_GET(self):
        host = self.headers.get("Host", "").split(":")[0].lower()

        # Check if this host is in the exempt list
        if self._is_exempt(host):
            self.send_response(204)
            self.end_headers()
            return

        # Build the redirect URL
        target = self.redirect_url
        if target and "?" not in target:
            path = self.path or "/"
            if path != "/":
                parsed = urlparse(target)
                target = f"{parsed.scheme}://{parsed.netloc}{path}"

        logger.info(f"Redirect HTTPS {host}{self.path} -> {target}")
        self.send_response(302)
        self.send_header("Location", target)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        self.do_GET()

    def do_HEAD(self):
        self.do_GET()

    def _is_exempt(self, hostname):
        """Check if hostname is in the exempt list."""
        h = hostname.lower()
        for exempt in self.redirect_exempt:
            if h == exempt or h.endswith("." + exempt):
                return True
        return False

    def log_message(self, format, *args):
        pass


def fetch_certificate(hostname, port=443, timeout=10):
    """
    Fetch real certificate from a website via openssl.
    Returns (cert_pem, key_pem) or (None, None) on failure.
    """
    # Not called today (REDIRECT_ALL uses the wildcard cert directly, see _setup_ssl), but
    # kept as the real-cert-forwarding path this was originally built for and may wire back
    # in. hostname was being interpolated straight into a shell=True command string with no
    # escaping -- a config value (REDIRECT_ALL_TO's host) containing shell metacharacters
    # would execute as arbitrary commands, running as root. Hostnames (or anything else
    # reaching this function later) should never be trusted into a shell string: build the
    # argv list directly, no shell at all, so there is nothing to escape or inject into.
    if not re.match(r"^[A-Za-z0-9](?:[A-Za-z0-9.-]{0,253}[A-Za-z0-9])?$", hostname):
        logger.warning(f"Refusing to fetch cert for implausible hostname: {hostname!r}")
        return None, None
    try:
        result = run(
            ["openssl", "s_client", "-servername", hostname, "-connect", f"{hostname}:{port}"],
            input="", capture_output=True, text=True, timeout=timeout,
        )

        if result.returncode != 0:
            logger.warning(f"Failed to fetch cert for {hostname}: {result.stderr}")
            return None, None

        # Extract the certificate from openssl output
        output = result.stdout
        if "-----BEGIN CERTIFICATE-----" not in output:
            logger.warning(f"No certificate found for {hostname}")
            return None, None

        # Extract cert (between BEGIN and END markers)
        lines = output.split('\n')
        cert_lines = []
        in_cert = False
        for line in lines:
            if "-----BEGIN CERTIFICATE-----" in line:
                in_cert = True
            if in_cert:
                cert_lines.append(line)
            if "-----END CERTIFICATE-----" in line:
                break

        cert_pem = '\n'.join(cert_lines)

        # For the key, we don't have it from the server
        # We'll create a dummy key or use a pre-existing one
        logger.info(f"Fetched certificate for {hostname}")
        return cert_pem, None

    except Exception as e:
        logger.error(f"Error fetching cert for {hostname}: {e}")
        return None, None


class CertForwardingHTTPSServer(http.server.HTTPServer):
    """HTTPS server that fetches and forwards real website certificates."""

    allow_reuse_address = True

    def __init__(self, host, port, handler, redirect_target):
        self.redirect_target = redirect_target
        self.target_hostname = urlparse(redirect_target).hostname or "example.com"
        super().__init__((host, port), handler)
        self._setup_ssl()

    def _setup_ssl(self):
        """Set up SSL context with proper certificate handling."""
        try:
            # Use wildcard certificate (always available)
            wildcard_crt = Path("/root/ghostlink/certs/wildcard.crt")
            key_file_path = Path("/root/ghostlink/certs/wildcard.key")

            if not wildcard_crt.exists() or not key_file_path.exists():
                logger.error("Wildcard cert/key not found - cannot establish SSL")
                raise FileNotFoundError("Missing certificate files")

            # Create proper SSL context
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(str(wildcard_crt), str(key_file_path))

            # Wrap socket BEFORE calling serve_forever
            self.socket = ctx.wrap_socket(self.socket, server_side=True)
            logger.info(f"SSL enabled with wildcard certificate")

        except Exception as e:
            logger.error(f"SSL setup failed: {e}")
            raise


def load_config(root_path):
    """Load config.txt and extract relevant keys."""
    cfg_path = Path(root_path) / "config.txt"
    cfg = {}
    if cfg_path.exists():
        for line in cfg_path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    return cfg


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <root_path> [<listen_ip>]")
        sys.exit(1)

    root = Path(sys.argv[1])
    listen_ip = sys.argv[2] if len(sys.argv) > 2 else "0.0.0.0"
    listen_port = 443

    # Load config
    cfg = load_config(root)
    redirect_all = cfg.get("REDIRECT_ALL", "FALSE").upper() in ("TRUE", "YES", "ON", "1")
    redirect_to = cfg.get("REDIRECT_ALL_TO", "")
    redirect_exempt_str = cfg.get("REDIRECT_EXEMPT", "anthropic.com,claude.ai,claude.com")

    if not redirect_all:
        logger.error("REDIRECT_ALL not enabled in config")
        sys.exit(1)

    if not redirect_to:
        logger.error("REDIRECT_ALL_TO not configured")
        sys.exit(1)

    # Parse exempt list
    exempt_list = set()
    if redirect_exempt_str:
        for h in redirect_exempt_str.split(","):
            h = h.strip().lower()
            if h:
                exempt_list.add(h)

    logger.info(f"Starting HTTPS server on {listen_ip}:{listen_port}")
    logger.info(f"REDIRECT_ALL_TO: {redirect_to}")
    logger.info(f"REDIRECT_EXEMPT: {exempt_list}")

    # Set up handler class variables
    HTTPSHandler.redirect_url = redirect_to
    HTTPSHandler.redirect_exempt = exempt_list

    # Create and run server
    try:
        server = CertForwardingHTTPSServer(listen_ip, listen_port, HTTPSHandler, redirect_to)
        logger.info("HTTPS server running (Ctrl+C to stop)")
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("Shutting down")
    except Exception as e:
        logger.error(f"Server error: {e}", exc_info=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
