# ARM64 port status

Socrates BSD 9 on `qemu-system-aarch64 -M virt`. The x86_64 tree is
untouched and unaffected.

## Where it is

| | Milestone | State |
|---|---|---|
| M0 | Repo, toolchain, first pixel | done |
| M1 | Console, exceptions, timer, render loop | done |
| M2 | Input — keyboard and pointer | done |
| M3 | Storage — virtio-blk, exFAT | done (PCIe ECAM not needed, see below) |
| M4 | Network — virtio-net, TCP/IP, HTTP | done (browser UI not wired) |
| M5 | Userland — aarch64 `.bsd`, `svc #0` | done (Agora UI not wired) |
| M6 | Model and Wikipedia | not started |
| M7 | Real hardware — Raspberry Pi | not started |

Boot animation plays, the login screen renders and animates at a locked
**60 fps**, typing fills the password field, the pointer tracks the host
cursor absolutely, the 8 GB exFAT volume mounts and lists, and the network
stack pings its gateway and fetches a real page over HTTP. An aarch64
`.bsd` application loads, runs, and calls back into the kernel through
`svc #0` — and the x86_64 build of the same program is refused.

Every device — keyboard, tablet, disk, NIC — is virtio over MMIO, sharing
one virtqueue implementation. That is why three milestones landed without
the PCIe ECAM window the plan budgeted for.

## How to run it

```sh
make all
make run                      # tcg; see "hvf" below
python3 tools/arm_run.py 400 tcg     # headless, serial + register dump
python3 tools/arm_shot.py 300        # capture a frame to build/screen.ppm
python3 tools/arm_input_test.py 330  # drive real input through QMP
```

`make run` picks up `../Socrates BSD 9/disk.img` automatically if present,
**read-only** — that volume holds `wiki.zim` and the model, and nothing in
this port needs to write to it yet. `DISK=` and `DISK_RO=off` override.

TCG needs roughly 250 s to get through EDK2 and the boot animation before
the login screen appears. That is emulation speed, not a bug.

## The one open problem: hvf

Running on the actual CPU is the whole reason for an ARM64 build, and it
does not work yet. qemu 11.0.3's hvf backend aborts this guest:

```
Assertion failed: (isv), function hvf_handle_exception, file hvf.c, line 2268
```

The abort is **not conditional on anything the kernel does**. It
reproduces with:

- the render loop replaced by an empty counted spin
- the framebuffer never touched
- no display device attached at all
- `gic-version=2`, `gic-version=3` and `highmem=off` alike
- both timers disarmed, all four DAIF bits masked, both ends of the GIC
  quiet

A guest parked in `wfi` never triggers it; a guest executing instructions
does, after about half a second. The same binary runs indefinitely under
tcg at a steady 60 fps.

Not yet tried: attaching a debugger to qemu itself to read the guest PC at
the abort (lldb cannot drive an hvf guest without the right entitlements),
or a newer qemu.

## Things that cost a lot to learn

**Limine maps no device memory.** It hands over with the MMU on and its own
tables installed, covering the kernel, RAM and the framebuffer — and
nothing else. A hardcoded `*(volatile uint32_t *)0x09000000` for the UART
is an access with no translation behind it. It faults *before*
`exceptions_init()` can run, because reaching `exceptions_init()` requires
getting past the UART, so `VBAR_EL1` is still zero and the CPU vectors to
address 0 and loops there forever. From outside: a machine that is up,
burning CPU, and silent. This presented for days as a "render loop stall".

**A dying qemu never flushes a `-serial file:` chardev.** The last lines
before a crash — the only ones that matter — are lost, and the truncated
tail is not where execution stopped. Two wrong diagnoses came from
trusting it. `tools/arm_run.py` reads serial over a socket instead, and
asks the CPU for its registers when the guest goes quiet. That register
dump is what finally identified the fault above.

**Print-bisection does not work when the failure eats the log.** Halting
at a numbered checkpoint does: if the machine is still running, execution
reached it, and nothing has to survive a buffer. `CHK(n)` in `kernel.c`
with `make EXTRA=-DHALT_AT=n`.

**`-global virtio-mmio.force-legacy=false` is required.** qemu's virtio-mmio
proxy still defaults to the pre-1.0 layout. Without the flag the devices
are present, report version 1, and are skipped by a 1.0-only driver —
indistinguishable from devices that were never attached. The kernel dumps
every populated slot with its device ID and version so the two cases can
be told apart.

**netstack.h's ping counter is gated on `ping_active`.** Calling
`icmp_send_echo` directly sends a perfectly good echo request whose reply
arrives, parses, and is then dropped — indistinguishable from a gateway
that never answered. Drive the stack's own state, not just its wire
functions.

**aarch64 caches are not coherent between instruction and data sides.**
Code written through a data mapping and then executed must be cleaned to
the point of unification (`dc cvau`) and the instruction side invalidated
(`ic ivau`) in between. x86 does this in hardware, so the other tree's
loader has no equivalent and nothing hints that it is needed. Skipping it
runs whatever was in that memory before — zeroes on the first load, the
*previous* app on the second.

**Adding a device invalidates `build/efi-vars.fd`.** EDK2 stores the PCI
path it booted from; a stale entry sends it to the UEFI shell, which looks
exactly like a kernel that failed to load.

## What the plan got wrong, usefully

Storage was supposed to require PCIe ECAM, a bus walk, BAR sizing and the
ARM64 page-table rewrite. It required none of them: virtio-blk binds to the
same virtio-mmio transports as input, at addresses M1 already maps. The
ECAM work is still ahead for anything PCI-only, but it is no longer between
here and a working filesystem.

The exception trampoline was called the highest risk in the port. It was
real but small — the expensive part was not writing the vectors, it was
that a fault handler on a bad stack, or one that can re-enter itself,
reports nothing at all. Both are fixed in `vectors.S`: a private stack in
`.bss` and a depth counter.
