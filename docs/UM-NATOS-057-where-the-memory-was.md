# UM-NATOS-057 — Where the Memory Was

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-07 · Status: **41 KB of instruction RAM and 3.7 KB of heap recovered. Almost none of it was deleted.**

---

## 1. Abstract

```
                  before      after
iram free            848     41,040      of 131,072
heap              30,216     33,896
```

This report covers `next_moves/08` steps 349–352. UM-NATOS-056 covers 341–348.

§3 is the part worth reading: **40 KB of the 41 KB was not freed, it was
moved** — and the reason it could be moved is a rule this project got wrong,
enforced, and then had corrected by its own build system.

Also here: the bring-up/join restructure that removed two workarounds rather
than adding a third (§5), and the step-319 panic finally identified from three
numbers and a linker script (§6).

---

## 2. Two kinds of memory for code

An ESP32 runs code from two places, and the difference is the whole of §3.

**iram** — 128 KB of internal RAM at `0x40080000`. Fast, always readable, and
**the only place code can execute from when the flash cache is unavailable.**
Interrupt handlers live here. So does the flash driver itself.

**irom** — flash, memory-mapped through a cache. There is 4 MB of it and the
whole kernel image is 225 KB, so it is effectively unlimited. The catch: while
the SPI flash bus is being driven for something else — an erase, a write — the
cache cannot serve reads, and code executing from there faults.

**Everything defaults to iram.** Moving an object to irom is one line in
`kernel/linker.ld`:

```
*lwip_*.c.o(.literal .literal.* .text .text.*)
```

So "reclaiming iram" mostly means deciding, per file, whether it may live in
flash. That decision needs a rule, and the rule is where this got interesting.

---

## 3. How 40 KB moved

iram had **848 bytes free of 131,072** — 0.6% — and had already refused kernel
instrumentation twice: step 328 could not fit a 20-byte counter, step 341 was
88 bytes short of a preferences module.

The largest single occupant was **lwIP: 40,023 bytes of `.text`** across 23
objects. TCP in, TCP out, DHCP, ARP, the pbuf allocator, the timers.

It has no business being in iram, and the argument is short:

- every lwIP entry point runs **on the net task** — `netif_wifi_input()` from
  the drain loop, `sys_check_timeouts()` from the tick, the TCP and UDP
  callbacks from those two
- **none of it runs from an interrupt handler**
- **none of it touches the flash bus**
- and a flash erase masks interrupts, so no task switch can land inside one:
  lwIP *cannot* be executing while the bus is taken

One line in the linker script. `.text` went **129,200 → 89,008**.

**Nothing was deleted and nothing got slower in any way that matters.** lwIP
executes from flash through the instruction cache now, which is how the majority
of this kernel has always run.

### 3.1 The rule that made it invisible

This was available for months. What hid it was a rule this project wrote for
itself and got too broad.

Step 292 spent three rounds and a white screen finding that `wificred.c` — which
drives `flash_read`/`erase`/`write` — had been placed in irom. The conclusion
recorded was:

> *"code that touches the flash bus must not live on it"*

Under that rule lwIP looks questionable: it is *near* the network, the network
is *near* the flash records, and nobody wants to be the person who put the TCP
stack in flash and spent a week on it.

Step 316 wrote a **build check** to enforce that rule mechanically. It
immediately failed the build on **`kmain.c`**, which calls `flash_read_id()` in
the boot banner on every single boot and has worked since the beginning.

So the rule was wrong. `flash.c` masks interrupts for the whole transaction, so
while the bus is taken **only `flash.c` itself executes**. Its callers are not
running. Their placement is irrelevant. The rule that survives is narrow:

> **the flash DRIVER must not live on the flash bus.**

And step 291 had the evidence all along: moving `wificred.c` to iram to fix a
white screen **did not fix it** — step 292 found the real cause was a call0 into
windowed code. The placement change was never shown to fix anything, and a rule
was generalised from it anyway.

**A check is a claim about the system.** Writing this one down forced the claim
to be exact, the exact claim failed on a file that had always worked, and the
correction is what made 40 KB obvious.

---

## 4. The 3.7 KB of heap, which was deleted

Static DRAM comes straight out of the heap — the heap is whatever is left
between `_bss_end` and the stack — and the heap is where the WiFi driver
allocates. Two buffers were sized for capabilities that no longer exist:

- **`g_out`, 1,760 → 512.** The transmit buffer of the hand-written network
  path, sized to mirror a whole received frame so a large ping could be echoed.
  That path has not run since step 233 put lwIP in charge. The overflow guard
  already in the code turns anything larger into a declined reply.
- **`WEB_BODY_MAX`, 1,536 → 768.** The browser shows nine lines of thirty-nine
  columns. 768 is two screens of scrollback.

### 4.1 What is left, and why it stays

| | bytes | |
|---|---|---|
| `g_stacks` | 26,624 | 13 tasks x 2 KB, **one size for all** |
| `PBUF_POOL` | 9,216 | already halved from 12 buffers |
| `g_blob_stack` | 7,168 | what the vendor driver's task demands |
| `ram_heap` | 6,163 | lwIP's own, already cut from 16 KB |
| `g_q` | 4,800 | three full-size frames; 512-byte slots was step 333's bug |

**The one structural win was not taken.** `TASK_STACK_WORDS` is 512 words for
every task, and `task.h:197` already records that one size for all "stopped
being workable". The telemetry has printed the evidence for months —
`tightest stack=net 664/2048` — while the reporting, IPC and VM tasks use a
fraction of theirs. Per-task sizing returns 8–10 KB.

It is also a change to `task_create()` and the stack pool, which is the
scheduler. This log has enough examples of what that costs when done in passing.

---

## 5. The restructure that removed two workarounds

`wifi_bringup()` did two jobs welded together:

```
turn the radio on     blob_init, PHY, esp_wifi_init, esp_wifi_start
get onto a network    associate -> start the netif -> start DHCP
```

DHCP is a broadcast conversation. On a station that has not associated, nobody
hears it. So **"radio on, not connected" was not a state this system could be
in** — which is exactly the state a view that lists networks needs while the
user chooses.

Three attempts held that seam shut before it was opened:

| step | what was tried | what happened |
|---|---|---|
| 313 | skip the association | the DHCP broadcast still ran on a station that never tried. **The board rebooted.** |
| 347 | associate with an impossible SSID | sequence intact; DHCP started on a dead link and backed off |
| 349 | restart DHCP after a real join | worked — a patch on a patch |

Step 350 moved the data path into the join. The bring-up brings up a radio;
`join_named()` associates, waits, and **then** starts lwIP.

`wifi_data_path_start()` is **idempotent**, and that is the whole difficulty:
the bring-up ran it once by construction, but a join can happen many times and
`netif_wifi_start()` calls `lwip_init()` and `netif_add()`, which must not run
twice. First call brings lwIP up; every later one restarts DHCP — which is what
a second join means anyway.

**It was called "cleanliness, not capability" one step before it fixed a
capability bug**, and that correction is in the log where the claim was made.

---

## 6. The step-319 panic, identified

Filed unexplained once step 329 showed its `GRANT DRIFT`, `LOST` and
`multiframe` lines were pre-Tier-B instruments reporting normal operation. Three
numbers survived:

```
exccause : 29 (StoreProhibited)    excvaddr : 0x00000000    epc : 0x40080009
```

`linker.ld:97` puts `.vectors.window.of4` at `_vecbase + 0x000`, and `_vecbase`
is `0x40080000`. **The faulting instruction is nine bytes inside
WindowOverflow4** — the register-window spill handler, which stores the outgoing
registers to the *caller's* frame through `a5`.

A store fault there at a near-null address means the frame pointer it was handed
was **null**: the hardware was asked to spill a register window to nowhere. Step
30's *"garbage stack pointer"* and step 292's zeroed `a0`/`a1`, not a fault in
the scan.

**Not fixed.** The trigger is gone, it has not recurred, and the instruments
that appeared to corroborate it were measuring something else. `panic.c`
recognises the shape now and says so, so the next reader starts where this
finished rather than decoding an address.

---

## 7. What remains

1. **Per-task stack sizing** (§4.1) — 8–10 KB, and the scheduler.
2. **The null-`sp` fault** (§6) — identified, unreproducible.
3. **`term.c` and `notes.c` onto `keyboard.c`** — owed since step 285. Three
   copies of the multi-tap keyboard where there were two, made worse on purpose
   to keep two working apps off an untested module. That module has since run
   all session.
4. Group-key rekeying, roaming, PMKSA caching, WPA3/SAE, and the all-channel
   scan that has panicked since step 202.

**41 KB of instruction RAM recovered by reading a linker script correctly, and
3.7 KB of heap by deleting two capabilities nothing had used in a hundred
steps.**
