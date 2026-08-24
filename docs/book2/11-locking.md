# Chapter 11 — Locking, and Why Contention Cost Is a Count and Not a Duration

> Sources: `docs/UM-NATOS-014-locking.md`
> Code: `kernel/critical.h`, `kernel/mutex.c`, `kernel/mutex.h`, `kernel/console.c`, `kernel/display.c`, `kernel/vm.c`

---

## 11.1 Two primitives, because the two problems have opposite shapes

nat-os had no way to make anything mutually exclusive until this work. Two
outstanding items forced it: the heap was task-context-only with no locking, and
console output interleaved so a telemetry report could land in the middle of a
typed line.

Two primitives were built, not one:

> the two motivating problems have opposite shapes: the heap needs a handful of
> memory operations to be indivisible, and the console needs a 13-millisecond
> write to be indivisible. Solving both with the same tool would mean either an
> unsafe heap or a wrecked scheduler.

| | Critical section | Blocking mutex |
|---|---|---|
| Mechanism | Raise `PS.INTLEVEL` to mask the tick | Block the task, remove it from the rotation |
| Cost while held | Scheduling latency, for the whole duration | Two context switches per handoff |
| Correct for | A few memory operations | Anything longer |
| Usable from an ISR | Yes | **No** — a handler cannot block |
| Used by | `heap_alloc`, `heap_free`, `ipc.c`, `task.c` | Console, display, application-shared state |

The asymmetry that decides it:

> **The critical section is the wrong tool for the console.** Writing a
> 150-character line at 115200 baud takes about 13 ms. Masking the tick for that
> long to solve a cosmetic interleaving problem would degrade every scheduling
> deadline in the system.

## 11.2 Critical sections

The entire implementation:

```c
/* Masks everything up to and including the timer's level 3. */
#define CRIT_LEVEL 3u

static inline uint32_t crit_enter(void)
{
    uint32_t ps = xt_get_ps();
    xt_set_intlevel(CRIT_LEVEL);
    return ps & 0xFu;
}

static inline void crit_exit(uint32_t saved_level)
{
    xt_set_intlevel(saved_level);
}
```

Two design points, both in the header comment.

**It returns and restores the previous level rather than forcing 0.**

> so nesting works and a section inside an interrupt handler does not silently
> re-enable interrupts on the way out.

**The header shouts about misuse**, in capitals, which is unusual for this
codebase and therefore deliberate:

```c
 * THIS IS THE WRONG TOOL FOR ANYTHING LONG. Interrupts are masked for the whole
 * section, so scheduling latency is degraded by exactly its duration. Writing a
 * 150-character line to UART0 at 115200 baud takes about 13 ms — masking the
 * tick for that long would wreck the scheduler to solve a cosmetic problem. Use
 * a mutex (mutex.h) for anything that is not a handful of memory operations.
```

The correctness argument is a single-core one and is stated as such: "On a single
core with no other bus master, that is sufficient for mutual exclusion: nothing
else can be running." If APP_CPU were ever started, every use of this primitive
would need re-examining.

## 11.3 Blocking, and the ownership sentinel

`MUTEX_FREE` is `-2`, not `-1`, and the reason is a one-line bug that would have
been very hard to find:

> Before the scheduler starts, `task_current()` returns `-1`, so using `-1` for
> "unheld" would make an unowned mutex indistinguishable from one held by the
> boot context, and the free and recursive tests would both match. A one-line
> problem that would have produced a mutex quietly granting itself to everyone
> during startup.

The mutex is **recursive**:

> A console lock is exactly the kind of thing that acquires a nested acquisition
> by accident, and self-deadlock on a kernel with no debugger is an expensive way
> to discover it.

That decision paid off immediately in the display driver, where `display_clear()`
draws through `display_fill_rect()` — a non-recursive lock would deadlock on the
first clear.

## 11.4 The starvation defect

The first working mutex released the lock and woke every waiter to race for it.
The reasoning recorded in the source was that with at most eight tasks a
thundering herd was not worth managing.

That reasoning was wrong, and the instrumentation said so precisely:

```
lock owner=1  waiters=0x00000004  acq=238542  contended=137  err=0  states=1121111
work a/b=238541/0
```

> Worker-a had acquired the lock **238,542 times**. Worker-b — `waiters` bit 2,
> state 2, `TASK_BLOCKED` — had acquired it **zero times**, and had not completed
> a single iteration of its loop.

And the diagnosis, which rules out the obvious explanation:

> This was not a lost wakeup. Worker-b was being woken correctly and re-blocking
> every time. Worker-a's loop is tight enough that it re-acquired the lock long
> before the woken waiter was next scheduled. **Race-on-release provides no
> ordering guarantee at all**, and against a tight loop that is not merely
> unfair — it starves the waiter indefinitely.

### The fix: direct handoff

`mutex_unlock()` now transfers ownership to a waiter rather than freeing the
lock. The successor is the owner *before* it runs again, so nothing can cut in
front:

```c
    /* Hand the lock to the longest-waiting candidate rather than releasing it.
     * Ownership transfers here, atomically with the wakeup, so a task still
     * spinning in its own lock/unlock loop cannot take it first. */
    if (m->waiters != 0u) {
        int next = -1;
        for (int id = 0; id < TASK_MAX; id++) {
            if (m->waiters & (1u << id)) {
                next = id;
                break;
            }
        }
        m->waiters &= ~(1u << next);
        m->granted |= 1u << next;
        m->owner    = next;
        m->depth    = 1;
        m->acquisitions++;
        task_unblock(next);
        crit_exit(s);
        return;
    }
```

This reintroduces a complication the first design was avoiding: `owner` names a
task that is not yet on the CPU. A `granted` bitmask resolves it, and the *order
of the tests* in `mutex_lock` is load-bearing:

```c
void mutex_lock(mutex_t *m)
{
    for (;;) {
        uint32_t s  = crit_enter();
        int      me = task_current();

        /* Checked first: a handoff already made this task the owner while it
         * was blocked, so it must return holding the lock without touching
         * depth. Testing owner == me first would read the handoff as a
         * recursive acquisition and leave depth permanently too high. */
        if (me >= 0 && (m->granted & (1u << me))) {
            m->granted &= ~(1u << me);
            crit_exit(s);
            return;
        }

        if (m->owner == MUTEX_FREE) {
            m->owner = me;
            m->depth = 1;
            m->acquisitions++;
            crit_exit(s);
            return;
        }
        if (m->owner == me) {
            m->depth++;                 /* recursive acquisition */
            crit_exit(s);
            return;
        }

        /* Held by someone else. Join the wait list and mark this task
         * unrunnable, both while still masked so a tick cannot land between
         * them — that split is the classic lost-wakeup: the holder releases,
         * sees an empty wait list, and this task blocks forever. */
        m->contentions++;
        if (me >= 0) {
            m->waiters |= 1u << me;
            task_block();
        }
        crit_exit(s);

        /* Only now allow the switch. If this runs before the scheduler has
         * started — me < 0 — there is no context to block, so the loop simply
         * spins, which is correct during single-threaded boot. */
        task_yield();
    }
}
```

Three things in that function are worth naming separately:

1. **`granted` before `owner == me`** — otherwise a handoff reads as a recursive
   acquisition and `depth` climbs forever.
2. **`waiters |=` and `task_block()` inside one critical section** — the protocol
   Chapter 9 §9.2 demands, honoured exactly.
3. **`me < 0` spins instead of blocking** — correct during single-threaded boot,
   and the reason the `MUTEX_FREE == -2` sentinel matters.

### Non-owner unlock is refused, not obeyed

```c
    /* Releasing a lock this task does not hold is a bug in the caller. Counted
     * and refused rather than acted on: clearing another task's ownership would
     * let two tasks into the section and corrupt whatever it protects, far from
     * where the mistake was made. */
    if (m->owner != task_current()) {
        m->errors++;
        crit_exit(s);
        return;
    }
```

> Refusing a non-owner unlock matters more than it looks: clearing another
> task's ownership would admit two holders to the protected section, corrupting
> whatever it guards far from where the mistake was made.

## 11.5 The cost of fairness

With the handoff in place and the lock taken on *every* worker iteration:

```
work a/b=2062/2061      (was 238541/0)
```

Fair, and throughput collapsed roughly thirtyfold.

> A handoff costs a full scheduling round-trip — the successor cannot run until
> the next tick — so a lock contended on every iteration becomes bounded by the
> tick rate rather than by the work.

That is a real property of a blocking lock, not a defect, but it is a
pathological workload. Contending once every 32 iterations is representative:

```
work a/b=50143/50143    acq=3133  contended=200  err=0  skew=0
```

Both workers now advance **exactly** in step.

`kmain.c` carries the constant and its justification:

```c
/* Contend every Nth iteration rather than every one. Taking the lock on every
 * pass means a worker holds it for most of its iteration, so contention is
 * near-certain and each handoff costs a full scheduling round-trip — measured
 * at roughly a thirtyfold throughput collapse. That is a real property of a
 * blocking lock, but it is a pathological workload, not a representative one. */
#define BUMP_EVERY 32u
```

The lesson, stated at M6 and rediscovered twice afterwards:

> a blocking mutex is the wrong instrument for a lock contended at high
> frequency; that case wants a critical section, or a design that does not share.

## 11.6 How mutual exclusion is actually tested

The contention target is deliberately hostile:

```c
/* Contention target. The workers hammer this from two tasks, and the reporter
 * checks it against their own iteration counts. The read-modify-write below is
 * deliberately slow and deliberately non-atomic: without the mutex it would be
 * reliably corrupted rather than occasionally, which is what makes the result
 * meaningful instead of lucky. */
static mutex_t           g_shared_lock;
static volatile uint32_t g_shared;

static void shared_bump(void)
{
    mutex_lock(&g_shared_lock);
    uint32_t v = g_shared;
    for (volatile int i = 0; i < 6; i++) {
        /* Widen the window between read and write. A tick landing here is the
         * whole point: unprotected, the other worker's increment is lost. */
    }
    g_shared = v + 1;
    mutex_unlock(&g_shared_lock);
}
```

> Each worker increments a shared counter under the lock through a deliberately
> non-atomic read-modify-write with a widened window; the kernel checks that
> counter against the workers' own independently maintained tallies.
> Unprotected, this drifts by thousands within a second.

`skew=0` throughout. That is a *cross-check*, not an assertion: two independently
maintained counters agreeing is much stronger evidence than one counter having
the value it was supposed to.

## 11.7 The self-test that could only pass by accident

This is a small story with a large moral, and it is the first appearance in this
book of a pattern Chapter 28 catalogues.

The critical-section test holds a section across two full tick periods and checks
the tick count is frozen, then resumed:

```
[6a] critical : PASS  ticks held at 1 across 2 periods, resumed at 7
```

> Both halves matter: freezing proves interrupts were masked, and resuming
> proves the tick was **deferred rather than lost**. Masking that silently
> dropped ticks would keep the scheduler running while making every timeout
> wrong.

And then:

> This test originally ran in `m6_selftest()` before `timer_start()`, where
> `timer_ticks()` is 0 and stays 0 regardless of what masking does — **a test
> that could only ever pass by accident.** It reported FAIL, which is the one
> outcome that made the flaw visible. It now runs from the reporter task with
> the tick live.

The test was saved by *failing*. Had it passed in its original position, it would
have been a permanent green light on a mechanism it could not observe. The
deferred call is still visible in `kmain.c`:

```c
/* Defined below with the other self-tests, but called from the reporter, which
 * is the only context where a running tick makes it meaningful. */
static void m6_critical_test(void);
```

```c
static void task_report(void)
{
    /* ... */
    /* Deferred until here: it needs a running tick to mean anything. */
    m6_critical_test();
```

This test is also the *only* instrument that noticed the 183 ms comparator stall
of Chapter 7 §7.9:

> Only the M6 critical-section self-test noticed, because it is the one test
> that asserts something about the tick *resuming* rather than about work
> getting done.

## 11.8 Console arbitration

Locking is at **message granularity**, not per character:

```c
    if (str_eq(line, "help"))       { cmd_help(); }
```
is wrapped by:
```c
    /* One command, one uninterrupted response. */
    console_lock();
```

and the two deliberate exclusions:

> `uart_putc()` stays lock-free, and the panic handler ignores this layer
> entirely — it runs after a fault, possibly with the lock held by the task that
> faulted, and a panic that deadlocks trying to report a panic is the worst
> failure mode available to a kernel with no debugger.

Result: `ps` output arrived as four unbroken lines with no report interleaved,
where previously a report would land mid-table.

One gap remains and is recorded: **keystroke echo is not arbitrated**, because
"locking each keystroke would serialise the console against a human typing
speed".

## 11.9 The same defect from the other side, and the finding that matters

*This section is UM-NATOS-014 revision 1.2, and it is the most transferable
result in the report.*

§11.5 established that a blocking mutex is the wrong instrument for a
high-frequency lock, and fixed the raycaster by batching: one acquisition per
frame instead of 240. That fix held. What it did not anticipate is that the
**other** party would arrive later and reintroduce the same shape from the
opposite direction.

Applications draw through `vm.c`, which took the draw lock **per primitive** —
exactly what the raycaster used to do to itself. The renderer holds the lock for
a whole frame; the applications interrupt it constantly with short takes.

### Blocked time is not lock-busy time

The mutex counted acquisitions and contentions from the beginning. Neither is a
duration, and the question was a duration, so `display.c` was instrumented to
time both the hold and the interval between asking and getting.

Measured over 8.08 s with applications running:

| quantity | value |
|---|---|
| lock held | 979 ms — **12% of wall clock** |
| aggregate blocked | 13,051 ms — 1.6 tasks blocked at all times |
| acquisitions | 100 |
| contentions | 202 — two blocked waiters per acquisition |

> **The lock was free 88% of the time**, and thirteen seconds of blocking
> accumulated in eight seconds of wall clock. That is only possible because
> "blocked" is not "lock busy": a task that cannot have the lock is descheduled,
> and the interval includes being selected again afterwards. 13,051 ms across
> 202 contentions is **63 ms per contention against a 24 ms hold** — most of it
> spent getting back onto the CPU.

The accessors are named for exactly this reason, and `display.h` says so:

```c
/* Lock timing.
 *
 * "Blocked" is time from asking for the lock to holding it. It is NOT time the
 * lock was busy: a task that cannot have it is descheduled, so the figure also
 * covers being selected again afterwards, and measurement showed that is the
 * larger part — 63 ms blocked against a 24 ms hold. The name matters, because
 * "wait" would imply the panel was the bottleneck and send the next person to
 * shorten holds, which was tried and changed nothing. */
uint32_t display_lock_blocked_ms(void);
uint32_t display_lock_hold_ms(void);
uint32_t display_lock_takes(void);
uint32_t display_lock_contentions(void);
```

Naming a counter after what it measures rather than after what it feels like is
a small discipline that here prevented a whole wrong investigation.

### The obvious fix, and why it failed

Composition writes to a private buffer and touches no shared hardware, so the
lock was narrowed to cover only the blit:

| | before | after |
|---|---|---|
| lock held | 979 ms | **734 ms** (−25%, as designed) |
| blocked | 13,051 ms | **13,085 ms** (unchanged) |
| frames/s | 3.0 | 3.2 |

> The change did precisely what it was built to do and moved the outcome by
> nothing. **Hold duration was never the cost.** The narrowed hold was kept
> anyway, because a lock should cover the shared thing rather than the whole
> operation that happens to use it — but it is not an optimisation and is not
> recorded as one.

That last clause is a small act of bookkeeping honesty that recurs in Chapter 18:
a change kept on its merits, explicitly *not* credited with a result it did not
produce.

### Attribution decided the fix

The aggregate says the system is blocked and cannot say *on whom*, and the two
readings imply opposite remedies. Per-task attribution answered it:

```
blocked, display task    3,880 ms    65% of wall clock
blocked, application host 5,894 ms    98% of wall clock
```

> **Both, mutually** — contending not for the panel, which was idle, but for the
> CPU afterwards.

```c
/* Milliseconds a specific task has spent blocked on the draw lock. The
 * aggregate says the system is blocked; this says which task, which is what
 * decided the fix — both the renderer and the applications were blocked most of
 * the time, on a lock that was free 91% of it. */
uint32_t display_lock_blocked_of(int task_id);
```

### Stop blocking

`vp_fill`, `vp_text` and `SYS BLIT` now take the lock with `display_try_lock()`
and skip the primitive when it is busy:

```c
    /* Best effort. See display_try_lock(): an application that blocks here
     * costs the whole system a scheduling round trip, and the pixel it wanted
     * to draw is worth far less than that. */
    if (!display_try_lock()) {
        g_draw_skipped++;
        return;
    }
    display_fill_rect(ax, ay, w, h, colour);
    display_unlock();
```

| | before | after |
|---|---|---|
| frames/s | 3.0 | **9.9** |
| blocked, applications | 5,894 ms | **0** |
| contentions | 202 per 8 s | 10 total |
| primitives skipped | — | 45 of 1,325 = **3.4%** |

Within 11% of the 11.1 fps measured with every application killed, so nearly all
the lost frame rate is recovered with the applications still running.

And skips are counted:

> Skips are counted rather than hidden. A best-effort policy that silently
> discards work is indistinguishable from a broken one, and the ratio is the only
> thing that distinguishes "reasonable" from "starving applications of the
> screen".

### The finding

> **The cost of a contended blocking lock is dominated by the number of blocking
> events, not the time the lock is held.** Each one costs a scheduling
> round-trip whether the lock was held for 24 ms or 24 µs. So the levers are
> batching (fewer takes) or not blocking at all (`try_lock`) — and shortening
> holds, the intuitive move, does nothing.

The two levers were reached for in that order: §11.5 batched (the raycaster's
194× improvement); §11.9 stopped blocking. And the reason batching could not be
used the second time is worth noting:

> the renderer and the applications cannot be batched together: they are
> different tasks with no common boundary to batch across.

`display.h` carries the batching interface with the frequency argument attached:

```c
/* Hold the draw lock across a batch of primitives.
 *
 * Every drawing call already locks, and the mutex is recursive, so this is only
 * about FREQUENCY. A caller issuing hundreds of small primitives pays a full
 * scheduling round-trip per contended acquisition — a raycaster taking the lock
 * once per column spent 25 seconds per frame on 37 ms of work. Holding it for
 * the batch turns that into one acquisition. */
```

This also explains an otherwise-odd optimisation in Chapter 24: the launcher
draws each icon row as one rectangle per *run* of set pixels rather than one per
pixel, and the reason is not micro-optimisation but lock count.

### Since written — what `cont=0` actually means

Best-effort drawing did what it was built to do, and it changed what the
contention counter can tell a reader. On the finished device the status line
reports:

```
dlock hold ms=7965    <- against ~7,860 ms of uptime
cont=0
drawskip climbing into the thousands
dblk disp/apps/touch/shell=3630/0/0/0
```

Read quickly, `cont=0` says *there is no contention problem*. It says nothing of
the sort. It says **nobody is permitted to contend**: every consumer except the
display task itself takes the lock with `display_try_lock()` and walks away, so
the one counter that would report a queue can no longer be incremented by
anything but the display task. The queue moved into `drawskip`, which is a
different counter with a different name in a different part of the status line.

The row that carries the real information is `dblk`: 3,630 blocking events, all
of them the display task, none from applications, touch or the shell. That is
the design working — and it is also why UM-NATOS-029 spent a session reading
`cont=0` as an absence of the problem it had itself removed.

Two smaller points, both worth having in the locking chapter because both look
like defects and are not:

- **`hold ms=7965` against ~7,860 ms of uptime is not an error.** The renderer
  holds the draw lock essentially continuously; the figure exceeds uptime by the
  few seconds of hold time accumulated before the uptime sample. What makes the
  number trustworthy at all is that `draw_lock()` guards its telemetry on
  `depth == 1u`, so re-entrant acquisitions do not each add their own hold time.
- **`vp_fill()` takes `display_try_lock()` and then calls
  `display_fill_rect()`, which takes `draw_lock()` on the same mutex.** That is
  not a recursive-acquire bug: `mutex_try_lock()` increments `depth` when the
  caller already owns the mutex, exactly as `mutex_lock()` does.

> An instrument that double-counted its own re-entry here would have produced a
> hold time obviously larger than uptime, and the number would have been thrown
> away as broken. It is only *slightly* larger, which is what a correct
> measurement of near-total ownership looks like — and the difference between
> those two outcomes is a `depth == 1u` in a telemetry guard.

## 11.10 Metrics

| Quantity | Value |
|---|---|
| Primitives | 2 |
| Critical section cost | `RSR`/`WSR` on `PS`, no memory |
| Mutex size | 24 B |
| Lock acquisitions measured | 3,133 |
| Contentions | 200 |
| Non-owner unlocks | 0 |
| Lost updates | 0 |
| Throughput, pathological contention | ~2,060 iterations |
| Throughput, 1-in-32 contention | ~50,143 iterations |
| Draw lock held, applications running | 12% of wall clock |
| Blocked per contention | 63 ms, against a 24 ms hold |
| Effect of narrowing the hold 25% | none |
| Frames/s, blocking → best-effort drawing | 3.0 → 9.9 |
| Primitives skipped under best-effort | 3.4% |

## 11.11 What this does not establish

- **No deadlock detection.** Two tasks taking two mutexes in opposite orders will
  hang, and the kernel will not say so. The idle task will simply run.
- **No timeouts.** `mutex_lock()` waits forever.
- **No condition variables or semaphores.** There is no way to wait for an event,
  only for a lock. (The WiFi OSI layer builds its own on top of `task_sleep` —
  Chapter 27 §27.3.)
- ~~**Priority inheritance drops a boost on the first release**, so nested holds
  of two boosted mutexes lose it early.~~ **Withdrawn.** There is no priority
  inheritance in this kernel: `task_boost()` exists and nothing calls it, and
  `mutex.c` does not mention priority (Ch. 9 §9.7, Ch. 30 §30.5). This entry was
  a precise caveat about the nesting behaviour of a code path that never
  executes — **a documented limitation is not evidence that the thing it limits
  exists**, and a sufficiently specific one is very good at looking like it.
- **The contention counter cannot see the queue any more.** `cont` counts
  blocking acquisitions, and best-effort drawing removed almost all of them;
  what a reader wants is `drawskip` and `dblk` (§11.9).
- **Keystroke echo is not arbitrated.**
- **Critical sections are unbounded by convention only.** Nothing measures or
  enforces how long one is held.
- **Untested against an ISR.** The mutex is documented as unusable from an
  interrupt handler and nothing enforces that.
- **Best-effort drawing has no fairness of its own.** A skipped primitive is
  simply lost. At 3.4% that is invisible, but the policy contains no bound on how
  unlucky one application can be.
- **The skip ratio is measured under one workload** — four small test programs
  drawing into strips. An application doing sustained full-viewport blits would
  contend far harder.
- **`try_lock` does not compose with batching.** An application wanting several
  primitives drawn atomically has no way to ask for that.

---

**Next:** the three mechanisms that were supposed to catch failures, and had
never been observed to catch anything.
