#pragma once
// BadUSB "live type" — call once at boot after c6_uart is up. Spawns a
// background task that watches SDIO scratch reg 44 for ASCII bytes from
// the C6 web UI and injects them as HID keyboard reports on the same
// queue the physical keyboard uses.
void live_type_start(void);
