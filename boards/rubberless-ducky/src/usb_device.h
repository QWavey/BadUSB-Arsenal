#ifndef USB_DEVICE_H_
#define USB_DEVICE_H_

#include <stdint.h>

/* Standard USB Request Types */
#define REQ_TYPE_STANDARD               (0x00 << 5)
#define REQ_TYPE_CLASS                  (0x01 << 5)
#define REQ_TYPE_VENDOR                 (0x02 << 5)

/* Standard USB Requests */
#define GET_STATUS                      0x00
#define CLEAR_FEATURE                   0x01
#define SET_FEATURE                     0x03
#define SET_ADDRESS                     0x05
#define GET_DESCRIPTOR                  0x06
#define SET_DESCRIPTOR                  0x07
#define GET_CONFIGURATION               0x08
#define SET_CONFIGURATION               0x09
#define GET_INTERFACE                   0x0A
#define SET_INTERFACE                   0x0B
#define SYNCH_FRAME                     0x0C

/* Descriptor Types */
#define DESC_DEVICE                     0x01
#define DESC_CONFIGURATION              0x02
#define DESC_STRING                     0x03
#define DESC_INTERFACE                  0x04
#define DESC_ENDPOINT                   0x05
#define DESC_DEVICE_QUALIFIER           0x06
#define DESC_OTHER_SPEED_CONFIG         0x07
#define DESC_INTERFACE_POWER            0x08
#define DESC_OTG                        0x09
#define DESC_HID                        0x21
#define DESC_HID_REPORT                 0x22

#endif /* USB_DEVICE_H_ */