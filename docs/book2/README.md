# nat-os — An Operating System Written From Scratch for the ESP32

### The Complete Engineering Narrative

**Used Medias LLC — Embedded Systems Division**
Compiled from the source tree, the 138-commit history, and engineering reports
UM-NATOS-001 through UM-NATOS-030.

> **Reports 029 and 030 are synthesised here.** They are one investigation in two
> parts — a day that eliminated eleven theories correctly without finding the
> cause, and a second session that found it in one bit — and they are told as
> **Chapters 28b and 28c**. Those two chapters are lettered rather than numbered
> so that every cross-reference in the rest of the book, and in the reports,
> still points where it did.
>
> Six chapters carry what they changed: **9** (ageing is not priority
> inheritance, and it is not inert either), **11** (`cont=0` means nobody is
> permitted to contend), **18** (the stall was never a stall), **25** (the
> renderer was the best instrument for a bug it did not contain), **28** (five
> further instances, and two counting bases kept apart) and **29** (a nineteenth
> standing rule).
>
> **Reports 031–032 are not synthesised.** They remain the primary record, and
> where they invalidate something here the affected chapter carries a
> since-written note rather than a silent edit: the device model and the syscall
> harness are built (Ch. 30 §30.2), and per-application permissions are
> containment rather than security (Ch. 30 §30.4).
>
> **Two later reports are cited for one fact each**, because without them this
> book would describe a boot chain the repository no longer has: **035** (the
> second-stage bootloader is this project's own) and **036** (the kernel owns
> the CPU clock, and for one session it did not). Chapter 1 §1.2, Chapter 3
> §3.1.1 and Chapter 7 §7.8. Appendix E §E.1c lists exactly what is drawn from
> them.
>
> Appendix B was re-verified against the interpreter and is current.

---

## What this book is

nat-os is an operating system for the ESP32 that uses no ESP-IDF, no Arduino, no
FreeRTOS, and no C library. The kernel owns scheduling, memory, drivers and
application execution. Every instruction from the image entry point onward is
project code — and, since UM-NATOS-035, so is every instruction before it: the
second-stage bootloader is `boot/`, 2,736 bytes of this project's own, and the
only thing still borrowed in the boot chain is a 3 KB partition table that
nothing reads (Ch. 3 §3.1.1).

This book is the long-form account of how that was built and, more usefully, how
it repeatedly failed to be built. The thirty engineering reports in `docs/` are
the primary record; this book is their synthesis — it follows the system layer by
layer, quotes the code that implements each decision, and carries the defect
stories through to the standing rules they produced.

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
| **Scheduling** | Preemptive, three priority levels with ageing so no ready task waits more than ~600 ms; blocking and sleeping. **Not** priority inheritance — the mechanism exists and is unwired (Ch. 30 §30.5) |
| **Memory** | Bump-and-free heap with a checkable invariant, per-application arenas, bounds-checked at every access |
| **Applications** | Register-based bytecode VM, 35 opcodes, 14 syscalls; faults contained, runaway programs bounded; the kernel can call *into* a program via event handlers |
| **Devices** | A table applications reach through one syscall — light, speaker, persistence, I²C, keypad, loopback, microSD — with per-application permissions |
| **Display** | ILI9341 over SPI2 with DMA; per-application viewports that cannot be escaped |
| **Input** | XPT2046 touch, gated on PENIRQ *and* pressure, confined per application |
| **UI** | Touch launcher — icon grid, hybrid cursor, double-tap to start a program |
| **Graphics** | Raycast 3D view, one ray per column, face shading, optional framebuffer |
| **Storage** | Checksummed flash record surviving power cycles; microSD read over SPI |
| **Sensors** | SAR ADC1 (8 channels), bit-banged I²C master |
| **Sound** | LEDC hardware PWM, no CPU while sounding |
| **Radio** | 802.11 receive: beacon decode, continuous scanning, through Espressif's PHY blob |
| **Failure** | Stack guards enforced per switch, hang detector, panic to serial *and* flash *and* the panel |
| **Boot** | This project's own second-stage bootloader, 2,736 B; the kernel sets its own CPU clock. Espressif's loader kept as the recovery path and the A/B control |

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

18. [The Display: From 387 ms to 43 ms, and a Stall That Was Never There](18-display.md)
19. [Touch: Four Attempts, and an Axis Inverted for Three Days](19-touch.md)
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
- 28b. [Two Mysteries, Eleven Theories, and a Novel That Called It](28b-two-mysteries.md)
- 28c. [One Bit](28c-one-bit.md)
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

`pdf/nat-os-book-6x9-kdp.pdf` is a print-ready interior for a 6 × 9 in
paperback, laid out to Amazon KDP's paperback specification: mirrored margins
with a 0.625 in gutter, unnumbered front matter, and a contents whose page
numbers are read back out of the rendered PDF and re-verified against the
finished file. **424 pages** — 6 of front matter and 418 numbered; spine width
0.9951 in on premium colour (0.9548 in if the interior is printed on white).

The page count by edition, because the spine depends on it and nothing else in
this pipeline records it:

| | pages |
|---|---|
| 001–028, as first compiled | 372 |
| with the 029–032 since-written notes | 390 |
| **this edition: 029 and 030 synthesised, boot chain corrected** | **424** |

Rebuild it with:

```
python docs/style/build_book2_pdf.py
```

It needs `markdown`, `playwright` and `pypdf`, and it re-derives the gutter from
the measured page count rather than trusting the number above.

`build_book2_pdf.py` is a thin patch over `build_book_pdf.py`, which builds the
original `docs/book/`. It exists because chapters 28b and 28c are *lettered*, and
two things in the original assume digits: the contents groups chapters by a
literal `"28-"` prefix, and the title parser matches `Chapter <digits>`. A
lettered chapter would have vanished from the contents rather than failing
loudly, which is the reason the patch is a separate file with a check in it
rather than a regex loosened in place.

### Cover

`pdf/nat-os-cover-6x9-kdp.pdf` is the matching wrap — back, spine and front in
one flat file, **13.245 × 9.250 in** with 0.125 in bleed. The 2 × 1.2 in barcode
area at the back cover's bottom right is left clear.

**Rebuild the cover whenever the interior changes.** The spine is a function of
the page count, so a cover built against a 390-page interior does not fit a
424-page one — and that mismatch is the one error in this pipeline that a
proof-read cannot catch, because both files are individually valid.

The cover also *prints* figures that go stale, and all four changed for this
edition:

| Cover art | was | now | why |
|---|---|---|---|
| pages | 390 | **424** | read from the measured interior, never a literal |
| chapters | 31 | **33** | 28b and 28c |
| reports | 32 | **30** | this edition *synthesises* 001–030 rather than noting 029–032 from outside |
| standing rules | 18 | **19** | Rule 19, from UM-NATOS-029 §7.2 |

The report count going *down* is not an error and is the one figure here worth a
sentence. The old cover counted every report the book drew on, including four it
only carried notes about. This one counts the reports the book *is a synthesis
of*, which is a claim a reader can check by opening it.

Only the page count is derived; the other three are literals in
`build_cover2.py`. Each substitution is checked at build time and the build
fails if the text it replaces is not found — because a cover that silently kept
the previous edition's numbers is a valid PDF, and this pipeline has shipped one
before.

```
python docs/style/build_cover2.py                    # premium-color, the default
python docs/style/build_cover2.py --paper bw-white   # if the interior is greyscale
python docs/style/build_cover2.py --size 13.124x9.250
```

**The spine width depends on the interior plan, not just the page count.** KDP's
per-page caliper is 0.002252 in for black ink on white and 0.002347 in for
premium colour, which on 424 pages is the difference between a 13.205 in wrap
and a 13.245 in one — and submitting the wrong one is rejected with *"your
expected cover size is X but the submitted file size is Y"*. This interior uses
colour (panel tints, table headers), so premium colour is the default here.

`--size` takes KDP's stated expected size verbatim and back-solves the spine,
which is the quickest way out of that rejection.

The page box is set to the exact wrap after rendering, because Chromium emits
page boxes in whole points and would otherwise land a third of a point short.

The four screens on it are **reconstructions, not photographs** — no screenshot
of the running board exists in this repository. `docs/style/os_screens.py`
redraws the panel at its native 240 × 320 using the kernel's own data: the 5×8
font parsed out of `display.c`, the 8×8 icons out of `desktop.c`, the map out of
`raycast.c`, the real raycasting algorithm, and every colour as its RGB565
constant quantised through 16 bits. Change any of those in the kernel and the
images change, which is the only thing that makes them honest. The back cover
says so too.

`cover/cover-proof.png` is a 96 dpi proof for looking at before uploading
anything. The four screen PNGs beside it are regenerated on every cover build.

### What this edition changed

For a reader holding both: this is `docs/book/` with reports 029 and 030
synthesised rather than noted, plus a factual pass over the boot chain. Nothing
was deleted. The original edition is unmodified in `docs/book/`, and builds with
`build_book_pdf.py` exactly as before.

| | |
|---|---|
| New chapters | 28b, 28c |
| Chapters with new material | 1, 3, 5, 7, 9, 11, 12, 18, 25, 28, 29, 30, and the preface and conventions |
| Appendices with new material | A, C, E, F, G |
| Standing rules | 18 → 19 |
| Pages | 390 → 424 |

## Licence

The kernel and this book are MIT — see `LICENSE`. The two binaries in `vendor/`
are unmodified ESP-IDF artefacts, copyright Espressif, redistributed under
Apache 2.0.
