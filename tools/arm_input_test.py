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
import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = "/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
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
    "-global", "virtio-mmio.force-legacy=false",
    "-device", "virtio-keyboard-device",
    "-device", "virtio-tablet-device",
    "-cdrom", f"{ROOT}/os.iso",
    "-display", "none",
    "-serial", f"file:{ROOT}/build/input-serial.log",
    "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait",
], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

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
