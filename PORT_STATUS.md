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
| M4 | Network — virtio-net, TCP/IP, HTTP, browser | done |
| M5 | Userland — aarch64 `.bsd`, `svc #0` | done |
| M6 | Model and Wikipedia | done (see the speed caveat) |
| M7 | Real hardware | device tree done and tested against a real Pi blob; Pi drivers written, untested on hardware (see below) |

Boot animation plays, the login screen renders and animates at a locked
**60 fps**, typing fills the password field, the pointer tracks the host
cursor absolutely, the 8 GB exFAT volume mounts and lists, and the network
stack pings its gateway and fetches a real page over HTTP. An aarch64
`.bsd` application loads, runs, and calls back into the kernel through
`svc #0` — and the x86_64 build of the same program is refused.

The desktop is wired up: logging in brings up the window manager, dock,
menu bar and clock, and the browser fetches and renders a real page.

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

## hvf works, and here is what was wrong

For most of this port qemu's hvf backend aborted the guest with
`Assertion failed: (isv)` about half a second into any kernel that was
executing instructions. It reproduced with the render loop replaced by an
empty spin, with the framebuffer never touched, with no display device
attached, and under every GIC variant — but never in a guest parked in
`wfi`, and never under tcg.

The cause was one bound in `mmio_map_init()`. The kernel identity-maps
RAM as Normal memory, and the loop ran to a hardcoded 4 GB while the
machine only had 2 GB. **Normal memory is speculatable** — the core may
fetch from it unasked, because the architecture guarantees that reading
backed memory has no side effects. Mapping a gigabyte that nothing backs
voids that guarantee, and the core eventually speculates into a physical
address no device answers for. hvf sees a stage-2 fault whose instruction
syndrome cannot be decoded, because there was no instruction — and
asserts.

That explains every symptom: it needs execution (speculation does not
happen in `wfi`), it is unrelated to what the code does (the access is not
in the code), and tcg never shows it (emulation does not speculate).

The bound now comes from Limine's memory map. Verified clean at `-m 1024`,
`-m 2048` and `-m 4096` — a constant would only ever have been right for
one of them.

**What it bought**, same measurement under each accelerator:

| | tcg | hvf |
|---|---:|---:|
| Prompt eval to first token | 41,374 ms | **3,359 ms** |
| 397 MB of weights resident | 1,079 ms | **426 ms** |
| Boot to kernel | ~200 s | ~5 s |

`EXTRA=-DBAREMIN` builds a kernel that spins and does nothing else. That
is what proved the fault was in this code rather than in the handover —
it runs indefinitely under hvf, so bisecting forward from it was possible.

## Porting the desktop

desktop.h and its four dependents (term.h, browser.h, apps.h, store.h)
compiled unchanged except for three things that are not really UI at all:

  - `outb(0x64, 0xFE)` to reboot and two ACPI port pokes to power off
    became `machine_reset()` and `machine_poweroff()`, which are PSCI
    calls — an interface the architecture defines, rather than a keyboard
    controller and a chipset being used as power buttons.

  - `igpu.h` became an inert declaration. There is no Intel blitter on
    `virt`, but term.h's `gpu` diagnostics read the struct, and forking a
    1,400-line file to avoid one struct would cost more than it saves.
    The commands now report "no integrated GPU on this machine", which is
    true. The CPU renderer in gfx.h is what draws on both trees anyway.

  - A handful of x86 driver facts term.h reports — PS/2 packet length,
    the e1000 link-status register — became the virtio equivalents behind
    the same names.

Build with `EXTRA=-DAUTO_BROWSER` to have the desktop open the browser on
a fixed URL at login. Clicking a dock icon over QMP means knowing where
the dock put it, which depends on the panel size and item count, so a
coordinate-clicking test really tests the dock layout and fails for
reasons unrelated to the browser.

## Display: the kernel drives virtio-gpu

`src/vtgpu.h` talks to virtio-gpu over MMIO and creates its own scanout,
so the display no longer comes from the firmware at all. Verified at
1440x900 with no ramfb attached — `-device virtio-gpu-device,xres=,yres=`
and the kernel asks the device what the display is.

That exists because the firmware path has a ceiling. EDK2's ramfb driver
offers three modes topping out at 1024x768, and asking for more does not
degrade to the next one — Limine finds no match and falls back to
800x600, so requesting a larger screen produced a smaller one.
`-device virtio-gpu-pci` is no way out either: this EDK2 build produces
no GOP for it, so Limine reports no framebuffer at all.

Limine's framebuffer is still the fallback, so a ramfb-only machine boots
to a desktop unchanged. Presenting costs two commands per frame
(TRANSFER_TO_HOST_2D then RESOURCE_FLUSH) because a virtio-gpu resource
is not on screen the moment it is written, unlike a linear framebuffer.

## M7: real hardware

### The device tree — done, and tested against a real board's blob

`src/fdt.h` parses the flattened device tree, and every device address
the kernel uses comes from it instead of a constant. The qemu `virt`
values remain as defaults, so a machine that passes no tree boots exactly
as before.

Two things it had to learn to handle a Raspberry Pi's tree, both of which
the first version got away with on qemu and would have got wrong on a
board:

**Cell counts are per-node, not universal.** `reg` is a list of address
and size values whose widths come from the *parent's* `#address-cells`
and `#size-cells`. On `virt` both are the 64-bit default, so reading a
fixed 8-byte address happened to work. Under a Pi's `soc` node an address
is one cell, and reading eight bytes returns the address with the size
welded onto the end of it.

**Bus addresses are not CPU addresses.** A Pi describes its peripherals at
`0x7e000000`, which is what the VideoCore sees; the ARM core reaches the
identical registers at `0xfe000000`. The `ranges` property on each
intervening node is the translation, and a kernel that ignores it programs
a device that is not there. The Pi 4 has three such buses — `soc`,
`emmc2bus` and `scb` — with different cell counts each.

Lookups are also by `compatible` alone now, never by node name. A Pi's
UART node is `serial@7e201000` and its interrupt controller is
`interrupt-controller@40041000`; neither string should have to appear in
a kernel.

`make test` runs the parser on the host against two blobs: the
`bcm2711-rpi-4-b.dtb` the Raspberry Pi firmware actually ships, and one
dumped from qemu. The expected addresses are what `dtc` prints for those
nodes worked through the `ranges` translation by hand, not this parser's
own output written down afterwards. All seventeen checks pass, including
`0x7e201000 -> 0xfe201000`, `0x40041000 -> 0xff841000`,
`0x7e340000 -> 0xfe340000` and `0x7d580000 -> 0xfd580000`.

Getting the tree at all under qemu needs `-M virt,acpi=off` — EDK2 hands
the OS either ACPI tables or a device tree and deliberately removes the
DTB when ACPI is present. `DTB=1 tools/arm_run.py` sets it, and with it
the kernel identifies the machine, extracts every address from the tree,
builds its device mapping from those, and boots to the desktop.

### The memory map is now derived, not assumed

The mapper used to hard-code one window — `0x08000000` to `0x0C000000`,
which is where qemu keeps its devices — plus whole gigabytes of RAM up to
a bound taken from the firmware. A Pi has nothing at `0x08000000` and its
peripherals are at `0xFC000000`, sharing a gigabyte with real memory, so
neither half of that survives.

`mmio_map_init()` now classifies each gigabyte independently from two
lists filled before it runs: what the firmware reports as backed, and what
the device tree says holds registers. A gigabyte that is entirely RAM and
holds no device becomes one 1 GB block; anything else is described 2 MB at
a time, with device blocks taking precedence over RAM and everything else
left **invalid**. That last part is the point — it is the general form of
the fix for the hvf abort described below, and it means a Pi's
peripherals cannot come up mapped as cacheable memory.

### The framebuffer is not device memory

The Pi framebuffer is registered with the mapper so the CPU can reach it
at all, and the first version registered it as an ordinary device
window. That would have worked and been unusably slow.

Device memory on this architecture means Device-nGnRnE: no gathering, no
reordering, no early acknowledgement. That is exactly right for a
control register, where writing one has a side effect and the order
matters, and ruinous for a framebuffer, where it turns every pixel into
its own bus transaction the core waits on — three quarters of a million
of them per 1024x768 frame.

A framebuffer wants the opposite: stores gathered and reordered freely,
since nothing reads it until the frame is done, but never held in a
cache the display controller cannot see. That is Normal Non-cacheable,
and there was no MAIR index for it — Limine leaves 0 and 1 as Normal
write-back and 2 through 7 as zero. The kernel now programs index 3
itself, after checking the slot really is unused, because MAIR is a live
register and a mapping already made against index 3 would change meaning
underneath it. If the slot is taken, the framebuffer falls back to
Normal write-back and says so.

`mmio_report()` prints which type is in use, along with how many
gigabytes needed 2 MB granularity and how much reported RAM that
granularity had to leave out. The last number should be zero; it is on
both machines this has run on.

### The Pi drivers — written, and not tested on hardware

Three drivers, in the order they matter:

- **`src/mbox.h`** — the VideoCore property mailbox. A Pi is a VideoCore
  computer with an ARM core attached: the firmware owns the clocks, the
  power rails and the display, and the ARM side asks for things. There is
  no register that sets the SD card clock, only a message requesting it.
- **`src/pifb.h`** — a framebuffer from the firmware, eight tags in one
  message. This is the display path that depends on no UEFI graphics
  protocol at all.
- **`src/emmc.h`** — the SD card, which on a Pi *is* the disk. SDHCI,
  polled, with the full CMD0/CMD8/ACMD41/CMD2/CMD3/CMD9/CMD7/ACMD6
  bring-up and both CSD versions for capacity.
- **`src/genet.h`** — the Pi 4's on-SoC gigabit MAC. Descriptor rings in
  the controller's own SRAM, buffers in host memory with explicit cache
  maintenance, since DMA here is not coherent the way x86's is.

They are wired in behind the same interfaces everything else uses:
`src/blk.h` dispatches storage between virtio-blk and the SD card, and
`e1000.h` dispatches the network between virtio-net and GENET. Both
backends can never coexist — a Pi has no virtio and `virt` has no GENET —
so the choice is made once at boot.

**None of this has run on a Raspberry Pi.** There is not one here. What
*is* verified is everything that decides whether those drivers are even
reachable: the tree parses, the addresses resolve to the right physical
numbers, the board is identified, and the mapping is built from what the
firmware reports rather than from constants. The register sequences
themselves come from the SD physical layer and SDHCI specifications and
from the published GENET layout, and the places where a value is
inference rather than specification are marked as such in the source
rather than left to look like fact.

The known gap worth naming: `genet.h` asserts a gigabit link rather than
negotiating one, because it has no MDIO code and never talks to the PHY.
On a Pi 4 the PHY auto-negotiates by itself and the MAC-to-PHY link is
fixed, so that is right in the ordinary case — but against a 100 Mbit
switch the two ends would disagree, and the symptom would be a link that
passes no traffic rather than a slow one. MDIO is the next thing that
file needs, and it needs hardware to write against.

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

**Four functions in this codebase return 0 for success while their
neighbours return 1 for success.** `zim_find`, `exf_lookup`,
`llm_fpu_selftest` and `llm_load_begin` have all been misread that way
during this port, and each time the symptom was a component that appeared
broken while working perfectly — a passing FPU reported as FAIL, a model
that "would not load". Check the definition, not the shape of the name.

**Loading a model is two calls, not one.** `llm_load()` parses the GGUF
metadata; `llm_load_begin()`/`llm_load_step()` then stream the weights.
Calling begin() first fails with "no model loaded", which reads like a
missing file rather than a missing step.

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
