# UM-NATOS-034 — The Second Receiver

**Used Medias LLC — Embedded Systems Division**
Revision 2.8 · 2026-08-19 · Status: **The analog bus is visible** — §27 captures 5,095 regi2c transactions of the PHY's RF calibration; the comparison needs nat-os's APP CPU started — §22 fixes a real 9.5 dB TX-power defect that is not the cause; §24 verifies the descriptor layout; §25 finds the last candidate was never in the archive this report named, transcribes it in 251 bytes with no new blob, and eliminates it too

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

---

## 17. Is this the same as open-mac, and is it possible at all?

Two questions asked at the end of the session. The second deserves a straight
answer rather than a hedge.

### 17.1 The same job, one layer apart

Yes, substantially. `wifimac_tx()` was transcribed line by line from open-mac's
`transmit_80211_frame` and verified against it register by register
(UM-NATOS-028 section 3.2). Both projects replace Espressif's closed MAC
software with their own register writes while still calling the closed PHY blob.
Both read the same undocumented block at 0x3FF73xxx.

**But open-mac runs on top of ESP-IDF, and that is the whole difference.**

open-mac replaces the MAC *protocol* layer only. Before a line of its code runs,
ESP-IDF has already done the power domain, the clocks, the coexistence arbiter,
the PHY init and lmacInit -- the entire undocumented bring-up. It inherits a
radio that is *already keyed up*, and answers the question "how do I frame and
queue a packet on it".

nat-os declined all of that, so it must perform the bring-up itself, and **the
bring-up is the part nobody wrote down.** That is exactly why the three things
open-mac calls and nat-os does not turned out to be `esp_wifi_power_domain_on`,
`coex_bt_high_prio` and `lmacInit` -- all initialisation, none of it protocol.

So nat-os is attempting something **strictly harder** than open-mac. open-mac
stands on Espressif's initialisation and rewrites what sits above it; nat-os
stands on nothing and must rediscover what sits below. And the layer open-mac
actually contributes -- the protocol -- is the part that already works here.
`wifimac_tx()` has been verified twice and is not the problem.

### 17.2 Is it possible?

**Yes.** Four reasons, in order of weight.

**Receive already works, from scratch.** This kernel brings up the PHY, tunes a
channel, arms DMA, recycles descriptors and decodes 802.11 frames with no
ESP-IDF underneath it. That is not a small piece of the same problem, it is most
of it, already done.

**Nothing is sealed.** ESP-IDF is open source and on this machine. The blob is a
binary we possess and can disassemble. The hardware is on the desk and answers
in seconds. There is no cryptographic gate, no signed firmware, no missing key
-- only undocumented ordering.

**The OS shim already exists.** `wifi_osi_impl.c` implements the interface the
vendor stack expects and `ositest` exercises it end to end. The usual blocker
for running vendor networking code outside its RTOS is already solved here.

**A method exists that cannot fail in principle.** Below.

### 17.3 Why the diff failed, and what would not

The differential compared **destinations, not routes.**

A snapshot shows state. Hardware bring-up depends on *sequence*, and on
transient values that do not survive to be photographed. A register written to 1
and then back to 0 appears as 0 in both dumps and vanishes from the diff. So the
nineteen candidates of section 15.4 may not contain the answer at all -- not
because the method is bad, but because the answer may be an order rather than a
value.

What would work is a **trace**: every register write ESP-IDF makes from reset
through its first successful transmission, in order, with values, then replayed.

That is obtainable. ESP-IDF's register accesses go through macros, and
instrumenting them logs every C-level write with address, value and order --
which covers all of esp_wifi's init, the part nat-os is missing. The blob's
internal writes are not caught that way, but **both stacks call the same blob
identically**, so replaying the C-level sequence around the same blob calls
should converge. Espressif also maintains a QEMU fork with ESP32 support that
can trace memory accesses including the blob's.

None of that is exotic. It is tedious, mechanical, and not blocked.

### 17.4 The honest estimate, and the real question

Weeks to months of focused work. Not an afternoon, and not a hundred sessions of
poking registers -- poking is the thing that does not scale. Twenty eliminations
in one session and no defect found, because each poke tests one hypothesis and
there are thousands of them. A trace tests all of them at once.

But possibility is not priority, and the strategic question is separate from the
technical one.

**nat-os's founding constraint is no ESP-IDF.** WiFi on this chip appears to
require an initialisation sequence that exists only inside ESP-IDF and the blob.
Reproducing it is possible, and is also, precisely, reimplementing the thing the
project exists to avoid. That tension is real, and it is a decision about what
nat-os *is* rather than a bug to be fixed.

An SX1262 has no equivalent problem. It is a documented SPI peripheral with a
published register map: write what the datasheet names, in the order it gives,
and the radio keys up. There is no "part Espressif never wrote down", which is
the thing that has cost four sessions here.

So: possible, yes. Worth it, only if owning this particular radio matters more
than the months. That is not a question this report can answer.


---

## 18. The trace, and the first new evidence in four sessions

**Added revision 1.9, 2026-08-19, same day as UM-NATOS-035.**

§13 ended on a sentence that turned out to be the whole problem:

> the snapshot diff compares destinations, not routes.

Thirty stable register differences were applied wholesale and nothing changed.
That eliminated the shortlist and left two shapes of cause that a snapshot
cannot reach by construction:

- **order** — both firmwares arrive at the same state by different paths, and
  the hardware cares which;
- **transients** — a write to a self-clearing bit. A "go" bit reads back zero a
  microsecond later, so no number of snapshots, however carefully filtered, can
  ever contain it.

The second would explain every observation in this project at once: completions
counted, canary intact, receive unaffected, nothing on the air.

### 18.1 The instrument

`tools/idf_ref` now traces instead of only dumping. Core 1 reads a 16-register
window into DRAM as fast as the bus allows, with no comparison and no branching
beyond the loop; core 0 transmits into the middle of that; the buffer is
differenced afterwards and printed as an ordered list of changes.
`tools/serial/tx_trace.py` drives the sweep and reduces the output.

**Measured, not estimated:** 1.92 µs per sample. The design predicted 0.55 µs
from 8 cycles per word; a read from the MAC peripheral bus costs about 28
cycles, not 8. So the blind spot is three times wider than designed — anything
faster than ~2 µs can still be missed — and it is recorded here rather than
quietly corrected, because **the width of this instrument's blind spot is the
one number a reader needs in order to judge a negative result from it.**

Two design errors were found and fixed by running it:

1. **One frame per window was not enough.** `esp_wifi_80211_tx()` queues to the
   WiFi task and returns; when the hardware is actually touched is decided by a
   scheduler this code does not control. The first sweep's only active window
   came back empty on the second pass — not noise, a broken experiment. Eight
   frames spread across the window fixed it, and changed what silence *means*:
   with one frame, an empty window was ambiguous; with eight, it is evidence.

2. **The first sweep covered the wrong 1 KB.** `0x3FF73000`–`0x3FF73400` was
   chosen because every register this project had found interesting fell inside
   it. Fifteen of those sixteen windows were completely silent, and the two
   addresses §12 spent a session on — `0x3FF73C40`, `0x3FF73C68` — were outside
   the swept range entirely. The sweep is now the full 4 KB, 64 windows.

A third correction was to `tx_trace.py` itself, which printed *"nothing but
clocks (both)"* for windows where **nothing had changed at all**. Thirty-two
thousand samples of a register that never moved is not "only counters moved". It
was the instrument claiming to have seen something it had not, and it is now
labelled `SILENT`.

### 18.2 What it found

Forty-four of sixty-four windows are silent across 32,768 samples each. The
transmit path is live in **41 addresses**, and they are concentrated:

| region | changes | shape |
|---|---|---|
| `0x3FF73C00` | 4092 | free-running — changes every sample |
| `0x3FF73D80`, `DB4` | 4183 | the busiest structured block |
| `0x3FF732F0`–`FC` | 1347 | four registers moving together |
| `0x3FF73834` | 382 | monotonically counting down |
| `0x3FF73428`, `73424` | 172 | paired |
| `0x3FF73CB8` | 98 | toggles `0x2000`/`0x4000`/`0` |
| `0x3FF73D84`–`D8C`, `DAC`, `DB0`, `DB8` | ~400 | counters and a state machine |

`0x3FF73DB8` is the one worth staring at. It cycles, repeatedly, and each cycle
is accompanied by `0x3FF73D8C`, `73D88` and `73D84` each incrementing once:

```
0x000  ->  0x210  ->  0x230  ->  0x020  ->  0x000
```

`0x210` is bits 4 and 9. `0x230` adds **bit 5**, and bit 5 is gone again one or
two samples later. Post, trigger, in flight, done — the shape of a transmit slot
being used, with a self-clearing bit in the middle of it.

That is a candidate, not a conclusion. It is named here so that the next session
does not have to re-derive it, and so that if it turns out to be wrong there is
a record of why it looked right.

### 18.3 The comparison

nat-os's `wifimac.c` names **25** MAC registers. Of those, **21 are silent
during ESP-IDF's entire transmit.** The overlap is four:

```
0x3FF73C48   0x3FF73CC8   0x3FF73D1C   0x3FF73D20
```

And `wifimac_tx()` itself — verified line by line against `hal_mac_txq_enable`,
and this project's own code rather than a blob call — writes exactly two
addresses inside the traced range: `0x3FF73D1C` and `0x3FF73D20`, slot 0. The
rest of its writes go to `0x3FF742xx`, which is outside it.

**So nat-os's transmit touches 2 of the 41 registers ESP-IDF's transmit
touches**, and none of the busy structured block at `0x3FF73D80`–`0x3FF73DB8`.

### 18.4 What this does and does not establish

It has to be said plainly, because the number above is the most suggestive thing
this project has produced on WiFi and suggestive is exactly where it has gone
wrong before.

**Three reasons the comparison is weaker than it looks:**

1. **A transmit-time trace cannot see initialisation.** A register written once
   at setup and never again is silent in every window. nat-os calls
   `ic_mac_init` and `lmacInit`, which are blob code and write registers
   `wifimac.c` never names. "Not in the source" is not "not written".

2. **Not every change is a write.** `0x3FF73C00` changing at every one of 2,047
   possible transitions is a free-running counter, not something ESP-IDF wrote.
   Several of the 41 will be hardware-updated status and statistics, which nat-os
   would be wrong to write.

3. **The two firmwares are not doing the same job.** §13 already paid for
   forgetting this: the reference was a SoftAP and nat-os a minimal sniffer, and
   most of the differences were legitimate. It is a promiscuous raw injector now,
   which is much closer, but "closer" is not "identical".

**What it does establish** is narrower and still worth the session: the transmit
path's live register set is now *known*, it is 41 addresses rather than a 4 KB
block, and 44 windows of it are provably not involved. Every previous attempt
searched a space nobody had bounded.

### 18.5 The next step, stated exactly

**Run the same tracer on nat-os and diff the two traces.**

This is the comparison the instrument was built for, and it is immune to all
three caveats above — because a blind spot present in both instruments cannot
manufacture a difference, only hide one. It is the same argument the second
receiver rests on, and that is the argument that settled a question three months
of single-board tests could not.

One obstacle, and it is real: **nat-os is single-core.** The tracer works by
capturing on one core while the other transmits. nat-os would have to start the
APP CPU — unstall it through `DPORT_APPCPU_CTRL` and point it at a capture loop
that touches nothing else. That is a bounded piece of work and squarely this
project's kind, but it is not free, and it should be recorded as the cost before
anyone starts.

**The lead most worth testing first**, and it is cheap: have nat-os write the
`0x3FF73DB8` sequence — `0x210`, then `0x230`, then watch bit 5 clear — around
its existing `wifimac_tx()`. If the radiated-signal test from §5 stays silent,
that is one more clean negative. If it does not, this is over.

---

## 19. The lead in §18 was wrong, and the instrument that replaced it

**Added revision 2.0, 2026-08-19.**

§18.5 proposed writing `0x3FF73DB8`'s cycle — `0x210`, then `0x230`, watching
bit 5 self-clear — into `wifimac_tx()`, on the reading that it was a control
register with a self-clearing "go" bit that nat-os never writes.

### 19.1 It is read-only

Probed with the MAC running and the receiver armed — the exact state in which
ESP-IDF's trace shows it cycling:

```
   register     wrote      read-back   verdict
   0x3FF73DB8   ffffffff   00000000    READ-ONLY
   0x3FF73DB8   55555555   00000000
   0x3FF73DB8   00000230   00000000
   0x3FF73DAC   ffffffff   00000000    READ-ONLY
   0x3FF73DB0   ffffffff   00000000    READ-ONLY
   0x3FF73DB4   ffffffff   0ff00a06    READ-ONLY, and free-running
   0x3FF73D1C   ffffffff   0f3fffff    WRITABLE   <- positive control
   0x3FF73D20   ffffffff   3fffffff    WRITABLE   <- positive control
```

The positive controls matter: `0x3FF73D1C` and `0x3FF73D20` are the two
registers `wifimac_tx()` actually writes, and they accept every pattern. So the
probe works, and the rejection is real.

**Bit 5 self-clears because the hardware clears it.** nat-os does not write
these registers because it *cannot*. The proposed fix would have been code that
provably does nothing.

The error is worth naming, because §18.4 listed it as a caveat and then reasoned
past it: *"not every change is a write."* A polling trace cannot tell a software
store from a hardware update, and §18 picked the busiest structured block and
called it a control path.

### 19.2 What they are instead: an instrument

Read-only status is worth more than the lead it replaced. Every transmit counter
nat-os owns is its own bookkeeping — `hardware=`, `completions reaped=`,
`chain acks=` — and this project has a rule about that:

> a successful completion count is not evidence.

These are inside the MAC. `txwatch` samples them around one posted frame.

**And it needed two corrections before it said anything true.**

The first window was 64 samples ≈ 100 µs, and showed nat-os reaching `0x258` and
stopping. That would have been reported as "nat-os never completes a transmit".
It is wrong: ESP-IDF's own completion comes ~320 µs after entering `0x258`. At
768 samples / 728 µs, nat-os completes the full cycle. *A measurement that ends
before the event cannot be told apart from an event that never happens.*

The second was the absence of a control. With one, the MAC turned out to run
`0x210 → 0x230 → 0x020` — the exact pattern §18 identified as the transmit
signature — **while nothing was being transmitted at all.**

### 19.3 The result

Ten trials each, one posted probe request versus nothing posted, same session,
same channel, receiver armed throughout:

| | control | transmit |
|---|---|---|
| `0x058` seen | **0/10** | **5/10** |
| `0x258` seen | **0/10** | **8/10** |
| `0x210 → 0x230` seen | 3/10 | 2/10 |
| `0x3FF73D84` steps | 8 | 2 |
| `0x3FF73D88` steps | 9 | 2 |
| `0x3FF73D8C` steps | 12 | 10 |

Three things follow.

**nat-os's transmit does reach the MAC.** `0x058` and `0x258` never occur idle
and occur in most transmit trials. The state path
`0x000 → 0x058 → 0x258 → 0x220 → 0x020 → 0x000` is caused by `wifimac_tx()` and
by nothing else. This kernel is not failing to arm the hardware.

**`0x210 → 0x230` is background.** It appears in both columns at the same rate,
so it is not a transmit signature — it is the receiver, the TSF timer, or beacon
timing. §18's central identification is retracted.

**The counters are background too.** `D84` and `D88` step *less* during transmit
than idle, which is what receive-side counters do when the radio is busy
elsewhere.

### 19.4 Why this is progress

The evidence about the transmit fault has, until now, been entirely negative:
§5's second receiver hears nothing. "Nothing on the air" is consistent with a
fault anywhere from frame construction to the antenna.

This is the first **positive**, hardware-sourced statement about where the
boundary is. The MAC runs a complete transmit state cycle, distinguishable from
its idle behaviour, in response to nat-os posting a frame. Combined with §5:

> the MAC completes a transmit cycle **and** nothing reaches the air

which puts the fault after the MAC's state machine and before the antenna —
independently corroborating §5's RF/PHY conclusion from the MAC's own registers
rather than from an absent signal. Two unrelated instruments now agree, and
neither is nat-os's own bookkeeping.

### 19.5 Next

**Run the idle control on `tools/idf_ref`.** §18's trace was taken during
transmit bursts, so its `0x210 → 0x230` activity is a mix of transmit and
background, and nothing there separates them. The same two-column experiment on
the reference board answers the question this whole line of work has been
circling: *what does ESP-IDF's MAC do that nat-os's does not, when both are
posting a frame and everything else is subtracted?*

That is a small change to `idf_ref` — a pass with no `esp_wifi_80211_tx()` —
and it is the first version of this comparison with a control on both sides.

---

## 20. A control on both sides, and what is left

**Added revision 2.1, 2026-08-19.**

§19 ended by naming the one experiment this line of work had never run: the same
idle control on `tools/idf_ref`. §18's trace was taken during transmit bursts,
so its activity was a mix of transmit and background with nothing separating
them.

`idf_ref` now captures each window twice, **back to back**: once with no
`esp_wifi_80211_tx()` at all, once with a burst of eight. Back-to-back because
ambient traffic varies over minutes, so a control taken later would be a weak
one. `tools/serial/tx_control.py` pairs them.

### 20.1 The result

`0x3FF73DB8` transitions on the reference board, by mode:

```
--- idle (no frame sent) ---          --- transmitting ---
    000 -> 210   x10                     000 -> 210   x8     <- also idle
    210 -> 230   x10                     210 -> 230   x8     <- also idle
    230 -> 020   x10                     230 -> 020   x7     <- also idle
    020 -> 000   x10                     000 -> 258   x10    <- NEVER idle
    (nothing else)                       258 -> 220   x9     <- NEVER idle
                                         220 -> 020   x11
                                         258 <-> 259  x98    (parked, toggling)
```

**`0x210 -> 0x230` is background on ESP-IDF too.** It appears at the same rate
with and without a transmit, on the reference board, against its own control.
§18's central identification is now refuted twice by independent experiments —
once on nat-os (§19), once here.

**ESP-IDF's real transmit signature is `-> 0x258 -> 0x220 -> 0x020`.** Those
transitions occur ten and nine times with a burst and *zero* times without.

### 20.2 The two MACs do the same thing

Set the two transmit-caused paths side by side:

```
ESP-IDF   000 -> (058) -> 258 -> 220 -> 020 -> 000
nat-os    000 ->  058  -> 258 -> 220 -> 020 -> 000
```

They are the same path. nat-os's MAC runs the transmit state machine ESP-IDF's
MAC runs, in the same order, through the same states, including the `258 <-> 259`
toggle while parked.

Two boards, two firmwares, two independently-derived controls, agreeing.

### 20.3 What this settles, and what it costs

**Settled: the fault is not in the MAC layer.** That is where nat-os's own code
lives — `wifimac.c`, `wifimac_tx()`, the register writes this project
reconstructed from disassembly — and at the MAC's own status registers the two
firmwares are now indistinguishable. Combined with §5:

> nat-os's MAC executes the same transmit sequence as a working stack's MAC,
> **and nothing reaches the air.**

The fault is below the MAC and above the antenna: the PHY/RF path.

This is the third instrument to reach that conclusion and the first to reach it
positively. §5 inferred it from an absent radio signal, §19 from nat-os's own
MAC completing a cycle; this one from a working stack and a failing one being
identical where they can be compared.

**The cost is that it points at the blob.** The MAC is nat-os's code and could be
fixed. The PHY is 847 KB of Espressif's binary that this project calls but does
not control, and §16 already established it cannot be replaced from public
information. The remaining difference is in *how nat-os calls `libphy`* — the
calibration sequence, its arguments, or the state it expects — not in any
register nat-os writes directly.

That is a smaller search space than the project has had at any previous point,
and it is a less tractable one.

### 20.4 What is worth doing next

Two things, and the second is the honest one.

**A `phyinit` argument and sequence differential.** nat-os calls
`register_chipv7_phy` with an init-data blob and a calibration mode; ESP-IDF
calls it with data derived from NVS-stored calibration and a mode chosen by
whether calibration data is valid. Those arguments, and what ESP-IDF does to the
PHY *after* that call, are the remaining candidates. Unlike the register hunt,
this is a bounded list.

**And the strategic answer §17 already gave.** An SX1262 over SPI3 has no PHY
blob, no undocumented calibration, and a published register map. The hardware is
ordered. Every session spent here buys function on one radio; the LoRa path buys
the same capability on a radio this project can actually own — which is what
`docs/conceptual/the-ark-and-fiendnet.md` needs it for.

The transmit investigation is not abandoned. It is now correctly bounded, which
is what four sessions of register work was actually for.

---

## 21. The two-board test, measured rather than inherited

**Added revision 2.2, 2026-08-19.**

§20 concluded that the transmit fault is below the MAC, by pairing a measurement
made that day — the two MACs execute the same transmit sequence — with §5's
"nothing reaches the air", which is from an earlier session and was not re-run.

Worse, §5 had a gap of its own. Its receiver was validated against **a real
access point**. It was never pointed at `tools/idf_ref`. So one assumption was
carrying the weight of the whole investigation without ever being measured:
*that a receiver which hears an access point would also hear a nearby ESP32.*

Both boards were run together, both directions, each with a control on its own
receiver.

### 21.1 The wrong answer on the way

The first attempt put `idf_ref` in AP mode — so it would beacon for direction A —
and left promiscuous receive on for direction B. In that combination it heard
**four frames in forty seconds**, and could not hear the access point nat-os
hears continuously. ESP-IDF's promiscuous receive is all but disabled in AP mode.

The script reported *"CONTROL PASSES, TEST FAILS — the expected result"* anyway,
from a deaf receiver. It also compared every received address against `None`,
because its regex looked for a colon-separated MAC that `machw` does not print:
a detector that could not have succeeded.

Two failures, both producing the expected answer. That is the worst way for a
test to be wrong, and it is the third time in this investigation that an
instrument has agreed with the hypothesis by not working —
`wifi_sweep.py`'s frame-count detector, §18's missing control, and now this.

> **A negative from an instrument that has not been shown to work is not a
> negative. It is nothing at all.**

The two directions need two separate builds, and `REF_LINK_AP` now selects one.

### 21.2 Direction A — can nat-os hear `idf_ref`?

`idf_ref` as an AP beaconing `NATOS-CTRL-6` on channel 6. nat-os's `scan`:

```
frames=366  recycled=366  networks=2
5c:01:3b:51:2b:41  x175  "NATOS-CTRL-6"     <- idf_ref
44:25:38:19:0d:1a  x10   "TC7NR"            <- ambient AP, the control
```

**175 beacons, by BSSID and by name.** nat-os's receiver hears this transmitter,
easily, and hears an unrelated one at the same time.

### 21.3 Direction B — can `idf_ref` hear nat-os?

`idf_ref` back in STA + promiscuous, receive only. nat-os beaconing and sending
probe requests continuously for 45 seconds, 30 cm away, same channel:

```
idf_ref heard 315 frames from 3 sources
   44:25:38:19:0d:1a  x291      <- ambient AP, the control
   6e:a6:d8:b0:78:b1  x21
   e2:49:d2:8f:5e:6d  x3

nat-os MAC = 5c:01:3b:50:3f:64  (decoded from machw's 0x503b015c 0x0000643f)
```

**Zero.** A receiver taking 315 frames from three transmitters over the same
45 seconds records nothing at all from the board sitting next to it.

### 21.4 What is now on measured ground

| | control | result |
|---|---|---|
| `idf_ref` → nat-os | nat-os also hears an ambient AP | **175 frames** |
| nat-os → `idf_ref` | `idf_ref` hears an ambient AP ×291, plus two more | **0 frames** |

Two boards, 30 cm apart, one channel, one session. One of them transmits and is
heard 175 times. The other transmits and is heard zero times. Both receivers are
proven working, in the same run, by transmitters neither of them controls.

§5's foundation is confirmed. It is no longer inherited, and the receiver in it
is no longer assumed to be able to hear an ESP32 — it demonstrably can.

So §20's conclusion stands on two measurements from the same session rather than
one plus a memory:

> nat-os's MAC executes the same transmit sequence as a working stack's MAC
> (§20), **and** a proven receiver 30 cm away hears nothing from it (§21).

The fault is below the MAC and above the antenna. Nothing in the record now
rests on an unmeasured assumption about either instrument.

---

## 22. The arguments, compared

**Added revision 2.3, 2026-08-19.**

§20 and §21 bounded the fault to the RF/PHY path and named what was left: *how
nat-os calls `libphy`*. `register_chipv7_phy` takes three arguments. Here they
are, against `esp_phy/src/phy_init.c`.

### 22.1 Argument 1 — the init data. Six bytes were wrong.

A byte-for-byte diff of nat-os's `g_phy_init_data[128]` against ESP-IDF's
`phy_init_data.h`, with `LIMIT()` evaluated and `CONFIG_ESP_PHY_MAX_TX_POWER`
at its default 20:

```
   idx   ESP-IDF   nat-os    delta   note
    44       78       40    +9.5dB  TX power  11b 1/2 Mbps
    45       72       40    +8.0dB  TX power  11b 5.5/11
    46       66       40    +6.5dB  TX power  11g 6-24
    47       60       40    +5.0dB  TX power  11g 36/48
    48       56       40    +4.0dB  TX power  11g 54
    49       52       40    +3.0dB  TX power  11n MCS7
```

**Six of 128 bytes differ, and all six are the transmit power table.** The other
122 match exactly.

ESP-IDF writes them as `LIMIT(CONFIG_ESP_PHY_MAX_TX_POWER * 4, 40, 78)` and so
on, with

```c
#define LIMIT(val, low, high) ((val < low) ? low : (val > high) ? high : val)
```

so the values actually passed are `80` clamped to each ceiling: 78, 72, 66, 60,
56, 52. nat-os's table held `40, 40, 40, 40, 40, 40` — **the macro's LOW bound,
copied six times instead of evaluated.** Units are 0.25 dBm, so nat-os was
asking for 10.0 dBm where the reference asks for 19.5.

Fixed.

### 22.2 Argument 2 — the calibration buffer's MAC field

```c
ESP_ERROR_CHECK(esp_efuse_mac_get_default(sta_mac));
memcpy(cal_data->mac, sta_mac, 6);
register_chipv7_phy(init_data, cal_data, calibration_mode);
```

`esp_phy_calibration_data_t` is `version[4]`, `mac[6]`, then the opaque
remainder. nat-os passed a zeroed `.bss` buffer, so a MAC of all zeros.

Its role is to tie stored calibration to the chip that produced it, and with
`PHY_RF_CAL_FULL` — which recalibrates regardless — it may well not matter.
Filled anyway. Leaving a known difference in place while hunting an unknown one
is how this investigation has previously lost sessions.

### 22.3 Argument 3 — the calibration mode. Matches.

nat-os passes `PHY_RF_CAL_FULL`. ESP-IDF passes `PHY_RF_CAL_FULL` on the
no-NVS path, and on the NVS path falls back to it whenever stored data fails to
load — which is every first boot. No difference.

### 22.4 The result: both fixed, and it still does not transmit

Verified in the linked image (`4e 48 42 3c 38 34` at `g_phy_init_data + 44`),
`phyinit` returns 0, and the two-board rig from §21:

```
idf_ref heard 52 frames from 2 sources over 45 s
   44:25:38:19:0d:1a  x37       <- ambient AP, control passes
   6e:a6:d8:b0:78:b1  x15
   5c:01:3b:50:3f:64  ABSENT    <- nat-os
```

**Still nothing.** Which was the expectation rather than a surprise: 10 dBm is
10 mW, audible across a room, so a 9.5 dB shortfall was never going to explain
total silence at 30 cm. The defect is real, it is in the RF domain, it is fixed,
and it is not the cause.

Recorded as a clean negative. Two concrete differences in the PHY arguments
have been found and eliminated, and the argument list is now exhausted.

### 22.5 What is left in the surrounding sequence

The arguments match. What does not is what ESP-IDF does *around* the call, in
`esp_phy_enable()`:

| ESP-IDF | nat-os | verdict |
|---|---|---|
| `esp_phy_common_clock_enable()` → `DPORT_WIFI_CLK_WIFI_BT_COMMON_M` = `0x3c9` | ungates `0x3c9` | **identical** |
| `phy_update_wifi_mac_time(false, ts)` before the clock enable | not called | candidate |
| `coex_bt_high_prio()` after init, unconditional on ESP32 | not called | **not available** |
| `esp_phy_release_init_data()` | n/a, static table | not applicable |

`coex_bt_high_prio()` is the one previously listed as a lead and then recorded
as absent from the archives. That stands, and the reason is now clear: it lives
in `libcoexist.a`, which nat-os does not link and which is not among the two
archives this project has. It is called unconditionally on ESP32 by every
working stack.

That is a short list, and it is the last of the bounded ones.

---

## 23. `phy_update_wifi_mac_time` — eliminated by reading it

**Added revision 2.4, 2026-08-19.**

§22.5 left two candidates in the sequence around `register_chipv7_phy`. This is
the first, and it is eliminated without running anything.

It is not a blob symbol. It is `static inline` in ESP-IDF's own
`esp_phy/src/phy_init.c`, so the source is simply there to read:

```c
static inline void phy_update_wifi_mac_time(bool en_clock_stopped, int64_t now)
{
    static uint32_t s_common_clock_disable_time = 0;

    if (en_clock_stopped) {
        s_common_clock_disable_time = (uint32_t)now;
    } else {
        if (s_common_clock_disable_time) {
            uint32_t diff = (uint64_t)now - s_common_clock_disable_time;
            if (s_wifi_mac_time_update_cb) {
                s_wifi_mac_time_update_cb(diff);
            }
            s_common_clock_disable_time = 0;
        }
    }
}
```

On the enable path `en_clock_stopped` is `false`, so it takes the `else`. The
guard is `s_common_clock_disable_time`, a static initialised to zero, **and the
only assignment to it is from the `en_clock_stopped == true` branch — which is
reached only from `esp_phy_disable()`.**

nat-os enables the PHY once at boot and never disables it. So on nat-os's path
that static is permanently zero, the body never executes, and the call is a
no-op. Were it somehow to execute, it would invoke
`s_wifi_mac_time_update_cb`, a callback nat-os has never registered. A double
no-op.

**What it is actually for:** compensating the WiFi MAC's notion of time across a
clock-disable/enable cycle — light sleep, or an explicit
`esp_phy_disable()`/`esp_phy_enable()` pair. It exists to close a gap that only
opens when the radio has been powered down and brought back. nat-os has never
powered it down, so there is no gap.

Eliminated by proof rather than by experiment, which is the cheaper and the
stronger of the two. Nothing was implemented, because implementing it would have
meant writing a function that provably does nothing.

### 23.1 The list is now one item long

| candidate | status |
|---|---|
| the three arguments | **exhausted** (§22) — one real defect found, fixed, not the cause |
| clock ungate `0x3c9` | identical |
| `phy_update_wifi_mac_time` | **no-op on this path** (§23) |
| `coex_bt_high_prio()` | called unconditionally on ESP32 by every working stack; lives in `libcoexist.a`, which this project does not have |

Everything bounded and available has been checked. What remains needs a third
Espressif archive, added to a project whose direction has been removing them, to
pursue a capability the SX1262 provides with no blob at all.

That is not a technical dead end — `libcoexist.a` is obtainable. It is a
decision about what nat-os is, and §17 and `docs/blob-free.md` have both already
argued which way it goes.

---

## 24. Open-MAC community intelligence, checked

**Added revision 2.5, 2026-08-19.** Findings from the `ESP32-MAC-Reversing`
community, supplied by the project owner, checked against this codebase.

### 24.1 The `ebuf` / `lldesc` layout — already correct, and now proven

The headline item: Espressif published internal buffer headers in
`esp-extconn/priv_include/target/esp32/if_ebuf.h`, removing the need for blind
structure-padding analysis.

This mattered because nat-os's TX descriptor was reconstructed by guesswork, and
a wrong descriptor would explain **every observation in this investigation** — a
MAC that runs its full transmit state machine (§20) while nothing demodulable
reaches the air (§21), because the DMA engine fetched from a garbage pointer.

It is not wrong. Compiled with the target toolchain and read out of `.rodata`,
one field at a time:

| nat-os field | resulting word | ESP-IDF `LLDESC_*_MASK` |
|---|---|---|
| `owner` | `0x80000000` | `OWNER 0x80000000` |
| `has_data` | `0x40000000` | `EOF 0x40000000` |
| `_unknown` | `0x3F000000` | `offset:5` + `SOSF 0x20000000` |
| `length` | `0x00FFF000` | `LENGTH 0x00fff000` |
| `size` | `0x00000FFF` | bits 0–11 |

`sizeof` 12, `packet` at offset 4, `next` at offset 8 — identical to `lldesc_t`
on all three. The `__attribute__((packed))` collapses the two `uint16_t`
bitfields into a single 32-bit word, which is exactly what the hardware expects.

A negative, but a useful one: this was the strongest remaining hypothesis and it
is now eliminated by measurement rather than left standing as an assumption.

*Method note:* the first pass at this reasoned that `uint16_t length : 12`
could not begin at bit 12 of a 16-bit storage unit and therefore had to spill,
making every subsequent field wrong. That reasoning was plausible and false.
Compiling it settled the question in one command.

### 24.2 What the report made visible: §21 had a blind spot

Not one of the community's findings, but found while chasing them — and it
undermines a conclusion this report published three sections ago.

`tools/idf_ref` used ESP-IDF promiscuous mode with default filtering.
`WIFI_PROMIS_FILTER_MASK_FCSFAIL` is documented *"do not open it in general"*
and is **off by default** — so the receiver in §21 silently discarded every
frame that arrived corrupt.

That is precisely the ambiguity §1 says this whole line of work exists to
remove:

> An AP that ignores a malformed frame looks exactly like a radio that never
> transmitted.

A promiscuous receiver with default filtering has the same blindness. §21's
"nat-os heard 0 times" was therefore consistent with two entirely different
faults: nothing radiating, or something malformed radiating.

The filter is now open — `WIFI_PROMIS_FILTER_MASK_ALL` plus
`WIFI_PROMIS_CTRL_FILTER_MASK_ALL` — and corrupt frames are counted separately
through `rx_ctrl.rx_state`.

### 24.3 A broken detector, caught — the third this session

The first run with the filter open reported bad-FCS frames rising from 625 to
832 while nat-os transmitted, and concluded *"something IS radiating and is
malformed."*

**That was wrong.** Those are cumulative counters read at two different times;
the second window is simply later. It is the frame-count-delta detector
`wifi_sweep.py` was retired for, rebuilt from scratch.

Re-run as alternating windows, differencing the counter per window and
comparing *rates*:

```
   window        all frames/s   bad-FCS/s
   silent  1         0.59        0.59
   transmit1         0.66        0.66
   silent  2         2.22        1.78
   transmit2         2.43        1.89
   silent  3        29.15       24.05
   transmit3        27.40       22.94

   mean bad-FCS/s  silent=8.81  transmitting=8.50  delta=-0.31
   spread (pstdev) across all windows = 10.51
```

The ambient corrupt-frame rate varies by a factor of forty on its own. The
transmitting mean is *lower* than the silent mean, and both are buried inside
the spread. **No separation.**

### 24.4 What this leaves

§21's conclusion stands, and is now stronger than when it was written:

> A working receiver 30 cm away, explicitly configured to report frames that
> fail their checksum, records no rise in corrupt frames while nat-os
> transmits continuously.

Not merely "nothing demodulable" — **nothing at all, including malformed frames
the receiver was configured to catch.**

Stated limit: the ambient bad-FCS rate here swings between 0.6/s and 24/s, so a
very low rate of corrupt transmissions could hide inside it. Six windows
excludes a signal comparable to ambient, not a weak one.

### 24.5 Community findings not yet used

Recorded so the next session does not re-derive them:

- **Ghidra function discovery** — search for the byte pattern `36 x1 01`, then
  key `d` then `f`. That is the Xtensa `entry` prologue, and it finds function
  boundaries Ghidra's auto-analysis misses in the blobs. Applicable if `libpp`
  is ever disassembled further.
- **Analyse the `.a` archives, not the linked golden binary** — the archives
  retain inline strings and compiler annotations that the link discards.
- **OS adapter table hooking** for ISR-context callbacks. The technique stands;
  the defect it was attached to does not — see §24.6.
- **`PLCP0` = `tx_config`** in the community's naming. nat-os treats these as
  two registers four bytes apart (`0x3FF73D1C` and `0x3FF73D20`), both writable
  and both written. Worth settling which convention is right if the register map
  is revisited.
- **`HT-SIG` and the gap at `0x3FF74260`/`0x3FF74264`.** nat-os writes PLCP1,
  PLCP2 and DURATION but nothing between PLCP2 and DURATION. Those two words are
  HT-only in the community's map and nat-os transmits at non-HT rates, so this
  is expected — but it has not been confirmed.

### 24.6 The OSI table is not incomplete. Correcting §24.5.

§24.5 listed a standing defect: `ositest` reporting `osi vtable checks = 0x2e,
INCOMPLETE (want 0x3F)`, two of six checks failing. The bitmask decodes as
`0x01` malloc/free, `0x02` semaphore, `0x04` queue, `0x08` event group, `0x10`
free-heap-size, `0x20` random — so `0x2e` means **both heap checks failed** while
the pool-backed ones passed. A blob unable to allocate memory through the OS
adapter would have been an excellent candidate for a MAC that runs but does not
radiate.

**It does not reproduce.** `0x3F ALL PASS`, every time:

| condition | result |
|---|---|
| framebuffer on, 89 KB free | `0x3f ALL PASS` |
| framebuffer off | `0x3f ALL PASS` |
| after `wifipd on` / `phyinit` / `macinit` — the state that matters | `0x3f ALL PASS` |
| the exact earlier sequence: `wintest 12`, `vendorcall 10`, `romcall`, then `ositest` | `0x3f ALL PASS` |
| twice in a row, to check for stickiness | `0x3f ALL PASS` |

The cause of the single `0x2e` is unknown and is not being guessed at here. What
is certain is that it is not a standing defect, and that §24.5 recorded it as one
**on the strength of a single observation, made in passing, while checking
something else.**

That is the process error worth keeping. This report has spent four sections on
detectors that agreed with a hypothesis by being broken; this is the mirror
image — one unverified reading promoted into the record as a known fault, where
it would have cost the next session a day. A number seen once is an observation.
It becomes a defect when it reproduces.

---

## 25. `coex_bt_high_prio` — no third blob was needed, and it is not the cause

**Added revision 2.6, 2026-08-19.**

§22.5 named this as the last bounded candidate and said it "lives in
`libcoexist.a`, which this project does not have", framing the next step as a
decision about adding a third vendor archive.

**That was wrong, and it was asserted rather than checked.**

### 25.1 It is not in `libcoexist.a`

`libcoexist.a` contains `coex_bt_request` and `coex_bt_release` and no
`coex_bt_high_prio` at all. The symbol is defined in
`esp_phy/lib/esp32/librtc.a`, object `bt_bb.o`:

```
librtc.a        180K   0000003c T coex_bt_high_prio
librftest.a     1.2M   U
libbttestmode.a 1.4M   U
libnet80211.a   1.4M   U
libpp.a         576K   none
libphy.a        840K   none
```

Three archives merely reference it. One defines it, and it is not the one this
report named.

### 25.2 It did not need linking. It needed transcribing.

251 bytes, and every instruction is a load, a mask, and a store. No calls, no
loops, no branches, no data structures. Its literal pool is eight peripheral
addresses in the `0x60000000` alias, which on ESP32 mirrors `0x3FF40000`:

| literal | actual | block |
|---|---|---|
| `0x3FF5C080` | — | BT baseband |
| `0x600310D0` | `0x3FF710D0` | BT/PHY |
| `0x60031300` | `0x3FF71300` | BT/PHY |
| `0x60033D30` | **`0x3FF73D30`** | **WiFi MAC** |
| `0x60033D38` | **`0x3FF73D38`** | **WiFi MAC** |
| `0x60033D40` | **`0x3FF73D40`** | **WiFi MAC** |
| `0x600041C4` | `0x3FF441C4` | GPIO |
| `0x3FF5D040` | — | BT baseband |

**Three of the eight are WiFi MAC registers in the transmit block** — the same
block as this kernel's `0x3FF73D1C`/`0x3FF73D20` transmit config and the status
registers traced in §19–§20. A function named for Bluetooth coexistence
configures the region this investigation has spent four sessions inside. Which
is exactly why it was worth transcribing rather than dismissing by its name.

Reimplemented as the `coexprio` shell command, order preserved exactly as
disassembled.

### 25.3 It applies, it persists, and it changes nothing

```
before: D=0x00003202  E=0x0c800000  F=0x00000000
after : D=0x01403203  E=0x0c80000a  F=0x00000010
```

The writes take. And re-reading after `macinit` shows the values unchanged —
`before` equals `after` on the second invocation — so the MAC bring-up does not
clobber them.

Tested in both orderings, because order has mattered in this project before:

| sequence | result |
|---|---|
| `phyinit` → `macinit` → `chan` → `macrx` → **`coexprio`** | not heard |
| `phyinit` → **`coexprio`** → `macinit` → `chan` → `macrx` (ESP-IDF's order) | not heard |

Receiver 30 cm away with the FCS-fail filter open, hearing 1,400+ frames from
two dozen other transmitters throughout. nat-os's MAC address appears zero
times. `txstat` reports `hardware=382 completions reaped=295 chain acks=75`,
unchanged.

### 25.4 What this settles

**No third vendor archive was ever required.** The decision §22.5 framed as
strategic — add `libcoexist.a` to a project whose direction is removing blobs —
turned out not to exist. The function was transcribable, and transcribing it
cost less than the paragraph arguing about whether to link it.

**And the last bounded candidate is eliminated.** Every item on §23.1's list has
now been checked:

| candidate | status |
|---|---|
| the three PHY arguments | exhausted (§22); one real 9.5 dB defect found and fixed |
| clock ungate `0x3c9` | identical |
| `phy_update_wifi_mac_time` | no-op on this path (§23) |
| `coex_bt_high_prio` | **applied, persists, changes nothing (§25)** |
| `ebuf`/`lldesc` descriptor layout | verified bit-for-bit correct (§24.1) |
| OS adapter table | complete, `0x3F ALL PASS` (§24.6) |

The MAC executes the same transmit sequence as a working stack (§20). A proven
receiver hears nothing, including malformed frames (§21, §24.2). Every argument,
every adjacent call, and every data structure has been checked against the
reference and matches.

Whatever is missing is not in a list this project can enumerate any more.

### 25.5 A note on the two wrong assertions

§22.5 stated the symbol's location as fact without looking, and that error
would have led to adding an unnecessary vendor binary — the precise thing the
project's direction is against. §24.5 recorded a one-off `ositest` reading as a
standing defect.

Both were written in summary sections, at the end of long sessions, about things
adjacent to the work rather than in it. The measured findings in this report have
held up; the asides have not. That is worth knowing about how this record is
produced: **the confidence of a sentence in a report should track how it was
arrived at, and in this document that has not always been true.**

---

## 26. The region nobody looked at

**Added revision 2.7, 2026-08-19.**

§20 concluded the fault was **below the MAC and above the antenna**. §22 through
§25 then searched PHY *arguments*, an adjacent *call*, a *descriptor layout* and
an *OS table*.

Every instrument in this investigation — `tools/idf_ref`'s `dump_all`, nat-os's
`regdump`, and the tracer sweep — covered exactly three regions:

```
0x3FF00000  DPORT      64 words
0x3FF48000  RTC        64 words
0x3FF73000  MAC      1280 words
```

`grep` for the PHY and baseband blocks across every instrument returns **zero**.
The search was narrowed to the PHY and then continued everywhere except the PHY.

### 26.1 Closed

Three ranges added to both firmwares, extents taken from the addresses
`coex_bt_high_prio` itself writes:

```
0x3FF5C000  bb0    512 words
0x3FF5D000  bb1    512 words
0x3FF71000  phy   1024 words
```

Restoring `idf_ref`'s second `dump_all()` — lost when the link test replaced the
sweep — and widening `reg_diff.py`'s timeouts for a dump that grew from 1,408 to
3,456 registers.

### 26.2 The result: one register out of 2,048

| region | stable differences |
|---|---|
| buffer RAM `0x3FF74000+` | 245 (random on both sides, always excluded) |
| MAC | 59 |
| DPORT | 17 |
| RTC | 15 |
| **PHY + baseband (2,048 registers)** | **1** |

```
0x3FF5D040   ESP-IDF 40000000   nat-os 80000000   xor c0000000
```

`0x3FF5D040` is one of the eight addresses `coex_bt_high_prio` writes — its last
operation is `[0x3FF5D040] &= 0x7FFFFFFF`, clearing bit 31. This differential ran
without `coexprio`, which is why nat-os still had bit 31 set. But ESP-IDF also
has **bit 30 set**, and nothing in the transcribed function sets it.

Written directly: `0x80000000 → 0x40000000`, matching ESP-IDF exactly, and it
persisted through 45 seconds of transmitting. **Still not heard.**

### 26.3 What this actually establishes

This is a negative, and it is the most informative one in the report.

> The entire memory-mapped state of the MAC, the PHY and the baseband is now
> verified equivalent between a stack that transmits and one that does not.

2,048 PHY and baseband registers, one difference, matched, no change. Add the
MAC executing the same transmit sequence (§20), the same arguments (§22), the
same descriptor layout (§24.1), and a working receiver hearing nothing including
malformed frames (§24.2).

Every register the CPU can address is the same. Nothing comes out.

### 26.4 So the difference is somewhere a register dump cannot reach

That is not a shrug; it names a specific place.

**The ESP32's radio is not configured entirely through memory-mapped
registers.** The analog front end — VCO, PLL, filters, I/Q, the transmit chain —
is programmed over an internal I²C bus, reached through `rom_i2c_writeReg`. This
project already uses that mechanism: `kernel/clock.c` programs the BBPLL through
it, because there is no register-level path and the analog I²C master is not in
the TRM.

**Those writes are invisible to every instrument in this report.** `libphy` can
issue hundreds of them inside `register_chipv7_phy` and no register dump, no
snapshot diff and no tracer will show a single one.

Which is consistent with everything observed: identical memory-mapped state,
identical MAC behaviour, a PHY that returns 0, and no RF.

### 26.5 The next instrument, and why it is different in kind

The regi2c *host* interface is memory-mapped even though the analog registers
behind it are not. `ANA_CONFIG_REG` is at `0x6000E044`, which through the
peripheral alias is `0x3FF4E044`.

So the analog programming **can** be traced, by watching the I²C host block
rather than the analog registers — the same core-1 tracer already built for §18,
pointed at `0x3FF4E000` during `register_chipv7_phy` on both boards.

That is a different kind of evidence, not another candidate off a list. It is
also the first proposal in this investigation that would show what the blob
*does* rather than what it leaves behind.

Cost: real. `register_chipv7_phy` runs for milliseconds and issues an unknown
number of transactions; capturing them needs a wider buffer than the 3.9 ms the
tracer currently holds, and the two boards must be doing genuinely the same job
for the comparison to mean anything — a condition §13 already paid for once.

### 26.6 Honest status

Not stuck for want of ideas. Stuck in the sense that everything cheap is done,
and what remains is either that trace, or an SDR to answer the one question no
instrument here has ever answered directly: **does any RF energy leave this board
at all?**

Every negative in this report has been consistent with two possibilities —
"transmits nothing" and "transmits something unreceivable" — and §24.2 narrowed
that only as far as an ESP32 receiver can see. A spectrum analyser or an
RTL-SDR answers it in one look, and costs less than another session.

---

## 27. Watching the analog bus

**Added revision 2.8, 2026-08-19.** §26 ended by proposing this and calling it a
different kind of evidence. It is, and it works.

### 27.1 Finding the host

§26's argument: the analog front end is not memory-mapped, but the I²C host that
reaches it is ordinary hardware, so the traffic can be watched even though the
destinations cannot.

`ANA_CONFIG_REG 0x6000E044` is the only address ESP-IDF publishes, and `soc.h`
has no `DR_REG_*_BASE` covering `0x3FF4Exxx` at all — the block is undocumented.
So rather than assume, the `i2cprobe` command performs a real transaction and
reports which words moved:

```
rom_i2c_readReg(0x66, 4, 0) returned 0x00000018
0x3ff4e010  0x01c60566 -> 0x00180066
```

One word. Decoded against the call:

| bits | field | value |
|---|---|---|
| 7:0 | block | `0x66` = `I2C_BBPLL` |
| 15:8 | register | `0x00` |
| 23:16 | **data** | `0x18` — exactly what the call returned |

The *before* value is what confirms it beyond doubt: `0x01C60566` is block
`0x66`, register `0x05`, data `0xC6`. Register 5 is `I2C_BBPLL_OC_DCUR` and
`kernel/clock.c` writes it `(3<<6)|6 = 0xC6`. **The word still held this
kernel's own last PLL write.**

### 27.2 The instrument, and the mistake inside it

A change-recording capture on core 1: it appends only when the word differs, so
coverage is bounded by the number of transactions rather than by elapsed time.
That matters because `register_chipv7_phy` runs for 55 ms and §18's fixed-rate
tracer holds 3.9 ms.

The first run captured **78 transactions, every one to block `0x66`** — the
BBPLL, the CPU clock, the single analog block with nothing to do with the radio.

The instrument was pointed at the one word that could not contain the answer.
That is §26's mistake — bound the search to the PHY, then look everywhere else —
repeated at a smaller scale, four hours later, by the same process.

nat-os's `phyi2c` settled it, by snapshotting the whole host block either side of
`phyinit_run()`:

```
0x3ff4e000  00000000 -> 00170063   blk=63
0x3ff4e004  00000000 -> 00000562   blk=62
0x3ff4e008  00000000 -> 0147026b   blk=6b
0x3ff4e00c  00000000 -> 01800168   blk=68
0x3ff4e010  01c60566 -> 00c60566   blk=66   <- the one being watched
```

Five words, five different analog blocks. ESP-IDF publishes headers for exactly
two of these — BBPLL and APLL. **`0x62`, `0x63`, `0x68`, `0x6B` are the
undocumented RF blocks**, and the capture was watching none of them.

### 27.3 The result

Widened to all five words. `docs/data/i2c-trace-idf-phyinit.json`:

**5,095 transactions across eight analog blocks, over 55.4 ms.**

| block | transactions | |
|---|---|---|
| `0x62` | 3007 | undocumented RF |
| `0x63` | 861 | undocumented RF |
| `0x64` | 707 | undocumented RF |
| `0x6b` | 241 | undocumented RF |
| `0x67` | 157 | undocumented RF |
| `0x66` | **56** | BBPLL — the only block the first capture saw |
| `0x6a` | 45 | undocumented RF |
| `0x68` | 21 | undocumented RF |

**Fifty-six of five thousand and ninety-five.** The first capture saw 1.1% of the
traffic, and the 98.9% it missed is the entire radio.

The opening of the sequence, in order:

```
   idx      us   blk reg data
     0     0.0   6a  02  68
     2     6.0   6a  00  26
     6    94.5   62  00  00
     7    95.7   62  00  30
     8   101.2   62  01  30
     9   102.5   62  01  80
    10   106.3   62  02  08
    11   110.0   62  03  08
    19   142.7   63  01  f3
    21   143.5   6b  03  a8
    27   154.1   6b  04  07
    31   161.4   62  0a  b0
```

This is the ESP32's RF calibration writing to its analog front end. Nothing in
this project has ever seen it, and as far as this author knows it is not
published anywhere.

### 27.4 Stated limits

- **The trace is a lower bound.** Five words is ~140 cycles, about 0.6 µs at
  240 MHz, against transactions observed ~1 µs apart. Some are missed. It records
  what happened *at least*, never *at most*.
- **Duplicate suppression.** Two identical consecutive transactions to the same
  word collapse to one. Visible in the excerpt above — `6a 02 68` at index 0 and
  1 are two separate captures of the same value from different words, whereas a
  genuine repeat would vanish.
- **Only the five block words are watched.** `phyi2c` showed other words in the
  host block move too (`0x3FF4E02C`, `40`, `44`, `50`, `54`), one of which
  carried block `0x64`. So the word-to-block mapping is not one-to-one and the
  capture may still be partial.

### 27.5 What is now needed, and what it costs

The reference sequence exists. The comparison does not, because **nat-os is
single-core**: `register_chipv7_phy` runs synchronously and there is no second
core to sample from while it does.

That is the APP CPU work §19.5 already priced: unstall through
`DPORT_APPCPU_CTRL_A..D` and point it at a capture loop in IRAM that touches
nothing else. Bounded, well-understood, and not free.

With it, the two sequences can be diffed directly — and both boards call the same
blob with arguments already verified identical (§22), so **any difference at all
in 5,095 transactions is the answer**, and no difference at all is a negative
strong enough to close the RF hypothesis for good.

For the first time in this investigation the next step produces a decisive
result in either direction.

### 27.6 A note kept from §25.5

That section observed that this report's measured findings hold up while its
asides do not. §27 is the third instance in one session of the same specific
error: narrow the search correctly, then aim the instrument somewhere else. §26
did it with the PHY blocks, §27.2 did it with the host word, and both were caught
only by widening on a hunch rather than by design.

The pattern is worth naming because it is not carelessness — each aim was
*plausible*. The defence is not to be more careful; it is to make the instrument
cover the whole of the region the argument names, and to treat a narrow aim as a
claim that needs its own justification.
