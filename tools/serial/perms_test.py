"""Verify per-application device permissions on the live board.

One boot, one sequence, because the claim being tested is about STATE that
changes across commands: a grant made by the launch table, a denial counter that
moves when a device is revoked out from under a running program, and a slot that
must not hand its capabilities to whoever lands in it next. Running each step
from a fresh reset would test none of that.

The first thing it does is kill the two boot applications. Not tidiness -- the
`dev` program is the only one that actually reaches hardware, so it is the only
one whose refusals are observable, and with APP_MAX at 4 and ping/pong resident
it is worth making certain it gets a slot rather than discovering afterwards
that the interesting step never ran.

Usage: perms_test.py COM5
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"

STEPS = [
    # (command, seconds to capture, what this step is for)
    ("kill 0",           2.0, "clear the boot applications out of the way"),
    ("kill 1",           2.0, ""),
    ("perms",            2.0, "baseline: nothing running, nothing granted"),
    ("run dev",          6.0, "launch: the table grants light+store+echo"),
    ("perms",            2.0, "the grant is visible and is NOT all devices"),
    ("dev",              2.0, "counters before we revoke anything"),
    ("perms 0 0 off",    6.0, "revoke the light sensor from a RUNNING program"),
    ("perms",            2.0, "light gone; denials should now be climbing"),
    ("dev",              2.0, "refusals/denials moved, and dev is still alive"),
    ("perms 0 0 on",     4.0, "give it back; it should recover, not stay broken"),
    ("perms",            2.0, "light restored, denial count frozen where it was"),
    ("perms 0 6 on",     2.0, "grant the SD card, which the table never gave it"),
    ("perms",            2.0, "sd is listed"),
    ("kill 0",           3.0, "retire the slot"),
    ("run counter",      3.0, "a DEV_PERM_NONE program lands in the same slot"),
    ("perms",            2.0, "CRITICAL: it must NOT have inherited sd"),
]

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.2
ser.dsrdtr = False
ser.rtscts = False
ser.open()

# Fresh boot, so the counters start where the kernel put them.
ser.dtr = False
ser.rts = True
time.sleep(0.15)
ser.rts = False
ser.reset_input_buffer()

print("== waiting for boot self-tests ==", flush=True)
deadline = time.time() + 12.0
while time.time() < deadline:
    ser.read(4096)

for cmd, secs, why in STEPS:
    print("\n" + "=" * 68)
    print(f"$ {cmd}" + (f"      ({why})" if why else ""))
    print("=" * 68, flush=True)
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        buf += ser.read(4096)
    text = buf.decode("utf-8", "replace")
    for line in text.splitlines():
        s = line.rstrip()
        if not s.strip():
            continue
        # The reporter line is 2 kB of counters every 200 ms and would bury
        # everything this test is about.
        if "switches r/a/b=" in s or s.lstrip().startswith("lock owner="):
            continue
        print("  | " + s, flush=True)

ser.close()
