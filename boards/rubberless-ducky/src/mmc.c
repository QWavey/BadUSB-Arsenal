#include "diskio.h"
#include <avr32/io.h>
#include <stdint.h>
#include <string.h>

#define SD_PIN_MISO 25  /* PA25 CAN be used as GPIO BEFORE usb_device_init() is called! */
#define SD_PIN_MOSI 14
#define SD_PIN_SCK  15
#define SD_PIN_CS   17  /* FOUND BY SCANNER! */
#define SD_PIN_PWR  18

#define _GPIO_PORT(n) ((volatile avr32_gpio_port_t *)(AVR32_GPIO_ADDRESS + (n)*sizeof(avr32_gpio_port_t)))

static inline void SELECT(void)   { _GPIO_PORT(0)->ovrc = (1u << SD_PIN_CS); }
static inline void DESELECT(void) { _GPIO_PORT(0)->ovrs = (1u << SD_PIN_CS); }

static uint8_t spi_fast = 0;

static void spi_delay(void) {
    if (spi_fast) return; /* Full speed! */
    for (volatile int i = 0; i < 16; i++); /* ~400kHz init speed — SD spec min is 100-400kHz */
}

/* ---- Hardware SPI0 (fast path) --------------------------------------------
 * The SD is wired to this chip's dedicated SPI0 pins — confirmed in the UC3B
 * headers: MISO=PA25, MOSI=PA14, SCK=PA15 (peripheral function A), CS on PA17.
 * Software bit-banging those was the SD bottleneck (~1-5 ms/sector).  We drive
 * the data lines with the SPI0 controller (one register write/read per byte,
 * real clock) and keep CS as a manual GPIO on PA17.  disk_initialize() tries
 * this first and AUTO-FALLS-BACK to bit-bang if the controller doesn't read a
 * valid boot sector — so a wrong config degrades to today's behaviour, never a
 * dead boot. */
#define SPI_BASE      0xFFFF2400u
#define SPI_CR        (*(volatile uint32_t*)(SPI_BASE+0x00))
#define SPI_MR        (*(volatile uint32_t*)(SPI_BASE+0x04))
#define SPI_RDR       (*(volatile uint32_t*)(SPI_BASE+0x08))
#define SPI_TDR       (*(volatile uint32_t*)(SPI_BASE+0x0C))
#define SPI_SR        (*(volatile uint32_t*)(SPI_BASE+0x10))
#define SPI_CSR0      (*(volatile uint32_t*)(SPI_BASE+0x30))
#define SPI_SR_RDRF   (1u<<0)   /* Receive Data Register Full */
#define SPI_SR_TDRE   (1u<<1)   /* Transmit Data Register Empty */
#define SPI_SCBR_SLOW 120u      /* ~400 kHz @ PBA 48 MHz — card init */
#define SPI_SCBR_FAST 10u       /* ~4.8 MHz — safe, ~5-10x the bit-bang throughput */

static uint8_t use_hw_spi = 0;

/* Set when a bounded hardware-SPI wait expires.  A block transfer that hit a
 * timeout moved garbage, so the read and write paths check this flag before
 * they trust the data. */
static volatile uint8_t spi_timed_out = 0;

/* Route MISO/MOSI/SCK to SPI0 (function A); CS (PA17) stays a manual GPIO. */
static void spi_pins_peripheral(void) {
    volatile avr32_gpio_port_t *pa = _GPIO_PORT(0);
    uint32_t m = (1u<<SD_PIN_MISO) | (1u<<SD_PIN_MOSI) | (1u<<SD_PIN_SCK);
    pa->puers = (1u<<SD_PIN_MISO);  /* keep MISO pulled up when the card floats it */
    pa->pmr0c = m;    /* function A = (PMR1,PMR0)=(0,0) */
    pa->pmr1c = m;
    pa->gperc = m;    /* clear GPER -> the SPI peripheral drives the pin */
}
/* Give MISO/MOSI/SCK back to GPIO (for the bit-bang fallback). */
static void spi_pins_gpio(void) {
    volatile avr32_gpio_port_t *pa = _GPIO_PORT(0);
    pa->gpers = (1u<<SD_PIN_MISO) | (1u<<SD_PIN_MOSI) | (1u<<SD_PIN_SCK);
}
static void spi_hw_init(uint32_t scbr) {
    SPI_CR = (1u<<7);                        /* SWRST */
    SPI_CR = (1u<<7);
    SPI_MR = (1u<<0) | (1u<<4);              /* MSTR=1 master, MODFDIS=1; PS=0 fixed, PCS=0->CSR0 */
    SPI_CSR0 = (1u<<1) | ((scbr & 0xFFu)<<8);/* NCPHA=1,CPOL=0 (SPI mode 0), 8 bits, SCBR=scbr */
    SPI_CR = (1u<<0);                        /* SPIEN */
}
static void spi_set_clock(uint32_t scbr) {
    SPI_CSR0 = (1u<<1) | ((scbr & 0xFFu)<<8);
}
static BYTE spi_hw_xfer(BYTE out) {
    uint32_t g = 200000;
    while (!(SPI_SR & SPI_SR_TDRE) && --g);
    if (!g) spi_timed_out = 1;
    SPI_TDR = out;                            /* PCS=0; CS is the manual GPIO on PA17 */
    g = 200000;
    while (!(SPI_SR & SPI_SR_RDRF) && --g);
    if (!g) spi_timed_out = 1;
    return (BYTE)(SPI_RDR & 0xFF);
}

/* Pipelined block read: keep the double-buffered SPI shifter continuously fed
 * (queue byte i+1's clocks while byte i is still coming in) so there are no idle
 * gaps between bytes.  ~1.4-2x the naive per-byte read of a 512-byte sector.
 * Sends 0xFF (MISO is what the card drives). */
static void spi_hw_read_block(BYTE *buf, unsigned n) {
    if (n == 0) return;
    uint32_t g;
    SPI_TDR = 0xFF;                           /* clock out byte 0 */
    for (unsigned i = 0; i < n; i++) {
        if (i + 1 < n) {                      /* queue the NEXT byte's clocks early */
            g = 200000; while (!(SPI_SR & SPI_SR_TDRE) && --g);
            if (!g) { spi_timed_out = 1; return; }
            SPI_TDR = 0xFF;
        }
        g = 200000; while (!(SPI_SR & SPI_SR_RDRF) && --g);
        if (!g) { spi_timed_out = 1; return; }
        buf[i] = (BYTE)(SPI_RDR & 0xFF);      /* collect byte i */
    }
}

/* Pipelined block write: stream bytes into TDR as fast as TDRE allows (received
 * data is irrelevant on a write, so we don't stall on RDRF), then wait for
 * TXEMPTY so the block has fully shifted out before we continue. */
static void spi_hw_write_block(const BYTE *buf, unsigned n) {
    uint32_t g;
    for (unsigned i = 0; i < n; i++) {
        g = 200000; while (!(SPI_SR & SPI_SR_TDRE) && --g);
        if (!g) { spi_timed_out = 1; return; }
        SPI_TDR = buf[i];
    }
    g = 200000; while (!(SPI_SR & (1u<<9)) && --g);   /* TXEMPTY: shifter drained */
    if (!g) spi_timed_out = 1;
    (void)SPI_RDR;                                     /* clear any RDRF/overrun */
}

static BYTE sd_spi_transfer(BYTE out) {
    if (use_hw_spi) return spi_hw_xfer(out);
    BYTE in = 0;
    volatile avr32_gpio_port_t *pa = _GPIO_PORT(0);

#define SPI_BIT(bit_idx) \
    if (out & (1 << (7 - bit_idx))) pa->ovrs = (1u << SD_PIN_MOSI); \
    else                            pa->ovrc = (1u << SD_PIN_MOSI); \
    spi_delay(); \
    pa->ovrs = (1u << SD_PIN_SCK); /* SCK HIGH */ \
    spi_delay(); \
    in <<= 1; \
    if (pa->pvr & (1u << SD_PIN_MISO)) in |= 1; \
    pa->ovrc = (1u << SD_PIN_SCK); /* SCK LOW */ \
    spi_delay();

    SPI_BIT(0); SPI_BIT(1); SPI_BIT(2); SPI_BIT(3);
    SPI_BIT(4); SPI_BIT(5); SPI_BIT(6); SPI_BIT(7);
    return in;
}

static BYTE rcv_spi(void) { return sd_spi_transfer(0xFF); }

static void dly_100us(void) {
    for (volatile int i = 0; i < 480; i++);
}

static BYTE send_cmd(BYTE cmd, DWORD arg) {
    BYTE n, res;
    if (cmd & 0x80) {
        cmd &= 0x7F;
        res = send_cmd(55, 0);
        if (res > 1) return res;
    }
    DESELECT();
    rcv_spi();
    SELECT();
    rcv_spi();
    sd_spi_transfer(0x40 | cmd);  /* SD cmds always have top two bits = 01 */
    sd_spi_transfer((BYTE)(arg >> 24));
    sd_spi_transfer((BYTE)(arg >> 16));
    sd_spi_transfer((BYTE)(arg >> 8));
    sd_spi_transfer((BYTE)arg);
    n = 0x01;
    if (cmd == 0)  n = 0x95;  /* CMD0  CRC (only one needed in SPI mode) */
    if (cmd == 8)  n = 0x87;  /* CMD8  CRC */
    sd_spi_transfer(n);
    if (cmd == 12) rcv_spi();  /* CMD12 needs one extra byte before response */
    n = 10;
    do {
        res = rcv_spi();
    } while ((res & 0x80) && --n);
    return res;
}

static volatile uint32_t cached_sector = 0xFFFFFFFF;
static uint8_t sector_cache[512];
static BYTE CardType;
static uint8_t spi_dead = 0;

/* ---- Multi-sector cache ----
 * PA25 = USBB D-. After usb_device_init(), SPI MISO is dead.
 * Every sector PetitFS reads before USB init is cached here.
 * SCSI READ_10 serves from this cache.
 * FIFO eviction: when full, oldest slot is reused.
 *
 * This cache exists only to answer host mass-storage (SCSI READ_10) requests
 * after SPI dies.  The firmware now forces a HID-only host profile (see
 * parse_attackmode in main.c), so the host never mounts the drive and this
 * cache is never queried at runtime — only the handful of sectors pre-cached
 * at boot are ever stored.  MC_MAX was therefore cut from 32 to 8 (16 KB ->
 * 4 KB), and that reclaimed 12 KB of SRAM was handed to payload_ram in main.c
 * to raise the payload size ceiling.  8 slots still comfortably cover the 3-4
 * sectors mc_precache() touches at boot. */
#define MC_MAX 8
#define SD_READ_ATTEMPTS 3   /* bounded tries for one block read before it is reported as an error */
static uint32_t mc_sectors[MC_MAX];
static uint8_t  mc_data[MC_MAX][512];
static uint16_t mc_count = 0;
static uint16_t mc_next = 0;       /* FIFO eviction slot */

uint32_t mc_total_sectors = 0;     /* Real card capacity from CSD */
uint32_t mc_partition_sectors = 0; /* Partition size from VBR BPB */

static void mc_store(uint32_t sector, const uint8_t *data) {
    for (uint16_t i = 0; i < mc_count; i++) {
        if (mc_sectors[i] == sector) return; /* already cached */
    }
    uint16_t slot;
    if (mc_count < MC_MAX) {
        slot = mc_count;
        mc_count++;
    } else {
        slot = mc_next;
        mc_next = (mc_next + 1) % MC_MAX;
    }
    mc_sectors[slot] = sector;
    memcpy(mc_data[slot], data, 512);
    if (sector + 1 > mc_total_sectors) mc_total_sectors = sector + 1;
}

void mc_reset(void) {
    mc_count = 0;
    mc_next = 0;
    cached_sector = 0xFFFFFFFF;
    spi_dead = 0;
}

static int mc_find(uint32_t sector, uint8_t *buff) {
    for (uint16_t i = 0; i < mc_count; i++) {
        if (mc_sectors[i] == sector) {
            if (buff) memcpy(buff, mc_data[i], 512);
            return 0;
        }
    }
    return -1;
}

void mc_precache(uint32_t sector) {
    uint8_t dummy[512];
    if (mc_find(sector, NULL) == 0) return;
    sd_read_sector(sector, dummy);
}

void sd_mark_spi_dead(void) {
    spi_dead = 1;
}

/* Prove the current transport/clock reads correctly: sector 0 of a FAT card
 * ends in the 0x55AA boot signature.  Read it twice (forcing a real re-read
 * each time) so a marginal too-fast clock that corrupts intermittently is
 * rejected, not just gross failure.  Returns 1 if trustworthy. */
static int spi_validate_reads(void) {
    uint8_t s0[512];
    for (int i = 0; i < 2; i++) {
        cached_sector = 0xFFFFFFFF;                 /* force a fresh read */
        if (disk_readp(s0, 0, 0, 512) != RES_OK) return 0;
        if (s0[510] != 0x55 || s0[511] != 0xAA)   return 0;
    }
    cached_sector = 0xFFFFFFFF;                     /* leave nothing stale cached */
    return 1;
}

/* SD power-on init handshake (CMD0/8/ACMD41/58).  Uses sd_spi_transfer(), so it
 * runs over whichever transport is currently selected (hardware SPI or bit-bang).
 * Returns CardType (0 = no usable card). */
static BYTE sd_card_init_seq(void) {
    BYTE n, cmd, ty = 0, ocr[4];
    for (n = 10; n; n--) rcv_spi();                /* >=74 clocks, CS high */
    if (send_cmd(0, 0) == 1) {                      /* CMD0  GO_IDLE_STATE */
        if (send_cmd(8, 0x1AA) == 1) {              /* CMD8  SEND_IF_COND */
            for (n = 0; n < 4; n++) ocr[n] = rcv_spi();
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
                for (int t = 10000; t && send_cmd(0x80 | 41, 1UL << 30); t--) dly_100us();
                if (send_cmd(58, 0) == 0) {          /* CMD58 READ_OCR */
                    for (n = 0; n < 4; n++) ocr[n] = rcv_spi();
                    ty = (ocr[0] & 0x40) ? 12 : 10;
                }
            }
        } else {
            if (send_cmd(0x80 | 41, 0) <= 1) { ty = 2; cmd = 0x80 | 41; }
            else                             { ty = 1; cmd = 1; }
            for (int t = 10000; t && send_cmd(cmd, 0); t--) dly_100us();
            if (send_cmd(16, 512) != 0) ty = 0;
        }
    }
    DESELECT();
    rcv_spi();
    return ty;
}

DSTATUS disk_initialize(void) {
    BYTE ty;
    volatile avr32_gpio_port_t *pa = _GPIO_PORT(0);
    cached_sector = 0xFFFFFFFF;
    mc_count = 0;
    mc_total_sectors = 0;

    /* DAT1/DAT2 pull-ups */
    pa->gpers = (1u << 18) | (1u << 19);
    pa->oderc = (1u << 18) | (1u << 19);
    pa->puers = (1u << 18) | (1u << 19);
    for (int ms = 0; ms < 10; ms++) dly_100us();

    /* CS (PA17) is a manual GPIO output, deselected, in BOTH transports. */
    pa->gpers = (1u << SD_PIN_CS);
    pa->oders = (1u << SD_PIN_CS);
    pa->ovrs  = (1u << SD_PIN_CS);

    /* ===== Attempt 1: hardware SPI0 (fast) ===== */
    use_hw_spi = 1;
    spi_pins_peripheral();
    spi_hw_init(SPI_SCBR_SLOW);
    for (int ms = 0; ms < 10; ms++) dly_100us();
    ty = sd_card_init_seq();
    if (ty) {
        CardType = ty;
        /* Auto-tune the SPI clock: use the FASTEST divider that still reads
         * sector 0 correctly on this board.  SCBR = PBA(48MHz)/SPCK, so
         * 2->24MHz, 4->12MHz, 6->8MHz, 10->4.8MHz.  Each is validated; the
         * first that passes wins.  If none do, ty stays effectively rejected
         * and we drop to bit-bang below. */
        static const uint8_t scbr_ladder[] = { 4, 6, 10 };   /* 12, 8, 4.8 MHz */
        int locked = 0;
        for (unsigned k = 0; k < sizeof(scbr_ladder); k++) {
            spi_set_clock(scbr_ladder[k]);
            if (spi_validate_reads()) { locked = 1; break; }
        }
        if (!locked) ty = 0;
    }

    /* ===== Fallback: software bit-bang (proven-good path) ===== */
    if (!ty) {
        use_hw_spi    = 0;
        spi_fast      = 0;
        cached_sector = 0xFFFFFFFF;   /* discard anything the HW attempt cached */
        mc_count      = 0;
        spi_pins_gpio();
        pa->oderc = (1u << SD_PIN_MISO);
        pa->puers = (1u << SD_PIN_MISO);
        pa->oders = (1u << SD_PIN_MOSI) | (1u << SD_PIN_SCK);
        pa->ovrs  = (1u << SD_PIN_MOSI);
        pa->ovrc  = (1u << SD_PIN_SCK);
        for (int ms = 0; ms < 10; ms++) dly_100us();
        ty = sd_card_init_seq();
        spi_fast = 1;
    }
    CardType = ty;
    DESELECT();
    rcv_spi();

    /* Read real card capacity from CSD while SPI is still alive */
    if (ty) {
        spi_fast = 1;
        if (send_cmd(9, 0) == 0) {
            uint16_t t = 40000;
            while (rcv_spi() != 0xFE && --t);
            if (t) {
                uint8_t csd[16];
                for (uint8_t i = 0; i < 16; i++) csd[i] = rcv_spi();
                rcv_spi(); rcv_spi();
                if ((csd[0] >> 6) == 1) {
                    /* CSD v2 (SDHC/SDXC) */
                    uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16)
                                    | ((uint32_t)csd[8] << 8)
                                    | csd[9];
                    mc_total_sectors = (c_size + 1) * 1024;
                } else {
                    /* CSD v1 */
                    uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10)
                                    | ((uint32_t)csd[7] << 2)
                                    | (csd[8] >> 6);
                    uint32_t c_size_mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
                    uint32_t block_len = 1 << (csd[5] & 0x0F);
                    mc_total_sectors = (c_size + 1) * (1 << (c_size_mult + 2)) * block_len / 512;
                }
            }
            DESELECT();
            rcv_spi();
        }
    }

    return ty ? 0 : STA_NOINIT;
}

/* One CMD17 block read into `buf`.  Returns 1 only when the card answered, the
 * data token arrived, and no bounded SPI wait expired while the block moved.
 * The card is deselected on every path. */
static int read_block_once(DWORD cmd_sector, BYTE *buf) {
    int ok = 0;
    if (send_cmd(17, cmd_sector) == 0) {
        uint16_t t = 40000;
        while (rcv_spi() != 0xFE && --t);
        if (t) {
            spi_timed_out = 0;
            if (use_hw_spi) spi_hw_read_block(buf, 512);
            else            for (int i = 0; i < 512; i++) buf[i] = rcv_spi();
            rcv_spi(); rcv_spi();                  /* CRC: read and discarded */
            ok = !spi_timed_out;
        }
    }
    DESELECT();
    rcv_spi();
    return ok;
}

DRESULT disk_readp(BYTE* buff, DWORD sector, UINT offset, UINT count) {
    if (cached_sector != sector) {
        /* SDHC/SDXC (CardType==12) use block address; all others need byte address */
        DWORD cmd_sector = (CardType == 12) ? sector : (sector * 512);
        int ok = 0;
        /* A failed attempt can leave sector_cache half overwritten, so the cache
         * is invalidated first; only a complete, error-free read validates it. */
        cached_sector = 0xFFFFFFFF;
        for (uint8_t attempt = 0; attempt < SD_READ_ATTEMPTS && !ok; attempt++)
            ok = read_block_once(cmd_sector, sector_cache);
        if (!ok) return RES_ERROR;          /* cached_sector and mc_store() untouched */
        cached_sector = sector;
        /* Store in multi-sector cache for post-USB reads */
        mc_store(sector, sector_cache);
    }
    if (buff) {
        for (UINT i = 0; i < count; i++) {
            buff[i] = sector_cache[offset + i];
        }
    }
    return RES_OK;
}

DRESULT sd_read_sector(DWORD sector, BYTE *buff) {
    /* LIVE mode (SPI still reachable after USB — the normal case now): always
     * read the real card so the host sees its own writes on read-back.  The
     * mc_data cache is only a read-only boot SNAPSHOT; serving from it here
     * would mask fresh writes and break delete/move/copy on the mounted drive. */
    if (!spi_dead) {
        return disk_readp(buff, sector, 0, 512);
    }
    /* FALLBACK: SPI was marked unavailable, so only the boot snapshot can answer.
     * Any other sector is reported as an error. Returning zeros as success would
     * make the host read blank data as if it were the card's real contents. */
    if (mc_find(sector, buff) == 0) return RES_OK;
    return RES_ERROR;
}

DRESULT sd_write_sector(DWORD sector, const BYTE *buff) {
    DWORD cmd_sector = (CardType == 12) ? sector : (sector * 512);
    cached_sector = 0xFFFFFFFF;            /* a failed write may leave the cached copy stale */
    spi_timed_out = 0;
    if (send_cmd(24, cmd_sector) == 0) {
        sd_spi_transfer(0xFE);
        if (use_hw_spi) spi_hw_write_block(buff, 512);
        else            for (int i = 0; i < 512; i++) sd_spi_transfer(buff[i]);
        sd_spi_transfer(0xFF); sd_spi_transfer(0xFF);
        BYTE resp = rcv_spi();
        if ((resp & 0x1F) == 0x05) {
            UINT bc = 40000;
            while (rcv_spi() == 0 && --bc);   /* wait out the card's programming-busy (MISO held low) */
            DESELECT(); rcv_spi();
            cached_sector = 0xFFFFFFFF;
            /* Don't claim success if the card never released busy (bc exhausted),
             * or if an SPI wait expired while the block moved: the sector may not
             * be committed, and reporting RES_OK would let the host believe a
             * write landed that didn't. */
            return (bc && !spi_timed_out) ? RES_OK : RES_ERROR;
        }
    }
    DESELECT(); rcv_spi();
    return RES_ERROR;
}

/* Petit-FatFs streaming write hook (needed by pf_write, used by EXFIL).
 * Protocol, per ChaN:
 *   disk_writep(0, sector) -> initiate a single-block write at LBA `sector`
 *   disk_writep(data, n)   -> stream n data bytes into the open block
 *   disk_writep(0, 0)      -> finalize (pad, CRC, wait for the card)
 * Uses the same bit-banged SPI + CMD24 path as sd_write_sector, and refuses if
 * the SD has been marked offline. */
DRESULT disk_writep(const BYTE *buff, DWORD sa) {
    static uint16_t wc;            /* bytes still expected in the open block */
    DRESULT res = RES_ERROR;

    if (spi_dead) return RES_ERROR;

    if (buff) {                    /* ---- stream data bytes ---- */
        uint16_t bc = (uint16_t)sa;
        while (bc && wc) { sd_spi_transfer(*buff++); wc--; bc--; }
        res = RES_OK;
    } else if (sa) {               /* ---- initiate block write ---- */
        DWORD adr = (CardType & CT_BLOCK) ? sa : sa * 512;
        if (send_cmd(24, adr) == 0) {          /* CMD24 WRITE_SINGLE_BLOCK */
            sd_spi_transfer(0xFF);
            sd_spi_transfer(0xFE);             /* data-block start token */
            wc = 512;
            res = RES_OK;
        }
    } else {                       /* ---- finalize block write ---- */
        uint16_t bc = wc + 2;                  /* pad remaining bytes + dummy CRC */
        while (bc--) sd_spi_transfer(0);
        if ((rcv_spi() & 0x1F) == 0x05) {      /* data response = accepted */
            uint16_t to = 5000;
            while (rcv_spi() != 0xFF && to) { dly_100us(); to--; }
            if (to) res = RES_OK;
        }
        DESELECT(); rcv_spi();
        cached_sector = 0xFFFFFFFF;            /* invalidate read cache line */
    }
    return res;
}

