"""What ESP-IDF's transmit does to the MAC, with background subtracted.

UM-NATOS-034 §19. Every earlier version of this comparison lacked a control on
at least one side, and §18 lacked one on both — it took a trace during transmit
bursts, saw `0x3FF73DB8` cycling, and called that the transmit signature. With a
control on nat-os the same cycle turned out to happen while nothing was being
transmitted.

`tools/idf_ref` now captures each register window TWICE, back to back: once with
no `esp_wifi_80211_tx()` at all, once with a burst of eight. Ambient traffic
varies over minutes, so a control taken minutes later would be a weak one;
back-to-back on the same window is as close to one-variable as this gets.

This script pairs them and reports, per address:

    tx-only     changed with a burst and never without  -> caused by transmit
    both        changed in both                         -> background
    idle-only   changed without a burst and not with    -> background, and a
                                                           warning that the
                                                           sample is thin

Only the first column is evidence about transmit. Everything the project
believed about this register block before §19 came from not separating them.

Usage: tx_control.py [port] [out.json]
"""
import json
import os
import re
import sys
import time
from collections import defaultdict

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(__file__), "..", "..", "docs", "data", "tx-control-idf-ref.json")

HDR = re.compile(r"TRACE base=([0-9a-f]{8}) words=(\d+) snaps=(\d+) cycles=(\d+) mode=(\w+)")
ROW = re.compile(r"^T (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})")


def capture(port, secs=300.0):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.3
    s.dsrdtr = s.rtscts = False
    s.open()
    s.dtr = False
    s.rts = True
    time.sleep(0.15)
    s.rts = False
    s.reset_input_buffer()
    print(f"== capturing from {port} (128 windows x 2 modes; several minutes) ==",
          flush=True)
    buf, end, last = b"", time.time() + secs, 0
    while time.time() < end:
        buf += s.read(8192)
        n = buf.count(b"TRACEEND")
        if n >= last + 32:
            last = n
            print(f"   {n} captures...", flush=True)
        if b"TRACESWEEPEND" in buf:
            break
    s.close()
    txt = buf.decode("utf-8", "replace")
    if "TRACESWEEPEND" not in txt:
        print("!! sweep did not finish -- output is partial")
    return txt


def parse(txt):
    windows, cur = [], None
    for ln in txt.splitlines():
        m = HDR.search(ln)
        if m:
            cur = {"base": int(m.group(1), 16), "snaps": int(m.group(3)),
                   "cycles": int(m.group(4)), "mode": m.group(5), "rows": []}
            continue
        if cur is None:
            continue
        m = ROW.match(ln.strip())
        if m:
            cur["rows"].append((int(m.group(1)), int(m.group(2), 16),
                                int(m.group(3), 16), int(m.group(4), 16)))
            continue
        if "TRACEEND" in ln:
            windows.append(cur)
            cur = None
    return windows


txt = capture(PORT)
windows = parse(txt)
if not windows:
    print("!! nothing parsed. Is the updated tools/idf_ref flashed to this port?")
    raise SystemExit(1)

tx_w = [w for w in windows if w["mode"] == "tx"]
id_w = [w for w in windows if w["mode"] == "idle"]
print(f"\n   captures: {len(tx_w)} tx, {len(id_w)} idle")
if not id_w:
    print("!! no idle captures -- this is the old firmware, and the whole point")
    print("   of this script is the control. Reflash tools/idf_ref.")
    raise SystemExit(1)

# Count how many captures of each mode saw each address move at all. Counting
# CAPTURES rather than changes keeps one chatty window from dominating.
tx_seen, id_seen = defaultdict(int), defaultdict(int)
tx_n, id_n = defaultdict(int), defaultdict(int)
for w in tx_w:
    for a in {r[1] for r in w["rows"]}:
        tx_seen[a] += 1
    tx_n[w["base"]] += 1
for w in id_w:
    for a in {r[1] for r in w["rows"]}:
        id_seen[a] += 1
    id_n[w["base"]] += 1

per_base_tx = max(tx_n.values()) if tx_n else 0
per_base_id = max(id_n.values()) if id_n else 0

addrs = sorted(set(tx_seen) | set(id_seen))
tx_only = [a for a in addrs if tx_seen[a] and not id_seen[a]]
both = [a for a in addrs if tx_seen[a] and id_seen[a]]
id_only = [a for a in addrs if id_seen[a] and not tx_seen[a]]

print(f"   each window captured {per_base_tx}x with a burst, {per_base_id}x without\n")
print(f"   TX-ONLY  (caused by transmit) : {len(tx_only)}")
print(f"   BOTH     (background)         : {len(both)}")
print(f"   IDLE-ONLY(background, thin)   : {len(id_only)}")

print("\n== addresses that move ONLY when a frame is sent ==")
print("   these, and only these, are evidence about ESP-IDF's transmit\n")
print("   addr          tx  idle")
print("   " + "-" * 30)
for a in tx_only:
    print(f"   0x{a:08x}   {tx_seen[a]:3d}  {id_seen[a]:4d}")
if not tx_only:
    print("   NONE. Either the burst did not land in any window, or every")
    print("   address that moves does so regardless -- both of which would")
    print("   invalidate the comparison rather than answer it.")

print("\n== background: moves with or without a transmit ==")
print("   addr          tx  idle")
print("   " + "-" * 30)
for a in both:
    print(f"   0x{a:08x}   {tx_seen[a]:3d}  {id_seen[a]:4d}")

# The specific question §19 left open: what state path does 0x3FF73DB8 take on
# ESP-IDF once background is removed, and does it match nat-os's
# 000 -> 058 -> 258 -> 220 -> 020 -> 000?
print("\n== 0x3FF73DB8 state transitions, by mode ==")
for mode, ws in (("tx", tx_w), ("idle", id_w)):
    seen = defaultdict(int)
    for w in ws:
        for _, a, o, n in w["rows"]:
            if a == 0x3FF73DB8 and not (o in (0, 1) and n in (0, 1)):
                seen[(o, n)] += 1
    print(f"   --- {mode} ---")
    if not seen:
        print("       (none)")
    for (o, n), c in sorted(seen.items(), key=lambda kv: -kv[1]):
        print(f"       {o:03x} -> {n:03x}   x{c}")

json.dump([{"base": w["base"], "mode": w["mode"], "cycles": w["cycles"],
            "rows": w["rows"]} for w in windows], open(OUT, "w"), indent=1)
print(f"\n   full capture written to {OUT}")
