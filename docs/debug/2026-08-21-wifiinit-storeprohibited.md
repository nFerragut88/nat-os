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
