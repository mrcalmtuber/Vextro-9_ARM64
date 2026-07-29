# Socrates BSD 9 — ARM64

A bare-metal **aarch64** operating system built from scratch — custom kernel, TrueType font rasterizer, window manager, TCP/IP stack, web browser, language model, offline Wikipedia, boot animation and desktop UI, all without a libc or external OS dependencies.

This is a standalone port of [Socrates BSD 9](https://github.com/mrcalmtuber/socrates-bsd-9), which targets x86_64. The two repositories are independent: the x86_64 tree is untouched and continues to work.

**Two thirds of the codebase came across unchanged.** The compression, filesystem, font, archive, browser and inference code was written integer-only and reads every multi-byte value one byte at a time, so there was not a single unaligned cast or endianness assumption to fix. What had to be written was the machine layer — and most of that turned out to be *deletion*, because on this machine every device speaks virtio.

### What replaced what

| Concern | x86_64 | ARM64 |
|---|---|---|
| Boot | Limine (BIOS + UEFI) | Limine (UEFI only), base revision 6 |
| Interrupts | 8259 PIC, 256-entry IDT | `VBAR_EL1`, 16 x 128-byte code slots |
| Timer | PIT 8254 + IRQ0 | Architected generic timer, no interrupt at all |
| Serial | COM1 @ 0x3F8 | PL011 @ 0x09000000 |
| RTC | CMOS ports 0x70/0x71 | PL031 |
| Keyboard | PS/2 controller + ISR | virtio-input |
| Pointer | PS/2 + VMware backdoor for absolute | virtio-input tablet — **absolute natively** |
| Storage | ATA PIO, LBA48 | virtio-blk |
| Network | Intel E1000 | virtio-net |
| Display | Limine framebuffer | **virtio-gpu, driven by the kernel** |
| Syscalls | `int $0x80` | `svc #0` |
| Reboot / power off | PS/2 reset pulse, ACPI port pokes | PSCI `SYSTEM_RESET` / `SYSTEM_OFF` |
| GPU accel | Intel Gen9 blitter | dropped — no counterpart on this machine |
| Audio | AC'97 | dropped |

`vmmouse.h` and the PS/2 packet-length negotiation are **deleted, not ported**: the virtio tablet reports absolute positions, so there is nothing to calibrate and no capture to escape from.

Five virtio devices — keyboard, tablet, disk, network, GPU — share one virtqueue implementation. That is why the PCIe ECAM window the port plan budgeted for was never needed: everything binds to the `virt` machine's MMIO transports.

### Hardware discovery

The kernel reads the flattened device tree, so every device address comes from the firmware rather than from a constant. Devices are matched by `compatible` alone, never by node name — a Raspberry Pi's UART node is `serial@7e201000` and its interrupt controller is `interrupt-controller@40041000`, and neither string should have to appear in a kernel. The qemu `virt` addresses remain as fallbacks for machines that pass no tree.

Two things the parser has to get right that only show up off qemu. **Cell counts are per-node**: `reg` widths come from the parent's `#address-cells`, which is the 64-bit default on `virt` and one cell under a Pi's `soc`. And **bus addresses are not CPU addresses**: a Pi describes its peripherals at `0x7e000000` because that is what the VideoCore sees, while the ARM core reaches the same registers at `0xfe000000`. The `ranges` property is the translation, and ignoring it programs a device that is not there.

`make test` runs the parser on the host against the `bcm2711-rpi-4-b.dtb` the Raspberry Pi firmware actually ships and against a blob dumped from qemu, checking seventeen addresses worked through `ranges` by hand.

The memory map follows from the same source: `mmio_map_init()` classifies each gigabyte from what the firmware reports as backed and what the tree says holds registers, mapping RAM as Normal, devices as Device, and everything else **not at all**.

### Raspberry Pi

Written, and **not yet run on hardware** — there is no Pi here to test against, and the source says so where it matters:

- **`src/mbox.h`** — the VideoCore property mailbox. A Pi is a VideoCore computer with an ARM core attached: the firmware owns the clocks, the power rails and the display, so there is no register that sets the SD clock, only a message asking for it
- **`src/pifb.h`** — a framebuffer straight from the firmware, eight tags in one message; the display path that needs no UEFI graphics protocol at all
- **`src/emmc.h`** — the SD card, which on a Pi *is* the disk. SDHCI, polled, full CMD0/CMD8/ACMD41/CMD2/CMD3/CMD9/CMD7/ACMD6 bring-up, both CSD versions
- **`src/genet.h`** — the Pi 4's on-SoC gigabit MAC, with descriptor rings in the controller's own SRAM and explicit cache maintenance for the buffers, since DMA here is not coherent the way x86's is

They sit behind the interfaces everything else already uses: `blk.h` dispatches storage between virtio-blk and the SD card, `e1000.h` dispatches the network between virtio-net and GENET. The two backends never coexist, so the choice is made once at boot.

---

## Features

### Desktop
- **Window manager** — z-ordered, click-to-focus windows with titlebar drag, close buttons, drop shadows and spawn animations
- **Menubar** — working Socrates / Apps menus (About, Restart, Shut Down, app launchers), live clock + date, network status indicator
- **Dock** — pictogram icons with hover tooltips, running indicators, bottom/left/right placement, adjustable size
- **Wallpaper themes** — five gradient themes with the dragon emblem, switchable live from Settings
- **Absolute pointer** — the cursor tracks the host's straight away, with no click-to-grab and no capture to escape from. On x86 that needed a second driver talking to a VMware backdoor port, because PS/2 can only report *relative* motion; here the virtio tablet reports positions natively, so the backdoor and the packet-length negotiation are deleted rather than ported
- **Scroll wheel** — arrives as an ordinary `REL_WHEEL` event, and is routed to whichever window has focus: terminal scrollback, browser pages, store shelves, article lists
- **Resolution independent** — the desktop lays itself out around whatever the display reports, and windows clamp to the usable area so they fit on any panel. The kernel drives virtio-gpu directly, so it is not limited to the firmware's mode table

### Filesystem
- **exFAT** on a virtio-blk disk (`disk.img`) — the same image the x86_64 build uses, byte for byte
- exFAT rather than FAT32 because **FAT32 caps a single file at 4 GB**; exFAT carries 64-bit sizes, so an offline archive of any size fits. The default volume is 8 GB and **sparse**, costing only the few megabytes actually used
- Full read/write driver: allocation-bitmap free space, contiguous (`NoFatChain`) *and* chained files, checksummed directory entry sets, free-form UTF-16 names — no more 8.3
- **`fs_read_range()`** reads a window out of a file, so nothing has to fit in a buffer. `peek <file> <offset> [n]` exposes it from the shell
- virtio-blk driver behind the same five-function interface the filesystems already called, so `exfat.h` and `fat32.h` compile untouched; MBR partitions are probed, so a FAT32 boot partition can sit beside the exFAT system one
- One `fs_*` layer decides which filesystem is mounted; no app talks to a driver directly
- `disk.img` is a standard image: **mount it on your host** (macOS: `hdiutil attach -imagekey diskimage-class=CRawDiskImage disk.img`) to exchange files with the OS — including dropping in a multi-gigabyte archive
- Login keycode persists on disk (`keycode.sys`) — delete it to re-register
- A 16 KB read-ahead window and a cached FAT sector: small sequential reads used to cost one drive command per 512 bytes, and walking a fragmented file's cluster chain re-started from its first cluster on every call

### Terminal
- Crisp monospace grid rendering (8x8 bitmap font) with a blinking block cursor
- Command history (Up/Down), line editing (Left/Right/Home/End/Del), 240-line scrollback (PgUp/PgDn)
- Working directory (`cd` / `pwd`, shown in the prompt) and output redirection: `ls > list.txt`, `echo hi >> notes.txt`
- Commands: `help` `clear` `ls` `cat` `cd` `pwd` `rm` `mkdir` `cp` `df` `run` `echo` `date` `uptime` `mem` `mouse` `net` `arp` `ping` `dns` `fetch` `store` `img` `peek` `zim` `open` `history` `reboot` `shutdown`

### Networking
- **Full TCP/IP stack** — IPv4, ICMP (ping), UDP, DNS resolver, polled TCP client, async HTTP/1.0 client with redirects
- **virtio-net driver** — sharing the virtqueue layer with input, storage and the GPU. The E1000 driver would have *compiled* unchanged, and been quietly wrong: its descriptor-then-doorbell sequences carry no memory barriers, because x86 orders stores and the code was written where that is free
- Works against real websites through QEMU user networking (`ping`, `dns`, `fetch`, and the browser)

### Browser
- Loads real `http://` pages over the in-kernel TCP stack (no TLS — bare metal has no secrets)
- HTML-to-text renderer: headings, paragraphs, lists, `<pre>`, entities, word wrap
- **Clickable links**, Back/Reload, editable address bar, scrollbar, status bar with load progress
- Internal pages: `socrates://home`, `socrates://help`, `socrates://about`, `socrates://file/<name>`
- Offline encyclopedia articles at `zim://<title>`, with internal links resolved back to archive entries

### App Store
- **Agora** — a working package manager with a storefront: browse a catalog, **Install**, **Open**, **Remove**
- Installing is a real operation — the payload is validated, copied to `/apps/<id>.bsd` on the system volume and recorded in the registry at `/apps/apps.db`, so **installed apps survive a reboot**
- Installed apps join the **dock** (after a separator, with their store icon) and the **Apps menu**, and launch straight into their own window
- Two package sources, one catalog:
  - the repository seeded on disk at `/store/pkg` (also carried in the initrd, so the storefront works on an ISO-only boot)
  - a **network repository fetched over the in-kernel TCP/IP stack** — `Refresh` pulls an index of `key:value` blocks, and each payload is downloaded with its own HTTP GET
- Every package is a **`.bsd` executable** (see below), validated field by field before a byte reaches the disk
- Ships five apps, all integer-only: **Mandelbrot** (16.16 fixed-point fractal), **Orbit** (five-body Newtonian integrator), **Game of Life** (149×100 torus over a heat map), **Plasma** (sine table built at runtime by a magic-circle oscillator) and **Voronoi** (28-site partition, network-only — it exists purely to exercise the download path)
- Also driveable from the shell: `store list` `store install <id>` `store remove <id>` `store run <id>` `store refresh` `store repo [url]`

### Apps
- **Files** — ramdisk explorer; double-click text files to view them in the browser
- **Goldsmith** — mouse paint app with palette, brush sizes and eraser
- **Monolith** — live system monitor (uptime, memory, pointer, TCP state, ARP cache)
- **Matrix** — falling glyph rain demo
- **hello** — userland ELF64 app rendering into its own window via `int 0x80` syscalls

### Graphics
- Portable base: the firmware (GOP/VESA) linear framebuffer — works on any GPU vendor, any VM
- **Intel Gen9/9.5 iGPU driver** (Skylake → Comet Lake): maps BAR0 GTTMMADR, programs a private GGTT window, brings up the BCS blitter ring in legacy submission mode, and executes `XY_COLOR_BLT` packets — with a CPU-verified self-test at boot
- Register/command encodings adapted from the Linux i915 driver; probe-then-bail structure after SerenityOS — display modesetting is deliberately left to firmware
- If no supported iGPU is present the OS silently stays on the CPU renderer (`gpu` in the terminal shows which path is live; `gpu test` blits to the visible framebuffer on real hardware)
- **GPU hang capture** (i915 error-state style): when a submission's breadcrumb never lands, the driver latches EIR/ESR, the per-engine IPEHR/IPEIR (the exact command header that broke the pipeline, decoded by name — e.g. a malformed `XY_COLOR_BLT`), ACTHD, INSTDONE, `RING_FAULT_REG` GGTT faults, the HWS page and the ring contents around the parse point — then attempts a `GDRST` blitter-domain engine reset, falling back to CPU rendering after repeated hangs. `gpu error` prints the full report; `gpu decode <hex>` decodes any command dword

### Wikipedia offline — the ZIM reader
Browse a complete offline encyclopedia on bare metal.

- **`src/zim.h`** — reads Kiwix ZIM archives straight off the exFAT volume, a window at a time. Nothing is loaded whole: only one decompressed cluster is held in memory, and consecutive articles usually share it
- Lookup is a **binary search over the archive's sorted path list** — about twenty reads across 400k entries, and only about twenty-five across a full dump's 19.7 million. It barely slows as the archive grows
- **`src/zstd.h`** — a complete Zstandard decompressor (FSE/tANS, Huffman, sequence reconstruction with repeat offsets), written from RFC 8878. ZIM has defaulted to zstd since 2021, so nothing opens without it. xz/LZMA2 clusters work too, via `lzma.h`
- **Wikipedia app** — type a title, get live prefix results with redirects marked; Enter or a click opens the article
- **Ask it questions** — the bubble in the header switches to a chat panel that retrieves an article and answers from it. The model loads by itself: drop a Qwen2 GGUF at `/qwen2.gguf` and it streams into memory in the background while the desktop stays live, with progress in the header. Nothing to type
- Articles render through the existing browser at `zim://<title>`, and **internal links are clickable**: relative hrefs, `../A/` namespace prefixes and percent-encoding are all resolved back to archive entries, so you can browse from article to article with Back working
- `zim open|info|main|ls|find|get` drive the same reader from the shell

Verified on a real 937 MB Simple English archive (399,853 entries, 3,711 clusters): lookups land on the same clusters and byte counts in the kernel as in a host-side reference run.

### Compressed images — the `.sci` format
Full-colour pictures stored compressed and decompressed when opened.

- **`src/lzma.h`** — a complete LZMA / LZMA2 / xz decompressor: range decoder, the full probability model, the LZMA2 chunk layer and enough of the xz container to walk its blocks. It decodes straight into the caller's buffer and uses that buffer as its dictionary window, so there is no separate 32 MB ring
- **`src/sci.h`** — the container. Each row gets a PNG-style prediction filter (None/Sub/Up/Average/Paeth, chosen per row by lowest absolute sum), and the filtered plane is one LZMA stream. Header carries dimensions, channels, filter mode, codec and both sizes, and is validated before anything is decoded
- **`tools/mkimg.py`** — PNG or PPM in, `.sci` out. It decodes PNG itself with Python's own `zlib`, so the build needs no Pillow and no opencv
- **Photos** — a gallery app: `.sci` files found in `/pics` and `/` down the left, the decoded picture on the right, click to toggle fit / 1:1. The status bar shows the real ratio. Double-clicking a `.sci` in Files opens it here, and `img <file>` works from the shell

The two shipped samples land at **623 KB → 12 KB (1%)** for the emblem and **450 KB → 241 KB (53%)** for a deliberately noisy interference field — the latter still smaller than its PNG.

### The `.bsd` executable format
A custom x86_64 container that every app store package uses, living in `bsdfmt/`.

- **`bsd_format.h`** — an 80-byte header: 4-byte magic `'B' 'S' 'D' 0x64`, a `uint32_t` version that pads the magic to 8 bytes so no `uint64_t` straddles an alignment boundary, then 64-bit entry point, text offset/size, data offset/size, load addresses, bss size and flags. `bsd_validate()` is a freestanding, overflow-safe checker shared verbatim by the host tools, the kernel loader and the store's download path
- **`bsd_maker.c`** — packs raw x86_64 machine code (`-t code.bin -d data.bin -b bss`), or repacks a linked ELF64's two `PT_LOAD` segments (`-e prog.elf`), which is how the store's packages are built
- **`bsd_run.c`** — POSIX loader: one anonymous `mmap()` for the image, copy the segments in, then `mprotect()` the text `PROT_READ|PROT_EXEC` and the data `PROT_READ|PROT_WRITE`. Neither is ever writable and executable at once, so **W^X holds** and the NX bit never fires on the entry jump. The entry address is cast to `long (*)(long)` and called
- Segments are page aligned and never share a page — otherwise `mprotect()`, which works at page granularity, could not give them different protections. `bsd_run` detects hosts with pages coarser than 4 KB (16 KB on Apple silicon) and refuses rather than silently mapping RWX
- Images carry no relocations, so a loader may place them anywhere as long as it preserves the text↔data distance

```sh
cd bsdfmt && make demo      # build the tools, pack two examples, run them
./bsd_run -n prog.bsd       # map and protect without calling (works on any host)
```

The kernel's loader (`src/desktop.h`) dispatches on magic: `.bsd` for store packages, ELF64 for `hello`, and it rejects anything else.

### Core
- **Custom TrueType rasterizer** — integer-only engine rendering Comic Neue (OFL) with 8x8 supersampled AA; no floats, no GPU. Baselines and glyph origins are snapped to whole pixels (left fractional, a 13px baseline lands on an exact half-pixel and fringes every letter), and each glyph's coverage mask is cached per size rather than re-rasterized on every frame — which is what makes the finer sampling affordable
- **Boot animation** — full-color video playback via raw RGB565 frames embedded at link time; any key skips it
- **Login screen** — first-boot keycode registration (persisted to disk) with melt animation on bad passwords
- **Machine layer** — `VBAR_EL1` vector table, the architected generic timer (a monotonic system register, so the render loop paces itself with no interrupt controller at all), PL011 console, PL031 RTC, PSCI for reset and power-off
- **Its own page tables** — Limine hands over with the MMU on and maps the kernel, RAM and the framebuffer, but *no device registers*. The kernel builds a TTBR0 covering exactly the device blocks that exist, before its first line of output
- **virtio stack** — one MMIO transport and one split virtqueue implementation, shared by keyboard, tablet, disk, network and GPU. No PCIe bus walk, no BAR sizing, no ECAM window
- **Device tree** — device addresses read from the firmware's FDT, matched by `compatible`, with the qemu `virt` addresses as fallbacks
- **Syscall interface** — `svc #0` with a save/restore trampoline. AArch64 gives SVC its own exception class in `ESR_EL1`, so a system call is told apart from a fault by reading a register rather than by dedicating a vector to it
- **Limine bootloader** — UEFI only (ARM has no BIOS), base revision 6

---

## Demo

▶️ **[Watch the boot animation](boot.mp4)** — a full-color RGB565 video decoded and played by the kernel itself at boot (no GPU, no codec library).

> Sequence: boot animation → login screen → windowed desktop.

<!-- Screenshots: drop PNGs in docs/ and embed, e.g. ![desktop](docs/desktop.png) -->

---

## Requirements

| Tool | Notes |
|------|-------|
| `aarch64-elf-gcc` | Cross-compiler targeting bare-metal ELF |
| `aarch64-elf-ld` | Matching cross-linker |
| `xorriso` | ISO creation |
| `python3` + `opencv-python` + `numpy` | Boot animation conversion (only if `boot.mp4` changes) |
| `qemu-system-aarch64` | Running in a VM |
| `edk2-aarch64-code.fd` | UEFI firmware — ARM has no BIOS, so the ISO is UEFI-only |

On macOS, install the cross-toolchain with Homebrew:

```sh
brew install aarch64-elf-gcc aarch64-elf-binutils xorriso qemu
pip3 install opencv-python numpy
```

The firmware ships with QEMU at `/opt/homebrew/share/qemu/edk2-aarch64-code.fd`.

---

## Build

```sh
make
```

This will:
1. Compile the kernel, `llm.c` (the one translation unit allowed FP/SIMD),
   the exception vectors and the userland `hello` app — linked to aarch64
   ELF64 and repacked into a `.bsd` image by `bsdfmt/bsd_maker`
2. Convert `boot.mp4` to raw RGB565 frames and embed them
3. Assemble the bootable UEFI ISO at `os.iso`

`.bsd` images declare their architecture in the fourth magic byte — `0xAA`
here, `0x64` on x86_64 — so each kernel refuses the other's binaries at
the first check rather than executing them as instructions they are not.

---

## Run

```sh
make run
```

QEMU's `virt` machine with 2 GB of RAM, virtio input, network and GPU, and
`../Socrates BSD 9/disk.img` attached **read-only** if it exists. Override
with `DISK=` and `DISK_RO=off`.

### Accelerator

`ACCEL` defaults to `hvf` — the guest runs on the actual CPU, which is the
entire reason for an ARM64 build. `ACCEL=tcg` still works and is worth
running occasionally: emulation enforces weaker memory ordering than Apple
silicon does, so it catches missing barriers that hvf would hide.

The difference, same measurement under each:

| | tcg | hvf |
|---|---:|---:|
| Prompt eval to first token | 41,374 ms | **3,359 ms** |
| 397 MB of weights resident | 1,079 ms | **426 ms** |
| Boot to kernel | ~200 s | ~5 s |

### Display

The kernel drives virtio-gpu itself, so the resolution is whatever the
device reports rather than whatever the firmware's mode table contains:

```sh
qemu-system-aarch64 -M virt -device virtio-gpu-device,xres=1440,yres=900 ...
```

Verified at 1440x900 with no `ramfb` attached at all. Limine's framebuffer
remains a fallback, but EDK2's ramfb driver offers three modes topping out
at 1024x768 — and asking for more does not degrade to the next one, it
falls back to 800x600.

### Headless harness

```sh
python3 tools/arm_run.py 400 tcg      # serial over a socket + register dump
python3 tools/arm_shot.py 300         # capture a frame
python3 tools/arm_input_test.py 330   # drive real keyboard/pointer input
```

`arm_run.py` reads serial over a socket rather than `-serial file:`,
because a dying qemu never flushes a file chardev and the lost tail is not
where execution stopped. When the guest goes quiet it asks the CPU for its
registers, which is what identified the one genuinely hard bug in this
port.

---

## Project Layout

```
src/            Kernel source
  kernel.c      Entry point, render loop, login flow, syscall stubs
  desktop.h     Window manager, menubar, dock, wallpaper, ELF loader
  term.h        Terminal (commands, history, scrollback)
  browser.h     Browser (HTML renderer, navigation, links)
  apps.h        Files / Settings / Photos / Wikipedia + RAG chat / About
  llm.h llm.c   Local transformer inference (the one FP/SIMD translation unit)
  store.h       Agora app store (catalog, installer, registry, storefront)
  lzma.h        LZMA / LZMA2 / xz decompressor (images + ZIM clusters)
  zstd.h        Zstandard decompressor (modern ZIM clusters)
  zim.h         ZIM archive reader (offline Wikipedia)
  sci.h         .sci compressed image format + decoder
  gfx.h         Theme palette + drawing primitives + monospace text
  netstack.h    IPv4 / ICMP / UDP / DNS / TCP / HTTP
  exfat.h       exFAT driver (read/write, 64-bit sizes, range reads)
  fat32.h       FAT32 driver (fallback)
  ttf.h         TrueType rasterizer

  --- the aarch64 machine layer ---
  arm.h         System registers, PL011, PL031, generic timer, page
                tables, PSCI reset/power-off, device-tree probe
  fdt.h         Flattened device tree parser (match by `compatible`)
  vectors.S     VBAR_EL1 table + the `svc #0` save/restore trampoline
  virtio.h      virtio-mmio transport + split virtqueue (shared by all five)
  vtinput.h     Keyboard + absolute tablet     vtgpu.h  virtio-gpu scanout
  blk.h         Block layer: virtio-blk or the SD card, one sector view
  ata.h         virtio-blk behind the ATA name the filesystems call
  emmc.h        Raspberry Pi SD card (SDHCI, polled)
  e1000.h       Network dispatch behind the NIC name netstack.h calls
  genet.h       Broadcom GENET v5 — the Pi 4's gigabit MAC
  mbox.h        VideoCore property mailbox    pifb.h  firmware framebuffer
  keyboard.h    Ring buffer + scancode tables (no ISR, no port I/O)
  igpu.h        Inert: no integrated GPU on this machine
  bsdload.h     .bsd loader — executable window, I-cache maintenance

  *_x86.h.ref   The x86_64 originals, kept for reference
apps/           Userland app source + seed files for the disk
  store/        App store packages + packages.txt (repository metadata)
  pics/         Sample PNGs, converted to .sci at build time
bsdfmt/         The .bsd executable format
  bsd_format.h  Header layout + shared validator
  bsd_maker.c   Builder (raw machine code, or repack an ELF64)
  bsd_run.c     POSIX loader: mmap + mprotect W^X + call
  bsd.ld        Linker script: page-separated text and data segments
tools/          arm_run.py (boot + serial socket + register dump)
                arm_shot.py (frame capture), arm_input_test.py (QMP input)
                mkexfat.py, mkfat32.py, video converter, mkimg.py
limine-binary/  Pre-built Limine bootloader binaries
Makefile        Top-level build system
```

---

## Status

Milestones M0-M7 are complete and verified under emulation:

| | Milestone | Verified by |
|---|---|---|
| M0 | Toolchain, UEFI ISO, first pixel | boots and paints |
| M1 | Console, exceptions, timer, render loop | 60 fps, locked |
| M2 | Keyboard + absolute pointer | host `(0.75, 0.70)` arrives as `599,419` |
| M3 | virtio-blk + exFAT | 8 GB volume mounts; real root listing |
| M4 | virtio-net + TCP/IP + browser | ICMP 4/4; `HTTP 200`, page renders |
| M5 | aarch64 `.bsd` + `svc #0` | app runs; x86_64 image refused |
| M6 | Model + offline Wikipedia | ZIM v6, 399,853 entries; model answers " Paris" |
| M7 | Device tree | UART/RTC/GIC/virtio all read from the tree |

**One thing is honestly incomplete:**

- **The Raspberry Pi drivers are not written** — mailbox framebuffer, SD,
  USB. There is no Pi here to test against, and shipping untested drivers
  for hardware nobody has run is not worth doing. The device tree work is
  the part that had to come first regardless, and the display already does
  not depend on firmware.

See `PORT_STATUS.md` for the things that cost the most to find.

---

## Download

Pre-built ISOs are available under [Releases](../../releases).

---

## License

Source code is released under the [Apache License 2.0](LICENSE).
Comic Neue font is licensed under the [SIL Open Font License 1.1](assets/OFL.txt).
Limine bootloader is [BSD 2-Clause](https://github.com/limine-bootloader/limine).
