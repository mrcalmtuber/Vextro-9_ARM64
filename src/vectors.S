/*
 * AArch64 exception vector table.
 *
 * Not a table of descriptors like the x86 IDT — a table of *code*. Sixteen
 * slots of 128 bytes each, 2 KB-aligned, and the CPU branches into the slot
 * rather than reading a gate out of it. There is no vector number either:
 * the handler reads ESR_EL1 to find out what happened.
 *
 * The sixteen are four groups of four (synchronous, IRQ, FIQ, SError):
 *
 *   0x000  taken from EL1 while using SP_EL0
 *   0x200  taken from EL1 while using SP_EL1   <- where Limine leaves us
 *   0x400  taken from EL0 in aarch64 state
 *   0x600  taken from EL0 in aarch32 state
 *
 * Every slot reports and halts. Nothing here takes interrupts yet: the
 * render loop paces itself off the generic timer's counter, so the only
 * exceptions that can arrive are faults.
 *
 * Each slot does two things before it reports, and both were learned the
 * hard way:
 *
 *   - It switches to a stack of its own. A fault handler that runs on the
 *     stack it was interrupted on cannot report a fault *caused* by that
 *     stack, and a bad stack is one of the likeliest things to go wrong
 *     early in a port. This one lives in the kernel's own .bss, so it is
 *     mapped by the same program header as the code reading it and cannot
 *     depend on anything the firmware left behind.
 *
 *   - It refuses to re-enter. Without the depth counter, a fault inside
 *     the reporter re-enters the same slot, faults again at the same
 *     instruction, and loops forever. From the outside that is
 *     indistinguishable from a hang: the machine stays up, the screen
 *     stops, and almost nothing reaches the serial line because the very
 *     first thing to fault is the reporter's own prologue. That exact
 *     loop is what earlier work on this port spent a long time chasing as
 *     a "render loop stall".
 *
 * Registers are not saved because none of these return. When a device
 * needs a real interrupt, this grows a save/restore trampoline; until
 * something does, it would be untested code.
 */

.section .text, "ax", %progbits

.macro VECTOR_ENTRY kind
    .balign 0x80
    /* Second time through? Report nothing and stop; see above. */
    adrp    x0, fault_depth
    add     x0, x0, :lo12:fault_depth
    ldr     w1, [x0]
    add     w2, w1, #1
    str     w2, [x0]
    cbnz    w1, 99f

    /* A stack that belongs to us, not to whatever faulted. */
    adrp    x0, fault_stack_top
    add     x0, x0, :lo12:fault_stack_top
    mov     sp, x0

    mov     x0, #\kind
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      arm_fault
99: b       .
.endm

.balign 2048
.global exception_vectors
.type exception_vectors, %object
exception_vectors:
    /* Current EL, SP_EL0 */
    VECTOR_ENTRY 0          /* synchronous */
    VECTOR_ENTRY 1          /* IRQ         */
    VECTOR_ENTRY 2          /* FIQ         */
    VECTOR_ENTRY 3          /* SError      */

    /* Current EL, SP_EL1 — the ones that can actually fire here */
    VECTOR_ENTRY 0
    VECTOR_ENTRY 1
    VECTOR_ENTRY 2
    VECTOR_ENTRY 3

    /* Lower EL, aarch64 */
    VECTOR_ENTRY 0
    VECTOR_ENTRY 1
    VECTOR_ENTRY 2
    VECTOR_ENTRY 3

    /* Lower EL, aarch32 */
    VECTOR_ENTRY 0
    VECTOR_ENTRY 1
    VECTOR_ENTRY 2
    VECTOR_ENTRY 3
.size exception_vectors, . - exception_vectors

.section .bss, "aw", %nobits
.balign 16
.global fault_depth
fault_depth:
    .space 16
.global fault_stack
fault_stack:
    .space 16384
.global fault_stack_top
fault_stack_top:
