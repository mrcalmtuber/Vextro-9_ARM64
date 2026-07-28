#ifndef FDT_H
#define FDT_H

#include <stdint.h>

/*
 * The flattened device tree.
 *
 * Every address this kernel uses so far is a constant taken from qemu's
 * `virt` machine: the UART at 0x09000000, the RTC at 0x09010000, the GIC
 * at 0x08000000, the virtio transports at 0x0A000000. They are correct
 * for that machine and wrong for every other one, and there is no way to
 * find out which machine you are on by guessing.
 *
 * The device tree is how ARM systems answer that. The firmware hands over
 * a blob describing what exists and where, and a kernel that reads it
 * runs on hardware it was not compiled for. That is the whole difference
 * between a kernel that boots one qemu invocation and a kernel that boots
 * a board.
 *
 * The format is deliberately simple to parse and this reads it directly
 * rather than building a tree: a linear walk over the structure block,
 * matching nodes by name and properties by name, with no allocation and
 * no index. There are perhaps a dozen lookups in the whole kernel, all at
 * start-up, so an index would cost more than it saves.
 *
 * Everything here is big-endian on the wire — the format predates the
 * architecture's preference and never changed — so every read goes
 * through be32()/be64() and there is not a single direct dereference of a
 * multi-byte field.
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

static uint64_t fdt_be64(const uint8_t *p) {
    return ((uint64_t)fdt_be32(p) << 32) | (uint64_t)fdt_be32(p + 4);
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
 * Find a property on the first node whose name matches.
 *
 * `compatible` is matched too, because that is how a device tree is meant
 * to be read: a board says its UART is compatible with "arm,pl011" even
 * when the node is called something else entirely. Matching only on node
 * names works on qemu and fails on real hardware, which is exactly the
 * kind of thing that cannot be discovered without the hardware.
 */
static const uint8_t *fdt_find_prop(const char *node_name,
                                    const char *compatible,
                                    const char *prop_name,
                                    uint32_t *out_len) {
    if (!fdt_blob) return 0;

    const uint8_t *p   = fdt_blob + fdt_struct_off;
    const uint8_t *end = fdt_blob + fdt_total;

    int depth = 0;
    int in_match = 0;           /* inside a node whose name matched */
    int match_depth = -1;
    /* Properties of a candidate node are seen before we can check its
     * "compatible", so the address is remembered and only returned once
     * the node has been confirmed. */
    const uint8_t *pending = 0;
    uint32_t pending_len = 0;
    int compat_ok = (compatible == 0);

    while (p + 4 <= end) {
        uint32_t tok = fdt_be32(p);
        p += 4;

        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            uint32_t n = 0;
            while (p + n < end && p[n]) n++;
            p += (n + 4) & ~3u;         /* name, NUL, padded to 4 */
            depth++;

            if (!in_match && (node_name == 0 || fdt_name_is(name, node_name))) {
                in_match = 1;
                match_depth = depth;
                pending = 0;
                compat_ok = (compatible == 0);
            }
            continue;
        }

        if (tok == FDT_END_NODE) {
            if (in_match && depth == match_depth) {
                if (compat_ok && pending) {
                    if (out_len) *out_len = pending_len;
                    return pending;
                }
                in_match = 0;
                match_depth = -1;
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

            /*
             * Only the matched node's *own* properties count.
             *
             * A match stays live across the node's children so that the
             * closing token can be recognised, but a child's properties
             * belong to the child. Without this check, the GIC's nested
             * v2m frame overwrites the distributor address with its own,
             * and the kernel programs an MSI window believing it is an
             * interrupt controller — which is exactly what happened the
             * first time this ran against a real device tree.
             */
            if (!in_match || depth != match_depth) continue;
            const char *pname = fdt_string(nameoff);

            if (compatible && fdt_streq(pname, "compatible")) {
                /* A NUL-separated list; any entry may match. */
                uint32_t i = 0;
                while (i < len) {
                    if (fdt_streq((const char *)val + i, compatible)) {
                        compat_ok = 1;
                        break;
                    }
                    while (i < len && val[i]) i++;
                    i++;
                }
            }
            if (fdt_streq(pname, prop_name)) {
                pending = val;
                pending_len = len;
            }
            continue;
        }

        if (tok == FDT_NOP) continue;
        if (tok == FDT_END) break;
        break;                          /* unknown token: stop rather than guess */
    }
    return 0;
}

/*
 * The base address of a device, from its `reg` property.
 *
 * `reg` is a list of (address, size) pairs whose cell counts come from
 * the parent's #address-cells and #size-cells. On every 64-bit ARM system
 * of interest both are 2, so this reads 64-bit values and says so rather
 * than pretending to a generality it has not been tested against.
 */
static uint64_t fdt_reg_base(const char *node_name, const char *compatible) {
    uint32_t len = 0;
    const uint8_t *reg = fdt_find_prop(node_name, compatible, "reg", &len);
    if (!reg || len < 8) return 0;
    return fdt_be64(reg);
}

#endif /* FDT_H */
