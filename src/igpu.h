#ifndef IGPU_H
#define IGPU_H

#include <stdint.h>
#include "arm.h"

/*
 * There is no integrated GPU on this machine, and this file says so.
 *
 * The x86 tree drives an Intel Gen9 blitter: BAR probing, a GGTT window,
 * a command ring, hardware status pages, hang detection and a register
 * dump good enough to name the offending command. None of that has a
 * counterpart on qemu's `virt` machine, which offers a plain linear
 * framebuffer and nothing to submit work to, so the driver is dropped
 * rather than ported. The original is kept as igpu_x86.h.ref.
 *
 * What survives is the shape. term.h's `gpu` commands are ordinary
 * diagnostics that read these fields, and rewriting them to be
 * conditionally absent would fork a 1,400-line file across two
 * architectures to save one struct. Declaring the struct inert instead
 * keeps term.h byte-identical with the x86 tree and makes the commands
 * tell the truth: they report "no integrated GPU on this machine" because
 * `active` is zero and `status` says so.
 *
 * The CPU renderer in gfx.h is what actually draws on both trees. The
 * x86 igpu path is an accelerator that already bails out cleanly when
 * probing fails, so nothing above this notices its absence.
 */

/* Error bits the crash dump decodes. Kept so the decoder compiles; no
 * hardware here ever sets them. */
#define IGPU_ERR_INSTRUCTION  (1u << 0)
#define IGPU_ERR_PAGE_TABLE   (1u << 4)
#define IGPU_ERR_MEM_REFRESH  (1u << 1)
#define IGPU_ERR_PRIV         (1u << 2)

static struct {
    int active;
    const char *name;
    const char *status;
    uint16_t device_id;
    volatile uint8_t *mmio;
    volatile uint8_t *ggtt;
    uint64_t mmio_size;
    uint64_t aperture_base;
    uint64_t aperture_size;
    uint32_t ggtt_slots;
    uint32_t base_slot;
    uint32_t ring_gpu, hws_gpu, status_gpu, target_gpu;
    uint32_t ring_tail;
    uint32_t seqno;
    int      fb_blittable;
    uint32_t fb_gpu_addr;
    uint64_t fb_phys;
    uint32_t fb_pitch_bytes;
    uint32_t fb_w, fb_h;
} igpu = {
    .active = 0,
    .name   = "none",
    .status = "no integrated GPU on this machine (aarch64/virt)"
};

static struct {
    int      valid;
    int      hang_count;
    int      reset_ok;
    uint32_t seqno_expected, seqno_seen;
    uint32_t eir, esr, instdone;
    uint32_t ipeir, ipehr;
    uint32_t acthd_lo, acthd_hi;
    uint32_t ring_head, ring_tail, ring_ctl, ring_start;
    uint32_t fault_reg, fault_tlb0, fault_tlb1;
    uint32_t hws[8];
    uint32_t ring_window[8];
    char     cmd_name[48];
} igpu_crash;

static void igpu_decode_cmd(uint32_t dw, char *out, uint32_t out_max) {
    (void)dw;
    const char *s = "n/a";
    uint32_t i = 0;
    while (s[i] && i + 1 < out_max) { out[i] = s[i]; i++; }
    if (out_max) out[i] = '\0';
}

/* Always fails: there is no blitter to hand the rectangle to, and
 * returning success would leave the caller's verification comparing
 * against pixels nothing wrote. */
static int igpu_screen_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint32_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
    return -1;
}

static void igpu_init(void) { }

#endif /* IGPU_H */
