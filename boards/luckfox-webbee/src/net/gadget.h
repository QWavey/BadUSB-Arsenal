#ifndef GHOST_GADGET_H
#define GHOST_GADGET_H

/*
 * USB gadget setup via /sys/kernel/config/usb_gadget/.
 * Composite device: NCM + HID + optional storage.
 * On RV1106 the UDC name is typically "ffb00000.usb" — we discover it at runtime.
 */

int gadget_start(void);   /* reads g_cfg */
int gadget_stop(void);

#endif
