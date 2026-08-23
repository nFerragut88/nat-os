# UM-NATOS-044 — Windowed Frames Survive Preemption

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-23 · Status: **Milestone. The pin is removed. One blocking-path fault remains.**

---

## 1. Abstract

The kernel can now preempt a task that is executing windowed vendor code and
restore it correctly. That has not been true at any point in this project, and
the pin — the mechanism that has stood in for it since step 14 — is off.

This report records what changed, what it cost, what it did not fix, and what
the next person should read first. It covers `next_moves/08` steps 128–167.

The single fact that unblocked eleven failed builds is one bit:

```
CWOE  <-  if PS.EXCM then 0 else PS.WOE
```

---

## 2. State before and after

| | before (step 127) | after (step 167) |
|---|---|---|
| windowed frames across a switch | destroyed | **survive** |
| the pin | required | **off** |
| `wintorture`, pin off | immediate panic | **CORRECT, 10 real switches** |
| `wifiinit` | no fault (pinned) | no fault (**unpinned**) |
| heap usable | 79,680 B | 76,400 B |

Measured, flash-verified, at the committed default:

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
wincollide ran   wintest ok   blobphy rc=0   blobtx force 0x3004
wifiinit   no fault, no reset
```

The `switches during the call` line is quoted deliberately. `wintorture` prints
it as its own control, and steps 115–119 drew four conclusions from runs where it
read `0 -- NONE, so this proves nothing` (step 120).

---

## 3. What was actually wrong

### 3.1 Tier B, and why it took eleven builds

Tier B saves all 64 physical address registers per task on switch-out and
restores them on switch-in, making the register file private rather than shared.
UM-NATOS-043 costed and recommended it.

Every component was verified individually, and early:

- the save is bit-exact (step 129);
- the restore lands all 64 registers — `all 64 landed` (step 136);
- `g_win_union` must be deleted alongside it (steps 132, 133);
- the grant must come from `frame[88]`, the task's real `WINDOWSTART` (step 140).

**All four were correct.** Assembly still failed eleven times, always the same
way: the restore broke `wintorture` *with the pin on* — breaking the working case
before it could be judged on the case it was built for.

### 3.2 The cause: `PS.EXCM`

From the Xtensa ISA, §6.1.3, Window Overflow Check:

```
procedure WindowCheck (wr, ws, wt)
   n <- if (wr != 0 or ws != 0 or wt != 0) and WindowStart[WindowBase+1] then 1
        ...
   if CWOE = 1 and n != 0 then  PS.OWB <- WindowBase ... PS.EXCM <- 1 ...
```

with `ref()` defined as "1 if the register is used by the instruction".

**The overflow check fires on any register reference, not only on `ENTRY`.** An
`s32i` after a `ROTW` traps whenever the next window's `WINDOWSTART` bit is set.
Every attempt from step 128 on rested on the opposite assumption.

And the gate is `CWOE <- if PS.EXCM then 0 else PS.WOE`. `_handler_level3`
**clears `PS.EXCM` at entry**, deliberately, so a fault inside the handler reaches
the panic handler rather than the double-exception vector. That clear sits
between the prologue and the restore:

| site | `PS.EXCM` | `CWOE` | check | outcome |
|---|---|---|---|---|
| prologue save pass | 1 | 0 | off | worked from step 131 |
| every restore attempt | 0 | 1 | on | failed, eleven times |

Identical code, opposite results, because of one bit nobody had checked.

The fix is two instructions: set `PS.EXCM` for the duration of the rotation.

**Step 155 had done the exact opposite by accident** — setting `WINDOWSTART` to
all-ones across the rotation, which guarantees `WindowStart[WindowBase+1]` at
every stop and so maximises the fault. It was recorded as "not supported" without
recognising why.

---

## 4. Four unrelated defects found on the way

Each was present since `_handler_level3` was written, each was hidden by the pin,
and each was confirmed by a **changed symptom** rather than by argument.

| step | defect | evidence it went |
|---|---|---|
| 142 | tasks created with `WINDOWSTART = 0` — architecturally meaningless; the restore never read the word, so the seed was always overwritten | drift checker stopped firing |
| 145 | the switch frame written through the CALL12 extended save area (`[caller_sp-48 .. caller_sp-20]`) | `_WindowUnderflow12` signature gone |
| 147 | handler scratch stranded by the window rotation — `a3` kept its value in a physical register the epilogue never reloads | `0xaa8a` gone from `excvaddr` |
| 114 | `OSI_FOREVER_CAP` meaning ticks on one side of the ABI boundary and 1.5 ms spins on the other — 4 s vs 600 ms | report timing moved 152 s → 12.7 s |

Step 145's reserve (`TASK_FRAME_RESERVE 48`) is kept on arithmetic grounds
independent of any symptom: a CALL12 frame's `a4..a11` live 48 bytes below the
caller's sp, the interrupted sp is a caller's sp, and the switch frame was
written straight through it.

---

## 5. Cost

```
heap 76,400 B usable   (79,680 at the start of this work)
  3,072 B   Tier B register-file slots, 12 tasks x 256 B  (exactly as costed, 043 §5.1)
    112 B   radio allocation metering
     96 B   diagnostics
```

`TASK_FRAME_RESERVE` costs 48 bytes of stack per task, not heap. Tightest stack
measured 1,492 of 2,048 B free.

**The `~2.1 µs` per-switch estimate in UM-NATOS-043 §5.2 is still unmeasured**
and should be confirmed with `xt_ccount()` before it is quoted as a fact.

---

## 6. What the radio still needs

Tier B removed the pin. It did **not** make the blob's blocking wait work.

### 6.1 The park fault, well-bounded

Replacing step 113's busy-spin with a real `task_sleep()` reproduces the original
`wifiinit` fault:

```
exccause 28   epc 0x400800d5   = _WindowUnderflow8 + 0x15,  l32e a4, a7, -32
excvaddr 0x00060500   ->  a7 = 0x00060520
```

Established:

- **deterministic** — identical across sleep durations 1, 4, 16 ticks (159);
- **independent of Tier B and of the pin** — unchanged with both removed (164);
- **`a7` holds a PS value where a caller's stack pointer belongs**.

Eliminated: rotation displacement (166, measured net-zero); `bit(base) CLEAR` as
an anomaly (UM-NATOS-042 §5, re-confirmed 166 — it is the normal state inside an
underflow handler); any race or timing window (159).

Not an anomaly, contrary to step 165: `a3` holding a PS. `RSIL` returns the full
previous PS and `osi_qpoll_w`, `blob_trylock_w` and `blob_unlock_w` all use it
immediately before the park (167).

**The defect is a frame link holding a processor-state word.** The economical
account is a save area written from the wrong register or read at the wrong
offset, and that is checkable against the handler's store list rather than by
experiment.

### 6.2 And beyond it

UM-NATOS-042 §9.5 is unchanged and is the larger part: interrupts were never
wired (`_set_intr` clamps and counts), `_task_delay` returns immediately, the
timer entries are stubs, event callbacks never fire, and there is no data path
above the MAC. The radio has requested 104 bytes of heap across four allocations
(step 127) — it has not begun.

---

## 7. Method — what this stretch cost, and why

Forty steps produced roughly a dozen real findings. The failures cluster, and
naming them is cheaper than repeating them.

**Instruments that changed what they measured — four in six steps.** Steps 152,
153, 154 and 158 each added a capture *into the path under test*. The two that
worked (146, 151) read data the kernel was **already recording**. The rule that
falls out: *do not add reads to the blocking path.*

**Instruments that could not report their own state.** Step 158's probe printed
"park never reached" both when the park was never reached and when it was reached
and never returned — the two outcomes it existed to separate.

**Findings already in the log, re-derived.** Three times: step 156 re-measured
step 14's conclusion about rotating inside a context switch, 140 steps later;
steps 165 and 166 re-raised `bit(base) CLEAR`, which UM-NATOS-042 §5 already
lists as eliminated. **Read §5's table before building on an anomalous dump
line.**

**A control line discarded.** `wintorture` prints `switches during the call` and
says outright when a run proves nothing. Four steps of conclusions rested on a
grep for the word CORRECT.

**A toolchain failure read as a result.** A flash refused with
`Could not open COM5`; the suite that followed exercised the previous image and
was reported as green. `build.ps1` now counts `Hash of data verified` and refuses
fewer than three — though the honest correction is that the exit code was already
checked and the failure was already printed. The gap was in the reading.

**And the answer was in a manual nobody opened.** Thirty steps of experiment
produced a correct empirical rule — *rotating in the restore path breaks the
working case* — and no explanation. The explanation was four pages of the ISA
reference and one `pdftotext`.

---

## 8. Next steps, in order

1. **Confirm the switch cost.** `xt_ccount()` around the save and restore. 043
   §5.2's estimate is unverified and is the only number in that report not
   measured.
2. **Read the store list for the park fault.** `a7` is recovered from `[a9-12]`;
   find what writes that word. No new probe — the failing path is four
   instructions bounded by a spill and a `retw`.
3. **Then the blocking wait.** With the park fixed, step 113's spin becomes a
   real sleep, and the radio can wait on an interrupt rather than burning 600 ms
   per `_queue_recv`.
4. **Then interrupts.** UM-NATOS-042 §9.5. Nothing above the MAC works until
   `_set_intr` does something.
5. **Instrumentation debt.** Probes across five files, several built on premises
   since disproved. 042 §9.3 has warned about this since step 102 and it has
   grown since. It wants a file-at-a-time pass with a build between each.

---

## 9. Reproducing the before/after

`BLOB_PIN_DISABLE` in `kernel/blobcall.c` is kept as a switch rather than
deleted. Set it to `0` and `wintorture` reproduces step 121's baseline:

```
*** KERNEL PANIC ***  IllegalInstruction  epc 0x6eeeeeee
frames : task 5 held 0x0000aa8a granted 0x00000008 LOST 0x0000aa82
```

That is the strongest before/after comparison in the investigation and deleting
the switch would make it unreproducible.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 128–167.
Companion reports: UM-NATOS-038 (rev 1.5), 041 (rev 2.0), 042 (rev 1.1),
043 (rev 1.3).

**Nothing has been on air.**

Written by: Hare
