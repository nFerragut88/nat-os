# next_moves

Where the work stands and what to do next, written to survive losing whatever
conversation produced it.

**Written 2026-08-18**, after the session that fixed the display DMA bug and
built the device model. If the tree has moved on, trust `git log` and the
numbered UM-NATOS reports over this file.

---

## Context, short form

nat-os is a from-scratch operating system for the ESP32-2432S028R ("Cheap
Yellow Display"). No ESP-IDF, no FreeRTOS. Preemptive scheduler, own heap, own
bytecode VM with isolated applications, own drivers for display, touch, SD,
audio, ADC, I²C, flash and 802.11 receive.

Public at **github.com/nFerragut88/nat-os**. Everything below is on `main`.

### The three things that constrain everything

1. **`-mabi=call0`.** The kernel is call0; Espressif's blobs are windowed.
   Bridges live in `kernel/window.S` — `w2c_callN` for windowed→call0,
   `phy_stack_call` for call0→windowed on a private 6 KB stack. This is
   permanent and is why WiFi/BT integration is hard.
2. **Naming a symbol changes the link.** Referencing something not already in
   the image pulls fresh objects out of `libphy.a`/`libpp.a` and has broken
   working code — `register_chipv7_phy` died inside a calibration function
   nobody touched. **Check with `nm` that a symbol is already linked before
   calling it.**
3. **A successful register write is not evidence.** This kernel has been caught
   at least four times by hardware that reads back exactly what was written
   while doing nothing. Measure behaviour, not stores.

### Build and flash

```powershell
cd C:\Users\nobod\Projects\nat-os
.\build.ps1 -Flash              # builds, assembles .vasm, flashes COM5
.\build.ps1                     # build only, no serial needed
```

The link emits ~20 "incompatible Xtensa configuration (ABI does not match)"
warnings. **Those are expected** — mixed call0/windowed objects — and are not a
problem.

### Talking to the board

`tools/serial/` holds the helpers (rescued from a scratchpad, so they are
plain and unpolished):

```powershell
python tools\serial\send_live.py COM5 "dev" 3        # command a RUNNING board
python tools\serial\send_cmd.py  COM5 22 "dev" 3     # reset, wait 22s, command
python tools\serial\grab_fb.py   COM5 fb.png         # framebuffer -> PNG
```

`send_live.py` never touches DTR/RTS — opening a Windows serial port normally
asserts DTR, which resets the board and destroys the state you came to read.
`send_cmd.py` resets deliberately.

**COM5 sometimes disappears for a few seconds after a flash.** That is the CH340
re-enumerating; wait and retry.

### The shell is the interface

Type `help` on the serial console. The diagnostics that matter most:

| command | what |
|---|---|
| `dev` | list the device table with live readings |
| `dev <id> <chan> [val]` | read/write a device |
| `devw` / `devr` | bulk transfer in/out |
| `vmargtest` | argument-harness self-test, 12/12 expected |
| `dmastat` | DMA counters from a **running** system |
| `fbdump` | framebuffer as hex, for `grab_fb.py` |
| `fbpattern` | known image through the real blit path |
| `camfreeze` / `campos` | hold the 3D camera still / where it is |
| `view3d` | open the 3D view from the terminal |
| `phyinit` → `macinit` → `chan 1` → `macrx` | WiFi bring-up, in that order |
| `probe` / `txstat` / `macstat` | WiFi transmit and receive state |

---

## What is true right now

**Working and verified on hardware:** display (DMA path correct as of today),
touch, SD card, audio, ADC, I²C, flash persistence, the scheduler, the VM,
application isolation, the device model with seven devices, and **802.11
receive** — real beacons decoded continuously.

**Not working:** **802.11 transmit.** The MAC accepts frames and reports every
one complete; nothing on Earth hears them.

**Applications can now do everything on the old capability list except use the
network.**

---

## The moves

Each has its own file. Roughly in order of value, but they are independent.

| # | move | size | blocked on |
|---|---|---|---|
| [01](01-wifi-transmit.md) | Make transmit reach the air | large, risky | accepting a link change |
| ~~[02](02-vm-events-and-frames.md)~~ | ~~VM entry points~~ — **done**, UM-NATOS-031 rev 1.1 | — | — |
| ~~[03](03-permissions.md)~~ | ~~Per-application device permissions~~ — **done**, UM-NATOS-032 | — | — |
| [04](04-scheduler-timing.md) | A real-time path for control loops | medium | nothing needs it *yet* |
| [05](05-open-unknowns.md) | Two unexplained behaviours | small | nothing |
| [06](06-documentation-debt.md) | Claims that have gone stale | small | nothing |

02 and 03 are done. Of what is left:

**If you only do one thing:** [06](06-documentation-debt.md). It is the smallest
and it is the one that decays — every report written on top of a stale claim
inherits it.

**If you want the satisfying one:** [01](01-wifi-transmit.md), knowing it may
cost a working receive path and a day.

**Note for 03's successor:** permissions shipped with a named prerequisite.
There is no image identity — no signature, no content hash — so what exists is
containment, not security, and the `store` device's per-slot banks have the same
gap. UM-NATOS-032 §3 states both. Image identity closes both and is not on this
list yet.

---

## Standing rules this project has paid for

These are in the reports; collected here because they are easy to lose.

1. **A counter cannot see a picture.** Six instruments agreed the display was
   working while the panel was garbled. Get visual output out of the machine
   and look at it (`fbdump`).
2. **A guard can only be shown unnecessary by measuring it in its absence.**
   A timeout bound was raised, the counter read zero, and that was taken as
   proof the raise was unneeded — circular; the raise is *why* it read zero.
3. **A diagnostic must use the path an application uses.** `beep` called the
   driver directly and was reporting on a route no program could take.
4. **A diagnostic must not alter what it reports.** Listing the device table
   used to consume a keypress.
5. **Read positions, never infer them from arrival order.** Indexed rows caught
   what a silent parser hid twice.
6. **Refuse rather than clamp.** A silently shortened transfer is a program
   being lied to.
7. **Record dead theories.** Eleven were eliminated for the display bug; an
   untested lead nobody wrote down gets retried in three weeks.
8. **Ports prove an abstraction; purpose-built code does not.**
