# NatVM ABI — frozen

**Used Medias LLC — Embedded Systems Division**
Revision 1.2 · 2026-09-07 · Covers `next_moves` VM-01 through VM-08.

This document is derived **from `kernel/vm.c` and `kernel/vm.h` as they are**,
not from the proposal. Where the older documents and the code disagree, the code
is right and this file follows it.

A compiler may depend on everything in §1–§5 and §7. §6 is a convention a compiler
must adopt and the VM does not enforce. §8 is what is still missing.

---

## 1. Machine model

| | |
|---|---|
| registers | **16**, `r0`–`r15`, 32-bit, **global** — no hardware frame |
| instruction | **4 bytes**, fixed |
| `pc` | **byte offset into the arena**, must be 4-aligned |
| memory | one **arena** per program, holding **code and data together** |
| return addresses | **kernel memory**, `vm->call[32]` — *not* in the arena |
| call depth | **32** (`VM_CALL_DEPTH`); exceeding it faults |
| arithmetic | integer only. **No floating point** — fixed-point by convention |

Two properties matter to a compiler more than the instruction set:

**Every load and store is bounds-checked against the arena, in software.** A
runaway pointer faults the program and nothing else. This is what makes a stack
convention safe to invent in the compiler (§6) — the VM will catch an overrun
even though it knows nothing about frames.

**Return addresses are not reachable by the program.** They live in kernel
memory, so no bytecode can forge or corrupt one. A compiler cannot implement
closures or coroutines by manipulating them, and does not need to defend them.

---

## 2. Instruction encoding

```
byte 0   opcode
byte 1   a     destination register, or the only operand
byte 2   b     source register, or immediate low byte
byte 3   c     source register / offset, or immediate high byte
```

**R-format** reads `a`, `b`, `c` as register indices.
**I-format** reads `a` as a register and `(b, c)` as a little-endian 16-bit
immediate.

A register index of 16 or more is `VM_FAULT_REG`, not a masked index: silently
truncating turns a producer bug into wrong answers rather than a diagnosed stop.

Branch and call immediates are **signed, in instructions**, relative to the
instruction *after* the branch.

---

## 3. Opcodes

| | | |
|---|---|---|
| `0x00` | `HALT` | stop, status in `r0` |
| `0x01` | `NOP` | |
| `0x02` | `MOV a, b` | |
| `0x03` | `LDI a, imm16` | zero-extends |
| `0x04` | `LDIH a, imm16` | sets the high half; `LDI`+`LDIH` builds a 32-bit constant |
| `0x10`–`0x1c` | `ADD SUB MUL DIV MOD AND OR XOR SHL SHR SAR NOT NEG` | `DIV`/`MOD` by zero → `VM_FAULT_DIV0` |
| `0x1d` | `ADDI a, imm` | **two operands**: `r[a] += imm`, signed. Not a three-operand add — the first draft of this document got that wrong and the assembler caught it |
| `0x20`–`0x25` | `SEQ SNE SLT SLTU SLE SLEU` | set `a` to 0 or 1 |
| `0x30` | `JMP simm` | |
| `0x31` `0x32` | `BRZ a, simm` `BRNZ a, simm` | |
| `0x33` | `CALL simm` | pushes the return address to **kernel** memory |
| `0x34` | `RET` | underflow → `VM_FAULT_RET` |
| `0x40` `0x41` | `LDW a, b, off` `LDB` | bounds-checked; `LDW` must be 4-aligned |
| `0x42` `0x43` | `STW a, b, off` `STB` | bounds-checked |
| `0x50` | `SYS n` | §4 |

Thirty-five in total. An unknown opcode is `VM_FAULT_OPCODE`.

---

## 4. Syscalls — `SYS n`

**Register-positional.** This is the part a compiler must encode exactly.

| | | |
|---|---|---|
| `0x00` | `EXIT` | `r0` = status |
| `0x01` | `PUTC` | `r0` = character |
| `0x02` | `PUTS` | `r0` = arena offset of a NUL-terminated string |
| `0x03` | `PUTD` | `r0` = value, printed unsigned decimal |
| `0x04` | `TICKS` | → `r0` = `timer_ticks()` |
| `0x05` | `FILL` | `r0`=x `r1`=y `r2`=w `r3`=h `r4`=colour |
| `0x06` | `TEXT` | `r0`=str offset `r1`=x `r2`=y `r3`=fg `r4`=bg `r5`=scale |
| `0x07` | `DIMS` | → `r0` = (width << 16) \| height |
| `0x08` | `TOUCH` | → `r0`=touched `r1`=x `r2`=y, **viewport-relative** |
| `0x09` | `BLIT` | `r0`=offset of RGB565 pixels `r1`=x `r2`=y `r3`=w `r4`=h |
| `0x0a` | `SEND` | `r0`=destination id `r1`=arena offset `r2`=length |
| `0x0b` | `RECV` | `r0`=arena offset `r1`=buffer size |
| `0x0c` | `DEVICE` | `r0`=operation (`DEV_OP_*`), args in `r1`–`r4` — the transfer pair uses `r4` for the length |
| `0x0d` | `EVENT` | `r0`=event id, `r1`=handler code offset (0 unregisters), `r2`=interval |

A syscall number outside this table is `VM_FAULT_SYSCALL`.

Coordinates passed to `FILL`, `TEXT` and `BLIT` are **viewport-relative** and
clipped by the kernel. A program cannot draw outside its strip; it does not need
to know where its strip is.

---

## 5. Events and faults

```
VM_EVT_TICK = 0     periodic, r2 = interval in ticks
VM_EVT_KEY  = 1     a key from the terminal queue
```

`sys event` registers a handler at a code offset. The kernel calls into it by
pushing a return address and setting `pc`; the handler **returns like any other
function**, and the kernel detects the exit by the return stack coming back to
the depth it had before the injection — no marker, no cooperation required.

This is the mechanism `when` and `every` compile to.

**Faults**, all of which stop the program and never the kernel:
`NONE OPCODE REG PC ALIGN BOUNDS DIV0 CALL_DEPTH RET STRING SYSCALL`.

---

## 6. The frame convention — VM-03

**The VM has no frames.** Sixteen registers, global, and `CALL` saves only a
return address. A compiler must therefore define where locals live, and this is
that definition.

The important discovery: **this needs no VM change.** The arena is
bounds-checked on every access, so a stack built inside it is as safe as any
other data — an overrun faults the program exactly like a bad pointer.

```
arena:  [ code ][ static data ] ........ free ........ [ stack ] arena_len
                                                        ^
                                         r15 (SP) grows DOWN
```

| register | role |
|---|---|
| `r0`–`r3` | arguments, and `r0` is the return value |
| `r4`–`r11` | **caller-saved** scratch |
| `r12`–`r13` | reserved |
| `r14` | **frame pointer — required, not optional.** See below |
| `r15` | **stack pointer** — byte offset into the arena, 4-aligned |

Prologue and epilogue are ordinary arithmetic:

```
    addi  r15, -N             ; N = frame size in bytes, multiple of 4
    ...                       ; locals at STW/LDW r15 + offset
    addi  r15, +N
    ret
```

**[step 357] `r14` was written here as "optional; a compiler with fixed frames
may skip it". That was wrong, and the first compiler to target this ABI found
out why.** Any code generator with an *expression stack* — pushing the left
operand of a binary operator while the right is evaluated — moves `r15` in the
middle of the statement that reads a local. A local addressed as `r15 + 8` is
somewhere else after the first `+` in the expression addressing it. So locals
are addressed through `r14`, the callee saves and restores it, and a compiler
may only skip it if it never pushes anything during an expression.

Consequences a compiler must respect:

- **Nesting is capped at 32.** `VM_CALL_DEPTH` is a kernel array; recursion
  deeper than that is `VM_FAULT_CALL_DEPTH`, not a stack overflow.
- **The stack and the heap share the arena.** A collision is a program bug the
  VM cannot see — only an overrun past `arena_len` faults. A compiler should
  emit a check, or size frames statically.
- **`r15` is initialised** to `arena_len & ~3` by `vm_init()` (step 355), and
  **event handlers save and restore all sixteen registers**, so the stack
  pointer survives a tick or a key arriving mid-function.
- Proved, not asserted: `tools/app_frame.vasm` calls a function that allocates
  locals, which calls another that allocates its own and writes over its whole
  frame, and checks on the way out that the outer locals and the stack pointer
  are intact. `run frame` prints
  `frame: PASS locals survived a nested call`.

---

## 7. The device manifest — VM-07

A program **declares** the devices it needs; the kernel resolves the declaration
against its own table at load time. The kernel no longer decides what a program
wants, which is what made a `permissions { }` block compilable.

In assembly, one directive per device, anywhere in the file:

```
    .permission light
    .permission store
    .permission echo
```

It assembles to **no bytes**. It is a manifest, not code, and it appears in the
generated header instead:

```c
#define VM_APP_DEV_PERM_COUNT 3u
static const char *const vm_app_dev_perms[] = { "light", "store", "echo" };
```

The loader resolves it with `device_perms_from_names()` **before** starting
anything, and:

- a name the device table knows becomes its bit, by lookup — never by position
- **a name it does not know refuses the launch.** Not a dropped permission: a
  program reaching for a sensor that is silently not there runs blind, and
  refusing is the cheaper failure to diagnose

The names are the ones in `device.c`: `light beep store i2c keys echo sd`.

**[step 358] `DEV_OP_FIND` (7) is the name-to-id lookup a compiler needs**: r1 is
the arena offset of a NUL-terminated name, and it answers r0 = found, r1 = id.
A compiler that emitted a literal id would hard-code `device.c`'s table order
into every program it ever produced -- the coupling this manifest exists to
remove -- so NatScript resolves its declared names through this at startup.
It discloses nothing new: `DEV_OP_COUNT` and `DEV_OP_NAME` already let any
program walk the whole table.

A compiler emits this list from its `permissions` block. There is nothing else
to encode — no ordering, no bit assignment, no kernel-side table to keep in step.

---

## 8. What is still missing

Everything above can be depended on now. These cannot:

1. **Image identity** (VM-08). `device.h` says it plainly: *"A permission grant
   is only meaningful if the image it applies to cannot be substituted."*
   A manifest is what a program **asks** for, and asking is not proof. Without
   signing, permissions are containment rather than security, and calling them
   security would be the seventh thing in this project to claim an outcome it
   had not earned.
2. **A string type.** `PUTS` takes an arena offset to NUL-terminated bytes;
   `"Scans: " + count` in the proposal implies allocation, and there is no
   allocator inside an arena.

---

## 9. What this means for NatScript

| | status |
|---|---|
| VM-01 opcode ABI | **done — §2, §3** |
| VM-02 syscall ABI | **done — §4** |
| VM-03 frame layout | **done — §6**, `r15` initialised at step 355, proved by `tools/app_frame.vasm` |
| VM-04 nested call/ret | **already works**, 32 deep, faults on underflow |
| VM-05 event ABI | **done — §5** |
| VM-06 tick/key delivery | implemented; `app_evt.vasm` exercises both |
| VM-07 manifest | **done — §7**, step 356 |
| VM-08 permission enforcement | enforced from the manifest; **image identity still missing** |
| VM-09..11 grammar, parser, codegen | **done — `tools/natc.py`, `docs/natscript.md`**, step 357 |
| VM-12 buffers and device syntax | **done — `natscript.md` §7, §8**, step 358 |
| VM-13 the honest test | **taken — `natscript.md` §10**: 117 lines of assembly became 37 |

So `when` and `every` have a mechanism, `device` is a compile-time lookup
against a table that already exists, and `permissions` has a format to compile
into. **Every VM-side prerequisite the proposal listed is now met.** What is
left is the language: grammar, lexer, parser, and AST to bytecode.

**The honest test remains the one the proposal set**: rewrite `app_dev.vasm` in
NatScript, and if it is not shorter and clearer than the assembly, the language
has not earned itself.
