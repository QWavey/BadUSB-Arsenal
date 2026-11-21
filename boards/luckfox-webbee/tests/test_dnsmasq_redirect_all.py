"""Tests for the REDIRECT_ALL (captive-portal style) dnsmasq generation in
onboard/ghostlink.sh.

Strategy: the config generator is a POSIX-sh function (write_dnsmasq_conf). We
extract just that function and its helpers (getcfg, truthy, log) from the live
script, source them in a throwaway sh process, and run the generator against a
temp config.txt -- exactly the way the boot and reload paths call it. A fake
`ip` (empty output) is placed first on PATH so the per-URL redirect target is
deterministic (falls back to 172.32.0.93, as on a device whose usb0 has no addr
yet). This needs a POSIX shell, so the suite runs under WSL/Linux; on a host
without `sh` the module skips rather than errors.

Two things are proven:
  * Regression: with REDIRECT_ALL absent or its target empty, the generated
    config is byte-for-byte what the script produced before this feature.
  * REDIRECT_ALL=TRUE emits per-domain `server=` exemptions (allowlist + the
    target's own host) and a single catch-all `address=/#/172.32.0.93` that is
    the last resolution directive in the file.
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

# --- the exact config the script generated before REDIRECT_ALL existed -------
# Captured from the pre-feature generator with NET_MODE!=NAT and a fake `ip`.
PRISTINE_BASE = (
    "interface=usb0\n"
    "bind-interfaces\n"
    "no-resolv\n"
    "no-hosts\n"
    "dhcp-range=172.32.0.100,172.32.0.120,255.255.0.0,12h\n"
    "server=1.1.1.1\n"
    "server=9.9.9.9\n"
)
PRISTINE_TAIL = (
    "log-facility=/tmp/dnsmasq.log\n"
    "pid-file=/run/ghostlink-dnsmasq.pid\n"
    "dhcp-leasefile=/tmp/dnsmasq.leases\n"
)
PRISTINE_A = PRISTINE_BASE + PRISTINE_TAIL
PRISTINE_B = (
    PRISTINE_BASE
    + "address=/example.com/172.32.0.93\n"
    + "address=/www.example.com/172.32.0.93\n"
    + PRISTINE_TAIL
)

DEFAULT_EXEMPT = [
    "anthropic.com",
    "claude.ai",
    "claude.com",
    "claudeusercontent.com",
    "msftconnecttest.com",
    "msftncsi.com",
    "windowsupdate.com",
]

_HDR = re.compile(r"^(log|getcfg|truthy|write_dnsmasq_conf)\(\)")


def _extract_funcs(text):
    """Pull just the helper + generator functions out of ghostlink.sh.

    Mirrors the shell's own layout: each wanted function starts at column 0 as
    `name()`; one-liners end with `}` on the same line, multi-line ones end at a
    column-0 `}`. Robust to line-number shifts from future edits.
    """
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
WORK="$1"
CONF="$2"
mkdir -p "$WORK/bin"
printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/ip"
chmod +x "$WORK/bin/ip"
ROOT="$WORK/root"
mkdir -p "$ROOT/logs"
cp "$CONF" "$ROOT/config.txt"
PATH="$WORK/bin:$PATH"
export GL_ROOT="$ROOT"
. "$WORK/funcs.sh"
CFG="$ROOT/config.txt"
log() { :; }
IFACE=$(getcfg DNSMASQ_IFACE); IFACE=${IFACE:-usb0}
DR=$(getcfg DHCP_RANGE)
DNS=$(getcfg DNS_UPSTREAM)
RED=$(getcfg REDIRECT)
RF=$(getcfg REDIRECT_FROM)
RT=$(getcfg REDIRECT_TO)
OUT=$(write_dnsmasq_conf "$IFACE" "$DR" "$DNS" "${RED:-FALSE}" "${RF:-}" "${RT:-172.32.0.93}")
cat "$OUT"
"""


def generate(config_text, tmp_path):
    """Run the live generator against config_text, return the config it writes."""
    work = tmp_path
    (work / "funcs.sh").write_text(_extract_funcs(SCRIPT.read_text()), encoding="utf-8")
    (work / "runner.sh").write_text(_RUNNER, encoding="utf-8")
    cfg = work / "in.txt"
    cfg.write_text(config_text, encoding="utf-8")
    res = subprocess.run(
        ["sh", str(work / "runner.sh"), str(work), str(cfg)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert res.returncode == 0, f"runner failed: {res.stderr}"
    assert res.stderr.strip() == "", f"unexpected stderr: {res.stderr}"
    return res.stdout


_COMMON = (
    "DNSMASQ=TRUE\n"
    "DNSMASQ_IFACE=usb0\n"
    "DHCP_RANGE=172.32.0.100,172.32.0.120,255.255.0.0,12h\n"
    "DNS_UPSTREAM=1.1.1.1,9.9.9.9\n"
    "NET_MODE=PASSIVE\n"
)


def _resolution_lines(cfg):
    return [ln for ln in cfg.splitlines() if ln.startswith(("server=", "address="))]


# --- regression: the FALSE / empty-target paths are untouched ----------------

def test_redirect_all_absent_is_byte_for_byte_identical(tmp_path):
    cfg = _COMMON + "REDIRECT=FALSE\nREDIRECT_FROM=\nREDIRECT_TO=172.32.0.93\n"
    assert generate(cfg, tmp_path) == PRISTINE_A


def test_per_url_redirect_still_identical_with_all_absent(tmp_path):
    cfg = (
        _COMMON
        + "REDIRECT=TRUE\nREDIRECT_FROM=example.com,www.example.com\n"
        + "REDIRECT_TO=172.32.0.93\n"
    )
    assert generate(cfg, tmp_path) == PRISTINE_B


def test_redirect_all_true_but_empty_target_falls_back_to_baseline(tmp_path):
    # REDIRECT_ALL on, but no target host -> must stay the old config exactly.
    cfg = (
        _COMMON
        + "REDIRECT=FALSE\nREDIRECT_FROM=\nREDIRECT_TO=172.32.0.93\n"
        + "REDIRECT_ALL=TRUE\nREDIRECT_ALL_TO=\n"
    )
    out = generate(cfg, tmp_path)
    assert out == PRISTINE_A
    assert "address=/#/" not in out
    assert "REDIRECT_ALL" not in out


# --- REDIRECT_ALL=TRUE behaviour --------------------------------------------

def test_redirect_all_true_default_exempt_and_catchall_last(tmp_path):
    cfg = (
        _COMMON
        + "REDIRECT=FALSE\nREDIRECT_FROM=\nREDIRECT_TO=172.32.0.93\n"
        + "REDIRECT_ALL=TRUE\n"
        + "REDIRECT_ALL_TO=https://evilcorp.example/login?x=1\n"
        + "REDIRECT_EXEMPT=\n"  # empty -> default allowlist
    )
    out = generate(cfg, tmp_path)

    # a comment names the mode that produced this config
    assert "# mode: REDIRECT_ALL" in out

    # every default-exempt domain resolves upstream via BOTH servers
    for d in DEFAULT_EXEMPT:
        assert f"server=/{d}/1.1.1.1" in out
        assert f"server=/{d}/9.9.9.9" in out

    # the target's own host (parsed out of the URL) is exempt too, as server=
    assert "server=/evilcorp.example/1.1.1.1" in out
    assert "server=/evilcorp.example/9.9.9.9" in out
    assert "address=/evilcorp.example/" not in out  # target is upstream, not spoofed

    # the catch-all exists and is the LAST resolution directive
    assert "address=/#/172.32.0.93" in out
    assert _resolution_lines(out)[-1] == "address=/#/172.32.0.93"

    # the untouched dhcp/server base lines are still present and unchanged
    assert out.startswith(PRISTINE_BASE)
    assert out.endswith(PRISTINE_TAIL)


def test_redirect_all_true_keeps_per_url_and_normalises_exempt(tmp_path):
    cfg = (
        _COMMON
        + "REDIRECT=TRUE\nREDIRECT_FROM=portal.test\nREDIRECT_TO=172.32.0.93\n"
        + "REDIRECT_ALL=TRUE\nREDIRECT_ALL_TO=landing.example.org\n"
        + "REDIRECT_EXEMPT=foo.com, bar.net ,*.baz.io\n"
    )
    out = generate(cfg, tmp_path)

    # the existing per-URL spoof survives alongside REDIRECT_ALL
    assert "address=/portal.test/172.32.0.93" in out

    # custom allowlist: whitespace trimmed, leading "*." stripped
    assert "server=/foo.com/1.1.1.1" in out
    assert "server=/bar.net/1.1.1.1" in out
    assert "server=/baz.io/1.1.1.1" in out
    assert "server=/*.baz.io/1.1.1.1" not in out

    # target host exempt, catch-all last
    assert "server=/landing.example.org/9.9.9.9" in out
    assert _resolution_lines(out)[-1] == "address=/#/172.32.0.93"

    # default allowlist is NOT used when REDIRECT_EXEMPT is provided
    assert "server=/anthropic.com/1.1.1.1" not in out


def test_redirect_all_true_filters_aaaa_before_the_catchall(tmp_path):
    # An IPv6-capable PC would otherwise get real AAAA answers and skip the
    # device, so the catch-all mode must answer AAAA for every name with "::"
    # (dnsmasq 2.85 on the real device has no filter-aaaa option -- confirmed,
    # it rejects that config outright -- so this uses the much older, always-
    # supported address=/domain/ip directive instead), and that line must come
    # before the catch-all (the catch-all stays last).
    cfg = (
        _COMMON
        + "REDIRECT=FALSE\nREDIRECT_FROM=\nREDIRECT_TO=172.32.0.93\n"
        + "REDIRECT_ALL=TRUE\nREDIRECT_ALL_TO=https://landing.example.org/\n"
    )
    out = generate(cfg, tmp_path)
    lines = out.splitlines()
    assert "address=/#/::" in lines
    assert lines.index("address=/#/::") < lines.index("address=/#/172.32.0.93")
    assert _resolution_lines(out)[-1] == "address=/#/172.32.0.93"


def test_redirect_all_off_has_no_aaaa_filter(tmp_path):
    cfg = _COMMON + "REDIRECT=FALSE\nREDIRECT_FROM=\nREDIRECT_TO=172.32.0.93\n"
    out = generate(cfg, tmp_path)
    assert "address=/#/::" not in out
