#!/usr/bin/env python3
"""wrap_inject.py - add (or verify) the optional integrity header on INJECT.BIN.

The firmware accepts a raw compiled payload as before, OR a payload prefixed with
an 8-byte integrity header:

    "RDKY" (4 bytes) | word_count : u16 LE | crc16 : u16 LE | payload...

crc16 is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the payload body.
If the header is present but the CRC or length mismatches, the firmware refuses
to type the payload and blinks 6x red instead of injecting a corrupt stream.

Usage:
    python wrap_inject.py INJECT.BIN                 # -> INJECT.BIN.wrapped
    python wrap_inject.py INJECT.BIN -o OUT.BIN      # choose output
    python wrap_inject.py --verify SOME.BIN          # check an existing header
"""
import argparse
import sys


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


MAGIC = b"RDKY"


def wrap(body: bytes) -> bytes:
    word_count = len(body) // 2
    crc = crc16_ccitt(body)
    header = MAGIC + word_count.to_bytes(2, "little") + crc.to_bytes(2, "little")
    return header + body


def verify(blob: bytes) -> bool:
    if len(blob) < 8 or blob[:4] != MAGIC:
        print("no RDKY header (raw payload)")
        return False
    word_count = int.from_bytes(blob[4:6], "little")
    hdr_crc = int.from_bytes(blob[6:8], "little")
    body = blob[8:]
    got = crc16_ccitt(body)
    ok = (got == hdr_crc) and (word_count * 2 <= len(body))
    print(f"word_count={word_count} header_crc=0x{hdr_crc:04x} "
          f"computed_crc=0x{got:04x} body={len(body)}B -> {'OK' if ok else 'MISMATCH'}")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    ap.add_argument("--verify", action="store_true", help="verify an existing header instead of writing one")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        blob = f.read()

    if args.verify:
        return 0 if verify(blob) else 1

    out = args.output or (args.input + ".wrapped")
    with open(out, "wb") as f:
        f.write(wrap(blob))
    print(f"wrote {out} ({len(blob)} -> {len(blob) + 8} bytes, "
          f"crc=0x{crc16_ccitt(blob):04x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
