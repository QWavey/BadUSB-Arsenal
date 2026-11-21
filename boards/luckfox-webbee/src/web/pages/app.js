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
    <div><b>${esc(s.name)}</b> &nbsp;·&nbsp; host <code>${esc(s.hostname)}</code> &nbsp;·&nbsp; cfg-gen ${esc(s.cfg_gen)}</div>
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

// Reads config.txt. Throws on an error reply, so no error text ends up in the editor.
// A 404 counts as an empty file only when missingOk is set, so a toggle never rewrites a file it could not read.
async function configText(missingOk = false) {
  const r = await fetch('/api/config');
  if (r.status === 404 && missingOk) return '';   // a missing config.txt loads as an empty editor
  if (!r.ok) throw new Error('GET /api/config ' + r.status);
  // fatal: an invalid byte throws instead of being silently replaced
  const buf = await r.arrayBuffer();
  return new TextDecoder('utf-8', { fatal: true }).decode(buf);
}

function loadCfg() {
  return enqueue(async () => {
    try { $('#cfg').value = await configText(true); }
    catch (e) { flash('config.txt could not be read', 'danger'); }
  });
}

function saveCfg() {
  return enqueue(async () => {
    const body = $('#cfg').value;
    const r = await api('/api/config', { method: 'POST', body });
    if (r && r.ok === true) { refreshStatus(); flash('config saved', 'ok'); }
    else                    { flash('save failed', 'danger'); }
  });
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

// Booleans follow the firmware rules: TRUE, YES, ON or 1, in any case.
const TRUTHY = ['TRUE', 'YES', 'ON', '1'];

// Config reads and writes run one at a time, so a reload, a save and a toggle never overlap.
let configQueue = Promise.resolve();
function enqueue(fn) {
  configQueue = configQueue.then(() => {
    // a stalled request must not block the queue forever: the job is raced against a 10 s timer that rejects
    // the timer starts when the job starts, not while it waits in the queue
    let timer;
    const limit = new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('timeout')), 10000); });
    return Promise.race([fn(), limit]).finally(() => clearTimeout(timer));
  }).catch(() => flash('config request failed', 'danger'));
  return configQueue;
}

// Documented defaults of the toggles. A missing, blank or unknown value means the default.
const DEFAULT_ON = { LOG_TO_SD: true };
const FALSY = ['FALSE', 'NO', 'OFF', '0'];

function toggleKey(key) { return enqueue(() => toggleKeyNow(key)); }

async function toggleKeyNow(key) {
  const t = await configText();
  const eol = t.includes('\r\n') ? '\r\n' : '\n';   // keep the file's line endings
  const lines = t.split(/\r?\n/);
  const keyRe = new RegExp('^\\s*' + key + '\\s*=', 'i');   // the firmware matches keys in any case
  let idx = -1;                                      // the last assignment wins
  lines.forEach((ln, i) => { if (keyRe.test(ln)) idx = i; });
  if (idx < 0) {
    lines.push(`${key}=${DEFAULT_ON[key] ? 'FALSE' : 'TRUE'}`);
  } else {
    const ln = lines[idx];
    const rest = ln.slice(ln.indexOf('=') + 1);
    // '#' starts a comment only at the start of the value or after whitespace
    const cut = rest.match(/(^|\s+)#/);
    const value = (cut ? rest.slice(0, cut.index) : rest).trim().toUpperCase();
    const on = TRUTHY.includes(value) || (!FALSY.includes(value) && !!DEFAULT_ON[key]);
    const tail = !cut ? '' : (cut.index === 0 ? ' ' + rest : rest.slice(cut.index));
    lines[idx] = `${key}=${on ? 'FALSE' : 'TRUE'}${tail}`;
  }
  const next = lines.join(eol);
  const r = await api('/api/config', { method: 'POST', body: next });
  if (!r || r.ok !== true) throw new Error('POST /api/config failed');
  // the textarea holds LF line endings: compare and update in that form, and never overwrite unsaved edits
  const lf = s => s.replace(/\r\n/g, '\n');
  if ($('#cfg').value === lf(t)) $('#cfg').value = lf(next);
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
