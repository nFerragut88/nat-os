# Chapter 2 — call0: The Calling Convention That Made a Scheduler Tractable

> Sources: `docs/UM-NATOS-003-abi.md`
> Code: `kernel/start.S`, `kernel/vectors.S`, `kernel/window.S`, `build.ps1`

---

## 2.1 The decision, and why it is load-bearing

The Xtensa LX6 supports two calling conventions: the **windowed** ABI and the
**call0** ABI. nat-os is built entirely with call0.

This is the single change that makes a from-scratch scheduler tractable on this
architecture, and it cannot be revisited cheaply once drivers and a VM exist,
because ABIs cannot be mixed within a link. It had to be decided before the
first line of kernel code was written, and it was — and it was *tested* rather
than assumed available (§2.4).

## 2.2 What the windowed ABI does

The Xtensa architecture optionally implements a **register window**: 64 physical
address registers of which 16 are visible, with the visible window rotating on
call and return. `ENTRY` rotates the window and allocates a stack frame; `RETW`
reverses it.

When the window wraps, the hardware raises a **window overflow exception**, and
the handler must spill registers to the stack. Returning past a spilled frame
raises **window underflow**, and the handler must reload them.

For an operating system, three consequences follow:

- A context switch cannot simply save 16 registers. Live windows belonging to
  the outgoing task exist in the physical register file and must be spilled
  first, or restored state will be silently wrong.
- The spill mechanism must work during the switch itself, which constrains what
  the switch code may touch.
- Failures do not manifest at the switch. They manifest several returns later,
  in a function that did nothing wrong, with a corrupted frame.

UM-NATOS-003 §2 names this plainly:

> This is the single hardest part of writing an Xtensa kernel and the point at
> which such projects most often stall.

## 2.3 What call0 does instead

The call0 ABI does not use windows. It behaves as a conventional RISC calling
convention:

| Register | Role under call0 |
|---|---|
| `a0` | Return address |
| `a1` | Stack pointer |
| `a2`–`a7` | Arguments / return values |
| `a12`–`a15` | Callee-saved |

No `ENTRY`, no `RETW`, no window exceptions. **A context switch becomes a
conventional register save and restore.**

The entry stub says so in its own header comment:

```asm
/* nat-os — entry stub.
 *
 * The first instruction of the OS. Runs with whatever state the bootloader
 * left behind, so it establishes its own stack before touching C.
 *
 * Built with -mabi=call0: no ENTRY/RETW, no register windows, no spill
 * exceptions. a1 is the stack pointer and a0 is the return address, exactly
 * as on a conventional architecture. That choice is what makes the context
 * switch later a plain register save rather than window-spill surgery.
 */
```

## 2.4 Verification: the ABI was tested, not assumed

The availability of call0 for this target was tested against the installed
toolchain (`xtensa-esp32-elf-gcc`, crosstool-NG esp-14.2.0_20241119) **before any
project code was written.**

Test input:

```c
int add(int a, int b) { return a + b; }
int chain(int x)      { return add(x, add(x, x)); }
```

Compiled at `-O2` under each ABI. The relevant emitted instructions:

| `-mabi=windowed` (default) | `-mabi=call0` |
|---|---|
| `entry sp, 32` | *(none)* |
| `retw.n` | `ret.n` |

The windowed build allocates a window frame on entry and returns with `RETW`.
The call0 build emits neither — a plain `ret.n`, with no window management at
all.

**Result: call0 is supported by this toolchain for this target.** Confirmed
again in the linked kernel; the disassembly of `_start` shows the `.bss` clear
loop using `bgeu` / `s32i.n` / `addi.n` with no `entry` or `retw` present.

That loop, as written:

```asm
    /* Zero .bss. The bootloader only copies segments that have file content,
     * so uninitialised data arrives as whatever was in RAM at reset. */
    movi    a2, _bss_start
    movi    a3, _bss_end
    movi    a4, 0
.Lbss_loop:
    bgeu    a2, a3, .Lbss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .Lbss_loop
.Lbss_done:

    /* Into C. kmain must not return. */
    call0   kmain
```

`call0 kmain` — the instruction that gives the ABI its name — rather than
`call4`/`call8`/`call12`, which rotate the window.

## 2.5 The costs, all three of them

### 2.5.1 ROM routines are unusable

The ESP32 ROM exports hundreds of functions — memory routines, SPI helpers,
cryptography — all compiled for the **windowed** ABI. Calling them from call0
code corrupts state.

The report assessed the impact as low, because a from-scratch OS writes its own
drivers by definition, and named the concrete instance:

> `kernel/uart.c` writes UART registers directly instead of calling the ROM's
> output routines, specifically for this reason.

The file confirms it:

```c
/* nat-os — raw UART0 output.
 *
 * Register-level, no ROM calls (ROM routines are windowed-ABI and this kernel
 * is call0). ...
 */
```

The report also noted that "an assembly wrapper that switches conventions is
possible, but none is currently needed". That sentence stayed true for two days
and then stopped being true, decisively — see §2.7.

### 2.5.2 All code must use one ABI

Every object in the link must agree. Any third-party library compiled as
windowed cannot be linked in. This closes off casual reuse of ESP-IDF
components.

It also produced an unexpected benefit three weeks later. UM-NATOS-024 §2, on
the ADC:

> ADC2 is unusable on this part whenever WiFi is running, which is the standard
> reason to avoid it. **That reason does not apply here.** The `-mabi=call0`
> build cannot link Espressif's precompiled radio libraries at all
> (UM-NATOS-003 §5.1), so WiFi will never run and ADC2 is permanently free.

That reasoning was correct when written and was overtaken by §2.7, which is a
pleasing failure mode for a constraint.

### 2.5.3 Slightly larger code

Without windows, callee-saved registers are spilled to the stack conventionally,
so code is typically marginally larger. At 1,124 bytes of text this was "not
currently measurable and not expected to matter". At the present 116,692 bytes
against 131,072 of IRAM it still has not mattered, though the margin is now
thinner than it was.

## 2.6 The payoff, made concrete

With call0, a task switch is:

1. Push `a0`, `a12`–`a15` and any live caller-saved registers onto the outgoing
   task's stack.
2. Store the resulting stack pointer in the outgoing task's control block.
3. Load the incoming task's stack pointer.
4. Pop the registers.
5. `ret` — which returns into the incoming task.

No spilling, no window state, no exception interaction. UM-NATOS-003 §6 predicted
this would make M2 "a manageable milestone rather than the project's wall".

That prediction was half right, and the way it was wrong is Chapter 8. M2 did
consume twelve build cycles and three wrong hypotheses — but not one of them had
anything to do with register windows. The defect was `PS.EXCM` disabling the
zero-overhead loop, which is an entirely different mechanism and one that would
have bitten a windowed kernel identically.

The interrupt entry sequence in `vectors.S` is where the saving is visible. It
saves twenty-one words and nothing else:

```asm
_handler_level3:
    addi     a1, a1, -96
    s32i     a0,  a1, 0
    s32i     a2,  a1, 4
    /* ... a3 through a15 ... */
    rsr.sar  a2
    s32i     a2,  a1, 60
    rsr.epc3 a2
    s32i     a2,  a1, 64
    rsr.eps3 a2
    s32i     a2,  a1, 68
```

UM-NATOS-008 §3.2 states the accounting directly:

> **This is the entire context-save cost of the call0 decision.** Under the
> windowed ABI the same handler would additionally have to spill live register
> windows before touching the register file.

## 2.7 The reversal: making windowed code run anyway

Two days after the report declared ROM routines unusable and vendor libraries
unlinkable, both became possible — without changing the kernel's ABI at all.

The mechanism is that the window vectors had never been implemented. `vectors.S`
recorded the intent:

> Because the kernel is built -mabi=call0 there are no register windows, so the
> window overflow/underflow vectors are never taken by ITS OWN code.

and then recorded the correction, which is the interesting part:

```asm
 * They were described here as "left as traps", which was the intent and not
 * what the linker script did: nothing was placed before _vecbase + 0x1C0, so
 * slots 0x000..0x180 were zero-filled and a window exception would have
 * executed zeros. PS.WOE is set by the ROM, so the mechanism was armed with
 * nothing behind it.
 *
 * They are now real handlers — see kernel/window.S — which is what lets
 * WINDOWED code run here at all.
```

`window.S` implements the six window vectors. Its header makes the safety
argument for adding them:

```asm
 * ---- why this is safe to add ---------------------------------------------
 *
 * These vectors are currently EMPTY. ...
 *
 * PS.WOE is already SET — the ROM leaves it that way and task.c preserves it
 * when fabricating task frames — so the mechanism is armed and only the
 * handlers are missing.
 *
 * Filling empty vectors is therefore purely additive. Code that never triggers
 * a window exception cannot tell the difference, which is the whole reason this
 * can go in without risking anything that already works.
```

That `task.c` detail is not incidental. Task creation inherits PS from the
running kernel rather than fabricating one:

```c
        /* Resume at the entry point, with interrupts admitted. PS is taken
         * from the running kernel with INTLEVEL forced to 0, so the task
         * inherits the same execution mode rather than a guessed one — the
         * ROM leaves WOE and CALLINC set, and fabricating a different PS would
         * put the task in a subtly different state from its creator. */
        frame[TASK_FRAME_IDX_EPC3] = (uint32_t)entry;
        frame[TASK_FRAME_IDX_EPS3] = xt_get_ps() & ~0xFu;
```

A decision made in Chapter 8 for reasons of caution turned out, in Chapter 27,
to be the reason windowed code could run inside a task at all.

### The handlers themselves

Six vectors, taken verbatim rather than derived, and the reason is stated:

```asm
 * Taken verbatim from Espressif's xtensa_vectors.S rather than derived. A wrong
 * register number here does not fault — it silently reloads a caller's frame
 * with the wrong values, which is the least debuggable failure this project
 * could construct. This is the third time today that fetching a definition
 * beat recalling one (UM-NATOS-024 §4).
```

The `CALL4` pair is short enough to show whole:

```asm
    .section .vectors.window.of4, "ax"
    .align 4
    .global _WindowOverflow4
_WindowOverflow4:
    s32e    a0, a5, -16             /* save a0 to call[j+1]'s stack frame */
    s32e    a1, a5, -12
    s32e    a2, a5,  -8
    s32e    a3, a5,  -4
    rfwo                            /* rotate back to call[i] */

    .section .vectors.window.uf4, "ax"
    .align 4
    .global _WindowUnderflow4
_WindowUnderflow4:
    l32e    a0, a5, -16
    l32e    a1, a5, -12
    l32e    a2, a5,  -8
    l32e    a3, a5,  -4
    rfwu
```

`CALL8`'s overflow handler is where a subtle ABI requirement lives, and it cost
this project a full debugging session in Chapter 27:

```asm
_WindowOverflow8:
    s32e    a0, a9, -16
    l32e    a0, a1, -12             /* a0 <- call[j-1]'s sp */
    s32e    a1, a9, -12
    s32e    a2, a9,  -8
    s32e    a3, a9,  -4
    s32e    a4, a0, -32
    s32e    a5, a0, -28
    s32e    a6, a0, -24
    s32e    a7, a0, -20
    rfwo
```

That second instruction — `l32e a0, a1, -12` — fetches the *caller's* stack
pointer from a slot every windowed frame is required to carry. `ENTRY` does not
write it; the caller's prologue does. Call0 code has no reason to know that, and
when nat-os first called into the PHY blob on a private stack, nothing had
written it. The handler loaded a fresh `.bss` zero and spilled to address
`0 - 32`. Chapter 27 §27.5 is the full story, including why the reported fault
address was never where the store was.

### The verification: a checksum over handler invocations

`window.h` records how the handlers were proved rather than merely observed not
to crash:

```c
/* Calls a windowed-ABI function from call0 code and returns what it returned.
 *
 * win_probe(n) recurses n deep and returns n. With CALL8 the 64 physical
 * registers are exhausted after 8 frames, so any depth above that MUST take
 * window overflow exceptions on the way down and underflows on the way back.
 * The return value is therefore a checksum over every handler invocation: a
 * handler that reloads the wrong register returns the wrong number rather than
 * merely failing to crash. */
uint32_t win_call_probe(uint32_t depth);
```

Then three escalating proofs, each with a distinct thing it establishes:

```c
/* Calls vendor_probe() in vendor/windowed/, which is compiled -mabi=windowed by
 * the same compiler that builds Espressif's libraries. Standing proof that this
 * kernel can call vendor-ABI code, not just hand-written windowed assembly. */
uint32_t win_call_vendor(uint32_t depth, uint32_t seed);

/* Calls a three-argument WINDOWED function at an arbitrary address.
 *
 * Espressif's ROM holds hundreds of routines at fixed addresses, all windowed,
 * needing no linking and no environment. This makes every one of them callable.
 * crc32_le lives at 0x4005CFEC and is a pure function, which is why it is the
 * first vendor code this kernel runs. */
uint32_t rom_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);

/* Calls a windowed function that calls BACK into call0 code on every
 * iteration -- proof the reverse bridge round-trips. */
uint32_t win_call_bridge(uint32_t fn_add, uint32_t depth);
```

`crc32_le` was chosen as the first ROM call because it is a *pure function with
a known answer*. A CRC-32 that comes back matching the textbook value cannot
have been produced by a broken window handler.

The commit history reads as a ladder:

```
window: implement the register-window vectors; windowed-ABI code now runs
window: link and call a real -mabi=windowed object from the call0 kernel
window: run Espressif ROM code -- crc32_le returns the textbook CRC-32
```

Three commits, three independent levels of the same claim.

## 2.8 Mixed ABIs in one image

The build compiles two sets of objects with different ABIs and links them
together. From `build.ps1`:

```powershell
# Objects built for the WINDOWED ABI, linked alongside the call0 kernel.
#
# Everything in kernel/ is -mabi=call0 and always will be. This directory holds
# code that is not: the ABI every precompiled Espressif library uses. Mixing the
# two in one image is a link-time question, not a compile-time one, and the
# register-window handlers in kernel/window.S are what make the resulting calls
# actually work at run time.
$winsrc = Get-ChildItem "$root\vendor\windowed\*.c" -ErrorAction SilentlyContinue
if ($winsrc) {
    Write-Host "== compiling windowed ABI ==" -ForegroundColor Cyan
    $wflags = @("-mabi=windowed", "-mlongcalls", "-ffreestanding", "-fno-builtin",
                "-fno-stack-protector", "-Os", "-Wall", "-Wextra", "-std=c11")
    foreach ($src in $winsrc) {
        $obj = Join-Path $build ($src.BaseName + ".o")
        Write-Host ("  {0}  [windowed]" -f $src.Name)
        & $gcc @wflags -c $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $($src.Name)" }
        $objs += $obj
    }
}
```

The two halves meet through explicit bridges in `window.S`. The WiFi OSI table
is the largest consumer:

```
libpp (windowed) → wifi_osi.c entry (windowed, does nothing)
                 → w2c_callN (window.S)
                 → wifi_osi_impl.c (call0: heap, scheduler, tick)
```

declared as:

```c
/* Windowed -> call0 bridges, for the OSI table's bodies. See window.S. */
uint32_t w2c_call0f(uint32_t fn);
uint32_t w2c_call1(uint32_t fn, uint32_t a);
uint32_t w2c_call2(uint32_t fn, uint32_t a, uint32_t b);
uint32_t w2c_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);
```

The design note in `wifi_osi_impl.c` explains why the split is where it is:

```c
 * call0, deliberately. The OSI table itself must be windowed because libpp
 * calls it, but the table's only job is to forward: the work happens here,
 * where the kernel's heap, scheduler and tick can be used directly. Each
 * windowed entry crosses back through w2c_callN() in window.S.
 *
 * That split is the design. Writing these bodies in the windowed file would
 * mean either duplicating the kernel's primitives or calling into them across
 * a boundary that does not permit it — the fault that put IllegalInstruction
 * at 0x4008a810 the first time it was tried.
```

## 2.9 The build flags

Set in `build.ps1` for compile, and `-mabi=call0` must appear on the **link**
line as well, because the driver uses it to select compatible internal libraries:

```powershell
$cflags = @(
    "-mabi=call0", "-mtext-section-literals", "-mlongcalls",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-fno-tree-loop-distribute-patterns",
    "-Os", "-Wall", "-Wextra", "-std=c11",
    "-I", "$root\kernel"
)
```

| Flag | Purpose |
|---|---|
| `-mabi=call0` | Select the non-windowed ABI. **The load-bearing flag.** |
| `-mtext-section-literals` | Place literal pools inside `.text` so they land in IRAM. Without this they land in a section the linker script does not map, and `l32r` loads fault |
| `-mlongcalls` | Permit calls beyond the short-displacement range |

Chapter 5 covers the rest.

## 2.10 Residual risk

UM-NATOS-003 §8 listed two, and both have now been paid.

**Exception and interrupt handlers.** "Their entry sequence must save state
explicitly and correctly; call0 simplifies but does not eliminate this work."
Correct. Chapter 8's defect was in exactly that sequence, though not in the part
that saves registers.

**Toolchain internals.** "`libgcc` routines pulled in for operations such as
64-bit division must also be call0. The current build links `-nostdlib` and uses
none, but that will change and should be re-checked at that point."

It changed. Two ways:

1. `kstring.c` had to be written because GCC synthesises calls to `memcpy` and
   `memset` from ordinary C — struct assignments, large local initialisers —
   even under `-fno-builtin` (Chapter 14 §14.9).
2. The WiFi link needed `__divsf3` from libgcc, and needed Espressif's ROM
   symbol table without letting it displace the kernel's own `memcpy`. The
   comment in `build.ps1` records what went wrong on the first attempt:

```powershell
    # The first attempt linked Espressif's newlib and libgcc ROM scripts too,
    # which define memcpy and sprintf by BARE ASSIGNMENT rather than PROVIDE.
    # That silently redirected the kernel's own call0 memcpy to a windowed ROM
    # routine and panicked the board on boot.
```

The fix was to rename those references inside the vendor archive with `objcopy`,
answer them in `vendor/windowed/`, and link only `esp32.rom.ld` — "whose every
entry is PROVIDE, verified".

> Nothing the kernel defines can be displaced, because nothing strong is defined
> at all.

---

**Next:** how a `.bin` file on flash becomes the first instruction of `_start`.
