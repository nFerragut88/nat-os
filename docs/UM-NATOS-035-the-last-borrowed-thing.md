# UM-NATOS-035 — The Last Borrowed Thing

**Used Medias LLC — Embedded Systems Division**
Revision 1.2 · 2026-08-19 · Status: **Running on hardware; §8's verification was incomplete — see UM-NATOS-036. §14 is the mechanical reference.**

> **CORRECTION.** This bootloader shipped without configuring the SoC clock,
> which Espressif's does. The board ran at 40 MHz instead of 80, and every
> instrument in §8 was blind to it because they are all derived from CCOUNT.
> The `-WiFi` build was also never tested on it and does not work.
>
> The clock is fixed in `kernel/clock.c`. The WiFi gap is open and `-WiFi`
> is gated to Espressif's loader. UM-NATOS-036 has the whole account,
> including what §8 should have said.

---

## 1. Abstract

`docs/blob-free.md` ended with a single sentence naming what was left:

> One thing stands between this and a kernel that is entirely its own code:
> `vendor/bootloader.bin`, 17 KB, Espressif's second-stage bootloader. Writing a
> replacement is a bounded, achievable project. It is also, now, the only one
> left.

It is written. It is 2,736 bytes, it lives in `boot/`, `build.ps1 -Flash` now
builds and flashes it by default, and the kernel boots on it with every
subsystem passing.

The interesting content of this report is not the bootloader — which is three
memory copies and one address-space mapping, and was never going to be hard. It
is **why it was short**, and the one defect that stopped it.

The defect is worth the report on its own. It was found in eleven seconds by an
exception handler in the ESP32's mask ROM, printing an address that named the
cause outright. This project has spent whole sessions on faults where six
instruments reported success and only the glass disagreed. This one announced
itself. The contrast is the lesson, and §7 argues it was not luck.

---

## 2. What the job actually is

The ROM's first-stage loader reads an image from flash `0x1000`, copies its
segments into RAM, and jumps to it. Everything after that is the second stage's
problem. For this kernel, that problem is four segments:

```
Segment 1  DROM  0x3f400020  0x05530   flash-mapped -- needs the MMU
Segment 2  DRAM  0x3ffb0000  0x00308   copy
Segment 3  DRAM  0x3ffb0308  0x0009c   copy
Segment 4  IRAM  0x40080000  0x0de28   copy
Entry             0x4008040c
```

Three copies and one mapping. That is the entire specification.

> **Correction to `boot/README.md`.** That file was written before the blob-free
> default landed and quotes the segment table of the `-WiFi` build: DROM
> `0x06d48`, IRAM `0x1c234`. The numbers above are the current default image.
> The kernel's IRAM segment shrank by 57 KB when 1.4 MB of vendor archive left
> the link.

---

## 3. Why this was a short project and the PHY is not

UM-NATOS-034 §17 spent several thousand words on whether WiFi transmit is
*possible*. The honest answer there was "yes, in some number of sessions, and it
would not make the project clean." The bootloader is the opposite case in every
respect, and the difference is worth naming precisely, because "replace the
vendor code" sounds like one kind of task and is actually two.

| | flash MMU / cache | PHY calibration |
|---|---|---|
| specified where | ESP32 TRM, SDK headers | nowhere |
| constants | published register addresses | measured per-die at Espressif |
| verifiable | boots or does not | radiates or does not, no middle |
| this project's exposure | 2,736 bytes | 847 KB |

Every constant the bootloader needs was **read out of the SDK headers during
this session** — `soc/dport_reg.h`, `soc/ext_mem_defs.h`, `hal/mmu_ll.h`,
`bootloader.ld` — not recalled. That discipline is now standing practice for the
reason UM-NATOS-029 and 030 both paid for: a recalled constant that is nearly
right produces a failure that looks like a logic error.

### 3.1 The chicken-and-egg that was already solved

A second-stage bootloader normally has one genuinely awkward problem: it must
read flash in order to set up the machinery that makes flash readable. The cache
is off, so nothing flash-mapped can be called, and the usual answer is to link
against the ROM's SPI routines — which is to say, against somebody else's code,
in the one program whose entire purpose is not to need any.

nat-os had already solved this and did not know it. From UM-NATOS-018:

```c
int flash_read(uint32_t addr, void *dst, uint32_t len);   /* kernel/flash.c */
```

`flash.c` drives the SPI1 controller through its registers directly. No ROM
calls, no vendor code, and — the property that matters here — **no dependence on
the cache**, since it never reads a flash-mapped address to do its work. It
saves and restores the six SPI1 registers it touches and sets its own clock, so
it does not care what state the ROM left the controller in.

It is compiled into the bootloader image unchanged and used as-is. That single
file is the difference between this being an afternoon and being a milestone.

There is a general point here that is worth keeping. `flash.c` was written to
give the kernel a persistent store. Its reusability in a context that did not
exist yet came from a property it was given for a different reason: it depends
on nothing but hardware. Code that depends only on hardware turns out to be
portable to situations its author never considered.

---

## 4. The link map, and the constraint that decides every address

`boot/boot.ld`:

```
iram (rwx) : ORIGIN = 0x40078000, LENGTH = 0x8000    /* 32 KB */
dram (rw)  : ORIGIN = 0x3FFF0000, LENGTH = 0x6000    /* 24 KB */
```

One constraint decides both. This program copies nat-os into memory; if any part
of it lived where nat-os lands, it would overwrite itself mid-copy, and the
failure would be a hang with no message.

```
nat-os IRAM   0x40080000 .. 0x400A0000     boot .text  0x40078000 .. 0x40078900
nat-os DRAM   0x3FFB0000 .. 0x3FFDC200     boot .data  0x3FFF0000 .. 0x3FFF0170
```

Both windows sit clear, and they are the windows Espressif's own bootloader uses
for exactly this reason — taken from its `bootloader.ld` rather than chosen.
Where a vendor made a decision for a reason that still applies, adopting the
decision is not borrowing; the reasoning is what is being adopted.

### 4.1 No DROM, deliberately

There is no flash-mapped region in that map and there must not be. This program
runs with the cache disabled — turning it on is part of its job — so any `.rodata`
it placed in flash would be unreadable at the moment it was needed. `.rodata` is
named explicitly into DRAM instead.

This is the sort of thing that would otherwise be found by a string printing as
garbage, at which point the obvious theory is a broken UART.

---

## 5. Two decisions taken before flashing

Both are cases where the code as first written would probably have worked. Both
were changed anyway, and the reasoning is the same in each case: the failure mode
was *intermittent*, and an intermittent boot failure is worse to diagnose than
one that never works.

### 5.1 The cache goes on once, at the end

The first draft enabled the cache inside the segment loop, immediately after
mapping DROM. DROM is segment 1 of 4. Three more segments are then copied over
SPI1 — with the cache live, and the cache controller is a second bus master on
the same flash chip.

Two masters driving one flash chip is a corruption that would appear as a random
bad byte somewhere in a 56 KB kernel image, on some boots. `cache_enable_drom()`
now runs once, after the loop, when nothing will touch SPI1 again.

Whether the cache would in fact have issued a transaction is beside the point.
It had no work to do — nothing was executing from flash — so it very likely would
have stayed quiet. "Very likely quiet" is not a property to build a boot chain
on.

### 5.2 The RTC watchdog is disabled here, not just in the kernel

The ROM arms the RTC watchdog before jumping to the second stage, so that a
second stage which hangs still produces a reset. nat-os disables it in
`watchdog_disable_all()` — but not until the kernel is running, which leaves the
whole of the bootloader depending on being fast enough.

Three register writes remove the dependency. The kernel disables it again; doing
it twice costs nothing.

---

## 6. The one defect

First flash. The log is worth reading in full, because of how much of it is
success:

```
[boot] nat-os second stage
[boot] flash id 0x00684016
[boot] segments 0x00000004  entry 0x4008040c
[boot]   0x3f400020 len 0x00005530  -> mmu
[boot]   0x3ffb0000 len 0x00000308  -> copy
[boot]   0x3ffb0308 len 0x0000009c  -> copy
[boot]   0x40080000 len 0x0000de28  -> copy
Fatal exception (3): LoadStoreError
epc1=0x4007846b   excvaddr=0x40080000
```

The header parsed. All four segments were found with correct addresses and
lengths. DROM mapped. Both DRAM segments copied. It died on the fourth.

`excvaddr=0x40080000` is the IRAM segment's load address — the destination of
the store that faulted, named by the hardware.

**Cause.** IRAM is instruction memory. It answers aligned 32-bit accesses and
nothing else; a byte store to it raises `LoadStoreError`. `flash.c` reassembles
the SPI FIFO one byte at a time:

```c
for (uint32_t i = 0; i < rxlen; i++) {
    uint32_t word = REG(SPI1_W(i / 4u));
    rx[i] = (uint8_t)(word >> (8u * (i % 4u)));    /* byte store */
}
```

That is correct everywhere the kernel itself uses it — the store, the SD buffer,
the fault record, all DRAM — and wrong only here. This is the first caller in the
project's history that ever asked it to write instruction memory.

**Fix.** `flash.c` is not changed. It is a file the kernel depends on and it is
right for every one of its own callers; changing it to serve one unusual caller
would put the risk in the wrong place. `boot.c` reads into a 1 KB DRAM bounce
buffer and copies across in words:

```c
static uint8_t g_bounce[1024] __attribute__((aligned(4)));

static int copy_to_iram(uint32_t off, uint32_t dst, uint32_t len)
{
    while (len) {
        uint32_t n = (len > BOUNCE_LEN) ? BOUNCE_LEN : len;
        if (flash_read(off, g_bounce, n) != 0) return -1;
        const uint32_t *src = (const uint32_t *)(const void *)g_bounce;
        volatile uint32_t *d = (volatile uint32_t *)dst;
        for (uint32_t w = 0; w < (n + 3u) / 4u; w++) d[w] = src[w];
        off += n; dst += n; len -= n;
    }
    return 0;
}
```

Segments are dispatched on destination: DROM to the MMU, instruction bus through
the bounce buffer, everything else direct.

---

## 7. Why this fault was easy, and why that was not luck

This project's recent history is full of faults that took a session or more:
UM-NATOS-029's dead DMA engine (eleven theories), UM-NATOS-030's single wrong
bit (six instruments reporting success), UM-NATOS-034's silent transmitter
(still open, three months of register poking). This one took eleven seconds.

The difference is not difficulty. It is that **this fault had a hardware
reporter and those did not.**

A byte store to IRAM is architecturally illegal, so the CPU raises an exception
with the offending address in a register, and the mask ROM's handler prints it.
Nothing had to be inferred. Compare the transmit problem, where writing a value
to a register the hardware does not honour is perfectly legal, completes
successfully, reads back correctly, and increments a completion counter — and
the only disagreement is that no radio energy exists.

The rule this project already keeps — **a successful register write is not
evidence** — has a corollary that this session makes concrete:

> Faults the architecture can detect are cheap. Faults that are only detectable
> by the absence of an effect are expensive, and the expense has nothing to do
> with how simple the bug turns out to be.

It is also why the four `uart_puts` calls in the segment loop earned their space
in a 2.7 KB image. They cost nothing and they turned "the board does not boot"
into "it dies on segment 4 of 4, writing to 0x40080000."

---

## 8. Verification

Hardware, COM5, clean build of both halves flashed together.

**Boot.** One ROM banner in 15 seconds — no reset loop. Full kernel banner:
`rtc wdt disarmed`, `vecbase 0x40080000`, `sd SDSC ready`, `messages loaded 3
saved`, `display init ok bytes=153634 fullscreen=44 ms`, `heap 121344 B`,
`store loaded, boot #56`.

**Self-tests, all PASS.** no-leak (10,000 cycles, `check=0`), oom-safe, arenas,
VM program / faults / runaway / predicate, task interleave, arena isolation
(`rogue` faulted at offset 256 = arena size, neighbours still advancing),
release, mutex.

**Subsystems, after boot, over the shell.**

| | |
|---|---|
| `sd` | `sd_init OK type=SDSC` |
| `sdread 0` | valid boot sector, `signature=0x55 0xaa`, partition 0 read |
| `progs` | 11 programs listed from DROM |
| `romcall` | `crc32_le(0,"123456789",9) = 0xcbf43926` |
| `i2c` | `bus looks sane`, scan clean |
| `3d` / `3d off` | view and launcher both render |
| `stacks` | 9 tasks, tightest 1344 of 2048 B free |
| `mem` | `check=0` |

Two of those are load-bearing beyond looking green.

**`romcall` is a known-answer test.** `0xcbf43926` is the published CRC-32 check
value for the string `"123456789"`. Getting it right means the ROM was reached,
executed, and returned correctly — so the MMU work did not disturb the ROM's own
address space. A wrong answer would have been visible; this is the rare case
where the instrument cannot be wrong in a way that flatters us.

**Everything printed is DROM.** Every string in that log is flash-mapped through
the MMU entry this bootloader wrote. If the mapping were off by a page, the
banner would be garbage rather than absent. The font is DROM too, so
`display init ok` and the rendered launcher say the same thing again through a
different path.

There is one more piece of evidence, and it is accidental. The first successful
boot ran an **older kernel image** than the source tree — only the bootloader
had been reflashed. It loaded a 145 KB image with a different segment table,
different entry point, and WiFi symbols in it, correctly. The bootloader knows
nothing about the kernel it loads, and that was demonstrated before it was
claimed.

---

## 9. What this changes in the tree

`build.ps1 -Flash` now builds `boot/` and writes **our** second stage at
`0x1000`. `-VendorBootloader` restores Espressif's.

The vendor path is kept working deliberately: it is how a broken bootloader gets
recovered, and the day that is needed is not the day to be debugging the
recovery. It is also the A/B control if ours is ever suspected.

```powershell
.\build.ps1 -Flash                     # our bootloader
.\build.ps1 -Flash -VendorBootloader   # Espressif's, for recovery or comparison
```

### Size

| | |
|---|---|
| `vendor/bootloader.bin` | 17,536 B |
| `boot/build/boot.bin` | **2,736 B** |

Espressif's is not bloated; it is general. It parses a partition table, verifies
SHA-256, supports secure boot, flash encryption, OTA slot selection, and
anti-rollback. nat-os has one image at a fixed offset and needs none of it. The
6× difference is the cost of generality nobody here is using, and it is the
usual result when a program is written for one machine.

---

## 10. What is still not ours, stated plainly

Three things, and only one of them is a real dependency.

**The first-stage loader.** In the ESP32's mask ROM. It is silicon. It cannot be
replaced by anyone, including Espressif, and a project that counted it would be
counting the instruction decoder too.

**`vendor/partitions.bin`, 3 KB at `0x8000`.** Still flashed, still borrowed, and
now **read by nothing in this boot chain** — `boot.c` hardcodes `APP_OFFSET
0x10000`, on the grounds that a kernel with exactly one image does not need a
table to find it. It stays because esptool and external tooling expect one to be
there. Generating it is a `struct` and a CRC; it has not been done because
removing it would change nothing.

**`vendor/bootloader.bin`.** No longer in the boot path. Kept as the recovery
image, which is the reason it was ever in the tree.

So: **the executable chain from reset to shell prompt is now this project's own
code, except for the part that is physically in the chip.**

---

## 11. Honest accounting

Three things this does not achieve, recorded here so nobody has to rediscover
them.

**It does not make WiFi clean.** `build.ps1 -WiFi` still links 1.4 MB of
Espressif's PHY and MAC archives. `blob-free.md` argued that WiFi transmit could
never buy independence; that argument is unaffected. What changed is that the
*default* build's independence is now complete rather than nearly complete.

**It does not add capability.** The board booted before. It boots now, from a
smaller program, that this project can read. The value is in the second clause.

**It was not hard.** Roughly 250 lines including comments, one defect, one
session. It is recorded at this length because the *reasons* it was easy —
`flash.c`'s hardware-only dependencies, published constants, an architecture that
detects its own violations — are transferable, and the reasons the open problems
are hard are the same reasons inverted.

---

## 12. Rules earned

**A file that depends only on hardware is portable to contexts its author never
imagined.** `flash.c` was written for a persistent store. It worked unmodified
inside a program that runs before the kernel exists, because it never depended on
the kernel existing. That property was not designed for; it fell out of the
constraint that it use no ROM calls.

**Faults the architecture can detect are cheap; faults detectable only as an
absent effect are expensive.** Independent of how simple the bug is. A byte store
to IRAM and a register write the radio ignores are both one-line mistakes; one
cost eleven seconds and the other is in its third month.

**Print the step, not the outcome.** Four `uart_puts` calls in a 2.7 KB image
converted a dead board into a named failing operation at a named address. In a
program with no debugger, no panic handler of its own, and no return path, the
log *is* the instrument.

**Adopting a vendor's reasoning is not borrowing their code.** The bootloader's
two memory windows come from Espressif's `bootloader.ld`. The constraint that
produced them — do not live where the kernel lands — is ours as much as theirs,
and re-deriving the same answer to look independent would have been theatre.

---

## 13. Where this leaves the project

`docs/blob-free.md` closed with the bootloader as "the only one left." It is
done. The remaining work is capability, not independence:

- **01 — WiFi transmit.** Unchanged and still open. The recorded next method is a
  register-write *trace* of ESP-IDF from reset through first transmit, not more
  snapshot diffing. UM-NATOS-034 §13 explains why a snapshot compares
  destinations and not routes.
- **LoRa Phase 0b.** SPI3 is up and self-tested; the SX1262 driver waits on
  hardware. This is the transport that was always meant to carry the network in
  `docs/conceptual/the-ark-and-fiendnet.md`, and it is a documented peripheral
  with no blob — the clean path to the same capability.
- **04 — scheduler timing**, and the remainder of **05**.

---

---

## 14. Reference — how it actually works

**Added revision 1.2.** The sections above explain why the bootloader is the way
it is. They do not say what it *does*, and `docs/README.md` states that these
reports exist so the project can be picked up cold without re-deriving decisions
from the source. Judged against that, §§1–13 fell short: every constant lived
only in `boot/boot.c`'s comments. This closes it.

Every address below was read from the SDK headers named beside it.

### 14.1 The image header, at `0x10000`

24 bytes, then a 4-byte header per segment. `boot/boot.c` mirrors it as a packed
struct:

```
offset  size  field
  0      1    magic          0xE9
  1      1    segment_count
  2      1    spi_mode
  3      1    spi_speed / spi_size
  4      4    entry_addr
  8      1    wp_pin
  9      3    spi_pin_drv
 12      2    chip_id
 14      1    min_chip_rev
 15      8    reserved
 23      1    hash_appended
--- then, per segment ---
  0      4    load_addr
  4      4    data_len          (always a multiple of 4)
```

Segments follow one another with no padding between them. The loader walks them
in order, keeping a running flash offset.

### 14.2 Dispatch, by destination

The whole of the loader's logic is one three-way branch on `load_addr`:

| range | meaning | action |
|---|---|---|
| `0x3F400000`–`0x3F800000` | DROM, flash-mapped | write an MMU entry; copy nothing |
| `≥ 0x40000000` | instruction bus | copy **in 32-bit words**, via a DRAM bounce buffer |
| otherwise | DRAM, byte-accessible | `flash_read()` straight to the destination |

The middle row is §6's defect. IRAM answers aligned 32-bit accesses only, and
`kernel/flash.c` reassembles the SPI FIFO a byte at a time.

### 14.3 The flash MMU

From `soc/dport_reg.h`, `soc/ext_mem_defs.h`, `hal/mmu_ll.h`:

```
table base      0x3FF10000      one 32-bit entry per 64 KB page
page size       0x10000
DROM window     0x3F400000 .. 0x3F800000
entry index     (vaddr & 0x3FFFFF) >> 16
entry value     paddr >> 16          i.e. the physical flash page number
MMU_INVALID     bit 8                so a valid entry is a page number under 256
```

A segment at virtual `0x3f400020` from flash offset `0x11018` therefore maps the
page *containing* each, not the exact addresses — the low 16 bits of virtual and
physical must already agree, and in an esptool-produced image they do, because
`elf2image` places DROM so that they do. That congruence is not a coincidence to
rely on silently; UM-NATOS-011 is where this project first met it.

### 14.4 The cache

From `soc/dport_reg.h`:

```
DPORT_PRO_CACHE_CTRL_REG    0x3FF00040
DPORT_PRO_CACHE_CTRL1_REG   0x3FF00044
  PRO_CACHE_ENABLE          bit 3   (in CTRL)
  PRO_CACHE_FLUSH_ENA       bit 4
  PRO_CACHE_FLUSH_DONE      bit 5
  PRO_CACHE_MASK_DROM0      bit 4   (in CTRL1 — cleared to let DROM through)
```

Order matters twice, and both are §5:

- the cache is **disabled** while MMU entries are written;
- it is **enabled once, after the segment loop**, never inside it, so the cache
  controller is never a second bus master while SPI1 is still copying.

### 14.5 Reading flash with the cache off

`kernel/flash.c`, unchanged, compiled into the loader:

```c
int      flash_read(uint32_t addr, void *dst, uint32_t len);
uint32_t flash_read_id(void);
```

It drives SPI1 through its registers — no ROM calls — saves and restores the six
registers it touches, sets its own clock, and never reads a flash-mapped address
to do its work. `flash_read_id()` is the loader's first act after the watchdog:
an id of `0` or `0xFFFFFF` means the bus is dead, and every read after it would
be garbage that looks like data. The measured id on this board is `0x00684016`.

### 14.6 Memory map

```
boot .text   0x40078000 .. 0x40080000   (32 KB window, ~2.2 KB used)
boot .data   0x3FFF0000 .. 0x3FFF6000   (24 KB window, ~0.4 KB used)

nat-os IRAM  0x40080000 .. 0x400A0000   <- must not overlap the above
nat-os DRAM  0x3FFB0000 .. 0x3FFDC200   <- must not overlap the above
```

No DROM region in `boot.ld`, deliberately: this runs with the cache off, so
`.rodata` goes to DRAM (§4.1).

### 14.7 Order of operations

```
boot_start.S   set a1 = _stack_top          (not the ROM's stack)
               zero .bss                    (NOLOAD; holds last boot's garbage)
               call0 boot_main

boot_main      rtc_wdt_disable()            0x3FF480A4 = 0x50D83AA1, 0x3FF4808C = 0, relock
               flash_read_id()              sanity, before anything depends on it
               read header at 0x10000       reject if magic != 0xE9
               per segment: MMU / bounce-copy / direct copy
               cache_enable_drom()          once, here
               jump to hdr.entry_addr
```

### 14.8 What it does not do, and the consequence

No partition table, no checksum, no SHA-256, no OTA, no secure boot, no flash
encryption — and, as UM-NATOS-036 found the hard way, **no clock configuration.**
That last one is not in this list as a defect any more: `kernel/clock.c` owns it,
which is a better place for it (UM-NATOS-036 §5). But it belongs in this section
because a reader comparing this loader with Espressif's needs to know that the
difference exists and where the responsibility went.

### 14.9 Recovery

```powershell
.\build.ps1 -Flash -VendorBootloader          # Espressif's, and the A/B control
python esptool.py --chip esp32 --port COM5 write_flash 0x1000 vendor\bootloader.bin
```

The board cannot be bricked this way: ROM download mode is reached by holding
GPIO0 low at reset and does not depend on anything in flash.

---

*Filed 2026-08-19. Written, flashed, verified on hardware the same session.
Revision 1.1 corrected §8's verification claim; revision 1.2 added §14, because
the report explained itself well and documented itself badly.*

Written by: Hare