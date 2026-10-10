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
LOG_PATH   = ROOT / "logs" / "ghostlink.log"
STATS_PATH = Path("/tmp/ghostlink.stats.json")
LEASE_PATH = Path("/tmp/dnsmasq.leases")


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
  if (name === 'Records') refreshRecords();
  if (name === 'System')  refreshSystem();
  if (name === 'HID')     refreshPayloads();
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
window.addEventListener('hashchange', () => {
  const h = location.hash.slice(1);
  if (['Live','Records','Query','Config','HID','System'].includes(h)) openTab(h);
});
(function boot() {
  const h = (location.hash || '#Live').slice(1);
  openTab(['Live','Records','Query','Config','HID','System'].includes(h) ? h : 'Live');
  loadCfg();
  tick();
  setInterval(tick, 2000);
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

    def _maybe_redirect(self):
        host = self.headers.get("Host", "")
        cfg = load_cfg()
        to = should_redirect(host, cfg)
        if to is None: return False
        if not to.startswith("http"): to = "http://" + to
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
        else:
            self._send(404, "text/plain", "not found\n")

    def do_POST(self):
        if self._maybe_redirect(): return
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        if u.path == "/api/config":
            tmp = CFG_PATH.with_suffix(".new")
            tmp.write_bytes(body); tmp.replace(CFG_PATH)
            # kick dnsmasq to pick up new address= rules
            try: subprocess.run(["sh", str(ROOT/"ghostlink.sh"), "reload-dnsmasq"],
                                timeout=5, capture_output=True)
            except Exception: pass
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
            ok = subprocess.call(["python3", str(ROOT/"ducky.py"), payload]) == 0
            return self._send_json({"ok": ok}, code=200 if ok else 500)
        if u.path == "/api/reboot":
            self._send_json({"ok": True})
            threading.Thread(target=lambda: (time.sleep(1), os.system("reboot"))).start()
            return
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
