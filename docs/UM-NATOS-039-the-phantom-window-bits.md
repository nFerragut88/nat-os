# UM-NATOS-039 — The Phantom Window Bits and the Writer in the Restore

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-21 · Status: **Init-time double exception root-caused, fixed and verified. One new downstream failure exposed and deliberately left unfixed pending its own investigation.**

---

## 1. Abstract

Since the vendor WiFi stack began running from flash (UM-NATOS-038),
`esp_wifi_init_internal` has died deterministically in a StoreProhibited double
exception. Seven experiment series across three sessions hunted the writer of
"phantom" WINDOWSTART bits: ownerless live-frame claims that appeared between
context switches while only call0 code ran, and that killed whichever task
next parked on top of them.

The writer has been identified with hardware-level proof, and it is the kernel's
own context switch. `_handler_level3`'s restore path computed the grant mask
into `a3`, rotated WINDOWBASE, and only then executed `wsr.windowstart a3` —
by which time the name `a3` no longer referred to the register holding the
mask. Whenever outgoing and incoming WINDOWBASE differed, WINDOWSTART received
the low sixteen bits of whatever stale value occupied the wrong physical slot.
Same-base grants worked only because old and new views name the same physical
register — which is also why every regression suite stayed green for months.

The fix is a two-instruction reorder. Every restore now commits exactly the
mask the scheduler computed; the double exception is gone; all three window
regressions pass.

Removing this bug exposed — rather than fixed — a second failure: the WiFi
driver's own task is the first genuinely multi-frame windowed program this
kernel has ever suspended, and the park machinery assumes the call0-shell
shape every nat-os task has had until now. That failure is recorded here,
reproduced byte-identically twice, and intentionally untouched.

---

## 2. The original issue

Calling `esp_wifi_init_internal` at `0x403014dc` from task 5's call0 shell
killed the system at tick ~398 of the run, every run:

```
*** KERNEL PANIC ***
  exccause : 28 (StoreProhibited)   -> double exception
  epc1     : 0x4008b59b             = win_spill_all + 0xB
  windowstart at death: 0xe4c8 @ wb 7, bit(base) SET   (family: e4c0/e4c8/e4d0/e4e0)
  excvaddr : occasionally tiny, e.g. 0x00000009
```

What was already known before this session, each item measured rather than
inferred:

* Task 5's excursion itself behaved correctly: it spilled to exactly one frame
  before parking, and its voluntary block never returned (the post-wake phase
  of the block sampler was never reached).
* The sweep machinery (M6) ran benignly on every death run — the fault was in
  *what* it swept, not in the sweep.
* The multi-frame counter read zero throughout: no task was ever caught
  switching out with several live frames by the bookkeeping's own lights.
* Switch-IN records were clean: every context grant handed the incoming task
  a correct single-bit state. The pollution therefore happened **between**
  switches, while only call0 code was current — an interval in which the
  kernel, as inventoried, executes nothing windowed.
* The fatal words varied within a family (`0xe4c0…0xe4e0`) whose members are
  all low halves of `0x3ffbxxxx` addresses. Nobody could explain why a
  stack-pointer-shaped value kept becoming a WINDOWSTART word.

Hypotheses eliminated along the way (all recorded in
`docs/debug/2026-08-21-wifiinit-storeprohibited.md`): deferred tick delivery
during handler execution (refuted by its own counter), glue-path WOE/crossing
defects, an unlogged second interrupt entry point (ELF scan found only
literal-pool false positives), and cross-task ownership consumption as a root
cause (superseded: real, but downstream).

---

## 3. The instrument

Session 3 added four probes, all retained:

| probe | what it captures |
|---|---|
| ring sampler | `{seq, WB, WS}` + current task id appended by every level-3 interrupt save path (64 entries) |
| restore readback | `rsr.windowstart` immediately after the restore's write pair, compared against the word written |
| junk-source capture | the value of `a3` at the instant `wsr.windowstart` consumes it |
| restore history | last 16 restores as `{written, committed}` pairs |

One instrumentation mistake was made and corrected, and is preserved here per
investigation rules: the first switch-IN recorder captured its registers after
`wsr.windowbase`+`rsync` and therefore measured post-rotation garbage (a
stack address reported as a window base). It was corrected to capture before
the write pair. The junk-source probe later exploited exactly this rotation —
deliberately — to expose what the remapped operand names.

---

## 4. The finding

Run 1, pre-fix image. Every sampled quantum clean, then:

```
switch-in : n 398 wb 1 ws 0x00000002 rbck 0x0000e8e0  MISMATCH
switch-out: n 398 wb 13 ws 0x00002000                 (task 5 leaving, clean)
sbp-last  : task 6 wb 1 ws 0x0000e8e0                 (same word, later)
```

The restore wrote `0x0002`. An immediate readback — same handler, interrupts
masked at level 3, zero intervening agents — saw `0xe8e0`. Between the last
clean sample and that readback the only writers that exist are the two `wsr`
instructions themselves. By elimination, the restore wrote the phantom word.

Run 2 added history, and produced the decisive table:

```
rst-hist:  391..397   junksrc 0x00000002 / verify 0x00000002   (healthy)
           398        junksrc 0x3ffbe9a8 / verify 0x0000e9a8   (fatal)
```

### Mechanism

Xtensa keeps sixty-four physical address registers. WINDOWBASE selects which
group of four answers to the names `a0..a3`; after `wsr.windowbase` plus
`rsync`, every register *name* resolves through the new base. The restore path
did:

```
a3 <- (1 << saved_WB) | union      ; computed under OUTGOING view (slot outWB*4+3)
wsr.windowbase saved_WB ; rsync    ; view rotates
wsr.windowstart a3                 ; operand now resolves to slot newWB*4+3
```

* Same-base grant (outWB == inWB == 1, every call0 shell): old-a3 and new-a3
  are the **same physical register**. The write is accidentally exact.
  History entries 391–397 show the identity, and nothing else.
* Cross-base grant (task 5 returning from its base-13 excursion to any base-1
  task): the mask sits in slot 55; the write reads slot 7, where stale
  register content lived. Entry 398: slot 7 held stack pointer `0x3ffbe9a8`.
  WINDOWSTART is architecturally sixteen bits wide, so hardware latched
  `0xe9a8`.

That single word retro-explains the entire case file:

* Phantom words across all sessions (`0xe4c0/e4c8/e4d0/e8e0/e9a8/9248…`) are
  low halves of `0x3ffbxxxx` values — truncated stale register content.
* Pollution required a prior excursion: task 5 was the only task whose base is
  not 1, so only its quanta created cross-base restores.
* Death followed the excursion but landed on an innocent bystander: whoever
  inherited the poisoned grant and next parked under it.
* Regressions never reproduced it: their tasks all share one base, so every
  restore took the coincidental-identity path. Green suites were measuring the
  bug's absence of opportunity, not its absence.
* The driver task's later sightings matched the poisoned word exactly
  (`sbp-last` read `0xe8e0` after a readback of `0xe8e0`): tasks inherit the
  corruption at grant and carry it through their whole quantum.

---

## 5. The solution

Two instructions, reordered in `kernel/vectors.S`:

```asm
wsr.windowstart a3          ; written under the OUTGOING view:
rsync                       ;   operand is the register that holds the mask
wsr.windowbase  a2
rsync
```

The transient inconsistency between the two writes spans two special-register
stores with interrupts masked at level 3 and no window operation between them;
no allocation-chain check can observe it. No ABI change, no architecture
change, no masking layer: the scheduler's contract ("a task is granted exactly
what it parked with") is now enforced by hardware-visible fact instead of by
register-naming coincidence.

Verification:

* wifiinit ×2, byte-identical logs apart from timing noise: **every** switch-in
  reports `commit ok`; history shows written == committed on every restore,
  including cross-base grants (`n463 wb 13 ws 0x2000 rbck 0x2000`).
* The StoreProhibited double exception no longer occurs.
* Regressions with the fix: `wintorture`, `wincollide`, `blobphy` — all
  `corrupt=0 fault=none`.

---

## 6. The following issue

With bug 1 gone, wifiinit proceeds further than it ever has and dies
deterministically, twice, with identical signatures:

```
exccause : 0 (IllegalInstruction)     epc 0x4008b8af (ROM)
panic-time windows: wb 13 ws 0x00002000 bit(base) SET   -- CLEAN single bit
last osi  : entry 29 _queue_recv      -- the driver's own task parks, never wakes
death at tick 463, in task 5's world, seven ticks after the park below
```

and the record that matters most:

```
sbp-last  : task 9 wb 3 ws 0x0000000a        -- TWO live frames at park
blk-window: pre 0xa00a@wb3 | post-spill 0x0a2b@wb3 | wake sentinel untouched
task 9    : stack 0x3ffb0bd4 + 7168 B        -- the WiFi driver's own task
```

Task 9 is the first genuinely windowed, genuinely multi-frame program this
kernel has ever suspended. Every nat-os task until now has been a call0 shell
that parks with exactly one frame at a known base — and every piece of park
machinery encodes that assumption.

*Revision (session 4):* the suspicion that the sweep fails on such tasks was
**disproven by measurement**. Outcome instrumentation at every multi-frame
park (`sbp-post`) shows `win_spill_call0()` reduces task 9's two live frames
to a clean single bit (`ws 0xa -> ws 0x8 @ wb 3`, "single-bit ok") — the
seven-bit residue seen earlier belongs to the adapter stub's own mid-chain
sampling point, not to the sweep's result. The sweep is nonetheless
load-bearing: clamping it out entirely (X8 diagnostic, since reverted)
converted the deterministic fault into a livelock in which ticks and
scheduling continued but wifiinit never progressed.

With the sweep verified good and every restore committing its exact mask,
the second failure therefore stands as follows: task 9 parks cleanly on
`_queue_recv` and is **never woken** (the post-wake sampler sentinel is
untouched in every run); no osi call happens afterwards; and some seconds
into the wait, task 5 — executing ROM init code with clean window state and
a committed grant — faults with IllegalInstruction at a **timing-variable
PC inside a small ROM cluster** (observed: `0x4008b8af` at tick 463,
`0x4008b977` at tick 367, `0x4008b8e4` present in the saved frame's a7;
adding nothing but heartbeat output moved both the tick and the PC).

This failure could not exist before: bug 1 always killed the system at tick
~398, long before queue-receive territory. It is a distinct defect with its
own causal chain, and per investigation discipline it gets its own
hypothesis loop rather than a speculative patch. Current hypotheses, neither
tested:

* **H-C** — the wake task 9 awaits is produced by a path nat-os does not
  serve yet (notably a WiFi interrupt route), so t9 legitimately never
  wakes; t5's ROM-side wait eventually wanders through stale register state.
* **H-D** — t5's own return chain is corrupted during the wait by a writer
  outside the inventoried paths; the wandering PC is the symptom, and the
  wait-loop is innocent.

Nothing has been changed for it. The full evidence set is in
`docs/debug/2026-08-21-wifiinit-storeprohibited.md`, sessions 3–4.

---

## 7. Transferable conclusions

1. **A green regression suite constrains the future less than it seems.** All
   three suites passed for months against a kernel that corrupted window state
   on every cross-base switch, because none of them ever performed one. A test
   plan should enumerate the ABI transitions a mechanism can see, not merely
   repeat the ones the current workload happens to make.
2. **Special-register writes through GPR operands do not survive a view
   rotation.** Any `wsr.*` whose operand was prepared before
   `wsr.windowbase` must either execute before the rotation or be reloaded
   after it. This is now documented at both call sites in `vectors.S`.
3. **Readbacks close elimination loops cheaply.** Four small probes turned a
   three-session elimination search into a two-run proof, because they
   measured the write at the only place ambiguity could hide: immediately
   after it.

Written by: Tortoise