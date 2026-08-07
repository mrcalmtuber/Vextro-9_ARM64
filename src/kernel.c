#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "arm.h"
#include "virtio.h"
#include "paccel.h"
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
#include "bootanim.h"

/*
 * Vextro 9 for ARM64.
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

/* Short accumulators for the desktop busy meter, averaged four times a
 * second -- the 120-frame serial report is far too coarse for something
 * being watched on screen. */
static uint64_t busy_acc = 0, idle_acc = 0;
static uint32_t busy_frames = 0;
static uint32_t present_n  = 0;      /* frames counted                 */

#define COLOR_BLACK 0x000000u
#define COLOR_GOLD  0xD4AF37u

/* Report the root directory at boot. Listing real names off a real
 * volume is the only thing that proves the whole stack — queue, request,
 * FAT walk, directory-entry decoding — rather than just that a device
 * answered. */
static void boot_list_entry(const char *name, uint32_t size, int is_dir) {
    serial_puts("[vextro/arm64]   ");
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
 * The pointer.
 *
 * It used to be stamped into the back buffer along with everything else,
 * which meant moving it *changed the frame*: the flip compares against
 * the previous frame and presents the bounding box of what differs, so a
 * pointer crossing an otherwise still desktop dragged a full-width band
 * of unchanged pixels across the virtio-gpu boundary every frame.
 *
 * virtio-gpu has a cursor plane, and it is now driven: the sprite lives
 * in its own resource and moving it is one 56-byte command on a separate
 * queue, with no scanout traffic at all. Where that is unavailable --
 * ramfb, and the Raspberry Pi's mailbox framebuffer -- the arrow is
 * composited into scanout after the flip instead, which keeps it out of
 * the back buffer either way.
 *
 * Two colours: X is the outline, . the fill.
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

#define CURSOR_W 12
#define CURSOR_H 18

/* Expand the ASCII art into the ARGB the cursor plane wants: opaque
 * black outline, opaque white fill, everything else fully transparent. */
static void cursor_build_argb(uint32_t *out) {
    for (uint32_t i = 0; i < CURSOR_W * CURSOR_H; i++) out[i] = 0;
    for (uint32_t row = 0; row < CURSOR_H; row++) {
        const char *line = CURSOR_IMG[row];
        for (uint32_t col = 0; col < CURSOR_W; col++) {
            char c = line[col];
            if (c == ' ') continue;
            out[row * CURSOR_W + col] =
                (c == 'X') ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
}

/* ---- software compositing, for panels with no cursor plane ---- */

/*
 * Where there is no cursor plane -- ramfb, and the Pi's mailbox
 * framebuffer -- the pointer is composited by the flip rather than drawn
 * after it.
 *
 * Drawing it after was the first attempt and it was worse than what it
 * replaced: every frame the flip wrote desktop pixels over the sprite and
 * the overlay put it back, so on any animating screen -- the login vortex
 * above all -- the pointer was erased and redrawn sixty times a second,
 * with no vblank to hide it in. That is the flicker, and it is why the
 * pointer looked like it was sliding underneath things.
 *
 * Compositing inside the flip writes every pixel exactly once with its
 * final value. prevbuf still records the clean desktop pixel, so change
 * detection keeps comparing like with like.
 *
 * The virtio-gpu path never comes through here: there the sprite is a
 * hardware plane and the scanout does not contain it at all.
 */
static int32_t cur_prev_x = 0, cur_prev_y = 0;
static int     cur_valid = 0;

/* The sprite's colour at a screen position, or 0 where it does not cover. */
static int cursor_at(int32_t px, int32_t py, int32_t cx, int32_t cy,
                     uint32_t *out) {
    int32_t dx = px - cx, dy = py - cy;
    if (dx < 0 || dx >= CURSOR_W || dy < 0 || dy >= CURSOR_H) return 0;
    char c = CURSOR_IMG[dy][dx];
    if (c == ' ') return 0;
    *out = (c == 'X') ? 0x000000u : 0xFFFFFFu;
    return 1;
}

/*
 * The fling trail.
 *
 * A hard flick can carry the pointer most of the way across the screen in
 * a couple of frames, which reads as the sprite disappearing and
 * reappearing somewhere else. A few fading ghosts along the path make the
 * movement legible -- you can see where it went rather than inferring it.
 *
 * Deliberately not always on. Below the fling threshold the trail is
 * empty and the pointer costs exactly what it did before, which matters
 * because slow movement is the precise mode and the last thing it wants
 * is decoration.
 */
#define TRAIL_N 5

static struct { int32_t x, y; uint8_t life; } trail[TRAIL_N];
static int trail_used = 0;

/* Weight of a ghost, 0-255, falling off with age. */
static uint32_t trail_weight(uint8_t life) {
    return (uint32_t)life * 40u;          /* life 4 -> 160, life 1 -> 40 */
}

/* Blend towards `c` by w/256, per channel, in integer arithmetic. */
static uint32_t trail_blend(uint32_t bg, uint32_t c, uint32_t w) {
    uint32_t br = (bg >> 16) & 0xFF, bgn = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    uint32_t cr = (c  >> 16) & 0xFF, cg  = (c  >> 8) & 0xFF, cb = c  & 0xFF;
    uint32_t r = br + ((cr - br) * w >> 8);
    uint32_t g = bgn + ((cg - bgn) * w >> 8);
    uint32_t b = bb + ((cb - bb) * w >> 8);
    return (r << 16) | (g << 8) | b;
}

/* The strongest ghost covering a pixel, if any. */
static int trail_at(int32_t px, int32_t py, uint32_t *colour, uint32_t *weight) {
    uint32_t best = 0, bc = 0;
    for (int i = 0; i < trail_used; i++) {
        if (!trail[i].life) continue;
        uint32_t c;
        if (!cursor_at(px, py, trail[i].x, trail[i].y, &c)) continue;
        uint32_t w = trail_weight(trail[i].life);
        if (w > best) { best = w; bc = c; }
    }
    if (!best) return 0;
    *colour = bc;
    *weight = best;
    return 1;
}

/*
 * Age the trail by one frame, and record where the pointer was if it was
 * moving fast enough to be worth showing.
 */
static void trail_step(int32_t cx, int32_t cy, int moved_fast) {
    for (int i = 0; i < trail_used; i++)
        if (trail[i].life) trail[i].life--;

    /* compact out the dead, so the search above stays short */
    int k = 0;
    for (int i = 0; i < trail_used; i++)
        if (trail[i].life) trail[k++] = trail[i];
    trail_used = k;

    if (!moved_fast) return;
    if (trail_used == TRAIL_N) {
        for (int i = 1; i < TRAIL_N; i++) trail[i-1] = trail[i];
        trail_used = TRAIL_N - 1;
    }
    trail[trail_used].x = cx;
    trail[trail_used].y = cy;
    trail[trail_used].life = 4;
    trail_used++;
}
/*
 * Where the login screen is in its sequence.
 *
 * The machine used to have one anonymous passcode and two states: set it,
 * or type it. With accounts there is a first run that asks for a name and
 * a password twice, and a normal path that picks an account and asks for
 * its password.
 */
enum {
    LOGIN_PASSWORD = 0,   /* pick an account, type its password */
    LOGIN_NEW_NAME,       /* first run: choose a username        */
    LOGIN_NEW_PW,
    LOGIN_NEW_CONFIRM
};
/*
 * Whether the account being created is an administrator.
 *
 * Defaults to yes: on a machine with one user that is what they want, and
 * it is what the first account used to be given with no say in it.
 *
 * Declining does not leave the machine without one. Somebody has to be
 * able to create and remove accounts, so a separate `admin` account is
 * made alongside -- with the same password, because the alternative is a
 * fixed default one, and a known password on an administrator account is
 * a hole rather than a convenience. The login screen says so plainly
 * rather than leaving it to be discovered.
 */
#ifdef NO_ADMIN_DEFAULT
static int  login_want_admin = 0;   /* test hook: start with the box clear */
#else
static int  login_want_admin = 1;
#endif

/* The failure animation, which this tree never needed because it never
 * refused anything. */
static int      melt_active = 0;
static uint32_t melt_tick = 0;
#define MELT_DURATION 120        /* ~2 seconds at 60 Hz */

static int  login_stage = LOGIN_PASSWORD;
static int  login_sel = 0;               /* which account is highlighted */
static char login_msg[96] = "";          /* replaces the prompt when set */
static char login_notice[112] = "";      /* shown under the box, additive */
static char pending_name[USER_NAME_MAX];
static char pending_pw[64];   /* matches the login field below */


/*
 * The accounts on this machine, above the login box.
 *
 * Lives here rather than in login.h because login.h is included before
 * the filesystem layer that users.h needs, and login.h is byte-identical
 * across the two architecture trees -- a property worth keeping.
 *
 * Only drawn when there is a choice to make: one account and the box on
 * its own is the whole interface, exactly as before.
 */
#define ACCT_W   150
#define ACCT_H   34
#define ACCT_GAP 10

static void login_draw_users(uint32_t *buf, uint32_t w, uint32_t h,
                             int32_t mx, int32_t my, uint8_t lmb) {
    static uint8_t prev = 0;
    int click = (lmb & 1) && !prev;
    prev = lmb & 1;

    int32_t total = user_count * ACCT_W + (user_count - 1) * ACCT_GAP;
    int32_t x0 = ((int32_t)w - total) / 2;
    int32_t y0 = ((int32_t)h - LOGIN_BOX_H) / 2 - ACCT_H - 26;
    if (y0 < 8) y0 = 8;

    for (int i = 0; i < user_count; i++) {
        int32_t x = x0 + i * (ACCT_W + ACCT_GAP);
        int sel = (i == login_sel);
        int hot = (mx >= x && mx < x + ACCT_W && my >= y0 && my < y0 + ACCT_H);

        if (click && hot) login_sel = i;

        gfx_rect(buf, w, h, x, y0, ACCT_W, ACCT_H,
                 sel ? 0x2A2410u : 0x0E1017u);
        gfx_rect_outline(buf, w, h, x, y0, ACCT_W, ACCT_H,
                         sel ? COLOR_GOLD : (hot ? 0x6A5A20u : 0x2A3040u));

        const char *nm = user_name_of(i);
        int tw = ttf_text_width(nm, 14);
        ttf_draw_string(buf, (int)w, (int)h, x + (ACCT_W - tw) / 2, y0 + 7,
                        nm, sel ? COLOR_GOLD : 0x9098A8u, 14);

        if (user_is_admin(i))
            gfx_rect(buf, w, h, x + 6, y0 + 6, 4, 4, COLOR_GOLD);
    }
}


/*
 * The administrator checkbox, and any notice under the login box.
 *
 * Sits below the box during the first-run sequence, where the account is
 * being created and the choice still means something. Same clickable
 * shape as login.h's Show/Hide button.
 */
#define CHK_BOX 18

static void login_draw_admin_check(uint32_t *buf, uint32_t w, uint32_t h,
                                   int32_t mx, int32_t my, uint8_t lmb) {
    static uint8_t prev = 0;
    int click = (lmb & 1) && !prev;
    prev = lmb & 1;

    int32_t by = ((int32_t)h - LOGIN_BOX_H) / 2 + LOGIN_BOX_H + 22;
    const char *label = "Make this an administrator account";
    int32_t tw = ttf_text_width(label, 14);
    int32_t total = CHK_BOX + 10 + tw;
    int32_t x0 = ((int32_t)w - total) / 2;

    int hot = (mx >= x0 && mx < x0 + total &&
               my >= by - 2 && my < by + CHK_BOX + 2);
    if (click && hot) login_want_admin = !login_want_admin;

    gfx_rect(buf, w, h, x0, by, CHK_BOX, CHK_BOX,
             login_want_admin ? 0x2A2410u : 0x0E1017u);
    gfx_rect_outline(buf, w, h, x0, by, CHK_BOX, CHK_BOX,
                     hot ? COLOR_GOLD : (login_want_admin ? COLOR_GOLD
                                                          : 0x3A4050u));
    if (login_want_admin) {
        /* a tick, drawn as two strokes */
        for (int k = 0; k < 4; k++)
            gfx_rect(buf, w, h, x0 + 4 + k, by + 8 + k, 2, 2, COLOR_GOLD);
        for (int k = 0; k < 6; k++)
            gfx_rect(buf, w, h, x0 + 8 + k, by + 12 - k, 2, 2, COLOR_GOLD);
    }

    ttf_draw_string(buf, (int)w, (int)h, x0 + CHK_BOX + 10, by + 1, label,
                    login_want_admin ? 0xD8DCE6u : 0x8891A0u, 14);

    if (!login_want_admin)
        ttf_draw_string(buf, (int)w, (int)h, x0, by + CHK_BOX + 8,
                        "An 'admin' account will be created, same password",
                        0x8A8F9Cu, 12);
}

/* A line under the box that is not the prompt, so both can be shown. */
static void login_draw_notice(uint32_t *buf, uint32_t w, uint32_t h) {
    if (!login_notice[0]) return;
    int32_t by = ((int32_t)h - LOGIN_BOX_H) / 2 + LOGIN_BOX_H + 24;
    int32_t tw = ttf_text_width(login_notice, 13);
    ttf_draw_string(buf, (int)w, (int)h, ((int32_t)w - tw) / 2, by,
                    login_notice, C_GOLD_DIM, 13);
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
            serial_puts("[vextro/arm64] halted at checkpoint " #n "\n"); \
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

    /* Someone else wrote the panel, so what prevbuf claims is on screen
     * is no longer true. This flag was declared and set on this tree but
     * never read; the x86 tree has honoured it since it was added. */
    if (gfx_force_full_flip) {
        gfx_force_full_flip = 0;
        prev_valid = 0;
        cur_valid = 0;
    }

    /*
     * The pointer is composited here only when there is no hardware
     * plane. With one, the scanout never contains the sprite and moving
     * it costs no pixels at all.
     */
    int soft_cursor = !(vgpu_ready && vgpu_cursor_ok);
    int32_t cx = mouse_x, cy = mouse_y;
    int32_t ux0 = cx, uy0 = cy, ux1 = cx + CURSOR_W, uy1 = cy + CURSOR_H;
    if (soft_cursor && cur_valid) {
        if (cur_prev_x < ux0) ux0 = cur_prev_x;
        if (cur_prev_y < uy0) uy0 = cur_prev_y;
        if (cur_prev_x + CURSOR_W > ux1) ux1 = cur_prev_x + CURSOR_W;
        if (cur_prev_y + CURSOR_H > uy1) uy1 = cur_prev_y + CURSOR_H;
    }

    /*
     * Age the trail and fold every live ghost into the same rectangle --
     * they have to be in it whether they are being drawn or erased.
     *
     * Only on the software path. The hardware cursor plane carries one
     * sprite and no history, so there is nothing to trail with; asking
     * for one would mean giving up the plane and repainting the scanout,
     * which is the cost the plane exists to avoid.
     */
    if (soft_cursor) {
        trail_step(cx, cy, paccel_is_fling());
        for (int i = 0; i < trail_used; i++) {
            if (trail[i].x < ux0) ux0 = trail[i].x;
            if (trail[i].y < uy0) uy0 = trail[i].y;
            if (trail[i].x + CURSOR_W > ux1) ux1 = trail[i].x + CURSOR_W;
            if (trail[i].y + CURSOR_H > uy1) uy1 = trail[i].y + CURSOR_H;
        }
    }
    if (ux0 < 0) ux0 = 0;
    if (uy0 < 0) uy0 = 0;
    if (ux1 > (int32_t)w) ux1 = (int32_t)w;
    if (uy1 > (int32_t)h) uy1 = (int32_t)h;

    for (uint32_t row = 0; row < h; row++) {
        const uint32_t *src = backbuf + row * w;
        uint32_t       *cmp = prevbuf + row * w;

        int in_cur = soft_cursor && (int32_t)row >= uy0 &&
                     (int32_t)row < uy1 && ux1 > ux0;

        uint32_t c0 = 0, c1 = w;
        if (prev_valid) {
            while (c0 < w && src[c0] == cmp[c0]) c0++;
            if (c0 == w) {
                if (!in_cur) continue;              /* row unchanged */
                c0 = (uint32_t)ux0;
                c1 = (uint32_t)ux1;
            } else {
                while (c1 > c0 && src[c1 - 1] == cmp[c1 - 1]) c1--;
                if (in_cur) {
                    if ((uint32_t)ux0 < c0) c0 = (uint32_t)ux0;
                    if ((uint32_t)ux1 > c1) c1 = (uint32_t)ux1;
                }
            }
        }

        volatile uint32_t *dst = vram + row * pitch_px;
        for (uint32_t col = c0; col < c1; col++) {
            uint32_t px = src[col];
            cmp[col] = px;                    /* record the clean pixel */
            uint32_t sprite, gw;
            if (in_cur) {
                if (trail_used &&
                    trail_at((int32_t)col, (int32_t)row, &sprite, &gw))
                    px = trail_blend(px, sprite, gw);
                if (cursor_at((int32_t)col, (int32_t)row, cx, cy, &sprite))
                    px = sprite;              /* the pointer wins, always */
            }
            dst[col] = px;
        }

        if (row < y0) y0 = row;
        if (row + 1 > y1) y1 = row + 1;
        if (c0 < x0) x0 = c0;
        if (c1 > x1) x1 = c1;
    }
    prev_valid = 1;
    cur_prev_x = cx;
    cur_prev_y = cy;
    cur_valid = 1;

    /* The hardware plane moves independently of the scanout. */
    if (vgpu_ready && vgpu_cursor_ok)
        vtgpu_cursor_move((uint32_t)cx, (uint32_t)cy);

    if (y1 <= y0) return;              /* nothing moved; nothing to show */

    DSB();

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
    ba_init((int)scr_w, (int)scr_h);

    for (uint32_t i = 0; i < scr_w * scr_h; i++) backbuf[i] = 0;

    /* 24 fps, measured against the architected counter rather than a
     * programmed one-shot — no PIT to set up and no channel to gate. The
     * deadline is advanced from the previous one rather than from now, so
     * the time spent drawing a frame comes out of that frame's budget
     * instead of being added to it. */
    uint64_t frame_ticks = timer_hz() / 24;
    uint64_t next = timer_count();

    for (int f = 0; f < BA_FRAMES; f++) {
        ba_render(backbuf, (int)scr_w, f);
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

    serial_puts("\n[vextro/arm64] kmain reached at EL1\n");

    /* Vectors first: from here on a fault says what it was instead of
     * hanging, which matters more the more driver code arrives. */
    fdt_report(dtb);

    /* What the VideoCore says about the board, on a Pi. Nothing to ask
     * on a machine that has no mailbox, so this is silent on virt. */
    mbox_report();

    exceptions_init();
    timer_takeover();
    fpu_init();
    serial_puts("[vextro/arm64] vectors installed, timer disarmed, FP on\n");
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

        /* Hand the arrow to the cursor plane, once. The hot spot is the
         * top-left corner, which is where this sprite's point is. */
        if (vgpu_cursor_ok) {
            static uint32_t cur_argb[CURSOR_W * CURSOR_H];
            cursor_build_argb(cur_argb);
            vtgpu_cursor_define(cur_argb, CURSOR_W, CURSOR_H, 0, 0);
        }
    } else if (board_kind != BOARD_VIRT && pifb_init(1280, 800)) {
        panel_w  = pifb_w;
        panel_h  = pifb_h;
        pitch_px = pifb_pitch_px;
        vram     = pifb_addr;
        serial_puts("[vextro/arm64] display: VideoCore framebuffer\n");
    } else if (fb_request.response != NULL &&
               fb_request.response->framebuffer_count >= 1) {
        struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
        panel_w  = (uint32_t)fb->width;
        panel_h  = (uint32_t)fb->height;
        pitch_px = (uint32_t)(fb->pitch / (fb->bpp / 8));
        vram     = (volatile uint32_t *)fb->address;
        serial_puts("[vextro/arm64] display: firmware framebuffer\n");
    } else {
        serial_puts("[vextro/arm64] no display: no virtio-gpu and no framebuffer\n");
        halt_forever();
    }

    serial_puts("[vextro/arm64] panel ");
    serial_put_u64(panel_w); serial_puts("x"); serial_put_u64(panel_h);
    serial_puts("\n");
    CHK(2);

    for (uint32_t row = 0; row < panel_h; row++)
        for (uint32_t col = 0; col < panel_w; col++)
            vram[row * pitch_px + col] = 0;

    CHK(3);

    uint32_t w = panel_w > BUF_MAX_W ? BUF_MAX_W : panel_w;
    uint32_t h = panel_h > BUF_MAX_H ? BUF_MAX_H : panel_h;

    serial_puts("[vextro/arm64] framebuffer ");
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
            serial_puts("[vextro/arm64] sector 0 reads, boot signature ");
            serial_puts((probe[510] == 0x55 && probe[511] == 0xAA)
                        ? "present\n" : "absent\n");
        } else {
            serial_puts("[vextro/arm64] sector 0 READ FAILED\n");
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

        /*
         * Accounts.
         *
         * This tree accepted any password until now, because the volume
         * it tests against is attached read-only and there was nowhere to
         * keep a record. /etc/users.db lives on whichever volume is
         * writable; a machine with an old plaintext /keycode.sys is
         * carried forward into an administrator account named "admin".
         */
        /* The dock and Apps menu read this; without it an installed app
         * is invisible until the store window is opened. The x86 tree has
         * always called it. */
        store_init();

        users_load();
        if (user_count == 0 && users_migrate_keycode())
            serial_puts("[vextro/arm64] users: migrated /keycode.sys\n");

        if (user_count == 0) {
            login_stage = LOGIN_NEW_NAME;
            serial_puts("[vextro/arm64] users: none, first-run setup\n");
        } else {
            login_stage = LOGIN_PASSWORD;
            login_sel = 0;
            serial_puts("[vextro/arm64] users: ");
            serial_put_u64((uint64_t)user_count);
            serial_puts(" account(s)\n");
        }

        serial_puts("[vextro/arm64] exFAT: ");
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
    serial_puts("[vextro/arm64] .bsd: running embedded app, ");
    serial_put_u64(hello_len);
    serial_puts(" bytes\n");
    if (bsd_exec(hello_bsd, hello_len) != 0) {
        serial_puts("[vextro/arm64] .bsd: refused - ");
        serial_puts(app_err);
        serial_puts("\n");
    } else {
        serial_puts("[vextro/arm64] .bsd: app returned cleanly\n");
        /* Count what sys_draw_pixel actually put in the back buffer. The
         * serial output proves the app ran; this proves its drawing
         * reached the same memory the compositor presents. */
        uint32_t gold = 0;
        for (uint32_t i = 0; i < w * h; i++)
            if (backbuf[i] == 0xD4AF37u) gold++;
        serial_puts("[vextro/arm64] .bsd: app drew ");
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
                serial_puts("[vextro/arm64] .bsd: trying the x86_64 image from disk\n");
                if (bsd_exec(foreign, (uint64_t)got) != 0) {
                    serial_puts("[vextro/arm64] .bsd: correctly refused - ");
                    serial_puts(app_err);
                    serial_puts("\n");
                } else {
                    serial_puts("[vextro/arm64] .bsd: WRONG - ran a foreign image\n");
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
            serial_puts("[vextro/arm64] llm arena ");
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
        serial_puts("[vextro/arm64] llm fpu selftest ");
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
        serial_puts("[vextro/arm64] m6: opening wiki.zim\n");
        if (zim_open("/wiki.zim") == 0) {
            serial_puts("[vextro/arm64] m6: zim v");
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
                serial_puts("[vextro/arm64] m6: article \"");
                serial_puts(de.title[0] ? de.title : de.url);
                serial_puts("\" -> ");
                serial_put_u64(blen);
                serial_puts(" bytes\n");
            } else {
                serial_puts("[vextro/arm64] m6: article read failed - ");
                serial_puts(zim_err);
                serial_puts("\n");
            }
        } else {
            serial_puts("[vextro/arm64] m6: zim open failed - ");
            serial_puts(zim_err);
            serial_puts("\n");
        }

        /* The model. Loading is incremental so the UI can show progress;
         * here it just runs to completion and reports how long it took. */
        serial_puts("[vextro/arm64] m6: loading qwen2.gguf\n");
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
            serial_puts("[vextro/arm64] m6: cannot open /qwen2.gguf\n");
        } else if (llm_load(llm_read_thunk, &gguf, gguf.size, &lerr) != 0) {
            serial_puts("[vextro/arm64] m6: gguf metadata failed - ");
            serial_puts(lerr); serial_puts("\n");
        } else {
            const llm_info_t *mi = llm_get_info();
            serial_puts("[vextro/arm64] m6: gguf ok, n_embd ");
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
                    serial_puts("[vextro/arm64] m6: weights ");
                    serial_put_u64((uint64_t)pct);
                    serial_puts("%\n");
                }
            }
            if (llm_weights_loaded()) {
                serial_puts("[vextro/arm64] m6: weights resident in ");
                serial_put_u64(timer_ms() - t0);
                serial_puts(" ms\n");

                /* One token, end to end: tokenise, evaluate, pick, detokenise. */
                int32_t toks[16];
                int nt = llm_encode("The capital of France is", toks, 16);
                serial_puts("[vextro/arm64] m6: prompt is ");
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
                serial_puts("[vextro/arm64] m6: next token = \"");
                serial_puts(piece);
                serial_puts("\" in ");
                serial_put_u64(timer_ms() - t1);
                serial_puts(" ms\n");
            } else {
                serial_puts("[vextro/arm64] m6: load failed - ");
                serial_puts(lerr ? lerr : "?");
                serial_puts("\n");
            }
        } else {
            serial_puts("[vextro/arm64] m6: load_begin failed - ");
            serial_puts(lerr ? lerr : "?");
            serial_puts("\n");
        }
    }
#endif

    display_boot_animation(vram, w, h, pitch_px);
    serial_puts("[vextro/arm64] boot animation done\n");

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
            serial_puts("[vextro/arm64] http: GET http://example.com/\n");
        }
        if (net_fetch_started && !net_fetch_reported &&
            (http_state == HTTP_DONE || http_state == HTTP_ERROR)) {
            net_fetch_reported = 1;
            if (http_state == HTTP_DONE) {
                serial_puts("[vextro/arm64] http: status ");
                serial_put_u64((uint64_t)http_status_code);
                serial_puts(", ");
                serial_put_u64((uint64_t)http_body_len);
                serial_puts(" bytes of body\n");
            } else {
                serial_puts("[vextro/arm64] http: failed - ");
                serial_puts(http_err);
                serial_puts("\n");
            }
        }

        if (e1000_found && frames == 400) {
            /* Frame counts separate the three ways "no reply" happens:
             * nothing sent, nothing received, or received and discarded
             * upstream. Each needs a different next question. */
            serial_puts("[vextro/arm64] net: tx ");
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
            serial_puts("[vextro/arm64] icmp reply from gateway (");
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
            serial_puts("[vextro/arm64] pointer ");
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
        /*
         * Logging out. Handled here rather than where it is requested,
         * because both the menu and the shell ask for it from inside a
         * draw or a command, and session_end() closes every window --
         * pulling the list out from under whatever is walking it.
         */
        if (want_logout) {
            want_logout = 0;
            serial_puts("[vextro/arm64] logout: ");
            serial_puts(user_name_of(user_current));
            serial_puts("\n");
            session_end();
            user_current = -1;
            desktop_mode = 0;
            login_stage = LOGIN_PASSWORD;
            login_initialized = 0;      /* the vortex starts over */
            prev_valid = 0;
            pw_len = 0;
            pw[0] = '\0';
            login_msg[0] = '\0';
            for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
        }

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

            const uint64_t bm_t0 = timer_count();
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
                serial_puts("[vextro/arm64] desktop: browser opened on example.com\n");
            }
#endif
#ifdef AUTO_ASK
            /* The opt-in dialog would otherwise block, and the loader
             * correctly refuses on an unanswered question. */
            if (ai_enabled < 0) { ai_choice_save(1); ai_autoload_start(); }
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
            vga_flip(vram, w, h, pitch_px);
            CHK(5);

            frames++;
            if (frames % 120 == 0 && present_n) {
                serial_puts("[vextro/arm64] scanout: ");
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
                serial_puts("[vextro/arm64] desktop: 120 frames in ");
                serial_put_u64(ms);
                serial_puts(" ms (");
                serial_put_u64(ms ? (120 * 1000) / ms : 0);
                serial_puts(" fps)\n");
            }

            /*
             * Busy against idle for the System gadget. The measurement
             * brackets the wait rather than the work, because what the
             * meter reports is the share of each frame the machine could
             * not spend waiting -- and on this port the wait is
             * timer_wait_until, not an interrupt.
             *
             * Averaged over 15 frames so the bar moves four times a
             * second instead of flickering at the frame rate.
             */
            const uint64_t bm_t2 = timer_count();
            next_frame += frame_ticks;
            timer_wait_until(next_frame);
            busy_acc += bm_t2 - bm_t0;
            idle_acc += timer_count() - bm_t2;
            if (++busy_frames >= 15) {
                sys_busy_record((uint32_t)(busy_acc / busy_frames),
                                (uint32_t)(idle_acc / busy_frames));
                busy_acc = idle_acc = 0;
                busy_frames = 0;
            }
            continue;
        }

        /* A refused password melts the screen, as it does on x86. */
        if (melt_active) {
            while (kb_getchar() != 0) { }        /* discard input */
            screen_melt(backbuf, w, h, melt_tick);
            melt_tick++;
            if (melt_tick >= MELT_DURATION) {
                melt_active = 0;
                melt_tick = 0;
                melt_inited = 0;
                login_initialized = 0;
                for (uint32_t i = 0; i < w * h; i++) backbuf[i] = COLOR_BLACK;
                prev_valid = 0;
            }
            vga_flip(vram, w, h, pitch_px);
            next_frame += frame_ticks;
            timer_wait_until(next_frame);
            continue;
        }

        /*
         * The login screen.
         *
         * This tree used to accept any password, including an empty one,
         * because the volume it tests against is attached read-only and
         * there was nowhere to keep a record. There is now: a small
         * writable disk carries /etc/users.db, and the check here is the
         * same one the x86 tree runs.
         */
        char ch;
        int enter_pressed = 0;
        while ((ch = kb_getchar()) != 0) {
            if (ch == '\b') {
                if (pw_len > 0) pw_len--;
            } else if (ch == '\n') {
                enter_pressed = 1;
            } else if (ch == KEY_UP) {
                if (login_stage == LOGIN_PASSWORD && login_sel > 0) login_sel--;
            } else if (ch == KEY_DOWN) {
                if (login_stage == LOGIN_PASSWORD && login_sel + 1 < user_count)
                    login_sel++;
            } else if (ch >= 0x20 && ch < 0x7F && pw_len < (int)sizeof(pw) - 1) {
                pw[pw_len++] = ch;
            }
            pw[pw_len] = '\0';
        }

        if (enter_pressed && pw_len > 0) {
            switch (login_stage) {
            case LOGIN_NEW_NAME:
                if (!user_name_ok(pw)) {
                    str_copy(login_msg, user_err, sizeof(login_msg));
                } else {
                    str_copy(pending_name, pw, sizeof(pending_name));
                    login_stage = LOGIN_NEW_PW;
                    login_msg[0] = '\0';
                }
                break;

            case LOGIN_NEW_PW:
                str_copy(pending_pw, pw, sizeof(pending_pw));
                login_stage = LOGIN_NEW_CONFIRM;
                break;

            case LOGIN_NEW_CONFIRM:
                if (!str_eq(pending_pw, pw)) {
                    str_copy(login_msg, "Those did not match. Try again.",
                             sizeof(login_msg));
                    login_stage = LOGIN_NEW_PW;
                } else if (!login_want_admin && str_eq(pending_name, "admin")) {
                    /* The fallback account would collide with theirs. */
                    str_copy(login_msg, "'admin' is the fallback name - tick "
                             "the box or pick another.", sizeof(login_msg));
                    login_stage = LOGIN_NEW_NAME;
                } else if (user_add(pending_name, pending_pw,
                                    login_want_admin) < 0) {
                    str_copy(login_msg, user_err, sizeof(login_msg));
                    login_stage = LOGIN_NEW_PW;
                } else {
                    /*
                     * Someone has to be able to create and remove
                     * accounts. If they did not want that to be them, a
                     * separate administrator is made alongside.
                     */
                    if (!login_want_admin &&
                        user_add("admin", pending_pw, 1) >= 0) {
                        str_copy(login_notice,
                                 "Administrator account 'admin' created, "
                                 "same password", sizeof(login_notice));
                        serial_puts("[vextro/arm64] users: created 'admin' "
                                    "alongside a standard account\n");
                    }
                    login_sel = user_find(pending_name);
                    login_stage = LOGIN_PASSWORD;
                    login_msg[0] = '\0';
                }
                for (uint32_t i = 0; i < sizeof(pending_pw); i++)
                    pending_pw[i] = '\0';
                break;

            case LOGIN_PASSWORD:
            default:
                if (user_check(login_sel, pw)) {
                    user_current = login_sel;
                    session_begin(user_name_of(user_current));
                    desktop_mode = 1;
                    for (uint32_t i = 0; i < w * h; i++)
                        backbuf[i] = COLOR_BLACK;
                    prev_valid = 0;
    #ifdef CU_SELFTEST
                    /* The Unix toolset, exercised against real files. The
                     * terminal is a framebuffer window a harness cannot
                     * read, so term_print is mirrored to serial. */
                    wm_open(WK_TERM);
                    fs_write_file("/t.txt",
                        "alpha bravo charlie\n"
                        "delta echo foxtrot\n"
                        "alpha again\n"
                        "delta echo foxtrot\n", 70);
                    {
                        static const char *cu[] = {
                            "ls /", "wc /t.txt", "grep -n alpha /t.txt",
                            "sort -u /t.txt", "tr a-z A-Z /t.txt",
                            "sed s/alpha/OMEGA/g /t.txt",
                            "cut -d ' ' -f 2 /t.txt",
                            "sha256sum /t.txt", "file /t.txt",
                            "hexdump -n 16 /t.txt", "seq 3",
                            "test -f /t.txt", "arch", "nproc", "lscpu",
                            "lspci", "lsblk", "free", "cal 7 2026",
                            "id", "man grep", "tree /", "find / -name t*",
                            0
                        };
                        for (int c = 0; cu[c]; c++) {
                            serial_puts("[cu] $ ");
                            serial_puts(cu[c]);
                            serial_puts("\n");
                            char line[64];
                            str_copy(line, cu[c], sizeof(line));
                            term_exec(line);
                        }
                        serial_puts("[cu] done\n");
                    }
#endif
                serial_puts("[vextro/arm64] login: ");
                    serial_puts(user_name_of(user_current));
                    serial_puts(user_is_admin(user_current) ? " (admin)\n"
                                                            : "\n");
                } else {
                    melt_active = 1;
                    melt_tick = 0;
                    serial_puts("[vextro/arm64] login: refused\n");
                }
                break;
            }
            pw_len = 0;
            pw[0] = '\0';
        }
        if (desktop_mode) continue;

        {
            static char prompt_buf[96];
            const char *prompt;
            switch (login_stage) {
            case LOGIN_NEW_NAME:
                prompt = "Vextro 9 ARM64 - Create an account. Username:";
                break;
            case LOGIN_NEW_PW:     prompt = "Choose a password:"; break;
            case LOGIN_NEW_CONFIRM: prompt = "Type it once more:"; break;
            default:
                str_copy(prompt_buf, "Password for ", sizeof(prompt_buf));
                str_append(prompt_buf, user_name_of(login_sel),
                           sizeof(prompt_buf));
                str_append(prompt_buf, ":", sizeof(prompt_buf));
                prompt = prompt_buf;
                break;
            }
            if (login_msg[0]) prompt = login_msg;

            login_render(backbuf, w, h, mouse_x, mouse_y, pw,
                         mouse_buttons, prompt);

            if (login_stage == LOGIN_PASSWORD && user_count > 1)
                login_draw_users(backbuf, w, h, mouse_x, mouse_y,
                                 mouse_buttons);

            /* The administrator choice, while the account is being made. */
            if (login_stage != LOGIN_PASSWORD)
                login_draw_admin_check(backbuf, w, h, mouse_x, mouse_y,
                                       mouse_buttons);
            else
                login_draw_notice(backbuf, w, h);
        }

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
            serial_puts("[vextro/arm64] 120 frames in ");
            serial_put_u64(ms);
            serial_puts(" ms (");
            serial_put_u64(ms ? (120 * 1000) / ms : 0);
            serial_puts(" fps)\n");
        }

        next_frame += frame_ticks;
        timer_wait_until(next_frame);
    }
}
