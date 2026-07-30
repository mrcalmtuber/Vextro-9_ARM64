#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "arm.h"
#include "virtio.h"
#include "vtinput.h"
#include "vtgpu.h"
#include "mbox.h"
#include "pifb.h"
#include "ata.h"
#include "gfx.h"                /* rtc_read: exFAT timestamps entries */
#include "exfat.h"
#include "e1000.h"
#include "netstack.h"
#include "igpu.h"
#include "llm.h"
#include "bsdload.h"
#include "desktop.h"
#include "ttf.h"
#include "login.h"
#include "boot_animation.h"

/*
 * Socrates BSD 9 for ARM64.
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
 * Input, storage and networking are real: virtio-input drives the
 * keyboard and an absolute pointer, virtio-blk carries an exFAT volume,
 * and virtio-net carries a stack that resolves names and fetches pages.
 * All three ride the same virtqueue layer, which is why the port needed
 * no PCIe bus walk to get here.
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

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_dtb_request dtb_request = {
    .id = LIMINE_DTB_REQUEST_ID,
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
static uint64_t present_px = 0;      /* pixels handed to the scanout   */
static uint32_t present_n  = 0;      /* frames counted                 */

#define COLOR_BLACK 0x000000u
#define COLOR_GOLD  0xD4AF37u

/* Report the root directory at boot. Listing real names off a real
 * volume is the only thing that proves the whole stack — queue, request,
 * FAT walk, directory-entry decoding — rather than just that a device
 * answered. */
static void boot_list_entry(const char *name, uint32_t size, int is_dir) {
    serial_puts("[socrates/arm64]   ");
    serial_puts(is_dir ? "dir  " : "file ");
    serial_puts(name);
    if (!is_dir) {
        serial_puts("  ");
        serial_put_u64(size);
        serial_puts(" bytes");
    }
    serial_puts("\n");
}


/*
 * The pointer, drawn by the kernel rather than the panel.
 *
 * ramfb has no hardware cursor plane and virtio-input reports positions
 * rather than moving one, so the arrow is composited into the back buffer
 * like everything else. Two colours: X is the outline, . the fill.
 */
static const char *CURSOR_IMG[18] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      XXX   ",
};

static void draw_cursor(uint32_t bw, uint32_t bh) {
    uint32_t cx = (uint32_t)mouse_x;
    uint32_t cy = (uint32_t)mouse_y;
    for (uint32_t row = 0; row < 18; row++) {
        const char *line = CURSOR_IMG[row];
        for (uint32_t col = 0; col < 12; col++) {
            char c = line[col];
            if (c == ' ') continue;
            uint32_t px = cx + col;
            uint32_t py = cy + row;
            if (px < bw && py < bh)
                backbuf[py * bw + px] = (c == 'X') ? 0x000000u : 0xFFFFFFu;
        }
    }
}

static void halt_forever(void) {
    for (;;) __asm__ volatile("wfi");
}

/* ---- the system call table ---- */

/*
 * Where a running app's pixels land.
 *
 * The x86 build draws into the terminal window's client area, which is
 * where an app's canvas belongs once there is a window manager to own
 * one. Until the desktop is wired up here, the canvas sits at a fixed
 * offset on screen. What the ABI actually promises — the syscall numbers,
 * the argument meanings, the clipping — is identical either way; moving
 * it into a window later changes two constants.
 */
static uint32_t app_canvas_x = 100, app_canvas_y = 150;
static uint32_t app_screen_w = 0,   app_screen_h = 0;
static int      app_wants_exit = 0;

#define APP_CANVAS_W 598
#define APP_CANVAS_H 402

/*
 * Called from the SVC trampoline in vectors.S.
 *
 * Ordinary C reached by an ordinary `bl`, with no interrupt attribute,
 * because the trampoline has already saved everything the AAPCS does not.
 * That is why the x86 build's __attribute__((interrupt)) — which GCC
 * implements only for x86, and which the port plan called its highest
 * risk — needed no counterpart here at all.
 */
uint64_t arm_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
    switch (num) {
    case 1: {                           /* sys_print */
        const char *s = (const char *)(uintptr_t)a0;
        if (!s) return (uint64_t)-1;
        serial_puts("[app] ");
        /* Bounded: the string comes from the app, and an unterminated one
         * would otherwise walk off the end of its window. */
        for (uint32_t i = 0; i < 4096 && s[i]; i++) serial_putc(s[i]);
        serial_putc('\n');
        return 0;
    }
    case 2: {                           /* sys_draw_pixel */
        uint32_t x = (uint32_t)a0, y = (uint32_t)a1;
        if (x >= APP_CANVAS_W || y >= APP_CANVAS_H) return 0;
        uint32_t px = app_canvas_x + x, py = app_canvas_y + y;
        if (px >= app_screen_w || py >= app_screen_h) return 0;
        backbuf[py * app_screen_w + px] = (uint32_t)a2;
        return 0;
    }
    case 3: {                           /* sys_get_mouse */
        int32_t *out = (int32_t *)(uintptr_t)a0;
        if (!out) return (uint64_t)-1;
        out[0] = mouse_x; out[1] = mouse_y;
        out[2] = (int32_t)mouse_buttons; out[3] = 0;
        return 0;
    }
    case 4:                             /* sys_exit */
        app_wants_exit = 1;
        return 0;
    default:
        serial_puts("[app] unknown syscall\n");
        return (uint64_t)-1;
    }
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
 * Push the back buffer at the panel, touching as little as possible.
 *
 * Three things happen in one pass, and folding them together is the
 * point: finding what changed, copying it, and recording where it was.
 *
 * The row scan used to answer only "is this row identical?" and then
 * copy the whole row if not. A moving pointer changes twelve pixels of a
 * 1280-wide row, so that copied a hundred times more than it needed to —
 * and, worse, it learned nothing it could pass on. Scanning inward from
 * both ends of the row costs the same comparisons, copies only the span
 * between them, and yields a bounding box for free.
 *
 * That box is what makes the difference on virtio-gpu, where presenting
 * is an explicit transfer of guest pixels into the host's copy of the
 * resource followed by a flush. Handing it the whole screen every frame
 * means moving four megabytes to show a cursor that moved four pixels.
 */
static void vga_flip(volatile uint32_t *vram,
                     uint32_t w, uint32_t h, uint32_t pitch_px) {
    uint32_t y0 = h, y1 = 0, x0 = w, x1 = 0;    /* dirty bounding box */

    for (uint32_t row = 0; row < h; row++) {
        const uint32_t *src = backbuf + row * w;
        uint32_t       *cmp = prevbuf + row * w;

        uint32_t c0 = 0, c1 = w;
        if (prev_valid) {
            while (c0 < w && src[c0] == cmp[c0]) c0++;
            if (c0 == w) continue;                  /* row unchanged */
            while (c1 > c0 && src[c1 - 1] == cmp[c1 - 1]) c1--;
        }

        volatile uint32_t *dst = vram + row * pitch_px;
        for (uint32_t col = c0; col < c1; col++) {
            dst[col] = src[col];
            cmp[col] = src[col];
        }

        if (row < y0) y0 = row;
        if (row + 1 > y1) y1 = row + 1;
        if (c0 < x0) x0 = c0;
        if (c1 > x1) x1 = c1;
    }
    prev_valid = 1;

    if (y1 <= y0) return;              /* nothing moved; nothing to show */

    /* the panel is device memory; make sure the writes have left */
    DSB();

    /*
     * A linear framebuffer is on screen the moment it is written. A
     * virtio-gpu resource is not: the guest's pixels have to be
     * transferred into the host's copy and that copy flushed to the
     * display, so presenting is an explicit step rather than a
     * consequence of storing — and it is charged by area, which is why
     * it is handed the box rather than the screen.
     */
    /* What the boundary actually costs, as a share of the screen. A
     * scanout transfer is charged by area, so this is the number that
     * says whether the dirty box is earning its keep. */
    present_px += (uint64_t)(x1 - x0) * (y1 - y0);
    present_n++;

    if (vgpu_ready) vtgpu_present(x0, y0, x1 - x0, y1 - y0);
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

/*
 * What applications may borrow from the kernel.
 *
 * Defined here rather than in bsdload.h because that header is included
 * before ttf.h and gfx.h, so none of these names exist yet at that point.
 * The loader holds a pointer and this fills it in once everything is
 * declared.
 *
 * The rasteriser is the reason the mechanism exists: it is the one engine
 * large enough that statically linking it into every application would be
 * absurd, and the one an application is most likely to want.
 */
static const bsd_export_t kernel_exports[] = {
    { "ttf_draw_string", (uint64_t)(uintptr_t)ttf_draw_string },
    { "ttf_text_width",  (uint64_t)(uintptr_t)ttf_text_width  },
    { "gfx_rect",        (uint64_t)(uintptr_t)gfx_rect        },
    { 0, 0 }
};

void kmain(void) {
#ifdef BAREMIN
    /*
     * The smallest possible guest: no page tables, no vectors, no serial,
     * no devices. If this aborts under hvf then nothing this kernel does
     * is responsible, and the fault is in the handover or the hypervisor
     * rather than in the port.
     */
    {
        volatile uint64_t n = 0;
        for (;;) n++;
    }
#endif
    /*
     * Before anything, including the first character of output. The UART
     * is a device register, device registers are not mapped by anything
     * Limine set up, and an unmapped access this early faults into a
     * vector table that has not been installed yet. Printing first is not
     * an option — this has to be the first statement in the kernel.
     */
    /*
     * Establish what physically exists before mapping any of it. This
     * has to precede the first line of output, because the UART is
     * reached through the tables mmio_map_init() builds.
     *
     * Every entry counts, not just the usable ones: reserved memory is
     * still backed memory, and the framebuffer and the firmware's own
     * tables live in entries this kernel must be able to reach.
     */
    if (memmap_request.response) {
        for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
            struct limine_memmap_entry *e = memmap_request.response->entries[i];
            ram_region_add(e->base, e->length);
        }
    }

    /*
     * And where the devices are — also before mapping, and therefore
     * before any output at all. On qemu `virt` the built-in addresses
     * are already right; on a board none of them are, including the
     * UART's, so there is no way to report a problem here. fdt_report()
     * says what was found once there is a console to say it on.
     */
    const void *dtb = dtb_request.response ? dtb_request.response->dtb_ptr : 0;
    fdt_discover(dtb);

    bsd_set_exports(kernel_exports);
    app_region_init();
    mmio_map_init();

    serial_puts("\n[socrates/arm64] kmain reached at EL1\n");

    /* Vectors first: from here on a fault says what it was instead of
     * hanging, which matters more the more driver code arrives. */
    fdt_report(dtb);

    /* What the VideoCore says about the board, on a Pi. Nothing to ask
     * on a machine that has no mailbox, so this is silent on virt. */
    mbox_report();

    exceptions_init();
    timer_takeover();
    fpu_init();
    serial_puts("[socrates/arm64] vectors installed, timer disarmed, FP on\n");
    mmu_report();
    mmio_report();

    CHK(1);
    /*
     * The display, preferring the one this kernel drives itself.
     *
     * virtio-gpu is tried before Limine's framebuffer rather than as a
     * fallback from it, because the firmware path is the thing being
     * escaped: EDK2's ramfb driver offers three modes and stops at
     * 1024x768, and asking for more silently yields 800x600. Asking the
     * GPU what the display is gets the real answer — and means the kernel
     * needs no display support from the firmware at all, which is what
     * running on hardware without a UEFI GOP will require.
     *
     * On a Raspberry Pi neither of those exists. There is no virtio
     * anything, and a UEFI graphics protocol only exists if the board
     * was booted through the UEFI firmware rather than the stock one.
     * The VideoCore will hand over a framebuffer if asked directly, and
     * that path depends on nothing but the mailbox — so it sits between
     * the two, tried when the kernel-driven option is absent but before
     * giving up and taking whatever the firmware left.
     *
     * Limine's framebuffer remains the fallback, so a machine with only
     * ramfb still boots to a desktop.
     */
    uint32_t panel_w = 0, panel_h = 0, pitch_px = 0;
    volatile uint32_t *vram = 0;

    if (vtgpu_init(1280, 800)) {
        panel_w  = vgpu_w;
        panel_h  = vgpu_h;
        pitch_px = vgpu_w;
        vram     = (volatile uint32_t *)vgpu_fb;
    } else if (board_kind != BOARD_VIRT && pifb_init(1280, 800)) {
        panel_w  = pifb_w;
        panel_h  = pifb_h;
        pitch_px = pifb_pitch_px;
        vram     = pifb_addr;
        serial_puts("[socrates/arm64] display: VideoCore framebuffer\n");
    } else if (fb_request.response != NULL &&
               fb_request.response->framebuffer_count >= 1) {
        struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
        panel_w  = (uint32_t)fb->width;
        panel_h  = (uint32_t)fb->height;
        pitch_px = (uint32_t)(fb->pitch / (fb->bpp / 8));
        vram     = (volatile uint32_t *)fb->address;
        serial_puts("[socrates/arm64] display: firmware framebuffer\n");
    } else {
        serial_puts("[socrates/arm64] no display: no virtio-gpu and no framebuffer\n");
        halt_forever();
    }

    serial_puts("[socrates/arm64] panel ");
    serial_put_u64(panel_w); serial_puts("x"); serial_put_u64(panel_h);
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

    /* Storage before the animation: it is the slowest thing to come up,
     * it needs nothing from the display, and bringing it up first means a
     * failure is reported in the first second rather than after the
     * animation has played out. */
    blk_init();

    /*
     * Prove the disk end to end rather than trusting the capacity field.
     *
     * A device that enumerates and reports a size can still fail every
     * transfer — a queue the device never really accepted looks identical
     * from here until something asks it for data. Reading sector zero and
     * checking for the partition table's signature costs one request and
     * distinguishes "a disk is attached" from "the disk works".
     */
    if (blk_present()) {
        static uint8_t probe[512];
        if (blk_read(0, 1, probe) == 0) {
            serial_puts("[socrates/arm64] sector 0 reads, boot signature ");
            serial_puts((probe[510] == 0x55 && probe[511] == 0xAA)
                        ? "present\n" : "absent\n");
        } else {
            serial_puts("[socrates/arm64] sector 0 READ FAILED\n");
        }

        exfat_mount();
        /* fs_open() dispatches on fs_kind, which fs_mount() sets. Mounting
         * the volume without it leaves every path lookup going to the
         * FAT32 branch of a filesystem that is not FAT32, and everything
         * reports "file not found". */
        fs_mount();

        /*
         * Start pulling the model in, if one is on the volume.
         *
         * The x86 tree does this and this one never did — so the chat
         * panel here only worked after somebody typed `llm load` by hand,
         * and the Wikipedia window's subtitle offered to answer questions
         * against weights that were never going to arrive. The work
         * itself happens in the render loop, a slice of a frame at a
         * time, so this only opens the file.
         */
        ai_autoload_start();

        serial_puts("[socrates/arm64] exFAT: ");
        if (exf_vol.mounted) {
            serial_puts("mounted, ");
            serial_put_u64(exf_vol.cluster_bytes);
            serial_puts("-byte clusters, ");
            serial_put_u64(exf_total_kb() / 1024);
            serial_puts(" MiB total, ");
            serial_put_u64(exf_free_kb() / 1024);
            serial_puts(" MiB free\n");
            exf_list("/", boot_list_entry);
        } else {
            serial_puts("not mounted (");
            serial_puts(exf_errstr);
            serial_puts(")\n");
        }
    }


    app_screen_w = w;
    app_screen_h = h;

    /*
     * Run the embedded application.
     *
     * This is the whole userland path in one call: the container's magic
     * is checked against this architecture, the image is validated,
     * copied into an executable window, made visible to the instruction
     * cache, and entered — and it calls back in through `svc #0` to print
     * and to draw. Nothing about hello.c changed to get here; only the
     * header it includes and the linker script it uses.
     */
    extern const uint8_t hello_bsd[], hello_bsd_end[];
    uint64_t hello_len = (uint64_t)(hello_bsd_end - hello_bsd);
    serial_puts("[socrates/arm64] .bsd: running embedded app, ");
    serial_put_u64(hello_len);
    serial_puts(" bytes\n");
    if (bsd_exec(hello_bsd, hello_len) != 0) {
        serial_puts("[socrates/arm64] .bsd: refused - ");
        serial_puts(app_err);
        serial_puts("\n");
    } else {
        serial_puts("[socrates/arm64] .bsd: app returned cleanly\n");
        /* Count what sys_draw_pixel actually put in the back buffer. The
         * serial output proves the app ran; this proves its drawing
         * reached the same memory the compositor presents. */
        uint32_t gold = 0;
        for (uint32_t i = 0; i < w * h; i++)
            if (backbuf[i] == 0xD4AF37u) gold++;
        serial_puts("[socrates/arm64] .bsd: app drew ");
        serial_put_u64(gold);
        serial_puts(" pixels into the back buffer\n");
    }

    /*
     * Now hand the loader the x86_64 build of the same program, off the
     * data volume.
     *
     * A loader that runs the right image is only half the guarantee. The
     * fourth magic byte exists so the wrong one is refused at the first
     * check rather than entered and executed as instructions it is not —
     * and the only way to know that works is to try it.
     */
    if (exf_vol.mounted) {
        static uint8_t foreign[8192];
        exf_dirent_t d;
        if (exf_lookup("/hello", &d) && !(d.attr & EXF_ATTR_DIR) &&
            d.size <= sizeof(foreign)) {
            /* exf_read_file reports the count through its fourth
             * argument and returns 0 for success — not the byte count. */
            uint32_t got = 0;
            if (exf_read_file(&d, foreign, sizeof(foreign), &got) == 0 && got) {
                serial_puts("[socrates/arm64] .bsd: trying the x86_64 image from disk\n");
                if (bsd_exec(foreign, (uint64_t)got) != 0) {
                    serial_puts("[socrates/arm64] .bsd: correctly refused - ");
                    serial_puts(app_err);
                    serial_puts("\n");
                } else {
                    serial_puts("[socrates/arm64] .bsd: WRONG - ran a foreign image\n");
                }
            }
        }
    }

    /*
     * Hand the inference arena the largest usable region Limine reports.
     *
     * The model is hundreds of megabytes — far past anything that can be
     * a static array — so the weights live in whatever the bootloader did
     * not claim. The higher-half direct map is what makes the region
     * addressable: it covers all of physical memory at a fixed offset, so
     * a usable range becomes a pointer by adding the offset.
     */
    if (memmap_request.response && hhdm_request.response) {
        uint64_t best_base = 0, best_len = 0, usable = 0, installed = 0;
        for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
            struct limine_memmap_entry *e = memmap_request.response->entries[i];

            /* What the machine has, as `mem` and the system monitor report
             * it. Reclaimable and kernel ranges count — they are RAM, and
             * leaving them out would tell a 2 GB machine it has 1.9. */
            if (e->type == LIMINE_MEMMAP_USABLE ||
                e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
                e->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)
                installed += e->length;

            /* What the inference arena may have is a stricter question:
             * only ranges nothing else is using. */
            if (e->type != LIMINE_MEMMAP_USABLE) continue;
            usable += e->length;
            if (e->length > best_len) { best_len = e->length; best_base = e->base; }
        }
        system_total_memory_mb = installed / (1024 * 1024);
        uint64_t total = usable;
        if (best_len > (16ull << 20)) {
            llm_arena_init((void *)(uintptr_t)(hhdm_request.response->offset
                                               + best_base), best_len);
            serial_puts("[socrates/arm64] llm arena ");
            serial_put_u64(best_len / (1024 * 1024));
            serial_puts(" MiB of ");
            serial_put_u64(total / (1024 * 1024));
            serial_puts(" MiB usable\n");
        }
    }

    /*
     * Floating point, end to end, before anything depends on it.
     *
     * llm.c is the one translation unit compiled with FP enabled, and the
     * only architecture-specific line in it is the square root. A wrong
     * answer here means every normalisation and attention score in the
     * model is wrong too, and that failure would otherwise surface as
     * plausible-looking nonsense hundreds of megabytes and several
     * minutes later.
     */
    {
        uint32_t scaled = 0;
        /* Returns 0 for success, like the rest of llm.c and unlike the
         * neighbouring predicates — worth stating, because reading it as
         * a boolean reports a passing FPU as broken. */
        int rc = llm_fpu_selftest(&scaled);
        serial_puts("[socrates/arm64] llm fpu selftest ");
        serial_puts(rc == 0 ? "pass" : "FAIL");
        serial_puts(" (x10000 = ");
        serial_put_u64(scaled);
        serial_puts(")\n");
    }

    e1000_init(hhdm_request.response ? hhdm_request.response->offset : 0);
    netstack_init();

#ifdef M6_SELFTEST
    /*
     * Milestone 6: the archive and the model, on real data.
     *
     * Both are the reason this port exists — they are the parts that were
     * unusably slow under emulation on x86 — and both are large enough
     * that "it compiles" says very little. This opens the actual 982 MB
     * archive off the disk, reads an article out of it, then loads the
     * 397 MB model and runs a token through it, reporting what each step
     * produced rather than that it returned.
     */
    if (exf_vol.mounted) {
        serial_puts("[socrates/arm64] m6: opening wiki.zim\n");
        if (zim_open("/wiki.zim") == 0) {
            serial_puts("[socrates/arm64] m6: zim v");
            serial_put_u64(zim.major);
            serial_puts(", ");
            serial_put_u64(zim.article_count);
            serial_puts(" entries, ");
            serial_put_u64(zim.cluster_count);
            serial_puts(" clusters, ");
            serial_put_u64(zim.title_count);
            serial_puts(" in the title listing\n");

            /* Pull a real article body out, decompressing its cluster. */
            const uint8_t *body = 0;
            uint32_t blen = 0;
            zim_dirent_t de;
            uint32_t rank = zim.title_count > 8 ? 8 : 0;
            uint32_t idx = zim_title_at(rank);
            if (zim_content(idx, &body, &blen, &de) == 0) {
                serial_puts("[socrates/arm64] m6: article \"");
                serial_puts(de.title[0] ? de.title : de.url);
                serial_puts("\" -> ");
                serial_put_u64(blen);
                serial_puts(" bytes\n");
            } else {
                serial_puts("[socrates/arm64] m6: article read failed - ");
                serial_puts(zim_err);
                serial_puts("\n");
            }
        } else {
            serial_puts("[socrates/arm64] m6: zim open failed - ");
            serial_puts(zim_err);
            serial_puts("\n");
        }

        /* The model. Loading is incremental so the UI can show progress;
         * here it just runs to completion and reports how long it took. */
        serial_puts("[socrates/arm64] m6: loading qwen2.gguf\n");
        const char *lerr = "?";
        uint64_t t0 = timer_ms();

        /*
         * Two stages, and the first is easy to miss: llm_load() parses the
         * GGUF's metadata — architecture, dimensions, tensor table,
         * tokenizer — and only then does llm_load_begin() size the arena
         * and start streaming the payload. Calling begin() first fails
         * with "no model loaded", which reads like a missing file rather
         * than a missing step.
         *
         * Both return 0 for success, as does llm_fpu_selftest and most of
         * this codebase's lower layers — unlike the predicates next to
         * them, which return 1. Reading either as a boolean inverts it.
         */
        static fs_file_t gguf;
        int staged = 0;
        if (fs_open("/qwen2.gguf", &gguf) != 0) {
            serial_puts("[socrates/arm64] m6: cannot open /qwen2.gguf\n");
        } else if (llm_load(llm_read_thunk, &gguf, gguf.size, &lerr) != 0) {
            serial_puts("[socrates/arm64] m6: gguf metadata failed - ");
            serial_puts(lerr); serial_puts("\n");
        } else {
            const llm_info_t *mi = llm_get_info();
            serial_puts("[socrates/arm64] m6: gguf ok, n_embd ");
            serial_put_u64(mi->n_embd);
            serial_puts(", layers ");
            serial_put_u64(mi->n_layer);
            serial_puts(", vocab ");
            serial_put_u64(mi->n_vocab);
            serial_puts("\n");
            staged = 1;
        }

        if (staged && llm_load_begin(&lerr) == 0) {
            int done = 0, last_pct = -1;
            while (!done) {
                int r = llm_load_step(&lerr);
                if (r < 0) break;
                done = r;
                int pct = llm_load_progress();
                if (pct / 25 != last_pct / 25) {
                    last_pct = pct;
                    serial_puts("[socrates/arm64] m6: weights ");
                    serial_put_u64((uint64_t)pct);
                    serial_puts("%\n");
                }
            }
            if (llm_weights_loaded()) {
                serial_puts("[socrates/arm64] m6: weights resident in ");
                serial_put_u64(timer_ms() - t0);
                serial_puts(" ms\n");

                /* One token, end to end: tokenise, evaluate, pick, detokenise. */
                int32_t toks[16];
                int nt = llm_encode("The capital of France is", toks, 16);
                serial_puts("[socrates/arm64] m6: prompt is ");
                serial_put_u64((uint64_t)nt);
                serial_puts(" tokens\n");
                uint64_t t1 = timer_ms();
                for (int i = 0; i < nt; i++) {
                    llm_eval_begin(toks[i], i);
                    while (!llm_eval_step()) { }
                }
                int id = llm_argmax();
                char piece[64];
                llm_decode(id, piece, sizeof(piece));
                serial_puts("[socrates/arm64] m6: next token = \"");
                serial_puts(piece);
                serial_puts("\" in ");
                serial_put_u64(timer_ms() - t1);
                serial_puts(" ms\n");
            } else {
                serial_puts("[socrates/arm64] m6: load failed - ");
                serial_puts(lerr ? lerr : "?");
                serial_puts("\n");
            }
        } else {
            serial_puts("[socrates/arm64] m6: load_begin failed - ");
            serial_puts(lerr ? lerr : "?");
            serial_puts("\n");
        }
    }
#endif

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

    int net_selftest_sent = 0, net_selftest_seen = 0;
    int desktop_mode = 0;
    int auto_browser_done = 0;
    int auto_ask_done = 0;
    int net_fetch_started = 0, net_fetch_reported = 0;


    for (;;) {
        /* Poll phase. Devices are drained once per frame rather than from
         * an interrupt: the loop already visits everything every 16 ms,
         * and a used-ring index costs less to read than an interrupt
         * costs to route. */
        vtinput_poll();
        net_poll();

        /*
         * Ping the gateway a few times at start-up and report what came
         * back.
         *
         * Enumerating a NIC and reading its MAC proves the transport came
         * up, not that a packet can make a round trip: ARP has to resolve,
         * a frame has to reach the host, and a reply has to land in a
         * buffer this driver posted. Sending one is cheap and the answer
         * is unambiguous. It runs from the loop rather than at init
         * because replies only arrive once something is polling for them.
         */
        if (e1000_found && net_selftest_sent < 4 &&
            frames > 30 && (frames % 60) == 0) {
            /* ping_active is what tells the reply handler these are ours;
             * without it the echo replies arrive, parse correctly and are
             * dropped on the floor, which looks exactly like a network
             * that cannot reach the gateway. */
            ping_active = 1;
            ping_sent_tick = net_ticks;
            icmp_send_echo(net_gw_ip, (uint16_t)net_selftest_sent);
            net_selftest_sent++;
        }
        /*
         * One real HTTP fetch, after the pings have proved the link.
         *
         * This is the whole upper stack in one request — DNS resolution,
         * the TCP handshake, a GET, a chunked or content-length body, and
         * the redirect budget — none of which ICMP touches. It is the
         * thing the browser does, minus the rendering.
         */
        if (e1000_found && !net_fetch_started && frames == 360) {
            net_fetch_started = 1;
            http_get("example.com", 80, "/");
            serial_puts("[socrates/arm64] http: GET http://example.com/\n");
        }
        if (net_fetch_started && !net_fetch_reported &&
            (http_state == HTTP_DONE || http_state == HTTP_ERROR)) {
            net_fetch_reported = 1;
            if (http_state == HTTP_DONE) {
                serial_puts("[socrates/arm64] http: status ");
                serial_put_u64((uint64_t)http_status_code);
                serial_puts(", ");
                serial_put_u64((uint64_t)http_body_len);
                serial_puts(" bytes of body\n");
            } else {
                serial_puts("[socrates/arm64] http: failed - ");
                serial_puts(http_err);
                serial_puts("\n");
            }
        }

        if (e1000_found && frames == 400) {
            /* Frame counts separate the three ways "no reply" happens:
             * nothing sent, nothing received, or received and discarded
             * upstream. Each needs a different next question. */
            serial_puts("[socrates/arm64] net: tx ");
            serial_put_u64(vnet_tx_count);
            serial_puts(" frames, rx ");
            serial_put_u64(vnet_rx_count);
            serial_puts(" frames, arp entries ");
            int n = 0;
            for (int i = 0; i < ARP_CACHE_SIZE; i++) if (arp_cache[i].valid) n++;
            serial_put_u64((uint64_t)n);
            serial_puts("\n");
        }
        if (e1000_found && ping_replies != net_selftest_seen) {
            net_selftest_seen = ping_replies;
            serial_puts("[socrates/arm64] icmp reply from gateway (");
            serial_put_u64((uint64_t)net_selftest_seen);
            serial_puts(" of ");
            serial_put_u64((uint64_t)net_selftest_sent);
            serial_puts(")\n");
        }

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

        /*
         * Two modes, one loop.
         *
         * The login screen owns the frame until a password is accepted,
         * then desktop_render() does — window manager, dock, menu bar,
         * terminal, browser and all. That whole stack is portable and
         * compiled for aarch64 unchanged; what it needed was the four
         * things the milestones before this built, which is why wiring it
         * up is a branch rather than a port.
         */
        if (desktop_mode) {
            char dch;
            while ((dch = kb_getchar()) != 0)
                desktop_key_input(dch);

            /* Read then subtract, rather than read then zero, so a notch
             * that lands between the two is carried into the next frame
             * instead of being dropped. */
            int32_t wheel = mouse_wheel;
            if (wheel) {
                mouse_wheel -= wheel;
                desktop_wheel_input(wheel);
            }

            /*
             * The portable code's 60 Hz tick, which nothing on this tree
             * was providing.
             *
             * `sys_ticks` lives in gfx.h and the x86 kernel gets it free:
             * its PIT interrupt increments it sixty times a second. This
             * port has no timer interrupt at all — the render loop paces
             * itself against the architected counter, which was the right
             * design and left a counter that everything above still reads
             * sitting at zero forever.
             *
             * The visible result was a menu bar clock that drew once at
             * boot and then never changed, because it only recomputes the
             * time when `sys_ticks` has advanced thirty. A machine that
             * had been up for an hour still said the minute it started.
             *
             * Derived from real elapsed time rather than counted per
             * frame, so a slow frame does not make the clock lose time —
             * which is the drift the x86 side actually suffers from.
             */
#ifdef INPUT_TRACE
            /* Does a press reach the render loop at all, and where? The
             * pointer moving proves only that positions arrive; buttons
             * come through a different event type on the same queue. */
            {
                static uint8_t last_btn = 0;
                if (mouse_buttons != last_btn) {
                    serial_puts("[input] buttons ");
                    serial_put_u64(mouse_buttons);
                    serial_puts(" at ");
                    serial_put_u64((uint64_t)mouse_x);
                    serial_putc(',');
                    serial_put_u64((uint64_t)mouse_y);
                    serial_putc('\n');
                    last_btn = mouse_buttons;
                }
            }
#endif
            sys_ticks = (uint32_t)(timer_ms() * 60ULL / 1000ULL);

            desktop_render(backbuf, w, h, mouse_x, mouse_y, mouse_buttons);

#ifdef AUTO_BROWSER
            /*
             * Open the browser on a fixed URL, for the headless harness.
             *
             * After the first desktop_render, not before: the window
             * manager sizes and centres a new window against the screen
             * dimensions it caches during a render pass, so opening one
             * earlier places it using a size of zero — which lands it off
             * the edge of the panel. That is exactly the bug this build
             * exists to catch, so the test must not reproduce it itself.
             *
             * Clicking a dock icon over QMP would mean knowing where the
             * dock put it, which depends on the panel size and the item
             * count, so a coordinate-clicking test really tests the dock
             * layout. This uses the same entry point the dock does.
             */
            if (!auto_browser_done) {
                auto_browser_done = 1;
                wm_open(WK_BROWSER);
                brw_navigate("http://example.com/");
                serial_puts("[socrates/arm64] desktop: browser opened on example.com\n");
            }
#endif
#ifdef AUTO_ASK
            /*
             * Put one question to the chat engine, for the harness.
             *
             * The same reasoning as AUTO_BROWSER above: reaching the chat
             * panel with a pointer means clicking a dock icon and then a
             * bubble whose positions depend on the panel size, so a
             * coordinate-driven test mostly tests the layout. This uses
             * the entry point the shell's `ask` uses.
             *
             * Waiting for the weights is the point of the second
             * condition — asking before they are resident is answered,
             * correctly, with "no model", which would make this test pass
             * while proving nothing.
             */
            if (!auto_ask_done && llm_weights_loaded()) {
                auto_ask_done = 1;
                wm_open(WK_WIKI);
                wiki_ask(AUTO_ASK);
            }
#endif
            draw_cursor(w, h);
            vga_flip(vram, w, h, pitch_px);
            CHK(5);

            frames++;
            if (frames % 120 == 0 && present_n) {
                serial_puts("[socrates/arm64] scanout: ");
                serial_put_u64(present_px / present_n);
                serial_puts(" px/frame of ");
                serial_put_u64((uint64_t)w * h);
                serial_puts("\n");
                present_px = 0; present_n = 0;
            }
            if (frames % 120 == 0) {
                uint64_t now = timer_ms();
                uint64_t ms  = now - last_report_ms;
                last_report_ms = now;
                serial_puts("[socrates/arm64] desktop: 120 frames in ");
                serial_put_u64(ms);
                serial_puts(" ms (");
                serial_put_u64(ms ? (120 * 1000) / ms : 0);
                serial_puts(" fps)\n");
            }
            next_frame += frame_ticks;
            timer_wait_until(next_frame);
            continue;
        }

        /* Echo what has been typed, so the field fills in as keys land. */
        char ch;
        while ((ch = kb_getchar()) != 0) {
            if (ch == '\b') {
                if (pw_len > 0) pw_len--;
            } else if (ch == '\n') {
                /*
                 * First boot sets the password; later boots check it.
                 * The volume this port tests against is attached
                 * read-only, so there is nowhere to persist it yet and
                 * any password is accepted on the first Enter — the
                 * check itself lives in desktop.h and comes back with a
                 * writable disk.
                 */
                desktop_mode = 1;
                for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
                prev_valid = 0;
                serial_puts("[socrates/arm64] desktop: entering\n");
                pw_len = 0;

            } else if (ch >= 0x20 && ch < 0x7F && pw_len < (int)sizeof(pw) - 1) {
                pw[pw_len++] = ch;
            }
            pw[pw_len] = '\0';
        }
        if (desktop_mode) continue;

        login_render(backbuf, w, h, mouse_x, mouse_y, pw,
                     mouse_buttons, "Socrates BSD 9 - ARM64");
        draw_cursor(w, h);

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
