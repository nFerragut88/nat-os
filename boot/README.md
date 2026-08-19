# A second-stage bootloader

The last thing in this project that was somebody else's binary.

**Status: done.** 2,736 bytes, running on hardware, flashed by
`build.ps1 -Flash` by default. Full account in
[UM-NATOS-035](../docs/UM-NATOS-035-the-last-borrowed-thing.md).

`vendor/bootloader.bin` is 17 KB of Espressif's second-stage bootloader, and
`docs/blob-free.md` named it as the only remaining dependency after WiFi moved
behind a switch. Unlike `libphy.a` it is entirely replaceable: the format is
documented, the hardware it touches is documented, and UM-NATOS-002 already
describes the image layout this project's own build produces.

## What it has to do

The ROM's first-stage loader reads a second-stage image from flash `0x1000`,
copies its segments into RAM, and jumps to it. From there this bootloader must
put nat-os in memory and start it.

nat-os's image, read from the binary the build already produces:

```
Segment 1  DROM  0x3f400020  0x05530   flash-mapped, needs the MMU
Segment 2  DRAM  0x3ffb0000  0x00308   copy
Segment 3  DRAM  0x3ffb0308  0x0009c   copy
Segment 4  IRAM  0x40080000  0x0de28   copy
Entry             0x4008040c
```

Three copies and one address-space mapping. That is the whole job.

(Those numbers are the blob-free default build. An earlier revision of this file
quoted the `-WiFi` build's table, where DROM is `0x06d48` and IRAM `0x1c234` --
the kernel's instruction segment shrank by 57 KB when the vendor archives left
the link.)

## Why this is achievable and the PHY is not

Every constant it needs is published:

| | |
|---|---|
| image header format | ESP32 TRM, and UM-NATOS-002 |
| flash MMU table | `0x3FF10000`, one 32-bit entry per 64 KB page |
| DROM window | `0x3F400000`–`0x3F800000`, entry index from bits 21:16 |
| cache control | `DPORT_PRO_CACHE_CTRL_REG`, documented bits |
| reading flash | **already ours** — `kernel/flash.c` drives SPI1 directly |

That last row is why this is a short project rather than a long one. nat-os has
had its own register-level flash driver since UM-NATOS-018, with no ROM calls
and no vendor code. The bootloader reuses it.

## Recovery

A broken bootloader stops the board from starting. It is not a brick:

```powershell
python esptool.py --chip esp32 --port COM5 write_flash 0x1000 vendor/bootloader.bin
```

Espressif's original goes straight back at `0x1000`. That is the whole reason it
stays in the tree.

## Files

| | |
|---|---|
| `boot.ld` | link map. IRAM `0x40078000`, DRAM `0x3FFF0000` -- both clear of where nat-os lands, which is the constraint that decides both. No DROM region: this runs with the cache off, so `.rodata` goes to DRAM. |
| `boot_start.S` | entry from the ROM. Sets a stack of our own, zeroes `.bss` (which is NOLOAD, so on entry it holds whatever the last boot left), calls `boot_main`. |
| `boot.c` | the work. Header parse, MMU mapping, segment copies, cache enable, jump. |
| `build_boot.ps1` | builds and optionally flashes. Compiles `kernel/flash.c`, `kernel/uart.c` and `kernel/kstring.c` again rather than linking against the kernel, which is not in memory yet. |

## The one thing that bit

IRAM answers aligned 32-bit accesses only. `flash.c` reassembles the SPI FIFO a
byte at a time -- correct for every caller the kernel has, illegal for this one.
First flash died with `LoadStoreError`, `excvaddr=0x40080000`, which is the IRAM
segment's load address; the hardware named the cause outright.

`flash.c` is unchanged. `copy_to_iram()` in `boot.c` reads into a 1 KB DRAM
bounce buffer and copies across in words.
