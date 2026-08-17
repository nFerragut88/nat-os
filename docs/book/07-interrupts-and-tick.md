# Chapter 7 — Interrupts and the Tick

> Sources: `docs/UM-NATOS-008-m1-verification.md`
> Code: `kernel/vectors.S`, `kernel/xtensa.h`, `kernel/timer.c`, `kernel/panic.c`

---

## 7.1 What M1 had to establish

Milestone 1 establishes that nat-os can be interrupted and resume correctly: a
vector table is installed, a periodic timer interrupt is dispatched, and code
interrupted mid-execution continues with its register state intact.

The third property is the one that matters, and the report says why:

> Every scheduler is built on it, and a defect in the interrupt save/restore
> path does not announce itself — it surfaces later as corruption in unrelated
> code. M1 therefore measures it rather than assuming it.

## 7.2 Timer source: the core comparator, not a peripheral

The ESP32 offers two families of timer: the TIMG peripherals, and the core's own
CCOUNT/CCOMPARE comparators. **CCOMPARE1 was selected.**

| | TIMG peripheral | CCOMPARE1 (selected) |
|---|---|---|
| Clock gating | Required | None — inside the core |
| Interrupt matrix routing | Required | None — fixed internal interrupt |
| Prescaler / config registers | Several | One comparator register |
| Ways to be wrong about something other than interrupts | Many | Few |

The reasoning generalises well beyond timers, and it is the same argument that
puts a bit-banged SPI in front of the display (Chapter 18) and a bit-banged I²C
on the sensor bus (Chapter 22):

> For the first interrupt on a kernel with no driver model, minimising unrelated
> setup is worth more than the peripheral's extra features.

Every element of that table was later paid for by some other driver. Clock gating
cost the audio driver a full evening (Chapter 23 §23.4); matrix routing cost the
interrupt chapter four distinct failures (Chapter 22 §22.3). Choosing the source
with none of those for the *first* interrupt meant that when it did not work,
there were few candidates.

CCOMPARE1 raises **internal interrupt 15**, which is **level 3** on this core:

```c
/* CCOMPARE1 fires internal interrupt 15, which is a level-3 interrupt on this
 * core. Level 3 has a dedicated vector, so the handler is entered only for this
 * class of event and needs no EXCCAUSE decoding. */
#define XT_TIMER1_INTERRUPT   15u
#define XT_TIMER1_LEVEL        3u
```

## 7.3 Interrupt level 3, not 1

Level 1 interrupts on Xtensa are dispatched through the general exception vector
with `EXCCAUSE = 4`, requiring the handler to distinguish interrupts from
genuine exceptions. Levels 2 and above have **dedicated vectors** and are entered
only for that class of event.

Level 3 was chosen: the handler needs no `EXCCAUSE` decoding, and the exception
vectors stay free for the panic path.

## 7.4 The comparator is one-shot, and the write is the acknowledgement

CCOMPARE has no auto-reload. The handler must recompute and rewrite the next
deadline, and **that write is also the interrupt acknowledgement** — there is no
separate acknowledge step.

```c
/* Writing CCOMPAREn also acknowledges that comparator's pending interrupt —
 * there is no separate acknowledge step for these. */
static inline void xt_set_ccompare1(uint32_t v)
{
    __asm__ volatile ("wsr.ccompare1 %0; esync" :: "a"(v));
}
```

UM-NATOS-008 §2.3 analysed the failure mode of forgetting it:

> Forgetting it produces immediate re-entry rather than a missed tick, which is
> a loud failure and therefore an acceptable one.

That analysis is correct and it is incomplete, and the report adds a revision
note admitting so:

> **Revision 1.1 note.** This section is correct and incomplete. It reasons
> about the handler forgetting to rewrite the deadline, and concludes the
> failure would be loud. It does not consider a **second writer** to the same
> register, which arrived with the scheduler two milestones later and failed
> silently.

§7.9 is that defect.

## 7.5 The vector table and the handler

### Slots

Four vector slots are populated at M1. Each is 64 bytes and contains a single
jump:

```asm
    .section .vectors.level3, "ax"
    .align 4
    .global _vector_level3
_vector_level3:
    j       _handler_level3

    .section .vectors.kernel, "ax"
    .align 4
    .global _vector_kernel
_vector_kernel:
    j       _handler_panic

    .section .vectors.user, "ax"
    .align 4
    .global _vector_user
_vector_user:
    j       _handler_panic

    .section .vectors.double, "ax"
    .align 4
    .global _vector_double
_vector_double:
    j       _handler_panic
```

Three of the four go to a panic handler that did not exist in the milestone's
specification. UM-NATOS-008 §2.4 explains the addition:

> **No JTAG probe is available yet.** Without this, any unexpected fault
> produces a silent reset with no evidence. The handler runs on a dedicated
> 512-byte stack because the faulting stack may be the cause of the fault, and
> it halts rather than resetting so the output is not scrolled away by a boot
> loop.

```asm
_handler_panic:
    /* Use a known-good stack: the faulting one may be why we are here. */
    movi    a1, _panic_stack_top
    rsr.exccause a2
    rsr.epc1     a3
    rsr.ps       a4
    call0   kernel_panic
.Lpanic_hang:
    j       .Lpanic_hang

    .size _handler_panic, . - _handler_panic

    .section .bss
    .align 16
_panic_stack:
    .space 512
_panic_stack_top:
```

Chapter 12 covers what `kernel_panic` grew into: three reporting channels ordered
by decreasing reliability, and a screen drawn on a panel by a driver operating
under three suspended assumptions.

### Entry and exit

Hardware saves **nothing** on entry. It records the interrupted PC in `EPC3`, the
processor state in `EPS3`, raises `PS.INTLEVEL`, and jumps. Everything else is
the handler's responsibility.

```asm
_handler_level3:
    addi     a1, a1, -96
    s32i     a0,  a1, 0
    s32i     a2,  a1, 4
    s32i     a3,  a1, 8
    s32i     a4,  a1, 12
    s32i     a5,  a1, 16
    s32i     a6,  a1, 20
    s32i     a7,  a1, 24
    s32i     a8,  a1, 28
    s32i     a9,  a1, 32
    s32i     a10, a1, 36
    s32i     a11, a1, 40
    s32i     a12, a1, 44
    s32i     a13, a1, 48
    s32i     a14, a1, 52
    s32i     a15, a1, 56
    rsr.sar  a2
    s32i     a2,  a1, 60
    rsr.epc3 a2
    s32i     a2,  a1, 64
    rsr.eps3 a2
    s32i     a2,  a1, 68
```

At M1 this saved only `a0`, `a2`–`a11` and `SAR` — 12 words, padded to 64 bytes.
`a12`–`a15` were deliberately *not* saved in assembly, because they are
callee-saved under call0 and are preserved by the C handler itself.

The frame shown above is the M2 version: 96 bytes, 21 words, saving `a12`–`a15`
as well because "the resumed task expects *its* values, not the interrupted
task's". Chapter 8 is why. It also saves three more registers that M1 had never
heard of, and Chapter 8 §8.6 is that story.

Return is `RFI 3`, which restores PC and PS from `EPC3`/`EPS3` atomically.

## 7.6 Synchronising instructions, baked into the accessors

Several special registers require a synchronising instruction before the write
takes architectural effect:

| Register | Required after write |
|---|---|
| `VECBASE` | `ISYNC` — affects instruction fetch |
| `PS` | `RSYNC` |
| `INTENABLE`, `CCOMPARE1` | `ESYNC` |

These are built into `xtensa.h`'s accessors rather than left to call sites:

```c
/* Thin inline wrappers, no abstraction: kernel code that touches these needs to
 * see exactly which register it is hitting. ...
 *
 * Omitting those produces failures that appear several instructions later, so
 * they are baked into the accessors rather than left to call sites.
 */
```

The whole of `xtensa.h` is worth reading as an example of the style: no
abstraction, one function per register, and a comment on every non-obvious one.

```c
static inline void xt_set_vecbase(uint32_t base)
{
    __asm__ volatile ("wsr.vecbase %0; isync" :: "a"(base));
}

/* PS.INTLEVEL masks interrupts at or below its value. Setting it to 0 admits
 * every level; the other PS bits are preserved because clobbering WOE or UM
 * here would change the execution mode out from under the kernel. */
static inline void xt_set_intlevel(uint32_t level)
{
    xt_set_ps((xt_get_ps() & ~0xFu) | (level & 0xFu));
}
```

That preservation of `WOE` in `xt_set_intlevel` is the same caution as the PS
inheritance in `task_create` (Chapter 2 §2.7), and for the same reason: the ROM
leaves bits set whose meaning this kernel does not use but must not disturb.

One accessor carries a warning about how its result may be used:

```c
/* Which CPU interrupt lines are asserting RIGHT NOW, regardless of whether they
 * are enabled. A handler must mask this with INTENABLE before acting on it:
 * INTERRUPT reports the hardware's opinion, and a line can be pending for a
 * peripheral this kernel has not enabled and cannot service. */
static inline uint32_t xt_get_interrupt(void)
```

Chapter 22 §22.2 shows the consumer that made that warning necessary.

## 7.7 The timer driver

`timer.c` is 121 lines, of which roughly half are comments about two defects.
The state is minimal:

```c
/* Written by the ISR, read by the main context — volatile, and read/written
 * as a single 32-bit word so no lock is needed on this core. */
static volatile uint32_t g_ticks;
static volatile uint32_t g_last_delta;   /* cycles between the last two ticks */
static volatile uint32_t g_late;         /* re-arms that had already elapsed */

static uint32_t g_interval;              /* cycles between ticks */
static uint32_t g_next;                  /* CCOUNT value of the next tick */
```

Starting it is four lines and one deliberate ordering:

```c
void timer_start(uint32_t interval_cycles)
{
    g_interval = interval_cycles;
    g_ticks = 0;
    g_late = 0;
    g_last_delta = 0;

    g_next = xt_ccount() + interval_cycles;
    xt_set_ccompare1(g_next);

    xt_enable_interrupt(XT_TIMER1_INTERRUPT);
    xt_set_intlevel(0);          /* admit all levels; nothing is masked now */
}
```

`xt_set_intlevel(0)` last, after the comparator is armed and the source is
enabled. Reversing those would admit an interrupt before there was a deadline
for it to have missed.

## 7.8 The M1 results

Captured 2026-08-14, 10-second window:

```
  .data loaded : ok
  .bss cleared : ok
  stack top    : 0x3ffdc200
  code at      : 0x4008046c  (IRAM ok)
  vecbase      : 0x40080000  (installed)
  tick every   : 2400000 cycles
  intenable    : 0x00008000
  ps           : 0x00060720

  waiting for first tick... arrived

  tick 25  delta=2400030cy  late=0  regchecks=1556680  corrupt=0
```

| # | Exit criterion | Result |
|---|---|---|
| 1 | Tick counter advances at a stable rate | **PASS** — `delta = 2,400,030` cycles against 2,400,000 requested |
| 2 | Interrupted code resumes correctly | **PASS** — 1,556,680 checks, **0** corruptions |
| 3 | System survives without reset | **PASS** — full capture window, no panic, no reboot |

### How criterion 2 was measured

Six caller-saved registers are loaded with distinct non-zero patterns and
verified immediately afterwards, in a continuous loop. Because ticks fire
asynchronously, a large fraction of iterations are interrupted mid-sequence —
which is exactly the case a faulty save or restore would corrupt.

> Distinct patterns (`0x11111111`, `0x22222222`, …) were used so that a stray
> zero, or a neighbouring register's value, is unmistakable rather than
> plausible.

That last clause is the same reasoning as the heap's magic words (Chapter 10
§10.3) and the stack fill pattern (Chapter 8 §8.4): a canary whose corruption
produces a *plausible* value is a canary that will be believed.

### Reading the numbers

**Handler overhead is 30 cycles.** The measured interval exceeds the requested one
by a constant 30 cycles, which is the prologue cost before `CCOUNT` is sampled.
Constant rather than growing, so there is no cumulative drift.

**`late = 0`** — no deadline was ever missed.

**`intenable = 0x00008000`** — bit 15 only, confirming CCOMPARE1 is the sole
enabled source. Verified rather than assumed, which mattered later when a second
source was routed.

**`ps = 0x00060720`** — `INTLEVEL = 0`, so nothing is masked. The upper bits are
`WOE`/`CALLINC` left by the ROM; harmless under call0 — and, two milestones
later, the reason windowed code could be made to run at all.

### The CPU frequency, derived

The kernel does not know the CPU frequency, so the interval is expressed in
cycles and the real rate is derived by counting ticks against the *host's* clock
during a capture window of known duration.

25 ticks within ~10 s at 2,400,000 cycles per tick implies a CPU clock near
**80 MHz** — not the 240 MHz maximum. The bootloader leaves the core at its
default and nothing in the kernel raises it.

> This is a **measurement of the current configuration**, not a specification
> figure, and it should be re-derived rather than assumed if boot configuration
> changes. It also explains the apparent slowness of the M0 spin-loop heartbeat.

80 MHz appears throughout the rest of this book — in the display's `delay_us`,
in the watchdog's prescaler, in every cycles-to-milliseconds conversion — always
as a derived figure. `display.c` labels it as such:

```c
/* Derived in UM-NATOS-008 §5.2 from the measured tick rate. Only used for the
 * panel's reset and sleep-out delays, where being wrong by a factor of three
 * still leaves them long enough. */
#define CPU_HZ 80000000u
```

## 7.9 The second writer, and 183 milliseconds of stopped clock

*This section is UM-NATOS-008 revision 1.1, added after the tick was found
stalling.*

### The setup

§7.4 records that CCOMPARE has no auto-reload, so the handler recomputes the
deadline and that write doubles as the acknowledgement. It analyses one failure
— the handler forgetting to write — and correctly calls it loud.

The failure that actually occurred needs *two parties*, and M1 had only one.

### Two writers, one shadow

`task_yield()` ends a slice early by pulling CCOMPARE1 back to `ccount + 64`. It
writes the hardware register directly and does not tell `timer.c`, which keeps
its own `g_next` shadow of what the deadline should be.

So the handler fires 64 cycles after a yield, sees no reason to think anything
unusual happened, and executes:

```c
g_next += g_interval;
```

adding a **whole interval** to a deadline that was already a whole interval away.
Each yield pushes the tick one interval further into the future.

Measured at the moment of failure:

```
ccount     0x04672eab
ccompare1  0x05433b92     14,630,119 cycles ahead
                          = 18 tick periods = 183 ms
```

### The guard existed and faced the wrong way

The handler already had a correction:

```c
if ((int32_t)(g_next - xt_ccount()) <= 0) {   /* deadline in the PAST */
    g_next = xt_ccount() + g_interval;
    g_late++;
}
```

> That is the direction anyone reasons about: the handler was delayed, the
> deadline slipped behind, catch up rather than storm. It is right, it was
> exercised, and `g_late` counted it.
>
> Nothing guarded the other direction, because a deadline arriving *too early*
> has no obvious cause — until a second writer exists.

The fix bounds it both ways, and is in the shipped source:

```c
    int32_t ahead = (int32_t)(g_next - xt_ccount());
    if (ahead <= 0 || ahead > (int32_t)g_interval) {
        g_next = xt_ccount() + g_interval;
        g_late++;
    }
```

### Why it stayed hidden for three milestones

The rate is self-correcting on average. A yield defers the tick; the next handler
still advances `g_next` by one interval; nothing accumulates unless yields
outpace ticks.

> Shortening the display task's sleep from 8 ticks to 2 multiplied the yield rate
> until they did. That commit is where the symptom appears in a bisect, and it
> did not introduce the defect — it removed the margin that had been concealing
> it.

And the part that should worry anyone reading a performance number from this
project:

> **Nothing else showed a symptom.** Sleeps ran long, timeouts were loose, and
> frame-rate figures computed as frames-per-tick were taken against a clock that
> intermittently stopped. Only the M6 critical-section self-test noticed, because
> it is the one test that asserts something about the tick *resuming* rather
> than about work getting done.

Chapter 18 §18.7 is where that consequence lands: a framebuffer measured as
worthless, and re-measured as clearly worthwhile once this and one other defect
were fixed.

### The standing rule the defect produced — and the rule it defeated

The scheduler already had a rule, from Chapter 16's freeze:

> *A routine adjusting a scheduler deadline must only ever move it earlier.*

`task_yield()` obeys that rule exactly, and this defect happened anyway. The
report is precise about why:

> The rule constrains the **writer**. The defect was in the other party's
> **bookkeeping**: `timer.c` maintained a shadow of a register it did not
> exclusively own, and a shadow is only as good as its exclusivity. Moving the
> deadline earlier is safe for the deadline and quietly invalidates every
> assumption anyone else has cached about it.

> **A shared hardware register with a software shadow has exactly one safe
> shape: either one writer, or every writer maintains the shadow.** Two writers
> and one shadow is a defect waiting for enough traffic to surface.

### Recovered

```
[6a] critical : PASS  entered at 31, held at 31 across 2 periods, resumed at 62
frames/s      12.4 -> 16.0
```

The frame rate improved because `task_sleep(2)` had been waiting on ticks that
were not arriving.

## 7.10 The third defect in the same function

There is a further chapter to `timer_isr`, from UM-NATOS-028 §4, and it is
included here rather than in the WiFi chapter because it belongs to this file.

`g_ticks++` sat at the bottom of the handler *unconditionally*. But
`task_yield()` makes the handler run on every yield as well as on every real
deadline. So `timer_ticks()` counted **scheduling activity, not time**:

> Measured at **217 ticks per real second** where 100 is correct, and far higher
> when a task sat in an idle-yield loop.

The present handler opens by refusing to count an early entry, and the comment
is the longest in the file:

```c
void timer_isr(void)
{
    uint32_t now = xt_ccount();

    /* Not every entry here is a tick.
     *
     * task_yield() ends a slice by pulling CCOMPARE1 back to ccount+64, so this
     * handler runs on every yield as well as on every real deadline. Counting
     * entries therefore counts yields, and g_ticks++ used to sit at the bottom
     * of this function unconditionally.
     *
     * That made timer_ticks() a measure of scheduling activity rather than of
     * time, and every deadline expressed in ticks came due early in proportion
     * to how much yielding the system happened to be doing. ...
     *
     * So: advance the clock only when the deadline has actually passed. An
     * early entry falls through to the re-arm below, which restores CCOMPARE1
     * to the real deadline the yield displaced -- the yield still got its
     * context switch, because _handler_level3 calls the scheduler regardless of
     * what this function decides. */
    if ((int32_t)(now - g_next) < 0) {
        xt_set_ccompare1(g_next);
        return;
    }

    g_last_delta = now - (g_next - g_interval);

    g_next += g_interval;
    /* ... the two-sided bound from §7.9 ... */
    xt_set_ccompare1(g_next);   /* re-arm and acknowledge */
    g_ticks++;
}
```

The final clause of that comment is the elegant part. An early entry returns
without doing bookkeeping, but the *switch still happens*, because the scheduler
is called from `_handler_level3` and not from `timer_isr`. The yield gets what it
asked for; the clock does not get corrupted. Two concerns that were tangled are
separated by a single early return.

Three defects in 121 lines, all in the same function, all silent, all found by
something else breaking. The file's own summary of the second one applies to all
three:

> Nothing else showed a symptom, which is the uncomfortable part.

---

**Next:** the milestone that turned one interruptible context into several, and
the twelve build cycles it took.
