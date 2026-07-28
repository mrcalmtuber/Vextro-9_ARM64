#ifndef ARM_H
#define ARM_H

#include <stdint.h>

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

/* ---- PL011 UART ---- */

#define PL011_BASE    0x09000000UL
#define PL011_DR      (*(volatile uint32_t *)(PL011_BASE + 0x00))
#define PL011_FR      (*(volatile uint32_t *)(PL011_BASE + 0x18))
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
#define PL031_BASE 0x09010000UL
#define PL031_DR   (*(volatile uint32_t *)(PL031_BASE + 0x00))

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
     * Bounded by a count small enough that overrunning it costs a frame,
     * not a minute. `yield` is deliberately absent: it is only a hint on
     * real silicon, but under a hypervisor it can trap, which turns a
     * spin that was meant to be cheap into one that leaves the machine
     * apparently dead. Reading the counter is enough of a pause.
     */
    for (uint32_t i = 0; i < 4000000u; i++) {
        if (timer_count() >= until) return;
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

/*
 * Take the machine over from the firmware.
 *
 * EDK2 runs its own periodic tick off the EL1 physical timer and leaves
 * it armed when it hands control on. Nothing turns it off in between, so
 * the first comparator match after we arrive raises PPI 30 into an
 * interrupt controller this kernel has not configured — and the CPU
 * takes an exception out of the middle of whatever it was doing. Masking
 * interrupts and disarming the timer is the first thing a kernel should
 * do with inherited hardware, and costs two register writes.
 */
static void timer_takeover(void) {
    irq_disable();
    SYSREG_WRITE(cntp_ctl_el0, 0);      /* disable, unmask; no comparator */
    ISB();
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
