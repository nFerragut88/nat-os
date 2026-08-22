# 2026-08-21 — wifiinit StoreProhibited: H1 experiment record

Branch `dev`. Companion to step 77.5 in docs/next_moves/08-wifi-via-loaded-blob.md.

## Hypothesis (H1, as implemented)

A level-3 tick lands while the interrupted context is inside a window
handler (`EPS3.EXCM` set). The handler saves context and schedules another
task out from under a half-finished spill; the frames mid-spill belong to no
task and are absent from `g_win_union`; the next rotation spills through stale
pointers, later surfacing as `_WindowOverflow12+9` storing through a garbage
a13 (~25) → StoreProhibited → double exception.

## Experiment

`_handler_level3` branches on saved EPS3 bit 4 before dispatching:
EXCM set ⇒ service sources (tick must re-arm CCOMPARE1) but skip
`task_schedule` this tick; count every hit in `g_tick_excm_hits`, record first
EPC3/EPS3. Counters visible in panic dump (`tick-excm :`) and `'intr'`.

One build iteration was needed: the defer flag originally rode in a5 across
`call0 intr_dispatch`, which clobbers caller-saved a5 — no tick switched, the
hang detector reset the board every ~3 s (TG0WDT boot loop). Fixed by branching
before the call.

## Expected result

- wifiinit survives with hits > 0 ⇒ H1 confirmed, fixed.
- wifiinit panics with hits > 0 ⇒ preemption real but not sufficient.
- wifiinit panics with hits = 0 ⇒ H1 refuted.

## Actual result

wifiinit panicked with the identical signature:

    exccause 29 (StoreProhibited), DEPC 0x40080109 (_WindowOverflow12+9),
    excvaddr 9, ws 0xe4c8, last osi entry 15 _semphr_take,
    saved frame on task 6's sp

and:

    tick-excm : never deferred          (zero hits)
    multiframe : 0 switch-outs with >1 live frame

blobphy independently reproduced the M6 signature (`tasks[5].sp`
0x3ffba820 → 0x3ffc0040) and returned rc=0 without crashing.

## Conclusion

**H1 ELIMINATED.** A deterministic failure cannot be caused by an event that
provably does not occur. No preemption of any windowed state happens before the
fault — neither mid-handler nor multi-frame switch-out. Remaining hypotheses:
H2 (DRAM overwrite of parked contexts / save areas; the M6 sp-change is
reproducible and harmless in blobphy, so the change itself is not the poison)
and H3 (bridge save-area discipline; weakened — upstream commit 154e5be already
wrote the caller's save area from the bridge and it changed nothing).

## Regressions (all pass, unchanged from main)

- wincollide: runs=156 wrong=0
- wintorture: checksum CORRECT (switches during call: 0, as always)
- blobphy: rc=0

## Next recommended experiment

Instrument the voluntary path instead of the preemptive one: at the contended
`_semphr_take` block (osi_s_semphr_take), snapshot WINDOWBASE/WINDOWSTART and
the [sp−12] save-area word immediately before `osi_impl_sem_take` and again
after wake, per excursion; diff them. One variable, direct evidence of WHO
changes window state around the only voluntary switch in the failing window.

---

## Session 2 — X4/X5/X6: the sweep mechanism, measured

### X4 — window state across the voluntary block (blk_sample in wifi_osi_stubs.c)

* **Changed**: `blk_sample()` at three points of every blocking excursion
  (pre-spill / post-spill / post-wake), surfaced as `blk-window:` in the panic
  dump.
* **Expected**: post-spill WS > 1 bit would mean win_spill_all cannot reach the
  one-frame steady state; wake-state anomalies would indict the restore path.
* **Actual**: pre 0x2aaa@wb13 (6 frames) -> post-spill ONE true frame (probe's
  own call8 frame accounts for the extra bit -- verified in disassembly) ->
  wake = BSS zero. epc1 again inside win_spill_all (win_spill_all+0xB).
* **Conclusion**: win_spill_all is CORRECT on the failing path; the excursion
  never returns from its block. The fatal overflow belongs to a LATER spill --
  and the only non-stub caller chain is spill_before_parking ->
  win_spill_call0.

### X5 — who sweeps, and what they see

* **Changed**: `sbp-last` recorder in spill_before_parking (task/wb/ws at every
  >1-bit sighting, last-one-wins); DEADBEEF sentinels on blk samples.
* **Expected**: if a task parks with foreign bits, its sweep is the killer.
* **Actual**: run A: `task 6 wb 1 ws 0xe4c0` -- display task (call0, own mask
  one bit @1) reading SIX bits none of which are its own. Run B: `task 5 wb 13
  ws 0x2800` at its park inside the block path. One run recovered base
  `0xeeeeeeee` = STACK_FILL: the sweep walked into memory that was never a
  save area. Victim varies by run; signature does not.
* **Conclusion**: H4 confirmed at mechanism level: a task sweeps WINDOWSTART
  bits belonging to nobody, rotates onto phantom frames whose physical sp
  slots hold stale/garbage values (~25), and OF12 faults storing through
  a13-16. This also explains multiframe=0 (each switch-out is recorded with
  one bit) and why only wifiinit dies (needs an excursion parked while other
  tasks keep parking/yielding).

### X6 — switch-in/switch-out recorders (vectors.S)

* **Changed**: raw WB/WS captured at every save (switch-out) and immediately
  before the wsr pair at restore (switch-in); printed as `switch-in` /
  `switch-out`. First build of the switch-in probe stored a2/a3 AFTER
  wsr.windowbase+rsync and measured post-rotation register garbage (code +
  stack addresses) -- capture must precede the write; kept as a comment.
* **Expected** (decisive split): dirty grant => leak upstream of the write;
  clean grant => bits materialise under a call0-only current task.
* **Actual**: `switch-in n398 wb 1 ws 0x00000002` -- CLEAN single-bit grant;
  paired `switch-out n398 wb 13 ws 0x2000`; seq unchanged at death (no
  further switches); yet sbp-last then reads SEVEN bits (0xe4e0) in the same
  task quantum. Panic-time hardware: 0xe4c8@wb7 (the dying sweep's rotations).
* **Conclusion**: the restore path is innocent. Phantom window state appears
  BETWEEN switches, while a call0-only task is current. No kernel writer
  exists there -- so something executes WINDOWED CODE without a context
  switch, or a writer outside the inventoried three sites exists. All ROM
  entries go through rom_call3/4 glue (grep-verified), phy_stack_call writes
  only single-bit words.

### Next experiment (X7)

Sample WB/WS into a small ring buffer from the tick handler (2 rsr + stores),
or record the same-task-resume branch (u=live) whenever it sees >1 bits for a
call0-only task. Either pins the exact interval in which the phantom bits are
written, and identifies the writer. Alternative confirmation-by-prevention
(clearly marked diagnostic, not a fix): clamp spill_before_parking to the
current task's own recorded mask instead of sweeping what the hardware shows;
survival of wifiinit + green regressions would prove causality end to end.
