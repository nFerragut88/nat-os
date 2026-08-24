# Chapter 9 — Scheduling: Priority, Sleep, Ageing, and Two Clocks That Lied

> Sources: `docs/UM-NATOS-009-m2-verification.md` §11, `docs/UM-NATOS-028` §4
> Code: `kernel/task.c`, `kernel/task.h`, `kernel/kmain.c`

---

## 9.1 Round robin, and nothing cleverer

M2 shipped a strict round robin over ready tasks, and the report defends the
absence of everything else:

> Selection scans forward from the current task and takes the first `READY` one.
> No priorities, no sleeping, no blocking — none of those have a consumer yet,
> and each would be an untested mechanism in the one code path that must be
> correct.

Four things were added afterwards, and the order matters:

> each was forced by the one before it, and the fourth was forced by a failure
> the third made possible.

1. **`TASK_BLOCKED`** arrived with the mutex, which needed it.
2. **An idle task** arrived with blocking, which needed it.
3. **`TASK_SLEEPING`** arrived with priorities, which needed it.
4. **Ageing** arrived because priorities starved a task to silence.

## 9.2 Blocking, and the three things it required

`TASK_BLOCKED` is a state the scheduler skips:

```c
typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_BLOCKED,       /* waiting on something; the scheduler will not pick it */
    TASK_SLEEPING,      /* waiting for a tick deadline                          */
} task_state_t;
```

### `task_block()` deliberately does not yield

This is the classic lost-wakeup guard and it is worth quoting the whole
interface contract, because the split it demands is unusual:

```c
/* ---- blocking ----------------------------------------------------------
 *
 * task_block() marks the calling task not-runnable. It does NOT yield — the
 * caller is expected to hold a critical section (critical.h) while blocking so
 * that the decision to block and the record of what it is waiting for cannot be
 * split by a tick, then to leave that section and call task_yield().
 *
 *     uint32_t s = crit_enter();
 *     wait_list |= 1u << task_current();
 *     task_block();
 *     crit_exit(s);
 *     task_yield();
 *
 * Nothing here knows what a task is waiting for. That belongs to whatever built
 * the wait — see mutex.c. */
void task_block(void);
```

The implementation restates the reasoning at the point where somebody would be
tempted to "fix" it:

```c
void task_block(void)
{
    if (g_current < 0) {
        return;                     /* boot context has nothing to block */
    }
    g_tasks[g_current].state = TASK_BLOCKED;
    /* Deliberately does NOT yield. The caller marks itself blocked while still
     * holding a critical section — so the decision to block and the record of
     * what it is waiting for cannot be split by a tick — then leaves the
     * critical section and yields. Yielding here instead would either fire the
     * tick with the wait-list half-updated, or be silently deferred until the
     * caller's crit_exit(), which reads as a bug at the call site. */
}
```

The failure it prevents: *the holder releases, sees an empty wait list, and the
waiter sleeps forever.* Chapter 11 shows `mutex_lock()` obeying the protocol
exactly.

### The idle task

With blocking, a moment where every task is waiting has nothing to switch to,
and the old fallback of resuming the interrupted task would run a task that had
just blocked.

Idle is registered rather than being a special slot, and is kept **out of the
round robin**:

```c
/* Registers a task as the idle task: it is chosen only when nothing else is
 * runnable, rather than taking an equal share of the round robin. Without one,
 * a system where every task blocks has nothing to run. */
void task_set_idle(int id);
```

> leaving it in the rotation would spend a seventh of the machine deliberately
> doing nothing.

It executes `WAITI 0`, which stops the core until an interrupt arrives:

```c
/* Idle. Runs only when every other task is blocked. WAITI stops the core until
 * an interrupt arrives, so an idle system draws less power than one spinning —
 * and the tick is what wakes it. */
static void task_idle(void)
{
    for (;;) {
        __asm__ volatile ("waiti 0");
    }
}
```

Chapter 24 §24.5 records that this task silently failed to be created for days,
which cost both the power saving and — more seriously — the invariant it exists
to protect.

## 9.3 Sleeping

A sleeping task carries a tick deadline; the scheduler wakes expired sleepers on
every tick, so a sleep resolves to one tick:

```c
/* Wakes any sleeping task whose deadline has passed. Called from the scheduler,
 * which runs every tick, so a sleep resolves to one tick. */
static void wake_sleepers(void)
{
    uint32_t now = timer_ticks();
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_SLEEPING &&
            (int32_t)(now - g_tasks[i].wake_tick) >= 0) {
            g_tasks[i].state = TASK_READY;
        }
    }
}
```

### `task_wake()` and the `woken` flag

There is a second way out of a sleep, and its design is one of the more careful
things in the kernel. `task_wake()` takes **SLEEPING as well as BLOCKED**, and
the comment explains why the combination is what a peripheral wait needs:

```c
void task_wake(int id)
{
    if (id < 0 || id >= TASK_MAX) {
        return;
    }
    /* Deliberately takes SLEEPING as well as BLOCKED, which is the whole
     * difference from task_unblock().
     *
     * A task waiting on hardware wants two things at once: to be released the
     * instant the device speaks, and to be released anyway if it never does. A
     * plain block gives the first without the second, and a task blocked on a
     * peripheral that has failed is deaf forever. A plain sleep gives the second
     * without the first.
     *
     * Sleeping with a deadline and letting an interrupt cut it short gives both,
     * and the failure mode is the good one: if the interrupt never arrives, the
     * deadline still fires and the caller falls back to polling. That is how the
     * touch task survives a PENIRQ that does not work — it degrades to exactly
     * what it did before this existed rather than to nothing.
     *
     * Safe from an ISR. It only promotes a state to READY; wake_tick is left
     * alone, and the scheduler's sleep sweep ignores a task that is no longer
     * SLEEPING. */
    if (g_tasks[id].state == TASK_BLOCKED ||
        g_tasks[id].state == TASK_SLEEPING) {
        g_tasks[id].state = TASK_READY;
        g_tasks[id].woken = 1;
    }
}
```

"It degrades to exactly what it did before this existed rather than to nothing"
is the design goal, and Chapter 22 §22.6 is the case where it was needed: PENIRQ
is routed, its handler runs, and no finger has ever been observed to trigger it.
The touch task polls, and the interrupt path is one line from reinstatement.

The `woken` flag distinguishes the two exits:

```c
    /* Set by task_wake(), cleared by task_sleep(). Distinguishes "an interrupt
     * cut this sleep short" from "the deadline has not arrived yet", which
     * task_sleep must tell apart to re-arm without defeating touch_irq_wait. */
    volatile uint8_t woken;
```

## 9.4 The sleep that did not sleep

`task_sleep()` is a loop, and it became one because of a defect found during the
WiFi work (UM-NATOS-028 §4). The reason is subtle enough that the source carries
the measurement:

```c
void task_sleep(uint32_t ticks)
{
    if (g_current < 0) {
        return;                     /* no context to sleep */
    }

    /* Loops, because task_yield() does not switch — it ARMS a switch.
     *
     * It writes CCOMPARE1 to ccount + 64 and returns; the context change
     * happens when that comparator fires, roughly sixty cycles later. So the
     * caller keeps running past this function for a short window, and whatever
     * it does in that window happens DURING what it believes is a sleep.
     *
     * Measured directly: task_sleep(50) — half a second — returned to its
     * caller in 107 cycles, which is the cost of the function body and nothing
     * else. The sleep did happen; it just started after the caller had already
     * read the clock and concluded no time had passed. Two separate
     * measurements this session were wrong because of it, including the first
     * TSF check, which is what exposed it.
     *
     * Re-arming until the deadline genuinely arrives closes that window and
     * costs one extra pass in the normal case.
     *
     * The `woken` flag is what keeps this from breaking touch_irq_wait. That
     * caller wants a sleep an interrupt can cut short, so "the deadline has not
     * arrived" must not be confused with "task_wake released me deliberately" —
     * without the flag, this loop would put the task straight back to sleep and
     * turn every early wake into a full-length one. */
    uint32_t deadline = timer_ticks() + ticks;

    uint32_t crit = crit_enter();
    g_tasks[g_current].woken = 0;
    crit_exit(crit);

    for (;;) {
        crit = crit_enter();
        if (g_tasks[g_current].woken) {
            g_tasks[g_current].woken = 0;
            crit_exit(crit);
            return;                 /* released early, on purpose */
        }
        if ((int32_t)(timer_ticks() - deadline) >= 0) {
            crit_exit(crit);
            return;                 /* the time really has passed */
        }
        g_tasks[g_current].wake_tick = deadline;
        g_tasks[g_current].state     = TASK_SLEEPING;
        crit_exit(crit);
        task_yield();
    }
}
```

Two bugs were stacked here and are worth separating.

**`task_yield()` arms a switch; it does not perform one.** The caller runs on for
~60 cycles into what it believes is a sleep. `task_sleep(50)` returned in 107
cycles.

**`timer_ticks()` counted ISR entries, not time** (Chapter 7 §7.10), so the
deadline the loop was comparing against was measured in the wrong unit.

After both fixes:

> `task_sleep(50)` now takes 639 ms / 64 ticks, and 64 × 10 ms = 640 ms — the
> tick count and the wall clock agree, which they did not before.

And the sting:

> But the wider point is the uncomfortable one, and `timer.c` had already
> written it down about an *earlier* bug in the same function: **nothing showed
> a symptom.** Sleeps ran short, timeouts ran loose, and every frame-rate figure
> this project has ever recorded was measured against a clock running fast by a
> load-dependent factor.

### The touch regression this fix caused

Fixing `task_sleep` broke something that had been accidentally relying on it
being broken, which is worth recording as a category of hazard.

The touch loop originally called `task_sleep(1u)` back when `task_sleep` neither
slept nor yielded. So it polled **repeatedly inside whatever slice it got**,
which is exactly why touch felt continuous. Replacing it with `task_yield()`
looked equivalent and was not:

> a yield surrenders the CPU after **every single poll**, so the task sampled
> once per slice instead of many times, and a quick tap fell between samples.

> Fixing `task_sleep` had, with perfect irony, broken the thing that was
> accidentally relying on it being broken.

The final configuration is a real 10 ms sleep at HIGH priority, and `kmain.c`
records the reasoning and the measurement:

```c
    /* HIGH, with the renderer, and polling on a real 10 ms sleep.
     * ...
     * HIGH because touch is user INPUT: at NORMAL it runs only when ageing
     * rescues it from behind a renderer that never sleeps, roughly every
     * 300 ms, and a quick tap falls between samples. A real sleep rather than
     * a yield because a yield surrenders the CPU after EVERY poll, sampling
     * once per slice instead of many times. It costs one SPI read per tick.
     *
     * Measured: ~15 samples per reporter interval before, ~150 after. */
    task_set_priority(id_touch,  TASK_PRIO_HIGH);
```

Ten times the sample rate for *less* CPU than the old flat-out polling, and —
unlike it — "a rate that is a property of the clock rather than of whatever else
happens to be running".

## 9.5 Priorities

Three levels, strictly ordered, round-robin among equals:

```c
#define TASK_PRIO_LOW    0
#define TASK_PRIO_NORMAL 1
#define TASK_PRIO_HIGH   2
#define TASK_PRIO_LEVELS 3
```

The implementation is one loop, and the elegance is that two properties fall out
of one condition:

> The scan starts past the current task and takes a candidate only on *strictly*
> greater priority, which yields both properties at once — among equals the first
> one met wins, and the scan begins somewhere different each time.

### Strict priority is only safe because tasks sleep

```c
 * Strict ordering is only safe because tasks SLEEP. A high-priority task that
 * spins instead of sleeping starves everything beneath it completely — this
 * kernel's previous `while (...) task_yield();` idiom would do exactly that.
 * task_sleep() is therefore not a convenience here; it is what makes priorities
 * usable at all.
```

That is not hypothetical. The demonstration workers were first placed at LOW and
performed **exactly zero iterations**, because the NORMAL band never empties.

> The failure was quiet in the worst way: `corrupt=0` continued to be reported,
> and means nothing when no work is being done. An instrument reading healthy
> because its subject had stopped is the same shape as [the EXCM defect] and as
> [the frozen marker].

`kmain.c` records the resulting policy in full, including the reason nothing sits
at LOW:

```c
    /* Priorities. The renderer is the only HIGH task: it wakes, draws a frame,
     * and sleeps, so it takes what it needs and then stands aside. Strict
     * priority is only safe because of that sleep — a HIGH task that spun would
     * starve every level beneath it outright.
     *
     * Nothing sits at LOW. Strict priority starves it absolutely: the workers
     * were put there first and did exactly zero iterations, because the NORMAL
     * band never empties — the application host never sleeps. That killed the
     * M2 register-integrity evidence, since corrupt=0 means nothing when no
     * work is being done.
     *
     * The renderer's speedup came from being HIGH, not from anything being LOW,
     * so everything else shares NORMAL. ... */
```

### Measured

```
renderer alone at HIGH:     2 fps -> 8.5 fps
with a busy NORMAL band:    ~3 fps
workers: 821,248 iterations each, exactly equal, corrupt=0
```

with a caveat added in a later revision that is the point of Chapter 28:

> *These frame rates were computed as frames per TICK, and the tick was later
> found to stall for up to 183 ms at a time. The comparison is still valid —
> both figures came from the same clock — but the absolute values are not
> trustworthy. The renderer measured 16.0 fps once the clock was corrected.*

The workers also sleep one tick per N iterations, and the reasoning is precise
about what the evidence requires:

> They exist to prove the switch preserves registers across arbitrary
> suspension, which needs them running *continuously*, not *constantly*.
> Spinning flat out bought no extra evidence, cost the renderer half its frame
> rate, and — because it ended the mutex thrash between them — sleeping raised
> their own throughput almost tenfold.

## 9.6 Ageing, because sleeping was not enough

§9.5 argues strict priority is safe **because tasks sleep**. That argument is
sound and it is not a guarantee: it depends on every high-priority task being
written to yield often enough, and nothing enforces it.

It failed the first time a task was made hungrier. Shortening the display task's
sleep from 8 ticks to 2 left it ready roughly 61% of the time, and the reporter
task stopped being scheduled at all:

```
reporter lines in 20 s     0
crash                      none
watchdog reset             none
```

And the reason nothing caught it:

> Nothing was wrong that any existing mechanism could see. **The hang detector
> asks whether ANY distinct switch happened, not whether every ready task got a
> turn**, and switches were happening constantly between the display task and
> its neighbours. The only reason the starvation was noticed at all is that the
> starved task happened to be the one that prints. A quieter victim would have
> produced no symptom.

### The policy

```c
effective = base + min(waiting / TASK_AGE_TICKS, TASK_AGE_MAX)
```

```c
#define TASK_AGE_TICKS  30u
#define TASK_AGE_MAX    3u      /* cap, so ageing cannot invert LOW past HIGH twice over */
```

Three properties are deliberate.

**Ageing is a property of the SELECTION, not of the task.**

```c
    /* Effective priority = base + ageing credit, computed per candidate below.
     * The base priority is never modified: ageing is a property of the
     * SELECTION, not of the task, so a task that finally runs returns to its
     * declared priority automatically rather than needing to be restored. That
     * distinction is what keeps this separate from priority inheritance, which
     * really does change a task's priority and really does have to undo it. */
```

The report puts the general form well: **a mechanism that must remember to
restore something is a mechanism that can forget.**

**Only READY tasks earn credit.** A task waiting on a deadline or a mutex is not
being treated unfairly by the scheduler; crediting it would let it barge ahead
the moment it became runnable.

**The bound is a latency, not a throughput guarantee.** `TASK_AGE_TICKS × 2` is
600 ms at the shipped values. "A task that is aged in and then immediately blocks
still makes no progress, and that is the caller's problem."

### The selection loop, whole

```c
    int next = -1;
    int best = -1;
    for (int i = 1; i <= TASK_MAX; i++) {
        int candidate = (g_current + i) % TASK_MAX;
        if (candidate == g_idle_id) {
            continue;
        }
        if (g_tasks[candidate].state == TASK_READY) {
            uint32_t credit = g_tasks[candidate].waiting / TASK_AGE_TICKS;
            if (credit > TASK_AGE_MAX) {
                credit = TASK_AGE_MAX;
            }
            int eff = (int)g_tasks[candidate].prio + (int)credit;
            if (eff > best) {
                best = eff;
                next = candidate;
            }
        }
    }
    /* Nothing else runnable — fall back to idle. Resuming the interrupted task
     * is NOT an acceptable answer once blocking exists: that task may be the
     * one that just blocked, and running a blocked task defeats the whole
     * mechanism. */
    if (next < 0 && g_idle_id >= 0 && g_tasks[g_idle_id].state == TASK_READY) {
        next = g_idle_id;
    }

    if (next < 0) {
        /* No runnable task and no idle task. Before blocking existed this could
         * only happen at boot; it can now also mean every task is blocked with
         * nobody left to wake them, which is a deadlock the kernel cannot
         * resolve. Resuming the interrupted context is the least-bad answer and
         * at least keeps the console alive to say so. */
        return current_sp;
    }
```

And the bookkeeping, done once the decision is final:

```c
    /* Every READY task that was NOT chosen waits one more tick; the chosen one
     * resets. Sleeping and blocked tasks are untouched — a task waiting on a
     * deadline or a mutex is not being treated unfairly by the scheduler, and
     * crediting it would let it barge ahead the moment it becomes runnable. */
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state != TASK_READY) {
            continue;
        }
        if (i == next) {
            if (g_tasks[i].waiting > g_max_wait) {
                g_max_wait = g_tasks[i].waiting;
            }
            if (g_tasks[i].waiting >= TASK_AGE_TICKS) {
                g_age_rescues++;    /* it only ran because ageing lifted it */
            }
            g_tasks[i].waiting = 0;
        } else {
            g_tasks[i].waiting++;
        }
    }
```

### Measured, including the number that is zero

Re-running the configuration that caused the starvation:

```
reporter lines in 20 s     0  ->  8
fair maxwait               35 ticks (350 ms), bounded as designed
fair rescues               114 decisions changed by ageing
```

At the shipped sleep value it reads `rescues=0, maxwait=15` — **inert in normal
operation**, which is correct for a backstop. Both numbers are reported anyway,
and the reason is a good general principle:

> a fairness policy nobody measures is a fairness policy nobody has, and
> `rescues=0` is the only thing that distinguishes "never needed" from "never
> working".

```c
/* Longest any ready task has waited without being scheduled, in ticks, and the
 * number of times ageing changed the decision. Reported because a fairness
 * policy nobody measures is a fairness policy nobody has. */
uint32_t task_max_wait(void);
uint32_t task_age_rescues(void);
```

### What it did not do

> It did not raise the frame rate. With ageing in place *and* the display sleep
> shortened, the renderer still ran at 3.0 fps — identical to before. The frame
> rate was bounded by lock contention, which is a different problem that ageing
> does not touch.
>
> Ageing fixed a class of silent failure. It is not a performance feature and is
> not recorded as one.

### Since written — what `rescues` reads on the finished device

The paragraph above records `rescues=0, maxwait=15` at the shipped sleep value
and calls the policy "inert in normal operation". That was true of the system as
it stood at M2. It is not true of the finished device.

A status dump taken during the display investigation of UM-NATOS-029, on a board
running the launcher, the renderer, the touch task and applications together,
reads **`g_age_rescues` = 4,513**. Ageing is not a backstop on this device; it is
making thousands of scheduling decisions, and the reason is in Chapter 11: the
display task blocks on the draw lock (`dblk disp/apps/touch/shell=3630/0/0/0`)
while everything else uses `display_try_lock()` and declines to wait. Ageing is
what gets the blocked task back onto the CPU.

Two things follow, and both are the sort of claim this book prefers to state
with the number attached:

- **The policy is load-bearing, not decorative.** Removing it would not restore
  a slightly less fair scheduler; it would leave the one task that genuinely
  blocks at the mercy of two tasks that never do.
- **`rescues=0` and `rescues=4513` are the same instrument answering honestly
  about two different machines.** The M2 measurement was not wrong. It was a
  measurement of a system that had three tasks and no renderer, and it went on
  being quoted about a system that has twelve tasks and a 3D view.

> A number is a measurement of the configuration it was taken in. This book
> quotes several that outlived their configuration; this is the one where the
> instrument was designed well enough to say so when re-read.

## 9.7 Priority inheritance

> **This section describes a mechanism that has never run.** `task_boost()` and
> `task_unboost()` are in `task.c`, they are correct, and **nothing calls them**
> — `mutex.c` contains no mention of priority at all, and `git log -S task_boost`
> returns one commit whose diff never touches `mutex.c`. The feature was
> described as shipped in UM-NATOS-014 §9, in that commit message, in this book's
> own front matter and in the timeline; it was built and never wired to a lock.
> UM-NATOS-014 rev 1.3 withdrew the claim. Ch. 30 §30.5 is the withdrawal in
> full, and Ch. 28b §28b.2.1 is the measurement that shows what runs instead.
>
> The section is kept because the *argument* for inheritance is sound and the
> code is the shape a wiring-up would use — and because its final paragraph, the
> one saying inheritance would not have helped the case that actually mattered,
> is the reason nobody missed it.

Priorities existed for less than a day before inversion did. The renderer runs at
HIGH and takes the display lock; the applications beneath it run at NORMAL and
take the same lock. A NORMAL task holding it while the renderer waits can be
preempted by any other NORMAL task, so the renderer waits not for the critical
section but for the whole NORMAL band to drain.

```c
/* Priority, and the boost used for inheritance. task_boost() only ever raises;
 * task_unboost() returns to the priority the task was created with. */
void task_boost(int id, int prio);
void task_unboost(int id);
```

```c
void task_boost(int id, int prio)
{
    if (id >= 0 && id < TASK_MAX && prio > (int)g_tasks[id].prio) {
        g_tasks[id].prio = (uint8_t)prio;
    }
}

void task_unboost(int id)
{
    if (id >= 0 && id < TASK_MAX) {
        g_tasks[id].prio = g_tasks[id].base_prio;
    }
}
```

Release restores the **base** priority rather than tracking nested holds, and the
limitation is recorded rather than solved:

> a task holding two boosted mutexes loses the boost on the first release.
> Recorded rather than solved: nothing in this kernel holds two, and inventing a
> nesting scheme with no consumer would be guessing at requirements.

And the finding that matters more than the feature:

> Inheritance bounds the *cost of waiting*. It does nothing about [the
> frequency] lesson, which is about the *number of waits*. A raycaster took the
> display lock once per column and spent 25 seconds per frame on 37 ms of work;
> holding it once per frame instead was a **194×** improvement. Inheritance would
> not have helped there at all.

Chapter 11 is that argument in full.

## 9.8 CPU time accounting: the third clock

`xt_ccount()` measures wall clock, which counts every preemption as though it
were the work. This produced wrong numbers throughout the project until a
per-task cycle count was added:

```c
/* ---- CPU time accounting ------------------------------------------------
 *
 * Cycles each task has actually RUN, as opposed to how much wall-clock passed
 * while it was trying to.
 *
 * Every timing figure in this kernel used to be a pair of xt_ccount() reads
 * around some work, which counts every tick, every other task's slice and every
 * interrupt that landed in the middle as though it were the work itself. The
 * measured cost of a full-screen fill was 43 ms before the scheduler existed
 * and 249-362 ms from a task afterwards — the same bytes to the same panel,
 * differing by eight times in nothing but bookkeeping.
 *
 * Charged at the switch, which is the only place that knows a task stopped
 * running. Time spent inside the level-3 handler is charged to whichever task
 * it interrupted; that is a small and deliberate inaccuracy, and the
 * alternative is accounting inside the ISR that measures the ISR.
 *
 * MEASURED, and the correction is not uniform. A full-screen fill from the
 * SHELL task read 249-362 ms wall-clock and 63-79 ms of work — the shell runs
 * at NORMAL and is preempted constantly, so nearly all of that was other tasks.
 * The raycaster's blit did not move at all: 55.5 ms either way, because the
 * display task runs at HIGH and is barely interrupted, so wall-clock was
 * already very close to its work.
 *
 * Which means the fix matters most exactly where the old numbers were least
 * trusted, and changes nothing where they were quietly correct. A conclusion
 * drawn from one task's timings could not have been carried to another's. */
static uint32_t g_run_cycles[TASK_MAX];
static uint32_t g_slice_start;      /* ccount when the running task got the CPU */
```

The accessor needs a critical section, and the reason is a good illustration of
how a two-word read can be non-atomic:

```c
uint32_t task_cpu_cycles(void)
{
    uint32_t crit = crit_enter();
    uint32_t v = (g_current >= 0)
               ? g_run_cycles[g_current] + (xt_ccount() - g_slice_start)
               : xt_ccount();
    crit_exit(crit);
    return v;
}
```

```c
 * The critical section is not optional: without it a tick landing between the
 * two loads returns an accumulated total from before the switch added to a
 * slice start from after it, which reads as a wildly negative interval.
```

**And then this clock lied too.** UM-NATOS-028 §8 lists it first among the
instruments that lied:

> **The blit timer.** `t_blit` uses `task_cpu_cycles()`, which only advances
> when the scheduler credits a slice at a context switch — it does not tick
> *inside* one. So the figure tracks context switches, not work. A 55.85 → 31.3
> ms shift was read as a serious regression, reported to the user as a 44%
> *improvement* an hour earlier, and was neither. It was an artifact both times.

Three clocks, each correct for a different question, and each having been used
for the wrong one at least once. The summary is in Chapter 0b's table and it is
worth keeping in front of you for the rest of the book.

## 9.9 The final task set

Nine tasks, created in `kmain` with checked creation:

```c
    id_report = must_create("report", task_report);
    id_a      = must_create("worker-a", task_a);
    id_b      = must_create("worker-b", task_b);
    id_vm     = must_create("vm-host", task_vm);
    id_apps   = must_create("app-host", task_apps);
    id_shell  = must_create("shell", task_shell);

    /* Created last and registered as idle, so it is outside the round robin and
     * chosen only when every other task is blocked. Without it, a moment where
     * all tasks are waiting has nothing to switch to. */
    id_disp   = must_create("display", task_display);
    id_touch  = must_create("touch", task_touch);
    id_idle   = must_create("idle", task_idle);
    task_set_idle(id_idle);
```

`must_create` exists because of Chapter 24 §24.5:

```c
/* Creates a task or stops the kernel. See the note at the first call site. */
static int must_create(const char *name, task_entry_fn entry)
{
    int id = task_create(name, entry);
    if (id < 0) {
        kernel_panic_msg("task table full — raise TASK_MAX", 0);
    }
    return id;
}
```

with the call-site note:

```c
    /* Checked, because the unchecked version cost this kernel its idle task.
     * task_create() returns -1 when the table is full, kmain made nine calls
     * against a TASK_MAX of 8, and the ninth failed silently for days: the
     * return value was assigned to id_idle, passed to task_set_idle(), which
     * bounds-checks and ignores it, and never printed. A creation that cannot
     * fail quietly is worth the four lines. */
```

| Task | Priority | Role |
|---|---|---|
| `report` | NORMAL | Telemetry every 200 ticks; runs the critical-section self-test |
| `worker-a`, `worker-b` | NORMAL | M2 regression: register integrity across preemption |
| `vm-host` | NORMAL | The kernel's own VM, running `spin` |
| `app-host` | NORMAL | `app_tick()` — the third scheduling level |
| `shell` | NORMAL | Polls UART0 |
| `display` | HIGH | Renderer / launcher / note pad / terminal |
| `touch` | HIGH | 100 Hz XPT2046 poll |
| `idle` | — | `WAITI 0`, outside the rotation |

## 9.10 Metrics

| Quantity | Value |
|---|---|
| Priority levels | 3, plus ageing credit up to 3 |
| Ageing threshold | 30 ticks per level, capped at 3 |
| Worst-case wait for a ready task | ~600 ms, bounded |
| Longest wait observed under stress | 35 ticks (350 ms) |
| Ageing rescues at shipped settings | 0 — inert, as intended for a backstop |
| Tasks | 9 of 12 slots |
| `task_sleep(50)` before / after | 107 cycles / 639 ms |
| Touch samples per reporter interval, before / after | ~15 / ~150 |

---

**Next:** the allocator underneath all of this, and the single invariant that
makes it checkable.
