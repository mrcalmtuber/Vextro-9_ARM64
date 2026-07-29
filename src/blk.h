#ifndef BLK_H
#define BLK_H

#include <stdint.h>
#include "ata.h"     /* virtio-blk, on qemu virt   */
#include "emmc.h"    /* the SD card, on a Pi       */

/*
 * The block layer: one disk-shaped hole for two very different drivers.
 *
 * exfat.h and fat32.h used to call ata_read() by name — a name that was
 * already a fiction on this tree, since there is no ATA anywhere in it
 * and the function was talking to a virtio transport. That was true
 * enough while there was exactly one way to reach a disk, and it is the
 * reason adding a second one is a layer rather than an #ifdef: the
 * filesystems want sectors, not a bus.
 *
 * The two backends never coexist. A machine is either qemu's virt, where
 * storage is virtio and there is no SD slot, or a Raspberry Pi, where
 * the card *is* the disk and virtio does not exist. So the choice is
 * made once at boot from what was actually found, and the dispatch below
 * costs one predictable branch per sector.
 *
 * This mirrors the x86 tree's block layer deliberately, down to the
 * function names. The two trees are separate copies and will drift; the
 * least that can be done about that is to make the same idea look the
 * same in both, so a fix written against one is recognisable in the
 * other.
 */

#define BLK_NONE   0
#define BLK_VIRTIO 1
#define BLK_EMMC   2

static int      blk_kind = BLK_NONE;
static uint64_t blk_total_sectors = 0;

static const char *blk_bus_name(void) {
    switch (blk_kind) {
        case BLK_VIRTIO: return "virtio-blk";
        case BLK_EMMC:   return "SD card";
    }
    return "none";
}

static int blk_present(void) { return blk_kind != BLK_NONE; }
static uint64_t blk_sectors(void) { return blk_total_sectors; }

/*
 * Probe both, take whichever answers.
 *
 * virtio first because it is the cheaper probe — a magic number at a
 * known address — and because a machine that has it has nothing else.
 * The SD controller's probe involves powering the slot, resetting the
 * controller and a conversation with the card, none of which is worth
 * doing on a machine that already has a disk.
 */
static void blk_init(void) {
    blk_kind = BLK_NONE;
    blk_total_sectors = 0;

    ata_init();
    if (ata_present) {
        blk_kind = BLK_VIRTIO;
        blk_total_sectors = ata_sectors;
    } else if (emmc_init()) {
        blk_kind = BLK_EMMC;
        blk_total_sectors = emmc_sectors;
    }

    serial_puts("[blk] ");
    if (blk_kind == BLK_NONE) {
        serial_puts("no disk found\n");
        return;
    }
    serial_puts(blk_bus_name());
    serial_puts(", ");
    serial_put_u64(blk_total_sectors / 2048);
    serial_puts(" MB\n");
}

static int blk_read(uint64_t lba, uint32_t count, void *buf) {
    if (lba + count > blk_total_sectors) return -1;
    switch (blk_kind) {
        case BLK_VIRTIO: return ata_read(lba, count, buf);
        case BLK_EMMC:   return emmc_read(lba, count, buf);
    }
    return -1;
}

static int blk_write(uint64_t lba, uint32_t count, const void *buf) {
    if (lba + count > blk_total_sectors) return -1;
    switch (blk_kind) {
        case BLK_VIRTIO: return ata_write(lba, count, buf);
        case BLK_EMMC:   return emmc_write(lba, count, buf);
    }
    return -1;
}

static int blk_flush(void) {
    switch (blk_kind) {
        case BLK_VIRTIO: return ata_flush();
        case BLK_EMMC:   return emmc_flush();
    }
    return -1;
}

#endif /* BLK_H */
