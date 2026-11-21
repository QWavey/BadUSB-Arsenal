"""The attack timeline rolls over at 2 MB and keeps only whole records."""
import importlib.util
import json
import os
import sys
import types
from pathlib import Path

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"

# webui imports these Linux-only modules at top level; stub them only where they do not exist.
for _name in ("fcntl", "pty", "termios"):
    try:
        __import__(_name)
    except ImportError:
        sys.modules[_name] = types.ModuleType(_name)


def _load(root):
    os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location("webui_timeline_roll", ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_rollover_keeps_whole_records_and_the_new_event(tmp_path):
    web = _load(tmp_path)
    tl = web.TIMELINE
    tl.parent.mkdir(parents=True, exist_ok=True)
    line = json.dumps({"kind": "old", "pad": "y" * 900}) + "\n"
    tl.write_text(line * 2400, encoding="utf-8")          # a little over 2 MB
    web._timeline("probe", n=1)
    data = tl.read_bytes()
    assert len(data) <= 512 * 1024 + 2048                  # the newest 512 KB, plus one record
    rows = [json.loads(ln) for ln in data.decode("utf-8").splitlines()]   # every line is JSON
    assert rows[-1]["kind"] == "probe"
    assert not tl.with_name(tl.name + ".tmp").exists()     # the temp file was renamed into place
