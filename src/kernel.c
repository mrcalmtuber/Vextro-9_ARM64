#include <stdint.h>
#include <stddef.h>
#include "limine.h"

/*
 * Socrates BSD 9 for ARM64 — milestone 0.
 *
 * Deliberately the smallest kernel that proves the whole boot chain
 * before a single driver is written: UEFI firmware hands off to Limine,
 * Limine sets a framebuffer through GOP and enters here at EL1 with the
 * MMU already on, and we draw something recognisable.
 *
 * The full x86_64 kernel is kept beside this as kernel_x86.c.ref and
 * comes back a subsystem at a time as each milestone lands — console and
 * interrupts next, then input, storage, network. Growing it that way
 * keeps every step bootable, instead of having nothing to look at until
 * every driver is finished at once.
 */

/* ---- Limine requests ---- */

__attribute__((used, section(".limine_reqs_start")))
static volatile uint64_t start_marker[] = LIMINE_REQUESTS_START_MARKER;

/*
 * Base revision 6, not the 3 the x86 tree asks for.
 *
 * Limine dropped support for the older revisions on aarch64 — it refuses
 * to load anything below 6 on this architecture — and the bundled header
 * is new enough to speak it, which is why the x86 side never had to move.
 */
__attribute__((used, section(".limine_reqs")))
static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_reqs")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_reqs_end")))
static volatile uint64_t end_marker[] = LIMINE_REQUESTS_END_MARKER;

/* Freestanding C runtime helpers — the compiler emits calls to these */
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *p = d; const uint8_t *q = s;
    while (n--) *p++ = *q++;
    return d;
}

/*
 * PL011 UART, QEMU `virt`.
 *
 * Hardcoded for now. The real address comes from the device tree, and a
 * parser arrives with the real-hardware milestone where a board will not
 * agree with QEMU — but on `virt` it is fixed, and it is the only way to
 * say anything at all before there is a framebuffer to say it on.
 */
#define PL011_BASE    0x09000000UL
#define PL011_DR      (*(volatile uint32_t *)(PL011_BASE + 0x00))
#define PL011_FR      (*(volatile uint32_t *)(PL011_BASE + 0x18))
#define PL011_FR_TXFF (1u << 5)                 /* transmit FIFO full */

static void uart_putc(char c) {
    if (c == '\n') uart_putc('\r');
    while (PL011_FR & PL011_FR_TXFF) { }
    PL011_DR = (uint32_t)(unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

static void uart_put_u64(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[20] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    uart_puts(buf + i);
}

static void halt_forever(void) {
    for (;;) __asm__ volatile("wfi");
}

void kmain(void) {
    uart_puts("\n[socrates/arm64] kmain reached at EL1\n");

    if (fb_request.response == NULL ||
        fb_request.response->framebuffer_count < 1) {
        uart_puts("[socrates/arm64] no framebuffer from Limine\n");
        halt_forever();
    }

    struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
    uint32_t w        = (uint32_t)fb->width;
    uint32_t h        = (uint32_t)fb->height;
    uint32_t pitch_px = (uint32_t)(fb->pitch / (fb->bpp / 8));
    volatile uint32_t *vram = (volatile uint32_t *)fb->address;

    uart_puts("[socrates/arm64] framebuffer ");
    uart_put_u64(w); uart_puts("x"); uart_put_u64(h);
    uart_puts(" bpp "); uart_put_u64(fb->bpp);
    uart_puts(" pitch "); uart_put_u64(fb->pitch);
    uart_puts("\n");

    if (memmap_request.response != NULL) {
        uint64_t usable = 0;
        for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
            struct limine_memmap_entry *e = memmap_request.response->entries[i];
            if (e->type == LIMINE_MEMMAP_USABLE) usable += e->length;
        }
        uart_puts("[socrates/arm64] usable memory ");
        uart_put_u64(usable / (1024 * 1024));
        uart_puts(" MB\n");
    }

    /*
     * The desktop's own gold-on-dark palette, as a gradient with a
     * border and a centred checker. Chosen so that a wrong pitch or a
     * swapped pixel format is obvious at a glance rather than needing a
     * probe to detect.
     */
    for (uint32_t y = 0; y < h; y++) {
        uint32_t shade = (y * 64) / (h ? h : 1);
        uint32_t bg = (shade << 16) | (shade << 8) | (shade + 16);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = bg;
            if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2)
                px = 0xD4AF37;                          /* border */
            else if (((x / 32) + (y / 32)) % 2 == 0 &&
                     x > w / 4 && x < 3 * w / 4 &&
                     y > h / 4 && y < 3 * h / 4)
                px = 0x1B2030;                          /* centred checker */
            vram[y * pitch_px + x] = px;
        }
    }

    uart_puts("[socrates/arm64] framebuffer painted - milestone 0 complete\n");
    halt_forever();
}
