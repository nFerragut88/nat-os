# UM-NATOS-034 — The Second Receiver

**Used Medias LLC — Embedded Systems Division**
Revision 1.7 · 2026-08-19 · Status: **Negative result** — §15 matches the reference to the job and sharpens the shortlist to nineteen

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

---

## 11. The power domain: not a blob problem after all (revision 1.3)

§10.4 concluded that `esp_wifi_power_domain_on` was unreachable because the
symbol is not in `libpp` or `libphy`. True, and the wrong conclusion.

**It is not a blob function.** It is ESP-IDF, it is open source, and it is six
register operations on documented registers:

```c
void IRAM_ATTR esp_wifi_bt_power_domain_on(void)
{
    CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_FORCE_PD);
    esp_rom_delay_us(10);
    wifi_bt_common_module_enable();
    DPORT_SET_PERI_REG_MASK(DPORT_CORE_RST_EN_REG, MODEM_RESET_FIELD_WHEN_PU);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_CORE_RST_EN_REG, MODEM_RESET_FIELD_WHEN_PU);
    CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_WIFI_FORCE_ISO);
    wifi_bt_common_module_disable();
}
```

That is a general lesson worth more than the experiment: **"the symbol is not in
the archive" is not the same as "the behaviour is out of reach."** Two of the
three leads this project has carried since UM-NATOS-028 were dismissed on that
confusion.

### 11.1 The isolation hypothesis, dead on arrival

`WIFI_FORCE_ISO` clamps signals leaving the WiFi power domain. Asserted, the
digital side would work perfectly while nothing reached the analog side — §3's
symptom stated as a register.

Read on a fresh boot, before anything touched them:

```
DIG_PWC = 0x00155550   WIFI_FORCE_PD(17)  = clear
DIG_ISO = 0xaaaa5000   WIFI_FORCE_ISO(28) = clear
```

Both already clear. The ROM or the second-stage bootloader sets this up, and the
domain has been powered and un-isolated the entire time. **Hypothesis
eliminated in one read**, which is the cheapest any of them has been.

### 11.2 One thing was genuinely unset

`WIFI_CLK = 0xfffce030`, and `DPORT_WIFI_CLK_WIFI_BT_COMMON_M` is `0x3C9`.
Those bits AND to **zero** — the WiFi/BT common module clock was never enabled.
Applying the sequence set them (`0xe030` → `0xe3f9`).

### 11.3 Result: another behaviour change, still no radiation

Full bring-up after the sequence: `phyinit` 0, MAC running, channel tuned,
receiver armed, `lmacInit` clean. Then beaconing at 10 Hz with the second board
listening 30 cm away.

`chain acks` moved again, and this time strangely:

| configuration | chain acks / frames handed over |
|---|---|
| baseline | 39/302, 10/219 — about 5–13% |
| `lmacInit` | 82/165 |
| `lmacInit` + `lmacInitAc(0..3)` | 8/172 |
| **+ power domain sequence** | **236/124 — nearly twice the frames sent** |

Chain acknowledgements now **exceed** the number of frames queued, which has
never happened. Something in the DMA chain is being processed more than once per
frame. That is a specific, new, unexplained observation and the best remaining
thread.

The receiver's answer did not change: `networks=1`, the real access point, never
the transmitter's MAC. System healthy throughout — `heap check=0`, DMA 1,070,824
transfers 0 timeouts, receive decoding normally.

### 11.4 Where this leaves transmit

Three attempts in one session, each cheap, each eliminating something, none
radiating:

| attempt | cost | result |
|---|---|---|
| CCA / EDCA pokes | none | registers live, no radiation |
| `lmacInit` / `lmacInitAc` | 1,200 bytes | `chain acks` 5%→50%→5%, no radiation |
| power domain sequence | none | common clock was off, now on; `chain acks` > frames, no radiation |

What has not been tried, and is now the honest next step, is **differential
tracing**: run ESP-IDF's own stack on identical hardware, capture every register
write it makes during a transmit, and diff it against nat-os. That converts the
problem from guessing which register matters into reading which one differs.
It needs a second toolchain and a bare devkit — the CYD's JTAG pins are the
display's — and it is a different kind of work from anything in this project so
far.

Until someone does that, transmit is a research problem rather than a task with
a next step.

---

## 12. The receiver, proved against a known-good transmitter (revision 1.4)

Every conclusion in this report rests on a receiver whose only proven reference
was a router across the house. That left one gap: "our transmitter is silent"
and "our receiver is deaf in some way we have not noticed" were never fully
separated. §5 already showed the receiver's frame count varies a lot with what
else is running, which made the gap real rather than theoretical.

Closing it needed a transmitter we control that definitely works. So: ESP-IDF
5.4.1, unmodified, as a SoftAP on the second board — `tools/idf_ref`.

```
REF-AP ssid=natref channel=6 mac=5c:01:3b:51:2b:41
```

nat-os, promiscuous on channel 6, 30 cm away:

```
frames=235  recycled=235  networks=2
5c:01:3b:51:2b:41  x170  "natref"
44:25:38:19:0d:1a  x53   "TC7NR"
```

**170 beacons in 15 seconds**, MAC and SSID matching the reference exactly.

### 12.1 What this settles

| | beacons heard |
|---|---|
| ESP-IDF SoftAP, ~10 Hz, 30 cm | **170** |
| nat-os transmitter, ~10 Hz, 30 cm | **0** |

Same receiver, same channel, same room, same session. The receiver is not the
problem, and now it is not the problem *by measurement against a working
transmitter* rather than by inference from a distant router.

Stated precisely: the two rows are not simultaneous, because a radio cannot
hear itself and there are only two boards. The comparison is across runs. What
is shared is everything else.

### 12.2 And a correction about what this needed

§11.4 said differential tracing needed "a second toolchain and a bare devkit —
the CYD's JTAG pins are the display's". The JTAG part is true and was
irrelevant: it reached for the heaviest method and then let that method's
constraint rule out the whole approach. **The CYD is an ordinary ESP32-WROOM-32
and runs ESP-IDF perfectly well.** Nothing here needed JTAG, a devkit, or
anything but the two boards already on the desk and a UART.

The second toolchain part is fair — this is the first thing in the project that
depends on Espressif's tooling rather than replacing it, and the first build
took 379 seconds.

---

## 13. Phase B: the differential, and the address that was never set

Three attempts at guessing which register mattered each eliminated something and
none found it. This stops guessing.

Both stacks call the same PHY blob, so whatever it does internally is identical.
Any difference between a radio that transmits and one that does not has to be
visible in the registers the *surrounding* code touches — and both firmwares can
dump those.

**The point of a differential is that you do not need to know what a register
means to notice that it differs.**

### 13.1 The filter that makes it readable

Most of that address space is counters — the TSF timer, statistics, free-running
clocks — and a naive diff is almost entirely those.

So each board dumps **twice**, a second apart. Anything that moves between a
board's own two dumps is volatile by definition and discarded.

```
addresses dumped by both : 1408
volatile (discarded)     :   35
stable, comparable       : 1373
STABLE AND DIFFERENT     :  342
```

Of the 342, 245 are in `0x3FF74000`+ and hold random-looking values on both
sides — buffer RAM, not control registers. That leaves **97 real candidates**,
which is a list a person can read.

### 13.2 What it found immediately

```
0x3ff73008   ESP-IDF: 513b015c    nat-os: 00000000
0x3ff7300c   ESP-IDF: 0000412b    nat-os: 00000000
```

Little-endian, that is `5c:01:3b:51:2b:41` — **the reference AP's own MAC
address**, exactly as its firmware printed it. The same at `0x3FF73040/44`
(ending `2b:40`, the station interface) and `0x3FF73048/4C`. The match masks at
`0x3FF73028` and `0x3FF73068` hold `ffffffff` / `0001ffff` in the reference and
**zero** in nat-os.

**This driver has been running with a hardware MAC address of
00:00:00:00:00:00 since it was written.** `hal_mac_set_bssid` is even in the
image — it survives `--gc-sections` because vendor code references it — and
nat-os has never called it.

That is a real defect found in about a minute by a method that had not been
tried, after three sessions of informed guessing found nothing.

### 13.3 Fixed, and it did not fix transmit

`machw` programs all three address slots and both masks from the chip's own
eFuse MAC:

```
before: 0x00000000 0x00000000
after : 0x503b015c 0x0000643f     = 5c:01:3b:50:3f:64
```

Then, with the reference AP 30 cm away — an *active* peer, so a probe request
that arrives earns a probe response addressed back:

```
sent 10 probe requests   frames addressed to us=0
sent 10 probe requests   frames addressed to us=0
sent 10 probe requests   frames addressed to us=0
```

Nothing. Receive was re-checked afterwards and is unaffected — 757 frames,
hearing both the reference (556) and the router (171) — so the masks did not
break the working path.

### 13.4 The remaining candidates, which are now specific

The diff hands over a shortlist instead of a hunch. The ones that look like
state rather than noise:

| address | ESP-IDF | nat-os | reads like |
|---|---|---|---|
| `0x3ff73118` / `311c` | `400a0000` / `3ffae000` | `ffffffff` / `00000000` | **a DRAM pointer** — `3ffae000` is data RAM |
| `0x3ff73120` / `3124` | `400a0000` / `3ffae000` | `ffffffff` / `00000000` | the same pair again |
| `0x3ff73c40` | `01e839e0` | `00000000` | another pointer-shaped value |
| `0x3ff730f8`–`3104` | `05000000` | `87800000` | a mode or rate field |
| `0x3ff73400`–`3430` | rate-table-looking bytes | different bytes | per-rate configuration |
| `0x3ff73148` / `314c` | `00000900` / `00003000` | `00000000` | unset in nat-os |

Two independent pointer pairs into DRAM that nat-os leaves at zero and
`ffffffff` are the most interesting thing on the list, because a transmit path
handed no buffer is a transmit path that completes and radiates nothing —
which is the symptom, stated as a register, for the fourth time this report.

### 13.5 What actually changed today

Transmit still does not work. What changed is the *method*:

- three sessions of guessing produced three eliminations and no defects
- one differential produced a real defect in a minute and a shortlist of six
  more, each with an address and a value to try

`tools/idf_ref` and `tools/serial/reg_diff.py` make that repeatable. Anyone
picking this up starts from a list, not a theory.

---

## 14. The limit of the differential, found by exceeding it

§13.4 left about ninety candidates. One at a time is right for three and is an
afternoon for ninety, so the next move was to apply them all: if transmit
started, bisect; if nothing changed, the answer is not in this register set at
all — which is worth more than ninety individual negatives.

Fifty MAC registers were written to the reference's values. **The board stopped
responding.** It recovered completely on reset — apps running, `corrupt=0`,
nothing permanent — but the run produced no measurement.

### 14.1 Why that is the useful result

The obvious reading is "be more careful." The correct reading is that **the
premise was wrong**.

The two firmwares are not in the same configuration and were never going to be:

| | reference | nat-os |
|---|---|---|
| mode | SoftAP, full stack | promiscuous receive, minimal |
| beaconing | via the vendor's own scheduler | via a hand-written register poke |
| queues, rate control, timers | all initialised by the stack | mostly untouched |

So a great many of those 342 differences are **legitimate configuration
differences**, not defects. They say "these two radios are doing different
jobs", which is true and not a bug. Copying them wholesale writes the settings
of a fully-configured AP into a minimally-configured sniffer, and the crash is
the honest consequence.

**A differential is a pointer at candidates, not a set of values to copy.** The
MAC address in §13.2 was a genuine find precisely because zero is not a
legitimate configuration of anything — it is wrong in any mode. That is the
shape of difference worth acting on, and most of the ninety do not have it.

### 14.2 What would make the next differential sharper

Match the reference to nat-os instead of matching nat-os to the reference. An
ESP-IDF build in **promiscuous/sniffer mode that transmits a raw frame** —
`esp_wifi_80211_tx()` — is doing very nearly what nat-os does, and the diff
against it would be small enough that every remaining difference is suspicious
rather than merely different.

That is a change to `tools/idf_ref`, not to nat-os, and it is the obvious next
step for whoever continues.

### 14.3 Standing rule earned

**A difference is not a defect until you know the two sides were doing the same
job.** This report spent its first eleven sections comparing a transmitter
against a receiver and being careful about it; §13 compared two transmitters in
different modes and briefly forgot to be.

---

## 15. A reference doing the same job, and a sharper list

§14.2 said the next step was to match the reference to nat-os rather than the
reverse. Done: `tools/idf_ref` is now promiscuous, unassociated, on a fixed
channel, pushing a hand-built raw 802.11 probe request out every 100 ms via
`esp_wifi_80211_tx()`. That is nat-os's job, described exactly.

### 15.1 The reference was verified before it was trusted

`esp_wifi_80211_tx()` can fail silently, and a reference that is not really
transmitting would poison every comparison drawn from it. nat-os was used as the
detector:

```
802.11 frame: probe-req  len=72
bssid ff:ff:ff:ff:ff:ff
bytes: ... 40 00 00 00 ff ff ff ff ff ff 5c 01 3b 51 2b 40 ff ff ff ff ff ff
                                         ^^^^^^^^^^^^^^^^^ the reference's MAC
```

Frame rate at the receiver went from ~1.5/s (the house router alone) to about
20/s. The reference radiates, and says who it is while doing it.

### 15.2 The diff did not shrink; it got sharper

| | SoftAP reference | raw-injection reference |
|---|---|---|
| stable and different | 342 | 335 |
| buffer RAM (`0x3FF74000`+) | 245 | 245 |
| **MAC control registers** | — | **58** |
| **of those, "reference has a value, nat-os has zero"** | — | **19** |

Total barely moved, which is worth stating plainly because the expectation in
§14.2 was that it would. What changed is the *quality*: "reference has
something, nat-os has nothing" is the shape that cannot be a legitimate mode
difference, and there are nineteen of them.

### 15.3 The same register block, caught a second time

```
0x3ff73000  ref=513b015c  nat=00000000
0x3ff73004  ref=0000402b  nat=00000000
```

The MAC address again — at `0x3FF73000`, **not** `0x3FF73008` where the SoftAP
reference kept it. There are two address slots and the live one depends on the
interface: the AP build used `0x3FF73008`, the STA build uses `0x3FF73000`.

`machw` had programmed only `0x3FF73008` — the slot belonging to the mode nat-os
is not in. §13 found the address was never written; §15 found it was then
written to the wrong place. Both were visible only against a reference doing the
same job.

Fixed, both slots now written. Receive unaffected. **Transmit unchanged:**
`frames addressed to us=0`, three times, with an active peer 30 cm away.

### 15.4 The handoff, and what has since been struck off it

Nineteen registers, each with an address and a value, where a working
transmitter holds something and this one holds nothing:

```
3ff73000 513b015c   3ff73004 0000402b   3ff73040 513b015c   3ff73044 0000402b
3ff73048 513b015c   3ff7304c 0000412b   3ff73068 ffffffff   3ff7306c 0000ffff
3ff7311c 3ffae000   3ff73124 3ffae000   3ff73148 00000900   3ff7314c 00003000
3ff73164 0000000c   3ff73800 00030000   3ff73804 00030000   3ff73c40 01e839e0
3ff73c54 00000404   3ff73c78 00000003   3ff73d40 00000010
```

Applied together with the address slots and the masks, transmit is still silent.
Applied individually, no single one has changed the outcome.

That is where this stops. Not because the list is exhausted — `0x3FF73C40`
holding `01e839e0` against zero is a pointer-shaped value nobody has chased —
but because the pattern of this session is now clear: **each register poke costs
a few minutes and eliminates one thing, and there is no evidence the answer is
in the register set at all.** The difference may be in ordering, in timing, in
the PHY block the dump does not reach, or in a ROM call neither side exposes.

`tools/idf_ref`, `reg_diff.py` and `wifireg` make any future attempt cheap. The
next person should start from §15.4 and should not start from a theory.

---

## 16. The 0x3FF73Cxx block, eliminated

`0x3FF73C40` was singled out in §15.4 as the one pointer-shaped value nobody had
chased: `01e839e0` in a working transmitter, zero here.

Written, and it held — reading back `0x01e839e0`, so the write was real and not
silently rejected. Receive unaffected at 359 frames. **Transmit unchanged:**
`frames addressed to us=0`, three times, active peer 30 cm away.

Then the rest of the block, since `0x3FF73C40`–`C78` reads like one functional
unit and a bisect was available if it worked:

| register | was | set to |
|---|---|---|
| `3ff73c44` | `078482bf` | `0404001f` |
| `3ff73c54` | `00000000` | `00000404` |
| `3ff73c5c` | `0fff0fff` | `ffff0fff` |
| `3ff73c6c` | `a5000c24` | `a5802d24` |
| `3ff73c78` | `00000000` | `00000003` |

Receive fine at 950 frames. Transmit still silent. **The block is eliminated.**

### 16.1 One register refused the write

`0x3FF73C68` was written with `00000000` and read back `40110011`. It is
hardware-driven or read-only, which is worth knowing: a difference at a register
software cannot set is not a difference software failed to set. At least one
entry on the §15.4 list was never actionable, and there may be others — a check
worth running before anyone spends time on the remainder.

### 16.2 The count

Across this report: the framing hypothesis, CCA, EDCA, `lmacInit`, `lmacInitAc`,
the power domain, the MAC address (twice, two different slots), the DMA pointer
pairs, and now the whole `0x3FF73Cxx` block. Every one cheap, every one
eliminating something, none radiating.

That is the evidence for §15.4's conclusion rather than an argument against it:
**there is still no sign the answer is in the register set.** The next person
should read §15.4 before poking anything, and should probably not poke anything.

