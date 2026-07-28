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


/*
 * A system call is the one exception that returns.
 *
 * Everything else in this table reports and halts, so the whole file
 * needed no register saving. SVC changes that: the app expects to resume
 * at the instruction after it with its registers intact, which means a
 * real save/restore trampoline and an eret rather than a branch to self.
 *
 * The synchronous slots therefore test ESR_EL1's exception class first
 * and only fall through to the fault reporter when it is not 0x15. That
 * test is why aarch64 needs no dedicated syscall vector the way x86 needs
 * int $0x80 wired into the IDT: the cause is already in a register.
 *
 * ELR_EL1 and SPSR_EL1 are saved and restored around the call because
 * arm_syscall() is ordinary C that may itself take an exception — a page
 * fault while copying a string from the app, say. Without saving them,
 * that nested exception overwrites the return address and the app comes
 * back to the wrong instruction, which is a bug that looks like memory
 * corruption in the app rather than like a kernel one.
 *
 * Frame is 192 bytes: x0-x18 and x29 in pairs, then ELR/SPSR, then x30.
 * x19-x28 are absent deliberately — the AAPCS makes them callee-saved, so
 * arm_syscall() preserves them without help.
 */
.macro VECTOR_SYNC kind
    .balign 0x80
    sub     sp, sp, #192
    stp     x0, x1, [sp, #0]
    mrs     x0, esr_el1
    lsr     x1, x0, #26
    cmp     x1, #0x15               /* EC 0x15: SVC from AArch64 */
    b.eq    svc_dispatch
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #192

    /* Not a system call. Report it, once, on a stack of our own. */
    adrp    x0, fault_depth
    add     x0, x0, :lo12:fault_depth
    ldr     w1, [x0]
    add     w2, w1, #1
    str     w2, [x0]
    cbnz    w1, 99f
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

svc_dispatch:
    stp     x2, x3,   [sp, #16]
    stp     x4, x5,   [sp, #32]
    stp     x6, x7,   [sp, #48]
    stp     x8, x9,   [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x29, [sp, #144]
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x2, x3,   [sp, #160]
    str     x30,      [sp, #176]

    ldr     x0, [sp, #64]           /* saved x8 -> syscall number */
    ldr     x1, [sp, #0]            /* saved x0 -> arg0           */
    ldr     x2, [sp, #8]            /* saved x1 -> arg1           */
    ldr     x3, [sp, #16]           /* saved x2 -> arg2           */
    bl      arm_syscall
    str     x0, [sp, #0]            /* result is the app's new x0 */

    ldp     x2, x3,   [sp, #160]
    msr     elr_el1, x2
    msr     spsr_el1, x3
    ldr     x30,      [sp, #176]
    ldp     x18, x29, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8, x9,   [sp, #64]
    ldp     x6, x7,   [sp, #48]
    ldp     x4, x5,   [sp, #32]
    ldp     x2, x3,   [sp, #16]
    ldp     x0, x1,   [sp, #0]
    add     sp, sp, #192
    eret


.balign 2048
.global exception_vectors
.type exception_vectors, %object
exception_vectors:
    /* Current EL, SP_EL0 */
    VECTOR_SYNC 0           /* synchronous */
    VECTOR_ENTRY 1          /* IRQ         */
    VECTOR_ENTRY 2          /* FIQ         */
    VECTOR_ENTRY 3          /* SError      */

    /* Current EL, SP_EL1 — the ones that can actually fire here */
    VECTOR_SYNC 0
    VECTOR_ENTRY 1
    VECTOR_ENTRY 2
    VECTOR_ENTRY 3

    /* Lower EL, aarch64 */
    VECTOR_SYNC 0
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
