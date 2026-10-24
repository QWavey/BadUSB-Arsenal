#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include <stdint.h>

#define USB_VID  0x03EB // Default VID (as documented in README; override with vid= in CONFIG.TXT)
#define USB_PID  0x2402 // Default PID (as documented in README; override with pid= in CONFIG.TXT)

extern uint8_t usb_device_descriptor[];
extern const uint16_t usb_device_descriptor_size;

extern const uint8_t* usb_config_descriptor;
extern const uint8_t usb_config_descriptor_hid[];
extern const uint8_t usb_config_descriptor_msc[];
extern const uint8_t usb_config_descriptor_comp[];
extern uint16_t usb_config_descriptor_size;
extern const uint16_t usb_config_descriptor_hid_size;
extern const uint16_t usb_config_descriptor_msc_size;
extern const uint16_t usb_config_descriptor_comp_size;

extern const uint8_t usb_hid_report_descriptor[];
extern const uint16_t usb_hid_report_descriptor_size;

extern const uint8_t usb_str_lang_descriptor[4];
extern uint8_t usb_str_manufacturer_descriptor[66]; // max 32 chars + header
extern uint8_t usb_str_product_descriptor[66];
extern uint8_t usb_str_serial_descriptor[26];

extern uint8_t usb_custom_serial_set;

void usb_set_string_descriptor(uint8_t *desc, const char *str, uint8_t max_chars);

#endif /* USB_DESCRIPTORS_H_ */

#define HID_IN_REPORT_SIZE 9
