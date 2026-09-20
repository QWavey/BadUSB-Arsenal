# CLEANUP.md — restructure manifest

Moves recorded during the /cleanup run.

## 2026-09-20 — move diagnostic firmwares to `firmware/tests/`

| Old path | New path |
|----------|----------|
| `firmware/main/usb_probe.c` | `firmware/tests/usb_probe.c` |
| `firmware/main/device_only_test.c` | `firmware/tests/device_only_test.c` |

Both files are git-tracked and fully recoverable (`git restore`). They were
**not** referenced by any build file — only documented in `README.md`, which
was updated to point at `firmware/tests/`. Their GPIO10/WS2812 usage is the
same status LED the shipping `firmware/main/led.c` already owns; the shipping
firmware changes no pins as a result of this move.

Also added:
- `firmware/tests/README.md` — what each test answered, pin warnings, how they were run.

## Intentionally left alone (flagged, not changed)

- `docs/reference_spotpear-hub_hid.[ch]` — CherryUSB driver reference cited by README; already in `docs/`.
- `c6-firmware/sdkconfig` — generated file that is tracked despite `.gitignore **/sdkconfig`; remove from git with `git rm --cached` if desired.
- `fs-force.patch` — upstream HAL patch file, referenced by README.