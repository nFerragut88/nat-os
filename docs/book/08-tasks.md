# Chapter 8 — Tasks, Frames, and the Defect That Cost Twelve Builds

> Sources: `docs/UM-NATOS-009-m2-verification.md`
> Code: `kernel/task.h`, `kernel/task.c`, `kernel/vectors.S`

---

## 8.1 The claim, and how each half of it fails

M1 proved that the kernel could be interrupted and resume *itself*. M2 is the
harder claim, because the interrupt now returns to a **different** context than
the one it interrupted.

| Claim | How it fails if untrue |
|---|---|
| A task can be created that has never run | The first switch jumps to a fabricated frame and lands nowhere |
| A running task can be suspended mid-instruction-stream | Registers or PC are lost; corruption appears later, elsewhere |
| A suspended task can be resumed exactly | Silent data corruption in the resumed task |
| The scheduler distributes time | One task starves the others; looks like a hang |
| Tasks do not corrupt each other's stacks | Overflow into a neighbour; no MMU to catch it |

> The third and fourth are the ones that actually broke.

## 8.2 One way for a task to come into existence

An earlier design adopted the boot context as task 0, capturing its stack pointer
on the first interrupt, and fabricated frames for every other task.

> That is two mechanisms for the same thing, and only one of them worked: tasks 1
> and 2 ran, task 0 never resumed.

The design was changed so that **every task without exception is created by
fabricating a frame that looks as though it had already been interrupted.**
`kmain` creates the tasks, starts the tick, and is then abandoned — its stack is
never reclaimed and it never runs again.

`g_current` starts at `-1`, which tells the scheduler there is no outgoing
context to save on the first switch:

```c
/* -1 means "no task is running yet": the boot context is about to be abandoned
 * and its stack pointer must NOT be saved, because doing so would overwrite the
 * fabricated frame of whichever task occupies that slot.
 *
 * An earlier design adopted the boot context as task 0 instead, capturing its
 * stack pointer on the first interrupt. That created two ways for a task to
 * come into existence — fabricated and captured — and only the fabricated path
 * worked: tasks 1 and 2 ran, task 0 never resumed. Deleting the second path was
 * cheaper than debugging it, and leaves one code path to be correct about. */
static int g_current = -1;
```

> This deleted the failing path rather than debugging it, and left one code path
> to be correct about instead of two.

This principle — *delete the second mechanism rather than debug it* — recurs.
It reappears in §8.3 (one switching mechanism, not two), in Chapter 26 (one
command set, not two), and in Chapter 16 (one `retire()` path, not three).

## 8.3 The switch happens inside the interrupt, not beside it

There is no separate `switch_to()` routine. The level-3 handler saves the full
context onto the interrupted task's own stack, passes the stack pointer to
`task_schedule()`, and resumes on whatever pointer comes back. If that pointer
belongs to a different task, the return *is* the context switch.

```asm
    /* Hand the current stack pointer to the scheduler; it returns the stack
     * pointer to resume on, which may belong to a different task. From this
     * instruction onward a1 may no longer be the stack we arrived on. */
    mov      a2, a1
    call0    task_schedule
    mov      a1, a2
```

Three instructions. Everything a scheduler does is on the other side of that
`call0`.

`task_yield()` therefore does not switch directly — it pulls the comparator
deadline forward so the ordinary tick fires almost immediately:

> One switching mechanism, exercised constantly, rather than a second one
> exercised rarely.

## 8.4 EPC3 and EPS3 must travel in the frame

This is the trick that makes a stack swap a task switch, and it is worth stating
carefully because it is the one thing about the design that is not obvious.

`RFI 3` restores PC and PS from `EPC3`/`EPS3`. These are **single registers, not
per-task state.** Swapping stack pointers alone would resume the new task's
*registers* at the *old* task's program counter.

From `vectors.S`:

```asm
 * EPC3 and EPS3 hold the interrupted PC and processor state, and RFI 3 restores
 * from them. They are single registers, not per-task. Swapping stacks alone
 * would therefore resume the new task's registers but the OLD task's program
 * counter. Saving them into each task's frame and restoring before RFI is what
 * makes a switch actually switch.
```

### The frame layout

96 bytes, 21 words, 16-byte aligned as Xtensa requires.

| Offset | Word | Contents | Why |
|---|---|---|---|
| 0 | 0 | `a0` | Return address; clobbered by `call0` and saved before that happens |
| 4–56 | 1–14 | `a2`–`a15` | Both caller- and callee-saved sets: the resumed task expects *its* values |
| 60 | 15 | `SAR` | Clobbered by any shift |
| 64 | 16 | `EPC3` | Interrupted PC |
| 68 | 17 | `EPS3` | Interrupted PS |
| 72–80 | 18–20 | `LBEG`, `LEND`, `LCOUNT` | Zero-overhead loop state — §8.6 |

`a1` is not stored: the frame's own address *is* the saved stack pointer.

The layout is declared in `task.h` and written by hand in `vectors.S`, and the
two must agree. There is a static assertion for the size and an explicit
acknowledgement that field order is not enforced:

```c
/* Saved-context frame, in 32-bit words. Assembly in vectors.S writes this
 * layout by hand; the two MUST agree. A static assertion in task.c checks the
 * size, but the field order is only guaranteed by keeping these in step. */
#define TASK_FRAME_WORDS  21
#define TASK_FRAME_BYTES  96           /* 21 words, padded to 16-byte alignment */

#define TASK_FRAME_IDX_SAR    15
#define TASK_FRAME_IDX_EPC3   16
#define TASK_FRAME_IDX_EPS3   17
#define TASK_FRAME_IDX_LBEG   18
#define TASK_FRAME_IDX_LEND   19
#define TASK_FRAME_IDX_LCOUNT 20
```

```c
_Static_assert(TASK_FRAME_BYTES >= TASK_FRAME_WORDS * 4,
               "frame must hold every saved word");
_Static_assert((TASK_FRAME_BYTES % 16) == 0,
               "Xtensa requires a 16-byte aligned stack");
```

### The handler sequence, annotated

```
addi a1, a1, -96        ; frame on the interrupted task's stack
save a0, a2..a15, SAR, EPC3, EPS3, LBEG, LEND, LCOUNT
clear PS.EXCM           ; MUST precede any C — see §8.7
call0 intr_dispatch     ; tick bookkeeping, comparator re-arm
mov  a2, a1
call0 task_schedule     ; a2 in = current sp, a2 out = sp to resume
mov  a1, a2             ; from here a1 may be a different task's stack
restore EPS3, EPC3, SAR, LBEG, LEND, LCOUNT, a0, a2..a15
addi a1, a1, 96
rfi  3
```

Two ordering constraints, both recorded in the source:

**Loop state is saved before any C is called**, because `task_schedule` contains
a loop of its own and would otherwise destroy what it was meant to preserve.

**`LCOUNT` is restored last** of the three:

```asm
    /* LOOP state. LCOUNT is written last: it is the register that actually arms
     * the hardware loop, so LBEG and LEND must already describe the right body
     * before it becomes live. */
    l32i       a2,  a1, 72
    wsr.lbeg   a2
    l32i       a2,  a1, 76
    wsr.lend   a2
    l32i       a2,  a1, 80
    wsr.lcount a2
```

## 8.5 Task creation

```c
int task_create(const char *name, task_entry_fn entry)
{
    for (int id = 0; id < TASK_MAX; id++) {
        if (g_tasks[id].state != TASK_UNUSED) {
            continue;
        }

        uint32_t *stack = g_stacks[id];
        for (int i = 0; i < TASK_STACK_WORDS; i++) {
            stack[i] = STACK_FILL;
        }
        stack[0] = STACK_GUARD;

        /* Frame sits at the top of the stack, 16-byte aligned. */
        uint32_t top = (uint32_t)&stack[TASK_STACK_WORDS];
        top &= ~15u;
        uint32_t *frame = (uint32_t *)(top - TASK_FRAME_BYTES);

        for (int i = 0; i < TASK_FRAME_WORDS; i++) {
            frame[i] = 0;
        }

        /* Resume at the entry point, with interrupts admitted. PS is taken
         * from the running kernel with INTLEVEL forced to 0, so the task
         * inherits the same execution mode rather than a guessed one — the
         * ROM leaves WOE and CALLINC set, and fabricating a different PS would
         * put the task in a subtly different state from its creator. */
        frame[TASK_FRAME_IDX_EPC3] = (uint32_t)entry;
        frame[TASK_FRAME_IDX_EPS3] = xt_get_ps() & ~0xFu;
        frame[TASK_FRAME_IDX_SAR]  = 0;

        g_tasks[id].sp         = (uint32_t)frame;
        g_tasks[id].state      = TASK_READY;
        /* ... */
        return id;
    }
    return -1;
}
```

Two constants, both chosen so their corruption is unmistakable:

```c
/* Written into the lowest stack word; if it changes, the task overflowed. */
#define STACK_GUARD 0x57ACC0DEu

/* Fill pattern, so untouched stack is distinguishable from used stack and
 * headroom can be measured rather than guessed. */
#define STACK_FILL  0xEEEEEEEEu
```

The fill pattern is what makes `task_stack_headroom()` a measurement rather than
an estimate:

```c
uint32_t task_stack_headroom(int id)
{
    if (id < 0 || id >= TASK_MAX || g_tasks[id].stack_base == 0) {
        return 0;
    }
    uint32_t untouched = 0;
    /* Walk up from just above the guard until the fill pattern stops. */
    for (int i = 1; i < TASK_STACK_WORDS; i++) {
        if (g_tasks[id].stack_base[i] != STACK_FILL) {
            break;
        }
        untouched++;
    }
    return untouched;
}
```

Chapter 12 §12.4 records what happened when this function was finally called for
the first time — it had existed, unused, for six milestones.

That function has a failure mode worth noting, and Chapter 27 §27.8 pays for it:
an *unprimed* `.bss` stack is all zeros, and a scan for a fill pattern reads
that as fully consumed. The PHY stack reported `6144 of 6144 bytes used` in a
real panic report, which "looked exactly like a stack exhaustion that had not
happened".

## 8.6 A hypothesis that measurement rejected, and a fix kept anyway

Before the real cause was found, the first explanation for M2's failure was
stale `LCOUNT`: a task suspended mid-loop, its loop state lost across the switch,
the hardware later branching back to a previous `LEND`.

The context frame was extended to save `LBEG`/`LEND`/`LCOUNT`, and the build
reflashed.

> **The symptom did not change**, and every dumped frame showed
> `lct=0x00000000`. No task had ever been suspended mid-loop.

And then the decision that makes this section worth reading:

> The frame change was nevertheless **kept**, on its own merits: a task
> genuinely can be suspended mid-loop, and losing that state would corrupt it on
> resume. ESP-IDF's context switch saves these three registers for the same
> reason. A real fix for a real hazard — just not for this defect. Recorded
> because a confident prediction that measurement rejected is the most reusable
> part of the exercise.

The `task.h` comment records the whole arc, including the observation that
*fixed* the symptom without explaining it:

```c
 * LBEG/LEND/LCOUNT back the Xtensa zero-overhead LOOP instruction, and they are
 * part of a task's context whether or not that task ever knows it. GCC emits
 * LOOP for ordinary counted C loops, so any task can be suspended mid-loop with
 * LCOUNT non-zero. Omitting these three words cost a full debugging session:
 * the scheduler's own round robin compiled to a LOOP, stale LCOUNT survived
 * into the handler, the hardware branched back to LEND, and the "no candidate
 * matched" fallback overwrote a selection that had in fact matched. It presented
 * as a task switching to itself forever. Adding a uart_puts() inside the loop
 * made the symptom disappear, because a loop body containing a call cannot be a
 * zero-overhead loop and GCC emitted a plain branch instead.
```

That last sentence is a Heisenbug in one line: *the instrumentation fixed it*.
Which is precisely what §8.7 explains.

## 8.7 The defect: `PS.EXCM` disables the zero-overhead loop

### The symptom

Tasks entered correctly and then the board went silent. Switch 4 reported
`2 -> 2` — the scheduler selecting the task it was already running — and repeated
forever. The task table printed microseconds later showed all three tasks
`st=1 (READY)` with stable stack pointers.

> The scheduler was refusing candidates that were plainly available.

### Two earlier conclusions that were wrong

Both were recorded in the repository before being disproved, and corrected rather
than quietly dropped.

**"The timer interrupt is not firing."** It was firing. `ticks=0` was sampled
before the first tick had occurred, because the reporter waited 100 ticks before
printing and the diagnostic loop printed faster than the tick period.

**"The board goes silent, so the switch is fatal."** The switch worked. The
silence was the workers doing exactly what they were written to do: spin without
producing output.

The lesson from the second is one of the most useful in the whole report set:

> Every failure mode in this kernel — masked interrupt, bad `RFI`, dead task,
> working-but-quiet task — presents identically as nothing on the wire. One
> entry marker per task turned that silence into a sequence and settled it
> immediately. **That should have been the first instrument, not the fourth.**

The entry markers are still in the tree, with that reasoning attached:

```c
/* Entry markers, same purpose as task 0's: they distinguish "the round robin
 * reached this task" from "the switch died on the way there". Tick 2 enters
 * worker-a, tick 3 enters worker-b, and tick 4 is the first time any task is
 * RESUMED from a frame the handler actually saved rather than one fabricated
 * by task_create — three different mechanisms that all fail as silence. */
static void task_a(void) { uart_puts("  [task 1 entered]\n"); worker(&work_a_count, ...); }
static void task_b(void) { uart_puts("  [task 2 entered]\n"); worker(&work_b_count, ...); }
```

and in the reporter:

```c
    /* First thing a fabricated task ever does. If this appears, the RFI landed
     * on the entry point and the task owns the CPU; if it never appears, the
     * switch itself is where control is lost. Those two failures are otherwise
     * indistinguishable, because both present as silence. */
    uart_puts("\n  [task 0 entered]\n");
```

### The root cause

GCC compiled the round robin into an Xtensa **zero-overhead `LOOP`**.

> **On Xtensa, the zero-overhead loop-back is disabled while `PS.EXCM` is set.**
> The `LEND` comparison never fires, so the body executes exactly once and falls
> straight through into whatever instruction occupies the `LEND` slot. Here that
> instruction is `or a12, a3, a3` — the `next = g_current` fallback.
>
> Hardware sets `EXCM` on interrupt entry. The handler had never cleared it, so
> every call into C ran with hardware loops silently degraded to a single
> iteration.

This accounts for the entire observation set, including the parts that looked
arbitrary:

| Observation | Explanation |
|---|---|
| Switches 1–3 correct | All were first-iteration hits — one iteration is enough |
| Switch 4 wrong | First case needing a second iteration; task 3 is `UNUSED` |
| Result was always `g_current` | That is precisely the instruction at `LEND` |
| Instrumentation fixed it | A body containing a call cannot be a hardware loop |
| `volatile` counter fixed it | Denies GCC the constant trip count |
| Saving `LCOUNT` did nothing | `LCOUNT` was never the mechanism |

Six observations, one cause. That is the shape of a correct diagnosis, and it is
the contrast with Chapter 27 §27.6, where four theories each explained *part* of
a symptom set and none explained all of it.

### The scope is far wider than the scheduler

This is the sentence that makes the defect worth a chapter:

> GCC emits hardware loops for ordinary counted C loops, so *every* C function
> reachable from an interrupt handler was affected — every future driver, and
> the VM interpreter. The scheduler is simply where it happened to become
> visible, and it became visible only because a single wrong iteration still
> produced a plausible-looking answer three times in a row.

### The decisive experiment

The hypotheses were separated with a test hook, `task_select_probe()`, holding
the selection loop in its original hardware-loop form. Calling the **same
function** from two contexts in the **same build**:

```
from kmain (PS.EXCM = 0):   probe(2) = 0    correct
from the ISR (PS.EXCM = 1): probe(2) = 2    wrong
```

with the handler's own PS dumped alongside: `isr ps=0x00060733` — `INTLEVEL=3`,
and bit 4 `EXCM` **set**. After the fix, the same line reads `isr ps=0x00060723`
with `EXCM` clear and `probe(2) = 0`.

The methodological point is stated explicitly:

> This is what a controlled comparison is for. The earlier A/B varied the *code
> generation* and could only ever show correlation; varying the *context* while
> holding the code identical isolates the cause.

The probe is still in the tree, with its purpose recorded:

```c
/* Test hook. Byte-for-byte the selection loop as it was WITHOUT the volatile
 * workaround, so GCC is free to emit a zero-overhead LOOP again. Called from
 * kmain before timer_start(), i.e. single-threaded with no interrupt source
 * armed and no context switching in existence.
 *
 * This separates two very different explanations for the M2 defect:
 *   - wrong answers here  => the loop is mis-executed on its own, and interrupts
 *                            were never involved
 *   - correct answers here => the loop is fine in isolation and something about
 *                            interrupt context corrupts it
 */
int task_select_probe(int current)
{
    int next = current;
    for (int i = 1; i <= TASK_MAX; i++) {
        int candidate = (current + i) % TASK_MAX;
        if (g_tasks[candidate].state == TASK_READY) {
            next = candidate;
            break;
        }
    }
    return next;
}
```

### The fix: five instructions

```asm
    /* Clear PS.EXCM before calling any C.
     *
     * Hardware sets EXCM on interrupt entry, and while it is set the Xtensa
     * zero-overhead loop is DISABLED: the LEND comparison never fires, so a
     * hardware loop body executes exactly once and falls through to whatever
     * instruction sits at LEND. GCC emits these loops for ordinary counted C
     * loops, which means every C function reachable from this handler is
     * affected, not just the one that exposed it.
     *
     * That is what broke M2. ...
     *
     * Safe to clear here: PS.INTLEVEL is still 3, so interrupts stay masked,
     * and EPC3/EPS3 are already saved, so the eventual RFI is unaffected. It
     * also means a fault inside the handler reaches the kernel exception
     * vector and the panic handler, instead of the double-exception vector. */
    rsr.ps   a2
    movi     a3, ~0x10
    and      a2, a2, a3
    wsr.ps   a2
    rsync
```

Note the third benefit in the safety argument: with `EXCM` cleared, a fault
inside the handler reaches the *kernel* exception vector and therefore the panic
handler, rather than the double-exception vector. That is a debuggability
improvement obtained for free.

The `volatile` workaround was removed. The selection loop is a plain counted
loop again, compiles to a hardware `LOOP`, and is correct — with the history
recorded in place so nobody reintroduces the workaround:

```c
    /* Plain counted loop. GCC is free to emit a zero-overhead LOOP here, and
     * does; that is correct now that _handler_level3 clears PS.EXCM before
     * calling C. This loop previously needed a `volatile` counter to force
     * ordinary branches, which worked but treated the symptom. */
```

### The standing rule

> **Standing rule for interrupt handlers — clear `PS.EXCM` before calling C.**
> Hardware sets `EXCM` on interrupt entry, and while it is set the Xtensa
> zero-overhead loop-back is disabled. GCC emits these loops for ordinary
> counted C loops, so this silently degrades **every C function reachable from a
> handler** — drivers and the VM interpreter included, not just the scheduler
> where it was found. `_handler_level3` now clears it; any future handler must
> do the same.

## 8.8 Results

Sustained run, 10 ms tick, three tasks:

```
t=202   switches r/a/b=68/67/67     work a/b=1528960/1698281    guards=ok  corrupt=0
t=403   switches r/a/b=135/134/134  work a/b=3104989/3426830    guards=ok  corrupt=0
t=604   switches r/a/b=202/201/201  work a/b=4681019/5155380    guards=ok  corrupt=0
t=805   switches r/a/b=269/268/268  work a/b=6257049/6883930    guards=ok  corrupt=0
t=1006  switches r/a/b=336/335/335  work a/b=7833079/8612480    guards=ok  corrupt=0
t=1207  switches r/a/b=403/402/402  work a/b=9409109/10341029   guards=ok  corrupt=0
```

**Distribution.** 403/402/402 across three tasks — even to within one switch,
which is the expected residue of where the run was sampled.

**Integrity.** `corrupt=0` across ~1,200 preemptions.

**Stacks.** 463 of 512 words free in both workers, stable across the run: about
196 bytes used, no growth. Guards intact.

**Switch cost.** 96 bytes of stack and 21 register transfers per switch, twice
(save and restore).

Final verification figures: **3,418 ticks, 1,140/1,139/1,139 switches, zero
corruption.**

### How the workers make corruption visible

The verification tasks hold four values live across a compiler barrier
positioned where an interrupt can land:

```c
static void worker(volatile uint32_t *count, volatile uint32_t *bad,
                   volatile uint32_t *bumps, uint32_t seed)
{
    uint32_t n     = 0;
    uint32_t magic = seed;
    uint32_t alt   = ~seed;
    uint32_t acc   = seed ^ 0x9E3779B9u;

    for (;;) {
        n++;
        acc = acc * 1664525u + 1013904223u;

        /* Barrier only — keeps the values live across a point where an
         * interrupt can land, without dictating where they live. */
        __asm__ volatile ("" : "+r"(n), "+r"(magic), "+r"(alt), "+r"(acc));
        /* ... */
        if (magic != seed || alt != ~seed) {
            (*bad)++;
            magic = seed;       /* repair, so one fault is not counted forever */
            alt   = ~seed;
        }
        *count = n;
    }
}
```

The barrier is a *constraint*, not a *placement*, and that distinction is
recorded because getting it wrong manufactured a bug:

> Registers are deliberately *not* pinned with explicit `__asm__("aN")`
> bindings: that claims registers the compiler may already be using, and the
> writes then land on arbitrary memory. An earlier build did exactly this and
> manufactured a bug that did not exist.

The repair line matters too: without it a single fault would be re-detected
forever and the counter would report a corruption rate rather than a corruption
count.

The comment above the worker's declaration in `kmain.c` explains why they are
still running, ten chapters later:

```c
 * These tasks are M2 artefacts: they prove a context switch preserves registers
 * across arbitrary suspension, and they have now done so 460,800 times with
 * corrupt=0. The claim is established; what remains is a REGRESSION CHECK, and
 * a regression check does not need most of the machine.
 * ...
 * Deleting them was the alternative. Kept instead because guards=ok and
 * corrupt=0 are the only continuous evidence that the scheduler still does what
 * UM-NATOS-009 says it does.
```

## 8.9 The metrics, including the ones about the process

| Quantity | Value |
|---|---|
| Image size | 5,312 B (includes retained instrumentation) |
| Frame | 96 B / 21 words |
| Tick interval | 800,000 cycles (~10 ms) |
| Tasks | 3 of 4 slots at M2; 9 of 12 now |
| Stack per task | 2 KB; ~196 B peak use |
| Ticks observed | 3,418 |
| Switches observed | 1,140 / 1,139 / 1,139 |
| Corruption events | 0 |
| **Build cycles spent on the defect** | **12** |
| **Wrong hypotheses recorded** | **3** |
| **Instructions in the fix** | **5** |

The last three rows are unusual to publish and are the most informative in the
table. Twelve build-and-flash cycles, three recorded wrong hypotheses, and a
five-instruction fix.

## 8.10 The switch tracing that is still there

Two `#define` switches remain in `task.c`, both defaulted off, both kept for a
stated reason:

```c
/* Switch tracing. Set to 0 for a quiet boot; raise it to watch the first N
 * switches when touching the handler or the frame layout. Retained rather than
 * deleted because it is what turned M2's silence into a sequence. */
#define TRACE_SWITCHES 0

/* A/B switch. With the in-loop probes compiled in, the selection loop is
 * correct; with them out, switch 4 returned 2 -> 2 while the task table said
 * every task was READY. Same source otherwise. Flip this to reproduce. */
#define TRACE_PROBES 0
```

When enabled, the trace prints the whole frame about to be restored, plus the
task table, and marks whether the frame was fabricated or saved:

```c
    uart_puts(fabricated ? "  (fabricated frame)\n" : "  (SAVED frame)\n");
```

> Ticks 1-3 restore frames fabricated by task_create; tick 4 is the first
> restore of a frame the handler itself saved. Dumping both means the saved
> frame can be read against a known-good fabricated one instead of against
> expectations.

The trace also honestly declares its own side effect:

```c
 * This runs at interrupt level 3 and blocks on the UART for the length of the
 * dump, which is far longer than a tick period. Ticks are missed as a result
 * and timer_late_count() will climb — that is expected and harmless here,
 * because the question is what the frame CONTAINS, not when it arrives.
```

And the task table is dumped alongside the frame for a specific diagnostic
reason:

```c
    /* The whole task table. If the round robin stops offering a task, the
     * question is whether the scheduler skipped it or whether its state field
     * stopped saying READY — and those are different bugs. */
```

That is the general form of the counter idiom from Chapter 0b: instrument the
two candidate explanations separately so the reading distinguishes them.

## 8.11 What M2 did not establish

- **No memory protection.** A native task can corrupt any other; guards catch
  overflow, nothing catches a wild pointer. Isolation arrives only with the
  bytecode VM.
- ~~No blocking.~~ → Chapter 9. What remains missing is any wait other than a
  lock or a deadline — there is no way to wait for an *event*.
- ~~No priorities.~~ → Chapter 9.
- ~~Fixed ceiling, `TASK_MAX = 4`.~~ Now 12. "The ceiling moved, it did not stop
  being a ceiling" — stacks are still 2 KB and statically allocated.
- ~~No idle task.~~ Added with blocking, and it executes `WAITI` rather than
  spinning. Chapter 24 §24.5 records the day it was discovered *not to exist*.
- **Single core.** APP_CPU is untouched.
- **No audit of other `EXCM` consequences.** §8.7 establishes that hardware loops
  were degraded while `EXCM` was set. Whether anything else in the kernel
  silently depended on `EXCM` being set for the duration of the handler has not
  been examined:

  > Nothing suggests it does — the handler is short and the only C it calls is
  > the tick and the scheduler — but the question was not asked systematically.

  That is still true and is in Chapter 30.

---

**Next:** what the round robin became once things needed to sleep, block, and be
prevented from starving each other.
