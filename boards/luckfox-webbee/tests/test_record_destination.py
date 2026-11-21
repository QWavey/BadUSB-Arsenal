"""Records can go to the flash or the SD card. The shell points records/ and verdicts/ at the
chosen place; the sniffer rotates when that link moves, and the switch is in the web UI."""
import importlib.util
import os
import sys
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
    spec = importlib.util.spec_from_file_location("webui_record_dest_under_test", ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_a_moved_record_target_makes_the_sniffer_rotate(tmp_path, monkeypatch):
    sn = _load_sniff("sniff_target_move")
    rec, ver = tmp_path / "records", tmp_path / "verdicts"
    rec.mkdir(); ver.mkdir()
    monkeypatch.setattr(sn, "REC_DIR", rec)
    monkeypatch.setattr(sn, "VER_DIR", ver)
    w = sn.Writer({"RECORD_PCAP": "TRUE", "VERDICT_STREAM": "FALSE"})
    rotated = []
    monkeypatch.setattr(w, "_rotate", lambda: rotated.append(True))
    w._check_record_target()                    # same target: nothing happens
    assert rotated == []
    w._rec_target = "/somewhere/else"           # the link now points elsewhere
    w._check_record_target()
    assert rotated == [True]
    w._check_record_target()                    # the new target is remembered
    assert rotated == [True]


def test_the_record_switch_is_in_the_web_ui_and_off_by_default(tmp_path):
    web = _load_webui(tmp_path)
    assert 'data-k="RECORD_SD"' in web.HTML
    assert "RECORD_SD=TRUE" in web.DEFAULT_CONFIG
