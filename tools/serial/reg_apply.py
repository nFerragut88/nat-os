"""Apply every stable register difference, then ask whether transmit works.

UM-NATOS-034 §13 produced a shortlist: registers where ESP-IDF holds something
while transmitting and nat-os holds something else while failing to. One at a
time is the right discipline when there are three candidates. There are about
thirty, and at a couple of minutes each that is an afternoon.

So: apply them ALL, and let the answer decide what happens next.

    transmit starts working -> bisect. The answer is in this set, and halving
                               it finds which register in five more runs.
    nothing changes         -> the answer is NOT in this register set, which is
                               worth far more than thirty individual negatives.
                               It would mean the difference is somewhere the
                               dump does not reach: the PHY/RF block, a ROM
                               call, or an ordering the registers cannot show.

The second outcome is the likely one and is still progress. A shortlist that
can be eliminated wholesale is a shortlist that stops being re-derived.

Skipped deliberately:
  - 0x3FF74000+   buffer RAM, random on both sides
  - the MAC address and mask registers, already applied by `machw`
  - the pointer pairs, already applied and eliminated

Usage: reg_apply.py [nat_port] [regdiff.txt]
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
DIFF = sys.argv[2] if len(sys.argv) > 2 else "regdiff.txt"

ALREADY = {0x3ff73008, 0x3ff7300c, 0x3ff73028, 0x3ff7302c,
           0x3ff73040, 0x3ff73044, 0x3ff73048, 0x3ff7304c,
           0x3ff73068, 0x3ff7306c,
           0x3ff73118, 0x3ff7311c, 0x3ff73120, 0x3ff73124}

rows = []
for line in open(DIFF, encoding="utf-8"):
    m = re.match(r"0x([0-9a-f]+) ref=([0-9a-f]+) nat=([0-9a-f]+)", line)
    if not m:
        continue
    a, r, n = (int(g, 16) for g in m.groups())
    if a >= 0x3FF74000:          # buffer RAM
        continue
    if a in ALREADY:
        continue
    if a < 0x3FF73000:           # DPORT and RTC: clock/power, not MAC state
        continue
    rows.append((a, r, n))

print(f"== {len(rows)} MAC registers to align with the reference ==", flush=True)

s = serial.Serial()
s.port, s.baudrate, s.timeout = PORT, 115200, 0.3
s.dsrdtr = s.rtscts = False
s.open()
s.dtr = False
s.rts = True
time.sleep(0.15)
s.rts = False
s.reset_input_buffer()
time.sleep(12.0)


def cmd(c, wait=2.0):
    s.reset_input_buffer()
    s.write((c + "\n").encode())
    s.flush()
    end, buf = time.time() + wait, b""
    while time.time() < end:
        buf += s.read(4096)
    return buf.decode("utf-8", "replace")


def find(text, pat):
    m = re.search(pat, text)
    return m.group(0) if m else ""


for c in ("fb off", "wifipd on", "phyinit", "macinit", "chan 6",
          "macrx", "machw", "lmacinit"):
    out = cmd(c, 3.0)
    print(f"   {c:<12} {find(out, r'(returned 0|MAC IS RUNNING|tuned to channel .|receiver armed|after : .*|lmacInit.*returned|applied.*)')}")

# the pointer pairs, already eliminated but part of the reference state
for a, v in ((0x3ff73118, 0x400a0000), (0x3ff7311c, 0x3ffae000),
             (0x3ff73120, 0x400a0000), (0x3ff73124, 0x3ffae000)):
    cmd(f"wifireg {a:08x} {v:08x}", 1.0)

print(f"\n== applying {len(rows)} differences ==", flush=True)
for a, r, n in rows:
    cmd(f"wifireg {a:08x} {r:08x}", 0.8)
print("   done", flush=True)

print("\n== did the receiver survive? ==", flush=True)
print("   " + find(cmd("scan", 3.0), r"frames=\d+\s+recycled=\d+\s+networks=\d+"))

print("\n== does it transmit? (reference AP is an active peer) ==", flush=True)
for i in range(3):
    print("   " + find(cmd("probe", 5.0), r"frames addressed to us=\d+"), flush=True)
print("   " + find(cmd("txstat", 3.0), r"hardware=\d+\s+completions reaped=\d+"))
print("   " + find(cmd("txstat", 3.0), r"chain acks=\d+\s+forced=\d+"))
s.close()
