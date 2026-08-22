# UM-NATOS-043 — Confining the Register File

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-22 · Status: **Design and costing. Not implemented. Recommends Tier B.**

---

## 1. Abstract

Steps 115–126 of `next_moves/08` tried five placements of a spill-on-preemption
sweep and ended by proving the approach cannot work as conceived. The blocker is
not the sweep: it is that the Xtensa register file is a resource shared by every
task with no ownership record, and at least one window in it is claimed by
nobody.

This report costs the alternative — give each windowed task its own copy of the
register file — and recommends it. It exists because the recommendation is a
deliberate reversal of a decision made at step 115, and a reversal deserves its
reasoning written down rather than asserted.

The cost is **3,072 bytes of DRAM and roughly 2 µs per context switch**, which is
0.02% of CPU at the measured switch rate. What it buys is the deletion of five
failed designs, the removal of the pin, and containment of a defect that has
blocked WiFi since step 86.

---

## 2. Why the question changed

### 2.1 What was believed

Windowed frames do not survive preemption (UM-NATOS-038 §12.3). The kernel's
answer was to **partition** the register file between tasks: `g_win_mask[]`
records which window each task owns, `g_win_union` is the set of all of them, and
the restore grants a task `(1 << base) | g_win_union` so it does not tread on
anyone else's frames.

Five revisions of that ownership rule failed, at steps 53, 54, 57, 94 and 108.
Step 108 concluded the rule could not be fixed by granting bits back, because the
registers behind them are reused.

### 2.2 What is now measured

**Step 121** — with the pin disabled, `wintorture` dies immediately and names its
own mechanism:

```
frames : task 5 held 0x0000aa8a granted 0x00000008 LOST 0x0000aa82
```

Seven frames held, one granted, six lost. And `granted 0x00000008` is the base
bit alone, so `g_win_union` was **zero** at the moment six frames needed
covering. The partitioning design does not merely leak; with the pin off it
contributes nothing.

**Step 124** — the sweep, on the task's own stack, drives `LOST` to zero and
still fails:

```
overflow  : prev good frame sp 0xeeeeeeee
underflow : recovered a0 0x00000000 from save area 0xeeeeeeee
```

`0xeeeeeeee` is the stack fill pattern. One of the seven windows in
`WINDOWSTART` **is not a live frame at all** — its `a1` is poison. This is the
"ownerless phantom frame set" the X7 comment in `vectors.S` names.

**Step 126** — a guard in the overflow handlers, refusing to store through a
frame pointer with bit 31 set, does not fix it either:

```
epc 0x6eeeeeee
```

`retw` computes `(PC & 0xC0000000) | (a0 & 0x3FFFFFFF)`; with `a0 = 0xEEEEEEEE`
that is exactly `0x6EEEEEEE`. The guard stops the phantom being *written*
through, but `rfwo` still clears its bit, so a later underflow tries to *restore*
a frame that was never saved. The same guard cannot be applied to the underflow:
an overflow that skips has nothing to save, an underflow that skips has nothing
to return to.

### 2.3 The conclusion that forces this report

**A phantom window can be neither spilled nor restored.** Any design that leaves
a `WINDOWSTART` bit set for a window holding no real frame fails the moment
anything walks it — and a sweep is exactly such a thing.

So the problem is not "how do we spill correctly". It is that **the register file
is shared, unconfined state**, and the kernel has been trying to bookkeep
ownership of it in software for seventy steps.

---

## 3. The framing that suggests the fix

`kernel/arena.h` states the kernel's isolation model, and states its boundary:

> This is the ONLY isolation mechanism this kernel will have. […] Native tasks
> are not confined by arenas and never will be; they are trusted code.

Arenas confine *applications*: the interpreter bounds-checks every load and
store, so an out-of-bounds address is **unrepresentable**. Native tasks get a
stack and a guard word, and nothing else.

The register file is a third category that neither covers. It is not memory an
arena could bound, and it is not private to a task the way a stack is. It is one
circular buffer of 64 physical registers that every task uses, with the hardware
recording only *that* a window is live — never *whose* it is.

**`g_win_mask` and `g_win_union` were an attempt to add that missing ownership
record in software.** They are the register-file equivalent of asking each
application to promise to stay inside its arena. That is not how the arena model
works, and it is not why the arena model succeeds.

---

## 3A. The book specified this failure before the kernel existed

Chapter 2 of the book (`docs/book/02-abi.md` §2.2), written to justify choosing
call0 *before the first line of kernel code*, lists three consequences of the
windowed ABI for an operating system. All three are the last seventy steps:

| §2.2, at design time | Measured, steps 115–126 |
|---|---|
| "Live windows belonging to the outgoing task exist in the physical register file and must be spilled first, or restored state will be silently wrong." | Step 121: `task 5 held 0x0000aa8a granted 0x00000008 LOST 0x0000aa82` |
| "The spill mechanism must work during the switch itself, which constrains what the switch code may touch." | Five sweep placements, steps 115–119; and §8.1's flaw in this report's own plan |
| "Failures do not manifest at the switch. They manifest several returns later, in a function that did nothing wrong, with a corrupted frame." | `epc 0x6eeeeeee` — a `retw`, several returns downstream, in code that did nothing wrong |

It also quotes UM-NATOS-003 §2: *"This is the single hardest part of writing an
Xtensa kernel and the point at which such projects most often stall."*

nat-os chose call0 to avoid that entirely, and §2.5.2 records the price —
"Any third-party library compiled as windowed cannot be linked in" — with
UM-NATOS-024 concluding contentedly that "WiFi will never run and ADC2 is
permanently free". The bridges in §2.7 overturned that, and in doing so
reintroduced precisely the problem call0 was selected to avoid. This report is
the bill for that.

### 3A.1 The distinction that decides Tier B

§2.2 says live windows "must be **spilled** first". That is the standard answer,
and it is what steps 115–126 attempted in five placements.

**A spill trusts `WINDOWSTART`.** It walks the set bits and writes each claimed
frame out through the frame's own stack pointer. Step 124 measured a bit that no
frame backs, whose `a1` is `0xeeeeeeee`. So every spill-based approach was
doomed by the data, not by where it was placed — which is why five placements
produced five failures and one withdrawn conclusion.

**A wholesale save does not trust `WINDOWSTART`.** It copies 64 registers
because they exist, indifferent to what any bit claims about them. A phantom is
saved and restored as faithfully as a real frame, and a frame that is wrong in
private harms nobody.

That is the deepest reason to prefer Tier B, and it is stronger than the cost
argument in §5: **the sweep was not merely awkward to place, it was asking a
register that has been proven to lie.**

---

## 4. Tier B: give each windowed task its own register file

On every switch involving a task that uses windows, save all 64 physical address
registers plus `WINDOWBASE` and `WINDOWSTART`, and restore the incoming task's.

The register file stops being shared. Each task's window state is private, which
means:

- **No ownership bookkeeping.** `g_win_mask[]`, `g_win_union`, the grant
  computation in the restore, and the five revisions behind them are deleted.
  Nothing needs to know whose window is whose, because no task can see another's.
- **No sweep.** Spill-on-preemption exists to move frames to memory before
  another task overwrites the registers. If the registers are saved wholesale,
  there is nothing to move.
- **The pin can go.** The pin exists because windowed frames do not survive
  preemption. With the register file saved and restored, they do.
- **The phantom is contained.** A bit set for a window that never held a frame
  stays with the task that produced it. It cannot be inherited, walked, or
  spilled by anyone else.

### 4.1 Mechanics

`a0..a15` at `WINDOWBASE = b` alias physical registers `AR[(b*4 + i) mod 64]`, so
four positions — `b = 0, 4, 8, 12` — expose all 64 with no overlap.

Per position: rotate, stash the window's `a0` in a special register, load a
pointer into `a0`, store `a1..a15`, recover the stashed `a0` into an
already-saved register, store it. Roughly 21 instructions, four times, for the
save; the same for the restore.

No `entry`, no `retw`, no `call` — only `wsr`/`rsr`/`movi`/`s32i`/`l32i`. **No
window exception can occur inside the sequence**, which is what makes it safe in
a context where `spill_before_parking()` and every sweep placement were not.

The save area is a `.bss` array, **not** the switch frame. `TASK_FRAME_BYTES`
stays 112. This matters: the tightest stack measured is the shell at 736 of 2048
bytes free, and adding 256 bytes to every frame would consume a third of that
margin for a structure that does not need to be on the stack.

---

## 5. Costing

### 5.1 Memory

Current image, measured (`xtensa-esp32-elf-size -A`):

```
.text        57,852        .flash.text  24,324
.data         1,516        .bss         62,224
heap usable  79,680        DRAM total  147,456
```

64 registers × 4 bytes = **256 bytes per task**.

| Scope | `.bss` added | Heap after | Change |
|---|---:|---:|---:|
| All 12 slots | 3,072 | 76,608 | −3.9% |
| Windowed tasks only (2) | 512 | 79,168 | −0.6% |

`.bss` and the heap share DRAM — `.data + .bss + heap` = 143,420 of 147,456 — so
`.bss` growth is heap loss, one for one. **3,072 bytes is the honest figure** for
the simple version that treats every slot alike; 512 is available if the
allocation is made conditional, at the cost of a per-task flag and two code
paths. The recommendation is to pay the 3,072 and keep one path.

### 5.2 Time

~170 instructions per switch, single-cycle class, at 80 MHz ≈ **2.1 µs**.

Measured switch rate: `switch-in : n 6176` over a ~60 s run ≈ 100/s.

```
100 switches/s × 2.1 µs = 210 µs/s = 0.021% of CPU
```

Against a 10 ms tick (`TICK_INTERVAL_CYCLES` 800,000 at 80 MHz), one switch costs
0.02% of a tick period. **Latency, not throughput, is the thing to watch**, and
2.1 µs added to interrupt-to-task latency is well inside the tick.

This is not a measurement. It is an estimate from instruction counts, and it
should be confirmed by counting cycles across the sequence before and after —
`xt_ccount()` is already used for exactly this elsewhere.

### 5.3 Complexity

**Removed:** `g_win_mask[]`, `g_win_union`, `g_win_base[]`, the grant computation
in `_handler_level3`, the X6/X7 restore recorders that exist to police it, and
`spill_before_parking()`'s reason to exist. That is a net *reduction*, and it
retires the machinery behind steps 53, 54, 57, 94 and 108.

**Added:** one save loop and one restore loop, in assembly, in the interrupt
prologue. Roughly 60 lines. Every instruction in them is a special-register move
or an aligned word access; there is no control flow that can fault.

### 5.4 Risk

| Risk | Severity | Mitigation |
|---|---|---|
| Rotating `WINDOWBASE` in the handler | Medium | No windowed instruction executes inside the sequence, so no window exception can fire. This is the property every previous placement lacked. |
| Scratch register aliasing after rotation | Medium | Each position stashes its `a0` in a special register before using it as a pointer. The failure mode is loud (wrong values restored) rather than silent. |
| Layout shift from +3 KB `.bss` | Low | Step 117 measured this concern and disproved it — a 1 KB `.bss` addition changed nothing. |
| Latency regression | Low | 2.1 µs against a 10 ms tick. Confirm by measurement, not assumption. |
| Does not fix the phantom | **Certain** | See §6. This is containment, not repair, and the report says so rather than discovering it later. |

---

## 6. What this does not do

**It does not eliminate the phantom window.** A `WINDOWSTART` bit set for a
window that never held a frame will still be set, still be saved, and still be
restored — to its owner. Whatever produces it is unchanged.

What changes is the blast radius. Today the phantom is in shared state, so
anything that walks the register file walks it, and the failure lands wherever
the walk happens. With Tier B it stays inside one task, exactly as a rogue
application's bad address stays inside one arena.

That is the honest analogy to the isolation model, and its limits are the same
ones the model already documents:

- an application's out-of-bounds address is **unrepresentable** — prevention;
- a native task's is **not preventable at all** on this hardware — no MMU,
  no interpreter between the code and memory.

**Tier B buys containment, not prevention.** The blob task can still fault on its
own phantom. It will no longer take another task's frames with it.

Finding the phantom's source remains open, and is the older question steps 53, 54
and 57 were circling. Tier B makes it survivable rather than blocking, which is
the difference between a defect and a stoppage.

**It also does not, by itself, make the radio work.** UM-NATOS-042 §9.5 still
stands: interrupts were never wired, `_task_delay` returns immediately, the timer
entries are stubs, and event callbacks never fire. This unblocks one thing.

---

## 7. Why not the alternatives

**Tier A — check the doors.** Range-check every pointer the blob passes through
the OSI table and the bridges, as `H_OK()` already does for queue handles. Real
hardening, cheap, and worth doing — but it checks the direction that can be
checked, and the register file is not reached through a door. It does not touch
this problem.

**Tier C — terminate the blob task instead of panicking.** There is no native
task termination in this kernel at all: no `task_kill`, no `task_exit`. The panic
handler already knows the faulting task. Adding it would turn "the board resets"
into "WiFi is down, the shell still works", which is the survivability half of the
rogue model. Worth doing, and **after** Tier B — because with the register file
shared, a fault is not attributable to one task, and terminating the wrong one is
worse than halting.

**Spill-on-preemption.** Closed by step 126, on evidence: a phantom can be
neither spilled nor restored.

**Keeping the pin.** The pin works, and everything green today is green because
of it. But it forbids preemption inside windowed code, which forces every
blocking wait in the vendor path to be a busy-spin — step 113's fix is exactly
that, burning 600 ms of CPU per `_queue_recv`. A wait that must sleep until an
interrupt arrives cannot be written under the pin, and interrupts are the next
thing that has to work.

---

## 8. Implementation and test plan

1. **Add save and restore together, as an identity.** *(Corrected — the
   original plan said "save only, unused", and that is not implementable. See
   §8.1.)* Save all 64 ARs to the task's slot and immediately restore all 64
   from that same slot, leaving the existing grant computation in place. If both
   loops are right the pair is a no-op, so the suite staying green tests them
   without anything yet depending on the data. Measure the added cycles with
   `xt_ccount()` in the same build.
2. **Switch the restore over.** Take `WINDOWBASE`/`WINDOWSTART` from the saved
   register file rather than from the frame, and delete the
   `(1 << base) | g_win_union` grant. Suite unchanged is the bar.
3. **Delete the bookkeeping.** `g_win_mask[]`, `g_win_union`, `g_win_base[]`, and
   the recorders that police them. Suite unchanged is the bar.
4. **Disable the pin** (`BLOB_PIN_DISABLE 1`) and run `wintorture`. This is the
   test, and it must be read with its control: `switches during the call` must be
   non-zero, or the run proves nothing (step 120). Expect `checksum CORRECT` with
   switches occurring, against step 121's baseline of an immediate
   `IllegalInstruction`.
5. **Then `wifiinit`,** and only then consider replacing step 113's busy-spin
   with a real blocking wait.

### 8.1 Why step 1 cannot be save-only

The original plan called for adding the save first, with nothing reading it — the
discipline that kept the tree green through steps 115–126. It does not work here,
and the reason is worth recording because it is a property of the mechanism
rather than of the code.

At each `WINDOWBASE` position, saving the sixteen visible ARs needs two scratch
registers **belonging to the window being saved**: one to hold the array pointer,
and one to carry the next base into `wsr.windowbase`. Their values do reach
memory, but the register file is left holding a pointer and a loop constant.

The rotation cannot repair it. `wsr.windowbase` takes an AR operand, and the
instant it executes the view has moved, so the clobbered register is no longer
addressable. A pass that visits four positions therefore writes all 64 registers
out correctly and **leaves three of the four windows wrong**.

This is why real implementations never separate the two halves: the restore pass
rewrites every AR from memory, so clobbering during the save is harmless by
construction. The two loops are only correct as a pair.

The safe first step is therefore an **identity** — save, then immediately restore
from the same slot. It exercises both loops, depends on nothing, and is a no-op
when correct, so a green suite is a real test of it. That preserves the intent of
the original step 1 (add nothing that anything depends on) while respecting the
mechanism.

Each step builds, flashes, and runs the suite. Any step that regresses is
reverted before the next is attempted — the discipline steps 115–126 used, which
is why the tree is green after eleven failed attempts.

---

## 9. Recommendation

**Implement Tier B.**

It costs 3,072 bytes of a 79,680-byte heap and 0.02% of CPU. It deletes more code
than it adds. It retires five failed designs, removes the pin, and contains a
defect that has blocked this work since step 86 — without pretending to have
fixed it.

The decision it reverses is step 115's, where saving the whole register file was
set aside as expensive without being costed. That was the wrong call, and it was
wrong in a specific way worth naming: the cost was estimated from the operation's
size rather than from what it removed.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 115–126.
Companion reports: UM-NATOS-038 (rev 1.5), 041 (rev 2.0), 042 (rev 1.1).

**Nothing has been on air.**

Written by: Hare
