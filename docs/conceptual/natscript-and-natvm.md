# NatScript and NatVM — Revised Proposal

**Status: conceptual.** Nothing here is committed to. Revision 2 rewrites the
original proposal against what nat-os actually is as of 2026-08-18, after the
device model and argument harness shipped (UM-NATOS-031).

The original is kept at `NatScript Language and NatVM Architecture Proposal.pdf`.
This version changes the emphasis in four places, and each change is argued
rather than asserted:

1. **The differentiator is isolation, not the language.**
2. **`when` and `every` are the same missing feature, and it is a VM feature.**
3. **Bytecode portability is worth less than it sounds.**
4. **The prior art is industrial control, not Java.**

---

## 1. What this is for

The purpose is not another general-purpose language. It is to make programming
a physical computer feel like programming a computer.

```
NatScript source
      ↓
NatScript compiler
      ↓
NatOS bytecode
      ↓
NatVM
      ↓
NatOS device model
      ↓
physical hardware
```

A developer should be able to say *"I have a scanner"* rather than *"I have a
peripheral on this bus behind these registers."*

That much of the original stands unchanged, and half of it already exists.

---

## 2. What already exists (2026-08-18)

The original said the VM was "the beginnings of the necessary execution
architecture." It is further along than that now, and knowing exactly how far
changes what the language has to invent.

| capability | state |
|---|---|
| bytecode ISA, 4-byte fixed instructions, 16 registers | shipped, ~30 opcodes |
| assembler (`tools/vasm.py`) | shipped, host-side |
| per-application arenas, offset-domain bounds checks | shipped |
| viewport confinement (a program cannot draw outside its strip) | shipped |
| fault isolation — one program dies, others continue | shipped |
| **device model** — a peripheral is a table entry, reached by name | **shipped** |
| **validated-argument harness** — one place `(offset, length)` is checked | **shipped** |
| **caller identity** on every device call | **shipped** |
| 13 syscalls, the last of which reaches the device table | shipped |

**`device scanner;` is nearly free.** `DEV_OP_NAME` already returns a device's
name into the calling program's own arena, so a NatScript `device` declaration
is a compile-time lookup against a table that exists.

**Permissions are nearly free too**, for an accidental reason. `device_t` grew a
`caller` argument because the `store` device needed to bank persistent slots per
application. Every device call therefore already carries *who is asking*. A
permission check is one bitmap indexed by caller, consulted in `device_read` and
`device_write`. The plumbing was built for an unrelated purpose.

---

## 3. The differentiator is isolation

The original sells the language. The language is the least defensible part.

MicroPython already lets someone write `while True: sensor.read()`, has a far
larger library ecosystem, and is familiar to everyone. Arduino won its category
on libraries, not syntax. Espruino, CircuitPython and Lua-on-ESP all exist. A
new syntax competes badly against all of them.

**What none of them has is application isolation.**

MicroPython does not isolate applications from each other. FreeRTOS does not.
Arduino does not. A misbehaving program takes the device with it, every time.

nat-os does isolate: arenas, viewport confinement, offset-domain arithmetic on
every program-supplied quantity, and a faulting program that is terminated alone
while its neighbours keep running. That has been verified repeatedly, including
by a program written specifically to violate it (`gfxrogue`).

That is the rare property. It is what makes *"the customer writes their own
application for their own warehouse"* a reasonable thing to offer rather than a
support nightmare, and it is what the permissions model in §7 rests on.

**Recommended positioning: isolation and permissions are the product; NatScript
is how you reach them.**

---

## 4. `when` and `every` are one missing feature

The original treats event syntax (§5) and recurring tasks (§6) as two ideas.
They are the same idea and it is a **VM** change, not a language change.

```
when button.pressed { ... }      needs the kernel to call INTO a program
every 10ms { ... }               needs the kernel to call INTO a program
```

A nat-os application today is **one linear thing with one program counter**. It
runs from `start`, and the kernel's only interaction is to give it a quantum.
There is no mechanism to enter it at a second location.

Everything the system currently offers is the opposite direction:

- **polling** — `sys device`, `sys touch`: the program asks, the kernel answers
- **mailboxes** — `sys send` / `sys recv`, program to program

So before any `when` syntax can exist, the VM needs:

1. **Multiple entry points per program.** A handler table in the image, so the
   kernel can call `on_button_pressed` rather than resuming `main`.
2. **A register/frame discipline across that call.** Which registers a handler
   may clobber, and where its locals live.
3. **An event source.** Devices currently *refuse* when they have nothing
   (`keys` channel 0 returns "empty"). Something has to convert that into a
   delivery.
4. **A decision about re-entrancy.** What happens when an event arrives while a
   handler is running.

None of that is hard. All of it is prerequisite, and none of it is language
design. **This is the real Phase 2 work.**

---

## 5. Concrete VM gaps a compiler will hit

The original's §15 says document the ABI before designing syntax. Doing part of
that now, these are the specific things that would bite:

**There is no stack frame convention.** `call` and `ret` exist, with a call-depth
fault, but the sixteen registers are **global**. There are no frames, no spill
area, no stack pointer. A compiler emitting nested calls with locals would have
to invent a frame discipline entirely in arena memory — and if it is not decided
in the VM, every program carries its own incompatible one.

*This is the single most important thing to fix before a language targets it.*

**`ldw ra, rb, off` caps `off` at 255.** Array indexing works — add, then load at
offset zero — but a code generator has to know that is the shape.

**Immediates are 16-bit.** A 32-bit constant needs `ldi` + `ldih`. Fine, but the
compiler must never emit one without the other.

**No multiply-accumulate, no floating point.** Fixed point throughout, as the
raycaster already does. A language that lets people write `0.5` without saying
what that costs is lying to them.

**The syscall ABI is register-positional and undocumented outside comments.**
`sys device` takes its operation in r0 and arguments in r1..r4. That is fine for
hand-written assembly and needs writing down properly before a compiler depends
on it.

---

## 6. Prior art, and why not Java

The original compares NatScript to Java. The JVM comparison is structurally
accurate and strategically misleading — it invites the reader to imagine a
general-purpose runtime with a garbage collector.

The nearer and more useful prior art is **industrial control**:

- **IEC 61131-3** — Structured Text, Function Block Diagram, and Ladder Logic.
  Built around a *scan cycle* and event handling. These solved the "program a
  physical machine" problem in the 1970s and remain in production everywhere.
  Unfashionable, and worth reading precisely for that reason.
- **Ladder logic** specifically encodes "when this condition, then that action"
  as the primitive, which is what `when` is reaching for.
- **Erlang/Elixir** for message-driven `when` semantics and supervision trees —
  the "let it crash, restart the subtree" model maps well onto isolated
  applications that can fault independently.
- **MicroPython / CircuitPython** as the realistic competitor to study honestly.
- **Arduino** as the reminder that ecosystems beat syntax.

---

## 7. Permissions

Worth doing early, and now cheap. A program declares what it needs:

```
permissions {
    scanner;
    network;
    display;
}
```

The enforcement point already exists — `device_read(caller, id, chan, ...)`. What
is missing is the *identity* half: a permission grant is only meaningful if the
image it applies to cannot be swapped for another. That means image signing or
at minimum a content hash, and it is a larger question than the check itself.

**Honest sequencing:** implement the check first, treat the manifest as advisory,
and do not claim security until the identity problem is solved.

---

## 8. Bytecode portability is worth less than it sounds

The original imagines the same NatBytecode running on ESP32 and ARM.

This is the weakest ambition in the document. The VM is a few hundred lines and
is the *easy* part of a port. What does not port is every driver, every register
map, every timing assumption, every interrupt route — which is to say the entire
rest of the system. Bytecode portability buys the cheapest five percent of the
work and then stops.

Keep the VM portable because it costs nothing to avoid ESP32 assumptions in it.
Do not treat it as a feature, and do not let it drive design decisions.

---

## 9. Revised development sequence

The original's Phase 1–7 is sound in spirit. This reorders it against what
exists.

**Phase 1 — Document the ABI properly.** Every opcode, encoding, register
behaviour, call convention, syscall signature, fault behaviour. Appendix B of
the book is the oldest part of the stack and the least re-verified.

**Phase 2 — Fix the frame convention.** Decide, in the VM, where locals and
parameters live. Everything else waits on this.

**Phase 3 — Multiple entry points and event delivery.** The prerequisite for
both `when` and `every`. Ends with a hand-written assembly program that
registers a handler and receives an event.

**Phase 4 — Permissions check** against the existing `caller`, manifest
advisory.

**Phase 5 — A minimal NatScript**: variables, expressions, functions,
conditionals, loops, `device`, `when`, `every`. Nothing else.

**Phase 6 — Compile to the existing bytecode**, and only then find out whether
the ISA is a good target.

**Phase 7 — Real applications.** Rewrite `app_dev.vasm` in NatScript. If it is
not shorter and clearer than the assembly, the language has not earned itself.

Phases 1–3 are VM work with no language in sight. That is the honest shape of
it.

---

## 10. The principle, unchanged

> Make physical computing feel like programming a computer, rather than
> programming a collection of electrical components.

That still holds. What changed is the order of the work and the claim being
made: **the language is the interface, the isolation is the product.**

The immediate goal is not to prove the vision. It is to make one small physical
computer demonstrate that the abstraction works — and as of today, one does.
