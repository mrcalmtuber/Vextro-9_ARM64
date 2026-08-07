#!/usr/bin/env python3
"""
Boot the ARM64 build and capture the screen.

The serial log proves the render loop is running at the right rate; it
says nothing about whether the picture is correct. This grabs a PPM
through QMP so a frame can be looked at, or diffed against the x86 build's
output for the same input — which is the cross-architecture regression
test the port plan calls for.

Usage: tools/arm_shot.py [seconds-before-capture] [out.ppm] [hvf|tcg]
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
# The volume with wiki.zim and the model. Writable now: accounts and
# home directories live on whichever volume the system mounts, and
# fs_mount() takes the largest, which is this one.
def _shared_disk(root):
    """The x86_64 tree's volume, wherever that tree happens to sit.

    Its *directory* name is not the operating system's name and did not
    change when the system was renamed, so the rebrand rewrote this path
    to a folder that does not exist and the harness quietly booted with no
    volume at all -- no exFAT, no encyclopedia, no model. Look for the
    file instead of trusting one spelling of the folder.
    """
    for name in ("Socrates BSD 9", "Vextro 9", "Vextro"):
        p = os.path.join(root, "..", name, "disk.img")
        if os.path.exists(p):
            return p
    return os.path.join(root, "..", "Socrates BSD 9", "disk.img")

DISK = _shared_disk(ROOT)

# EDK2 keeps its variables in a second flash bank and wants one even when
# empty. It is regenerated whenever it is missing or older than the ISO:
# the firmware records the exact device path it booted from, and a stale
# entry sends it to the UEFI shell instead of the disc the moment the
# device set changes — which looks exactly like a kernel that failed to
# load, and has cost this port an afternoon more than once.
VARS = os.path.join(ROOT, "build", "efi-vars.fd")
ISO = os.path.join(ROOT, "os.iso")
if (not os.path.exists(VARS) or
        (os.path.exists(ISO) and os.path.getmtime(VARS) < os.path.getmtime(ISO))):
    os.makedirs(os.path.dirname(VARS), exist_ok=True)
    with open(VARS, "wb") as fh:
        fh.write(b"\0" * (64 << 20))

QMP_PORT = 4482

WAIT = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build/screen.ppm")
ACCEL = sys.argv[3] if len(sys.argv) > 3 else "tcg"

cpu = ["-cpu", "host"] if ACCEL == "hvf" else ["-cpu", "cortex-a72"]
proc = subprocess.Popen([
    "qemu-system-aarch64", "-M", "virt", "-m", "2048", *cpu,
    "-accel", ACCEL,
    "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={FIRMWARE}",
    "-drive", f"if=pflash,format=raw,unit=1,file={ROOT}/build/efi-vars.fd",
    "-device", "ramfb",
] + ([
    "-drive", f"if=none,id=d0,format=raw,file={DISK}",
    "-device", "virtio-blk-device,drive=d0",
] if os.path.exists(DISK) else []) + [
    # This tree's own writable volume: /etc/users.db and the home
    # directories live here, because the disk above is shared with the
    # x86 tree and attached read-only.
    "-drive", f"if=none,id=d1,format=raw,file={ROOT}/build/data.img",
    "-device", "virtio-blk-device,drive=d1",
] + [
    "-netdev", "user,id=n0",
    "-device", "virtio-net-device,netdev=n0",
    "-global", "virtio-mmio.force-legacy=false",
    "-device", "virtio-keyboard-device",
    "-device", "virtio-tablet-device",
    "-cdrom", f"{ROOT}/os.iso",
    "-display", "none",
    "-serial", f"file:{ROOT}/build/shot-serial.log",
    "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait",
], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


# See the note in arm_run.py: a headless QEMU that outlives this script keeps
# a lock on the data disk, which lives in the x86 tree, and breaks `make run`
# over there with an error naming neither this script nor this tree. The
# create_connection below raises on timeout, so this is not a hypothetical.
def reap():
    if proc.poll() is None:
        proc.kill()
        proc.wait()


atexit.register(reap)

time.sleep(2)
sock = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=10)
f = sock.makefile("rw", encoding="utf-8", newline="\n")
f.readline()
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
f.flush()
f.readline()

print(f"booting ({ACCEL}); capturing in {WAIT:.0f}s", flush=True)
time.sleep(WAIT)

f.write(json.dumps({"execute": "screendump", "arguments": {"filename": OUT}}) + "\n")
f.flush()
while True:
    r = json.loads(f.readline())
    if "return" in r or "error" in r:
        print(r, flush=True)
        break

f.write(json.dumps({"execute": "quit"}) + "\n")
f.flush()
time.sleep(0.5)
proc.kill()
print(f"wrote {OUT}", flush=True)
