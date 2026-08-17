# Chapter 13 — Why a Bytecode VM Is the Only Isolation Available

> Sources: `docs/UM-NATOS-001-architecture.md` §4, `docs/UM-NATOS-012-m4-verification.md` §2
> Code: `kernel/vm.h`, `kernel/arena.h`

---

## 13.1 The argument, restated in one place

Chapter 1 introduced the constraint. This chapter is the design that follows from
it, gathered in one place because the VM's shape is almost entirely determined by
that single hardware fact and it is easy to lose the thread across five reports.

**The premise.** The ESP32's MMU translates flash addresses for
execute-in-place caching. It does not implement per-process page tables. There is
one address space and any code can write any address.

**The consequence.** A native-code application can corrupt the kernel and every
other application, and no kernel design prevents this.

**The response.** Run applications in an interpreter. Every memory access
executes as a VM instruction, so the interpreter can bounds-check it in software.
That recovers, in the interpreter, the guarantee the silicon will not give.

**The decision sentence**, from UM-NATOS-001 §4.2:

> An operating system whose applications can silently corrupt each other is
> firmware with extra steps, and on hardware without an MMU the VM is the only
> mechanism that can deliver the property.

## 13.2 The four properties the VM buys

The isolation is the reason. Three more properties come with it, and two of them
turned out to matter as much.

### 1. Isolation, enforced per access

Stated in `vm.h` in the strongest terms the file can manage:

```c
 * THE VM IS THE ISOLATION MECHANISM. The ESP32 has no MMU paging, so there is
 * no hardware that can confine an application (UM-NATOS-001 §4.2). Every memory
 * access a program makes is bounds-checked against its arena in software, on
 * every instruction, and that check is not optional and must never be compiled
 * out for speed. If it is removed, this kernel has no isolation of any kind.
```

`arena.h` says the same thing from the other side:

```c
 * This is the ONLY isolation mechanism this kernel will have. ... the
 * interpreter must do it in software on every access, and it must not be
 * compiled out for speed.
```

Two files, two independent statements of the same non-negotiable. That
redundancy is deliberate: a future optimiser reading either file is told.

### 2. Preemption at a safe boundary, for free

> **Preemption becomes trivial.** The dispatch loop can check a quantum counter
> between instructions; an application can always be interrupted at a known-safe
> boundary, with no native context switch required for application tasks.

The consequence for the kernel is large and is worth stating separately, because
it is the reason there are only nine native tasks:

> native preemptive context switching is needed only for kernel and driver
> tasks, not for applications. The number of native tasks is expected to stay
> small.

`vm.h` restates it as an interface property:

```c
 * Execution is bounded by an instruction quantum rather than run to completion,
 * so a task hosting a VM can be preempted at an instruction boundary — the
 * safe-boundary preemption model of UM-NATOS-001 §4.2. A program that never
 * terminates costs its quantum and no more.
```

### 3. A small, auditable instruction set

The instruction set is ours, so it can be kept small. UM-NATOS-007 §6 set a
ceiling of "roughly 40 opcodes — and resist convenience additions". The result
is 35, and it has not moved since it was fixed. Chapter 14 §14.2 covers why
twelve syscalls were added without adding a single opcode.

### 4. Faults that stop a program rather than the system

```c
 *   - A faulting program stops the program, never the kernel. Every fault is a
 *     recorded code and a halt, and vm_run() returns normally afterwards.
```

## 13.3 The two costs, and where they landed

**Slower than native.** Real, and at the time of the decision unmeasured. Later
derived at roughly **109 cycles per bytecode instruction** including full
dispatch, operand decode, register-range validation and — for two of the three
instructions in the measured loop — a bounds-checked memory access
(Chapter 14 §14.8).

**Requires a producer.** A compiler or assembler must exist. That cost was
deliberately moved off the device:

> It runs on the **host**. Nothing about a producer needs to live in 176 KB of
> DRAM, and keeping it off the device means the on-device side stays a pure
> interpreter with no parsing, no symbol table, and no attack surface from
> untrusted text.

That last clause is a security property obtained as a side effect of a memory
decision, and it is worth noticing: the device cannot be attacked through
malformed *source* because it never sees source.

## 13.4 Register machine, not stack machine

Recorded as pending in the roadmap and settled at M4.

| | Stack machine | Register machine (selected) |
|---|---|---|
| Instructions per unit of work | High — operands pushed and popped | Low — operands named directly |
| Encoding | Compact, often 1 byte | 4 bytes fixed |
| Decode | Trivial | One byte plus three fields |
| Producer complexity | Lower — expression trees map directly | Higher — needs register allocation |
| Dispatch overhead per useful operation | Paid several times | Paid once |

The deciding argument:

> Dispatch is the inner loop of everything this kernel will ever run. A stack
> machine spends a large share of its instructions moving operands rather than
> computing with them, and each of those pays the full dispatch cost. A register
> machine executes the same program in materially fewer instructions, which more
> than repays a wider encoding.
>
> The cost is a more demanding producer. That cost lands on the host, where
> there is no memory constraint and no consequence for being slow.

The concrete number: `sum(1..10)` compiles to **70 executed instructions**
including the loop, syscalls, and a call/return.

## 13.5 Return addresses live in kernel memory

This is the design decision that most clearly shows what "unrepresentable rather
than refused" buys.

`CALL` pushes to a kernel-side stack of 32 entries in the `vm_t` structure, not
into the arena:

```c
typedef struct {
    uint32_t reg[VM_REGS];
    uint32_t pc;                        /* byte offset into the arena   */

    /* Return addresses, deliberately outside anything the program can
     * address. See the header comment. */
    uint32_t call[VM_CALL_DEPTH];
    uint32_t call_sp;
    /* ... */
} vm_t;
```

The header comment:

```c
 *   - Return addresses live in KERNEL memory, not in the arena. A program
 *     cannot reach its own call stack, so it cannot corrupt a return address
 *     however wrong or hostile it is. This is the one structure where being
 *     unable to express something is worth more than the flexibility of a
 *     conventional in-memory stack.
```

And the report's version:

> A program therefore **cannot address its own return addresses**. No amount of
> wild pointer arithmetic within its arena can corrupt where a function returns
> to, because that data is not in the arena at all. The entire family of
> stack-smashing bugs is unexpressible rather than defended against.
>
> The cost is a fixed call depth and no stack-allocated locals in the
> conventional sense. For a kernel whose isolation is entirely software-enforced,
> being unable to express the attack is worth more than the flexibility.

A depth of 32 is a real limit and it is a *diagnosed* one:
`VM_FAULT_CALL_DEPTH` exists and is checked on every `CALL`. Chapter 30 notes it
is one of four fault classes implemented but never exercised on hardware.

## 13.6 No condition codes, and why that is a scheduling decision

Comparisons produce 0 or 1 into a register rather than setting flags, so branches
need only test a register for zero:

```c
    /* comparison — produce 0 or 1 so branches stay simple */
    VM_OP_SEQ  = 0x20,
    VM_OP_SNE  = 0x21,
    VM_OP_SLT  = 0x22,   /* signed                                   */
    VM_OP_SLTU = 0x23,
    VM_OP_SLE  = 0x24,
    VM_OP_SLEU = 0x25,
```

The reason is not aesthetic:

> That removes a whole class of state — condition codes with their own
> save/restore obligations across a context switch — from a kernel that has
> already paid once for forgetting to save processor state.

The "already paid once" is Chapter 8: `EPC3`, `EPS3`, `SAR`, and the three loop
registers all had to be added to the frame, one of them after a full debugging
session. Designing the VM so its architectural state is *only* what is already in
`vm_t` means a VM context switch is a struct that is never copied at all — the
task switch saves the interpreter's C stack, and `vm_t` simply stays where it is.

## 13.7 Choices that close failure modes

Four small decisions in the ISA, each closing a specific class of bug:

**Register indices are validated, not masked.**

> An index of 16 or more faults. Masking would turn a producer bug into wrong
> answers instead of a diagnosed stop.

**Shift counts are masked to 5 bits.**

> A shift of 32 or more is undefined in C; leaving it undefined would let a
> program's behaviour depend on the host compiler, which is precisely what a VM
> exists to prevent.

That is a subtle and good point. The VM's job is to give a program *defined*
behaviour; inheriting C's undefined behaviour would defeat it at exactly the
place a hostile program would probe.

**Word accesses must be 4-byte aligned.**

> Faulting is deterministic; the alternative is an unaligned-access exception in
> kernel context.

**Branch displacements count instructions**, so a branch cannot target a
misaligned address. The assembler enforces the same thing at the other end:

```python
    def rel(self, tok, here):
        """Branch displacement, counted in instructions from the NEXT one."""
        target = self.value(tok)
        if target % INSN_BYTES:
            raise AsmError(f"branch target '{tok}' is not instruction-aligned")
```

## 13.8 The asymmetry that is the whole security model

Stated in `app.c`:

```c
/* Reads the progress word out of a live arena. The kernel can see into an
 * arena; a program cannot see out of one. That asymmetry is the whole design. */
uint32_t app_published(int id)
```

and in `kmain.c` for the kernel's own hosted VM:

```c
/* The counter the bytecode publishes into its arena. Reading it from kernel
 * code is not a special capability — an arena is ordinary DRAM, and the
 * asymmetry is deliberate: the kernel can see into an arena, a program cannot
 * see out of one. */
static uint32_t vm_counter(void)
```

UM-NATOS-013 §3 makes the same point about *observability* rather than access:

> An application publishes a progress word at a known offset in its own arena.
> The kernel reads it directly. **The kernel can see into an arena; a program
> cannot see out of one** — that asymmetry is the entire security model, and the
> progress word makes it visible rather than theoretical.

This is why the kernel can report `published=1270666` in `ps` without any
cooperation from the program, and why a faulted program's progress can be read
one last time before its arena is released.

## 13.9 What the model does not claim

Worth stating before Chapter 14 measures what it does claim.

- **Native tasks are not isolated and never will be.** Drivers, the shell, the
  renderer and the kernel share one address space with nothing between them.
- **A program can overwrite its own instructions.** Code and data share the arena
  with no separation and no relocation. `vm_run` re-checks the PC on every fetch
  for exactly this reason (Chapter 14 §14.6).
- **A program can consume its whole quantum forever.** That is a scheduling cost,
  not a hang, and it is measured rather than asserted (Chapter 14 §14.7).
- **Nothing limits how much a program draws.** A program that draws constantly
  costs the display mutex and everyone's frame rate, and nothing measures or
  limits that per-application. Chapter 11's best-effort policy bounds the damage
  to the system without bounding it per program.
- **There is no device model.** Twelve syscalls, all hardcoded. An application
  cannot read the light sensor, scan the I²C bus, make a sound, save state, or
  receive a keypress. Chapter 31 argues this is the single most valuable thing
  left to build.

---

**Next:** the instruction set, the dispatch loop, and the six deliberately
malformed programs that could not escape.
