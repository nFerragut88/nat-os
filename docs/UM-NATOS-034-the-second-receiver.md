# UM-NATOS-034 — The Second Receiver

**Used Medias LLC — Embedded Systems Division**
Revision 1.2 · 2026-08-19 · Status: **Negative result, and a definitive one** — §9 and §10 add two more, and §10 corrects the lead list itself

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

---

## 9. Postscript: CCA and EDCA, eliminated (revision 1.1)

The rig above made a cheap follow-up possible, and it is recorded because a
plausible hypothesis that dies is worth as much as one that lives.

### 9.1 The hypothesis

`hal_mac.o` is already in the image — `--gc-sections` kept the nine functions
nat-os calls and discarded the other thirty-eight as unreferenced. Two of the
discarded ones are **leaf functions**, pure read-modify-writes with no calls, so
they can be replicated as direct pokes with no link change whatsoever:

```
hal_mac_tx_set_cca(mode):   0x3FF73C58        bits 31:30 = mode
hal_mac_tx_config_edca(p):  0x3FF73D1C - qid*8  AIFS 27:24, CW 21:12
```

Read on a live board after `phyinit`, `macinit`, `chan 6`, both were **entirely
zero** — the first time anyone had looked. A MAC that believes the channel is
permanently busy would behave exactly as §3 describes: accept the frame, arm the
queue, retire the descriptor, key nothing.

### 9.2 The result

Swept against the two-board rig, one register at a time, transmitter beaconing
continuously throughout:

| swept | receiver saw the transmitter's MAC |
|---|---|
| CCA mode 0, 1, 2, 3 | no |
| AIFS 2, 3, 7 | no |
| CW 15, 31 | no |

`scan` on the receiver ended every run the same way: `networks=1`, the real
access point, and nothing else. **Eliminated.**

### 9.3 What was learned anyway

**Those registers are live.** Setting AIFS=7 and CW=31 drove `forced` from 0 to
699 out of 947 frames — the driver's own guard for a completion that never
arrives. The transmit state machine demonstrably responds to them. It simply
does not radiate either way.

**And a misreading, corrected.** `wifimac_tx()` already writes `|= 0x02000000`,
which is bit 25 — *inside* the AIFS field — and `|= 0x00003000`, which is CW
bits 1:0. So AIFS was never 0 during a transmit; it was 2, and CW was 3. The
existing driver had been setting sane-ish EDCA values by accident, through bits
nobody had decoded. The "degenerate arbitration" half of the hypothesis was
wrong before it was tested.

### 9.4 Two instrument failures, both mine

The first sweep keyed on the receiver's raw frame-count **delta**, and flagged
every single step as a hit. A real access point in the room delivers about 1.5
frames a second, so any eight-second window shows a dozen new frames whatever
the transmitter does. **The instrument was measuring the neighbourhood.** The
detector must key on the transmitter's source MAC, which is the entire reason
this rig exists (§2).

The second was worse in kind: the patch that fixed the detector silently failed
to apply, and the run went ahead with the broken one still in place. The
conclusion survived only because the raw `scan` output at the end is ground
truth and needs no detector at all.

Recorded because §7 already had two invalid runs of three, and this is two more.
A rig built to stop bad measurements does not stop bad measurements; it only
makes them cheap to notice.

### 9.5 Where that leaves it

The zero-link-risk surface is now exhausted. Everything remaining —
`esp_wifi_power_domain_on`, `lmacInit`, `coex_bt_high_prio` — requires naming a
symbol, and the measured blast radius of the largest is **305,888 bytes of
vendor object pulled in by one reference, against a 142,016-byte image**, with
`tx_pwctrl_background` among the cascade — the same object as the calibration
that broke last time.

`wifitx` and `tools/serial/wifi_sweep.py` remain. Both are one command away from
re-testing any future guess in about a minute.

---

## 10. `lmacInit`: the risk taken, and the model that was wrong (revision 1.2)

The link change everyone had been avoiding since UM-NATOS-028 was made
deliberately, with the safety procedure from `next_moves/01` followed step by
step. It did not fix transmit. It corrected three things that were believed
about the problem, and two of them mattered more than the attempt.

### 10.1 The blast-radius model was badly wrong

§9.5 predicted **305,888 bytes** of vendor object pulled in by one reference,
against a 142,016-byte image. Measured:

| | predicted | actual |
|---|---|---|
| image growth | ~306 kB | **+1,200 bytes** |
| new text symbols | hundreds | **10** |

The model forgot `--gc-sections`. The archive members *are* pulled in to resolve
the 47 undefined symbols, and then everything not reachable from `lmacInit`
itself is discarded. What survived was `lmacInit`, `lmacInitAc`, `lmacConfMib`,
`rcAttach`, `rc_cal` and `wDev_reset_bcnSendTick`.

**The thing this project has feared for three reports cost 1,200 bytes.** The
fear was well-founded when it was formed — `register_chipv7_phy` really did
break — but it was never re-measured, and it had grown into a reason not to try.

### 10.2 The canary held, and so did everything else

Flashed to one board only, leaving the second on the previous image as an
unchanged receiver, so the control could not drift with the experiment.

| check | result |
|---|---|
| `phyinit` returns 0 | **yes** — the exact canary that failed last time |
| receive after the link change | **yes**, AP decoded, descriptors recycling |
| `lmacInit` through the windowed bridge | returned cleanly |
| system after | apps running, DMA 109,336 transfers 0 timeouts, `heap check=0` |

### 10.3 It changed the transmit path and still did not radiate

`chain acks` — the one transmit counter that has never claimed success (§5) —
moved, and moved twice:

| configuration | chain acks / completions |
|---|---|
| before | 39/302, 10/219 — roughly 5–13% |
| `lmacInit` | **82/165 — about 50%** |
| `lmacInit` + `lmacInitAc(0..3)` | 8/172 — back down to 5% |

So the lower MAC is genuinely wired to that counter, and arming all four access
categories made it *worse*. The receiver's answer never changed: `networks=1`,
the real access point, and never the transmitter's MAC.

### 10.4 The correction that matters most: two of the three leads do not exist

`next_moves/01` has listed three remaining leads since UM-NATOS-028. Searching
the archives nat-os actually has:

| lead | where it is |
|---|---|
| `lmacInit` / `lmacInitAc` | `lmac.o` — **present, now tried, does not fix it** |
| `coex_bt_high_prio` | **not in libpp or libphy at all** |
| `esp_wifi_power_domain_on` | **not in libpp or libphy at all** |

Both missing ones are ESP-IDF functions, not vendor-blob functions. `hal_coex.o`
exists in the archive and exports nothing.

**So the lead list has been two-thirds fictional.** Not wrong about what
open-mac calls — wrong about what is reachable from here. Any future attempt at
those two has to replicate them from documentation as direct register writes, in
the way §9 replicated the CCA and EDCA pokes, and nobody has established what
they do on this silicon.

### 10.5 What is kept

`lmacInit` stays, behind the `lmacinit` shell command, called from nowhere at
boot. It costs 1,200 bytes, breaks nothing, and is the only way to reproduce the
`chain acks` behaviour for whoever looks next.

### 10.6 `lmacTxFrame`, examined and deliberately not called

Revision 1.2 first claimed `lmac.o` had brought `lmacTxFrame` — the vendor's own
transmit entry point — into reach for the same price, and that it was the
obvious next thing. **That was wrong on both counts**, and the correction is
worth more than the claim was.

Of lmac.o's 50 functions, `--gc-sections` kept exactly two:

```
40090ee4 T lmacInit
40090e98 T lmacInitAc
```

`lmacTxFrame` was discarded, and so was `.bss.our_instances`, the per-AC state
table it works from. It is not linked and not free.

More importantly it should not be called even if it were. Disassembled, its
first act is to index that table and check a state byte:

```
l8ui a6, a5, 18          ; per-AC state, stride 36
addi a5, a6, -3
bltui a5, 2, ok          ; proceed only when the state is 3 or 4
...
movi a2, 0x6f8           ; assert, line 1784
callx8 a8
j    99                  ; jumps to ITSELF -- forever
```

Four reasons, any one sufficient:

1. **The failure mode is an unrecoverable hang**, not a fault. That loop runs
   inside `phy_stack_call`, which masks interrupts, so the watchdog may never
   fire. It needs the power pulled.
2. **The precondition cannot be checked first.** `our_instances` is a static
   that is not in the image, so there is no way to read the state byte and learn
   whether the assert would pass.
3. **Its first argument is a packet structure that would have to be
   fabricated**, and it dereferences it immediately. A wrong layout is vendor
   code walking garbage.
4. It is not linked, so trying costs the object plus its dependency chain
   anyway.

This is precisely the situation the project's discipline exists for. A vendor
function whose assert path is an infinite loop, whose precondition is
unreadable, and whose argument must be guessed, is not a lead — it is a way to
lose an afternoon and a working receiver at the same time.

Left undone, on purpose, and recorded so nobody re-derives it as an obvious
next step.

