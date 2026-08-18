"""Revoke a device out from under a RUNNING program, then give it back.

The other test proves the launch table grants what it says. This one proves the
check is live: app_dev prints a light reading roughly every 100 ms and stops
after sixteen, so there is a ~2 s window in which a revoke is observable as the
readings CEASING -- and, because its refusal path retries rather than exits, as
the program never reaching its sixteenth reading. Restoring the grant has to
resume it. A permission that only takes effect at launch would show none of this.

The revoke is sent without waiting, because waiting is how the first version of
this test missed the window entirely and revoked from a slot whose program had
already finished.

Usage: perms_live.py COM5
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"


def drain(ser, secs, tag):
    print("\n---- " + tag + " " + "-" * max(0, 60 - len(tag)), flush=True)
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        buf += ser.read(4096)
    for line in buf.decode("utf-8", "replace").splitlines():
        s = line.rstrip()
        if not s.strip():
            continue
        if "switches r/a/b=" in s or s.lstrip().startswith("lock owner="):
            continue
        print("  | " + s, flush=True)


def send(ser, cmd):
    print(f"\n$ {cmd}", flush=True)
    ser.write((cmd + "\n").encode())
    ser.flush()


ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.2
ser.dsrdtr = False
ser.rtscts = False
ser.open()

ser.dtr = False
ser.rts = True
time.sleep(0.15)
ser.rts = False
ser.reset_input_buffer()

print("== waiting for boot self-tests ==", flush=True)
end = time.time() + 12.0
while time.time() < end:
    ser.read(4096)

send(ser, "kill 0")
drain(ser, 1.0, "free a slot")
send(ser, "kill 1")
drain(ser, 1.0, "free another")

ser.reset_input_buffer()

# BOTH lines in one write, with no read in between.
#
# app_dev is 463 instructions end to end and finishes in well under a second, so
# there is no window to aim at -- an earlier version of this test waited 1.2 s
# for it to "get going" and revoked from a program that had already exited and
# printed all sixteen readings. The shell consumes one line per poll, so the
# revoke lands a few instructions into the program rather than after it.
print("\n$ run dev", flush=True)
print("$ perms 0 0 off   (same write -- no gap)", flush=True)
ser.write(b"run dev\nperms 0 0 off\n")
ser.flush()
drain(ser, 5.0, "REVOKED mid-run: readings must stop and it must NOT exit")

send(ser, "perms")
drain(ser, 1.5, "still running, holding store+echo but not light")

send(ser, "dev")
drain(ser, 1.5, "denial counter")

send(ser, "perms 0 0 on")
drain(ser, 5.0, "RESTORED -- it must resume and reach sixteen")

ser.close()
