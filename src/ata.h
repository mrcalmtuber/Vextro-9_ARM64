#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include "virtio.h"

/*
 * The block device, behind the name the filesystems already call.
 *
 * There is no ATA here at all — the disk is virtio-blk. The file keeps
 * its name and its five entry points because exfat.h and fat32.h are
 * 3,000 lines of portable code that ask for sectors by LBA and do not
 * care what answers, and changing them to say something else would be
 * churn with no reader benefit. The x86 original is kept alongside as
 * ata_x86.h.ref.
 *
 * What that original had to do, and this does not: probe a bus, negotiate
 * LBA28 versus LBA48, drive a six-register command sequence per transfer,
 * and spin on a status byte between every 512-byte burst through a 16-bit
 * port. Here a request is a header, a payload and a status byte handed to
 * a queue in one go, and the transfer size is whatever was asked for.
 *
 * Polled to completion, like everything else in this system. The render
 * loop is single-threaded and a disk read has nobody to yield to, so
 * spinning on the used ring is not a compromise — it is the same thing an
 * interrupt would do, minus the GIC.
 */

#define VIRTIO_ID_BLOCK 2

#define VIRTIO_BLK_T_IN    0        /* device -> memory */
#define VIRTIO_BLK_T_OUT   1        /* memory -> device */
#define VIRTIO_BLK_T_FLUSH 4

#define VIRTIO_BLK_S_OK    0

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

/* The interface the filesystems compile against. */
static int      ata_present = 0;
static uint64_t ata_sectors = 0;

static uint64_t vblk_base = 0;
static virtq_t  vblk_q;

static struct vring_desc  vblk_desc[VQ_SIZE]  __attribute__((aligned(16)));
static struct vring_avail vblk_avail          __attribute__((aligned(16)));
static struct vring_used  vblk_used           __attribute__((aligned(16)));

static struct virtio_blk_req_hdr vblk_hdr __attribute__((aligned(16)));
static volatile uint8_t          vblk_status __attribute__((aligned(16)));

/*
 * A bounce buffer, and a deliberate one.
 *
 * The device is given physical addresses, so any buffer handed to it must
 * be physically contiguous for its whole length. Kernel buffers are
 * virtually contiguous and usually physically contiguous too — Limine
 * loads each segment as one block — but "usually" is the wrong standard
 * for a disk: a request that straddles a discontinuity would read the
 * right number of sectors into the wrong memory, silently, and show up
 * much later as a corrupt file rather than an I/O error.
 *
 * 64 KB covers the largest read anything here issues (exfat's 32-sector
 * read-ahead is 16 KB) in one trip, and larger transfers are simply
 * chunked. The copy costs far less than the transfer it protects.
 */
#define VBLK_BOUNCE_SECTORS 128
static uint8_t vblk_bounce[VBLK_BOUNCE_SECTORS * 512] __attribute__((aligned(4096)));

static void vblk_memcpy(void *d, const void *s, uint32_t n) {
    uint8_t *p = d; const uint8_t *q = s;
    while (n--) *p++ = *q++;
}

/*
 * One request, start to finish.
 *
 * The three descriptors are the shape every virtio-blk request takes: a
 * header the device reads, a payload it reads or writes depending on the
 * direction, and a status byte it always writes. Head index 0 every time
 * is safe only because this waits for completion before returning — with
 * a single request in flight the ring never has anything to collide with.
 */
static int vblk_request(uint32_t type, uint64_t sector,
                        void *data, uint32_t bytes, int device_writes) {
    if (!ata_present) return -1;

    vblk_hdr.type     = type;
    vblk_hdr.reserved = 0;
    vblk_hdr.sector   = sector;
    vblk_status       = 0xFF;

    uint64_t p_hdr = virt_to_phys(&vblk_hdr);
    uint64_t p_st  = virt_to_phys((void *)&vblk_status);
    uint64_t p_dat = bytes ? virt_to_phys(data) : 0;
    if (!p_hdr || !p_st || (bytes && !p_dat)) return -1;

    vq_buf_t bufs[3];
    int n = 0;
    bufs[n].phys = p_hdr; bufs[n].len = sizeof(vblk_hdr);
    bufs[n].device_writes = 0; n++;
    if (bytes) {
        bufs[n].phys = p_dat; bufs[n].len = bytes;
        bufs[n].device_writes = device_writes; n++;
    }
    bufs[n].phys = p_st; bufs[n].len = 1;
    bufs[n].device_writes = 1; n++;

    virtq_offer_chain(vblk_base, 0, &vblk_q, 0, bufs, n);

    /* Bounded so a device that never answers costs a stalled frame rather
     * than a hung machine with nothing on screen to explain it. */
    uint64_t deadline = timer_count() + timer_hz() * 5;
    while (!virtq_has_used(&vblk_q)) {
        if (timer_count() > deadline) return -1;
    }
    virtq_take(&vblk_q);

    uint32_t st = vio_rd(vblk_base, VIO_INT_STATUS);
    if (st) vio_wr(vblk_base, VIO_INT_ACK, st);

    DMB();
    return vblk_status == VIRTIO_BLK_S_OK ? 0 : -1;
}

static int ata_read(uint64_t lba, uint32_t count, void *buf) {
    uint8_t *out = buf;
    while (count) {
        uint32_t n = count > VBLK_BOUNCE_SECTORS ? VBLK_BOUNCE_SECTORS : count;
        if (vblk_request(VIRTIO_BLK_T_IN, lba, vblk_bounce, n * 512, 1) != 0)
            return -1;
        vblk_memcpy(out, vblk_bounce, n * 512);
        out   += n * 512;
        lba   += n;
        count -= n;
    }
    return 0;
}

static int ata_write(uint64_t lba, uint32_t count, const void *buf) {
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = count > VBLK_BOUNCE_SECTORS ? VBLK_BOUNCE_SECTORS : count;
        vblk_memcpy(vblk_bounce, in, n * 512);
        if (vblk_request(VIRTIO_BLK_T_OUT, lba, vblk_bounce, n * 512, 0) != 0)
            return -1;
        in    += n * 512;
        lba   += n;
        count -= n;
    }
    return 0;
}

static int ata_flush(void) {
    if (!ata_present) return -1;
    return vblk_request(VIRTIO_BLK_T_FLUSH, 0, 0, 0, 0);
}

/*
 * Bring up the virtio-blk device at `index`.
 *
 * There can be more than one: this port attaches the big shared volume
 * read-only and a small writable one beside it, because /etc/users.db and
 * the home directories have to be written somewhere and the shared disk
 * is also opened by the x86 tree. Each is brought up on its own queue and
 * the block layer switches between them.
 */
static void ata_init_at(uint32_t index) {
    ata_present = 0;
    ata_sectors = 0;

    vblk_base = virtio_find(VIRTIO_ID_BLOCK, index);
    if (!vblk_base) {
        if (index == 0)
            serial_puts("[vextro/arm64] virtio-blk: no device\n");
        return;
    }
    if (!virtio_begin(vblk_base)) {
        serial_puts("[vextro/arm64] virtio-blk: feature negotiation refused\n");
        return;
    }
    if (!virtq_setup(vblk_base, 0, &vblk_q, vblk_desc, &vblk_avail, &vblk_used)) {
        serial_puts("[vextro/arm64] virtio-blk: queue 0 would not start\n");
        return;
    }
    virtio_ready(vblk_base);

    /* Capacity is the first field of config space, in 512-byte sectors. */
    uint32_t lo = *mmio32(vblk_base + VIO_CONFIG + 0);
    uint32_t hi = *mmio32(vblk_base + VIO_CONFIG + 4);
    ata_sectors = ((uint64_t)hi << 32) | lo;
    ata_present = 1;

    serial_puts("[vextro/arm64] virtio-blk ");
    serial_put_u64(index);
    serial_puts(": ");
    serial_put_u64(ata_sectors);
    serial_puts(" sectors (");
    serial_put_u64(ata_sectors / 2048);
    serial_puts(" MiB)\n");
}

static void ata_init(void) { ata_init_at(0); }

#endif /* ATA_H */
