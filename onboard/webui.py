#!/usr/bin/env python3
"""
GhostLink operator console. Monospace, dense, keyboard-first.

Routes:
  /                       dashboard
  /app.js /style.css      SPA assets
  /api/status             firmware + config snapshot
  /api/live               live stats (pps, matches, iface counters, verdict tail)
  /api/config  GET/POST   config.txt (raw)
  /api/records            list of pcap+jsonl files
  /api/download/<name>    raw file download (pcap or jsonl)
  /api/delete             POST {"name":"rec_000001.pcap"}
  /api/clear              POST — nukes records + verdicts (confirm on client)
  /api/query?host=&rule=  verdict search
  /api/log                tail of ghostlink.log
  /api/leases             dnsmasq DHCP leases
  /api/ifaces             interface table
  /api/hid    POST        fire a ducky payload
  /api/reboot POST        reboot the box
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
# Operator console: everything one typeface, no cards, no shadows, no pulse,
# no rounded corners. Dense tables. Horizontal rules as section dividers.
# Two theme variants (dark = CRT phosphor, light = teletype paper) that both
# commit to the same "sits next to tcpdump" identity.

HTML = r"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ghostlink</title>
<link rel="stylesheet" href="/style.css">
</head><body>

<header id="topbar">
  <span class="brand">ghostlink</span>
  <span class="sep">|</span>
  <span id="t-host" class="muted">host &hellip;</span>
  <span class="sep">|</span>
  <span id="t-iface" class="muted">if &hellip;</span>
  <span class="sep">|</span>
  <span id="t-pps" class="num">0 pps</span>
  <span class="sep">|</span>
  <span id="t-mps" class="match">0 /m</span>
  <span class="sep">|</span>
  <span id="t-disk" class="muted">&hellip; MB free</span>
  <span class="sep">|</span>
  <span id="t-cfg" class="muted">cfg &hellip;</span>
  <span class="spacer"></span>
  <span id="t-rec" class="rec" title="RECORD_PCAP state">[-]</span>
</header>

<nav id="tabs">
  <a href="#live"    data-pane="live"   data-key="1"><span class="k">1</span>live</a>
  <a href="#records" data-pane="records" data-key="2"><span class="k">2</span>records</a>
  <a href="#query"   data-pane="query"   data-key="3"><span class="k">3</span>query</a>
  <a href="#config"  data-pane="config"  data-key="4"><span class="k">4</span>config</a>
  <a href="#hid"     data-pane="hid"     data-key="5"><span class="k">5</span>hid</a>
  <a href="#system"  data-pane="system"  data-key="6"><span class="k">6</span>system</a>
</nav>

<main>

<section id="pane-live" class="pane active">
  <div class="rule">live &mdash; verdict tail</div>
  <div id="live-info" class="row row-stats">loading&hellip;</div>
  <div class="rule">verdicts (last 50)</div>
  <table id="verdicts" class="t t-verdicts">
    <thead><tr>
      <th class="w-frame">frame</th>
      <th class="w-time">t</th>
      <th class="w-flow">flow</th>
      <th class="w-sz">sz</th>
      <th class="w-rule">rule</th>
      <th>host / detail</th>
    </tr></thead>
    <tbody id="verdicts-body"><tr><td colspan="6" class="muted">waiting&hellip;</td></tr></tbody>
  </table>
  <div class="rule">interfaces</div>
  <table id="ifaces" class="t">
    <thead><tr><th>name</th><th>state</th><th>ipv4</th><th>mac</th>
      <th class="num-th">rx pkts</th><th class="num-th">rx bytes</th>
      <th class="num-th">tx pkts</th><th class="num-th">tx bytes</th>
    </tr></thead>
    <tbody id="ifaces-body"><tr><td colspan="8" class="muted">&hellip;</td></tr></tbody>
  </table>
</section>

<section id="pane-records" class="pane">
  <div class="rule">records on <code>/root/ghostlink</code></div>
  <div class="row">
    <span class="muted" id="rec-summary">&hellip;</span>
    <span class="spacer"></span>
    <button data-fn="clearAll" class="danger">clear ALL records</button>
  </div>
  <table class="t t-records">
    <thead><tr>
      <th>name</th>
      <th class="num-th">size</th>
      <th>modified</th>
      <th class="w-actions">actions</th>
    </tr></thead>
    <tbody id="records-body"><tr><td colspan="4" class="muted">&hellip;</td></tr></tbody>
  </table>
</section>

<section id="pane-query" class="pane">
  <div class="rule">query verdict stream</div>
  <div class="row row-inputs">
    <label>host <input id="q-host" placeholder="example.com"></label>
    <label>rule <input id="q-rule" placeholder="DNS | REDIRECT"></label>
    <button data-fn="runQuery" data-key="return">run</button>
    <span class="muted hint">
      <kbd>/</kbd> focus query &nbsp; <kbd>return</kbd> run
    </span>
  </div>
  <div class="rule" id="q-result-rule">no query run yet</div>
  <table class="t t-verdicts">
    <thead><tr>
      <th class="w-frame">frame</th><th class="w-time">t</th>
      <th class="w-flow">flow</th><th class="w-sz">sz</th>
      <th class="w-rule">rule</th><th>host / detail</th>
    </tr></thead>
    <tbody id="query-body"></tbody>
  </table>
</section>

<section id="pane-config" class="pane">
  <div class="rule">config.txt &mdash; save hot-reloads most keys</div>
  <div class="row">
    <button data-fn="loadCfg">reload from device</button>
    <button data-fn="saveCfg" class="ok">save</button>
    <span class="spacer"></span>
    <label class="muted inline-spoof">
      quick-spoof <input id="spoof-host" placeholder="newhost.com">
      <button data-fn="addSpoof">+ append</button>
    </label>
  </div>
  <textarea id="cfg" spellcheck="false" wrap="off"></textarea>
</section>

<section id="pane-hid" class="pane">
  <div class="rule">HID / BadUSB</div>
  <p class="muted">
    Requires <code>ENABLE_HID=TRUE</code> + reboot so <code>/dev/hidg0</code>
    gets wired into the gadget. Without it, the fire button returns 500.
  </p>
  <div class="row row-inputs">
    <label>payload
      <input id="pl" placeholder="/root/ghostlink/payloads/hello.ducky">
    </label>
    <button data-fn="runHid" class="warn">fire</button>
  </div>
  <div class="rule">available payloads</div>
  <ul id="payloads" class="plain"><li class="muted">&hellip;</li></ul>
</section>

<section id="pane-system" class="pane">
  <div class="rule">dhcp leases <span class="muted">(dnsmasq)</span></div>
  <table class="t">
    <thead><tr><th>expires</th><th>mac</th><th>ipv4</th><th>hostname</th><th>id</th></tr></thead>
    <tbody id="leases-body"><tr><td colspan="5" class="muted">&hellip;</td></tr></tbody>
  </table>
  <div class="rule">ghostlink.log &mdash; tail</div>
  <pre id="log" class="scroll"></pre>
  <div class="rule">danger</div>
  <div class="row">
    <button data-fn="reboot" class="danger">reboot device</button>
  </div>
</section>

</main>

<footer id="statusbar">
  <span><kbd>1</kbd>&ndash;<kbd>6</kbd> switch</span>
  <span><kbd>/</kbd> focus query</span>
  <span><kbd>r</kbd> reload records</span>
  <span><kbd>s</kbd> save config</span>
  <span><kbd>esc</kbd> clear focus</span>
  <span class="spacer"></span>
  <span id="t-uptime">uptime &hellip;</span>
</footer>

<script src="/app.js"></script>
</body></html>
"""

# ====== CSS =================================================================
# Design system — justify every value. All sizes/weights/colors live here.
#   Font:    one mono stack; no system-ui, no proportional. Mono fits the
#            product (packet inspector) and all data is tabular.
#   Scale:   11 / 12 / 13 / 14 / 18 / 24 px. 6 real steps, not 6 close ones.
#   Palette: dark = CRT phosphor; light = teletype paper (NOT cream).
#   Signal:  one color — amber (var(--signal)). Match green and alert red
#            carry semantic meaning only.
#   Depth:   zero shadows. One hairline on separators. Radius = 0 everywhere.

CSS = r""":root {
  --bg: #0e0e10; --panel: #15151a; --line: #2a2a2f;
  --fg: #d4d4c8; --dim: #6b6b63;
  --signal: #e5a95f;    /* CRT amber — the only accent */
  --alert:  #d16c6c;    /* muted red, not neon */
  --match:  #9ab87a;    /* muted olive, verdict hits */
  --font-mono: "JetBrains Mono","Berkeley Mono","SF Mono",Consolas,Menlo,monospace;
  --f-xs: 11px; --f-sm: 12px; --f-base: 13px; --f-lg: 14px; --f-xl: 18px; --f-xxl: 24px;
  --sp-1: 4px; --sp-2: 8px; --sp-3: 16px; --sp-4: 24px; --sp-5: 40px;
}

@media (prefers-color-scheme: light) {
  :root:not([data-theme="dark"]) {
    --bg: #efece3;      /* teletype paper, not cream — slightly cooler, carbon-tinted */
    --panel: #e5e2d8;
    --line: #b8b4a8;
    --fg: #1e1e22;
    --dim: #6e6a60;
    --signal: #a05a0f;  /* same amber hue, saturated down for paper */
    --alert:  #a63838;
    --match:  #4f6c2b;
  }
}

* { box-sizing: border-box; margin: 0; padding: 0; }

html, body {
  background: var(--bg); color: var(--fg);
  font-family: var(--font-mono); font-size: var(--f-base);
  line-height: 1.55;
  -webkit-font-smoothing: antialiased;
}

body {
  display: flex; flex-direction: column; min-height: 100vh;
}

code, kbd, input, textarea, button { font-family: var(--font-mono); }

/* ---- top bar ---------------------------------------------------- */
#topbar {
  display: flex; align-items: center; gap: var(--sp-2);
  padding: var(--sp-2) var(--sp-3);
  border-bottom: 1px solid var(--line);
  font-size: var(--f-sm);
  flex-wrap: wrap;
}
#topbar .brand {
  color: var(--signal); font-weight: 400;
  letter-spacing: 0;    /* no crushed tracking */
}
#topbar .sep { color: var(--line); }
#topbar .muted { color: var(--dim); }
#topbar .num { color: var(--fg); }
#topbar .match { color: var(--match); }
#topbar .rec { color: var(--dim); }
#topbar .rec.on { color: var(--alert); }  /* functional, no animation */
#topbar .spacer { flex: 1; }

/* ---- tabs ------------------------------------------------------- */
#tabs {
  display: flex; border-bottom: 1px solid var(--line);
  padding: 0 var(--sp-3);
}
#tabs a {
  color: var(--dim); text-decoration: none;
  padding: var(--sp-2) var(--sp-3);
  border-right: 1px solid var(--line);
  font-size: var(--f-sm);
}
#tabs a:first-child { border-left: 1px solid var(--line); }
#tabs a .k {
  color: var(--signal); margin-right: var(--sp-2);
  display: inline-block; min-width: 1ch;
}
#tabs a:hover { color: var(--fg); }
#tabs a.active { color: var(--fg); background: var(--panel); }
#tabs a.active .k { color: var(--signal); }

/* ---- main / panes ---------------------------------------------- */
main { flex: 1; padding: var(--sp-3); overflow-x: auto; }

.pane { display: none; }
.pane.active { display: block; }

/* horizontal-rule section headings replace eyebrow/kicker labels */
.rule {
  color: var(--dim);
  margin: var(--sp-4) 0 var(--sp-2);
  padding-bottom: var(--sp-1);
  border-bottom: 1px solid var(--line);
  font-size: var(--f-sm);
  letter-spacing: 0;
  text-transform: none;    /* explicitly not uppercase */
}
.pane > .rule:first-child { margin-top: 0; }

.row {
  display: flex; gap: var(--sp-3); flex-wrap: wrap; align-items: center;
  margin-bottom: var(--sp-2);
}
.row-stats { gap: var(--sp-4); font-size: var(--f-sm); }
.row-stats b { color: var(--fg); font-weight: 400; }
.row-inputs label { display: flex; align-items: center; gap: var(--sp-2); }

.spacer { flex: 1; }

/* ---- form controls -------------------------------------------- */
input, textarea, button {
  background: var(--bg); color: var(--fg);
  border: 1px solid var(--line);
  padding: var(--sp-1) var(--sp-2);
  font-size: var(--f-base);
  border-radius: 0;        /* explicitly sharp */
}
input { min-width: 20ch; }
input:focus, textarea:focus, button:focus {
  outline: 1px solid var(--signal); outline-offset: -1px;
}
button { cursor: pointer; }
button:hover { border-color: var(--signal); color: var(--signal); }
button.ok:hover    { border-color: var(--match); color: var(--match); }
button.warn:hover  { border-color: var(--signal); color: var(--signal); }
button.danger      { border-color: var(--alert); color: var(--alert); }
button.danger:hover{ background: var(--alert); color: var(--bg); }

textarea {
  width: 100%; min-height: 480px;
  font-size: var(--f-base); line-height: 1.5;
  white-space: pre; overflow: auto; tab-size: 2;
}

.inline-spoof input { margin-left: var(--sp-1); }

/* ---- tables (information density) ---------------------------- */
.t { width: 100%; border-collapse: collapse; font-size: var(--f-base); }
.t th, .t td {
  padding: var(--sp-1) var(--sp-2);
  text-align: left; vertical-align: top;
  border-bottom: 1px solid var(--line);
  white-space: nowrap;
}
.t th {
  color: var(--dim); font-weight: 400;
  border-bottom-color: var(--line);
}
.t td.muted { color: var(--dim); }
.t .num-th, .t .num { text-align: right; font-variant-numeric: tabular-nums; }
.t tbody tr:hover { background: var(--panel); }
.t-verdicts td.rule-REDIRECT { color: var(--signal); }
.t-verdicts td.rule-DNS      { color: var(--match); }
.t-verdicts .w-frame { width: 6ch;  color: var(--dim); }
.t-verdicts .w-time  { width: 14ch; color: var(--dim); }
.t-verdicts .w-flow  { width: 10ch; color: var(--dim); }
.t-verdicts .w-sz    { width: 6ch;  text-align: right; }
.t-verdicts .w-rule  { width: 10ch; }
.t-records .w-actions { width: 20ch; }
.t-records td.name { color: var(--fg); }
.t-records button {
  padding: 0 var(--sp-2); font-size: var(--f-xs);
  margin-right: var(--sp-1);
}

/* ---- misc ---------------------------------------------------- */
code { color: var(--signal); padding: 0 2px; }
.muted { color: var(--dim); }
.hint  { font-size: var(--f-sm); margin-left: auto; }
kbd {
  background: var(--panel); border: 1px solid var(--line);
  padding: 0 var(--sp-1); font-size: var(--f-xs);
}
ul.plain { list-style: none; }
ul.plain li {
  padding: var(--sp-1) 0; border-bottom: 1px solid var(--line);
  font-size: var(--f-sm);
}
ul.plain li a { color: var(--fg); text-decoration: none; }
ul.plain li a:hover { color: var(--signal); }

pre.scroll {
  background: var(--panel); border: 1px solid var(--line);
  padding: var(--sp-2); max-height: 320px; overflow: auto;
  font-size: var(--f-sm); color: var(--fg);
  white-space: pre-wrap;
}

/* ---- status bar -------------------------------------------- */
#statusbar {
  display: flex; gap: var(--sp-4);
  padding: var(--sp-1) var(--sp-3);
  border-top: 1px solid var(--line);
  font-size: var(--f-xs); color: var(--dim);
  position: sticky; bottom: 0; background: var(--bg);
}

/* ---- blink only on functional REC indicator ------------------- */
.rec.on { animation: blink 1.6s steps(2) infinite; }
@keyframes blink { 50% { opacity: 0.3; } }

/* keep layout honest on narrow screens */
@media (max-width: 760px) {
  #tabs { overflow-x: auto; }
  main { padding: var(--sp-2); }
  .t { font-size: var(--f-sm); }
  textarea { min-height: 300px; }
}
"""

# ====== JS ==================================================================
# No framework. ~200 lines. Keyboard-first, polls three endpoints.

JS = r"""
const $  = s => document.querySelector(s);
const $$ = s => document.querySelectorAll(s);

const fmtBytes = n => {
  if (n == null) return '-';
  const u = ['B','K','M','G'];
  let i = 0; while (n >= 1024 && i < u.length-1) { n /= 1024; i++; }
  return (i === 0 ? n : n.toFixed(1)) + u[i];
};
const fmtTime = ts => {
  const d = new Date(ts * 1000);
  const p = n => String(n).padStart(2,'0');
  return `${d.getFullYear()}-${p(d.getMonth()+1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
};
const esc = s => String(s ?? '').replace(/[&<>"]/g,
  c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

async function api(path, opts) {
  const r = await fetch(path, opts || {});
  const t = await r.text();
  try { return JSON.parse(t); } catch { return t; }
}

/* --- pane routing ---------------------------------------------- */
function activate(name) {
  $$('#tabs a').forEach(a => a.classList.toggle('active', a.dataset.pane === name));
  $$('.pane').forEach(p => p.classList.toggle('active', p.id === 'pane-' + name));
  if (location.hash !== '#'+name) location.hash = name;
  if (name === 'records') refreshRecords();
  if (name === 'system')  refreshSystem();
  if (name === 'hid')     refreshPayloads();
}

/* --- top bar + live pane --------------------------------------- */
async function tick() {
  try {
    const [st, live] = await Promise.all([api('/api/status'), api('/api/live')]);
    $('#t-host').textContent  = 'host ' + st.hostname;
    $('#t-iface').textContent = 'if ' + st.sniff_iface;
    $('#t-disk').textContent  = (live.free_mb ?? st.free_mb) + ' MB free';
    $('#t-cfg').textContent   = 'cfg ' + st.cfg_gen;
    $('#t-pps').textContent   = (live.pps ?? 0) + ' pps';
    $('#t-mps').textContent   = (live.mps ?? 0) + ' /s match';
    const recOn = st.record_pcap;
    const rec = $('#t-rec');
    rec.textContent = recOn ? '[REC]' : '[---]';
    rec.classList.toggle('on', recOn);
    $('#t-uptime').textContent =
      `uptime ${st.uptime || '-'} · verdicts ${st.records_count} · free ${live.free_mb} MB`;

    /* live info row */
    $('#live-info').innerHTML =
      `<span>frames <b>${live.frames ?? '-'}</b></span>` +
      `<span>matches <b>${live.matches ?? '-'}</b></span>` +
      `<span>pps <b>${live.pps ?? 0}</b></span>` +
      `<span>match/s <b>${live.mps ?? 0}</b></span>` +
      `<span>iface <b>${live.iface || '-'}</b></span>` +
      `<span>rx pkts <b>${(live.if_rx_p ?? 0).toLocaleString()}</b></span>` +
      `<span>tx pkts <b>${(live.if_tx_p ?? 0).toLocaleString()}</b></span>`;

    /* verdict tail */
    const vb = $('#verdicts-body');
    if (live.tail && live.tail.length) {
      vb.innerHTML = live.tail.map(v => {
        const rule = v.rule || '';
        const host = v.host || v.detail || '';
        return `<tr>
          <td>${v.f ?? ''}</td>
          <td>${esc((v.t||'').split('.')[0])}</td>
          <td>${esc(v.flow || '')}</td>
          <td class="num">${v.sz ?? ''}</td>
          <td class="rule-${rule}">${esc(rule)}</td>
          <td>${esc(host)}</td>
        </tr>`;
      }).reverse().join('');
    }

    /* ifaces */
    const ib = $('#ifaces-body');
    if (live.ifaces && live.ifaces.length) {
      ib.innerHTML = live.ifaces.map(i =>
        `<tr><td>${esc(i.name)}</td><td>${esc(i.state)}</td>
         <td>${esc(i.ipv4||'-')}</td><td>${esc(i.mac||'-')}</td>
         <td class="num">${(i.rx_p||0).toLocaleString()}</td>
         <td class="num">${fmtBytes(i.rx_b)}</td>
         <td class="num">${(i.tx_p||0).toLocaleString()}</td>
         <td class="num">${fmtBytes(i.tx_b)}</td>
        </tr>`).join('');
    }
  } catch (e) { /* ignore */ }
}

/* --- records --------------------------------------------------- */
async function refreshRecords() {
  const l = await api('/api/records');
  const b = $('#records-body');
  if (!Array.isArray(l) || !l.length) {
    b.innerHTML = '<tr><td colspan="4" class="muted">no files</td></tr>';
    $('#rec-summary').textContent = '0 files, 0 B';
    return;
  }
  l.sort((a,b)=> b.mtime - a.mtime);
  const total = l.reduce((s,f)=> s + f.size, 0);
  $('#rec-summary').textContent = `${l.length} files, ${fmtBytes(total)}`;
  b.innerHTML = l.map(f => `<tr>
    <td class="name">${esc(f.name)}</td>
    <td class="num">${fmtBytes(f.size)}</td>
    <td>${fmtTime(f.mtime)}</td>
    <td>
      <a href="/api/download/${encodeURIComponent(f.name)}" download>download</a>
      &nbsp;&middot;&nbsp;
      <a href="#" onclick="delRec('${f.name}');return false">delete</a>
    </td></tr>`).join('');
}

async function delRec(name) {
  if (!confirm('delete ' + name + ' ?')) return;
  await api('/api/delete', { method:'POST', body: JSON.stringify({name}) });
  refreshRecords();
}

async function clearAll() {
  if (!confirm('nuke ALL records + verdicts? this cannot be undone.')) return;
  await api('/api/clear', { method:'POST' });
  refreshRecords();
}

/* --- query ----------------------------------------------------- */
async function runQuery() {
  const qs = new URLSearchParams();
  if ($('#q-host').value) qs.set('host', $('#q-host').value);
  if ($('#q-rule').value) qs.set('rule', $('#q-rule').value);
  const r = await fetch('/api/query?' + qs);
  const txt = await r.text();
  const lines = txt.trim().split('\n').filter(Boolean);
  $('#q-result-rule').textContent =
    `${lines.length} match${lines.length === 1 ? '' : 'es'}` +
    ($('#q-host').value ? ` for host~"${$('#q-host').value}"` : '') +
    ($('#q-rule').value ? ` rule=${$('#q-rule').value}` : '');
  const b = $('#query-body');
  if (!lines.length) { b.innerHTML = '<tr><td colspan="6" class="muted">no matches</td></tr>'; return; }
  b.innerHTML = lines.slice(0,500).map(l => {
    let v = {}; try { v = JSON.parse(l); } catch {}
    return `<tr>
      <td>${v.f ?? ''}</td>
      <td>${esc((v.t||'').split('.')[0])}</td>
      <td>${esc(v.flow||'')}</td>
      <td class="num">${v.sz ?? ''}</td>
      <td class="rule-${v.rule||''}">${esc(v.rule||'')}</td>
      <td>${esc(v.host || v.detail || '')}</td>
    </tr>`;
  }).join('');
}

/* --- config ---------------------------------------------------- */
async function loadCfg() { $('#cfg').value = await api('/api/config'); }
async function saveCfg() {
  const r = await api('/api/config', { method:'POST', body: $('#cfg').value });
  flash(r.ok ? 'saved' : 'save failed', r.ok ? 'ok' : 'err');
}
async function addSpoof() {
  const h = $('#spoof-host').value.trim(); if (!h) return;
  let txt = $('#cfg').value || await api('/api/config');
  if (/^REDIRECT=.*$/m.test(txt))      txt = txt.replace(/^REDIRECT=.*$/m, 'REDIRECT=TRUE');
  else                                  txt += '\nREDIRECT=TRUE\n';
  if (/^REDIRECT_FROM=.*$/m.test(txt)) {
    txt = txt.replace(/^REDIRECT_FROM=.*$/m, (m) => {
      const [k, v] = m.split('=');
      const list = (v || '').split(/[,\s]+/).filter(Boolean);
      if (!list.includes(h)) list.push(h);
      return k + '=' + list.join(',');
    });
  } else {
    txt += `REDIRECT_FROM=${h}\n`;
  }
  $('#cfg').value = txt;
  $('#spoof-host').value = '';
  saveCfg();
}

/* --- hid ------------------------------------------------------- */
async function refreshPayloads() {
  const l = await api('/api/records');  /* just to warm */
  const ul = $('#payloads');
  try {
    const r = await fetch('/api/payloads');
    const files = await r.json();
    if (!files.length) { ul.innerHTML = '<li class="muted">no payloads in /root/ghostlink/payloads</li>'; return; }
    ul.innerHTML = files.map(f =>
      `<li><a href="#" onclick="firePayload('${f}');return false">${esc(f)}</a></li>`
    ).join('');
  } catch { ul.innerHTML = '<li class="muted">could not list payloads</li>'; }
}
async function firePayload(p) {
  const r = await api('/api/hid', { method:'POST', body: p });
  flash(r.ok ? 'fired ' + p : 'HID failed (ENABLE_HID=TRUE?)', r.ok ? 'ok' : 'err');
}
async function runHid() { firePayload($('#pl').value); }

/* --- system (dhcp, log) --------------------------------------- */
async function refreshSystem() {
  const [leases, log] = await Promise.all([api('/api/leases'), api('/api/log')]);
  const lb = $('#leases-body');
  if (!Array.isArray(leases) || !leases.length) {
    lb.innerHTML = '<tr><td colspan="5" class="muted">no leases</td></tr>';
  } else {
    lb.innerHTML = leases.map(l =>
      `<tr><td>${fmtTime(l.expires)}</td><td>${esc(l.mac)}</td>
       <td>${esc(l.ip)}</td><td>${esc(l.host||'-')}</td>
       <td class="muted">${esc(l.id||'-')}</td></tr>`).join('');
  }
  $('#log').textContent = typeof log === 'string' ? log : '';
}

async function reboot() {
  if (!confirm('reboot?')) return;
  await api('/api/reboot', { method:'POST' });
}

/* --- toast (one-liner, inline bottom, not floating card) -------- */
function flash(msg, cls) {
  const el = document.createElement('div');
  el.textContent = '[ ' + msg + ' ]';
  el.style.cssText =
    'position:fixed;left:0;right:0;bottom:28px;text-align:center;'+
    'color:var(--'+(cls==='err'?'alert':'match')+');font-size:var(--f-sm);'+
    'background:var(--bg);padding:2px 0;';
  document.body.appendChild(el);
  setTimeout(()=>el.remove(), 1600);
}

/* --- keyboard -------------------------------------------------- */
window.addEventListener('keydown', e => {
  const t = e.target.tagName;
  if (t === 'INPUT' || t === 'TEXTAREA') {
    if (e.key === 'Escape') e.target.blur();
    if (e.key === 'Enter' && (e.target.id === 'q-host' || e.target.id === 'q-rule')) {
      e.preventDefault(); runQuery();
    }
    if (e.key === 's' && (e.ctrlKey||e.metaKey) && e.target.id === 'cfg') {
      e.preventDefault(); saveCfg();
    }
    return;
  }
  const map = {'1':'live','2':'records','3':'query','4':'config','5':'hid','6':'system'};
  if (map[e.key]) { activate(map[e.key]); return; }
  if (e.key === '/') { e.preventDefault(); activate('query'); $('#q-host').focus(); return; }
  if (e.key === 'r' && location.hash === '#records') { refreshRecords(); return; }
  if (e.key === 's' && location.hash === '#config')  { saveCfg(); return; }
});

/* --- clicks ---------------------------------------------------- */
document.addEventListener('click', e => {
  const t = e.target.closest('[data-pane]');
  if (t) { e.preventDefault(); activate(t.dataset.pane); return; }
  const b = e.target.closest('button[data-fn]');
  if (!b) return;
  const fn = { runQuery, loadCfg, saveCfg, addSpoof, runHid, reboot, clearAll }[b.dataset.fn];
  if (fn) fn();
});

/* --- boot ------------------------------------------------------ */
(function boot() {
  const h = (location.hash || '#live').slice(1);
  activate(['live','records','query','config','hid','system'].includes(h) ? h : 'live');
  loadCfg();
  tick();
  setInterval(tick, 2000);
})();
"""


# ====== HTTP handler ========================================================

def ifaces_snapshot():
    out = []
    base = Path("/sys/class/net")
    if not base.exists(): return out
    # read /proc/net/dev for a canonical list
    try:
        with open("/proc/net/dev") as f:
            for ln in f.readlines()[2:]:
                name = ln.split(":")[0].strip()
                if not name: continue
                s = Path(f"/sys/class/net/{name}/statistics")
                stat = {}
                for key, fn in (("rx_b","rx_bytes"), ("tx_b","tx_bytes"),
                                ("rx_p","rx_packets"), ("tx_p","tx_packets")):
                    try: stat[key] = int((s/fn).read_text())
                    except Exception: stat[key] = 0
                # ipv4
                ipv4 = None
                try:
                    r = subprocess.run(["ip","-4","-o","addr","show","dev",name],
                                       capture_output=True, text=True, timeout=2)
                    m = re.search(r"inet (\S+)", r.stdout)
                    if m: ipv4 = m.group(1)
                except Exception: pass
                mac = None
                try: mac = (Path(f"/sys/class/net/{name}/address").read_text().strip())
                except Exception: pass
                state = "down"
                try: state = (Path(f"/sys/class/net/{name}/operstate").read_text().strip())
                except Exception: pass
                out.append({"name": name, "state": state, "ipv4": ipv4, "mac": mac, **stat})
    except Exception:
        pass
    return out


def tail_file(p: Path, n_lines: int = 20) -> str:
    try:
        # cheap: read last 32KB
        with open(p, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            back = min(size, 32768)
            f.seek(size - back)
            tail = f.read().decode("utf-8", "replace")
        return "\n".join(tail.splitlines()[-n_lines:])
    except Exception:
        return ""


def read_dhcp_leases():
    out = []
    if not LEASE_PATH.exists(): return out
    try:
        for ln in LEASE_PATH.read_text().splitlines():
            parts = ln.split()
            if len(parts) < 4: continue
            out.append({
                "expires": int(parts[0]),
                "mac":     parts[1],
                "ip":      parts[2],
                "host":    parts[3] if parts[3] != "*" else None,
                "id":      parts[4] if len(parts) > 4 else None,
            })
    except Exception: pass
    return out


def read_stats():
    try: return json.loads(STATS_PATH.read_text())
    except Exception: return {}


def tail_verdicts(n=50):
    try:
        files = sorted(VER_DIR.glob("rec_*.jsonl"), key=lambda p: p.stat().st_mtime, reverse=True)
    except Exception:
        return []
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


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a, **k): pass

    def _send(self, code, ctype, body, extra_hdrs=None):
        if isinstance(body, str): body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra_hdrs or {}).items(): self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, obj, code=200):
        self._send(code, "application/json", json.dumps(obj))

    def _free_mb(self):
        try:
            st = os.statvfs("/root")
            return int(st.f_bavail * st.f_bsize / 1024 / 1024)
        except Exception: return -1

    def do_GET(self):
        u = urlparse(self.path)
        p = u.path
        if p == "/" or p == "/index.html":
            self._send(200, "text/html; charset=utf-8", HTML)
        elif p == "/style.css":
            self._send(200, "text/css", CSS)
        elif p == "/app.js":
            self._send(200, "application/javascript", JS)
        elif p == "/api/status":
            cfg = load_cfg()
            recs = len(list(REC_DIR.glob("rec_*.pcap"))) if REC_DIR.exists() else 0
            self._send_json({
                "name":        cfg.get("DEVICE_NAME", "WebBee-Ghost"),
                "hostname":    cfg.get("HOSTNAME", "ghostlink"),
                "sniff_iface": cfg.get("SNIFF_IFACE", "usb0"),
                "record_pcap": cfg.get("RECORD_PCAP","TRUE").upper() == "TRUE",
                "verdict":     cfg.get("VERDICT_STREAM","TRUE").upper() == "TRUE",
                "redirect":    cfg.get("REDIRECT","FALSE").upper() == "TRUE",
                "hid":         cfg.get("ENABLE_HID","FALSE").upper() == "TRUE",
                "records_count": recs,
                "free_mb":     self._free_mb(),
                "cfg_gen":     int(CFG_PATH.stat().st_mtime) if CFG_PATH.exists() else 0,
                "uptime":      uptime_str(),
            })
        elif p == "/api/live":
            st = read_stats()
            st["ifaces"] = ifaces_snapshot()
            st["tail"]   = tail_verdicts(50)
            if "free_mb" not in st: st["free_mb"] = self._free_mb()
            self._send_json(st)
        elif p == "/api/config":
            txt = CFG_PATH.read_text() if CFG_PATH.exists() else ""
            self._send(200, "text/plain", txt)
        elif p == "/api/records":
            out = []
            for d, pat in ((REC_DIR, "*.pcap"), (VER_DIR, "*.jsonl")):
                if not d.exists(): continue
                for f in d.iterdir():
                    try:
                        st = f.stat()
                        out.append({"name": f.name, "size": st.st_size, "mtime": int(st.st_mtime)})
                    except Exception: pass
            self._send_json(out)
        elif p == "/api/payloads":
            d = ROOT / "payloads"
            out = []
            if d.exists():
                for f in d.iterdir():
                    if f.is_file() and f.suffix in (".ducky", ".txt"):
                        out.append(f.name)
            self._send_json(sorted(out))
        elif p == "/api/leases":
            self._send_json(read_dhcp_leases())
        elif p == "/api/log":
            self._send(200, "text/plain", tail_file(LOG_PATH, 40))
        elif p.startswith("/api/download/"):
            name = p[len("/api/download/"):]
            # strict allowlist: must match rec_NNNNNN.pcap or rec_NNNNNN.jsonl
            if not re.match(r"^rec_\d{6}\.(pcap|jsonl)$", name):
                return self._send(400, "text/plain", "bad name")
            d = REC_DIR if name.endswith(".pcap") else VER_DIR
            fp = d / name
            if not fp.exists(): return self._send(404, "text/plain", "not found")
            body = fp.read_bytes()
            self._send(200, "application/octet-stream", body,
                       extra_hdrs={"Content-Disposition": f'attachment; filename="{name}"'})
        elif p == "/api/query":
            qs = parse_qs(u.query)
            host = (qs.get("host", [""])[0]).lower()
            rule = (qs.get("rule", [""])[0]).upper()
            files = sorted(VER_DIR.glob("rec_*.jsonl"),
                           key=lambda p: p.stat().st_mtime, reverse=True) if VER_DIR.exists() else []
            lines = []
            for f in files[:4]:
                try:
                    for ln in f.read_text().splitlines():
                        lh = ln.lower()
                        if host and host not in lh: continue
                        if rule and f'"rule":"{rule}"' not in ln: continue
                        lines.append(ln)
                        if len(lines) >= 500: break
                except Exception: pass
                if len(lines) >= 500: break
            self._send(200, "application/x-ndjson", "\n".join(lines))
        else:
            self._send(404, "text/plain", "not found\n")

    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        if u.path == "/api/config":
            tmp = CFG_PATH.with_suffix(".new")
            tmp.write_bytes(body)
            tmp.replace(CFG_PATH)
            return self._send_json({"ok": True})
        if u.path == "/api/clear":
            removed = 0
            for d, pat in ((REC_DIR, "*.pcap"), (VER_DIR, "*.jsonl")):
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
                return self._send_json({"ok": False, "reason": "bad name"}, code=400)
            d = REC_DIR if name.endswith(".pcap") else VER_DIR
            try: (d / name).unlink(); return self._send_json({"ok": True})
            except Exception as e: return self._send_json({"ok": False, "err": str(e)}, code=500)
        if u.path == "/api/hid":
            p = body.decode("utf-8","replace").strip()
            if not p: return self._send_json({"ok": False}, code=400)
            ok = subprocess.call(["python3", str(ROOT / "ducky.py"), p]) == 0
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
    port = int(cfg.get("WEBUI_PORT", "80"))
    bind = cfg.get("WEBUI_BIND", "0.0.0.0")
    s = Server((bind, port), Handler)
    sys.stderr.write(f"webui listening {bind}:{port}\n")
    s.serve_forever()


if __name__ == "__main__":
    main()
