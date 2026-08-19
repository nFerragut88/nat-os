"""Differential register dump: ESP-IDF transmitting vs nat-os failing to.

UM-NATOS-034 Phase B. Three attempts at guessing which register matters have
each eliminated something and none has found it. This stops guessing.

Both stacks call the same PHY blob, so whatever the blob does internally is
identical in both. The difference between a radio that transmits and one that
does not therefore has to be visible in the registers the SURROUNDING code
touches, and that is a layer both firmwares can dump.

    The point of a differential is that you do not need to know what a
    register means to notice that it differs.

---- the filter that makes it readable -------------------------------------

Most of this address space is counters: the TSF timer, frame statistics, free
running clocks. A naive diff is almost entirely those.

So each board dumps TWICE, a second apart. Anything that changes between a
board's OWN two dumps is volatile by definition and discarded. What survives is
stable state -- the only kind where "these two differ" means anything.

That filter is the whole design. Without it this tool produces a thousand lines
of noise and hides the answer inside it.

Usage: reg_diff.py [ref_port] [nat_port]
       ref_port  runs tools/idf_ref  (dumps automatically after boot)
       nat_port  runs nat-os         (driven into transmit, then `regdump` x2)
"""
import re
import sys
import time

import serial

REF_PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
NAT_PORT = sys.argv[2] if len(sys.argv) > 2 else "COM5"

LINE = re.compile(r"REG\s+(\w+)\s+(?:0x)?([0-9a-fA-F]{8})((?:\s+(?:0x)?[0-9a-fA-F]{8})+)")


def parse(text):
    """-> list of {addr: value}, one dict per REGEND-terminated dump."""
    dumps, cur = [], {}
    for ln in text.splitlines():
        m = LINE.search(ln)
        if m:
            base = int(m.group(2), 16)
            vals = [int(v.replace("0x", ""), 16) for v in m.group(3).split()]
            for i, v in enumerate(vals):
                cur[base + i * 4] = v
        elif "REGEND" in ln:
            if cur:
                dumps.append(cur)
            cur = {}
    return dumps


def open_port(port, reset):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.3
    s.dsrdtr = s.rtscts = False
    s.open()
    if reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.15)
        s.rts = False
    s.reset_input_buffer()
    return s


def read_for(s, secs):
    end, buf = time.time() + secs, b""
    while time.time() < end:
        buf += s.read(8192)
    return buf.decode("utf-8", "replace")


def cmd(s, c, wait):
    s.write((c + "\n").encode())
    s.flush()
    return read_for(s, wait)


# ---- reference: dumps itself twice a few seconds after boot ---------------
print("== reference (ESP-IDF, transmitting) ==", flush=True)
ref = open_port(REF_PORT, reset=True)
ref_txt = read_for(ref, 22.0)
ref.close()
ref_dumps = parse(ref_txt)
mac = re.search(r"REF-(?:AP|RAW) .*mac=([0-9a-f:]{17})", ref_txt)
print(f"   ref mac {mac.group(1) if mac else '?'}   dumps captured: {len(ref_dumps)}")

# ---- nat-os: drive it into the same state, then dump twice ----------------
print("\n== nat-os (transmitting, or trying to) ==", flush=True)
nat = open_port(NAT_PORT, reset=True)
read_for(nat, 12.0)
for c in ("fb off", "wifipd on", "phyinit", "macinit", "chan 6",
          "macrx", "lmacinit", "beacon diffref"):
    out = cmd(nat, c, 3.0)
    tag = [l.strip() for l in out.splitlines()
           if l.strip().startswith(("returned", "MAC IS", "tuned", "receiver",
                                    "beaconing", "applied", "lmacInit"))]
    print(f"   {c:<14} {tag[0] if tag else ''}")
time.sleep(3.0)

nat_txt = cmd(nat, "regdump", 12.0)
time.sleep(1.0)
nat_txt += cmd(nat, "regdump", 12.0)
nat.close()
nat_dumps = parse(nat_txt)
print(f"   dumps captured: {len(nat_dumps)}")

if len(ref_dumps) < 2 or len(nat_dumps) < 2:
    print("\n!! need two dumps from each side to filter volatile registers.")
    print("   Refusing to diff -- an unfiltered diff is noise wearing a result's")
    print("   clothes, and this project has published one of those already.")
    raise SystemExit(1)

# ---- volatile filter ------------------------------------------------------
ref_vol = {a for a in ref_dumps[0] if ref_dumps[0][a] != ref_dumps[1].get(a)}
nat_vol = {a for a in nat_dumps[0] if nat_dumps[0][a] != nat_dumps[1].get(a)}
volatile = ref_vol | nat_vol

common = set(ref_dumps[0]) & set(nat_dumps[0])
stable = sorted(common - volatile)

print(f"\n   addresses dumped by both : {len(common)}")
print(f"   volatile (discarded)     : {len(common & volatile)}")
print(f"   stable, comparable       : {len(stable)}")

diffs = [(a, ref_dumps[0][a], nat_dumps[0][a])
         for a in stable if ref_dumps[0][a] != nat_dumps[0][a]]

print(f"\n   STABLE AND DIFFERENT     : {len(diffs)}")
print("\n   addr        ESP-IDF   nat-os    xor")
print("   " + "-" * 44)
for a, r, n in diffs:
    print(f"   0x{a:08x}  {r:08x}  {n:08x}  {r ^ n:08x}")

with open("regdiff.txt", "w", encoding="utf-8") as fh:
    for a, r, n in diffs:
        fh.write(f"0x{a:08x} ref={r:08x} nat={n:08x} xor={r ^ n:08x}\n")
print(f"\n   written to regdiff.txt")
