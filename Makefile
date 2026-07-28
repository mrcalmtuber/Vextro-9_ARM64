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

RES ?= 1280x800x32

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

build/kernel: build/kernel.o build/vectors.o build/boot_animation_data.o linker.ld
	$(LD) $(LDFLAGS) build/kernel.o build/vectors.o build/boot_animation_data.o -o $@

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

QEMU_COMMON := -M virt -m 2048 $(QEMU_CPU) -accel $(ACCEL) \
	-drive if=pflash,format=raw,unit=0,readonly=on,file=$(FIRMWARE) \
	-drive if=pflash,format=raw,unit=1,file=build/efi-vars.fd \
	-device ramfb \
	-cdrom os.iso

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
