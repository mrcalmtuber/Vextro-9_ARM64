# Socrates BSD 9 — ARM64
#
# The x86_64 build is preserved as Makefile.x86.ref. This one grows a
# milestone at a time rather than being ported wholesale, so that every
# commit produces something that boots.
#
# Milestone 0: kernel, UEFI ISO, framebuffer.

CC   := aarch64-elf-gcc
LD   := aarch64-elf-ld

# -mgeneral-regs-only is the aarch64 counterpart of the x86 build's
# -mno-sse/-mno-80387 pile, and a stricter one: it bars the compiler from
# touching the FP/SIMD registers anywhere in the kernel, so no interrupt
# handler can quietly acquire a floating-point dependency. The inference
# translation unit is the single exception and is built without it.
CFLAGS := -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
          -fno-stack-check -fno-lto -fno-pie -mgeneral-regs-only \
          -Isrc -Ikernel/include -Ibsdfmt $(EXTRA)

# Linked as a plain ET_EXEC at a fixed higher-half address, not a PIE.
#
# The x86 build passes -pie but its linker quietly produces ET_EXEC
# anyway; aarch64-elf-ld honours the flag, and the result is an ET_DYN
# whose .dynamic section has no PT_DYNAMIC program header to describe it,
# because the PHDRS block in linker.ld declares only the four PT_LOADs.
# Limine rejects that, correctly. Since the kernel is loaded at one fixed
# address either way, saying ET_EXEC outright is clearer than depending on
# a linker's fallback.
# --- Rebuild when the compiler flags change ---
#
# make compares timestamps, and a flag has none. Without this,
# `make iso EXTRA=-DSOMETHING` after an ordinary build leaves kernel.o
# untouched and produces an ISO that does not contain the thing that was
# asked for, silently.
#
# It happens at parse time, before any rule runs, and that placement is
# the load-bearing part. A recipe that deletes the objects is too late:
# macOS ships GNU Make 3.81, which stats every target while building its
# dependency graph and does not look again. It also compares
# modification times to the whole second, so a stamp rewritten in the
# same second as the previous build is not "newer" and changes nothing.
# Deleting before the graph exists sidesteps both.
BUILD_FLAGS := $(CFLAGS)
$(shell mkdir -p build; \
        [ "`cat build/.flags 2>/dev/null`" = "$(BUILD_FLAGS)" ] || \
        { printf '%s\n' "$(BUILD_FLAGS)" > build/.flags; \
          rm -f build/kernel.o build/llm.o; })

LDFLAGS := -nostdlib -static -no-pie -z text -T linker.ld

LIMINE := limine-binary
ISO    := iso_root

# UEFI firmware for QEMU's virt machine. ARM has no BIOS, so unlike the
# x86 build there is no El Torito BIOS image and no boot-sector install —
# the ISO is UEFI-only.
FIRMWARE := /opt/homebrew/share/qemu/edk2-aarch64-code.fd

RES ?= 1280x800x32

# The display device the kernel actually drives.
#
# ramfb is what the port started on and it is a dead end for anything
# interactive: EDK2's driver offers three modes topping out at 1024x768,
# and asking for more does not degrade to the next one, it drops to
# 800x600. virtio-gpu has no mode table — the kernel asks the device for
# the size it wants and gets it, which is how this reaches the same
# 1280x800 the x86 build uses by default.
#
# ramfb remains available for the headless harness, where the resolution
# does not matter and the extra device does not earn its place:
#     make run GPU=ramfb
RES_W := $(word 1,$(subst x, ,$(RES)))
RES_H := $(word 2,$(subst x, ,$(RES)))
GPU   ?= virtio
ifeq ($(GPU),ramfb)
QEMU_GPU := -device ramfb
else
QEMU_GPU := -device virtio-gpu-device,xres=$(RES_W),yres=$(RES_H)
endif

.PHONY: all iso run run-headless efi-vars clean test FORCE

# The device tree parser is pure byte manipulation with no architecture
# in it, so it can be tested on the host against blobs from real
# machines — a Raspberry Pi 4's, as shipped by the firmware, and qemu's
# own. Every address the Pi drivers are programmed with comes out of
# this file, and a wrong one produces a board that boots to a black
# screen and says nothing.
build/fdt_test: tools/fdt_test.c src/fdt.h
	@mkdir -p build
	cc -O2 -Wall -Wextra -o $@ $<

test: build/fdt_test
	./build/fdt_test tools/testdata/bcm2711-rpi-4-b.dtb tools/testdata/qemu-virt.dtb

FORCE:

all: os.iso

# --- Boot animation: video -> raw RGB565 + header ---
build/boot_anim.raw kernel/include/boot_animation.h: boot.mp4 tools/convert_video.py
	@mkdir -p build kernel/include
	python3 tools/convert_video.py boot.mp4 build/boot_anim.raw kernel/include/boot_animation.h

build/boot_animation_data.o: src/boot_animation_data.S build/boot_anim.raw
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# --- Kernel ---
build/vectors.o: src/vectors.S
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/kernel.o: src/kernel.c $(wildcard src/*.h) kernel/include/boot_animation.h build/res.stamp
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# --- The demo application, built for aarch64 and embedded ---
#
# apps/app.ld now says elf64-littleaarch64 and apps/socrates.h issues
# `svc #0` instead of `int $0x80`, but hello.c itself is unchanged: the
# syscall numbers and their argument meanings are the same on both
# architectures, so only the header that reaches them differs.
build/bsd_maker: bsdfmt/bsd_maker.c
	@mkdir -p build
	cc -O2 -o $@ $<

build/hello.bsd: apps/hello.c apps/socrates.h apps/app.ld build/bsd_maker
	@mkdir -p build
	$(CC) $(CFLAGS) -Iapps -nostdlib -c apps/hello.c -o build/hello.o
	$(LD) -nostdlib -no-pie -T apps/app.ld build/hello.o -o build/hello.elf
	aarch64-elf-objcopy -O binary build/hello.elf build/hello.bin
	./build/bsd_maker -o $@ -t build/hello.bin --text-vaddr 0x1000 --entry 0x1000

build/hello_bsd_data.o: src/hello_bsd_data.S build/hello.bsd
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# --- Inference, the one translation unit that may use FP/SIMD ---
#
# The kernel is built -mgeneral-regs-only so no code that might run in an
# exception path can quietly acquire a floating-point dependency. llm.c is
# nothing but float math, so it is the deliberate exception, exactly as it
# is the one file the x86 tree builds without -mno-sse. Porting it needed
# a single line: an SSE sqrtss became FSQRT, which aarch64 has in its base
# instruction set.
# -O3 for this translation unit, and deliberately *not* -ffast-math.
#
# Letting the compiler reassociate floating-point sums was tried, on the
# theory that a dot product cannot vectorise without it. It was worth
# about nothing — 40.8 s against 38.1 s on the same question, slightly
# the wrong side of noise — because dequantisation dominates and the
# arithmetic was never the bottleneck. `llm bench` says so directly: two
# milliseconds to expand the model's largest weight, under one to
# multiply by it.
#
# It also cost something real. With reassociation permitted, the batched
# and unbatched paths through the same maths vectorise differently and
# stop agreeing bit-for-bit, so which of the two ran changed the answer.
# Paying determinism for nothing is a bad trade.
LLM_CFLAGS := -O3 -Wall -Wextra -ffreestanding \
              -fno-stack-protector -fno-stack-check -fno-lto -fno-pie \
              -Isrc -Ikernel/include -Ibsdfmt $(EXTRA)

build/llm.o: src/llm.c $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(LLM_CFLAGS) -c $< -o $@

build/kernel: build/kernel.o build/llm.o build/vectors.o build/boot_animation_data.o build/hello_bsd_data.o linker.ld
	$(LD) $(LDFLAGS) build/kernel.o build/llm.o build/vectors.o build/boot_animation_data.o build/hello_bsd_data.o -o $@

# --- ISO root ---
build/res.stamp: FORCE
	@mkdir -p build
	@echo '$(RES)' | cmp -s - $@ || echo '$(RES)' > $@

$(ISO)/boot/kernel: build/kernel
	@mkdir -p $(ISO)/boot/limine $(ISO)/EFI/BOOT
	cp $< $@

$(ISO)/boot/limine/limine.conf: limine.conf build/res.stamp
	@mkdir -p $(ISO)/boot/limine $(ISO)/EFI/BOOT
	sed 's|^\( *resolution:\).*|\1 $(RES)|' limine.conf > $@
	cp $(LIMINE)/limine-uefi-cd.bin $(ISO)/boot/limine/
	cp $(LIMINE)/BOOTAA64.EFI       $(ISO)/EFI/BOOT/

iso: os.iso

os.iso: $(ISO)/boot/kernel $(ISO)/boot/limine/limine.conf
	xorriso -as mkisofs \
		-R -J \
		-V "SOCRATES_ARM64" \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		-o os.iso \
		$(ISO) 2>&1 | tail -3

# --- Run ---
# hvf: the guest runs on the actual CPU, which is the entire reason for an
# ARM64 build. ACCEL=tcg is kept because emulation enforces weaker memory
# ordering than Apple silicon does, so it catches missing barriers that
# hvf would hide — which matters wherever virtio descriptors are written.
ACCEL ?= hvf
ifeq ($(ACCEL),hvf)
QEMU_CPU := -cpu host
else
QEMU_CPU := -cpu cortex-a72
endif

# virtio-input over MMIO rather than PCI: the `-device` names ending in
# -device (rather than -pci) bind to the virt machine's virtio-mmio
# transports, which live in a region the kernel already maps. That keeps
# input independent of the PCIe ECAM work that storage needs.
#
# force-legacy=false is not optional. qemu's virtio-mmio proxy still
# defaults to the pre-1.0 layout, where the queue rings are one contiguous
# allocation described by a page-frame number instead of three independent
# physical addresses, and the config space is guest-endian. This driver
# implements the 1.0 layout only, so without this the devices are present,
# report version 1, and are skipped — which looks exactly like devices
# that were never attached.
#
# The tablet is absolute, which is why the x86 build's VMware backdoor
# driver has no counterpart here — the pointer tracks the host cursor
# without a grab, and there is nothing to calibrate.
# Deferred (`=`), not immediate (`:=`), and that one character matters.
#
# QEMU_NET and QEMU_DISK are defined *below* this line, so with immediate
# expansion they were both empty when it was evaluated — and `make run`
# had been starting the machine with no network adapter and no data disk
# for as long as this target existed. Nothing said so: the kernel simply
# reported no virtio-blk and no virtio-net, which looks exactly like a
# machine that was meant to have neither.
QEMU_COMMON = -M virt -m 2048 $(QEMU_CPU) -accel $(ACCEL) \
	-drive if=pflash,format=raw,unit=0,readonly=on,file=$(FIRMWARE) \
	-drive if=pflash,format=raw,unit=1,file=build/efi-vars.fd \
	$(QEMU_GPU) \
	-global virtio-mmio.force-legacy=false \
	-device virtio-keyboard-device \
	-device virtio-tablet-device \
	$(QEMU_NET) \
	$(QEMU_DISK) \
	-cdrom os.iso

# User-mode networking: no privileges, no bridge, and its fixed addressing
# (10.0.2.15/24, gateway .2, DNS .3) is exactly what netstack.h already
# defaults to, so nothing needs configuring on either side.
QEMU_NET := -netdev user,id=n0 -device virtio-net-device,netdev=n0

# The data disk, if one is present. virtio-blk over the same MMIO
# transports as input, which is what let the storage milestone skip the
# PCIe ECAM window entirely — no bus walk, no BAR sizing, no page-table
# work beyond what milestone 1 already did.
#
# Attached read-only by default. This is the 8 GB volume holding wiki.zim
# and the model, it took a long time to build, and nothing in this port
# needs to write to it yet. Pass DISK_RO=off deliberately when that
# changes.
DISK    ?= ../Socrates BSD 9/disk.img
DISK_RO ?= on

# $(wildcard) cannot be used here, and the reason is a trap worth naming:
# it splits its argument on whitespace and the default path has spaces in
# it, so `$(wildcard ../Socrates BSD 9/disk.img)` looks for three separate
# files, matches none, and quietly decides there is no disk. `make run`
# therefore booted with no volume at all — no exFAT, no saved keycode, no
# encyclopedia, no model — while the headless harness, which is Python and
# does its own test, found the same file without trouble.
#
# The path is quoted everywhere it reaches a shell for the same reason.
DISK_PRESENT := $(shell test -f "$(DISK)" && echo yes)
ifeq ($(DISK_PRESENT),yes)
QEMU_DISK := -drive if=none,id=d0,format=raw,readonly=$(DISK_RO),file="$(DISK)" \
	-device virtio-blk-device,drive=d0
else
QEMU_DISK :=
endif

# EDK2 keeps its variables in a second flash bank; it wants one the same
# size as the code image even when empty.
#
# Recreated on every run, and *not* as a target with the ISO as its
# prerequisite, which is what this used to be.
#
# EDK2 writes a boot entry naming the exact device path it booted from,
# and a stale one sends the firmware to the UEFI shell instead of the disc
# the moment anything about the device set changes — which looks exactly
# like a kernel that failed to load. So it has to be discarded when the
# ISO changes.
#
# The catch is that a `build/efi-vars.fd: os.iso` rule can never fire.
# The firmware *writes to this file while running*, so it is always newer
# than the ISO it recorded an entry for, and make therefore always
# considers it up to date. The rule was self-defeating: it looked like it
# handled the problem and could not, and `make run` eventually dropped
# into the UEFI shell with no explanation.
#
# Nothing in this store is worth keeping — there are no boot entries to
# preserve and no settings to lose — so recreating it unconditionally is
# both correct and free.
.PHONY: efi-vars
efi-vars:
	@mkdir -p build
	@dd if=/dev/zero of=build/efi-vars.fd bs=1m count=64 2>/dev/null

# Not every QEMU is built with the same display backends: Homebrew's
# macOS build ships Cocoa and no SDL, most Linux builds have GTK and SDL.
# Ask this one what it has rather than hard-coding a backend, and only
# pass sub-options the chosen backend accepts — QEMU rejects the whole
# option if it does not know one of them.
#
# zoom-to-fit matters more than it sounds: without it, Cocoa and GTK draw
# the guest at 1:1 in the middle of the full-screen window and surround it
# with black, which looks exactly like a desktop that refuses to resize.
QEMU_DISPLAY := $(shell d=$$(qemu-system-aarch64 -display help 2>/dev/null); \
  if   echo "$$d" | grep -qx sdl;   then echo 'sdl,show-cursor=off,grab-mod=lshift-lctrl-lalt'; \
  elif echo "$$d" | grep -qx gtk;   then echo 'gtk,show-cursor=off,grab-on-hover=on,zoom-to-fit=on'; \
  elif echo "$$d" | grep -qx cocoa; then echo 'cocoa,show-cursor=off,zoom-to-fit=on'; \
  else echo none; fi)

# (a shell `case` cannot be used here: the ")" in its patterns would
# close make's own $(shell ...) expansion early)
QEMU_FSKEY := $(if $(findstring cocoa,$(QEMU_DISPLAY)),Ctrl + Cmd + F,Ctrl + Alt + F)

run: os.iso efi-vars
	@echo ""
	@echo "  [TIP] Toggle full-screen on/off at any time with: $(QEMU_FSKEY)"
	@echo "  [TIP] The pointer is absolute — just move it, no click to grab."
	@echo "  [TIP] The keycode for the shared disk.img is: exfat"
	@echo ""
	qemu-system-aarch64 $(QEMU_COMMON) \
		-display $(QEMU_DISPLAY) \
		-full-screen \
		-serial stdio

# Headless, for the scripted harness: serial to a log, QMP for input and
# screenshots. Same shape as the x86 tree's tools/qemu_drive.py workflow.
run-headless: os.iso efi-vars
	qemu-system-aarch64 $(QEMU_COMMON) -display none \
		-serial file:build/serial.log \
		-qmp tcp:127.0.0.1:4480,server,nowait

clean:
	rm -rf build os.iso $(ISO)/boot $(ISO)/EFI
