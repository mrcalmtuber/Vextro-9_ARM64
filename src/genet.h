#ifndef GENET_H
#define GENET_H

#include <stdint.h>
#include "mbox.h"

/*
 * Broadcom GENET v5 — the Raspberry Pi 4's gigabit Ethernet.
 *
 * Every earlier Pi hangs its network off the USB controller, which means
 * a USB stack before a single packet moves. The Pi 4 put a real MAC on
 * the SoC bus instead, and that is why this driver exists and why it
 * only claims the Pi 4: on a Pi 3 there is nothing here to talk to.
 *
 * The shape is a descriptor ring like the e1000's, with two differences
 * that matter more than they look:
 *
 *   The descriptors are in the controller's own SRAM, not in host
 *   memory. There is no ring base register to point somewhere useful —
 *   descriptor N is at a fixed offset inside the MAC's register window,
 *   and it is written with ordinary MMIO stores. That makes the ring
 *   trivially coherent and removes the entire class of bug where the
 *   device reads a descriptor the CPU has not flushed.
 *
 *   The buffers are in host memory and the DMA engine reads them with
 *   the caches switched off. Those do need flushing, and the Pi's
 *   coherency arrangements make it worth being explicit rather than
 *   trusting that a `dsb` is enough.
 *
 * There are sixteen transmit and seventeen receive queues; this uses one
 * of each, the default queue, which is the one that exists without any
 * priority configuration. Polled, no interrupts, matching everything
 * else in this kernel.
 *
 * Untested on hardware. Written from the register layout the Linux
 * bcmgenet driver and the U-Boot port both encode; the parts that are
 * inference rather than specification are marked where they arise.
 */

#define GENET_SYS_OFF          0x0000
#define GENET_SYS_REV_CTRL     (GENET_SYS_OFF + 0x00)
#define GENET_SYS_PORT_CTRL    (GENET_SYS_OFF + 0x04)
#define GENET_SYS_RBUF_FLUSH   (GENET_SYS_OFF + 0x08)
#define GENET_SYS_TBUF_FLUSH   (GENET_SYS_OFF + 0x0C)

#define GENET_EXT_OFF          0x0080
#define GENET_EXT_RGMII_OOB    (GENET_EXT_OFF + 0x0C)

#define GENET_RBUF_OFF         0x0300
#define GENET_RBUF_CTRL        (GENET_RBUF_OFF + 0x00)
#define GENET_RBUF_TBUF_SIZE   (GENET_RBUF_OFF + 0xB4)

#define GENET_UMAC_OFF         0x0800
#define GENET_UMAC_CMD         (GENET_UMAC_OFF + 0x008)
#define GENET_UMAC_MAC0        (GENET_UMAC_OFF + 0x00C)
#define GENET_UMAC_MAC1        (GENET_UMAC_OFF + 0x010)
#define GENET_UMAC_MAX_FRAME   (GENET_UMAC_OFF + 0x014)
#define GENET_UMAC_MDIO_CMD    (GENET_UMAC_OFF + 0x614)
#define GENET_UMAC_MIB_CTRL    (GENET_UMAC_OFF + 0x580)

#define CMD_TX_EN              (1u << 0)
#define CMD_RX_EN              (1u << 1)
#define CMD_SPEED_SHIFT        2
#define CMD_SPEED_1000         2
#define CMD_SW_RESET           (1u << 13)
#define CMD_LCL_LOOP_EN        (1u << 15)
#define CMD_TX_PAUSE_IGN       (1u << 28)
#define CMD_RX_PAUSE_IGN       (1u << 29)

#define MIB_RESET_RX           (1u << 0)
#define MIB_RESET_RUNT         (1u << 1)
#define MIB_RESET_TX           (1u << 2)

/* Descriptor rings, in the controller's SRAM. */
#define GENET_RX_DESC_BASE     0x2000
#define GENET_TX_DESC_BASE     0x4000
#define GENET_DESC_STRIDE      0x0C

#define GENET_RX_DMA_OFF       0x2000
#define GENET_TX_DMA_OFF       0x4000
#define GENET_DMA_RING_OFF     0x0C00     /* per-ring config, after descs */
#define GENET_DMA_REGS_OFF     0x1040     /* global DMA control           */

#define DMA_RING_CFG           0x00
#define DMA_CTRL               0x04
#define DMA_STATUS             0x08
#define DMA_SCB_BURST_SIZE     0x0C

/* Per-ring registers, 0x40 apart, ring 16 is the default queue. */
#define GENET_DEFAULT_Q        16
#define DMA_RING_STRIDE        0x40
#define RDMA_WRITE_PTR         0x00
#define TDMA_READ_PTR          0x00
#define DMA_PROD_INDEX         0x08
#define DMA_CONS_INDEX         0x0C
#define DMA_RING_BUF_SIZE      0x10
#define DMA_START_ADDR         0x14
#define DMA_END_ADDR           0x1C
#define DMA_MBUF_DONE_THRESH   0x24
#define DMA_XON_XOFF_THRESH    0x28
#define RDMA_READ_PTR          0x2C
#define TDMA_WRITE_PTR         0x2C

#define DMA_EN                 (1u << 0)
#define DMA_RING_BUF_EN_SHIFT  1

/* Descriptor words. */
#define DESC_LENGTH_STATUS     0x00
#define DESC_ADDRESS_LO        0x04
#define DESC_ADDRESS_HI        0x08

#define DESC_LENGTH_SHIFT      16
#define DESC_OWN               (1u << 15)
#define DESC_EOP               (1u << 14)
#define DESC_SOP               (1u << 13)
#define DESC_RX_ERRORS         0x3Fu       /* CRC, overrun, length, ...   */

#define GENET_RX_DESCS         64
#define GENET_TX_DESCS         32
#define GENET_BUF_SIZE         2048
#define GENET_FRAME_MAX        1514

static uint64_t genet_base = 0;
static int      genet_found = 0;
static uint8_t  genet_mac[6];

/* Buffers are DMA-visible; the descriptors are not (they live in the
 * controller's SRAM), which is why only these are page-aligned. */
static uint8_t genet_rx_buf[GENET_RX_DESCS][GENET_BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t genet_tx_buf[GENET_TX_DESCS][GENET_BUF_SIZE] __attribute__((aligned(4096)));

static uint32_t genet_rx_index = 0;      /* next descriptor to inspect */
static uint32_t genet_rx_cons  = 0;      /* consumer index, mod 2^16   */
static uint32_t genet_tx_index = 0;
static uint32_t genet_tx_prod  = 0;

static void genet_log(const char *s) {
    serial_puts("[genet] ");
    serial_puts(s);
    serial_putc('\n');
}

static inline uint32_t genet_rd(uint32_t off) {
    DSB();
    return *(volatile uint32_t *)(uintptr_t)(genet_base + off);
}
static inline void genet_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(genet_base + off) = v;
    DSB();
}

static inline uint32_t genet_rx_ring_reg(uint32_t reg) {
    return GENET_RX_DMA_OFF + GENET_DMA_RING_OFF +
           GENET_DEFAULT_Q * DMA_RING_STRIDE + reg;
}
static inline uint32_t genet_tx_ring_reg(uint32_t reg) {
    return GENET_TX_DMA_OFF + GENET_DMA_RING_OFF +
           GENET_DEFAULT_Q * DMA_RING_STRIDE + reg;
}

static void genet_delay_us(uint64_t us) {
    uint64_t hz = timer_hz();
    uint64_t target = timer_count() + (hz / 1000000ULL) * us + 1;
    while (timer_count() < target) { }
}

/*
 * Push a buffer out to memory the DMA engine can see, or pull one back.
 *
 * The x86 tree needs none of this: its DMA is cache-coherent by
 * architecture, which is why e1000.h has no cache maintenance in it at
 * all. On this platform coherency is a property of how a particular
 * master is wired, and the safe assumption for a peripheral on the SoC
 * bus is that it is not. `dc cvac` cleans a line out to the point of
 * coherency before the device reads it; `dc ivac` throws away a stale
 * line before the CPU reads what the device wrote.
 */
static void genet_clean(const void *p, uint32_t len) {
    uintptr_t a = (uintptr_t)p & ~63UL;
    uintptr_t e = (uintptr_t)p + len;
    for (; a < e; a += 64) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    DSB();
}

static void genet_invalidate(const void *p, uint32_t len) {
    uintptr_t a = (uintptr_t)p & ~63UL;
    uintptr_t e = (uintptr_t)p + len;
    for (; a < e; a += 64) __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    DSB();
}

/* ---- transmit ---- */

static void genet_transmit(const void *frame, uint16_t len) {
    if (!genet_found || len == 0) return;
    if (len > GENET_FRAME_MAX) len = GENET_FRAME_MAX;

    /*
     * A short frame is padded to the 60-byte minimum here rather than
     * left to the MAC. The controller can be configured to pad, but the
     * padding bytes are then whatever the buffer held — which leaks
     * whatever the previous frame put there onto the wire.
     */
    uint32_t n = len;
    if (n < 60) n = 60;

    uint32_t slot = genet_tx_index % GENET_TX_DESCS;
    uint8_t *buf = genet_tx_buf[slot];
    for (uint32_t i = 0; i < n; i++)
        buf[i] = i < len ? ((const uint8_t *)frame)[i] : 0;
    genet_clean(buf, n);

    uint64_t pa = virt_to_phys(buf);
    if (!pa) { genet_log("transmit buffer has no physical address"); return; }

    uint32_t desc = GENET_TX_DESC_BASE + slot * GENET_DESC_STRIDE;
    genet_wr(desc + DESC_ADDRESS_LO, (uint32_t)pa);
    genet_wr(desc + DESC_ADDRESS_HI, (uint32_t)(pa >> 32));
    genet_wr(desc + DESC_LENGTH_STATUS,
             (n << DESC_LENGTH_SHIFT) | DESC_SOP | DESC_EOP | (1u << 12));

    genet_tx_index++;
    genet_tx_prod = (genet_tx_prod + 1) & 0xFFFF;
    genet_wr(genet_tx_ring_reg(DMA_PROD_INDEX), genet_tx_prod);
}

/* ---- receive ---- */

/*
 * Hand back one frame if the controller has left one.
 *
 * The producer index is a free-running 16-bit counter, so the number of
 * frames waiting is the difference from our consumer index — modular
 * arithmetic, not a comparison, or the ring appears empty forever after
 * the first wrap.
 */
static int genet_rx_poll(uint8_t **out, uint16_t *out_len) {
    if (!genet_found) return 0;

    uint32_t prod = genet_rd(genet_rx_ring_reg(DMA_PROD_INDEX)) & 0xFFFF;
    if (((prod - genet_rx_cons) & 0xFFFF) == 0) return 0;

    uint32_t slot = genet_rx_index % GENET_RX_DESCS;
    uint32_t desc = GENET_RX_DESC_BASE + slot * GENET_DESC_STRIDE;
    uint32_t status = genet_rd(desc + DESC_LENGTH_STATUS);
    uint32_t len = (status >> DESC_LENGTH_SHIFT) & 0xFFF;

    genet_rx_index++;
    genet_rx_cons = (genet_rx_cons + 1) & 0xFFFF;
    genet_wr(genet_rx_ring_reg(DMA_CONS_INDEX), genet_rx_cons);

    if ((status & DESC_RX_ERRORS) || len == 0 || len > GENET_BUF_SIZE) return 0;

    /*
     * The frame arrives behind a 64-byte receive status block that the
     * controller prepends and that is not part of the packet. Returning
     * a pointer into the middle of the buffer rather than copying is
     * deliberate — the network stack above reads the frame and is done
     * with it before the ring can wrap.
     */
    genet_invalidate(genet_rx_buf[slot], GENET_BUF_SIZE);
    if (len <= 64) return 0;

    *out = genet_rx_buf[slot] + 64;
    *out_len = (uint16_t)(len - 64);
    return 1;
}

/* ---- bring-up ---- */

static void genet_umac_reset(void) {
    /*
     * The order here is the part that is easy to get wrong: the system
     * block's reset must be asserted and released around the MAC's own,
     * and the receive buffer needs flushing on both sides of it. A MAC
     * reset alone leaves the DMA engine holding descriptors from before
     * the reset.
     */
    genet_wr(GENET_SYS_RBUF_FLUSH, 1);
    genet_delay_us(10);
    genet_wr(GENET_SYS_RBUF_FLUSH, 0);
    genet_delay_us(10);

    genet_wr(GENET_UMAC_CMD, 0);
    genet_wr(GENET_UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP_EN);
    genet_delay_us(2);
    genet_wr(GENET_UMAC_CMD, 0);

    /* Statistics counters reset, then left running. */
    genet_wr(GENET_UMAC_MIB_CTRL, MIB_RESET_RX | MIB_RESET_RUNT | MIB_RESET_TX);
    genet_wr(GENET_UMAC_MIB_CTRL, 0);

    genet_wr(GENET_UMAC_MAX_FRAME, GENET_BUF_SIZE);

    uint32_t rbuf = genet_rd(GENET_RBUF_CTRL);
    rbuf |= (1u << 1);                 /* 64-byte status block on receive */
    genet_wr(GENET_RBUF_CTRL, rbuf);
}

static void genet_set_mac(const uint8_t mac[6]) {
    genet_wr(GENET_UMAC_MAC0, ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) |
                              ((uint32_t)mac[2] << 8)  | (uint32_t)mac[3]);
    genet_wr(GENET_UMAC_MAC1, ((uint32_t)mac[4] << 8) | (uint32_t)mac[5]);
}

static int genet_init_rings(void) {
    /* Stop both engines before touching a descriptor. */
    genet_wr(GENET_RX_DMA_OFF + GENET_DMA_REGS_OFF + DMA_CTRL, 0);
    genet_wr(GENET_TX_DMA_OFF + GENET_DMA_REGS_OFF + DMA_CTRL, 0);

    for (uint32_t i = 0; i < GENET_RX_DESCS; i++) {
        uint64_t pa = virt_to_phys(genet_rx_buf[i]);
        if (!pa) return 0;
        uint32_t d = GENET_RX_DESC_BASE + i * GENET_DESC_STRIDE;
        genet_wr(d + DESC_ADDRESS_LO, (uint32_t)pa);
        genet_wr(d + DESC_ADDRESS_HI, (uint32_t)(pa >> 32));
        genet_wr(d + DESC_LENGTH_STATUS, 0);
        genet_invalidate(genet_rx_buf[i], GENET_BUF_SIZE);
    }
    for (uint32_t i = 0; i < GENET_TX_DESCS; i++) {
        uint32_t d = GENET_TX_DESC_BASE + i * GENET_DESC_STRIDE;
        genet_wr(d + DESC_ADDRESS_LO, 0);
        genet_wr(d + DESC_ADDRESS_HI, 0);
        genet_wr(d + DESC_LENGTH_STATUS, 0);
    }

    /*
     * Ring extents are in *words*, not descriptors and not bytes: each
     * descriptor is three 32-bit words, so a 64-entry ring ends at word
     * 192. The end address is inclusive of the last word, hence the -1.
     */
    genet_wr(genet_rx_ring_reg(DMA_START_ADDR), 0);
    genet_wr(genet_rx_ring_reg(RDMA_READ_PTR), 0);
    genet_wr(genet_rx_ring_reg(RDMA_WRITE_PTR), 0);
    genet_wr(genet_rx_ring_reg(DMA_END_ADDR), GENET_RX_DESCS * 3 - 1);
    genet_wr(genet_rx_ring_reg(DMA_PROD_INDEX), 0);
    genet_wr(genet_rx_ring_reg(DMA_CONS_INDEX), 0);
    genet_wr(genet_rx_ring_reg(DMA_RING_BUF_SIZE),
             (GENET_RX_DESCS << 16) | GENET_BUF_SIZE);
    genet_wr(genet_rx_ring_reg(DMA_XON_XOFF_THRESH),
             ((GENET_RX_DESCS >> 4) << 16) | (GENET_RX_DESCS >> 4));

    genet_wr(genet_tx_ring_reg(DMA_START_ADDR), 0);
    genet_wr(genet_tx_ring_reg(TDMA_READ_PTR), 0);
    genet_wr(genet_tx_ring_reg(TDMA_WRITE_PTR), 0);
    genet_wr(genet_tx_ring_reg(DMA_END_ADDR), GENET_TX_DESCS * 3 - 1);
    genet_wr(genet_tx_ring_reg(DMA_PROD_INDEX), 0);
    genet_wr(genet_tx_ring_reg(DMA_CONS_INDEX), 0);
    genet_wr(genet_tx_ring_reg(DMA_RING_BUF_SIZE),
             (GENET_TX_DESCS << 16) | GENET_BUF_SIZE);
    genet_wr(genet_tx_ring_reg(DMA_MBUF_DONE_THRESH), 1);
    genet_wr(genet_tx_ring_reg(DMA_XON_XOFF_THRESH), 0);

    /* Enable only the default ring, then the engines. */
    genet_wr(GENET_RX_DMA_OFF + GENET_DMA_REGS_OFF + DMA_RING_CFG,
             1u << GENET_DEFAULT_Q);
    genet_wr(GENET_TX_DMA_OFF + GENET_DMA_REGS_OFF + DMA_RING_CFG,
             1u << GENET_DEFAULT_Q);
    genet_wr(GENET_RX_DMA_OFF + GENET_DMA_REGS_OFF + DMA_CTRL,
             DMA_EN | (1u << (GENET_DEFAULT_Q + DMA_RING_BUF_EN_SHIFT)));
    genet_wr(GENET_TX_DMA_OFF + GENET_DMA_REGS_OFF + DMA_CTRL,
             DMA_EN | (1u << (GENET_DEFAULT_Q + DMA_RING_BUF_EN_SHIFT)));

    /*
     * Both indices start at zero and stay in step with the hardware's.
     *
     * Unlike the virtio rings elsewhere in this kernel, there is nothing
     * to "post": every receive descriptor already points at a buffer
     * from the loop above, and the engine fills them in order. The
     * driver only ever writes the consumer index — the producer index is
     * the controller's, and reading it is how a frame is noticed.
     */
    genet_rx_index = 0;
    genet_rx_cons  = 0;
    genet_tx_index = 0;
    genet_tx_prod  = 0;
    return 1;
}

static int genet_init(void) {
    genet_found = 0;
    if (board_kind != BOARD_PI4 || !bcm_genet_base) return 0;
    genet_base = bcm_genet_base;

    /* The revision register is the first proof that the window is
     * mapped and the block is powered: it reads back as the GENET
     * version, and anything else means this is not a GENET. */
    uint32_t rev = genet_rd(GENET_SYS_REV_CTRL);
    uint32_t major = (rev >> 24) & 0x0F;
    if (major != 6 && major != 5) {
        serial_puts("[genet] unexpected revision ");
        serial_put_hex32(rev);
        serial_puts(" - not driving it\n");
        return 0;
    }

    genet_umac_reset();

    /*
     * The MAC address comes from the firmware, which reads it from the
     * board's OTP. A Pi's address is assigned to that board and printed
     * on it; inventing one instead would work on a home network and fail
     * anywhere that expects a machine to keep its identity.
     */
    if (!mbox_mac(genet_mac)) {
        genet_log("firmware would not give a MAC address");
        return 0;
    }
    genet_set_mac(genet_mac);

    if (!genet_init_rings()) {
        genet_log("could not set up the descriptor rings");
        return 0;
    }

    /*
     * Bring the MAC up at a gigabit.
     *
     * The speed is asserted rather than negotiated: this driver has no
     * MDIO code, so it never talks to the PHY and never learns what the
     * link settled on. On a Pi 4 the PHY auto-negotiates on its own and
     * the RGMII link between it and the MAC is fixed, so this is right
     * in the ordinary case — but a 100 Mbit switch would leave the two
     * ends disagreeing, and the symptom would be a link that passes no
     * traffic rather than a slow one. MDIO is the next thing this file
     * needs, and it needs hardware to write against.
     */
    uint32_t cmd = genet_rd(GENET_UMAC_CMD);
    cmd &= ~(3u << CMD_SPEED_SHIFT);
    cmd |= (CMD_SPEED_1000 << CMD_SPEED_SHIFT);
    cmd |= CMD_TX_EN | CMD_RX_EN | CMD_TX_PAUSE_IGN | CMD_RX_PAUSE_IGN;
    genet_wr(GENET_UMAC_CMD, cmd);

    genet_found = 1;
    serial_puts("[genet] up, MAC ");
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        if (i) serial_putc(':');
        serial_putc(hex[(genet_mac[i] >> 4) & 0xF]);
        serial_putc(hex[genet_mac[i] & 0xF]);
    }
    serial_putc('\n');
    return 1;
}

#endif /* GENET_H */
