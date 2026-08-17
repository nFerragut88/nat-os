# Chapter 31 — Where It Could Go Next

> Sources: `docs/UM-NATOS-007-roadmap.md` §2.1, and the closing sections of every report

---

## 31.1 The one structural item

UM-NATOS-007's revision 1.1 makes the argument, and nothing since has weakened
it:

> **The one structural item on it is missing.** Every driver above is reachable
> only from the kernel. The VM has twelve syscalls, all hardcoded, and no device
> model — so an application cannot read the light sensor, scan the I2C bus or
> receive a keypress. Each new peripheral has meant a kernel edit plus a
> hand-written syscall, which was tolerable at two and is the obvious next piece
> of *architecture* rather than more drivers.

Seventeen reports later, the list of things applications cannot do has grown to:

- read the light sensor
- scan or talk to the I²C bus
- make a sound
- save state
- read the microSD card
- receive text
- read a key press
- know anything about the network

Every one of those is a kernel edit plus a hand-written syscall away, and the
thirteenth syscall would be as ad-hoc as the twelfth.

### What a device model would have to do

The confinement discipline of Chapter 17 is the constraint. Whatever replaces
hand-written syscalls has to preserve four properties that were expensive to
establish:

1. **Offset-domain arithmetic on every program-supplied quantity.** Four places
   in the current tree do this correctly, and each was reasoned about
   individually.
2. **Length bounded before it is multiplied.** `SYS BLIT` is the only syscall that
   has needed this so far, and it is the model.
3. **A pointer copied out of the arena before use**, not handed onward — because
   "a program's own stores could change the string while it is being rendered".
4. **The quantum distinction.** Any service whose cost is measured in
   milliseconds must end the caller's slice; any service that reads a published
   snapshot must not.

The report's own framing is the right one: this is *architecture*, not another
driver. It is the difference between a system that has twelve services and a
system that can have any number.

### And the missing harness

> Nothing enforces that a future one will [check its arguments], and there is no
> shared harness that would catch an unchecked length in a new service.

A device model that came with a validation harness — one place where a
program-supplied `(offset, length)` pair is checked, used by every service —
would close the largest safety gap in Chapter 30 at the same time as the largest
capability gap.

---

## 31.2 The three open faults, in order of tractability

### 1. The DMA duplicate re-send

The most tractable, and the guidance is already written:

> Fix the duplicate re-send first. It is a real defect, it is independent of the
> timing question, and **it makes any later timeout change safe rather than
> destructive.**

A timeout currently falls through to the FIFO path and re-sends bytes the DMA
already sent, advancing the panel's window by one span. Making a timeout
*abandon* the span rather than re-send it would cost one dropped row and remove a
class of corruption.

Then, separately, the timeout itself. `task_cpu_cycles()` is the principled clock
and cannot bound a spin inside one slice; the current answer is a raised
wall-clock bound. A DMA completion *interrupt* would remove the spin entirely,
and Chapter 22's matrix is the infrastructure for it — which would also give
`task_wake()` its first real consumer.

### 2. The panel window desynchronisation

A hypothesis with a mechanism and an instrument already built:

> with the view visibly garbled, force one clean `set_window()` and full-screen
> fill from the shell, touching no framebuffer. If the view repairs, it is panel
> desynchronisation, and the fix belongs in the blit path — re-asserting window
> and CS state per frame rather than assuming they survived.

One command, one observation, a binary answer. This is the cheapest open item in
the project and it is one `display_resync` away.

### 3. WiFi transmit

> `periph_module_reset(0x19)`. nat-os has only ever *ungated* the WiFi
> peripheral, never *reset* it, and a MAC left in whatever state the ROM
> bootloader put it in would plausibly receive while refusing to transmit. That
> asymmetry fits the symptom exactly.

And the test is already built and unforgeable: twenty probe requests, and count
frames addressed to this station.

---

## 31.3 The gaps whose closing would change the shape of the system

### A filesystem

The microSD driver stops at a FAT16 signature. A read-only FAT16 implementation —
directory walk, FAT chain traversal, file open and read — is perhaps 400 lines
and would make "install an application" mean something for the first time.

That in turn makes the assembler's output a *file* rather than a compiled-in
array, which makes `PROGRAMS[]` a directory listing, which makes the launcher's
grid dynamic.

UM-NATOS-001 §7 called writing FAT "a substantial subproject" and it is. It is
also the single change that most alters what the system *is*.

### A host-side test path

Several defects in this book are in pure functions:

- `vm_in_bounds()` and `arena_contains()` — already cross-checked on-device
  against 35 cases, which is the right test in the wrong place
- `vp_fill()`'s clipping and re-check
- the terminal's ring arithmetic — checked by simulating 70 lines through a
  48-line ring at all 40 scroll positions, by hand
- `calib.c`'s fit and pair check
- `map_axis()`
- the fixed-point projection in `raycast.c`
- `heap_check()`'s ten invariants, against a synthetic heap

None of those needs an ESP32. A host build of the pure files plus a few hundred
lines of test would let them be exercised exhaustively rather than sampled — and
Chapter 28's Shape 2 (*a test that cannot fail*) is much harder to construct when
the test enumerates its input space.

### An asset pipeline

The mechanism has existed since Chapter 4 — `.rodata` mapped from flash at zero
DRAM cost. What is missing is tooling: a script that turns a PNG or a font into a
linkable object. The icons are hand-written 8×8 constants "because there is
nowhere else to put them yet".

### Execute-in-place for code

119,151 bytes of text against 131,072 of IRAM. The headroom is real and finite,
and an IROM segment is the answer — with the `l32r` literal-pool constraint of
Chapter 4 §4.4 as the thing to solve first.

---

## 31.4 The smaller items, ranked by cost against value

**Cheap and valuable:**

- Extend the flash record to hold the PHY calibration data (1,904 B). Saves a
  full RF calibration on every boot.
- An assertion tying `desktop_overlay_into()`'s button position to
  `desktop_chrome_touch()`'s hit test — the one layout agreement that is adjacent
  rather than enforced.
- Per-task stack sizing. The margins are measured; the worst task uses 444 B of
  2,048 and the best uses 204. Trimming would return several KB of DRAM.
- Read the factory eFuse ADC calibration and report volts rather than counts.
- A `mutex_lock()` timeout, which would turn the undetectable deadlock of
  Chapter 30 §30.5 into a reported one.

**Cheap and uncertain:**

- Attach one I²C device. Everything in Chapter 22 Part C is correct by
  inspection and has never moved a byte; a single sensor would test all of it at
  once.
- Try 80 MHz on the panel with a person watching, now that `spiclk` makes it a
  runtime choice.
- Start APP_CPU, if only to stop half of every `INT_ENA` field addressing a core
  that does not exist.

**Expensive and structural:**

- The device model (§31.1).
- The filesystem (§31.3).
- SMP, which UM-NATOS-001 §7 said should be settled before M2 fixed the
  scheduler's shape, and which was never settled. Every `crit_enter()` in the
  tree assumes a single core.

---

## 31.5 What should not be done

A short list, because "do not build this" is as useful as a roadmap.

**Do not remove the bounds checks.** Stated twice, in two files, in the strongest
terms either can manage:

> that check is not optional and must never be compiled out for speed. If it is
> removed, this kernel has no isolation of any kind.

**Do not replace the `switch` dispatch with a computed goto** without a
measurement showing dispatch is the bottleneck.

> it is also the kind of construct where a missing entry is a silent jump rather
> than a diagnosed fault.

**Do not remove the destructive shell commands.**

> A recovery path that has never been observed to fire is confidence without
> evidence.

**Do not delete an instrument because its bug is fixed.**

> removing an instrument once it has found its bug is how the bug returns
> unnoticed.

**Do not accept a display change on the strength of a number.** Three attempts at
the DMA stall each had a plausible mechanism and a good number *before anyone
looked at the screen*, and all three broke the view.

**Do not chase the DMA stall by removing the guard.**

> Do not remove the guard to study the failure. That was the first mistake.

**Do not call a workaround a fix.**

> A workaround that is honestly labelled is a debt; one that is called a solution
> is a defect with good manners.

---

## 31.6 What this project already demonstrates

Worth stating plainly, because the preceding two chapters are entirely about
limits.

An operating system was written from scratch for a $10 development board, with no
RTOS, no C library, no framework, and no debugger. It has preemptive priority
scheduling with a bounded starvation guarantee, a checkable heap, software-
enforced memory isolation on hardware that offers none, a bytecode virtual
machine with a host toolchain, nine device drivers, a touch-driven interface, a
3D renderer, persistent state, three independent failure-reporting channels, and
an 802.11 receiver reached through a vendor binary in a different calling
convention.

It fits in 37,248 bytes.

Every measurement in it was taken on hardware. Every retracted conclusion is
still in the record next to its correction. Every mechanism that has never been
observed to work says so.

The gaps in Chapter 30 are long because somebody wrote them down.

---

**Part VI ends here.** The appendices follow.
