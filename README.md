<p align="center">
  <a href="https://www.amazon.de/dp/B0F3XMYYQY?ref=cm_sw_r_cso_wa_apan_dp_LIGHTSABER281DP7UB21&ref_=cm_sw_r_cso_wa_apan_dp_LIGHTSABER281DP7UB21&social_share=cm_sw_r_cso_wa_apan_dp_LIGHTSABER281DP7UB21&th=1">
    <img width="479" height="450" alt="ESP32-S3 DevKitC board" src="https://github.com/user-attachments/assets/b76b5615-5280-4249-b5c5-efd52f53d6e5" />
  </a>
</p>

# ESP32-S3 BadUSB (proof of concept)

A proof-of-concept BadUSB built on the ESP32-S3 DevKitC. It emulates a USB HID keyboard, injects keystrokes, and runs custom payloads. A web control panel served over Wi-Fi lets you upload, edit, and run BadUSB scripts from a browser.

It is meant for security research, red teaming, and authorized penetration testing.

## Hak5 language files

This repository does not include or redistribute Hak5's DuckyScript language files or any other Hak5 content. Those files belong to Hak5 LLC.

Get them from the official [Hak5 DuckyScript languages folder](https://github.com/hak5/usbrubberducky-payloads/tree/master/languages) and copy the `languages` folder onto the SD card. If that link is down, try the [Wayback Machine copy](http://web.archive.org/web/20250000000000*/https://github.com/hak5/usbrubberducky-payloads/tree/master/languages).

USB Rubber Ducky and DuckyScript are trademarks of Hak5 LLC. This project is not affiliated with or endorsed by Hak5. If Hak5 asks for the reference to be removed, I will remove it right away.

## Hardware

- Board: ESP32-S3 DevKitC-1 (N16R8)
- SD card interface: SPI

### SD card wiring

| SD card pin | ESP32-S3 DevKitC-1 pin |
| ----------: | :--------------------- |
|       `3V3` | `3V3`                  |
|       `GND` | `GND`                  |
|   `CS (SS)` | `GPIO10`               |
|      `MOSI` | `GPIO11`               |
|      `MISO` | `GPIO13`               |
|       `CLK` | `GPIO12`               |

## Features implemented

- Basic key presses and combinations (Win+R, Alt+Tab, etc.)
- String typing
- Script management: save, delete, and run scripts from the UI
- Wi-Fi access point hosting and web UI

## Roadmap

Status: **Done**, **Planned**, or **Not implemented yet**.

| #  | Feature (what it does)                                                                                       | Status             |
| -- | ------------------------------------------------------------------------------------------------------------ | ------------------ |
| 1  | Basic key combinations (press multiple keys at once)                                                         | Done               |
| 2  | Strings and variables (store and manipulate text)                                                            | Done               |
| 3  | Wi-Fi detection (detect available networks and run commands depending on whether a network is there)         | Done               |
| 4  | LED control (turn LEDs on and off)                                                                           | Done               |
| 5  | Raw keycodes (send specific key signals)                                                                     | Done               |
| 6  | SD card detection (detect SD card status)                                                                    | Done               |
| 7  | Uploading files (add non-text files)                                                                         | Done               |
| 8  | Function variables (store values for functions)                                                              | Planned            |
| 9  | Repeat and replay commands (repeat key sequences)                                                            | Planned            |
| 10 | Custom fonts in the web GUI (use fonts from the SD card)                                                     | Planned            |
| 11 | Math operations (perform calculations)                                                                       | Not implemented yet |
| 12 | Blocks (visual programming blocks)                                                                           | Not implemented yet |
| 13 | Hold keys (hold keys across time intervals)                                                                  | Not implemented yet |
| 14 | Rower payloads (run multiple scripts in sequence)                                                            | Not implemented yet |
| 15 | Boolean variables (store true or false)                                                                      | Not implemented yet |
| 16 | Syntax error highlighting (show errors in the editor)                                                        | Not implemented yet |
| 17 | Customisation web GUI (adjust GUI settings)                                                                  | Not implemented yet |
| 18 | Expose SD card over USB (show or hide SD card access)                                                        | Not implemented yet |
| 19 | Noisy OS detection (detect the operating system)                                                             | Planned            |
| 20 | Keylog addon (record keystrokes)                                                                             | Not implemented yet |
| 21 | Connecting to Wi-Fi (establish a wireless connection)                                                        | Not implemented yet |
| 22 | Web actions (perform tasks over Wi-Fi)                                                                       | Not implemented yet |
| 23 | Copy, cut, and paste files from the SD card to the PC                                                        | Not implemented yet |
| 24 | Disable USB function on boot and only draw power                                                             | Not implemented yet |
| 25 | Mouse functionality                                                                                          | Not implemented yet |
| 26 | Silent OS detection                                                                                          | Not implemented yet |
| 27 | HID over Bluetooth                                                                                           | Not implemented yet |
| 28 | Start the script when someone connects or disconnects to the ESP (Bluetooth, Wi-Fi)                          | Not implemented yet |
| 29 | Turn Bluetooth and Wi-Fi on and off                                                                          | Not implemented yet |
| 30 | Turn Bluetooth and Wi-Fi on and off when a Wi-Fi network is detected                                         | Not implemented yet |
| 31 | Chain commands together so they work under varying conditions                                                | Not implemented yet |
| 32 | While and for loops                                                                                          | Not implemented yet |
| 33 | Advanced scripting ([example](https://github.com/QWavey/ESP-32-S3-devkitc-BadUSB/wiki/Commands#example))     | Not implemented yet |

## File structure

On the SD card:

- `/languages`: language files, such as `en.json` and `de.json`. You need to create these yourself. They are not included in this repo.
- `/DESIGNS`: contains the `/CSS`, `/JS`, and `/FONTS` folders. You create these yourself.
- `index.html`: the website. Copy it from this repo's [`index.html`](https://github.com/QWavey/ESP-32-S3-devkitc-BadUSB/edit/main/index.html).

Planned layout:

- `index.html`: frontend for writing and running scripts
- `syntaxes.json`: every command (`STRING`, `DELAY`, `REPEAT`, and so on) with a description and examples
- `main.cpp`: firmware, ESP32 BadUSB logic
- `README.md`: this documentation
- `/payloads`: payload folder (done)
- `/FONTS`: fonts folder (done, but not working correctly yet)
- `/items`: custom designs and assets such as CSS, JS, and HTML
- `/languages`: keymap files (done)
- `config.json`: main config at the SD root for Wi-Fi hosting, password, and so on
- `/uploads`: uploaded files (done)

## Usage

1. Flash the firmware onto the ESP32-S3.
2. Copy `index.html` to the SD card.
3. Connect the SD card to the pins above. Do this while the power is off.
4. Connect to the Wi-Fi access point.
5. Open `192.168.4.1` in your browser.
6. Upload and run scripts from the web UI.

## Next steps

1. Add a `syntaxes.json` that lists all commands.
2. Define a full JSON schema for supported commands.
3. Add status indicators in the UI for working and broken features.
4. Implement the missing features one at a time.
5. Add customisation and design options.

The work-in-progress wiki is at [XQuantumWaveX/ESP-32-S3-devkitc-BadUSB](https://github.com/XQuantumWaveX/ESP-32-S3-devkitc-BadUSB/wiki).

## Contributing

Contact me on Discord (sniper74) and send your code suggestions.

## Safety and disclaimer

This project is for education, experimentation, and authorized testing. Do not use it for illegal or harmful activity. You are responsible for following local laws. It is provided as-is, with no warranty. Back up anything important before you experiment.

Electronics safety:

- Disconnect power before changing any wiring, and check voltages before you touch components.
- Use components and fuses with the correct ratings. Substituting parts can create fire or shock risks.
- Keep liquids away from the board, and work on a non-conductive surface.
- Keep small parts and tools away from children and pets.
- Do not cut open batteries, capacitors, or sealed components, and do not eat any part of the circuit.
- Do not attempt high-voltage, medical, or otherwise hazardous work without professional supervision.
- If a step is unclear or risky, check the official documentation or ask an expert first.

Any modifications you make are at your own risk. If you publish or share derived work, give credit and do not distribute instructions that enable harm.
