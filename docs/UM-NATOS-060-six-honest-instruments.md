# UM-NATOS-060 — Six Honest Instruments

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-08 · Status: **The networking stack builds, associates, handshakes and binds an address again. Six of the seven faults found getting there were in the measurements, not the system.**

---

## 1. Abstract

```
                      before        after
-WiFi build      iram +10,551   links, 299,616 bytes
                 dram +27,828
the radio        took the USB link down     up and stable
the join         "join failed"              associates in 2 ticks
the handshake    m1=0, unexplained ~140 steps   done=1 micbad=0
DHCP             "no offer"                 bound, 192.168.1.140
```

This report covers `next_moves/08` steps **372–385**. UM-NATOS-059 covers
367–371.

The networking stack was verified working in August, and had not been in a
buildable image for some time. It builds now, and every WPA primitive —
PBKDF2, SHA-1, HMAC-SHA1, AES key unwrap — is verified executing from flash
rather than RAM.

**§4 is the report.** Seven things were investigated as faults. **One was a
fault.** The other six were instruments reporting something true and *narrower*
than what they were read as, and the cost of that was measured in days, not
minutes.

---

## 2. The build, resurrected

`-WiFi` overflowed **iram by 10,551 bytes and dram by 27,828**. Found while
updating the README (§3), which is the only reason it was found at all.

**The DRAM half was 48 KB paid for an instrument that was off.** `appcpu.c`'s
regi2c capture is two 6,144-entry arrays that core 1 fills while watching the
PHY's analog bus — 49,152 bytes of static DRAM, most of the overflow.

Not dead code: a shell command dumps them, and `appcpu.h` records the sizing
being done carefully after an earlier version pushed `_bss_end` past `_heap_end`
and faulted a self-test on an arena that never existed. So they are **allocated
when armed and freed when disarmed**. Core 1 writes them with no lock, so arming
installs the buffers *before* the run flag, disarming clears the flag *first*,
and the capture loop refuses to run on a null pointer.

**The iram half was 16 KB moved** under step 352's rule: the flash driver must
not live on the flash bus, and neither may an interrupt handler; everything else
may be fetched through the cache. The WPA crypto runs on the net task during
association. `netif_wifi.c` and `tcpsrv.c` were held in iram by nothing but not
having been listed.

Listed one file at a time, **not** as `*wpa*.o`. `wpa_cb.o` and `wpa_hs.o` are
called *by the blob*, from a context this project has never established, and a
wildcard would have swept them in on the strength of their names.

### 2.1 The five kilobytes not taken

`wifimac.c` is 5,359 bytes with 66 references, **all from `shell.c`** —
apparently reachable only by typing a command.

It also contains `wifimac_isr()`, routed through `intr_route()`. **An interrupt
handler executing from flash is the one placement this kernel cannot survive.**
Checked before moving, which is why `ipc.c`'s 531 bytes were taken instead.

---

## 3. The front door was a census

The README had **zero** mentions of NatScript, the compiler, or the permission
manifest — the most significant thing built in twenty steps, invisible to
anyone arriving. It was also wrong about what it did describe: 12 syscalls (14),
37,248 bytes (235,600), 21 reports (59), 145 KB of DRAM free (≈33 KB of heap).

None were lies when written. Every one is UM-NATOS-059 §3's **census defect**: a
count, correct on the day, hardened into prose, never rechecked. **The README is
`ARENA_MAX 4` in English.**

Every number in it is now measured, and the language example was put through
`natc` rather than written from memory.

---

## 4. Six instruments

The finding. Each of these was read as a fault in the system, and each was a
measurement answering a question nobody had asked.

### 4.1 A log only one observer could read — step 375

Two joins failed. Four minutes and **91 KB** of serial captured across them, and
not one line came from the code that knew why: `wifiapp.c`'s `logln()` writes
into an on-screen ring buffer and stops there.

UM-NATOS-055 credits that log with naming three defects *"without a capture, a
reset or a theory"*, which is true and is why it exists. It also meant the most
informative diagnostic in the system was readable by exactly one observer, and
not the one holding the serial cable. **A diagnostic only one observer can read
is half a diagnostic.**

### 4.2 A wait that said nothing until it was over — step 379

The app reached `associating, waiting for the AP` and went silent for 117
seconds, while the kernel stayed alive with `fault=none` and every task READY.

That loop is bounded at 1000 ticks — ten seconds — and `wifi_joined()` is a read
of one variable. **A bounded wait outliving its bound by a factor of ten is the
interesting kind of impossible**, and the log said nothing about it because
nothing in it spoke until it was over.

Made to count out loud, it answered: `AP answered after ticks 2`. Twenty
milliseconds. The association was never slow.

### 4.3 A counter that only counted in passive mode — step 380

```c
if (g_hs_passive) { g_hs_msg1++; ... return 0; }
if (!g_hs_have_pmk || !buf || len < O_KD) { return 0; }   /* silent */
```

`m1` is incremented **only in passive mode**. In normal operation the next line
returns without counting when the PMK is absent.

So **`m1=0` has never meant "the access point sent nothing"** — it has meant
*"nothing arrived while a PMK was already in place"*. Steps 241–245 built two
hypotheses on the first reading. Step 246 concluded from it that the station
never associated. That reading stood for roughly **140 steps**.

Counting arrivals before every test gave `rx=2`, and the handshake completes.

### 4.4 Counters in code that cannot run — steps 381, 384, 385

`dhcp offer/ack`, `arp` and `icmp` are incremented inside net.c's hand-written
parser. `g_use_lwip` is set to 1 where it is defined and **nothing in the tree
assigns it**, so that parser has been unreachable since step 233.

They read zero for a hundred and fifty steps regardless of what the network did
— and step 380 read `dhcp offer/ack 0/0` as *"an encrypted link that gets no
DHCP response"* and named it the next problem. Step 381 withdrew it: lwIP had
bound `192.168.1.140` the whole time.

**A zero that cannot become anything else is not a measurement**, and printing it
beside real ones lends it their credibility.

Fixed in two steps for a reason. 384 stopped them being printed — a change that
cannot alter behaviour. 385 made the flag `static const` and let the **compiler**
drop the dead half: 2,806 bytes of `.text` and 532 of `.bss`, a deletion nobody
had to get right by hand, on a network stack that had started working three days
earlier.

### 4.5 A transient state sampled once — step 381b

With lwIP's real DHCP state finally visible, the first reading was
`state 8 tries 2 addr 0.0.0.0` — `DHCP_STATE_CHECKING`, which looks like a hang.

It is the ARP probe lwIP sends before accepting an address, and it resolves to
`BOUND` a moment later. **The first honest instrument still produced a wrong
conclusion**, because it was read once, from a report that fires around bring-up
and never again. Sampling three times turned "stuck at 8" into "8 then 10".

### 4.6 A flag set since step 318 and printed nowhere — step 383

`g_used_cached` records whether a join derived its PMK or took it from the
cache. Whether PBKDF2 — the four thousand rounds moved to flash — had ever
actually executed had to be argued from *"the network was forgotten, and forget
calls `pmkcache_forget`"*.

That argument is sound and it is not a reading. The field is printed now, and
the very next join read `cached=1`, because the derivation had already
re-populated the cache: **the instrument arrived one join too late to witness the
thing it was added for.**

---

## 5. The one real fault

The radio took the USB link with it. Twice, from two different entry points, the
serial link died at the instant the radio came up, and afterwards the port
re-enumerated while the board emitted nothing.

`GetOverlappedResult` failing with *access denied* is a **USB-level** failure —
the CH340 went away, and a hung ESP32 does not make a separate chip disappear.
Against a session in which enumeration failed repeatedly on its own
(`VID_0000&PID_0002`), the reading that fits is **supply**.

Moving the board to a different USB socket fixed it, and nothing in software
was involved. **Of the seven things investigated, this is the one that was
actually broken, and it was not code.**

---

## 6. `tools/board.py`

Written because §4.1 through §4.6 kept recurring in a capture script rewritten
from memory each session. Every guarantee in it is a failure this log had:

| the failure | what it does |
|---|---|
| empty logs read as *"the board is silent"* while a stale process held the port | open failure is loud, exits non-zero, **names the holding process** |
| a command typed into a still-booting board and lost | **probes for the prompt**, reports how long it took |
| a dropped link discarding everything captured | keeps it, with `[board] link lost` where it happened |
| a retry loop flashing **eight times** after succeeding once | matches esptool's own verification line |
| `COM5` hardcoded, for a board now on COM6 | **finds the port and names it** |

It was wrong on its first run: `await_shell()` waited for the boot banner, on the
theory that opening the port always resets the board. It does not always, and
against a board already up it waited 45 seconds for a line printed minutes
earlier — correct output with a false explanation attached, which is the disease
it was built to cure. It probes now.

**That is not an argument against the tool. It is the argument for running it
before trusting it**, which is the discipline the rest of this log applies to the
kernel.

---

## 7. The pattern

Six instruments, one shape: **each reported something true and narrower than the
question being asked of it.**

None was dishonest. `m1` really did count message ones in passive mode.
`dhcp offer/ack` really did count offers the hand-written parser saw. The
wifi log really did reach the panel. Every one of them was *correct* and
*misread*, and a wrong conclusion built on a correct number looks better
evidenced than a guess.

This log has spent seven reports on the opposite failure — a status claiming an
outcome for work that never ran. **This is its mirror: a status reporting a real
outcome for work nobody was asking about.** The first kind is caught by asking
"did that actually run". The second is caught only by asking **"where is this
number incremented, and does that code still run?"**

Three corrections follow from it, and they are cheap:

1. **Count at the point where the thing happens**, before any test that could
   discard it. `g_hs_rx` before the PMK check; `m1` after.
2. **Sample twice before calling a state stuck.** A number is a measurement of a
   moment, not a property of a build — the same error that published a wrong
   heap figure three times (059 §3, 378).
3. **Make a diagnostic reachable by every observer who needs it.** The panel and
   the serial line; on demand and not only at bring-up.

---

## 8. What remains

1. **A `cached=0` reading** (§4.6) — one forgotten network away from replacing
   an argument with a measurement.
2. **VM-08 proper.** Real image identity needs secure boot and a key burned into
   eFuses, which are one-time programmable. Deliberately not done to somebody's
   only board.
3. **`APP_MAX` is 4**, pinned by screen geometry rather than memory: the strip
   band is 224 to 288, exactly four at 16 pixels each.
4. **Per-task stack sizing.** 8–10 KB, and the measurement argues against doing
   it from a snapshot: `net` showed 332 bytes used and has been recorded at
   1,384 under lwIP load.
5. **The null-`sp` window fault** — identified at 351, never reproduced.

**A from-scratch kernel that associates to WPA2, completes a four-way handshake
with crypto it executes out of flash, binds a DHCP address, and can say clearly
which of those it did.**
