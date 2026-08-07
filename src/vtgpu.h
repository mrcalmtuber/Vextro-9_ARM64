#ifndef VTGPU_H
#define VTGPU_H

#include <stdint.h>
#include "virtio.h"

/*
 * virtio-gpu, driven from the kernel.
 *
 * The reason this exists is a ceiling. Every display so far has come from
 * the firmware: Limine asks EDK2's GOP for a mode and hands over whatever
 * it gets. EDK2's ramfb driver offers three modes and stops at 1024x768,
 * and asking for more does not degrade to the next one — it fails to
 * match and falls back to 800x600, so requesting a larger screen produced
 * a smaller one. virtio-gpu-pci is no way out either: this EDK2 build
 * produces no GOP for it at all, so Limine reports no framebuffer and the
 * kernel halts before it starts.
 *
 * So the only way past 1024x768 is to stop asking the firmware. This
 * talks to the device directly, asks it what the display actually is, and
 * creates a scanout at that size.
 *
 * It costs far less than it sounds like, because it is the fifth virtio
 * device in this port rather than the first: transport, feature
 * negotiation, queue setup, descriptor chaining and the barriers are all
 * shared with input, block and network. What is specific to graphics is
 * six commands and a resource to point them at.
 *
 * MMIO again, not PCI — `-device virtio-gpu-device` binds to the same
 * transports as everything else, so the PCIe ECAM window the plan
 * budgeted for is still not needed.
 */

#define VIRTIO_ID_GPU 16

/* Commands, from the device's control queue vocabulary. */
#define VGPU_CMD_GET_DISPLAY_INFO      0x0100
#define VGPU_CMD_RESOURCE_CREATE_2D    0x0101
#define VGPU_CMD_SET_SCANOUT           0x0103
#define VGPU_CMD_RESOURCE_FLUSH        0x0104
#define VGPU_CMD_TRANSFER_TO_HOST_2D   0x0105
#define VGPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

/* Cursor queue vocabulary. These do not go on the control queue: the
 * device has a second virtqueue for them precisely so a pointer that
 * moves every frame never queues behind a scanout transfer. */
#define VGPU_CMD_UPDATE_CURSOR         0x0300
#define VGPU_CMD_MOVE_CURSOR           0x0301

#define VGPU_RESP_OK_NODATA            0x1100
#define VGPU_RESP_OK_DISPLAY_INFO      0x1101

/* B8G8R8X8: blue in the low byte, matching the 0x00RRGGBB words the
 * whole renderer already writes, so no per-pixel swizzle is needed. */
#define VGPU_FORMAT_B8G8R8X8 2

/* The cursor needs the alpha variant of the same layout: the sprite is a
 * 12x18 arrow inside a 64x64 image and everything around it has to be
 * transparent rather than black. */
#define VGPU_FORMAT_B8G8R8A8 1

struct vgpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct vgpu_display_one {
    struct vgpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct vgpu_resp_display_info {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_display_one pmodes[16];
} __attribute__((packed));

struct vgpu_resource_create_2d {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct vgpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_resource_attach_backing {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct vgpu_mem_entry entry;        /* exactly one; see below */
} __attribute__((packed));

struct vgpu_set_scanout {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct vgpu_transfer_to_host_2d {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_resource_flush {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* Both cursor commands share this shape; MOVE_CURSOR simply ignores the
 * resource and hotspot fields. */
struct vgpu_cursor_pos {
    uint32_t scanout_id;
    uint32_t x, y;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_update_cursor {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_cursor_pos pos;
    uint32_t resource_id;
    uint32_t hot_x, hot_y;
    uint32_t padding;
} __attribute__((packed));

static uint64_t vgpu_base = 0;
static virtq_t  vgpu_q;
static int      vgpu_ready = 0;
static uint32_t vgpu_w = 0, vgpu_h = 0;

static struct vring_desc  vgpu_desc[VQ_SIZE] __attribute__((aligned(16)));
static struct vring_avail vgpu_avail         __attribute__((aligned(16)));
static struct vring_used  vgpu_used          __attribute__((aligned(16)));

/* Queue 1: the cursor plane. Its own rings, because a pointer that moves
 * every frame must not queue behind a scanout transfer. */
static virtq_t  vgpu_cq;
static int      vgpu_cursor_ok = 0;
static int      vgpu_cursor_busy = 0;   /* commands the device has not read yet */
static struct vring_desc  vgpu_cdesc[VQ_SIZE] __attribute__((aligned(16)));
static struct vring_avail vgpu_cavail         __attribute__((aligned(16)));
static struct vring_used  vgpu_cused          __attribute__((aligned(16)));
static uint8_t  vgpu_creq[64] __attribute__((aligned(64)));

/* The cursor image. 64x64 is what the device expects; the arrow occupies
 * the top-left corner and the rest is transparent. */
#define VGPU_CURSOR_DIM 64
#define VGPU_CURSOR_RES 2
static uint32_t vgpu_cursor_img[VGPU_CURSOR_DIM * VGPU_CURSOR_DIM]
    __attribute__((aligned(4096)));

/* One request and one response buffer. Requests are issued one at a time
 * and waited on, so a single pair cannot be raced. */
static uint8_t vgpu_req[512] __attribute__((aligned(64)));
static uint8_t vgpu_rsp[512] __attribute__((aligned(64)));

/*
 * The scanout itself.
 *
 * Statically reserved at the largest size this kernel will present, and
 * page aligned so the single memory entry describing it is a clean run of
 * physical pages. The device is told the real dimensions at SET_SCANOUT
 * time; anything beyond them is simply never transferred.
 */
#define VGPU_MAX_W 1920
#define VGPU_MAX_H 1200
static uint32_t vgpu_fb[VGPU_MAX_W * VGPU_MAX_H] __attribute__((aligned(4096)));

/*
 * Issue one command and wait for the response.
 *
 * Two descriptors: the request the device reads and the response it
 * writes. Every command in this driver has that shape, so there is one
 * submit path rather than six.
 */
static int vgpu_cmd(uint32_t req_len, uint32_t rsp_len) {
    uint64_t p_req = virt_to_phys(vgpu_req);
    uint64_t p_rsp = virt_to_phys(vgpu_rsp);
    if (!p_req || !p_rsp) return -1;

    for (uint32_t i = 0; i < rsp_len; i++) vgpu_rsp[i] = 0;

    vq_buf_t bufs[2];
    bufs[0].phys = p_req; bufs[0].len = req_len; bufs[0].device_writes = 0;
    bufs[1].phys = p_rsp; bufs[1].len = rsp_len; bufs[1].device_writes = 1;

    virtq_offer_chain(vgpu_base, 0, &vgpu_q, 0, bufs, 2);

    uint64_t deadline = timer_count() + timer_hz() * 2;
    while (!virtq_has_used(&vgpu_q)) {
        if (timer_count() > deadline) return -1;
    }
    virtq_take(&vgpu_q);

    uint32_t st = vio_rd(vgpu_base, VIO_INT_STATUS);
    if (st) vio_wr(vgpu_base, VIO_INT_ACK, st);

    DMB();
    struct vgpu_ctrl_hdr *h = (struct vgpu_ctrl_hdr *)vgpu_rsp;
    return (h->type == VGPU_RESP_OK_NODATA ||
            h->type == VGPU_RESP_OK_DISPLAY_INFO) ? 0 : -1;
}

static void vgpu_hdr(uint32_t type) {
    for (uint32_t i = 0; i < sizeof(vgpu_req); i++) vgpu_req[i] = 0;
    struct vgpu_ctrl_hdr *h = (struct vgpu_ctrl_hdr *)vgpu_req;
    h->type = type;
}

/*
 * Bring the display up at whatever size the device reports.
 *
 * GET_DISPLAY_INFO first, deliberately: the point of this driver is to
 * stop guessing what the screen is. qemu reports the size its window or
 * `-g` option asked for, so the desktop comes up at the real display
 * dimensions instead of the largest mode some firmware table happens to
 * contain.
 */
static int vtgpu_init(uint32_t want_w, uint32_t want_h) {
    vgpu_ready = 0;
    vgpu_base = virtio_find(VIRTIO_ID_GPU, 0);
    if (!vgpu_base) return 0;
    if (!virtio_begin(vgpu_base)) return 0;
    if (!virtq_setup(vgpu_base, 0, &vgpu_q, vgpu_desc, &vgpu_avail, &vgpu_used))
        return 0;
    virtio_ready(vgpu_base);

    /* What is actually attached? */
    vgpu_hdr(VGPU_CMD_GET_DISPLAY_INFO);
    if (vgpu_cmd(sizeof(struct vgpu_ctrl_hdr),
                 sizeof(struct vgpu_resp_display_info)) != 0) {
        serial_puts("[vextro/arm64] virtio-gpu: display info refused\n");
        return 0;
    }
    struct vgpu_resp_display_info *di =
        (struct vgpu_resp_display_info *)vgpu_rsp;

    uint32_t dw = di->pmodes[0].r.width;
    uint32_t dh = di->pmodes[0].r.height;
    if (!di->pmodes[0].enabled || dw == 0 || dh == 0) { dw = want_w; dh = want_h; }
    if (dw > VGPU_MAX_W) dw = VGPU_MAX_W;
    if (dh > VGPU_MAX_H) dh = VGPU_MAX_H;

    /* Create the resource the scanout will read from. */
    vgpu_hdr(VGPU_CMD_RESOURCE_CREATE_2D);
    struct vgpu_resource_create_2d *c =
        (struct vgpu_resource_create_2d *)vgpu_req;
    c->resource_id = 1;
    c->format = VGPU_FORMAT_B8G8R8X8;
    c->width = dw;
    c->height = dh;
    if (vgpu_cmd(sizeof(*c), sizeof(struct vgpu_ctrl_hdr)) != 0) {
        serial_puts("[vextro/arm64] virtio-gpu: resource create refused\n");
        return 0;
    }

    /*
     * Back it with our own memory.
     *
     * One entry covering the whole framebuffer, which is only correct
     * because the buffer is a single physically contiguous run — it lives
     * in .bss, and Limine loads each segment as one block. A scattered
     * buffer would need one entry per run, and describing it as one would
     * hand the device the right length starting at the right page and
     * silently wrong pixels after the first discontinuity.
     */
    vgpu_hdr(VGPU_CMD_RESOURCE_ATTACH_BACKING);
    struct vgpu_resource_attach_backing *ab =
        (struct vgpu_resource_attach_backing *)vgpu_req;
    ab->resource_id = 1;
    ab->nr_entries = 1;
    ab->entry.addr = virt_to_phys(vgpu_fb);
    ab->entry.length = dw * dh * 4;
    if (!ab->entry.addr ||
        vgpu_cmd(sizeof(*ab), sizeof(struct vgpu_ctrl_hdr)) != 0) {
        serial_puts("[vextro/arm64] virtio-gpu: attach backing refused\n");
        return 0;
    }

    /* Point scanout 0 at it. */
    vgpu_hdr(VGPU_CMD_SET_SCANOUT);
    struct vgpu_set_scanout *ss = (struct vgpu_set_scanout *)vgpu_req;
    ss->r.x = 0; ss->r.y = 0; ss->r.width = dw; ss->r.height = dh;
    ss->scanout_id = 0;
    ss->resource_id = 1;
    if (vgpu_cmd(sizeof(*ss), sizeof(struct vgpu_ctrl_hdr)) != 0) {
        serial_puts("[vextro/arm64] virtio-gpu: set scanout refused\n");
        return 0;
    }

    vgpu_w = dw; vgpu_h = dh;
    vgpu_ready = 1;

    serial_puts("[vextro/arm64] virtio-gpu: scanout ");
    serial_put_u64(dw); serial_puts("x"); serial_put_u64(dh);
    serial_puts("\n");

    /*
     * The cursor plane.
     *
     * Everything below is optional: if the device has no second queue, or
     * refuses any of these, vgpu_cursor_ok stays 0 and the caller falls
     * back to compositing the pointer in software. virtq_setup returns 0
     * for a queue that does not exist, so probing is safe.
     */
    if (virtq_setup(vgpu_base, 1, &vgpu_cq, vgpu_cdesc, &vgpu_cavail,
                    &vgpu_cused)) {
        vgpu_hdr(VGPU_CMD_RESOURCE_CREATE_2D);
        struct vgpu_resource_create_2d *cc =
            (struct vgpu_resource_create_2d *)vgpu_req;
        cc->resource_id = VGPU_CURSOR_RES;
        cc->format = VGPU_FORMAT_B8G8R8A8;   /* alpha, unlike the scanout */
        cc->width = VGPU_CURSOR_DIM;
        cc->height = VGPU_CURSOR_DIM;

        if (vgpu_cmd(sizeof(*cc), sizeof(struct vgpu_ctrl_hdr)) == 0) {
            vgpu_hdr(VGPU_CMD_RESOURCE_ATTACH_BACKING);
            struct vgpu_resource_attach_backing *cb =
                (struct vgpu_resource_attach_backing *)vgpu_req;
            cb->resource_id = VGPU_CURSOR_RES;
            cb->nr_entries = 1;
            cb->entry.addr = virt_to_phys(vgpu_cursor_img);
            cb->entry.length = sizeof(vgpu_cursor_img);
            if (cb->entry.addr &&
                vgpu_cmd(sizeof(*cb), sizeof(struct vgpu_ctrl_hdr)) == 0)
                vgpu_cursor_ok = 1;
        }
    }

    serial_puts(vgpu_cursor_ok
                ? "[vextro/arm64] virtio-gpu: hardware cursor plane\n"
                : "[vextro/arm64] virtio-gpu: no cursor queue, "
                  "compositing the pointer\n");
    return 1;
}

/*
 * Hand the sprite to the device, once.
 *
 * `img` is the arrow as ARGB rows; everything outside it is transparent.
 * The transfer goes on the *control* queue -- it is an ordinary resource
 * update -- while the UPDATE_CURSOR that arms the plane goes on the
 * cursor queue.
 */
static void vtgpu_cursor_define(const uint32_t *img, uint32_t iw, uint32_t ih,
                                uint32_t hot_x, uint32_t hot_y) {
    if (!vgpu_cursor_ok) return;

    for (uint32_t i = 0; i < VGPU_CURSOR_DIM * VGPU_CURSOR_DIM; i++)
        vgpu_cursor_img[i] = 0;
    for (uint32_t y = 0; y < ih && y < VGPU_CURSOR_DIM; y++)
        for (uint32_t x = 0; x < iw && x < VGPU_CURSOR_DIM; x++)
            vgpu_cursor_img[y * VGPU_CURSOR_DIM + x] = img[y * iw + x];
    DSB();

    vgpu_hdr(VGPU_CMD_TRANSFER_TO_HOST_2D);
    struct vgpu_transfer_to_host_2d *t =
        (struct vgpu_transfer_to_host_2d *)vgpu_req;
    t->r.x = 0; t->r.y = 0;
    t->r.width = VGPU_CURSOR_DIM; t->r.height = VGPU_CURSOR_DIM;
    t->offset = 0;
    t->resource_id = VGPU_CURSOR_RES;
    if (vgpu_cmd(sizeof(*t), sizeof(struct vgpu_ctrl_hdr)) != 0) {
        vgpu_cursor_ok = 0;
        return;
    }

    /* Arm the plane. The cursor queue takes no response. */
    for (uint32_t i = 0; i < sizeof(vgpu_creq); i++) vgpu_creq[i] = 0;
    struct vgpu_update_cursor *u = (struct vgpu_update_cursor *)vgpu_creq;
    u->hdr.type = VGPU_CMD_UPDATE_CURSOR;
    u->pos.scanout_id = 0;
    u->pos.x = 0; u->pos.y = 0;
    u->resource_id = VGPU_CURSOR_RES;
    u->hot_x = hot_x; u->hot_y = hot_y;
    DSB();

    uint64_t p = virt_to_phys(vgpu_creq);
    if (!p) { vgpu_cursor_ok = 0; return; }
    vq_buf_t b;
    b.phys = p; b.len = sizeof(*u); b.device_writes = 0;
    virtq_offer_chain(vgpu_base, 1, &vgpu_cq, 0, &b, 1);
    vgpu_cursor_busy++;
}

/*
 * Move the pointer.
 *
 * One 56-byte command and no scanout traffic at all -- the whole reason
 * for driving the plane rather than compositing. Deliberately not waited
 * on: a cursor position is a hint the device applies when it can, and
 * blocking on it would put a round trip in every frame to no purpose.
 */
static void vtgpu_cursor_move(uint32_t x, uint32_t y) {
    if (!vgpu_cursor_ok) return;

    /*
     * There is one request buffer, and the device reads it
     * asynchronously, so it cannot be refilled until the previous command
     * has been consumed. Reap whatever has completed; if the outstanding
     * one has not, skip this frame rather than overwrite a buffer the
     * device is still reading. A dropped move costs nothing -- the next
     * frame sends the current position, not a stale one.
     */
    while (virtq_has_used(&vgpu_cq)) {
        virtq_take(&vgpu_cq);
        if (vgpu_cursor_busy) vgpu_cursor_busy--;
    }
    if (vgpu_cursor_busy) return;

    for (uint32_t i = 0; i < sizeof(vgpu_creq); i++) vgpu_creq[i] = 0;
    struct vgpu_update_cursor *u = (struct vgpu_update_cursor *)vgpu_creq;
    u->hdr.type = VGPU_CMD_MOVE_CURSOR;
    u->pos.scanout_id = 0;
    u->pos.x = x;
    u->pos.y = y;
    u->resource_id = VGPU_CURSOR_RES;
    DSB();

    uint64_t p = virt_to_phys(vgpu_creq);
    if (!p) return;
    vq_buf_t b;
    b.phys = p; b.len = sizeof(*u); b.device_writes = 0;
    virtq_offer_chain(vgpu_base, 1, &vgpu_cq, 0, &b, 1);
    vgpu_cursor_busy++;
}

/*
 * Present the framebuffer.
 *
 * Two commands, and both are needed: TRANSFER_TO_HOST_2D copies the
 * guest's pixels into the host's copy of the resource, and
 * RESOURCE_FLUSH tells it to put that on the screen. Skipping the
 * transfer leaves the display showing whatever it had; skipping the flush
 * updates a resource nobody looks at. Unlike a linear framebuffer, where
 * a store is the whole story, nothing here is implicit.
 */
static void vtgpu_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!vgpu_ready) return;
    if (w == 0 || h == 0) return;

    vgpu_hdr(VGPU_CMD_TRANSFER_TO_HOST_2D);
    struct vgpu_transfer_to_host_2d *t =
        (struct vgpu_transfer_to_host_2d *)vgpu_req;
    t->r.x = x; t->r.y = y; t->r.width = w; t->r.height = h;
    t->offset = (uint64_t)y * vgpu_w * 4 + (uint64_t)x * 4;
    t->resource_id = 1;
    if (vgpu_cmd(sizeof(*t), sizeof(struct vgpu_ctrl_hdr)) != 0) return;

    vgpu_hdr(VGPU_CMD_RESOURCE_FLUSH);
    struct vgpu_resource_flush *f = (struct vgpu_resource_flush *)vgpu_req;
    f->r.x = x; f->r.y = y; f->r.width = w; f->r.height = h;
    f->resource_id = 1;
    vgpu_cmd(sizeof(*f), sizeof(struct vgpu_ctrl_hdr));
}

#endif /* VTGPU_H */
