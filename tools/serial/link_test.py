"""Two boards, both directions, each with a control on its own receiver.

UM-NATOS-034 §5's second-receiver result is load-bearing for everything since,
including §20's conclusion that the transmit fault is below the MAC. That
receiver was validated against a real access point, and never against
tools/idf_ref -- so "a receiver that hears an AP would also hear a nearby ESP32"
was carrying weight while unmeasured. §21 measures it.

---- the two directions need two different idf_ref builds -------------------

Found the hard way, and it cost a wrong verdict. The first attempt put idf_ref
in AP mode (so it would beacon for direction A) *and* left promiscuous receive
on (for direction B). In that combination it heard FOUR frames in forty seconds
and could not hear an access point nat-os hears continuously: ESP-IDF's
promiscuous receive is all but disabled in AP mode.

The script reported "control passes, test fails" anyway, from a deaf receiver.

    A negative from an instrument that has not been shown to work is not a
    negative. It is nothing at all.

So:

    DIRECTION A   build idf_ref with REF_LINK_AP 1   (AP, beacons NATOS-CTRL-6)
                  nat-os's `scan` lists beacon sources -> look for the SSID
                  control: nat-os also lists some other, ambient AP

    DIRECTION B   build idf_ref with REF_LINK_AP 0   (STA + promiscuous)
                  nat-os transmits; idf_ref tallies every source MAC
                  control: idf_ref's tally contains some other transmitter

This script runs DIRECTION B, which is the one with a moving part. Direction A
is two commands and is described above.

---- the detector rule ------------------------------------------------------

`wifi_sweep.py` once keyed on a frame-count delta and reported a hit at every
step, because a real AP delivers frames continuously and the counter always
moves.

    key on the transmitter's MAC address, never on frame volume.

Usage: link_test.py [nat_port] [ref_port] [seconds]
"""
import re
import sys
import time

import serial

NAT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
REF = sys.argv[2] if len(sys.argv) > 2 else "COM6"
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0


def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dsrdtr = s.rtscts = False
    s.open()
    s.dtr = False
    s.rts = True
    time.sleep(0.15)
    s.rts = False
    s.reset_input_buffer()
    return s


def cmd(s, c, w):
    s.write((c + "\n").encode())
    s.flush()
    end, b = time.time() + w, b""
    while time.time() < end:
        b += s.read(8192)
    return b.decode("utf-8", "replace")


ref, nat = open_port(REF), open_port(NAT)
time.sleep(13.0)

print("== nat-os: radio up ==", flush=True)
for c, w in (("fb off", 2), ("wifipd on", 3), ("phyinit", 8),
             ("macinit", 5), ("chan 6", 3), ("macrx", 3)):
    cmd(nat, c, w)

# nat-os's MAC, decoded from the two hardware slot registers rather than from a
# printed string. `machw` reports them as `after : 0x503b015c 0x0000643f`, which
# is the address little-endian across two words.
#
# The first version of this script regex'd for a colon-separated MAC that
# `machw` never prints, got None, and compared every received address against
# None -- a detector that could not succeed. It then reported the expected
# answer, which is the worst way for a broken test to fail.
out = cmd(nat, "machw", 4.0)
m = re.search(r"after\s*:\s*0x([0-9a-f]{8})\s+0x([0-9a-f]{8})", out)
if not m:
    print("!! could not read nat-os's MAC from `machw`. Refusing to run:")
    print("   a MAC-matching test with no MAC to match is a test that always")
    print("   says NO.")
    raise SystemExit(1)
lo, hi = int(m.group(1), 16), int(m.group(2), 16)
mac_b = [lo & 0xFF, (lo >> 8) & 0xFF, (lo >> 16) & 0xFF, (lo >> 24) & 0xFF,
         hi & 0xFF, (hi >> 8) & 0xFF]
NAT_MAC = ":".join(f"{b:02x}" for b in mac_b)
print(f"   nat-os MAC = {NAT_MAC}  (from 0x{lo:08x} 0x{hi:08x})")

print(f"\n== {SECS:.0f} s: nat-os transmitting, idf_ref listening ==", flush=True)
ref.reset_input_buffer()
cmd(nat, "beacon", 2.0)
end = time.time() + SECS
while time.time() < end:
    cmd(nat, "probe", 5.0)

raw, t2 = b"", time.time() + 3.0
while time.time() < t2:
    raw += ref.read(8192)
txt = raw.decode("utf-8", "replace")

srcs = {}
for l in txt.splitlines():
    m = re.match(r"LINKSRC ([0-9a-f:]{17}) (\d+)", l.strip())
    if m:
        srcs[m.group(1)] = int(m.group(2))
tot = re.findall(r"LINK frames=(\d+) sources=(\d+)", txt)
frames = int(tot[-1][0]) if tot else 0

print(f"   idf_ref heard {frames} frames from {len(srcs)} sources")
for mac, n in sorted(srcs.items(), key=lambda kv: -kv[1])[:14]:
    print(f"      {mac}  x{n}" + ("   <<< NAT-OS" if mac == NAT_MAC else ""))

# The control: idf_ref must be shown to hear SOMETHING before its silence about
# nat-os means anything. Other transmitters in the tally are that proof.
others = {k: v for k, v in srcs.items() if k != NAT_MAC}
control = len(others) >= 2 and sum(others.values()) >= 20
test = NAT_MAC in srcs

print(f"\n   CONTROL  idf_ref hears other transmitters : "
      f"{'YES' if control else 'NO'}  ({len(others)} sources, {sum(others.values())} frames)")
print(f"   TEST     idf_ref hears nat-os              : {'YES' if test else 'NO'}")

print("\n" + "=" * 66)
if not control:
    print("CONTROL FAILS -- idf_ref's receiver is not demonstrably working, so")
    print("its silence about nat-os is not evidence. Check REF_LINK_AP is 0.")
elif test:
    print("nat-os TRANSMITS and is heard. This contradicts UM-NATOS-034 §5 and")
    print("the record needs revisiting before anything else is concluded.")
else:
    print("CONTROL PASSES, TEST FAILS. A working receiver 30 cm away, hearing")
    print("other transmitters throughout, hears nothing from nat-os. §5's")
    print("foundation is now measured on live hardware rather than inherited.")
print("=" * 66)

nat.close()
ref.close()
