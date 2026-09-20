# Board bring-up test firmwares

Historical research firmwares used during board bring-up to answer specific
hardware questions. They are **not** part of the normal build — the shipping
`firmware/main/CMakeLists.txt` does not compile them, and they must not be
flashed over a working setup.

| File | Question it answered | Result |
|------|----------------------|--------|
| `usb_probe.c` | Which Type-C carries the P4's Full-Speed OTG lines (bypassing the CH334 hub, which the HS-OTG stack cannot address). Boots into FS **host** mode, flashes the onboard WS2812 (GPIO10) per port state, and tells you which port enumerates a keyboard. | The core Type-C / U4-Slave carries FS-OTG. |
| `device_only_test.c` | Whether U4 with the switch on "1 Slave" routes the P4's HS-OTG directly to U4. Becomes a TinyUSB HID **device** (VID 303A / PID 0x4004) and checks if a PC on U4 sees a new keyboard. | Routing needed the PHY swap; the production firmware uses the FS-OTG core port instead. |

## Be careful with pins

- `usb_probe.c` drives the WS2812 on **GPIO10** — the same status LED the
  shipping `firmware/main/led.c` drives. It is safe to run them at different
  times, never together.
- `device_only_test.c` uses no GPIOs.

## How they were used

1. Copy the file into `firmware/main/` temporarily and add it to the CMake
   `SRCS`, then `idf.py build flash monitor`.
2. Restore `firmware/main/CMakeLists.txt` afterwards.

The production build (`main.c` + `hid_host_task.c` + `c6_uart.c` + `kb_layouts.c`
+ `led.c`) supersedes both probes — keep these here for reference only.