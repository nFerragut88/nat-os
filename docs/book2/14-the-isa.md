# Chapter 14 — The Instruction Set and the Interpreter

> Sources: `docs/UM-NATOS-012-m4-verification.md`
> Code: `kernel/vm.h`, `kernel/vm.c`, `kernel/kstring.c`

---

## 14.1 Encoding

Fixed 4-byte instructions. No variable length, no prefixes.

```
byte 0   opcode
byte 1   a      destination register, or the sole operand
byte 2   b      source register, or immediate low byte
byte 3   c      source register / offset, or immediate high byte
```

```c
/* ---- instruction encoding -------------------------------------------------
 *
 * R-format reads a, b, c as register indices. I-format reads a as a register
 * and (b, c) as a little-endian 16-bit immediate. A register index of 16 or
 * more is a fault rather than a masked-down index: silently truncating turns a
 * producer bug into wrong answers instead of a diagnosed stop.
 */
```

Sixteen registers, `r0`–`r15`. A 32-entry kernel-side call stack. Four-byte
instructions and 4-byte alignment enforced on both the PC and word accesses.

```c
#define VM_REGS       16
#define VM_CALL_DEPTH 32       /* kernel-side return stack */
#define VM_INSN_BYTES 4
```

## 14.2 The 35 opcodes

| Group | Opcodes |
|---|---|
| Control | `HALT NOP MOV LDI LDIH` |
| Arithmetic / logic | `ADD SUB MUL DIV MOD AND OR XOR SHL SHR SAR NOT NEG ADDI` |
| Comparison | `SEQ SNE SLT SLTU SLE SLEU` |
| Control flow | `JMP BRZ BRNZ CALL RET` |
| Memory | `LDW LDB STW STB` |
| Services | `SYS` |

The full enumeration, with the semantics recorded inline — this is the
authoritative ISA definition and Appendix B is derived from it:

```c
enum {
    /* control */
    VM_OP_HALT = 0x00,
    VM_OP_NOP  = 0x01,
    VM_OP_MOV  = 0x02,   /* a <- b                                   */
    VM_OP_LDI  = 0x03,   /* a <- imm16 (zero-extended)               */
    VM_OP_LDIH = 0x04,   /* a <- (a & 0xFFFF) | (imm16 << 16)        */

    /* arithmetic and logic */
    VM_OP_ADD  = 0x10,   /* a <- b + c                               */
    VM_OP_SUB  = 0x11,
    VM_OP_MUL  = 0x12,
    VM_OP_DIV  = 0x13,   /* c == 0 faults                            */
    VM_OP_MOD  = 0x14,   /* c == 0 faults                            */
    VM_OP_AND  = 0x15,
    VM_OP_OR   = 0x16,
    VM_OP_XOR  = 0x17,
    VM_OP_SHL  = 0x18,   /* shift count taken modulo 32              */
    VM_OP_SHR  = 0x19,   /* logical                                  */
    VM_OP_SAR  = 0x1A,   /* arithmetic                               */
    VM_OP_NOT  = 0x1B,   /* a <- ~b                                  */
    VM_OP_NEG  = 0x1C,   /* a <- -b                                  */
    VM_OP_ADDI = 0x1D,   /* a <- a + sign_extend(imm16)              */

    /* comparison — produce 0 or 1 so branches stay simple */
    VM_OP_SEQ  = 0x20,
    VM_OP_SNE  = 0x21,
    VM_OP_SLT  = 0x22,   /* signed                                   */
    VM_OP_SLTU = 0x23,
    VM_OP_SLE  = 0x24,
    VM_OP_SLEU = 0x25,

    /* control flow — immediates count INSTRUCTIONS, relative to the next one */
    VM_OP_JMP  = 0x30,
    VM_OP_BRZ  = 0x31,
    VM_OP_BRNZ = 0x32,
    VM_OP_CALL = 0x33,
    VM_OP_RET  = 0x34,

    /* memory — every access bounds-checked; c is an unsigned byte offset */
    VM_OP_LDW  = 0x40,   /* a <- u32 at [b + c]; must be 4-byte aligned */
    VM_OP_LDB  = 0x41,   /* a <- u8  at [b + c]                        */
    VM_OP_STW  = 0x42,   /* u32 at [b + c] <- a                        */
    VM_OP_STB  = 0x43,   /* u8  at [b + c] <- a                        */

    VM_OP_SYS  = 0x50    /* imm16 selects the service; see below       */
};
```

Note the opcode *numbering*: groups start at multiples of 0x10 with gaps. That is
not decoration — it leaves room to add within a group without disturbing the
existing values, which matters once a producer and programs exist.

### One opcode for every service, and why that mattered

`SYS` is one opcode of 35. Twelve services hang off it via an immediate. M4
shipped five; seven were added afterwards, and:

> The ISA itself is unchanged: `SYS` is still one opcode of 35, and every
> addition is a service number rather than an instruction. That was the point of
> spending an opcode on a dispatch number instead of encoding services directly
> — **the instruction set has not had to move since it was fixed.**

The roadmap's principal risk for M4 was "instruction set design is difficult to
revise once a producer and programs exist". This is the design decision that
retired that risk.

> **Since written.** Fourteen services now, not twelve: `device` (12) and `event`
> (13). The claim above held through both, and the opcode count is still 35.
>
> `device` is the last service that will be added for *hardware* — a new
> peripheral is a `device.h` table entry rather than a service number, which is
> the same argument one level up (UM-NATOS-031). `event` came afterwards anyway,
> and deliberately: it is about the execution model rather than about a
> peripheral, and no device table can express "the kernel may call into this
> program". The distinction is recorded in `vm.h` beside the enum so the earlier
> claim is not quietly broken.

## 14.3 State

Everything the VM needs is in one struct, which is what makes `vm_run()`
resumable and multiplexing free:

```c
typedef struct {
    uint32_t reg[VM_REGS];
    uint32_t pc;                        /* byte offset into the arena   */

    uint32_t call[VM_CALL_DEPTH];
    uint32_t call_sp;

    /* Viewport, in panel coordinates. Assigned by the kernel; the program can
     * read its size but never its position, and cannot draw outside it. */
    uint32_t vx, vy, vw, vh;

    int      arena;                     /* owning arena id              */

    /* Which application this VM is. Messaging addresses applications, not
     * arenas or tasks, so the identity has to be here. -1 for a VM the kernel
     * hosts directly, which therefore cannot be sent to. */
    int      app_id;
    uint32_t base;                      /* cached kernel address        */
    uint32_t size;                      /* cached arena length          */

    int      fault;
    uint32_t fault_pc;                  /* pc of the faulting insn      */
    uint32_t fault_detail;              /* offending offset, opcode, …  */

    int      yield_now;

    uint32_t executed;                  /* instructions retired         */
    uint32_t exit_status;
} vm_t;
```

> **Since written.** The struct has grown an event block — `evt_handler[]`,
> `evt_param[]`, `evt_due[]`, `evt_arg[]`, `evt_pending`, `in_handler`,
> `handler_sp` and `saved_reg[]`. The excerpt above is M4's and is no longer the
> whole of `vm_t`; `kernel/vm.h` is.
>
> The property this section is about survived the addition intact, and that is
> the point worth recording: event delivery adds **state**, not a second program
> counter. A handler is entered by pushing the current PC onto the same `call`
> stack and jumping, so `vm_run()` is still resumable at any instruction boundary
> and the interpreter still never has to be reentrant. See Appendix B §B.5.

`base` and `size` are cached from the arena so the inner loop does not make a
function call per access. That duplication is the subject of §14.5.

`fault_pc` and `fault_detail` are separate from `pc` deliberately: a faulted VM
retains the *state at the fault* while `pc` is wherever the fetch got to. The
diagnostic in `app.c` prints all three.

## 14.4 The fault taxonomy

Ten codes. `VM_FAULT_NONE` is the only non-terminal value:

```c
enum {
    VM_FAULT_NONE = 0,
    VM_FAULT_OPCODE,        /* unknown opcode                          */
    VM_FAULT_REG,           /* register index >= VM_REGS               */
    VM_FAULT_BOUNDS,        /* access outside the arena                */
    VM_FAULT_ALIGN,         /* misaligned word access                  */
    VM_FAULT_DIV0,
    VM_FAULT_PC,            /* pc outside the arena, or misaligned     */
    VM_FAULT_CALL_DEPTH,    /* call stack exhausted                    */
    VM_FAULT_RET,           /* return with an empty call stack         */
    VM_FAULT_SYSCALL,       /* unknown syscall number                  */
    VM_FAULT_STRING         /* PUTS string unterminated inside arena   */
};
```

Recording a fault captures three things at once:

```c
static void fault(vm_t *vm, int code, uint32_t detail)
{
    vm->fault        = code;
    vm->fault_pc     = vm->pc;
    vm->fault_detail = detail;
}
```

and every code has a name, because a number in a `ps` table is not a diagnosis:

```c
const char *vm_fault_name(int f)
{
    switch (f) {
    case VM_FAULT_NONE:       return "none";
    case VM_FAULT_OPCODE:     return "bad opcode";
    case VM_FAULT_REG:        return "bad register";
    case VM_FAULT_BOUNDS:     return "out of bounds";
    case VM_FAULT_ALIGN:      return "misaligned";
    case VM_FAULT_DIV0:       return "divide by zero";
    case VM_FAULT_PC:         return "bad pc";
    case VM_FAULT_CALL_DEPTH: return "call depth";
    case VM_FAULT_RET:        return "return underflow";
    case VM_FAULT_SYSCALL:    return "bad syscall";
    case VM_FAULT_STRING:     return "unterminated string";
    default:                  return "unknown";
    }
}
```

## 14.5 The bounds predicate, and its cross-check

```c
/* Bounds predicate. Deliberately identical in meaning to arena_contains(), but
 * expressed against the cached base/size so the inner loop does not make a
 * function call per access.
 *
 * Comparison happens in the OFFSET domain. Testing `off + len` against `size`
 * directly can wrap and admit an access far outside the arena, which is exactly
 * what a hostile length would aim for. m4_selftest() cross-checks this against
 * arena_contains() so the duplication cannot drift unnoticed. */
int vm_in_bounds(const vm_t *vm, uint32_t off, uint32_t len)
{
    if (off > vm->size) {
        return 0;
    }
    return len <= vm->size - off;
}
```

Three lines. The whole isolation guarantee reduces to them.

Duplicated logic drifts, so the agreement is **tested rather than trusted**:

```
[4d] predicate: PASS  vm_in_bounds agrees with arena_contains over 35 cases
```

> 35 combinations of offset and length, including zero lengths, the exact end,
> one past the end, and lengths chosen to wrap the address space. All agree.

The function is exported from `vm.h` specifically so the test can reach it:

```c
/* Exposed so the bounds predicate can be cross-checked against
 * arena_contains() rather than assumed to agree with it. */
int vm_in_bounds(const vm_t *vm, uint32_t off, uint32_t len);
```

That is the "samples must straddle" discipline of Chapter 21 §21.4 applied in
advance: the 35 cases are chosen to *span* the boundary rather than to agree
with it.

### The checked accessors

Four functions, each of which is the only way memory is touched:

```c
static uint32_t load_u32(vm_t *vm, uint32_t off, int *ok)
{
    if (!vm_in_bounds(vm, off, 4)) {
        fault(vm, VM_FAULT_BOUNDS, off);
        *ok = 0;
        return 0;
    }
    if (off & 3u) {
        fault(vm, VM_FAULT_ALIGN, off);
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return *(volatile uint32_t *)(vm->base + off);
}

static int store_u32(vm_t *vm, uint32_t off, uint32_t v)
{
    if (!vm_in_bounds(vm, off, 4)) {
        fault(vm, VM_FAULT_BOUNDS, off);
        return 0;
    }
    if (off & 3u) {
        fault(vm, VM_FAULT_ALIGN, off);
        return 0;
    }
    *(volatile uint32_t *)(vm->base + off) = v;
    return 1;
}
```

Bounds *before* alignment. Both orders would be safe, but this one means an
out-of-range misaligned access reports `BOUNDS` rather than `ALIGN`, which is the
more useful diagnosis.

`volatile` on the access is deliberate: the compiler must not cache or reorder a
read of memory that another agent — the kernel reading a progress word, or a
`SYS RECV` writing into the arena — can change.

## 14.6 The dispatch loop

```c
int vm_run(vm_t *vm, uint32_t quantum)
{
    if (vm->fault != VM_FAULT_NONE) {
        return VM_RUN_FAULTED;
    }

    while (quantum--) {
        /* Instruction fetch is bounds-checked like any other access. Code and
         * data share the arena, so nothing here may be assumed valid merely
         * because the pc got there by executing. */
        if (!vm_in_bounds(vm, vm->pc, VM_INSN_BYTES) || (vm->pc & 3u)) {
            fault(vm, VM_FAULT_PC, vm->pc);
            return VM_RUN_FAULTED;
        }

        const volatile uint8_t *ins = (const volatile uint8_t *)(vm->base + vm->pc);
        uint8_t  op  = ins[0];
        uint8_t  a   = ins[1];
        uint8_t  b   = ins[2];
        uint8_t  c   = ins[3];
        uint32_t imm = (uint32_t)b | ((uint32_t)c << 8);
        int32_t  simm = (int32_t)(int16_t)imm;

        uint32_t next = vm->pc + VM_INSN_BYTES;
```

The comment on the fetch is the important one. A program can overwrite its own
code, so the PC being valid one instruction ago says nothing about now.

### Register validation, once per instruction

```c
        /* One register-range check for the whole instruction. Operands that an
         * opcode does not use are still indices in a well-formed program, so
         * checking uniformly costs nothing and closes the gap where an unused
         * field carries garbage. Immediate formats exempt b and c. */
        int imm_form = (op == VM_OP_LDI  || op == VM_OP_LDIH || op == VM_OP_ADDI ||
                        op == VM_OP_JMP  || op == VM_OP_BRZ  || op == VM_OP_BRNZ ||
                        op == VM_OP_CALL || op == VM_OP_SYS);
        int off_form = (op == VM_OP_LDW || op == VM_OP_LDB ||
                        op == VM_OP_STW || op == VM_OP_STB);

        if (op != VM_OP_HALT && op != VM_OP_NOP && op != VM_OP_RET) {
            if (a >= VM_REGS) {
                fault(vm, VM_FAULT_REG, a);
                return VM_RUN_FAULTED;
            }
            if (!imm_form && b >= VM_REGS) {
                fault(vm, VM_FAULT_REG, b);
                return VM_RUN_FAULTED;
            }
            if (!imm_form && !off_form && c >= VM_REGS) {
                fault(vm, VM_FAULT_REG, c);
                return VM_RUN_FAULTED;
            }
        }
```

"Checking uniformly costs nothing and closes the gap where an unused field
carries garbage" — a well-formed program's unused fields are zero, so validating
them is free and turns a producer bug into a diagnosis.

### The switch

```c
        switch (op) {
        case VM_OP_HALT: vm->executed++; return VM_RUN_HALTED;
        case VM_OP_NOP:  break;

        case VM_OP_MOV:  r[a] = r[b]; break;
        case VM_OP_LDI:  r[a] = imm; break;
        case VM_OP_LDIH: r[a] = (r[a] & 0xFFFFu) | (imm << 16); break;

        case VM_OP_ADD:  r[a] = r[b] + r[c]; break;
        case VM_OP_SUB:  r[a] = r[b] - r[c]; break;
        case VM_OP_MUL:  r[a] = r[b] * r[c]; break;

        case VM_OP_DIV:
            if (r[c] == 0u) { fault(vm, VM_FAULT_DIV0, 0); return VM_RUN_FAULTED; }
            r[a] = r[b] / r[c];
            break;
        /* ... */

        /* Shift counts are masked to 5 bits. A shift of 32 or more is undefined
         * in C and would let a program's behaviour depend on the host compiler,
         * which is precisely what a VM exists to prevent. */
        case VM_OP_SHL:  r[a] = r[b] << (r[c] & 31u); break;
        case VM_OP_SHR:  r[a] = r[b] >> (r[c] & 31u); break;
        case VM_OP_SAR:  r[a] = (uint32_t)((int32_t)r[b] >> (r[c] & 31u)); break;

        /* ... */

        case VM_OP_JMP:
            next = (uint32_t)((int32_t)next + simm * VM_INSN_BYTES);
            break;
        case VM_OP_BRZ:
            if (r[a] == 0u) { next = (uint32_t)((int32_t)next + simm * VM_INSN_BYTES); }
            break;

        case VM_OP_CALL:
            if (vm->call_sp >= VM_CALL_DEPTH) {
                fault(vm, VM_FAULT_CALL_DEPTH, vm->call_sp);
                return VM_RUN_FAULTED;
            }
            vm->call[vm->call_sp++] = next;
            next = (uint32_t)((int32_t)next + simm * VM_INSN_BYTES);
            break;
        case VM_OP_RET:
            if (vm->call_sp == 0u) {
                fault(vm, VM_FAULT_RET, 0);
                return VM_RUN_FAULTED;
            }
            next = vm->call[--vm->call_sp];
            break;

        case VM_OP_LDW: r[a] = load_u32(vm, r[b] + c, &ok); break;
        case VM_OP_LDB: r[a] = load_u8 (vm, r[b] + c, &ok); break;
        case VM_OP_STW: ok = store_u32(vm, r[b] + c, r[a]); break;
        case VM_OP_STB: ok = store_u8 (vm, r[b] + c, r[a]); break;

        /* ... SYS ... */

        default:
            fault(vm, VM_FAULT_OPCODE, op);
            return VM_RUN_FAULTED;
        }

        if (!ok) {
            return VM_RUN_FAULTED;
        }

        vm->pc = next;
        vm->executed++;
    }

    return VM_RUN_QUANTUM;
}
```

Branch displacements are multiplied by `VM_INSN_BYTES` and computed relative to
`next`, so a branch cannot target a misaligned address by construction.

### Why a `switch` and not a computed goto

```c
 * The dispatch loop is a switch. A computed-goto table would be faster, but it
 * is also the kind of construct where a missing entry is a silent jump rather
 * than a diagnosed fault, and correctness of the bounds checks matters more
 * here than dispatch overhead. Revisit only with a measurement showing dispatch
 * is actually the bottleneck.
```

The `default:` case is the payoff: an unknown opcode is `VM_FAULT_OPCODE`, not
undefined behaviour.

## 14.7 The three exits

```c
/* Why vm_run() returned. */
enum {
    VM_RUN_HALTED = 0,      /* HALT or SYS_EXIT — program finished     */
    VM_RUN_FAULTED,         /* see vm_fault()                          */
    VM_RUN_QUANTUM          /* budget spent; call vm_run() again       */
};
```

> `vm_run(vm, quantum)` executes at most `quantum` instructions and returns
> `VM_RUN_QUANTUM` if the budget runs out. State lives entirely in `vm_t`, so
> execution resumes exactly where it stopped.
>
> This is the safe-boundary preemption: a task hosting a VM can be descheduled
> between any two bytecode instructions without the program being able to
> observe it. **The interpreter never has to be reentrant, and the timer
> interrupt never has to understand bytecode.**

That last sentence is the payoff for the whole design.

## 14.8 Results

```
  vm says sum(1..10) = 55
  [4a] program  : HALTED ok  insns=70 resumes=4 status=0 fault=none
  [4b] faults   : bounds=ok div0=ok opcode=ok reg=ok ret=ok align=ok  PASS
  [4c] runaway  : PASS  bounded at 500 insns, no fault, kernel alive
  [4d] predicate: PASS  vm_in_bounds agrees with arena_contains over 35 cases
```

### The program, and what makes the result meaningful

The demo exercises arithmetic, a counted loop, a bounds-checked store and reload
through arena memory, a call and return, and four syscalls. It ran to `HALT` with
exit status 0 in 70 instructions.

But:

> It was run with a quantum of 16, so it was **suspended and resumed 4 times**
> mid-execution and produced the correct answer regardless — which is the
> property that matters, not the answer itself.

### Containment: six malformed programs

| Probe | Attempts | Expected fault | Result |
|---|---|---|---|
| `bounds` | Store to offset `0xFFF0` in a 2 KB arena | `VM_FAULT_BOUNDS` | ok |
| `div0` | Divide by zero | `VM_FAULT_DIV0` | ok |
| `opcode` | Byte `0xFF` as an instruction | `VM_FAULT_OPCODE` | ok |
| `reg` | `MOV r16, r0` | `VM_FAULT_REG` | ok |
| `ret` | `RET` with an empty call stack | `VM_FAULT_RET` | ok |
| `align` | Word load at offset 1 | `VM_FAULT_ALIGN` | ok |

Each was **hand-encoded as raw bytes rather than assembled**, and the reason
matters:

> Routing them through the assembler would only have demonstrated that
> well-formed programs behave; the claim under test is about malformed ones.

The assembler would refuse to emit `MOV r16, r0`:

```python
def reg(tok):
    m = re.fullmatch(r"[rR](\d+)", tok)
    if not m:
        raise AsmError(f"expected a register, got '{tok}'")
    n = int(m.group(1))
    if not 0 <= n < 16:
        raise AsmError(f"register out of range: {tok}")
    return n
```

So a test that used it would be testing the producer, not the interpreter.

And the strongest evidence is indirect:

> every line of output after this test exists only because the kernel survived
> all six.

### Runaway

A one-instruction program branching to itself. Executed exactly 500 instructions
under a 500-instruction quantum, reported `VM_RUN_QUANTUM`, raised no fault, and
left the kernel running.

> An unterminating program is a scheduling cost, not a hang.

### Under the scheduler: the arithmetic is the proof

```
vm arena     : id=0 base=0x3ffb23f0 program=28 B
tasks        : report=0 a=1 b=2 vm=3

t=1801  switches r/a/b=451/450/450  work a/b=10585776/11610220  guards=ok
        freew a/b=480/480  corrupt=0
        | vm sw=450  insns=3291313  counter=1097103  fault=none
```

The hosted program increments a counter in its own arena forever, performing a
bounds-checked store every iteration, "so the isolation path is on the hot loop
rather than exercised once at startup".

The two preemption mechanisms compose:

> The timer interrupt suspends the VM task wherever it happens to be — almost
> always somewhere inside the interpreter's own C code, mid-instruction from the
> bytecode's point of view — and `vm_run()` separately returns at a bytecode
> instruction boundary when its quantum expires. Neither is aware of the other,
> and the program cannot observe either.

And then the check that makes this a measurement rather than an observation:

> **The arithmetic is the proof.** The loop body is exactly three instructions
> (`add`, `stw`, `jmp`), so instructions retired must be three times the counter
> plus the three-instruction preamble:
>
> ```
> 1,097,103 × 3 + 3 = 3,291,312     observed 3,291,313
> ```
>
> The residual of one is the sample being taken mid-iteration, between the
> increment and the store. Across **450 preemptions** and 3.29 million bytecode
> instructions, not one instruction was lost, replayed, or half-executed. A
> context switch that dropped or repeated an interpreter step would show here as
> drift, and there is none.

This is the single best-designed measurement in the project. Two independently
maintained quantities — a counter the *program* writes and an instruction count
the *interpreter* keeps — related by a known constant. Any error in either shows
as a residual.

### Dispatch cost, derived

Between consecutive reports 200 ticks apart, the VM retired 365,704 instructions.
200 ticks is 160,000,000 CCOUNT cycles, of which the VM task receives roughly one
quarter under an even four-way round robin:

```
40,000,000 cycles ÷ 365,704 instructions ≈ 109 cycles per bytecode instruction
```

> That figure covers full dispatch, operand decode, register-range validation,
> and — for two of the three instructions in the loop — a bounds-checked memory
> access. It is a **ceiling** rather than a precise cost, since the quarter-share
> assumption ignores time spent in the interrupt handler itself.
>
> 109 cycles is high enough that a computed-goto dispatch table would be worth
> measuring if the VM ever becomes the bottleneck, and low enough that nothing
> here justifies trading away the diagnosability of a `switch` today.

## 14.9 `kstring.c`, and the trap it nearly created

The link failed on an undefined reference to `memcpy` from a function whose
source never mentions it.

> GCC synthesises calls to `memcpy`/`memset` from ordinary C — byte-copy loops,
> struct assignments, large local initialisers — and does so even under
> `-fno-builtin`, which only governs the treatment of those names when written
> explicitly. Under `-nostdlib` nothing supplies them.

`kernel/kstring.c` provides `memcpy`, `memset`, `memmove` and `memcmp`. And then:

> The trap worth recording is what happens next: GCC will recognise the copy loop
> *inside* `memcpy` as a memcpy and rewrite it into a call to itself. That bug is
> silent, infinitely recursive, and would surface as a stack overflow in whatever
> unrelated code first copied a struct.

`-fno-tree-loop-distribute-patterns` is the supported way to prevent it, and it
is on the build's command line with that explanation attached (Chapter 5 §5.4).

## 14.10 Metrics

| Quantity | Value |
|---|---|
| Opcodes | 35 (roadmap ceiling ~40) |
| Instruction width | 4 B fixed |
| Registers | 16 |
| Call depth | 32, in kernel memory |
| Demo program | 120 B, 6 labels |
| Instructions to compute `sum(1..10)` | 70 |
| Quantum resumptions during demo | 4 |
| Fault classes exercised | 6 of 10 defined |
| Bounds predicate cases cross-checked | 35 |
| Bytecode instructions under the scheduler | 3,291,313 |
| Preemptions survived | 450 |
| Instruction-accounting drift | 1 (mid-iteration sample) |
| Dispatch cost | ~109 cycles/instruction (ceiling) |
| Image size at M4 | 10,560 B |

## 14.11 What M4 does not establish

- **No arena-relative program loading.** A program is copied to offset 0 and
  addresses everything relative to its arena. There is no relocation, no linking
  of separately assembled units, and no code/data separation within the arena —
  a program can overwrite its own instructions.
- **Only 6 of 10 fault classes are exercised.** `VM_FAULT_PC`,
  `VM_FAULT_CALL_DEPTH`, `VM_FAULT_SYSCALL` and `VM_FAULT_STRING` are
  implemented but untested on hardware.
- **Dispatch cost is bounded, not precisely measured.** A direct measurement
  would need CCOUNT sampled either side of a `vm_run()` call — and, given
  Chapter 9 §9.8, `task_cpu_cycles()` rather than `xt_ccount()`.
- **No multiple concurrent VMs** at M4, though nothing in the design prevented
  more. Chapter 16 added four.
- **The hosted program is not replaced when it stops.** A halted or faulted VM
  yields its task forever rather than loading something else.
- **Syscall argument validation is per-call, not systematic.**

  > Every syscall added since checks its own arguments, and each was reasoned
  > about individually. Nothing enforces that a future one will, and there is no
  > shared harness that would catch an unchecked length in a new service.

  That last one is the most serious open item in this chapter and is in
  Chapter 30.

  > **Since written.** The harness exists: `kernel/vmarg.c`, one place where an
  > `(offset, length)` pair from a program is checked, built *before* the device
  > model rather than after it (UM-NATOS-031 §2). `sys device` uses it for every
  > buffer it touches.
  >
  > **No arena-relative loading and no code/data separation are still true**, and
  > a program can still overwrite its own instructions — which is why the
  > interpreter bounds-checks the instruction fetch itself (§14.6).

### One more thing this chapter does not establish

**There is no stack frame convention, and there are no stack frames.** `call`
pushes a return address onto the kernel-side stack and `ret` pops it; that is the
whole mechanism. No frame pointer, no argument convention, no callee-saved set,
and no working recursion — a recursive routine's return addresses stack up
correctly while its locals overwrite each other.

This is not an omission in the chapter. It is a property of the machine, and it
is the direct cost of the decision in §14.3 that makes return addresses
uncorruptable: a program with no stack of its own has nowhere to put a frame. A
compiler targeting this ISA would have to build a data stack in the arena by
hand. Appendix B §B.6 states it in full.

---

**Next:** the producer that makes the ISA usable, and the programs written in it.
