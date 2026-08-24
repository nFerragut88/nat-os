# Conventions, and a Note on Evidence

## Typographic conventions

Source is quoted verbatim from the tree, with its comments intact. The comments
in nat-os carry a substantial part of the design rationale — in several files
the comment explaining why a line exists is longer than the file's code — so
stripping them for brevity would remove most of what makes the quotation worth
reading.

Where a quotation is abridged, the elision is marked:

```c
    /* ... */
```

Register addresses are given in the form the source uses. Hex constants are
lower case in code and upper case in prose, matching the reports.

Report citations use the reports' own numbering: **UM-NATOS-017 §7.1** is
section 7.1 of `docs/UM-NATOS-017-touch.md`. The renaming from `cyd-os` to
`nat-os` did not change document or section numbers, so `UM-CYDOS-014 §5.2` in
an old commit message is `UM-NATOS-014 §5.2` here.

## The evidence grading used throughout

The reports draw a hard line between three kinds of claim, and this book keeps
it. Every quantitative statement in this book falls into one of these:

| Grade | Meaning | Example |
|---|---|---|
| **Measured** | Observed on the target board, with the raw output recorded | "43 ms full-screen fill via SPI2 + DMA" |
| **Derived** | Computed from measured quantities, with the arithmetic shown | "~109 cycles per bytecode instruction" — from a quarter-share assumption, so a ceiling |
| **Transcribed** | Taken from documentation or a vendor header, not independently confirmed | The SRAM region boundaries in Chapter 4 |

The distinction matters more than it usually does. UM-NATOS-004 §2 transcribes
the ESP32's SRAM boundaries from the Technical Reference Manual and marks them
as unverified, while marking the addresses the kernel actually uses as confirmed
on hardware. UM-NATOS-024 §3 is the story of a constant transcribed *from
memory* rather than from the vendor header — `SENS_SAR1_EN_PAD_FORCE` written as
bit 27 instead of bit 31 — which produced a driver whose every register read back
exactly as written and which was converting nothing.

Where this book states a number, it says which grade it is if there is any
ambiguity.

## What "verified on hardware" means here, and what it does not

UM-NATOS-019 is entirely about the gap between a mechanism *existing* and a
mechanism *working*. Three separate safety mechanisms — stack guards, the panic
handler, and the hang detector — had each been confirmed to exist and none had
been observed to fire. The rule that came out of it:

> **A startup artefact is not evidence the thing it introduces works.** The
> shell was signed off because its banner printed; the banner proves a task was
> created and the TRANSMIT path works, and says nothing about receive. The
> receive path was one byte behind for the shell's entire existence.

So in this book, "verified on hardware" means one of:

- A counter was read back with a value that could only be produced by the
  mechanism working (`escapes=0` across 136 fills; `corrupt=0` across 3,418
  ticks).
- The mechanism was **triggered deliberately** and its output captured (`fault`,
  `smash` and `hang` exist in the shell for exactly this).
- A quantity was measured against an independent clock (the TSF timer's 1 MHz
  rate, confirmed against CCOUNT over half a second, two clocks with no
  connection to each other).

It does not mean "the screen looked right", except where the book says so
explicitly — and Chapter 18 records three occasions where a number improved, the
change was reported, and the panel had not been looked at.

## The counter idiom

nat-os instruments nearly everything, and the instrumentation follows a pattern
worth naming because it appears in every chapter.

A claim is turned into a **pair** of counters where possible: one that must be
non-zero and one that must be zero. Alone, either is uninformative.

```
touch g/w = 81/109211        last = ...->191,174
```

81 touches delivered to an application and 109,211 withheld for landing outside
its viewport. Delivery alone would not show confinement; withholding alone would
be indistinguishable from a broken syscall. Both together are the claim
(UM-NATOS-017 §8.2).

```
vp calls=136  escapes=0  maxy=250/320
```

136 rectangles reached the panel; none escaped its viewport; the lowest row any
application painted is 250, which is exactly slot 2's viewport bottom edge
(`168 + 2×28 + 26`). "Not one row below it, and nowhere near the panel's 320."

The same idea applies to policies that discard work. `display_try_lock()` lets a
drawing primitive be skipped when the panel is busy, and the skip is counted:

```c
    if (!display_try_lock()) {
        g_draw_skipped++;
        return;
    }
```

with the reasoning recorded beside it in `vm.c`:

```c
/* Drawing primitives dropped because the panel was busy. Counted, not hidden: a
 * best-effort policy that silently discards work is indistinguishable from a
 * broken one, and the ratio of skipped to drawn is the only thing that says
 * whether the policy is reasonable or is starving applications of the screen. */
static uint32_t g_draw_skipped;
```

Measured at 45 of 1,325 primitives, 3.4%.

## Units and clocks

Three clocks appear in this book and they measure different things. Confusing
them has produced at least four wrong conclusions in this project's history, so:

| Clock | What it counts | Where it lies |
|---|---|---|
| `xt_ccount()` | CPU cycles, free-running | **Wall clock.** Keeps running while the reading task is descheduled. A full-screen fill measures 43 ms single-threaded and 249–362 ms from a task — the same bytes to the same panel. |
| `task_cpu_cycles()` | Cycles the calling task has actually run | Only advances at context switches, so it cannot bound a spin *inside* one slice. |
| `timer_ticks()` | Scheduler ticks, 10 ms each | Counted ISR entries rather than elapsed time until UM-NATOS-028 §4. Measured at 217 ticks per real second where 100 is correct. |

The CPU runs at 80 MHz. This was **derived**, not specified: UM-NATOS-008 §5.2
counted 25 ticks of 2,400,000 cycles within a ~10 s capture window and inferred
a clock near 80 MHz. At the time the bootloader left the core at that setting and
nothing in the kernel raised it, so the figure was a measurement of the current
configuration rather than a specification, and the text here said it should be
re-derived if boot configuration ever changed.

> **Since written — boot configuration changed, and the warning was worth its
> place.** A replacement second-stage bootloader (UM-NATOS-035) did not touch the
> clock, so the board ran at 40 MHz for a session and **no instrument said so**:
> every duration in this kernel comes from `CCOUNT`, which counts cycles, so
> halving the clock prints the same number for twice the wall-clock time.
> `kernel/clock.c` now switches the SoC to the 320 MHz PLL and divides it to
> 80 MHz, reports the result at boot as `cpu clock : 80 MHz`, and says which of
> the two loaders set it. The 80 MHz in every measurement in this book is
> therefore now *set and reported* rather than inferred — but the measurements
> themselves predate that, and were taken on a machine that happened to be
> configured the same way. UM-NATOS-036 §11 re-verified the rate against an
> independent clock.
>
> Add this to the table above, because it is the sharpest instance of it in the
> project: **a clock that is wrong by a ratio cannot be caught by any instrument
> derived from that clock.** Ch. 28 §28.5 is the general shape.

The tick is 10 ms (`TICK_INTERVAL_CYCLES` = 800,000 cycles at 80 MHz).

## A word on the tone of the source comments

The comments quoted throughout are unusual for kernel source in that they
frequently argue with themselves, record retracted conclusions, and name what a
piece of code *cannot* do. That is deliberate and it is the same discipline as
the reports. From `touch.c`:

```c
 * ---- why the original calibration passed ----------------------------------
 *
 * This axis has been backwards since the touch driver was written, and the
 * calibration in UM-NATOS-017 §7 did not catch it — but NOT because it ignored
 * direction. It tested direction explicitly, and concluded the opposite:
 *
 *     "the horizontal drag ended at rx=3527 against a maximum of 3536,
 *      so raw X increases left to right"
 *
 * The flaw is which sample it trusted.
```

A comment that records the wrong answer alongside the right one is a comment
that stops the wrong answer being rediscovered. Several of them in this tree have
already done that job.
