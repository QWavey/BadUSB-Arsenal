#ifndef GHOST_HID_H
#define GHOST_HID_H

int hid_start(void);
int hid_stop(void);

/* Fires a DuckyScript payload now. Returns 0 on success. */
int hid_run_payload(const char *path);

#endif
