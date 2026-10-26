#!/usr/bin/env bash
# v1.1.0+: publish a GitHub release with the corresponding .espkg attached
# and marked as `latest`. Called after `git push origin vX.Y.Z`.
#
# Usage:
#   tools/release.sh <version>            # e.g. tools/release.sh 1.1.0
#   tools/release.sh <version> <fw>       # e.g. tools/release.sh 1.1.0 4.35
#
# If <fw> is omitted, the highest-numbered dist/badusb-full-*.espkg is used.

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <version> [fw-version]" >&2
  exit 1
fi
tag="v$1"
fw="${2:-}"

if [[ -z "$fw" ]]; then
  # Take the highest-numbered espkg file in dist/.
  latest=$(ls -1 dist/badusb-full-*.espkg 2>/dev/null | sort -V | tail -n 1 || true)
  if [[ -z "$latest" ]]; then
    echo "no dist/badusb-full-*.espkg found - build one first" >&2
    exit 1
  fi
  espkg="$latest"
else
  espkg="dist/badusb-full-${fw}.espkg"
fi
if [[ ! -f "$espkg" ]]; then
  echo "espkg not found: $espkg" >&2
  exit 1
fi

# Extract the tag-annotation body for the release notes.
notes=$(git tag -n99 --format='%(contents)' "$tag" 2>/dev/null || true)
if [[ -z "$notes" ]]; then
  notes=$(git log -1 --format='%B' "$tag" 2>/dev/null || echo "Release $tag")
fi

# Create or update the release.
if gh release view "$tag" >/dev/null 2>&1; then
  echo "[release] $tag already exists - uploading espkg + marking latest"
  gh release upload "$tag" "$espkg" --clobber
  gh release edit   "$tag" --latest
else
  echo "[release] creating $tag with $espkg"
  gh release create "$tag" "$espkg" --title "Version ${tag#v}" --notes "$notes" --latest
fi
echo "[release] done: $(gh release view "$tag" --json url --jq .url)"
