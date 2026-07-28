#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include "virtio.h"

/*
 * The network adapter, behind the name the stack already calls.
 *
 * There is no e1000 here — the card is virtio-net. As with ata.h, the
 * file keeps its name and its entry points because netstack.h is 1,200
 * lines of portable IPv4/ICMP/UDP/DNS/TCP/HTTP that wants to hand over an
 * Ethernet frame and be told when one arrives, and has no opinion about
 * what carries it. The x86 original is kept as e1000_x86.h.ref.
 *
 * The e1000 driver it replaces is MMIO-based and would have *compiled*
 * unchanged, which made porting it tempting. It would also have been
 * wrong in a way that is hard to see: its descriptor-then-doorbell
 * sequences carry no memory barriers at all, because x86 orders stores
 * against each other and the code was written where that is free. On
 * aarch64 nothing orders them, so the card can be told to transmit a
 * descriptor it has not yet observed — a bug that appears as occasional
 * dropped or corrupt frames under load and never reproduces on demand.
 * Rather than retrofit barriers into a driver for a card this machine
 * does not have, the traffic goes through the virtqueue layer milestones
 * 2 and 3 already use, where the barriers are written once and shared.
 *
 * Header layout: with VIRTIO_F_VERSION_1 negotiated the per-packet header
 * is always the 12-byte virtio_net_hdr_v1, including num_buffers, whether
 * or not mergeable receive buffers were negotiated. Getting that length
 * wrong shifts every frame by two bytes, which does not fail loudly — it
 * produces frames whose EtherType is garbage and which the stack above
 * silently discards.
 */

#define VIRTIO_ID_NET 1

/* MAC is the only feature bit that changes what this driver must do:
 * without it the device has no address to report and one would have to be
 * invented. */
#define VIRTIO_NET_F_MAC_BIT 5

#define VNET_HDR_LEN   12
#define VNET_FRAME_MAX 1514
#define VNET_BUF       (VNET_HDR_LEN + VNET_FRAME_MAX + 2)

#define VNET_RX_BUFS 32
#define VNET_TX_BUFS 8

/* The surface netstack.h compiles against. */
static uint8_t e1000_mac[6];
static int     e1000_found = 0;

static uint64_t vnet_base = 0;
static virtq_t  vnet_rx, vnet_tx;

static struct vring_desc  vnet_rx_desc[VQ_SIZE] __attribute__((aligned(16)));
static struct vring_avail vnet_rx_avail         __attribute__((aligned(16)));
static struct vring_used  vnet_rx_used          __attribute__((aligned(16)));
static struct vring_desc  vnet_tx_desc[VQ_SIZE] __attribute__((aligned(16)));
static struct vring_avail vnet_tx_avail         __attribute__((aligned(16)));
static struct vring_used  vnet_tx_used          __attribute__((aligned(16)));

static uint8_t vnet_rx_buf[VNET_RX_BUFS][VNET_BUF] __attribute__((aligned(64)));
static uint8_t vnet_tx_buf[VNET_TX_BUFS][VNET_BUF] __attribute__((aligned(64)));

static uint32_t vnet_tx_next = 0;

/* Counters, not decoration. "No reply" has three very different causes —
 * nothing was sent, nothing came back, or something came back and the
 * stack discarded it — and they are indistinguishable from the outside. */
static uint32_t vnet_tx_count = 0;
static uint32_t vnet_rx_count = 0;

/*
 * The receive buffer handed out by the last poll, still being read.
 *
 * netstack.h takes a pointer and a length and parses the frame in place
 * before asking for the next one, so a buffer cannot go back to the
 * device the moment it is dequeued — the stack is still looking at it.
 * It is re-posted at the start of the following call instead. Returning
 * it early is the kind of bug that only shows under load, when the device
 * happens to refill the buffer mid-parse.
 */
static int vnet_pending = -1;

static void vnet_post_rx(uint32_t slot) {
    uint64_t p = virt_to_phys(vnet_rx_buf[slot]);
    if (!p) return;
    virtq_offer(vnet_base, 0, &vnet_rx, (uint16_t)slot, p, VNET_BUF, 1);
}

static void e1000_read_mac(void) {
    if (!e1000_found) return;
    volatile uint8_t *cfg = (volatile uint8_t *)mmio32(vnet_base + VIO_CONFIG);
    for (int i = 0; i < 6; i++) e1000_mac[i] = cfg[i];
}

/*
 * Send one frame.
 *
 * Copied into a driver-owned buffer rather than described in place: the
 * caller's frame lives in a scratch global it reuses immediately, and the
 * device reads asynchronously. Buffers rotate so a slow device does not
 * stall the next send, and completions are reaped rather than waited on —
 * nothing above this blocks on a transmit.
 */
static int e1000_transmit(const uint8_t *data, uint16_t len) {
    if (!e1000_found) return -1;
    if (len > VNET_FRAME_MAX) return -1;

    while (virtq_has_used(&vnet_tx)) virtq_take(&vnet_tx);

    uint32_t slot = vnet_tx_next;
    vnet_tx_next = (vnet_tx_next + 1) % VNET_TX_BUFS;

    uint8_t *b = vnet_tx_buf[slot];
    for (int i = 0; i < VNET_HDR_LEN; i++) b[i] = 0;
    for (uint16_t i = 0; i < len; i++) b[VNET_HDR_LEN + i] = data[i];

    uint64_t p = virt_to_phys(b);
    if (!p) return -1;

    virtq_offer(vnet_base, 1, &vnet_tx, (uint16_t)slot, p,
                VNET_HDR_LEN + len, 0);
    vnet_tx_count++;
    return 0;
}

static int e1000_rx_poll(uint8_t **out_buf, uint16_t *out_len) {
    if (!e1000_found) return 0;

    if (vnet_pending >= 0) {            /* the stack has finished with it */
        vnet_post_rx((uint32_t)vnet_pending);
        vnet_pending = -1;
    }

    if (!virtq_has_used(&vnet_rx)) return 0;

    struct vring_used_elem *e = virtq_take(&vnet_rx);
    uint32_t slot = e->id;
    if (slot >= VNET_RX_BUFS) return 0;

    uint32_t total = e->len;
    if (total <= VNET_HDR_LEN) {        /* header only: nothing to parse */
        vnet_post_rx(slot);
        return 0;
    }

    uint32_t flen = total - VNET_HDR_LEN;
    if (flen > VNET_FRAME_MAX) flen = VNET_FRAME_MAX;

    DMB();                              /* the device wrote it; see all of it */
    vnet_rx_count++;
    *out_buf = vnet_rx_buf[slot] + VNET_HDR_LEN;
    *out_len = (uint16_t)flen;
    vnet_pending = (int)slot;

    uint32_t st = vio_rd(vnet_base, VIO_INT_STATUS);
    if (st) vio_wr(vnet_base, VIO_INT_ACK, st);
    return 1;
}

/*
 * The hhdm argument is vestigial — it is what the x86 driver needed to
 * turn Limine's physical descriptor addresses into something it could
 * dereference. Here every address handed to the device comes from asking
 * the MMU, so nothing is offset by hand. The parameter stays so kernel.c
 * reads the same on both trees.
 */
static void e1000_init(uint64_t hhdm_offset) {
    (void)hhdm_offset;
    e1000_found = 0;

    vnet_base = virtio_find(VIRTIO_ID_NET, 0);
    if (!vnet_base) {
        serial_puts("[socrates/arm64] virtio-net: no device\n");
        return;
    }

    /* Reset, then negotiate MAC alongside VERSION_1. virtio_begin() is
     * not reused here because it offers VERSION_1 only, and the address
     * is worth having. */
    vio_wr(vnet_base, VIO_STATUS, 0);
    DSB();
    vio_wr(vnet_base, VIO_STATUS, VIO_STATUS_ACK);
    vio_wr(vnet_base, VIO_STATUS, VIO_STATUS_ACK | VIO_STATUS_DRIVER);

    vio_wr(vnet_base, VIO_DRV_FEAT_SEL, 1);
    vio_wr(vnet_base, VIO_DRV_FEAT, 1u << (VIRTIO_F_VERSION_1_BIT - 32));
    vio_wr(vnet_base, VIO_DRV_FEAT_SEL, 0);
    vio_wr(vnet_base, VIO_DRV_FEAT, 1u << VIRTIO_NET_F_MAC_BIT);

    vio_wr(vnet_base, VIO_STATUS,
           VIO_STATUS_ACK | VIO_STATUS_DRIVER | VIO_STATUS_FEATURES_OK);
    DSB();
    if (!(vio_rd(vnet_base, VIO_STATUS) & VIO_STATUS_FEATURES_OK)) {
        vio_wr(vnet_base, VIO_STATUS, VIO_STATUS_FAILED);
        serial_puts("[socrates/arm64] virtio-net: features refused\n");
        return;
    }

    if (!virtq_setup(vnet_base, 0, &vnet_rx,
                     vnet_rx_desc, &vnet_rx_avail, &vnet_rx_used) ||
        !virtq_setup(vnet_base, 1, &vnet_tx,
                     vnet_tx_desc, &vnet_tx_avail, &vnet_tx_used)) {
        serial_puts("[socrates/arm64] virtio-net: queues would not start\n");
        return;
    }

    virtio_ready(vnet_base);
    e1000_found = 1;

    /* Receive buffers must be posted before the first packet arrives, or
     * it is dropped with nowhere to land. */
    for (uint32_t i = 0; i < VNET_RX_BUFS && i < vnet_rx.qsize; i++)
        vnet_post_rx(i);

    e1000_read_mac();

    serial_puts("[socrates/arm64] virtio-net: up, MAC ");
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        if (i) serial_putc(':');
        serial_putc(hx[(e1000_mac[i] >> 4) & 0xF]);
        serial_putc(hx[e1000_mac[i] & 0xF]);
    }
    serial_puts("\n");
}

#endif /* E1000_H */
