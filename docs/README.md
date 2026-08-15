# cyd-os — Engineering Documentation

**Used Medias LLC — Embedded Systems Division**
Document set: `UM-CYDOS-001` … `UM-CYDOS-017`
Project: cyd-os — a from-scratch operating system for the ESP32 "Cheap Yellow Display"
Last revised: 2026-08-14

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
| [UM-CYDOS-012](UM-CYDOS-012-m4-verification.md) | Milestone 4 Verification Report | Register-based bytecode VM, the ISA, the assembler, and containment of malformed programs |
| [UM-CYDOS-013](UM-CYDOS-013-m5-verification.md) | Milestone 5 Verification Report | Application table and lifecycle, three-level scheduling, the shell, and the escape attempt |
| [UM-CYDOS-014](UM-CYDOS-014-locking.md) | Locking Primitives | Critical sections vs blocking mutex, task blocking and idle, and a measured starvation defect |
| [UM-CYDOS-015](UM-CYDOS-015-display.md) | Display Driver | ILI9341 over bit-banged SPI, span rendering with no framebuffer, and measured throughput |
| [UM-CYDOS-016](UM-CYDOS-016-display-syscalls.md) | Display Syscalls, and a Total System Freeze | Per-application viewports, pointer discipline, measured containment, and a yield that stopped the clock |
| [UM-CYDOS-017](UM-CYDOS-017-touch.md) | Touchscreen, and a Verification Method That Failed Three Times | XPT2046 over PENIRQ, the GPIO two-bank bug, calibration by controlled input, a capture that erased its own evidence, and input confinement |

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
| Task blocking | **Live** — `TASK_BLOCKED` plus an idle task using `WAITI`, UM-CYDOS-014 §3 |
| Display | **Working on hardware** — ILI9341, no framebuffer, 387 ms full-screen fill, colour order confirmed, UM-CYDOS-015 |
| Application graphics | **Live** — `FILL`/`TEXT`/`DIMS` confined to per-application viewports; 0 escapes across 136 audited fills, UM-CYDOS-016 §5 |
| Touch | **Working on hardware** — XPT2046 via PENIRQ, both axes calibrated by measurement, UM-CYDOS-017 |
| Application input | **Live** — `SYS TOUCH` confined to the asking application's viewport; 81 delivered, 109,211 withheld, 0 confinement failures, UM-CYDOS-017 §8 |
| DRAM budget | **Measured** — 158,000 B allocatable today (167,680 before `TASK_MAX` rose to 8); a full framebuffer is unnecessary, UM-CYDOS-010 §7.2 |
| Flash cache | **Enabled** — `.rodata` mapped from flash, UM-CYDOS-011 |
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
