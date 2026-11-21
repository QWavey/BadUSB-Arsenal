#!/bin/sh
# Rebuild src/web/pages_embed.c from src/web/pages/*.{html,js,css}.
# Run from anywhere: sh scripts/embed.sh
set -e
cd "$(dirname "$0")/.."
if python3 -c 'import sys' >/dev/null 2>&1; then PY=python3
elif python -c 'import sys' >/dev/null 2>&1; then PY=python
else echo "embed: needs python3 (or python) on PATH" >&2; exit 1
fi
exec "$PY" scripts/embed.py "$@"
