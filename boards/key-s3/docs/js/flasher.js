// ESP-S3-Key WebFlasher
// Flashes the ESP32-S3 Key BadUSB firmware over WebSerial using esptool-js
// (the Espressif JS port of esptool). The default target is the full image set
// — bootloader + partition table + boot_app0 + app, each at its offset, from
// firmware/manifest.json — so it can flash a bare chip. A chosen .bin or .espkg
// is treated as an app-only update at the app offset (0x10000).

import { ESPLoader, Transport } from '../vendor/esptool-bundle.js';

const $ = id => document.getElementById(id);

// ---- Log ------------------------------------------------------------------
const MAX_LOG_LINES = 500;
const pad2 = n => String(n).padStart(2, '0');
function tsNow() { const d = new Date(); return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`; }
function log(msg, cls) {
  const el = $('consoleOut');
  if (!el) return;
  String(msg).split('\n').forEach(part => {
    if (part === '' ) return;
    const line = document.createElement('div');
    const t = document.createElement('span'); t.className = 'ts'; t.textContent = `[${tsNow()}] `;
    const b = document.createElement('span'); if (cls) b.className = cls; b.textContent = part;
    line.appendChild(t); line.appendChild(b); el.appendChild(line);
  });
  while (el.childElementCount > MAX_LOG_LINES) el.removeChild(el.firstElementChild);
  el.scrollTop = el.scrollHeight;
  if (cls === 'err') openDrawer('console');
}

// esptool-js writes to this terminal object.
const espTerminal = {
  clean() {},
  writeLine(data) { log(String(data)); },
  write(data) { /* partial writes: collapse into the log without spamming */ if (String(data).trim()) log(String(data).trim()); },
};

// ---- Drawers --------------------------------------------------------------
const DRAWERS = { console: 'btnConsole', filesDrawer: 'btnFiles' };
let drawerOpener = null;
function openDrawer(id, moveFocus) {
  const wasOpen = Object.keys(DRAWERS).some(d => $(d).classList.contains('is-open'));
  Object.keys(DRAWERS).forEach(d => {
    const el = $(d), btn = $(DRAWERS[d]), on = d === id;
    el.classList.toggle('is-open', on); el.inert = !on;
    if (btn) btn.setAttribute('aria-expanded', String(on));
  });
  if (id === 'console') { const out = $('consoleOut'); out.scrollTop = out.scrollHeight; }
  if (id) {
    if (!moveFocus) return;
    if (!wasOpen) drawerOpener = document.activeElement;
    const close = $(id).querySelector('.x'); if (close) close.focus({ preventScroll: true });
  } else if (wasOpen && drawerOpener && document.contains(drawerOpener)) {
    drawerOpener.focus({ preventScroll: true }); drawerOpener = null;
  }
}
function closeDrawers() { openDrawer(null); }
Object.keys(DRAWERS).forEach(d => $(DRAWERS[d]).addEventListener('click', () =>
  $(d).classList.contains('is-open') ? closeDrawers() : openDrawer(d, true)));
document.querySelectorAll('[data-close]').forEach(b => b.addEventListener('click', closeDrawers));
document.addEventListener('keydown', e => { if (e.key === 'Escape') closeDrawers(); });

// ---- Wizard ---------------------------------------------------------------
const LAST_STEP = 4;
let step = 0;
const views = [...document.querySelectorAll('.view')];
const rail  = [...document.querySelectorAll('#rail li')];

function syncRail(n) {
  rail.forEach(li => {
    const i = +li.dataset.step;
    const current = i === n && n < LAST_STEP;
    li.classList.toggle('on', current);
    li.classList.toggle('done', i < n);
    let fill; if (i < n) fill = 100; else if (current) fill = Math.round((n + 1) / rail.length * 100); else fill = 0;
    li.style.setProperty('--fill', fill);
    if (current) li.setAttribute('aria-current', 'step'); else li.removeAttribute('aria-current');
    const btn = li.querySelector('.railbtn'); if (btn) btn.disabled = i >= n;
  });
}
function goTo(n) {
  if (n === step) return;
  const from = views.find(v => +v.dataset.step === step);
  const to = views.find(v => +v.dataset.step === n);
  if (!to) return;
  if (from) { from.classList.remove('is-on'); from.classList.add('is-out'); setTimeout(() => from.classList.remove('is-out'), 320); }
  to.classList.add('is-on');
  step = n; document.body.dataset.step = String(n); syncRail(n);
  const h = to.querySelector('h1'); if (h) { h.setAttribute('tabindex', '-1'); h.focus({ preventScroll: true }); }
}
document.querySelectorAll('[data-go]').forEach(b => b.addEventListener('click', () => goTo(+b.dataset.go)));
rail.forEach(li => { const btn = li.querySelector('.railbtn'); if (btn) btn.addEventListener('click', () => { if (li.classList.contains('done')) goTo(+li.dataset.step); }); });

// ---- Page-map progress ----------------------------------------------------
const MAP_MAX_CELLS = 96;
let mapCells = [];
function buildPageMap(units) {
  const wrap = $('pagemap'); if (!wrap) return;
  const n = Math.max(1, Math.min(units, MAP_MAX_CELLS));
  wrap.textContent = ''; mapCells = [];
  for (let i = 0; i < n; i++) { const c = document.createElement('i'); c.style.setProperty('--i', i); wrap.appendChild(c); mapCells.push(c); }
  wrap.classList.remove('is-done', 'is-error', 'is-erasing');
  wrap.setAttribute('aria-valuenow', '0');
}
function paintProgress(done, total) {
  const wrap = $('pagemap');
  const upto = Math.round(mapCells.length * done / (total || 1));
  for (let i = 0; i < mapCells.length; i++) { mapCells[i].classList.toggle('on', i < upto); mapCells[i].classList.toggle('next', i === upto); }
  const pct = Math.round(100 * done / (total || 1));
  if (wrap) wrap.setAttribute('aria-valuenow', String(pct));
  $('rdDone').textContent = Math.round(done / 1024);
  $('rdTotal').textContent = Math.round(total / 1024);
}
function mapState(state) {
  const wrap = $('pagemap'); if (!wrap) return;
  wrap.classList.toggle('is-erasing', state === 'erasing');
  wrap.classList.toggle('is-done', state === 'done');
  wrap.classList.toggle('is-error', state === 'error');
  if (state) mapCells.forEach(c => c.classList.remove('next'));
  if (state === 'error') { const next = mapCells.find(c => !c.classList.contains('on')); if (next) next.classList.add('fail'); }
}
function setPhase(text) { const e = $('phase'), f = $('footStat'); if (e) e.textContent = text; if (f) f.textContent = text; }
function setStatus(text, state) {
  const l = $('statusText'), w = $('status');
  if (l) l.textContent = text; if (w) w.dataset.state = state || 'idle';
  document.body.dataset.state = state || 'idle';
}
function showFlashError(msg) { const el = $('flashErr'); if (!el) return; el.hidden = !msg; el.textContent = msg || ''; }

// ---- Image loading --------------------------------------------------------
const APP_OFFSET = 0x10000;   // factory/app partition in the default_8MB scheme

// esptool-js v0.5.x wants each part's data as a binary string (one char/byte).
function bufToBinaryString(buf) {
  const bytes = new Uint8Array(buf);
  let out = '';
  const CHUNK = 0x8000;
  for (let i = 0; i < bytes.length; i += CHUNK) {
    out += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
  }
  return out;
}

async function fetchPart(path) {
  const r = await fetch(path, { cache: 'no-store' });
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
  return r.arrayBuffer();
}

// Default target: the full image set from the manifest.
async function loadManifestImages() {
  const r = await fetch('./firmware/manifest.json', { cache: 'no-store' });
  if (!r.ok) throw new Error(`manifest.json: HTTP ${r.status}`);
  const man = await r.json();
  const parts = [];
  for (const p of man.parts) {
    const buf = await fetchPart('./firmware/' + p.path);
    parts.push({ data: bufToBinaryString(buf), address: p.offset, bytes: buf.byteLength });
    log(`image: ${p.path} ${buf.byteLength} B @ 0x${p.offset.toString(16)}`);
  }
  $('pickName').textContent = `full image · v${man.version || '?'}`;
  return parts;
}

// A chosen .espkg: pull the fw (app) payload out of the container and flash it
// at the app offset. Container: 'ESPKG\x01', u32 manifest length, manifest JSON,
// then every sd file's bytes in order, then the fw bytes.
function extractEspkgApp(buf) {
  const b = new Uint8Array(buf);
  const magic = 'ESPKG\x01';
  for (let i = 0; i < 6; i++) if (b[i] !== magic.charCodeAt(i)) throw new Error('not a valid .espkg (bad magic)');
  const manLen = b[6] | (b[7] << 8) | (b[8] << 16) | (b[9] << 24);
  if (manLen === 0 || manLen > 8192 || 10 + manLen > b.length) throw new Error('.espkg manifest length invalid');
  const man = JSON.parse(new TextDecoder().decode(b.subarray(10, 10 + manLen)));
  const fwSize = (man.fw && man.fw.size) | 0;
  if (!fwSize) throw new Error('.espkg has no firmware payload (web-only package)');
  let sdTotal = 0; for (const f of (man.sd || [])) sdTotal += (f.size | 0);
  const fwStart = 10 + manLen + sdTotal;
  if (fwStart + fwSize > b.length) throw new Error('.espkg truncated');
  return { data: bufToBinaryString(b.buffer.slice(fwStart, fwStart + fwSize)), address: APP_OFFSET, bytes: fwSize, version: man.version };
}

// ---- WebSerial + esptool --------------------------------------------------
let transport = null;
let esploader = null;
let flashing = false;
let stopping = false;
let images = null;         // pending fileArray

$('btnConnect').onclick = async () => {
  if (!('serial' in navigator)) { log('no WebSerial here. use Chrome or Edge.', 'err'); setStatus('No WebSerial', 'error'); return; }
  const btn = $('btnConnect');
  btn.classList.add('loading');
  try {
    const port = await navigator.serial.requestPort({});
    transport = new Transport(port, true);
    esploader = new ESPLoader({ transport, baudrate: 921600, romBaudrate: 115200, terminal: espTerminal, debugLogging: false });
    setStatus('Connecting', 'live');
    const chip = await esploader.main();            // reset to bootloader, detect, load stub
    log(`connect: ${chip}`, 'ok');
    $('pickChip').textContent = chip;
    setStatus(chip, 'ok');
    if (!images) { try { images = await loadManifestImages(); } catch (e) { log('images: ' + e.message, 'err'); } }
    $('btnFlash').disabled = !images;
    goTo(3);
  } catch (e) {
    if (e && (e.name === 'NotFoundError' || /No port selected/i.test(e.message || ''))) log('connect: no device picked');
    else { log('connect: ' + (e && e.message ? e.message : e), 'err'); setStatus('Connect failed', 'error'); }
    try { if (transport) await transport.disconnect(); } catch (_) {}
    transport = null; esploader = null;
  } finally {
    btn.classList.remove('loading');
  }
};

$('fileBin').onchange = async ev => {
  const f = ev.target.files[0]; if (!f) return;
  try {
    const buf = await f.arrayBuffer();
    if (f.name.toLowerCase().endsWith('.espkg')) {
      const app = extractEspkgApp(buf);
      images = [app];
      $('pickName').textContent = `${f.name} · app @ 0x${APP_OFFSET.toString(16)}`;
      log(`firmware: ${f.name}, app ${app.bytes} B (from .espkg)`, 'ok');
    } else {
      // A raw .bin is treated as an app image at the app offset (the device
      // keeps its existing bootloader). A full merged image would be flashed
      // at 0x0 — rebuild from the manifest for that.
      images = [{ data: bufToBinaryString(buf), address: APP_OFFSET, bytes: buf.byteLength }];
      $('pickName').textContent = `${f.name} · app @ 0x${APP_OFFSET.toString(16)}`;
      log(`firmware: ${f.name}, ${buf.byteLength} B @ 0x${APP_OFFSET.toString(16)}`, 'ok');
    }
    $('btnFlash').disabled = !esploader;
  } catch (e) { log(`${f.name}: ${e.message}`, 'err'); ev.target.value = ''; }
};

async function runFlash() {
  if (!esploader) { log('flash: connect first', 'err'); return; }
  if (!images) { try { images = await loadManifestImages(); } catch (e) { log('images: ' + e.message, 'err'); return; } }

  const btn = $('btnFlash'), track = $('progress');
  btn.disabled = true; flashing = true;
  mapState(null); showFlashError(null);
  setStatus('Flashing', 'live');

  const total = images.reduce((n, p) => n + p.bytes, 0);
  const before = []; let acc = 0; for (const p of images) { before.push(acc); acc += p.bytes; }
  buildPageMap(Math.max(24, Math.round(total / 4096)));
  $('rdParts').textContent = `0 / ${images.length} parts`;
  paintProgress(0, total);

  try {
    setPhase('writing');
    await esploader.writeFlash({
      fileArray: images.map(p => ({ data: p.data, address: p.address })),
      flashSize: 'keep',
      flashMode: 'keep',
      flashFreq: 'keep',
      eraseAll: false,
      compress: true,
      reportProgress: (fileIndex, written, fileTotal) => {
        const done = (before[fileIndex] || 0) + written;
        paintProgress(done, total);
        $('rdParts').textContent = `${written >= fileTotal ? fileIndex + 1 : fileIndex} / ${images.length} parts`;
      },
    });

    if (stopping) throw new Error('stopped');
    setPhase('resetting');
    log('flash: done, resetting', 'ok');
    try { await esploader.after('hard_reset'); } catch (_) {}
    paintProgress(total, total);
    setPhase('done'); mapState('done');
    setStatus('Flashed', 'ok');
    log('done. the device is rebooting into the new firmware.', 'ok');
    // "Flash multiple devices": drop this unit and loop back to Connect so the
    // next board can be picked, instead of ending on the done screen.
    if ($('flashMany') && $('flashMany').checked) {
      log('flash many: unplug this device, plug the next, then Connect.', 'ok');
      try { if (transport) await transport.disconnect(); } catch (_) {}
      transport = null; esploader = null;
      $('btnFlash').disabled = true;
      setStatus('Next device', 'ok');
      setTimeout(() => goTo(2), 900);
    } else {
      setTimeout(() => goTo(4), 700);
    }
  } catch (e) {
    const msg = (e && e.message) ? e.message : String(e);
    log('flash: ' + msg, 'err'); showFlashError(msg);
    setPhase('failed'); mapState('error');
    setStatus('Flash failed', 'error');
    btn.disabled = false;
  } finally {
    flashing = false;
  }
}

// ---- Stop / reset: clear the progress cells and drop the connection --------
function clearPageMap() {
  mapCells.forEach(c => c.classList.remove('on', 'next', 'fail'));
  const wrap = $('pagemap');
  if (wrap) { wrap.classList.remove('is-done', 'is-error', 'is-erasing'); wrap.setAttribute('aria-valuenow', '0'); }
  if ($('rdDone')) $('rdDone').textContent = '0';
  if ($('rdParts')) $('rdParts').textContent = `0 / ${images ? images.length : 0} parts`;
  setPhase('ready'); showFlashError(null);
}
async function stopFlash() {
  // Abort an in-flight write by dropping the transport (writeFlash then throws,
  // and its catch marks the error state), then wipe the progress boxes.
  if (flashing) {
    stopping = true;
    log('stop: aborting flash', 'err');
    try { if (transport) await transport.disconnect(); } catch (_) {}
    transport = null; esploader = null;
  }
  flashing = false;
  clearPageMap();
  setStatus(esploader ? 'Connected' : 'No device', esploader ? 'ok' : 'idle');
  $('btnFlash').disabled = !esploader;
  stopping = false;
}
const _btnStop = $('btnStop'); if (_btnStop) _btnStop.addEventListener('click', stopFlash);

// ---- Hold to flash (irreversible, so it takes a deliberate press) ----------
const HOLD_MS = 700;
(function wireHold() {
  const btn = $('btnFlash');
  let timer = null;
  function reset() { if (timer) { clearTimeout(timer); timer = null; } btn.style.setProperty('--hold-dur', '140ms'); btn.style.setProperty('--hold', '0'); }
  function begin(e) {
    if (btn.disabled || flashing || timer) return;
    if (e.cancelable) e.preventDefault();
    btn.style.setProperty('--hold-dur', HOLD_MS + 'ms'); btn.style.setProperty('--hold', '1');
    timer = setTimeout(() => { reset(); runFlash(); }, HOLD_MS);
  }
  btn.addEventListener('pointerdown', begin);
  btn.addEventListener('pointerup', reset);
  btn.addEventListener('pointerleave', reset);
  btn.addEventListener('pointercancel', reset);
  btn.addEventListener('keydown', e => { if ((e.key === 'Enter' || e.key === ' ') && !e.repeat) begin(e); });
  btn.addEventListener('keyup', e => { if (e.key === 'Enter' || e.key === ' ') reset(); });
  btn.addEventListener('blur', reset);
})();

// ---- Boot -----------------------------------------------------------------
document.body.dataset.step = '0';
syncRail(0);
setStatus('No device', 'idle');
setPhase('ready');
if (!('serial' in navigator)) {
  log('no WebSerial in this browser. use Chrome or Edge, or flash with arduino-cli / esptool.', 'err');
  $('btnConnect').disabled = true;
  setStatus('No WebSerial', 'error');
}
window.addEventListener('beforeunload', ev => { if (flashing) { ev.preventDefault(); ev.returnValue = ''; } });
