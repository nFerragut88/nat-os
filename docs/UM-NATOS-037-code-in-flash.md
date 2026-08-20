# UM-NATOS-037 — Code in Flash, and Three Things That Only Move Reveals

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-19 · Status: **Shipped; verified on hardware, both bootloaders**

---

## 1. Abstract

`kernel/linker.ld` declared three regions — `iram`, `dram`, `drom` — and no
region for executable code in flash. Every instruction this kernel had ever run
lived in 128 KB of IRAM, and the `-WiFi` build was using 112 KB of it.

`.rodata` has been mapped from flash since UM-NATOS-011. Instructions never
were, for no reason beyond nobody having needed it.

```
                    before        after
default build      56,516       36,680   of 131,072      free 94,392
-WiFi build       111,945       86,129                   free 44,943
```

Two files moved: `shell.c` and `kmain.c`. Free IRAM in the WiFi build went from
19,127 bytes to 44,943.

The mechanism was small — the bootloader already maps DROM and this is the same
MMU on the instruction side. What the report is actually about is the three
defects that only surfaced *because* code moved, none of which were in the new
code.

---

## 2. What was built

| | |
|---|---|
| `kernel/linker.ld` | an `irom` region and a `.flash.text` output section |
| `boot/boot.c` | `mmu_map()` takes an entry offset; an IROM case in the segment loop |
| cache | `PRO_CACHE_MASK_IRAM0` cleared alongside `DROM0` |
| image | gains a segment, mapped rather than copied |

Two placement details that are not style:

**`.flash.text` is declared before `.text`.** The linker assigns each input
section exactly once, in the order the output sections appear. Put it after and
`.text`'s wildcard claims the objects first, silently.

**Literals travel with the code.** `.literal` is selected alongside `.text` for
each object, because `-mtext-section-literals` keeps a function's literal pool
beside it and `l32r` reaches backwards only 256 KB. `linker.ld` already carried
a note about this for the `.rodata` case; it applies with the sign reversed
here.

---

## 3. The window that is not the window

`soc/ext_mem_defs.h`:

```
IRAM0_CACHE_ADDRESS_LOW     0x400D0000     <- an ESP32 app's "IROM"
IROM0_CACHE_ADDRESS_LOW     0x40800000     <- NOT that
```

The name says IROM0 and it is the wrong one. An ESP32 application's
flash-executable code goes in the window named **IRAM0_CACHE**.

They are not interchangeable, and the difference is not only the address.
From `hal/esp32/mmu_ll.h`, `mmu_ll_get_entry_id()`:

```
DROM0_CACHE  -> entry offset   0
IRAM0_CACHE  -> entry offset  64
IRAM1_CACHE  -> entry offset 128
IROM0_CACHE  -> entry offset 192
```

Same `>> 16` shift, same `0x3FFFFF` mask, different block of MMU entries. Using
the DROM offset for an IROM address writes a **perfectly valid entry** that
points the wrong window at the right flash page — and the CPU then executes
whatever was already mapped where the code should have been.

This was caught by reading the header rather than by a fault, which is the only
reason it is a footnote instead of a section.

---

## 4. Three defects that only moving code could find

### 4.1 A misaligned call target, latent for months

The first link after moving `shell.c` failed:

```
shell.c.o: dangerous relocation: call0: misaligned call target: rom_call3
```

`call0` encodes its target as an offset in units of four bytes, so an entry
point must be 4-byte aligned. `rom_call3` and `rom_call4` were **the only two
globals in `kernel/window.S` without an `.align 4`** — every other one had it.

It had been latent for as long as `rom_call3` has existed, because the
surrounding layout happened to land it on a 4-byte boundary. Moving `shell.c`
changed the layout and it failed immediately.

`rom_call4` was added earlier the same day, by copying `rom_call3`'s shape —
which propagated a defect that was already there. Both are aligned now.

> **A latent alignment bug surfaces when something unrelated moves.** It is not
> found by testing the thing that has the bug.

### 4.2 A segment the loader had never been shown

With `kmain.c` added, the blob-free build reset twenty-five times in sixteen
seconds. The bootloader's own log said why:

```
[boot]   0x00000000 len 0x000012e0  -> copy
```

A segment with load address **zero**. esptool inserts these — `ImageSegment(0,
...)` — to pad a flash-mapped segment until its data lands congruent with its
virtual address. The IROM segment had crossed an alignment boundary and needed
4,832 bytes of padding. Espressif's bootloader skips them; ours copied them to
address 0.

**Nothing about the IROM support was wrong.** The image acquired a *kind of
segment* this loader had never encountered, because no previous build had
required padding.

The skip is written as "below `DRAM_LOW`" rather than "`== 0`", so any future
load address the loader does not understand is refused loudly rather than
written somewhere.

### 4.3 And a gate hid it for one build

The `-WiFi` build passed this test while the blob-free build failed it.

Not because of WiFi. Because `build.ps1 -WiFi` forces Espressif's second stage
(the gate from UM-NATOS-036 §7), so **the two builds were being tested by two
different bootloaders**, and only one of them had the bug.

That gate exists for a good reason and it will keep doing this for as long as it
is there. Anyone testing a boot-chain change must run **both** builds, and know
which loader each one used.

---

## 5. Placement: what may move, and how it was decided

Code executing from flash stalls on a cache miss and cannot be fetched at all
while the flash bus is busy. So this list stays in IRAM:

| | why |
|---|---|
| `vectors.S`, `panic.c` | the fault path, which must work when nothing else does |
| `flash.c`, `store.c` | drive SPI1; code in flash could not read itself |
| `task.c` | the scheduler switch |
| `window.S` | window handlers, reached from any vendor call |
| `appcpu.c` | core 1 runs with no cache enabled at all |

`kmain.c` was cleared by analysis rather than by eye. It defines sixteen
symbols — `kmain`, the task entry points, helpers — none in ISR context, and
`nm` confirmed that **no object in the list above references any of them**.

The subtle case was the idle task: if the cache were disabled during a flash
write and the scheduler then entered a flash-resident task, it would fault. It
cannot. `flash.c` does not disable the cache — it drives SPI1 directly and wraps
every operation in `crit_enter`/`crit_exit`, so nothing else runs for the
duration, and `flash.o` and `store.o` already have their rodata pinned to DRAM
by `.dram.rodata`.

---

## 6. Verification

On **our own** bootloader, blob-free build:

```
[boot]   0x00000000 len 0x000012e0  -> padding, skipped
[boot]   0x400d0000 len 0x00004d7c  -> mmu irom
```

One ROM banner. Every self-test PASS. `mem`, `romcall`, `sd`, `i2c`, `stacks`,
`3d` and `3d off` all working — which **is** the proof that `shell.c` and
`kmain.c` execute from flash, since all of that code now lives there. `romcall`
still returns `0xcbf43926`. The 3D view renders.

On **Espressif's** bootloader, `-WiFi` build: boots, all self-tests pass.

And the acceptance test named in `next_moves/07` before any of this was written:

```
*** KERNEL PANIC ***
exccause : 0  (IllegalInstruction)
epc      : 0x400d0c11
recorded : yes, the next boot will report this
panel    : 175955 bytes drawn
```

`epc` is **inside the flash window**. The illegal instruction executed from
flash, the handler in IRAM caught it, recorded it to flash, and repainted the
panel. That is precisely the scenario the placement rules exist to protect, and
it was checked rather than assumed.

---

## 7. What this unlocks

Not WiFi. The radio is closed as a negative (UM-NATOS-034 §29) and nothing here
touches it.

What it unlocks is everything that was quietly competing for 19 KB: a LoRa
driver and a DTN bundle layer for `docs/conceptual/the-ark-and-fiendnet.md`, a
filesystem, more devices, and NatScript's runtime. It also makes
`next_moves/08` possible at all, though 08's own file argues against it.

The ceiling was going to bite whatever happened next. It is now 94 KB away in
the default build instead of 19.

---

## 7.1 One thing this made harder, recorded because it does not announce itself

Moving code into flash added a constraint nobody was looking for.

While an SPI NOR chip is erasing it answers RDSR and little else — it cannot
serve a read, cache or no cache. Before this report that did not matter: all code
was in IRAM, so a task scheduled during an erase could still execute. Now
`shell.c` and `kmain.c` cannot.

`flash.c` makes this safe today, by accident of a choice made for another
reason: it holds a critical section across the entire 125 ms erase (measured,
`next_moves/04`), so no other task runs at all. **That critical section is now
doing two jobs and only one of them is written on it** — it stops preemption,
and by stopping preemption it stops flash-resident code from running while the
chip is busy.

Anyone narrowing it — which looks obviously correct, since polling a status
register plainly does not require interrupts off — will produce a hang whose
cause is three files from the change. The argument and the three ways out are
recorded at `flash_erase_sector()`, and the reason is now in `linker.ld`'s
placement rules, flagged as **not** a cache-off reason because every other entry
in that list is.

§5 answers "what may move". This is the other question — **what does moving make
harder** — and it is the one that stays quiet.

---

## 8. Rules earned

**A latent bug surfaces when something unrelated moves.** `rom_call3` had been
missing its alignment for months and every test passed, because the layout
happened to be kind. Moving an unrelated file broke the link instantly. Tests
that exercise the buggy code will not find this class; only rearrangement will.

**A skip should name what it accepts, not what it rejects.** The padding segment
is skipped by "below the lowest address I understand", not by "equals zero".
The first refuses tomorrow's surprise; the second waits for it.

**When two build configurations use different boot chains, a boot-chain change
must be tested in both.** The `-WiFi` gate meant one build was exercising
Espressif's loader and the other ours, and a bug that existed in only one of
them looked like a bug that depended on WiFi.

---

*Filed 2026-08-19.*
