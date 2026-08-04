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

/*
 * The window's backing store.
 *
 * mmio_map_init() maps this with a single 2 MB block descriptor, and a
 * block descriptor's output address has no low bits: the *physical*
 * address must be 2 MB aligned or the mapping is malformed.
 *
 * Asking the compiler for that alignment is the obvious way to get it and
 * the wrong one. It aligns the whole .bss section to 2 MB, so the linker
 * gives that segment a file offset of 0x200000 -- and Limine checks
 * p_offset against the size of the file it is loading. It only ever
 * worked because 18 MB of boot animation used to pad the kernel past that
 * point. The moment the animation became code instead of data, a
 * half-megabyte kernel had a segment beginning a megabyte and a half
 * beyond its own end, and the loader refused it:
 *
 *     PANIC: elf: p_offset + p_filesz exceeds file size
 *
 * So the alignment is taken by hand, out of a buffer twice the size, and
 * taken on the physical address rather than the virtual one because that
 * is the one the descriptor holds. .bss occupies nothing in the file,
 * which is what makes the spare 2 MB free, and the section is left at
 * page alignment where it belongs.
 */
static uint8_t  app_region_store[2 * APP_WINDOW_SIZE];
static uint8_t *app_region = app_region_store;

/* ===== import resolution =====
 *
 * Set by the kernel once everything it exports has been declared —
 * bsdload.h is included before ttf.h, so the table cannot be built here.
 */
static const bsd_export_t *bsd_exports = 0;

static void bsd_set_exports(const bsd_export_t *table) { bsd_exports = table; }

static int bsd_name_eq(const char *a, const char *b) {
    for (int i = 0; i < BSD_IMPORT_NAMELEN; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;                       /* full-width name, no terminator */
}

/*
 * Find the import table in a loaded data segment and fill it in.
 *
 * `data` points at the image's data as mapped, `len` bounds it. The tag
 * is searched for on eight-byte boundaries because the structure is
 * eight-aligned by construction; scanning bytewise would find a tag that
 * happened to straddle two unrelated values.
 *
 * Returns the number of names resolved, or -1 if the table is malformed.
 * An image with no table at all is not an error — that is every image
 * that existed before this — so it returns zero.
 */
static int bsd_resolve_imports(uint8_t *data, uint64_t len) {
    if (!data || len < sizeof(bsd_import_hdr_t)) return 0;

    uint64_t limit = len - sizeof(bsd_import_hdr_t);
    for (uint64_t off = 0; off <= limit; off += 8) {
        bsd_import_hdr_t *hdr = (bsd_import_hdr_t *)(data + off);
        if (hdr->magic != BSD_IMPORT_MAGIC) continue;

        if (hdr->count == 0 || hdr->count > BSD_IMPORT_MAX) return -1;

        /* The entries must lie wholly inside the segment. A count that
         * overruns is the one way this can be turned into a write past
         * the image, so it is checked before anything is written. */
        uint64_t need = sizeof(bsd_import_hdr_t) +
                        (uint64_t)hdr->count * sizeof(bsd_import_t);
        if (need > len - off) return -1;

        bsd_import_t *e = (bsd_import_t *)(data + off + sizeof(bsd_import_hdr_t));
        int found = 0;
        for (uint32_t i = 0; i < hdr->count; i++) {
            e[i].addr = 0;
            if (!bsd_exports) continue;
            for (int k = 0; bsd_exports[k].name; k++) {
                if (bsd_name_eq(e[i].name, bsd_exports[k].name)) {
                    e[i].addr = bsd_exports[k].addr;
                    found++;
                    break;
                }
            }
        }
        return found;
    }
    return 0;                       /* no table: an ordinary image */
}


static const char *app_err = "";

/* Set when the running app asks to stop, or faults out. */
static volatile int app_running = 0;

static void app_region_init(void) {
    const uint64_t base = virt_to_phys(app_region_store);
    const uint64_t want = (base + (APP_WINDOW_SIZE - 1))
                          & ~(uint64_t)(APP_WINDOW_SIZE - 1);
    app_region      = app_region_store + (want - base);
    app_region_phys = want;
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

    /*
     * Fill in whatever the image asked to borrow from the kernel.
     *
     * The whole loaded span is searched, not just the data segment. An
     * application with nothing mutable has no data segment at all — the
     * demo here is one, `data_size` is zero and its import table lands in
     * .rodata, which the application linker folds into text. Since the
     * table has to be found by searching anyway, searching all of it
     * removes a dependency on which section a compiler chose.
     *
     * That this can write into the text image is specific to how this
     * kernel maps applications: one window, readable, writable and
     * executable together, because the .bsd format carries no relocations
     * and needs no separate protections. The POSIX loader in bsdfmt does
     * enforce W^X per segment, so an image meant to run under both must
     * put its table somewhere writable there.
     *
     * Before the cache maintenance below, deliberately: these writes go
     * through the data mapping and the clean-to-unification that follows
     * is what makes them visible to the fetch side.
     */
    int nimp = bsd_resolve_imports(base, span);
    if (nimp < 0) {
        serial_puts("[socrates/arm64] .bsd: malformed import table\n");
        return -1;
    }
    if (nimp > 0) {
        serial_puts("[socrates/arm64] .bsd: resolved ");
        serial_put_u64((uint64_t)nimp);
        serial_puts(" imported symbols\n");
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
