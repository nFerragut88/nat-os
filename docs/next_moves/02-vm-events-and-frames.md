# 02 — VM entry points and a frame convention

**Size:** medium. **Risk:** low. **Blocked on:** nothing.

*The recommended next move.* It unblocks the language, nothing depends on the
answer being a particular one, and the failure modes are loud.

---

## Why

Two separate-looking wishes from the NatScript proposal are the same missing
feature, and it is a **VM** feature rather than a language one:

```
when button.pressed { ... }      the kernel must call INTO a program
every 10ms { ... }               the kernel must call INTO a program
```

A nat-os application today is **one linear thing with one program counter**. It
starts at offset 0 and the kernel's only interaction is handing it a quantum.
There is no way to enter it anywhere else.

Everything the system currently offers runs the other direction: polling
(`sys device`, `sys touch`) and mailboxes (`sys send`/`sys recv`).

## Part A — a frame convention (do this first)

**There is no stack frame convention, and this is the single most important gap.**

`call` and `ret` exist, with a call-depth fault. But the sixteen registers are
**global** — no frames, no spill area, no stack pointer. A compiler emitting
nested calls with locals would have to invent a frame discipline entirely in
arena memory, and if the VM does not decide it, every program carries its own
incompatible one.

Decide, in `kernel/vm.c` and `kernel/vm.h`:

- Which register is the frame pointer (suggest a high one, `r15`)
- Whether the VM maintains it or programs do
- Where locals live — top of arena growing down is the obvious choice
- Which registers a callee may clobber vs must preserve
- What happens on overflow: it must **fault**, like every other bound in this
  kernel, not wrap

Then document it. This is Phase 1 of the NatScript sequence and it is small.

## Part B — multiple entry points

A program image needs a **handler table** so the kernel can call something other
than `start`. Sketch:

```
offset 0    : entry, as today
offset 4..N : handler table -- (event id, code offset) pairs
```

`vasm.py` would gain a directive:

```
.on  key_pressed, handle_key
.on  tick_10ms,   update
```

The assembler already emits labels and knows their offsets, so this is
mechanical — see `emit_header()` in `tools/vasm.py`, which already writes
`_AT_<LABEL>` defines.

## Part C — delivery

`app_tick()` in `kernel/app.c` currently calls `vm_run(&a->vm, quantum)`. It
would gain: before resuming normally, check whether an event is pending for
this application, and if so enter at the handler instead.

Decisions needed, and they are the real design work:

1. **Re-entrancy.** What happens when an event arrives while a handler runs?
   Suggest: queue it, one deep, drop the rest and count the drops. Never
   re-enter.
2. **Registers across the call.** The frame convention from Part A answers this.
3. **Where events come from.** Devices currently *refuse* when they have nothing
   (`keys` channel 0 returns empty). Something must turn "the queue became
   non-empty" into a delivery. Simplest first version: the kernel polls the
   device on the application's behalf, which needs no driver changes at all.
4. **`every 10ms`.** A timer entry in the same table, serviced from the existing
   tick. Cheaper than the event path and worth doing first as the easier half.

## How to know it works

Write it in assembly before any language exists:

```
; app_evt.vasm -- registers a handler, prints the key, exits after 5
.on key_pressed, on_key
start:
        ...
on_key:
        ; r0 = the character
```

If a hand-written program can receive five keypresses through a handler and
exit cleanly, the mechanism is real. **That is the milestone** — not the syntax.

## Do not

Do not design NatScript syntax first. The proposal's own §15 warns that a
beautiful language targeting an awkward VM moves the problem one layer up, and
that warning is correct.

## Where the code is

- `kernel/vm.c` / `vm.h` — the interpreter, `VM_REGS 16`, the syscall switch
- `kernel/app.c` — `app_tick()`, the per-application lifecycle
- `tools/vasm.py` — the assembler; `OPS` and `emit_header()`
- `docs/conceptual/natscript-and-natvm.md` §4, §5 — the reasoning
- `docs/book/14-the-isa.md` — the ISA as documented (**oldest, least
  re-verified part of the stack — check it against `vm.c` before trusting it**)
