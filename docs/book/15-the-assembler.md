# Chapter 15 — The Producer: `vasm.py` and the Programs

> Sources: `docs/UM-NATOS-012-m4-verification.md` §4
> Code: `tools/vasm.py`, `tools/*.vasm`, `kernel/generated/*.h`, `build.ps1`

---

## 15.1 Why the producer is on the host

An instruction set with no producer is a specification, not a tool. `vasm.py`
assembles a text source into a C header containing a byte array and label
offsets.

```python
"""vasm — assembler for the nat-os bytecode VM.

Runs on the host, not the device. A producer has to exist for the ISA to be
usable (UM-NATOS-007 §6), but nothing about it needs to live in 176 KB of DRAM,
and keeping it here means the on-device side stays a pure interpreter.

    python tools/vasm.py prog.vasm -o kernel/generated/prog.h --name vm_prog
"""
```

Three consequences of that placement, one of which is a security property:

- **No memory constraint.** 381 lines of Python with a two-pass assembler,
  string escapes, and label resolution costs the device nothing.
- **No consequence for being slow.** It runs once per build.
- **No attack surface from untrusted text.** The device never parses anything.
  There is no symbol table, no tokeniser, and no error path on the device at all.

## 15.2 Syntax

```
; comment                    (also #)
label:                       byte offset within the arena
    ldi   r1, 10
    add   r2, r1, r3
    ldw   r1, r2, 4          ; r1 <- u32 at [r2 + 4]
    brz   r4, done           ; label operands are instruction-relative
    sys   puts
    halt
    .string "text\n"         ; NUL-terminated
    .byte  1, 2, 3
    .word  0xdeadbeef        ; 4-byte aligned automatically
    .align 4
    .space 64                ; n zero bytes
```

Immediates accept decimal, `0x` hex, `'c'` character literals, and **`@label`**
for a label's byte offset — which is what makes string arguments to `SYS PUTS`
expressible:

```
        ldi     r0, @msg
        sys     puts
```

## 15.3 The opcode and syscall tables

```python
# mnemonic -> (opcode, format). Formats:
#   none  no operands                    ab    a, b
#   abc   a, b, c                        ai    a, imm16
#   i     imm16 (relative label ok)      air   a, relative label
#   abo   a, b, byte offset              sys   syscall name or number
OPS = {
    "halt": (0x00, "none"), "nop":  (0x01, "none"),
    "mov":  (0x02, "ab"),   "ldi":  (0x03, "ai"),  "ldih": (0x04, "ai"),

    "add":  (0x10, "abc"),  "sub":  (0x11, "abc"), "mul":  (0x12, "abc"),
    "div":  (0x13, "abc"),  "mod":  (0x14, "abc"), "and":  (0x15, "abc"),
    "or":   (0x16, "abc"),  "xor":  (0x17, "abc"), "shl":  (0x18, "abc"),
    "shr":  (0x19, "abc"),  "sar":  (0x1A, "abc"), "not":  (0x1B, "ab"),
    "neg":  (0x1C, "ab"),   "addi": (0x1D, "ai"),

    "seq":  (0x20, "abc"),  "sne":  (0x21, "abc"), "slt":  (0x22, "abc"),
    "sltu": (0x23, "abc"),  "sle":  (0x24, "abc"), "sleu": (0x25, "abc"),

    "jmp":  (0x30, "i"),    "brz":  (0x31, "air"), "brnz": (0x32, "air"),
    "call": (0x33, "i"),    "ret":  (0x34, "none"),

    "ldw":  (0x40, "abo"),  "ldb":  (0x41, "abo"),
    "stw":  (0x42, "abo"),  "stb":  (0x43, "abo"),

    "sys":  (0x50, "sys"),
}
```

The syscall table carries a warning about drift, and a note on where the error
lands:

```python
# Must stay in step with the VM_SYS_* enum in kernel/vm.h. A name missing here
# is caught at assembly time ("cannot parse value"), which is the right place —
# the alternative is a numeric syscall that assembles cleanly and faults on the
# device with VM_FAULT_SYSCALL.
SYSCALLS = {
    "exit": 0, "putc": 1, "puts": 2, "putd": 3, "ticks": 4,
    "fill": 5, "text": 6, "dims": 7, "touch": 8, "blit": 9, "send": 10, "recv": 11,
}
```

This is the one place in the system where a definition is genuinely duplicated
across the host/device boundary and *not* cross-checked at runtime. The
mitigation is that the failure lands at assembly time with a comprehensible
message rather than on the device with a fault code.

## 15.4 Two passes

**Pass 1 — lay out, recording label offsets.** Every item is recorded with its
offset, kind, payload and source line:

```python
    def _layout(self, line, lineno):
        parts = line.split(None, 1)
        head = parts[0].lower()
        rest = parts[1] if len(parts) > 1 else ""

        if head == ".string":
            data = parse_string(rest.strip()) + b"\x00"
            self.items.append((self.offset, "raw", data, lineno))
            self.offset += len(data)
        elif head == ".byte":
            /* ... */
        elif head == ".word":
            self._pad_to(4)
            /* ... */
        elif head in OPS:
            # Instructions must be 4-byte aligned; the VM faults otherwise, so
            # pad rather than emit something that cannot execute.
            self._pad_to(INSN_BYTES)
            self.items.append((self.offset, "insn", (head, split_operands(rest)), lineno))
            self.offset += INSN_BYTES
        else:
            raise AsmError(f"unknown mnemonic or directive '{head}'")
```

The `_pad_to(INSN_BYTES)` before every instruction is the producer half of the
`VM_FAULT_ALIGN` contract: the assembler makes misalignment *unrepresentable*
rather than leaving it for the interpreter to diagnose.

**Pass 2 — encode, resolving labels.** Errors carry the source line:

```python
            except AsmError as e:
                where = f"line {lineno}: " if lineno else ""
                raise AsmError(f"{where}{e}") from None
```

### Branch displacements

```python
    def rel(self, tok, here):
        """Branch displacement, counted in instructions from the NEXT one."""
        target = self.value(tok)
        if target % INSN_BYTES:
            raise AsmError(f"branch target '{tok}' is not instruction-aligned")
        delta = (target - (here + INSN_BYTES)) // INSN_BYTES
        if not -32768 <= delta <= 32767:
            raise AsmError(f"branch to '{tok}' out of range ({delta})")
        return delta & 0xFFFF
```

Two checks the interpreter would otherwise have to make, or a program would
otherwise get wrong: alignment and range.

### Operand encoding

```python
    def insn(self, mnem, ops, here):
        opcode, fmt = OPS[mnem]
        a = b = c = 0

        def need(n):
            if len(ops) != n:
                raise AsmError(f"{mnem} takes {n} operand(s), got {len(ops)}")

        if fmt == "none":
            need(0)
        elif fmt == "ab":
            need(2); a, b = reg(ops[0]), reg(ops[1])
        elif fmt == "abc":
            need(3); a, b, c = reg(ops[0]), reg(ops[1]), reg(ops[2])
        elif fmt == "ai":
            need(2)
            a = reg(ops[0])
            v = self.value(ops[1])
            if not -32768 <= v <= 65535:
                raise AsmError(f"immediate out of range: {v}")
            b, c = v & 0xFF, (v >> 8) & 0xFF
        /* ... */
        elif fmt == "abo":
            need(3)
            a, b = reg(ops[0]), reg(ops[1])
            off = self.value(ops[2])
            if not 0 <= off <= 255:
                raise AsmError(f"offset out of range 0..255: {off}")
            c = off
        elif fmt == "sys":
            need(1)
            key = ops[0].strip().lower()
            v = SYSCALLS[key] if key in SYSCALLS else self.value(ops[0])
            b, c = v & 0xFF, (v >> 8) & 0xFF

        return bytes((opcode, a, b, c))
```

Every operand class is range-checked. The register parser refuses 16 and above
(Chapter 14 §14.8 notes why the containment tests had to bypass it entirely).

### String handling done properly

Comment stripping and operand splitting are both string-aware, which matters
because `.string "a;b"` and `.string "a,b"` are legal:

```python
def split_operands(text):
    """Split on commas that are not inside a quoted string."""
    parts, cur, in_str, esc = [], "", False, False
    for ch in text:
        if in_str:
            cur += ch
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            cur += ch
        elif ch == ",":
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts
```

## 15.5 The output

A C header, with the bytes and every label as a `#define`:

```c
/* Generated by tools/vasm.py — do not edit.
 * Source: tools/spin.vasm
 */
#ifndef NATOS_GENERATED_VM_SPIN_H
#define NATOS_GENERATED_VM_SPIN_H

#include <stdint.h>

static const uint8_t vm_spin[] = {
    0x03, 0x01, 0x18, 0x00, 0x03, 0x02, 0x00, 0x00, 0x03, 0x09, 0x01, 0x00,
    0x10, 0x02, 0x02, 0x09, 0x42, 0x02, 0x01, 0x00, 0x30, 0x00, 0xfd, 0xff,
    0x00, 0x00, 0x00, 0x00,
};

#define VM_SPIN_LEN 28u

#define VM_SPIN_AT_START 0u
#define VM_SPIN_AT_LOOP 12u
#define VM_SPIN_AT_COUNTER 24u

#endif
```

Twenty-eight bytes. Seven instructions and a word.

The exported label offsets are what let the kernel read a program's published
progress without any agreement beyond the source file:

```c
static uint32_t vm_counter(void)
{
    if (g_vm_arena < 0) {
        return 0;
    }
    return *(volatile uint32_t *)(g_vm_base + VM_SPIN_AT_COUNTER);
}
```

`VM_SPIN_AT_COUNTER` is generated from `counter:` in the `.vasm` file. Move the
label and the kernel follows automatically.

### Decoding that array by hand

Worth doing once, because it demonstrates that the encoding really is as simple
as claimed:

| Bytes | Decode | Meaning |
|---|---|---|
| `03 01 18 00` | `LDI r1, 0x0018` | `r1 = 24` — `@counter` |
| `03 02 00 00` | `LDI r2, 0` | iteration count |
| `03 09 01 00` | `LDI r9, 1` | the constant to add |
| `10 02 02 09` | `ADD r2, r2, r9` | `r2 += r9` |
| `42 02 01 00` | `STW r2, r1, 0` | store `r2` at `[r1 + 0]` — bounds-checked |
| `30 00 fd ff` | `JMP -3` | back three instructions to `loop` |
| `00 00 00 00` | — | the `counter` word |

`fd ff` is −3 as a little-endian 16-bit value. `loop` is at offset 12; the `JMP`
is at offset 20, so `next` is 24, and 24 + (−3 × 4) = 12. Correct.

## 15.6 The programs

Twelve `.vasm` sources currently assemble. They fall into four groups.

### Progress demonstrators

**`app_a.vasm`** — three instructions per iteration:

```
; Application A — counts. Three instructions per iteration.
start:
        ldi     r1, @counter
        ldi     r2, 0
        ldi     r9, 1
loop:
        add     r2, r2, r9
        stw     r2, r1, 0           ; publish progress where the kernel can see it
        jmp     loop

        .align  4
counter:
        .word   0
```

**`app_b.vasm`** — four instructions per iteration, and the comment explains why
that difference is the point:

```
; Application B — publishes squares. Four instructions per iteration, so it
; advances at a visibly different rate from A under the same quantum. Two
; applications sharing a core should not advance in lockstep, and the differing
; rates make that observable rather than assumed.
start:
        ldi     r1, @square
        ldi     r2, 0
        ldi     r9, 1
loop:
        add     r2, r2, r9
        mul     r4, r2, r2
        stw     r4, r1, 0
        jmp     loop
```

That design is what makes the M5 result checkable to the digit (Chapter 16 §16.5):
A publishes an iteration count, B publishes its square, and both must agree with
their instruction totals.

### The demonstration program

`demo.vasm` exercises every mechanism M4 claims, and says so:

```
; nat-os — M4 demonstration program.
;
; Exercises every mechanism the milestone claims: arithmetic, a counted loop,
; a bounds-checked store and reload, a call/return through the kernel-side
; return stack, and syscalls that produce UART output.
;
; Computes sum(1..10) = 55, round-trips it through arena memory so the load and
; store paths are actually used rather than assumed, and prints it.

start:
        ldi     r1, 0               ; sum
        ldi     r2, 1               ; i
        ldi     r3, 11              ; limit (exclusive)
        ldi     r9, 1               ; a constant 1 to add with

loop:
        slt     r4, r2, r3          ; r4 = i < limit
        brz     r4, done
        add     r1, r1, r2          ; sum += i
        add     r2, r2, r9          ; i++
        jmp     loop

done:
        ; Round-trip through memory. @total is a byte offset into the arena, so
        ; both of these go through the bounds check like any other access.
        ldi     r5, @total
        stw     r1, r5, 0
        ldi     r6, 0
        ldw     r6, r5, 0

        ldi     r0, @msg
        sys     puts
        mov     r0, r6
        call    print_num           ; return address lives in kernel memory
        ldi     r0, 10              ; '\n'
        sys     putc

        ldi     r0, 0
        sys     exit

print_num:
        sys     putd
        ret

        .align  4
total:
        .word   0
msg:
        .string "  vm says sum(1..10) = "
```

The round-trip through memory is not decoration: without it, `LDW` and `STW`
would be untested by the demonstration and the bounds path would be exercised
only by the deliberately malformed probes.

### The escape attempts

**`app_rogue.vasm`** — the memory escape. Its comment is the clearest statement
of the isolation model anywhere in the tree:

```
; Application ROGUE — deliberately written to escape its arena.
;
; UM-NATOS-007 §7 names this as the milestone's principal risk: "an application
; deliberately written to escape its arena must fail to do so."
;
; It walks a store upward through memory four bytes at a time, starting above
; its own code so it does not destroy the loop doing the walking. Every store
; inside the arena succeeds; the first store past the end must fault, and the
; reported offset should equal the arena size exactly — which makes this a test
; of WHERE the boundary is, not merely that one exists.
;
; Note what it cannot do. A VM address is an offset into its own arena, so there
; is no value it could load into r5 that names another application's memory. The
; escape is not refused, it is unrepresentable.
start:
        ldi     r1, @counter
        ldi     r2, 0
        ldi     r9, 1
        ldi     r5, 128             ; start probing well above this program
        ldi     r6, 4

probe:
        add     r2, r2, r9
        stw     r2, r1, 0           ; keep publishing so progress is visible
        stw     r2, r5, 0           ; the probe — succeeds until it does not
        add     r5, r5, r6
        jmp     probe
```

Note the second store: it keeps publishing progress *while* probing, so the
kernel can see the program was alive and working right up to the fault.

**`app_gfx_rogue.vasm`** — the same question asked of the screen:

```
; Application GFX-ROGUE — deliberately tries to paint the entire panel.
;
; UM-NATOS-013 §5.2 established that an application cannot reach another's
; MEMORY. This is the same question asked of the SCREEN: given a syscall that
; draws, can a program scribble over the kernel's status area, the colour strip,
; or a neighbour's output?
;
; It asks for a 240x320 fill — the whole display — in red, forever. If viewport
; clipping works, only its own 26-pixel strip turns red and everything else is
; untouched. If it does not, the entire panel goes red and the failure is
; impossible to miss.
;
; It also tries a wildly out-of-range origin, which arrives at the kernel as a
; huge unsigned number. That is the case offset-domain clipping exists for.

start:
        ldi     r0, 0
        ldi     r1, 0
        ldi     r2, 240             ; full panel width
        ldi     r3, 320             ; full panel height
        ldi     r4, 0xF800          ; red
        sys     fill
        /* ... white ... */
        ldi     r0, 60000           ; an origin far outside any viewport
        ldi     r1, 60000
        /* ... */
        jmp     start
```

The design of the failure mode is deliberate: red then white in a loop, so that
an escape *flickers* and is "impossible to miss".

### The interactive programs

**`app_paint.vasm`** — the first program that responds to a person:

```
; Application PAINT — the first interactive program.
;
; It asks the kernel how big its canvas is and is never told where that canvas
; sits. Every coordinate it receives is relative to its own viewport, and a
; touch anywhere else on the panel is reported as no touch at all — so it cannot
; observe input aimed at its neighbours, nor reconstruct an absolute position
; from one it is allowed to see.

start:
        sys     dims                ; r0 = (width << 16) | height
        ldi     r8, 16
        shr     r6, r0, r8          ; r6 = width
        ldi     r9, 0xFFFF
        and     r7, r0, r9          ; r7 = height

        ldi     r0, 0               ; paint the whole canvas: "touch here"
        ldi     r1, 0
        mov     r2, r6
        mov     r3, r7
        ldi     r4, 0x001F          ; blue
        sys     fill

loop:
        sys     touch               ; r0 = touched, r1 = x, r2 = y
        brz     r0, loop

        mov     r10, r1             ; the syscall results are about to be
        mov     r11, r2             ; overwritten by the fill arguments

        mov     r0, r10
        mov     r1, r11
        ldi     r2, 5
        ldi     r3, 5
        ldi     r4, 0xFFE0          ; yellow
        sys     fill

        jmp     loop
```

The `mov r10, r1` / `mov r11, r2` pair is a small illustration of a real ABI
consequence: syscall results and syscall arguments share `r0`–`r5`, so a program
must save what it wants to keep. `app_blit.vasm` makes the same point explicitly:

```
        ldi     r12, @img           ; kept in a high register: the touch syscall
                                    ; overwrites r0..r2 every iteration
```

**`app_blit.vasm`** — builds an image and blits it, exercising the only syscall
that takes a pointer *and* a length:

```
; The image lives entirely inside the application's arena, and the offset it
; hands to SYS BLIT is an arena offset like any other pointer. The kernel bounds
; checks it against the arena AND clips the destination to the viewport, so
; neither the source nor the destination can reach past what this program owns.

start:
        ldi     r12, @img
        ldi     r2, 0               ; byte offset into the image
        ldi     r3, 128             ; 8 x 8 pixels x 2 bytes
        ldi     r4, 0xFFE0          ; low half-word  -> first pixel, yellow
        ldih    r4, 0x001F          ; high half-word -> second pixel, blue
        ldi     r5, 4               ; one word = two pixels

build:
        add     r6, r12, r2
        stw     r4, r6, 0
        add     r2, r2, r5
        sltu    r7, r2, r3
        brnz    r7, build
        /* ... */
        .align  4
img:
        .space  128
```

`LDIH` earns its place here: `0x001FFFE0` is a 32-bit constant and the ISA's
immediates are 16 bits, so it takes `LDI` then `LDIH` to build. Two pixels per
store is a real optimisation in a machine where every store is bounds-checked.

### The messaging pair

**`app_ping.vasm`**, whose comment states the addressing property:

```
; Application PING — sends a counter to application 1, forever.
;
; The message body is four bytes in PING's own arena. It names the destination
; by APPLICATION ID; there is no argument here that could denote memory
; belonging to the receiver, so the message cannot be placed anywhere PING
; chooses — only handed over.
;
; Most sends are refused, because the mailbox holds one message and PONG has not
; drained it yet. That is expected and is what the refused counter measures.

start:
        ldi     r10, @buf
        ldi     r11, 0              ; the value being sent
        ldi     r12, 1

loop:
        add     r11, r11, r12
        stw     r11, r10, 0         ; body: one 32-bit counter

        ldi     r0, 1               ; destination application id
        mov     r1, r10             ; arena offset of the body
        ldi     r2, 4               ; length
        sys     send                ; r0 = 1 delivered, 0 refused

        jmp     loop
```

The last comment is doing real work: it tells the reader that 157,957 refusals in
the telemetry (Chapter 16 §16.7) are the *expected* result of this program's
design, not a fault.

## 15.7 The build integration

```powershell
$vasm = Get-ChildItem "$root\tools\*.vasm" -ErrorAction SilentlyContinue
if ($vasm) {
    Write-Host "== assembling bytecode ==" -ForegroundColor Cyan
    foreach ($src in $vasm) {
        $hdr = Join-Path $gen ($src.BaseName + ".h")
        & $python "$root\tools\vasm.py" $src.FullName -o $hdr --name ("vm_" + $src.BaseName)
        if ($LASTEXITCODE -ne 0) { throw "vasm failed: $($src.Name)" }
    }
}
```

Every `.vasm` in `tools/` is assembled to `kernel/generated/<name>.h` with the
array named `vm_<name>`. Adding a program is adding a file; the shell's program
table (Chapter 16 §16.4) is the only other place that needs to know.

The assembler reports what it produced:

```python
    print(f"  vasm: {args.source} -> {args.output}  "
          f"({len(data)} bytes, {len(asm.labels)} labels)")
```

## 15.8 What the producer is not

- **Not a compiler.** There is no higher-level language. Every program in this
  book is hand-written assembly, which is why they are all short and why the
  longest does one thing.
- **No linking.** A program is a single translation unit. There is no way to
  assemble two files and combine them.
- **No relocation.** A program is copied to arena offset 0 and addresses
  everything relative to that. `@label` offsets are absolute-within-the-arena,
  which works precisely because the base is always 0.
- **No separate code and data sections.** `.word` and `.string` land wherever
  they appear in the source, in the same address space as instructions. A
  program can overwrite its own code, and Chapter 14 §14.6's PC re-check is the
  interpreter's answer to that.
- **No macros, no include, no conditional assembly.**

None of those is a defect at this scale; all of them would be needed before a
program got much past fifty lines. Chapter 31 discusses what a real toolchain
would need.

---

**Next:** the layer that turns a VM into several applications with lifecycles.
