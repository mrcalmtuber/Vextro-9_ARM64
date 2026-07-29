#ifndef EMMC_H
#define EMMC_H

#include <stdint.h>
#include "mbox.h"

/*
 * The SD card, which on a Raspberry Pi is the disk.
 *
 * There is no SATA port, no NVMe slot and no IDE controller. Every Pi
 * boots from a card in a slot, and a kernel that cannot read one has no
 * filesystem at all — so this file is what makes the difference between
 * an OS that runs on a Pi and an OS that draws a desktop on a Pi and
 * then has nothing to put in it.
 *
 * The controller is SDHCI, an actual specification rather than a
 * Broadcom invention, which is a relief: the Pi 4 routes the card to a
 * clean SDHCI 3.0 block ("emmc2") and earlier boards use an Arasan one
 * that is close enough to share this code. Both are driven the same way
 * here, and the only board-specific part is where the clock comes from —
 * which, this being a Pi, is a question for the firmware rather than a
 * register.
 *
 * Card initialisation is a fixed conversation and the order is not
 * negotiable, because each step is what makes the next one legal:
 *
 *   CMD0    reset every card on the bus to idle
 *   CMD8    state the host's voltage; a card that answers is v2 or later
 *   ACMD41  ask the card to power up, repeatedly, until it says it has
 *   CMD2    read the card identification, moving it to the ident state
 *   CMD3    ask the card to pick an address, moving it to standby
 *   CMD9    read the card-specific data, which carries the capacity
 *   CMD7    select that address, moving it to transfer
 *   ACMD6   switch to the four-bit bus
 *
 * Transfers are polled through the data port a word at a time. The
 * controller has a DMA engine and this does not use it: the descriptor
 * format is another 200 lines, the card is the slowest thing in the
 * machine by two orders of magnitude, and a polled 512-byte block is
 * already faster than the card can supply one. If storage ever becomes
 * the bottleneck on this platform, that is where to look — but it will
 * not be, and pretending otherwise would be optimising the wrong end.
 *
 * Nothing here has run on a Raspberry Pi. It is written from the SD
 * physical layer and SDHCI specifications, and the parts that are
 * guesses are marked as guesses rather than left to look like fact.
 */

/* ---- SDHCI registers ---- */
#define EMMC_ARG2           0x00
#define EMMC_BLKSIZECNT     0x04
#define EMMC_ARG1           0x08
#define EMMC_CMDTM          0x0C
#define EMMC_RESP0          0x10
#define EMMC_RESP1          0x14
#define EMMC_RESP2          0x18
#define EMMC_RESP3          0x1C
#define EMMC_DATA           0x20
#define EMMC_STATUS         0x24
#define EMMC_CONTROL0       0x28
#define EMMC_CONTROL1       0x2C
#define EMMC_INTERRUPT      0x30
#define EMMC_IRPT_MASK      0x34
#define EMMC_IRPT_EN        0x38
#define EMMC_CAPABILITIES   0x40
#define EMMC_SLOTISR_VER    0xFC

/* STATUS */
#define SR_CMD_INHIBIT      (1u << 0)
#define SR_DAT_INHIBIT      (1u << 1)
#define SR_READ_AVAILABLE   (1u << 11)
#define SR_WRITE_AVAILABLE  (1u << 10)

/* CONTROL1 */
#define C1_CLK_INTLEN       (1u << 0)
#define C1_CLK_STABLE       (1u << 1)
#define C1_CLK_EN           (1u << 2)
#define C1_SRST_HC          (1u << 24)
#define C1_SRST_CMD         (1u << 25)
#define C1_SRST_DATA        (1u << 26)

/* INTERRUPT */
#define INT_CMD_DONE        (1u << 0)
#define INT_DATA_DONE       (1u << 1)
#define INT_WRITE_RDY       (1u << 4)
#define INT_READ_RDY        (1u << 5)
#define INT_ERROR_MASK      0xFFFF0000u

/* CMDTM: command index, type, response and data direction */
#define CMD_RSPNS_NONE      (0u << 16)
#define CMD_RSPNS_136       (1u << 16)
#define CMD_RSPNS_48        (2u << 16)
#define CMD_RSPNS_48BUSY    (3u << 16)
#define CMD_CRCCHK_EN       (1u << 19)
#define CMD_IXCHK_EN        (1u << 20)
#define CMD_ISDATA          (1u << 21)
#define TM_MULTI_BLOCK      (1u << 5)
#define TM_DAT_DIR_CH       (1u << 4)   /* card to host: a read */
#define TM_AUTO_CMD12       (1u << 2)
#define TM_BLKCNT_EN        (1u << 1)

#define CMD_INDEX(n)        ((uint32_t)(n) << 24)

/* Commands, pre-assembled with the response type each one returns. */
#define CMD_GO_IDLE         (CMD_INDEX(0)  | CMD_RSPNS_NONE)
#define CMD_ALL_SEND_CID    (CMD_INDEX(2)  | CMD_RSPNS_136 | CMD_CRCCHK_EN)
#define CMD_SEND_REL_ADDR   (CMD_INDEX(3)  | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define CMD_SELECT_CARD     (CMD_INDEX(7)  | CMD_RSPNS_48BUSY | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define CMD_SEND_IF_COND    (CMD_INDEX(8)  | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define CMD_SEND_CSD        (CMD_INDEX(9)  | CMD_RSPNS_136 | CMD_CRCCHK_EN)
#define CMD_STOP_TRANS      (CMD_INDEX(12) | CMD_RSPNS_48BUSY | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define CMD_SET_BLOCKLEN    (CMD_INDEX(16) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define CMD_READ_SINGLE     (CMD_INDEX(17) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA | TM_DAT_DIR_CH)
#define CMD_READ_MULTI      (CMD_INDEX(18) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA | TM_DAT_DIR_CH | TM_MULTI_BLOCK | TM_BLKCNT_EN | TM_AUTO_CMD12)
#define CMD_WRITE_SINGLE    (CMD_INDEX(24) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA)
#define CMD_WRITE_MULTI     (CMD_INDEX(25) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA | TM_MULTI_BLOCK | TM_BLKCNT_EN | TM_AUTO_CMD12)
#define CMD_APP             (CMD_INDEX(55) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN)

/* Application commands, valid only immediately after CMD55. */
#define ACMD_SET_BUS_WIDTH  (CMD_INDEX(6)  | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define ACMD_SEND_OP_COND   (CMD_INDEX(41) | CMD_RSPNS_48)

static int      emmc_present = 0;
static uint64_t emmc_sectors = 0;
static uint32_t emmc_rca = 0;          /* the card's chosen address     */
static int      emmc_sdhc = 0;         /* addresses blocks, not bytes   */
static uint32_t emmc_resp[4];
static uint32_t emmc_base_clock = 0;

static void emmc_log(const char *s) {
    serial_puts("[emmc] ");
    serial_puts(s);
    serial_putc('\n');
}

static inline volatile uint32_t *emmc_reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(bcm_emmc_base + off);
}

static inline uint32_t emmc_rd(uint32_t off) { DSB(); return *emmc_reg(off); }
static inline void emmc_wr(uint32_t off, uint32_t v) { DSB(); *emmc_reg(off) = v; }

/*
 * Delays are in real microseconds, from the architected counter.
 *
 * A spin count would mean something different on every core this could
 * run on, and the SD specification's timings are in time — 74 clocks at
 * 400 kHz before the first command, one millisecond for a reset to
 * settle. Using the counter makes them mean what they say.
 */
static void emmc_delay_us(uint64_t us) {
    uint64_t hz = timer_hz();
    uint64_t target = timer_count() + (hz / 1000000ULL) * us + 1;
    while (timer_count() < target) { }
}

static int emmc_wait_reg(uint32_t off, uint32_t mask, uint32_t want, uint64_t us) {
    uint64_t hz = timer_hz();
    uint64_t limit = timer_count() + (hz / 1000000ULL) * us + 1;
    while (timer_count() < limit)
        if ((emmc_rd(off) & mask) == want) return 1;
    return 0;
}

/* Wait for one of the interrupt-status bits, and treat the error bits as
 * always-interesting: a command that fails sets an error bit and never
 * sets the completion bit, so waiting only for completion hangs. */
static int emmc_wait_int(uint32_t mask, uint64_t us) {
    uint64_t hz = timer_hz();
    uint64_t limit = timer_count() + (hz / 1000000ULL) * us + 1;
    while (timer_count() < limit) {
        uint32_t st = emmc_rd(EMMC_INTERRUPT);
        if (st & mask) {
            emmc_wr(EMMC_INTERRUPT, mask);
            return 1;
        }
        if (st & INT_ERROR_MASK) {
            emmc_wr(EMMC_INTERRUPT, st);
            return 0;
        }
    }
    return 0;
}

/*
 * Set the card clock.
 *
 * SDHCI divides a base clock the host controller reports, but on a Pi
 * that field is frequently zero — the VideoCore owns the clock tree and
 * the controller does not know its own input. So the firmware is asked
 * first and the capability register is only the fallback, which is
 * exactly backwards from how the specification describes it and exactly
 * right for this board.
 *
 * The divider is the SDHCI 3.0 ten-bit form, split across two fields
 * with the low two bits of the register holding the *high* bits of the
 * divider — an encoding that exists for backward compatibility and
 * catches everyone once.
 */
static int emmc_set_clock(uint32_t hz) {
    if (!emmc_base_clock) return 0;

    /* Stop the clock while the divider changes; changing it live is
     * undefined and on some cards produces a permanently confused one. */
    emmc_wr(EMMC_CONTROL1, emmc_rd(EMMC_CONTROL1) & ~C1_CLK_EN);

    uint32_t div = 1;
    while ((emmc_base_clock / div) > hz && div < 2046) div += 2;
    uint32_t d = div / 2;                    /* register holds div/2 */

    uint32_t c1 = emmc_rd(EMMC_CONTROL1);
    c1 &= ~0xFFE0u;                          /* clear both divider fields */
    c1 |= (d & 0xFF) << 8;
    c1 |= ((d >> 8) & 0x03) << 6;
    c1 &= ~(0xFu << 16);
    c1 |= (11u << 16);                       /* data timeout, near maximum */
    c1 |= C1_CLK_INTLEN;
    emmc_wr(EMMC_CONTROL1, c1);

    if (!emmc_wait_reg(EMMC_CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, 100000)) {
        emmc_log("clock never stabilised");
        return 0;
    }
    emmc_wr(EMMC_CONTROL1, emmc_rd(EMMC_CONTROL1) | C1_CLK_EN);
    emmc_delay_us(2000);
    return 1;
}

/*
 * Issue one command.
 *
 * `code` is a pre-assembled CMDTM word carrying the index, the response
 * type and whether data follows. The response is copied out immediately,
 * because RESP0 is reused by the next command and a caller that reads it
 * later reads someone else's answer.
 */
static int emmc_command(uint32_t code, uint32_t arg, uint64_t timeout_us) {
    if (!emmc_wait_reg(EMMC_STATUS, SR_CMD_INHIBIT, 0, 500000)) {
        emmc_log("command line never freed");
        return 0;
    }
    /* A command that moves data must also wait for the data line, except
     * for the abort commands, which exist precisely to free it. */
    if (code & CMD_ISDATA)
        if (!emmc_wait_reg(EMMC_STATUS, SR_DAT_INHIBIT, 0, 500000)) {
            emmc_log("data line never freed");
            return 0;
        }

    emmc_wr(EMMC_INTERRUPT, emmc_rd(EMMC_INTERRUPT));   /* clear stale bits */
    emmc_wr(EMMC_ARG1, arg);
    emmc_wr(EMMC_CMDTM, code);

    if (!emmc_wait_int(INT_CMD_DONE, timeout_us)) return 0;

    emmc_resp[0] = emmc_rd(EMMC_RESP0);
    emmc_resp[1] = emmc_rd(EMMC_RESP1);
    emmc_resp[2] = emmc_rd(EMMC_RESP2);
    emmc_resp[3] = emmc_rd(EMMC_RESP3);
    return 1;
}

/* CMD55 then the application command. The card only accepts an ACMD in
 * the window immediately after CMD55, so the two are always issued as a
 * pair and never separated. */
static int emmc_app_command(uint32_t code, uint32_t arg, uint64_t timeout_us) {
    if (!emmc_command(CMD_APP, emmc_rca << 16, 100000)) return 0;
    return emmc_command(code, arg, timeout_us);
}

/* ---- data transfer ---- */

static int emmc_transfer(uint64_t lba, uint32_t count, void *buf, int write) {
    if (!emmc_present || count == 0) return -1;
    uint32_t *p = (uint32_t *)buf;

    /* An SDHC or SDXC card is addressed in 512-byte blocks; a standard
     * capacity card is addressed in bytes. Getting this backwards reads
     * sector 0 for every request on one kind of card and fails
     * immediately on the other. */
    uint32_t addr = emmc_sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);

    emmc_wr(EMMC_BLKSIZECNT, (count << 16) | 512);

    uint32_t code;
    if (write) code = (count > 1) ? CMD_WRITE_MULTI : CMD_WRITE_SINGLE;
    else       code = (count > 1) ? CMD_READ_MULTI  : CMD_READ_SINGLE;

    if (!emmc_command(code, addr, 1000000)) {
        emmc_log(write ? "write command rejected" : "read command rejected");
        return -1;
    }

    for (uint32_t b = 0; b < count; b++) {
        if (!emmc_wait_int(write ? INT_WRITE_RDY : INT_READ_RDY, 1000000)) {
            emmc_log("block never became ready");
            return -1;
        }
        if (write) for (int i = 0; i < 128; i++) emmc_wr(EMMC_DATA, *p++);
        else       for (int i = 0; i < 128; i++) *p++ = emmc_rd(EMMC_DATA);
    }

    /* A multi-block transfer ends with a data-done interrupt; auto-CMD12
     * has already stopped the card by then. */
    if (!emmc_wait_int(INT_DATA_DONE, 2000000)) {
        emmc_log("transfer never completed");
        return -1;
    }
    return 0;
}

static int emmc_read(uint64_t lba, uint32_t count, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    while (count) {
        /* The block count field is sixteen bits, and a card is happier
         * with runs it can stream than with one enormous request. */
        uint32_t n = count > 1024 ? 1024 : count;
        if (emmc_transfer(lba, n, p, 0) != 0) return -1;
        lba += n; count -= n; p += (uint64_t)n * 512;
    }
    return 0;
}

static int emmc_write(uint64_t lba, uint32_t count, const void *buf) {
    uint8_t *p = (uint8_t *)(uintptr_t)buf;
    while (count) {
        uint32_t n = count > 1024 ? 1024 : count;
        if (emmc_transfer(lba, n, p, 1) != 0) return -1;
        lba += n; count -= n; p += (uint64_t)n * 512;
    }
    return 0;
}

/*
 * There is nothing to flush.
 *
 * A card commits a block before it releases the data line, and this
 * driver waits for that on every write — so by the time a write returns,
 * it is on the card. Saying so is better than issuing a command that
 * does nothing in order to look symmetrical with the ATA driver.
 */
static int emmc_flush(void) { return emmc_present ? 0 : -1; }

/* ---- bring-up ---- */

static int emmc_reset(void) {
    emmc_wr(EMMC_CONTROL0, 0);
    emmc_wr(EMMC_CONTROL1, emmc_rd(EMMC_CONTROL1) | C1_SRST_HC);
    if (!emmc_wait_reg(EMMC_CONTROL1, C1_SRST_HC, 0, 500000)) {
        emmc_log("controller would not reset");
        return 0;
    }

    /*
     * Where the base clock comes from.
     *
     * The capability register is supposed to report it and on a Pi
     * frequently reports zero, because the VideoCore owns the clock tree
     * and the SDHCI block genuinely does not know its own input. Asking
     * the firmware is the reliable path; the capability register is the
     * fallback, and 100 MHz — the Pi 4's emmc2 default — is the last
     * resort, marked as the guess it is.
     */
    uint32_t clk_id = MBOX_CLOCK_EMMC2;
    emmc_base_clock = mbox_get_clock(clk_id);
    if (!emmc_base_clock) {
        clk_id = MBOX_CLOCK_EMMC;
        emmc_base_clock = mbox_get_clock(clk_id);
    }

    /*
     * Ask for the fastest input the firmware will give.
     *
     * The card clock is this divided down, so a low base caps the card
     * no matter what divider is chosen — and the firmware's default for
     * emmc2 depends on config.txt and on whether anything has asked
     * before. Requesting the maximum costs one message and is refused
     * harmlessly on a board that does not allow it, in which case the
     * value already read still stands.
     */
    if (emmc_base_clock) {
        uint32_t max = mbox_max_clock(clk_id);
        if (max > emmc_base_clock) {
            uint32_t got = mbox_set_clock(clk_id, max);
            if (got) emmc_base_clock = got;
        }
    }

    if (!emmc_base_clock) {
        uint32_t caps = emmc_rd(EMMC_CAPABILITIES);
        emmc_base_clock = ((caps >> 8) & 0xFF) * 1000000u;
    }
    if (!emmc_base_clock) {
        emmc_base_clock = 100000000u;
        emmc_log("base clock unknown, assuming 100 MHz");
    }

    /* Identification runs at 400 kHz: every card must answer at that
     * speed, and the faster modes are only legal once one has said what
     * it is. */
    if (!emmc_set_clock(400000)) return 0;

    emmc_wr(EMMC_IRPT_EN, 0);                  /* polled, never signalled */
    emmc_wr(EMMC_IRPT_MASK, 0xFFFFFFFFu);      /* but all bits visible    */
    emmc_wr(EMMC_INTERRUPT, 0xFFFFFFFFu);
    return 1;
}

static int emmc_card_init(void) {
    if (!emmc_command(CMD_GO_IDLE, 0, 100000)) {
        emmc_log("card did not go idle");
        return 0;
    }

    /*
     * CMD8 states the host's voltage range and a pattern the card must
     * echo back. A card that answers is version 2 or later and may be
     * high capacity; a card that does not is version 1, which this
     * driver still handles — it simply never sets the HCS bit below.
     */
    int v2 = emmc_command(CMD_SEND_IF_COND, 0x1AA, 100000) &&
             (emmc_resp[0] & 0xFFF) == 0x1AA;

    /*
     * ACMD41 is a request, not a command: the card answers "not yet"
     * until its internal supply has come up, and the host is expected to
     * keep asking. Bit 31 of the response is the card saying it is done.
     * A second is generous; a card that needs longer is failing.
     */
    uint32_t ocr = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t arg = 0x00FF8000u;            /* every voltage we support */
        if (v2) arg |= (1u << 30);             /* host supports high capacity */
        if (!emmc_app_command(ACMD_SEND_OP_COND, arg, 100000)) {
            emmc_delay_us(1000);
            continue;
        }
        ocr = emmc_resp[0];
        if (ocr & 0x80000000u) break;
        emmc_delay_us(1000);
    }
    if (!(ocr & 0x80000000u)) {
        emmc_log("card never finished powering up");
        return 0;
    }
    emmc_sdhc = (ocr & (1u << 30)) != 0;

    if (!emmc_command(CMD_ALL_SEND_CID, 0, 100000)) {
        emmc_log("no card identification");
        return 0;
    }
    if (!emmc_command(CMD_SEND_REL_ADDR, 0, 100000)) {
        emmc_log("card would not take an address");
        return 0;
    }
    emmc_rca = emmc_resp[0] >> 16;

    /*
     * Capacity, from the card-specific data.
     *
     * The 136-bit response arrives in four registers with the CRC byte
     * stripped, which shifts everything eight bits from where the
     * specification's bit numbering says it is — the single most common
     * place to get this wrong. CSD version 2 (any SDHC or SDXC card)
     * stores a 22-bit size in units of 512 KB; version 1 stores a
     * device size and a size multiplier that have to be combined.
     */
    if (!emmc_command(CMD_SEND_CSD, emmc_rca << 16, 100000)) {
        emmc_log("no card-specific data");
        return 0;
    }
    uint32_t csd_ver = (emmc_resp[3] >> 22) & 0x3;
    if (csd_ver == 1) {
        uint32_t c_size = ((emmc_resp[1] >> 8) & 0x3FFFFF);
        emmc_sectors = ((uint64_t)c_size + 1) * 1024;
    } else {
        uint32_t c_size      = ((emmc_resp[2] & 0x3FF) << 2) |
                               ((emmc_resp[1] >> 30) & 0x3);
        uint32_t c_size_mult = (emmc_resp[1] >> 15) & 0x7;
        uint32_t read_bl_len = (emmc_resp[2] >> 16) & 0xF;
        uint64_t blocks = ((uint64_t)c_size + 1) * (1ULL << (c_size_mult + 2));
        emmc_sectors = blocks * (1ULL << read_bl_len) / 512;
    }
    if (emmc_sectors == 0) {
        emmc_log("card reports no capacity");
        return 0;
    }

    if (!emmc_command(CMD_SELECT_CARD, emmc_rca << 16, 500000)) {
        emmc_log("card would not be selected");
        return 0;
    }
    if (!emmc_command(CMD_SET_BLOCKLEN, 512, 100000)) {
        emmc_log("card refused a 512-byte block length");
        return 0;
    }

    /*
     * Four data lines instead of one, which is four times the bandwidth
     * for one command. Not fatal if refused — the card keeps working on
     * a single line, just slowly — so this reports and carries on rather
     * than failing the whole probe.
     */
    if (emmc_app_command(ACMD_SET_BUS_WIDTH, 2, 100000))
        emmc_wr(EMMC_CONTROL0, emmc_rd(EMMC_CONTROL0) | (1u << 1));
    else
        emmc_log("staying on a one-bit bus");

    /* 25 MHz is default speed, which every card supports without the
     * high-speed switch this driver does not implement. */
    emmc_set_clock(25000000);
    return 1;
}

static int emmc_init(void) {
    emmc_present = 0;
    if (!bcm_emmc_base) return 0;

    /* The firmware powers the slot down when nothing has asked for it.
     * Not fatal if the call is refused — a board booted through UEFI has
     * usually had it on since the boot loader read the ESP. */
    mbox_power_on(MBOX_POWER_SD);

    if (!emmc_reset()) return 0;
    if (!emmc_card_init()) return 0;

    emmc_present = 1;
    serial_puts("[emmc] SD card: ");
    serial_put_u64(emmc_sectors / 2048);
    serial_puts(" MB, ");
    serial_puts(emmc_sdhc ? "block-addressed (SDHC/SDXC)\n"
                          : "byte-addressed (standard capacity)\n");
    return 1;
}

#endif /* EMMC_H */
