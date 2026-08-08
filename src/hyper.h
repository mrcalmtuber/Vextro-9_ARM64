#ifndef VEXTRO_HYPER_H
#define VEXTRO_HYPER_H

#include <stdint.h>

/*
 * There is no hypervisor on this port, and this file says exactly why.
 *
 * The x86 tree runs a guest on AMD-V: VMCB, nested page tables, VMRUN,
 * and an exit handler for CPUID, I/O, MSR access and hypercalls. None of
 * that has an ARM counterpart at EL1 -- stage-2 translation, HCR_EL2 and
 * VTTBR_EL2 all live at EL2, and a kernel at EL1 cannot reach them. So
 * what decides this is a single measured fact, and kmain measures it:
 * the exception level the firmware actually handed over at.
 *
 * What that measurement found, on this machine:
 *
 *   -M virt                      no EL2 on the board at all
 *   -M virt,virtualization=on    EL2 exists, but Apple's HVF refuses:
 *                                "HVF does not support providing
 *                                 Virtualization extensions to the
 *                                 guest CPU"
 *   ...with -accel tcg           Limine panics on an ARMv8.0 core:
 *                                "Booting at EL2 without VHE support is
 *                                 not supported"
 *   ...and -cpu max              boots, and kmain reports EL2
 *
 * So the port can reach EL2, in emulation, on a core with the
 * Virtualization Host Extensions. That is a real result rather than a
 * guess, and it is the prerequisite an ARM hypervisor would build on --
 * but it is not one, and this file does not pretend otherwise. Chamber
 * reads these fields and reports the level it is running at.
 *
 * The struct is declared inert rather than the window being compiled
 * out, for the same reason src/igpu.h keeps its shape: it lets
 * desktop.h, store.h and chamber.h stay byte-identical across the two
 * trees instead of forking them over one absent feature.
 */

/* Set in kmain from CurrentEL, not assumed. */
extern int arm_current_el;

#define HV_SCREEN_COLS  80
#define HV_SCREEN_ROWS  25
#define HV_LOG_MAX 64

typedef struct {
    uint64_t code;
    uint64_t info1;
    uint64_t rip;
} hv_exit_t;

static struct {
    int      supported;
    int      npt;
    int      nrip;
    int      enabled;
    int      running;
    int      finished;
    uint32_t asid_max;
    uint32_t revision;
    const char *status;

    uint64_t vmcb_phys, npt_phys;

    uint32_t vmruns;
    uint32_t n_cpuid, n_io, n_msr, n_hlt, n_hypercall, n_npf, n_intr, n_other;
    uint64_t last_code;
    uint32_t io_port, io_value;
    uint32_t hypercall_arg, hypercall_ret;

    hv_exit_t log[HV_LOG_MAX];
    int       log_n;
    int       log_head;
} hv = { .status = "no hypervisor on this port" };

static void hv_init(void) {
    hv.supported = 0;
    hv.enabled = 0;
    /* Say which of the two situations this is: a kernel at EL1 cannot
     * host a guest no matter what it does, while one at EL2 could. */
    hv.status = (arm_current_el >= 2)
        ? "at EL2: virtualisation is reachable, but not implemented here"
        : "at EL1: EL2 is required to host a guest, and this is not it";
}

static void hv_reset(void) { hv_init(); }
static int  hv_step(void) { return -1; }
static void hv_run(int budget) { (void)budget; }

static const char *hv_exit_name(uint64_t code) { (void)code; return "none"; }

static void hv_hex(uint64_t v, char *out) {
    static const char digits[] = "0123456789ABCDEF";
    int hi = 15;
    while (hi > 0 && ((v >> (hi * 4)) & 0xF) == 0) hi--;
    int n = 0;
    for (int i = hi; i >= 0; i--) out[n++] = digits[(v >> (i * 4)) & 0xF];
    out[n] = '\0';
}

static int hv_screen_row(int row, char *out, int max) {
    (void)row; (void)max;
    out[0] = '\0';
    return 0;
}

static uint32_t hv_scratch32(uint32_t off) { (void)off; return 0; }

#endif /* VEXTRO_HYPER_H */
