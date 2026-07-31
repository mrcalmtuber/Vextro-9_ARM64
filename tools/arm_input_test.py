#!/usr/bin/env python3
"""
Drive the ARM64 build's input path end to end.

Boots, waits for the login screen, then sends real pointer motion and real
keystrokes through QMP and captures the result. This is the check that
matters for the input milestone: the serial log can say a virtio device
was found and its queues came up, and the pointer can still be landing in
the wrong place or the keyboard delivering the wrong characters. Only a
frame shows that.

The events go in as a host would generate them — absolute coordinates for
the tablet, qcodes for the keyboard — so nothing about the guest's
interpretation is assumed.

Usage: tools/arm_input_test.py [boot-wait-seconds] [hvf|tcg]
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
DISK = os.path.join(ROOT, "..", "Socrates BSD 9", "disk.img")

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

QMP_PORT = 4483

BOOT_WAIT = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0
ACCEL = sys.argv[2] if len(sys.argv) > 2 else "tcg"

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
    "-serial", f"file:{ROOT}/build/input-serial.log",
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


def qmp(cmd, args=None):
    msg = {"execute": cmd}
    if args:
        msg["arguments"] = args
    f.write(json.dumps(msg) + "\n")
    f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r
        # events (RESET, SHUTDOWN, ...) are not replies; keep reading


def send(events):
    return qmp("input-send-event", {"events": events})


def move_to(fx, fy):
    """Absolute move, given as a fraction of the screen."""
    return send([
        {"type": "abs", "data": {"axis": "x", "value": int(fx * 32767)}},
        {"type": "abs", "data": {"axis": "y", "value": int(fy * 32767)}},
    ])


def type_text(s):
    for ch in s:
        qcode = {"-": "minus", ".": "dot", " ": "spc"}.get(ch, ch)
        send([{"type": "key", "data": {"down": True,
                                       "key": {"type": "qcode", "data": qcode}}}])
        time.sleep(0.05)
        send([{"type": "key", "data": {"down": False,
                                       "key": {"type": "qcode", "data": qcode}}}])
        time.sleep(0.05)


print(f"booting ({ACCEL}); login expected in {BOOT_WAIT:.0f}s", flush=True)
time.sleep(BOOT_WAIT)

# Enter drops the login screen and starts the desktop.
print("pressing Enter to enter the desktop", flush=True)
send([{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ret"}}}])
time.sleep(0.1)
send([{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ret"}}}])
time.sleep(25)
qmp("screendump", {"filename": f"{ROOT}/build/desktop.ppm"})
print("captured build/desktop.ppm", flush=True)

print("moving pointer to (0.25, 0.30)", flush=True)
print(" ", move_to(0.25, 0.30), flush=True)
time.sleep(3)
qmp("screendump", {"filename": f"{ROOT}/build/input-a.ppm"})

print("typing", flush=True)
type_text("socrates")
time.sleep(3)

print("moving pointer to (0.75, 0.70) and clicking", flush=True)
move_to(0.75, 0.70)
time.sleep(1)
send([{"type": "btn", "data": {"down": True, "button": "left"}}])
time.sleep(1)
qmp("screendump", {"filename": f"{ROOT}/build/input-b.ppm"})
send([{"type": "btn", "data": {"down": False, "button": "left"}}])

time.sleep(2)
qmp("quit")
time.sleep(0.5)
proc.kill()
print("captured build/input-a.ppm and build/input-b.ppm", flush=True)
