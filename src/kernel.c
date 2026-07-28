#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "arm.h"
#include "virtio.h"
#include "vtinput.h"
#include "ttf.h"
#include "login.h"
#include "boot_animation.h"

/*
 * Socrates BSD 9 for ARM64 — milestone 1.
 *
 * The machine now has a console, exception vectors, a time source and a
 * paced render loop, which is enough for the whole portable rendering
 * stack to come back: the boot animation, the TrueType rasteriser, and
 * the login screen's demoscene vortex. All of that compiled for aarch64
 * without a single change — it was written integer-only and reads every
 * multi-byte value one byte at a time.
 *
 * What is deliberately *not* here yet is an interrupt controller. The
 * x86 build needs the PIT and IRQ0 to pace itself; aarch64 exposes a
 * monotonic counter in a system register, so the loop can wait on real
 * elapsed time with no controller, no vector and no handler. Building
 * the GIC before a device needs it would be shipping untested code, so
 * it arrives with the first driver that actually raises an interrupt.
 *
 * Input is stubbed at this milestone (see the note by the globals), so
 * the login screen animates but cannot yet be typed into. That is M2.
 */

/* ---- Limine requests ---- */

__attribute__((used, section(".limine_reqs_start")))
static volatile uint64_t start_marker[] = LIMINE_REQUESTS_START_MARKER;

/*
 * Base revision 6, not the 3 the x86 tree asks for: Limine has dropped
 * the older revisions on aarch64 and refuses to load anything below it.
 */
__attribute__((used, section(".limine_reqs")))
static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_reqs_end")))
static volatile uint64_t end_marker[] = LIMINE_REQUESTS_END_MARKER;

/* Freestanding C runtime helpers — the compiler emits calls to these */
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *p = d; const uint8_t *q = s;
    while (n--) *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *p = d; const uint8_t *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

/* ---- framebuffer ---- */

#ifndef BUF_MAX_W
#define BUF_MAX_W 1920
#endif
#ifndef BUF_MAX_H
#define BUF_MAX_H 1080
#endif

static uint32_t backbuf[BUF_MAX_W * BUF_MAX_H];
static uint32_t prevbuf[BUF_MAX_W * BUF_MAX_H];
static int      prev_valid = 0;

#define COLOR_BLACK 0x000000u
#define COLOR_GOLD  0xD4AF37u

static void halt_forever(void) {
    for (;;) __asm__ volatile("wfi");
}

/*
 * Stop the kernel at a numbered point, chosen at build time.
 *
 * Bisecting this port by print statement does not work, because the two
 * failures being chased both destroy the evidence: qemu's hvf backend
 * aborts the process on an MMIO access it cannot decode, and a dying qemu
 * never flushes the serial chardev, so the last several lines the kernel
 * wrote are simply gone. Twice that truncated tail was mistaken for the
 * place execution stopped, and both times the real fault was further on.
 *
 * Halting is immune to that. If the machine is still running after a
 * checkpoint, execution reached it — no output required, and nothing to
 * lose in a buffer. Build with -DHALT_AT=n to stop at checkpoint n.
 */
#ifndef HALT_AT
#define HALT_AT 0
#endif
#define CHK(n)                                                        \
    do {                                                              \
        if ((HALT_AT) == (n)) {                                       \
            serial_puts("[socrates/arm64] halted at checkpoint " #n "\n"); \
            halt_forever();                                           \
        }                                                             \
    } while (0)

/*
 * Present the back buffer.
 *
 * Carries over the x86 build's row-skipping: comparing two rows in RAM
 * is far cheaper than writing one to the panel, and on a mostly-still
 * screen almost every row is unchanged.
 */
static int row_same(const uint32_t *a, const uint32_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static void vga_flip(volatile uint32_t *vram,
                     uint32_t w, uint32_t h, uint32_t pitch_px) {
    for (uint32_t row = 0; row < h; row++) {
        const uint32_t *src = backbuf + row * w;
        uint32_t       *cmp = prevbuf + row * w;
        if (prev_valid && row_same(src, cmp, w)) continue;
        volatile uint32_t *dst = vram + row * pitch_px;
        for (uint32_t col = 0; col < w; col++) {
            dst[col] = src[col];
            cmp[col] = src[col];
        }
    }
    prev_valid = 1;
    /* the panel is device memory; make sure the writes have left */
    DSB();
}

static void fill_rect(uint32_t w, uint32_t x, uint32_t y,
                      uint32_t rw, uint32_t rh, uint32_t color) {
    for (uint32_t row = y; row < y + rh; row++)
        for (uint32_t col = x; col < x + rw; col++)
            backbuf[row * w + col] = color;
}

/* ---- boot animation ---- */

/*
 * Renders through the back buffer rather than straight at the panel.
 *
 * The x86 version writes each scaled pixel directly into video memory.
 * That is a lot of small stores into a mapping the hypervisor has to
 * emulate, and QEMU's hvf backend aborts outright on an MMIO access whose
 * instruction syndrome it cannot decode — which is what a tight loop of
 * adjacent stores can compile into. Building the frame in normal memory
 * and blitting it once is both what every other part of this system
 * already does and far less work at the boundary.
 */
static void display_boot_animation(volatile uint32_t *vram,
                                   uint32_t scr_w, uint32_t scr_h,
                                   uint32_t pitch_px) {
    uint32_t scale_x = scr_w / BOOT_ANIM_W;
    uint32_t scale_y = scr_h / BOOT_ANIM_H;
    uint32_t scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale == 0) scale = 1;

    uint32_t dst_w = BOOT_ANIM_W * scale;
    uint32_t dst_h = BOOT_ANIM_H * scale;
    uint32_t off_x = (scr_w - dst_w) / 2;
    uint32_t off_y = (scr_h - dst_h) / 2;

    const uint16_t *frames = (const uint16_t *)boot_anim_data;

    for (uint32_t i = 0; i < scr_w * scr_h; i++) backbuf[i] = 0;

    /* 24 fps, measured against the architected counter rather than a
     * programmed one-shot — no PIT to set up and no channel to gate. */
    uint64_t frame_ticks = timer_hz() / 24;
    uint64_t next = timer_count();

    for (uint32_t f = 0; f < BOOT_ANIM_FRAME_COUNT; f++) {
        const uint16_t *src = frames + f * BOOT_ANIM_W * BOOT_ANIM_H;

        for (uint32_t sy = 0; sy < BOOT_ANIM_H; sy++) {
            for (uint32_t sx = 0; sx < BOOT_ANIM_W; sx++) {
                uint16_t c = src[sy * BOOT_ANIM_W + sx];
                uint32_t r = (c >> 11) & 0x1F;
                uint32_t g = (c >> 5)  & 0x3F;
                uint32_t b = c & 0x1F;
                uint32_t pixel = (r << 19) | (g << 10) | (b << 3);

                for (uint32_t dy = 0; dy < scale; dy++)
                    for (uint32_t dx = 0; dx < scale; dx++)
                        backbuf[(off_y + sy * scale + dy) * scr_w +
                                (off_x + sx * scale + dx)] = pixel;
            }
        }
        vga_flip(vram, scr_w, scr_h, pitch_px);

        next += frame_ticks;
        timer_wait_until(next);
    }

    for (uint32_t i = 0; i < scr_w * scr_h; i++) backbuf[i] = 0;
    vga_flip(vram, scr_w, scr_h, pitch_px);
}

void kmain(void) {
    /*
     * Before anything, including the first character of output. The UART
     * is a device register, device registers are not mapped by anything
     * Limine set up, and an unmapped access this early faults into a
     * vector table that has not been installed yet. Printing first is not
     * an option — this has to be the first statement in the kernel.
     */
    mmio_map_init();

    serial_puts("\n[socrates/arm64] kmain reached at EL1\n");

    /* Vectors first: from here on a fault says what it was instead of
     * hanging, which matters more the more driver code arrives. */
    exceptions_init();
    timer_takeover();
    fpu_init();
    serial_puts("[socrates/arm64] vectors installed, timer disarmed, FP on\n");
    mmu_report();

    CHK(1);
    if (fb_request.response == NULL ||
        fb_request.response->framebuffer_count < 1) {
        serial_puts("[socrates/arm64] no framebuffer from Limine\n");
        halt_forever();
    }

    struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
    uint32_t panel_w  = (uint32_t)fb->width;
    uint32_t panel_h  = (uint32_t)fb->height;
    uint32_t pitch_px = (uint32_t)(fb->pitch / (fb->bpp / 8));
    volatile uint32_t *vram = (volatile uint32_t *)fb->address;

    serial_puts("[socrates/arm64] fb addr "); serial_put_hex64((uint64_t)(uintptr_t)vram);
    serial_puts(" pitch "); serial_put_u64(fb->pitch);
    serial_puts(" bpp "); serial_put_u64(fb->bpp);
    serial_puts("\n[socrates/arm64] fb PAR "); serial_put_hex64(mmu_probe_write((uint64_t)(uintptr_t)vram));
    serial_puts("\n");
    CHK(2);

    for (uint32_t row = 0; row < panel_h; row++)
        for (uint32_t col = 0; col < panel_w; col++)
            vram[row * pitch_px + col] = 0;

    CHK(3);

    uint32_t w = panel_w > BUF_MAX_W ? BUF_MAX_W : panel_w;
    uint32_t h = panel_h > BUF_MAX_H ? BUF_MAX_H : panel_h;

    serial_puts("[socrates/arm64] framebuffer ");
    serial_put_u64(panel_w); serial_puts("x"); serial_put_u64(panel_h);
    serial_puts("  timer "); serial_put_u64(timer_hz() / 1000000);
    serial_puts(" MHz\n");

    display_boot_animation(vram, w, h, pitch_px);
    serial_puts("[socrates/arm64] boot animation done\n");

    vtinput_init((int32_t)w, (int32_t)h);

    /*
     * Render loop, paced against the counter.
     *
     * The x86 version sleeps on hlt and is woken by IRQ0; with no
     * interrupt controller yet this waits on elapsed time directly,
     * which is both simpler and more honest about the frame rate — the
     * clock cannot drift just because a frame ran long.
     */
    uint64_t frame_ticks = timer_hz() / 60;
    uint64_t next_frame = timer_count();
    uint64_t frames = 0;
    uint64_t last_report_ms = timer_ms();

    /* What has been typed so far. The login screen renders it as dots;
     * checking it against a password is desktop.h's job, and arrives with
     * the milestone that brings the desktop up. */
    char pw[64] = {0};
    int  pw_len = 0;

    int32_t last_mx = -1, last_my = -1;
    uint8_t last_mb = 0;


    for (;;) {
        /* Poll phase. Devices are drained once per frame rather than from
         * an interrupt: the loop already visits everything every 16 ms,
         * and a used-ring index costs less to read than an interrupt
         * costs to route. */
        vtinput_poll();

        /* Report the pointer only when it moves. A per-frame log would
         * bury everything else at 60 Hz, and the interesting question —
         * does a host coordinate arrive as the same coordinate here — is
         * about transitions, not steady state. */
        if (mouse_x != last_mx || mouse_y != last_my ||
            mouse_buttons != last_mb) {
            last_mx = mouse_x; last_my = mouse_y; last_mb = mouse_buttons;
            serial_puts("[socrates/arm64] pointer ");
            serial_put_u64((uint64_t)(uint32_t)last_mx);
            serial_puts(",");
            serial_put_u64((uint64_t)(uint32_t)last_my);
            serial_puts(" buttons ");
            serial_put_u64(last_mb);
            serial_puts("\n");
        }

        /* Echo what has been typed, so the field fills in as keys land. */
        char ch;
        while ((ch = kb_getchar()) != 0) {
            if (ch == '\b') {
                if (pw_len > 0) pw_len--;
            } else if (ch == '\n') {
                pw_len = 0;
            } else if (ch >= 0x20 && ch < 0x7F && pw_len < (int)sizeof(pw) - 1) {
                pw[pw_len++] = ch;
            }
            pw[pw_len] = '\0';
        }

        login_render(backbuf, w, h, mouse_x, mouse_y, pw,
                     mouse_buttons, "Socrates BSD 9 - ARM64");
        if (frames == 0) CHK(4);

        fill_rect(w, 0,     0,     w, 1, COLOR_GOLD);
        fill_rect(w, 0,     h - 1, w, 1, COLOR_GOLD);
        fill_rect(w, 0,     0,     1, h, COLOR_GOLD);
        fill_rect(w, w - 1, 0,     1, h, COLOR_GOLD);

        vga_flip(vram, w, h, pitch_px);
        CHK(5);

        frames++;
        if (frames % 120 == 0) {
            /* Interval, not total. Dividing by time since boot reports the
             * average over the whole run, which converges and stops
             * showing what the renderer is doing now — the number worth
             * watching while a driver lands underneath it. */
            uint64_t now = timer_ms();
            uint64_t ms  = now - last_report_ms;
            last_report_ms = now;
            serial_puts("[socrates/arm64] 120 frames in ");
            serial_put_u64(ms);
            serial_puts(" ms (");
            serial_put_u64(ms ? (120 * 1000) / ms : 0);
            serial_puts(" fps)\n");
        }

        next_frame += frame_ticks;
        timer_wait_until(next_frame);
    }
}
