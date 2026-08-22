# UM-NATOS-041 — The Callback That Cannot Block

**Used Medias LLC — Embedded Systems Division**
Revision 1.1 · 2026-08-22 · Status: **Failure isolated to a single measured mechanism. Not fixed. The remaining callback architecture is scoped and largely unbuilt.** Rev 1.1 corrects a task attribution in section 3 and adds section 3.4.

---

## 1. Abstract

UM-NATOS-039 root-caused and fixed the phantom WINDOWSTART bits, and in doing so
exposed a new downstream failure that it deliberately left alone. This report
characterises that failure, names its mechanism from measurement, and — because
the question was raised directly — sets out what the vendor stack still requires
of callbacks, which of those requirements are met, and which have not been
started.

The failure: `esp_wifi_init_internal` now reaches OSI entry 29, `_queue_recv`,
and dies with `IllegalInstruction` at the `retw.n` that exits `w2c_call2`, the
windowed-to-call0 bridge. The mechanism is that a task's live window frames are
dropped from WINDOWSTART across a context switch, so its caller frames' physical
registers are reused and `a0` is no longer a return address when it resumes.

This is not a vendor problem. It is nat-os's own window-ownership rule meeting
the one case the design never covered: **a callback that blocks.**

---

## 2. Where the failure now stands

Post-039, the fault is materially different from the one 038 and 039 describe:

| | before 039 | now |
|---|---|---|
| exception | StoreProhibited, **double** | IllegalInstruction, first-level |
| location | `_WindowOverflow12+9`, a window vector | `w2c_call2`'s `retw.n` |
| WINDOWSTART | `0xe4c8` — the low half of a stack pointer | `0x2000` — one clean bit |
| overflow probe | recovered `0xeeeeeeee` (STACK_FILL) | recovers real DRAM addresses |
| blob progress | OSI entry 15, `_semphr_take` | OSI entry 29, `_queue_recv` |

Every one of those is an improvement. The window state is now coherent, the
handlers recover real pointers, and the driver gets measurably further into its
own initialisation before failing.

---

## 3. The mechanism, measured

An eight-entry ring records `{seq, a0, a1, WINDOWSTART}` at the `retw` itself, so
the newest entry *is* the faulting crossing rather than a value assumed to belong
to it:

```
#5  a0 0x8008cfa8  n=2  a1 0x3ffb9240  ws 0x00002aaa
#6  a0 0x8008d08d  n=2  a1 0x3ffb9240  ws 0x00002aaa
#7  a0 0x8008d03c  n=2  a1 0x3ffb9220  ws 0x00002aaa
#8  a0 0x8008d14a  n=2  a1 0x3ffb9240  ws 0x00002aaa
#9  a0 0x0000000d  n=0  a1 0x3ffb9240  ws 0x00002000
```

Three facts follow directly.

**3.1 The instruction is illegal because `a0` is not a return address.**
`retw` raises IllegalInstruction when `PS.WOE` is clear, `PS.EXCM` is set, or the
callinc field `a0[31:30]` is zero. The first is measured set; the second is
measured clear, and independently confirmed by the absence of a double exception.
`a0 = 0x0000000d` has callinc `n=0`. The condition is now measured on this
silicon rather than recalled from the ISA.

**3.2 Six window frames are dropped across one switch.**
`ws` moves `0x00002aaa` to `0x00002000`. The first is bits 1, 3, 5, 7, 9, 11, 13
— seven live frames. The second is bit 13 alone. `a1` is unchanged across the
transition, so this is the same frame on the same stack: the frames were not
unwound, they were **disclaimed**.

**3.3 The disclaiming is nat-os's own restore rule.**
Steps 56-57 of `next_moves/08` established that a resumed task is granted
`1 << its own base`, and that a non-running task contributes nothing to the
union. That is correct when the task truly holds one frame and wrong when it
holds seven. The caller frames' physical registers are then free for another
context to reuse, which is exactly what happens before this task resumes.

**3.4 Correction to rev 1.0, and a second failure alongside it.**

Rev 1.0 read `last osi : entry 29 _queue_recv` as belonging to the faulting
task. It does not: that counter is global, and the blocking instrumentation
proves the two are different tasks.

```
blk-window: pre ws 0xa00a wb 3 | spill ws ...a2b wb 3 | wake ws 0xdeadbeef | union 0x00000000
sbp-last  : task 9 wb 3 ws 0x0000000a
sbp-post  : wb 3 ws 0x00000008  single-bit ok
retw ring : a1 0x3ffb9240  (inside task 5's stack, 0x3ffb8cf0 + 2048)
```

`blk-window` and `sbp-*` carry `wb 3`, which is task 9 — the blob task. The
faulting `retw` is task 5, the shell. So `_queue_recv` is what task 9 last
called; it is not what task 5 was doing when it died.

Three things follow.

- **`wake ws 0xdeadbeef` is the sentinel: the post-wait sample never ran.** Task
  9 entered the blocking `_queue_recv` and never returned from it. That is a
  second, distinct failure — a callback that blocks forever — and it is not the
  one this report characterises.
- **`sbp-post` reports `single-bit ok`.** The park machinery *does* reduce a
  task to one live frame, for task 9 at least. So the spill is not simply absent.
- **`union 0x00000000`.** With an empty union, a resuming task is granted exactly
  `1 << its own base` and nothing else. That is the disclaiming mechanism of 3.3,
  now confirmed from the other side: task 5 held seven frames, its recorded mask
  was one bit, and the union carried none of the rest.

The mechanism in 3.1-3.3 stands unchanged. What rev 1.0 got wrong was implying a
single task walked the whole path; two are involved, and separating them is what
makes the union reading decisive.

**Not read as fact:** `spill ws 0x0000000a2b` is printed with ten hex digits
where the printer emits eight. The value is suspect until the print is fixed, so
no conclusion is drawn here about whether `win_spill_all()` reduced task 9's
frame count in the stub.

---

## 4. Why the protections in place do not cover it

Three mechanisms exist to keep windowed code safe, and each has a documented
boundary that this path crosses.

**4.1 The pin** stops the scheduler switching away from a task inside windowed
code. The blocking callback path releases it deliberately: `_semphr_take` and
`_queue_recv` spill, call `blob_unlock()`, and wait — because a callback that
holds the pin while waiting for another task deadlocks against the task it is
waiting for. The release is not a bug; it is the only way the wait can complete.

**4.2 The spill** is intended to reduce a parking task to one live frame, which
would make the one-bit restore correct. The ring shows seven bits live at
crossings #5 through #8, so on this path either the spill did not run or the
frames were re-established before the switch. That is the next measurement, and
the ring is already tagged to correlate against `sbp-last` / `sbp-post`.

**4.3 The interrupt mask is advisory** (UM-NATOS-038 §13). The blob writes `PS`
directly, as IDF's PHY does, so a mask taken before entering vendor code cannot
be relied upon by anything whose correctness depends on it.

The failure lives precisely in the gap where all three stop applying at once.

---

## 5. The callback architecture: what exists, what does not

The concern was raised that WiFi will require callbacks and that the register
window makes them hard. The first half is right; the second is already solved.
The two are worth separating because they are different problems.

**5.1 Solved — the ABI crossing.** Callbacks from the blob into nat-os *are* the
OSI table: 118 function pointers the driver calls for mutexes, queues, timers,
memory and task creation. Four bridges in `kernel/window.S` carry the crossing:

```
w2c_call0f   w2c_call1   w2c_call2   w2c_call3
```

Callback bodies are compiled `-mabi=windowed` in `vendor/windowed/`; the kernel
is `-mabi=call0`; the boundary is enforced by file rather than by function.
Crossing without a bridge produces an `epc` with bit 31 set — a windowed return
encoding jumped to as an address. This project has hit that four separate times,
which is why the split is structural.

**5.2 In progress — blocking callbacks.** Sections 3 and 4. This is the current
work.

**5.3 Not started — callbacks from interrupt context.** `_set_intr` presently
clamps the requested priority to `CRIT_LEVEL` and counts the request; no WiFi ISR
has ever been installed. Two consequences:

- The MAC is interrupt-driven, so nothing will *receive* until it is.
- `blobcall.c`'s header states the constraint plainly: an interrupt handler
  cannot take a mutex. The exclusion protecting every blob entry today does not
  extend to ISR context, so this needs a new mechanism rather than a reuse of the
  existing one.

The clamp is itself load-bearing and must survive whatever replaces it: the
blob's `.iram` sections live in flash here, and nat-os holds `crit_enter()`
across a flash erase. An interrupt allocated above `CRIT_LEVEL` would fire
mid-erase and fetch from a chip that cannot answer.

**5.4 Not started — event callbacks.** `net80211_host.c` accepts event
registrations and never calls back. Scan results, association state and
disconnects all arrive that way in a working driver.

---

## 6. Distance to working WiFi

Stated plainly, because the current bug is the smallest of the remaining pieces.
Beyond 5.3 and 5.4: the adapter's timer entries are stubs, `_task_delay` returns
immediately rather than sleeping — which converts any driver wait loop into a
busy spin — and above the MAC there is no data path of any kind. A beacon on the
air is a reachable milestone; a network connection needs a stack that does not
exist yet.

What *is* established: the loader, the three-window split, the ABI bridges, PHY
initialisation on real hardware (`rc=0`), the OSI table's layout accepted by the
blob, and a transmit entry that executes and returns a real driver error.

---

## 7. Rules earned

**7.1 An instrument whose silence is indistinguishable from a result is worse
than no instrument.** Nine separate instruments in this investigation reported
something other than what they were trusted for. The families recur and are worth
naming:

- a constant read once and carried forward (`_phy_stack_top`, four steps)
- a counter that could not produce the value it was trusted to rule out
  (`wintorture`'s switch count, which made "windowed frames survive preemption"
  look measured for dozens of steps)
- a probe compared against a stale symbol, so its silence was a tautology
- a result carried in a summary table long after it stopped being true
  (`wifiinit 0x101`, still quoted in a commit whose own build panicked)
- an uninitialised scratch register satisfying its own filter, reporting a
  positive value that meant "nothing has happened yet" for twenty-five steps
- a singleton global on a hot path, which cannot attribute anything — written
  once, and then written again in the very step that documented the first
- a probe sampling at the right instant but the wrong *place*: `a0` is legal at
  the reload and illegal at the `retw` eight instructions later, and a latch at
  the former retracted a correct conclusion about the latter

**7.2 A ring beats a singleton.** The only instrument in this codebase that has
not lied is the tagged history ring — Tortoise's in the restore path, and the one
in section 3. Both identify the faulting event rather than assuming the last
value belongs to it.

**7.3 Reasoning about the call chain has now been wrong twice.** Both times the
answer came from naming a frame by address and disassembling it. Elimination
arguments over "which function must it be" have a poor record here.

**7.4 A green suite measures opportunity, not correctness.** Reaffirmed
independently: `wifiinit` was quoted as returning `ESP_ERR_NO_MEM` across four
commits and three documents while the same command panicked on the same build.
Nobody re-ran it, because the suites in between exercised a different path.

---

## 8. Next

1. Correlate the retw ring against `sbp-last` / `sbp-post` by sequence tag, to
   settle whether the spill ran on the `_queue_recv` path at all (4.2).
2. If it ran and the frames were re-established, the restore rule needs to grant
   what the task actually held rather than a fixed single bit — which requires
   recording the count at spill time, not inferring it at restore.
3. The instrumentation added across steps 42-80 spans four files and shares
   globals. It should come out deliberately, file by file with a build between
   each, once this failure closes. An earlier attempt to remove it as an
   end-of-session tidy-up cascaded into three build failures and was reverted.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 78-80.
Companion reports: UM-NATOS-038 (rev 1.5), UM-NATOS-039, UM-NATOS-040.

**Nothing has been on air.**

Written by: Hare
