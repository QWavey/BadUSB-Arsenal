#include "usb_descriptors.h"
#include <stddef.h>
#include <string.h>

uint8_t usb_custom_serial_set = 0;

void usb_set_string_descriptor(uint8_t *desc, const char *str, uint8_t max_chars) {
    uint8_t len = 0;
    while (str[len] && len < max_chars) {
        len++;
    }
    desc[0] = (uint8_t)(2 + len * 2);
    desc[1] = 0x03;
    for (uint8_t i = 0; i < len; i++) {
        desc[2 + i * 2] = (uint8_t)str[i];
        desc[3 + i * 2] = 0;
    }
}

/* USB Standard Device Descriptor */
uint8_t usb_device_descriptor[] = {
    18,         /* bLength */
    0x01,       /* bDescriptorType: Device */
    0x00, 0x02, /* bcdUSB: 2.00 */
    0x00,       /* bDeviceClass: 0 = defined at Interface level */
    0x00,       /* bDeviceSubClass */
    0x00,       /* bDeviceProtocol */
    64,         /* bMaxPacketSize0 */
    (USB_VID & 0xFF), (USB_VID >> 8), /* idVendor */
    (USB_PID & 0xFF), (USB_PID >> 8), /* idProduct */
    0x00, 0x01, /* bcdDevice: 1.00 */
    0x01,       /* iManufacturer */
    0x02,       /* iProduct */
    0x03,       /* iSerialNumber */
    0x01        /* bNumConfigurations */
};
const uint16_t usb_device_descriptor_size = sizeof(usb_device_descriptor);

/* HID Keyboard Report Descriptor */
const uint8_t usb_hid_report_descriptor[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x06, /* Usage (Keyboard) */
    0xA1, 0x01, /* Collection (Application) */
    0x05, 0x07, /* Usage Page (Key Codes) */
    0x19, 0xE0, /* Usage Minimum (224) */
    0x29, 0xE7, /* Usage Maximum (231) */
    0x15, 0x00, /* Logical Minimum (0) */
    0x25, 0x01, /* Logical Maximum (1) */
    0x75, 0x01, /* Report Size (1) */
    0x95, 0x08, /* Report Count (8) */
    0x81, 0x02, /* Input (Data, Variable, Absolute) - Modifier bytes */
    0x95, 0x01, /* Report Count (1) */
    0x75, 0x08, /* Report Size (8) */
    0x81, 0x01, /* Input (Constant) - Reserved byte */
    0x95, 0x05, /* Report Count (5) */
    0x75, 0x01, /* Report Size (1) */
    0x05, 0x08, /* Usage Page (LEDs) */
    0x19, 0x01, /* Usage Minimum (1) */
    0x29, 0x05, /* Usage Maximum (5) */
    0x91, 0x02, /* Output (Data, Variable, Absolute) - LED report */
    0x95, 0x01, /* Report Count (1) */
    0x75, 0x03, /* Report Size (3) */
    0x91, 0x01, /* Output (Constant) - LED report padding */
    0x95, 0x06, /* Report Count (6) */
    0x75, 0x08, /* Report Size (8) */
    0x15, 0x00, /* Logical Minimum (0) */
    0x25, 0x65, /* Logical Maximum (101) */
    0x05, 0x07, /* Usage Page (Key Codes) */
    0x19, 0x00, /* Usage Minimum (0) */
    0x29, 0x65, /* Usage Maximum (101) */
    0x81, 0x00, /* Input (Data, Array) - Key arrays (6 bytes) */
    0xC0        /* End Collection */
};
const uint16_t usb_hid_report_descriptor_size = sizeof(usb_hid_report_descriptor);

/* USB Composite Configuration Descriptor (HID + MSC) */

/* HID Only Descriptor */
const uint8_t usb_config_descriptor_hid[] = {
    9, 0x02, 34, 0, 1, 0x01, 0x00, 0xA0, 0x32,
    9, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    9, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, sizeof(usb_hid_report_descriptor), 0x00,
    7, 0x05, 0x81, 0x03, 0x08, 0x00, 0x01  /* bInterval = 1 ms (fast polling) */
};
const uint16_t usb_config_descriptor_hid_size = sizeof(usb_config_descriptor_hid);

/* MSC Only Descriptor */
const uint8_t usb_config_descriptor_msc[] = {
    9, 0x02, 32, 0, 1, 0x01, 0x00, 0xA0, 0x32,
    9, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
    7, 0x05, 0x83, 0x02, 64, 0x00, 0x00,
    7, 0x05, 0x02, 0x02, 64, 0x00, 0x00
};
const uint16_t usb_config_descriptor_msc_size = sizeof(usb_config_descriptor_msc);

/* Composite Descriptor (HID + MSC) */
const uint8_t usb_config_descriptor_comp[] = {
    9, 0x02, 57, 0, 2, 0x01, 0x00, 0xA0, 0x32,
    9, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    9, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, sizeof(usb_hid_report_descriptor), 0x00,
    7, 0x05, 0x81, 0x03, 0x08, 0x00, 0x01,  /* bInterval = 1 ms (fast polling) */
    9, 0x04, 0x01, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
    7, 0x05, 0x83, 0x02, 64, 0x00, 0x00,
    7, 0x05, 0x02, 0x02, 64, 0x00, 0x00
};
const uint16_t usb_config_descriptor_comp_size = sizeof(usb_config_descriptor_comp);

const uint8_t *usb_config_descriptor = usb_config_descriptor_hid;
uint16_t usb_config_descriptor_size = sizeof(usb_config_descriptor_hid);

const uint8_t usb_str_lang_descriptor[] = {
    4,
    0x03,
    0x09, 0x04 /* English (US) */
};

/* Project-neutral strings: no third-party vendor or product names. The arrays
 * are zero-filled past the initialised bytes. */
uint8_t usb_str_manufacturer_descriptor[66] = {
    2 + 10 * 2,
    0x03,
    'R', 0, 'u', 0, 'b', 0, 'b', 0, 'e', 0, 'r', 0, 'l', 0, 'e', 0, 's', 0, 's', 0
};

uint8_t usb_str_product_descriptor[66] = {
    2 + 17 * 2,
    0x03,
    'R', 0, 'u', 0, 'b', 0, 'b', 0, 'e', 0, 'r', 0, 'l', 0, 'e', 0, 's', 0, 's', 0, ' ', 0,
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0
};

/* Modified at runtime to bypass OS caching */
uint8_t usb_str_serial_descriptor[26] = {
    2 + 6 * 2,
    0x03,
    '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '1', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};