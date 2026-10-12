# DuckyScript on GhostLink

GhostLink's DuckyScript is a subset of DS 3.0 plus everything from
[QWavey/ESP-S3-Key-BadUSB][esp] that could be mapped to the WebBee's
hardware. Commands fall into three groups:

1. **Fully supported** — fire for real.
2. **GhostLink extensions** (`GL_*`) — write-through to the device.
3. **Advisory** — the WebBee hardware doesn't support them, so the script
   parses, logs, and emits a timeline event, but no I/O happens.

[esp]: https://github.com/QWavey/ESP-S3-Key-BadUSB

## The hardware the WebBee has (and doesn't)

| Thing                | WebBee has it? | DuckyScript surface                                |
|----------------------|---------------|----------------------------------------------------|
| HID keyboard         | ✓ (hidg0)     | STRING, keycodes, modifiers, HOLD/RELEASE          |
| Ethernet (eth0)      | ✓             | JOIN_INTERNET / LEAVE_INTERNET → NET_MODE          |
| USB gadget OTG       | ✓             | identity override via Clone tab                    |
| On-board button      | ✓ (evdev)     | WAIT_FOR_BUTTON_PRESS, BUTTON_DEF                  |
| WiFi radio           | ✗             | WIFI_* commands are advisory                       |
| Bluetooth            | ✗             | BT_*, BLUETOOTH_* commands are advisory            |
| RGB LED array        | ✗             | LED_*, RGB, BLINK_LED_* advisory                   |
| SD-card as MSC       | ✗ (slot only) | SD_*, STORAGE_*, UPLOAD_FILE, DOWNLOAD_FILE adv.   |
| Thumbdrive gadget    | ✗             | HIDE_PAYLOAD, REVERT_TO_THUMBDRIVE advisory        |

## Fully supported

The classics (DS 1/2): `STRING`, `STRINGLN`, `STRING_BASH`,
`STRING_POWERSHELL`, `DELAY`, `WAIT`, `DEFAULTDELAY`, `DEFAULTCHARDELAY`,
`REPEAT`, modifier+key chords (`CTRL ALT T`), named keys (`ENTER`,
`TAB`, `UPARROW`…, `F1`-`F12`), `KEYCODE`, `INJECT_MOD`.

Hold-release: `HOLD <keys...>`, `RELEASE` / `STOPHOLD`.

Flow control: `IF (...)`, `WHILE (...)`, `END_IF`, `END_WHILE`, `VAR`,
`DEFINE`, `SET`, `$VAR` substitution.

Scripts: `IMPORT <file>` (inline include), `RUN_PAYLOAD <name>` (chain),
`EXTENSION name` + `END_EXTENSION`, `SET_BOOT_SCRIPT <name>`.

Random: `RANDOM_LOWERCASE_LETTER`, `RANDOM_UPPERCASE_LETTER`,
`RANDOM_LETTER`, `RANDOM_NUMBER`, `RANDOM_SPECIAL`, `RANDOM_CHAR`.

Time: `GET_TIME` → `$_TIME`, `GET_DAY` → `$_DAY`.

System: `REBOOT`, `SHUTDOWN`, `FACTORY_RESET`, `JOIN_INTERNET`,
`LEAVE_INTERNET`, `IF_ONLINE` / `IF_OFFLINE` → `$_ONLINE`.

Button: `WAIT_FOR_BUTTON_PRESS` / `BUTTON_WAIT`, `SET_BUTTON_PIN
<evdev-path>`.

## GhostLink extensions

| cmd                     | purpose                                         |
|-------------------------|-------------------------------------------------|
| `GL_SET KEY=VAL`        | overlay-write a `config.txt` line               |
| `GL_RELOAD_DNSMASQ`     | kick dnsmasq to pick up new rules               |
| `GL_RUN <name>`         | run a sibling .sh payload                       |
| `GL_LOG <text>`         | append to ghostlink.log                         |
| `GL_WAIT <seconds>`     | sleep                                           |
| `GL_SPOOF_ADD <host>`   | append to `REDIRECT_FROM`                       |
| `GL_REBOOT`             | reboot the device                               |

## Advisory

The script tokenizer colors advisory commands in orange italics, logs
the intent to `ghostlink.log`, and emits an `ducky_advisory` event to
the Attack Timeline so what the script "wanted" to do is visible even
when the hardware can't fulfill it. See the hardware table above.
