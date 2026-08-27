# UM-NATOS-052 — The Page, and the Reboot

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-26 · Status: **nat-os serves a web page over WPA2. The board reboots underneath it three times in eight.**

---

## 1. Abstract

A browser rendered a page served by this kernel over an encrypted link.

Every layer at once, with CCMP underneath it: 802.11 association, the WPA2
four-way handshake, an installed pairwise key, Ethernet, ARP, IPv4, TCP, HTTP.
UM-NATOS-050 did this on an open network; this is the same stack with the
encryption in place.

It also does not stay up, and most of this report is about why — because the
answer turned out to be nothing to do with WiFi at all.

Four things account for the distance:

- **A retraction.** The instrumentation added at the end of UM-NATOS-051 was
  stopping the machine it was installed to measure. §3.
- **The drain loop**, which was starving lwIP's timers and made TCP behave
  exactly like a link that loses packets. §5.
- **A rate**, finally, instead of a story: eight runs, and the watchdog is the
  blocker. §6.
- **A breadcrumb that survives a reset**, which named a culprit in one step
  after five steps of argument had named none — and then a controlled
  experiment cleared it. §7–§8.

This report covers `next_moves/08` steps 260–266. UM-NATOS-051 covers 236–259.

---

## 2. State before and after

| | after 051 (step 259) | now (step 266) |
|---|---|---|
| WPA2 association | reliable | unchanged |
| four-way handshake | completes | unchanged |
| DHCP over WPA2 | bound once | **6 runs in 8** |
| **HTTP over WPA2** | never attempted | **HTTP 200, confirmed externally** |
| lwIP timers | **never ran** | run every pass |
| watchdog reset | 2 in 4, no mechanism | **3 in 8, culprit narrowed** |
| watchdog evidence | none survives the reset | **RTC breadcrumb, validated** |
| eliminated as cause | starvation, stack, lock contention | **+ the display task** |

```
$ curl http://192.168.1.140/
HTTP 200   350 bytes

    nat-os
    This page was served by a from-scratch operating system over a
    Wi-Fi driver it reverse-engineered the interface to.
      uptime: 252 s
      lwIP rx: 465 (dropped 0)
      lwIP tx: 23 (errors 0)
      connections: 2
      requests: 2
```

---

## 3. The instrument that stopped the machine

UM-NATOS-051 §12 closed by adding a TIMG0 `WDTCONFIG0` readback to the periodic
status line, to catch the watchdog being reconfigured by the blob, and reported
that across a clean run the value never moved.

**Both halves were wrong, and the second caused the first.**

```
cap27 .. cap30     PMK ready after 15 s, full runs
cap31              FIRST run carrying the readback
                   -> stalls at [blobtask] wifi prio 23/25 -> HIGH
every run since    stalls identically
```

The values quoted came from the boot-phase status lines of a run that had
**already stalled**. Confirmed by A/B on the one call site with nothing else
changed:

| build | runs reaching `PMK ready` |
|---|---|
| before the readback | 4 / 4 |
| with the readback | **0 / 4** |
| readback removed | 4 / 4 |

Why a plain read of `0x3FF5F048` prevents the crypto from running is **not
known** and is not guessed at. It is now a one-line switch, which is a better
position than a mystery.

This is the same shape as step 211's retracted lock leak and worse in one way:
that phantom merely sent a reader to the wrong file, while this one **silently
disabled the subsystem it was installed to measure.** An instrument that stops
the machine reports nothing about the machine.

---

## 4. Four conclusions drawn from file sizes

Step 260 concluded, in bold, *"on WPA2 this station receives and does not
answer."* It answers. The other machine's ARP table resolves the address to this
board's own MAC.

Every observation behind that claim was taken **after** the poll had stopped
reporting — which step 260 recorded two sections later without connecting it to
the headline above it.

Worse, four conclusions in the same stretch were drawn from **byte counts**
rather than from the milestone the run was supposed to reach:

```
vfy.txt      12657 bytes -- called "stalled", CONTAINED "PMK ready after 15 s"
cap_ctrl      8561 bytes -- called "the constant print stalls too"
ctl, ctl2     8541 bytes -- called "the committed state is broken"
verify        8541 bytes -- called "the committed state is broken"
```

The last four have `rom stubs` count **zero**: the command was never delivered,
so the capture says nothing whatever about the build. On that basis §3's A/B was
briefly declared invalid and the fault "intermittent" — a retraction that itself
needed retracting.

**A capture without the command's first line is not evidence, and file size is
not a milestone.** The harness now retries until the command's first output line
appears, and labels a run that never delivers.

### 4.1 And a moving target underneath it

Two hours of the same stretch were spent diagnosing a board that had silently
re-enumerated from COM5 to COM3 to COM6, and orphaned python processes holding
the port across bash timeouts. Several confident statements about kernel
behaviour were, in fact, statements about a serial port that no longer existed.

The lesson is not "check the port". It is that **a test harness is an instrument
and deserves the same suspicion as any other** — the same suspicion this project
applies to a return code, a counter, or a scan record at a guessed offset.

---

## 5. The drain loop was starving lwIP's timers

```c
while (g_tail != g_head) {
    netif_wifi_input(...);
    g_tail = (g_tail + 1u) % NET_SLOTS;
}
```

**Unbounded.** On a busy network the ring is never empty — frames arrive as fast
as they are taken — so the loop never exits. `netif_wifi_tick()` sits directly
below it and never runs.

```
before   lwip report lines in an 85 KB capture:  2
after    lwip report lines:                     10
```

**The half that matters.** Input kept running and the timers stopped. lwIP's
input path drives TCP for a request that *arrives*, which is why the page ever
loaded at all. Its timer path is ARP expiry, DHCP renewal and above all **TCP
retransmission**.

A stack that receives but never retransmits answers instantly when nothing is
lost and hangs forever when anything is. That is precisely how the page behaved
across five attempts: mostly a timeout, occasionally instant. The bug was not in
the radio, the keys, or TCP — it was one missing bound on a loop.

Bounded to sixteen frames per pass: above the burst the ring holds between
iterations, far below where the timers starve.

---

## 6. A rate, at last

Eight runs of the fixed build, each taken to a lease and then fetched from
another machine:

| | count |
|---|---|
| command delivered | 8 / 8 |
| DHCP lease | 6 / 8 |
| **TG0WDT_SYS_RESET** | **3 / 8** |
| HTTP 200 | run 6 — lease, no reset, ARP resolved first |

**Every HTTP failure coincided with either a watchdog reset or a fetch made
without pinging first.**

This reprioritised everything. The watchdog had been carried for four steps as a
curiosity — noted, deferred, "not mechanised". It is not a curiosity. It is the
thing standing between nat-os and a working server, and at three in eight it
will defeat any attempt at persistence long before the design of the polling
loop does.

### 6.1 What a rate buys

Steps 253, 255 and 260 each drew a confident conclusion from a **single run**,
and each had to be retracted. Eight runs cost forty minutes and settled more
than those three steps of argument did.

**Do not diagnose an intermittent fault from one run.** It is written into the
step log because it has now been learned four times in that file.

### 6.2 One thing recorded and not explained

Both successful fetches in this project — step 261 and run 6 — were made *after*
pinging the board. A fetch without that has failed every time, including a run
that had a lease and no reset. Windows should ARP on its own before the SYN. It
may be a slow or dropped first ARP reply, which would be a real station-side
defect.

---

## 7. A breadcrumb that survives the reset

A watchdog reset is the one failure in this kernel that leaves **nothing** — no
exception, no register dump, no `LAST FAULT` record, just a reboot. Four steps
of argument produced no mechanism because every observation died with the board.

RTC slow memory does not die with it: `TG0WDT_SYS_RESET` resets the digital core
and the RTC domain keeps its contents. Three words written per tick — sequence,
current task, tick — are read back and printed on the next boot, guarded by a
magic so a power-cycled RTC reads as absent rather than as garbage.

**Validated before it was trusted.** The shell's `hang` command wedges the
system deliberately, so the recorder was proved against a watchdog reset *on
demand* rather than hoped at on a 3-in-8 fault:

```
hang
TG0WDT_SYS_RESET
boot #701
LAST TICK : task 4 at tick 1738 (6140 ticks that boot)
```

### 7.1 It named the display, three times of three

```
tasks : report=0 a=1 b=2 vm=3 apps=4 shell=5 disp=6 touch=7

run 11:  LAST TICK : task 6 at tick 21524
run 12:  LAST TICK : task 6 at tick 19410
run 13:  LAST TICK : task 6 at tick 15706
```

Every hypothesis until then had pointed at the WiFi side — the blob, the poll
loop, the crypto, the TIMG0 register. The display task was never a suspect; it
is the only task untouched by any of this work.

### 7.2 Widened, it looked conclusive

Adding the eight most recent task ids and the panel-lock holder:

```
run 22:  task 6 at tick 24040  lock6  hist 6:234566
run 23:  task 6 at tick 25680  lock6  hist 66666666
```

`lock6` in both — the display holds the panel mutex when the watchdog fires —
and run 23's history is eight consecutive scheduler entries of task 6. A
coincidence of timing gives a mixed history; `66666666` is a monopoly.

UM-NATOS-029 had spent a whole report on the display driver monopolising the
panel. The office novel's Scheduler refuses Touch a context switch because *"the
Display Driver is holding the panel lock, and I do not take a lock off a man who
holds it."* This looked like that sentence with a reset attached.

---

## 8. And the controlled experiment cleared it

`dfreeze` sets `g_display_frozen` and nothing repaints. Four runs with it on.

**The watchdog still fires**, and the reset that happened is qualitatively
different from every un-frozen one:

| | un-frozen | frozen |
|---|---|---|
| last task | 6 | 6 |
| panel lock | **held by 6** | **`(unsigned)-2` — nobody** |
| history | `66666666`, `6:234566` | `12346796` — mixed |

So the lock-holding and the eight-in-a-row were **symptoms of drawing**, not the
mechanism. Stop the drawing and the board still resets, with the lock free and
seven different tasks in the last eight scheduler entries.

**§7's hypothesis is disproved.** The display was simply the most-caught task
because it is HIGH priority and runs constantly — the naive prior that step 264
warned about in advance, now confirmed as the explanation for its prominence
rather than as a cause.

### 8.1 The rate does not say what it looks like it says

1 in 4 against 3 in 8 is **not a distinguishable reduction** at these numbers.
It is consistent with no effect and with a modest one alike, and four runs cannot
separate them. The non-result is recorded rather than rounded into a story.

What *is* established does not depend on the rate: a reset occurred with the
display frozen, the lock free and a mixed history. That eliminates the display
as **necessary**, which is all the experiment was asked to do.

---

## 9. What survives every reset

- **The tick stops entirely.** The breadcrumb freezes; `watchdog_liveness`
  never reports a starved window, because it is not being called at all.
- **The feeds are healthy right to the end** — `wdt f/s=600/0` in the last
  status line before a reset.
- **It only happens once WiFi has been running for minutes at a stretch.**

A scheduler that stops being entered while the CPU is alive is an
**interrupts-off condition**. The blob takes critical sections, runs at nat-os
priority HIGH, and is the one component whose internals this project cannot
read.

Eliminated so far, each by measurement rather than argument: starvation, the
shell stack, lock contention, and the display task.

---

## 10. Method

### 10.1 Three retractions in seven steps

Step 259's cfg finding, step 260's "receives and does not answer", and the brief
declaration that step 260's A/B was invalid. Each was corrected in place, with
the correcting step named, rather than quietly rewritten.

That is not a good ratio and it has one cause: **conclusions drawn from single
runs and from proxies.** The corrective — a rate, and a milestone rather than a
file size — is cheap and was available the whole time.

### 10.2 The instrument is part of the system

Twice in this report an instrument changed the thing it measured: the TIMG0
readback stopped the crypto (§3), and the test harness reported kernel faults
that were serial-port faults (§4.1). A third instrument, the RTC breadcrumb,
was **validated against a forced fault before being trusted** (§7) — which is
the discipline the other two lacked, and it cost one shell command.

### 10.3 External verification, twice

The page was fetched by `curl` from the build machine, and then loaded by the
operator in their own browser. The milestone rests on a person and a machine
this project does not control — the standard the beacon met at step 209 and the
ping at step 230.

---

## 11. What is on the network

```
ssid       ivory-billed         WPA2-PSK / CCMP
station    5c:01:3b:50:3f:64    aid 12
address    192.168.1.140        DHCP, 6 leases in 8 runs
keys       pairwise CCMP installed, group CCMP installed
service    HTTP on port 80, 200 OK, two connections
uptime     until the watchdog, ~3 runs in 8
```

---

## 12. What remains

1. **The interrupt level in the breadcrumb.** If the tick stops because
   interrupts are masked, the last entry before the freeze is written with the
   level that did it. Three more words, and it tests the only hypothesis left
   standing after §9.
2. **Persistence.** The stack lives inside one shell command's poll loop; when
   `wifiinit start` returns, nothing services lwIP and the board goes silent.
   Servicing it from a task is what turns "I caught the page during a run" into
   "the board is a web server" — and it waits on the reset being understood,
   because at 3 in 8 it would be defeated anyway.
3. **The first-ARP question** of §6.2.
4. **The cfg readback** of §3, still unexplained.
5. Group-key rekeying, roaming, PMKSA caching, WPA3/SAE, and the all-channel
   scan that has panicked since step 202.

**The page is real and externally confirmed. The board reboots underneath it,
and after seven steps the cause is narrowed to the one component whose source
this project does not have.**
