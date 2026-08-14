# UM-CYDOS-009 — Milestone 2 Verification Report

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-14 · Status: **PASS with a qualified fix** — exit criteria met on hardware; one defect closed by workaround rather than by root cause

---

## 1. Abstract

Milestone 2 establishes preemptive multitasking: several independent execution
contexts share one core, each suspended at an arbitrary instruction by a timer
interrupt and later resumed as though nothing happened.

M1 proved that the kernel could be interrupted and resume *itself* correctly.
M2 is the harder claim, because the interrupt now returns to a **different**
context than the one it interrupted. Everything above this layer — drivers, the
VM host, any notion of concurrency — assumes it works.

The milestone passes. It is reported as **PASS with a qualified fix** because
the defect that dominated the effort is closed empirically but not explained.
Section 6 documents that honestly rather than presenting a clean result.

## 2. What M2 had to establish

| Claim | How it fails if untrue |
|---|---|
| A task can be created that has never run | The first switch jumps to a fabricated frame and lands nowhere |
| A running task can be suspended mid-instruction-stream | Registers or PC are lost; corruption appears later, elsewhere |
| A suspended task can be resumed exactly | Silent data corruption in the resumed task |
| The scheduler distributes time | One task starves the others; looks like a hang |
| Tasks do not corrupt each other's stacks | Overflow into a neighbour; no MMU to catch it |

The third and fourth are the ones that actually broke.

## 3. Design decisions

### 3.1 One way for a task to come into existence

An earlier design adopted the boot context as task 0, capturing its stack
pointer on the first interrupt, and fabricated frames for every other task.
That is two mechanisms for the same thing, and only one of them worked: tasks 1
and 2 ran, task 0 never resumed.

The design was changed so that **every task without exception is created by
fabricating a frame that looks as though it had already been interrupted.**
`kmain` creates the tasks, starts the tick, and is then abandoned — its stack
is never reclaimed and it never runs again. `g_current` starts at `-1`, which
tells the scheduler there is no outgoing context to save on the first switch.

This deleted the failing path rather than debugging it, and left one code path
to be correct about instead of two.

### 3.2 The switch happens inside the interrupt, not beside it

There is no separate `switch_to()` routine. The level-3 handler saves the full
context onto the interrupted task's own stack, passes the stack pointer to
`task_schedule()`, and resumes on whatever pointer comes back. If that pointer
belongs to a different task, the return *is* the context switch.

`task_yield()` therefore does not switch directly — it pulls the comparator
deadline forward so the ordinary tick fires almost immediately. One switching
mechanism, exercised constantly, rather than a second one exercised rarely.

### 3.3 EPC3 and EPS3 must travel in the frame

`RFI 3` restores PC and PS from `EPC3`/`EPS3`. These are **single registers, not
per-task state.** Swapping stack pointers alone would resume the new task's
registers at the *old* task's program counter. Saving both into each frame and
restoring them before `RFI` is what converts a stack swap into a task switch.

<!--FIGURE: frame_layout -->

### 3.4 Round robin, and nothing cleverer

Selection scans forward from the current task and takes the first `READY` one.
No priorities, no sleeping, no blocking — none of those have a consumer yet, and
each would be an untested mechanism in the one code path that must be correct.

## 4. Implementation

### 4.1 Frame layout

96 bytes, 21 words, 16-byte aligned as Xtensa requires.

| Offset | Contents | Why |
|---|---|---|
| 0 | `a0` | Return address; clobbered by `call0` and saved before that happens |
| 4–56 | `a2`–`a15` | Both caller- and callee-saved sets: the resumed task expects *its* values |
| 60 | `SAR` | Clobbered by any shift |
| 64 | `EPC3` | Interrupted PC |
| 68 | `EPS3` | Interrupted PS |
| 72–80 | `LBEG`, `LEND`, `LCOUNT` | Zero-overhead loop state — see §6.4 |

`a1` is not stored: the frame's own address *is* the saved stack pointer.

### 4.2 Handler sequence

```
addi a1, a1, -96        ; frame on the interrupted task's stack
save a0, a2..a15, SAR, EPC3, EPS3, LBEG, LEND, LCOUNT
call0 timer_isr         ; tick bookkeeping, comparator re-arm
mov  a2, a1
call0 task_schedule     ; a2 in = current sp, a2 out = sp to resume
mov  a1, a2             ; from here a1 may be a different task's stack
restore EPS3, EPC3, SAR, LBEG, LEND, LCOUNT, a0, a2..a15
addi a1, a1, 96
rfi  3
```

Loop state is saved **before** any C is called, because `task_schedule` contains
a loop of its own and would otherwise destroy what it was meant to preserve.
`LCOUNT` is restored **last** of the three, since it is the register that arms
the hardware loop and the bounds must already be valid when it becomes live.

### 4.3 Task creation

`task_create` fills a stack with `0xEEEEEEEE`, writes a `0x57ACC0DE` guard at
the lowest word, and builds a frame at the top with all registers zero,
`EPC3 = entry`, and `EPS3` taken from the running kernel with `INTLEVEL` forced
to 0. Inheriting PS rather than fabricating one keeps the task in the same
execution mode as its creator instead of a guessed one.

## 5. Verification method

Three tasks, all created identically:

- **worker-a**, **worker-b** — hold four values live across a compiler barrier
  positioned where an interrupt can land, then verify two of them still equal
  their seed and its complement. Any register or stack fault increments a
  corruption counter. Registers are deliberately *not* pinned with explicit
  `__asm__("aN")` bindings: that claims registers the compiler may already be
  using, and the writes then land on arbitrary memory. An earlier build did
  exactly this and manufactured a bug that did not exist.
- **report** — a task like any other, suspended and resumed on the same
  schedule. That its output stays coherent is itself part of the test.

Instrumentation added during debugging and deliberately retained: entry markers
per task, a dump of the frame about to be restored, the task table, and
per-candidate scheduler probes behind `TRACE_PROBES`.

## 6. The defect

### 6.1 Symptom

Tasks entered correctly and then the board went silent. Switch 4 reported
`2 -> 2` — the scheduler selecting the task it was already running — and
repeated forever. The task table printed microseconds later showed all three
tasks `st=1 (READY)` with stable stack pointers. The scheduler was refusing
candidates that were plainly available.

<!--FIGURE: switch_sequence -->

### 6.2 Two earlier conclusions that were wrong

Both were recorded in the repository before being disproved, and are corrected
here rather than quietly dropped.

**"The timer interrupt is not firing."** It was firing. `ticks=0` was sampled
before the first tick had occurred, because the reporter waited 100 ticks before
printing and the diagnostic loop printed faster than the tick period.

**"The board goes silent, so the switch is fatal."** The switch worked. The
silence was the workers doing exactly what they were written to do: spin without
producing output. Every failure mode in this kernel — masked interrupt, bad
`RFI`, dead task, working-but-quiet task — presents identically as nothing on
the wire. One entry marker per task turned that silence into a sequence and
settled it immediately. That should have been the first instrument, not the
fourth.

### 6.3 Root cause

GCC compiled the round robin into an Xtensa **zero-overhead `LOOP`**:

<!--FIGURE: loop_defect -->

The `next = g_current` fallback — the value meaning "no candidate matched" —
occupies the `LEND` slot. In the hardware-loop form, that assignment reaches the
caller even when a candidate did match.

The bug was found by instrumenting the loop, and the instrumentation *made it
disappear*. That was the real clue: a loop body containing a call cannot be a
zero-overhead loop, so adding `uart_puts()` silently changed the code
generation. Disassembly confirmed the `LOOP` and the fallback's position.

### 6.4 A hypothesis that measurement rejected

The obvious explanation was stale `LCOUNT`: a task suspended mid-loop, its loop
state lost across the switch, the hardware later branching back to a previous
`LEND`. The context frame was extended to save `LBEG`/`LEND`/`LCOUNT` and the
build reflashed.

**The symptom did not change**, and every dumped frame showed
`lct=0x00000000`. No task had ever been suspended mid-loop. The hypothesis was
wrong.

The frame change was nevertheless **kept**, on its own merits: GCC emits `LOOP`
for ordinary counted C loops, so a task *can* be suspended mid-loop, and losing
that state would corrupt it on resume. ESP-IDF's own context switch saves these
three registers for precisely this reason. It is a real fix for a real hazard —
it is simply not the fix for *this* defect.

### 6.5 The controlled comparison

Identical source in all three arms; only the code generation differs.

| Selection loop compiles to | Switch 4 | Result |
|---|---|---|
| Hardware `LOOP` | `2 -> 2` | Broken |
| Ordinary branches — `uart_puts()` in body | `2 -> 0` | Correct |
| Ordinary branches — `volatile` counter | `2 -> 0` | Correct |

The shipped fix is a `volatile` loop counter, which denies GCC the constant trip
count it needs to emit `LOOP`.

### 6.6 Status: workaround, not root cause

**This is not closed.** The result is reproducible and unambiguous, but *why*
the hardware loop mis-selects is unexplained — stale `LCOUNT` was ruled out by
measurement, so the mechanism is something else. Candidates not yet
distinguished: the early `break` leaving the loop armed and interacting with
interrupt entry; a code-generation defect around the signed-modulo idiom inside
a `LOOP` body; or an erratum.

Operationally this means **one `volatile` qualifier is load-bearing.** Removing
it as untidy reintroduces a scheduler that starves every task but one, and does
so silently. The declaration carries a comment saying so. Resolving it properly
requires single-stepping the loop under JTAG and watching the candidate register.

## 7. Results

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

**Integrity.** `corrupt=0` across ~1,200 preemptions. Both workers held their
invariants across every suspension.

**Stacks.** 463 of 512 words free in both workers, stable across the run: about
196 bytes used, no growth. Guards intact.

**Switch cost.** 96 bytes of stack and 21 register transfers per switch, twice
(save and restore).

## 8. Watchdog — corrected from inference to measurement

UM-CYDOS-006 and UM-CYDOS-008 recorded the watchdog state as "inference, not
measurement", reasoning that survival through the capture window implied nothing
was armed. **That inference was wrong.** The bootloader arms the RTC watchdog and
expects the application to take ownership. M0 and M1 survived by luck of timing.

It has now been measured: `RTCWDT_RTC_RESET` on every boot once M2 kept the CPU
busy, and `WDTCONFIG0` reading back `0x0` after the disable. The RTC watchdog and
both timer-group watchdogs are now disabled explicitly at kernel entry, behind
the `0x50D83AA1` write-protect key, and re-locked afterward.

They are disabled rather than fed, because a watchdog is only useful once
something is responsible for feeding it. Until the scheduler owns that duty, an
armed watchdog reboots working code. Re-enabling belongs in M3 with an idle task
feeding it, at which point it becomes a genuine hang detector.

## 9. What M2 does not establish

- **No memory protection.** The ESP32 has no MMU paging. A native task can
  corrupt any other; guards catch overflow, nothing catches a wild pointer.
  Isolation arrives only with the bytecode VM (UM-CYDOS-001 §4.2).
- **No blocking.** Tasks cannot sleep or wait. `TASK_READY` is the only live
  state, so a task with nothing to do burns its full slice.
- **No priorities.** Strict round robin.
- **Fixed ceiling.** `TASK_MAX = 4`, 2 KB stacks, statically allocated.
- **No idle task.** With every task busy this has not mattered; it will as soon
  as blocking exists.
- **Single core.** APP_CPU is untouched.
- **The §6 defect is not root-caused.**

## 10. Metrics

| Quantity | Value |
|---|---|
| Image size | 5,040 B |
| Frame | 96 B / 21 words |
| Tick interval | 800,000 cycles (~10 ms) |
| Tasks | 3 of 4 slots |
| Stack per task | 2 KB; ~196 B peak use |
| Switches observed | 1,207+ |
| Corruption events | 0 |
| Build cycles spent on §6 | 9 |
| Wrong hypotheses recorded | 3 |

## 11. References

- UM-CYDOS-001 §4.2 — isolation model and why native tasks are not applications
- UM-CYDOS-003 — `call0` ABI selection; why register windows are absent
- UM-CYDOS-006 §6, UM-CYDOS-008 §6 — watchdog notes corrected by §8 above
- UM-CYDOS-008 — M1 interrupt dispatch and register-integrity measurement
- `kernel/vectors.S` — handler and frame layout
- `kernel/task.c` — scheduler, fabricated frames, and the `volatile` of §6.6
- `kernel/watchdog.c` — disable sequence and register addresses
