"""Ducky runner limits: a runaway loop stops, unclosed or malformed blocks do not run,
the delay is capped, and a payload chain has a depth limit."""
import importlib.util
import os
from pathlib import Path

import pytest

DUCKY = Path(__file__).resolve().parents[1] / "onboard" / "ducky.py"


def _load(tmp_path, monkeypatch):
    monkeypatch.setenv("GL_ROOT", str(tmp_path))
    spec = importlib.util.spec_from_file_location("ducky_limits_under_test", DUCKY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _interp(mod, tmp_path, monkeypatch):
    script = tmp_path / "p.ducky"
    script.write_text("REM\n")
    it = mod.Interp(script)
    ran = []
    monkeypatch.setattr(it, "_run_line", lambda line: ran.append(line))
    return it, ran


def test_nested_loops_hit_the_step_limit(tmp_path, monkeypatch):
    mod = _load(tmp_path, monkeypatch)
    it, _ = _interp(mod, tmp_path, monkeypatch)
    body = ["WHILE (1)", "WHILE (1)", "REM", "END_WHILE", "END_WHILE"]
    with pytest.raises(mod._Budget):
        it.exec_body(body)


def test_an_unclosed_while_does_not_run_its_body(tmp_path, monkeypatch):
    mod = _load(tmp_path, monkeypatch)
    it, ran = _interp(mod, tmp_path, monkeypatch)
    it.exec_body(["WHILE (1)", "STRING never"])
    assert ran == []


def test_an_unclosed_if_does_not_run_the_rest(tmp_path, monkeypatch):
    mod = _load(tmp_path, monkeypatch)
    it, ran = _interp(mod, tmp_path, monkeypatch)
    it.exec_body(["IF (0)", "STRING never", "STRING never too"])
    assert ran == []


def test_a_malformed_if_header_is_not_run_as_a_plain_line(tmp_path, monkeypatch):
    mod = _load(tmp_path, monkeypatch)
    it, ran = _interp(mod, tmp_path, monkeypatch)
    it.exec_body(["IF $A == 1", "STRING unconditional", "END_IF"])
    assert ran == []


def test_a_well_formed_if_still_runs(tmp_path, monkeypatch):
    mod = _load(tmp_path, monkeypatch)
    it, ran = _interp(mod, tmp_path, monkeypatch)
    it.exec_body(["IF (1)", "STRING yes", "END_IF"])
    assert ran == ["STRING yes"]


def test_delay_is_capped_at_an_hour(tmp_path, monkeypatch):
    mod = _load(tmp_path, monkeypatch)
    assert mod._ms("99999999999") == 3_600_000
    assert mod._ms("250") == 250
    assert mod._ms("nonsense", 7) == 7


def test_a_payload_chain_stops_at_the_depth_limit(tmp_path, monkeypatch):
    mod = _load(tmp_path, monkeypatch)
    monkeypatch.setenv("GL_DUCKY_DEPTH", str(mod.MAX_DEPTH + 1))
    script = tmp_path / "p.ducky"
    script.write_text("STRING x\n")
    assert mod.Interp(script).run() == 1
    assert mod._child_env()["GL_DUCKY_DEPTH"] == str(mod.MAX_DEPTH + 2)


def test_run_payload_rejects_traversal_and_bad_names(tmp_path, monkeypatch):
    # Bug-hunt finding: RUN_PAYLOAD had no name validation at all, unlike its sibling
    # GL_RUN (gl_run_script()), which has used this exact whitelist from the start. Path
    # joining does not strip "..", so "RUN_PAYLOAD ../../../etc/passwd" resolved straight
    # outside payloads/. Confirm the traversal (and a few other shapes GL_RUN already
    # rejects) never reaches subprocess.Popen, while a normal name still does.
    import subprocess
    mod = _load(tmp_path, monkeypatch)
    script = tmp_path / "p.ducky"
    script.write_text("REM\n")
    it = mod.Interp(script)

    calls = []
    monkeypatch.setattr(subprocess, "Popen", lambda *a, **k: calls.append((a, k)))

    (tmp_path / "payloads").mkdir()
    (tmp_path / "payloads" / "x.ducky").write_text("REM\n")
    # A file that genuinely exists outside payloads/, reachable only via traversal.
    (tmp_path / "secret.ducky").write_text("REM\n")

    for bad in ("../secret.ducky", "../../secret.ducky", "a/b.ducky", "", ".ducky",
                "noext", "has space.ducky"):
        it._run_line(f"RUN_PAYLOAD {bad}")
    assert calls == [], f"a rejected name reached Popen: {calls}"

    it._run_line("RUN_PAYLOAD x.ducky")
    assert len(calls) == 1, "a valid, in-directory name must still run"
