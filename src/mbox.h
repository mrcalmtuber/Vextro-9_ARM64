#ifndef MBOX_H
#define MBOX_H

#include <stdint.h>
#include "arm.h"

/*
 * The VideoCore property mailbox.
 *
 * A Raspberry Pi is not really an ARM computer with a GPU attached. It
 * is a VideoCore computer with an ARM core attached, and the VideoCore
 * boots first, owns the clocks, owns the power rails, and owns the
 * display. The ARM side asks for things politely through a mailbox and
 * the firmware decides.
 *
 * That is a genuinely different arrangement from anything on the x86
 * tree, where a driver programs the hardware directly and the only
 * negotiation is with a BIOS that has already got out of the way. Here,
 * asking is the interface. There is no register that sets the SD card
 * clock; there is a message you send to the firmware asking it to.
 *
 * The protocol is one 32-bit word per direction. The word is a physical
 * address with the channel number in its low four bits, which works
 * because every buffer is 16-byte aligned and the low bits are therefore
 * always zero — a small, ugly, effective trick that the whole platform
 * rests on.
 *
 * The buffer itself is a sequence of tags, each a request the firmware
 * answers in place: it overwrites the request with the response and sets
 * the high bit of the length word to say it did. So the reply arrives in
 * the same memory the question was written into, and a tag that comes
 * back without that bit set was not understood.
 *
 * Cache is the trap. The firmware reads this buffer with a DMA engine
 * that does not see the ARM's caches, so the buffer must be written back
 * before the doorbell and invalidated after — and skipping either gives
 * you a reply that is either ignored or read as whatever was in the line
 * beforehand. The mailbox is aliased through an uncached window to make
 * that impossible to get wrong.
 */

#define MBOX_READ    0x00
#define MBOX_STATUS  0x18
#define MBOX_WRITE   0x20

#define MBOX_FULL    0x80000000u
#define MBOX_EMPTY   0x40000000u

#define MBOX_CH_PROP 8          /* ARM -> VideoCore, property tags */

/* ---- request codes ---- */
#define MBOX_REQUEST         0x00000000u
#define MBOX_RESP_OK         0x80000000u

#define MBOX_TAG_GET_FIRMWARE    0x00000001u
#define MBOX_TAG_GET_BOARD_REV   0x00010002u
#define MBOX_TAG_GET_MAC         0x00010003u
#define MBOX_TAG_GET_ARM_MEMORY  0x00010005u
#define MBOX_TAG_SET_POWER       0x00028001u
#define MBOX_TAG_GET_CLOCK_RATE  0x00030002u
#define MBOX_TAG_SET_CLOCK_RATE  0x00038002u
#define MBOX_TAG_GET_MAX_CLOCK   0x00030004u

#define MBOX_TAG_FB_ALLOC        0x00040001u
#define MBOX_TAG_FB_RELEASE      0x00048001u
#define MBOX_TAG_FB_GET_PHYS     0x00040003u
#define MBOX_TAG_FB_SET_PHYS     0x00048003u
#define MBOX_TAG_FB_SET_VIRT     0x00048004u
#define MBOX_TAG_FB_SET_DEPTH    0x00048005u
#define MBOX_TAG_FB_SET_PIXORDER 0x00048006u
#define MBOX_TAG_FB_SET_OFFSET   0x00048009u
#define MBOX_TAG_FB_GET_PITCH    0x00040008u

#define MBOX_TAG_LAST            0x00000000u

#define MBOX_CLOCK_EMMC   0x01
#define MBOX_CLOCK_UART   0x02
#define MBOX_CLOCK_ARM    0x03
#define MBOX_CLOCK_CORE   0x04
#define MBOX_CLOCK_EMMC2  0x0C

#define MBOX_POWER_SD     0x00
#define MBOX_POWER_USB    0x03

/*
 * The message buffer. Sixteen-byte aligned because the low four bits of
 * the doorbell word carry the channel and must therefore be free, and
 * generously sized because a framebuffer request chains eight tags.
 */
#define MBOX_WORDS 40
static volatile uint32_t mbox_buf[MBOX_WORDS] __attribute__((aligned(16)));

static int mbox_present(void) { return bcm_mbox_base != 0; }

static inline volatile uint32_t *mbox_reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(bcm_mbox_base + off);
}

/*
 * The address the VideoCore must be given.
 *
 * It is not the ARM physical address. The GPU sees memory through its
 * own bus with an alias at 0xC0000000 that bypasses its L2 cache, and
 * that is the alias the firmware expects for a property buffer. On a Pi
 * 4 the low-peripheral layout makes the conversion a straight OR; the
 * older boards are the same for the addresses a kernel actually uses.
 *
 * Getting this wrong does not produce an error. The firmware simply
 * reads a different piece of memory, finds no valid tag, and never
 * replies — so the symptom is a mailbox call that times out, which looks
 * exactly like hardware that is not there.
 */
static uint32_t mbox_bus_addr(const volatile void *p) {
    uint64_t pa = virt_to_phys((const void *)(uintptr_t)p);
    if (!pa) return 0;
    return (uint32_t)(pa | 0xC0000000u);
}

/*
 * Push the buffer out of the caches, ring, wait, pull it back in.
 *
 * `dc civac` cleans and invalidates one cache line to the point of
 * coherency; the loop covers the whole buffer both before and after,
 * because the firmware's write is equally invisible to a stale line
 * sitting in this core's D-cache.
 */
static void mbox_flush(void) {
    uintptr_t start = (uintptr_t)mbox_buf & ~63UL;
    uintptr_t end   = (uintptr_t)mbox_buf + sizeof(mbox_buf);
    for (uintptr_t a = start; a < end; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    DSB();
}

/* Every wait here is bounded. A firmware that never answers must not be
 * able to hang the boot: the machine still has a serial console, and a
 * kernel spinning forever in a mailbox call has neither that nor a
 * screen to say so on. */
#define MBOX_SPIN 0x2000000

static int mbox_call(uint32_t channel) {
    if (!mbox_present()) return 0;

    uint32_t addr = mbox_bus_addr(mbox_buf);
    if (!addr) return 0;

    mbox_flush();

    int i;
    for (i = 0; i < MBOX_SPIN; i++)
        if (!(*mbox_reg(MBOX_STATUS) & MBOX_FULL)) break;
    if (i == MBOX_SPIN) return 0;

    DSB();
    *mbox_reg(MBOX_WRITE) = (addr & ~0xFu) | (channel & 0xF);

    /*
     * Replies for other channels can turn up first, so the loop reads
     * until it sees one whose low bits are the channel we asked on. A
     * driver that takes the first word it is handed works right up until
     * something else in the system uses a mailbox.
     */
    for (i = 0; i < MBOX_SPIN; i++) {
        if (*mbox_reg(MBOX_STATUS) & MBOX_EMPTY) continue;
        DSB();
        uint32_t r = *mbox_reg(MBOX_READ);
        if ((r & 0xF) != (channel & 0xF)) continue;
        mbox_flush();
        return mbox_buf[1] == MBOX_RESP_OK;
    }
    return 0;
}

/*
 * A one-tag property request.
 *
 * `nargs` words are sent and up to `nres` words come back in the same
 * place. This covers every call in the kernel except the framebuffer
 * set-up, which has to chain its tags into a single message so the
 * firmware sees mode, depth and pixel order as one atomic change.
 */
static int mbox_prop(uint32_t tag, const uint32_t *args, uint32_t nargs,
                     uint32_t *res, uint32_t nres) {
    uint32_t space = nargs > nres ? nargs : nres;
    if (space < 1) space = 1;
    if (6 + space > MBOX_WORDS) return 0;

    mbox_buf[0] = (6 + space) * 4;      /* total size in bytes */
    mbox_buf[1] = MBOX_REQUEST;
    mbox_buf[2] = tag;
    mbox_buf[3] = space * 4;            /* value buffer size   */
    mbox_buf[4] = nargs * 4;            /* request length      */
    for (uint32_t i = 0; i < space; i++)
        mbox_buf[5 + i] = i < nargs ? args[i] : 0;
    mbox_buf[5 + space] = MBOX_TAG_LAST;

    if (!mbox_call(MBOX_CH_PROP)) return 0;

    /* Bit 31 of the length word is the firmware saying it understood the
     * tag. Without checking it, an unsupported request reads back as
     * whatever was sent — which looks like a plausible answer. */
    if (!(mbox_buf[4] & 0x80000000u)) return 0;

    for (uint32_t i = 0; i < nres; i++) res[i] = mbox_buf[5 + i];
    return 1;
}

/* ---- the calls the kernel actually makes ---- */

static uint32_t mbox_firmware_rev(void) {
    uint32_t r = 0;
    return mbox_prop(MBOX_TAG_GET_FIRMWARE, 0, 0, &r, 1) ? r : 0;
}

static uint32_t mbox_board_rev(void) {
    uint32_t r = 0;
    return mbox_prop(MBOX_TAG_GET_BOARD_REV, 0, 0, &r, 1) ? r : 0;
}

/* Base and size of the memory the ARM side is allowed to use. The
 * firmware keeps the rest for the GPU, and the split is configurable in
 * config.txt — so this is a question, not a constant. */
static int mbox_arm_memory(uint32_t *base, uint32_t *size) {
    uint32_t r[2] = { 0, 0 };
    if (!mbox_prop(MBOX_TAG_GET_ARM_MEMORY, 0, 0, r, 2)) return 0;
    if (base) *base = r[0];
    if (size) *size = r[1];
    return 1;
}

static uint32_t mbox_get_clock(uint32_t id) {
    uint32_t a = id, r[2] = { 0, 0 };
    if (!mbox_prop(MBOX_TAG_GET_CLOCK_RATE, &a, 1, r, 2)) return 0;
    return r[1];
}

static uint32_t mbox_max_clock(uint32_t id) {
    uint32_t a = id, r[2] = { 0, 0 };
    if (!mbox_prop(MBOX_TAG_GET_MAX_CLOCK, &a, 1, r, 2)) return 0;
    return r[1];
}

static uint32_t mbox_set_clock(uint32_t id, uint32_t hz) {
    uint32_t a[3] = { id, hz, 0 }, r[2] = { 0, 0 };
    if (!mbox_prop(MBOX_TAG_SET_CLOCK_RATE, a, 3, r, 2)) return 0;
    return r[1];
}

/* Power a device on and wait for the firmware to say it is up: bit 0 is
 * the request, bit 1 asks it to block until stable. */
static int mbox_power_on(uint32_t device) {
    uint32_t a[2] = { device, 3 }, r[2] = { 0, 0 };
    if (!mbox_prop(MBOX_TAG_SET_POWER, a, 2, r, 2)) return 0;
    return (r[1] & 1) != 0;
}

/*
 * What the firmware says about the board it is running on.
 *
 * Worth printing at boot for a reason beyond curiosity: the memory split
 * is set in config.txt and is the one piece of the machine's shape that
 * the device tree does not describe, so a Pi that comes up with a
 * suspiciously small arena has usually given most of its RAM to the GPU.
 * Seeing it stated is the difference between diagnosing that in a minute
 * and hunting for a leak.
 */
static void mbox_report(void) {
    if (!mbox_present()) return;

    uint32_t fw = mbox_firmware_rev();
    uint32_t rev = mbox_board_rev();
    serial_puts("[mbox] firmware ");
    serial_put_hex32(fw);
    serial_puts("  board revision ");
    serial_put_hex32(rev);
    serial_puts("\n");

    uint32_t base = 0, size = 0;
    if (mbox_arm_memory(&base, &size)) {
        serial_puts("[mbox] ARM memory ");
        serial_put_u64(size / (1024 * 1024));
        serial_puts(" MB at ");
        serial_put_hex32(base);
        serial_puts("\n");
    } else {
        serial_puts("[mbox] firmware would not report the memory split\n");
    }
}

/* The board's own MAC address, so a Pi keeps the one printed on it
 * rather than inventing a different one on every boot. */
static int mbox_mac(uint8_t mac[6]) {
    uint32_t r[2] = { 0, 0 };
    if (!mbox_prop(MBOX_TAG_GET_MAC, 0, 0, r, 2)) return 0;
    mac[0] = (uint8_t)(r[0]);
    mac[1] = (uint8_t)(r[0] >> 8);
    mac[2] = (uint8_t)(r[0] >> 16);
    mac[3] = (uint8_t)(r[0] >> 24);
    mac[4] = (uint8_t)(r[1]);
    mac[5] = (uint8_t)(r[1] >> 8);
    return 1;
}

#endif /* MBOX_H */
