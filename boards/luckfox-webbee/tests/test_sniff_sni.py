"""TLS SNI reading in the sniffer: the hostname in a ClientHello, for the seen-hosts list.

The ClientHello bytes are built here by hand, so the test does not need a network.
"""
import importlib.util
from pathlib import Path

SNIFF = Path(__file__).resolve().parents[1] / "onboard" / "sniff.py"


def _load():
    spec = importlib.util.spec_from_file_location("sniff_sni_under_test", SNIFF)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def client_hello(host=None, extra_ext=b""):
    """A TLS 1.2-style ClientHello record. host=None leaves out the SNI extension."""
    exts = extra_ext
    if host is not None:
        name = host.encode()
        sni_data = (len(name) + 3).to_bytes(2, "big") + b"\x00" + len(name).to_bytes(2, "big") + name
        exts += b"\x00\x00" + len(sni_data).to_bytes(2, "big") + sni_data
    body = (b"\x03\x03" + bytes(32) + b"\x00"            # version, random, empty session id
            + b"\x00\x02\x13\x01"                        # one cipher suite
            + b"\x01\x00"                                # compression: null
            + len(exts).to_bytes(2, "big") + exts)
    hs = b"\x01" + len(body).to_bytes(3, "big") + body   # handshake: ClientHello
    return b"\x16\x03\x01" + len(hs).to_bytes(2, "big") + hs


SUPPORTED_VERSIONS = b"\x00\x2b\x00\x03\x02\x03\x04"


def test_sni_is_read_from_a_clienthello():
    sn = _load()
    assert sn._extract_hostname(client_hello("amazon.de"), 443, 50000) == "amazon.de"


def test_sni_is_read_when_another_extension_comes_first():
    sn = _load()
    assert sn._extract_hostname(client_hello("shop.example.com", SUPPORTED_VERSIONS), 443, 50000) == "shop.example.com"


def test_sni_is_lowercased():
    sn = _load()
    assert sn._extract_hostname(client_hello("WWW.Example.COM"), 443, 50000) == "www.example.com"


def test_no_sni_gives_empty_name():
    sn = _load()
    assert sn._extract_hostname(client_hello(None), 443, 50000) == ""


def test_truncated_hello_gives_empty_name_and_no_exception():
    sn = _load()
    full = client_hello("amazon.de")
    for cut in range(0, len(full)):
        assert sn._extract_hostname(full[:cut], 443, 50000) in ("", "amazon.de")


def test_non_tls_bytes_on_443_are_ignored():
    sn = _load()
    assert sn._extract_hostname(b"GET / HTTP/1.1\r\nHost: amazon.de\r\n\r\n", 443, 50000) == ""


def test_invalid_name_is_rejected():
    sn = _load()
    assert sn._extract_hostname(client_hello("bad_name!.example"), 443, 50000) == ""


def test_http_host_header_still_works_on_port_80():
    sn = _load()
    req = b"GET / HTTP/1.1\r\nHost: example.com:8080\r\n\r\n"
    assert sn._extract_hostname(req, 80, 50000) == "example.com"
