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

### There is no device model

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

### Syscall validation is per-call, not systematic

> Every syscall added since checks its own arguments, and each was reasoned about
> individually. **Nothing enforces that a future one will, and there is no shared
> harness that would catch an unchecked length in a new service.**

Given that the whole isolation claim rests on those checks, this is the most
dangerous gap in the book. Chapter 17's `SYS BLIT` shows how much care one
syscall needed; nothing guarantees the next one gets it.

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

### The DMA stall, and the duplicate re-send it causes

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

### The 3D view's startup glitch

Garbled for ten to thirty seconds after opening; repaired by launching an
unrelated program. Eight theories eliminated by direct measurement; a ninth has a
mechanism and is untested. `display_resync()` is the instrument built to settle
it.

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
- **Priority inheritance drops a boost on the first release**, so nested holds of
  two boosted mutexes lose it early. "Nothing in this kernel holds two."
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
