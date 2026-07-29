#ifndef FDT_H
#define FDT_H

#include <stdint.h>

/*
 * The flattened device tree.
 *
 * Every address this kernel started with was a constant taken from
 * qemu's `virt` machine: the UART at 0x09000000, the RTC at 0x09010000,
 * the GIC at 0x08000000, the virtio transports at 0x0A000000. They are
 * correct for that machine and wrong for every other one, and there is
 * no way to find out which machine you are on by guessing.
 *
 * The device tree is how ARM systems answer that. The firmware hands
 * over a blob describing what exists and where, and a kernel that reads
 * it runs on hardware it was not compiled for. That is the whole
 * difference between a kernel that boots one qemu invocation and a
 * kernel that boots a board.
 *
 * The format is deliberately simple to parse and this reads it directly
 * rather than building a tree: a linear walk over the structure block,
 * matching nodes by name and properties by name, with no allocation and
 * no index. There are perhaps a dozen lookups in the whole kernel, all
 * at start-up, so an index would cost more than it saves.
 *
 * Everything here is big-endian on the wire — the format predates the
 * architecture's preference and never changed — so every read goes
 * through be32()/be64() and there is not a single direct dereference of
 * a multi-byte field.
 *
 * Two things this file used to get away with and no longer can, both
 * discovered the moment a Raspberry Pi's tree was put through it:
 *
 *   Cell counts are per-node, not universal. `reg` is a list of address
 *   and size values whose widths come from the *parent's*
 *   #address-cells and #size-cells. On qemu virt both are the 64-bit
 *   default, so reading a fixed 8-byte address happened to work. Under
 *   a Pi's `soc` node an address is one cell, and reading eight bytes
 *   returns the address with the size welded onto the end of it.
 *
 *   Bus addresses are not CPU addresses. A Pi describes its peripherals
 *   at 0x7e000000, which is the address the VideoCore sees; the ARM core
 *   reaches the same registers at 0xfe000000. The `ranges` property on
 *   each intervening node is the translation, and a kernel that ignores
 *   it programs a device that is not there.
 */

#define FDT_MAGIC       0xD00DFEEDu

#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static const uint8_t *fdt_blob = 0;
static uint32_t fdt_struct_off = 0, fdt_strings_off = 0, fdt_total = 0;

/* Wire order is big-endian, and the blob may sit at any alignment, so
 * every multi-byte value is assembled a byte at a time. */
static uint32_t fdt_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* An address or size of `cells` 32-bit words, big-endian, most
 * significant first.  One and two are the only widths that occur; more
 * would not fit a 64-bit result and are refused by the callers. */
static uint64_t fdt_cells(const uint8_t *p, uint32_t cells) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < cells; i++)
        v = (v << 32) | fdt_be32(p + i * 4);
    return v;
}

static int fdt_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Node names carry a unit address: "pl011@9000000". A match on the part
 * before the '@' is what callers want, since the address is the thing
 * being looked up. */
static int fdt_name_is(const char *node, const char *want) {
    while (*want) {
        if (*node != *want) return 0;
        node++; want++;
    }
    return *node == '\0' || *node == '@';
}

static int fdt_init(const void *blob) {
    fdt_blob = 0;
    if (!blob) return 0;
    const uint8_t *p = (const uint8_t *)blob;
    if (fdt_be32(p) != FDT_MAGIC) return 0;

    fdt_struct_off  = fdt_be32(p + 8);
    fdt_strings_off = fdt_be32(p + 12);
    fdt_total       = fdt_be32(p + 4);
    fdt_blob = p;
    return 1;
}

static const char *fdt_string(uint32_t off) {
    return (const char *)(fdt_blob + fdt_strings_off + off);
}

/*
 * What each node on the path down declares about its children.
 *
 * The walk is linear but the tree is not, so these form a stack: at any
 * point during the walk, entries 1..depth are exactly the ancestors of
 * the node being looked at.  That is what makes it possible to translate
 * an address without a second pass — everything needed to do it is
 * already on the stack when the matching node is reached.
 */
#define FDT_MAX_DEPTH 24

struct fdt_level {
    uint32_t ac, sc;              /* cells this node's children use     */
    const uint8_t *ranges;        /* child -> parent address mapping    */
    uint32_t ranges_len;
    int      has_ranges;
};

static struct fdt_level fdt_path[FDT_MAX_DEPTH];

/*
 * Translate an address out of a child bus and up to the CPU's view.
 *
 * `depth` is the depth of the node the address came from, so the first
 * translation to apply is its parent's — entry depth-1.  A node with no
 * `ranges` at all is treated as identity, which is what a tree that
 * simply omits it means in practice; an empty `ranges` says identity
 * outright.
 *
 * The walk stops at depth 2 rather than 1 because the root node's
 * address space is the CPU's: a device sitting directly under the root
 * is already where it says it is, and there is nothing above the root to
 * translate into.
 */
static uint64_t fdt_translate(uint64_t addr, int depth) {
    for (int d = depth - 1; d >= 2; d--) {
        const struct fdt_level *lv = &fdt_path[d];
        if (!lv->has_ranges || lv->ranges_len == 0) continue;   /* identity */

        uint32_t child_ac  = lv->ac;
        uint32_t parent_ac = fdt_path[d - 1].ac;
        uint32_t sz_cells  = lv->sc;
        uint32_t entry     = (child_ac + parent_ac + sz_cells) * 4;
        if (entry == 0) continue;

        int mapped = 0;
        for (uint32_t off = 0; off + entry <= lv->ranges_len; off += entry) {
            const uint8_t *e = lv->ranges + off;
            uint64_t cbase = fdt_cells(e, child_ac);
            uint64_t pbase = fdt_cells(e + child_ac * 4, parent_ac);
            uint64_t size  = fdt_cells(e + (child_ac + parent_ac) * 4, sz_cells);
            if (addr >= cbase && addr < cbase + size) {
                addr = pbase + (addr - cbase);
                mapped = 1;
                break;
            }
        }
        /* An address the parent does not claim is not reachable from it.
         * Saying so beats inventing a translation. */
        if (!mapped) return 0;
    }
    return addr;
}

/*
 * Find a property on the first node whose name matches.
 *
 * `compatible` is matched too, because that is how a device tree is
 * meant to be read: a board says its UART is compatible with "arm,pl011"
 * even when the node is called something else entirely. Matching only on
 * node names works on qemu and fails on real hardware, which is exactly
 * the kind of thing that cannot be discovered without the hardware.
 *
 * `out_depth`, when asked for, receives the matched node's depth so the
 * caller can translate an address it found there.
 */
static const uint8_t *fdt_find_prop_at(const char *node_name,
                                       const char *compatible,
                                       const char *prop_name,
                                       uint32_t *out_len,
                                       int *out_depth) {
    if (!fdt_blob) return 0;

    const uint8_t *p   = fdt_blob + fdt_struct_off;
    const uint8_t *end = fdt_blob + fdt_total;

    int depth = 0;

    /*
     * Every node on the current path is a candidate, tracked separately.
     *
     * A single "am I inside a match" flag cannot work, for two reasons
     * that both showed up against real trees. A node's own properties
     * are only decidable at its closing token — "compatible" may not
     * have been seen yet when "reg" goes past — so the answer has to be
     * held until then; and with a flag, a child's properties overwrite
     * the parent's, which is how the GIC's nested v2m frame once
     * replaced the distributor address with its own MSI window.
     *
     * Per-depth state is both simpler and exactly right: properties
     * always precede child nodes in the structure block, so by the time
     * a child is entered its parent's entry is complete and untouched.
     *
     * It is also what allows `node_name` to be null, which is how a
     * device is found by `compatible` alone — and that is the normal
     * case on a board, where nothing is called what qemu calls it. A
     * Raspberry Pi's UART node is "serial@7e201000", its interrupt
     * controller is "interrupt-controller@40041000", and neither name
     * appears anywhere in this kernel.
     */
    struct {
        int active;
        int compat_ok;
        const uint8_t *pending;
        uint32_t pending_len;
    } cand[FDT_MAX_DEPTH];
    for (int i = 0; i < FDT_MAX_DEPTH; i++) cand[i].active = 0;

    /* The spec's defaults, used by any node that declares neither. */
    fdt_path[0].ac = 2;
    fdt_path[0].sc = 1;
    fdt_path[0].ranges = 0;
    fdt_path[0].ranges_len = 0;
    fdt_path[0].has_ranges = 0;

    while (p + 4 <= end) {
        uint32_t tok = fdt_be32(p);
        p += 4;

        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            uint32_t n = 0;
            while (p + n < end && p[n]) n++;
            p += (n + 4) & ~3u;         /* name, NUL, padded to 4 */
            depth++;

            if (depth < FDT_MAX_DEPTH) {
                /* Inherit until the node says otherwise: a bus that does
                 * not redeclare cell counts uses its parent's. */
                fdt_path[depth].ac = fdt_path[depth - 1].ac;
                fdt_path[depth].sc = fdt_path[depth - 1].sc;
                fdt_path[depth].ranges = 0;
                fdt_path[depth].ranges_len = 0;
                fdt_path[depth].has_ranges = 0;

                cand[depth].active =
                    (node_name == 0 || fdt_name_is(name, node_name));
                cand[depth].compat_ok  = (compatible == 0);
                cand[depth].pending    = 0;
                cand[depth].pending_len = 0;
            }
            continue;
        }

        if (tok == FDT_END_NODE) {
            if (depth > 0 && depth < FDT_MAX_DEPTH &&
                cand[depth].active && cand[depth].compat_ok &&
                cand[depth].pending) {
                if (out_len) *out_len = cand[depth].pending_len;
                if (out_depth) *out_depth = depth;
                return cand[depth].pending;
            }
            depth--;
            continue;
        }

        if (tok == FDT_PROP) {
            if (p + 8 > end) break;
            uint32_t len     = fdt_be32(p);
            uint32_t nameoff = fdt_be32(p + 4);
            const uint8_t *val = p + 8;
            p = val + ((len + 3) & ~3u);
            const char *pname = fdt_string(nameoff);

            /* Bus geometry is recorded for every node on the way past,
             * matched or not, because the translation needs the whole
             * ancestor chain and not just the node that was asked for. */
            if (depth > 0 && depth < FDT_MAX_DEPTH) {
                if (fdt_streq(pname, "#address-cells") && len >= 4)
                    fdt_path[depth].ac = fdt_be32(val);
                else if (fdt_streq(pname, "#size-cells") && len >= 4)
                    fdt_path[depth].sc = fdt_be32(val);
                else if (fdt_streq(pname, "ranges")) {
                    fdt_path[depth].ranges = val;
                    fdt_path[depth].ranges_len = len;
                    fdt_path[depth].has_ranges = 1;
                }
            }

            if (depth <= 0 || depth >= FDT_MAX_DEPTH || !cand[depth].active)
                continue;

            if (compatible && fdt_streq(pname, "compatible")) {
                /* A NUL-separated list; any entry may match. */
                uint32_t i = 0;
                while (i < len) {
                    if (fdt_streq((const char *)val + i, compatible)) {
                        cand[depth].compat_ok = 1;
                        break;
                    }
                    while (i < len && val[i]) i++;
                    i++;
                }
            }
            if (fdt_streq(pname, prop_name)) {
                cand[depth].pending = val;
                cand[depth].pending_len = len;
            }
            continue;
        }

        if (tok == FDT_NOP) continue;
        if (tok == FDT_END) break;
        break;                          /* unknown token: stop rather than guess */
    }
    return 0;
}

static const uint8_t *fdt_find_prop(const char *node_name,
                                    const char *compatible,
                                    const char *prop_name,
                                    uint32_t *out_len) {
    return fdt_find_prop_at(node_name, compatible, prop_name, out_len, 0);
}

/*
 * Where a device's registers are, as the CPU must address them.
 *
 * `reg` is a list of (address, size) pairs in the parent bus's cell
 * widths, and the address is in the parent bus's space — so both the
 * decoding and the translation come from the ancestor chain rather than
 * from an assumption. `index` selects which pair, since a controller
 * with several register banks lists them all.
 */
static uint64_t fdt_reg_index(const char *node_name, const char *compatible,
                              uint32_t index, uint64_t *out_size) {
    uint32_t len = 0;
    int depth = 0;
    const uint8_t *reg = fdt_find_prop_at(node_name, compatible, "reg", &len, &depth);
    if (!reg || depth < 1) return 0;

    uint32_t ac = fdt_path[depth - 1].ac;
    uint32_t sc = fdt_path[depth - 1].sc;
    if (ac == 0 || ac > 2 || sc > 2) return 0;

    uint32_t entry = (ac + sc) * 4;
    if (entry == 0 || (index + 1) * entry > len) return 0;

    const uint8_t *e = reg + index * entry;
    uint64_t addr = fdt_cells(e, ac);
    if (out_size) *out_size = sc ? fdt_cells(e + ac * 4, sc) : 0;

    return fdt_translate(addr, depth);
}

static uint64_t fdt_reg_base(const char *node_name, const char *compatible) {
    return fdt_reg_index(node_name, compatible, 0, 0);
}

/* Does the root node claim compatibility with `what`?  This is how a
 * board is identified: "brcm,bcm2711" is a Raspberry Pi 4 and nothing
 * else, whatever the vendor called the model. */
static int fdt_board_is(const char *what) {
    return fdt_find_prop(0, what, "compatible", 0) != 0;
}

#endif /* FDT_H */
