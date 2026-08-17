# Chapter 4 — The Memory Map, and an Overlap That Was Not There

> Sources: `docs/UM-NATOS-004-memory-map.md`, `docs/UM-NATOS-011-flash-cache.md`
> Code: `kernel/linker.ld`

---

## 4.1 The regions

The ESP32 has 520 KB of internal SRAM in three blocks, some visible at more than
one address depending on whether it is accessed as instruction or data memory.

| Block | Size | Data (DRAM) view | Instruction (IRAM) view |
|---|---|---|---|
| SRAM0 | 192 KB | — | `0x40070000`–`0x400A0000` |
| SRAM1 | 128 KB | `0x3FFE0000`–`0x40000000` | `0x400A0000`–`0x400C0000` |
| SRAM2 | 200 KB | `0x3FFAE000`–`0x3FFE0000` | — |

External flash is additionally mapped through the cache:

| Purpose | Address base |
|---|---|
| Flash instruction (execute in place) | `0x400D0000` |
| Flash read-only data | `0x3F400000` |

These are **transcribed**, not verified, and the report says so explicitly:

> These boundaries are transcribed from the ESP32 Technical Reference Manual's
> system address mapping and should be checked against it before being relied on
> for anything beyond the current layout. The addresses nat-os actually uses are
> confirmed working on hardware; the surrounding region boundaries are not
> independently verified.

That distinction is the whole discipline of Chapter 0b applied to an address
table, and it matters: three separate defects in this project came from a
constant recalled rather than fetched (Chapters 22 and 23).

### Two properties that constrain everything

**IRAM requires aligned 32-bit access.** Byte or unaligned halfword loads from
IRAM fault. Read-only data — notably string literals, which string handling
touches unaligned constantly — must therefore live in DRAM or flash, not IRAM,
even though it is logically part of the program image.

**Part of SRAM0 serves as instruction cache.** When flash execute-in-place is
enabled, a portion of SRAM0 is consumed and unavailable as IRAM. nat-os does not
execute from flash, so this does not currently apply, and it is listed as
unmeasured in §4.7.

## 4.2 The layout, as declared

```
MEMORY
{
  /* Internal SRAM0 — instruction RAM. */
  iram (rwx) : ORIGIN = 0x40080000, LENGTH = 0x20000   /* 128 KB */

  /* Internal SRAM2 — data RAM, below the ROM's reserved region. */
  dram (rw)  : ORIGIN = 0x3FFB0000, LENGTH = 0x2C200   /* ~176 KB */

  drom (r)   : ORIGIN = 0x3F400020, LENGTH = 0x400000 - 0x20
}
```

| Section | Region | Rationale |
|---|---|---|
| `.vectors` | IRAM, first | `VECBASE` must be 1024-byte aligned |
| `.text` (code + literals) | IRAM | Executable |
| `.dram.rodata` | DRAM | Must survive a disabled flash cache — §4.5 |
| `.flash.rodata` | DROM | Everything else read-only |
| `.data` | DRAM | Writable initialised data |
| `.bss` | DRAM | Zeroed by `start.S`; `NOLOAD`, so it costs no image bytes |
| Heap | DRAM | Between `.bss` and the boot stack reservation |
| Stack | DRAM, top-down from `_stack_top` | `ORIGIN + LENGTH` = `0x3FFDC200` |

### Why DRAM starts at `0x3FFB0000` and not `0x3FFAE000`

SRAM2 begins 8 KB lower. The origin is set higher because the upper region of
DRAM (above roughly `0x3FFE0000`) is used by the ROM for its own stack and data
during boot; starting low and ending below that region avoids being overwritten
while the bootloader is still running. The chosen length places the top at
`0x3FFDC200`, comfortably clear.

Confirmed on hardware at M0: `.bss` spanning `0x3FFB0188`–`0x3FFB018C`, stack top
`0x3FFDC200`, executing code observed at `0x40080088`.

> **Stack top `0x3FFDC200`** — equals `ORIGIN(dram) + LENGTH(dram)` =
> `0x3FFB0000 + 0x2C200`, confirming the linker script's arithmetic reached the
> running image.

That is a small thing and it is a real check. A linker script can be arithmetically
correct and still not be the script that was used.

## 4.3 The vectors section

The CPU dispatches exceptions and interrupts to fixed offsets from `VECBASE`, so
slots are placed by *absolute position* in the linker script, not by declaration
order:

```
  .vectors : ALIGN(1024)
  {
    _vecbase = .;

    . = _vecbase + 0x000;
    KEEP(*(.vectors.window.of4))
    . = _vecbase + 0x040;
    KEEP(*(.vectors.window.uf4))
    . = _vecbase + 0x080;
    KEEP(*(.vectors.window.of8))
    . = _vecbase + 0x0C0;
    KEEP(*(.vectors.window.uf8))
    . = _vecbase + 0x100;
    KEEP(*(.vectors.window.of12))
    . = _vecbase + 0x140;
    KEEP(*(.vectors.window.uf12))

    . = _vecbase + 0x1C0;
    KEEP(*(.vectors.level3))

    . = _vecbase + 0x300;
    KEEP(*(.vectors.kernel))

    . = _vecbase + 0x340;
    KEEP(*(.vectors.user))

    . = _vecbase + 0x3C0;
    KEEP(*(.vectors.double))

    . = _vecbase + 0x400;
  } > iram
```

The architectural offsets, from the script's own comment:

```
 *   0x180 level 2      0x1C0 level 3      0x200 level 4
 *   0x240 level 5      0x280 debug        0x2C0 NMI
 *   0x300 kernel exc   0x340 user exc     0x3C0 double exc
```

Only the slots the kernel uses are populated; the rest are left as gap. A stray
dispatch into a gap is a bug, and the resulting fault is caught by the panic
vectors — which is the design intent stated in the script.

The window slots at `0x000`–`0x180` were empty until Chapter 2 §2.7, and the
linker script now records the correction:

```
     * These were previously left EMPTY — the section jumped straight to 0x1C0,
     * so a window exception executed zero-filled space. Nothing in this kernel
     * takes them (it is -mabi=call0 throughout), but PS.WOE is set by the ROM,
     * so the mechanism was armed with no handlers behind it.
```

### Verified before flashing, not after

A misplaced vector produces a silent reset with no output, which is close to
undiagnosable without a debugger. UM-NATOS-008 §4.1 records the pre-flight check
performed on the linked ELF using `nm`:

```
_vecbase = 0x40080000   1024-aligned: True

symbol             address      offset   expected
_vector_level3     0x400801C0   0x01C0   OK
_vector_kernel     0x40080300   0x0300   OK
_vector_user       0x40080340   0x0340   OK
_vector_double     0x400803C0   0x03C0   OK

_handler_level3 = 0x40080924   (1892 bytes from vector; j range ~128KB)
```

Each slot is 64 bytes and contains a single `j` to a handler in `.text`. A jump
rather than an address load, because `j` has ±128 KB range — sufficient for the
whole kernel — and avoids needing a literal pool inside a fixed-offset slot.

## 4.4 Flash-mapped read-only data

`.rodata` originally lived in DRAM because IRAM cannot serve unaligned reads.
That constraint is specific to IRAM. Flash mapped through the *data* cache has
no such restriction, so the reasoning that forced the original placement simply
does not carry over.

Moving it required **zero lines of C**. The change is entirely in the link map,
because the borrowed bootloader already does the hard part: it recognises load
addresses in the flash-mapped ranges, programs the flash MMU to point at the
image data in place, enables the cache, and only then jumps.

### The `0x20` that matters

```
  drom (r)   : ORIGIN = 0x3F400020, LENGTH = 0x400000 - 0x20
```

The linker script explains the offset at length, because getting it wrong
produces an image that links, flashes, and then reads garbage:

```
   * The 0x20 offset is not arbitrary. The MMU maps in 64 KB pages, and the
   * mapped page begins at the start of the image — which is occupied by the
   * 24-byte image header plus an 8-byte segment header. Starting the region
   * 32 bytes in makes the virtual address agree with the flash offset, which is
   * the congruence esptool and the bootloader both assume.
```

Confirmed in the generated image: `file_offs` reports each segment's *header*, so
the DROM payload begins at `0x20`. Flashed at `0x10000`, the data sits at
`0x10020` and maps to `0x3f400020`. The congruence holds.

### Literal pools deliberately stay in IRAM

```
   * Literal pools are deliberately NOT moved. `-mtext-section-literals` keeps
   * them beside the code in IRAM because `l32r` addresses backwards within
   * 256 KB of the PC; a literal in flash would be out of range and the link
   * would fail — or worse, not fail.
```

"or worse, not fail" is the operative phrase, and UM-NATOS-011 §2.2 expands:

> This would most likely fail at link time — but "most likely" is not
> "certainly", and a literal silently resolving to the wrong address is a
> substantially worse outcome than a failed link.

### The measurement

| Check | Before | After |
|---|---|---|
| `.rodata` location | DRAM `0x3ffb0000` | flash, mapped `0x3f400020` |
| `.bss` start | `0x3ffb04f0` | `0x3ffb0010` |
| Heap usable | 166,432 B | **167,680 B** |
| Image size | 6,720 B | 6,736 B |

> The heap grew by 1,248 bytes — **exactly** the size of the section that moved.
> An inexact match would have indicated something else shifting at the same time.

Image size grew by 16 bytes: one additional segment header plus alignment. The
`.rodata` bytes were always in the image; only their destination changed.

The change is also close to self-verifying:

> every string the kernel prints is `.rodata`. If the mapping were wrong, the
> banner would be garbage or the board would fault before producing output.

1,248 bytes is 0.7% of the heap and was never the point. The point was what it
unblocks: fonts, sprites and UI bitmaps at zero DRAM cost, and — eventually —
application bytecode executing in place so an arena holds only an application's
*data*. The first of those arrived immediately; the 5×8 font is 475 bytes of
`.rodata` and costs zero DRAM, which is why the flash-cache work was scheduled
before the display driver rather than after.

## 4.5 The section that must not be in flash

Mapping `.rodata` into flash introduced a hazard, and UM-NATOS-011 §4 predicted
it in writing before anything triggered it:

> **Cache-off windows become hazardous.** Any future code that writes flash must
> disable the cache while doing so. During that window, *any* access to
> `.rodata` faults — including a string in an interrupt handler, and including
> the panic handler's own messages. nat-os has no equivalent [of `IRAM_ATTR`]
> yet and does not need one until it writes flash, but the panic path is the
> case to fix first: a fault handler that cannot print because the cache is off
> is worse than no fault handler.

The prediction came due in Chapter 20, and the answer is the `.dram.rodata`
section:

```
  /* Read-only data that must survive a disabled flash cache.
   *
   * UM-NATOS-011 §4 records the hazard: writing flash requires the cache to be
   * turned off, and while it is off EVERY access to flash-mapped .rodata
   * faults. That includes the panic handler's own message strings — a fault
   * during a flash write would reach a panic handler that then faults trying to
   * describe itself, which is the worst failure mode available to a kernel with
   * no debugger.
   *
   * Selecting by object file rather than by attributing individual strings:
   * string literals are awkward to place by hand, and a rule that catches an
   * object's rodata wholesale cannot be defeated by someone adding a new
   * message later and forgetting the annotation. */
  .dram.rodata : ALIGN(4)
  {
    *panic.o(.rodata .rodata.*)
    *uart.o(.rodata .rodata.*)
    *watchdog.o(.rodata .rodata.*)

    /* flash.o and store.o run with the flash chip busy: any flash-mapped read
     * here would hit a chip that cannot answer. */
    *flash.o(.rodata .rodata.*)
    *store.o(.rodata .rodata.*)
    . = ALIGN(4);
  } > dram
```

The by-object-file selection is the interesting part, and UM-NATOS-018 §3.1
argues it explicitly:

> The linker rule selects **by object file** rather than by annotation precisely
> so that adding a string to one of these files cannot quietly break it. An
> annotation can be forgotten on a new string; a file-scoped rule cannot.

Note also what nat-os does *not* do. ESP-IDF disables the cache around flash
writes. nat-os never disables it, and instead makes the dangerous reads
impossible:

- all kernel code executes from IRAM, so instruction fetch never touches flash
- the five files above have their `.rodata` in DRAM
- interrupts are masked for the whole operation, so no handler can run

> A cache *hit* is harmless — it never reaches the chip. Only a miss would, and
> with no flash-mapped address referenced there is nothing to miss on.

The residual risk is stated rather than hidden: this argument depends on every
function reachable from the driver keeping its read-only data out of flash, and
nothing instruments it. It is listed in Chapter 30.

## 4.6 The heap, placed by the linker

```
  /* Kernel stack grows down from the top of DRAM. */
  _stack_top = ORIGIN(dram) + LENGTH(dram);

  /* Boot stack reservation. ... 4 KB is far more than the boot path
   * uses and costs 2% of DRAM. */
  _boot_stack_size = 4K;

  /* Heap: everything between the end of .bss and the boot stack reservation.
   * Placed by the linker rather than sized by hand so it absorbs changes in
   * .bss automatically — a fixed heap base silently shrinks when a static
   * array grows, which is the sort of thing that shows up as a mysterious
   * allocation failure much later. */
  _heap_start = ALIGN(_bss_end, 8);
  _heap_end   = _stack_top - _boot_stack_size;
```

Two decisions, both argued in UM-NATOS-010 §2.4.

**The heap is placed, not sized.** A hand-chosen base would silently shrink the
heap whenever a static array grew, and surface much later as an allocation
failure with no obvious connection to the change that caused it.

**The boot stack reservation is permanent** even though the boot context is
abandoned at handoff, "because the heap must not be adjacent to a stack that grew
further than expected before that point".

## 4.7 The overlap that was not there

This section is about a risk that was raised as "high severity, blocks M1", was
wrong, and produced a rule.

### The observation

The measured boot trace shows the bootloader loading its own segments:

```
load:0x3fff0030,len:1184
load:0x40078000,len:13232
load:0x40080400,len:3028
```

The third lands at **`0x40080400`** — inside the region nat-os declares as its
IRAM (`0x40080000`, length `0x20000`). At M0 the kernel's `.text` occupied
`0x40080000`–`0x400802E0`, ending **288 bytes short** of it. M1 would cross it.

### Why it is harmless

The ESP-IDF bootloader links itself into **two** IRAM segments:

```
iram_loader_seg : org = 0x40078000, len = 0x8000   /* 32KB, APP CPU cache */
iram_seg        : org = 0x40080400, len = 0xfc00
```

| Segment | Receives |
|---|---|
| `iram_loader_seg` @ `0x40078000` | `.iram1.*`, the loader support code — **the code that copies the application** |
| `iram_seg` @ `0x40080400` | `.entry.text`, `.init` — early startup only |

The bootloader's own commentary states the intent:

> "The main purpose is to make sure the bootloader can load into main memory
> without overwriting itself."
>
> "IRAM POOL1, used for APP CPU cache. Bootloader runs from here during the
> final stage of loading the app because APP CPU is still held in reset."

By the time application segments are copied, execution has moved to
`iram_loader_seg`. The region at `0x40080400` holds initialisation code that has
already completed and is *expected* to be overwritten.

### Documentation was not treated as sufficient

A kernel was built with ~24 KB forced into `.text` so its IRAM segment spanned
well past the disputed boundary, with sentinel words at both ends:

| | |
|---|---|
| Image size | 26,048 bytes (vs 1,216 baseline) |
| IRAM span | `0x4008025C` … `0x40086258` — crosses `0x40080400` by ~24 KB |
| First sentinel | intact |
| Last sentinel | intact |
| Boot | normal; all M0 assertions still pass; heartbeat advancing |

**The entire span was copied intact and the kernel ran normally.**

### The corrected guidance, and the rule

- The IRAM origin of `0x40080000` is **correct** and should not be changed.
- Moving it to `0x40088000` — the previously recommended "cheap mitigation" —
  would have discarded 32 KB of IRAM to solve a problem that does not exist.
- **M1 is not blocked by this.**

The report is candid about how the wrong conclusion was reached:

> The risk was raised from the boot trace alone: an address in the log fell
> inside a region the linker script claimed, and the conclusion was drawn from
> geometry without consulting the bootloader's design. The lesson for later
> milestones is that an apparent conflict in an address map is a prompt to read
> the other party's link script, not to move our own.

This is the same shape as three later defects: a constant recalled instead of
fetched (Chapter 22 §22.4), a register field remembered instead of read
(Chapter 22 §22.3), and a window handler that could have been derived and was
copied instead (Chapter 2 §2.7). In every case, going to the source of truth was
cheaper than reasoning around it.

## 4.8 Unverified assumptions

Carried forward from UM-NATOS-004 §6, and still open:

| Assumption | Status |
|---|---|
| DRAM above `0x3FFE0000` is ROM-reserved | Believed; not independently confirmed |
| IRAM window `0x40080000`+`0x20000` is fully available | Confirmed by §4.7's experiment |
| SRAM region boundaries in §4.1 | Transcribed, not verified against silicon |
| Cache consumption of SRAM0 when XIP is enabled | Not measured — relevant from the first flash-resident code build |

One more, added later and worth its own line because it was a *constraint that
turned out not to exist*. UM-NATOS-028 §3 records that the WiFi work had been
shaped for weeks by a belief that referencing libphy would cost 48 KB of IRAM:

> Referencing those four functions cost **2,459 bytes** (116,692 → 119,151 text
> against 131,072 of IRAM), not the 48 KB this project has warned about since
> `MAC-NEXT.md`. That figure came from a `--whole-archive` measurement, which
> pulls every object; a real link pulls only what is referenced. A constraint
> that had been shaping decisions for weeks simply did not exist.

---

**Next:** how the source becomes the image whose segments this chapter placed.
