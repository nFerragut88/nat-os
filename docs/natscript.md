# NatScript — the language, v0

**Used Medias LLC — Embedded Systems Division**
Revision 0.1 · 2026-09-07 · Covers `next_moves` VM-09, VM-10, VM-11.

NatScript compiles to NatVM bytecode. This document describes **what the
compiler in `tools/natc.py` actually accepts today**, not what the proposal
imagines. Where the two differ, §7 says so by name.

The proposal's own §15 warned against designing syntax before the ABI was
written down. The ABI is now written down and frozen — `docs/vm-abi.md`,
revision 1.1 — so this is the step after that warning, not around it.

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

There are no arrays, no buffers, no pointers, and no structs. §7.

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

Zero is false and everything else is true; there is no separate boolean type.

Redeclaring a name in the same function is an error. There is **no block
scoping**: a `let` inside an `if` is visible for the rest of the function. That
is a simplification, not a design, and §7 lists it.

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

`&&` and `||` **short-circuit**, and yield 0 or 1 rather than the value of
whichever side decided it — so `x = a && b` stores a truth value.

`>` and `>=` compile to `<` and `<=` with the operands reversed. The VM has no
greater-than opcode and does not need one.

Comparisons are **signed** (`slt`, `sle`). Shifts: `>>` is logical (`shr`).

### Built-in calls

| | |
|---|---|
| `print(...)` | each argument in turn — a string literal as text, anything else as a number |
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

## 7. What v0 does not have

Named rather than discovered:

1. **No arrays, buffers, or pointers.** This is the big one. `device` operations
   that write into the arena — `DEV_OP_NAME`, the transfer pair — take an arena
   offset, and NatScript has no way to name one. **So `app_dev.vasm` is not yet
   rewritable in NatScript**, and the proposal's honest test is not yet
   available. That is the next piece of work, not a footnote.
2. **No `device`, `when`, or `every` syntax.** The VM has the mechanisms —
   `sys device`, `sys event`, and a manifest — and the language does not reach
   them yet. `when` and `every` are compilable the moment there is something for
   a handler to do.
3. **No block scoping**, and no shadowing.
4. **No `for`, no `break`, no `continue`, no `else` on a `while`.**
5. **Strings are literals only.** `"Scans: " + count` from the proposal needs an
   allocator inside an arena, which does not exist.
6. **No constant folding.** `2 * 3` emits a multiply.

---

## 8. Evidence

`tools/app_hello.nat` is the first nat-os program nobody wrote in assembly:
recursion (`fib`), a `while` loop with a remainder (`gcd`), nested frames,
`else if` chains, and a short-circuit `&&` whose right side would divide by zero
if it were evaluated.

Checked in a host simulator of the ABI before it was flashed — 14,518
instructions, every value correct, and `r15` back at the top of the arena where
it started, which is the frame convention balancing across nine levels of
recursion. **A simulator is not the kernel**; `run hello` on the board is the
reading that counts.

---

## 9. Where this sits

| | |
|---|---|
| VM-09 grammar | **this document** |
| VM-10 lexer, parser | `tools/natc.py`, §2–§5 |
| VM-11 AST to bytecode | `tools/natc.py`, §6 |
| VM-12 arrays, device syntax | **not started** — §7.1, §7.2 |
| VM-13 the honest test | **blocked on VM-12** |

The proposal set the test and it still stands: **rewrite `app_dev.vasm` in
NatScript, and if it is not shorter and clearer than the assembly, the language
has not earned itself.** v0 cannot attempt it. That is stated here rather than
left for a reader to discover.
