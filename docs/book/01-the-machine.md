# Chapter 1 — The Machine, and the Problem It Cannot Solve

> Sources: `docs/UM-NATOS-001-architecture.md`, `docs/UM-NATOS-004-memory-map.md`
> Code: `kernel/arena.h`, `kernel/vm.h`, `kernel/task.h`

---

## 1.1 The target

Every architectural decision in nat-os traces back to a property of one specific
piece of silicon, so it is worth being precise about what that silicon is before
anything else.

| Property | Value | Consequence for the design |
|---|---|---|
| SoC | ESP32-D0WD, dual-core Xtensa LX6 @ 240 MHz | Real SMP is possible; deferred indefinitely — APP_CPU is still never started |
| Internal SRAM | 520 KB (SRAM0 192 KB / SRAM1 128 KB / SRAM2 200 KB) | Ample for a kernel; tight once a graphics stack is added |
| PSRAM | **None** | No external RAM to fall back on |
| Flash | 4 MB, executed in place via cache | Code can exceed IRAM, but requires cache setup |
| MMU | Flash-cache address translation **only** | **No per-process virtual address spaces** |
| Display | ILI9341 240×320 with resistive touch | The framebuffer question, §1.4 |

The chip identifies itself, and the identification was checked rather than
assumed. UM-NATOS-010 §7 records it: **ESP32-D0WD-V3 rev 3.1**, features `WiFi,
BT, Dual Core, 240MHz, VRef calibration in efuse, Coding Scheme None`. No
embedded PSRAM, and none on the module. That check exists because "add external
RAM" is the obvious escape hatch from the memory constraint in §1.4, and it is
closed without a hardware change.

The board is the ESP32-2432S028R, sold as the "Cheap Yellow Display". Every
measurement anywhere in this project was taken on it, and mostly on one physical
unit. Where that matters — the touch calibration, the SPI clock ceiling, the
speaker's frequency response — the book says so.

## 1.2 Five layers, and where the project starts writing

UM-NATOS-001 divides the system into five layers. The scope decision is not
"what does the OS contain" but *where the project starts writing its own code*.

| Layer | Contents | nat-os |
|---|---|---|
| L0 | Mask-ROM bootloader | Silicon. Cannot be replaced. |
| L1 | Second-stage bootloader: clock config, flash cache/MMU init, image loading | **Borrowed.** Replaceable later. |
| L2 | Scheduler, context switching, synchronisation, memory allocation | **Written from scratch** |
| L3 | Driver model, virtual filesystem, inter-process communication | **Written from scratch** |
| L4 | Application format, loader, virtual machine, shell | **Written from scratch** |

### The argument for starting at L2

Writing L1 means implementing clock trees, flash cache configuration, and SPI
timing calibration against the technical reference manual. That is silicon
bring-up, not operating system design. It is a legitimate project and a
different one, and it delays the first scheduler by weeks.

Borrowing L1 costs one binary dependency — 17,536 bytes — and yields a running
CPU with flash mapped and a well-defined handoff. The borrowed component is
isolated behind a documented interface (the image header, Chapter 3), so
replacing it later is a contained change rather than a rewrite.

The cost of that decision is not zero and it is recorded. UM-NATOS-002 §6 lists
three places where the kernel currently relies on L1 or the ROM having done
something:

1. **UART0 is already configured.** The kernel writes bytes into the TX FIFO
   without setting baud rate, pin mux, or line discipline. This works because
   the ROM configured UART0 at 115200 for its own output. A replacement L1 that
   did not do this would produce a silent kernel.
2. **Flash cache is configured but unused** at M0. When the kernel started
   mapping `.rodata` from flash (Chapter 4), this became load-bearing.
3. **CPU clock.** The kernel neither reads nor sets it.

Item 2 came due. Chapter 4 covers the DROM region that depends on it; Chapter 20
covers the flash driver that had to reconcile with the cache it shares a bus
with.

## 1.3 The problem: this hardware cannot isolate anything

> **The ESP32 cannot provide memory protection between applications.** Its MMU
> translates flash addresses for execute-in-place caching; it does not implement
> per-process page tables. There is one address space, and any code can write
> any address.

This is a hardware property, not an implementation gap. It cannot be engineered
around at the kernel level. Its direct consequence is that **a native-code
application can corrupt the kernel and every other application, and no kernel
design prevents this.**

That sentence is the hinge of the whole project. Two responses were considered.

### Option A — native applications via an ELF loader

Applications compiled separately, stored on SD, loaded into heap at runtime,
relocated, and executed natively.

- Fast — applications run at full CPU speed.
- Precedent exists on ESP32.
- **No isolation whatsoever.** One bad pointer takes down the system.
- Relocation and symbol binding are intricate and a rich source of subtle bugs.

### Option B — a bytecode virtual machine (selected)

Applications compiled to a bytecode the kernel interprets. Every memory access
executes as a VM instruction, so the interpreter can bounds-check it.

- **Isolation becomes achievable in software** — the VM refuses out-of-range
  access rather than performing it. This recovers, in the interpreter, the
  guarantee the silicon will not give.
- The instruction set is ours, so it can be kept small and auditable.
- **Preemption becomes trivial.** The dispatch loop can check a quantum counter
  between instructions; an application can always be interrupted at a known-safe
  boundary, with no native context switch required for application tasks.
- Slower than native — the interpretation overhead was, at the time of the
  decision, real and unmeasured. It has since been derived at roughly 109 cycles
  per bytecode instruction (Chapter 14).
- Costs flash for the interpreter, and requires a compiler or assembler.

**Decision: Option B.** The report states the argument in one sentence:

> An operating system whose applications can silently corrupt each other is
> firmware with extra steps, and on hardware without an MMU the VM is the only
> mechanism that can deliver the property.

The preemption consequence is worth stating separately because it simplifies the
kernel enormously: native preemptive context switching is needed only for kernel
and driver tasks, not for applications. There are currently nine native tasks
and the number is expected to stay small.

### The strong form of the guarantee

It is easy to describe the VM as "refusing" illegal accesses, and that
undersells it. UM-NATOS-013 §5.2 makes the sharper claim after running a program
written for no other purpose than to escape:

> The deeper point is what the rogue could not attempt. A VM address is an
> offset into its own arena, so there is no value it could load that names
> another application's memory. Reaching a neighbour is not refused — it is
> **unrepresentable**. The bounds check exists for the weaker case of a program
> walking off its own end, which is precisely what was observed.

That distinction — *refused* versus *unrepresentable* — recurs for every
resource an application touches, and by the end of Chapter 17 it covers three:

| Resource | Confined by | Outside its allocation |
|---|---|---|
| Memory | Arena bounds check | Unrepresentable — an address outside the arena cannot be formed |
| Pixels | Viewport clipping | Clipped before it exists |
| Input | Viewport test on delivery | Reported as no touch |

The design intent is visible in the header that declares arenas:

```c
/* nat-os — VM application arenas.
 *
 * An arena is a contiguous block of DRAM with a recorded base and length. It is
 * the unit of memory an application owns, and its bounds are what the bytecode
 * interpreter will check every load and store against (UM-NATOS-001 §4.2).
 *
 * This is the ONLY isolation mechanism this kernel will have. The ESP32 has no
 * MMU paging, so there is no hardware that can be asked to enforce these
 * bounds — the interpreter must do it in software on every access, and it must
 * not be compiled out for speed. Native tasks are not confined by arenas and
 * never will be; they are trusted code.
 */
```

The last sentence is the boundary of the claim, and it is stated everywhere it
applies. From `task.h`:

```c
 * There is no memory protection between them. The ESP32 has no MMU paging, so
 * a native task can corrupt any other. Stack guards catch the most common case
 * (overflow) but nothing catches a wild pointer.
```

nat-os isolates *applications*. It does not isolate *drivers*, and it never
will on this silicon.

## 1.4 The second constraint: 176 KB, and a display that wants 150 of it

The other number that shapes the architecture is DRAM.

Of the 520 KB of SRAM, the kernel's linker script claims about 176 KB as data
memory (Chapter 4 explains why it starts where it does). After kernel data,
`.rodata`, task stacks and a 4 KB boot-stack reservation, UM-NATOS-010 §7
measured **166,432 bytes allocatable** at M3 — later 167,680 B once `.rodata`
moved to flash, then 158,048 B once `TASK_MAX` rose from 4 to 8 and added 8 KB
of statically allocated stacks.

Against that, a full-screen 16-bit framebuffer for a 240×320 panel is:

```
240 × 320 × 2 = 153,600 bytes
```

| Consumer | Bytes | Share of heap |
|---|---|---|
| Full 240×320 16-bit framebuffer | 153,600 | 92.3% |
| Remaining for all arenas | 12,832 | 7.7% |

**A full-screen framebuffer and any meaningful set of concurrent VM applications
cannot coexist in internal DRAM.** With one committed, 12.8 KB is left — a single
small arena, with no room for a second application.

Four escapes were considered and three closed:

1. **No full framebuffer.** Drive the ILI9341 from a line or tile buffer and push
   updates incrementally. A 240-pixel line buffer is 480 B.
2. **Reduced colour depth.** 8bpp halves it to 76,800 B — still 46% of the heap.
3. **Partial/dirty-region updates.**
4. **External PSRAM.** **Confirmed absent.** Closed without a hardware change.

And flash, the obvious fifth option, is closed for reasons of mechanism rather
than capacity (UM-NATOS-010 §7.1):

- Flash is memory-mapped **read-only** at runtime. It cannot be the target of a
  store instruction.
- Writing requires erasing a 4 KB sector and then programming pages, costing tens
  of milliseconds — roughly three orders of magnitude more than a frame's budget.
- Endurance is ~100,000 erase cycles per sector. A 153,600 B framebuffer spans
  about 38 sectors; at 30 fps each is erased 30 times per second, exhausting
  rated endurance in **under an hour**.
- Flash writes require the cache to be disabled, stalling any code executing from
  flash.

> Flash as framebuffer is therefore not slow-but-workable. It destroys the part.

### The premise, challenged

Option 1 was selected, and then UM-NATOS-010 §7.2 went further and questioned
whether the framebuffer was needed at all:

> **The ILI9341 contains its own frame memory** — a GRAM of 240×320 at 18bpp,
> roughly 172,800 bytes — and drives the panel from it autonomously. Pixels
> written to the controller stay there; the host is not refreshing a surface
> continuously.
>
> The ESP32 therefore never requires a full local copy. It requires a *transfer*
> buffer: 480 B for one 240-pixel line at 16bpp. The 153,600 B in §7 was never a
> hardware requirement — it is the cost of keeping a redundant second copy of
> something the display already stores.

This strengthened option 1 "from *the least bad choice* to *the correct one*",
and the display driver was written with no framebuffer anywhere in the system.

The story does not end there, and the ending is instructive enough to preview.
A framebuffer for the 3D view *was* eventually added, measured as worthless,
defaulted off, and then — after two unrelated defects were fixed — re-measured
as clearly worth having and defaulted on. Chapter 18 §18.7 is that reversal, and
what made it findable was that the framebuffer was a *runtime switch* rather than
a compile-time decision:

> Keeping the switch means the claim can be rechecked whenever the display path
> changes underneath it, rather than being inherited as folklore.

## 1.5 Explicit non-goals

Stated in UM-NATOS-001 §6 and still true:

- **POSIX compatibility.** Nothing here targets a standard API.
- **Binary compatibility with ESP-IDF or Arduino.** Existing sketches will not
  run. Chapter 2 explains why this is not merely unimplemented but structurally
  impossible for anything compiled the usual way.
- **Reuse of the sibling BibleOS/The Word Device codebase.** That is
  LVGL-on-Arduino firmware and is not a migration target.
- **Memory protection between native tasks.** Impossible on this silicon;
  protection applies to VM applications only.

## 1.6 The component inventory, then and now

UM-NATOS-001 §5 listed what existed at the time it was written. The comparison
with the present tree is the shape of the whole book:

| Component | Layer | At M0 | Now |
|---|---|---|---|
| Entry stub (`start.S`) | L2 | Complete | Unchanged, 44 lines |
| UART console (`uart.c`) | L3 | Output only | Output, polled receive, an output tee |
| Kernel entry (`kmain.c`) | L2 | Placeholder with self-checks | 1,820 lines: nine tasks, six self-tests, the boot report |
| Link map (`linker.ld`) | L2 | RAM-only | IRAM + DRAM + DROM, `.dram.rodata` by object file |
| Build pipeline (`build.ps1`) | — | Complete | Plus bytecode assembly, windowed-ABI objects, vendor archives |
| Timer / interrupt controller | L2 | **Not started** | `timer.c`, `intr.c`, a routable matrix |
| Scheduler and context switch | L2 | **Not started** | `task.c`, 689 lines, three priority levels with ageing |
| Heap allocator | L2 | **Not started** | `heap.c` with a ten-case structural check |
| Bytecode VM | L4 | **Not started** | `vm.c`, 35 opcodes, 14 syscalls, a device table |
| Display, touch, SD drivers | L3 | Not started | All three, plus ADC, I²C, audio, flash, WiFi |

## 1.7 The open architectural questions, revisited

UM-NATOS-001 §7 listed four. Their fates are worth recording because two were
answered by measurement and two by not needing an answer.

**1. SMP.** "Whether the scheduler becomes SMP-aware or core 1 is dedicated to
drivers is undecided and should be settled before M2 fixes the scheduler's
shape." It was never settled and the scheduler's shape was fixed anyway. APP_CPU
has never been started. The absence has cost exactly once, and expensively: the
GPIO interrupt-enable field selects *which CPU* receives a pin, and the first
choice of bit delivered PENIRQ edges "perfectly to a core that was halted"
(Chapter 22).

**2. Flash execute-in-place.** Resolved for *data* in UM-NATOS-011 (Chapter 4).
Never attempted for *code*: the kernel is 116 KB of text against 128 KB of IRAM
and still fits. The `l32r` literal-pool constraint in §4.3 is why an IROM segment
would be more than a linker-script change.

**3. Application toolchain.** Answered by `tools/vasm.py` (Chapter 15) — an
assembler, not a compiler. It runs on the host, which keeps the on-device side a
pure interpreter with no parsing and no attack surface from untrusted text.

**4. Persistence.** Answered partially. There is a flash-backed record and a
microSD *reader*, and there is no filesystem. Writing FAT remains "a substantial
subproject" and remains unwritten. Chapter 21 stops at reading a FAT16 signature
at LBA 240.

---

**Next:** the calling convention, which is the one decision that had to be made
before any code could be written and cannot be revisited cheaply afterward.
