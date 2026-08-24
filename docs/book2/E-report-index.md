# Appendix E — Report Index and Cross-Reference

The engineering reports under `docs/`, their revisions, and the chapters of this
book that draw on each.

The book synthesises **001–030**. Reports 029 and 030 were written after the
first compilation and are now merged: Chapters 28b and 28c are their narrative,
and Chapters 9, 11, 18, 25, 28 and 29 carry what they changed.

Reports **031–032** are listed in §E.1b and are *not* merged; where they
invalidate a chapter, that chapter carries a since-written note. A small number
of later reports are cited for one fact each, and only where the book would
otherwise be wrong about the machine as it stands: **035** and **036** for the
boot chain and the clock (Ch. 1 §1.2, Ch. 3 §3.1.1, Ch. 7 §7.8). They are listed
in §E.1c. Nothing else from 033 onward is drawn on.

---

## E.1 The set

| ID | Title | Rev | Status | Chapters |
|---|---|---|---|---|
| 001 | System Architecture and Scope | 1.0 | current | 1, 13 |
| 002 | Boot Chain and Image Format | 1.0 | measured on hardware | 3 |
| 003 | Xtensa ABI Selection | 1.0 | verified by codegen inspection | 2 |
| 004 | Memory Map and Allocation Policy | 1.0 | current | 4 |
| 005 | Build and Flash Pipeline | 1.0 | current | 5 |
| 006 | Milestone 0 Verification | 1.0 | **PASS** | 6 |
| 007 | Development Roadmap M1–M5 | 1.1 | all milestones complete | 6, 31 |
| 008 | Milestone 1 Verification | 1.1 | **PASS** | 7 |
| 009 | Milestone 2 Verification | 1.3 | **PASS** | 8, 9 |
| 010 | Milestone 3 Verification | 1.0 | **PASS** | 10 |
| 011 | Flash Cache and Read-Only Data Placement | 1.0 | complete | 4 |
| 012 | Milestone 4 Verification | 1.1 | **PASS** | 13, 14, 15, 17 |
| 013 | Milestone 5 Verification | 1.1 | **PASS** | 16 |
| 014 | Locking Primitives | 1.3 | complete — **§9 withdrawn**, priority inheritance was never wired | 11, 30 |
| 015 | Display Driver | 1.4 | complete — the DMA stall it left open is closed by 030 | 18, 25 |
| 016 | Display Syscalls, and a Total System Freeze | 1.0 | complete | 17 |
| 017 | Touchscreen, and a Verification Method That Failed Three Times | 1.2 | complete | 19, 28 |
| 018 | Persistence, and a Read Defect That Looked Like the Wrong Thing | 1.0 | complete | 20 |
| 019 | Failure Handling, and Three Mechanisms That Had Never Fired | 1.2 | complete | 12 |
| 020 | microSD over SPI, and a Pad Table That Is Not in Pin Order | 1.0 | complete | 21 |
| 021 | The Launcher, and Four Defects It Found by Existing | 1.3 | complete | 24 |
| 022 | The Note Pad, and a Workaround Wearing a Costume | 1.1 | complete | 26 |
| 023 | The Interrupt Matrix, and Four Ways to Deliver an Edge to Nobody | 1.0 | **infrastructure only** | 22 |
| 024 | The ADC, and a Wrong Bit in a Right Register | 1.0 | verified | 22 |
| 025 | I²C, and Why a Preempted Master Is Legal | 1.0 | **bus only** | 22 |
| 026 | The Shell on the Panel, and Not a Menu of It | 1.1 | complete | 26 |
| 027 | Audio, and Three Ways to Be Silent | 1.0 | working | 23 |
| 028 | WiFi, Touch, and the Column That Ate the 3D View | 1.2 | **rx working, tx not**; §3.1 carries a withdrawn claim | 27, 9, 28 |
| 029 | Two Mysteries, One Confirmed Bug, and a Novel That Called It First | 1.1 | **superseded by 030 as to cause**; its eleven eliminations stand | **28b**, 9, 11 |
| 030 | One Bit | 1.0 | **fixed and confirmed on hardware** | **28c**, 18, 25, 28, 29 |

Reports 029 and 030 are one investigation in two parts and should be read in
order. 029 is the session that did not find it; 030 is the bit. The book keeps
them as two chapters for the same reason the reports are two documents: the
first one's reasoning is only instructive while it is still wrong.

## E.1b Written after the book, and not merged

| ID | Title | Rev | Status | Supersedes |
|---|---|---|---|---|
| 031 | The Device Model, and What a Narrow Interface Survives | 1.1 | **shipped**, seven devices + events verified | Ch. 30 §30.2 ×2, Ch. 31 |
| 032 | Containment, and Why It Is Not Security | 1.0 | **shipped**, verified on hardware | 031 §4 |

## E.1c Cited for one fact each

Later reports are not synthesised. Two are cited, because without them this book
would describe a boot chain the repository no longer has.

| ID | Title | Cited for | Where |
|---|---|---|---|
| 035 | The Last Borrowed Thing | The second-stage bootloader is this project's own: 2,736 B, `boot/`, no partition table read | Ch. 1 §1.2, Ch. 3 §3.1.1, Ch. 5 §5.8, App. A §A.2b |
| 036 | The Half-Speed Board | The kernel owns the clock. `kernel/clock.c` switches to the PLL; without it the board ran at 40 MHz and every `CCOUNT`-derived instrument was blind | Ch. 1 §1.2, Ch. 00b, Ch. 7 §7.8 |

Both are corrections of fact rather than of method, which is why they are cited
rather than told. The method they carry — *a successful measurement is not
evidence if the measurement and the fault share a dependency* — is Chapter 28's
Shape 4 arriving from a new direction, and it is recorded in Chapter 00b's clock
table where a reader will meet it first.

**Read Chapter 28c, or 030, before acting on anything in Chapter 18 §18.9.**
That section eliminates eleven theories correctly and reaches the wrong
conclusion, for a reason worth knowing: the system falls back to a path where the
defect is genuinely absent.

**031 and 032 are a pair.** The device model gave every application access to
every device; permissions took it back. Neither is complete without the other,
and both are bounded by the same missing piece — there is no image identity, so
what exists is containment rather than security.

## E.2 Reading orders

**New to the project (from `docs/README.md`):** 001 → 002 → 004 → 003.

**Picking up implementation work:** 006 (what is known-good) then 007 (what is
next and what it will break).

**Reproducing a build:** 005 alone is sufficient.

**Debugging anything:** 028 §8, 017 §6, 018 §5.1 — the three catalogues of
instruments that lied. This book's Chapter 28 consolidates them.

## E.3 Reports that were revised, and what changed

Revision is unusually informative here, because the project revises *in place*
with the superseded text left standing.

| Report | Rev | What the revision added |
|---|---|---|
| 007 | 1.1 | §2.1 — eleven reports of unplanned work against five of planned |
| 008 | 1.1 | §8 — the second writer to CCOMPARE1, and 183 ms of stopped clock |
| 009 | 1.1–1.3 | §11 — blocking, sleeping, priorities; §11.4 — ageing |
| 012 | 1.1 | §10 — seven syscalls added after M4, and the check each needed |
| 013 | 1.1 | §8 — messaging, copied never shared |
| 014 | 1.1–1.2 | §9 — priority inheritance; §10 — the same defect from the other side |
| 015 | 1.1–1.4 | SPI2, DMA, the renderer's two defects, **§5.8 overturning §5.7** |
| 017 | 1.1–1.2 | §8 — input confinement; §4.1, §7.1–7.4 — the inverted axis and the corner calibration |
| 019 | 1.1–1.2 | §6 — the fault persisted; §7 — the panic screen |
| 021 | 1.1–1.3 | §6 — icons and the close button; **§6.7 correcting §6.6** |
| 022 | 1.1 | The calibration fault corrected; §3.2's characterisation retracted |
| 026 | 1.1 | §4.1 — scrollback |
| 028 | 1.1 | §10.1 — the frozen-renderer measurement |
| 029 | 1.1 | A header note declaring the whole report superseded as to cause, written by the author of its own successor. §10's conclusions are marked wrong; §4's eliminations are marked still valid |

## E.4 Conclusions published and later retracted

Recorded here in one place because the project's own README lists honesty about
these as a rule.

| Claim | Where published | Where retracted |
|---|---|---|
| "Watchdogs are not armed at this point in boot" | 006 §6, 008 §6 | 009 §8 — measured, `rst:0x10` on every boot |
| "The IRAM origin conflicts with the bootloader" | 004 §5 (original) | 004 §5.1–5.3 — disproved by a 24 KB span experiment |
| "Pressure never exceeds 1 under a firm press" | 017 (original) | 017 §5.3 — actual 1123 |
| "PENIRQ never asserts across two full drags" | 017 (original) | 017 §5.3 — actual 17 |
| "Raw X increases left to right" | 017 §7 | 017 §7.1 — it decreases |
| "The touch mapping reads 24 px low on X" | 022 §3.2 | 022 §3.2 note — it is a magnification, not an offset |
| "`SPI_CTRL2` MISO delay is disproven" | 018 (during) | 018 §5 — untested; the run never flashed |
| "The framebuffer makes no measurable difference" | 015 §5.7 | 015 §5.8 — both figures taken through two faults |
| "The relayout broke the 3D view" | 021 §6.6 | 021 §6.7 — geometry alone breaks nothing; it was the camera |
| "A rising completion count means the frame went out" | 028 §3 (initial) | 028 §3 — a phone caught it |
| "Referencing libphy costs 48 KB of IRAM" | `MAC-NEXT.md` | 028 §3 — 2,459 bytes |
| "The chrome column paints over the 3D view" (post-relayout) | `desktop.c` guard | `desktop.c` — geometry says otherwise; narrowed to an assertion |
| "`periph_module_reset()` returns a zero mask for WiFi" | 028 §3.1 | 028 §3.1 rev 1.2 — asserted from memory, never verified |
| "The framebuffer dump shows a render bug" | during 030 | 030 §5.1 — the dump corrupted its own evidence; the buffer was pristine |
| "`camfreeze` + `fbhash` settles render-versus-transport" | during 030 | 030 §5.3 — a frozen camera rendering a *wrong* scene hashes constant too |
| "The raised timeout bound was unnecessary" | during 029 | 030 §5.4 — circular; the raise is why it read zero |
| "The 55.9 ms blit is correct; keep it" | Ch. 18 §18.9 | Ch. 18 §18.9 note — that was the fallback path, where the bug is absent |
| "A task blocking on a held mutex raises the owner's priority" | 014 §9, commit `ee19907` | 014 §9 rev 1.3 — `task_boost()` is never called |
| "The repair needs continuous interleaved drawing" | 029 §10 | 030 §3 — it needs a *timeout*; drawing wide enough to use DMA is how one is provoked |
| "DMA has been dead since boot, and that is the display's problem" | 029 §6 | 030 §1 — true, and it was the only thing making the display usable |
| "The bootloader replacement is verified; every subsystem passes" | 035 §8 | 036 §1 — every self-test is logical, and the board was running at half speed |

Twenty-one retractions. Every one is still in the record next to its correction.

The three added here share a shape worth naming: each is a *correct* observation
promoted to a conclusion it does not support. Drawing really does repair the
view; DMA really was dead; every self-test really did pass. What failed in all
three cases was the step from *this is true* to *therefore this is the
mechanism*.

The last one is the oldest surviving error in the project and the only one found
by *reading* rather than by measuring: three documents and a commit message
described a feature, and the commit's own diff never touched the file that would
have had to change.

## E.5 Predictions that came true

| Predicted in | Prediction | Realised in |
|---|---|---|
| 011 §4 | A flash write will collide with the data cache | 018 §3 — "It broke exactly as predicted, on the first version of the driver" |
| 010 §8 | `arena_contains()` exists and nothing calls it | 012 — closed by the interpreter |
| 010 §7 | A full framebuffer and concurrent applications cannot coexist | 015 — the driver has none |
| 002 §6 | Flash cache is configured but unused; will become load-bearing | 011 |
| 007 §4 | An incorrect register save will manifest later, in unrelated code | 009 §6 — exactly that shape |
| 007 §7 | The isolation claim will be tested by a program written to escape | 013 §5.2 |

And one that has not:

| 009 §9 | Nothing has audited the other consequences of `PS.EXCM` | **Still open** |

## E.6 Where each standing rule came from

| Rule | Report §|
|---|---|
| Clear `PS.EXCM` before calling C | 009 §6 |
| One writer, or every writer maintains the shadow | 008 §8 |
| A yield must never defer the clock it depends on | 016 §3 |
| Contention cost is a count, not a duration | 014 §10.5 |
| An instrument reporting itself invalid outranks the number beside it | 017 §6 |
| A negative result needs a demonstrably-run experiment | 018 §5 |
| A startup artefact is not evidence | 019 §4.1 |
| Never infer direction from a gesture's endpoint | 017 §7.1 |
| A cross-check only tests what its samples distinguish | 020 §4.2 |
| A correct diagnosis does not license arbitrary scope | 021 §6.6 |
| Tolerating a defect is not fixing it | 022 §3.3 |
| Reverting two changes destroys which one mattered | 021 §6.7 |

And the seven earned after the book was first compiled (Ch. 29 §29.6):

| Rule | Report § |
|---|---|
| 13 — A counter cannot see a picture | 030 §6, rule 1 |
| 14 — A diagnostic sharing a resource corrupts the evidence | 030 §6, rule 2 |
| 15 — Read positions; never infer them from arrival order | 030 §6, rule 3 |
| 16 — A tool must refuse to interpret incomplete input | 030 §6, rule 4 |
| 17 — Before fixing a defect, find out what depends on it | 030 §6, rule 5 |
| 18 — One-bit register constants deserve the datasheet | 030 §6, rule 6 |
| 19 — A guard is shown unnecessary only by measuring it in its absence | 029 §7.2, restated 030 §5.4 |

## E.7 The status table

`docs/README.md` maintains a live status table. Its current state, condensed:

**Complete and verified on hardware:** M0–M5, isolation, the full stack,
locking, task blocking, scheduling with ageing, the 3D renderer, the display,
application graphics, touch, the ADC, the on-screen shell, audio, application
input, application messaging, application bitmaps, the flash cache, persistence,
failure handling, fault reporting, the launcher, notes, stack margins.

**Infrastructure only:** the interrupt matrix — "peripheral interrupts routable
for the first time; PENIRQ proven by injection but never observed to fire from a
finger".

**Bus verified, nothing attached:** I²C — "no byte has yet been transferred".

**Reading only:** microSD.

**Measured then, unmeasured now:** the DRAM budget.

**Closed:** the bootloader IRAM overlap, the panic handler, the watchdogs.

**Ordered, not in hand:** the JTAG debug probe.

## E.8 The report format

Every report follows the same shape, and it is worth naming because the shape
does work:

1. **Abstract** — what this establishes, in two paragraphs, including what went
   wrong.
2. **Design decisions** — usually a table of alternatives with the selected one
   marked, and the deciding argument in one sentence.
3. **Implementation** — file by file.
4. **Verification method** — how the claim was turned into a number.
5. **Results** — raw captured output, then interpretation.
6. **The defect** — where there was one, with wrong hypotheses recorded.
7. **Metrics** — a table, including process metrics like "build cycles spent"
   and "wrong hypotheses recorded".
8. **What this does not establish** — the gaps.
9. **References** — other reports and the source files.

Section 8 is the one the README calls "the most useful part", and Chapter 30 is
all thirty of them merged, plus the closures and new gaps from 031–032.

One report in the set does not fit that shape, and the exception is deliberate.
UM-NATOS-029 carries a header note, added by the author of its successor,
declaring the whole document superseded as to cause — then leaves every section
standing, including the eleven eliminations that were right and the conclusion
that was wrong. It is the only report in the project that is *kept because it is
wrong*, and Chapter 28b is in this book for the same reason.
