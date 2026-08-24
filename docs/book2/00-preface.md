# Preface

## Why this book exists

There are two kinds of documentation an operating system produces. The first
describes what the system does: the API, the data structures, the state
machines. The second describes what the system's authors *believed* at each
point, what measurement said instead, and what changed as a result.

nat-os has an unusual amount of the second kind. Twenty-eight engineering
reports exist under `docs/`, and they follow two rules that are worth stating up
front because they shape everything in this book:

> **Measured is separated from assumed.** On a from-scratch kernel the
> difference between "this is true" and "this should be true" is the difference
> between a working boot and a silent reset, so claims verified on hardware are
> marked as such and claims taken from documentation are marked separately.

> **Every report ends with what it does *not* establish.** Those sections are
> the most useful part. They are where the known gaps live, and several of them
> correctly predicted the next defect.

That second rule turns out to be load-bearing. UM-NATOS-011 §4 predicted, in
writing, that the next thing to break would be a flash write colliding with the
data cache. It broke exactly that way, on the first version of the driver, and
the report that records it (UM-NATOS-018 §3) says so. UM-NATOS-010 §8 recorded
that `arena_contains()` existed and nothing called it; the isolation guarantee
arrived two milestones later and the report that delivered it cites the gap.
UM-NATOS-009 §9 recorded that nothing had audited the other consequences of
`PS.EXCM`; nothing has, and it is still listed as open in Chapter 30.

The reports are the primary record. This book is not a replacement for them and
does not try to be. What it adds is *continuity*: a defect found in the touch
driver in Chapter 19 is the same defect that decided the launcher's icon
selection in Chapter 24, and the reports say so in cross-references while this
book says so in a narrative. A reader picking up UM-NATOS-017 cold learns that
the X axis was inverted; a reader of this book learns why nothing noticed for
three days, which is the transferable part.

## The shape of the thing being described

nat-os runs on the ESP32-2432S028R — the board sold everywhere as the "Cheap
Yellow Display". A dual-core Xtensa LX6 at 240 MHz nominal (the system runs at
80 MHz; `kernel/clock.c` sets it, and until UM-NATOS-036 nothing did), 520 KB of internal
SRAM of which about 176 KB is usable as data, 4 MB of flash executed in place
through a cache, an ILI9341 240×320 panel, an XPT2046 resistive touch
controller, a microSD slot, a light sensor, a speaker connector, and an RGB LED.

No PSRAM. No MMU paging. That second absence is the single fact from which most
of this system's architecture follows, and Chapter 1 is about it.

The kernel starts at what UM-NATOS-001 calls layer L2 — scheduler, memory,
synchronisation — and owns everything above it. L1, the second-stage bootloader,
was borrowed for most of the story this book tells: 17,536 bytes that configure
the flash controller, program the flash MMU, enable the cache and jump to an
entry point. The interface between L1 and nat-os is the image header and nothing
else, which is why replacing L1 later was a contained change rather than a
rewrite — and it has since been replaced. `boot/` holds this project's own
second-stage bootloader, and the chapters that describe the borrowed one carry
since-written notes saying where the responsibility moved. Chapter 1 §1.2 has
the summary, including the defect that came with it.

## How to read this

**If you are new to the project**, read Chapters 1 through 6 in order. That
gives you the hardware, the calling convention, how the thing boots, where
everything lives, how it is built, and what "milestone 0" proved. About 60 pages,
and after them the rest of the book can be read in any order.

**If you are picking up implementation work**, read Chapter 30 first. It is the
consolidated inventory of everything the system does *not* establish, drawn from
the "what this does not establish" section of every report. It is where the next
defect probably lives.

**If you are debugging something**, Chapter 28 is the catalogue of every
instrument in this kernel that has been caught reporting confidently and wrongly,
and Chapter 29 is the set of standing rules those failures produced. Both are
short and both have saved time more than once.

**If you want the reference material**, the appendices carry the ISA, the shell
commands, the register map, a file inventory, and a table of every number this
project has measured on hardware.

## A note on the failures

This book records the defects honestly, including the embarrassing ones. A touch
axis that was inverted for three days behind a calibration that could only ever
return one answer. A tick deadline that raced into the future on every yield. An
idle task that silently failed to be created and whose absence was printed on
screen and read without being registered. A self-test that could not fail by
construction and was believed anyway. A capture harness that rebooted the board
it was measuring and then reported twelve seconds of a system nobody was
touching. Three separate peripherals whose registers read back byte-for-byte
correct while the hardware sat completely dead.

None of that is included for colour. The failures are more instructive than the
successes because the successes are mostly the ordinary application of published
technique, and the failures are where this project learned something it could not
have read. Where a defect produced a rule, the rule is stated at the point of the
defect and again in Chapter 29.

One theme recurs often enough to name here rather than let it accumulate: **the
kernel's own instruments were wrong more often than the hardware was.** Chapter
28 counts seven separate cases in a single session. The pattern is always the
same — a signal that the measurement was invalid, sitting next to a number that
looked reasonable, and the number believed.

## Acknowledgement of scope

This is one board, one panel, one flash chip, one microSD card, one finger, and
one person's judgement about whether an icon is legible at 24 pixels. Where a
result depends on that, this book says so. The scheduler, the heap, the arena
model, the bytecode VM and the application model assume an ESP32 and nothing
more; the board-specific parts are the pin maps and the panel and touch
controllers, and they are confined to their own files.
