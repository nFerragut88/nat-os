# nat-os — An Operating System Written From Scratch for the ESP32

### The Complete Engineering Narrative

**Used Medias LLC — Embedded Systems Division**
Compiled from the source tree, the 138-commit history, and engineering reports
UM-NATOS-001 through UM-NATOS-028.

---

## What this book is

nat-os is an operating system for the ESP32 that uses no ESP-IDF, no Arduino, no
FreeRTOS, and no C library. The kernel owns scheduling, memory, drivers and
application execution. Only the second-stage bootloader and the partition table
are borrowed, and both are replaceable. Every instruction from the image entry
point onward is project code.

This book is the long-form account of how that was built and, more usefully, how
it repeatedly failed to be built. The twenty-eight engineering reports in
`docs/` are the primary record; this book is their synthesis — it follows the
system layer by layer, quotes the code that implements each decision, and
carries the defect stories through to the standing rules they produced.

It is written to be read in two ways:

- **Front to back**, as the story of a kernel growing from a 1,216-byte image
  that could print a banner into a 37,248-byte device with a scheduler, a
  virtual machine, nine drivers, a touch launcher, a 3D renderer and an 802.11
  receiver.
- **By chapter**, as a reference. Every chapter names the source files it
  describes and the reports it draws on, so a chapter can be read cold.

## What the system does

| | |
|---|---|
| **Scheduling** | Preemptive, three priority levels with ageing so no ready task waits more than ~600 ms; blocking, sleeping, priority inheritance |
| **Memory** | Bump-and-free heap with a checkable invariant, per-application arenas, bounds-checked at every access |
| **Applications** | Register-based bytecode VM, 35 opcodes, 12 syscalls; faults contained, runaway programs bounded |
| **Display** | ILI9341 over SPI2 with DMA; per-application viewports that cannot be escaped |
| **Input** | XPT2046 touch, gated on PENIRQ *and* pressure, confined per application |
| **UI** | Touch launcher — icon grid, hybrid cursor, double-tap to start a program |
| **Graphics** | Raycast 3D view, one ray per column, face shading, optional framebuffer |
| **Storage** | Checksummed flash record surviving power cycles; microSD read over SPI |
| **Sensors** | SAR ADC1 (8 channels), bit-banged I²C master |
| **Sound** | LEDC hardware PWM, no CPU while sounding |
| **Radio** | 802.11 receive: beacon decode, continuous scanning, through Espressif's PHY blob |
| **Failure** | Stack guards enforced per switch, hang detector, panic to serial *and* flash *and* the panel |

## Table of contents

### Front matter

- [Preface — why this book exists, and how to read it](00-preface.md)
- [Conventions and a note on evidence](00b-conventions.md)

### Part I — Foundations

1. [The Machine, and the Problem It Cannot Solve](01-the-machine.md)
2. [call0: The Calling Convention That Made a Scheduler Tractable](02-abi.md)
3. [Boot: From Silicon to `_start`](03-boot.md)
4. [The Memory Map, and an Overlap That Was Not There](04-memory-map.md)
5. [The Build Pipeline](05-build.md)
6. [Milestone 0: Proving Ownership of the Machine](06-milestone-zero.md)

### Part II — The Kernel

7. [Interrupts and the Tick](07-interrupts-and-tick.md)
8. [Tasks, Frames, and the Defect That Cost Twelve Builds](08-tasks.md)
9. [Scheduling: Priority, Sleep, Ageing, and Two Clocks That Lied](09-scheduling.md)
10. [Memory: A Heap With One Invariant, and Arenas](10-heap-and-arenas.md)
11. [Locking, and Why Contention Cost Is a Count and Not a Duration](11-locking.md)
12. [Failure Handling: Three Mechanisms That Had Never Fired](12-failure-handling.md)

### Part III — The Virtual Machine

13. [Why a Bytecode VM Is the Only Isolation Available](13-why-a-vm.md)
14. [The Instruction Set and the Interpreter](14-the-isa.md)
15. [The Producer: `vasm.py` and the Programs](15-the-assembler.md)
16. [Applications: Lifecycle, Three Levels of Scheduling, Messaging](16-applications.md)
17. [Syscalls and the Confinement Model](17-syscalls.md)

### Part IV — Drivers

18. [The Display: From 387 ms to 43 ms, and a Stall Still Open](18-display.md)
19. [Touch: Four Attempts, and an Axis Inverted for Three Months](19-touch.md)
20. [Persistence: A Read Defect That Looked Like the Wrong Thing](20-persistence.md)
21. [microSD, and a Pad Table That Is Not in Pin Order](21-sdcard.md)
22. [The Interrupt Matrix, the ADC, and I²C](22-matrix-adc-i2c.md)
23. [Audio, and Three Ways to Be Silent](23-audio.md)

### Part V — The Device

24. [The Launcher, and Four Defects It Found by Existing](24-launcher.md)
25. [The Renderer](25-renderer.md)
26. [The Note Pad and the Shell on the Panel](26-notes-and-term.md)
27. [WiFi: Mixed ABIs, a Vendor Blob, and Frames Off the Air](27-wifi.md)

### Part VI — Method

28. [The Instruments That Lied](28-instruments-that-lied.md)
29. [The Standing Rules](29-standing-rules.md)
30. [What Is Not Established](30-not-established.md)
31. [Where It Could Go Next](31-next.md)

### Appendices

- [Appendix A — File inventory](A-file-inventory.md)
- [Appendix B — Bytecode ISA reference](B-isa-reference.md)
- [Appendix C — Shell command reference](C-shell-reference.md)
- [Appendix D — Address and register map](D-register-map.md)
- [Appendix E — Report index and cross-reference](E-report-index.md)
- [Appendix F — Every measured number](F-measurements.md)
- [Appendix G — Timeline from the commit history](G-timeline.md)

## Print edition

`../pdf/nat-os-book-6x9-kdp.pdf` is a print-ready interior for a 6 × 9 in
paperback, laid out to Amazon KDP's paperback specification: mirrored margins
with a 0.625 in gutter, unnumbered front matter, and a contents whose page
numbers are read back out of the rendered PDF and re-verified against the
finished file. 372 pages; spine width 0.8377 in on white paper.

Rebuild it with:

```
python docs/style/build_book_pdf.py
```

It needs `markdown`, `playwright` and `pypdf`, and it re-derives the gutter from
the measured page count rather than trusting the number above. Cover art is a
separate KDP upload and is not generated here.

## Licence

The kernel and this book are MIT — see `LICENSE`. The two binaries in `vendor/`
are unmodified ESP-IDF artefacts, copyright Espressif, redistributed under
Apache 2.0.
