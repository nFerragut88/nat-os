# Appendix B — Bytecode ISA Reference

Derived from `kernel/vm.h` (authoritative) and `tools/vasm.py` (the producer).

---

## B.1 Machine model

| Property | Value |
|---|---|
| Registers | 16, `r0`–`r15`, 32-bit, no special roles |
| Instruction width | 4 bytes, fixed |
| PC | Byte offset into the arena; must be 4-byte aligned |
| Call stack | 32 entries, **in kernel memory**, unaddressable by the program |
| Address space | The arena only. Offset 0 is the start of the loaded image |
| Condition codes | None. Comparisons write 0 or 1 to a register |
| Endianness of immediates | Little — byte 2 is the low half |

## B.2 Encoding

```
byte 0   opcode
byte 1   a      destination register, or the sole operand
byte 2   b      source register, or immediate low byte
byte 3   c      source register / offset, or immediate high byte
```

**Formats**, as `vasm.py` names them:

| Format | Operands | `a` | `b` | `c` |
|---|---|---|---|---|
| `none` | — | 0 | 0 | 0 |
| `ab` | `rA, rB` | reg | reg | 0 |
| `abc` | `rA, rB, rC` | reg | reg | reg |
| `ai` | `rA, imm16` | reg | imm low | imm high |
| `i` | `label` | 0 | disp low | disp high |
| `air` | `rA, label` | reg | disp low | disp high |
| `abo` | `rA, rB, off` | reg | reg | 0–255 |
| `sys` | `name` \| `n` | 0 | num low | num high |

**Register validation.** Every operand that is an index is checked against 16 and
faults with `VM_FAULT_REG` otherwise — including operands an opcode does not use,
because "operands that an opcode does not use are still indices in a well-formed
program". `HALT`, `NOP` and `RET` are exempt entirely; immediate and offset forms
exempt `b` and `c` respectively.

**Branch displacements count INSTRUCTIONS**, signed 16-bit, relative to the
instruction *after* the branch:

```
target_pc = next_pc + simm * 4
```

The assembler refuses a misaligned target and a displacement outside
−32768…32767.

## B.3 Opcodes

### Control — `0x00`–`0x04`

| Op | Mnemonic | Format | Semantics |
|---|---|---|---|
| `0x00` | `halt` | none | Stop; `vm_run` returns `VM_RUN_HALTED` |
| `0x01` | `nop` | none | — |
| `0x02` | `mov rA, rB` | ab | `a ← b` |
| `0x03` | `ldi rA, imm` | ai | `a ← imm16` (zero-extended) |
| `0x04` | `ldih rA, imm` | ai | `a ← (a & 0xFFFF) \| (imm16 << 16)` |

`LDI` then `LDIH` is how a 32-bit constant is built. `app_blit.vasm` uses the
pair to make `0x001FFFE0` — two RGB565 pixels in one word.

### Arithmetic and logic — `0x10`–`0x1D`

| Op | Mnemonic | Format | Semantics |
|---|---|---|---|
| `0x10` | `add rA, rB, rC` | abc | `a ← b + c` |
| `0x11` | `sub rA, rB, rC` | abc | `a ← b − c` |
| `0x12` | `mul rA, rB, rC` | abc | `a ← b × c` |
| `0x13` | `div rA, rB, rC` | abc | `a ← b / c`; **`c == 0` faults** |
| `0x14` | `mod rA, rB, rC` | abc | `a ← b % c`; **`c == 0` faults** |
| `0x15` | `and rA, rB, rC` | abc | |
| `0x16` | `or rA, rB, rC` | abc | |
| `0x17` | `xor rA, rB, rC` | abc | |
| `0x18` | `shl rA, rB, rC` | abc | `a ← b << (c & 31)` |
| `0x19` | `shr rA, rB, rC` | abc | logical, `a ← b >> (c & 31)` |
| `0x1A` | `sar rA, rB, rC` | abc | arithmetic, `a ← (int32)b >> (c & 31)` |
| `0x1B` | `not rA, rB` | ab | `a ← ~b` |
| `0x1C` | `neg rA, rB` | ab | `a ← −b` |
| `0x1D` | `addi rA, imm` | ai | `a ← a + sign_extend(imm16)` |

Shift counts are **masked to 5 bits**, not faulted:

> A shift of 32 or more is undefined in C and would let a program's behaviour
> depend on the host compiler, which is precisely what a VM exists to prevent.

Arithmetic wraps; there is no overflow trap.

### Comparison — `0x20`–`0x25`

All write 0 or 1 into `rA`.

| Op | Mnemonic | Semantics |
|---|---|---|
| `0x20` | `seq rA, rB, rC` | `b == c` |
| `0x21` | `sne rA, rB, rC` | `b != c` |
| `0x22` | `slt rA, rB, rC` | `(int32)b < (int32)c` |
| `0x23` | `sltu rA, rB, rC` | `b < c` unsigned |
| `0x24` | `sle rA, rB, rC` | `(int32)b <= (int32)c` |
| `0x25` | `sleu rA, rB, rC` | `b <= c` unsigned |

> Comparisons produce 0 or 1 into a register rather than setting flags, so
> branches need only test a register for zero. That removes a whole class of
> state — condition codes with their own save/restore obligations across a
> context switch.

### Control flow — `0x30`–`0x34`

| Op | Mnemonic | Format | Semantics |
|---|---|---|---|
| `0x30` | `jmp label` | i | unconditional |
| `0x31` | `brz rA, label` | air | branch if `a == 0` |
| `0x32` | `brnz rA, label` | air | branch if `a != 0` |
| `0x33` | `call label` | i | push `next_pc`; branch. **Depth 32; overflow faults** |
| `0x34` | `ret` | none | pop; **empty stack faults** |

### Memory — `0x40`–`0x43`

Address is `rB + c`, where `c` is an unsigned byte offset 0–255. **Every access
is bounds-checked against the arena.**

| Op | Mnemonic | Semantics | Extra check |
|---|---|---|---|
| `0x40` | `ldw rA, rB, off` | `a ← u32 at [b+off]` | 4-byte aligned |
| `0x41` | `ldb rA, rB, off` | `a ← u8 at [b+off]` | — |
| `0x42` | `stw rA, rB, off` | `u32 at [b+off] ← a` | 4-byte aligned |
| `0x43` | `stb rA, rB, off` | `u8 at [b+off] ← a` | — |

### Services — `0x50`

| Op | Mnemonic | Format |
|---|---|---|
| `0x50` | `sys name` | sys |

## B.4 Syscalls

Arguments in `r0`–`r5`; results in `r0`–`r2`.

| # | Name | Arguments | Result | Ends slice? |
|---|---|---|---|---|
| 0 | `exit` | `r0` = status | — (halts) | — |
| 1 | `putc` | `r0` = character | — | no |
| 2 | `puts` | `r0` = arena offset of a NUL-terminated string | — | no |
| 3 | `putd` | `r0` = value | — | no |
| 4 | `ticks` | — | `r0` ← `timer_ticks()` | no |
| 5 | `fill` | `r0`=x `r1`=y `r2`=w `r3`=h `r4`=colour | — | **yes** |
| 6 | `text` | `r0`=str `r1`=x `r2`=y `r3`=fg `r4`=bg `r5`=scale | — | **yes** |
| 7 | `dims` | — | `r0` ← `(w << 16) \| h` | no |
| 8 | `touch` | — | `r0`←touched `r1`←x `r2`←y | no |
| 9 | `blit` | `r0`=offset `r1`=x `r2`=y `r3`=w `r4`=h | `r0` ← 1 drawn, 0 not | **yes** |
| 10 | `send` | `r0`=dest id `r1`=offset `r2`=len | `r0` ← 1 delivered, 0 refused | no |
| 11 | `recv` | `r0`=offset `r1`=max | `r0` ← bytes, `r1` ← sender | no |

**The slice rule:** a syscall whose cost is measured in milliseconds ends the
caller's slice regardless of remaining quantum; one that reads a published
snapshot does not.

**Confinement:**
- Display coordinates are viewport-relative; a program is never told where its
  viewport sits.
- A touch outside the viewport is reported as **no touch at all**.
- `send` names a destination **application**, never an address.
- `blit` bounds `w` and `h` against the panel *before* multiplying, so
  `w × h × 2` provably cannot wrap.
- `text` and `puts` walk the string one bounds-checked byte at a time; an
  unterminated string faults rather than reading past the arena.

**Limits:** message body ≤ 64 B (`IPC_MSG_MAX`); string buffer 48 B
(`VM_STR_MAX`), truncated not faulted; text scale clamped to 1–4.

## B.5 Faults

| Code | Name | Raised by |
|---|---|---|
| 0 | `NONE` | — |
| 1 | `OPCODE` | Unknown opcode byte |
| 2 | `REG` | Register index ≥ 16 |
| 3 | `BOUNDS` | Access outside the arena |
| 4 | `ALIGN` | Misaligned word access, or odd `blit` offset |
| 5 | `DIV0` | `div`/`mod` by zero |
| 6 | `PC` | PC outside the arena, or misaligned |
| 7 | `CALL_DEPTH` | 33rd nested `call` |
| 8 | `RET` | `ret` with an empty call stack |
| 9 | `SYSCALL` | Unknown service number |
| 10 | `STRING` | `puts` string unterminated inside the arena |

Six of the ten have been exercised on hardware. `PC`, `CALL_DEPTH`, `SYSCALL` and
`STRING` are implemented and untested.

Every fault records the code, the PC of the faulting instruction, and a detail
value (the offending offset, opcode, register index or syscall number).

## B.6 `vm_run()` return values

| Value | Meaning |
|---|---|
| `VM_RUN_HALTED` | `HALT` or `SYS EXIT`. The program finished |
| `VM_RUN_FAULTED` | See `vm_fault()`. Terminal |
| `VM_RUN_QUANTUM` | Budget spent, or an expensive syscall ended the slice. **Call again** |

State lives entirely in `vm_t`, so a resumption continues at the exact
instruction boundary where it stopped.

## B.7 Assembler directives

| Directive | Effect |
|---|---|
| `label:` | Records the current byte offset |
| `.string "…"` | Bytes plus a NUL. Escapes: `\n \r \t \0 \\ \" \'` |
| `.byte a, b, …` | Raw bytes |
| `.word v, …` | 32-bit little-endian; pads to 4-byte alignment first |
| `.align n` | Pad to a multiple of n |
| `.space n` | n zero bytes |
| `; …` or `# …` | Comment (string-aware) |

**Values** accept decimal, `0x` hex, `'c'` character literals, a bare label name,
and `@label` for a label's byte offset.

Instructions are padded to 4-byte alignment automatically, because "the VM faults
otherwise, so pad rather than emit something that cannot execute".

## B.8 A worked decode

`tools/spin.vasm`, assembled to 28 bytes:

| Offset | Bytes | Decode |
|---|---|---|
| 0 | `03 01 18 00` | `ldi r1, 0x0018` — `@counter` |
| 4 | `03 02 00 00` | `ldi r2, 0` |
| 8 | `03 09 01 00` | `ldi r9, 1` |
| 12 | `10 02 02 09` | `add r2, r2, r9` |
| 16 | `42 02 01 00` | `stw r2, r1, 0` |
| 20 | `30 00 fd ff` | `jmp −3` → offset 12 |
| 24 | `00 00 00 00` | `counter:` `.word 0` |

Displacement check: the `jmp` is at 20, so `next` is 24; 24 + (−3 × 4) = 12,
which is `loop:`.

## B.9 Producing a program

```bash
python tools/vasm.py tools/myprog.vasm -o kernel/generated/myprog.h --name vm_myprog
```

Emits `vm_myprog[]`, `VM_MYPROG_LEN`, and `VM_MYPROG_AT_<LABEL>` for every label.
`build.ps1` does this automatically for every `tools/*.vasm`.

To make it launchable, add a row to `PROGRAMS[]` in `kmain.c`:

```c
{ "myprog", vm_myprog, VM_MYPROG_LEN, 512u, VM_MYPROG_AT_COUNTER },
```

— name, image, length, arena bytes, and the offset of the progress word the
kernel reads.
