# nat-os

An operating system written from scratch for the ESP32. No ESP-IDF, no Arduino,
no FreeRTOS, no C library — the kernel owns scheduling, memory, drivers and
application execution.

Developed and verified on the ESP32-2432S028R, the board commonly sold as the
"Cheap Yellow Display", and that is the only hardware any measurement in this
repository was taken on. Nothing above the drivers is specific to it: the
scheduler, heap, arena model, bytecode VM and application model assume an ESP32
and nothing more. The board-specific parts are the pin maps and the panel and
touch controllers, and they are confined to their own files.

Only the second-stage bootloader and partition table are borrowed (`vendor/`),
and both are replaceable. Every instruction from the image entry point onward is
project code.

## What works

| | |
|---|---|
| **Scheduling** | Preemptive, priority with ageing so no ready task waits more than ~600 ms, blocking, sleeping, priority inheritance |
| **Memory** | Bump-and-free heap, per-application arenas, bounds-checked at every access |
| **Applications** | Register-based bytecode VM, 35 opcodes, 12 syscalls; faults contained, runaway programs bounded |
| **Display** | ILI9341 over SPI2 with DMA; per-application viewports that cannot be escaped |
| **Input** | XPT2046 touch, gated on PENIRQ *and* pressure, confined per application |
| **UI** | Touch launcher — icon grid, cursor, double-tap to start a program |
| **Graphics** | Raycast 3D view at 16 fps, optional framebuffer |
| **Storage** | Flash record surviving power cycles; microSD read over SPI |
| **Failure** | Stack guards enforced per switch, hang detector, panic to serial *and* flash *and* the panel |

Image: 37,248 bytes. Roughly 145 KB of the 180 KB DRAM is left for applications.

## Two decisions that shape everything

**`-mabi=call0`.** The Xtensa windowed ABI makes a context switch require
spilling live register windows, which is where from-scratch Xtensa kernels
usually stall. call0 removes register windows entirely, reducing the switch to a
conventional register save. The cost is that ROM routines (which are windowed)
cannot be called — irrelevant here, since the kernel writes its own drivers.

**A bytecode VM for applications.** The ESP32 has no MMU paging, so hardware
memory protection between processes is impossible. Running applications in an
interpreter whose loads and stores are bounds-checked recovers that guarantee in
software, and makes preemption at instruction boundaries trivial. An application
deliberately written to escape its arena is part of the test suite; it cannot.

Both are argued in full under `docs/`.

## Build

Requires **PlatformIO** installed — for its toolchain only, not as a build
system. The build uses `xtensa-esp32-elf-gcc` and `esptool.py` from PlatformIO's
package directory and nothing else.

```powershell
.\build.ps1                              # build
.\build.ps1 -Flash -Port COM5            # build and flash
.\build.ps1 -Flash -Monitor -Port COM5   # build, flash, attach monitor
```

The bootloader and partition table come from `vendor/`; pass `-Vendor <path>` to
use your own. See `vendor/README.md` for what they are and how to rebuild them.

Boot output is best captured with the port opened *before* reset — the kernel
prints within milliseconds of the jump, and attaching afterwards loses the
banner. Note that opening a serial port on Windows asserts DTR, which resets the
board; deassert `dtr` and `rts` *before* `open()`. UM-NATOS-017 §5 is the story
of learning that twice.

## Shell

Over serial at 115200:

```
fb [on|off]   framebuffer for the 3D view      sd            probe the microSD card
ps            list applications                sdread <lba>  dump one 512 B block
progs         list loadable programs           taps          recent touch presses
run <name>    start a program                  stacks        per-task stack headroom
kill <id>     stop an application              3d            switch launcher / 3D view
mem           heap statistics                  hang          wedge the kernel (watchdog resets)
help          this                             fault         illegal instruction (panics)
                                               smash         break a stack guard (panics)
```

The last three exist on purpose. A recovery path that has never been observed to
fire is confidence without evidence.

## Layout

```
kernel/
  start.S vectors.S   entry, exception/interrupt vectors, panic entry
  task.c timer.c      scheduler, context switch, tick
  heap.c arena.c      allocator and per-application arenas
  vm.c app.c ipc.c    bytecode interpreter, application lifecycle, messaging
  display.c raycast.c ILI9341 driver, 3D renderer
  touch.c desktop.c   XPT2046, touch launcher
  flash.c store.c sd.c persistence and removable storage
  mutex.c critical.h  locking
  panic.c watchdog.c  failure handling
  linker.ld           memory map
docs/                 engineering reports — see docs/README.md
tools/                host-side bytecode assembler and program sources
vendor/               the two borrowed binaries
```

## Documentation

`docs/` holds 21 engineering reports. Start with `docs/README.md` for the index
and reading order.

They are written to be read by someone picking the project up cold, and they
follow two rules that are unusual enough to mention:

**Measured is separated from assumed.** On a from-scratch kernel the difference
between "this is true" and "this should be true" is the difference between a
working boot and a silent reset, so claims verified on hardware are marked as
such and claims taken from documentation are marked separately.

**Every report ends with what it does *not* establish.** Those sections are the
most useful part. They are where the known gaps live, and several of them
correctly predicted the next defect.

The reports also record the defects honestly, including the embarrassing ones —
a touch axis that was inverted for three months behind a calibration that could
only ever give one answer, a tick deadline that raced into the future on every
yield, an idle task that silently failed to be created. The failures are more
instructive than the successes and are written up that way.

## Licence

MIT — see `LICENSE`. The two binaries in `vendor/` are unmodified ESP-IDF
artefacts, copyright Espressif, redistributed under Apache 2.0.
