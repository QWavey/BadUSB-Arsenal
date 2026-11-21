"""Tests for detect_and_bind_lan_ip's guard conditions in onboard/ghostlink.sh.

This feature was added and manually verified on a real device today (NET_MODE=NAT,
usb0 link-bounce self-heal for a PC holding a stale lease), but had no automated
coverage at the shell level -- only its Python half (lan_detect.py) has tests. The
guard conditions (no carrier -> skip, NAT mode -> skip, detector missing -> skip) are
exactly what keeps this background feature from doing anything in the wrong device
state, so they are the highest-value, cheapest-to-test part: pure shell logic, no
real `ip link`/python3 invocation needed to prove they gate correctly.

Strategy: same as test_dnsmasq_redirect_all.py -- extract the real functions from the
live script, source them in a throwaway sh process with a fake `ip`/`timeout`/`python3`
on PATH that just records whether they were invoked, and check the recording.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ONBOARD = Path(__file__).resolve().parents[1] / "onboard"
SCRIPT = ONBOARD / "ghostlink.sh"

pytestmark = pytest.mark.skipif(
    shutil.which("sh") is None, reason="needs a POSIX shell (run under WSL/Linux)"
)

_HDR = re.compile(r"^(log|getcfg|truthy|lan_state_get|lan_state_ip|eth0_has_ip|detect_and_bind_lan_ip)\(\)")


def _extract_funcs(text):
    """Same column-0-brace extraction as test_dnsmasq_redirect_all.py's _extract_funcs."""
    lines = text.splitlines()
    out, i = [], 0
    while i < len(lines):
        line = lines[i]
        if _HDR.match(line):
            out.append(line)
            if line.rstrip().endswith("}"):
                i += 1
                continue
            i += 1
            while i < len(lines):
                out.append(lines[i])
                if lines[i].startswith("}"):
                    i += 1
                    break
                i += 1
            continue
        i += 1
    return "\n".join(out) + "\n"


_RUNNER = r"""#!/bin/sh
set -u
WORK="$1"; CARRIER="$2"; NET_MODE_VAL="$3"; HAVE_DETECTOR="$4"
mkdir -p "$WORK/bin" "$WORK/sys/class/net/eth0" "$WORK/root"
printf '%s' "$CARRIER" > "$WORK/sys/class/net/eth0/carrier"
[ "$HAVE_DETECTOR" = "yes" ] && : > "$WORK/root/lan_detect.py"
# Fake `ip`, `timeout`, `python3`, `sh`: each just appends its invocation to a log so
# the test can tell whether the background detector subshell actually started.
for cmd in ip timeout python3; do
    cat > "$WORK/bin/$cmd" <<EOF2
#!/bin/sh
echo "\$0 \$*" >> "$WORK/calls.log"
exit 0
EOF2
    chmod +x "$WORK/bin/$cmd"
done
PATH="$WORK/bin:$PATH"
export GL_ROOT="$WORK/root"
export GL_ETH0_CARRIER_PATH="$WORK/sys/class/net/eth0/carrier"
ROOT="$WORK/root"
LOG="$WORK/root/logs/ghostlink.log"
mkdir -p "$WORK/root/logs"
. "$WORK/funcs.sh"
CFG="$ROOT/config.txt"
printf 'NET_MODE=%s\n' "$NET_MODE_VAL" > "$CFG"
NET_MODE="$NET_MODE_VAL"
# detect_and_bind_lan_ip backgrounds its real work in "(...) &", which inherits this
# script's stdout/stderr by default. Python's subprocess.run reads those as pipes until
# EOF -- but the orphaned background subshell (its multi-minute retry loop) keeps that
# pipe's write end open long after this script itself exits, so the read never reaches
# EOF and subprocess.run hangs until its own timeout. Redirecting this script's own
# stdout/stderr to a real file BEFORE calling the function means the background child
# inherits a file descriptor, not a pipe -- writes to it never block on a reader, and
# this script exits immediately once its own (synchronous) part of the call returns.
exec >"$WORK/out.log" 2>&1
detect_and_bind_lan_ip
sleep 0.2  # let the backgrounded subshell (if any) make its first call before we check
# A correctly-skipped guard means calls.log was never created -- that's success, not a
# script failure, so cat's nonzero exit for "file not found" must not become this
# script's own exit status (it's the last command here).
cat "$WORK/calls.log" >"$WORK/calls.out" 2>/dev/null || : > "$WORK/calls.out"
exit 0
"""


def _run(tmp_path, net_mode, have_detector):
    work = tmp_path
    (work / "funcs.sh").write_text(_extract_funcs(SCRIPT.read_text()), encoding="utf-8")
    (work / "runner.sh").write_text(_RUNNER, encoding="utf-8")
    res = subprocess.run(
        ["sh", str(work / "runner.sh"), str(work), "1", net_mode, "yes" if have_detector else "no"],
        capture_output=True, text=True, timeout=15,
    )
    # stdout/stderr are redirected to out.log inside the runner (see its comment) so the
    # orphaned background subshell can't hold subprocess.run's pipes open; read that
    # instead of res.stdout for diagnostics, but calls.out is the actual assertion data.
    out_log = (work / "out.log")
    diag = out_log.read_text(encoding="utf-8", errors="replace") if out_log.exists() else ""
    assert res.returncode == 0, f"runner failed: {res.stderr}\n--- out.log ---\n{diag}"
    calls_out = work / "calls.out"
    return calls_out.read_text(encoding="utf-8", errors="replace") if calls_out.exists() else ""


def test_skipped_in_nat_mode(tmp_path):
    # NAT mode already manages eth0/the PC's addressing itself (natd.py); the comment
    # in detect_and_bind_lan_ip says this is deliberate, not an oversight.
    out = _run(tmp_path, "NAT", have_detector=True)
    assert out.strip() == "", f"detector ran in NAT mode, should have been skipped: {out!r}"


def test_skipped_without_the_detector_script(tmp_path):
    out = _run(tmp_path, "PASSIVE", have_detector=False)
    assert out.strip() == "", f"detector ran without lan_detect.py present: {out!r}"


def test_runs_in_passive_mode_with_detector_present(tmp_path):
    # Positive case: carrier=1 (set by the runner), NET_MODE not NAT, detector present
    # -> the backgrounded subshell should actually invoke timeout/python3.
    out = _run(tmp_path, "PASSIVE", have_detector=True)
    assert "python3" in out and "lan_detect.py" in out, \
        f"detector did not run when it should have: {out!r}"
