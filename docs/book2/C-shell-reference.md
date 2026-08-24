# Appendix C — Shell Command Reference

The shell is a native task polling UART0, and — since Chapter 26 — the same
`execute()` reached from an on-panel keypad. **One command set, not two.**

Over serial at 115200. On the panel, via the `shell` icon.

UM-NATOS-026 records "all 25 commands reachable without a host" as the metric at
the time it was written. The table has since grown to roughly seventy, most of
them the bring-up probes of §C.8 — the claim that matters is unchanged, because
there is still exactly one command set.

---

## C.1 Everyday commands

| Command | Effect |
|---|---|
| `help` | The command list |
| `ps` | Applications: id, name, state, arena size, instructions retired, published value, fault diagnosis |
| `progs` | Loadable programs with image and arena sizes |
| `run <name>` | Start a program **by name**, never by index |
| `kill <id>` | Stop an application and release its arena |
| `mem` | `free`, `largest`, `blocks`, `high_water`, `check` |
| `stacks` | Per-task stack headroom, in bytes free of 2,048 |
| `3d [off]` | Hand the region to the raycaster, or back to the launcher |
| `fb [on\|off]` | Framebuffer for the 3D view |

`mem`'s three numbers are chosen so no one of them can hide a problem the others
would show: `largest` beside `free` is the fragmentation indicator, and `check`
beside both is the structural one.

## C.2 Storage

| Command | Effect |
|---|---|
| `sd` | Probe the microSD card; reports the failing stage by name if it fails |
| `sdread <lba>` | Read and dump one 512 B block, decoding an MBR or FAT signature |

`sdread 0` proves the bus. `sdread 240` proves the addressing mode — see
Chapter 21 §21.5 for why those are different claims.

## C.3 Touch

| Command | Effect |
|---|---|
| `cal` | Run the four-target calibration on the panel |
| `calshow` | The last run's outcome, all four readings, and the calibration in use |
| `taps` | Dump the touch press log — raw and mapped coordinates, pressure |
| `tapsclear` | Empty it |
| `touchcfg <prio> <sleep>` | Retune touch scheduling without a reflash |

`calshow` exists because the calibration's result line is printed the instant the
fourth target is tapped — "exactly when nobody has a capture attached".

`touchcfg` is the runtime-tunable knob that turned four hypotheses into one flash
(Chapter 27 §27.11).

## C.4 Sensors and buses

| Command | Effect |
|---|---|
| `adc` | Read every ADC1 channel |
| `ldr` | Watch the light sensor only |
| `ldrscan` | Watch all eight channels at once — the four touch pins are the control group |
| `i2c` | Self-test both lines in both directions, then scan `0x08`–`0x77` |
| `intr` | Interrupt matrix counters: per-line service counts, spurious, disabled mask |
| `tone <hz>` | Tone on GPIO26. `tone 0` stops. **Try 3000, not 440** |
| `beep` | A short 3 kHz beep |
| `light [threshold]` | One light reading, and a beep if it is below the threshold |

`light` is what the `dev` demo application used to do as a resident loop, which
meant the board beeped at the room whenever a shadow crossed it. A demo should
demonstrate and then get out of the way; a thing you want on demand belongs at
the prompt. Both halves go through the device table, so it exercises the same
path an application takes rather than a private shortcut.

## C.4b Devices and permissions

| Command | Effect |
|---|---|
| `dev` | List every device: channels, flags, and a sample of channel 0 |
| `dev <id> <chan>` | Read one channel |
| `dev <id> <chan> <value>` | Write one channel |
| `perms` | List every running application and the devices it may touch |
| `perms <app> <dev> on\|off` | Grant or revoke one device, on a *running* application |

`dev` goes through `device_read`/`device_write` — the same entry points an
application uses — rather than calling the drivers underneath. A diagnostic that
bypassed the table would report on a path no application can take, which is how
a self-test ends up passing for a broken system.

It never samples a device whose read **consumes**. Listing the table used to pop
a keypress off `keys`, which is a diagnostic quietly altering the thing it
reports; those show as `(consumes)` instead of a value.

`perms` lists devices **by name rather than as a hex mask**, because a mask is
exactly the kind of thing that gets misread on the wrong day. The mutator exists
to make refusals testable — revoke a device from a program that is using it and
the denial counter climbs while the program keeps running.

Note what is absent: **no program can grant itself anything.** There is no
`sys device` operation that reaches `device_grant()`.

`i2c` refuses to let a scan be believed unless the bus test passed:

```
bus looks sane; a scan can be believed
```
or
```
bus is NOT sane; ignore any scan result
```

## C.5 Deliberate failure

| Command | Induces | Expected |
|---|---|---|
| `hang` | Masks interrupts, spins | Watchdog resets the board — `rst:0x7` |
| `fault` | Executes `ill` | Panic, halt, evidence retained on serial, flash and panel |
| `smash` | Clobbers the running task's guard word | Panic at the next switch, **naming the task** |

> The last three exist on purpose. A recovery path that has never been observed
> to fire is confidence without evidence.

## C.6 Mixed-ABI and vendor code

| Command | Effect |
|---|---|
| `wintest [n]` | Call windowed-ABI code at recursion depth n; the return value is a checksum over every handler invocation |
| `vendorcall [n]` | Call a `-mabi=windowed` object built by the vendor compiler |
| `romcall` | Call `crc32_le` at `0x4005CFEC` in the ESP32 ROM |
| `bridgetest` | A windowed function calling *back* into call0 code every iteration |

Four escalating proofs, each establishing something the previous one did not
(Chapter 2 §2.7).

## C.7 WiFi

| Command | Effect |
|---|---|
| `phyver` | PHY version, from the blob's own printf shim |
| `phyinit` | Ungate the radio clock and call `register_chipv7_phy` |
| `macinit` | Bring up the MAC (needs `phyinit` first) |
| `maclive` | Which MAC registers move on their own — liveness by motion |
| `mactsf` | TSF rate, counted against the CPU's own cycle counter |
| `macaddr` | Factory MAC from eFuse, verified against its stored CRC8 |
| `macirq` | Route the MAC interrupt onto a CPU line |
| `macrx` | Arm the receiver, promiscuous |
| `chan <1..13>` | Tune a channel |
| `scan` | Decoded beacons: BSSID, SSID, count |
| `macstat` | Frame counters, recycled descriptors |
| `beacon` / `beaconoff` | Transmit a discoverable SSID |
| `probe` | Send probe requests and count frames addressed to us |
| `txstat` | Frames handed to hardware / completions reaped / forced |
| `txpwr` | Read and set `most_tpw` |
| `hwinit` | The MAC hardware init chain |
| `ositest` | Drive the OSI table through its function pointers, windowed→call0 and back |

`probe` is the unforgeable transmit test: a probe *response* addressed to this
station cannot be produced by any local fault.

## C.8 Bring-up probes

Grouped separately in `help`, with a warning:

```
  bring-up probes (they poke at live hardware):
```

| Command | Effect |
|---|---|
| `irqtest` | Inject an edge per candidate INT_ENA bit; report which reaches this CPU |
| `irqpoke` | Inject one GPIO edge without touching the pin's configuration |
| `adcprobe` | Sweep the RTC sensor-pad mux |
| `adcconv` | Prove a conversion actually runs — `DONE` low, then high 13 spins later |
| `adcdrive` | Sweep against GPIO32, a pin this kernel **drives**. Also drives touch MOSI |
| `findspk` | Square wave on each candidate speaker pin |
| `spktest` | Prove the square-wave generator works — **and prints its own blind spot** |

`irqtest` re-derives the PRO CPU enable bit on any board rather than trusting a
comment. `adcdrive` commands the input voltage rather than observing it.

## C.9 Display and measurement

| Command | Effect |
|---|---|
| `spiclk 0\|1\|2` | Panel clock: 40 / 20 / 10 MHz. Returns the register read back |
| `dfreeze` | Stop every drawer in the display task, leaving the buffer static |
| `fbsum` | Sample the framebuffer — used with `dfreeze` and a control reading |
| `blittest` | Static colour-bar test pattern through the raycaster's exact blit call |
| `resync` | Re-issue the panel window and end the transaction, drawing nothing |
| `fifopoke` | A 16-byte FIFO transfer, bypassing DMA |
| `sleeptest` | Measure what `task_sleep` actually costs |
| `tickrate` | Ticks against wall clock |
| `efusedump` | eFuse block 0 |
| `audio` | LEDC register read-back, including the clock gate |

Four of these — `spiclk`, `dfreeze`, `blittest`, `resync` — were built during a
single investigation and are the reason it produced a positive result. Chapter 27
§27.13 names the method:

> Make the variable runtime-tunable before forming a theory about it.

### C.9.1 The one-bit investigation's instruments

Ten more were added across the two sessions of Chapters 28b and 28c. All are
read-only or opt-in; none changes an existing code path unless invoked. They are
grouped here because their *design* is the interesting part: the first seven test
hypotheses, and the last three extract state — and it was the last three that
found the defect.

| Command | Effect |
|---|---|
| `dmastat` | DMA transfer and timeout counters from a **running** system, not from the boot self-test |
| `view3d` | Open or close the 3D view from the terminal, so the moment of opening belongs to the test |
| `campos` | Camera cell, sub-cell position, heading, and whether `wall_at()` says it is inside geometry |
| `camfreeze` | Hold the viewpoint still while the renderer keeps drawing every frame |
| `hog` / `hog draw` | Spin like `task_apps`; `draw` also fills continuously through `display_try_lock()` |
| `resyncn <n>` | A burst of `n` window setups, no pixels |
| `stripn <n>` | `gfxrogue`-shaped clipped fills without `gfxrogue` — same colours, same bursts |
| `fbsum` | Now also emits `fbhash`, FNV-1a over all 53,760 pixels |
| `fbpattern` | Eight flat colour bands through the identical `display_blit()`, renderer frozen |
| `fbdump` | The framebuffer out over the UART as indexed hex rows, terminated by `FBEND` |

`tools/serial/grab_fb.py` is the host half of `fbdump`: it reads to the
terminator, refuses to interpret an incomplete capture, and reconstructs a PNG.

Three notes that are really method, kept here because a reader reaching for one
of these commands is the person who needs them:

- **`view3d` exists because a startup fault needs the instrument to exist before
  the first frame does.** Every earlier attempt opened the view by hand, and the
  picture had healed before anything could be armed.
- **`camfreeze` is not `dfreeze`.** One freezes the camera and keeps rendering;
  the other stops the display task and freezes the buffer with it. Using the
  second where the first is meant answers nothing.
- **`resyncn` and `stripn` are a matched pair.** They put byte-identical traffic
  on the wire and differ only in who emits it, which is what made "the repair is
  not about the pixels" a measurement rather than an opinion.

## C.10 Behaviour

**Every command is parsed once.** `shell_run_line()` copies a caller's string into
the shell's own buffer and calls `execute()` — the same path a serial line takes.
A line longer than the buffer is **refused, not truncated**, because "a truncated
command is a different command, and `kill 12` truncated to `kill 1` kills
something".

**One command, one uninterrupted response.** `execute()` holds the console lock
for its whole run, so a telemetry line cannot land in the middle of a table.

**Unknown commands are rejected, not ignored.**

**`shell_poll()` never blocks**, so a user holding a key cannot starve the system.

**Receive is polled, not interrupt-driven** — "a console that drops a keystroke
under load is a better outcome than a second interrupt source competing with the
scheduler tick".

**Enter works.** It did not, for the shell's entire existence before Chapter 12
§12.5, because the RX FIFO was read through the APB address rather than its AHB
alias and the path ran exactly one byte behind.
