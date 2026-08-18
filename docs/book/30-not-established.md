# Chapter 30 — What Is Not Established

> Sources: the "what this does not establish" section of all 28 reports
> Code: throughout

---

## 30.1 Why this chapter is the most useful one

Every report in this project ends with a section stating what it does *not*
prove. The project README explains why:

> **Every report ends with what it does *not* establish.** Those sections are the
> most useful part. They are where the known gaps live, and several of them
> correctly predicted the next defect.

Three predictions came true:

| Predicted in | Prediction | Came true in |
|---|---|---|
| UM-NATOS-011 §4 | Writing flash will collide with the data cache | Chapter 20 §20.5 — "It broke exactly as predicted, on the first version of the driver" |
| UM-NATOS-010 §8 | `arena_contains()` exists and nothing calls it; arenas are bookkeeping, not protection | Chapter 14 — closed by the interpreter |
| UM-NATOS-009 §9 | Nothing has audited the other consequences of `PS.EXCM` | **Still open**, §30.2 |

This chapter consolidates all of them, ordered by what they would cost if
someone relied on the system anyway.

---

## 30.2 Structural gaps — the ones that shape what can be built next

### ~~There is no device model~~ — closed

> **Closed** (UM-NATOS-031). `sys device` reaches a table of seven devices —
> `light`, `beep`, `store`, `i2c`, `keys`, `echo`, `sd` — and a new peripheral is
> a table entry rather than a kernel edit and an ISA change. Seven of the eight
> things listed below as unreachable are reachable; only the network is left, and
> that is blocked on transmit rather than on architecture.
>
> One thing the design did not survive intact, recorded because "we designed it
> and it worked" is worth nothing without the entry that did not fit: the
> deliberately narrow read/write interface **had to grow bulk transfer** when the
> SD card and I²C were pushed through it (UM-NATOS-031 §4).
>
> It also opened a gap of its own, since every application could then reach every
> device — closed in turn by per-application permissions (UM-NATOS-032), with the
> caveat in §30.2 below.

The largest gap in the system, and the only *structural* item on the post-M5 list
that is missing. UM-NATOS-007 §2.1:

> Every driver above is reachable only from the kernel. The VM has twelve
> syscalls, all hardcoded, and no device model — so an application cannot read
> the light sensor, scan the I2C bus or receive a keypress. Each new peripheral
> has meant a kernel edit plus a hand-written syscall, which was tolerable at two
> and is the obvious next piece of *architecture* rather than more drivers.

The consequences appear in six separate reports as the same line: *no application
can reach it*. ADC, I²C, audio, persistence, microSD, text entry.

`notes.h` states the specific case most sharply:

> text entry is a kernel feature rather than a service applications can request.
> Any program wanting text input cannot have it. ... **the note pad takes text,
> so text entry appears solved, and it is solved for exactly one program.**

### ~~Syscall validation is per-call, not systematic~~ — closed

> **Closed** (UM-NATOS-031 §2). `kernel/vmarg.c` is the shared harness: one place
> where an `(offset, length)` pair from a program is checked, enforcing
> offset-domain arithmetic, bound-before-multiply, and copy-don't-lend.
>
> It was built **before** the device model rather than after it, which is the
> part worth keeping. The device model is the extensible surface — the whole
> point is that people who have not read this book will add entries to it — and a
> harness added afterwards would have been a harness the first three devices did
> not use.

> Every syscall added since checks its own arguments, and each was reasoned about
> individually. **Nothing enforces that a future one will, and there is no shared
> harness that would catch an unchecked length in a new service.**

Given that the whole isolation claim rests on those checks, this is the most
dangerous gap in the book. Chapter 17's `SYS BLIT` shows how much care one
syscall needed; nothing guarantees the next one gets it.

### There is no image identity — and two things depend on it

New, and the direct successor to the two items above. nat-os has **no way to say
which program a set of bytes is**: no signature, no content hash, nothing. Anyone
able to flash the board can put any bytes behind any name in the program table.

Two features are limited by exactly this, and neither can be finished before it:

- **Device permissions are containment, not security** (UM-NATOS-032 §3). They
  bound accident, not intent. This must not be described as security in any
  report until image identity exists.
- **The `store` device banks persistence on a reusable application slot.** A
  program granted `store` that lands in a slot an earlier program used reads what
  that program left. It is *not* fixed by clearing the bank on retire, which
  would delete the persistence the device exists to provide. The bank wants to be
  keyed on which program, and that is the same missing piece.

### No host-side test path

> **No test target.** There is no host-side unit test path; everything is verified
> on hardware.

Several defects in this book are in *pure functions* that a host test could
exercise exhaustively: the bounds predicates, the offset-domain clipping, the
ring-buffer arithmetic (which *was* checked that way, by hand — Chapter 26
§26.12), the calibration fit, the fixed-point projection.

### No filesystem

The microSD driver reads blocks and decodes an MBR far enough to find a FAT16
signature. There is no directory walk, no file lookup, no FAT chain traversal.
Consequently:

> **No program loading from storage.** Images are compiled into the kernel. There
> is no filesystem, so "install an application" has no meaning yet.

### No asset pipeline

> Nothing yet converts a font or bitmap into a linkable read-only object. The
> mechanism exists; the tooling does not.

The launcher's icons are "in the image because there is nowhere else to put them
yet".

### No execute-in-place for code

Only data is mapped from flash. An IROM segment "has not been attempted, and
would interact with the `l32r` constraint". The kernel is 119,151 bytes of text
against 131,072 of IRAM — enough headroom that it has not been forced, and not
much.

---

## 30.3 Open faults

### WiFi transmit does not reach the air

The single largest open item. The MAC accepts frames and reports 178 of 178
complete; twenty probe requests produced zero responses from two access points
that are received continuously.

Strongest untried lead: `periph_module_reset(0x19)`.

> nat-os has only ever *ungated* the WiFi peripheral, never *reset* it, and a MAC
> left in whatever state the ROM bootloader put it in would plausibly receive
> while refusing to transmit. That asymmetry fits the symptom exactly.

### ~~The DMA stall, and the duplicate re-send it causes~~ — closed

> **Closed** (UM-NATOS-030, Ch. 18 §18.11), though not as described here. The
> stall analysis below is correct and was entirely downstream: the actual defect
> was `DMA_OUTLINK_START` defined as `(1u << 30)`, which is `OUTLINK_RESTART`.
> Every DMA transfer ever issued asked the engine to resume the existing
> descriptor chain rather than begin the one just written.
>
> The duplicate re-send *was* a real defect and is separately fixed —
> `spi_tx()` now abandons a timed-out transfer rather than falling through. The
> timeout bound stayed raised at ~500 ms, which was always the right change and
> is now safe to keep because the thing it was masking is gone.
>
> Blit with DMA actually working: **31.4 ms**, against the 55.9 ms below.

DMA disables itself within about eight seconds of the raycaster starting, on a
spurious timeout caused by preemption landing between two adjacent lines. The
blit costs 55.9 ms instead of a possible ~22 ms, and **the 3D view has run this
way for its entire existence.**

The latent defect underneath it is worse than the performance cost:

> `spi2_dma_tx()` returns 0 on timeout, and `spi_tx()` then falls through to
> `spi2_tx()` — **re-sending bytes the DMA has already sent.** ... This is a
> latent bug in the shipped driver. It fires once per boot and is invisible,
> because the same timeout that causes it also disables DMA so it cannot happen
> again.

Three attempted fixes, three breakages. Guidance for the next attempt is in
Chapter 18 §18.9.

### ~~The 3D view's startup glitch~~ — closed

> **Closed.** Same one bit; same report. It was never a renderer fault and never
> only the 3D view — every transfer wider than 32 pixels was affected
> system-wide. `display_resync()` returned a correct negative, which is what
> eliminated the controller's window state and left the DMA stream.
>
> Eleven theories were eliminated before the cause was found. All eleven were
> correct, and several were performed on a system that had already fallen back to
> the FIFO path, where the bug genuinely was not present. Chapter 28 §28.11.

### MISO reads all zeros

New, and it is why the framebuffer had to be dumped over the UART rather than
read back off the panel. `panelid` gets `00 00 00 00 00` from both `0xD3` and
`0x04`. Either the panel's SDO is not populated on this module or the read path
is misconfigured, and **one negative does not separate them.**

### Phantom touches

New, and unexplained. Two independent unattended runs logged spurious presses at
~374 s and ~390 s. In the first, they **launched a program into slot 2.** Real,
reproducible, and with no proposed mechanism.

### PENIRQ never fires from a finger

The matrix is verified by injection, the handler runs, and the end-to-end path
has never been observed to work.

> real taps produced 24 touch events and 30 low PENIRQ readings while the armed
> edge detector latched nothing. **That gap is unexplained.**

Touch polls at 100 Hz instead, "byte for byte its previous behaviour", with
`touch_irq_wait()` intact and one line from reinstatement.

---

## 30.4 Isolation: the exact boundary of the claim

Stated in `arena.h`, `task.h`, `vm.h`, and three reports:

- **Native tasks are not isolated and never will be.** Nine tasks share one
  address space. "A driver bug can still corrupt the kernel."
- **Stack guards are a post-mortem, not a barrier.** A guard word is found
  *after* it has been overwritten. "A guard page would be a barrier; this
  hardware has no MMU to provide one."
- **Nothing catches a wild pointer** into the middle of a neighbour's stack.
- **The escape counter is not a proof of the clipping logic**, only evidence that
  it and the re-check agree on everything tried so far. "A case both get wrong
  identically would pass."
- **Four of ten VM fault classes have never been exercised on hardware**:
  `VM_FAULT_PC`, `VM_FAULT_CALL_DEPTH`, `VM_FAULT_SYSCALL`, `VM_FAULT_STRING`.
- **A program can overwrite its own instructions.** No code/data separation
  within an arena.
- **No per-application draw accounting.** A program that draws constantly costs
  everyone's frame rate; the best-effort policy bounds the damage to the system
  without bounding it per program.

---

## 30.5 Concurrency and the scheduler

- **No `EXCM` audit.** Whether anything else in the kernel silently depended on
  `EXCM` being set for the handler's duration "has not been examined
  systematically". Open since M2.
- **No deadlock detection.** Two tasks taking two mutexes in opposite orders will
  hang, and the kernel will not say so. "The idle task will simply run."
- **No timeouts on `mutex_lock()`.**
- **No condition variables or semaphores** in the kernel proper. There is no way
  to wait for an *event*, only for a lock or a deadline. (The WiFi OSI layer
  builds its own on `task_sleep`.)
- **There is no priority inheritance.** Not "it has a limitation" — it does not
  run. `task_boost()` and `task_unboost()` exist in `task.c` and are correct;
  **nothing calls them.** `mutex.c` is 133 lines and contains no mention of
  priority at all, and `git log -S task_boost` returns exactly one commit, whose
  diff touches `kmain.c`, `task.c` and `task.h` and never `mutex.c`.

  The feature was described as shipped in UM-NATOS-014 §9, in that commit
  message, in this book's own front matter, and in the timeline. The mechanism
  was built and never wired to a lock.

  What actually prevents inversion is **ageing**, which is a different thing and
  is genuinely implemented (Ch. 9). `task.c` even says so, in a comment written
  while the dead code sat forty lines below it:

  > The base priority is never modified: ageing is a property of the SELECTION,
  > not of the task... That distinction is what keeps this separate from priority
  > inheritance, which really does change a task's priority and really does have
  > to undo it.

  The old entry here read "priority inheritance drops a boost on the first
  release, so nested holds of two boosted mutexes lose it early" — a precise
  caveat about the nesting behaviour of a code path that never executes. **A
  documented limitation is not evidence that the thing it limits exists**, and a
  sufficiently specific caveat is good at looking like one.
- **Critical sections are unbounded by convention only.** Nothing measures or
  enforces how long one is held.
- **The mutex is untested against an ISR**, and nothing enforces the documented
  prohibition.
- **Heap allocation from an interrupt handler would corrupt the list**, and a
  critical section taken inside a handler is a no-op. Nothing prevents it.
- **No fragmentation characterisation under realistic load.** The leak test's
  size and lifetime distributions are both uniform; "a first-fit allocator's
  worst case is workload-shaped".
- **No allocation latency measurement.**
- **APP_CPU is never started.** Every use of `crit_enter()` assumes single-core
  and would need re-examining.
- **`task_wake()` has never woken a task.** "It exists, it is correct by
  inspection, and no interrupt has ever called it successfully."

---

## 30.6 Measurement and calibration

- **The DRAM budget is unremeasured.** 167,680 B at M3, 158,048 B after
  `TASK_MAX` rose to 8; it is now 12 with three more drivers, "so the current
  figure is lower and unremeasured".
- **Dispatch cost is a ceiling, not a measurement.** ~109 cycles/instruction, from
  a quarter-share assumption that ignores interrupt-handler time.
- **The CPU frequency is derived, not read.** 80 MHz, inferred from tick counts
  against a host clock. "It should be re-derived rather than assumed if boot
  configuration changes."
- **The SRAM region boundaries are transcribed, not verified.**
- **Cache consumption of SRAM0 under XIP is unmeasured.**
- **Flash cache-miss latency is unquantified.**
- **The touch calibration is linear, run once, on one unit, with one finger.**
  Nothing measures panel non-linearity *between* the target positions, and
  nothing detects a stale calibration.
- **`Z_THRESHOLD` is one number from one measurement.** "A lighter touch has not
  been tested, and a threshold that rejects a real press is worse than one that
  admits a bad sample."
- **The SPI clock ceiling was found by eye.** 80 MHz "works electrically and puts
  visible noise on the glass"; nothing in the kernel can measure that.
- **Double-tap timing is unmeasured.** 600 ms and the 2-tick minimum press were
  reasoned. "The counters exist to settle them and nobody has."
- **The skip ratio is measured under one workload.**
- **The speaker's response is uncharacterised** between 440 Hz and 3 kHz.
- **The ADC is uncalibrated** — counts, not volts, with factory eFuse data unread,
  and only channel 6 proven to track anything.
- **The save cadence constant has drifted from its justification.** 256 frames was
  ~60 s at 4.4 fps; the renderer is now several times faster.
- **The AHB alias for UART receive is validated behaviourally, not from
  documentation.** No erratum is cited.

---

## 30.7 Storage and persistence

- **No wear levelling.** One sector, erased and rewritten in place.
- **No power-cut testing.** The checksum is designed to reject a torn record;
  nothing has interrupted a write mid-erase. "The mechanism is reasoned, not
  measured." Same for the message store.
- **No record upgrade path.** A version bump silently discards the boot counter,
  the frame total and the calibration.
- **The cache argument remains an argument.** §20.5's reasoning that no
  flash-mapped read can occur during an operation "has not been instrumented —
  there is no assertion that would fire if a future edit broke it."
- **No verification against a second board.** The flash clock finding is from one
  part.
- **SDHC is untested.** The `CCS` branch exists and has never executed.
- **No SD write path, no card-removal detection.**
- **The full message store has never been exercised**, so the oldest-dropped path
  has never run outside reasoning.

---

## 30.8 Failure handling

- **Only two fault kinds are recorded.** A hang recovered by the watchdog writes
  nothing at all, "so the most common recoverable failure is the one the record
  cannot describe."
- **A fault during the recording itself is not survivable.** The checksum would
  reject the torn record on the next boot, correctly, "and the reason would be
  lost."
- **Panic mode is untested against a genuinely broken controller.** Every test ran
  on a display that was working perfectly at the moment of the fault, "so the
  bounds have never actually been reached."
- **The panic screen has no failure path of its own.** If the panel is wedged, the
  byte count says so — but only to somebody with a serial cable, which is the
  audience the screen exists to replace.
- **No per-task stack sizing.** All twelve slots get 2 KB regardless of measured
  use.
- **No pre-emptive overflow detection.**

---

## 30.9 Peripherals

- **I²C has never transferred a byte.** START, STOP, the ACK bit, repeated START
  and the read path are correct by inspection only.

  > The first real device will be the first test of that logic, and the most
  > likely defects are the ones inspection is worst at: a half-bit of timing, or
  > an ACK sampled on the wrong clock edge.

  Clock stretching — the property that justifies the whole bit-banging argument —
  has never happened. No multi-master arbitration detection. No bus recovery.
- **ADC2 is untouched** despite being permanently free.
- **The display is write-only.** MISO is wired and unused; the panel is never
  interrogated.
- **No orientation control, no gamma correction, no text clipping, no damage
  tracking.**
- **No descriptor chaining** in DMA — most of the remaining gap to the 31 ms
  floor.
- **The DAC was never shown to be broken**, only never shown to work.
- **77 of 116 WiFi OSI entries are still stubs.**
- **The MAC register map is reverse-engineered** except eFuse and DPORT.
- **No association, encryption, or IP.** A raw 802.11 receiver, not a network
  stack.
- **The PHY runs a full calibration every boot** because there is nowhere to
  persist 1,904 bytes.

---

## 30.10 Interface

- **No focus, no windows, no z-order.** Applications occupy fixed strips.
- **The screen is fully allocated.** 320 of 320 rows. The next feature wanting
  rows must take them from something named, and the assertions will refuse a
  build that overlaps.
- **Application strips are 14 rows and nothing draws into one**, so nothing has
  tested whether that is enough.
- **Nothing tests the overlay.** The close button's position is agreed between two
  matching constants with no assertion tying them — "the exact drift the layout
  assertions were added to prevent elsewhere."
- **Faulted programs cannot be dismissed from the launcher.**
- **The running marker is per-name, not per-instance.**
- **The `fb off` path still flickers.**
- **No multi-touch, no gestures, no pointer-up event, no filtering beyond the
  pressure gate.**
- **One pointer, shared.** Nothing arbitrates focus.
- **The shell has no history, no editing beyond backspace, no completion.**
  Uppercase is impossible on the panel keypad.
- **The tee is single-slot and not reentrant**, and would capture another task's
  output if one printed without the console lock — "which has never happened,
  which is not the same as being prevented."
- **48 lines of scrollback is a bound**, and what leaves is gone.
- **Two copies of the multi-tap keypad**, with a stated threshold for extracting
  it (a third consumer).

---

## 30.11 Build and tooling

- **No dependency tracking.** Every build recompiles every file.
- **No incremental or parallel build.**
- **Hard-coded toolchain paths.**
- **The producer is an assembler, not a compiler.** No linking of separately
  assembled units, no relocation, no macros, no includes.
- **The syscall table is duplicated** between `vm.h` and `vasm.py`, with no
  runtime cross-check. The failure lands at assembly time with a comprehensible
  message, which is the mitigation rather than the fix.
- **JTAG was ordered and never arrived.** Every defect in this book was found
  without one.

---

## 30.12 The honest bottom line

One board. One panel. One flash chip. One microSD card. One finger. One person's
judgement about whether an icon is legible at 24 pixels.

The reports say it themselves, in three places:

> The calibration is this board's, but only this board's.

> Every judgement about whether the interaction feels right came from a single
> person on a single panel.

> Nothing measures whether the icons or the shading are legible. Both were judged
> by one person on one panel, **which is the same standard the report set
> criticises elsewhere.**

That last clause — a report criticising itself by its own standard — is the
project's documentation discipline working correctly, and it is the right note to
end this chapter on.

---

**Next:** what would be worth building on top of all of it.
