# 06 — Claims that have gone stale

**Size:** small. **Risk:** none. **Blocked on:** nothing.

This project's whole discipline is that the record must not lie. These are
places where it currently does, through nothing worse than the tree moving on.

> **Status 2026-08-18: 6.1 through 6.4 are done.** Only 6.5 (rebuild the PDFs)
> remains.

---

## ~~6.1 UM-NATOS-028's header~~ — done

The body now contains a `§3.1` correction labelled "rev 1.2" and two new
sections, but the header still reads:

```
Revision 1.1 · 2026-08-16 · Status: Receive working, transmit not, one display fault still open
```

Both halves are wrong now. The display fault is **closed** (UM-NATOS-030), and
the revision is behind its own body.

Fix: bump to **1.2**, and change the status to something like *"Receive working,
transmit not; the display fault is closed in UM-NATOS-030."*

## ~~6.2 `docs/book/31-next.md`~~ — done

Chapter 31 is the source the whole device-model effort came from, and it is now
describing a system that no longer exists:

> Seventeen reports later, the list of things applications cannot do has grown
> to: read the light sensor, scan or talk to the I²C bus, make a sound, save
> state, read the microSD card, receive text, read a key press, know anything
> about the network.

**Seven of those eight are done.** Only the network remains, and that is blocked
on transmit rather than on architecture.

The chapter's *argument* — that a device model was the missing structural item —
was correct and should be kept. It needs a closing note saying it was acted on,
with a pointer to UM-NATOS-031.

## ~~6.3 The ISA reference is the oldest thing in the stack~~ — done

`docs/book/14-the-isa.md` and `B-isa-reference.md` were written before the
device syscall, before `vmarg`, and before anyone tried to reason about the VM
as a compiler target. They have not been re-verified against `kernel/vm.c`.

This matters more than ordinary doc rot, because
[02](02-vm-events-and-frames.md) is about to build on them. Specifically check:

- the opcode table against the `switch` in `vm_run()`
- `VM_REGS` is 16 and the assembler agrees (`tools/vasm.py` allows `r0`–`r15`)
- `ldw`/`stw` offset range — the assembler caps it at 255
- the syscall list — the ISA doc predates `VM_SYS_DEVICE = 12`
- **that no stack frame convention is documented, because none exists**

**Outcome, 2026-08-18.** Four of the five checks passed unchanged: all 35 opcodes
agree with the `switch`, `VM_REGS` is 16 and the assembler's `reg()` enforces the
same, and both the doc and the assembler cap `ldw`/`stw` offsets at 255. The
suspicion in this item was right about *which* thing had rotted — the syscall
table stopped at 11, missing `device` (12) and `event` (13).

Fixed, plus what re-reading turned up that this item did not predict:

- `vm.h`'s own `sys device` comment listed operations 0–4 and had never gained
  `XFER_OUT`/`XFER_IN`. Appendix B calls `vm.h` authoritative, so this was the
  worse of the two errors.
- `vm.h` said "arguments in r0..r3" when `text` reads through `r5`.
- Appendix B's `PROGRAMS[]` example lacked the `perms` field added hours earlier
  by move 03 — the doc was stale before the commit that staled it had landed.
- Events were absent from Appendix B entirely. Now §B.5.
- The frame convention got its own section (§B.6) rather than a line, because
  "there is no convention" is not the same claim as "no convention is
  documented", and a compiler writer needs the first one stated plainly:
  no frame pointer, no argument convention, no callee-saved set, and no working
  recursion, all as the direct cost of putting return addresses in kernel memory.

Chapter 14 got since-written notes rather than a rewrite, since it is explicitly
the M4 milestone chapter and its historical framing is the point.

## ~~6.4 The book does not know about any of today~~ — done

`docs/book/` synthesises reports 001–028. Reports 029, 030, 031 and 032 exist
now, and 030 in particular invalidates things the book says confidently about the
display being reliable.

Chapters 14 and 31 and Appendix B have since been brought current by 6.2 and 6.3,
so what remains here is narrower than it was: 18, 25 and 28.

Not urgent — the reports are the primary record and the book says so. But if the
book is ever rebuilt for print, chapters 18 (display), 25 (renderer), 28
(instruments that lied) and 31 (next) all need revisiting, and 28 in particular
gets a much better final example.

**Outcome, 2026-08-18.** All four revisited, plus seven files this item did not
name. Since-written notes rather than rewrites, so the wrong conclusions stay
readable next to their corrections.

- **18** — title changed (it was never a stall), §18.9's diagnosis marked wrong
  and kept in full, §18.11 added for the one bit, the closing advice annotated
  with how it aged. The first bullet of that advice — "the 55.9 ms blit is
  correct" — was the one that misled, because 55.9 ms *was* the fallback path
  where the bug is absent.
- **25** — §25.14 added. Nothing in `raycast.c` was ever wrong; the renderer was
  the best instrument for a bug it did not contain.
- **28** — two new shapes. Shape 9, a fallback path where the bug is genuinely
  absent, cost eleven correct eliminations. Shape 10, the instrument corrupting
  its own evidence, was committed four times by tools built to prevent exactly
  this. Tally 27 → 32, and the human-observation list went to five for five.
- **30** — device model and syscall harness closed; DMA stall and 3D glitch
  closed; MISO, phantom touches and image identity added.

**And one thing this item did not anticipate.** Checking the front matter's
feature table against `task.c` turned up a **false claim of a shipped feature**:
priority inheritance does not run. `task_boost()` and `task_unboost()` exist and
nothing calls them; `mutex.c` contains no mention of priority; and
`git log -S task_boost` returns one commit whose diff never touches `mutex.c`.

It was claimed in UM-NATOS-014 §9 (now withdrawn, rev 1.3), in that commit's own
message, in the book's front matter, in `docs/README.md` and in the timeline.
What actually bounds inversion is ageing, which is real. The claim survived
because §9 shipped with a caveat precise enough to read as a report from someone
who had run the code — **a documented limitation is not evidence that the thing
it limits exists.**

The oldest surviving error in the project, and the only one found by reading
rather than measuring.

## 6.5 Rebuild the PDFs

`docs/pdf/` is stale for anything touched since 028. `docs/style/build_pdfs.py`
does the work; it needs Chrome or Chromium and now falls back to an installed
Chrome if no bundled Chromium is present.

Lowest priority of everything in this folder.
