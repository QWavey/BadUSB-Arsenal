"""Bundle import checks the whole tarball before it writes anything.

A bundle with a member over 2 MB, more than 64 MB uncompressed, or more than 2000 files
is refused with 413 and leaves the device untouched. Small bundles still import.
"""
import http.client
import importlib.util
import io
import json
import os
import sys
import tarfile
import threading
import types
from pathlib import Path

import pytest

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"

# webui imports these Linux-only modules at top level; nothing uses them at import.
# Stub them only where the real modules are missing (Windows). Replacing a real
# `pty` for the whole session breaks the pty-shell tests collected later.
for _name in ("fcntl", "pty", "termios"):
    try:
        importlib.import_module(_name)
    except ImportError:
        sys.modules[_name] = types.ModuleType(_name)


def _load(root):
    os.environ["GL_ROOT"] = str(root)
    spec = importlib.util.spec_from_file_location("webui_bundle_under_test", ONBOARD / "webui.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _bundle(members):
    """A gzip tarball holding the given (name, bytes) regular files."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as t:
        for name, data in members:
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def _import(web, body):
    srv = web.Server(("127.0.0.1", 0), web.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=30)
        conn.request("POST", "/api/bundle/import", body=body,
                     headers={"Content-Type": "application/gzip"})
        resp = conn.getresponse()
        return resp.status, json.loads(resp.read() or b"{}")
    finally:
        srv.shutdown()
        srv.server_close()


@pytest.fixture
def web(tmp_path, monkeypatch):
    mod = _load(tmp_path)
    monkeypatch.setattr(mod.Handler, "_require_admin", lambda self: True)
    return mod


def test_small_bundle_imports(web, tmp_path):
    status, out = _import(web, _bundle([("config.txt", b"WEBUI_USER=root\n")]))
    assert status == 200 and out["ok"] is True
    assert (tmp_path / "config.txt").read_bytes() == b"WEBUI_USER=root\n"


def test_empty_member_name_is_skipped_not_written_as_root(web, tmp_path):
    # Bug-hunt finding: an empty tar member name passed both traversal checks (it
    # doesn't start with "/", and "" not in "".split("/") == [""] is false), and
    # ROOT / "" resolves to ROOT itself. A regular-file member with that name would
    # have its contents written through ROOT's own path via tmp.replace(dest) --
    # replacing the whole ghostlink directory with that one file. GL_ROOT (tmp_path)
    # must still exist as a directory afterwards, and the legitimate sibling member
    # must still have imported normally.
    before = tmp_path.is_dir()
    assert before
    status, out = _import(web, _bundle([("", b"anything"), ("config.txt", b"ok\n")]))
    assert status == 200 and out["ok"] is True
    assert tmp_path.is_dir(), "ROOT was replaced by the empty-named member's contents"
    assert (tmp_path / "config.txt").read_bytes() == b"ok\n"
    assert "" not in out["imported"]


def test_member_over_2mb_refuses_the_whole_bundle(web, tmp_path):
    body = _bundle([("notes.txt", b"hi"), ("big.bin", bytes(3 * 1024 * 1024))])
    status, out = _import(web, body)
    assert status == 413 and out["ok"] is False
    assert not (tmp_path / "notes.txt").exists()     # nothing written, not even the small member
    assert not (tmp_path / "big.bin").exists()


def test_expansion_over_64mb_is_refused(web, tmp_path):
    chunk = bytes(2 * 1024 * 1024)                     # each member is under the 2 MB per-file cap
    body = _bundle([("part%02d.bin" % i, chunk) for i in range(33)])   # 66 MB uncompressed, tiny gzip
    assert len(body) < 16 * 1024 * 1024               # passes the compressed-size check
    status, out = _import(web, body)
    assert status == 413 and out["ok"] is False
    assert not (tmp_path / "part00.bin").exists()


def test_too_many_files_is_refused(web, tmp_path):
    body = _bundle([("f%04d.txt" % i, b"x") for i in range(2001)])
    status, out = _import(web, body)
    assert status == 413 and out["ok"] is False
    assert not (tmp_path / "f0000.txt").exists()
