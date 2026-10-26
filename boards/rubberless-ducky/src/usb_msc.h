#ifndef USB_MSC_H_
#define USB_MSC_H_

#include <stdint.h>
#include <stdbool.h>

#define MSC_EP_IN   3
#define MSC_EP_OUT  2

/* Initializes the Mass Storage class state machine */
void usb_msc_init(void);

/* Called regularly in the main loop to process MSC events */
void usb_msc_task(void);
bool usb_msc_handle_setup(uint8_t req_type, uint8_t req, uint16_t wValue, uint16_t wIndex, uint16_t wLength);

#endif /* USB_MSC_H_ */
