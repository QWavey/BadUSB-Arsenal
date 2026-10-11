# GhostLink DuckyScript — supported syntax

GhostLink's `ducky.py` runs a usable subset of DuckyScript 3.0.

## Fully supported

### DS1 primitives
- `REM ...` / `# ...` / `REM_BLOCK` ... `END_REM`
- `STRING <text>`, `STRINGLN <text>` (adds Enter), `$var` interpolation inside STRING
- `DELAY <ms>`, `DEFAULTDELAY <ms>`, `DEFAULTCHARDELAY <ms>`, `WAIT <seconds>`
- Named keys: `ENTER` `RETURN` `ESC` `ESCAPE` `TAB` `SPACE` `BACKSPACE`
  `DELETE`/`DEL` `UP`/`UPARROW` `DOWN`/`DOWNARROW` `LEFT`/`LEFTARROW`
  `RIGHT`/`RIGHTARROW` `HOME` `END` `PAGEUP` `PAGEDOWN` `INSERT`
  `PRINTSCREEN` `CAPSLOCK` `NUMLOCK` `SCROLLLOCK` `PAUSE`/`BREAK`
  `MENU`/`APP` `F1`..`F12`
- Modifiers: `CTRL`/`CONTROL`, `ALT`, `SHIFT`, `GUI`/`WINDOWS`/`WIN`/`SUPER`/`COMMAND`/`CMD`
- Modifier-only lines (e.g. a bare `GUI` taps the Start key)
- `REPEAT N` (repeats the previous executed line N times)

### DS3 preprocessor
- `EXTENSION NAME` ... `END_EXTENSION` — define a reusable block in-script
- `DEFINE #CONST value` — compile-time literal substitution
- `EXTENSION name` — include a file from `/root/ghostlink/extensions/` or
  `/root/ghostlink/extensions/hak5/` (looks up `.ducky` then `.txt`)

### DS3 runtime
- `VAR $NAME = <expr>` — declare + assign
- `$NAME = <expr>` — reassign
- `IF (<expr>)` / `ELSE` / `END_IF`
- `WHILE (<expr>)` / `END_WHILE`
- Bare `NAME` or `NAME()` on its own line — call an EXTENSION block
- Operators: `+ - * / %` `== != < > <= >=` `&& || !` parens

### Built-in variables
- `$_CAPSLOCK_ON`, `$_NUMLOCK_ON`, `$_SCROLLLOCK_ON` — read live from the
  host via HID LED output reports (background reader on `/dev/hidg0`)
- `$_RANDOM_INT`, `$_RANDOM_SHORT`, `$_RANDOM_BYTE`, `$_RANDOM_NUMBER`
- `$_RANDOM_LOWERCASE_LETTER`, `$_RANDOM_UPPERCASE_LETTER`, `$_RANDOM_LETTER`

### GhostLink extensions
- `GL_SET KEY=VALUE` — atomic rewrite of one config.txt line
- `GL_RELOAD` / `GL_RELOAD_DNSMASQ` — kick dnsmasq to pick up new rules
- `GL_RUN name.sh` — fire a sibling payload
- `GL_LOG "text"` — append to `ghostlink.log`
- `GL_WAIT N` — sleep N seconds
- `GL_SPOOF_ADD host` — add host to `REDIRECT_FROM` + enable + reload
- `GL_REBOOT` — sync + reboot
- `WAIT_FOR_BUTTON_PRESS` / `BUTTON_WAIT` — block until on-board button

## Advisory-only (parsed, logged, not executed)

These commands come from Hak5 Rubber-Ducky firmware that can live-rebind
its USB gadget. On the Luckfox WebBee that's the brick path
([docs/HID.md](HID.md)), so we accept and log them but don't actually
switch gadget configurations:

- `ATTACKMODE ...`
- `LED_*`, `LCD_*`
- `EXFIL ...`
- `SAVE_HOST_KEYBOARD_LOCK_STATE`, `RESTORE_HOST_KEYBOARD_LOCK_STATE`
- `PAYLOAD_FINISHED`, `END`, `FINISH`, `RESTART_PAYLOAD`, `RESET`
- `DISABLE_BUTTON`, `ENABLE_BUTTON`

## Hak5 extension library — status

| extension | status |
|---|---|
| `button_deploy` | ✓ runs (ATTACKMODE no-op + WAIT_FOR_BUTTON_PRESS works) |
| `detect_ready` | ✓ runs (uses VAR/WHILE/CAPSLOCK; $_CAPSLOCK_ON reflects real HID state) |
| `passive_detect_ready` | ✓ runs (same substrate as detect_ready) |
| `hello_os` | ⚠ degrades (OS stays "UNKNOWN"; os_detect's heuristics not fully ported) |
| `os_detect` | ⚠ parses; some branches never fire because gadget rebind is unavailable |
| `passive_windows_detect` | ⚠ parses; limited to what HID LED state can reveal |
| `self_destruct` | ⚠ advisory — ATTACKMODE OFF + file delete not applicable to GhostLink |
| `exfil_auto_eof_detect` | ⚠ advisory (no EXFIL endpoint on this gadget) |
| `linux_hid_exfil` | ⚠ advisory |
| `windows_hid_exfil` | ⚠ advisory |
| `protected_storage_mode` | ⚠ advisory (no live ATTACKMODE switch) |
| `translate` | ✓ runs if plain substitution; falls back when gadget-dependent |

In practice: write new payloads using the subset above, and treat the
Hak5 library as inspiration. `button_deploy` and `detect_ready` port
directly.
