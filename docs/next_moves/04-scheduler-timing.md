# 04 — A real-time path for control loops

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
