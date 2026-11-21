"""The device's small DNS client: query building and reply parsing (onboard/dnsq.py)."""
import importlib.util
import socket
from pathlib import Path

DNSQ = Path(__file__).resolve().parents[1] / "onboard" / "dnsq.py"


def _load():
    spec = importlib.util.spec_from_file_location("dnsq_under_test", DNSQ)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def reply(qid, name, ip, rcode=0):
    """A reply to a query for name with one A record (name is a pointer to the question)."""
    head = qid + bytes([0x81, rcode & 0x0F]) + b"\x00\x01\x00\x01\x00\x00\x00\x00"
    question = b"".join(bytes([len(p)]) + p.encode() for p in name.split(".")) + b"\x00\x00\x01\x00\x01"
    answer = b"\xc0\x0c" + b"\x00\x01\x00\x01" + b"\x00\x00\x00\x3c" + b"\x00\x04" + socket.inet_aton(ip)
    return head + question + answer


def test_query_is_an_a_question_for_the_name():
    dq = _load()
    q = dq.build_query("amazon.de", qid=b"\x12\x34")
    assert q[:2] == b"\x12\x34"
    assert q.endswith(b"\x06amazon\x02de\x00\x00\x01\x00\x01")


def test_reply_gives_the_a_record():
    dq = _load()
    qid = b"\xab\xcd"
    assert dq.parse_a(reply(qid, "example.com", "93.184.216.34"), qid) == "93.184.216.34"


def test_reply_with_an_error_code_raises():
    dq = _load()
    qid = b"\x01\x02"
    try:
        dq.parse_a(reply(qid, "example.com", "1.2.3.4", rcode=3), qid)
    except ValueError as e:
        assert "3" in str(e)
    else:
        raise AssertionError("an NXDOMAIN reply must raise")


def test_reply_with_another_id_raises():
    dq = _load()
    try:
        dq.parse_a(reply(b"\x00\x01", "example.com", "1.2.3.4"), b"\x00\x02")
    except ValueError:
        pass
    else:
        raise AssertionError("a reply with the wrong id must be rejected")


def test_bad_name_is_refused():
    dq = _load()
    for bad in ("", "a..b", "x" * 64 + ".com"):
        try:
            dq.build_query(bad)
        except ValueError:
            continue
        raise AssertionError("bad name accepted: %r" % bad)


def test_truncated_reply_raises_valueerror_not_indexerror():
    # Bug-hunt finding: parse_a/_skip_name indexed into `data` with no bounds check, so a
    # reply cut short anywhere after the header raised IndexError -- not the ValueError the
    # docstring promised. data is an unauthenticated UDP payload (no source verification,
    # trivially spoofable), so a truncated/malformed reply is expected adversarial input,
    # not a programming error; every truncation point must raise ValueError, consistently.
    dq = _load()
    qid = b"\xaa\xbb"
    full = reply(qid, "example.com", "93.184.216.34")
    for cut in range(12, len(full)):
        try:
            dq.parse_a(full[:cut], qid)
        except ValueError:
            continue
        except Exception as e:
            raise AssertionError(f"cut at {cut}: raised {type(e).__name__}, not ValueError")
        else:
            raise AssertionError(f"cut at {cut}: a truncated reply must raise, not succeed")
