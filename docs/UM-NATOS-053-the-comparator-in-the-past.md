# UM-NATOS-053 — The Comparator in the Past

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-27 · Status: **nat-os serves HTTP over WPA2 continuously. The reset that bounded it every ninety seconds is found and fixed.**

---

## 1. Abstract

nat-os is a web server.

```
+0s +60s +120s +180s +240s :  HTTP 200, every one

    uptime: 589 s
    lwIP rx: 1329 (dropped 0)
    lwIP tx: 37 (errors 0)
    connections: 6
    requests: 6

resets in the whole capture: 0
```

UM-NATOS-052 ended with a page that was real, externally confirmed, and taken
away by a reboot roughly every ninety seconds. This report is the hunt for that
reboot, and it ends in three lines.

The shape of it:

- **Seven hypotheses eliminated by measurement**, none of them the answer, and
  every elimination correct. §3.
- **A blind spot that made them all miss**: the fault leaves no trace at the
  instant anything can observe it. §7.
- **Persistence**, which turned the page from something you had to catch into
  something that is simply there. §5.
- **The mechanism**, which was a single number read as signed. §6.
- **The fix**, which was a guard that could not tell *earlier* from *stale*. §7.

This report covers `next_moves/08` steps 267–273. UM-NATOS-052 covers 260–266.

---

## 2. State before and after

| | after 052 (step 266) | now (step 273) |
|---|---|---|
| HTTP over WPA2 | works, ~90 s at a time | **works, 589 s and counting** |
| watchdog reset | ~1 in 3, no mechanism | **mechanism found and fixed** |
| the network stack | inside one shell command's loop | **a task; survives the command** |
| bring-up poll | 600 s, and *was* the network | 60 s, and only bring-up |
| blob critical sections | **empty stubs** | implemented |
| tick line | the blob could disable it | guarded |
| eliminated causes | 3 | **7** |
| `TASK_MAX` | 12 | 13 |

---

## 3. Seven eliminations

Each of these was measured, not argued, and each was correct:

| # | hypothesis | how it died |
|---|---|---|
| 1 | task starvation | `starved` stayed 0 through a reset while feeds advanced (052 §7) |
| 2 | shell stack exhaustion | the same 236 bytes free in runs that never reset (052 §7) |
| 3 | lock contention | it was `g_shared_lock`, the torture-test mutex (052 §7) |
| 4 | the display task | froze it; the board still reset, lock free, history mixed (052 §8) |
| 5 | the blob's unprotected critical regions | implemented them; `crit0` at the reset. §4 |
| 6 | interrupt masking | `lvl 00000000` at every reset. §7.1 |
| 7 | the watchdog being reconfigured | `cfg 0xe01f8000/6000`, the armed values, at every reset |

Seven correct eliminations and no answer is not a failure of method. It is what
the method costs when the fault hides from the instrument — §7.

---

## 4. The blob's critical sections were never implemented

`wifi_osi_impl.c:1037` had carried a marker since the ISR path was written:

> *"The blob has its own answer — it wraps its critical regions in
> `_wifi_int_disable`/`_wifi_int_restore`, which mask interrupts globally — and
> those are already implemented here. **Whether that is sufficient is not yet
> measured, and this comment is the marker for it.**"*

They were not implemented:

```c
static uint32_t osi_wifi_int_disable(void *mux) { return 0; }   /* stub */
```

The blob had been entering every critical region it has, being told interrupts
were masked, and running with them fully enabled — putting **two contexts in
windowed vendor code at once**, the exact hazard the marker was left for.

That is the **sixth** entry found reporting success for work never done, after
`_event_post`, `_task_delay`, the event groups, `_queue_send_from_isr` and
`wpa_parse_wpa_ie`. The marker was right to exist and wrong about the facts.

Implemented with `rsil 3` and a level-only restore. **And it was not the
watchdog**: 1 reset in 6 against 3 in 8 is not a distinguishable rate, and the
one reset read `crit0` — the blob was not inside a critical region when the
board died.

A real defect, correctly fixed, and not the answer to the question that found
it.

---

## 5. The stack becomes a service

Until step 271 the whole network lived inside `net_poll_for()` — one shell
command's loop. When `wifiinit start` returned, nothing drained the ring or ran
lwIP's timers and the board went silent. **Every browser test was a race against
that window**, which is why three of them failed for reasons that had nothing to
do with the network.

`net_service_once()` is the loop body, factored out. A **net task** calls it
forever; `net_poll_for()` calls it as before; two flags ensure they never both
own the ring, which is single-consumer by design.

The poll drops from 600 s to 60 s — it is only the bring-up window now. The long
window existed *because the poll was the network*.

### 5.1 And the task ids shifted

```
before:  report=0 a=1 b=2 vm=3 apps=4 shell=5 disp=6 touch=7
now:     report=0 a=1 b=2 vm=3 apps=4 shell=5 net=6 disp=7 touch=8
```

**Every breadcrumb reading in steps 264–270 said "task 6" meaning the display.
From this build on, task 6 is the net task.** Recorded here and in the step log,
because a reader checking those steps against a current board would otherwise
draw the opposite conclusion from the same number.

`TASK_MAX` went 12 → 13: kmain creates nine and the blob takes two, so exactly
one slot was free and taking it would have cost the blob its second. One slot is
2 KB of DRAM against ~47 KB spare.

---

## 6. The mechanism: a signed comparison

The breadcrumb — RTC memory, written every scheduler entry, surviving the reset
— was widened step by step until it carried `CCOMPARE1 - CCOUNT`. Then:

```
LAST TICK : task 7 at tick 12623  lock7  lvl 00000000
            ahead 4208397466  late 2  cfg 0xe01f8000/6000  crit0
```

`ahead` is stored unsigned. Read as **signed 32-bit**:

```
4208397466 - 4294967296 = -86,569,830 cycles = -1.082 SECONDS at 80 MHz
```

**The comparator was armed 1.08 seconds in the past.** A second capture read
−64,463,462 — 0.81 s. An earlier one read +789,738: 9.87 ms, one interval,
correct. So it is an event, not the normal state.

`timer.c` drives the tick from **CCOMPARE1**, the core's own cycle comparator,
and it is **one-shot**. On Xtensa it asserts on the single cycle `CCOUNT` equals
it; if interrupts are masked for that cycle the match passes and is **gone** —
not latched. The next match is a full 2³² cycles away: **53.7 seconds**. The
watchdog fires at three.

That is every observation of the whole hunt in one number: the tick stops,
interrupts stay enabled, nothing is held, the watchdog config is untouched, and
the board reboots.

### 6.1 Why `timer.c`'s own guard never caught it

`timer_isr` already guards a bad deadline, and `g_late` counts it. `late` read
**2** all run, and neither was this.

**The guard lives inside the handler, and the handler is what is not running.**
A check that can only run when the fault is absent cannot catch the fault —
UM-NATOS-051 §10.1's instrument sharing a dependency with the thing it measures,
found again in a different disguise.

---

## 7. The fix, and why seven eliminations missed

`task_yield()` writes the comparator on **every voluntary switch**:

```c
uint32_t soon = xt_ccount() + 64u;
if ((int32_t)(soon - xt_get_ccompare1()) < 0) { xt_set_ccompare1(soon); }
```

*Only ever earlier, never later.* Correct, and paid for with a full debugging
session when writing it unconditionally froze the kernel — a task yielding
faster than 64 cycles pushed the deadline ahead forever.

**But it cannot tell EARLIER from STALE.** With the comparator 64 million cycles
behind, `soon - ccompare1` is large and *positive*; the guard reads that as
"already earlier" and declines to write. Every yield sailed straight past the one
write that would have restarted the clock. **The system had a rescue path running
constantly and refusing to use it.**

So the stale case is handled before the guard sees it:

```c
(void)timer_rescue();          /* re-arms only if the deadline has passed */
uint32_t soon = xt_ccount() + 64u;
if ((int32_t)(soon - xt_get_ccompare1()) < 0) { xt_set_ccompare1(soon); }
```

Also called from `osi_wifi_int_restore()`, one of the windows that eats the
match.

### 7.1 The blind spot

Seven hypotheses were eliminated correctly and none was the answer, because the
fault leaves no trace at the instant anything can observe it.

**The tick dies while interrupts are masked.** By the time any code runs to
record anything, they are unmasked again and the level reads 0. `lvl 00000000`
at every reset was a *true measurement of the wrong instant* — and it is what
retired the interrupt-masking hypothesis in step 268, correctly on its own terms
and wrongly in fact.

The only thing that survived the masked window was a value written *before* it
and read *after*: the comparator itself. Nothing about the fault was visible;
only its residue.

### 7.2 A static inline, for a reason paid for five times

`osi_wifi_int_restore()` is **windowed**. A windowed `call8` into a call0
function is the violation this project has paid for five times, so `timer_rescue`
lives in `timer.h` as a static inline with its state exported. Verified in the
disassembly: that function contains **no call of any kind**.

It is deliberately **not** wired into `phy_exit_critical()`, which also lowers
the level — `phy_host.c` is compiled into the pre-linked blob as well as the
kernel, and an inline there would reference kernel globals the blob cannot
resolve.

---

## 8. Method

### 8.1 An instrument that survives the fault

Everything in this report rests on the RTC breadcrumb of UM-NATOS-052 §7:
`TG0WDT_SYS_RESET` resets the digital core and the RTC domain keeps its
contents. It was **validated against a forced fault** — the shell's `hang`
command — before being trusted, and then widened five times: task, tick history,
panel lock, blob critical depth, interrupted PS, interrupt-level history,
watchdog registers, and finally the comparator.

Each widening eliminated a hypothesis. Only the last one answered anything,
which is the normal ratio and not a complaint.

### 8.2 Read the sign

`ahead 4208397466` is a large positive number and means −1.08 seconds. Printed
unsigned it looks like a comparator armed 52 seconds into the future, which is a
different and much less interesting bug.

**The whole hunt turned on reading one field as signed.**

### 8.3 Rate before conclusion, again

Step 270 recorded a non-result that would have been easy to dress up:

```
step 263 baseline        3 / 8
display frozen           1 / 4
critical sections real   1 / 6
level history            2 / 2
watchdog registers       2 / 2
comparator instrument    0 / 6
```

Read a build at a time it looks like signal. It is a ~35% coin. No build changed
the rate, including the two that fixed real defects, and saying so plainly is
what kept the search pointed at the fault instead of at a false fix.

The same caution applies to the fix in §7: **one ten-minute run is one run.**
The rate was ~1 in 3, and this file has been wrong about small samples five
times. That is written into step 273 as the next task rather than smoothed over.

---

## 9. What is on the network

```
ssid       ivory-billed         WPA2-PSK / CCMP
station    5c:01:3b:50:3f:64    aid 12
address    192.168.1.140        DHCP
service    HTTP on port 80      6 connections, 6 requests, 0 errors
uptime     589 s and counting   0 resets
```

---

## 10. What remains

1. **Confirm the reset rate is zero rather than lower.** §8.3.
2. **The first-ARP question** (UM-NATOS-052 §6.2): both successful fetches in
   this project were made after pinging. Windows should ARP on its own.
3. **The step-259 anomaly**, now much smaller: reading `TIMG0_WDTCONFIG0` from
   the *status line* reliably stops the crypto; reading it from the *feed path*
   does not. Not the register — the call site.
4. The `hist` nibble rendering, which prints task 10 as `:`.
5. Group-key rekeying, roaming, PMKSA caching, WPA3/SAE, and the all-channel
   scan that has panicked since step 202.

**A from-scratch operating system, on a radio it reverse-engineered the
interface to, over an encrypted link it negotiated itself, serving a page and
staying up to do it.**
