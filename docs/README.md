# cyd-os — Engineering Documentation

**Used Medias LLC — Embedded Systems Division**
Document set: `UM-CYDOS-001` … `UM-CYDOS-019`
Project: cyd-os — a from-scratch operating system for the ESP32 "Cheap Yellow Display"
Last revised: 2026-08-15

---

## Purpose of this set

These reports record the design of cyd-os in enough detail that the project can
be picked up cold — by someone else, or by the author after six months away —
without re-deriving decisions from the source.

Each report states **what was decided, what it was measured against, and what
remains unverified**. Claims that were tested on hardware are marked as such.
Claims taken from documentation or reasoning are marked separately, because on
a from-scratch kernel the difference between "this is true" and "this should be
true" is the difference between a working boot and a silent reboot.

## Index

| ID | Title | Covers |
|---|---|---|
| [UM-CYDOS-001](UM-CYDOS-001-architecture.md) | System Architecture and Scope | Layer model, what is built vs borrowed, the isolation problem, why a bytecode VM |
| [UM-CYDOS-002](UM-CYDOS-002-boot-chain.md) | Boot Chain and Image Format | ROM → 2nd stage → kernel, image header, measured boot trace |
| [UM-CYDOS-003](UM-CYDOS-003-abi.md) | Xtensa ABI Selection | Windowed vs call0, codegen evidence, consequences for the scheduler |
| [UM-CYDOS-004](UM-CYDOS-004-memory-map.md) | Memory Map and Allocation Policy | ESP32 regions, our layout, the bootloader overlap risk |
| [UM-CYDOS-005](UM-CYDOS-005-build-pipeline.md) | Build and Flash Pipeline | Toolchain, flags and why each one, reproducing a build, why not PlatformIO |
| [UM-CYDOS-006](UM-CYDOS-006-m0-verification.md) | Milestone 0 Verification Report | Test method, captured output, pass/fail per assertion |
| [UM-CYDOS-007](UM-CYDOS-007-roadmap.md) | Development Roadmap M1–M5 | Each milestone, its risks, and its exit criteria |
| [UM-CYDOS-008](UM-CYDOS-008-m1-verification.md) | Milestone 1 Verification Report | Vectors, timer source and level choice, interrupt entry/exit, measured results |
| [UM-CYDOS-009](UM-CYDOS-009-m2-verification.md) | Milestone 2 Verification Report | Task model, context frame, scheduler, the zero-overhead `LOOP` defect, watchdog correction |
| [UM-CYDOS-010](UM-CYDOS-010-m3-verification.md) | Milestone 3 Verification Report | Heap allocator, arena model and bounds checking, and the measured DRAM budget |
| [UM-CYDOS-011](UM-CYDOS-011-flash-cache.md) | Flash Cache and Read-Only Data Placement | Mapping `.rodata` into flash, the 0x20 page congruence, and the cache-off hazard |
| [UM-CYDOS-012](UM-CYDOS-012-m4-verification.md) | Milestone 4 Verification Report | Register-based bytecode VM, the ISA, the assembler, containment of malformed programs, and the syscalls added since |
| [UM-CYDOS-013](UM-CYDOS-013-m5-verification.md) | Milestone 5 Verification Report | Application table and lifecycle, three-level scheduling, the shell, the escape attempt, and messaging without shared memory |
| [UM-CYDOS-014](UM-CYDOS-014-locking.md) | Locking Primitives | Critical sections vs blocking mutex, task blocking and idle, a measured starvation defect, and why contention cost is the number of blocking events rather than the time held |
| [UM-CYDOS-015](UM-CYDOS-015-display.md) | Display Driver | ILI9341, span rendering with no framebuffer, and the path from bit-banged SPI to SPI2 with DMA |
| [UM-CYDOS-016](UM-CYDOS-016-display-syscalls.md) | Display Syscalls, and a Total System Freeze | Per-application viewports, pointer discipline, measured containment, and a yield that stopped the clock |
| [UM-CYDOS-017](UM-CYDOS-017-touch.md) | Touchscreen, and a Verification Method That Failed Three Times | XPT2046 over PENIRQ, the GPIO two-bank bug, calibration by controlled input, a capture that erased its own evidence, input confinement, and an axis that was inverted for three months because the direction test read its own worst sample |
| [UM-CYDOS-018](UM-CYDOS-018-persistence.md) | Persistence, and a Read Defect That Looked Like the Wrong Thing | SPI flash driver, a checksummed record that survives a power cycle, the inherited clock divider that shifted every read by a bit, and two hypotheses tested against stale firmware |
| [UM-CYDOS-019](UM-CYDOS-019-failure-handling.md) | Failure Handling, and Three Mechanisms That Had Never Fired | Stack guards enforced in the scheduler, the watchdog erasing its own panic reports, measured stack margins, a UART receive path one byte behind, and a fault reported to flash and to the panel |

## Reading order

New to the project: **001 → 002 → 004 → 003**. That gives the shape of the
system, how it starts, where things live, and why the calling convention is
unusual.

Picking up implementation work: **006** (what is known-good) then **007** (what
is next and what it will break).

Reproducing a build: **005** alone is sufficient.

## Status summary as of this revision

| Item | State |
|---|---|
| Milestone 0 — kernel boots, self-checks pass | **Complete, verified on hardware** |
| Milestone 1 — timer interrupt, tick counter | **Complete, verified on hardware** |
| Milestone 2 — native task switching | **Complete, verified on hardware** — 3,400+ switches, zero corruption |
| Milestone 3 — heap and arena model | **Complete, verified on hardware** — all three exit criteria |
| Milestone 4 — bytecode interpreter | **Complete, verified on hardware** — program runs, six fault classes contained |
| Isolation | **Live** — every VM memory access bounds-checked, UM-CYDOS-012 §6.2 |
| Full stack | **Running** — bytecode hosted in a preemptible native task, 3.3M instructions across 450 preemptions with zero accounting drift, UM-CYDOS-012 §6.6 |
| Milestone 5 — multiple applications | **Complete, verified on hardware** — all three exit criteria |
| Roadmap M0–M5 | **Complete.** Six native tasks, three scheduling levels, a shell, and an application deliberately written to escape its arena that could not |
| Locking | **Complete** — critical sections and a blocking mutex; heap and console both arbitrated, UM-CYDOS-014 |
| Task blocking | **Live** — `TASK_BLOCKED`, `TASK_SLEEPING` and an idle task using `WAITI`, UM-CYDOS-009 §11 |
| Scheduling | **Priorities** — three levels, strict, with `task_sleep()` and priority inheritance; renderer 2 fps → 8.5 fps, UM-CYDOS-009 §11 |
| 3D renderer | **Running** — grid raycaster, no framebuffer (measured not to help), UM-CYDOS-015 §5.7 |
| Display | **Working on hardware** — ILI9341, no framebuffer, **43 ms** full-screen fill via SPI2 + DMA, UM-CYDOS-015 §5.5 |
| Application graphics | **Live** — `FILL`/`TEXT`/`DIMS` confined to per-application viewports; 0 escapes across 136 audited fills, UM-CYDOS-016 §5 |
| Touch | **Working on hardware** — XPT2046 gated on PENIRQ **and** pressure; X axis was inverted for three months and is now calibrated from four labelled corners, UM-CYDOS-017 §4.1, §7.1 |
| Application input | **Live** — `SYS TOUCH` confined to the asking application's viewport; 81 delivered, 109,211 withheld, 0 confinement failures, UM-CYDOS-017 §8 |
| Application messaging | **Live** — copied through a kernel mailbox, never shared memory; 278 sent / 277 delivered / 0 bad buffers, UM-CYDOS-013 §8 |
| Application bitmaps | **Live** — `SYS BLIT`, arena-bounded source and viewport-clipped destination, UM-CYDOS-012 §10 |
| DRAM budget | **Measured** — 158,000 B allocatable today (167,680 before `TASK_MAX` rose to 8); a full framebuffer is unnecessary, UM-CYDOS-010 §7.2 |
| Flash cache | **Enabled** — `.rodata` mapped from flash, UM-CYDOS-011 |
| Persistence | **Live** — checksummed record in a flash sector at 2 MB; boot counter and a cumulative frame count survived 16 resets, UM-CYDOS-018 §6 |
| Failure handling | **Enforced** — guards checked on every switch across all eight tasks; `hang`/`fault`/`smash` each trigger their path on demand, UM-CYDOS-019 §5 |
| Fault reporting | **Three ways** — flash record read back by the next boot, UART report, and the reason drawn on the panel; ordered by decreasing reliability, UM-CYDOS-019 §6–7 |
| Stack margins | **Measured** — worst task uses 444 B of 2,048; minimum margin 78%, UM-CYDOS-019 §3.1 |
| Version control | **Initialised 2026-08-14** |
| JTAG debug probe | Ordered, not in hand |
| Bootloader IRAM overlap | **Closed** — investigated and disproved, UM-CYDOS-004 §5 |
| Panic handler | **Closed** — exercised deliberately with an `ill` instruction; prints EXCCAUSE/EPC and halts |
| Watchdogs | **Closed** — measured armed, now disabled and read back, UM-CYDOS-009 §8 |

> **Standing rule for interrupt handlers — clear `PS.EXCM` before calling C.**
> Hardware sets `EXCM` on interrupt entry, and while it is set the Xtensa
> zero-overhead loop-back is disabled: a hardware loop body executes once and
> falls through to whatever sits at `LEND`. GCC emits these loops for ordinary
> counted C loops, so this silently degrades **every C function reachable from a
> handler** — drivers and the VM interpreter included, not just the scheduler
> where it was found. `_handler_level3` now clears it; any future handler must
> do the same. UM-CYDOS-009 §6.

> **Standing rule for the scheduler — a yield must never defer the clock it
> depends on.** `task_yield()` originally wrote `CCOMPARE1 = ccount + 64`
> unconditionally, so any loop yielding faster than 64 cycles pushed the
> deadline ahead of `CCOUNT` forever, the timer interrupt stopped firing, and
> the whole kernel froze — the tick is what drives every context switch. Any
> routine adjusting a scheduler deadline must only ever move it **earlier**.
> UM-CYDOS-016 §3.

> **Standing rule for verification — when an instrument reports its own reading
> invalid, that outranks every value printed beside it.** Three times across two
> sessions a frozen marker, a sample counter stuck at an identical value, and a
> boot banner in a serial capture each said "this measurement is not what you
> think", and the plausible-looking numbers next to them were believed anyway.
> Two conclusions were published and later retracted as a result. Latch the
> quantity so timing cannot lie about it, then feed the system a controlled
> input rather than interpreting an uncontrolled one. UM-CYDOS-017 §6.

> **Standing rule for debugging — a negative result is only informative if the
> experiment demonstrably ran.** Two flash hypotheses were recorded as tested
> against a board that had never been reflashed: `build.ps1` builds, but
> `build.ps1 -Flash` is what flashes, and the invocation used named a script that
> does not exist. Every hypothesis returned bit-identical output, which was read
> as "none of these are the cause" when it meant "no experiment has run yet".
> The signature to watch for is **a run of results that do not vary when the
> input does** — suspect the harness before the theory, and verify the change
> reached the target rather than inferring it from the absence of an error.
> UM-CYDOS-018 §5.

> **Standing rule for verification — a startup artefact is not evidence the
> thing it introduces works.** The shell was signed off because its banner
> printed; the banner proves a task was created and the TRANSMIT path works, and
> says nothing about receive. The receive path was one byte behind for the
> shell's entire existence, so pressing Enter did nothing until the next
> keystroke — and every automated test drove it with CR **and** LF, the one
> input shape that hides it. Stack guards and the panic handler went unexercised
> for the same reason: each was confirmed to EXIST rather than observed to WORK.
> Trigger the mechanism on purpose, or treat it as untested. UM-CYDOS-019 §4.1.

> **Standing rule for calibration — never infer direction from the endpoint of a
> gesture.** The first and last samples of a drag are its two least trustworthy,
> because both sit at a contact transition where the panel is not bridged and the
> ADC reads its rail. A rail reading is near the top of the range, so a drag
> ending anywhere "ends near its maximum" and EVERY axis appears to increase —
> the test can only return one answer. The touch X axis was backwards for three
> months behind exactly that. Calibrate from labelled points, each a known
> position paired with a reading, and let direction fall out of comparing labels.
> UM-CYDOS-017 §7.1.

> **Standing rule for lock contention — the cost is the NUMBER of blocking
> events, not the time the lock is held.** Each one costs a scheduling
> round-trip whether the lock was held for 24 ms or 24 µs: the blocked task is
> descheduled and must be selected again. Measured here at 63 ms per contention
> against a 24 ms hold, with the lock free 88% of the time while two tasks sat
> blocked on it. Narrowing the hold by 25% changed the outcome by nothing. The
> levers are batching (fewer takes) or not blocking at all (`try_lock`) —
> shortening holds, the intuitive move, does nothing. UM-CYDOS-014 §10.5.
