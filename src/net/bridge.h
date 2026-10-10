#ifndef GHOST_BRIDGE_H
#define GHOST_BRIDGE_H

int bridge_start(void);
int bridge_stop(void);

/* Returns 1 if usb0 is up (gadget enumerated). */
int bridge_usb_up(void);

#endif
