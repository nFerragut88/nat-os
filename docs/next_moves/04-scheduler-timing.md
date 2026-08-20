# 04 — A real-time path for control loops

> **MEASURED 2026-08-19.** The measurement this file asks for has been done;
> see the section at the end. It changed the conclusion -- the flash erase is
> 125 ms, not "tens of milliseconds", and the histogram built to find it
> cannot see it.

**Size:** medium. **Risk:** low, but it touches the scheduler, which everything
depends on. **Blocked on:** nothing needs it *yet*.

---

## The finding

From a live status line:

```
fair maxwait=36
```

Thirty-six ticks — roughly **340 ms** worst case for a ready task to be
scheduled.

The scheduler is **fair**, which is not **real-time**. Ageing guarantees that
nothing starves forever. It guarantees nothing about *when*.

## Why it matters, eventually

The robot concept's load-bearing line is:

```
every 5ms { balance.update(); }
```

A balance loop that can be starved for a third of a second is not a balance
loop; it is a fall. And this is a **scheduler** property — it will not be fixed
by adding motors, so discovering it after building a leg would be expensive.

It also matters for anything with a deadline: audio without gaps, a bus protocol
with timing requirements, a motor commutation loop.

**Nothing in nat-os needs it today.** This is on the list because it is
invisible until it is urgent.

## Measure before changing

`maxwait=36` is the worst wait ever *observed*, which is not the same as the
worst possible. Before touching the scheduler:

1. Add a task that records **inter-arrival jitter** at a target period — not
   average, the histogram tail.
2. Run it against the real system: display task blitting, apps running, touch
   polling, flash saves happening.
3. **The flash save is the suspect.** `store_save()` erases a sector with
   interrupts masked — tens of milliseconds where nothing else runs at all.
   That alone may dominate the tail.

Get the number first. It may be that the honest fix is "do not erase flash
while a control loop is running," which is a policy change and free.

## Two designs

**A. A priority class above ageing.** A task marked real-time is never aged
past. Simple, fits the existing scheduler, and the guarantee is only as good as
the longest critical section anywhere in the kernel — which includes that flash
erase.

**B. A timer-driven callback that bypasses the scheduler entirely.** A control
routine called from the tick ISR, with the scheduler running everything else.
This is how motor control is actually done in practice, and it is probably the
right answer. It costs: such a routine cannot block, cannot allocate, cannot
touch the draw lock, and must be short enough to fit inside a tick.

**Recommendation: B**, with A as a fallback if the callback restrictions prove
too tight. B has the useful property that its guarantee does not depend on the
rest of the kernel behaving.

## Do not

Do not raise `TASK_AGE_TICKS` or reshuffle priorities to make the number look
better. Fairness was reasoned about and paid for (UM-NATOS-009). A real-time
path should be a **separate mechanism**, not a distortion of the fair one.

## Where the code is

- `kernel/task.c` — the scheduler, ageing, `g_max_wait`, `g_age_rescues`
- `kernel/timer.c` — the tick and `CCOMPARE1`
- `kernel/store.c` — `store_save()`, the long critical section
- `docs/conceptual/the-small-embodied-ai.md` §5
- `docs/book/09-scheduling.md`

---

## Measured, 2026-08-19 â€” and it changes the conclusion

`jitter` was added to `kernel/shell.c` and a histogram to `kernel/task.c`,
extending the existing `g_max_wait` accounting rather than building a parallel
one. `jitter save` times a real `store_save()`.

### The distribution

Ready-to-running wait, in ticks (1 tick = 10 ms), ~16,500 samples:

```
   bucket   count    share
   0         3166    191/1000
   1         8553    517/1000
   2-3        998     60/1000
   4-7        756     45/1000
   8-15         0      0/1000     <- empty
   16-31     2081    125/1000
   32-63      976     59/1000
   64+          0      0/1000
   worst 33-36 ticks
```

**Two findings, neither predicted above.**

**1. It is bimodal, with an empty 8-15 bucket.** 18.5% of waits are â‰¥16 ticks
and *nothing* waits 8-15. That is structure, not noise.

**2. Load does not change it.** Running the 3D view â€” the heaviest thing this
kernel does â€” moves every bucket by less than one percent. The tail is not
display contention.

### The suspect, timed directly

```
store_save() returned 0, took 125 ms (10,060,479 cycles at 80 MHz)
```

**125 ms**, consistently, across five runs. This file guessed "tens of
milliseconds"; it is over a hundred, and interrupts are masked for all of it.

### The finding that matters

125 ms is 12.5 ticks. **That lands exactly in the empty bucket.**

It is empty because `waiting` only advances on a scheduler decision, decisions
happen on ticks, and the tick is masked for the whole erase. **The histogram
cannot see the stall it was built to find** â€” the instrument and the fault share
a dependency, which is the failure this project keeps rediscovering.

`timer_late_count()` does notice: it rises 3-4 per erase. It undercounts â€”
counting missed-deadline *events*, not skipped ticks â€” but it is the only
counter in the system that moves at all.

### So the worst case is the sum, not the maximum

```
scheduler tail        up to  36 ticks   360 ms   (measured, load-independent)
a coinciding erase          12.5 ticks  125 ms   (measured, invisible to the above)
```

No single instrument reports the total, and until now nothing reported the
second term at all.

### What this changes about the two designs

**Design B â€” a timer-driven callback bypassing the scheduler â€” does not solve
this.** The erase masks interrupts, so a tick-ISR callback is blocked exactly as
a task is. B is still the right answer for scheduler contention and still
recommended, but it buys nothing against the 125 ms.

**The policy fix is not optional and is not a fallback.** "Do not erase flash
while a control loop is running" is required under *either* design, because no
scheduler change can preempt a masked interrupt.

That was this file's own hypothesis â€” *"it may be that the honest fix is a
policy change, and free"* â€” and measuring turned it from a guess into the load-
bearing half of the answer.

### Left open

- **Why the tail is 16-63 with `TASK_AGE_TICKS = 30`.** Ageing should produce a
  pile-up near 30, not a spread from 16. Not chased; it is a fairness question,
  not a latency one, and 18.5% of waits being long is the actionable fact.
- **Whether the erase can be shortened or made preemptible.** A 4 KB sector
  erase is a flash-chip property, but writing less often, or to a smaller
  region, is a design choice nobody has costed.
- **`timer_late_count()` undercounts.** It answers "did we miss deadlines" but
  not "by how much". Worth fixing before anything depends on it.

### For 10

`next_moves/10` already flags this: a relay node writing a bundle to flash while
a receive window opens will miss it. **The number is 125 ms.** A LoRa receive
window is comfortably shorter than that.
