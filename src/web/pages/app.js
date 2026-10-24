// GhostLink WebUI — single-file SPA, no deps.
const $ = s => document.querySelector(s);
// Escape config- or filesystem-derived text before it goes into innerHTML.
const esc = v => (v == null ? '' : String(v)).replace(/[&<>"']/g,
  c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));

async function api(path, opts = {}) {
  const r = await fetch(path, opts);
  const t = await r.text();
  try { return JSON.parse(t); } catch { return t; }
}

async function refreshStatus() {
  const s = await api('/api/status');
  if (typeof s === 'string') { $('#status').textContent = s; return; }
  $('#status').innerHTML = `
    <div><b>${esc(s.name)}</b> &nbsp;·&nbsp; host <code>${esc(s.hostname)}</code> &nbsp;·&nbsp; cfg-gen ${s.cfg_gen}</div>
    <div style="margin-top:10px">
      netmode: <b>${esc(s.netmode)}</b> · usb: <b>${esc(s.usb_mode)}</b>
      · passthrough: ${s.passthrough ? '✔' : '✘'}
      · recording: ${s.recording ? '✔' : '✘'}
      · verdict stream: ${s.verdict_stream ? '✔' : '✘'}
    </div>
    <div style="margin-top:6px">
      deadnet: ${s.deadnet ? '⚠ ON' : 'off'}
      · redirect: ${s.redirect ? '⚠ ON' : 'off'}
      · honeypot: ${s.honeypot ? '⚠ ON' : 'off'}
      · hid: ${s.hid ? '✔' : 'off'}
    </div>`;
}

async function refreshRecords() {
  const l = await api('/api/records');
  if (!Array.isArray(l)) { $('#recordlist').textContent = JSON.stringify(l); return; }
  if (!l.length) { $('#recordlist').textContent = '(no recordings yet)'; return; }
  const rows = l.sort((a, b) => b.mtime - a.mtime).map(f => {
    const dt = new Date(f.mtime * 1000).toISOString().replace('T', ' ').slice(0, 19);
    const mb = (f.size / 1048576).toFixed(2);
    return `<tr><td><code>${esc(f.name)}</code></td><td>${mb} MB</td><td>${dt}</td></tr>`;
  }).join('');
  $('#recordlist').innerHTML = `<table style="width:100%"><tr>
    <th style="text-align:left">file</th><th>size</th><th>last change</th></tr>${rows}</table>`;
}

async function loadCfg() {
  const t = await api('/api/config');
  $('#cfg').value = typeof t === 'string' ? t : JSON.stringify(t);
}

async function saveCfg() {
  const body = $('#cfg').value;
  const r = await api('/api/config', { method: 'POST', body });
  if (r.ok) { refreshStatus(); flash('config saved', 'ok'); }
  else      { flash('save failed', 'danger'); }
}

async function runQuery() {
  const qs = new URLSearchParams();
  if ($('#qhost').value) qs.set('host', $('#qhost').value);
  if ($('#qrule').value) qs.set('rule', $('#qrule').value);
  const r = await fetch('/api/query?' + qs.toString());
  $('#qout').textContent = await r.text();
}

async function runHid() {
  const p = $('#pl').value;
  if (!p) return;
  const r = await api('/api/hid', { method: 'POST', body: p });
  flash(r.ok ? 'payload fired' : 'payload failed', r.ok ? 'ok' : 'danger');
}

async function toggleKey(key) {
  const t = await api('/api/config');
  if (typeof t !== 'string') return;
  const re = new RegExp('^' + key + '=.*$', 'm');
  const cur = (t.match(re) || [''])[0];
  const next = cur.endsWith('TRUE') ? `${key}=FALSE` : `${key}=TRUE`;
  const body = cur ? t.replace(re, next) : (t + `\n${next}\n`);
  await api('/api/config', { method: 'POST', body });
  refreshStatus();
}

async function reboot() {
  if (!confirm('Reboot now?')) return;
  await api('/api/reboot', { method: 'POST' });
}

function flash(msg, cls) {
  const el = document.createElement('div');
  el.textContent = msg;
  el.style.cssText =
    `position:fixed;bottom:16px;right:16px;padding:10px 14px;border-radius:4px;background:var(--panel);
     border:1px solid var(--${cls});color:var(--${cls});`;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), 1800);
}

document.addEventListener('click', e => {
  const b = e.target.closest('button[data-fn]');
  if (!b) return;
  const fn = b.dataset.fn;
  const m = {
    toggleRedirect:  () => toggleKey('REDIRECT'),
    toggleRecording: () => toggleKey('LOG_TO_SD'),
    toggleDeadnet:   () => toggleKey('DEADNET'),
    reboot,
    runQuery,
    runHid,
    loadCfg,
    saveCfg,
  }[fn];
  if (m) m();
});

loadCfg();
refreshStatus();
refreshRecords();
setInterval(refreshStatus, 3000);
setInterval(refreshRecords, 10000);
