# vendor/phy — what Espressif's radio blob asks of its host

**Measured, not estimated.** `libphy.a` links completely against this directory
plus the ROM address tables. Nothing here is speculative.

## The finding

`libphy.a` exports 368 symbols and references 123 that its own objects do not
define — but 107 of those are defined by *other objects inside the same
archive*, which the linker resolves internally. The true external surface is:

**16 symbols.** Of those, 10 are already in the chip's ROM at fixed addresses
(`memcpy`, `sprintf`, `ets_delay_us`, `phy_get_romfuncs`, and the soft-float
helpers), leaving **6 to write** — all of them small, and all in `../windowed/phy_host.c`:

| symbol | implementation |
|---|---|
| `phy_enter_critical` / `phy_exit_critical` | `rsil 3` / restore — nat-os already has this as `crit_enter`/`crit_exit` |
| `esp_dport_access_reg_read` | a plain register read; the dual-core hazard workaround is unnecessary on one core |
| `rtc_get_xtal` | returns 40 — the CYD's crystal |
| `phy_printf` | routed to UART, or discarded |
| `temprature_sens_read` | stub |
| `phy_tx_pwr_track_en` | a flag |

`__divsf3` is the one soft-float helper *not* in ROM and comes from the
toolchain's `libgcc.a`.

## Verified link

```
xtensa-esp32-elf-gcc -nostdlib -nostartfiles \
  -Wl,--whole-archive libphy.a -Wl,--no-whole-archive phy_host.o \
  -T esp32.rom.ld -T esp32.rom.libgcc.ld \
  -T esp32.rom.newlib-funcs.ld -T esp32.rom.newlib-nano.ld \
  -lgcc

   text    data     bss     dec
  55799    2447     672   58918
```

`--whole-archive` matters: it forces every object in, so the link proves the
*entire* archive's dependencies are satisfied, not just the reachable subset.

## Does it fit alongside nat-os?

IRAM is 128 KB. nat-os is 57.5 KB of text, libphy is 55.8 KB — **110 KB, about
15 KB spare.** Tight, and real. Much of libphy need not live in IRAM anyway.

## The first attempt to link it into the kernel FAILED

Recorded because the failure mode is not obvious and will be met again.

Adding `libphy.a` plus the ROM linker scripts to nat-os's own link produced a
kernel that **panicked during boot**: `IllegalInstruction` at `0x4000c336`, a
ROM address, inside `msg_load`.

The cause is in the scripts, not the blob. `esp32.rom.newlib-*.ld` and
`esp32.rom.libgcc.ld` define symbols by **direct assignment**, not `PROVIDE`:

```
memcpy = 0x4000c0bc;      /* strong — overrides any definition */
PROVIDE ( ets_delay_us = 0x40008534 );   /* weak — defers to ours */
```

`esp32.rom.libgcc.ld` says so in its own header, and the reason is deliberate:
Espressif wants the ROM versions to beat `libgcc.a`. The consequence here is
that **every `memcpy` in the kernel was redirected to the ROM's**, including
compiler-generated struct copies — and the ROM's `memcpy` is **windowed ABI**,
called from call0 code. It faulted on the first use.

This is the ABI problem arriving from an unexpected direction. The work so far
was about calling *into* windowed code deliberately; this was a linker script
quietly *replacing the kernel's own functions* with windowed ones.

### What the fix looks like

The two ABIs cannot share one symbol name. `libphy.a` needs a windowed
`memcpy`; nat-os needs a call0 one. The standard remedy is to rename the blob's
references so they stop colliding:

```
objcopy --redefine-sym memcpy=phy_memcpy         --redefine-sym sprintf=phy_sprintf  libphy.a libphy_natos.a
```

then supply windowed `phy_memcpy` / `phy_sprintf` alongside the other shims, and
link **only** `esp32.rom.ld` — whose entries are all `PROVIDE` and therefore
cannot displace anything nat-os defines.

Not attempted yet. The kernel was restored to its working state first.

## It runs

```
> phyver
   libphy says: phy_version: 4791, 2c4672b, Dec 20 2023, 16:06:06
```

Espressif's radio blob, executing inside nat-os, reporting its own build. The
chain is call0 shell -> `rom_call3` -> `CALL8` -> blob -> the blob calling BACK
into our windowed `phy_printf` -> buffer -> call0 reads it. Both directions
across the ABI boundary, in one call.

`phy_version_print` was chosen because it takes no arguments and touches no RF.

### Two failures on the way, both mine, both the same shape

**1. Linker scripts displacing the kernel's own symbols.** Covered above. Fixed
by renaming the blob's references with `objcopy` and linking only
`esp32.rom.ld`, whose entries are all `PROVIDE`.

**2. Calling a windowed function from call0 by accident.** The log buffer was
first exposed as `phy_host_log()` / `phy_host_log_clear()`. Both live in the
windowed file, so both are windowed; the kernel called them directly, the callee
executed `ENTRY` without the caller having rotated the window, and it fell off
the end of the function into padding — `IllegalInstruction` at `0x4008a810`.

The comment at the top of `phy_host.c` describes that exact hazard. I wrote it,
then walked into it from the other direction in the same file. The buffer is now
exported as **data**: reading a symbol needs no calling convention, and a call
across this boundary always does.

### The check that should run before every flash

```
nm natos.elf | awk '$1 ~ /^4000[0-9a-f]{4}$/'
```

Any kernel symbol resolved to a ROM address is the displacement bug. After the
fix the only two are `ets_delay_us` and `phy_get_romfuncs` — both requested by
libphy, both weak. All 683 other symbols stay in nat-os's own regions.

Relocation is not displacement: `memcpy` moving from `4008a5e8` to `4008aa34`
is code shifting, and fine. `memcpy` becoming `4000c0bc` is the bug.

## What this does NOT establish

Linking is not running, and the gap is the whole remaining risk:

- **`phy_init()` has never been called.** It wants calibration data
  (`esp_phy_init_data_t`, normally read from a flash partition), correct RTC and
  clock state, and the register interface already brought up.
- **The shims are shaped right, not proven right.** `phy_printf` discarding
  output is fine; `temprature_sens_read` returning 0 may not be — the PHY uses
  temperature for power tracking, and a constant may just mean bad calibration
  rather than a crash.
- **Nothing here has been flashed.** This is a build-time result only. It is
  deliberately NOT linked into the kernel: nat-os works, and adding 56 KB of
  radio code that cannot yet be initialised would risk that for nothing.

## Why this matters

The question was whether a vendor blob would demand a whole operating system
underneath it — heap, FreeRTOS, NVS, the IDF's logging and locking. The answer
is six functions and a symbol table. That moves WiFi from "blocked on an
unknown" to "blocked on known work".

## Where the shims live

`../windowed/phy_host.c`, not here. They are compiled `-mabi=windowed` because
the blob **calls** them: a windowed caller issues `CALL8`, and a call0 callee
neither executes `ENTRY` nor returns with `RETW`, so the window would rotate
forward and never come back. The ABI has to match on both sides of every edge,
in both directions.

That is also why `phy_printf` writes to a buffer instead of calling
`uart_puts()` — that call would be exactly the broken edge described above.
