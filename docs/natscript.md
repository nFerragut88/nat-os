# NatScript — the language, v0

**Used Medias LLC — Embedded Systems Division**
Revision 0.8 · 2026-09-07 · Covers `next_moves` VM-09 through VM-13.

NatScript compiles to NatVM bytecode. This document describes **what the
compiler in `tools/natc.py` actually accepts today**, not what the proposal
imagines. Where the two differ, §11 says so by name.

The proposal's own §15 warned against designing syntax before the ABI was
written down. The ABI is now written down and frozen — `docs/vm-abi.md`,
revision 1.2 — so this is the step after that warning, not around it.

---

## 1. The shape of it

```
NatScript  ->  natc.py  ->  vasm assembly  ->  vasm.py  ->  bytecode  ->  NatVM
```

**The compiler emits assembly, not bytecode**, and that is deliberate. `vasm.py`
already resolves labels, lays out `.string`/`.word`/`.space`, emits the
generated header, and carries the `.permission` manifest. A second encoder of
the same instruction set is the shape that every *"these two must agree"*
comment in this project has eventually failed at.

It also means the intermediate is readable:

```
python tools/natc.py tools/app_hello.nat -o build/nat/app_hello.vasm
```

produces assembly a person can check line by line against `vm-abi.md`, which is
how a compiler earns trust it has not yet been given.

`build.ps1` runs both stages for every `tools/*.nat`. The intermediate lands in
`build/nat/`, never in `tools/`, so a generated `.vasm` cannot be mistaken for a
source file and edited.

---

## 2. Program structure

A file is a sequence of top-level statements, function declarations, and at most
one `permissions` block, in any order. **The top-level statements are the
program**; there is no `main`.

```
permissions {
    light
    store
}

func double(x) {
    return x * 2
}

let total = 0
total = double(21)
println("total is ", total)
```

**A statement ends at the end of the line.** A `;` is accepted and never
required. Inside brackets newlines are ignored, so a long call may be split
across lines without a continuation character.

Comments: `//`, `--`, `#` to end of line, and `/* */` across lines.

---

## 3. Values

**Integers, 32-bit, and nothing else.** No floats — the VM has no floating
point, and a language that lets someone write `0.5` without saying what it costs
is lying to them. `true` and `false` are 1 and 0.

**A string literal can be printed and nothing else.** It has no type, no length,
and no operations. Using one where a number is expected is a compile error
rather than an address quietly printed as a number.

There are no structs. Fixed-size byte buffers, and an arena offset as the
only kind of pointer, are §7.

---

## 4. Statements

| | |
|---|---|
| `let name = expr` | declares. Inside a function it is a local; at the top level it is a global |
| `name = expr` | assigns to something already declared |
| `if expr { } else if expr { } else { }` | |
| `while expr { }` | |
| `return expr` / `return` | only inside a function |
| `expr` | evaluated for its effect |
| `every <duration> { }` | top level only — §9 |
| `when key(name) { }` | top level only — §9 |

Zero is false and everything else is true; there is no separate boolean type.

Redeclaring a name in the same function is an error. There is **no block
scoping**: a `let` inside an `if` is visible for the rest of the function. That
is a simplification, not a design, and §11 lists it.

---

## 5. Expressions

Loosest to tightest:

```
||
&&
==  !=
<  <=  >  >=
+  -  |  ^
*  /  %  &  <<  >>
unary -  !
```

**`print` is signed.** Every comparison this language emits is signed (`slt`,
`sle`), so a value that compares as negative must not print as four billion —
`println(0 - 6)` prints `-6`. `sys putd` is unsigned (`vm-abi.md` §4), so the
compiler emits a six-instruction helper once per program that needs it. This was
found by the test suite's first case (§12), not by reading the code.

`-2147483648` is the one value that cannot be negated, and prints without its
sign. A branch for it would cost every other number a comparison.

`&&` and `||` **short-circuit**, and yield 0 or 1 rather than the value of
whichever side decided it — so `x = a && b` stores a truth value.

`>` and `>=` compile to `<` and `<=` with the operands reversed. The VM has no
greater-than opcode and does not need one.

Comparisons are **signed** (`slt`, `sle`). Shifts: `>>` is logical (`shr`).

**`/` and `%` are signed too, and that costs a helper.** The VM's `DIV` and
`MOD` are *unsigned* — `vm.c` keeps registers in a `uint32_t` and divides them
with no cast. A language that compared signed, printed signed, and divided
unsigned would produce `(14 - 16) / 3 == 1431655764`, which is what a program
on the board actually computed from a fourteen-pixel viewport. So the compiler
emits a divide helper, truncating toward zero with the remainder taking the sign
of the dividend, as C does.

`divu(a, b)` and `modu(a, b)` are the raw machine operation, for quantities
that are not signed integers: an address, a device reading, a pixel.

### Built-in calls

| | |
|---|---|
| `print(...)` | each argument in turn — a string literal as text, anything else as a **signed** number |
| `printu(n)` | the same number unsigned. Device readings, bitmaps, addresses |
| `println(...)` | the same, then a newline. `println("")` is a bare line break |
| `putc(n)` `exit(n)` `ticks()` `dims()` `fill(x,y,w,h,c)` `text(s,x,y,fg,bg,scale)` | the syscalls of `vm-abi.md` §4 |

---

## 6. Functions, and where the locals live

```
func gcd(a, b) {
    while b != 0 {
        let t = b
        b = a % b
        a = t
    }
    return a
}
```

**At most four parameters**, because the ABI passes them in `r0`–`r3`.

The generated code is `vm-abi.md` §6 exactly, and this compiler is that
convention's first real user — which is the whole reason it was written down
before the language existed.

```
f_gcd:
        addi    r15, -4
        stw     r14, r15, 0     ; caller's frame pointer
        addi    r15, -8         ; this function's locals
        mov     r14, r15        ; our frame base
        ...
        addi    r15, 8
        ldw     r14, r15, 0
        addi    r15, 4
        ret
```

Three things follow, and each of them is a constraint on programs:

**Locals are addressed through `r14`, not `r15`.** The expression evaluator
moves `r15` — see below — so a local at a fixed offset from the stack pointer
would move underneath the code reading it. `r14` is why `vm-abi.md` reserves it.

**Parameters are spilled into the frame on entry.** A parameter left in a
register would be destroyed by the first nested call, and a language where an
argument survives or not depending on whether the body happens to call something
is not one anybody can reason about.

**A frame is capped at 256 bytes**, because `ldw`/`stw` offsets are one byte.
Exceeding it is a compile error naming the function.

### The evaluation model

Every expression leaves its result in `r0`, and a binary operator pushes its
left operand to the arena stack while the right is evaluated:

```
    eval left  -> r0
    push r0
    eval right -> r0
    mov  r1, r0
    pop  r0
    add  r0, r0, r1
```

That is two instructions per operator more than a register allocator would emit,
and it is chosen anyway: it is correct for an expression of any depth and has no
case analysis to get wrong. There is exactly **one peephole** — a push followed
immediately by a pop becomes a `mov`, or nothing — because every call with one
argument otherwise pushes and pops it to achieve nothing.

The stack and the program image share one arena and only `VM_FAULT_BOUNDS`
stands between them. A recursive program needs an arena sized for it; `hello`
gets 3 KB for a 1,738-byte image.

---

## 7. Buffers, and what a pointer is here

```
buf namebuf[16]                          // sixteen zero bytes
buf xfersrc = [0xDE, 0xAD, 0xBE, 0xEF]   // those four
```

Top level only, fixed size, laid out by the assembler. There is nowhere to put a
per-call buffer — a `buf` inside a function would be a static wearing a local's
clothes.

**A buffer's value is its arena offset**, and that is the whole of what a
pointer is in this language: an integer the VM bounds-checks on every use. It is
why one can be handed to a device without anything else having to be trusted.

| | |
|---|---|
| `b[i]` | the byte at `b + i`. **The index is a byte offset**; there is one element type and it is a byte, so a scale factor would be a constant `1` a reader has to verify |
| `b[i] = v` | stores a byte |
| `word(p)` | the four bytes at `p`. A misaligned `p` faults |
| `setword(p, v)` | stores four bytes |
| `puts(p)` | prints NUL-terminated bytes at `p` — how a name written by the kernel gets read back |

Buffers are 4-aligned even though indexing is by byte, because a buffer is what
gets handed to `word()` and to a device transfer, and an unaligned one would
fault on the first `ldw` with nothing in the source to suggest why.

Indexing is **not** bounds-checked by the compiler. `b[99]` on a 16-byte buffer
compiles, and reads whatever is 99 bytes along — until it leaves the arena,
where `VM_FAULT_BOUNDS` stops the program and nothing else. That is the same
guarantee every other memory access in this system has.

---

## 8. Devices

The `permissions` block is **both** the manifest the kernel resolves at load
time **and** the device namespace of the program. What a program may reach and
what it can name are one list, and using a device that is not declared is a
compile error rather than a runtime refusal.

```
permissions {
    light
    store
    echo
}

if light.read(0) {
    println("light = ", light.value)
}
```

| | |
|---|---|
| `d.read(chan)` | → 1 if the device answered. The reading is `d.value` |
| `d.write(chan, value)` | → 1 if it took it |
| `d.info()` | → 1; channels in `d.value`, flags in `d.flags` |
| `d.xfer_out(chan, buf, len)` | arena → device |
| `d.xfer_in(chan, buf, len)` | device → arena |
| `d.id` | the resolved device number |
| `device.count()` | → how many devices exist |
| `device.name(id, buf, len)` | → 1; writes the name into `buf` |

**Every device call evaluates to whether the device agreed**, and the
out-parameters are stashed where `d.value` and `d.flags` can read them. The
alternative — having `read` evaluate to the reading — makes a refusal
indistinguishable from a sensor reporting zero, which is the failure mode this
project has spent two reports diagnosing.

### 8.1 Names are resolved at run time, not compiled in

A compiler that turned `light` into `ldi r1, 0` would hard-code `device.c`'s
**table order** into every generated program. Step 356 removed exactly that
coupling from the kernel; putting it back one layer out, multiplied by every
program ever compiled, would be worse than where it started.

So `natc` emits a startup prologue that looks each declared name up through
`DEV_OP_FIND`, and stores the id. Inserting a device into `device.c` cannot
re-aim anything.

**A declared device this board does not have stops the program**, before its
first statement:

```
  [natc] no such device: baro
```

Same judgement the loader makes about an unknown permission name, for the same
reason: a program reaching for hardware that is not there runs blind.

---

## 9. The screen

Until step 362 NatScript could compute, print, and reach a device — which made
it a language for writing **serial console programs**, on a board whose entire
point is a 240×320 panel and a touchscreen. `fill` and `text` were reachable and
`touch` was not, so a program could draw something and then had no way to learn
whether anybody had touched it.

```
screen.fill(x, y, w, h, colour)
screen.text(string, x, y, fg, bg, scale)
screen.blit(pixels, x, y, w, h)
screen.touched()                    -> 1 or 0; where, in screen.x / screen.y
screen.width      screen.height
```

`screen` needs no permission. It is the program's **own viewport strip**, not
somebody else's hardware: coordinates are viewport-relative, the kernel clips
them, and a program cannot draw outside its strip or learn where that strip
sits on the panel.

### 9.1 `touched()` reports whether, separately from where

Same rule as `d.read()` in §8: a call that must be able to say **no** cannot
also be the coordinate. `screen.touched()` evaluates to 1 or 0 and stashes the
point in `screen.x` and `screen.y`.

### 9.2 `width` and `height` ask, every time

They are properties rather than something a `screen.size()` call leaves behind.
A `size()` that had to be called first would give **0** to anyone who forgot it,
silently — and the panel is not going to change size between two instructions.
This started as a `size()` and was deleted.

Both come from `sys dims`, which reports the **viewport**, not the panel.

### 9.3 The panel is polled; only keys are delivered

`touch` is a syscall a program asks. The kernel pushes a key (§10) and nothing
else. So a program that wants to feel touches polls, and the natural place to
poll is a tick handler:

```
every 100ms {
    if screen.touched() { ... }
}
```

Ten times a second is faster than a finger and leaves the rest of the quantum to
everything else.

### 9.4 Proved against a finger, 2026-09-07

```
[tap] cell 0 at 12,3 taps 1
[tap] cell 1 at 36,3 taps 2
[tap] cell 2 at 71,1 taps 3
[tap] cell 3 at 101,12 taps 7
[tap] cell 0 at 0,13 taps 8
```

`tools/app_tap.nat` on the board, touched by hand. Twenty touches delivered
(`touch g/w=20/40`), four different cells, the counter climbing, and the digits
drawn on the panel by `render()`.

Every coordinate is **viewport relative**: x from 0 to 157 and y from 0 to 13,
inside a strip that begins at panel y=256. The program never learns where its
strip is, and the forty withheld touches are the ones that landed outside it.

The cell arithmetic is the signed-division fix (§5) doing real work: `cw` is
`180 / 6`, and `screen.x / cw` maps 12, 36, 71 and 101 to cells 0, 1, 2 and 3.

**Opening the shell stops all of this**, on purpose. `app.c` sets every running
program's viewport to `(0, 0, 0, 0)` while the shell view is up, so no touch can
be inside one — a program must not paint over the keyboard, or over the control
that closes it. Closing the shell restores every strip.

---

## 10. `when` and `every`

The two pieces of syntax the proposal cared most about, and the last thing the
VM could do that the language could not say.

```
every 1s {
    ticks = ticks + 1
    println("tick ", ticks)
}

when key(k) {
    print("got ")
    putc(k)
    println("")
}
```

Top level only. A handler is entered by the **kernel**, so it has no caller to
be nested inside; one declared within a function would be registered or not
depending on whether that function happened to run.

### 10.1 Durations

A tick is **10 ms** (`kmain.c`, `TICK_INTERVAL_CYCLES`), and has been real time
rather than a yield counter since the `timer_isr` fix.

| | |
|---|---|
| `every 100ms` | 10 ticks |
| `every 2s` | 200 ticks |
| `every 10 ticks` | 10 ticks, said directly |
| `every 10` | the same; ticks are the default unit |

**`every 5ms` is a compile error.** It is not a whole number of ticks, and
rounding it to 10 ms silently would be a lie about the period the program asked
for. The error says what a tick is.

### 10.2 One handler per event

`vm.c` keeps a single handler offset per event id, so a second `every` block
would **replace** the first and the first would never fire. That is a compile
error naming the line of the block it would have displaced.

### 10.3 Handlers arm after the top level, and then the program waits

The top-level statements are setup. When they finish, the handlers are
registered, and the main flow becomes a one-instruction wait.

Arming first would let a tick fire while the top level was still assigning the
globals the handler reads — a handler seeing a half-initialised program, which
is a bug that appears once in a hundred runs.

There is no yield syscall, so the wait is a spin and the scheduler preempts it
by quantum. That is the same shape `app_evt.vasm` has run in since events
existed; it is stated here rather than left to be discovered in a disassembly.

**A program with a handler does not exit.** `exit(0)` still works if that is
what is wanted.

### 10.4 A refused registration stops the program

`sys event` refuses rather than faulting when the VM will not take a handler.
The generated code checks, and stops:

```
  [natc] the kernel refused a handler
```

A program written around something happening on its own, with the handler
silently not registered, spins forever looking busy. That failure is expensive
to diagnose and the check costs two instructions.

---

## 11. What v0 still does not have

1. **No block scoping**, and no shadowing.
2. **No `for`, no `break`, no `continue`.**
3. **Strings are literals only, and this is now the biggest gap.** `"Scans: " +
   count` needs an allocator inside an arena, and there is not one. A literal
   can be printed; a number can be printed; a number that has to appear **on the
   panel** rather than on the serial line has to be turned into digits by hand:

   ```
   func render(n) {
       label[0] = 116          // t
       ...
       label[i] = 48 + n % 10
   }
   ```

   Nine lines in `tools/app_tap.nat` are what `"taps: " + n` costs today. It did
   not matter while every program printed to a terminal; it matters the moment
   one draws.
4. **No constant folding.** `2 * 3` emits a multiply.
5. **No compile-time bounds checking** on buffer indices — §7.
6. **Two event sources**, tick and key, because that is what the VM has.
   `when button.pressed` from the proposal needs a device that can deliver.

---

## 12. The honest test, taken

The proposal set it:

> *rewrite `app_dev.vasm` in NatScript, and if it is not shorter and clearer
> than the assembly, the language has not earned itself.*

`tools/app_devnat.nat` is that rewrite, and `tools/app_evtnat.nat` is the same
exercise for `app_evt.vasm`. Each is registered next to the assembly original,
so `run dev` / `run devnat` and `run evt` / `run evtnat` compare on the board
rather than on the page.

| | assembly | NatScript | |
|---|---|---|---|
| `app_dev` lines | **117** | **37** | 3.2x shorter |
| `app_dev` bytecode | **583 B** | **1,381 B** | 2.4x larger |
| `app_evt` lines | **55** | **16** | 3.4x shorter |
| `app_evt` bytecode | **328 B** | **551 B** | 1.7x larger |

**Three times shorter in source, and larger in bytecode.** Both halves are the
point.

The source result is the language earning itself: the enumeration loop is four
lines instead of a register-allocated counter, a comparison, a branch and a
manual increment; two event handlers and their registration are eight lines
instead of thirty-one.

The bytecode result is the price of §6's evaluation model — every operand
pushed and popped — and it is reported here rather than left for someone to
find with `ls`. On a board with 4 MB of flash and per-program arenas measured in
kilobytes, source size is the scarce thing and bytecode is not. That is a
judgement about this machine, not a general one.

### 12.2 Run on the board, 2026-09-07

The comparison was made on the hardware rather than on the page. Both programs
were flashed and run from the shell over the serial link.

```
> run dev                                  > run devnat
   started id=2 perms=light store echo        started id=2 perms=light store echo
  [dev] 0 = light                            [devnat] 0 = light
  [dev] 1 = beep                             [devnat] 1 = beep
  ...                                        ...
  [dev] 6 = sd                               [devnat] 6 = sd
  [dev] bulk transfer round trip OK          [devnat] bulk transfer round trip OK
  [dev] light = 143                          [devnat] light = 156
  ... sixteen readings ...                   ... sixteen readings ...
  [dev] 16 readings taken, exiting.          [devnat] 16 readings taken, exiting.
```

Same device table, same round trip through the loopback, same sixteen readings.
One from 117 lines of assembly, the other from 37 lines of NatScript.

`perms=light store echo` on both is the manifest of §8 resolving by name, and
`devnat` never contained a device number: it asked for `light`, `store` and
`echo` by name and the kernel answered.

`run hello` reproduced the host results exactly — `fib(0..9)`, `gcd(1071, 462)
= 21`, `triangle(100) = 5050`, and the short-circuit `&&` that would divide by
zero if its right side ran.

`run evtnat` and `run evt` both had the kernel calling into them, a tick a
second, counting up while the main flow sat in its wait.

**The host reference (§12) predicted all of this correctly**, which is the first
evidence that it agrees with `vm.c` on anything that matters. It is still not
the kernel and still not authoritative.

### 12.1 What the rewrites could not carry over

The assembly's comments. `app_dev.vasm` carries a paragraph about a jump that
landed past its setup code and silently skipped a slot claim; `app_evt.vasm`
explains that its spin counter exists so a clobbered register would show up in
`ps`. Those are the most valuable part of those files, they are not a language
feature, and they were copied across by hand.

A language makes the code shorter. It does not make the reasoning shorter, and
the reasoning is what took the time.

---

## 13. The test suite

`tools/tests/*.nat`, run by `tools/nattest.py`, run by `build.ps1` **before it
compiles anything with the compiler**. A failure fails the build.

This is the first automated test suite in this project. `natc.py` had none:
every language change could silently break something that had worked, and the
only detector was a person reading generated assembly.

A case carries its expectation in a header comment:

```
// expect: the exact text the program should print
// arena: 2048          (optional)
// keys: hi             (optional, fed to `when key`)
// steps: 40000         (optional instruction limit)
```

or, for a program that must **not** compile:

```
// error: some words the compiler's message must contain
```

**Ten of the twenty-four cases are negative**, and that ratio is deliberate.
Most of what a compiler owes its user is refusing things clearly: an undeclared
device, a second `every`, `every 5ms`, a string used as a number, the wrong
number of arguments. A refusal that happens for the wrong reason is a bug that a
"does it compile" test cannot see.

Two checks run on **every** case regardless of what it asserts:

- **the stack pointer must come back** to the top of the arena. A frame leaking
  four bytes per call is invisible in output and fatal in a loop.
- **the program must not fault.** `BOUNDS`, `ALIGN`, `DIV0`, `CALL_DEPTH` and
  `RET` all fail the case by name.

### 13.1 The oracle is not the kernel

`tools/natvm_ref.py` is a NatVM on the host, and its header says what it is for
in its first line: **it is not authoritative — `kernel/vm.c` is.** Its semantics
were checked against `vm.c` instruction by instruction, and that check is the
only thing that makes it useful.

It catches **natc** regressions. It cannot catch a disagreement between itself
and the kernel, and **nothing in the kernel may be changed to make a test here
pass.** A second implementation of an instruction set is exactly the shape this
project keeps finding bugs in (§1), and this one is allowed to exist only
because it is a test oracle rather than a second source of truth.

### 13.2 What it found immediately

The first case, on arithmetic precedence, failed on a line that had nothing to
do with precedence:

```
wanted: -6
got:    4294967290
```

`println(0 - 6)`. `sys putd` prints unsigned, every comparison the compiler
emits is signed, and nobody had put those two facts next to each other. The
language had been telling its user something untrue about its own arithmetic
since the day it printed a number, and three shipped programs were compiled with
it. §5 has the fix.

That is the whole argument for the suite, made by the suite, on its first run.
