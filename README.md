# nat-os

An operating system written from scratch for the ESP32. No ESP-IDF, no Arduino,
no FreeRTOS, no C library — the kernel owns scheduling, memory, drivers and
application execution.

Developed and verified on the ESP32-2432S028R, the board commonly sold as the
"Cheap Yellow Display", and that is the only hardware any measurement in these
documents was taken on. Nothing above the drivers is specific to it: the
scheduler, heap, arena model, bytecode VM and application model assume an ESP32
and nothing more. The board-specific parts are the pin maps and the panel and
touch controllers, and they are confined to their own files.

The project was called `cyd-os` until the drivers stopped being the interesting
part. Document numbering is unchanged, so `UM-CYDOS-014` in an older commit
message is `UM-NATOS-014` here.

Only the second-stage bootloader and partition table are borrowed, and both are
replaceable. Every instruction from the image entry point onward is project
code.

## Status

| Milestone | State |
|---|---|
| M0 — kernel boots, self-checks pass | **Complete, verified on hardware** |
| M1 — timer interrupt, tick counter | **Complete, verified on hardware** |
| M2 — two native tasks, preemptive switching | Next |
| M3 — heap and VM memory model | — |
| M4 — bytecode interpreter | — |
| M5 — two VM applications, isolated | — |

Current image: 3,424 bytes.

## Two decisions that shape everything

**`-mabi=call0`.** The Xtensa windowed ABI makes a context switch require
spilling live register windows, which is where from-scratch Xtensa kernels
usually stall. call0 removes register windows entirely, reducing the switch to a
conventional register save. The cost is that ROM routines (which are windowed)
cannot be called — irrelevant here, since the kernel writes its own drivers.

**A bytecode VM for applications.** The ESP32 has no MMU paging, so hardware
memory protection between processes is impossible. Running applications in an
interpreter whose loads and stores are bounds-checked recovers that guarantee in
software, and makes preemption at instruction boundaries trivial.

Both are documented in full under `docs/`.

## Build

Requires PlatformIO installed — for its toolchain only, not as a build system.

```powershell
.\build.ps1                              # build
.\build.ps1 -Flash -Port COM5            # build and flash
.\build.ps1 -Flash -Monitor -Port COM5   # build, flash, attach monitor
```

Boot output is best captured with the port opened *before* reset; the kernel
prints within milliseconds of the jump and attaching afterwards loses the
banner. See UM-NATOS-005 §8.

## Layout

```
kernel/
  start.S      entry stub — stack, .bss clear, into C
  vectors.S    exception/interrupt vectors, level-3 entry/exit, panic entry
  xtensa.h     special-register accessors with required synchronisation
  timer.c      CCOMPARE1 tick
  panic.c      fault decode and report
  uart.c       register-level UART0, no ROM calls
  kmain.c      kernel entry
  linker.ld    memory map
docs/          engineering reports — see docs/README.md
build.ps1      compile, link, package, flash
```

## Documentation

`docs/` holds the engineering report set (UM-NATOS-001 … 008). Start with
`docs/README.md` for the index and reading order.

Reports separate **measured** from **assumed**. On a from-scratch kernel the
difference between "this is true" and "this should be true" is the difference
between a working boot and a silent reset, so claims verified on hardware are
marked as such and claims taken from documentation are marked separately.
