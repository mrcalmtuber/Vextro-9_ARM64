#ifndef SOCRATES_H
#define SOCRATES_H

/*
 * Socrates BSD 9 — Native Hybrid Application Header (aarch64)
 *
 * Inline assembly wrappers around the kernel's syscall gateway. Include
 * this in any standalone app to reach OS services without linking
 * against kernel internals. The x86_64 original is kept alongside as
 * socrates_x86.h.ref.
 *
 * Syscall ABI:
 *   X8 = syscall number
 *   X0 = arg0   X1 = arg1   X2 = arg2
 *   Invoked via:  svc #0
 *   X0 carries the return value; all other GPRs are preserved.
 *
 * The x86 build used `int $0x80` and passed the number in RAX. Both are
 * software interrupts, but the aarch64 one is genuinely simpler: SVC has
 * a dedicated exception class in ESR_EL1, so the kernel's vector can tell
 * a system call from a fault by reading a register rather than by
 * dedicating an interrupt vector to it, and there is no descriptor table
 * to install. X8 rather than X0 for the number is the platform
 * convention, and it keeps all four argument registers free.
 *
 * The numbers themselves are unchanged, so an app's source is identical
 * on both architectures — only this header differs.
 */

typedef unsigned int      uint32_t;
typedef int               int32_t;
typedef unsigned long     uint64_t;
typedef long              int64_t;
typedef unsigned long     uintptr_t;

/* Dimensions of the canvas that sys_draw_pixel writes into. The kernel
 * clips anything outside, so these are the app's usable bounds. */
#define OS_CANVAS_W 598
#define OS_CANVAS_H 402

/* ---- Syscall 1: sys_print ----
 * Print a null-terminated string to the terminal canvas. */
static inline void os_print(const char *str) {
    register uint64_t x8 __asm__("x8") = 1;
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)str;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
}

/* ---- Syscall 2: sys_draw_pixel ----
 * Draw a single pixel at (x, y) relative to the terminal client area
 * origin (top-left of the black canvas). Color is 0xRRGGBB. */
static inline void os_draw_pixel(int x, int y, uint32_t color) {
    register uint64_t x8 __asm__("x8") = 2;
    register uint64_t x0 __asm__("x0") = (uint64_t)(unsigned)x;
    register uint64_t x1 __asm__("x1") = (uint64_t)(unsigned)y;
    register uint64_t x2 __asm__("x2") = (uint64_t)color;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
}

/* ---- Syscall 3: sys_get_mouse ----
 * Fill a 4-element int32_t buffer with the current mouse state:
 *   out[0] = screen X    out[1] = screen Y
 *   out[2] = button mask  out[3] = reserved (0) */
static inline void os_get_mouse(int32_t *out) {
    register uint64_t x8 __asm__("x8") = 3;
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)out;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
}

/* ---- Syscall 4: sys_exit ----
 * Give control back to the kernel. Returning from _start does the same
 * thing, but an app that ends inside a loop needs a way out. */
static inline void os_exit(void) {
    register uint64_t x8 __asm__("x8") = 4;
    __asm__ volatile("svc #0" :: "r"(x8) : "memory", "cc");
    for (;;) { }                    /* not reached */
}

#endif /* SOCRATES_H */
