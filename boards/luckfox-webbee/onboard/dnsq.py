"""Small DNS client for the device's own lookups (A records only).

The device has no resolver of its own (/etc/resolv.conf is empty), so the redirect
check asks an upstream server directly. Pure Python with no OS-specific imports, so
it can be tested on any machine.
"""
import os
import socket


def build_query(name, qid=None):
    """A standard recursive query for the A record of name."""
    qid = qid if qid is not None else os.urandom(2)
    q = bytearray(qid) + b"\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00"
    for label in name.rstrip(".").split("."):
        raw = label.encode("ascii")
        if not raw or len(raw) > 63:
            raise ValueError("bad name: " + name)
        q.append(len(raw))
        q += raw
    q += b"\x00\x00\x01\x00\x01"          # end of name, type A, class IN
    return bytes(q)


def _skip_name(data, pos):
    """Position just after the domain name that starts at pos (handles pointers).
    Raises ValueError on a name that runs past the end of data -- a UDP reply has no
    source verification, so this must not assume a well-formed/complete packet."""
    while True:
        if pos >= len(data):
            raise ValueError("truncated name")
        length = data[pos]
        if length == 0:
            return pos + 1
        if length & 0xC0:                  # compression pointer: two bytes, ends the name
            if pos + 2 > len(data):
                raise ValueError("truncated name (pointer)")
            return pos + 2
        pos += 1 + length


def parse_a(data, qid):
    """First IPv4 address in the answer section. Raises ValueError on any error, malformed
    or truncated reply, or no answer -- never anything else (data is an untrusted UDP
    payload with no source verification, so a spoofed or corrupt reply is expected input,
    not an exceptional one)."""
    if len(data) < 12 or data[:2] != qid:
        raise ValueError("unexpected DNS reply")
    rcode = data[3] & 0x0F
    if rcode:
        raise ValueError("DNS error code %d" % rcode)
    count = int.from_bytes(data[6:8], "big")
    pos = _skip_name(data, 12) + 4         # question: name, type, class
    for _ in range(count):
        pos = _skip_name(data, pos)
        if pos + 10 > len(data):
            raise ValueError("truncated answer record")
        rtype = int.from_bytes(data[pos:pos + 2], "big")
        rdlen = int.from_bytes(data[pos + 8:pos + 10], "big")
        if pos + 10 + rdlen > len(data):
            raise ValueError("truncated answer record data")
        rdata = data[pos + 10:pos + 10 + rdlen]
        pos += 10 + rdlen
        if rtype == 1 and rdlen == 4:
            return socket.inet_ntoa(rdata)
    raise ValueError("no A record")


def resolve(name, server, timeout=3.0):
    """IPv4 address of name, asked of the DNS server at server (port 53)."""
    qid = os.urandom(2)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.settimeout(timeout)
        s.sendto(build_query(name, qid), (server, 53))
        data, _ = s.recvfrom(2048)
    finally:
        s.close()
    return parse_a(data, qid)
