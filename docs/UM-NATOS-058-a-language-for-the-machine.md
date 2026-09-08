# UM-NATOS-058 — A Language for the Machine

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-07 · Status: **NatScript compiles, and every part of it has run on the board it was written for.**

---

## 1. Abstract

```
                          assembly      NatScript
app_dev   source lines         117            37      3.2x shorter
          bytecode           583 B       1,381 B      2.4x larger
app_evt   source lines          55            16      3.4x shorter
          bytecode           328 B         551 B      1.7x larger
```

This report covers `next_moves/08` steps **355–366**. UM-NATOS-057 covers
349–352.

At step 354 nat-os had a bytecode VM, an assembler, and thirteen programs
written by hand in registers and syscalls. It now has a compiler, a language
with variables, functions, recursion, buffers, strings, devices reached by
name, and `when`/`every`, and a program written in it that draws on the panel
and answers a finger.

**The interesting part is not the language.** It is that the last four defects
in this report were found by the machine rather than by reading: one by a test
suite on its first run, one by a program on hardware, one by a diagnostic that
lied about its own cause, and one by the board disagreeing with a simulator.
§6 is about that.

---

## 2. The prerequisites were closer than the list said

The NatScript proposal set out thirteen items. Four of them were already true
and nobody had checked.

**VM-03, a frame convention**, was called *"the single most important thing to
fix before a language targets it."* It took **one line**:

```c
vm->reg[15] = size & ~3u;      /* vm_init() */
```

The reason it could be one line is the property the VM already had: every load
and store is bounds-checked against the arena in software. A stack built inside
the arena is therefore exactly as safe as any other data, and running off it is
`VM_FAULT_BOUNDS` — the same fault a bad pointer gets, caught by machinery that
had been there since the VM was written. No new opcode, nothing for the VM to
understand about frames.

**VM-05 and VM-06, the event ABI**, were already implemented. `when` and `every`
turned out to be a *parser change*: the kernel delivers an event by pushing the
interrupted `pc` onto the same return stack `call` uses, so a handler is an
ordinary function and `ret` unwinds the injection. There is no separate handler
calling convention to get wrong.

**That is the return on writing the ABI down first** (`docs/vm-abi.md`, step
354). It was written as a description of what the code did. Two steps later it
was used as a specification, and not a word of it had to change.

### 2.1 Except one word

`vm-abi.md` said `r14` was *"a frame pointer (optional; a compiler with fixed
frames may skip it)"*. That was wrong, and the first compiler to target the ABI
found out why.

Any code generator with an **expression stack** — pushing the left operand of a
binary operator while the right is evaluated — moves `r15` in the middle of the
statement that reads a local. A local addressed as `r15 + 8` is somewhere else
after the first `+` in the expression addressing it.

So locals go through `r14`, the callee saves and restores it, and the document
was corrected in place. It had reserved the right register for a reason it had
not identified.

---

## 3. The compiler emits assembly, not bytecode

The decision that made this a day's work rather than a week's.

`vasm.py` already resolves labels, lays out `.string`/`.word`/`.space`, emits
the generated header and carries the permission manifest. A backend inside
`natc` would have been **a second encoder of the same instruction set** — and
every *"these two must agree"* comment in this project has eventually failed.

One failed while this was being written. `kmain.c` had its own copy of the
program launch under a comment reading *"the boot path and the shell path must
agree, or a program started at boot would behave differently from the same
program started by typing its name."* The comment was correct and the structure
could not keep it: the shell had just started resolving a permission manifest,
and the boot copy would have gone on granting a field that no longer existed.
The compiler caught it, which is luck — the same divergence in behaviour rather
than in types compiles fine.

It also leaves a readable intermediate. 437 lines of assembly a person can check
against `vm-abi.md`, which is how a compiler earns trust it has not been given.

---

## 4. Permissions: the image declares, the kernel disposes

Permissions were enforced before this. What did not exist was any way for a
**program** to say what it wanted: the bitmap was hand-written in the kernel's
program table, so **the kernel decided what the program needed.**

That is backwards, and it is exactly why a `permissions { }` block had nowhere
to compile to.

```
    .permission light          ->   #define VM_APP_DEV_PERM_COUNT 3u
    .permission store               static const char *const
    .permission echo                    vm_app_dev_perms[] = {"light","store","echo"};
```

An unknown name **refuses the launch** rather than dropping the permission and
starting a program that reaches for a sensor silently not there.

### 4.1 The bitmap was a positional bug waiting

The seven `P_*` defines deleted from `kmain.c` were a hand-maintained mirror of
`device.c`'s **table order**:

```c
#define P_STORE (1u << 2)      /* because "store" is the third entry */
```

Nothing checked the two lists against each other. Inserting a device above
`store` would have silently re-aimed every grant below it at different hardware,
and every program would have kept running — no symptom until something wrote to
the wrong bus.

**That bug never fired. It was available for about two hundred steps.**

### 4.2 And it constrained the language

A compiler turning `light.read()` into `ldi r1, 0` would have put that same
positional coupling back — not in one kernel file this time, but **in every
program it ever produced**.

So `DEV_OP_FIND` is the inverse of `DEV_OP_NAME`: a name out of the arena, an id
back, resolved once at startup. It discloses nothing new — `DEV_OP_COUNT` and
`DEV_OP_NAME` already let any program walk the whole table.

Out of which fell something better than what was designed: **the permissions
block is the device namespace.** A device that is not declared is not a name the
program can write, and using one is a compile error. What a program may reach
and what it can *say* are one list.

---

## 5. What the language is

Documented in `docs/natscript.md`, revision 0.9. In one page:

| | |
|---|---|
| values | 32-bit integers, signed. No floats — the VM has none |
| memory | `buf name[16]`, and **a buffer's value is its arena offset**. That is the whole of what a pointer is here: an integer the VM bounds-checks on every use |
| text | `format(line, "taps ", n)` writes into a buffer you declared. §5.2 |
| devices | `light.read(0)` → 1 or 0, reading in `light.value` |
| screen | `screen.fill/text/blit`, `screen.touched()`, `screen.width/height` |
| events | `every 100ms { }`, `when key(k) { }` |

### 5.1 Every device call reports whether the device agreed

`d.read(chan)` evaluates to 1 or 0, and the reading is in `d.value`.

Having `read` evaluate to the reading makes a refusal indistinguishable from a
sensor reporting zero — which is the failure this project has spent two reports
diagnosing, most memorably a program that *"announced sixteen readings and
reported a light level of zero without ever having read anything."*

Same rule gave `screen.touched()` its shape, and `screen.width`/`height` their
opposite one: those **ask every time** rather than reading what a `screen.size()`
call left behind, because anyone who forgot to call it would get 0, silently.
`size()` was written and then deleted.

### 5.2 Strings, without an allocator

There is no heap inside an arena and there is not going to be one. A heap inside
a bounds-checked fixed span would be a second memory manager, in bytecode, on a
machine with 3 KB per program.

`"taps: " + n` — what the proposal asked for — needs somewhere to put the
result, and the only somewhere is a buffer the compiler picks. Two such
expressions live at once would quietly share it. **Naming the destination costs
one argument and removes the whole class.**

The limit is the buffer's **declared size**, not a number the program passes and
could get wrong. A format that does not fit is truncated and stays terminated.

It made `app_tap` **shorter and smaller**: nine lines of open-coded digit
arithmetic became one, and the image went 2,503 → 2,149 bytes. That is not
usually how the trade goes.

---

## 6. Four defects, none found by reading

The part of this report worth keeping.

### 6.1 A test suite, and the lie it found on its first run

`natc.py` had no tests. Three steps of language work had gone in with a person
reading generated assembly as the only detector.

The first case, on arithmetic **precedence**, failed on a line that had nothing
to do with precedence:

```
println(0 - 6)      wanted -6, got 4294967290
```

`sys putd` prints **unsigned** — `vm-abi.md` §4 has always said so. Every
comparison the compiler emits is **signed**. Both facts were written down.
Nobody had put them next to each other.

The language had been telling its user something untrue about its own arithmetic
since the day it first printed a number, and three shipped programs were
compiled with it. Nothing looked wrong, because none of them prints a negative.

### 6.2 The board found the same class again, deeper

`app_tap`, written assuming it owned the screen, was handed a **14-pixel**
viewport and computed `(h - 16) / 3`. It got **1431655764**.

`vm.c` keeps registers in a `uint32_t` and writes `r[a] = r[b] / r[c]` with no
cast: **DIV and MOD are unsigned**, while `<` is signed and `print` is signed.
A language that compares signed, prints signed, and divides unsigned.

Nobody would have found that by reading. It took a program on hardware doing
arithmetic on a number it had not expected.

### 6.3 The oracle disagreed with the kernel

`tools/natvm_ref.py` — the host reference the test suite runs programs in —
implemented division **signed**.

So the reference and `vm.c` disagreed. That is exactly what a second
implementation of an instruction set does eventually, which is why the file's
header opens with **"NOT AUTHORITATIVE — kernel/vm.c is"** and says that nothing
in the kernel may be changed to make a test pass. The rule was applied without
argument.

**The suite could not have caught this**: the reference was signed, the kernel
unsigned, and no case exercised a negative dividend. A test suite proves what it
tests.

### 6.4 A diagnostic that named two causes and would not say which

```
   cannot start: no free slot or no memory
```

The heap had 28,728 bytes free and a 28,728-byte largest block. It was never
memory. But memory was named first, so memory is what got investigated — an
arena was trimmed and the board reflashed, for nothing. Only when a 512-byte
program failed identically did the theory die.

`ARENA_MAX` is 4, the kernel permanently keeps one for its own VM task, and
`ping` and `pong` start at boot and never exit. **One application slot exists in
practice.**

A diagnostic that lists its own possible causes without saying which one
happened is worse than one that says less. It reads as information and spends
the reader's time. This log has a long record of statuses reporting an outcome
for work that never ran; this is a nearer relative than it looks — not a false
claim, but a true statement carrying none.

---

## 7. The instrument was broken four times and the system was fine

Recorded because the ratio is the lesson, and because this log's usual failure
is the opposite one.

| | |
|---|---|
| three captures | read as *"the board is silent"*. A stale process held COM5, every open failed with "access denied", and the script swallowed it |
| one report | *"the app strips have vanished"*. The program had been running the whole time |
| one reflash | trimmed an arena that was never the problem (§6.4) |
| one capture | typed its command into a board that was still booting, because opening the port resets it |

**The board was working in all four cases.** The correction is the same as for
believing a false status: measure the measurer first.

---

## 8. What it is like to use

`tools/app_devnat.nat` against `tools/app_dev.vasm`, both registered so `run
dev` and `run devnat` compare on hardware rather than on the page. They print
the same device table, round-trip the same transfer through the loopback, take
the same sixteen readings.

The source result is the language earning itself: an enumeration loop is four
lines instead of a register-allocated counter, a comparison, a branch and a
manual increment; two event handlers and their registration are eight lines
instead of thirty-one.

**The bytecode result is the price**, and it is reported rather than left to be
found with `ls`. Every operand is pushed and popped, because that is correct for
an expression of any depth and has no case analysis to get wrong — worth more
than the instructions, for a compiler with no test suite behind it at the time.
On a board with 4 MB of flash and arenas measured in kilobytes, source is the
scarce thing. That is a judgement about this machine, not a general one.

### 8.1 What the rewrites could not carry over

The assembly's comments. `app_dev.vasm` carries a paragraph about a jump that
landed past its setup code and silently skipped a slot claim; `app_evt.vasm`
explains that its spin counter exists so a clobbered register would show in
`ps`. Those are the most valuable part of those files, they are not a language
feature, and they were copied across by hand.

**A language makes the code shorter. It does not make the reasoning shorter, and
the reasoning is what took the time.**

---

## 9. Touched

```
[tap] cell 0 at 12,3 taps 1
[tap] cell 1 at 36,3 taps 2
[tap] cell 2 at 71,1 taps 3
[tap] cell 3 at 101,12 taps 7
```

A program written in NatScript, drawing on the panel, answering a finger.

Every coordinate is **viewport relative** — x 0–157, y 0–13, inside a strip
beginning at panel y=256. The program never learns where its strip is, and the
forty withheld touches are the ones outside it. The isolation the whole system
exists for, holding on a program the kernel has never seen.

The cell arithmetic is §6.2's fix doing real work: `cw` is `180 / 6`, and
`screen.x / cw` maps 12, 36, 71 and 101 onto cells 0–3. The bug that began as a
wrong grid is the code that proves its own repair.

---

## 10. What remains

1. **One application slot in practice** (§6.4). `ARENA_MAX` is 4, the kernel
   keeps one, and two boot programs hold two of the remaining three for an IPC
   self-test that has passed since step ~90.
2. **VM-08, image identity.** `device.h` has said it since permissions were
   written: *"a permission grant is only meaningful if the image it applies to
   cannot be substituted."* Nothing is signed. A manifest is what a program
   **asks** for, and asking is not proof — the permissions in this report are
   containment, not security, and calling them security would be the eighth
   thing in this project to claim an outcome it had not earned.
3. **No string comparison, substring, or string-valued returns** (§5.2).
4. **Per-task stack sizing** — 8–10 KB, and it is the scheduler.
5. **The null-`sp` window fault** — identified at 351, never reproduced.

**A language, a compiler, a test suite, and a program that draws on a panel and
feels a finger — on an operating system, a VM, a device model and a touch driver
that were all written from scratch.**
