#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include "arm.h"

/*
 * virtio over MMIO, and the split virtqueue that rides on it.
 *
 * This replaces nothing in the x86 tree — it is the mechanism that lets
 * the rest of the port delete drivers rather than translate them. PS/2,
 * the VMware backdoor, ATA PIO and e1000 are four unrelated protocols on
 * the x86 side; on this machine keyboard, tablet, disk and network are
 * all the same ring buffer with a different device ID, so nearly all of
 * the cost is paid once, here.
 *
 * MMIO rather than PCI, deliberately. QEMU's `virt` machine exposes both,
 * but the PCI route needs the ECAM window mapped, a bus walk, BAR sizing
 * and capability parsing before a single byte moves — page-table work
 * that belongs to the storage milestone. The MMIO transports sit at fixed
 * addresses in a region this kernel already maps, so input can work now
 * and the PCI path can arrive when something actually needs it.
 *
 * Polled, not interrupt-driven. The whole system is a render loop that
 * already visits every device once a frame, and a used-ring index is
 * cheaper to read than an interrupt is to route through a GIC that does
 * not exist yet. This is the same choice the x86 tree made for the mouse.
 */

/* ---- transport registers ---- */

#define VIRTIO_MMIO_BASE        virtio_base
#define VIRTIO_MMIO_STRIDE      0x200UL
#define VIRTIO_MMIO_COUNT       32

#define VIO_MAGIC               0x000   /* "virt" */
#define VIO_VERSION             0x004
#define VIO_DEVICE_ID           0x008
#define VIO_VENDOR_ID           0x00C
#define VIO_DEV_FEAT            0x010
#define VIO_DEV_FEAT_SEL        0x014
#define VIO_DRV_FEAT            0x020
#define VIO_DRV_FEAT_SEL        0x024
#define VIO_QUEUE_SEL           0x030
#define VIO_QUEUE_NUM_MAX       0x034
#define VIO_QUEUE_NUM           0x038
#define VIO_QUEUE_READY         0x044
#define VIO_QUEUE_NOTIFY        0x050
#define VIO_INT_STATUS          0x060
#define VIO_INT_ACK             0x064
#define VIO_STATUS              0x070
#define VIO_QUEUE_DESC_LO       0x080
#define VIO_QUEUE_DESC_HI       0x084
#define VIO_QUEUE_DRIVER_LO     0x090
#define VIO_QUEUE_DRIVER_HI     0x094
#define VIO_QUEUE_DEVICE_LO     0x0A0
#define VIO_QUEUE_DEVICE_HI     0x0A4
#define VIO_CONFIG              0x100

#define VIO_MAGIC_VALUE         0x74726976u     /* 'virt' little-endian */

#define VIO_STATUS_ACK          1
#define VIO_STATUS_DRIVER       2
#define VIO_STATUS_DRIVER_OK    4
#define VIO_STATUS_FEATURES_OK  8
#define VIO_STATUS_FAILED       128

#define VIRTIO_ID_INPUT         18

/* Bit 32 of the feature space. Refusing to offer it puts a modern device
 * into legacy mode, where the queue layout and config endianness differ;
 * this driver only speaks the 1.0 layout, so it is required, not optional. */
#define VIRTIO_F_VERSION_1_BIT  32

/* ---- split virtqueue ---- */

#define VQ_SIZE 64                      /* power of two, per the spec */

#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2         /* device writes, driver reads */

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_SIZE];
    uint16_t used_event;
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VQ_SIZE];
    uint16_t avail_event;
} __attribute__((packed));

/*
 * One queue's memory plus the bookkeeping to drive it.
 *
 * The three rings are separate allocations rather than one contiguous
 * block. Legacy virtio required them packed together with padding to a
 * page boundary; the 1.0 MMIO transport takes three independent physical
 * addresses, which means each can simply be a static array with its own
 * alignment and no arithmetic to get wrong.
 */
typedef struct {
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t last_used;                 /* how far we have consumed */
    uint16_t qsize;
} virtq_t;

typedef struct {
    uint64_t base;                      /* transport MMIO base, 0 if absent */
    uint32_t device_id;
} virtio_dev_t;

static inline volatile uint32_t *vio_reg(uint64_t base, uint32_t off) {
    return mmio32(base + off);
}

static inline uint32_t vio_rd(uint64_t base, uint32_t off) {
    return *vio_reg(base, off);
}

static inline void vio_wr(uint64_t base, uint32_t off, uint32_t v) {
    *vio_reg(base, off) = v;
}

/*
 * Find the nth transport advertising `device_id`.
 *
 * QEMU populates its 32 MMIO slots from the top down and leaves the rest
 * reporting device 0, so a scan is the only way to know where anything
 * landed — and the order depends on the command line, not on the slot.
 */
static uint64_t virtio_find(uint32_t device_id, uint32_t index) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_STRIDE;
        if (vio_rd(base, VIO_MAGIC) != VIO_MAGIC_VALUE) continue;
        if (vio_rd(base, VIO_VERSION) != 2) continue;   /* 1.0 layout only */
        if (vio_rd(base, VIO_DEVICE_ID) != device_id) continue;
        if (seen++ == index) return base;
    }
    return 0;
}

/*
 * Bring a device up to the point where queues may be configured.
 *
 * The status handshake is a sequence, not a set of flags: the device is
 * allowed to reject the feature set, and it says so by clearing
 * FEATURES_OK when it is read back. Checking that is the difference
 * between a driver that fails at negotiation with a clear message and one
 * that fails later, on a queue that was never really accepted.
 */
static int virtio_begin(uint64_t base) {
    vio_wr(base, VIO_STATUS, 0);                        /* reset */
    DSB();
    vio_wr(base, VIO_STATUS, VIO_STATUS_ACK);
    vio_wr(base, VIO_STATUS, VIO_STATUS_ACK | VIO_STATUS_DRIVER);

    /* Offer exactly VIRTIO_F_VERSION_1 and nothing else. */
    vio_wr(base, VIO_DRV_FEAT_SEL, 1);
    vio_wr(base, VIO_DRV_FEAT, 1u << (VIRTIO_F_VERSION_1_BIT - 32));
    vio_wr(base, VIO_DRV_FEAT_SEL, 0);
    vio_wr(base, VIO_DRV_FEAT, 0);

    vio_wr(base, VIO_STATUS,
           VIO_STATUS_ACK | VIO_STATUS_DRIVER | VIO_STATUS_FEATURES_OK);
    DSB();
    if (!(vio_rd(base, VIO_STATUS) & VIO_STATUS_FEATURES_OK)) {
        vio_wr(base, VIO_STATUS, VIO_STATUS_FAILED);
        return 0;
    }
    return 1;
}

static void virtio_ready(uint64_t base) {
    vio_wr(base, VIO_STATUS,
           VIO_STATUS_ACK | VIO_STATUS_DRIVER |
           VIO_STATUS_FEATURES_OK | VIO_STATUS_DRIVER_OK);
    DSB();
}

/*
 * Hand a queue's three rings to the device.
 *
 * Physical addresses, not the virtual ones this kernel uses: the device
 * is on the other side of the MMU and sees memory as the bus does. Asking
 * the hardware to translate (AT S1E1R, via virt_to_phys) is better than
 * computing it, because it is the same walk the CPU will do and so cannot
 * disagree with it.
 */
static int virtq_setup(uint64_t base, uint32_t qidx, virtq_t *q,
                       struct vring_desc *desc,
                       struct vring_avail *avail,
                       struct vring_used *used) {
    vio_wr(base, VIO_QUEUE_SEL, qidx);
    if (vio_rd(base, VIO_QUEUE_READY) != 0) return 0;   /* already live */

    uint32_t max = vio_rd(base, VIO_QUEUE_NUM_MAX);
    if (max == 0) return 0;                             /* no such queue */
    uint32_t size = max < VQ_SIZE ? max : VQ_SIZE;

    q->desc = desc; q->avail = avail; q->used = used;
    q->qsize = (uint16_t)size;
    q->last_used = 0;

    for (uint32_t i = 0; i < size; i++) {
        desc[i].addr = 0; desc[i].len = 0;
        desc[i].flags = 0; desc[i].next = 0;
        avail->ring[i] = 0;
        used->ring[i].id = 0; used->ring[i].len = 0;
    }
    avail->flags = 0; avail->idx = 0; avail->used_event = 0;
    used->flags  = 0; used->idx  = 0; used->avail_event = 0;
    DSB();

    uint64_t pd = virt_to_phys(desc);
    uint64_t pa = virt_to_phys(avail);
    uint64_t pu = virt_to_phys(used);
    if (!pd || !pa || !pu) return 0;

    vio_wr(base, VIO_QUEUE_NUM, size);
    vio_wr(base, VIO_QUEUE_DESC_LO,   (uint32_t)pd);
    vio_wr(base, VIO_QUEUE_DESC_HI,   (uint32_t)(pd >> 32));
    vio_wr(base, VIO_QUEUE_DRIVER_LO, (uint32_t)pa);
    vio_wr(base, VIO_QUEUE_DRIVER_HI, (uint32_t)(pa >> 32));
    vio_wr(base, VIO_QUEUE_DEVICE_LO, (uint32_t)pu);
    vio_wr(base, VIO_QUEUE_DEVICE_HI, (uint32_t)(pu >> 32));
    DSB();
    vio_wr(base, VIO_QUEUE_READY, 1);
    DSB();
    return 1;
}

/*
 * Offer one buffer to the device and ring the doorbell.
 *
 * The barrier before publishing the index is the whole correctness
 * argument on this architecture. x86 stores are ordered against each
 * other, so the equivalent e1000 code in the x86 tree gets away with none
 * at all; aarch64 makes no such promise, and without the dsb the device
 * can observe a bumped avail->idx while the descriptor it points at is
 * still the previous frame's. That is a bug which appears as occasional
 * wrong input under load and never reproduces on demand.
 */
static void virtq_offer(uint64_t base, uint32_t qidx, virtq_t *q,
                        uint16_t slot, uint64_t phys, uint32_t len,
                        int device_writes) {
    q->desc[slot].addr  = phys;
    q->desc[slot].len   = len;
    q->desc[slot].flags = device_writes ? VRING_DESC_F_WRITE : 0;
    q->desc[slot].next  = 0;

    q->avail->ring[q->avail->idx % q->qsize] = slot;
    DSB();                              /* descriptor visible before index */
    q->avail->idx = (uint16_t)(q->avail->idx + 1);
    DSB();                              /* index visible before doorbell */
    vio_wr(base, VIO_QUEUE_NOTIFY, qidx);
}

/*
 * Offer a chain of buffers as one request.
 *
 * Block devices need this and input devices do not: a read is a header
 * the device reads, a payload it writes, and a status byte it writes,
 * and the three must arrive as a single unit with one head index. The
 * descriptors are linked through `next` and only the head goes into the
 * available ring — publishing them separately would present three
 * unrelated requests, none of them well-formed.
 */
typedef struct {
    uint64_t phys;
    uint32_t len;
    int      device_writes;
} vq_buf_t;

static void virtq_offer_chain(uint64_t base, uint32_t qidx, virtq_t *q,
                              uint16_t head, const vq_buf_t *bufs, int n) {
    for (int i = 0; i < n; i++) {
        uint16_t slot = (uint16_t)(head + i);
        q->desc[slot].addr  = bufs[i].phys;
        q->desc[slot].len   = bufs[i].len;
        q->desc[slot].flags = (uint16_t)
            ((bufs[i].device_writes ? VRING_DESC_F_WRITE : 0) |
             (i + 1 < n ? VRING_DESC_F_NEXT : 0));
        q->desc[slot].next  = (uint16_t)(head + i + 1);
    }

    q->avail->ring[q->avail->idx % q->qsize] = head;
    DSB();
    q->avail->idx = (uint16_t)(q->avail->idx + 1);
    DSB();
    vio_wr(base, VIO_QUEUE_NOTIFY, qidx);
}

/* Has the device returned anything? */
static inline int virtq_has_used(virtq_t *q) {
    DMB();
    return q->used->idx != q->last_used;
}

static inline struct vring_used_elem *virtq_take(virtq_t *q) {
    struct vring_used_elem *e = &q->used->ring[q->last_used % q->qsize];
    q->last_used = (uint16_t)(q->last_used + 1);
    return e;
}

#endif /* VIRTIO_H */
