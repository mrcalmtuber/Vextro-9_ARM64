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
LDFLAGS := -nostdlib -static -no-pie -z text -T linker.ld

LIMINE := limine-binary
ISO    := iso_root

# UEFI firmware for QEMU's virt machine. ARM has no BIOS, so unlike the
# x86 build there is no El Torito BIOS image and no boot-sector install —
# the ISO is UEFI-only.
FIRMWARE := /opt/homebrew/share/qemu/edk2-aarch64-code.fd

RES ?= 1024x768x32

.PHONY: all iso run clean FORCE

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
LLM_CFLAGS := -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
              -fno-stack-check -fno-lto -fno-pie -Isrc -Ikernel/include \
              -Ibsdfmt $(EXTRA)

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
# Defaults to tcg, which is not where this port wants to end up.
#
# Running on the actual CPU under hvf is the whole reason for an ARM64
# build, and it is where the model and the desktop become usable. But
# qemu 11.0.3's hvf backend aborts this guest — `Assertion failed: (isv)`,
# an MMIO exit whose instruction syndrome it declines to decode — and the
# abort is not conditional on anything this kernel does. It reproduces
# with the render loop replaced by an empty counted spin, with the
# framebuffer never touched, with no display device attached at all, and
# under gic-version=2, gic-version=3 and highmem=off alike. A guest parked
# in wfi never triggers it; a guest executing instructions does, after
# about half a second. The same binary runs indefinitely under tcg at a
# steady 60 fps.
#
# So the default is the one that works, and ACCEL=hvf is one word away for
# anyone retesting against a newer qemu. tcg has independent value anyway:
# emulation enforces weaker memory ordering than Apple silicon does, so it
# catches missing barriers that hvf would hide — which matters as soon as
# virtio descriptors arrive.
ACCEL ?= tcg
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
QEMU_COMMON := -M virt -m 2048 $(QEMU_CPU) -accel $(ACCEL) \
	-drive if=pflash,format=raw,unit=0,readonly=on,file=$(FIRMWARE) \
	-drive if=pflash,format=raw,unit=1,file=build/efi-vars.fd \
	-device ramfb \
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
ifneq ($(wildcard $(DISK)),)
QEMU_DISK := -drive if=none,id=d0,format=raw,readonly=$(DISK_RO),file=$(DISK) \
	-device virtio-blk-device,drive=d0
else
QEMU_DISK :=
endif

# EDK2 keeps its variables in a second flash bank; it wants one the same
# size as the code image even when empty.
#
# Regenerated whenever the ISO is, deliberately. EDK2 writes a boot entry
# naming the exact PCI path it booted from, and a stale one sends it to
# the UEFI shell instead of the disc the moment anything about the device
# set changes — which looks exactly like a kernel that failed to load.
build/efi-vars.fd: os.iso
	@mkdir -p build
	dd if=/dev/zero of=$@ bs=1m count=64 2>/dev/null

run: os.iso build/efi-vars.fd
	qemu-system-aarch64 $(QEMU_COMMON) -serial stdio

# Headless, for the scripted harness: serial to a log, QMP for input and
# screenshots. Same shape as the x86 tree's tools/qemu_drive.py workflow.
run-headless: os.iso build/efi-vars.fd
	qemu-system-aarch64 $(QEMU_COMMON) -display none \
		-serial file:build/serial.log \
		-qmp tcp:127.0.0.1:4480,server,nowait

clean:
	rm -rf build os.iso $(ISO)/boot $(ISO)/EFI
