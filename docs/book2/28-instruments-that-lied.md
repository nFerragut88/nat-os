# Chapter 28 — The Instruments That Lied

> Sources: `docs/UM-NATOS-017` §6, `docs/UM-NATOS-018` §5.1, `docs/UM-NATOS-028` §8, and every other report
> Code: throughout

---

## 28.1 Why this chapter exists

Across 138 commits and thirty reports, this project spent more debugging time on
faulty *measurement* than on faulty *hardware*. That is not a figure of speech.
UM-NATOS-028 §8 catalogues seven instruments that lied in a single session, and
ends:

> Every automated check this kernel has said everything was fine, and every one
> of them was telling the truth about the narrow thing it measured.
>
> The person looking at the screen was the only instrument that could see the
> actual fault, and twice in this session that person was right while the
> counters and the reasoning were wrong.

This chapter collects every instance in the project, groups them by failure
shape, and states what each shape looks like from the outside. It is the chapter
to read before diagnosing something.

## 28.2 Shape 1 — an instrument that reports its own reading invalid, believed anyway

**The signature: a signal that the measurement is invalid, sitting next to a
number that looks reasonable.**

UM-NATOS-017 §6 identified three instances and treated the pattern as the
finding:

| Instrument | What it said | What was believed instead |
|---|---|---|
| The marker block (Ch. 17 §17.9) | Frozen — the system is halted | The colour bars, which a frozen screen renders identically |
| `s/e=150`, three runs (Ch. 19 §19.4) | This counter is not accumulating | The pressure values printed beside it |
| A boot banner in a capture (Ch. 19 §19.4) | The board just restarted | The counters, as though continuous |

A fourth arrived later:

| The flash divider sweep (Ch. 20 §20.7) | Identical output across three configurations | The theories being tested |

> The peripherals were largely fine. **The verification method was what kept
> failing**, and it failed in the same shape each time.

### What breaks the deadlock

> **Latch the quantity so timing cannot lie about it**, then feed the system a
> controlled input rather than interpret an uncontrolled one.

Both halves are now habits in the tree. Latching:

```c
/* Extremes since boot. Confirming what the pressure channels do under a finger
 * needs a capture to coincide with the press, which is not something that can
 * be arranged reliably. Latching the extremes makes the question answerable at
 * any later moment instead. */
```

```c
/* The outcome of the last run, kept rather than only printed.
 *
 * The result line is emitted the instant the fourth target is tapped, which is
 * exactly when nobody has a capture attached ... A number that has
 * already scrolled past is not a measurement. */
```

Controlled input: `intr_selftest()` injects edges; `adcdrive` commands the input
voltage; the flash probe sweeps sixteen configurations in one boot; `touchcfg`
and `spiclk` make a variable adjustable while somebody watches.

## 28.3 Shape 2 — a test that cannot fail

**The signature: a real test, correctly performed, structurally incapable of
producing a negative result.**

Three instances, in three different subsystems:

### The direction test (Ch. 19 §19.8)

Direction inferred from the last sample of a drag. Release samples read the ADC
rail; a rail reading is near the top of any range; so *every* axis appears to
increase.

> The test could only ever return one answer.

### The IO_MUX cross-check (Ch. 21 §21.4)

Four entries checked, all four below the anomaly.

> The check was real, it was performed, and it could not have failed.

> **A cross-check only tests what its samples can distinguish.** The samples must
> **straddle** the thing being verified, not merely agree with it.

### The speaker self-test (Ch. 23 §23.4)

`spktest` drove GPIO32, which has no RTC mux and is therefore structurally
incapable of exhibiting the fault under investigation.

> **Verifying an instrument on the one case that cannot fail is not
> verification.**

And a fourth, which was caught by *failing*:

### The critical-section test (Ch. 11 §11.7)

Originally ran before `timer_start()`, where `timer_ticks()` is 0 and stays 0
regardless of what masking does.

> a test that could only ever pass by accident. **It reported FAIL, which is the
> one outcome that made the flaw visible.**

That is worth dwelling on: three of these were discovered by an unrelated failure
weeks later; one was discovered because it happened to be positioned where its
tautology produced the wrong answer rather than the right one.

### The mitigations now in the tree

- The bounds cross-check uses 35 cases chosen to include "zero lengths, the exact
  end, one past the end, and lengths chosen to wrap the address space" — samples
  that straddle by construction.
- The speaker probe **prints its own blind spot**, so the next reader is told
  what it does not cover before they read the result.
- The calibration's pair check compares readings that *should* match, which is a
  test whose failure mode is specific rather than vague.

## 28.4 Shape 3 — a register that reads back perfectly while nothing happens

**The signature: complete configuration confidence, zero effect.**

Four instances:

| Symptom | Reality | Chapter |
|---|---|---|
| ADC returning ~619 on every channel | Converting, attached to nothing — one wrong bit inside a twelve-bit field | 22 §22.10 |
| GPIO edge latched, CPU never interrupted | Delivered to the APP CPU, which is halted | 22 §22.5 |
| LEDC configured perfectly, silent | Clock gated off | 23 §23.4 |
| WiFi MAC accepting a masked write | Unknown until proven otherwise | 27 §27.3 |

> **A read-back cannot detect any of them.**

The ADC case is the sharpest, because it defeats a discipline that had been
working:

> **A wrong bit in the right register is invisible to a read-back.** That is the
> one failure mode the read-back discipline cannot catch, and it is worth naming
> because that discipline had been working well enough to feel sufficient.

### Four different answers, one per case

Each was caught by a technique the others could not have used:

1. **ADC** — a control group. Four pins that *should not* respond to light,
   measured simultaneously with the one that should. 265 counts against a
   noise floor of 47.
2. **GPIO** — injection. Five candidate bits swept in one boot, with the handler's
   own counter as the oracle.
3. **LEDC** — an external reference. "It works fine on my other project."
4. **WiFi MAC** — motion. Scan the register window twice and count words that
   changed with nothing driving them. A gated block is static; a live one has
   free-running counters.

The fourth is the most general and the only one that does not ask the hardware a
question at all. If a peripheral can be observed to *move*, no write that went
nowhere can fake it.

## 28.5 Shape 4 — a clock measuring the wrong thing

**The signature: a duration that varies with system load rather than with work.**

Three clocks, each correct for a different question, each used for the wrong one
at least once:

| Clock | Measures | Where it lied |
|---|---|---|
| `xt_ccount()` | Wall clock | A full-screen fill: 43 ms single-threaded, 249–362 ms from a task. The framebuffer's "450 µs per address window". The DMA timeout that fired on a working engine. |
| `task_cpu_cycles()` | Cycles the task ran | Only advances at context switches, so it cannot time a spin *inside* a slice. Reported a blit as both a 44% improvement and a serious regression; it was neither. |
| `timer_ticks()` | Was: ISR entries. Now: elapsed time | 217 ticks per real second where 100 is correct. Every tick-denominated deadline came due early by a load-dependent factor. Silent for days. |

Plus the comparator itself, which stalled for up to 183 ms at a time
(Ch. 7 §7.9).

The consequences reached everywhere:

> Sleeps ran short, timeouts ran loose, and **every frame-rate figure this
> project has ever recorded was measured against a clock running fast by a
> load-dependent factor.**

and

> Both figures above were taken on a clock that intermittently stalled and under
> draw-lock contention that dominated the frame. With both fixed the framebuffer
> is worth having and is now the default.

A conclusion published, defaulted off, and reversed — because the clock it was
measured against was wrong.

### The mitigation

The comment in `task.c` is the most useful thing written about this:

> **MEASURED, and the correction is not uniform.** A full-screen fill from the
> SHELL task read 249-362 ms wall-clock and 63-79 ms of work — the shell runs at
> NORMAL and is preempted constantly. The raycaster's blit did not move at all:
> 55.5 ms either way, because the display task runs at HIGH and is barely
> interrupted.
>
> Which means the fix matters most exactly where the old numbers were least
> trusted, and changes nothing where they were quietly correct. **A conclusion
> drawn from one task's timings could not have been carried to another's.**

## 28.6 Shape 5 — a diagnostic that changes what it measures

Two instances, and the second was caught because of the first:

**The contaminated blittest** (Ch. 27 §27.14) called `desktop_set_active(1)` to
stop the renderer overwriting a test pattern — which put the launcher into
repaint mode and erased the test block itself.

> **A diagnostic that changes the state it is measuring is worse than no
> diagnostic**, and this one was believed for several rounds.

**The correlated sample** (Ch. 22 §22.6): sampling `GPIO_PIN36` from the shell
returned zero three times running against a window covering 93% of the cycle.

> That is not chance — **the shell's sampling is correlated with the touch task
> being awake.** Only the armed task could answer without the measurement being
> correlated with its own subject.

The answer in both cases is a **control**:

> The **control** row is the load-bearing one. Taken with nothing done in between,
> it proves the freeze holds the buffer still and the sampler is stable — without
> it, "no change" would be indistinguishable from a broken measurement. That is
> the same lesson as the contaminated blittest above, **applied in advance for
> once.**

## 28.7 Shape 6 — a startup artefact standing in for behaviour

**The signature: a mechanism confirmed to *exist* rather than observed to
*work*.**

The canonical case is the shell (Ch. 12 §12.5):

> The shell was verified by observing that its **banner printed**, not by
> observing that a command **ran**. A banner proves the task was created and the
> transmit path works. It says nothing whatsoever about receive.

The receive path was one byte behind for the shell's entire existence — every
interactive session since M5.

The same substitution accounts for three more:

- **Stack guards** — a guard word was written, and `task_stack_headroom()` was
  never called (Ch. 12 §12.4).
- **The panic handler** — present and linking, never triggered (Ch. 12 §12.6).
- **The hang detector** — a watchdog register was armed, and its interaction with
  the panic path was never exercised (Ch. 12 §12.2).

And the cruellest variant:

> the question asked to confirm the driver — whether the colour bars were in the
> right order — is one a **frozen screen answers identically to a live one**. A
> static image was treated as evidence of a running system.

### The mitigation

Three shell commands that exist for no other purpose:

| Command | Induces |
|---|---|
| `hang` | masks interrupts, spins — the watchdog must reset the board |
| `fault` | executes `ill` — the panic handler must report and halt |
| `smash` | clobbers the running task's guard — the next switch must panic, naming the task |

> These are deliberately not compiled out. **A destructive command in a
> development shell is a smaller risk than a recovery path nobody can reach to
> test.**

## 28.8 Shape 7 — the harness did not run

**The signature: a run of results that do not vary when the input does.**

Two hypotheses about flash timing were recorded as tested against a board that
had never been reflashed (Ch. 20 §20.7). The invocation named a script that does
not exist; PowerShell's error went to a filtered stream.

> Every hypothesis returned bit-identical output, which was read as "none of
> these are the cause" when it meant "no experiment has run yet".

> **A negative result is only informative if the experiment demonstrably ran.**
> When consecutive hypotheses produce identical output, suspect the harness
> before the theory — and verify the change reached the target, rather than
> inferring it from the absence of an error message.

The near-identical failure in Chapter 19 §19.4 is the same shape from the capture
side, and produced the same signature: three runs, exactly 150 samples each.

And a related case in Chapter 12 §12.9: a scripted edit that silently rewrote the
body of the function it was redirecting.

> It is the **fourth scripted edit in this project to fail silently**, and the
> reason those edits now carry assertions that stop on a missed match rather than
> writing an unchanged file.

## 28.9 Shape 8 — a name that misleads

Two instances, both cheap to fix and both having cost real time:

**`raycast_framebuffer()`** returns a boolean and is named like an accessor. A
diagnostic used it as a pointer and stored through address `0x00000001`.
`raycast_fb_ptr()` now exists.

**`display_lock_wait_*`** would have been the natural name for the lock timing
accessors, and would have been wrong:

```c
 * The name matters, because "wait" would imply the panel was the bottleneck and
 * send the next person to shorten holds, which was tried and changed nothing.
```

They are `display_lock_blocked_*`. A name that asserts a mechanism is a claim,
and a wrong one sends the next reader in the wrong direction.

## 28.10 What actually worked

Positively, the techniques that broke deadlocks in this project:

**A control group measured simultaneously.** The ADC's four touch pins. The
`dfreeze` control row.

**Two independent clocks agreeing.** The TSF timer against CCOUNT, 0.1% over half
a second.

**An oracle that lives on the chip.** The eFuse CRC8 beat a hand-maintained OUI
list. `esptool`'s chip ID beat a belief about the part.

**A response nobody local could forge.** A probe *response* addressed to this
station, which "a radio cannot hear itself" makes unforgeable.

**Sixteen measurements in one boot.** The flash divider sweep. The
interrupt-enable bit sweep. `touchcfg`'s four combinations.

**Arithmetic that must balance.** `1,097,103 × 3 + 3 = 3,291,312` against an
observed 3,291,313. `14,999² = 224,970,001`. `240 × 320 × 2 + 34 = 153,634`.

**A pair of counters, one that must be non-zero and one that must be zero.**
`81/109211`. `136 fills / 0 escapes`. `265 counts / 47 control`. `372 frames /
374 recycled`.

**A switch that can be flipped.** `fb`, `spiclk`, `touchcfg`, `dfreeze`,
`DISPLAY_USE_SPI2`.

> a switch that can be flipped is a claim that can be rechecked.

**Reading the source of truth instead of probing around it.**

> Probing is the right tool when the question is *what does this hardware do*. It
> is the wrong tool when the question is *what is this constant*.

**A person looking at the screen.** Three times, a human observation outranked
the instruments and was right:

- *"the dots are following my finger"* — overridden as leftovers, correct
  (Ch. 19 §19.4)
- *"it works fine on my other project"* — eliminated the entire hardware half of
  the search space (Ch. 23 §23.6)
- *"no, it is a bug in the code, because the X button is also missing"* —
  correct against a confident hardware diagnosis (Ch. 27 §27.11)

Two more from the display defect (Ch. 18 §18.11), both from the person holding
the board rather than from anything in the tree:

- *"if I leave it on for a while, like maybe 5 minutes, then 3D view will open
  immediately fine, without running gfxrogue at all"* — offered as an aside and
  filed as a curiosity. It is the whole diagnosis. A fault that heals on a timer
  has a timer in it, and this system had exactly one timer that could disable a
  transport permanently.
- *"the kernel has to let the tasks take turns, right? I am just thinking out
  loud, I could be wrong"* — correct, and correct about the part that was
  actually load-bearing. `gfxrogue`'s effect on the view was never about pixels;
  it was about adding chances for the display task to be descheduled mid-wait.
  The mechanism was scheduling, exactly as guessed, and the guess was made
  against a confident and wrong analysis of the drawing path.

Both were volunteered tentatively. Neither was acted on for hours. That is now
five for five: **every time a human observation contradicted the instruments in
this project, the human was right.**

## 28.11 Shape 9 — a fallback path where the bug is genuinely absent

This is the most expensive shape in the book, it was found last, and it subsumes
eleven correct eliminations.

The display's DMA path had a one-bit defect (Ch. 18 §18.11). It also had a
wall-clock timeout that tripped spuriously on a preempted display task, and a
timeout **permanently disabled DMA** and fell back to the 64-byte FIFO path.

The FIFO path never touches `SPI_DMA_OUT_LINK_REG`.

So within about twelve seconds of boot, the system moved itself into a
configuration where the bug was not merely hidden but *genuinely not present*.
Every subsequent measurement was correct. Every elimination was valid. All of
them were performed on a machine that no longer had the defect.

> Eleven theories were investigated and eliminated before this was found. All
> eleven were downstream of it, and several were eliminated *correctly* — on a
> system that had already fallen back to a path where the bug genuinely was not
> present.

What makes this different from Shape 6 (a startup artefact standing in for
behaviour) is the direction: there, boot-time state was mistaken for steady
state. Here, **steady state was the artefact**, and the interesting window was
the first twelve seconds — the part everyone waited through before testing.

The tell was available the whole time and read as noise: *"if I leave it on for
five minutes, the 3D view opens fine."* A fault that heals on a timer is a fault
with a *timer* in it, and this system had exactly one timer that could disable a
transport permanently.

**The self-healing symptom was the diagnosis.** It was filed as a mystery for a
day.

### And the corollary that made it worse

The guard was raised — correctly, for sound independent reasons — and the device
got *worse*, because the spurious timeout was the only thing keeping the display
usable. This produced advice, written into this book, that amounted to "keep the
configuration in which the defect is invisible" (Ch. 18 §18.9).

> **Before fixing a defect, find out what depends on it.** The timeout bound was
> genuinely wrong and had become load-bearing.

## 28.12 Shape 10 — the instrument corrupted its own evidence

Built to end this class of problem, and it manufactured a fault on its first run.

`fbdump` sends the 3D view's framebuffer out over the UART as hex so a host can
reassemble it as a PNG — the direct answer to "a counter cannot see a picture",
and the right instrument for a system whose MISO reads all zeros and therefore
cannot be read back off the glass.

The first dump ran **without holding the console**. The reporter's periodic
status line landed in the middle of the hex, shifting rows; the host parser
silently discarded 61 malformed rows and reassembled the survivors as though they
were contiguous. The result was a convincing rainbow-streaked mess.

It was reported as proof of a render bug. It was proof of a dump bug.

With `console_lock()` held across the dump, the same buffer is pristine — and
that pristine image is what cut the search from "the system" to "between the
buffer and the glass", which is where the defect actually was. The instrument was
right the moment it stopped competing with the thing it was measuring.

Three fixes, each closing a separate way to be wrong:

1. **Hold the lock.** When a diagnostic and a subsystem share a resource, the
   diagnostic will corrupt the evidence.
2. **Index every row.** Position is now *read* rather than inferred from arrival
   order.
3. **Refuse incomplete input.** The host reads to an explicit `FBEND` terminator
   and prints `rows received N/112, malformed M, missing [...]`. A fixed capture
   window had silently truncated the dump at row 49, twice.

> **`!! DUMP INCOMPLETE` is worth more than a plausible image.**

### Two more from the same session

**A constant hash called decisive.** `camfreeze` + `fbhash` was presented as
settling render-versus-transport. It does not: a frozen camera stably rendering
a *wrong* scene produces a constant hash too. The test was sound; the claim made
for it was not. (Shape 2 — a test that cannot fail — in a new costume.)

**Circular evidence about a guard.** The timeout bound was raised, `dmastat` then
read `timeouts=0`, and that was taken as evidence the raise had been unnecessary.
The raise is *why* it read zero. **A guard can only be shown unnecessary by
measuring it in its absence.**

## 28.13 The tally

| Shape | Instances | Chapters |
|---|---|---|
| Instrument reports itself invalid, believed anyway | 4 | 17, 19 ×2, 20 |
| A test that cannot fail | 4 | 11, 19, 21, 23 |
| Register reads back, hardware dead | 4 | 22 ×2, 23, 27 |
| A clock measuring the wrong thing | 4 | 7, 9, 18, 25 |
| Diagnostic changes what it measures | 2 | 22, 27 |
| Startup artefact standing in for behaviour | 4 | 12 ×3, 18 |
| The harness did not run | 3 | 12, 19, 20 |
| A misleading name | 2 | 25, 11 |
| **A fallback path where the bug is absent** | **1** | **18, 25** |
| **The instrument corrupted its own evidence** | **4** | **18 ×4** |

Thirty-two distinct instances. Against them: four defects that were genuinely
in a peripheral's protocol or wiring, and none at all that turned out to be
faulty hardware.

That ratio is the single most useful number in this book.

### Five more, from the session before

The table above counts the instances this chapter examines in full. The session
that *preceded* the one-bit fix contributed five more, catalogued in Chapter 28b
§28b.7 and listed here so the shapes stay in one place:

| Instance | Shape |
|---|---|
| The boot self-test's `dma=N/0`, measured before the scheduler exists | 6 — a startup artefact standing in for behaviour |
| A guard raised, then read as unnecessary because it no longer fired | 2 — a test that cannot fail, inverted |
| `fbsum`'s six-number summary called a checksum | 9's cousin: a sample that cannot distinguish the two states |
| A harness that accepted an `EARLY` delay and never used it | 7 — the harness did not run |
| A parser returning the empty remainder after a bare token, for 38 samples | 8 — a name, and a field, that mislead |

Two counting bases coexist in the record and are worth keeping apart. This
chapter counts **instances by shape**, which is what a reader can act on;
UM-NATOS-030 §8 counts **instruments caught lying, cumulative**, which reached
fifteen at the close of that session. Neither number is the other's total, and
neither is wrong.

The last two rows of the tally were added after this chapter was first written,
by the defect in Chapter 18 §18.11. Note which shapes they are. Shape 9 cost
eleven correct eliminations because the *system* moved the bug out of reach;
Shape 10 was
committed four times by instruments built specifically to stop this from
happening again. The chapter's own thesis got a harder test than it wanted and
passed it in the least comfortable way available:

> The instruments were not merely unreliable in general. They were unreliable
> *about the specific thing being investigated*, which is when it matters.

---

**Next:** one fault, in full — the two sessions behind Shapes 9 and 10, and what
a day of correct experiments on the wrong machine looks like from inside.
