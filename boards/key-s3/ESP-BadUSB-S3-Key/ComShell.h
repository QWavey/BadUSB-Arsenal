#ifndef COM_SHELL_H
#define COM_SHELL_H

// v4.5: "Allow COM connections" mode.
//
// When enabled (persisted as NVS key `com_on`), on the NEXT boot the device
// presents itself over USB as a CDC virtual serial port ONLY — no HID, no
// MSC, no Mass Storage. PuTTY (or any COM-port terminal) can then connect
// to the ESP over the host's COM port and drive an interactive shell.
//
// Because the USB descriptor changes composition (HID/MSC swap for CDC),
// toggling the setting requires a reboot — same pattern as ATTACKMODE.

void comShellBegin();     // called once during setup() when com_on=true
void comShellLoop();      // service the CDC port each loop() tick
bool comShellActive();    // true if CDC shell is currently running

#endif // COM_SHELL_H
