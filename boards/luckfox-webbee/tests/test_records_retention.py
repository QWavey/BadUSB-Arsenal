"""Record retention, log trimming and the clear endpoint.

A device with no clock writes 1970 timestamps, so retention orders capture files by number.
Logs are trimmed in place, so a daemon that appends to an open handle keeps writing to the file.
Clear keeps the capture being written, so the recorder never writes to a file nobody can see.
"""
import http.client
import importlib.util
import json
import os
import sys
import threading
import time
import types
from pathlib import Path

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"

for _name in ("fcntl", "pty", "termios"):
    try:
        __import__(_name)
    except ImportError:
        sys.modules[_name] = types.ModuleType(_name)


def _load_sniff(name):
    spec = importlib.util.spec_from_file_location(name, ONBOARD / "sniff.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _load_webui(root):
    os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location("webui_records_under_test", ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_ring_keeps_the_newest_captures_when_the_clock_reads_1970(tmp_path, monkeypatch):
    sn = _load_sniff("sniff_ring_1970")
    rec, ver = tmp_path / "records", tmp_path / "verdicts"
    rec.mkdir(); ver.mkdir()
    monkeypatch.setattr(sn, "REC_DIR", rec)
    monkeypatch.setattr(sn, "VER_DIR", ver)
    now = time.time()
    for i in range(1, 7):
        p = rec / f"rec_{i:06d}.pcap"
        p.write_bytes(b"x")
        if i <= 4:
            os.utime(p, (now - 60, now - 60))       # captured earlier, with a real date
        else:
            os.utime(p, (0, 0))                     # captured this boot, clock at 1970
    sn.Writer({"RECORD_PCAP": "TRUE", "VERDICT_STREAM": "FALSE",
               "RECORD_RING_FILES": "4", "RECORD_MAX_AGE_H": "24", "RECORD_MIN_FREE_MB": "1"})
    kept = sorted(p.name for p in rec.glob("rec_*.pcap"))
    # The two oldest by number are gone. The writer opens its own new file as rec_000007.
    assert kept == ["rec_000003.pcap", "rec_000004.pcap", "rec_000005.pcap",
                    "rec_000006.pcap", "rec_000007.pcap"]


def test_trim_keeps_an_append_handle_writing_to_the_log(tmp_path):
    sn = _load_sniff("sniff_trim_inplace")
    log = tmp_path / "ghostlink.log"
    log.write_text("".join(f"line {i:06d} " + "x" * 80 + "\n" for i in range(12000)))   # about 1.1 MB
    handle = open(log, "a")                       # what a daemon's >> redirection holds open
    sn.trim_log(log, keep_kb=64)
    assert log.stat().st_size <= 64 * 1024 + 200
    handle.write("after trim\n")
    handle.flush()
    handle.close()
    assert log.read_text().endswith("after trim\n")


def test_clear_keeps_the_capture_being_written(tmp_path, monkeypatch):
    web = _load_webui(tmp_path)
    monkeypatch.setattr(web.Handler, "_require_admin", lambda self: True)
    rec = web.REC_DIR
    ver = web.VER_DIR
    rec.mkdir(parents=True, exist_ok=True); ver.mkdir(parents=True, exist_ok=True)
    for i in (1, 2, 3):
        (rec / f"rec_{i:06d}.pcap").write_bytes(b"p")
        (ver / f"rec_{i:06d}.jsonl").write_text("{}\n")
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
        conn.request("POST", "/api/clear", body=b"")
        out = json.loads(conn.getresponse().read())
    finally:
        srv.shutdown()
        srv.server_close()
    assert out["ok"] is True and out["kept"] == 2 and out["removed"] == 4
    assert sorted(p.name for p in rec.iterdir()) == ["rec_000003.pcap"]
    assert sorted(p.name for p in ver.iterdir()) == ["rec_000003.jsonl"]
