#!/usr/bin/env python3
"""
Boot the ARM64 build under QEMU and watch it properly.

Why this exists rather than `make run`:

  - Serial goes over a socket, not `-serial file:`. QEMU buffers a file
    chardev, and when hvf aborts the process dies without flushing, so the
    last few lines before a crash — the only ones that matter — are lost.
    Reading a socket means every byte the kernel emitted has already been
    received by the time the crash is noticed. Two wrong diagnoses on this
    port came from trusting a truncated log.

  - QEMU's own stderr is captured and printed. `Assertion failed: (isv)`
    is an hvf message, not a kernel one, and it never reaches the serial
    line at all.

  - If the kernel goes quiet, the CPU is asked where it is. `info
    registers` over QMP works on a live guest and answers in one shot what
    is otherwise inferred from print statements: PC, ELR_EL1, ESR_EL1 and
    FAR_EL1 together say what faulted, where, and on which address.

Usage: tools/arm_run.py [seconds] [hvf|tcg]
"""
import atexit
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = "/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
# Read-only: this is the volume with wiki.zim and the model on it.
DISK = os.path.join(ROOT, "..", "Socrates BSD 9", "disk.img")

# EDK2 keeps its variables in a second flash bank and wants one even when
# empty. It is regenerated whenever it is missing or older than the ISO:
# the firmware records the exact device path it booted from, and a stale
# entry sends it to the UEFI shell instead of the disc the moment the
# device set changes — which looks exactly like a kernel that failed to
# load, and has cost this port an afternoon more than once.
# Recreated every time, not just when older than the ISO.
#
# EDK2 writes to this file while running, so it is always newer than the
# ISO it recorded a boot entry for — which means an "is it stale?"
# comparison on modification times can never be true, and a stale entry
# sends the firmware to the UEFI shell instead of the disc. That looks
# exactly like a kernel that failed to load. Nothing in the store is
# worth keeping, so this just starts clean.
VARS = os.path.join(ROOT, "build", "efi-vars.fd")
os.makedirs(os.path.dirname(VARS), exist_ok=True)
with open(VARS, "wb") as fh:
    fh.write(b"\0" * (64 << 20))

SER_PORT, QMP_PORT = 4481, 4480

RUN_FOR = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
ACCEL = sys.argv[2] if len(sys.argv) > 2 else "hvf"

# tcg cannot use -cpu host, and needs a core with the features the kernel
# expects. Emulation also enforces weaker memory ordering than Apple
# silicon does, so a tcg run catches missing barriers hvf would hide.
cpu = ["-cpu", "host"] if ACCEL == "hvf" else ["-cpu", "cortex-a72"]

# `acpi=off` makes the firmware publish a device tree.
#
# EDK2 hands the OS *either* ACPI tables or a DTB, never both: when ACPI
# is available it deliberately removes the device tree from the UEFI
# configuration table so a guest cannot try to use both descriptions of
# the same machine. Limine reads that table, so with ACPI on there is no
# blob to pass and the kernel keeps its built-in `virt` addresses —
# which is correct on this machine and means the discovery path is never
# exercised. Turning ACPI off is the only way to test on virt what will
# always be true on a board.
machine = "virt,acpi=off" if os.environ.get("DTB") == "1" else "virt"

cmd = [
    "qemu-system-aarch64", "-M", machine, "-m", "2048", *cpu,
    "-accel", ACCEL,
    "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={FIRMWARE}",
    "-drive", f"if=pflash,format=raw,unit=1,file={ROOT}/build/efi-vars.fd",
    "-device", "ramfb",
] + ([
    "-drive", f"if=none,id=d0,format=raw,readonly=on,file={DISK}",
    "-device", "virtio-blk-device,drive=d0",
] if os.path.exists(DISK) else []) + [
    "-netdev", "user,id=n0",
    "-device", "virtio-net-device,netdev=n0",
    "-global", "virtio-mmio.force-legacy=false",
    "-device", "virtio-keyboard-device",
    "-device", "virtio-tablet-device",
    "-cdrom", f"{ROOT}/os.iso",
    "-display", "none",
    "-serial", f"tcp:127.0.0.1:{SER_PORT},server,nowait",
    "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait",
]

proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


# A leaked QEMU is not a harmless stray process. It holds an exclusive lock
# on the data disk, and that disk lives in the *x86* tree -- so a harness run
# that exits without reaping its child makes `make run` over there fail with
# `Failed to get "write" lock`, a message that points at the wrong tree
# entirely and reads like disk corruption. One escaped this way and spun at
# 100% CPU for over an hour before anyone noticed.
#
# The orderly shutdown at the bottom of this script only runs when the script
# reaches the bottom. This covers the paths that do not: the early exit when
# serial never comes up, and any exception in between.
def reap():
    if proc.poll() is None:
        proc.kill()
        proc.wait()


atexit.register(reap)


def connect(port, tries=50):
    for _ in range(tries):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2)
            s.settimeout(0.2)
            return s
        except OSError:
            if proc.poll() is not None:
                return None
            time.sleep(0.2)
    return None


ser = connect(SER_PORT)
qmp = connect(QMP_PORT)
if ser is None:
    print("qemu died before serial came up:", proc.stdout.read().decode(errors="replace"))
    sys.exit(1)

qf = None
if qmp:
    qf = qmp.makefile("rw", encoding="utf-8", newline="\n")
    qf.readline()
    qf.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    qf.flush()
    qf.readline()


def hmp(c):
    qf.write(json.dumps({"execute": "human-monitor-command",
                         "arguments": {"command-line": c}}) + "\n")
    qf.flush()
    while True:
        r = json.loads(qf.readline())
        if "return" in r:
            return r["return"]


log = open(os.path.join(ROOT, "build/serial.log"), "wb")
start = time.time()
last_rx = start
pending = b""
went_quiet = False

while time.time() - start < RUN_FOR:
    if proc.poll() is not None:
        break
    try:
        data = ser.recv(65536)
        if data:
            log.write(data)
            log.flush()
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                text = line.decode(errors="replace").rstrip("\r")
                if "socrates" in text or "arm64" in text:
                    print(text, flush=True)
            last_rx = time.time()
            went_quiet = False
    except socket.timeout:
        pass
    except OSError:
        break

    # Quiet for three seconds with the guest still up: ask the CPU where it is.
    if qf and not went_quiet and time.time() - last_rx > 3.0 and time.time() - start > 8.0:
        went_quiet = True
        try:
            regs = hmp("info registers")
        except Exception:
            continue
        print("\n--- guest went quiet; CPU state ---", flush=True)
        for line in regs.splitlines():
            if line.startswith(" PC=") or line.startswith("PC=") or "PSTATE" in line:
                print(line.strip(), flush=True)
        with open(os.path.join(ROOT, "build/regs.txt"), "w") as fh:
            fh.write(regs)
        print("--- full dump in build/regs.txt ---\n", flush=True)

rc = proc.poll()
if rc is None:
    if qf:
        try:
            hmp("quit")
        except Exception:
            pass
    time.sleep(0.3)
    proc.kill()

out = proc.stdout.read().decode(errors="replace").strip()
if out:
    print("\n--- qemu said ---")
    print(out)
print(f"\n--- qemu exit: {proc.poll()} after {time.time()-start:.1f}s ---")
