#ifndef PIFB_H
#define PIFB_H

#include <stdint.h>
#include "mbox.h"

/*
 * A framebuffer from the VideoCore, asked for directly.
 *
 * The kernel already has two ways to get a screen: virtio-gpu, which it
 * drives itself, and whatever Limine hands over from the firmware's
 * graphics protocol. On a Raspberry Pi neither is guaranteed. There is
 * no virtio anything, and a UEFI GOP exists only if the board is booted
 * through the UEFI firmware rather than the stock one — which is the
 * normal way to boot this OS on a Pi, but not the only one, and not the
 * one that works if the ESP is set up wrong.
 *
 * This is the path that does not depend on any of that. The VideoCore
 * owns the display controller outright; there are no mode registers to
 * program, no timings to compute, no EDID to parse. You send one message
 * describing the mode you want and the firmware replies with a pointer.
 * That is genuinely simpler than every other display path in this
 * project, and it is the only one that works on a Pi booted bare.
 *
 * The eight tags below go in a single message on purpose. Sent
 * separately, the firmware applies each as it arrives and briefly holds
 * a state nobody asked for — a 32-bit depth against the previous
 * resolution, say — and on some firmware revisions the allocation that
 * follows is sized from it. One message, one atomic change.
 */

static volatile uint32_t *pifb_addr = 0;
static uint32_t pifb_w = 0, pifb_h = 0, pifb_pitch_px = 0;

/*
 * The reply's pointer is a VideoCore bus address, not an ARM one.
 *
 * Bits 30 and 31 select the GPU's view of memory — cached alias,
 * uncached alias, and so on — and none of them mean anything to the ARM
 * core's MMU. Masking them off yields the physical address, which is
 * where the pixels actually are. A kernel that writes to the bus address
 * unmodified is writing 0xC0000000 bytes past the framebuffer, into
 * memory that on a 1 GB board does not exist.
 */
static uint64_t pifb_bus_to_phys(uint32_t bus) {
    return (uint64_t)(bus & 0x3FFFFFFFu);
}

static int pifb_init(uint32_t want_w, uint32_t want_h) {
    pifb_addr = 0;
    if (!mbox_present()) return 0;

    /*
     * A resolution of zero means "whatever the display is already
     * doing", which the firmware answers with the mode it detected from
     * the monitor. Asking for a specific size and being refused is worse
     * than asking for nothing and being told, so a zero request is the
     * caller's way of deferring to the panel.
     */
    if (want_w == 0 || want_h == 0) { want_w = 1024; want_h = 768; }

    mbox_buf[0]  = 35 * 4;
    mbox_buf[1]  = MBOX_REQUEST;

    mbox_buf[2]  = MBOX_TAG_FB_SET_PHYS;   /* display size              */
    mbox_buf[3]  = 8;
    mbox_buf[4]  = 8;
    mbox_buf[5]  = want_w;
    mbox_buf[6]  = want_h;

    mbox_buf[7]  = MBOX_TAG_FB_SET_VIRT;   /* framebuffer size          */
    mbox_buf[8]  = 8;
    mbox_buf[9]  = 8;
    mbox_buf[10] = want_w;
    mbox_buf[11] = want_h;

    mbox_buf[12] = MBOX_TAG_FB_SET_OFFSET; /* no panning                */
    mbox_buf[13] = 8;
    mbox_buf[14] = 8;
    mbox_buf[15] = 0;
    mbox_buf[16] = 0;

    mbox_buf[17] = MBOX_TAG_FB_SET_DEPTH;
    mbox_buf[18] = 4;
    mbox_buf[19] = 4;
    mbox_buf[20] = 32;

    /*
     * Pixel order 1 is RGB, 0 is BGR. The whole renderer above this
     * packs 0x00RRGGBB into a 32-bit word, which on a little-endian
     * machine puts blue in the low byte — so the framebuffer must be
     * BGR-ordered for those words to land as the colours they name. The
     * firmware's default flips between board revisions, which is why it
     * is set rather than assumed.
     */
    mbox_buf[21] = MBOX_TAG_FB_SET_PIXORDER;
    mbox_buf[22] = 4;
    mbox_buf[23] = 4;
    mbox_buf[24] = 0;                      /* BGR */

    mbox_buf[25] = MBOX_TAG_FB_ALLOC;
    mbox_buf[26] = 8;
    mbox_buf[27] = 8;
    mbox_buf[28] = 16;                     /* requested alignment, in    */
    mbox_buf[29] = 0;                      /* reply: address, then size  */

    mbox_buf[30] = MBOX_TAG_FB_GET_PITCH;
    mbox_buf[31] = 4;
    mbox_buf[32] = 4;
    mbox_buf[33] = 0;

    mbox_buf[34] = MBOX_TAG_LAST;

    if (!mbox_call(MBOX_CH_PROP)) {
        serial_puts("[pifb] firmware did not answer\n");
        return 0;
    }

    uint32_t got_w  = mbox_buf[5];
    uint32_t got_h  = mbox_buf[6];
    uint32_t bus    = mbox_buf[28];
    uint32_t size   = mbox_buf[29];
    uint32_t pitch  = mbox_buf[33];

    if (!bus || !size || !pitch || !got_w || !got_h) {
        serial_puts("[pifb] firmware allocated nothing\n");
        return 0;
    }

    uint64_t phys = pifb_bus_to_phys(bus);

    /*
     * The framebuffer is carved out of the GPU's share of memory, which
     * is deliberately *not* in the firmware's map of what the ARM may
     * use — so nothing has mapped it and nothing will unless it is asked
     * for by name. Registering it as a device region rather than as RAM
     * is the honest description: it is memory the CPU does not own, and
     * mapping it Normal-cacheable would leave frames sitting in a
     * write-back cache the display controller cannot see.
     */
    mmio_region_add(phys, size);
    mmio_map_init();

    pifb_addr     = (volatile uint32_t *)(uintptr_t)phys;
    pifb_w        = got_w;
    pifb_h        = got_h;
    pifb_pitch_px = pitch / 4;

    serial_puts("[pifb] ");
    serial_put_u64(got_w);
    serial_puts("x");
    serial_put_u64(got_h);
    serial_puts(" at ");
    serial_put_hex64(phys);
    serial_puts(", pitch ");
    serial_put_u64(pifb_pitch_px);
    serial_puts(" px\n");
    return 1;
}

#endif /* PIFB_H */
