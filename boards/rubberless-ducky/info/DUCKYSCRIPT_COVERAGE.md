# DuckyScript 3.0 Coverage

Status of every DuckyScript command/feature in this firmware, cross-referenced
against the [Hak5 DuckyScript Quick Reference](https://documentation.hak5.org/hak5-usb-rubber-ducky/duckyscript-tm-quick-reference).

The firmware is an **opcode interpreter** — it executes the compiled `INJECT.BIN`
byte stream, not DuckyScript text. So many commands are covered "for free":
they compile down to primitives the firmware already implements (raw keystrokes,
`GOTO`, `CALL`/`RETURN`, `IF`, `ASSIGN`, `eval_op`). Token values below were
**derived from real PayloadStudio-compiled samples** (`tests/1..4.bin`), never
guessed. Keep `tools/inject_inspect.py` in sync with any change here.

## CRITICAL fix — binary-operator operand order (found via tests/13.bin)
The compiler emits a binary operation as `e801 dest s1 s2 OP` (operator LAST).
The interpreter read `op` from the 4th word and `s2` from the 5th — swapped — so
`eval_op` received a register number as its opcode, hit `default: return 0`, and
EVERY `+ - * / % ^ == != < > <= >= && || & | << >>` evaluated to 0. Because
`IF`/`WHILE` conditions are compiled as a comparison into a temp then a branch,
this silently broke **all conditionals and loops** too. Simple copies
(`op == 0`, 4-word form) were unaffected, which is why tests/3–7 never exposed it.
Fixed: `s2 = word[pc+3]`, `op = word[pc+4]`; the 4th word being 0 still
disambiguates a plain copy. Verified against tests/13.bin.

## Bug-hunt fixes (HOLD/RELEASE + notes)
- **Multi-key HOLD (was broken) — FIXED.** `hid_send_report` built a fresh
  single-key report each call, so a second `HOLD` evicted the first and holding
  a modifier while pressing a key dropped the modifier. Added a held-key set
  (`g_held_keys[6]` + `g_held_mods`); HOLD adds to it, and every HOLD/typed key
  rebuilds the full 6-key report. The documented "Holding Multiple Keys" pattern
  now works, and keys typed while a HOLD is active carry the held state.
- **RELEASE freed ALL keys (was broken) — FIXED.** `KEY_UP` called
  `hid_release_all()` and ignored its key argument, so `RELEASE a` dropped every
  held key. Now removes only the named key/modifier from the held set.
  `hid_release_all()` (RESET / STOP / boot & button scrubs) still clears the
  whole set, which is the intended "release everything" semantics.
- **RANDOM_* on non-US hosts (known limitation, documented).** `RANDOM_LETTER/
  CHAR/SPECIAL` are generated on-device via a US-QWERTY `char_to_hid`, so on a
  German (etc.) host they mis-map (Y/Z swap, wrong shift-symbols). No on-device
  fix is possible — the firmware can't know the host layout. STRING is
  unaffected (the compiler pre-encodes it against the selected keymap). See the
  note at `char_to_hid`.
- **Block-terminator scan (theoretical only, documented).** IF/BUTTON_DEF locate
  their END marker by raw word value + block id. A keystroke word can't collide
  with `0x1ff4`/`0xebf4` (needs modifier byte 0xf4, which the compiler never
  emits), so valid payloads are safe; only a corrupt/hand-crafted stream could
  defeat it. Noted at the IF handler.

## v7 changes — bug fix + firmware-native SD features

- **LED_R color fix (verified).** Source order `LED_OFF / LED_R / LED_G` always
  compiles to `0xeaed / 0xeced / 0xebed` (confirmed in `tests/12.bin`,
  `tests/13.bin`), so `0xeced` is **LED_R (red)**. The firmware had it lighting
  green; every payload's `LED_R` showed the wrong colour. Now red.
  `tools/inject_inspect.py` relabelled to match.
- **PRNG upgraded to xorshift32** (was an LCG). Makes `$_JITTER` timing genuinely
  non-uniform (an LCG's low bits are regular and modelable by a fixed-cadence
  detector); RANDOM_* benefits too.
- **Silent failures now visible via LED blink codes:** payload larger than the
  20 KB RAM buffer → 3× red (pre-run) and again at end-of-run; CALL/BUTTON
  nesting past depth 32 → 4× red at end-of-run; integrity-check failure → 6× red.
- **`CONFIG.TXT`** (SD root, optional): retune timing / jitter / VID-PID /
  autocal / dryrun without a reflash. See `CONFIG.sample.txt`.
- **Boot-time payload selection:** hold the button at power-on to pick
  `INJECT1/2/3.BIN` (red flash per press); not held → `INJECT.BIN`, zero latency.
- **Optional integrity header** (`RDKY` magic + CRC16, added by
  `tools/wrap_inject.py`): a corrupt payload is refused, not typed as garbage.
- **Multi-sector keystroke-reflection loot:** reflection no longer hard-caps at
  one 512-byte sector; full sectors are committed from main context and it
  continues into the next (board SD-write limits still apply).
- **Dry-run lint** (`CONFIG.TXT dryrun=1`): validate jump targets, blink the
  result (3 green = OK / N red = N bad targets), type nothing.
- **Self-calibrating timing (invention, `autocal=1`):** measure the host input-
  stack latency from the CapsLock LED echo and trim the warm-up ramp on a
  proven-fast host. Only ever speeds up a fast host; slow/no-echo keeps defaults.

## Covered ✅

| Area | Commands |
|---|---|
| Typing | `STRING`, `STRINGLN`, all key names + modifiers (raw keystroke words) |
| Timing | `DELAY` (incl. >255 ms chunking) |
| Control flow | `IF`/`ELSE`/`END_IF`, `WHILE`/`END_WHILE` (via `GOTO`), `FUNCTION`/`RETURN` (`CALL`/`RETURN`) |
| Math/logic | all operators + `^` exponent, `%` modulus (`eval_op`) |
| Variables | `VAR`, `$_` internal vars (see table below) |
| Attack modes | `ATTACKMODE HID/STORAGE/OFF`, `VID_/PID_/MAN_/PROD_/SERIAL_`, `SAVE_/RESTORE_ATTACKMODE` |
| Lock keys | all 9 `WAIT_FOR_*`, `SAVE_/RESTORE_HOST_KEYBOARD_LOCK_STATE` |
| Button | `WAIT_FOR_BUTTON_PRESS`, `BUTTON_DEF`/`END_BUTTON`, `ENABLE_/DISABLE_BUTTON` |
| LEDs | `LED_OFF/R/G` |
| Holds | `HOLD`/`RELEASE` (`KEY_DOWN`/`KEY_UP`), `INJECT_MOD` (bare modifier) |
| Randomization | all 6 `RANDOM_*` |
| Exfil | `EXFIL` → `LOOT.BIN` |
| Payload control | `STOP_PAYLOAD`, `RESET`, **`RESTART_PAYLOAD`**, **`HIDE_PAYLOAD`**, **`RESTORE_PAYLOAD`** |

Bold = added in the full-parity pass.

## Token map (verified against tests/1..4.bin)

### Opcodes
| Command | Token | Source |
|---|---|---|
| `RESTART_PAYLOAD` | `0xeaf1` | tests/1.bin |
| `HIDE_PAYLOAD` | `0xf8e9` | tests/2.bin |
| `RESTORE_PAYLOAD` | `0xf9e9` | tests/2.bin |
| `INJECT_MOD` | `0xe6e9` | tests/11.bin |

### INJECT_MOD — was a real bug, now fixed
`INJECT_MOD <MODIFIER>` (tap a modifier alone, e.g. `INJECT_MOD WINDOWS`)
compiles to `0xe6e9` followed by a keycode-0 modifier word (`0x0008` for GUI).
The firmware did not handle `0xe6e9`, so it was dropped and the following
`0x0008` was read as `DELAY 8ms` — the modifier was never pressed. Now `0xe6e9`
sets a prefix flag; the next word is taken as a bare-modifier TAP (press+release)
when it is a keycode-0 modifier, or falls through to `KEY_DOWN` for the HOLD form
(`INJECT_MOD` then `HOLD CONTROL`). Verified against `tests/11.bin`.

### Internal variables added
| Variable | Token | Source |
|---|---|---|
| `$_SYSTEM_LEDS_ENABLED` (read) | `0x8642` | existing write |
| `$_EXFIL_MODE_ENABLED` | `0x9742` | tests/4.bin |
| `$_STORAGE_ACTIVITY_TIMEOUT` | `0x9842` | tests/4.bin |
| `$_BUTTON_TIMEOUT` | `0x9942` | tests/4.bin |
| `$_HOST_CONFIGURATION_REQUEST_COUNT` (read-only) | `0x9f42` | tests/4.bin |
| `$_JITTER_ENABLED` | `0xa242` | tests/3.bin |
| `$_JITTER_MAX` | `0xa342` | tests/3.bin |

## Resolved

### 1. Constant-pool integer literals were 256× too large (pre-existing) — FIXED
The constant pool stores 16-bit literals **little-endian**, but `get_word()`
reads the stream big-endian. So `10`→`0x0a00`, `300`→`0x2c01` (`0x012c`
byte-swapped), `65535`→`0xffff`. The register-block loader now byte-swaps each
pool entry back to host order (`main.c`). Verified against `tests/3..6.bin`.
This fixes `$_JITTER_MAX` and every numeric literal used in DuckyScript math.

### 2. `$_RANDOM_SEED` vs `$_RANDOM_INT` token collision — RESOLVED
`tests/6.bin` shows `$_RANDOM_INT` = `0xa842`. The firmware's old `0xf242`→
`rand_range` mapping was mislabeled; `0xf242` is `$_RANDOM_SEED`. Now:
`0xa842` → fresh `rand_range` (read), `0xf242` → seed (read/write).

## Keystroke Reflection — IMPLEMENTED

`$_EXFIL_MODE_ENABLED = TRUE` now makes the USB OUT-report callback decode the
host's lock-LED toggles into `LOOT.BIN`, per the Hak5 reflection protocol:
NUMLOCK toggle = 1-bit, CAPSLOCK toggle = 0-bit (MSB-first into bytes),
SCROLLLOCK toggle = end-of-stream (partial byte flushed). Bits assemble in RAM
during the wait loop (no SD I/O from the callback); the sector is committed by
`exfil_flush()` when `$_EXFIL_MODE_ENABLED` is cleared or at payload end. One
session is bounded by the 512-byte loot buffer (one sector) — ample for
credentials/SSIDs. Requires a pre-sized `LOOT.BIN` in the SD root (same as the
`EXFIL` command). Uses existing `SAVE_/RESTORE_HOST_KEYBOARD_LOCK_STATE` and
`WAIT_FOR_SCROLL_CHANGE`, all already present.

## Done behaviors
- `$_BUTTON_TIMEOUT`: wired into `WAIT_FOR_BUTTON_PRESS` (ms; 0 = wait forever;
  on timeout `$_BUTTON_PUSH_RECEIVED` stays 0).
- `HIDE_PAYLOAD`/`RESTORE_PAYLOAD`: mapped to MSC suppression (board-constrained
  interpretation — see `apply_attackmode` MISO note).

## Also wired (tests/7.bin)
- `$_RECEIVED_HOST_LOCK_LED_REPLY` = `0x9642` — exposed in `read_var` (returns
  `received_led_reply`). Completes OS detection: an extension clears it via
  `SAVE_HOST_LOCK_STATE`, toggles a lock key, then reads this to see if the host
  replied within its window.
- `$_EXFIL_LEDS_ENABLED` = `0x8942` — read/write; flashes the LED as each loot
  byte is saved during reflection (toggle-only, no blocking in the callback).

## Also wired (tests/8.bin, tests/9.bin)
- `$_RANDOM` = `0xf342` — full-range random (`rand_range(0,0xFFFF)`). **Fixes
  `VID_RANDOM`/`PID_RANDOM`** (and the `MAN_/PROD_/SERIAL_RANDOM` string forms),
  which previously read as 0 because the token was unhandled. Confirmed by
  `tests/9.bin`: `ATTACKMODE HID STORAGE VID_RANDOM PID_RANDOM` emits `VID_`
  (`0xf5f5`) / `PID_` (`0xf6f6`) each followed by `0xf342`.
- `$_STORAGE_LEDS_ENABLED` = `0x8742`, `$_INJECTING_LEDS_ENABLED` = `0x8842` —
  read/write storage vars.
- `$_RANDOM_LETTER_KEYCODE` = `0xff42`, `$_RANDOM_NUMBER_KEYCODE` = `0xfc42`,
  `$_RANDOM_CHAR_KEYCODE` = `0xfe42` — return `last_random_value` (the last
  value produced by a `RANDOM_*` generator). Best-effort; this family is unused
  in the payload library. (`LOWER/UPPER/SPECIAL_LETTER_KEYCODE` tokens not yet
  sampled — same niche family.)

## Real variables that official PayloadStudio does NOT emit tokens for
`$_LED_CONTINUOUS_SHOW_STORAGE_ACTIVITY`, `$_LED_SHOW_CAPS`, `$_LED_SHOW_NUM`,
`$_LED_SHOW_SCROLL` **are** genuine Hak5 internal variables — confirmed by
mechanically grepping the raw docs corpus (`llms-full.txt`), not an AI summary:
they appear verbatim in the official Internal Variables table (e.g. `$_LED_SHOW_CAPS`
= "bind the GREEN LED state to the CAPSLOCK state").

However, the official PayloadStudio compiler does **not** emit internal tokens
for these four. Verified from the raw compiled bytes:
- `tests/8.bin` (reading them) and `tests/10.bin` (writing them) both produce
  **dynamically-allocated user registers** (`0x0001`, `0x0002`, … assigned by
  declaration order), never a fixed `0x__42` internal token — whereas every
  other variable in the same doc table (incl. `$_STORAGE_LEDS_ENABLED`=`0x8742`,
  `$_INJECTING_LEDS_ENABLED`=`0x8842`) DOES get one.
- e.g. `tests/10.bin`: `e801 0001 6842 0000` = `ASSIGN reg[1] = TRUE`.

This is a discrepancy inside Hak5's own toolchain (documented, but not compiled).
Consequences for this firmware:
- No stable token exists in the byte stream, so there is nothing for the
  firmware to hook — a user register's index depends on declaration order and
  is not a reliable handle. Not fixable at the firmware level.
- All four are unused across the 374 official payloads.
- If these LED behaviors are wanted regardless, they could be implemented as a
  FIRMWARE-NATIVE feature (bind LED to caps/num/scroll or storage activity),
  decoupled from the DuckyScript variable PayloadStudio won't emit. Optional.

## Non-behavioral item
- `$_STORAGE_ACTIVITY_TIMEOUT`: stored/readable, but there is no
  storage-activity-LED feature on this board to bind it to (the SD is not shown
  as a host drive during typing). Board constraint, not a missing command.

## Bottom line
Every command and internal variable in the Hak5 DuckyScript 3.0 reference is
implemented, explicitly board-constrained, or shown to be non-existent in the
compiler. All tokens verified against compiled samples `tests/1..9.bin`.
Not yet validated on real AT32UC3B1 hardware.
