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

Nine. NA-001 to NA-004 came from spot-checking; NA-005 and NA-006 from
`AUDIT scheduler`; NA-007 to NA-009 from `AUDIT PANIC`. All FIXED — the audit's own `AUDIT_ONLY` rule was
suspended deliberately, by the owner, after the first four were reported.

| ID | class | pri | where | state |
|---|---|---|---|---|
| NA-001 | LIKELY_BUG | P2 | `task.c` `task_sleep()` | fixed: clamp + `task_sleep_clamped()` |
| NA-002 | CONFIRMED_BUG | P0 (latent) | `heap.c` `align_up()` | fixed: guard + boot self-test |
| NA-003 | UNVERIFIED_ASSUMPTION | P3 | `vm.c` `VM_OP_SAR` | fixed: built from unsigned ops |
| NA-004 | UNVERIFIED_ASSUMPTION | P3 | `vmarg.c` `vmarg_items()` | fixed: guard + regression test |
| NA-005 | CONFIRMED_BUG | P2 (dormant) | `task.c` / `touch.c` lost wakeup | fixed: `task_sleep_armed()` + `waketest` |
| NA-006 | UNVERIFIED_ASSUMPTION | P1 (latent) | `task.h` `TASK_AGE_MAX` | fixed: `_Static_assert` |
| NA-007 | CONFIRMED_BUG | P1 | `panic.c` no re-entry guard | fixed: depth guard + `nestfault` |
| NA-008 | LIKELY_BUG | P2 (latent) | `panic.c` `halt_forever()` | fixed: masks interrupts |
| NA-009 | CONFIRMED_BUG | P3 | `kmain.c` fault report | fixed: reports what is known |

## AUDIT PANIC — run 2026-08-20, AUDIT_ONLY suspended by the owner

All six CHECK items covered. Two passed on inspection and were already reasoned
about in the source:

- **Flash operation from fault context.** `store_record_fault()` writes flash
  from the handler, including possibly mid-erase if that is what faulted.
  `flash_wait_ready()` polls WIP with a bounded timeout, so re-entry costs a
  wait rather than corruption.
- **Placement.** `panic.o`'s code *and* its `.rodata` are pinned to IRAM by
  `linker.ld`, so the handler does not need the cache to report that the cache
  is what died. Deliberate, and documented at the linker script.

### NA-007 — a fault inside the panic handler destroyed the evidence

`_handler_panic` sets `a1 = _panic_stack_top` unconditionally, so a second fault
re-enters cleanly rather than overflowing. It does something quieter and worse:
`store_record_fault()` runs again and **overwrites the record of the original
fault with the one the panic handler itself caused.**

That record is the only evidence that survives to the next boot on a board with
no serial cable attached. The effect is to replace the cause with its own
consequence, and report it confidently.

This is not a remote possibility. The handler does the two least reliable things
in the kernel, in order — write flash, then drive a display peripheral this
file's own comment calls *"a peripheral whose state nobody has verified"* — while
the system is by definition in an unknown state.

**Fixed** with a depth guard. Second entry does nothing that could fault a third
time: no flash, no display, no scheduler, one string and a spin.

**Verified on hardware** — `nestfault` panics and then faults on purpose inside
the handler, after the record is safely written:

```
  recorded : yes, the next boot will report this
  nest test: faulting on purpose inside the panic handler
*** PANIC DURING PANIC ***
  the FIRST fault's record is preserved and was not overwritten;
--- next boot ---
LAST FAULT   : kernel-detected failure, detail 24301      <- 0x5eed, the original
```

**The first run of this test proved nothing, and said PASS.** The block that
injects the fault had silently failed to be inserted, so no second fault ever
occurred and the record survived for the most boring possible reason. The next
boot still showed the expected value. Only checking the raw output — and noticing
that `PANIC DURING PANIC` never printed — separated "the guard worked" from "the
guard was never reached". The exerciser now prints an explicit
`THE STORE DID NOT FAULT - test is inconclusive` line if the injected store ever
stops faulting, so that failure cannot recur silently.

### NA-008 — the panic handler never masked interrupts

`halt_forever()` spins in `for (;;) {}` at whatever interrupt level it inherited.
Both callers today are safe **by accident, not by construction**: `kernel_panic()`
arrives through a vector that has already raised `PS.INTLEVEL`, and `task.c`'s
stack-guard call sits inside `task_schedule()`, which runs from the tick ISR.

But `kernel_panic_msg()` is a general-purpose entry point in a public header
whose contract documents a `.rodata` placement requirement and says nothing about
interrupt context. Called from an ordinary task, the old code would print
`halted`, **disarm the watchdog**, and then spin with the tick still firing — so
the scheduler would switch away and every other task would carry on running,
unrecoverably, behind a screen claiming the kernel had stopped.

**Fixed** by masking at `CRIT_LEVEL` in a shared prologue, making "does not
return" true for every caller rather than for the two that happen to exist.

### NA-009 — the surviving record named the wrong category

`kernel_panic_msg()` writes `STORE_FAULT_GUARD` for **every** kernel-detected
failure, but the enum comment and `kmain.c`'s decoder both describe that kind
specifically as a broken stack guard with `detail = task id`. So the "task table
full" panic in `must_create()` would be reported on the next boot as
`stack guard overwritten, task 0`.

The `why` string is not persisted, so the real reason cannot be recovered after
the reboot. Guessing the most common cause is what made this evidence worse than
silence. **Fixed** by reporting what is actually known — `kernel-detected
failure, detail N` — rather than restructuring the record, which the audit's
`DO_NOT_REFACTOR` rule puts out of scope.

### Left open

- **The `why` string does not survive a reboot.** Fixing it properly means room
  in the record for a reason code or a short string. Worth doing before anything
  ships without a serial cable attached, which is the whole premise of the
  device.

## AUDIT scheduler — run 2026-08-20, AUDIT_ONLY suspended by the owner

Covered: the four states and all six transition sites, wake/sleep races, tick
wraparound, priority ageing, starvation, voluntary yield, interrupt-context
scheduling, and the context frame in `vectors.S`.

**Clean, and verified rather than assumed:**

- **Tick wraparound.** Both `task_sleep()` and `wake_sleepers()` compare with
  `(int32_t)(now - deadline) >= 0`. Correct across the wrap.
- **The context frame.** `LBEG`/`LEND`/`LCOUNT` are saved *before* any C is
  called and `PS.EXCM` is cleared before the call — the M2 defect of
  UM-NATOS-009 §6.3. On restore `LCOUNT` is written **last**, because it is the
  register that arms the loop. `SAR` covered. This area was already right.
- **Starvation is bounded**, and the arithmetic was checked rather than trusted:
  a NORMAL task behind a permanently-ready HIGH one needs `waiting >= 60` ticks
  (600 ms — matching the header comment), a LOW task `>= 90`.

### NA-005 — a wake delivered to a running task was lost

The arm-then-sleep sequence in `touch_irq_wait()`:

```c
crit_enter(); register_waiter(); arm_the_pin(); crit_exit();
task_sleep(timeout);          /* <-- an edge landing HERE was lost */
```

An edge in that gap found the task still **RUNNING**. `task_wake()` acted only
on `BLOCKED`/`SLEEPING`, so it did nothing; and `touch_isr()` latches
`g_irq_flag` only when *no* waiter is registered, and one was. The sleep then
ran its full timeout with the event already gone.

The header comment at `touch_irq_wait()` claims this window is closed by
registering inside a critical section. **It closes the earlier half** — an edge
before the waiter is registered — and reads as though it closes both. It did
not close the half between `crit_exit()` and the sleep.

**Dormant, which is why it is worth fixing rather than noting.** `touch_irq_wait()`
has no caller; `kmain.c` says it is "one line from being reinstated" and the
board reports `irq=0`. So it is a trap set for whoever flips that line, and the
cost of fixing it while nothing depends on it is zero.

**Fixed** by splitting the clear from the sleep. `task_wake()` now records the
wake for a task in any live state; `task_sleep()` keeps its old behaviour by
clearing on entry (a plain delay must not be cut short by a stale flag), and the
new `task_sleep_armed()` does not clear, so a wake from the arm window survives:

```c
crit_enter(); task_arm_wake(); arm_the_pin(); crit_exit();
task_sleep_armed(timeout);    /* an edge landing HERE returns at once */
```

**Verified on hardware, with a negative control** — `waketest`:

```
armed  0 ticks   (the wake survived)
plain 47 ticks   (control: the wake was discarded, full sleep)
PASS
```

The control is the point. A test that only measured the armed case would pass if
`task_sleep_armed()` returned early for some unrelated reason. `plain` exceeding
its 30-tick request by 17 is the scheduler tail from `next_moves/04`, not an
anomaly.

### NA-006 — the ageing cap is exactly sized, and nothing said so

`TASK_AGE_MAX 3` bounds starvation only while

```
TASK_AGE_MAX > TASK_PRIO_LEVELS - 1
```

because selection is on strictly greater effective priority and a freshly-run
top-priority task scores `TASK_PRIO_LEVELS - 1`. With 3 levels and a cap of 3,
that holds **by exactly one**. Adding a fourth priority level would not slow
anything down or warn — it would make LOW tasks stop running entirely, showing
up as a hang somewhere unrelated.

Not a live bug; an unstated precondition on a value that looks like a tuning
knob. **Fixed** with a `_Static_assert`, verified in both directions: it builds
clean at 3 levels and fails with *"ageing cap too small to bound starvation
across the priority range"* at 4.

### Left open

- **`task_block()`/`task_sleep()` have no interrupt-context guard.** No ISR
  calls them today. Cheap to assert if one ever might.
- **Why the wait tail is 16-63 with `TASK_AGE_TICKS = 30`** — still open from
  `next_moves/04`, still a fairness question rather than a latency one.

## AUDIT VM — run 2026-08-19, AUDIT_ONLY suspended by the owner

**The invariant holds.** `no VM operation may access memory outside its
authorized arena` was checked at **every one of the 14 places `vm.c` touches
the arena**, not by sampling:

| site | guard |
|---|---|
| `load_u32/u8`, `store_u32/u8` | `vm_in_bounds(off, width)` + alignment |
| `copy_string` → `vmarg_string` | one bounds-checked byte at a time, copied not borrowed |
| `DEV_OP_NAME` | `max` clamped to `DEVICE_NAME_MAX`, then `vmarg_store` |
| `DEV_OP_XFER_IN/OUT` | `len` bounded before use, `DEVICE_XFER_MAX` bounce buffer |
| `SYS_BLIT` | `w`,`h` bounded **before** the multiply |
| `SYS_SEND` / `SYS_RECV` | `len`/`max` vs `IPC_MSG_MAX`, and `ipc_recv` honours `max` |
| instruction fetch | `vm_in_bounds(pc, 4)` + alignment |

Both bounds primitives — `vm_in_bounds()` and `arena_contains()` — compare in
the **offset domain**, so `off + len` never wraps. `arena_contains()` says why
in its own comment. `PUTS` walks bytes individually and is bounded by
`vm->size`, closing the classic unterminated-string leak. The device `caller` id
comes from `vm->app_id`, never from a register.

Arithmetic: `DIV`/`MOD` fault on zero, registers are unsigned so there is no
`INT32_MIN / -1` trap, and all three shifts mask to 5 bits.

**Two findings, both P3, both about claims rather than behaviour.**

**NA-003 — `VM_OP_SAR`.** Was `(int32_t)r[b] >> n`. Right-shifting a *negative*
signed value is implementation-defined in C. GCC picks arithmetic and always
has, so this was never wrong in practice — but the comment two lines above it
says a VM exists precisely to stop a program's behaviour depending on the host
compiler, and `SAR` was the one opcode that did. Rebuilt from unsigned
operations, with `n == 0` handled separately because `0xFFFFFFFF << 32` would
itself be the undefined shift being avoided.

**NA-004 — `vmarg_items()`.** The `count > max_items` check bounds the product
by `max_items * elem`, and nothing verified *that* fits in 32 bits. Every caller
today is safe (BLIT passes 76800 and 2), so it was an unstated precondition
rather than a live bug — but on a harness whose purpose is making services safe
by default, an unstated precondition is the wrong shape. Now checked against
`count`, so the fault lands on the program's argument. Regression test added:
`count under a huge ceiling that STILL wraps is refused`. `vmargtest` is 13/13.

**Not found:** any way for a program to reach memory outside its arena. The VM
is well built, and the audit's value here was in two claims that were slightly
wider than the code supported — not in a hole.

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
