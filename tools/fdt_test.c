/*
 * Run the kernel's device tree parser against real device tree blobs, on
 * the host.
 *
 * src/fdt.h is pure byte manipulation with no architecture in it at all,
 * which makes it the one part of the machine layer that can be tested
 * without the machine.  That matters more here than it sounds: the
 * addresses this file resolves are the addresses every driver is
 * programmed with, and getting one wrong on hardware nobody has plugged
 * in yet produces a board that boots to a black screen and says nothing.
 *
 * The expected values below are not this parser's output written down
 * after the fact.  They are what `dtc` prints for the same nodes, worked
 * through the `ranges` translation by hand — so a change that breaks the
 * arithmetic fails here rather than on a Pi.
 *
 *   cc -O2 -Wall -o build/fdt_test tools/fdt_test.c
 *   build/fdt_test tools/testdata/bcm2711-rpi-4-b.dtb virt.dtb
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fdt.h"

static int failures = 0;

static void check(const char *what, uint64_t got, uint64_t want) {
    int ok = (got == want);
    if (!ok) failures++;
    printf("  %-34s %-12s got 0x%llx", what, ok ? "ok" : "FAIL",
           (unsigned long long)got);
    if (!ok) printf(", wanted 0x%llx", (unsigned long long)want);
    printf("\n");
}

static void *slurp(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)n);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(1); }
    fclose(f);
    if (out_len) *out_len = n;
    return p;
}

/*
 * A Raspberry Pi 4, from the blob the firmware actually ships.
 *
 * Every address here crosses at least one `ranges` translation, which is
 * the whole point: the tree describes the peripherals at 0x7e000000
 * because that is what the VideoCore sees, and the ARM core reaches the
 * identical registers at 0xfe000000.  A parser that returns the number
 * written in the node name is wrong by 128 MB.
 */
static void test_pi4(const char *path) {
    printf("%s\n", path);
    void *blob = slurp(path, 0);
    if (!fdt_init(blob)) { printf("  FAIL: not a device tree\n"); failures++; return; }

    check("root is bcm2711", fdt_board_is("brcm,bcm2711"), 1);
    check("root is not bcm2837", fdt_board_is("brcm,bcm2837"), 0);
    /* "simple-bus" is claimed by soc, scb and emmc2bus — and by no root
     * node anywhere. A board check that searches the whole tree instead
     * of the root says yes to this, which is how the wrong drivers get
     * chosen on a machine that merely contains a bus. */
    check("a bus's compatible is not the board's",
          fdt_board_is("simple-bus"), 0);

    /* soc: ranges 0x7e000000 -> 0xfe000000, one address cell */
    check("pl011 uart", fdt_reg_base(0, "arm,pl011"), 0xfe201000ULL);
    check("gpio", fdt_reg_base(0, "brcm,bcm2711-gpio"), 0xfe200000ULL);
    check("mailbox", fdt_reg_base(0, "brcm,bcm2835-mbox"), 0xfe00b880ULL);

    /* soc: a second range, 0x40000000 -> 0xff800000, and two banks in
     * one reg property — the distributor and the CPU interface */
    check("gic-400 distributor", fdt_reg_index(0, "arm,gic-400", 0, 0), 0xff841000ULL);
    check("gic-400 cpu interface", fdt_reg_index(0, "arm,gic-400", 1, 0), 0xff842000ULL);

    /* emmc2bus: a different node with *two* address cells, so a parser
     * that assumes one reads the address as its own high half */
    uint64_t sz = 0;
    check("emmc2 (sd card)", fdt_reg_index(0, "brcm,bcm2711-emmc2", 0, &sz), 0xfe340000ULL);
    check("emmc2 size", sz, 0x100ULL);

    /* scb: two address cells and two size cells, 0x7c000000 -> 0xfc000000 */
    check("genet ethernet", fdt_reg_base(0, "brcm,bcm2711-genet-v5"), 0xfd580000ULL);

    /* Absent hardware must report absent rather than zero-by-accident */
    check("no virtio-mmio", fdt_reg_base(0, "virtio,mmio"), 0);

    /* The raw property lookup the drivers use, on a property that is not
     * `reg`: the SD controller's interrupt line, three cells of it. */
    uint32_t len = 0;
    const uint8_t *irq = fdt_find_prop_at(0, "brcm,bcm2711-emmc2",
                                         "interrupts", &len, 0);
    check("emmc2 interrupts present", irq != 0, 1);
    check("emmc2 interrupts length", len, 12);
    free(blob);
}

/*
 * qemu's virt, for the regression half: two address cells, no bus
 * translation anywhere, and a GIC whose v2m child node carries a `reg`
 * of its own directly underneath the distributor's.  Returning the
 * child's is the bug this parser was written to stop.
 */
static void test_virt(const char *path) {
    printf("%s\n", path);
    void *blob = slurp(path, 0);
    if (!fdt_init(blob)) { printf("  FAIL: not a device tree\n"); failures++; return; }

    check("root is not a pi", fdt_board_is("brcm,bcm2711"), 0);
    check("a device's compatible is not the board's",
          fdt_board_is("arm,pl011"), 0);
    /* what the root really claims, as the positive half of the same test */
    check("root is a qemu virt machine", fdt_board_is("linux,dummy-virt"), 1);
    check("pl011 uart", fdt_reg_base(0, "arm,pl011"), 0x09000000ULL);
    check("pl031 rtc", fdt_reg_base(0, "arm,pl031"), 0x09010000ULL);
    check("gic distributor", fdt_reg_index(0, "arm,cortex-a15-gic", 0, 0), 0x08000000ULL);
    check("gic cpu interface", fdt_reg_index(0, "arm,cortex-a15-gic", 1, 0), 0x08010000ULL);
    free(blob);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rpi4.dtb> [virt.dtb]\n", argv[0]);
        return 2;
    }
    test_pi4(argv[1]);
    if (argc > 2) test_virt(argv[2]);

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
