/**
 * @file usb_hid.c
 * @brief USB HID implementation for AT32UC3B1
 *
 * This file implements the USB device stack for the AVR32 UC3B1's built-in
 * USB Device controller (USBB peripheral). It handles:
 *
 *   - USB hardware initialization
 *   - Control endpoint (EP0) state machine
 *   - Standard USB requests (GET_DESCRIPTOR, SET_CONFIGURATION, etc.)
 *   - HID class requests (GET_REPORT, SET_REPORT, GET_IDLE, SET_IDLE)
 *   - Interrupt IN endpoint for sending HID reports to host
 *   - Interrupt OUT endpoint for receiving HID reports from host
 *
 * Register names follow avr32/uc3b0256.h / uc3b1256.h naming conventions.
 */

#include "usb_hid.h"
#include "usb_descriptors.h"
#include "usb_msc.h"
#include <avr32/io.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

void error_blink(int code);   /* diagnose.c: blinks `code` on the red LED, never returns */

/* Bounded wait for an endpoint's CFGOK flag.  A timeout means the endpoint's
 * FIFO could not be allocated, so show `code` on the red LED instead of spinning
 * forever.  (Codes 5 to 8 are EP0 to EP3.) */
#define USB_CFG_WAIT_LIMIT 2000000u
static void usb_wait_cfgok(const volatile uint32_t *uesta, uint32_t mask, int code)
{
    uint32_t t = USB_CFG_WAIT_LIMIT;
    while (!(*uesta & mask)) {
        if (--t == 0) error_blink(code);
    }
}

extern uint16_t current_vid;
extern uint16_t current_pid;

#ifndef AVR32_PM_HSBMASK_USBB_OFFSET
#define AVR32_PM_HSBMASK_USBB_OFFSET  (AVR32_USBB_CLK_HSB - 32)
#endif
#ifndef AVR32_PM_PBBMASK_USBB_OFFSET
#define AVR32_PM_PBBMASK_USBB_OFFSET  (AVR32_USBB_CLK_PBB - 96)
#endif

/* =========================================================================
 * Internal constants
 * ======================================================================= */

/* Endpoint numbers */
#define EP_CONTROL   0
#define EP_HID_IN    1
#define EP_HID_OUT   1   /* Same physical EP number, opposite direction */

/* Control request bmRequestType fields */
#define REQ_DIR_HOST_TO_DEV   0x00
#define REQ_DIR_DEV_TO_HOST   0x80
#define REQ_TYPE_STANDARD     0x00
#define REQ_TYPE_CLASS        0x20
#define REQ_TYPE_VENDOR       0x40
#define REQ_RCPT_DEVICE       0x00
#define REQ_RCPT_INTERFACE    0x01
#define REQ_RCPT_ENDPOINT     0x02

/* Standard request codes */
#define GET_STATUS          0x00
#define CLEAR_FEATURE       0x01
#define SET_FEATURE         0x03
#define SET_ADDRESS         0x05
#define GET_DESCRIPTOR      0x06
#define SET_DESCRIPTOR      0x07
#define GET_CONFIGURATION   0x08
#define SET_CONFIGURATION   0x09
#define GET_INTERFACE       0x0A
#define SET_INTERFACE       0x0B
#define SYNCH_FRAME         0x0C

/* HID class request codes */
#define HID_GET_REPORT      0x01
#define HID_GET_IDLE        0x02
#define HID_GET_PROTOCOL    0x03
#define HID_SET_REPORT      0x09
#define HID_SET_IDLE        0x0A
#define HID_SET_PROTOCOL    0x0B

/* Descriptor types */
#define DESC_DEVICE          0x01
#define DESC_CONFIGURATION   0x02
#define DESC_STRING          0x03
#define DESC_INTERFACE       0x04
#define DESC_ENDPOINT        0x05
#define DESC_HID             0x21
#define DESC_HID_REPORT      0x22

/* Device states */
typedef enum {
    DEVICE_STATE_UNATTACHED   = 0,
    DEVICE_STATE_DEFAULT      = 1,
    DEVICE_STATE_ADDRESSED    = 2,
    DEVICE_STATE_CONFIGURED   = 3,
    DEVICE_STATE_SUSPENDED    = 4,
} device_state_t;

/* =========================================================================
 * Internal state
 * ======================================================================= */
static device_state_t         s_state             = DEVICE_STATE_UNATTACHED;
static uint8_t                s_address           = 0;
static uint8_t                s_configuration     = 0;
static uint8_t                s_idle_rate         = 0;
static usb_event_callback_t   s_callbacks[5]      = { NULL };
static usb_hid_out_callback_t s_out_callback      = NULL;
static uint8_t                s_ctrl_buf[64];

/* Storage "budget" governing how much mass-storage work usb_device_task() is
 * allowed to do, so the keyboard always keeps priority:
 *    < 0  → unlimited (idle windows: let the SD mount as fast as it likes)
 *    == 0 → none      (mid-keystroke: storage must not block a press/release)
 *    > 0  → at most this many usb_msc_task() steps, decremented each step
 *           (during typing: a trickle of one tiny step between keystrokes so
 *            the loop never stalls on a burst of SD sector reads). */
static volatile int           s_msc_budget        = -1;

/* =========================================================================
 * USBB Peripheral access helpers
 * ======================================================================= */

/* Read USBB register */
#define USBB_RD(reg)           (AVR32_USBB.reg)
#define USBB_WR(reg, val)      (AVR32_USBB.reg = (val))
#define USBB_SET(reg, mask)    (AVR32_USBB.reg |=  (mask))
#define USBB_CLR(reg, mask)    (AVR32_USBB.reg &= ~(mask))

/* Endpoint FIFO base address */
#define USBB_FIFO_BASE         ((volatile uint8_t *)AVR32_USBB_SLAVE)
#define USBB_EP_FIFO(ep)       (USBB_FIFO_BASE + ((ep) * 0x10000))

/* Write a byte to EP FIFO */
static inline void ep_fifo_write_byte(uint8_t ep, uint8_t data) {
    USBB_EP_FIFO(ep)[0] = data;
}

/* Read a byte from EP FIFO */
static inline uint8_t ep_fifo_read_byte(uint8_t ep) {
    return USBB_EP_FIFO(ep)[0];
}

/* =========================================================================
 * USB setup packet structure
 * ======================================================================= */
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

#define USB_POLL_TIMEOUT 50000000u

/* =========================================================================
 * Internal function prototypes
 * ======================================================================= */
static void usbb_init(void);
static void usbb_attach(void);
static void ep0_handle_setup(void);
void ep0_send(const uint8_t *data, uint16_t length, uint16_t requested_length);
static void ep0_stall(void);
void ep0_ack_status_in(void);
static const uint8_t *get_string_descriptor(uint8_t index, uint16_t *out_len);
static void fire_callback(usb_event_t event);

/* =========================================================================
 * Public API implementation
 * ======================================================================= */



void usb_device_init(void)
{
    s_out_callback  = NULL;
    s_state         = DEVICE_STATE_UNATTACHED;
    s_address       = 0;
    s_configuration = 0;
    s_idle_rate     = 0;

    /*
     * Force a clean USB disconnect before re-initialising.
     *
     * After dfu-programmer 'launch --no-reset', the USBB peripheral is still
     * active as a DFU device with D+ pulled HIGH.  The host never sees a
     * disconnect event, so it tries to reuse the stale DFU enumeration and
     * fails immediately.
     *
     * Fix: assert DETACH first (pulls D+ LOW = disconnect), disable USBB
     * entirely, wait ~20 ms for the host to register the disconnect, then
     * call usbb_init() which re-enables everything from scratch.
     */

    /* Step 1 – assert detach while USBB is still alive */
    AVR32_USBB.udcon |= AVR32_USBB_UDCON_DETACH_MASK;

    /* Step 2 – freeze the clock then disable the controller */
    /* Step 3 – Freeze clock and disable */
    AVR32_USBB.usbcon |= AVR32_USBB_USBCON_FRZCLK_MASK;
    AVR32_USBB.usbcon &= ~AVR32_USBB_USBCON_USBE_MASK;

    /* Minimum 5ms settle: host hub needs ~2-5ms to detect D+ going low.
     * Without this, re-attach fires before the host registers the
     * disconnect, causing a botched re-enumeration and costly retry. */
    for (volatile int _i = 0; _i < 240000; _i++);

    /* Step 4 – full hardware re-init */
    usbb_init();
}

void usb_device_enable(void)
{
    /* Randomize all 6 serial number characters on every enable.
     * Windows caches USB mass-storage volume state by serial number.
     * A unique serial each cycle forces Windows to treat this as a
     * new device every time, skipping the stale-volume chkdsk delay.
     * This is the same trick the real Hak5 Ducky uses. */
    if (!usb_custom_serial_set) {
        static uint32_t serial_seed = 0xDEADBEEF;
        serial_seed ^= (serial_seed << 13);
        serial_seed ^= (serial_seed >> 17);
        serial_seed ^= (serial_seed << 5);
        const char hex[] = "0123456789ABCDEF";
        usb_str_serial_descriptor[2]  = hex[(serial_seed >>  0) & 0xF];
        usb_str_serial_descriptor[4]  = hex[(serial_seed >>  4) & 0xF];
        usb_str_serial_descriptor[6]  = hex[(serial_seed >>  8) & 0xF];
        usb_str_serial_descriptor[8]  = hex[(serial_seed >> 12) & 0xF];
        usb_str_serial_descriptor[10] = hex[(serial_seed >> 16) & 0xF];
        usb_str_serial_descriptor[12] = hex[(serial_seed >> 20) & 0xF];
    }

    usbb_attach();
}

void usb_device_disable(void)
{
    AVR32_USBB.udcon |= AVR32_USBB_UDCON_DETACH_MASK;
}

void usb_device_register_callback(usb_event_t event, usb_event_callback_t callback)
{
    if ((unsigned)event < 5) {
        s_callbacks[event] = callback;
    }
}

void usb_hid_register_out_callback(usb_hid_out_callback_t callback)
{
    s_out_callback = callback;
}

bool usb_hid_in_endpoint_ready(void)
{
    /* Check if IN endpoint FIFO is free (TXINI flag set means ready) */
    return !!(AVR32_USBB.uesta1 & AVR32_USBB_UESTA1_TXINI_MASK);
}

bool usb_hid_in_all_sent(void)
{
    /* NBUSYBK = number of banks still holding data the host hasn't read yet.
     * Zero means every queued report has actually been collected by the host.
     * Unlike TXINI (which only says "a bank is free" and can be true while a
     * just-queued report still sits unread in the OTHER bank of a double-
     * buffered endpoint), this is a true "delivered" signal — so we never send
     * a release until the host has really taken the press, which stops Windows
     * from coalescing the pair and dropping the keystroke. */
    return ((AVR32_USBB.uesta1 & AVR32_USBB_UESTA1_NBUSYBK_MASK)
            >> AVR32_USBB_UESTA1_NBUSYBK_OFFSET) == 0;
}

void usb_msc_set_suppressed(bool suppressed)
{
    /* Back-compat: suppress = no storage; un-suppress = unlimited. */
    s_msc_budget = suppressed ? 0 : -1;
}

void usb_msc_set_budget(int budget)
{
    s_msc_budget = budget;
}

bool usb_hid_send_report(const uint8_t *data, uint8_t length)
{
    if (!usb_hid_in_endpoint_ready()) {
        return false;
    }

    /* Write report bytes to EP1 IN FIFO */
    volatile uint8_t *fifo8 = (volatile uint8_t *)USBB_EP_FIFO(EP_HID_IN);
    for (uint8_t i = 0; i < length; i++) {
        fifo8[i] = data[i];
    }

    /* Clear TXINI to send the data */
    AVR32_USBB.uesta1clr = AVR32_USBB_UESTA1CLR_TXINIC_MASK;
    /* Set FIFOCON to release the bank */
    AVR32_USBB.uecon1clr = AVR32_USBB_UECON1CLR_FIFOCONC_MASK;

    return true;
}

void usb_device_task(void)
{
    uint32_t udint = AVR32_USBB.udint;

    /* ---- USB Reset ---- */
    if (AVR32_USBB.udint & AVR32_USBB_UDINT_EORST_MASK) {
        AVR32_USBB.udintclr = AVR32_USBB_UDINTCLR_EORSTC_MASK;

        extern uint8_t current_attackmode;
        uint32_t ep_num = (1 << EP_CONTROL) | (1 << EP_HID_IN);
        if (current_attackmode == 2 || current_attackmode == 3) {
            ep_num |= (1 << 2) | (1 << 3);
        }

        /* Reset + allocate endpoints.  Bare writes are fine here —
         * configure_hid_endpoints() re-configures everything after. */
        AVR32_USBB.uerst = (ep_num << 16);
        AVR32_USBB.uerst = ep_num;

        /* Configure EP0 (Control, 64 bytes) */
        AVR32_USBB.uecfg0 = (0 << AVR32_USBB_UECFG0_EPTYPE_OFFSET)  /* Control */
                          | (0 << AVR32_USBB_UECFG0_EPDIR_OFFSET)   /* OUT (don't care for control) */
                          | (3 << AVR32_USBB_UECFG0_EPSIZE_OFFSET)  /* 64 bytes */
                          | (0 << AVR32_USBB_UECFG0_EPBK_OFFSET)    /* Single bank */
                          | AVR32_USBB_UECFG0_ALLOC_MASK;           /* Allocate memory */

        /* Wait for allocation to complete */
        usb_wait_cfgok(&AVR32_USBB.uesta0, AVR32_USBB_UESTA0_CFGOK_MASK, 5);

        fire_callback(USB_EVENT_RESET);
    }

    /* ---- EP0 Control Pipe (handle before suspend/resume so a late
     *      SUSP flag does not swallow a SETUP during enumeration) ---- */
    if (AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_RXSTPI_MASK) {
        ep0_handle_setup();
    }

    /* ---- Suspend ---- */
    if (udint & AVR32_USBB_UDINT_SUSP_MASK) {
        AVR32_USBB.udintclr = AVR32_USBB_UDINTCLR_SUSPC_MASK;
        s_state = DEVICE_STATE_SUSPENDED;
        fire_callback(USB_EVENT_SUSPEND);
    }

    /* ---- Resume / Wakeup ---- */
    if (udint & AVR32_USBB_UDINT_WAKEUP_MASK) {
        AVR32_USBB.udintclr = AVR32_USBB_UDINTCLR_WAKEUPC_MASK;
        if (s_state == DEVICE_STATE_SUSPENDED) {
            s_state = (s_configuration > 0) ? DEVICE_STATE_CONFIGURED
                                             : DEVICE_STATE_DEFAULT;
        }
        fire_callback(USB_EVENT_RESUME);
    }
    
    extern uint8_t current_attackmode;
    if (s_state == DEVICE_STATE_CONFIGURED && (current_attackmode == 2 || current_attackmode == 3)
        && s_msc_budget != 0) {
        /* Storage is rate-limited by the budget so the keyboard always keeps
         * priority.  During typing the budget is a trickle (one step between
         * keystrokes), so a burst of SD sector reads can never stall the loop
         * mid-string (the "half-second freeze" + dropped characters).  During
         * idle windows the budget is unlimited so the mount finishes quickly. */
        usb_msc_task();
        if (s_msc_budget > 0) s_msc_budget--;
    }
}

/* =========================================================================
 * USBB hardware initialization
 * ======================================================================= */

static void usbb_init(void)
{
    /* ---- 1. Generic Clock 3 -> USBB (must be 48 MHz for USB FS) ---- */
    /* Select PLL0 (48 MHz) as source for GCLK3, enable it */
    AVR32_PM.gcctrl[3] = (1u << AVR32_PM_GCCTRL_PLLSEL_OFFSET)  /* PLL source */
                       | (0u << AVR32_PM_GCCTRL_OSCSEL_OFFSET)  /* PLL0 */
                       | AVR32_PM_GCCTRL_CEN_MASK;              /* Enable */

    /* ---- 2. Enable USBB bus clocks via Power Manager ---- */
    AVR32_PM.hsbmask |= (1u << AVR32_PM_HSBMASK_USBB_OFFSET);
    AVR32_PM.pbbmask |= (1u << AVR32_PM_PBBMASK_USBB_OFFSET);

    /* ---- 3. Enable USBB in USB Device mode ---- */
    /*
     * USBE=1  : Enable USB controller
     * UIMOD=1 : USB Device mode (UIMOD=0 would be OTG/Host mode)
     * OTGPADE=1: Enable OTG pad (D+/D- pull-up/down resistors)
     * FRZCLK=1 : Freeze the 48 MHz clock while we configure
     */
    AVR32_USBB.usbcon = AVR32_USBB_USBCON_USBE_MASK
                      | AVR32_USBB_USBCON_UIMOD_MASK   /* Device mode */
                      | AVR32_USBB_USBCON_OTGPADE_MASK
                      | AVR32_USBB_USBCON_FRZCLK_MASK; /* Freeze during setup */

    /* ---- 4. Configure EP0 (Control, 64 bytes) while clock is frozen ---- */
    AVR32_USBB.uecfg0 = AVR32_USBB_UECFG0_EPTYPE_CONTROL
                       | AVR32_USBB_UECFG0_EPSIZE_64
                       | AVR32_USBB_UECFG0_ALLOC_MASK;
    AVR32_USBB.uerst  = AVR32_USBB_UERST_EPRST0_MASK; /* Reset EP0 toggle */
    AVR32_USBB.uerst  = 0;
    AVR32_USBB.uecon0set = AVR32_USBB_UECON0SET_RSTDTS_MASK; /* Reset data toggle */

    /* ---- 5. Unfreeze the 48 MHz USB clock ---- */
    AVR32_USBB.usbcon &= ~AVR32_USBB_USBCON_FRZCLK_MASK;

    /* ---- 6. Enable EP0 SETUP received interrupt ---- */
    AVR32_USBB.uecon0set = AVR32_USBB_UECON0SET_RXSTPES_MASK;
}

static void usbb_attach(void)
{
    /* Pull D+ high to signal device presence to host */
    AVR32_USBB.udcon &= ~AVR32_USBB_UDCON_DETACH_MASK;
}

/* =========================================================================
 * Configure data endpoints after SET_CONFIGURATION
 * ======================================================================= */
static void configure_hid_endpoints(void)
{
    extern uint8_t current_attackmode;

    /* ---- EP1 IN (Interrupt IN, 64 bytes) - HID ---- */
    if (current_attackmode != 2) { // Mode 1 or 3
        AVR32_USBB.uerst |= AVR32_USBB_UERST_EPRST1_MASK;
        AVR32_USBB.uerst &= ~AVR32_USBB_UERST_EPRST1_MASK;

        AVR32_USBB.uecfg1 = (3 << AVR32_USBB_UECFG1_EPTYPE_OFFSET)  /* Interrupt */
                           | (1 << AVR32_USBB_UECFG1_EPDIR_OFFSET)   /* IN */
                           | (3 << AVR32_USBB_UECFG1_EPSIZE_OFFSET)  /* 64 bytes */
                           | (1 << AVR32_USBB_UECFG1_EPBK_OFFSET)    /* DOUBLE bank */
                           | AVR32_USBB_UECFG1_ALLOC_MASK;
        /* DOUBLE bank.  (A single-bank experiment — matching Arduino/wifiduck —
         * corrupted the first line on this USBB: the FIFO got written while the
         * lone bank was still busy, injecting stray keys.  The double bank + the
         * NBUSYBK drain wait in hid_send_one() is the reliable path here.) */
        

        usb_wait_cfgok(&AVR32_USBB.uesta1, AVR32_USBB_UESTA1_CFGOK_MASK, 6);
    }

    if (current_attackmode == 2 || current_attackmode == 3) {
        /* ---- EP2 OUT (Bulk OUT, 64 bytes) - MSC ---- */
        AVR32_USBB.uerst |= AVR32_USBB_UERST_EPRST2_MASK;
        AVR32_USBB.uerst &= ~AVR32_USBB_UERST_EPRST2_MASK;

        AVR32_USBB.uecfg2 = (2 << AVR32_USBB_UECFG2_EPTYPE_OFFSET)  /* Bulk */
                           | (0 << AVR32_USBB_UECFG2_EPDIR_OFFSET)   /* OUT */
                           | (3 << AVR32_USBB_UECFG2_EPSIZE_OFFSET)  /* 64 bytes */
                           | (0 << AVR32_USBB_UECFG2_EPBK_OFFSET)    /* Single bank */
                           | AVR32_USBB_UECFG2_ALLOC_MASK;
        

        usb_wait_cfgok(&AVR32_USBB.uesta2, AVR32_USBB_UESTA2_CFGOK_MASK, 7);

        /* ---- EP3 IN (Bulk IN, 64 bytes) - MSC ---- */
        AVR32_USBB.uerst |= AVR32_USBB_UERST_EPRST3_MASK;
        AVR32_USBB.uerst &= ~AVR32_USBB_UERST_EPRST3_MASK;

        AVR32_USBB.uecfg3 = (2 << AVR32_USBB_UECFG3_EPTYPE_OFFSET)  /* Bulk */
                           | (1 << AVR32_USBB_UECFG3_EPDIR_OFFSET)   /* IN */
                           | (3 << AVR32_USBB_UECFG3_EPSIZE_OFFSET)  /* 64 bytes */
                           | (0 << AVR32_USBB_UECFG3_EPBK_OFFSET)    /* Single bank */
                           | AVR32_USBB_UECFG3_ALLOC_MASK;
        

        usb_wait_cfgok(&AVR32_USBB.uesta3, AVR32_USBB_UESTA3_CFGOK_MASK, 8);

        usb_msc_init();
    }
}

/* =========================================================================
 * EP0 Control pipe handling
 * ======================================================================= */

/* Write data to EP FIFO using byte accesses */
static void fifo_write_bytes(uint8_t ep, const uint8_t *data, uint16_t length)
{
    volatile uint8_t *fifo8 = (volatile uint8_t *)USBB_EP_FIFO(ep);
    for (uint16_t i = 0; i < length; i++) {
        fifo8[i] = data[i];
    }
}

/* 
 * Send a block of data on EP0 IN. 
 * Handles multi-packet transfers and ZLP if needed.
 */
void ep0_send(const uint8_t *data, uint16_t length, uint16_t requested_length)
{
    if (length > requested_length) {
        length = requested_length;
    }

    uint16_t sent = 0;
    bool need_zlp = (length == 0) || ((length % 64) == 0);

    while (sent < length) {
        /* Wait for TXINI (IN bank free) with timeout */
        uint32_t timeout = USB_POLL_TIMEOUT;
        while (!(AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_TXINI_MASK)) {
            if (AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_RXSTPI_MASK) return;
            if (--timeout == 0) { ep0_stall(); return; }
        }

        uint16_t chunk = length - sent;
        if (chunk > 64) chunk = 64;

        fifo_write_bytes(EP_CONTROL, &data[sent], chunk);
        sent += chunk;

        /* Trigger IN packet (FIFOCON not used for EP0) */
        AVR32_USBB.uesta0clr = AVR32_USBB_UESTA0CLR_TXINIC_MASK;
    }

    if (need_zlp && requested_length > length) {
        /* Send ZLP IN */
        AVR32_USBB.uesta0clr = AVR32_USBB_UESTA0CLR_TXINIC_MASK;
        uint32_t timeout = USB_POLL_TIMEOUT;
        while (!(AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_TXINI_MASK)) {
            if (AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_RXSTPI_MASK) return;
            if (--timeout == 0) return;
        }
    }

    /* Wait for host to send ZLP OUT (status stage) */
    uint32_t timeout = USB_POLL_TIMEOUT;
    while (!(AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_RXOUTI_MASK)) {
        if (AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_RXSTPI_MASK) return;
        if (--timeout == 0) return; /* timeout: host didn't ACK, drop silently */
    }
    AVR32_USBB.uesta0clr = AVR32_USBB_UESTA0CLR_RXOUTIC_MASK;
}

static void ep0_handle_setup(void)
{
    uint8_t raw[8];
    volatile uint8_t *fifo8 = (volatile uint8_t *)USBB_EP_FIFO(EP_CONTROL);

    /* Read 8 bytes directly */
    for (int i = 0; i < 8; i++) {
        raw[i] = fifo8[i];
    }

    AVR32_USBB.uesta0clr = AVR32_USBB_UESTA0CLR_RXSTPIC_MASK;

    uint8_t  req_type = raw[0];
    uint8_t  req      = raw[1];
    uint16_t wValue   = (uint16_t)raw[2] | ((uint16_t)raw[3] << 8);
    uint16_t wIndex   = (uint16_t)raw[4] | ((uint16_t)raw[5] << 8);
    uint16_t wLength  = (uint16_t)raw[6] | ((uint16_t)raw[7] << 8);

    /* -------------------------------------------------------------------
     * Standard Device Requests
     * ----------------------------------------------------------------- */
    if ((req_type & 0x60) == REQ_TYPE_STANDARD) {

        if (req == GET_DESCRIPTOR) {
            uint8_t  desc_type  = (wValue >> 8) & 0xFF;
            uint8_t  desc_index = (wValue     ) & 0xFF;
            const uint8_t *desc_ptr = NULL;
            uint16_t       desc_len = 0;

            switch (desc_type) {
                case DESC_DEVICE:
                    /* Use the device descriptor as-is: apply_attackmode()
                     * already wrote the XOR'd VID/PID into it.  Do NOT
                     * overwrite with the raw current_vid/current_pid. */
                    desc_ptr = usb_device_descriptor;
                    desc_len = usb_device_descriptor_size;
                    break;

                case DESC_CONFIGURATION:
                    desc_ptr = usb_config_descriptor;
                    desc_len = usb_config_descriptor_size;
                    fire_callback(USB_EVENT_CONFIG_REQUESTED);
                    break;

                case DESC_STRING:
                    if (desc_index == 0xEE) {
                        fire_callback(USB_EVENT_CONFIG_REQUESTED);
                    }
                    desc_ptr = get_string_descriptor(desc_index, &desc_len);
                    break;

                case DESC_HID:
                    /* HID descriptor is embedded in config descriptor at offset 18 */
                    desc_ptr = usb_config_descriptor + 18;
                    desc_len = 9;
                    break;

                case DESC_HID_REPORT:
                    desc_ptr = usb_hid_report_descriptor;
                    desc_len = usb_hid_report_descriptor_size;
                    break;

                default:
                    ep0_stall();
                    return;
            }

            if (desc_ptr) {
                ep0_send(desc_ptr, desc_len, wLength);
            } else {
                ep0_stall();
            }
            return;
        }

        if (req == SET_ADDRESS) {
            s_address = (uint8_t)(wValue & 0x7F);
            /* Send zero-length IN status packet */
            ep0_ack_status_in();
            /*
             * Per USB 2.0 spec §9.4.6: the device must not apply the new
             * address until after the status stage of this request completes.
             * ep0_ack_status_in() clears TXINI which hands the (empty) bank
             * to the SIE.  We must wait for TXINI to be SET again by hardware,
             * which happens only after the host has received the ZLP and the
             * bank is free — i.e., the status stage is truly done.
             */
            {
                uint32_t timeout = USB_POLL_TIMEOUT;
                while (!(AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_TXINI_MASK)) {
                    if (--timeout == 0) return;
                }
            }
            AVR32_USBB.udcon = (AVR32_USBB.udcon & ~AVR32_USBB_UDCON_UADD_MASK)
                             | ((uint32_t)s_address << AVR32_USBB_UDCON_UADD_OFFSET)
                             | AVR32_USBB_UDCON_ADDEN_MASK;
            s_state = DEVICE_STATE_ADDRESSED;
            return;
        }

        if (req == SET_CONFIGURATION) {
            s_configuration = (uint8_t)(wValue & 0xFF);
            if (s_configuration > 0) {
                configure_hid_endpoints();
                s_state = DEVICE_STATE_CONFIGURED;
                fire_callback(USB_EVENT_ENUMERATED);
                fire_callback(USB_EVENT_CONFIG_REQUESTED);
            }
            ep0_ack_status_in();
            return;
        }

        if (req == GET_CONFIGURATION) {
            ep0_send(&s_configuration, 1, wLength);
            return;
        }

        if (req == GET_STATUS) {
            /*
             * bmAttributes = 0xC0 => D6=1 (self-powered), D5=0 (no remote wakeup)
             * GET_STATUS for device: bit 0 = self-powered, bit 1 = remote wakeup
             */
            uint8_t status[2] = { 0x01, 0x00 }; /* Self-powered */
            ep0_send(status, 2, wLength);
            return;
        }

        if (req == SET_FEATURE || req == CLEAR_FEATURE) {
            if (req == CLEAR_FEATURE && wValue == 0) { // ENDPOINT_HALT
                uint8_t ep = wIndex & 0x7F;
                if (ep == 1) {
                    AVR32_USBB.uecon1clr = AVR32_USBB_UECON1CLR_STALLRQC_MASK;
                    AVR32_USBB.uerst |= AVR32_USBB_UERST_EPRST1_MASK;
                    AVR32_USBB.uerst &= ~AVR32_USBB_UERST_EPRST1_MASK;
                    AVR32_USBB.uecon1set = AVR32_USBB_UECON1SET_RSTDTS_MASK;
                } else if (ep == 2) {
                    AVR32_USBB.uecon2clr = AVR32_USBB_UECON2CLR_STALLRQC_MASK;
                    AVR32_USBB.uerst |= AVR32_USBB_UERST_EPRST2_MASK;
                    AVR32_USBB.uerst &= ~AVR32_USBB_UERST_EPRST2_MASK;
                    AVR32_USBB.uecon2set = AVR32_USBB_UECON2SET_RSTDTS_MASK;
                } else if (ep == 3) {
                    AVR32_USBB.uecon3clr = AVR32_USBB_UECON3CLR_STALLRQC_MASK;
                    AVR32_USBB.uerst |= AVR32_USBB_UERST_EPRST3_MASK;
                    AVR32_USBB.uerst &= ~AVR32_USBB_UERST_EPRST3_MASK;
                    AVR32_USBB.uecon3set = AVR32_USBB_UECON3SET_RSTDTS_MASK;
                }
                // When host clears stall on MSC endpoints, BOT spec says we reset data toggles
                // and we can continue.
            }
            ep0_ack_status_in();
            return;
        }
    }

    /* -------------------------------------------------------------------
     * HID Class Requests
     * ----------------------------------------------------------------- */
    if ((req_type & 0x60) == REQ_TYPE_CLASS) {
        extern uint8_t current_attackmode;
        if ((current_attackmode == 2 || current_attackmode == 3) && usb_msc_handle_setup(req_type, req, wValue, wIndex, wLength)) return;

        if (req == HID_SET_IDLE) {
            s_idle_rate = (uint8_t)((wValue >> 8) & 0xFF);
            ep0_ack_status_in();
            return;
        }

        if (req == HID_GET_IDLE) {
            ep0_send(&s_idle_rate, 1, wLength);
            return;
        }

        if (req == HID_SET_PROTOCOL || req == HID_GET_PROTOCOL) {
            /* We only support Report Protocol (not Boot Protocol) */
            uint8_t protocol = 1; /* Report protocol */
            if (req == HID_GET_PROTOCOL) {
                ep0_send(&protocol, 1, wLength);
            } else {
                ep0_ack_status_in();
            }
            return;
        }

        if (req == HID_GET_REPORT) {
            /* Return an empty IN report */
            memset(s_ctrl_buf, 0, sizeof(s_ctrl_buf));
            s_ctrl_buf[0] = (uint8_t)(wValue & 0xFF); /* Report ID */
            ep0_send(s_ctrl_buf, (wLength < HID_IN_REPORT_SIZE) ? wLength : HID_IN_REPORT_SIZE, wLength);
            return;
        }

        if (req == HID_SET_REPORT) {
            /* Wait for OUT data — BOUNDED.  The previous unbounded spin could
             * lock the whole device forever if the host's OUT stage never
             * arrived (e.g. an aborted control transfer), which presented as
             * "after some host interaction the device stops responding". */
            uint32_t timeout = USB_POLL_TIMEOUT;
            while (!(AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_RXOUTI_MASK)) {
                if (AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_RXSTPI_MASK) return;
                if (--timeout == 0) { ep0_stall(); return; }
            }
            uint8_t led_state = USBB_EP_FIFO(EP_CONTROL)[0];
            AVR32_USBB.uesta0clr = AVR32_USBB_UESTA0CLR_RXOUTIC_MASK;
            if (s_out_callback) {
                s_out_callback(&led_state, 1);
            }
            ep0_ack_status_in();
            return;
        }
    }

    /* Unknown request -> STALL */
    ep0_stall();
}

/* Timeout for polling loops: ~1M iterations covers ~20 ms at 48 MHz */

/*
 * Send a zero-length IN packet to acknowledge an OUT control request
 * (status stage for host-to-device control transfers).
 *
 * AT32UC3B1 USBB: For EP0, clearing TXINI alone triggers the ZLP IN.
 * DO NOT clear FIFOCONC — it is not used for control endpoints.
 */
void ep0_ack_status_in(void)
{
    uint32_t timeout = USB_POLL_TIMEOUT;
    while (!(AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_TXINI_MASK)) {
        if (AVR32_USBB.uesta0 & AVR32_USBB_UESTA0_RXSTPI_MASK) return;
        if (--timeout == 0) return;
    }
    /* No data written — clearing TXINI sends a ZLP IN to the host */
    AVR32_USBB.uesta0clr = AVR32_USBB_UESTA0CLR_TXINIC_MASK;
}

/* Stall EP0 to signal an unsupported request */
static void ep0_stall(void)
{
    AVR32_USBB.uecon0set = AVR32_USBB_UECON0SET_STALLRQS_MASK;
}

/* Poll EP1 OUT removed */

/* =========================================================================
 * String descriptor lookup
 * ======================================================================= */
static const uint8_t *get_string_descriptor(uint8_t index, uint16_t *out_len)
{
    switch (index) {
        case 0:
            *out_len = usb_str_lang_descriptor[0];
            return usb_str_lang_descriptor;
        case 1:
            *out_len = usb_str_manufacturer_descriptor[0];
            return usb_str_manufacturer_descriptor;
        case 2:
            *out_len = usb_str_product_descriptor[0];
            return usb_str_product_descriptor;
        case 3:
            *out_len = usb_str_serial_descriptor[0];
            return usb_str_serial_descriptor;
        default:
            *out_len = 0;
            return NULL;
    }
}

/* =========================================================================
 * Callback dispatcher
 * ======================================================================= */
static void fire_callback(usb_event_t event)
{
    if ((unsigned)event < 5 && s_callbacks[event]) {
        s_callbacks[event]();
    }
}
