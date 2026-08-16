# The MAC is running

Measured on hardware, not inferred. Follows MAC-NEXT.md.

## What was done

open-mac's entire MAC initialisation:

```c
#define MAC_CTRL_REG _MMIO_DWORD(0x3ff73cb8)
static void init_mac() { MAC_CTRL_REG = MAC_CTRL_REG & 0xffffe800; }
```

plus one thing open-mac gets from ESP-IDF and nat-os had to do itself: ungating
`DPORT_WIFI_CLK_WIFI_EN` (0x406) in `DPORT_WIFI_CLK_EN_REG`, on top of the
WiFi/BT common bits (0x3C9) that `phyinit` already sets.

## Why the evidence is a register scan and not a readback

A readback proving the mask took effect would prove almost nothing. This kernel
has now been caught three separate times by a peripheral whose registers read
back exactly as written while the hardware did nothing — LEDC gated behind
`DPORT_PERIP_CLK_EN` bit 11 (UM-NATOS-027 §3.3) being the clearest. **A
successful store is not evidence.**

So the test is: read the 4 KB MAC window, wait, read it again, and count words
that changed with nothing driving them. A gated block is static. A running MAC
has free-running counters in it, and no write that went nowhere can fake one.

| stage | moving words |
|---|---|
| cold boot | **0** |
| after `phyinit` | **6** |
| after `macinit` | **13–15** |

## The TSF timer, identified by behaviour

Rates across the window (`maclive`), normalised against measured CPU cycles:

```
0x3ff73c00  1000 kHz   <- stable across every run
0x3ff73c14  1054 -> 1102 kHz
0x3ff73c18  1055 -> 1107 kHz
0x3ff73d80   527 ->  460 kHz
0x3ff73d84/88/8c   2 kHz  (three consecutive words, identical rate)
0x3ff73db4    16 kHz
0x3ff73dd0  1063 -> 1114 kHz
0x3ff73dd4  1054 -> 1107 kHz
```

`0x3ff73c00` is the only word that reports **exactly 1000 kHz on every run**;
the others drift between samples. Confirmed over a long interval against the
CPU's own cycle counter — two clocks with no connection to each other:

```
tsf advanced 622458 over 622 ms of cpu time -> 1000 kHz
tsf advanced 508686 over 508 ms of cpu time -> 1001 kHz
```

Agreement to 0.1% over half a second is not something a misread address
produces. **0x3ff73c00 is the 802.11 TSF timer**, and nat-os now has a 1 MHz
microsecond timebase — which it did not have before, its tick being 10 ms.

This identification rests on behaviour alone. Every other address in this
region remains a reverse-engineered guess.

## What did NOT happen

`MAC_CTRL_REG` read `0x00000000` both before and after, so `0 & 0xffffe800` was
a **no-op**. The 6 -> 13 jump in moving words came from the clock enable, not
from open-mac's write.

That is not a failure — `init_mac()` is a clearing operation and at cold boot
there is nothing to clear — but it means this register write is **currently
unverified**. It will matter once the blob has set those bits.

## Unrelated bug found on the way — now fixed

`task_sleep()` was returning without sleeping. Found because the first TSF
check used it and reported ~1 us for a requested 500 ms, with the CPU cycle
counter and the MAC's TSF independently agreeing.

TWO causes, and the first diagnosis in this file was wrong about the second:

**1. `timer_ticks()` counted ISR entries, not time.** `task_yield()` ends a
slice by pulling CCOMPARE1 to `ccount + 64`, so `timer_isr` ran on every yield
as well as every deadline — and `g_ticks++` sat at the bottom of it
unconditionally. Measured at **217 ticks per real second** where 100 is
correct, and far higher when a task sat in an idle-yield loop. Every deadline
expressed in ticks came due early, in proportion to how much yielding the
system happened to be doing. Fixed by advancing the clock only when the
deadline has actually passed: **217 -> 105**.

**2. `task_yield()` arms a switch; it does not perform one.** It writes the
comparator and returns, so the caller keeps running for ~60 cycles into what it
believes is a sleep. `task_sleep(50)` — half a second — returned to its caller
in **107 cycles**, the cost of the function body and nothing else. The sleep
did happen; it started after the caller had already read the clock. Fixed by
re-arming until the deadline genuinely arrives, with a `woken` flag so
`task_wake()` can still cut a sleep short — `touch_irq_wait()` depends on that
and a naive loop would have turned every early wake into a full-length one.

Verified: `task_sleep(50)` now takes 639 ms / 64 ticks, and 64 ticks x 10 ms =
640 ms — the tick count and the wall clock agree, which they did not before.
The overshoot past 50 is scheduling latency for a NORMAL-priority task waiting
behind the HIGH-priority display task, which is correct behaviour for a sleep.

### It made the 3D view 44% faster

| | before | after |
|---|---|---|
| ticks per real second | 217 | 105 |
| raycaster blit | 55.85 ms | **31.4 ms** |

Every spurious tick cost a full context save, a scheduler run and a restore.
Removing them was pure gain — the timing fix was expected to risk the frame
rate and improved it instead.

The uncomfortable part is the same one `timer.c` already noted about an earlier
bug in this function: **nothing showed a symptom.** Sleeps ran short, timeouts
ran loose, and every frame-rate figure this project has recorded was taken
against a clock that ran fast by a load-dependent factor.
