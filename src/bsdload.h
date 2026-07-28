#ifndef BSDLOAD_H
#define BSDLOAD_H

#include <stdint.h>
#include "arm.h"
#include "bsd_format.h"

/*
 * Loading and running a .bsd image on aarch64.
 *
 * The format needed one change to come across: its fourth magic byte is
 * the architecture, so aarch64 images say 0xAA where x86_64 images say
 * 0x64. Nothing else in the container moved. That was deliberate — adding
 * a machine *field* would have shifted every offset after it, so an old
 * kernel handed a new image would read the entry point out of the middle
 * of something else. Spending the byte that already exists means each
 * kernel rejects the other's images with the check it already has.
 *
 * Images are position independent by contract: aarch64 reaches its own
 * data through adrp/add pairs, which are PC-relative exactly as the
 * x86_64 build's RIP-relative addressing was, so an image can be placed
 * anywhere provided the distance between its text and data segments is
 * preserved. This loader keeps that distance and places the whole span at
 * the base of the application window.
 */

/* The window's backing store. 2 MB-aligned so a single block descriptor
 * can map it; see mmio_map_init(). */
static uint8_t app_region[APP_WINDOW_SIZE]
    __attribute__((aligned(APP_WINDOW_SIZE)));

static const char *app_err = "";

/* Set when the running app asks to stop, or faults out. */
static volatile int app_running = 0;

static void app_region_init(void) {
    app_region_phys = virt_to_phys(app_region);
}

/*
 * Copy an image into the window and jump to it.
 *
 * Returns 0 when the app ran to completion. The app shares this kernel's
 * exception level and stack: there is no separate address space to switch
 * to and nothing to schedule against, which matches how the x86 build
 * runs .bsd images through int $0x80 rather than a ring transition. EL0
 * is where this goes when there is a reason for it — a second process, or
 * an app that should not be able to reach kernel memory — and the syscall
 * path already handles being entered from a lower EL.
 */
static int bsd_exec(const uint8_t *file, uint64_t len) {
    app_err = "";

    if (len < sizeof(bsd_header_t)) { app_err = "file smaller than header"; return -1; }
    const bsd_header_t *h = (const bsd_header_t *)file;

    const char *why = bsd_validate(h, len);
    if (why) { app_err = why; return -1; }

    uint64_t span = bsd_image_span(h);
    if (span > APP_WINDOW_SIZE) { app_err = "image larger than the app window"; return -1; }
    if (!app_region_phys)       { app_err = "app window not mapped"; return -1; }

    uint8_t *base = (uint8_t *)(uintptr_t)APP_WINDOW_VA;
    for (uint64_t i = 0; i < span; i++) base[i] = 0;

    /* Offsets are kept relative to text_vaddr, so the text/data distance
     * the image was linked with survives the move. */
    uint64_t toff = h->text_vaddr - h->text_vaddr;      /* zero, by definition */
    for (uint64_t i = 0; i < h->text_size; i++)
        base[toff + i] = file[h->text_off + i];

    if (h->data_size) {
        uint64_t doff = h->data_vaddr - h->text_vaddr;
        for (uint64_t i = 0; i < h->data_size; i++)
            base[doff + i] = file[h->data_off + i];
    }

    uint64_t entry = APP_WINDOW_VA + (h->entry - h->text_vaddr);

    /*
     * The window was just written through the kernel's cacheable data
     * mapping and is about to be executed through a different one. On
     * aarch64 the instruction and data caches are not coherent with each
     * other, so without cleaning the data side to the point of unification
     * and invalidating the instruction side, the CPU can fetch whatever
     * happened to be in that memory before — which on a first run is
     * zeroes, and on a second run is the *previous* app. x86 does this in
     * hardware, which is why the other tree's loader has no equivalent.
     */
    for (uint64_t a = APP_WINDOW_VA; a < APP_WINDOW_VA + span; a += 64)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    DSB();
    for (uint64_t a = APP_WINDOW_VA; a < APP_WINDOW_VA + span; a += 64)
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    DSB();
    ISB();

    app_running = 1;
    ((void (*)(void))(uintptr_t)entry)();
    app_running = 0;
    return 0;
}

#endif /* BSDLOAD_H */
