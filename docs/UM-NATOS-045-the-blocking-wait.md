# UM-NATOS-045 — The Blocking Wait

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-23 · Status: **Milestone. The radio's blocking wait is a real sleep. The pin is off.**

---

## 1. Abstract

The WiFi blob's blocking wait no longer burns the CPU. `_queue_recv` sleeps
instead of spinning, on an unpinned scheduler, with windowed frames surviving
genuine preemption.

The defect was `spill_before_parking()` — a routine that existed to make parking
safe and, once Tier B removed the need for it, was the only thing making parking
unsafe.

This report covers `next_moves/08` steps 163–176. UM-NATOS-044 covers 128–162
and should be read first.

---

## 2. What changed

| | before (step 162) | after (step 176) |
|---|---|---|
| the pin | on | **off** |
| blob's blocking wait | busy-spin, 600 ms per `_queue_recv` | **real sleep** |
| `rounds/wait` | 191 | **24** |
| `wifiinit` | no fault (spinning) | **no fault (sleeping)** |

Measured, flash-verified, at the committed default:

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
wincollide ran   wintest ok   blobphy rc=0   blobtx force 0x3004
wifiinit : NO FAULT, NO RESET
[qr] budget spent, still waiting  calls=4 timeouts=3 rounds/wait=24
heap 76,368 B usable
```

---

## 3. The defect

### 3.1 Where the spill runs

Every parking primitive calls it — `task_block`, `task_sleep`, and `task_yield`,
which `task_sleep_armed` reaches. On the blob's blocking path that means it
executes **inside a call0 callee entered from a windowed bridge**.

The ISA is explicit about what lives there:

> The window save area for a frame is addressed with **negative offsets from the
> next stack frame's sp**. Four registers are saved in the base save area.

So the bridge's caller's `a0..a3` live at `[bridge_sp-16, bridge_sp)`. A call0
callee allocates its frame with `addi a1, a1, -size`, landing on exactly that
range. Measured at step 170:

```
park a1                        = 0x3ffb27c0
stub's save area below bridge  = [0x3ffb27c0, 0x3ffb27d0)
```

Exactly coincident. The spill writes the stub's registers into live call0
locals, they overwrite each other, and the bridge's `retw` underflows into the
wreckage — `_WindowUnderflow8 + 0x15`, `l32e a4, a7, -32`, with `a7` holding a
processor-state word scavenged from a dead switch frame.

### 3.2 Why step 113's spin worked

It makes no call0 call after the spill. Nothing is ever allocated over the save
area, so nothing is corrupted. The spin was not a fix for a solved problem; it
was the problem being routed around, at a cost of 600 ms of CPU per wait.

### 3.3 Why removing the spill is correct

`spill_before_parking()` exists because windowed frames did not survive
preemption (UM-NATOS-038 §12.3). **Tier B made them survive** — step 163
measured ten genuine preemptions with eight frames live and the checksum correct
every time. Reducing a task to one frame before it parks buys nothing now, and on
this path it was costing correctness.

This is a removal, not a workaround.

---

## 4. The "no ENTRY" hypothesis

The search had spent six steps on instruments that perturbed what they measured
and four more eliminating candidates. It was reoriented by a question asked from
outside the work:

> *is it failing because there is no `ENTRY` command for the window?*

That is the right shape for the evidence, and the ISA supports it. `ENTRY` is
what sets a bit — `WindowStart[WindowBase] <- 1`, §8.3.106 — and an **overflow**
is what writes the save area beneath it. The two are set by different events, so
a frame can have one without the other. A bit with no save area behind it is
exactly step 169's finding and step 126's phantom, and it produces precisely the
observed fault.

The first candidate it suggested — a task's initial frame, which step 142 gave a
bit and for which no `ENTRY` ever runs — was **wrong**, and testing it cost one
build (step 173). But the reframing was right, and it is what mattered:

**stop asking who wrote a bad value, and start asking which bit is set or cleared
without its matching event.**

That question has a short answer set. `WINDOWSTART` is written outside
`ENTRY`/`RETW` in exactly four places, so all four were tagged with a counter and
the value each last wrote. One run:

```
ws write : restore  n=6339  last=0x00000008     <- one bit
ws write : x20wipe  n=0
ws write : phypre   n=1  last=0x00000002
ws write : phypost  n=1  last=0x00000002
```

The task had been **saved holding a single frame**. Something had spilled during
the park when nothing should have — and `task_sleep()` calls
`spill_before_parking()` as its first act.

Six steps of instruments had produced nothing. One reframed question and one
four-site counter produced the defect in a single run.

---

## 5. Two invalid tests, both mine

The spill had been "eliminated" twice before this, and both eliminations were
wrong in the same way: **I removed a spill, not the spill.**

- **Step 172** removed `win_spill_call0()` from `osi_impl_park` and concluded
  "the spill is not implicated". `task_sleep` spilled anyway.
- **Step 175** called `task_sleep_armed()` directly to skip that. It reaches
  `task_yield()`, which also spills. The fault changed to a stack-guard overrun,
  which looked like progress and was the blob simply running longer before
  hitting the same thing. Enlarging the stack brought the underflow straight
  back.

Disabling `spill_before_parking()` at its single definition is the first test
that actually removed it from every parking path.

The lesson is narrow and worth keeping: **when eliminating a mechanism, remove it
at its definition, not at a call site.** Both false eliminations came from
patching one caller of three.

---

## 6. What this does not fix

The radio still does not work, and the remaining list is unchanged from
UM-NATOS-042 §9.5:

- interrupts were never wired — `_set_intr` clamps and counts;
- `_task_delay` returns immediately;
- the timer entries are stubs;
- event callbacks never fire;
- there is no data path above the MAC.

The blob has requested **104 bytes** of heap across four allocations. It has not
begun. What has changed is that the wait it performs while getting nowhere is now
a sleep rather than a spin, which is the precondition for waiting on an interrupt
at all.

---

## 7. Kept, and why

| change | basis |
|---|---|
| `spill_before_parking()` disabled | Tier B made it obsolete; measured as harmful |
| the park (`osi_impl_park` via `w2c_call2`) | replaces the busy-spin |
| `WINDOWSTART` writer tagging | produced §4's finding in one run; four counters |
| pin off (`BLOB_PIN_DISABLE 1`) | UM-NATOS-044 |

The blob task stack stays at 7,168 B. Step 175 raised it to 12 KB against a
guard overrun that belonged to the broken intermediate state; with the spill
removed the original size is sufficient, and the increase was reverted.

`BLOB_PIN_DISABLE` remains a switch rather than being deleted — step 121's
baseline is only reproducible with it back at `0`, and that is the strongest
before/after comparison in the investigation.

---

## 8. Next

1. **Wire interrupts.** `_set_intr` is the gate on everything above the MAC, and
   a wait that sleeps until an interrupt arrives is now expressible.
2. **Confirm the switch cost.** UM-NATOS-043 §5.2's `~2.1 µs` estimate is still
   unmeasured — the only number in that report not verified.
3. **Instrumentation debt.** Probes across six files now, several built on
   premises since disproved. UM-NATOS-042 §9.3 has warned since step 102.
4. **The save-area overlap remains latent.** Removing the spill removed the only
   path that exercised it, but a call0 callee entered from a `w2c_*` bridge still
   allocates over its caller's base save area. Steps 171–172 established that a
   plain `addi a1` cannot fix it — the ISA requires `MOVSP` and an Alloca
   handler, which this kernel does not have. It is dormant, not gone.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 163–176.
Companion reports: UM-NATOS-038 (rev 1.5), 041 (rev 2.0), 042 (rev 1.1),
043 (rev 1.3), 044 (rev 1.0).

**Nothing has been on air.**

Written by: Hare
