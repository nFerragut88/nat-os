# 06 — Claims that have gone stale

**Size:** small. **Risk:** none. **Blocked on:** nothing.

This project's whole discipline is that the record must not lie. These are
places where it currently does, through nothing worse than the tree moving on.

---

## 6.1 UM-NATOS-028's header

The body now contains a `§3.1` correction labelled "rev 1.2" and two new
sections, but the header still reads:

```
Revision 1.1 · 2026-08-16 · Status: Receive working, transmit not, one display fault still open
```

Both halves are wrong now. The display fault is **closed** (UM-NATOS-030), and
the revision is behind its own body.

Fix: bump to **1.2**, and change the status to something like *"Receive working,
transmit not; the display fault is closed in UM-NATOS-030."*

## 6.2 `docs/book/31-next.md`

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

## 6.3 The ISA reference is the oldest thing in the stack

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

## 6.4 The book does not know about any of today

`docs/book/` synthesises reports 001–028. Reports 029, 030 and 031 exist now, and
030 in particular invalidates things the book says confidently about the display
being reliable.

Not urgent — the reports are the primary record and the book says so. But if the
book is ever rebuilt for print, chapters 18 (display), 25 (renderer), 28
(instruments that lied) and 31 (next) all need revisiting, and 28 in particular
gets a much better final example.

## 6.5 Rebuild the PDFs

`docs/pdf/` is stale for anything touched since 028. `docs/style/build_pdfs.py`
does the work; it needs Chrome or Chromium and now falls back to an installed
Chrome if no bundled Chromium is present.

Lowest priority of everything in this folder.
