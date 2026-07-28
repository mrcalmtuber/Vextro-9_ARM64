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
    serial_puts("[socrates/arm64] SP     "); serial_put_hex64(sp);
    serial_puts("\n[socrates/arm64] TTBR0  "); serial_put_hex64(SYSREG_READ(ttbr0_el1));
    serial_puts("\n[socrates/arm64] TTBR1  "); serial_put_hex64(SYSREG_READ(ttbr1_el1));
    serial_puts("\n[socrates/arm64] TCR    "); serial_put_hex64(SYSREG_READ(tcr_el1));
    serial_puts("\n[socrates/arm64] SCTLR  "); serial_put_hex64(SYSREG_READ(sctlr_el1));
    serial_puts("\n[socrates/arm64] MAIR   "); serial_put_hex64(SYSREG_READ(mair_el1));
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

/* Where loaded applications are mapped, and the physical page backing
 * that window. Set before mmio_map_init(); zero means no app window. */
#define APP_WINDOW_VA   0x02000000UL
#define APP_WINDOW_SIZE (2u * 1024u * 1024u)
static uint64_t app_region_phys = 0;

static uint64_t dev_l0[512] __attribute__((aligned(4096)));
static uint64_t dev_l1[512] __attribute__((aligned(4096)));
static uint64_t dev_l2[512] __attribute__((aligned(4096)));

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
 * Highest physical address the firmware says is backed, rounded up to a
 * gigabyte. Set from Limine's memory map before mmio_map_init() runs.
 */
static uint64_t ram_top_gb = 4;

static void mmio_map_init(void) {
    for (int i = 0; i < 512; i++) { dev_l0[i] = 0; dev_l1[i] = 0; dev_l2[i] = 0; }

    /*
     * Only the 2 MB blocks that hold real devices, rather than the whole
     * first gigabyte.
     *
     * Mapping all of 0..1 GB is one descriptor and very tempting, but most
     * of that range is unbacked on this machine, and a mapping is a promise
     * that an access will land somewhere. Under emulation a stray read of
     * an unbacked address is logged and returns zero; under a hypervisor it
     * is a stage-2 fault on an instruction the backend may not be able to
     * decode, and qemu's hvf backend responds by aborting the whole
     * process. That failure names neither the address nor the instruction,
     * and it takes the serial log's tail with it.
     *
     * Mapping only what exists keeps that from being possible: a stray low
     * access now takes an ordinary stage-1 fault, and the handler prints
     * ESR and FAR and says exactly where it came from.
     *
     *   0x08000000  GICv2 distributor and CPU interface
     *   0x09000000  PL011 UART, PL031 RTC, fw_cfg, PL061 GPIO
     *   0x0A000000  virtio-mmio transports
     */
    for (uint64_t blk = 0x08000000ULL; blk < 0x0C000000ULL; blk += 0x200000ULL) {
        dev_l2[blk >> 21] = blk | PTE_BLOCK | PTE_ATTR(MAIR_DEVICE_IDX) |
                            PTE_AP_RW | PTE_AF | PTE_PXN | PTE_UXN;
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
     * runs, because a mapping needs to know what it points at.
     */
    if (app_region_phys) {
        dev_l2[APP_WINDOW_VA >> 21] =
            app_region_phys | PTE_BLOCK | PTE_ATTR(0) | PTE_AP_RW |
            (3ULL << 8) | PTE_AF | PTE_UXN;     /* PXN absent: EL1 may run it */
    }

    dev_l1[0] = virt_to_phys(dev_l2) | PTE_TABLE;

    /*
     * Identity-map the gigabytes that actually contain RAM, as Normal
     * memory, and not one gigabyte more.
     *
     * The upper bound is the important part, and getting it wrong cost
     * this port more time than anything else. Normal memory is
     * speculatable: the CPU may fetch from it without being asked,
     * because the architecture guarantees that reading backed memory has
     * no side effects. Map a range that is *not* backed and that
     * guarantee is void — the core will eventually speculate into a
     * physical address nothing answers for.
     *
     * Under emulation that is harmless, since tcg does not speculate. On
     * real silicon under a hypervisor it is a stage-2 fault on an access
     * with no instruction syndrome to decode, and qemu's hvf backend
     * responds by aborting the process: `Assertion failed: (isv)`. It
     * fires half a second into any guest that is executing instructions,
     * never in one parked in wfi, and bears no relation to what the code
     * was doing — because the access was never in the code.
     *
     * So the bound comes from the firmware's memory map rather than from
     * a constant. Limine mapped this range too, and replacing TTBR0
     * without it would unmap anything the bootloader reported by physical
     * address — the framebuffer among them.
     */
    for (uint64_t g = 1; g < ram_top_gb; g++) {
        dev_l1[g] = (g << 30) | PTE_BLOCK | PTE_ATTR(0) | PTE_AP_RW |
                    (3ULL << 8) | PTE_AF | PTE_PXN | PTE_UXN;
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
static void fdt_probe(const void *blob) {
    serial_puts("[socrates/arm64] fdt: blob ");
    serial_put_hex64((uint64_t)(uintptr_t)blob);
    if (blob) {
        serial_puts(" magic ");
        serial_put_hex32(fdt_be32((const uint8_t *)blob));
    }
    serial_puts("\n");
    if (!fdt_init(blob)) {
        serial_puts("[socrates/arm64] fdt: none (keeping virt defaults)\n");
        return;
    }

    uint64_t v;
    if ((v = fdt_reg_base("pl011",  "arm,pl011")))       pl011_base  = v;
    if ((v = fdt_reg_base("pl031",  "arm,pl031")))       pl031_base  = v;
    /* GICv2 first, then v3. The two are not interchangeable — v3's second
     * range is a redistributor, not a CPU interface, and its CPU
     * interface is reached through system registers instead — so only the
     * distributor is taken from a v3 tree and the rest is left alone
     * until something here actually programs one. */
    uint32_t len = 0;
    const uint8_t *reg = fdt_find_prop("intc", "arm,cortex-a15-gic", "reg", &len);
    if (reg && len >= 16) {
        gicd_base = fdt_be64(reg);
        if (len >= 32) gicc_base = fdt_be64(reg + 16);
    } else {
        reg = fdt_find_prop("intc", "arm,gic-v3", "reg", &len);
        if (reg && len >= 16) gicd_base = fdt_be64(reg);
    }
    if ((v = fdt_reg_base("virtio_mmio", "virtio,mmio"))) virtio_base = v;

    serial_puts("[socrates/arm64] fdt: uart ");
    serial_put_hex64(pl011_base);
    serial_puts(" rtc "); serial_put_hex64(pl031_base);
    serial_puts("\n[socrates/arm64] fdt: gicd ");
    serial_put_hex64(gicd_base);
    serial_puts(" gicc "); serial_put_hex64(gicc_base);
    serial_puts("\n[socrates/arm64] fdt: virtio ");
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
    serial_puts("\n[socrates/arm64] unhandled ");
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
