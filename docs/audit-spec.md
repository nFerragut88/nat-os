# NAT-AUDIT — an audit specification for this kernel

**Written by the project owner, 2026-08-19.** Kept verbatim below.

It is this project's scar tissue written as a specification. `DO_NOT_INFER_
HARDWARE_BEHAVIOR` is "a successful register write is not evidence";
`DO_NOT_OPTIMIZE_WITHOUT_MEASUREMENT` is what `next_moves/04` demonstrated the
same day; and `NEVER: label uncertainty as confirmed` is UM-NATOS-034 §25.5,
where exactly that happened twice.

Its `FLASH_POLICY` section also describes, in three layers, the design that
landed in `store.h` hours before it was written.

## Findings so far

Two, both from spot-checking rather than a full pass. Both now FIXED — the
audit's own `AUDIT_ONLY` rule was suspended deliberately, by the owner, after
they were reported.

| ID | class | pri | where | state |
|---|---|---|---|---|
| NA-001 | LIKELY_BUG | P2 | `task.c` `task_sleep()` | fixed: clamp + `task_sleep_clamped()` |
| NA-002 | CONFIRMED_BUG | P0 (latent) | `heap.c` `align_up()` | fixed: guard + boot self-test |

**NA-001.** `deadline = timer_ticks() + ticks` wraps. Every deadline comparison
in this kernel is the wrap-safe `(int32_t)(now - deadline) >= 0`, which is
correct and spans half the range — so `sleep(0xFFFFFFFF)` produced a deadline
one tick in the *past* and returned without sleeping at all. Clamped to
`TASK_SLEEP_MAX` (248 days at 100 Hz) and counted.

**NA-002.** `align_up(v)` is `(v + 7) & ~7` and wraps: `align_up(0xFFFFFFFF)` is
0. `heap_alloc` then matched the first free block — `b->size < 0` is never true
unsigned — and returned a **valid pointer** to a few bytes for a caller that
believed it held 4 GB. The invariant broke by *succeeding*, which is the
quietest way available.

Not reachable today: every caller passes a compile-time constant. It becomes
reachable the moment a size is computed at run time — a bundle length arriving
over the radio, which is precisely `next_moves/10`.

## What has NOT been audited

Everything else. `AUDIT VM`'s per-opcode arithmetic, the scheduler's
state-transition races, `AUDIT PANIC`, and most of `TEST_REQUIREMENTS`. Two
findings came from perhaps fifteen minutes on three items; the rest of the spec
is unrun.

`power_loss_during_commit` was checked and **passes**: `store_load()`
distinguishes an erased sector (magic/version) from a torn write (checksum) and
falls to defaults on either. Worth one caveat for Phase 1 — losing the record
cleanly is still losing it, and one sector with no redundancy means a torn write
costs everything since the last save.

---

```
NAT-AUDIT/1.0

TARGET:
  project = NatOS
  platform = ESP32 (original Xtensa)
  scope = kernel + scheduler + VM + memory + flash + ABI
  mode = AUDIT_ONLY

RULES:
  - DO_NOT_REFACTOR
  - DO_NOT_CHANGE_BEHAVIOR
  - DO_NOT_OPTIMIZE_WITHOUT_MEASUREMENT
  - DO_NOT_INFER_HARDWARE_BEHAVIOR
  - VERIFY_AGAINST_SOURCE_AND_DISASSEMBLY
  - REPORT_BEFORE_MODIFYING

CLASSIFICATION:
  CONFIRMED_BUG
  LIKELY_BUG
  UNVERIFIED_ASSUMPTION
  ACCEPTABLE_TRADEOFF

PRIORITY:
  P0 = corruption / crash / isolation violation
  P1 = incorrect kernel behavior
  P2 = timing / starvation / durability problem
  P3 = maintainability / architecture concern

AUDIT memory:
  CHECK:
    - allocation-size overflow
    - alignment overflow
    - invalid free
    - double free
    - interior-pointer free
    - corrupted block metadata
    - coalescing correctness
    - heap boundary checks

  REQUIRE:
    alloc(n) => valid aligned region OR failure
    free(p) => reject invalid/non-base pointers
    arithmetic => overflow-safe before dereference

AUDIT scheduler:
  CHECK:
    - READY/BLOCKED/SLEEPING transitions
    - wake/sleep race conditions
    - tick wraparound
    - priority aging
    - starvation
    - voluntary yield behavior
    - interrupt-context scheduling
    - context save/restore

  VERIFY:
    - scheduler invariants from source
    - generated Xtensa assembly
    - interrupt PS.EXCM handling
    - LOOP-register interaction
    - context frame layout

AUDIT VM:
  CHECK_EVERY_OPCODE:
    - offset arithmetic
    - length arithmetic
    - count arithmetic
    - index arithmetic
    - multiplication overflow
    - pointer validation
    - arena bounds
    - source/destination overlap assumptions

  INVARIANT:
    no VM operation may access memory outside its authorized arena

AUDIT FLASH:
  CHECK:
    - all erase callers
    - all write callers
    - all read callers
    - interrupt-disabled duration
    - cache/flash interaction
    - blocking operations inside critical sections
    - persistence durability semantics

  KNOWN:
    platform = original ESP32
    erase_suspend = unavailable
    erase_latency ~= 125ms

  DESIGN:
    render_loop MUST_NOT directly initiate flash erase
    storage_state MAY become DIRTY
    higher_layer MUST decide WHEN erase/write is permitted
    persistence MUST remain durable within existing semantic requirements
    multiple pending writes SHOULD be batchable

FLASH_POLICY:
  APPLICATION:
    mutate_state()
    -> mark_dirty()

  TIMING_LAYER:
    if safe_storage_window():
      commit_storage()
    else:
      defer()

  STORAGE_LAYER:
    commit_storage()
    -> erase()
    -> write()

AUDIT PANIC:
  CHECK:
    - fault persistence
    - flash operation from fault context
    - recursion
    - unavailable scheduler
    - unavailable interrupts
    - corruption during panic handling

TEST_REQUIREMENTS:
  heap:
    - free(NULL)
    - double_free
    - interior_pointer
    - boundary_pointer
    - maximum_size
    - overflow_size

  scheduler:
    - sleep(0)
    - sleep(1)
    - sleep(UINT32_MAX)
    - wake_before_timeout
    - wake_after_timeout
    - repeated_wake
    - tick_wrap

  VM:
    - zero_length
    - maximum_length
    - overflow_length
    - boundary_access
    - one_byte_outside_boundary

  flash:
    - save_during_receive_window
    - save_outside_receive_window
    - multiple_dirty_events
    - power_loss_during_commit

OUTPUT:
  For each finding:
    ID
    CLASSIFICATION
    PRIORITY
    FILE
    FUNCTION
    LINE
    INVARIANT
    OBSERVED_BEHAVIOR
    EXPECTED_BEHAVIOR
    REPRODUCER
    CONFIDENCE
    RECOMMENDED_FIX

  NEVER:
    modify code before reporting findings
    label uncertainty as confirmed
    invent missing architecture
    recommend broad rewrites when a local fix exists
```
