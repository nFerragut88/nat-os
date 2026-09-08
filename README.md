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
| **Applications** | Register-based bytecode VM, 35 opcodes, 14 syscalls; faults contained, runaway programs bounded |
| **A language** | **NatScript** — compiler, 27-case test suite, host reference VM. Variables, functions, recursion, buffers, strings, devices by name, `when`/`every` |
| **Devices** | A device table reached by NAME, with a per-program manifest the image declares and the loader resolves |
| **Display** | ILI9341 over SPI2 with DMA; per-application viewports that cannot be escaped |
| **Input** | XPT2046 touch, gated on PENIRQ *and* pressure, confined per application |
| **UI** | Touch launcher; a program can own the main 240x202 region, with the way out drawn where it cannot reach |
| **Graphics** | Raycast 3D view at 16 fps, optional framebuffer |
| **Storage** | Flash records surviving power cycles; microSD read over SPI |
| **Networking** | WPA2-PSK, DHCP, a DNS resolver and a TCP/HTTP client — see the caveat below |
| **Failure** | Stack guards enforced per switch, hang detector, panic to serial *and* flash *and* the panel |

Image: 235,600 bytes. Heap at boot: 29,240 bytes, all of it in one block.

**The networking is real and is not in the default build.** It was verified from
another machine — `HTTP 200`, ping, the board's MAC in the router's ARP table —
and it lives behind `-WiFi`, which produces a 301 KB image with 32,856 bytes of
heap at boot. Everything else in this table is in the image you get from
`.\build.ps1`. See UM-NATOS-054 to 057 for how the stack was built and where its
memory went, and step 374 in `docs/next_moves/08` for what had to move to make
that build link again.

## Writing a program

Applications are bytecode. They can be written in assembly (`tools/*.vasm`) or
in NatScript (`tools/*.nat`), which compiles to that assembly and then through
the same assembler — one encoder of the instruction set, not two.

```
permissions { light }

buf line[32]

let peak = 1

every 100ms {
    if light.read(0) {
        let v = light.value
        if v > peak { peak = v }
        format(line, "light ", v, "   peak ", peak)
        screen.text(line, 2, 4, 0xFFFF, 0x0000, 1)
    }
}
```

That is most of `tools/app_meter.nat`, which runs full-screen from an icon on
the board. Three properties are worth knowing before reading further:

- **`permissions` is the manifest and the device namespace.** A device that is
  not declared is not a name the program can write — a compile error, not a
  runtime refusal. The names are resolved to device ids **at load, by name**, so
  no program contains a device index.
- **Every load and store is bounds-checked**, and a buffer's value is simply its
  arena offset. That is the whole of what a pointer is here.
- **Coordinates are viewport-relative and clipped by the kernel.** A program
  cannot draw outside its region and cannot discover where its region is.

`docs/natscript.md` is the language reference; `docs/vm-abi.md` is the frozen
instruction and syscall ABI a compiler may depend on.

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

The build compiles `tools/*.nat` with `tools/natc.py`, assembles the result and
every `tools/*.vasm` with `tools/vasm.py`, and runs the compiler's own test
suite **before** using it — a failing suite fails the build.

The bootloader and partition table come from `vendor/`; pass `-Vendor <path>` to
use your own. See `vendor/README.md` for what they are and how to rebuild them.

### Talking to the board

```
python tools/board.py ports
python tools/board.py flash
python tools/board.py run "ps" "run meter"
python tools/board.py watch 60
```

Use this rather than a serial monitor for anything you intend to believe.
Opening the port usually resets the board, so a command sent too early is simply
lost; `board.py` probes for the prompt first, keeps what it captured if the link
drops, names the process holding the port when it cannot open it, and treats
esptool's own verification line as the only evidence of a successful flash.

Every one of those behaviours exists because its absence once cost a day —
UM-NATOS-059 §7 is the list, and `tools/board.py`'s own header repeats it.

## Shell

Over serial at 115200:

```
ps            list applications                mem           heap statistics
progs         list loadable programs           stacks        per-task stack headroom
run <name>    start a program                  taps          recent touch presses
kill <id>     stop an application              sd            probe the microSD card
light         read the light sensor            sdread <lba>  dump one 512 B block
dev <id> <ch> read a device                    3d            switch launcher / 3D view
fb [on|off]   framebuffer for the 3D view      help          this
              hang / fault / smash             break the kernel, on purpose
```

`help` lists the rest; there are considerably more, most of them bring-up
instruments for one driver or another.

The last three exist on purpose. A recovery path that has never been observed to
fire is confidence without evidence.

`run tamper` exists on purpose too, and must always be **refused**: it is a
program table entry carrying one program's image and another's identity, kept so
the manifest check is seen to work rather than assumed to.

## Layout

```
kernel/
  start.S vectors.S   entry, exception/interrupt vectors, panic entry
  task.c timer.c      scheduler, context switch, tick
  heap.c arena.c      allocator and per-application arenas
  vm.c app.c ipc.c    bytecode interpreter, application lifecycle, messaging
  device.c            the device table programs reach by name
  display.c raycast.c ILI9341 driver, 3D renderer
  touch.c desktop.c   XPT2046, touch launcher, full-screen application regions
  keyboard.c term.c   the on-screen keyboard and the terminal that uses it
  net.c wifi_*.c      WPA2 supplicant, driver shim and network path (-WiFi only)
  flash.c store.c sd.c persistence and removable storage
  mutex.c critical.h  locking
  panic.c watchdog.c  failure handling
  generated/          bytecode headers — build products, not sources
  linker.ld           memory map, and which objects live in flash rather than RAM
docs/                 engineering reports — see docs/README.md
  natscript.md        the language reference
  vm-abi.md           the frozen instruction and syscall ABI
tools/
  natc.py             the NatScript compiler
  vasm.py             the assembler both paths go through
  nattest.py tests/   the compiler's test suite, run by the build
  natvm_ref.py        a host NatVM used as a test oracle — NOT authoritative
  board.py            flashing and talking to the board
  *.nat *.vasm        the programs themselves
vendor/               the two borrowed binaries
```

## Documentation

`docs/` holds 59 engineering reports. Start with `docs/README.md` for the index
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
