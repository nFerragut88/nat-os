"""Capture and reduce a transmit trace from tools/idf_ref.

UM-NATOS-034 §13 recorded why the register work stalled:

    the snapshot diff compares destinations, not routes.

Applying all thirty stable differences wholesale changed nothing. That result
eliminated the shortlist, and left two shapes of cause that no snapshot can
reach: the ORDER in which state is established, and TRANSIENT writes to
self-clearing bits, which read back zero before any snapshot could see them.

The firmware side captures a 16-register window into DRAM at ~0.55 us per
sample while the other core transmits one frame. This side turns that into
something a person can read.

---- the volatile problem, again, and why it is different here ---------------

reg_diff.py discarded anything that moved on its own, because in a SNAPSHOT a
free-running counter is pure noise. Here it is not so simple: the TSF timer
moving proves the trace is live, and a counter that increments exactly once
during a transmit is a frame counter -- which is a result, not noise.

So nothing is discarded. Addresses are CLASSIFIED by how they behave:

    clock       changes in nearly every sample -- a free-running counter
    stepped     changes a handful of times     -- state, or a counter that
                                                  counts events rather than time
    one-shot    changes once or twice          -- the interesting column

and printed with the clocks folded to one line. A "go" bit written and
self-cleared shows up as two adjacent one-shot changes at the same address,
which is a signature worth naming rather than filtering away.

Usage: tx_trace.py [port] [out.json]
"""
import json
import os
import re
import sys
import time
from collections import defaultdict

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
# Default into docs/data/ rather than the working directory. This file is the
# evidence behind UM-NATOS-034 §18 -- the raw trace a reader would need to
# check the claims in it -- so it belongs beside the report, not in whatever
# directory the tool happened to be run from.
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(__file__), "..", "..", "docs", "data", "tx-trace-idf-ref.json")

HDR = re.compile(r"TRACE base=([0-9a-f]{8}) words=(\d+) snaps=(\d+) cycles=(\d+)")
ROW = re.compile(r"^T (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})")
END = re.compile(r"TRACEEND changes=(\d+)")


def capture(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.3
    s.dsrdtr = s.rtscts = False
    s.open()
    s.dtr = False
    s.rts = True
    time.sleep(0.15)
    s.rts = False
    s.reset_input_buffer()

    print(f"== capturing from {port} (reset, then the sweep) ==", flush=True)
    buf, end = b"", time.time() + 90.0
    while time.time() < end:
        buf += s.read(8192)
        if b"TRACESWEEPEND" in buf:
            break
    s.close()
    txt = buf.decode("utf-8", "replace")
    if "TRACESWEEPEND" not in txt:
        print("!! sweep did not finish in 90 s -- output is partial")
    return txt


def parse(txt):
    """-> list of windows, each {base, cycles, snaps, rows: [(snap, addr, old, new)]}"""
    windows, cur = [], None
    for ln in txt.splitlines():
        m = HDR.search(ln)
        if m:
            cur = {"base": int(m.group(1), 16), "words": int(m.group(2)),
                   "snaps": int(m.group(3)), "cycles": int(m.group(4)), "rows": []}
            continue
        if cur is None:
            continue
        m = ROW.match(ln.strip())
        if m:
            cur["rows"].append((int(m.group(1)), int(m.group(2), 16),
                                int(m.group(3), 16), int(m.group(4), 16)))
            continue
        if END.search(ln):
            windows.append(cur)
            cur = None
    return windows


def classify(rows, snaps):
    """addr -> ('clock'|'stepped'|'one-shot', change count)"""
    counts = defaultdict(int)
    for _, addr, _, _ in rows:
        counts[addr] += 1
    out = {}
    for addr, n in counts.items():
        if n > snaps // 4:
            kind = "clock"
        elif n > 8:
            kind = "stepped"
        else:
            kind = "one-shot"
        out[addr] = (kind, n)
    return out


txt = capture(PORT)
windows = parse(txt)
if not windows:
    print("!! no trace windows parsed. Is tools/idf_ref flashed to this port?")
    raise SystemExit(1)

# Two passes over the same 16 bases. Pairing them tests the assumption the
# sweep rests on: that a transmit does the same thing twice. If it does not,
# nothing reconstructed across windows can be trusted, and that is a finding
# rather than a setback.
half = len(windows) // 2
pass_a, pass_b = windows[:half], windows[half:]

print(f"\n   windows captured : {len(windows)}  ({half} per pass)")
if windows:
    w = windows[0]
    us = w["cycles"] / 240.0 / w["snaps"]
    print(f"   sample period    : {us:.2f} us   ({w['snaps']} samples, "
          f"{w['cycles'] / 240000.0:.2f} ms of coverage)")

print("\n   base        changes  clocks  stepped  one-shot")
print("   " + "-" * 50)
report = []
for w in pass_a:
    k = classify(w["rows"], w["snaps"])
    c = sum(1 for v in k.values() if v[0] == "clock")
    st = sum(1 for v in k.values() if v[0] == "stepped")
    os_ = sum(1 for v in k.values() if v[0] == "one-shot")
    print(f"   0x{w['base']:08x}  {len(w['rows']):7d}  {c:6d}  {st:7d}  {os_:8d}")
    report.append((w, k))

# ---- the column this instrument exists for -------------------------------
print("\n== one-shot and stepped changes, in time order ==")
print("   (a self-clearing trigger appears as two rows, same address,")
print("    adjacent snapshots, second value returning to the first)\n")
print("   snap   addr        from      to        kind")
print("   " + "-" * 56)

events = []
for w, k in report:
    for snap, addr, old, new in w["rows"]:
        kind = k[addr][0]
        if kind != "clock":
            events.append((snap, addr, old, new, kind))
events.sort()
for snap, addr, old, new, kind in events[:400]:
    print(f"   {snap:5d}  0x{addr:08x}  {old:08x}  {new:08x}  {kind}")
if len(events) > 400:
    print(f"   ... {len(events) - 400} more (all of them are in {OUT})")
if not events:
    print("   none. Every address that moved is free-running -- which means")
    print("   the transmit's own writes are outside the swept range, or were")
    print("   overwritten inside one sample period.")

# ---- does a transmit do the same thing twice? -----------------------------
print("\n== determinism check: pass 0 vs pass 1, same bases ==")
for wa, wb in zip(pass_a, pass_b):
    if wa["base"] != wb["base"]:
        print(f"   0x{wa['base']:08x}  base mismatch -- sweep desynchronised")
        continue
    ka, kb = classify(wa["rows"], wa["snaps"]), classify(wb["rows"], wb["snaps"])
    sa = {a for a, v in ka.items() if v[0] != "clock"}
    sb = {a for a, v in kb.items() if v[0] != "clock"}
    if sa == sb:
        if sa:
            verdict = "same addresses"
        elif wa["rows"] or wb["rows"]:
            verdict = "clocks only (both)"
        else:
            # Not the same as "only clocks moved". Nothing moved at all --
            # 32,768 samples of a register window that never changed. Saying
            # "clocks" here would be this instrument claiming to have seen
            # something it did not.
            verdict = "SILENT -- no change in either pass"
    else:
        verdict = f"DIFFERS  only-in-0={sorted(hex(x) for x in sa - sb)} " \
                  f"only-in-1={sorted(hex(x) for x in sb - sa)}"
    print(f"   0x{wa['base']:08x}  {verdict}")

with open(OUT, "w", encoding="utf-8") as fh:
    json.dump([{"base": w["base"], "cycles": w["cycles"], "snaps": w["snaps"],
                "rows": w["rows"]} for w in windows], fh, indent=1)
print(f"\n   full trace written to {OUT}")
