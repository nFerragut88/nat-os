# Chapter 3 — Boot: From Silicon to `_start`

> Sources: `docs/UM-NATOS-002-boot-chain.md`, `docs/UM-NATOS-006-m0-verification.md`, `docs/UM-NATOS-035-the-last-borrowed-thing.md`
> Code: `kernel/start.S`, `kernel/linker.ld`, `boot/boot.c`, `vendor/README.md`, `build.ps1`

---

## 3.1 Four stages

```
   power on
       │
       ▼
┌──────────────────┐
│ L0  ROM loader   │  mask ROM, in silicon at reset vector
│                  │  reads flash @ 0x1000
└────────┬─────────┘
         ▼
┌──────────────────┐
│ L1  2nd stage    │  17,536 bytes, borrowed
│                  │  reads partition table @ 0x8000
│                  │  finds app partition   @ 0x10000
│                  │  parses image header, copies segments
└────────┬─────────┘
         ▼
┌──────────────────┐
│ L2  nat-os       │  entry @ 0x4008000C  ← our first instruction
│     start.S      │  stack, .bss clear, call0 kmain
└──────────────────┘
```

> That is the chain as it stood for the whole of this book's narrative, and
> every boot trace quoted in it. **L1 is now this project's own**, 2,736 bytes,
> and it reads no partition table; §3.1.1 has the current chain and what
> changing it cost. The stages, the interface and the image format below are
> unchanged, which is the point of having had an interface.

### Stage L0 — mask ROM

Executes from ROM on reset. Determines boot mode from strapping pins (GPIO0 high
= normal flash boot), configures the SPI flash interface, and loads the
second-stage bootloader from flash offset `0x1000`. It also brings up UART0 at
115200 baud for its own diagnostic output — a side effect nat-os currently
depends on.

Not modifiable.

### Stage L1 — second-stage bootloader

Borrowed. Its responsibilities, as they matter to nat-os:

1. Read the partition table at `0x8000`.
2. Locate the application partition (offset `0x10000`).
3. Read the image header at that offset.
4. Copy each declared segment to its declared load address.
5. Verify the image checksum and SHA-256 hash.
6. Jump to the declared entry point.

Plus one thing not in that list that became load-bearing later: it recognises
load addresses in the flash-mapped ranges, programs the flash MMU to point at
the image data in place, and enables the cache *before* jumping. Chapter 4 §4.4
depends entirely on this.

> **The interface between L1 and nat-os is entirely the image header.** Nothing
> else is shared — no symbols, no runtime, no calling convention. This is why the
> bootloader is replaceable without touching kernel code.

`vendor/README.md` records what the binaries are and how to reproduce them:

```markdown
| file | size | sha256 (first 32) |
|---|---|---|
| `bootloader.bin` | 17536 B | `3d234a7471f67b013686dabd4dee7c1f` |
| `partitions.bin` | 3072 B | `6a88d59601a83a16a19a08114b59d338` |
```

They are unmodified ESP-IDF build artefacts, Apache 2.0, redistributed under that
licence rather than under nat-os's MIT. Reproducing them needs a stock
`esp32dev` PlatformIO project and nothing else:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
```

They were originally read from an unrelated project's build directory, which
UM-NATOS-007 §8.4 flagged as a coupling worth removing — "deleting or rebuilding
that project breaks this one's flash step". They now live in `vendor/`, and
`-Vendor <path>` overrides.

### 3.1.1 Since written — L1 is ours

**Everything above describes the boot chain as it stood for the whole of this
book's narrative. It is no longer the default chain.** UM-NATOS-035 replaced the
second stage; the account here is the mechanical difference, and Chapter 1 §1.2
carries what it cost.

```
┌──────────────────┐
│ L1  2nd stage    │  2,736 bytes, OURS — boot/boot.c, boot/boot_start.S
│                  │  reads NO partition table
│                  │  image at a hardcoded 0x10000
│                  │  parses image header, copies segments, maps DROM
└──────────────────┘
```

The job was never large, and stating it as a specification is most of the
report:

```
Segment 1  DROM  0x3f400020  0x05530   flash-mapped -- needs the MMU
Segment 2  DRAM  0x3ffb0000  0x00308   copy
Segment 3  DRAM  0x3ffb0308  0x0009c   copy
Segment 4  IRAM  0x40080000  0x0de28   copy
Entry             0x4008040c
```

Three copies and one address-space mapping. Every constant it needs is
published — the header format above, the flash MMU table at `0x3FF10000`, the
DROM window and its entry index in bits 21:16, `DPORT_PRO_CACHE_CTRL_REG` — and
the one thing that is usually hard, *reading flash before anything is
initialised*, was already this project's own code: `kernel/flash.c` has driven
SPI1 at register level with no ROM calls since UM-NATOS-018. It is compiled into
the loader unmodified.

> **A file that depends only on hardware is portable to contexts its author
> never imagined.** `flash.c` was written for a persistent store and ran, without
> a line changed, inside a program that executes before the kernel exists —
> because it never depended on the kernel existing. That property was not
> designed for; it fell out of the constraint that it use no ROM routines.

Four differences from the borrowed loader matter to a reader of this chapter:

| | Espressif's | ours |
|---|---|---|
| size | 17,536 B | **2,736 B** |
| partition table | parsed | **not read** — `APP_OFFSET` is `0x10000` |
| image checks | checksum + SHA-256 + secure boot + OTA + anti-rollback | magic byte `0xE9`, and nothing else |
| clock | switches to the 320 MHz PLL, divides to 80 | **does not touch it** — see Ch. 1 §1.2 |

The 6× size difference is not bloat on Espressif's side; it is generality. A
loader that must serve every ESP32 product parses a table, verifies a hash,
selects an OTA slot and honours flash encryption. This one has a single image at
a fixed offset and needs none of it.

**One defect, found in eleven seconds.** IRAM answers aligned 32-bit accesses
only, and `flash.c` reassembles the SPI FIFO a byte at a time — correct for
every caller the kernel has, illegal for this one. The first flash died with
`LoadStoreError`, `excvaddr=0x40080000`, which is the IRAM segment's own load
address. The hardware named the cause outright. `flash.c` is unchanged;
`copy_to_iram()` reads into a 1 KB DRAM bounce buffer and copies across in
words.

That contrast is why the report exists. This project has spent whole sessions on
faults where six instruments reported success and only the glass disagreed
(Ch. 28c). This one announced itself, at an address that named the mechanism,
because the architecture can detect the violation. **Faults the architecture can
detect are cheap; faults detectable only as an absent effect are expensive** —
independently of how simple the bug is.

**Recovery, and why the borrowed binary stays in the tree.** A broken bootloader
stops the board from starting, and it is not a brick: ROM download mode is
reached by holding GPIO0 low at reset and depends on nothing in flash.

```powershell
.\build.ps1 -Flash                     # ours
.\build.ps1 -Flash -VendorBootloader   # Espressif's — recovery, and the A/B control
```

That A/B path was kept working and tested on the argument that *the day it is
needed is not the day to be debugging it*. It was needed four hours later, and it
settled the half-speed clock defect in a single flash (Ch. 1 §1.2).

## 3.2 The image format

`esptool elf2image` converts the linked ELF into the format L1 expects. The
header declares, for each segment, a length and a load address, plus one entry
point for the whole image.

The M0 build:

```
Image version:  1
Entry point:    0x4008000C
2 segments
  Segment 1: len 0x00188  load 0x3FFB0000  file_offs 0x018  [BYTE_ACCESSIBLE, DRAM]
  Segment 2: len 0x002E0  load 0x40080000  file_offs 0x1A8  [IRAM]
Checksum:         0x8F (valid)
Validation hash:  8e3a272a…5c72f3 (valid)
```

Both load addresses correspond directly to `MEMORY` regions in
`kernel/linker.ld`. After `.rodata` moved to flash (Chapter 4), the same build
produces three:

```
Segment 1: len 0x004e0 load 0x3f400020 file_offs 0x00000018 [DROM]
Segment 2: len 0x00004 load 0x3ffb0000 file_offs 0x00000500 [BYTE_ACCESSIBLE,DRAM]
Segment 3: len 0x01510 load 0x40080000 file_offs 0x0000050c [IRAM]
```

> **There is no magic in this format.** It is a list of "copy N bytes to address
> A", followed by "jump to address E". Any producer that emits a conforming
> header will boot.

### Why the entry point is `+0x0C`

The entry point is `_start + 0x0C` rather than `+0x00` because `-mtext-section-literals`
places the literal pool at the head of the section, and the first 12 bytes are
literals. Xtensa loads large constants through a literal pool referenced by
`l32r`.

This produces a disassembly artefact worth remembering when reading a fault
address:

> `objdump` cannot distinguish literals from instructions, so disassembling from
> `0x40080000` shows plausible but meaningless instructions before the real code.

## 3.3 The entry stub, in full

`kernel/start.S` is 44 lines and does three things.

```asm
    .section .text.start, "ax"
    .align 4
    .global _start
    .type _start, @function

_start:
    /* Stack first — nothing below this line may call C. */
    movi    a1, _stack_top
    /* Xtensa wants the stack 16-byte aligned. */
    movi    a2, ~15
    and     a1, a1, a2

    /* Zero .bss. The bootloader only copies segments that have file content,
     * so uninitialised data arrives as whatever was in RAM at reset. */
    movi    a2, _bss_start
    movi    a3, _bss_end
    movi    a4, 0
.Lbss_loop:
    bgeu    a2, a3, .Lbss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .Lbss_loop
.Lbss_done:

    /* Into C. kmain must not return. */
    call0   kmain

    /* If it does return anyway, park rather than execute whatever follows. */
.Lhang:
    j       .Lhang

    .size _start, . - _start
```

Three observations.

**The stack comes first and the comment says why.** "Nothing below this line may
call C" is an executable constraint, not a style note: the `.bss` clear is
written in assembly rather than C precisely because C requires a stack.

**`.bss` is zeroed here because nobody else will.** It is declared `NOLOAD` in
the linker script, so it costs no image bytes and arrives as whatever was in RAM
at reset. M0's third assertion tests exactly this.

**The final `j .Lhang` is defensive.** `kmain` is documented as never returning,
and if it does anyway, parking is better than executing whatever the linker
happened to place next.

`_start` is placed first in `.text` deliberately:

```
  .text : ALIGN(4)
  {
    /* Entry stub first so the image entry point is predictable. */
    KEEP(*(.text.start))
```

## 3.4 The measured boot trace

Captured 2026-08-14 from the target board on COM5 at 115200 baud, with the
serial port opened *before* reset so no output was missed:

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
```

### Reading it

| Line | Meaning |
|---|---|
| `ets Jul 29 2019` | ROM build date — identifies the silicon revision's ROM |
| `rst:0x1 (POWERON_RESET)` | Clean cold boot. **Diagnostically important:** a watchdog or panic reset shows a different code, and is the first thing to check when a kernel change stops booting |
| `boot:0x13 (SPI_FAST_FLASH_BOOT)` | Normal flash boot; GPIO0 was high |
| `mode:DIO, clock div:2` | Flash interface configuration |
| `load:0x…` ×3 | **The bootloader loading itself**, not nat-os |
| `entry 0x400805e4` | Entry into the *bootloader*, not our kernel |

That last pair is the trap:

> The three `load:` lines and the `entry` line are frequently misread as the
> kernel loading. They are not. They are L1 placing its own segments before it
> has even read our partition. Our own load is silent — L1 does not log it — and
> the first evidence of nat-os is the banner.

Misreading them cost this project a false alarm; Chapter 4 §4.5 is that story.

### What the banner alone proves

The banner appearing at all proves the header was well-formed, the checksum
matched, both segments were copied, and the CPU reached `0x4008000C`. That is
four independent things, and it is also the ceiling of what a banner can prove —
a point Chapter 12 makes at length after the shell was signed off on the same
evidence and turned out to have a broken receive path.

## 3.5 Flash layout

| Offset | Contents | Size | Origin |
|---|---|---|---|
| `0x1000` | Second-stage bootloader | 2,736 B (17,536 B borrowed, with `-VendorBootloader`) | **Ours** — §3.1.1 |
| `0x8000` | Partition table | 3,072 B | Borrowed, and **read by nothing** |
| `0x10000` | nat-os kernel image | 1,216 B at M0, 37,248 B now | **Ours** |
| `0x200000` | Persistent record sector | 4,096 B | **Ours** (Chapter 20) |
| `0x201000` | Message store sector | 4,096 B | **Ours** (Chapter 26) |

Everything from `0x10000` onward is available to the project.

The partition table is the last borrowed thing in the layout, and it is borrowed
in a weak sense: `boot.c` hardcodes `APP_OFFSET 0x10000`, on the grounds that a
kernel with exactly one image does not need a table to find it, so nothing in
this boot chain reads those 3,072 bytes. They are still flashed because esptool
and external tooling expect a table to be there, and 3 KB is cheaper than the
surprise. Generating one is a `struct` and a CRC; it has not been done because
doing it would change nothing that runs.

> That is worth stating precisely rather than rounding off in either direction.
> "nat-os borrows a partition table" and "nat-os's boot chain is entirely its own
> code" are both true, and they are about different things: the first is about
> what is written to flash, the second about what executes.

The 2 MB offset for data is not arbitrary. `flash.c` refuses any erase or write
below `FLASH_DATA_ADDR`, and UM-NATOS-018 §2.1 gives the reason:

> This is not defence against a subtle bug. It is defence against the specific
> failure where a wrong constant erases the bootloader and the board stops
> enumerating over USB — which turns a five-minute fix into a recovery job. Every
> failure in this driver stays recoverable over serial.

## 3.6 Capturing boot output, and the harness that lied

Attaching a monitor *after* reset loses the banner, because the kernel prints
within milliseconds of the jump. The capture harness opens the port first and
then pulses the auto-reset circuit:

- `RTS` → `EN` (reset), asserted then released
- `DTR` → `GPIO0` (boot mode), held high for normal flash boot

Sequence: open port → `DTR=False`, `RTS=True` → 150 ms → `RTS=False` → read.

There is a trap here that this project fell into twice, and the second time was
by someone who had just read the write-up of the first time. From
`README.md`:

> Note that opening a serial port on Windows asserts DTR, which resets the
> board; deassert `dtr` and `rts` *before* `open()`. UM-NATOS-017 §5 is the story
> of learning that twice.

The symptom was three consecutive runs, across different builds and different
user actions, reporting **exactly 150 samples**.

> A counter that lands on an identical value every time is not accumulating. It
> is measuring a fixed window.

Every capture described as "no reset" was rebooting the board, waiting for it to
boot, and then reading roughly twelve seconds of a system nobody was touching.
Setting `dtr` and `rts` before `open()` fixed it; the same read then reported
647,406 samples and no boot banner.

Two published findings were retracted as a result, and the fix that mattered was
not the code change but the *verification*:

> What is new is the verification — uptime is now sampled twice across a
> connection and confirmed to increase, rather than the absence of a reset being
> assumed. Knowing the failure mode was not enough; a check was needed.

Chapter 28 catalogues this alongside its six siblings.

## 3.7 Failure modes, and the first diagnostic step

| Symptom | Most likely cause |
|---|---|
| No output at all | Image checksum invalid, or entry point outside a loaded segment |
| ROM trace then silence | Kernel reached but faulted before UART output — suspect stack or `.bss` |
| Boot loop with `rst:0x3` / `rst:0xc` / `rst:0x10` | Watchdog — kernel hung before feeding or disabling it |
| Boot loop with `rst:0x7` | TIMG0 watchdog — the hang detector fired (Chapter 12) |
| Garbled output | Baud mismatch |

> In every case the **reset reason on the first line** is the highest-value
> single datum, which is why the capture procedure resets the board explicitly
> rather than attaching to an already-running system.

`rst:0x10 (RTCWDT_RTC_RESET)` deserves special mention because it was misread for
three milestones. UM-NATOS-006 §6 originally reasoned that surviving an
8-second capture window implied no watchdog was armed. That inference was wrong
and is corrected in place in the report:

> **CORRECTED 2026-08-14.** The inference was wrong. The RTC watchdog **is**
> armed by the second-stage bootloader, which expects the application to take
> ownership of it. M0 survived its capture window by luck of timing, not because
> nothing was running.

Chapter 12 covers what the kernel does about it now.

## 3.8 Reproduction

```powershell
cd C:\src\nat-os
.\build.ps1 -Flash -Port COM5
# then capture with the port opened before reset
```

One caution, learned expensively and recorded as a standing rule
(UM-NATOS-018 §5):

> `build.ps1` builds; `build.ps1 -Flash` builds *and* flashes.

Two hypotheses were once recorded as tested against a board that had never been
reflashed, because the invocation used named a script that does not exist and
PowerShell's error went to a filtered stream. Every hypothesis returned
bit-identical output, which was read as "none of these are the cause" when it
meant "no experiment has run yet". Chapter 20 §20.5 is the full account. The
mitigation is narrow and dull:

> the flash step now always shows its `Hash of data verified.` line, and that
> line is checked before any capture is interpreted.

---

**Next:** where the segments the bootloader copies actually land, and why one of
them appeared to collide with the bootloader itself.
