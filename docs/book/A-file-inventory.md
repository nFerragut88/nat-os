# Appendix A — File Inventory

Every file in the tree, what it does, and which chapter covers it.

---

## A.1 The kernel

Sizes are source bytes. 671,884 bytes across `kernel/` in total.

### Entry, vectors, and the machine

| File | Bytes | Contents | Chapter |
|---|---|---|---|
| `start.S` | 1,343 | The entry stub: stack, `.bss` clear, `call0 kmain` | 3 |
| `vectors.S` | 8,807 | Vector slots, the level-3 handler and its 21-word frame, the panic entry | 7, 8 |
| `window.S` | 18,744 | Six register-window handlers, the windowed↔call0 bridges, `phy_stack_call` | 2, 27 |
| `window.h` | 3,376 | Bridge declarations, with the verification rationale for each | 2, 27 |
| `xtensa.h` | 3,519 | Special-register accessors with their required synchronisation | 7 |
| `linker.ld` | 7,095 | IRAM / DRAM / DROM, vector placement, `.dram.rodata`, heap bounds | 4 |

### Scheduling and time

| File | Bytes | Contents | Chapter |
|---|---|---|---|
| `task.c` | 26,656 | Task table, fabricated frames, the selection loop, ageing, CPU accounting, `task_sleep` | 8, 9 |
| `task.h` | 10,185 | Frame layout, priorities, ageing constants, the blocking protocol | 8, 9 |
| `timer.c` | 5,602 | CCOMPARE1 tick, three defects' worth of comments | 7 |
| `timer.h` | 763 | | 7 |
| `critical.h` | 1,341 | `crit_enter`/`crit_exit`, and a capitalised warning about misuse | 11 |
| `mutex.c` | 3,785 | Direct handoff, the `granted` bitmask, non-owner refusal. **No priority handling of any kind** — see Ch. 30 §30.5 | 11, 30 |
| `mutex.h` | 2,864 | | 11 |

### Memory

| File | Bytes | Contents | Chapter |
|---|---|---|---|
| `heap.c` | 8,582 | Address-ordered list, split/coalesce, ten-code `heap_check()` | 10 |
| `heap.h` | 2,292 | | 10 |
| `arena.c` | 3,154 | Arena lifecycle, zeroing, `arena_contains()` | 10 |
| `arena.h` | 2,040 | The isolation statement | 1, 10 |
| `kstring.c` | 1,996 | `memcpy`, `memset`, `memmove`, `memcmp` | 5, 14 |

### The virtual machine

| File | Bytes | Contents | Chapter |
|---|---|---|---|
| `vm.h` | 11,572 | The ISA, the syscall table, the fault codes, `vm_t` | 13, 14 |
| `vm.c` | 23,405 | Dispatch, checked accessors, `vp_fill`/`vp_text`, fourteen syscalls, event injection | 14, 17 |
| `vmarg.c` | — | The shared argument harness: one place an `(offset, length)` from a program is checked | 17, 30 |
| `device.c` | — | The device table — light, beep, store, i2c, keys, echo, sd — plus per-caller permissions | 30 |
| `app.c` | 6,830 | Application table, `retire()`, `app_tick()` | 16 |
| `app.h` | 4,126 | Strip geometry and the close-button argument | 16, 24 |
| `ipc.c` | 2,631 | Mailboxes, copy-in/copy-out | 16 |
| `ipc.h` | 2,291 | The "copied, never shared" rationale | 16 |

### Drivers

| File | Bytes | Contents | Chapter |
|---|---|---|---|
| `display.c` | 32,900 | Bit-banged and SPI2 backends, DMA, spans, font, panic mode, `display_resync` | 18 |
| `display.h` | 6,850 | Pin map, lock timing names, panic-mode contract | 18 |
| `touch.c` | 21,247 | XPT2046 burst, PD handling, pressure gate, calibration table | 19 |
| `touch.h` | 5,011 | | 19 |
| `calib.c` | 10,348 | Four inset targets, the pair check, the fit | 19 |
| `calib.h` | 1,407 | | 19 |
| `gpio.h` | 8,237 | Two-bank accessors, IO_MUX table, pin interrupts, the measured bit 15 | 19, 22 |
| `flash.c` | 11,450 | SPI1 user mode, register save/restore, the explicit clock divider | 20 |
| `flash.h` | 2,697 | | 20 |
| `store.c` | 3,902 | The 13-word record, checksum, version, one-writer discipline | 20 |
| `store.h` | 3,148 | | 20 |
| `sd.c` | 9,550 | Bit-banged SPI card init and block read | 21 |
| `sd.h` | 3,950 | Seven per-stage error codes, the SPI-mode argument | 21 |
| `intr.c` | 8,853 | Routing, dispatch, the hang defence, `intr_selftest` | 22 |
| `intr.h` | 4,313 | Sources vs lines | 22 |
| `adc.c` | 17,585 | SAR ADC1 and four probes | 22 |
| `adc.h` | 3,597 | | 22 |
| `i2c.c` | 9,242 | Bit-banged master, clock stretching, the two-direction self-test | 22 |
| `i2c.h` | 3,615 | | 22 |
| `audio.c` | 12,615 | LEDC PWM, the DAC post-mortem, probes with their limits printed | 23 |
| `audio.h` | 3,519 | | 23 |
| `uart.c` | 4,387 | TX FIFO, the AHB receive alias, the output tee | 12, 26 |
| `uart.h` | 635 | | 12 |
| `console.c` | 433 | Message-granularity console lock | 11 |
| `console.h` | 1,335 | | 11 |
| `efuse.c` | 2,910 | Factory MAC, verified against the on-chip CRC8 | 27 |
| `efuse.h` | 848 | | 27 |
| `phyinit.c` | 5,098 | The radio blob's clock, init data, calibration buffer | 27 |
| `phyinit.h` | 264 | | 27 |
| `wifimac.c` | 44,812 | MAC init, liveness by motion, TSF, rx/tx chains, beacon decode | 27 |
| `wifimac.h` | 6,579 | The behavioural TSF identification | 27 |
| `wifi_osi_impl.c` | 14,972 | Static pools, tagged handles, the call0 side of the OSI table | 27 |
| `wifi_osi_impl.h` | 2,228 | | 27 |

### Interface

| File | Bytes | Contents | Chapter |
|---|---|---|---|
| `desktop.c` | 24,845 | Icon grid, cursor, press latching, chrome, the overlay, six assertions | 24 |
| `desktop.h` | 5,981 | The launcher-not-window-manager argument, the cursor argument | 24 |
| `raycast.c` | 22,003 | Fixed-point marcher, face shading, wall seams, navigation, framebuffer | 25 |
| `raycast.h` | 3,024 | | 25 |
| `notes.c` | 15,089 | Multi-tap keypad, compose and inbox views | 26 |
| `notes.h` | 2,101 | The native-code limitation | 26 |
| `messages.c` | 3,193 | Flash-backed message store, separate sector | 26 |
| `messages.h` | 1,929 | | 26 |
| `term.c` | 15,002 | On-panel shell: layout, ring scrollback, keypad, three assertions | 26 |
| `term.h` | 1,676 | | 26 |
| `shell.c` | 53,226 | ~70 commands, `execute()`, `shell_run_line()` | 12, 16, 26 |
| `shell.h` | 1,921 | The program table type | 16 |

### Failure handling

| File | Bytes | Contents | Chapter |
|---|---|---|---|
| `panic.c` | 8,141 | Two entry points, record-before-report, the panel screen, `halt_forever` | 12 |
| `panic.h` | 790 | | 12 |
| `watchdog.c` | 5,108 | Disable-all, TIMG0 arm/feed/disarm, liveness on distinct switches | 12 |
| `watchdog.h` | 2,284 | | 12 |

### Kernel entry

| File | Bytes | Contents | Chapter |
|---|---|---|---|
| `kmain.c` | 74,035 | Nine tasks, six self-tests, the boot report, the colour strip, the reporter | 6, 8, 9, 16 |

### Generated

`kernel/generated/` holds build products, one header per `tools/*.vasm` plus the
sine table. Never edited; regenerated on every build.

```
app_a.h  app_b.h  app_blit.h  app_draw.h  app_gfx_rogue.h  app_paint.h
app_ping.h  app_pong.h  app_rogue.h  demo.h  sintab.h  spin.h
```

---

## A.2 Tools

| File | Lines | Contents | Chapter |
|---|---|---|---|
| `vasm.py` | 381 | Two-pass bytecode assembler; emits a C header with label offsets | 15 |
| `gen_sintab.py` | 33 | Fixed-point sine table generator | 25 |
| `demo.vasm` | 49 | `sum(1..10)` through memory, a call, four syscalls | 15 |
| `spin.vasm` | 23 | The kernel's hosted program: a bounds-checked store per iteration | 15 |
| `app_a.vasm` | 13 | Counter, 3 instructions per iteration | 15, 16 |
| `app_b.vasm` | 17 | Squares, 4 instructions per iteration | 15, 16 |
| `app_rogue.vasm` | 31 | Walks a store off the end of its arena | 15, 16 |
| `app_gfx_rogue.vasm` | 38 | Asks to fill the whole panel, forever | 15, 17 |
| `app_paint.vasm` | 40 | The first interactive program | 15, 17 |
| `app_blit.vasm` | 46 | Builds an image and blits it | 15, 17 |
| `app_draw.vasm` | 49 | Draws text and rectangles | 15 |
| `app_ping.vasm` | 29 | Sends a counter to application 1 | 15, 16 |
| `app_pong.vasm` | 47 | Receives and replies | 15, 16 |

---

## A.3 Vendor

| Path | Bytes | Contents | Chapter |
|---|---|---|---|
| `vendor/bootloader.bin` | 17,536 | ESP-IDF second-stage bootloader, Apache 2.0 | 3 |
| `vendor/partitions.bin` | 3,072 | Partition table, Apache 2.0 | 3 |
| `vendor/README.md` | 1,915 | What they are, where they came from, how to rebuild them | 3 |
| `vendor/windowed/` | — | `-mabi=windowed` C objects: the OSI table, the ROM-symbol answers | 2, 27 |
| `vendor/phy/` | — | `libphy_natos.a`, `libpp_natos.a` — patched Espressif archives | 5, 27 |

---

## A.4 Build and docs

| Path | Contents | Chapter |
|---|---|---|
| `build.ps1` | Assemble bytecode → compile call0 → compile windowed → link → elf2image → flash | 5 |
| `README.md` | Project overview, the two shaping decisions, the shell table | — |
| `LICENSE` | MIT | — |
| `docs/README.md` | Report index, status table, twelve standing rules | 29 |
| `docs/UM-NATOS-0NN-*.md` | Twenty-eight engineering reports | Appendix E |
| `docs/pdf/` | Rendered PDFs of every report | — |
| `docs/style/` | `build_pdfs.py`, `figures.py`, `report.css` | — |
| `docs/book/` | This book | — |

---

## A.5 The ten largest files, and what that says

| Rank | File | Bytes |
|---|---|---|
| 1 | `kmain.c` | 74,035 |
| 2 | `shell.c` | 53,226 |
| 3 | `wifimac.c` | 44,812 |
| 4 | `display.c` | 32,900 |
| 5 | `task.c` | 26,656 |
| 6 | `desktop.c` | 24,845 |
| 7 | `vm.c` | 23,405 |
| 8 | `raycast.c` | 22,003 |
| 9 | `touch.c` | 21,247 |
| 10 | `window.S` | 18,744 |

Two observations.

**`kmain.c` and `shell.c` are the two largest files and neither is a subsystem.**
They are, respectively, the boot report plus six self-tests, and roughly seventy
diagnostic commands (UM-NATOS-026 counted 25 when it was written; the growth
since is almost entirely the bring-up probes of Chapters 22, 23 and 27). About a
fifth of the kernel's source is instrumentation — which, given Chapter 28, is the
correct proportion.

**`task.c` at 26,656 bytes implements a scheduler in 689 lines**, of which a
substantial fraction is comment. The kernel proper — `start.S`, `vectors.S`,
`task.c`, `timer.c`, `heap.c`, `arena.c`, `mutex.c`, `critical.h` — is under
55 KB of source and compiles to a few thousand bytes of text.
