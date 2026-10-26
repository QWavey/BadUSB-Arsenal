/*
 * Compile-time embedded fallbacks for the WebUI. Users can override by
 * dropping index.html / app.js / style.css into /mnt/sd/WebUI, so these
 * strings only matter when the SD override is absent.
 *
 * Content is embedded as C strings. The build script regenerates this file
 * from src/web/pages/*.{html,js,css} when they change (see scripts/embed.sh).
 */

const char *EMBEDDED_INDEX_HTML =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<title>GhostLink</title><link rel=\"stylesheet\" href=\"/style.css\"></head>"
"<body><h1>GhostLink</h1>"
"<p>Minimal fallback UI. Copy <code>WebUI/</code> onto the SD for the full dashboard.</p>"
"<div id=\"s\">loading...</div>"
"<script src=\"/app.js\"></script></body></html>";

const char *EMBEDDED_APP_JS =
"fetch('/api/status').then(r=>r.text()).then(t=>{document.getElementById('s').textContent=t;});";

const char *EMBEDDED_STYLE_CSS =
"body{font-family:system-ui,sans-serif;background:#0a0d10;color:#e4e8ec;padding:24px;}"
"code{background:#141a20;padding:2px 6px;border-radius:3px;}";
