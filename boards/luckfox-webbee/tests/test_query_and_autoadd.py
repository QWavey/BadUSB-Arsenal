"""Query order, honeypot verdict format, and the auto-add with duplicate REDIRECT_FROM lines."""
import http.client
import importlib.util
import json
import os
import sys
import threading
import types
from pathlib import Path

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"

for _name in ("fcntl", "pty", "termios"):
    try:
        __import__(_name)
    except ImportError:
        sys.modules[_name] = types.ModuleType(_name)


def _load(root):
    os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location("webui_query_under_test", ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _get(web, path):
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=10)
        conn.request("GET", path)
        return conn.getresponse().read().decode("utf-8")
    finally:
        srv.shutdown()
        srv.server_close()


def test_query_keeps_the_newest_matches_in_order(tmp_path):
    web = _load(tmp_path)
    ver = web.VER_DIR
    ver.mkdir(parents=True, exist_ok=True)
    old = [json.dumps({"rule": "HTTP", "n": i}, separators=(",", ":")) for i in range(600)]
    new = [json.dumps({"rule": "HTTP", "n": i}, separators=(",", ":")) for i in range(600, 700)]
    (ver / "rec_000001.jsonl").write_text("\n".join(old) + "\n")
    (ver / "rec_000002.jsonl").write_text("\n".join(new) + "\n")
    rows = [json.loads(ln) for ln in _get(web, "/api/query?rule=HTTP").splitlines()]
    assert len(rows) == 500
    assert rows[0]["n"] == 200 and rows[-1]["n"] == 699    # the newest 500, oldest first


def test_honeypot_verdict_is_found_by_its_rule(tmp_path):
    web = _load(tmp_path)
    web._honeypot_emit_verdict({"rule": "HONEYPOT_CRED", "host": "example.com", "ts": "00:00:00"})
    rows = [json.loads(ln) for ln in _get(web, "/api/query?rule=HONEYPOT_CRED").splitlines()]
    assert len(rows) == 1 and rows[0]["host"] == "example.com"


def test_auto_add_leaves_one_redirect_from_line_and_converges(tmp_path, monkeypatch):
    web = _load(tmp_path)
    (tmp_path / "logs").mkdir()                   # the device has this directory
    # The last REDIRECT_FROM line is the one load_cfg reads, so it is the value that counts.
    (tmp_path / "config.txt").write_text(
        "REDIRECT=TRUE\nREDIRECT_FROM=bing.com\nREDIRECT_FROM=example.com\n", encoding="utf-8")
    monkeypatch.setattr(web, "read_seen", lambda limit=500: [{"host": "www.example.com"}])
    monkeypatch.setattr(web.subprocess, "run", lambda *a, **k: None)
    assert web._auto_add_once() == ["www.example.com"]
    text = (tmp_path / "config.txt").read_text(encoding="utf-8")
    assert text.count("REDIRECT_FROM=") == 1
    assert web.load_cfg()["REDIRECT_FROM"] == "example.com,www.example.com"
    # A second pass finds nothing new, so the config is not rewritten (no reload every 10 s).
    assert web._auto_add_once() == []
