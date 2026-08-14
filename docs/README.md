# cyd-os — Engineering Documentation

**Used Medias LLC — Embedded Systems Division**
Document set: `UM-CYDOS-001` … `UM-CYDOS-009`
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
| Milestone 2 — native task switching | **Complete, verified on hardware** — 1,200+ switches, zero corruption |
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
