#!/usr/bin/env python3
"""Regenerate src/web/pages_embed.c from the WebUI page sources.

The C server (src/web/httpd.c) serves /, /app.js and /style.css from
/mnt/sd/WebUI when that override exists, and from the strings in
pages_embed.c when it does not. This script turns src/web/pages/index.html,
app.js and style.css into the three C string constants. The output is
deterministic, so running it twice gives the same file.

Run it through scripts/embed.sh, or directly: python3 scripts/embed.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAGES = ROOT / "src" / "web" / "pages"
OUT = ROOT / "src" / "web" / "pages_embed.c"

SYMBOLS = [
    ("EMBEDDED_INDEX_HTML", "index.html"),
    ("EMBEDDED_APP_JS", "app.js"),
    ("EMBEDDED_STYLE_CSS", "style.css"),
]

HEADER = (
    "/*\n"
    " * Compile-time embedded fallbacks for the WebUI. Users can override by\n"
    " * dropping index.html / app.js / style.css into /mnt/sd/WebUI, so these\n"
    " * strings only matter when the SD override is absent.\n"
    " *\n"
    " * GENERATED FILE. Do not edit by hand. Run scripts/embed.sh, which rebuilds\n"
    " * this file from src/web/pages/*.{html,js,css}. The Makefile runs it when\n"
    " * those sources change.\n"
    " */\n"
    "\n"
)

# Escaped characters per C string piece. Pieces stay well under the 4095-char
# minimum that C compilers must accept for one literal.
MAX_PIECE = 900


def escape_byte(b):
    if b == 0x0A:
        return "\\n"
    if b == 0x5C:
        return "\\\\"
    if b == 0x22:
        return '\\"'
    if 0x20 <= b < 0x7F:
        return chr(b)
    # Non-ASCII and control bytes: fixed three-digit octal, so a following
    # digit can never extend the escape.
    return "\\%03o" % b


def c_pieces(data):
    if b"\x00" in data:
        raise SystemExit("embed: a page contains a NUL byte, which a C string cannot hold")
    pieces, cur, size = [], [], 0
    for b in data:
        token = escape_byte(b)
        if cur and size + len(token) > MAX_PIECE:
            pieces.append("".join(cur))
            cur, size = [], 0
        cur.append(token)
        size += len(token)
    pieces.append("".join(cur))
    return ['"' + piece + '"' for piece in pieces]


def build():
    out = [HEADER]
    for symbol, name in SYMBOLS:
        data = (PAGES / name).read_bytes().replace(b"\r\n", b"\n")
        pieces = c_pieces(data)
        out.append("const char *%s =\n" % symbol)
        for i, piece in enumerate(pieces):
            end = ";" if i == len(pieces) - 1 else ""
            out.append(piece + end + "\n")
        out.append("\n")
    return "".join(out)


def main():
    text = build()
    if "--check" in sys.argv[1:]:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print("pages_embed.c is out of date; run scripts/embed.sh")
            return 1
        print("pages_embed.c is up to date")
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d bytes)" % (OUT.relative_to(ROOT), len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
