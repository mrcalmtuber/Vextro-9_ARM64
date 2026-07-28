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
 * Every slot currently reports and halts. Nothing here takes interrupts
 * yet: the render loop paces itself off the generic timer's counter, so
 * the only exceptions that can arrive are faults — and a fault that names
 * itself is worth far more on bare metal than one that hangs quietly,
 * which is what the x86 build's isr_noop_err does today.
 *
 * Registers are not saved because none of these return. When a device
 * needs a real interrupt, this grows a save/restore trampoline; until
 * something does, it would be untested code.
 */

.section .text, "ax", %progbits

.macro VECTOR_ENTRY kind
    .balign 0x80
    mov     x0, #\kind
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      arm_fault
    b       .
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
