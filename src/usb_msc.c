/**
 * @file usb_msc.c
 * @brief Non-blocking Bulk-Only Transport (BOT) Mass-Storage Class.
 *
 * Why this is a state machine, not straight-line code
 * ---------------------------------------------------
 * The earlier version of this file processed a whole SCSI command per
 * invocation of usb_msc_task(): for SCSI_READ_10 it would issue
 * sd_read_sector() and ep3_in_xfer() for *every* requested block in one
 * call, with no-timeout spin-waits on the bulk endpoint FIFO between
 * 64-byte chunks.  That meant a single usb_msc_task() call could sink the
 * single-threaded main loop into Windows's back-to-back mount storm for
 * many seconds — and the firmware's HID typing window collapsed.
 * Symptom: "the payload only starts typing once Windows finishes mounting
 * the SD card."
 *
 * This rewrite makes usb_msc_task() a one-step-at-a-time state machine.
 * Every invocation does at most ONE small unit of work:
 *   - read one CBW into bot,
 *   - load one 512-byte sector from SD,
 *   - send one 64-byte chunk on EP3-IN,
 *   - receive one 64-byte chunk on EP2-OUT,
 *   - send the 13-byte CSW.
 * It then returns.  If the bulk endpoint bank isn't ready, the call
 * returns immediately and the state stays put for the next tick.
 *
 * Concretely each call is bounded by the slowest unit, which is the SD
 * sector read (~1–5 ms over SPI).  That's well under the HID 10 ms
 * bInterval, so the main loop has plenty of time to queue keystrokes
 * between MSC ticks — HID and STORAGE actually run concurrently.
 *
 * The external API (usb_msc_init, usb_msc_task, usb_msc_handle_setup) is
 * unchanged so the call sites in usb_hid.c and ep0_handle_setup don't move.
 */

#include "usb_msc.h"
#include "diskio.h"
#include <avr32/io.h>
#include <string.h>
#include <stdint.h>

extern uint32_t mc_total_sectors;
extern uint32_t mc_partition_sectors;

#define USBB_EP_FIFO(ep)  ((volatile uint8_t *)(AVR32_USBB_SLAVE_ADDRESS + ((ep) * 0x10000)))

/* SCSI Command OpCodes */
#define SCSI_TEST_UNIT_READY        0x00
#define SCSI_REQUEST_SENSE          0x03
#define SCSI_INQUIRY                0x12
#define SCSI_MODE_SENSE_6           0x1A
#define SCSI_START_STOP_UNIT        0x1B
#define SCSI_PREVENT_ALLOW          0x1E
#define SCSI_READ_CAPACITY_10       0x25
#define SCSI_READ_FORMAT_CAPACITIES 0x23
#define SCSI_READ_10                0x28
#define SCSI_WRITE_10               0x2A
#define SCSI_MODE_SENSE_10          0x55

#define BLOCK_SIZE 512u
#define MAX_PACKET 64u

#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u

/* Bulk-Only Transport states */
typedef enum {
    BOT_STATE_IDLE       = 0,  /* waiting for CBW on EP2-OUT  */
    BOT_STATE_TX_INLINE  = 1,  /* TX small inline buffer (INQUIRY, etc.) */
    BOT_STATE_TX_SECTOR  = 2,  /* TX successive SD sectors (READ_10) */
    BOT_STATE_RX_SECTOR  = 3,  /* RX successive SD sectors (WRITE_10) */
    BOT_STATE_CSW        = 4,  /* TX 13-byte CSW on EP3-IN */
} bot_state_t;

static struct {
    bot_state_t state;

    /* Current CBW fields (copied out of EP2 FIFO at IDLE → ...) */
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];

    /* CSW outputs */
    uint8_t  csw_status;        /* 0=pass, 1=fail */
    uint32_t data_residue;      /* bytes NOT transferred (reported in CSW) */

    /* Working buffer: either an inline response or one SD sector. */
    uint8_t  buffer[BLOCK_SIZE];
    uint16_t buffer_total;      /* valid bytes for INLINE TX */
    uint16_t buffer_offset;     /* next byte to TX or fill-offset on RX */
    uint8_t  buffer_loaded;     /* sector mode: 1=buffer holds current sector */

    /* Multi-block IO progress for READ_10 / WRITE_10 */
    uint32_t io_lba;
    uint16_t io_blocks_left;
} bot;

static const uint8_t inquiry_data[36] = {
    0x00, /* Direct-access block device */
    0x80, /* Removable */
    0x04, /* SPC-2 */
    0x02, /* Response data format */
    31,   /* Additional length */
    0x00, 0x00, 0x00,
    'R','u','b','b','e','r','l','s',
    'M','a','s','s',' ','S','t','o',
    'r','a','g','e',' ',' ',' ',' ',
    '1','.','0','0'
};

void storage_activity_mark(void); /* in main.c, currently a no-op LED hook */

/* ---------- endpoint helpers (all non-blocking) ---------- */

static inline bool ep3_in_bank_free(void) {
    /* TXINI is set by hardware when the IN bank is empty / writable. */
    return !!(AVR32_USBB.uesta3 & AVR32_USBB_UESTA3_TXINI_MASK);
}

static inline bool ep2_out_bank_ready(void) {
    /* RXOUTI is set by hardware when an OUT packet has been received. */
    return !!(AVR32_USBB.uesta2 & AVR32_USBB_UESTA2_RXOUTI_MASK);
}

static inline uint16_t ep2_out_byct(void) {
    /* Byte count of received OUT packet (0..64). */
    return (uint16_t)((AVR32_USBB.uesta2 & AVR32_USBB_UESTA2_BYCT_MASK)
                      >> AVR32_USBB_UESTA2_BYCT_OFFSET);
}

/* Write `len` bytes into the EP3-IN FIFO and arm the bank for transmission.
 * Caller must already have verified ep3_in_bank_free(). */
static void ep3_send_packet(const uint8_t *data, uint16_t len) {
    volatile uint8_t *fifo = USBB_EP_FIFO(MSC_EP_IN);
    for (uint16_t i = 0; i < len; i++) fifo[i] = data[i];
    AVR32_USBB.uesta3clr = AVR32_USBB_UESTA3CLR_TXINIC_MASK;
    AVR32_USBB.uecon3clr = AVR32_USBB_UECON3CLR_FIFOCONC_MASK;
}

/* Read up to `max` bytes out of the EP2-OUT FIFO and release the bank.
 * Returns the actual number of bytes read.  Caller must already have
 * verified ep2_out_bank_ready(). */
static uint16_t ep2_recv_packet(uint8_t *dest, uint16_t max) {
    uint16_t byct = ep2_out_byct();
    if (byct > max) byct = max;
    volatile uint8_t *fifo = USBB_EP_FIFO(MSC_EP_OUT);
    for (uint16_t i = 0; i < byct; i++) dest[i] = fifo[i];
    AVR32_USBB.uesta2clr = AVR32_USBB_UESTA2CLR_RXOUTIC_MASK;
    AVR32_USBB.uecon2clr = AVR32_USBB_UECON2CLR_FIFOCONC_MASK;
    return byct;
}

static inline void ep3_stall(void) {
    AVR32_USBB.uecon3set = AVR32_USBB_UECON3SET_STALLRQS_MASK;
}

static inline void ep2_stall(void) {
    AVR32_USBB.uecon2set = AVR32_USBB_UECON2SET_STALLRQS_MASK;
}

/* ---------- API: control transfers (class-specific on EP0) ---------- */

extern void ep0_send(const void*, uint16_t, uint16_t);
extern void ep0_ack_status_in(void);

bool usb_msc_handle_setup(uint8_t req_type, uint8_t req,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength) {
    (void)wValue; (void)wIndex;
    if (req_type == 0xA1 && req == 0xFE) { /* GET_MAX_LUN */
        uint8_t lun = 0;
        ep0_send(&lun, 1, wLength);
        return true;
    }
    if (req_type == 0x21 && req == 0xFF) { /* Bulk-Only Reset */
        bot.state = BOT_STATE_IDLE;
        ep0_ack_status_in();
        return true;
    }
    return false;
}

void usb_msc_init(void) {
    memset(&bot, 0, sizeof(bot));
    bot.state = BOT_STATE_IDLE;
}

/* ---------- inline-response setup ---------- */

/* Copy a small response into bot.buffer and transition to TX_INLINE.
 * Capped by the host-declared dCBWDataTransferLength so we don't overrun. */
static void start_inline_tx(const uint8_t *src, uint16_t len) {
    uint32_t alloc = bot.dCBWDataTransferLength;
    if (len > alloc) len = (uint16_t)alloc;
    if (len > BLOCK_SIZE) len = BLOCK_SIZE;
    memcpy(bot.buffer, src, len);
    bot.buffer_total  = len;
    bot.buffer_offset = 0;
    bot.data_residue  = alloc - len;
    bot.csw_status    = 0;
    bot.state         = BOT_STATE_TX_INLINE;
}

/* For commands with no data phase. */
static void finish_no_data(void) {
    bot.data_residue = bot.dCBWDataTransferLength;
    bot.csw_status   = 0;
    bot.state        = BOT_STATE_CSW;
}

/* A READ_10 / WRITE_10 CBW must carry a command block of 10 to 16 bytes, the data
 * direction the command implies (0x80 = IN for READ, 0x00 = OUT for WRITE), and a
 * transfer length equal to the blocks it moves. Call only after io_blocks_left is set. */
static bool cbw_rw10_ok(uint8_t direction)
{
    if (bot.bCBWCBLength < 10 || bot.bCBWCBLength > 16) return false;
    if (bot.io_blocks_left > 0 && (bot.bmCBWFlags & 0x80) != direction) return false;
    return bot.dCBWDataTransferLength == (uint32_t)bot.io_blocks_left * BLOCK_SIZE;
}

/* ---------- SCSI dispatch (runs once per CBW, then a state takes over) ---------- */

static void dispatch_scsi(void) {
    uint8_t opcode = bot.CBWCB[0];

    switch (opcode) {
        case SCSI_TEST_UNIT_READY:
        case SCSI_START_STOP_UNIT:
        case SCSI_PREVENT_ALLOW:
            finish_no_data();
            return;

        case SCSI_INQUIRY: {
            uint16_t alloc = (uint16_t)(((uint16_t)bot.CBWCB[3] << 8) | bot.CBWCB[4]);
            uint16_t len = sizeof(inquiry_data);
            if (len > alloc) len = alloc;
            start_inline_tx(inquiry_data, len);
            return;
        }

        case SCSI_REQUEST_SENSE: {
            uint8_t sense[18];
            memset(sense, 0, sizeof(sense));
            sense[0] = 0x70;  /* current errors, fixed format */
            sense[2] = 0x00;  /* no sense */
            sense[7] = 10;    /* additional sense length */
            uint16_t alloc = bot.CBWCB[4];
            uint16_t len = sizeof(sense);
            if (len > alloc) len = alloc;
            start_inline_tx(sense, len);
            return;
        }

        case SCSI_MODE_SENSE_6: {
            uint8_t mode[4] = { 3, 0, 0, 0 };
            uint16_t alloc = bot.CBWCB[4];
            uint16_t len = sizeof(mode);
            if (len > alloc) len = alloc;
            start_inline_tx(mode, len);
            return;
        }

        case SCSI_MODE_SENSE_10: {
            uint8_t mode[8];
            memset(mode, 0, sizeof(mode));
            mode[0] = 7;
            uint16_t alloc = (uint16_t)(((uint16_t)bot.CBWCB[7] << 8) | bot.CBWCB[8]);
            uint16_t len = sizeof(mode);
            if (len > alloc) len = alloc;
            start_inline_tx(mode, len);
            return;
        }

        case SCSI_READ_CAPACITY_10: {
            uint32_t disk = mc_partition_sectors > 0 ? mc_partition_sectors : mc_total_sectors;
            uint32_t last_lba = disk > 0 ? disk - 1 : 0;
            uint8_t cap[8] = {
                (uint8_t)(last_lba >> 24), (uint8_t)(last_lba >> 16),
                (uint8_t)(last_lba >> 8),  (uint8_t)(last_lba),
                0x00, 0x00, 0x02, 0x00  /* block size = 512 */
            };
            start_inline_tx(cap, 8);
            return;
        }

        case SCSI_READ_FORMAT_CAPACITIES: {
            uint32_t disk = mc_partition_sectors > 0 ? mc_partition_sectors : mc_total_sectors;
            uint32_t blocks = disk > 0 ? disk : 0x000FFFFF;
            uint8_t fmt[12] = {
                0x00, 0x00, 0x00, 0x08,
                (uint8_t)(blocks >> 24), (uint8_t)(blocks >> 16),
                (uint8_t)(blocks >> 8),  (uint8_t)(blocks),
                0x02,
                0x00, 0x02, 0x00
            };
            uint16_t alloc = (uint16_t)(((uint16_t)bot.CBWCB[7] << 8) | bot.CBWCB[8]);
            uint16_t len = sizeof(fmt);
            if (len > alloc) len = alloc;
            start_inline_tx(fmt, len);
            return;
        }

        case SCSI_READ_10: {
            bot.io_lba = ((uint32_t)bot.CBWCB[2] << 24)
                       | ((uint32_t)bot.CBWCB[3] << 16)
                       | ((uint32_t)bot.CBWCB[4] << 8)
                       | bot.CBWCB[5];
            bot.io_blocks_left = (uint16_t)(((uint16_t)bot.CBWCB[7] << 8) | bot.CBWCB[8]);
            /* Bounds-check against the advertised capacity (SCSI LBA-out-of-range).
             * Without it an out-of-range read pulls garbage sector-by-sector.
             * Overflow-safe form; permissive only when capacity is unknown
             * (disk==0), which matches the READ_CAPACITY fallback.  The CBW must
             * also match the command (see cbw_rw10_ok). */
            {
                uint32_t disk = mc_partition_sectors > 0 ? mc_partition_sectors : mc_total_sectors;
                if (!cbw_rw10_ok(0x80) ||
                    (disk > 0 && (bot.io_lba >= disk || bot.io_blocks_left > disk - bot.io_lba))) {
                    if (bot.dCBWDataTransferLength > 0) ep3_stall();   /* IN data stage */
                    bot.csw_status   = 1;
                    bot.data_residue = bot.dCBWDataTransferLength;
                    bot.state        = BOT_STATE_CSW;
                    return;
                }
            }
            bot.buffer_loaded = 0;
            bot.buffer_offset = 0;
            bot.data_residue  = bot.dCBWDataTransferLength;
            bot.csw_status    = 0;
            bot.state         = BOT_STATE_TX_SECTOR;
            return;
        }

        case SCSI_WRITE_10: {
            bot.io_lba = ((uint32_t)bot.CBWCB[2] << 24)
                       | ((uint32_t)bot.CBWCB[3] << 16)
                       | ((uint32_t)bot.CBWCB[4] << 8)
                       | bot.CBWCB[5];
            bot.io_blocks_left = (uint16_t)(((uint16_t)bot.CBWCB[7] << 8) | bot.CBWCB[8]);
            /* Bounds-check BEFORE any sd_write_sector: a write past the advertised
             * volume would corrupt a physical sector outside it (e.g. beyond the
             * partition).  Reject as LBA-out-of-range instead. Permissive only
             * when capacity is unknown (disk==0), and reject a CBW that does not
             * match the command (see cbw_rw10_ok). */
            {
                uint32_t disk = mc_partition_sectors > 0 ? mc_partition_sectors : mc_total_sectors;
                if (!cbw_rw10_ok(0x00) ||
                    (disk > 0 && (bot.io_lba >= disk || bot.io_blocks_left > disk - bot.io_lba))) {
                    if (bot.dCBWDataTransferLength > 0) ep2_stall();   /* OUT data stage */
                    bot.csw_status   = 1;
                    bot.data_residue = bot.dCBWDataTransferLength;
                    bot.state        = BOT_STATE_CSW;
                    return;
                }
            }
            bot.buffer_offset = 0;
            bot.data_residue  = bot.dCBWDataTransferLength;
            bot.csw_status    = 0;
            bot.state         = BOT_STATE_RX_SECTOR;
            return;
        }

        default:
            /* Unknown command: stall the data endpoint if there is one,
             * report failure in CSW. */
            if (bot.dCBWDataTransferLength > 0) {
                if (bot.bmCBWFlags & 0x80) ep3_stall();
                else                       ep2_stall();
            }
            bot.csw_status   = 1;
            bot.data_residue = bot.dCBWDataTransferLength;
            bot.state        = BOT_STATE_CSW;
            return;
    }
}

/* ---------- per-state handlers (each does ONE unit of work) ---------- */

static void tick_idle(void) {
    if (!ep2_out_bank_ready()) return;

    uint8_t cbw[31];
    uint16_t got = ep2_recv_packet(cbw, sizeof(cbw));
    if (got < 31) {
        /* Short packet — not a valid CBW; ignore and stay idle. */
        return;
    }

    uint32_t sig = (uint32_t)cbw[0]
                 | ((uint32_t)cbw[1] << 8)
                 | ((uint32_t)cbw[2] << 16)
                 | ((uint32_t)cbw[3] << 24);
    if (sig != CBW_SIGNATURE) {
        /* Invalid CBW signature: BOT spec says stall both endpoints and
         * await reset.  Easiest recovery: stall EP2-OUT and stay idle. */
        ep2_stall();
        return;
    }

    /* Any valid command from the host counts as storage activity, so the
     * firmware's "wait for the mount to settle" loop in main.c can tell when
     * Windows has stopped hammering the SD card. */
    storage_activity_mark();

    bot.dCBWTag = (uint32_t)cbw[4]
                | ((uint32_t)cbw[5] << 8)
                | ((uint32_t)cbw[6] << 16)
                | ((uint32_t)cbw[7] << 24);
    bot.dCBWDataTransferLength = (uint32_t)cbw[8]
                               | ((uint32_t)cbw[9]  << 8)
                               | ((uint32_t)cbw[10] << 16)
                               | ((uint32_t)cbw[11] << 24);
    bot.bmCBWFlags   = cbw[12];
    bot.bCBWLUN      = cbw[13];
    bot.bCBWCBLength = cbw[14];
    memcpy(bot.CBWCB, &cbw[15], 16);

    dispatch_scsi();
}

static void tick_tx_inline(void) {
    if (!ep3_in_bank_free()) return;

    uint16_t remaining = bot.buffer_total - bot.buffer_offset;
    uint16_t chunk = remaining > MAX_PACKET ? MAX_PACKET : remaining;
    ep3_send_packet(bot.buffer + bot.buffer_offset, chunk);
    bot.buffer_offset += chunk;

    if (bot.buffer_offset >= bot.buffer_total) {
        bot.state = BOT_STATE_CSW;
    }
}

static void tick_tx_sector(void) {
    if (bot.io_blocks_left == 0) {
        bot.data_residue = 0;
        bot.state = BOT_STATE_CSW;
        return;
    }

    if (!bot.buffer_loaded) {
        /* Load next sector from SD.  This is the longest unit of work
         * (~1–5 ms over SPI) but still bounded — small enough that the
         * main loop returns to HID work between sectors. */
        if (sd_read_sector(bot.io_lba, bot.buffer) != 0) {
            ep3_stall();
            bot.csw_status   = 1;
            bot.data_residue = (uint32_t)bot.io_blocks_left * BLOCK_SIZE;
            bot.state        = BOT_STATE_CSW;
            return;
        }
        bot.buffer_loaded = 1;
        bot.buffer_offset = 0;
        return; /* yield — send the first chunk on the next tick */
    }

    if (!ep3_in_bank_free()) return;

    uint16_t remaining = BLOCK_SIZE - bot.buffer_offset;
    uint16_t chunk = remaining > MAX_PACKET ? MAX_PACKET : remaining;
    ep3_send_packet(bot.buffer + bot.buffer_offset, chunk);
    bot.buffer_offset += chunk;
    if (bot.data_residue >= chunk) bot.data_residue -= chunk;

    if (bot.buffer_offset >= BLOCK_SIZE) {
        bot.buffer_loaded = 0;
        bot.io_lba++;
        bot.io_blocks_left--;
        if (bot.io_blocks_left == 0) {
            storage_activity_mark();
            bot.state = BOT_STATE_CSW;
        }
    }
}

static void tick_rx_sector(void) {
    if (bot.io_blocks_left == 0) {
        bot.data_residue = 0;
        bot.state = BOT_STATE_CSW;
        return;
    }

    if (!ep2_out_bank_ready()) return;

    uint16_t got = ep2_recv_packet(bot.buffer + bot.buffer_offset,
                                   (uint16_t)(BLOCK_SIZE - bot.buffer_offset));
    bot.buffer_offset += got;
    if (bot.data_residue >= got) bot.data_residue -= got;

    if (bot.buffer_offset >= BLOCK_SIZE) {
        if (sd_write_sector(bot.io_lba, bot.buffer) != 0) {
            ep2_stall();
            bot.csw_status   = 1;
            bot.data_residue = (uint32_t)bot.io_blocks_left * BLOCK_SIZE;
            bot.state        = BOT_STATE_CSW;
            return;
        }
        bot.buffer_offset = 0;
        bot.io_lba++;
        bot.io_blocks_left--;
        if (bot.io_blocks_left == 0) {
            storage_activity_mark();
            bot.state = BOT_STATE_CSW;
        }
    }
}

static void tick_csw(void) {
    if (!ep3_in_bank_free()) return;

    uint8_t csw[13];
    csw[0] = 0x55; csw[1] = 0x53; csw[2] = 0x42; csw[3] = 0x53; /* "USBS" */
    csw[4]  = (uint8_t)(bot.dCBWTag      );
    csw[5]  = (uint8_t)(bot.dCBWTag >>  8);
    csw[6]  = (uint8_t)(bot.dCBWTag >> 16);
    csw[7]  = (uint8_t)(bot.dCBWTag >> 24);
    csw[8]  = (uint8_t)(bot.data_residue      );
    csw[9]  = (uint8_t)(bot.data_residue >>  8);
    csw[10] = (uint8_t)(bot.data_residue >> 16);
    csw[11] = (uint8_t)(bot.data_residue >> 24);
    csw[12] = bot.csw_status;
    ep3_send_packet(csw, 13);

    bot.state = BOT_STATE_IDLE;
}

/* ---------- public per-iteration entry point ---------- */

void usb_msc_task(void) {
    switch (bot.state) {
        case BOT_STATE_IDLE:      tick_idle();       break;
        case BOT_STATE_TX_INLINE: tick_tx_inline();  break;
        case BOT_STATE_TX_SECTOR: tick_tx_sector();  break;
        case BOT_STATE_RX_SECTOR: tick_rx_sector();  break;
        case BOT_STATE_CSW:       tick_csw();        break;
    }
}
