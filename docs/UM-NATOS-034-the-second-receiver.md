# UM-NATOS-034 — The Second Receiver

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-19 · Status: **Negative result, and a definitive one**

---

## 1. Abstract

WiFi transmit has been the project's largest open item since UM-NATOS-028. The
MAC accepts frames and reports them complete — 178 of 178, `forced=0` — and
nothing in the world responds. Twenty probe requests drew zero replies from two
access points that are received continuously.

That evidence was **stuck**, and this report is about why and what unstuck it.

"An access point did not answer" is equally consistent with two entirely
different faults:

- nothing left the antenna at all, or
- something left but was malformed.

An AP that ignores a malformed frame looks exactly like a radio that never
transmitted. Eleven leads were investigated without that distinction ever being
settled, because every test asked a device we do not control to react, and got
back one bit with no detail.

A second nat-os board settles it. **Nothing is reaching the air.** The transmit
fault is in the RF/PHY path, not in frame construction, and an entire branch of
the search is now closed.

---

## 2. Why a second board is a different instrument

The receiver in this kernel already works: promiscuous mode, beacon decode,
continuous scanning, verified across two reports. Half a peer-to-peer link has
existed the whole time. What was missing was a receiver **we control and can
instrument**.

Three properties make it decisive where an AP was not:

**An unforgeable signature.** Every board reports its factory MAC with a
verified CRC. A beacon from board A carries A's MAC, which board B cannot
manufacture and no neighbouring router can forge. This is the same principle as
UM-NATOS-028's probe-response test: a radio cannot put someone else's identity
into your receiver.

```
COM5   5c:01:3b:50:3f:64
COM6   5c:01:3b:51:2b:40
```

**A control that runs first.** Board B is measured *before* board A transmits.
If B hears nothing in that window it has proved nothing about A, only about
itself — a negative result is only informative if the experiment demonstrably
ran (Rule 6). So B must hear a real access point before its silence about A
counts as evidence.

**A raw frame counter, not just a parser.** `scan` lists distinct beacon
sources, but `frames=` counts every descriptor the hardware filled. That
distinction is the whole point: a beacon too corrupt to parse would still raise
the frame count. Reading only the parsed list would have confused "malformed"
with "absent" — the exact confusion this test exists to remove.

---

## 3. Results

Both directions, ~30 cm apart, channel 6, one real access point in range.

| | run 1 | run 2 |
|---|---|---|
| Transmitter | COM5 | COM6 |
| Receiver | COM6 | COM5 |
| **Receiver heard real AP** | yes, `"TC7NR"` ×1 | yes, `"TC7NR"` ×9 |
| **Receiver raw frames** | 2 | 20 → 26 while watching |
| **Receiver networks seen** | 1 (the AP) | 1 (the AP) |
| Transmitter handed to hardware | 303 | 220 |
| Transmitter completions reaped | 302 | 219 |
| Transmitter `chain acks` | 39 | 10 |
| **Peer's MAC ever seen** | **never** | **never** |

The transmitter beacons every 100 ms — ten frames a second. The receiver in run
2 is capturing the distant access point at roughly two frames a second and
recycling descriptors while it does. A transmitter 30 cm away emitting at five
times that rate would dominate the capture completely.

It contributes nothing.

---

## 4. What this eliminates

**Framing and content are exonerated.** A malformed beacon is still radio
frequency energy. A receiver actively filling descriptors from an access point
across the house would register *something* from a board on the same desk. The
frame counter does not move.

**The remaining fault is that the radio never keys up.** The MAC's completion
counter is not evidence of radiation; it is evidence that the MAC believes it
handed the frame onward. This is the project's third standing rule arriving in a
new place: *a successful register write is not evidence*, extended to a
successful **completion count**.

The leads in `next_moves/01` survive and are now the whole of the search:
`esp_wifi_power_domain_on`, `lmacInit`, `coex_bt_high_prio`. All require naming
symbols that are not currently linked, and the second standing rule applies —
naming a symbol changes the link and has broken working code before.

### 4.1 The caveat, stated rather than buried

This proves no *demodulable* signal arrives. A transmission mangled badly enough
that an identical receiver cannot demodulate it would look the same as silence.

That does not weaken the conclusion, it only renames it: such a fault is still
in the RF/PHY path and still not in frame construction. Either way the branch
that was closed stays closed.

---

## 5. Two observations for whoever picks this up

**`chain acks` and completions disagree by an order of magnitude.** 39 acks
against 302 completions; 10 against 219. Nobody has explained what that ratio
means, and it is the only counter in the transmit path that does *not* claim
everything is fine. If any instrument on this board is telling the truth about
transmit, it is probably that one.

**A quiet receiver is not automatically a trustworthy one.** In run 1 the
receiver caught 2 frames while the transmitter's own receiver caught 49, in the
same room on the same channel. That is almost certainly how often
`wifimac_rx_service()` gets called rather than sensitivity — but it made run 1's
silence much weaker evidence than it appeared.

Run 1 alone would have supported the same conclusion for the wrong reason. The
roles were swapped specifically because the instrument looked weak, and run 2 is
what the conclusion actually rests on.

---

## 6. Found on the way: the renderer and the radio cannot coexist

`macrx` failed with *"out of DRAM for rx buffers"* on both boards, which stopped
the first two runs dead.

Measured:

| state | heap free | largest block |
|---|---|---|
| 3D framebuffer allocated | **832 B** | 832 B |
| `fb off` | **108,352 B** | 107,520 B |

The framebuffer is 80,640 bytes of a roughly 110 kB heap. The receiver needs
6,592 (four 1,600-byte buffers plus descriptors) and cannot have it.

**On this board you can have the 3D view or the radio, not both.** That is worth
recording as an architectural fact rather than a bug: it is one more argument for
the split in `docs/conceptual/the-ark-and-fiendnet.md`, where a relay node is
headless and never allocates a framebuffer at all.

---

## 7. Process, recorded because two runs were invalid

The first run reported "board B heard nothing" — from a receiver that had never
armed, because the `fb off` step had silently failed to apply to the script.
The test's own control caught it and refused to conclude anything, which is the
only reason it did not become a finding.

The second run had the weak receiver of §5.

Two invalid runs before a valid one, on a test built specifically to avoid
invalid runs. The control is what made both harmless.

---

## 8. Status

| Claim | Evidence |
|---|---|
| Receiver is live and instrumented | real AP decoded in every run, both boards |
| Transmit reports success | 302 and 219 completions, `forced=0` |
| Nothing demodulable reaches the air | peer frame count flat in both directions |
| Fault is RF/PHY, not framing | §4 |
| Renderer and radio cannot share the heap | §6, measured |

Open, and unchanged: transmit itself. What has changed is that the search is now
half the size, and the next attempt has a rig that answers in seconds instead of
a question nobody could answer at all.

`tools/serial/wifi_link.py` runs the whole thing.
