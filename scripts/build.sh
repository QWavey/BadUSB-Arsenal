#!/usr/bin/env bash
# Convenience wrapper. Expects the Luckfox toolchain in PATH. Example:
#   export PATH=$HOME/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH
#   ./scripts/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

CROSS="${CROSS_COMPILE:-arm-rockchip830-linux-uclibcgnueabihf-}"
if ! command -v "${CROSS}gcc" >/dev/null 2>&1; then
  echo "!! toolchain '${CROSS}gcc' not found — set CROSS_COMPILE= or add it to PATH" >&2
  exit 1
fi
make -j"$(nproc 2>/dev/null || echo 4)" CROSS_COMPILE="$CROSS"
echo "ok: out/ghostlink built for RV1106"
