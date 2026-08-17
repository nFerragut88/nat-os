# Chapter 20 — Persistence: A Read Defect That Looked Like the Wrong Thing

> Sources: `docs/UM-NATOS-018-persistence.md`
> Code: `kernel/flash.c`, `kernel/flash.h`, `kernel/store.c`, `kernel/store.h`, `kernel/linker.ld`

---

## 20.1 What was built

| Component | Role |
|---|---|
| `kernel/flash.c` | SPI1 user-mode read, sector erase, page program |
| `kernel/store.c` | One checksummed record: magic, version, boots, frames, the last fault, and the touch calibration |
| `kernel/linker.ld` | `.dram.rodata` extended to `flash.o` and `store.o` |

The mechanism is small. The report is mostly about a defect that sat in front of
it for the whole of the work:

> **every flash read returned the true value shifted right by one bit**,
> consistently, on every transaction. The chip ID read `0x34200B` where the part
> reports `0x684016`, which is exactly that value shifted once.

## 20.2 The record

```c
#define STORE_MAGIC   0x59444F53u      /* "SODY" little-endian */
#define STORE_VERSION 3u   /* 3 adds the touch calibration */
```

Validated whole, on every load:

```c
int store_load(void)
{
    store_t r;
    g_valid = 0;

    if (flash_read(FLASH_DATA_ADDR, &r, sizeof r) != 0) {
        goto defaults;
    }
    if (r.magic != STORE_MAGIC || r.version != STORE_VERSION) {
        goto defaults;      /* first run, or an erased sector reading 0xFF */
    }
    if (r.checksum != checksum(&r)) {
        goto defaults;      /* torn by a reset mid-write */
    }

    g_rec   = r;
    g_valid = 1;
    return 0;

defaults:
    /* ... reset every field ... */
    return -1;
}
```

> A first run and a corrupt sector are deliberately indistinguishable — both
> reset to defaults — because **a partially-believed record is worse than no
> record.**

The checksum is deliberately trivial and says so:

```c
/* Sum over every field but the checksum itself. Deliberately trivial: this
 * detects a torn or blank sector, which is what actually happens here. It is
 * not a defence against a determined corruption, and pretending otherwise would
 * be worse than the simple version. */
static uint32_t checksum(const store_t *r)
{
    return r->magic + r->version + r->boots + r->frames +
           r->fault_kind + r->fault_detail + r->fault_epc + r->fault_boot +
           r->cal_x_min + r->cal_x_max + r->cal_y_min + r->cal_y_max +
           0x9E3779B9u;
}
```

Naming what a mechanism does *not* defend against is a recurring habit in this
codebase, and it is what stops a checksum being cited later as integrity
protection.

## 20.3 A boot counter first, deliberately

```c
 * Persistence is unusually easy to *believe* you have: the value read back is
 * the value just written, whether or not it ever reached the chip. A RAM
 * variable passes that test perfectly.
 *
 * A counter that survives a power cycle cannot.
```

And the second field is stronger still:

> `frames` accumulates raycaster frames **across** boots, so it can only be
> correct if the previous value was read back correctly *and added to*. A boot
> counter proves a write reached the chip; a running total proves the read path
> too. That distinction is what made the one-bit shift visible at all — the
> counter stuck at 1 was the symptom that started the investigation.

Two fields, three distinct claims, each with its own failure mode. That is the
same design as the touch counters in Chapter 17 and the instruction/counter
cross-check in Chapter 14.

## 20.4 Where it lives, and the constant that protects the bootloader

```c
/* Bounded so a chip that never answers costs a delay rather than the system.
 * A sector erase is tens of milliseconds; this allows roughly a second. */
#define BUSY_TIMEOUT_CYCLES 80000000u
```

The data region starts at 2 MB, past the bootloader, the partition table and the
kernel image. Both `flash_erase_sector()` and `flash_write()` refuse any address
below `FLASH_DATA_ADDR`, and the reason is stated in terms of recovery cost:

> This is not defence against a subtle bug. It is defence against the specific
> failure where a wrong constant erases the bootloader and the board stops
> enumerating over USB — which turns a five-minute fix into a recovery job.
> **Every failure in this driver stays recoverable over serial.**

## 20.5 The cache hazard, closed differently than planned

UM-NATOS-011 §4 predicted this would break first, and it did — on the first
version of the driver, with a memorable presentation:

> every string literal in the kernel began printing as `0xFF` immediately after
> the first flash operation. The console filled with `??????????`.

But the cause was not the predicted one:

> It was worse and simpler — **the driver reconfigured SPI1 and walked away.**
> SPI1 shares the flash bus with the cache's SPI0, and those registers are how
> the cache issues its own reads. Leaving them altered left the cache unable to
> read flash at all.

The fix is to save and restore all controller registers — `USER`, `USER1`,
`USER2`, `CTRL`, `CTRL2`, later `CLOCK` — **unconditionally, including on the
failure path**:

> A driver that restores state only when it succeeds has simply moved the hazard
> to the error case, where it is harder to find.

### Why the cache is never disabled

The planned mitigation was to disable the cache around flash writes, as ESP-IDF
does. nat-os makes the dangerous reads impossible instead:

- all kernel code executes from IRAM, so instruction fetch never touches flash
- `flash.o`, `store.o`, `panic.o`, `uart.o` and `watchdog.o` have their
  `.rodata` placed in DRAM by the linker (Chapter 4 §4.5)
- interrupts are masked for the whole operation, so no handler can run and touch
  one either

> A cache *hit* is harmless — it never reaches the chip. Only a miss would, and
> with no flash-mapped address referenced there is nothing to miss on.

The residual risk is stated plainly, and the mitigation is structural:

> this argument depends on every function reachable from the driver keeping its
> read-only data out of flash. The linker rule selects **by object file** rather
> than by annotation precisely so that adding a string to one of these files
> cannot quietly break it. An annotation can be forgotten on a new string; a
> file-scoped rule cannot.

## 20.6 The read defect

### The symptom

```
flash id     : 0x0034200b        expected 0x00684016
store        : initialised, boot #1, frames 0, saved
store        : initialised, boot #1, frames 0, saved     ← after a reset
```

`0x684016 >> 1 == 0x34200B`. Every read, every time, shifted right one bit with a
zero entering at the top.

### Why it pointed at the command phase

A leading zero followed by the real data means the first sample was taken before
the chip drove anything. Two readings:

1. **framing** — the command phase is one clock short, so data sampling begins a
   clock early
2. **timing** — sampling is misaligned against the data by a clock

Reading 1 was pursued first, and the reason is the transferable part:

> the shift was *perfectly consistent*. Timing faults are expected to be
> marginal — to vary with temperature, or produce occasional rather than uniform
> corruption. A defect that reproduces bit-exactly on every transaction reads as
> structural.
>
> That instinct is wrong here. A sampling point misplaced by a *whole clock* is
> not marginal at all; it is as deterministic as a framing error and looks
> identical from the outside. **Consistency does not distinguish framing from
> timing. It only rules out marginality.**

### The measurement that eliminated a class

Rather than settle the encoding question, the command and address were moved out
of `USR_COMMAND`/`USR_ADDR` entirely and sent as ordinary MOSI bytes:

> A SPI NOR part cannot tell the difference; the phase split is a convenience of
> this controller, not something on the wire.

The result was **byte-for-byte identical**: `0x0034200b`.

> Two structurally different transactions producing the same shift is strong
> evidence, and it is the single most useful measurement in this report. It did
> not identify the cause, but it eliminated an entire class — the extra bit
> arrives regardless of how the command is framed, so no command-phase register
> can be responsible.

### The measurement that settled it

With framing eliminated, the remaining candidates were the sampling edge and the
clock. Both are register fields with several plausible values, and testing them
one at a time costs a build-and-flash cycle per hypothesis.

**Instead, all of them were tested in a single boot** — four clock dividers
against four sampling-edge combinations, with the known-good answer printed
alongside so the right row identifies itself:

```
flash probe  : want 0x00684016
  clk=0x00000000 edge=0x00000000 -> 0x0034200b
  clk=0x00000000 edge=0x00000040 -> 0x0034200b
  clk=0x00000000 edge=0x00000080 -> 0x00b4200b
  clk=0x00000000 edge=0x000000c0 -> 0x00b4200b
  clk=0x001c9109 edge=0x00000000 -> 0x00684016   <== MATCH
  clk=0x001c9109 edge=0x00000040 -> 0x00684016   <== MATCH
  ...
  clk=0x00009109 edge=0x000000c0 -> 0x00684016   <== MATCH
```

> `clk=0` means *inherited*. The reading is unambiguous: **every explicit divider
> reads correctly at every edge setting; the inherited one fails at all of them.**
> Sixteen measurements, one flash cycle, no interpretation required.

This is the same technique as `touchcfg` and `spiclk` in Chapter 27 §27.9, and it
is named there as a method note: *make the variable runtime-tunable before
forming a theory about it.*

The fix, with the whole history preserved in the source:

```c
/* ~8 MHz for user commands, explicitly set rather than inherited.
 *
 * This was the whole of the read defect. The bootloader leaves SPI1's divider
 * configured for its own fast cache reads, and at that rate a user transaction
 * sampled MISO one clock early: every byte came back as the true value shifted
 * right once, with a spurious leading zero. RDID read 0x34200B where the part
 * reports 0x684016.
 *
 * Two things made this hard to see. The shift was perfectly consistent, so it
 * looked like a framing error in the command phase rather than a timing one —
 * and moving the command out of USR_COMMAND into the MOSI stream changed
 * nothing at all, which is what finally ruled that out. A sweep across four
 * dividers and four sampling edges settled it in one boot: every explicit
 * divider read correctly at every edge, and the inherited one failed at all of
 * them.
 *
 * These operations are rare and small, so there is nothing to be gained by
 * [running the bus near its limit] ... */
```

There is a second inherited-state hazard in the same file, and it is documented
beside the first:

```c
/* Fast-read mode bits in SPI_CTRL. The bootloader leaves the controller in dual
 * or quad IO for cache reads; a user command issued in that mode goes out on
 * more than one line and the chip sees nonsense. */
#define CTRL_FASTRD_MODE   (1u << 13)
#define CTRL_FREAD_DUAL    (1u << 14)
/* ... */
```

Two independent ways for inherited controller state to corrupt a user
transaction, on one peripheral. The general lesson is in Chapter 29: **inherited
configuration is not neutral configuration.**

## 20.7 Two hypotheses tested against stale firmware

*This is the part of the investigation that cost the most and taught the most.*

> `build.ps1` builds; `build.ps1 -Flash` builds *and* flashes. For a stretch of
> the investigation the build step ran alone, and the results were read off a
> board still running older firmware. The invocation used was `flash.ps1` — a
> script that **does not exist**. PowerShell's error went to a stream that was
> being filtered out of the captured output.

Two conclusions were drawn from those runs:

| Hypothesis | Verdict recorded | Actually |
|---|---|---|
| `SPI_CTRL` fast-read mode bits | no change | genuinely no change |
| `SPI_CTRL2` MISO sampling delay | **disproven** | untested — never ran |

> The CTRL2 result was retested properly once the flash path was fixed. It *is*
> disproven — the conclusion was right. But it had been **right by accident**,
> and a comment in `flash.c` had already been written attributing the shift to
> CTRL2 as established fact. That comment was corrected.

The cost was not the wrong conclusion:

> during the stale window, **every hypothesis returned the same answer** — the
> board could not have responded differently, because it was running the same
> firmware throughout. A sequence of identical negative results was read as "none
> of these are the cause" when it actually meant "no experiment has run yet".

### The general form, and the standing rule

> This is the third time in this project that an instrument reported its own
> reading invalid and the reading beside it was believed anyway.
>
> The distinguishing signature is the same each time: **a run of results that do
> not vary when the input does.** Three different register configurations
> returning bit-identical output is not evidence about registers. It is evidence
> that the configuration is not reaching the device.

> **Standing rule.** A negative result is only informative if the experiment
> demonstrably ran. When consecutive hypotheses produce identical output, suspect
> the harness before the theory — and verify the change reached the target,
> rather than inferring it from the absence of an error message.

The mitigation is deliberately unglamorous:

> the flash step now always shows its `Hash of data verified.` line, and that
> line is checked before any capture is interpreted.

## 20.8 Verification

Persistence verified across hard resets, with the frame count accumulating:

```
--- run ~130 s so two saves land ---
  store  : loaded, boot #14, frames 256, saved
--- reset 1 ---
  store  : loaded, boot #15, frames 512, saved
--- reset 2 ---
  store  : loaded, boot #16, frames 512, saved
  flash id : 0x00684016
```

Three things asserted, each with a distinct failure mode:

> - **`boots` increments on every reset** — writes reach the chip
> - **`frames` grew 256 → 512 across reset 1** — the previous value was read back
>   correctly and added to; this cannot pass on a RAM variable or a broken read
> - **`frames` stayed 512 across reset 2** — the 6-second window did not reach
>   the 256-frame save point, so nothing was written. A counter that advanced
>   here would indicate a write not tied to the work it claims to measure

That third assertion is the subtle one and it is the best-designed part of the
test: a *negative* result that would have been positive if the save were firing
spuriously.

The chip ID now matches what `esptool` independently reports for the part — an
external oracle rather than an internal one, which is the pattern Chapter 27 §27.9
names as a method note.

## 20.9 The save interval was set by measurement

> The periodic save initially fired every 4096 frames, then 512, on an assumed
> ~8 fps. Neither ever fired inside a test window.
>
> The renderer actually runs at **4.4 fps** with the framebuffer on — 81 frames
> in 18.3 seconds, read from the existing telemetry. At 512 frames that is a save
> every two minutes, which is why a 75-second verification run saw nothing and
> was briefly mistaken for a broken save path.

The general point:

> **an interval nobody exercises is an interval nobody has tested.** A save path
> that fires every eight minutes would have shipped unverified, and its first
> real execution would have been in the field.

And an honest note that the constant has since drifted from its justification:

> **The save cadence figure also moved.** 256 frames was ≈ 60 s at the 4.4 fps
> measured here; after the lock contention fix the renderer runs several times
> faster, so the same 256 frames is now well under half that. The constant was
> chosen from a measured rate, and the rate changed underneath it — the flash
> sees more erases per hour than this report's wear analysis assumed.

## 20.10 The record has grown twice

Revision 1.0 described a four-field record of 20 B. It is now **13 words, 52 B,
at record version 3**:

| words | added by |
|---|---|
| magic, version, boots, frames | this chapter |
| fault kind, detail, EPC, boot | Chapter 12 §12.7 — the panic recorded to flash |
| cal x-min/max, y-min/max | Chapter 19 §19.10 — the on-device touch calibration |
| checksum | this chapter |

> Nothing broke as it grew, because the version field does exactly what it is for:
> an older record fails validation and resets to defaults rather than being read
> with the wrong layout.

With a caveat:

> **no upgrade path was ever written** — a version bump silently discards the
> boot counter, the frame total and the calibration, which is why the panel had
> to be recalibrated after the field was added.

### One writer, by construction

The calibration is persisted through a function that lives in `store.c` rather
than in `calib.c`, and the reason is a discipline argument:

```c
/* Declared in calib.h. Writing the record here keeps every write to it in one
 * file, which is what makes "the record is only changed in store.c" a property
 * rather than a habit. */
void calib_persist(uint32_t xmin, uint32_t xmax, uint32_t ymin, uint32_t ymax)
{
    g_rec.cal_x_min = xmin;
    /* ... */
    (void)store_save();
}
```

Same for the boot counter:

```c
/* Bumped by kmain once the record has been loaded. Kept here so the only code
 * that writes the counter is the code that owns it. */
void store_count_boot(void) { g_rec.boots++; }
```

This is Chapter 7 §7.9's standing rule applied prospectively: **one writer, or
every writer maintains the shadow.** Here, one writer, enforced by where the
function lives.

## 20.11 Load order in `kmain`

```c
    /* Persistence. Loaded before the scheduler starts so the boot count is
     * settled before anything can race it, and saved immediately so a power
     * cut a second later still records that this boot happened. */
    extern void store_count_boot(void);
#if FLASH_ENABLE
    uart_puts("  flash id     : ");
    uart_put_hex(flash_read_id());
    uart_puts("\n  store        : ");
    int found = (store_load() == 0);
    store_count_boot();
    int saved = (store_save() == 0);
    /* ... */
```

The chip ID is printed first, every boot. If the read path ever regresses, the
first line says so before any record is interpreted.

Then the calibration is restored before anything can be touched, and the
last-fault line is printed (Chapter 12 §12.7).

## 20.12 Metrics

| Quantity | Value |
|---|---|
| Record size | 52 B, record version 3 |
| Sector used | 4,096 B at `0x200000` |
| Flash user clock | ~8 MHz (80 MHz / 1 / 10) |
| Bootloader clock (inherited) | fast cache-read divider — **unusable for user commands** |
| Max transaction | 64 B; page program capped at 60 B + 4 B header |
| Save cadence | every 256 frames |
| Erase cost | tens of ms, interrupts masked |
| Registers saved/restored per transaction | 6 |
| Boots survived in test | 16 |
| Hypotheses tested against stale firmware | 2 |
| Chip ID | `0x684016`, matching `esptool` |

## 20.13 What this does not establish

- **No wear levelling.** One sector, erased and rewritten in place.
- **No power-cut testing.** The checksum is *designed* to reject a torn record;
  no test has interrupted a write mid-erase to confirm the rejection path
  executes. "The mechanism is reasoned, not measured."
- **No application access.** Persistence is kernel-only. No syscall exposes it.
- **No filesystem.** One fixed-layout record, not named storage.
- **No verification against a second board.** The clock finding is from one part.
  A different flash chip may tolerate the inherited divider, "which would make
  this defect appear board-specific to anyone who hits it".
- **The cache argument remains an argument.** §20.5 reasons that no flash-mapped
  read can occur during an operation. That has not been instrumented — there is
  no assertion that would fire if a future edit broke it.

---

**Next:** the storage a person can carry away, and a pad table that is not in pin
order.
