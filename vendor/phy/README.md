# vendor/phy — what Espressif's radio blob asks of its host

**Measured, not estimated.** `libphy.a` links completely against this directory
plus the ROM address tables. Nothing here is speculative.

## The finding

`libphy.a` exports 368 symbols and references 123 that its own objects do not
define — but 107 of those are defined by *other objects inside the same
archive*, which the linker resolves internally. The true external surface is:

**16 symbols.** Of those, 10 are already in the chip's ROM at fixed addresses
(`memcpy`, `sprintf`, `ets_delay_us`, `phy_get_romfuncs`, and the soft-float
helpers), leaving **6 to write** — all of them small, and all in `phy_host.c`:

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
