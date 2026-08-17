# Chapter 6 — Milestone 0: Proving Ownership of the Machine

> Sources: `docs/UM-NATOS-006-m0-verification.md`, `docs/UM-NATOS-007-roadmap.md`
> Code: `kernel/start.S`, `kernel/kmain.c`, `kernel/linker.ld`

---

## 6.1 Why M0 asserts rather than prints "hello"

A conventional first program prints a greeting. That proves the CPU reached the
kernel, and nothing else. It does not distinguish a working image from one where
`.data` was never copied, `.bss` holds reset garbage, or code is executing from
an unintended region.

> Those three failures are the characteristic failure modes of a hand-written
> linker script, and each produces symptoms that surface much later, in
> unrelated code, as inexplicable corruption. M0 therefore checks them
> explicitly at boot and reports each result, so a broken link map is caught at
> the point of change rather than during scheduler debugging.

And the reason this matters more here than it usually would:

> This matters more than usual here because **no JTAG probe is yet available**.
> Self-reporting is the only diagnostic channel.

The probe was ordered at M0 and never arrived. Every defect in this book was
found without one.

## 6.2 Configuration

| Item | Value |
|---|---|
| Date | 2026-08-14 |
| Target | ESP32 CYD board, fresh unit, COM5 |
| Image | `build/natos.bin`, 1,216 bytes |
| Bootloader | Borrowed, 17,536 bytes @ `0x1000` |
| Partition table | Borrowed, 3,072 bytes @ `0x8000` |
| Toolchain | `xtensa-esp32-elf-gcc` 14.2.0, `-mabi=call0` |
| Capture | Port opened before reset; auto-reset pulsed; 8 s window |

Flash write verified by `esptool`: all three regions reported "Hash of data
verified."

## 6.3 The captured output

```
ets Jul 29 2019 12:21:46

rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:2
load:0x3fff0030,len:1184
load:0x40078000,len:13232
load:0x40080400,len:3028
entry 0x400805e4


=====================================
 nat-os  milestone 0 — kernel alive
=====================================
  .data loaded : ok (0xc0deface)
  .bss cleared : ok
  bss span     : 0x3ffb0188 .. 0x3ffb018c (4 bytes)
  stack top    : 0x3ffdc200
  code at      : 0x40080088  (IRAM ok)

  no scheduler yet — next: timer interrupt + context switch
  heartbeat: 0 1 2 3 4 5 6
```

694 bytes received over the 8-second window.

## 6.4 The six assertions

| # | Assertion | Method | Result |
|---|---|---|---|
| 1 | Image header is well-formed and the bootloader accepts it | Banner appears at all; `esptool` reported checksum and SHA-256 valid | **PASS** |
| 2 | `.data` segment was copied to DRAM | Canary initialised to `0xC0DEFACE` in `.data`, read back at runtime | **PASS** — read back `0xc0deface` |
| 3 | `.bss` was zeroed by `start.S` | Canary in `.bss` compared against zero | **PASS** |
| 4 | Code executes from the declared IRAM window | Address of a function compared against `0x40080000`–`0x400A0000` | **PASS** — `0x40080088` |
| 5 | Stack is usable; call/return works under call0 | Every self-check is a C function call that returned | **PASS** (implicit) |
| 6 | Kernel remains running | Heartbeat counter advanced 0→6 over the capture window | **PASS** |

Assertion 5 is worth pausing on. It is not a separate test — it is the
observation that assertions 1 through 4 *printed at all*, which required a
working stack, a working call sequence, and a working return under an ABI whose
availability had been verified only by inspecting compiler output (Chapter 2
§2.4). It is the cheapest test in the file and it closes the largest question.

### Supporting observations

- **`rst:0x1 (POWERON_RESET)`** — a clean cold boot. Not a watchdog
  (`rst:0x3`/`0xc`/`0x10`) or panic reset, so the kernel did not crash and
  restart. This line is checked first on every subsequent capture in this book.
- **`.bss` span `0x3FFB0188`–`0x3FFB018C`** — 4 bytes, matching the single
  declared canary, in the DRAM region the linker script declared.
- **Stack top `0x3FFDC200`** — equals `ORIGIN(dram) + LENGTH(dram)`, confirming
  the linker script's arithmetic reached the running image.

The last two are the pattern this project uses everywhere: **a computed value
compared against an independently derived one.** The linker script says
`0x3FFB0000 + 0x2C200`; the running kernel prints `0x3FFDC200`. If those two ever
disagree, the image being run is not the image that was linked.

## 6.5 What M0 does not establish

Stated explicitly in the report so later work does not over-claim, and every item
came true:

**No interrupt handling.** No vector table is installed. Behaviour on any
exception or interrupt is undefined. → Chapter 7.

**No timing accuracy.** The heartbeat uses a spin loop of arbitrary length
against an unknown CPU clock. The counter proves liveness, not rate. → The clock
was derived at 80 MHz in Chapter 7 §7.6, "which also explains the apparent
slowness of the M0 spin-loop heartbeat".

**No watchdog management.** And here the report is corrected in place, which is
the most instructive entry:

> The kernel neither feeds nor disables any watchdog. That it survived 8 seconds
> suggests watchdogs are not armed at this point in boot, but this is inference,
> not measurement, and may change once interrupts are enabled.
>
> > **CORRECTED 2026-08-14.** The inference was wrong. The RTC watchdog **is**
> > armed by the second-stage bootloader, which expects the application to take
> > ownership of it. M0 survived its capture window by luck of timing, not
> > because nothing was running. Measured directly during M2: `rst:0x10
> > (RTCWDT_RTC_RESET)` on every boot once the CPU was kept busy.

An inference explicitly labelled as inference, then measured, then found wrong,
then corrected in place rather than quietly dropped. The labelling is what made
the correction cheap.

**No memory beyond the first few bytes.** DRAM and IRAM were exercised only at
their lowest addresses. → Directly relevant to the overlap question in Chapter 4
§4.7, which was settled by an experiment that deliberately spanned 24 KB.

**Single-core only.** Core 1 is untouched and in an unknown state. → Still true.
It cost exactly one debugging session (Chapter 22 §22.3).

## 6.6 The conclusion, and the number

> **Milestone 0 passes.** nat-os boots from flash on target hardware, its
> segments load where the linker script directs, the entry stub produces a
> working C environment under the call0 ABI, and execution is sustained.
>
> The kernel is 1,124 bytes of text with no ESP-IDF, Arduino, FreeRTOS, or C
> library linked. Every instruction from `0x4008000C` onward is project code.

1,124 bytes. For comparison, the present kernel is 116,692 bytes of text and
37,248 bytes of image, and the whole of the rest of this book is the growth
between those two numbers.

## 6.7 The roadmap that followed

UM-NATOS-007 defined five milestones. It is quoted here as written, because —
unusually — the report refuses to update its plan sections after the fact:

> **All five are complete**, each with a verification report carrying its own
> PASS. The plan sections below are left exactly as written at M0: they are the
> plan, and a plan rewritten after the fact to match what happened is not a
> record of anything.

| ID | Deliverable | Principal risk | Verified by |
|---|---|---|---|
| M0 | Kernel boots, self-checks pass | Link map errors | UM-NATOS-006 |
| M1 | Timer interrupt and tick counter | Vector installation; silent faults | UM-NATOS-008 |
| M2 | Two native tasks, preemptive switching | Context save correctness | UM-NATOS-009 |
| M3 | Heap allocator and VM memory model | Fragmentation; arena sizing | UM-NATOS-010 |
| M4 | Bytecode interpreter executing a program | Instruction set design | UM-NATOS-012 |
| M5 | Two VM applications time-sliced in bounded arenas | Isolation enforcement | UM-NATOS-013 |

And then the assessment of the risk column, which is the most useful sentence in
the report:

> Every principal risk in that column materialised except one. Vector
> installation did produce silent resets; context save correctness cost the
> LOOP-state defect that made every task switch to itself; isolation enforcement
> is the only one that went in cleanly first time.

Five for six is a good hit rate for a risk register, and the one that missed
missed in the good direction.

### The exit criteria

Each milestone carried numbered exit criteria, written before the work and
measured against afterwards. They are reproduced here because the discipline of
writing them first is visible in how specific they are:

**M1** — tick counter advances at a measured, stable rate; interrupted code
resumes correctly, verified by a checksum over registers across an interrupt;
system survives ≥60 seconds without reset.

**M2** — two tasks alternate for ≥10,000 switches without corruption;
register-pattern check passes on every resume; stack guard patterns intact.

**M3** — allocate/free cycles leave no leak across 10,000 iterations; arena
bounds are queryable by the interpreter; out-of-memory returns a failure rather
than corrupting.

**M4** — program computing a known result returns it correctly; out-of-bounds
access is refused and reported, not performed; interpreter yields at quantum
expiry; instructions-per-second measured and recorded.

**M5** — two applications interleave correctly; an application attempting
out-of-bounds access is terminated while the other continues unaffected;
terminating an application releases its arena completely.

Every one of those is a *number or a state that can be printed*, not a
judgement. Chapter 12 is largely about what happens when a criterion is not of
that shape.

### What came after M5, none of it planned

The roadmap's own revision 1.1 adds a table of eleven unplanned reports and then
draws two conclusions:

> **It is longer than the plan it follows.** Eleven reports of unplanned work
> against five of planned, which says the milestones were a plan for a kernel
> and what got built afterwards was a device.

> **The one structural item on it is missing.** Every driver above is reachable
> only from the kernel. The VM has twelve syscalls, all hardcoded, and no device
> model — so an application cannot read the light sensor, scan the I2C bus or
> receive a keypress. Each new peripheral has meant a kernel edit plus a
> hand-written syscall, which was tolerable at two and is the obvious next piece
> of *architecture* rather than more drivers.

That gap is now seventeen reports old and Chapter 31 argues it is the single
most valuable thing left to build.

## 6.8 Two cross-cutting items from the roadmap

**Version control — outstanding at M0.** The roadmap flagged it with a specific
reason:

> Not yet initialised. The sibling project The Word Device was lost and
> recovered only because an editor's local history happened to retain it. This
> should be resolved before M1.

It was initialised 2026-08-14, the same day, and the first commit is *"Initial
commit — cyd-os through Milestone 1"*. Appendix G is the resulting timeline, and
it is only possible because of that decision.

**JTAG probe — ordered, not in hand.**

> From M1 onward, failures are increasingly silent. UART cannot report a fault
> in the interrupt vector itself. Sequencing M1 after the probe arrives is
> recommended; if not, budget substantially more time.

M1 was not sequenced after the probe, and the extra time was duly spent — twelve
build cycles on one defect in M2 alone. But the absence also shaped the system
in ways that turned out to be valuable: the panic handler exists because there is
no debugger, the counters exist because there is no watchpoint, and Chapter 28
exists because the only instruments available were the ones this kernel built for
itself.

---

**Part I ends here.** The machine is understood, the ABI is chosen and proved,
the boot chain is traced, the memory map is placed and one false alarm in it is
disproved, the build is reproducible, and the first image has demonstrated that
every one of those things is true on the hardware rather than in principle.

**Part II** starts where M1 does: with a vector table, a comparator, and the
first interrupt this kernel ever took.
