#ifndef ARM_H
#define ARM_H

#include <stdint.h>
#include "fdt.h"

/*
 * The aarch64 machine layer — everything the x86 tree kept in idt.h and
 * the top of pci.h.
 *
 * Three things replace a surprising amount of code:
 *
 *   - There are no I/O ports. Every device is memory-mapped, so the
 *     inb/outb pair that ran the PS/2 controller, the PIT, the CMOS and
 *     the PCI config space simply has no counterpart. Drivers read and
 *     write volatile pointers instead.
 *
 *   - Time comes from the CPU, not a chip. The architected generic timer
 *     is a monotonic counter in a system register with a frequency the
 *     hardware reports, so the render loop can pace itself without
 *     programming anything or taking an interrupt at all. The x86 tree
 *     needed the PIT and an ISR to do the same job.
 *
 *   - Exceptions are code, not descriptors. Instead of a 256-entry table
 *     of gates, there is one 2 KB-aligned table of sixteen 128-byte
 *     slots, each holding actual instructions.
 *
 * Addresses below are QEMU's `virt` machine. They come from the device
 * tree on real hardware, and a parser arrives with that milestone.
 */

/* ---- system register access ---- */

#define SYSREG_READ(reg)                                    \
    ({ uint64_t _v; __asm__ volatile("mrs %0, " #reg : "=r"(_v)); _v; })

#define SYSREG_WRITE(reg, val)                              \
    do { __asm__ volatile("msr " #reg ", %0" :: "r"((uint64_t)(val))); } while (0)

#define ISB() __asm__ volatile("isb" ::: "memory")
#define DSB() __asm__ volatile("dsb sy" ::: "memory")
#define DMB() __asm__ volatile("dmb sy" ::: "memory")

static inline void irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline void irq_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }

/* Debug, SError, IRQ and FIQ all at once. Used when taking the machine
 * over from firmware, where the interesting question is not which of the
 * four is armed but whether any of them can arrive before this kernel is
 * ready — and the answer should be no. */
static inline void daif_mask_all(void) { __asm__ volatile("msr daifset, #0xf" ::: "memory"); }

/* ---- reaching device registers at all ----
 *
 * The kernel never runs with the MMU off: Limine enables it and hands over
 * with its own tables installed. Those tables map the kernel, usable RAM
 * and the framebuffer — and nothing else. Device registers are not in the
 * higher-half direct map (it covers memory, and MMIO is not memory), and
 * whether they happen to be identity-mapped low depends on how Limine sized
 * its tables, which in turn depends on the physical address range the CPU
 * reports. On `-cpu host` they were reachable; on `-cpu cortex-a72` they
 * were not, and the same binary printed six lines under one and nothing at
 * all under the other.
 *
 * The failure is worth describing because it does not look like what it is.
 * Touching an unmapped device register is a data abort, and it happens on
 * the first UART write — before VBAR_EL1 has been set, because setting it
 * requires getting far enough to call exceptions_init(). So the CPU vectors
 * to address zero, executes whatever the firmware left there, and ends up
 * in a fault loop with a garbage stack pointer. From the outside: a machine
 * that is powered on, consuming CPU, and silent. Days went into that.
 *
 * So the kernel maps its own device memory rather than depending on
 * anything inherited. It is the earliest possible thing after entry, it
 * has to come before the first character of output, and it means every
 * address below is one this kernel put in a page table itself.
 */

/* Physical addresses are identity-mapped by mmio_map_init(). */
static inline volatile uint32_t *mmio32(uint64_t phys) {
    return (volatile uint32_t *)(uintptr_t)phys;
}

/* ---- device addresses ----
 *
 * These are qemu `virt` constants, and they are defaults rather than
 * facts: fdt_probe() replaces each one with what the device tree actually
 * says before anything uses it. They stay as initialisers because the
 * console has to work in order to report that device tree parsing failed,
 * which means the very first serial write happens before any of this can
 * be known.
 */
static uint64_t pl011_base  = 0x09000000UL;
static uint64_t pl031_base  = 0x09010000UL;
static uint64_t gicd_base   = 0x08000000UL;
static uint64_t gicc_base   = 0x08010000UL;
static uint64_t virtio_base = 0x0A000000UL;

/*
 * Which machine this is.
 *
 * Not a decoration: a Raspberry Pi shares almost no devices with qemu's
 * `virt`, so the drivers that run are chosen by this and the ones that
 * do not are never touched. Set from the device tree's root node, which
 * names the SoC and cannot lie about it the way a heuristic can.
 */
#define BOARD_VIRT  0
#define BOARD_PI4   1       /* BCM2711: Pi 4, Pi 400, CM4              */
#define BOARD_PI3   2       /* BCM2837: Pi 3                           */

static int board_kind = BOARD_VIRT;

/* Broadcom peripherals, all zero until a Pi is identified. */
static uint64_t bcm_periph_base = 0;    /* 0xFE000000 on a Pi 4        */
static uint64_t bcm_mbox_base   = 0;    /* VideoCore property mailbox  */
static uint64_t bcm_emmc_base   = 0;    /* SD card host controller     */
static uint64_t bcm_genet_base  = 0;    /* gigabit ethernet, Pi 4 only */

static const char *board_name(void) {
    switch (board_kind) {
        case BOARD_PI4: return "Raspberry Pi 4 (BCM2711)";
        case BOARD_PI3: return "Raspberry Pi 3 (BCM2837)";
    }
    return "qemu virt";
}

/* ---- PL011 UART ---- */

#define PL011_BASE    pl011_base
#define PL011_DR      (*mmio32(PL011_BASE + 0x00))
#define PL011_FR      (*mmio32(PL011_BASE + 0x18))
#define PL011_FR_TXFF (1u << 5)

static void serial_putc(char c) {
    if (c == '\n') {
        while (PL011_FR & PL011_FR_TXFF) { }
        PL011_DR = '\r';
    }
    while (PL011_FR & PL011_FR_TXFF) { }
    PL011_DR = (uint32_t)(unsigned char)c;
}

static void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

static void serial_put_hex64(uint64_t v) {
    static const char hx[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(hx[(v >> i) & 0xF]);
}

static void serial_put_u64(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[20] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    serial_puts(buf + i);
}

/* The x86 tree logs bytes as hex from several drivers */
static void serial_put_hex32(uint32_t v) {
    static const char hx[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_putc(hx[(v >> i) & 0xF]);
}

/* ---- PL031 real-time clock ----
 *
 * Replaces the CMOS at ports 0x70/0x71. One 32-bit register holding a
 * Unix timestamp, which is friendlier than the CMOS's BCD registers —
 * no update-in-progress race to worry about, and no BCD conversion.
 */
#define PL031_BASE pl031_base
#define PL031_DR   (*mmio32(PL031_BASE + 0x00))

static uint32_t rtc_epoch(void) { return PL031_DR; }

/* ---- architected generic timer ----
 *
 * CNTFRQ_EL0 reports the counter's frequency and CNTPCT_EL0 is the count
 * itself: a monotonic tick that needs no programming and no interrupt.
 * The x86 side had to configure the PIT and service IRQ0 to get the same
 * thing, and its tick count drifted from real time whenever a frame ran
 * long. This does not.
 */
static uint64_t timer_hz(void) {
    uint64_t f = SYSREG_READ(cntfrq_el0);
    return f ? f : 62500000ULL;         /* QEMU virt's usual 62.5 MHz */
}

static uint64_t timer_count(void) {
    ISB();                              /* the counter read may be reordered */
    return SYSREG_READ(cntpct_el0);
}

/* Milliseconds since boot */
static uint64_t timer_ms(void) {
    return timer_count() / (timer_hz() / 1000);
}

/*
 * Time budgets, for work too big to finish inside one frame.
 *
 * Reading a 400 MB model off the disk, or running a transformer over a
 * retrieved article, cannot be done in a frame and cannot be paced by a
 * fixed number of pieces either: too many blows the frame's deadline, too
 * few means the work never finishes. The right count depends on how fast
 * the machine is, which is exactly what a count cannot express — so the
 * caller states a slice of time instead.
 *
 * The names match the x86 tree's, which spells them with rdtsc and a
 * calibrated PIT. Here the architected counter already reports its own
 * frequency, so there is nothing to calibrate. Sharing the names is what
 * lets apps.h and term.h stay byte-identical across the two trees.
 */
static inline uint64_t cycle_now(void) { return timer_count(); }

static inline int budget_expired_ms(uint64_t start, uint32_t ms) {
    uint64_t hz = timer_hz();
    return (timer_count() - start) >= (hz / 1000ULL) * (uint64_t)ms;
}

/* Microseconds, for spans a millisecond cannot resolve. */
static inline uint32_t cycles_to_us(uint64_t cycles) {
    uint64_t per_us = timer_hz() / 1000000ULL;
    return per_us ? (uint32_t)(cycles / per_us) : 0;
}

/* For reporting an elapsed span. */
static inline uint32_t cycles_to_ms(uint64_t cycles) {
    uint64_t per_ms = timer_hz() / 1000ULL;
    return per_ms ? (uint32_t)(cycles / per_ms) : 0;
}

/*
 * Spin until `until` on the raw counter, parking the core between checks.
 *
 * Bounded deliberately. A wait that can never finish — a target computed
 * from a bad frequency, say — would hang the machine with no output and
 * nothing to look at, which is the worst failure mode to debug on bare
 * metal. Giving up late and carrying on turns that into a visibly wrong
 * frame rate instead, which is something you can see and measure.
 */
static void timer_wait_until(uint64_t until) {
    /*
     * The bound is a second of elapsed *time*, not a spin count, so it
     * means the same thing on a 62.5 MHz virtual counter and on whatever
     * real hardware reports. A target that is nonsense — computed from a
     * bad frequency, or already far in the past after a long frame —
     * costs one late frame rather than a machine that never comes back.
     */
    uint64_t start = timer_count();
    uint64_t limit = timer_hz();
    for (;;) {
        uint64_t now = timer_count();
        if (now >= until) return;
        if (now - start > limit) return;
    }
}

/* ---- floating point ----
 *
 * The whole of fpu_init's CR0/CR4/fninit dance becomes one field: EL1
 * traps FP and SIMD until CPACR_EL1.FPEN says otherwise. There is no
 * EM/MP/TS equivalent and nothing to reset, and because this kernel
 * never context-switches, no state is ever saved either.
 */
static void fpu_init(void) {
    uint64_t cpacr = SYSREG_READ(cpacr_el1);
    cpacr |= (3ULL << 20);              /* FPEN = 0b11, no trapping at EL0/EL1 */
    SYSREG_WRITE(cpacr_el1, cpacr);
    ISB();
}

/* ---- GICv2, as inherited ---- */
#define GICD_BASE  gicd_base
#define GICC_BASE  gicc_base
#define GICD_CTLR  (*mmio32(GICD_BASE + 0x000))
#define GICC_CTLR  (*mmio32(GICC_BASE + 0x000))

/*
 * Take the machine over from the firmware.
 *
 * EDK2 runs a periodic tick and hands control on with everything still
 * armed: a timer counting toward a comparator, and a GIC configured to
 * deliver the result. Nothing switches that off in between. So the first
 * comparator match after arrival raises an interrupt into a controller
 * this kernel has not configured, out of the middle of whatever it was
 * doing.
 *
 * Both timers, not just one. The physical timer is the obvious candidate
 * and the wrong one: firmware on this machine ticks off the *virtual*
 * timer, so disarming only cntp_ctl_el0 leaves the actual source running
 * and looks like it should have worked. Masking DAIF.I is not a
 * substitute either — a masked interrupt is still asserted, still pending
 * at the controller, and still a wakeup.
 *
 * The GIC goes quiet too, at both ends: the distributor stops forwarding
 * and the CPU interface stops signalling. When this kernel wants
 * interrupts it will program the GIC deliberately, and starting from a
 * controller in a known state is worth more than inheriting one.
 */
static void timer_takeover(void) {
    daif_mask_all();

    SYSREG_WRITE(cntp_ctl_el0, 0);      /* physical timer: disarmed */
    SYSREG_WRITE(cntv_ctl_el0, 0);      /* virtual timer: the one in use */
    ISB();

    GICC_CTLR = 0;                      /* CPU interface: signal nothing */
    GICD_CTLR = 0;                      /* distributor: forward nothing */
    DSB();
    ISB();
}

/*
 * Report the translation regime the firmware handed over.
 *
 * Worth its twenty lines: on x86 the kernel builds its own page tables and
 * knows what they say, but here Limine enables the MMU before the kernel
 * runs and every address it uses afterwards — stack, framebuffer, its own
 * code — depends on tables it never saw. When something faults, the first
 * question is always whether the mapping was ever there, and guessing at
 * that from a fault address alone is how an afternoon disappears.
 */
static void mmu_report(void) {
    uint64_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    serial_puts("[vextro/arm64] SP     "); serial_put_hex64(sp);
    serial_puts("\n[vextro/arm64] TTBR0  "); serial_put_hex64(SYSREG_READ(ttbr0_el1));
    serial_puts("\n[vextro/arm64] TTBR1  "); serial_put_hex64(SYSREG_READ(ttbr1_el1));
    serial_puts("\n[vextro/arm64] TCR    "); serial_put_hex64(SYSREG_READ(tcr_el1));
    serial_puts("\n[vextro/arm64] SCTLR  "); serial_put_hex64(SYSREG_READ(sctlr_el1));
    serial_puts("\n[vextro/arm64] MAIR   "); serial_put_hex64(SYSREG_READ(mair_el1));
    serial_puts("\n");
}

/*
 * Ask the hardware whether an address is mapped, without touching it.
 *
 * AT S1E1W runs one address through the stage-1 write permission check and
 * leaves the answer in PAR_EL1: bit 0 set means the walk failed. This is
 * the only way to test a mapping that does not involve faulting on it,
 * which matters when the address under suspicion is the stack the fault
 * handler would need in order to tell you about it.
 */
static uint64_t mmu_probe_write(uint64_t va) {
    __asm__ volatile("at s1e1w, %0" :: "r"(va) : "memory");
    ISB();
    return SYSREG_READ(par_el1);
}

/* ---- device memory mapping ----
 *
 * A translation regime of our own for the bottom half of the address space.
 * TTBR1 — the kernel, the stack, the direct map, the framebuffer — stays
 * exactly as Limine left it, so the code performing this switch keeps
 * running throughout. Only TTBR0 is replaced, and the kernel uses no low
 * addresses except the device registers being mapped here.
 *
 * One 1 GB block descriptor covers everything the `virt` machine puts below
 * RAM: the GIC at 0x08000000, the UART at 0x09000000, the RTC at 0x09010000,
 * fw_cfg, and the virtio-mmio transports. PCIe windows arrive with storage.
 */
#define PTE_VALID   (1ULL << 0)
#define PTE_TABLE   (3ULL << 0)         /* table descriptor: valid + table */
#define PTE_BLOCK   (1ULL << 0)         /* block descriptor at L1/L2 */
#define PTE_ATTR(i) ((uint64_t)(i) << 2)
#define PTE_AP_RW   (0ULL << 6)         /* EL1 read/write, EL0 none */
#define PTE_AF      (1ULL << 10)        /* mandatory: without it, every
                                         * access faults on first use */
#define PTE_PXN     (1ULL << 53)
#define PTE_UXN     (1ULL << 54)

/*
 * MAIR_EL1 as Limine leaves it is 0x...FFFF: attribute 0 and 1 are Normal
 * write-back, and 2 through 7 are zero, which encodes Device-nGnRnE. Index
 * 2 is therefore device memory without having to reprogram MAIR and
 * without disturbing the attributes the existing mappings already use.
 */
#define MAIR_DEVICE_IDX 2

/*
 * Index 3, which this kernel programs itself: Normal, Non-cacheable,
 * inner and outer — the attribute a framebuffer wants.
 *
 * Limine leaves 2 through 7 as zero and uses only 0 and 1, so 3 is free.
 * That is checked rather than assumed: MAIR is a live register and a
 * mapping already made with index 3 would silently change meaning
 * underneath it. If the slot is not what it should be, the framebuffer
 * falls back to Normal write-back — slower to display, since the frame
 * sits in a cache the scanout hardware cannot see, but not wrong the way
 * an unexpected attribute change would be.
 */
#define MAIR_NC_IDX  3
#define MAIR_NC_ATTR 0x44ULL          /* Normal, outer NC, inner NC */

static int mair_nc_ready = 0;

static void mair_setup(void) {
    uint64_t mair = SYSREG_READ(mair_el1);
    uint64_t cur = (mair >> (8 * MAIR_NC_IDX)) & 0xFF;

    if (cur == MAIR_NC_ATTR) { mair_nc_ready = 1; return; }
    if (cur != 0x00) { mair_nc_ready = 0; return; }   /* in use: leave it */

    mair |= MAIR_NC_ATTR << (8 * MAIR_NC_IDX);
    SYSREG_WRITE(mair_el1, mair);
    DSB();
    ISB();
    mair_nc_ready = 1;
}

/* Where loaded applications are mapped, and the physical page backing
 * that window. Set before mmio_map_init(); zero means no app window. */
#define APP_WINDOW_VA   0x02000000UL
#define APP_WINDOW_SIZE (2u * 1024u * 1024u)
static uint64_t app_region_phys = 0;

static uint64_t dev_l0[512] __attribute__((aligned(4096)));
static uint64_t dev_l1[512] __attribute__((aligned(4096)));

/*
 * Physical address of a virtual one, asked of the hardware.
 *
 * The alternative is Limine's kernel-address response and some arithmetic,
 * but the MMU already knows the answer and AT S1E1R is how you ask it: it
 * runs the address through the current stage-1 tables and leaves the result
 * in PAR_EL1. Bit 0 set means the walk failed.
 */
static uint64_t virt_to_phys(const void *p) {
    uint64_t va = (uint64_t)(uintptr_t)p;
    __asm__ volatile("at s1e1r, %0" :: "r"(va) : "memory");
    ISB();
    uint64_t par = SYSREG_READ(par_el1);
    if (par & 1) return 0;              /* untranslatable */
    return (par & 0x0000FFFFFFFFF000ULL) | (va & 0xFFF);
}

/*
 * What physically exists, and where.
 *
 * Two lists, both filled before mmio_map_init() runs: the memory the
 * firmware reports as backed, and the register windows the device tree
 * says hold devices. The mapper builds tables from them rather than from
 * constants, which is the difference between a kernel that boots one
 * machine and one that boots the machine it is on.
 *
 * The `virt` device window is the initial entry so that the UART works
 * before any of this is known — the console has to exist in order to
 * report that device tree parsing failed.
 */
#define MMIO_REGIONS_MAX 10
#define RAM_REGIONS_MAX  12

/*
 * `kind` distinguishes registers from a framebuffer, and the difference
 * is not cosmetic.
 *
 * Device-nGnRnE is the right memory type for a control register: no
 * gathering, no reordering, no early acknowledgement, because writing one
 * has a side effect and the order it happens in is the whole point.
 * Applying that to a framebuffer is a performance catastrophe — every
 * 32-bit pixel store becomes its own bus transaction that the core waits
 * on, and a 1024x768 frame is three quarters of a million of them.
 *
 * A framebuffer wants the opposite: stores gathered and reordered
 * freely, since nothing reads it until the frame is finished, but never
 * held in a cache the display controller cannot see. That is Normal
 * Non-cacheable, and it is why REGION_FB exists.
 */
#define REGION_DEVICE 0
#define REGION_FB     1

typedef struct { uint64_t base, size; uint8_t kind; } phys_region_t;

static phys_region_t mmio_regions[MMIO_REGIONS_MAX] = {
    /* qemu virt: GIC at 0x08000000, PL011/PL031/fw_cfg at 0x09000000,
     * virtio-mmio transports at 0x0A000000 */
    { 0x08000000ULL, 0x04000000ULL, REGION_DEVICE },
};
static int mmio_region_count = 1;

static phys_region_t ram_regions[RAM_REGIONS_MAX];
static int ram_region_count = 0;

#define MMIO_BLOCK   0x200000ULL          /* 2 MB, the L2 block size */
#define MMIO_GIGA    0x40000000ULL

static void mmio_region_add_kind(uint64_t base, uint64_t size, uint8_t kind) {
    if (!base && !size) return;
    uint64_t end = (base + size + MMIO_BLOCK - 1) & ~(MMIO_BLOCK - 1);
    base &= ~(MMIO_BLOCK - 1);
    if (end <= base) return;

    for (int i = 0; i < mmio_region_count; i++) {
        uint64_t rend = mmio_regions[i].base + mmio_regions[i].size;
        if (base >= mmio_regions[i].base && end <= rend &&
            mmio_regions[i].kind == kind) return;              /* covered */
    }
    if (mmio_region_count >= MMIO_REGIONS_MAX) return;
    mmio_regions[mmio_region_count].base = base;
    mmio_regions[mmio_region_count].size = end - base;
    mmio_regions[mmio_region_count].kind = kind;
    mmio_region_count++;
}

static void mmio_region_add(uint64_t base, uint64_t size) {
    mmio_region_add_kind(base, size, REGION_DEVICE);
}

/* Which kind of mapping a block wants, or -1 if no region claims it.
 * A framebuffer wins over a device window if both somehow claim the same
 * block, because the framebuffer is the one whose performance depends on
 * the answer. */
static int mmio_kind_at(uint64_t base, uint64_t size) {
    int found = -1;
    uint64_t end = base + size;
    for (int i = 0; i < mmio_region_count; i++) {
        if (base < mmio_regions[i].base + mmio_regions[i].size &&
            end > mmio_regions[i].base) {
            if (mmio_regions[i].kind == REGION_FB) return REGION_FB;
            found = REGION_DEVICE;
        }
    }
    return found;
}

/* Replace the built-in `virt` window entirely — a board that is not virt
 * has nothing at 0x08000000 and mapping it would be a promise about an
 * address that answers to nobody. */
static void mmio_region_reset(void) { mmio_region_count = 0; }

/* Adjacent entries are merged, because a firmware memory map arrives as
 * dozens of small runs and the list only needs to answer "is this
 * backed?". */
static void ram_region_add(uint64_t base, uint64_t len) {
    if (!len) return;
    uint64_t end = base + len;

    for (int i = 0; i < ram_region_count; i++) {
        uint64_t rend = ram_regions[i].base + ram_regions[i].size;
        if (base <= rend && end >= ram_regions[i].base) {
            if (base < ram_regions[i].base) ram_regions[i].base = base;
            if (end > rend) rend = end;
            ram_regions[i].size = rend - ram_regions[i].base;
            return;
        }
    }
    if (ram_region_count >= RAM_REGIONS_MAX) return;
    ram_regions[ram_region_count].base = base;
    ram_regions[ram_region_count].size = len;
    ram_region_count++;
}

static int region_covers(const phys_region_t *list, int n,
                         uint64_t base, uint64_t size) {
    uint64_t end = base + size;
    for (int i = 0; i < n; i++)
        if (base >= list[i].base && end <= list[i].base + list[i].size) return 1;
    return 0;
}

static int region_overlaps(const phys_region_t *list, int n,
                           uint64_t base, uint64_t size) {
    uint64_t end = base + size;
    for (int i = 0; i < n; i++)
        if (base < list[i].base + list[i].size && end > list[i].base) return 1;
    return 0;
}

/*
 * Second-level tables come from a pool, one per gigabyte that needs
 * finer granularity than a 1 GB block. On qemu virt exactly one does; on
 * a Pi 4 it is two, since the peripherals share their gigabyte with RAM.
 */
#define MMIO_L2_MAX 8
static uint64_t dev_l2_pool[MMIO_L2_MAX][512] __attribute__((aligned(4096)));
static uint64_t dev_l2_gb[MMIO_L2_MAX];
static int dev_l2_n = 0;

static uint64_t *mmio_l2_alloc(uint64_t gb) {
    for (int i = 0; i < dev_l2_n; i++)
        if (dev_l2_gb[i] == gb) return dev_l2_pool[i];
    if (dev_l2_n >= MMIO_L2_MAX) return 0;
    uint64_t *t = dev_l2_pool[dev_l2_n];
    for (int i = 0; i < 512; i++) t[i] = 0;
    dev_l2_gb[dev_l2_n] = gb;
    dev_l2_n++;
    return t;
}

/*
 * Build the tables.
 *
 * Every gigabyte is classified independently. One that is entirely
 * backed RAM and holds no device becomes a single 1 GB block, which is
 * both cheapest and kindest to the TLB. Anything else — a device in it,
 * a hole in it, or gigabyte zero, which always carries the application
 * window — is described 2 MB at a time.
 *
 * The rule that matters is what happens to a block that is *neither* RAM
 * nor device: it is left invalid, and that is the whole point.
 *
 * Mapping unbacked space as Normal memory is the bug that cost this port
 * more time than anything else. Normal memory is speculatable — the CPU
 * may fetch from it unasked, because the architecture guarantees reading
 * backed memory has no side effects. Map a range that is not backed and
 * that guarantee is void: the core eventually speculates into a physical
 * address nothing answers for. Under emulation this is harmless, since
 * tcg does not speculate. On real silicon under a hypervisor it is a
 * stage-2 fault on an access with no instruction syndrome to decode, and
 * qemu's hvf backend responds by aborting the process outright:
 * `Assertion failed: (isv)`. It fires half a second into any guest that
 * is executing instructions, never in one parked in wfi, and bears no
 * relation to what the code was doing — because the access was never in
 * the code.
 *
 * Deriving the answer from the firmware's own map rather than from a
 * constant is what makes that impossible to get wrong on a machine
 * nobody has tried yet.
 */
/* How much reported memory the 2 MB granularity had to leave out, so
 * that conservative rounding is a number somebody can look at rather
 * than a silent loss. Zero on both machines this has been run on. */
static uint64_t mmio_ram_dropped = 0;

static void mmio_map_init(void) {
    for (int i = 0; i < 512; i++) { dev_l0[i] = 0; dev_l1[i] = 0; }
    dev_l2_n = 0;
    mmio_ram_dropped = 0;

    mair_setup();

    /* How far up anything of interest reaches. */
    uint64_t top = 0;
    for (int i = 0; i < ram_region_count; i++) {
        uint64_t e = ram_regions[i].base + ram_regions[i].size;
        if (e > top) top = e;
    }
    for (int i = 0; i < mmio_region_count; i++) {
        uint64_t e = mmio_regions[i].base + mmio_regions[i].size;
        if (e > top) top = e;
    }
    uint64_t ngb = (top + MMIO_GIGA - 1) / MMIO_GIGA;
    if (ngb > 512) ngb = 512;

    const uint64_t normal_flags = PTE_ATTR(0) | PTE_AP_RW | (3ULL << 8) |
                                  PTE_AF | PTE_PXN | PTE_UXN;
    const uint64_t device_flags = PTE_ATTR(MAIR_DEVICE_IDX) | PTE_AP_RW |
                                  PTE_AF | PTE_PXN | PTE_UXN;
    /* Shareability matters for the framebuffer in a way it does not for
     * a device register: Normal memory needs it declared, and outer
     * shareable is what keeps a store visible to another master. */
    const uint64_t fb_flags = PTE_ATTR(mair_nc_ready ? MAIR_NC_IDX : 0) |
                              PTE_AP_RW | (2ULL << 8) | PTE_AF |
                              PTE_PXN | PTE_UXN;

    for (uint64_t g = 0; g < ngb; g++) {
        uint64_t gbase = g * MMIO_GIGA;

        int whole_ram = region_covers(ram_regions, ram_region_count, gbase, MMIO_GIGA);
        int has_dev   = region_overlaps(mmio_regions, mmio_region_count, gbase, MMIO_GIGA);

        if (g != 0 && whole_ram && !has_dev) {
            dev_l1[g] = gbase | PTE_BLOCK | normal_flags;
            continue;
        }

        uint64_t *l2 = mmio_l2_alloc(g);
        if (!l2) {
            /* Out of tables. Falling back to a 1 GB Normal block is only
             * safe where the gigabyte really is all RAM; otherwise leave
             * it unmapped and let a fault name the address. */
            if (whole_ram) dev_l1[g] = gbase | PTE_BLOCK | normal_flags;
            continue;
        }

        for (uint64_t b = 0; b < 512; b++) {
            uint64_t pa = gbase + b * MMIO_BLOCK;
            /* Device first: where a device tree claims registers, they are
             * registers, whatever a memory map may also say about it. */
            int kind = mmio_kind_at(pa, MMIO_BLOCK);
            if (kind == REGION_FB)
                l2[b] = pa | PTE_BLOCK | fb_flags;
            else if (kind == REGION_DEVICE)
                l2[b] = pa | PTE_BLOCK | device_flags;
            else if (region_covers(ram_regions, ram_region_count, pa, MMIO_BLOCK))
                l2[b] = pa | PTE_BLOCK | normal_flags;
            else {
                /* Not fully backed, so not mapped — the conservative
                 * direction, and the one the hvf abort demands. A block
                 * that is *partly* RAM loses that part, which is only
                 * possible where a reported region does not end on a
                 * 2 MB boundary; counting it makes that visible. */
                if (region_overlaps(ram_regions, ram_region_count, pa, MMIO_BLOCK))
                    mmio_ram_dropped += MMIO_BLOCK;
                l2[b] = 0;
            }
        }
        dev_l1[g] = virt_to_phys(l2) | PTE_TABLE;
    }

    /*
     * One 2 MB window at 0x02000000 where loaded applications run.
     *
     * .bss cannot host them: Limine maps each program header with the
     * permissions it declares, so the kernel's data segments are mapped
     * execute-never and a jump into one faults. Rather than weaken the
     * kernel's own mapping, this maps a separate window as executable and
     * leaves everything else alone — the W^X property the .bsd format
     * exists to preserve is worth more than the two lines it costs here.
     *
     * The backing store is still a .bss array; only the view is
     * different. app_region_phys is filled in by the loader before this
     * runs, because a mapping needs to know what it points at. It is
     * written after the loop so it survives whatever the loop decided
     * about gigabyte zero.
     */
    if (app_region_phys) {
        uint64_t *l2 = mmio_l2_alloc(0);
        if (l2) {
            l2[APP_WINDOW_VA >> 21] =
                app_region_phys | PTE_BLOCK | PTE_ATTR(0) | PTE_AP_RW |
                (3ULL << 8) | PTE_AF | PTE_UXN; /* PXN absent: EL1 may run it */
            dev_l1[0] = virt_to_phys(l2) | PTE_TABLE;
        }
    }

    dev_l0[0] = virt_to_phys(dev_l1) | PTE_TABLE;

    SYSREG_WRITE(ttbr0_el1, virt_to_phys(dev_l0));
    DSB();
    ISB();
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    DSB();
    ISB();
}

/*
 * What the mapper decided, in one line.
 *
 * Separate from mmu_report() only because that runs before this file's
 * state exists. The two numbers are the ones worth seeing: how many
 * gigabytes needed 2 MB granularity rather than a single block, and how
 * much reported memory that granularity had to leave out. The second
 * should be zero, and if it is ever not, it is the first thing to look
 * at when a machine is mysteriously short of RAM.
 */
static void mmio_report(void) {
    serial_puts("[vextro/arm64] map: ");
    serial_put_u64((uint64_t)mmio_region_count);
    serial_puts(" device regions, ");
    serial_put_u64((uint64_t)dev_l2_n);
    serial_puts(" split gigabytes, framebuffer type ");
    serial_puts(mair_nc_ready ? "normal-nc" : "normal-wb (MAIR slot taken)");
    if (mmio_ram_dropped) {
        serial_puts(", DROPPED ");
        serial_put_u64(mmio_ram_dropped / 1024);
        serial_puts(" KB of reported RAM");
    }
    serial_puts("\n");
}

/*
 * Physical address to something the kernel can dereference.
 *
 * The low 4 GB is identity-mapped by mmio_map_init(), which covers all of
 * this machine's RAM and every device region, so the mapping is the
 * identity. It exists as a function because the x86 tree has one and the
 * portable code calls it; when this kernel maps memory somewhere other
 * than where it lives, only this changes.
 */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)phys;
}

/*
 * Reset the machine.
 *
 * The x86 tree pulses the PS/2 controller's reset line — `outb(0x64,
 * 0xFE)` — which is a keyboard controller being used as a power button
 * for historical reasons. aarch64 has an actual interface for this: PSCI,
 * a firmware call defined by the architecture. SYSTEM_RESET is function
 * 0x84000009 and takes no arguments.
 *
 * HVC rather than SMC because qemu's virt machine puts its PSCI
 * implementation at the hypervisor level when there is no secure
 * firmware, which is how this kernel is booted.
 */
static void machine_reset(void) {
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;   /* SYSTEM_RESET */
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
    for (;;) __asm__ volatile("wfi");     /* not reached */
}

/*
 * Replace the built-in addresses with what the device tree says.
 *
 * Called after the console works, deliberately. A device tree that cannot
 * be parsed has to be reported somehow, and the only way to report it is
 * the UART — so the UART's default address has to be usable before this
 * runs. On qemu `virt` the defaults are already right and nothing moves;
 * the point is that on a board they will not be, and this is what makes
 * that survivable.
 *
 * Matched by `compatible` rather than node name. A board is free to call
 * its UART node anything it likes and frequently does; what it may not do
 * is claim compatibility with "arm,pl011" for something that is not one.
 * Matching names works on qemu and quietly fails everywhere else, which
 * is precisely the class of bug that cannot be found without the board.
 */
static int fdt_ok = 0;

/*
 * Discovery: addresses only, no output.
 *
 * This has to run *before* mmio_map_init(), which is the awkward part.
 * The mapper needs to know which physical windows hold devices in order
 * to map them, and until it has run there is no UART — so discovery
 * cannot print a word about what it found, or even that it failed. All
 * of that waits for fdt_report(), which runs once the console exists.
 *
 * The alternative, mapping a generous window and probing afterwards, is
 * what the port did until a Raspberry Pi turned up: its peripherals are
 * at 0xFE000000 and no fixed guess covers both that and qemu's
 * 0x08000000 without also mapping three gigabytes of nothing in between.
 *
 * Matched by `compatible` rather than node name. A board is free to call
 * its UART node anything it likes and frequently does; what it may not
 * do is claim compatibility with "arm,pl011" for something that is not
 * one. Matching names works on qemu and quietly fails everywhere else,
 * which is precisely the class of bug that cannot be found without the
 * board.
 */
static void fdt_discover(const void *blob) {
    fdt_ok = fdt_init(blob);
    if (!fdt_ok) return;

    /*
     * The board first, because it decides which of the rest applies.
     * The Pi 4's tree claims "brcm,bcm2711"; the Pi 3's claims
     * "brcm,bcm2837". Both also claim a "raspberrypi,*" model string,
     * but the SoC is what the register layout follows.
     */
    if (fdt_board_is("brcm,bcm2711"))      board_kind = BOARD_PI4;
    else if (fdt_board_is("brcm,bcm2837")) board_kind = BOARD_PI3;
    else if (fdt_board_is("brcm,bcm2836") ||
             fdt_board_is("brcm,bcm2835")) board_kind = BOARD_PI3;

    /*
     * Nothing below names a node, only what it claims compatibility
     * with. A Raspberry Pi's UART node is "serial@7e201000" and its
     * interrupt controller is "interrupt-controller@40041000"; neither
     * of those strings should have to appear in a kernel, and a lookup
     * that depends on them is a lookup that works on exactly one
     * machine.
     */
    uint64_t v;
    if ((v = fdt_reg_base(0, "arm,pl011")))  pl011_base = v;
    if ((v = fdt_reg_base(0, "arm,pl031")))  pl031_base = v;

    /* GICv2 first, then v3. The two are not interchangeable — v3's second
     * range is a redistributor, not a CPU interface, and its CPU
     * interface is reached through system registers instead — so only the
     * distributor is taken from a v3 tree and the rest is left alone
     * until something here actually programs one. */
    static const char *gic2[] = { "arm,cortex-a15-gic", "arm,gic-400", 0 };
    for (int i = 0; gic2[i]; i++) {
        if ((v = fdt_reg_index(0, gic2[i], 0, 0))) {
            gicd_base = v;
            if ((v = fdt_reg_index(0, gic2[i], 1, 0))) gicc_base = v;
            break;
        }
    }
    if ((v = fdt_reg_base(0, "arm,gic-v3"))) gicd_base = v;
    if ((v = fdt_reg_base(0, "virtio,mmio"))) virtio_base = v;

    if (board_kind != BOARD_VIRT) {
        /*
         * A Pi's peripherals are one contiguous window, so finding any
         * one node inside it locates the whole thing. The GPIO
         * controller is the anchor: every Pi has one, it is always at
         * offset 0x200000, and unlike the UART it is never disabled or
         * handed to the second core.
         */
        if ((v = fdt_reg_base(0, "brcm,bcm2711-gpio")) ||
            (v = fdt_reg_base(0, "brcm,bcm2835-gpio")))
            bcm_periph_base = v - 0x200000ULL;
        else if (pl011_base)
            bcm_periph_base = pl011_base - 0x201000ULL;

        /* The mailbox has a node of its own; the arithmetic is only the
         * fallback for a tree that leaves it out. */
        if (!(bcm_mbox_base = fdt_reg_base(0, "brcm,bcm2835-mbox")))
            if (bcm_periph_base) bcm_mbox_base = bcm_periph_base + 0xB880ULL;

        /* Storage. The Pi 4 routes the SD card to a second, SDHCI-clean
         * controller ("emmc2"); earlier boards use the Arasan block. */
        if (!(bcm_emmc_base = fdt_reg_base(0, "brcm,bcm2711-emmc2")))
            if (!(bcm_emmc_base = fdt_reg_base(0, "brcm,bcm2835-sdhci")))
                bcm_emmc_base = fdt_reg_base(0, "brcm,bcm2711-sdhci");

        /* Ethernet, Pi 4 only: earlier boards hang theirs off USB. */
        if (!(bcm_genet_base = fdt_reg_base(0, "brcm,bcm2711-genet-v5")))
            bcm_genet_base = fdt_reg_base(0, "brcm,genet-v5");
    }

    /*
     * Tell the mapper what to map.
     *
     * The `virt` default window is dropped outright on a board rather
     * than kept alongside: a Pi has nothing at 0x08000000, and a mapping
     * is a promise that an access lands somewhere. Promising for an
     * address nothing answers to is how this port lost a week.
     */
    if (board_kind != BOARD_VIRT) {
        mmio_region_reset();
        if (bcm_periph_base) {
            /* The whole peripheral block: UART, GPIO, mailbox, SD, the
             * lot. 24 MB on a BCM2711 and 16 MB before it. */
            mmio_region_add(bcm_periph_base, 0x01800000ULL);
        }
        if (bcm_genet_base)  mmio_region_add(bcm_genet_base, 0x10000ULL);
        if (gicd_base)       mmio_region_add(gicd_base, 0x8000ULL);
        if (gicc_base)       mmio_region_add(gicc_base, 0x8000ULL);
        if (pl011_base)      mmio_region_add(pl011_base, 0x1000ULL);
    } else {
        if (virtio_base) mmio_region_add(virtio_base, 0x4000ULL);
        if (gicd_base)   mmio_region_add(gicd_base, 0x10000ULL);
        if (pl011_base)  mmio_region_add(pl011_base, 0x1000ULL);
    }
}

/* Reporting, once there is a console to report to. */
static void fdt_report(const void *blob) {
    serial_puts("[vextro/arm64] fdt: blob ");
    serial_put_hex64((uint64_t)(uintptr_t)blob);
    if (blob) {
        serial_puts(" magic ");
        serial_put_hex32(fdt_be32((const uint8_t *)blob));
    }
    serial_puts("\n");
    if (!fdt_ok) {
        serial_puts("[vextro/arm64] fdt: none (keeping virt defaults)\n");
        return;
    }

    serial_puts("[vextro/arm64] board: ");
    serial_puts(board_name());
    serial_puts("\n");

    if (board_kind != BOARD_VIRT) {
        serial_puts("[vextro/arm64] bcm: periph ");
        serial_put_hex64(bcm_periph_base);
        serial_puts(" mbox "); serial_put_hex64(bcm_mbox_base);
        serial_puts("\n[vextro/arm64] bcm: emmc ");
        serial_put_hex64(bcm_emmc_base);
        serial_puts(" genet "); serial_put_hex64(bcm_genet_base);
        serial_puts("\n");
    }

    serial_puts("[vextro/arm64] fdt: uart ");
    serial_put_hex64(pl011_base);
    serial_puts(" rtc "); serial_put_hex64(pl031_base);
    serial_puts("\n[vextro/arm64] fdt: gicd ");
    serial_put_hex64(gicd_base);
    serial_puts(" gicc "); serial_put_hex64(gicc_base);
    serial_puts("\n[vextro/arm64] fdt: virtio ");
    serial_put_hex64(virtio_base);
    serial_puts("\n");
}

/*
 * Power off.
 *
 * The x86 tree writes 0x2000 to the ACPI PM1a control block at 0x604, and
 * again at 0xB004 for older qemu — two magic port pokes chosen by trial.
 * PSCI has a function for it: SYSTEM_OFF, 0x84000008, no arguments, one
 * call, defined by the architecture rather than by a chipset.
 */
static void machine_poweroff(void) {
    register uint64_t x0 __asm__("x0") = 0x84000008ULL;   /* SYSTEM_OFF */
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
    for (;;) __asm__ volatile("wfi");     /* not reached */
}

/* ---- exception vectors ---- */

extern char exception_vectors[];        /* defined in vectors.S */

static void exceptions_init(void) {
    SYSREG_WRITE(vbar_el1, (uint64_t)(uintptr_t)exception_vectors);
    ISB();
}

/*
 * Called from the vector trampoline for anything unexpected.
 *
 * The x86 build halted silently on a fault, which meant a page fault and
 * a hung render loop looked identical from the outside. Bare metal has
 * no debugger attached by default, so a fault that says what it was and
 * where is worth more than the few lines it costs.
 */
void arm_fault(uint64_t kind, uint64_t esr, uint64_t elr, uint64_t far) {
    static const char *names[4] = {
        "synchronous", "IRQ", "FIQ", "SError"
    };
    serial_puts("\n[vextro/arm64] unhandled ");
    serial_puts(names[kind & 3]);
    serial_puts(" exception\n  ESR_EL1 ");
    serial_put_hex64(esr);
    serial_puts("  (EC ");
    serial_put_u64((esr >> 26) & 0x3F);
    serial_puts(")\n  ELR_EL1 ");
    serial_put_hex64(elr);
    serial_puts("\n  FAR_EL1 ");
    serial_put_hex64(far);
    serial_puts("\n");
    for (;;) __asm__ volatile("wfi");
}

#endif /* ARM_H */
