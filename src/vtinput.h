#ifndef VTINPUT_H
#define VTINPUT_H

#include <stdint.h>
#include "virtio.h"
#include "keyboard.h"

/*
 * Keyboard and pointer, over virtio-input.
 *
 * This is where the x86 tree's two most awkward drivers stop existing.
 * There, a mouse means a PS/2 controller, a three- or four-byte packet
 * whose length has to be discovered by a sample-rate knock, sign bits in
 * a status byte, and — because none of that gives absolute coordinates —
 * an entire second driver (vmmouse.h) talking to a VMware backdoor port
 * to get a pointer that tracks the host cursor instead of drifting away
 * from it.
 *
 * virtio's tablet is absolute natively. The device reports its axis range
 * in its config space, the events carry a position rather than a delta,
 * and the pointer lands where the host's does with no calibration, no
 * acceleration to undo and no backdoor. The whole of vmmouse.h and all
 * the packet-length negotiation in mouse.h are deleted, not ported.
 *
 * Both devices speak the same event record, so one drain routine serves
 * both and the difference between a keyboard and a tablet is which codes
 * turn up in it.
 */

/* Event types, from the Linux input layer that virtio-input borrows. */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_SYN 0x00
#define EV_REL 0x02
#define EV_ABS 0x03

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

#define ABS_X 0x00
#define ABS_Y 0x01

#define REL_X     0x00
#define REL_Y     0x01
#define REL_WHEEL 0x08

/* Linux keycodes that have no ASCII form. The main block needs no such
 * table — it already matches the scancode arrays in keyboard.h. */
#define LK_LEFTSHIFT  42
#define LK_RIGHTSHIFT 54
#define LK_HOME      102
#define LK_UP        103
#define LK_PAGEUP    104
#define LK_LEFT      105
#define LK_RIGHT     106
#define LK_END       107
#define LK_DOWN      108
#define LK_PAGEDOWN  109
#define LK_DELETE    111

/* Config space selectors */
#define VI_CFG_ID_NAME  0x01
#define VI_CFG_ABS_INFO 0x12

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

/* ---- pointer state, as the portable UI expects it ---- */

/* Relative motion banked between EV_SYN boundaries; see EV_REL. */
static int32_t vti_rel_dx = 0, vti_rel_dy = 0;

volatile int32_t mouse_x = 0;
volatile int32_t mouse_y = 0;
volatile uint8_t mouse_buttons = 0;
volatile int32_t mouse_wheel = 0;
volatile int     mouse_absolute = 0;

static int32_t vti_screen_w = 1, vti_screen_h = 1;

/*
 * Pointer facts term.h's `mouse` diagnostic reports.
 *
 * On x86 these describe a negotiated PS/2 stream: how many bytes each
 * packet turned out to be, and the tablet's coordinate ceiling. Here the
 * event record is a fixed 8-byte struct and the ceiling is the screen,
 * because the device reports absolute positions that this driver has
 * already scaled. The names stay so the diagnostic does.
 */
static int mouse_pkt_len = (int)sizeof(struct virtio_input_event);
#define mouse_max_x vti_screen_w
#define mouse_max_y vti_screen_h

/*
 * Two devices, each with its own rings.
 *
 * Statically allocated because there is no allocator, and 16-byte aligned
 * because the descriptor table must be — the spec's alignment rules are
 * about what the device may assume, and a misaligned ring is the kind of
 * fault that shows up as silently missing events rather than an error.
 */
#define VTI_EVENTS 32

typedef struct {
    uint64_t base;
    virtq_t  eventq;
    struct vring_desc  desc[VQ_SIZE]   __attribute__((aligned(16)));
    struct vring_avail avail           __attribute__((aligned(16)));
    struct vring_used  used            __attribute__((aligned(16)));
    struct virtio_input_event ev[VTI_EVENTS] __attribute__((aligned(16)));
    int live;
} vti_dev_t;

static vti_dev_t vti_kbd;
static vti_dev_t vti_tablet;

/* Absolute axis ranges, read from the tablet's config space. */
static int32_t vti_abs_min_x = 0, vti_abs_max_x = 32767;
static int32_t vti_abs_min_y = 0, vti_abs_max_y = 32767;

static uint32_t vti_cfg_u32(uint64_t base, uint32_t off) {
    return *mmio32(base + VIO_CONFIG + off);
}

static void vti_select(uint64_t base, uint8_t sel, uint8_t subsel) {
    volatile uint8_t *cfg = (volatile uint8_t *)mmio32(base + VIO_CONFIG);
    cfg[0] = sel;
    cfg[1] = subsel;
    DSB();
}

/*
 * Post every event buffer to the device.
 *
 * A virtio-input device only writes where it has been given room to
 * write, so the queue is filled once at start-up and each buffer is
 * handed straight back after being read. Running out means dropped
 * keystrokes, which is why the ring is deeper than a frame's worth of
 * plausible input rather than the two or three entries it would take to
 * work in the common case.
 */
static void vti_post_all(vti_dev_t *d) {
    for (uint16_t i = 0; i < VTI_EVENTS && i < d->eventq.qsize; i++) {
        uint64_t p = virt_to_phys(&d->ev[i]);
        if (!p) continue;
        virtq_offer(d->base, 0, &d->eventq, i, p,
                    (uint32_t)sizeof(struct virtio_input_event), 1);
    }
}

static int vti_start(vti_dev_t *d, uint32_t index) {
    d->live = 0;
    d->base = virtio_find(VIRTIO_ID_INPUT, index);
    if (!d->base) return 0;
    if (!virtio_begin(d->base)) return 0;
    if (!virtq_setup(d->base, 0, &d->eventq, d->desc, &d->avail, &d->used))
        return 0;
    virtio_ready(d->base);
    vti_post_all(d);
    d->live = 1;
    return 1;
}

/*
 * Which device is which.
 *
 * The transports are scanned in slot order, and slot order reflects the
 * qemu command line rather than anything about the devices, so a driver
 * that assumed "first is the keyboard" would break whenever the arguments
 * were reordered. Asking each device for its ABS_X range settles it: a
 * tablet reports a non-empty absolute axis and a keyboard does not.
 */
static int vti_is_tablet(uint64_t base) {
    vti_select(base, VI_CFG_ABS_INFO, ABS_X);
    volatile uint8_t *cfg = (volatile uint8_t *)mmio32(base + VIO_CONFIG);
    if (cfg[2] == 0) return 0;                  /* size 0: no such axis */
    uint32_t max = vti_cfg_u32(base, 8 + 4);    /* u.abs.max */
    return max > 0;
}

static void vti_read_abs_range(uint64_t base) {
    vti_select(base, VI_CFG_ABS_INFO, ABS_X);
    vti_abs_min_x = (int32_t)vti_cfg_u32(base, 8 + 0);
    vti_abs_max_x = (int32_t)vti_cfg_u32(base, 8 + 4);
    vti_select(base, VI_CFG_ABS_INFO, ABS_Y);
    vti_abs_min_y = (int32_t)vti_cfg_u32(base, 8 + 0);
    vti_abs_max_y = (int32_t)vti_cfg_u32(base, 8 + 4);

    if (vti_abs_max_x <= vti_abs_min_x) { vti_abs_min_x = 0; vti_abs_max_x = 32767; }
    if (vti_abs_max_y <= vti_abs_min_y) { vti_abs_min_y = 0; vti_abs_max_y = 32767; }
}

/*
 * Find and start both devices.
 *
 * The scan runs over every input transport rather than assuming a count,
 * so a machine with only a keyboard, only a tablet, or neither all behave
 * sensibly — the pointer simply stays where it was put.
 */
static void vtinput_init(int32_t screen_w, int32_t screen_h) {
    vti_screen_w = screen_w > 0 ? screen_w : 1;
    vti_screen_h = screen_h > 0 ? screen_h : 1;

    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;

    /* Report the transports as found. A device that is present but
     * speaking the legacy layout, or absent from the command line
     * entirely, are very different problems with the same symptom — no
     * input — and the slot dump distinguishes them immediately. */
    for (uint32_t i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uint64_t b = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_STRIDE;
        if (vio_rd(b, VIO_MAGIC) != VIO_MAGIC_VALUE) continue;
        uint32_t dev = vio_rd(b, VIO_DEVICE_ID);
        if (dev == 0) continue;                 /* empty slot */
        serial_puts("[vextro/arm64]   virtio slot ");
        serial_put_u64(i);
        serial_puts(": device ");
        serial_put_u64(dev);
        serial_puts(", version ");
        serial_put_u64(vio_rd(b, VIO_VERSION));
        serial_puts("\n");
    }

    uint32_t kbd_idx = 0xFFFFFFFFu, tab_idx = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uint64_t base = virtio_find(VIRTIO_ID_INPUT, i);
        if (!base) break;
        if (vti_is_tablet(base)) {
            if (tab_idx == 0xFFFFFFFFu) { tab_idx = i; vti_read_abs_range(base); }
        } else {
            if (kbd_idx == 0xFFFFFFFFu) kbd_idx = i;
        }
    }

    if (kbd_idx != 0xFFFFFFFFu) vti_start(&vti_kbd, kbd_idx);
    if (tab_idx != 0xFFFFFFFFu) {
        if (vti_start(&vti_tablet, tab_idx)) mouse_absolute = 1;
    }

    serial_puts("[vextro/arm64] virtio-input: keyboard ");
    serial_puts(vti_kbd.live ? "yes" : "no");
    serial_puts(", tablet ");
    serial_puts(vti_tablet.live ? "yes" : "no");
    if (vti_tablet.live) {
        serial_puts(" (abs x 0..");
        serial_put_u64((uint64_t)vti_abs_max_x);
        serial_puts(", y 0..");
        serial_put_u64((uint64_t)vti_abs_max_y);
        serial_puts(")");
    }
    serial_puts("\n");
}

static void vti_key_event(uint16_t code, uint32_t value) {
    /* value: 0 release, 1 press, 2 autorepeat */
    if (code == LK_LEFTSHIFT || code == LK_RIGHTSHIFT) {
        kb_shift = (value != 0);
        return;
    }
    /*
     * Buttons before the release filter, not after.
     *
     * A key release types nothing, so the filter below is right for a
     * keyboard — and mouse buttons arrive as key events on the tablet's
     * queue, through this same function, where a release is the entire
     * point. Discarding it left the button latched down forever.
     *
     * The symptom was odd enough to be worth recording: the pointer
     * tracked perfectly, dock icons highlighted under it, and the first
     * click after boot opened its window. Every click after that did
     * nothing at all, because the desktop triggers on the press *edge*
     * and the button never came back up to make another one. It reads
     * as a dock that stops responding, not as a missing release.
     */
    if (code >= 128) {
        if (code == BTN_LEFT)
            mouse_buttons = (uint8_t)((mouse_buttons & ~1u) | (value ? 1u : 0u));
        else if (code == BTN_RIGHT)
            mouse_buttons = (uint8_t)((mouse_buttons & ~2u) | (value ? 2u : 0u));
        else if (code == BTN_MIDDLE)
            mouse_buttons = (uint8_t)((mouse_buttons & ~4u) | (value ? 4u : 0u));
        return;
    }

    if (value == 0) return;             /* releases produce no characters */

    switch (code) {
    case LK_UP:       kb_push(KEY_UP);    return;
    case LK_DOWN:     kb_push(KEY_DOWN);  return;
    case LK_LEFT:     kb_push(KEY_LEFT);  return;
    case LK_RIGHT:    kb_push(KEY_RIGHT); return;
    case LK_PAGEUP:   kb_push(KEY_PGUP);  return;
    case LK_PAGEDOWN: kb_push(KEY_PGDN);  return;
    case LK_HOME:     kb_push(KEY_HOME);  return;
    case LK_END:      kb_push(KEY_END);   return;
    case LK_DELETE:   kb_push(KEY_DEL);   return;
    default: break;
    }

    if (code < 128) {
        char ch = kb_shift ? scancode_upper[code] : scancode_lower[code];
        if (ch) kb_push(ch);
        return;
    }

}

/*
 * Scale a device axis onto the screen.
 *
 * 64-bit intermediate on purpose: the tablet's range is 0..32767 and a
 * wide panel is a few thousand pixels, so the product overflows 32 bits
 * for the right half of the screen and the pointer would wrap to the left
 * edge — a bug that looks like the mouse jumping rather than like
 * arithmetic.
 */
static int32_t vti_scale(int32_t v, int32_t lo, int32_t hi, int32_t span) {
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    int64_t out = (int64_t)(v - lo) * (int64_t)(span - 1) / (int64_t)(hi - lo);
    return (int32_t)out;
}

static void vti_drain(vti_dev_t *d) {
    if (!d->live) return;

    while (virtq_has_used(&d->eventq)) {
        struct vring_used_elem *e = virtq_take(&d->eventq);
        uint32_t slot = e->id;
        if (slot >= VTI_EVENTS) continue;

        /*
         * The device wrote this buffer; see all of it.
         *
         * Seeing the used-ring index advance and reading the event it
         * points at are two loads with nothing ordering them, and this
         * architecture is free to satisfy the second from before the
         * first. The network driver has carried this barrier since it was
         * written; this one never did, because the failure does not look
         * like a memory-ordering bug.
         *
         * What it looked like: the pointer tracked perfectly and hover
         * highlighted dock icons, but clicking did nothing. A button is
         * an EV_KEY whose `value` says pressed or released, so a stale
         * read leaves the *release* carrying the press's value — the
         * button latches down and never comes up. The first click after
         * boot works and no click after it ever produces a press edge
         * again, which reads as a broken dock rather than a missing dmb.
         */
        DMB();
        struct virtio_input_event ev = d->ev[slot];

        switch (ev.type) {
        case EV_KEY:
#ifdef INPUT_TRACE
            if (ev.code >= 128) {
                serial_puts("[ev] key code ");
                serial_put_u64(ev.code);
                serial_puts(" value ");
                serial_put_u64(ev.value);
                serial_putc('\n');
            }
#endif
            vti_key_event(ev.code, ev.value);
            break;
        case EV_ABS:
            if (ev.code == ABS_X)
                mouse_x = vti_scale((int32_t)ev.value, vti_abs_min_x,
                                    vti_abs_max_x, vti_screen_w);
            else if (ev.code == ABS_Y)
                mouse_y = vti_scale((int32_t)ev.value, vti_abs_min_y,
                                    vti_abs_max_y, vti_screen_h);
            break;
        case EV_REL:
            /*
             * Banked rather than applied. The two axes arrive as separate
             * events and the acceleration curve needs the whole movement
             * to judge its speed -- taking them one at a time would read a
             * diagonal flick as two slow moves and refuse to accelerate
             * either. EV_SYN below is where a report ends.
             */
            if (ev.code == REL_X)          vti_rel_dx += (int32_t)ev.value;
            else if (ev.code == REL_Y)     vti_rel_dy += (int32_t)ev.value;
            else if (ev.code == REL_WHEEL) mouse_wheel += (int32_t)ev.value;
            break;
        case EV_SYN:
            if (vti_rel_dx || vti_rel_dy) {
                int32_t dx = vti_rel_dx, dy = vti_rel_dy;
                vti_rel_dx = vti_rel_dy = 0;
                paccel_apply(&dx, &dy);
                mouse_x += dx;
                mouse_y += dy;
            }
            break;
        default:
            break;
        }

        /* Hand the buffer straight back; the device needs the room. */
        uint64_t p = virt_to_phys(&d->ev[slot]);
        if (p) virtq_offer(d->base, 0, &d->eventq, (uint16_t)slot, p,
                           (uint32_t)sizeof(struct virtio_input_event), 1);
    }

    /* Acknowledge whatever the device raised. Nothing is routed to the
     * CPU yet, but an unacknowledged status latches and would block the
     * interrupt the moment the GIC does come up. */
    uint32_t st = vio_rd(d->base, VIO_INT_STATUS);
    if (st) vio_wr(d->base, VIO_INT_ACK, st);
}

/* Called once per frame from the render loop's poll phase. */
static void vtinput_poll(void) {
    vti_drain(&vti_kbd);
    vti_drain(&vti_tablet);

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= vti_screen_w) mouse_x = vti_screen_w - 1;
    if (mouse_y >= vti_screen_h) mouse_y = vti_screen_h - 1;
}

#endif /* VTINPUT_H */
