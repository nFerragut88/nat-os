# UM-NATOS-002 — Boot Chain and Image Format

**Used Medias LLC — Embedded Systems Division**
Revision 2.0 · 2026-08-19 · Status: current — L1 is now this project's own

> **Revised.** When this was written the second stage was Espressif's. It is now
> `boot/`, 2,736 bytes, described in UM-NATOS-035. §2.2, §5 and §6 are rewritten
> below; everything about the ROM, the image format and the L1↔kernel interface
> was unaffected by the change, which is the point §2.2 made and which held.

---

## 1. Abstract

This report documents the complete path from power-on to the first instruction
of nat-os, the binary format the kernel image must satisfy, and the measured
boot trace from the target board. It answers the question "how does this C code
actually run on the device" precisely enough to debug a failure to boot.

## 2. The four stages

<!--FIGURE: boot_chain -->

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
│ L1  2nd stage    │  2,736 bytes, OURS (boot/)
│                  │  image header @ 0x10000, fixed offset
│                  │  DROM -> flash MMU, RAM -> copy
│                  │  enables the cache, jumps
└────────┬─────────┘
         ▼
┌──────────────────┐
│ L2  nat-os       │  entry @ 0x4008000C  ← our first instruction
│     start.S      │  stack, .bss clear, call0 kmain
└──────────────────┘
```

### 2.1 Stage L0 — mask ROM

Executes from ROM on reset. Determines boot mode from strapping pins (GPIO0
high = normal flash boot), configures the SPI flash interface, and loads the
second-stage bootloader from flash offset `0x1000`. It also brings up UART0 at
115200 baud for its own diagnostic output — a side effect nat-os currently
depends on (see §6).

Not modifiable.

### 2.2 Stage L1 — second-stage bootloader

**Ours, since UM-NATOS-035.** `boot/`, 2,736 bytes against Espressif's 17,536.
What it does:

1. Read the image header at `0x10000` — a fixed offset, not looked up.
2. For each segment: map it through the flash MMU if it is DROM, copy it
   otherwise (via a bounce buffer if the destination is instruction memory).
3. Enable the data cache.
4. Jump to the declared entry point.

Deliberately *not* done: no partition table is read, no checksum or SHA-256 is
verified, no OTA slot is selected, no secure boot or flash encryption is
supported. This kernel has exactly one image at one offset; Espressif's
bootloader is six times larger because it is general, and the generality is
unused here. UM-NATOS-035 §9 has the accounting.

`vendor/bootloader.bin` stays in the tree as the recovery image and as the A/B
control — `build.ps1 -Flash -VendorBootloader`. That path is kept working and
tested, and it earned its keep within hours (UM-NATOS-036 §2).

**The interface between L1 and nat-os is entirely the image header.** Nothing
else is shared — no symbols, no runtime, no calling convention.

That claim was made in revision 1.0 as the reason the bootloader was replaceable
without touching kernel code. **It was not quite true, and §6 is where the
untruth was already written down.** The header is the only interface *by
design*; the machine state L1 leaves behind is an interface too, and an
undeclared one. See §6.

### 2.3 Stage L2 — nat-os

Begins at `_start` in `kernel/start.S`. See UM-NATOS-003 for why this code can
assume a call0 environment.

## 3. Image format

`esptool elf2image` converts the linked ELF into the format L1 expects. The
produced header declares, for each segment, a length and a load address, plus
one entry point for the whole image.

Measured output for the current build:

```
Image version:  1
Entry point:    0x4008000C
2 segments
  Segment 1: len 0x00188  load 0x3FFB0000  file_offs 0x018  [BYTE_ACCESSIBLE, DRAM]
  Segment 2: len 0x002E0  load 0x40080000  file_offs 0x1A8  [IRAM]
Checksum:         0x8F (valid)
Validation hash:  8e3a272a…5c72f3 (valid)
```

Both load addresses correspond directly to the `MEMORY` regions declared in
`kernel/linker.ld`. The entry point is `_start + 0x0C` rather than `+0x00`
because the assembler places the literal pool at the head of the section; see
UM-NATOS-005 §4.

**There is no magic in this format.** It is a list of "copy N bytes to address
A", followed by "jump to address E". Any producer that emits a conforming
header will boot.

## 4. Measured boot trace

Captured 2026-08-14 from the target board on COM5 at 115200 baud, with the
serial port opened *before* reset so no output was missed.

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

### 4.1 Reading the trace

| Line | Meaning |
|---|---|
| `ets Jul 29 2019` | ROM build date — identifies the silicon revision's ROM |
| `rst:0x1 (POWERON_RESET)` | Clean cold boot. **Diagnostically important:** a watchdog or panic reset shows a different code here, and is the first thing to check when a kernel change stops booting |
| `boot:0x13 (SPI_FAST_FLASH_BOOT)` | Normal flash boot; GPIO0 was high |
| `mode:DIO, clock div:2` | Flash interface configuration |
| `load:0x…` ×3 | **The bootloader loading itself**, not nat-os |
| `entry 0x400805e4` | Entry into the *bootloader*, not our kernel |

The three `load:` lines and the `entry` line are frequently misread as the
kernel loading. They are not. They are L1 placing its own segments before it
has even read our partition. Our own load is silent — L1 does not log it — and
the first evidence of nat-os is the banner.

### 4.2 Verified handoff

The banner appearing at all proves the header was well-formed, the checksum
matched, both segments were copied, and the CPU reached `0x4008000C`. The
self-check lines that follow prove more; see UM-NATOS-006.

## 5. Flash layout

| Offset | Contents | Size | Origin |
|---|---|---|---|
| `0x1000` | Second-stage bootloader | 2,736 B | **Ours** (`boot/`) |
| `0x8000` | Partition table | 3,072 B | Borrowed, and read by nothing |
| `0x10000` | nat-os kernel image | 80,496 B | **Ours** |

The partition table is still flashed because esptool and external tooling expect
one at `0x8000`. Nothing in this boot chain reads it — L1 hardcodes `0x10000`.
Generating our own is a struct and a CRC; it has not been done because removing
the borrow would change nothing that runs.

So the executable chain from reset to shell prompt is this project's own code,
except for the mask ROM, which is silicon.

## 6. Known dependencies on L1 behaviour

These are places where nat-os relies on the bootloader or ROM having done
something, and would break if L1 were replaced naively.

> **This list was right, and it was not read before L1 was replaced.**
> Item 3 below, written 2026-08-14, describes exactly the defect that landed on
> 2026-08-19 and took most of a session to find: the kernel ran at 40 MHz
> instead of 80 because the new L1 did not select a clock, and every timing
> instrument in the system is derived from CCOUNT and so could not detect it.
> UM-NATOS-036 has the account.
>
> The lesson is not about clocks. **A section headed "would break if L1 were
> replaced naively" is a checklist, and replacing L1 is when you run it.** The
> statuses below are now maintained rather than merely recorded.

1. **UART0 is already configured.** *Status: still a dependency, and now a
   handled one.* The kernel writes into the TX FIFO without setting baud rate,
   pin mux or line discipline, relying on the ROM having configured UART0 at
   115200. Our L1 does not disturb it, and `kernel/clock.c` rescales
   `UART_CLKDIV` by the frequency ratio when it switches to the PLL — without
   that the console would have gone to garbage at the moment the clock doubled.
   Full independence would still mean configuring UART0 in the kernel.
2. **Flash cache.** *Status: RESOLVED — now ours.* The kernel outgrew IRAM and
   does depend on the DROM mapping, and that mapping is now made by our own L1:
   MMU table at `0x3FF10000`, one 64 KB page per entry. See UM-NATOS-035 §14.
3. **CPU clock.** *Status: RESOLVED — moved into the kernel.* Was "running at
   whatever L1 selected". `kernel/clock.c` now brings the SoC to 80 MHz off the
   PLL itself, returns early if it finds a bootloader has already done it, and
   **reports what it found** in the boot banner. The kernel no longer has an
   unstated requirement; it has a stated and checked one.
4. **RTC watchdog.** *New.* The ROM arms it and expects the application to take
   ownership. Both our L1 and `watchdog_disable_all()` disarm it, deliberately
   twice — see UM-NATOS-035 §5.2.
5. **`0x3FF480B0` / `0x3FF480B4`, the RTC retention frequency registers.**
   *New, and the reason item 3 was found at all.* The PHY blob does not measure
   the crystal frequency; it looks it up in `RTC_XTAL_FREQ_REG`. A bootloader
   that leaves it zero makes `register_chipv7_phy()` hang. `clock_init()` writes
   both on every path, including the one where a bootloader already set the
   clock.

## 7. Failure modes and first diagnostic step

| Symptom | Most likely cause |
|---|---|
| No output at all | Image checksum invalid, or entry point outside a loaded segment |
| ROM trace then silence | Kernel reached but faulted before UART output — suspect stack or `.bss` |
| Boot loop with `rst:0x3` / `rst:0xc` | Watchdog — kernel hung before feeding or disabling it |
| Garbled output | Baud mismatch — the ROM's rate was changed or the monitor disagrees |

In every case the **reset reason on the first line** is the highest-value
single datum, which is why the capture procedure resets the board explicitly
rather than attaching to an already-running system.

## 8. Reproduction

```powershell
cd C:\src\nat-os
.\build.ps1 -Flash -Port COM5
# then capture with the port opened before reset
```

See UM-NATOS-005 for the capture harness and why attaching after reset loses
the banner.

## 9. References

- UM-NATOS-004 — Memory Map (segment addresses and the overlap risk)
- UM-NATOS-005 — Build and Flash Pipeline
- UM-NATOS-006 — Milestone 0 Verification Report
- **UM-NATOS-035 — The Last Borrowed Thing** (L1 replaced; §14 is the reference)
- **UM-NATOS-036 — The Half-Speed Board** (what §6 item 3 cost when unread)
