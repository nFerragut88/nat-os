# 07 — A flash-executable region, before IRAM runs out

> **DONE 2026-08-19 — UM-NATOS-037.** `shell.c` and `kmain.c` execute from
> flash. Free IRAM 19,127 -> 44,943 in the `-WiFi` build. The plan below held;
> what it did not predict was that moving code would surface three unrelated
> defects, which is what the report is mostly about.

**Size:** medium. **Risk:** medium — it changes the memory map and the boot
chain. **Blocked on:** nothing.

*Worth doing whatever happens to the radio.* 08 depends on it; the kernel needs
it anyway.

---

## The finding

Measured on the current `-WiFi` build:

```
.text    102,892
.iram1     8,941
used     111,833  of  131,072      ->  19,239 bytes free
```

`kernel/linker.ld` declares three regions:

```
iram (rwx) : ORIGIN = 0x40080000, LENGTH = 0x20000   /* 128 KB */
dram (rw)  : ORIGIN = 0x3FFB0000, LENGTH = 0x2C200
drom (r)   : ORIGIN = 0x3F400020, LENGTH = 0x400000 - 0x20
```

There is **no `irom`**. Every instruction this kernel executes lives in 128 KB of
IRAM, and 85% of it is spoken for.

`.rodata` already lives in flash — `drom` is mapped through the flash MMU by our
own bootloader (UM-NATOS-035 §14.3). **Code has never been given the same
treatment**, and there is no reason beyond nobody having needed it yet.

## Why it matters regardless of WiFi

128 KB is a hard ceiling on the entire kernel. Not on one subsystem — on all of
it, forever. The blob-free build is at 79 KB and comfortable; the `-WiFi` build
is at 112 KB and is not. Any of the following runs into it:

- a LoRa stack and a DTN bundle layer (`docs/conceptual/the-ark-and-fiendnet.md`)
- more VM syscalls, more devices, a filesystem
- 08, which needs far more room than exists

This is the kind of limit that is cheap to lift now and expensive to lift under
pressure.

## What it takes

The machinery is already in the tree. The bootloader maps DROM through the flash
MMU and enables the data cache; IROM is the same mechanism on the instruction
side.

1. **`kernel/linker.ld`** — add an `irom` region in the `0x400D0000` window and a
   section that collects code placed there. Default stays IRAM; an attribute
   opts a function *out* into flash. (ESP-IDF does the reverse — everything in
   flash, `IRAM_ATTR` opts in — and that is worth copying rather than inventing,
   because the interesting cases are the exceptions.)
2. **`boot/boot.c`** — the segment loop already dispatches on `load_addr`. Add
   the IROM window alongside the existing DROM case; the MMU arithmetic is
   identical, only the target window differs.
3. **The cache** — `cache_enable_drom()` clears `PRO_CACHE_MASK_DROM0`. IROM
   additionally needs `PRO_CACHE_MASK_IRAM0` cleared. UM-NATOS-035 §14.4 has the
   register and the ordering constraint.
4. **The image** — gains a fifth segment. `esptool image_info` will show it and
   the bootloader will map rather than copy it.

## What must NOT move to flash

This is the part that will bite, and it is why ESP-IDF's default is inverted.

Code executing from flash stalls on a cache miss. Anything that runs **with the
cache disabled**, or in a path that cannot tolerate an unbounded stall, has to
stay in IRAM:

- exception and interrupt vectors, and everything they reach (`kernel/vectors.S`,
  `panic.c`)
- `kernel/flash.c` — it drives SPI1 and is *called* with the cache off
- the scheduler's switch path
- anything the APP CPU runs (`appcpu.c`), which has no cache enabled at all
- the window-overflow handlers in `window.S`

Getting this wrong produces a hang with no message, in a fault handler, which is
the worst place in the system for one.

## How to know it worked

- `esptool image_info` shows the new segment, and the bootloader logs `-> mmu`
  for it rather than `-> copy`
- IRAM free rises by whatever was moved
- every self-test still passes, `romcall` still returns `0xcbf43926`
- **and the panic path still works** — force one with `fault` and confirm the
  report still reaches the UART and the panel. That is the test that catches a
  vector accidentally placed in flash.

## Related

- UM-NATOS-035 §14 — the MMU arithmetic, the cache registers, and the ordering
- UM-NATOS-011 — the 0x20 page congruence, met the first time `.rodata` moved
- UM-NATOS-004 — the memory map this changes
