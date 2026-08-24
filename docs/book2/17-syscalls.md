# Chapter 17 — Syscalls and the Confinement Model

> Sources: `docs/UM-NATOS-016-display-syscalls.md`, `docs/UM-NATOS-017-touch.md` §8, `docs/UM-NATOS-012` §10
> Code: `kernel/vm.c`, `kernel/vm.h`, `kernel/app.h`

---

## 17.1 Twelve services, one opcode

```c
/* Syscalls. Arguments in r0..r3, result in r0. */
enum {
    VM_SYS_EXIT  = 0,    /* r0 = status                                */
    VM_SYS_PUTC  = 1,    /* r0 = character                             */
    VM_SYS_PUTS  = 2,    /* r0 = offset of a NUL-terminated string     */
    VM_SYS_PUTD  = 3,    /* r0 = value, printed as unsigned decimal    */
    VM_SYS_TICKS = 4,    /* r0 <- timer_ticks()                        */

    VM_SYS_FILL  = 5,    /* r0=x r1=y r2=w r3=h r4=colour              */
    VM_SYS_TEXT  = 6,    /* r0=str offset r1=x r2=y r3=fg r4=bg r5=scale */
    VM_SYS_DIMS  = 7,    /* r0 <- (width << 16) | height               */
    VM_SYS_TOUCH = 8,    /* r0 <- touched, r1 <- x, r2 <- y            */
    VM_SYS_BLIT  = 9,    /* r0=offset r1=x r2=y r3=w r4=h              */
    VM_SYS_SEND  = 10,
    VM_SYS_RECV  = 11
};
```

> **Since written: fourteen, and the list is now closed.** Two were added.
>
> `VM_SYS_DEVICE = 12` reaches a table rather than a peripheral, so a new
> device is an entry in `device.h` rather than a fifteenth service and a
> fifteenth case here. It is **the last hand-written syscall for hardware**
> (UM-NATOS-031).
>
> `VM_SYS_EVENT = 13` came afterwards anyway, and deliberately: it registers a
> handler the *kernel* may call, which is about the execution model rather than
> about a peripheral, and no device table can express it. Every service in the
> list above runs one way — the program asks, the kernel answers. This is the
> first that runs the other way.
>
> The comment above the enum was also wrong and is fixed in the tree: arguments
> reach `r5`, not `r3` — `text` has always read six.
>
> Appendix B §B.4 and §B.5 carry the current definitions.

They fall into three groups by *what crosses the boundary*, and each group needed
a different kind of check:

| Syscall | Added in | Crosses the boundary |
|---|---|---|
| `EXIT` `PUTC` `PUTS` `PUTD` `TICKS` | M4 | Scalars, and one string read out of the arena |
| `FILL` `TEXT` `DIMS` | Chapter 18's syscalls | Coordinates, clipped to a viewport |
| `TOUCH` | Chapter 19 | Coordinates, *inward*, filtered by viewport |
| `BLIT` | | A pointer **and a length**, both program-supplied |
| `SEND` `RECV` | Chapter 16 | A buffer, copied through a kernel mailbox |

## 17.2 Viewports

Each application slot owns a horizontal strip. The geometry lives in `app.h`
because the close button depends on the exact boundary:

```c
#define APP_VIEW_Y0     224u
#define APP_VIEW_PITCH   16u
#define APP_VIEW_H       14u
#define APP_NAME_W       44u
#define APP_CLOSE_W      16u
#define APP_CHROME_W    (APP_NAME_W + APP_CLOSE_W)
#define APP_VIEW_W      (DISP_W - APP_CHROME_W)
```

Above the strips the kernel keeps its status area; below them the colour strip.
Neither is reachable from an application.

### Coordinates are relative, and position is never disclosed

```c
    /* Display. Coordinates are VIEWPORT-relative and clipped to it, so an
     * application cannot name a pixel outside its own region — the same
     * property arenas give it for memory. There is no syscall to move or
     * resize a viewport; that is the kernel's to assign. */
```

`SYS DIMS` returns **size only**:

```c
    case VM_SYS_DIMS:
        /* Size only. A program is told how much canvas it has, never where on
         * the panel it sits — position is not its business and knowing it would
         * only tempt a producer into absolute coordinates. */
        vm->reg[0] = (vm->vw << 16) | (vm->vh & 0xFFFFu);
        return 0;
```

> A program is told how much canvas it has and nothing about where that canvas
> sits, which removes the temptation for a producer to compute absolute
> coordinates and removes the possibility of it naming one.

The report states the parallel with memory explicitly:

> This is deliberately the arena model applied to a second resource.
> UM-NATOS-013 §5.2 established that an application cannot reach another's
> memory because an address outside its arena is *unrepresentable*. The same
> holds for pixels: a coordinate outside the viewport is clipped before it can
> exist.

## 17.3 Clipping belongs in the VM, not the driver

`display_fill_rect()` clips to the **panel**, which is the wrong boundary:

> A program allowed to paint anywhere on the panel could erase the kernel's
> status area, the colour strip, and every other application's output.

So `vp_fill()` clips to the **viewport**, in the offset domain, for exactly the
reason `arena_contains()` does:

```c
static void vp_fill(vm_t *vm, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint16_t colour)
{
    if (x >= vm->vw || y >= vm->vh) {
        return;                         /* origin outside — nothing to draw */
    }
    if (w > vm->vw - x) { w = vm->vw - x; }
    if (h > vm->vh - y) { h = vm->vh - y; }
    if (w == 0u || h == 0u) {
        return;
    }
    /* Re-derive the absolute rectangle and confirm it lies wholly inside the
     * viewport. This duplicates the clipping above on purpose: the check that
     * matters is on what actually reaches the panel, not on what the clipping
     * intended. */
    uint32_t ax = vm->vx + x, ay = vm->vy + y;
    if (ax < vm->vx || ay < vm->vy ||
        ax + w > vm->vx + vm->vw || ay + h > vm->vy + vm->vh) {
        g_vp_escapes++;
        return;                         /* refuse it as well as count it */
    }
    g_vp_calls++;
    if (ay + h > g_vp_max_y) {
        g_vp_max_y = ay + h;
    }

    if (!display_try_lock()) {
        g_draw_skipped++;
        return;
    }
    display_fill_rect(ax, ay, w, h, colour);
    display_unlock();
}
```

The offset-domain argument, from the file's own comment:

```c
 * Arithmetic is in the offset domain for the same reason arena_contains() is
 * (UM-NATOS-010 §5.2). Coordinates arrive as uint32_t, so a program passing a
 * "negative" value hands over something near 0xFFFFFFFF; comparing it against
 * the viewport width rejects it, whereas computing x + w first would wrap and
 * admit it.
```

`app_gfx_rogue.vasm` passes `60000, 60000` for exactly this case, and the
program's own comment says so: *"an origin far outside any viewport… That is the
case offset-domain clipping exists for."*

### The deliberate duplication

The re-check after clipping is not redundancy by accident:

> `vp_fill()` re-derives the absolute rectangle *after* clipping and re-checks it
> against the viewport before handing it to the driver, counting and refusing any
> that would fall outside. The duplication is deliberate: **what matters is what
> actually reaches the panel, not what the clipping intended.** If the two ever
> disagree, `escapes` stops being zero and says so.

## 17.4 Containment, measured

```
vp calls=136  escapes=0  maxy=250/320
```

- **`escapes = 0`** — no rectangle reaching the driver ever fell outside the
  viewport it came from, across 136 fills.
- **`maxy = 250`** — the lowest row any application has painted. Slot 2's
  viewport is `168 + 2×28 = 224`, height 26, bottom edge **exactly 250**. "Not
  one row below it, and nowhere near the panel's 320."

*(Those are the M5-era strip constants; the geometry moved in Chapter 24 §24.7,
and the counters moved with it because they are computed from `vm->vy` and
`vm->vh` rather than from literals.)*

The counters are declared with their rationale:

```c
/* Containment, measured rather than observed.
 *
 * Whether a hostile program's drawing stays inside its viewport is visible on
 * the panel, but "it looks right" is a judgement and this is the claim the
 * whole design turns on. These counters make it a number: every rectangle that
 * actually reaches the driver is re-checked against the viewport it came from,
 * and g_vp_escapes counts any that would fall outside. It must stay at zero no
 * matter what an application asks for. */
static uint32_t g_vp_escapes;
static uint32_t g_vp_max_y;     /* highest absolute row any app has painted */
static uint32_t g_vp_calls;
```

### Why a counter and not a photograph

This is the methodological point of the whole chapter:

> Containment was visible on the panel, and the report back was *"I think it
> looks good."*
>
> That is a judgement, and this is the claim the entire syscall design rests on.
> Given [the freeze that was missed by looking at the panel], a hedged glance was
> not the evidence it deserved. The counter is also the more durable form: **it
> keeps checking after everyone has stopped looking, and it distinguishes
> "confined" from "confined so far".**

And the honest limit of what the counter proves:

> **The escape counter is not a proof of the clipping logic**, only evidence that
> it and the re-check agree on everything tried so far. A case both get wrong
> identically would pass.

## 17.5 The first pointer to cross the boundary

`SYS TEXT` takes an arena offset, making it the first syscall to carry a pointer:

```c
#define VM_STR_MAX 48

/* Copies a NUL-terminated string out of the arena into kernel memory, one
 * bounds-checked byte at a time. The copy is not paranoia: display_text() takes
 * a kernel pointer, and handing it an arena address would let a program's own
 * writes change the string mid-render. A string that runs to the end of the
 * arena without a terminator faults rather than reading past it. */
static int copy_string(vm_t *vm, uint32_t off, char *dst, uint32_t max)
{
    for (uint32_t n = 0; n < max - 1u; n++) {
        int ok = 0;
        uint32_t ch = load_u8(vm, off + n, &ok);
        if (!ok) {
            return 0;                   /* load_u8 recorded the fault */
        }
        dst[n] = (char)ch;
        if (ch == 0u) {
            return 1;
        }
    }
    dst[max - 1u] = 0;
    return 1;                           /* truncated, not a fault */
}
```

Two properties: every byte goes through `load_u8`, which bounds-checks; and
truncation at 48 characters is *not* a fault, because a long string is a legal
thing to ask for.

`SYS PUTS` does the same walk directly, with a different termination condition,
and the comment names the class of bug:

```c
    case VM_SYS_PUTS: {
        /* Walked one byte at a time, each bounds-checked. A string that runs
         * to the end of the arena without a terminator is a fault, not a read
         * past the end — this is the classic way a "harmless" print primitive
         * becomes an information leak. */
        uint32_t off = vm->reg[0];
        for (uint32_t n = 0; n < vm->size; n++) {
            int ok = 0;
            uint32_t ch = load_u8(vm, off + n, &ok);
            if (!ok) {
                return 1;
            }
            if (ch == 0u) {
                return 0;
            }
            uart_putc((char)ch);
        }
        fault(vm, VM_FAULT_STRING, off);
        return 1;
    }
```

The loop bound is `vm->size` — a string cannot be longer than the arena — so
`VM_FAULT_STRING` is reachable and means precisely "unterminated within the
arena".

### Text overrun handling

```c
static void vp_text(vm_t *vm, uint32_t x, uint32_t y, const char *s,
                    uint16_t fg, uint16_t bg, uint32_t scale)
{
    if (scale == 0u || scale > 4u) {
        scale = 1u;
    }
    if (x >= vm->vw || y >= vm->vh) {
        return;
    }
    /* Reject rather than clip vertically: half a line of text is worse than
     * none, and the glyph renderer has no partial-row mode. */
    if (8u * scale > vm->vh - y) {
        return;
    }

    /* Truncate to what fits horizontally, so a long string cannot run out of
     * the viewport and across a neighbour's. */
    char buf[VM_STR_MAX];
    uint32_t room = (vm->vw - x) / (6u * scale);
    /* ... */
}
```

Horizontal overrun truncates; vertical overrun refuses. Two different answers for
two different reasons, both stated.

## 17.6 `SYS BLIT`: the first program-controlled length

```c
    /* Blit an image the application holds in its own arena.
     *   r0 = arena offset of RGB565 pixels, row-major, no padding
     *   r1 = x, r2 = y   (viewport-relative)
     *   r3 = w, r4 = h
     * The first syscall to take a pointer AND a length from the program, so it
     * is the first where the size itself is attacker-controlled. */
    VM_SYS_BLIT  = 9,
```

```c
    case VM_SYS_BLIT: {
        uint32_t off = vm->reg[0];
        uint32_t x   = vm->reg[1], y = vm->reg[2];
        uint32_t w   = vm->reg[3], h = vm->reg[4];

        /* Dimensions are bounded BEFORE they are multiplied. w*h*2 on
         * unchecked 32-bit values overflows readily — 65536x65536 wraps to
         * zero, which would pass a byte-length check and then blit whatever
         * followed. This is the first syscall where the length is supplied by
         * the program rather than implied by the operation. */
        if (w == 0u || h == 0u || w > DISP_W || h > DISP_H) {
            vm->reg[0] = 0;
            return 0;
        }

        /* Pixels are 16-bit; an odd offset would mean unaligned loads. Faults
         * rather than silently reading a shifted image. */
        if (off & 1u) {
            fault(vm, VM_FAULT_ALIGN, off);
            return 1;
        }

        uint32_t bytes = w * h * 2u;            /* <= 240*320*2, cannot wrap */
        if (!vm_in_bounds(vm, off, bytes)) {
            fault(vm, VM_FAULT_BOUNDS, off);
            return 1;
        }

        /* Clip to the viewport before drawing, exactly as vp_fill does. The
         * source stride stays the ORIGINAL width so a clipped blit still walks
         * the caller's rows correctly. */
        if (x >= vm->vw || y >= vm->vh) {
            vm->reg[0] = 0;
            return 0;
        }
        uint32_t cw = (w > vm->vw - x) ? (vm->vw - x) : w;
        uint32_t ch = (h > vm->vh - y) ? (vm->vh - y) : h;

        /* A skipped blit is not a fault: the program asked for something legal
         * and the panel was busy. It returns success with nothing drawn, the
         * same contract as a blit clipped entirely outside the viewport above. */
        if (!display_try_lock()) {
            g_draw_skipped++;
            vm->reg[0] = 0;
            return 0;
        }
        display_blit(vm->vx + x, vm->vy + y, cw, ch,
                     (const uint16_t *)(vm->base + off), w);
        display_unlock();

        g_blits++;
        vm->reg[0] = 1;
        vm->yield_now = 1;              /* milliseconds, not instructions */
        return 0;
    }
```

The ordering is the interesting part and the report states the rule:

> The dimensions are therefore each bounded against the panel *before* they are
> multiplied, after which the product provably cannot wrap.

`65536 × 65536 × 2` wraps to zero on 32 bits. A zero-byte length would satisfy a
naive bounds check and then copy whatever followed the buffer. Bounding `w ≤ 240`
and `h ≤ 320` first makes `w * h * 2 ≤ 153,600`, which cannot wrap — and the
comment says so in one clause: `/* <= 240*320*2, cannot wrap */`.

Note also the source stride: `display_blit(..., w)` passes the **original** width
as the stride while drawing `cw` columns, so a clipped blit still walks the
caller's rows correctly. `display.h` documents that parameter for this reason:

```c
/* Copies a caller-supplied RGB565 image to the panel. `src_stride` is the
 * source row pitch in PIXELS, so a clipped blit can walk the original rows
 * without the caller having to repack. */
```

## 17.7 Input, and the fourth confined resource

```c
    case VM_SYS_TOUCH: {
        touch_state_t t;
        touch_latest(&t);

        /* Absolute panel coordinates translated into this viewport, in the
         * offset domain so a touch above or left of the viewport underflows to
         * a huge value and is rejected rather than wrapping into range. */
        uint32_t dx = t.x - vm->vx;
        uint32_t dy = t.y - vm->vy;
        int inside  = t.down && (t.x >= vm->vx) && (t.y >= vm->vy) &&
                      (dx < vm->vw) && (dy < vm->vh);

        if (t.down && !inside) {
            g_touch_withheld++;
        }
        if (inside) {
            g_touch_given++;
        }

        vm->reg[0] = inside ? 1u : 0u;
        vm->reg[1] = inside ? dx : 0u;
        vm->reg[2] = inside ? dy : 0u;
        return 0;
    }
```

The confinement is stronger than refusal:

> Coordinates are **viewport-relative**, and a touch anywhere else on the panel
> is reported to the asking application as **no touch at all**.
>
> That is stronger than refusing to answer. An application is never told where
> its viewport sits — `SYS DIMS` returns size only — so it cannot reconstruct an
> absolute position even from a touch it is permitted to see, and it has no way
> to learn that a touch happened elsewhere.

### The four confined resources

| Resource | Confined by | Outside its allocation |
|---|---|---|
| Memory | Arena bounds check | Unrepresentable — an address outside the arena cannot be formed |
| Pixels | Viewport clipping | Clipped before it exists |
| Input | Viewport test on delivery | Reported as no touch |
| Messages | Destination is an application id | No argument can denote another's memory |

Four resources, one property. Every one of them was designed by asking "what
would an application have to be able to *say* in order to misbehave?" and then
removing the vocabulary.

### Measured

```
touch g/w = 81/109211        last = ...->191,174
```

> **81 touches delivered** to the paint application and **109,211 withheld** for
> landing outside its strip, with the last accepted touch at y=174 — inside it.
>
> Both halves matter. Delivery alone would not show confinement, and withholding
> alone would be indistinguishable from a broken syscall. The withheld count is
> large because the application polls continuously: it asked for the pointer tens
> of thousands of times while a finger was demonstrably on the glass elsewhere,
> and was told there was none every time.

### A snapshot, not the bus

```c
    /* Pointer input, viewport-relative like everything else an application
     * sees. Returns r0 = touched, r1 = x, r2 = y. A touch outside this
     * application's viewport reports as no touch at all: an application must
     * not be able to observe input directed at its neighbours, any more than it
     * can read their memory or draw on their pixels. */
```

> The syscall reads a state published by the polling task rather than driving the
> controller itself. Doing its own SPI would cost milliseconds per call and
> contend with the touch task for the bus.

That decision is why `TOUCH` does not end the time slice — see §17.8.

## 17.8 The quantum distinction

`vm_run()`'s quantum bounds **instructions**. A display fill is one instruction
and milliseconds of SPI:

```c
    /* Set by a syscall whose cost is measured in milliseconds rather than
     * instructions. The quantum bounds INSTRUCTIONS; a display fill is one
     * instruction and ~31 ms of bit-banged SPI, so without this a 2,000
     * instruction budget can be minutes of drawing inside a single vm_run()
     * and the preemption guarantee is worthless. */
    int      yield_now;
```

The dispatch loop honours it:

```c
        case VM_OP_SYS:
            if (do_syscall(vm, imm)) {
                vm->executed++;
                return vm->fault == VM_FAULT_NONE ? VM_RUN_HALTED : VM_RUN_FAULTED;
            }
            /* An expensive syscall ends the slice regardless of how much of the
             * instruction budget is left, so wall-clock time per vm_run() stays
             * bounded by roughly one display operation instead of by the
             * quantum. Without this the host task disappears for minutes and
             * every other application starves. */
            if (vm->yield_now) {
                vm->yield_now = 0;
                vm->pc = next;
                vm->executed++;
                return VM_RUN_QUANTUM;
            }
            break;
```

`FILL`, `TEXT` and `BLIT` set it. `TOUCH`, `SEND` and `RECV` do not.

> The rule is the cost of the work rather than the fact of being a syscall. A
> fill is one instruction and milliseconds of SPI, so without ending the slice a
> 2,000-instruction quantum becomes minutes of drawing. `TOUCH` reads a snapshot
> the polling task already published and costs nothing, so charging it a slice
> would only make an application slower for asking.

And the generalisation, which is the transferable part:

> This is a general hazard, not a display one: **any future syscall whose cost is
> measured in time rather than instructions needs the same treatment.**

## 17.9 The freeze, and how it survived three commits

*This is the second half of UM-NATOS-016, and it belongs here because it was
caused by the display work and hidden by how the display work was checked.*

### The symptom

Boot completed. Every task entered. The shell printed its banner. The reporter
ran the critical-section test and printed `PASS` at tick 8. Then nothing, ever
again — no output, no scheduling, a frozen panel retaining its last frame.

```
before: 0 report lines in 35 s, ticks frozen at 8
after:  16 report lines, t=3246, all tasks healthy
```

### The cause

`task_yield()` set the comparator unconditionally:

```c
xt_set_ccompare1(xt_ccount() + 64u);
```

The display task's frame delay was:

```c
uint32_t until = timer_ticks() + 25u;
while (timer_ticks() < until) { task_yield(); }
```

> `timer_ticks()` is a single load, so that loop runs in far fewer than 64
> cycles. **Every pass pushed the comparator deadline further ahead of `CCOUNT`,
> so it was never reached and the timer interrupt stopped firing.**
>
> That is not a slowdown. The tick drives every context switch in this kernel, so
> when it stops nothing else can run — and the task doing the yielding spins
> forever waiting for a clock it is itself preventing. A yield loop, the most
> innocuous-looking construct in the system, starved the mechanism that makes
> yielding mean anything.

The fix, still in `task.c`:

```c
    uint32_t soon = xt_ccount() + 64u;
    if ((int32_t)(soon - xt_get_ccompare1()) < 0) {
        xt_set_ccompare1(soon);
    }
```

> A yield can bring preemption forward. It can never defer it. The signed
> comparison handles `CCOUNT` wrapping.

### How it survived three commits

This is the part worth keeping.

> The display driver was verified by **looking at the panel** and asking whether
> it looked right, with the serial capture filtered down to boot-time lines.
> Sustained operation — the check applied to every previous milestone, and the
> one that produced every number in reports 008 through 015 — was not run. On
> that basis three commits were reported as sound, including one that said in as
> many words "regression: all self-tests still pass".

And the instrument that existed, and was not read:

> The status task draws a marker block that advances every frame, and
> UM-NATOS-015 §5 states its purpose explicitly: *"a display whose numbers all
> look plausible but never change is otherwise indistinguishable from a working
> one."* The instrument was built, documented, and then not read. **It was frozen
> the entire time.**

And the worst part:

> the question asked to confirm the driver — whether the colour bars were in the
> right order — is one a **frozen screen answers identically to a live one**. A
> static image was treated as evidence of a running system.

Bisection then briefly implicated the display syscall work, which was innocent:
reverting to the previously "known-good" commit reproduced the freeze
identically, which is what showed the defect predated it.

The summary sentence:

> The common thread is not carelessness about any one fact. It is reporting on
> whatever evidence was nearest rather than on the evidence that would settle the
> question.

### The rule

> **A yield must never defer the clock it depends on.**
>
> More generally: any routine that adjusts a deadline the scheduler depends on
> must only ever move it earlier. Deferring it, in a loop, disables preemption
> entirely — and does so silently, since the symptom appears wherever the system
> happened to stop rather than at the line responsible.

And the observation that ties it to Chapter 8:

> `_handler_level3`'s `PS.EXCM` rule is the other standing rule of this kind.
> Both share a shape: **a single unconditional register write, correct in
> isolation, catastrophic under repetition.**

Chapter 7 §7.9 is the third member of that family, from the other side: a write
that was correct for its own purpose and invalidated somebody else's cached
shadow of the same register.

## 17.10 Metrics

| Quantity | Value |
|---|---|
| Syscalls | 12 |
| Opcodes spent on them | 1 |
| Application viewport | 240×26 at M5; 180×14 now |
| Fills audited | 136 |
| Viewport escapes | **0** |
| Lowest row painted | 250, of a 320-row panel |
| String buffer | 48 B, per-byte bounds-checked |
| Touches delivered / withheld | 81 / 109,211 |
| Confinement failures | 0 |
| Commits the freeze survived | 3 |

## 17.11 What this does not establish

- **No pixel-level drawing.** Rectangles, text and bitmaps only; no lines or
  circles.
- **No viewport negotiation.** Sizes are fixed by slot; an application cannot
  request more, and there is no way to grant a full-screen application anything.
- **No read-back.** A program cannot see what is on screen, including its own
  output.
- **No per-application draw accounting.** A program that draws constantly costs
  everyone's frame rate, and nothing measures or limits that per program.
- **The escape counter is not a proof of the clipping logic.**
- **No syscall for anything else.** No ADC, no I²C, no audio, no keypress, no
  persistence. Chapter 31.
- **Syscall validation is per-call, not systematic.** Nothing enforces that a
  future syscall will check its arguments, and there is no shared harness that
  would catch an unchecked length in a new service.

---

**Part III ends here.** The isolation claim is complete: an application is
confined in memory, in pixels, in input and in messaging, and every one of those
confinements has a counter attached that has been read on hardware.

**Part IV** is the hardware underneath it.
