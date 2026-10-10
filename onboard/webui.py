#!/usr/bin/env python3
"""
GhostLink web UI (on-device, no busybox httpd needed).

Serves:
  /                 - dashboard (HTML inline)
  /app.js           - JS inline
  /style.css        - CSS inline
  /api/status       - JSON
  /api/config       - GET: config.txt verbatim; POST: overwrite + nudge
  /api/records      - list of recorded pcap+jsonl pairs
  /api/query        - stream of verdicts filtered by host= or rule=
  /api/hid          - fire a .ducky payload (POST body = path)
  /api/reboot       - POST reboot
"""

import http.server
import json
import os
import socketserver
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.parse import urlparse, parse_qs

ROOT     = Path(os.environ.get("GL_ROOT", "/root/ghostlink"))
CFG_PATH = ROOT / "config.txt"
REC_DIR  = ROOT / "records"
VER_DIR  = ROOT / "verdicts"


def load_cfg():
    cfg = {}
    if not CFG_PATH.exists():
        return cfg
    for ln in CFG_PATH.read_text().splitlines():
        s = ln.strip()
        if not s or s.startswith("#") or "=" not in s:
            continue
        k, v = s.split("=", 1)
        cfg[k.strip()] = v.split("#", 1)[0].strip()
    return cfg


HTML = """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>GhostLink</title><link rel="stylesheet" href="/style.css"></head>
<body>
<header><h1>GhostLink <span class="dot"></span></h1>
<nav><a href="#live">Live</a><a href="#records">Records</a>
<a href="#query">Query</a><a href="#config">Config</a><a href="#hid">HID</a></nav>
</header>
<section id="live"><h2>Live status</h2>
<div id="status" class="card">loading...</div>
<div class="row">
  <button data-fn="toggleRedirect">Toggle REDIRECT</button>
  <button data-fn="toggleRecord" class="warn">Toggle RECORD_PCAP</button>
  <button data-fn="reboot" class="danger">Reboot</button>
</div></section>
<section id="records"><h2>Records on device (/root)</h2>
<div id="recordlist" class="card">loading...</div></section>
<section id="query"><h2>Query verdict stream</h2>
<p>Filter the live JSONL side-channel. Example: <code>host=google.com</code> or
<code>rule=REDIRECT</code>. Scans the newest file first.</p>
<div class="row"><input id="qhost" placeholder="host= (contains)">
<input id="qrule" placeholder="rule= (DNS, REDIRECT, ...)">
<button data-fn="runQuery">Run</button></div>
<pre id="qout" class="card mono">(idle)</pre></section>
<section id="config"><h2>config.txt</h2>
<p>Save hot-reloads most keys. Changing ENABLE_HID requires a gadget rebind.</p>
<textarea id="cfg" rows="26" spellcheck="false"></textarea>
<div class="row"><button data-fn="loadCfg">Reload</button>
<button data-fn="saveCfg" class="ok">Save</button></div></section>
<section id="hid"><h2>HID / BadUSB</h2>
<p>Only runs after ENABLE_HID=TRUE and the device has rebound (/dev/hidg0).</p>
<div class="row"><input id="pl" placeholder="/root/ghostlink/payloads/hello.ducky">
<button data-fn="runHid" class="warn">Fire</button></div></section>
<footer>GhostLink shell PoC - <a href="https://github.com/QWavey/webbee-ghostlink">source</a></footer>
<script src="/app.js"></script></body></html>
"""

CSS = """:root{--bg:#0a0d10;--panel:#141a20;--fg:#e4e8ec;--muted:#7d8691;
--accent:#4ac0a1;--warn:#d9a450;--danger:#e05858;--ok:#4ac0a1;
--mono:ui-monospace,"SF Mono",Menlo,Consolas,monospace;}
*{box-sizing:border-box}html,body{margin:0;background:var(--bg);color:var(--fg);
font:15px/1.5 system-ui,-apple-system,sans-serif}
header{padding:20px 24px;display:flex;align-items:center;justify-content:space-between;
border-bottom:1px solid rgba(255,255,255,.06)}
header h1{margin:0;font-weight:600;font-size:20px}
header .dot{display:inline-block;width:8px;height:8px;border-radius:50%;
background:var(--accent);margin-left:6px;animation:pulse 1.6s infinite}
@keyframes pulse{50%{opacity:.35}}
nav a{color:var(--muted);margin-left:16px;text-decoration:none}
nav a:hover{color:var(--fg)}
section{padding:24px;max-width:1100px}
section h2{margin:0 0 10px;font-weight:500;font-size:16px;color:var(--muted);
text-transform:uppercase;letter-spacing:1px}
.card{background:var(--panel);border:1px solid rgba(255,255,255,.05);
border-radius:6px;padding:16px;margin-bottom:12px}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
button,input,textarea{background:var(--panel);color:var(--fg);
border:1px solid rgba(255,255,255,.1);padding:8px 12px;border-radius:4px;font:inherit}
button{cursor:pointer}button.ok{border-color:var(--ok);color:var(--ok)}
button.warn{border-color:var(--warn);color:var(--warn)}
button.danger{border-color:var(--danger);color:var(--danger)}
textarea{width:100%;min-height:460px;font-family:var(--mono);font-size:13px}
input{flex:1 1 240px}
pre.mono,code{font-family:var(--mono);font-size:13px}
pre.mono{max-height:400px;overflow:auto;white-space:pre-wrap}
footer{padding:24px;color:var(--muted);text-align:center;font-size:13px}
footer a{color:var(--accent)}
table{width:100%;border-collapse:collapse}td,th{padding:4px 8px;text-align:left}"""

JS = r"""const $=s=>document.querySelector(s);
async function api(p,o={}){const r=await fetch(p,o);const t=await r.text();
  try{return JSON.parse(t)}catch{return t}}
async function refreshStatus(){
  const s=await api('/api/status');
  $('#status').innerHTML=`<div><b>${s.name}</b> &middot; host <code>${s.hostname}</code>
    &middot; cfg-gen ${s.cfg_gen}</div>
    <div style="margin-top:10px">iface: <b>${s.sniff_iface}</b>
    &middot; record: ${s.record_pcap?'ON':'off'} &middot; verdict: ${s.verdict?'ON':'off'}
    &middot; redirect: ${s.redirect?'<span style="color:var(--warn)">ON</span>':'off'}
    &middot; hid: ${s.hid?'ON':'off'}</div>
    <div style="margin-top:6px;color:var(--muted)">records on disk: ${s.records_count},
    disk free: ${s.free_mb} MB</div>`;}
async function refreshRecords(){
  const l=await api('/api/records');
  if(!l.length){$('#recordlist').textContent='(no recordings yet)';return}
  const rows=l.sort((a,b)=>b.mtime-a.mtime).map(f=>{
    const dt=new Date(f.mtime*1000).toISOString().replace('T',' ').slice(0,19);
    const kb=(f.size/1024).toFixed(1);
    return `<tr><td><code>${f.name}</code></td><td>${kb} KB</td><td>${dt}</td></tr>`
  }).join('');
  $('#recordlist').innerHTML='<table><tr><th>file</th><th>size</th><th>mtime</th></tr>'+rows+'</table>';}
async function loadCfg(){const t=await api('/api/config');$('#cfg').value=t;}
async function saveCfg(){const r=await api('/api/config',{method:'POST',body:$('#cfg').value});
  if(r.ok){refreshStatus();flash('saved','ok')}else{flash('save failed','danger')}}
async function runQuery(){const qs=new URLSearchParams();
  if($('#qhost').value)qs.set('host',$('#qhost').value);
  if($('#qrule').value)qs.set('rule',$('#qrule').value);
  const r=await fetch('/api/query?'+qs);$('#qout').textContent=await r.text();}
async function runHid(){const p=$('#pl').value;if(!p)return;
  const r=await api('/api/hid',{method:'POST',body:p});
  flash(r.ok?'fired':'failed',r.ok?'ok':'danger');}
async function toggleKey(k){const t=await api('/api/config');if(typeof t!=='string')return;
  const re=new RegExp('^'+k+'=.*$','m');const cur=(t.match(re)||[''])[0];
  const next=cur.endsWith('TRUE')?`${k}=FALSE`:`${k}=TRUE`;
  const body=cur?t.replace(re,next):t+`\n${next}\n`;
  await api('/api/config',{method:'POST',body});refreshStatus();}
async function reboot(){if(!confirm('Reboot now?'))return;
  await api('/api/reboot',{method:'POST'});}
function flash(m,c){const e=document.createElement('div');e.textContent=m;
  e.style.cssText=`position:fixed;bottom:16px;right:16px;padding:10px 14px;border-radius:4px;
    background:var(--panel);border:1px solid var(--${c});color:var(--${c})`;
  document.body.appendChild(e);setTimeout(()=>e.remove(),1800);}
document.addEventListener('click',e=>{const b=e.target.closest('button[data-fn]');
  if(!b)return;const f=b.dataset.fn;const m={toggleRedirect:()=>toggleKey('REDIRECT'),
    toggleRecord:()=>toggleKey('RECORD_PCAP'),reboot,runQuery,runHid,loadCfg,saveCfg}[f];
  if(m)m();});
loadCfg();refreshStatus();refreshRecords();
setInterval(refreshStatus,3000);setInterval(refreshRecords,10000);"""


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_a, **_k):
        pass

    def _send(self, code, ctype, body):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _free_mb(self):
        try:
            st = os.statvfs("/root")
            return int(st.f_bavail * st.f_bsize / 1024 / 1024)
        except Exception:
            return -1

    def _status(self):
        cfg = load_cfg()
        REC_DIR.mkdir(parents=True, exist_ok=True)
        records_n = len(list(REC_DIR.glob("rec_*.pcap")))
        s = {
            "name":        cfg.get("DEVICE_NAME", "WebBee-Ghost"),
            "hostname":    cfg.get("HOSTNAME", "ghostlink"),
            "sniff_iface": cfg.get("SNIFF_IFACE", "eth0"),
            "record_pcap": cfg.get("RECORD_PCAP", "TRUE").upper() == "TRUE",
            "verdict":     cfg.get("VERDICT_STREAM", "TRUE").upper() == "TRUE",
            "redirect":    cfg.get("REDIRECT", "FALSE").upper() == "TRUE",
            "hid":         cfg.get("ENABLE_HID", "FALSE").upper() == "TRUE",
            "records_count": records_n,
            "free_mb":     self._free_mb(),
            "cfg_gen":     int(CFG_PATH.stat().st_mtime) if CFG_PATH.exists() else 0,
        }
        return s

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/" or u.path == "/index.html":
            self._send(200, "text/html; charset=utf-8", HTML)
        elif u.path == "/style.css":
            self._send(200, "text/css", CSS)
        elif u.path == "/app.js":
            self._send(200, "application/javascript", JS)
        elif u.path == "/api/status":
            self._send(200, "application/json", json.dumps(self._status()))
        elif u.path == "/api/config":
            txt = CFG_PATH.read_text() if CFG_PATH.exists() else ""
            self._send(200, "text/plain", txt)
        elif u.path == "/api/records":
            REC_DIR.mkdir(parents=True, exist_ok=True)
            out = []
            for p in REC_DIR.iterdir():
                try:
                    st = p.stat()
                    out.append({"name": p.name, "size": st.st_size, "mtime": int(st.st_mtime)})
                except Exception:
                    pass
            for p in VER_DIR.iterdir() if VER_DIR.exists() else []:
                try:
                    st = p.stat()
                    out.append({"name": p.name, "size": st.st_size, "mtime": int(st.st_mtime)})
                except Exception:
                    pass
            self._send(200, "application/json", json.dumps(out))
        elif u.path == "/api/query":
            qs = parse_qs(u.query)
            host = (qs.get("host", [""])[0]).lower()
            rule = (qs.get("rule", [""])[0]).upper()
            VER_DIR.mkdir(parents=True, exist_ok=True)
            files = sorted(VER_DIR.glob("rec_*.jsonl"),
                           key=lambda p: p.stat().st_mtime, reverse=True)
            lines = []
            for f in files[:3]:
                try:
                    for ln in f.read_text().splitlines():
                        lh = ln.lower()
                        if host and host not in lh: continue
                        if rule and f'"rule":"{rule}"' not in ln: continue
                        lines.append(ln)
                        if len(lines) >= 500: break
                except Exception:
                    pass
                if len(lines) >= 500: break
            self._send(200, "application/x-ndjson", "\n".join(lines))
        else:
            self._send(404, "text/plain", "not found\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        if self.path == "/api/config":
            tmp = CFG_PATH.with_suffix(".new")
            tmp.write_bytes(body)
            tmp.replace(CFG_PATH)
            self._send(200, "application/json", '{"ok":true}')
        elif self.path == "/api/hid":
            p = body.decode("utf-8", "replace").strip()
            if not p: return self._send(400, "application/json", '{"ok":false}')
            ok = subprocess.call(["python3", str(ROOT / "ducky.py"), p]) == 0
            self._send(200 if ok else 500, "application/json",
                       json.dumps({"ok": ok}))
        elif self.path == "/api/reboot":
            self._send(200, "application/json", '{"ok":true}')
            threading.Thread(target=lambda: (time.sleep(1), os.system("reboot"))).start()
        else:
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
