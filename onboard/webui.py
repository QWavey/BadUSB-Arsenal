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

import http.server
import json
import os
import re
import secrets
import socketserver
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.parse import urlparse, parse_qs

ROOT       = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))
CFG_PATH   = ROOT / "config.txt"
REC_DIR    = ROOT / "records"
VER_DIR    = ROOT / "verdicts"
PAY_DIR    = ROOT / "payloads"       # scripts library
LAY_DIR    = ROOT / "layouts"        # keyboard layouts
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
    with SESSION_LOCK:
        v = SESSIONS.get(tok)
        if not v: return ""
        user, exp = v
        if time.time() > exp:
            del SESSIONS[tok]
            return ""
        # sliding window: refresh on each use
        SESSIONS[tok] = (user, time.time() + SESSION_TTL)
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


def load_cfg():
    cfg = {}
    if not CFG_PATH.exists(): return cfg
    for ln in CFG_PATH.read_text().splitlines():
        s = ln.strip()
        if not s or s.startswith("#") or "=" not in s: continue
        k, v = s.split("=", 1)
        cfg[k.strip()] = v.split("#", 1)[0].strip()
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
    <button class="tablinks"        data-tab="HID">HID</button>
    <button class="tablinks"        data-tab="Scripting">Scripting</button>
    <button class="tablinks"        data-tab="LiveType">Live Type</button>
    <button class="tablinks"        data-tab="Terminal">Terminal</button>
    <button class="tablinks"        data-tab="System">System</button>
  </nav>

  <!-- Disk pressure banner (shown by JS when warn_level > 0) -->
  <div id="warnBanner" class="warn-banner" style="display:none;"></div>

  <!-- =========== LIVE =========== -->
  <section id="tab-Live" class="tabcontent" style="display:block">
    <div class="flex-row" style="justify-content: space-between; align-items: center;">
      <h3>Live Monitor</h3>
      <div id="recBadge" class="status-badge">REC OFF</div>
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
        <input type="text" id="q-rule" placeholder="rule  (DNS | REDIRECT)" style="min-width:200px;">
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
      <h3>config.txt</h3>
      <div class="flex-row" style="gap:10px;">
        <button onclick="loadCfg()">Reload</button>
        <button onclick="saveCfg()">Save</button>
      </div>
    </div>
    <div class="section">
      <h4>Quick Redirect</h4>
      <p class="dim" style="font-size:13px;">
        Appends to <code>REDIRECT_FROM</code>, sets <code>REDIRECT=TRUE</code> and saves. dnsmasq picks it up on save.
      </p>
      <div class="setting-row flex-row" style="gap:10px; flex-wrap: wrap;">
        <input type="text" id="spoof-host" placeholder="newhost.com" class="flex-grow">
        <input type="text" id="spoof-to" placeholder="REDIRECT_TO (optional, defaults to 172.32.0.93)" style="min-width:260px;">
        <button onclick="addSpoof()">+ Append &amp; Save</button>
      </div>
    </div>
    <div class="section">
      <h4>Raw config</h4>
      <p class="dim" style="font-size:13px; margin-bottom:10px;">
        Save hot-reloads most keys. Changing <code>ENABLE_HID</code> or
        <code>USB_MODE</code> needs a reboot.
      </p>
      <textarea id="cfg" spellcheck="false" wrap="off"
                style="width:100%; min-height:420px;"></textarea>
    </div>
  </section>

  <!-- =========== HID =========== -->
  <section id="tab-HID" class="tabcontent">
    <h3>HID / BadUSB</h3>
    <div class="section">
      <p class="dim" style="font-size:13px;">
        Requires <code>ENABLE_HID=TRUE</code> + reboot so <code>/dev/hidg0</code>
        gets wired into the gadget. Without it, fire returns 500.
      </p>
      <div class="setting-row flex-row" style="gap:10px;">
        <input type="text" id="pl" placeholder="/root/ghostlink/payloads/hello.ducky" class="flex-grow">
        <button onclick="runHid()">Fire</button>
      </div>
    </div>
    <div class="section">
      <h4>Available Payloads</h4>
      <ul id="payloads" class="file-list"><li class="dim">&hellip;</li></ul>
    </div>
  </section>

  <!-- =========== SCRIPTING (DuckyScript IDE) =========== -->
  <section id="tab-Scripting" class="tabcontent">
    <div class="flex-row" style="justify-content:space-between; align-items:center;">
      <h3>DuckyScript IDE</h3>
      <div id="scriptStatus" class="dim" style="font-size:12px;">idle</div>
    </div>

    <div class="ide-grid">
      <!-- LEFT: library + layout picker -->
      <aside class="ide-side">
        <div class="section" style="margin:0;">
          <h4>Scripts <span class="dim" id="scriptCount"></span></h4>
          <div class="setting-row flex-row" style="gap:6px; margin-bottom:8px;">
            <input type="text" id="newScriptName" placeholder="new_name.ducky" style="min-width:0;" class="flex-grow">
            <button onclick="scriptsNew()">+ New</button>
          </div>
          <ul id="scriptList" class="file-list"><li class="dim">&hellip;</li></ul>
        </div>
        <div class="section" style="margin:12px 0 0;">
          <h4>Keyboard layout</h4>
          <select id="layoutSel" onchange="setLayout(this.value)" style="width:100%;"></select>
          <p class="dim" style="font-size:12px;margin-top:6px;">
            Affects STRING char lookup when payloads run. Set live, no reboot.
          </p>
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
        <label class="sw">
          <input type="checkbox" id="liveEn" checked>
          <span>Live typing enabled</span>
        </label>
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
          <button onclick="termClear()" class="btn-ghost">Clear</button>
          <button onclick="cliLogout()" class="btn-ghost">Logout</button>
        </div>
      </div>
      <pre id="termOut" class="log-pane" style="min-height:300px; max-height:420px;"></pre>
      <div class="flex-row" style="gap:8px; margin-top:10px;">
        <span class="dim" style="font-family:'Fira Code',monospace;">root@ghost:/root#</span>
        <input type="text" id="termIn" class="flex-grow"
               placeholder="type a command, Enter to run (↑/↓ for history)"
               style="font-family:'Fira Code','Consolas',monospace;" autocomplete="off">
      </div>
      <div class="dim" style="font-size:11px; margin-top:6px;">
        timeout 15 s per command &middot; stdout+stderr captured &middot;
        tip: <code>ls /root/ghostlink/</code>, <code>top -b -n1 | head -20</code>
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
  grid-template-columns: 260px 1fr;
  gap: 14px;
  margin: 20px 0;
}
.ide-side { min-width: 0; }
.ide-main { min-width: 0; display: flex; flex-direction: column; gap: 10px; }

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

/* ---- Live Type pane ---------------------------------------------------- */
label.sw { display: inline-flex; align-items: center; gap: 10px; cursor: pointer; user-select: none; }
label.sw input { accent-color: var(--primary); width: 16px; height: 16px; }

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
  if (name === 'HID')       refreshPayloads();
  if (name === 'Scripting') { scriptsRefresh(); layoutsRefresh(); }
  if (name === 'LiveType')  { $('#liveInput').focus(); }
  if (name === 'Terminal')  { cliProbe(); setTimeout(() => $('#termIn') && $('#termIn').focus(), 50); }
}

/* --- live tick ------------------------------------------------- */
async function tick() {
  try {
    const [st, live] = await Promise.all([api('/api/status'), api('/api/live')]);
    const b = $('#recBadge');
    if (st.record_pcap) { b.classList.add('on'); b.textContent = 'REC ON'; }
    else                { b.classList.remove('on'); b.textContent = 'REC OFF'; }
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
      vb.innerHTML = live.tail.slice().reverse().map(v => `<tr>
        <td>${v.f ?? ''}</td>
        <td>${esc((v.t||'').split('.')[0])}</td>
        <td>${esc(v.flow || '')}</td>
        <td>${v.sz ?? ''}</td>
        <td class="rule-${v.rule||''}">${esc(v.rule || '')}</td>
        <td>${esc(v.host || v.detail || '')}</td>
      </tr>`).join('');
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
    return `<tr>
      <td>${v.f ?? ''}</td>
      <td>${esc((v.t||'').split('.')[0])}</td>
      <td>${esc(v.flow || '')}</td>
      <td>${v.sz ?? ''}</td>
      <td class="rule-${v.rule || ''}">${esc(v.rule || '')}</td>
      <td>${esc(v.host || v.detail || '')}</td>
    </tr>`;
  }).join('');
}

/* --- config --------------------------------------------------- */
async function loadCfg() { $('#cfg').value = await api('/api/config'); }
async function saveCfg() {
  const r = await api('/api/config', { method:'POST', body: $('#cfg').value });
  toast(r.ok ? 'saved' : 'save failed', !r.ok);
}
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
const TABS = ['Live','Records','Query','Config','HID','Scripting','LiveType','Terminal','System'];
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
    if (/^(STRING|DELAY|DEFAULTDELAY|DEFAULT_DELAY|DEFAULTCHARDELAY|REPEAT) /i.test(s)) return;
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
  const matches = DUCKY_KEYWORDS.filter(k => k.w.startsWith(word)).slice(0, 10);
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
  $('#editorTitle').textContent = name;
  $('#scriptStatus').textContent = 'opened ' + name;
  updateGutter();
  scriptsRefresh();
}
async function scriptSave() {
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
  if (!currentScript) return toast('nothing to run — save first', true);
  // auto-save then run so what you see is what fires
  await fetch('/api/scripts/' + encodeURIComponent(currentScript), {
    method:'POST', body: $('#scriptArea').value });
  $('#scriptStatus').textContent = 'running ' + currentScript + '...';
  const r = await api('/api/scripts/' + encodeURIComponent(currentScript) + '/run', { method:'POST' });
  if (r.ok) { toast('ran ' + currentScript); $('#scriptStatus').textContent = 'ok'; }
  else { toast('run failed rc=' + (r.rc != null ? r.rc : '?'), true);
         $('#scriptStatus').textContent = 'failed'; }
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
   Terminal (Web CLI)
   ============================================================ */
let termHist = [];
let termHistIdx = -1;

async function cliProbe() {
  try {
    const r = await fetch('/api/auth/me');
    const j = await r.json();
    if (j.authed) {
      $('#termLogin').style.display = 'none';
      $('#termView').style.display = 'block';
      $('#termUser').textContent = j.user;
    } else {
      $('#termLogin').style.display = 'block';
      $('#termView').style.display = 'none';
    }
  } catch(e) {
    $('#termLogin').style.display = 'block';
  }
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
  await fetch('/api/auth/logout', { method:'POST' });
  cliProbe();
}
function termAppend(html) {
  const out = $('#termOut');
  out.insertAdjacentHTML('beforeend', html);
  out.scrollTop = out.scrollHeight;
}
async function cliRun() {
  const cmd = $('#termIn').value;
  if (!cmd.trim()) return;
  termHist.push(cmd); if (termHist.length > 200) termHist.shift();
  termHistIdx = termHist.length;
  $('#termIn').value = '';
  termAppend(`<div><span class="cmd">root@ghost:/root# </span>${esc(cmd)}</div>`);
  const r = await fetch('/api/cli/exec', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({cmd})
  });
  if (r.status === 401) { cliProbe(); termAppend('<div class="err">session expired</div>'); return; }
  const j = await r.json();
  if (j.out) termAppend('<div>' + esc(j.out) + '</div>');
  if (j.err) termAppend('<div class="err">' + esc(j.err) + '</div>');
  const rcClass = j.rc === 0 ? 'rc0' : 'rc1';
  termAppend(`<div class="${rcClass}">[exit ${j.rc != null ? j.rc : (j.err||'?')}]</div>`);
}
function termClear() { $('#termOut').innerHTML = ''; }
function bindTerminal() {
  $('#termIn').addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); cliRun(); return; }
    if (e.key === 'ArrowUp') {
      e.preventDefault();
      if (termHistIdx > 0) { termHistIdx--; $('#termIn').value = termHist[termHistIdx] || ''; }
      return;
    }
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      if (termHistIdx < termHist.length - 1) { termHistIdx++; $('#termIn').value = termHist[termHistIdx]; }
      else { termHistIdx = termHist.length; $('#termIn').value = ''; }
      return;
    }
    if (e.key === 'l' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); termClear(); }
  });
  $('#loginPass').addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); cliLogin(); } });
}

/* --- boot: wire everything + first paint ---------------------------------- */
(function boot() {
  const h = (location.hash || '#Live').slice(1);
  openTab(TABS.includes(h) ? h : 'Live');
  loadCfg();
  tick();
  setInterval(tick, 2000);
  // Scripting
  bindEditor();
  updateGutter();
  scriptsRefresh();
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


# ====== HTTP handler ========================================================

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a, **k): pass

    def _send(self, code, ctype, body, extra=None):
        if isinstance(body, str): body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

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

    def _maybe_redirect(self):
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
        elif p == "/api/auth/me":
            u = self._auth_user()
            self._send_json({"authed": bool(u), "user": u or None})
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
            tmp = CFG_PATH.with_suffix(".new")
            tmp.write_bytes(body); tmp.replace(CFG_PATH)
            # kick dnsmasq to pick up new address= rules
            try: subprocess.run(["sh", str(ROOT/"ghostlink.sh"), "reload-dnsmasq"],
                                timeout=5, capture_output=True)
            except Exception: pass
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
            if tok: _session_drop(tok)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Set-Cookie", "gl_sess=; Path=/; Max-Age=0; SameSite=Strict")
            b = b'{"ok":true}'
            self.send_header("Content-Length", str(len(b)))
            self.end_headers(); self.wfile.write(b)
            return

        # ---- CLI exec (session-gated) ------------------------------------
        if u.path == "/api/cli/exec":
            if not self._require_auth(): return
            try: data = json.loads(body)
            except Exception: data = {}
            cmd = (data.get("cmd") or "").strip()
            if not cmd:
                return self._send_json({"ok": False, "err": "empty cmd"}, code=400)
            if len(cmd) > 8 * 1024:
                return self._send_json({"ok": False, "err": "cmd too long"}, code=413)
            try:
                r = subprocess.run(["sh", "-c", cmd],
                                   capture_output=True, timeout=15, cwd="/root")
                out = r.stdout.decode("utf-8", "replace")[-64*1024:]
                err = r.stderr.decode("utf-8", "replace")[-16*1024:]
                return self._send_json({"ok": True, "rc": r.returncode,
                                        "out": out, "err": err})
            except subprocess.TimeoutExpired:
                return self._send_json({"ok": False, "err": "timeout (15s)"}, code=504)
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
        self._send(404, "text/plain", "not found\n")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    cfg = load_cfg()
    port = int(cfg.get("WEBUI_PORT","80"))
    bind = cfg.get("WEBUI_BIND","0.0.0.0")
    s = Server((bind, port), Handler)
    sys.stderr.write(f"webui listening {bind}:{port}\n")
    s.serve_forever()


if __name__ == "__main__":
    main()
