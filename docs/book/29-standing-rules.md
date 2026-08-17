# Chapter 29 — The Standing Rules

> Sources: `docs/README.md`, and the report section that produced each rule
> Code: the file each rule now lives in

---

## 29.1 What a standing rule is here

`docs/README.md` carries twelve rules in block quotes after the status table.
Every one was produced by a specific defect, every one names the report section
that produced it, and every one is stated in a form that constrains *future*
code rather than describing past code.

They are reproduced here with their origin, the code they now live in, and — where
the project has since violated one — a note saying so.

---

## 29.2 The kernel rules

### Rule 1 — Clear `PS.EXCM` before calling C from an interrupt handler

> Hardware sets `EXCM` on interrupt entry, and while it is set the Xtensa
> zero-overhead loop-back is disabled: a hardware loop body executes once and
> falls through to whatever sits at `LEND`. GCC emits these loops for ordinary
> counted C loops, so this silently degrades **every C function reachable from a
> handler** — drivers and the VM interpreter included, not just the scheduler
> where it was found. `_handler_level3` now clears it; any future handler must do
> the same.

**Origin:** Chapter 8 §8.7. Twelve build cycles, three wrong hypotheses, five
instructions in the fix.

**Lives in:** `kernel/vectors.S`, with a 17-line comment.

**Scope note:** UM-NATOS-009 §9 records that whether anything *else* silently
depended on `EXCM` being set has never been examined systematically. Still open —
Chapter 30.

**Reappeared as:** the `StoreProhibited` in Chapter 27 §27.6, where a fault inside
a window-overflow handler running with `EXCM` set vectored to the double-exception
handler while `EPC1` still held the *original* faulting instruction. "Hours were
spent disassembling the wrong instruction."

---

### Rule 2 — A shared hardware register with a software shadow has one safe shape

> either one writer, or every writer maintains the shadow.

> `timer.c` kept `g_next` as its idea of the comparator deadline while
> `task_yield()` wrote CCOMPARE1 directly. The handler then added a whole interval
> to a deadline that was only 64 cycles old, so every yield pushed the tick
> further out — 18 tick periods, 183 ms, before anything noticed.

**Origin:** Chapter 7 §7.9.

**The sharp part:** the rule below it (Rule 3) *was obeyed exactly* and did not
prevent this.

> The rule constrains the **writer**. The defect was in the other party's
> **bookkeeping**, and a shadow is only as good as its exclusivity. Moving the
> deadline earlier is safe for the deadline and quietly invalidates every
> assumption anyone else has cached about it.

**Applied prospectively in:** `store.c`, where `calib_persist()` and
`store_count_boot()` both live in the file that owns the record —

```c
/* Declared in calib.h. Writing the record here keeps every write to it in one
 * file, which is what makes "the record is only changed in store.c" a property
 * rather than a habit. */
```

---

### Rule 3 — A yield must never defer the clock it depends on

> `task_yield()` originally wrote `CCOMPARE1 = ccount + 64` unconditionally, so
> any loop yielding faster than 64 cycles pushed the deadline ahead of `CCOUNT`
> forever, the timer interrupt stopped firing, and the whole kernel froze — the
> tick is what drives every context switch. Any routine adjusting a scheduler
> deadline must only ever move it **earlier**.

**Origin:** Chapter 17 §17.9. Survived three commits and two "verified on
hardware" claims.

**Lives in:** `kernel/task.c`:

```c
    uint32_t soon = xt_ccount() + 64u;
    if ((int32_t)(soon - xt_get_ccompare1()) < 0) {
        xt_set_ccompare1(soon);
    }
```

**The general form**, from the report:

> `_handler_level3`'s `PS.EXCM` rule is the other standing rule of this kind.
> Both share a shape: **a single unconditional register write, correct in
> isolation, catastrophic under repetition.**

Rule 2 is the third member of that family, from the bookkeeping side.

---

### Rule 4 — Lock contention cost is the NUMBER of blocking events, not the time held

> Each one costs a scheduling round-trip whether the lock was held for 24 ms or
> 24 µs: the blocked task is descheduled and must be selected again. Measured
> here at 63 ms per contention against a 24 ms hold, with the lock free 88% of
> the time while two tasks sat blocked on it. Narrowing the hold by 25% changed
> the outcome by nothing. The levers are batching (fewer takes) or not blocking
> at all (`try_lock`) — shortening holds, the intuitive move, does nothing.

**Origin:** Chapter 11 §11.9.

**Reached for twice:** batching gave the raycaster a **194×** improvement;
`try_lock` took the frame rate from 3.0 to 9.9 fps.

**Applied by an unrelated file:** the launcher draws each icon row as one
rectangle per *run* of set pixels rather than one per pixel — "not
micro-optimisation: every primitive takes the draw lock".

**Lives in:** `kernel/display.h`, in both the batching interface and the accessor
names.

---

## 29.3 The verification rules

### Rule 5 — When an instrument reports its own reading invalid, that outranks every value printed beside it

> Three times across two sessions a frozen marker, a sample counter stuck at an
> identical value, and a boot banner in a serial capture each said "this
> measurement is not what you think", and the plausible-looking numbers next to
> them were believed anyway. Two conclusions were published and later retracted
> as a result. **Latch the quantity so timing cannot lie about it, then feed the
> system a controlled input rather than interpreting an uncontrolled one.**

**Origin:** Chapter 19 §19.5. Four instances by the end of the book.

**Lives in:** the latching in `touch.c` and `calib.c`, and the controlled-input
probes in `intr.c`, `adc.c` and `flash.c`.

---

### Rule 6 — A negative result is only informative if the experiment demonstrably ran

> Two flash hypotheses were recorded as tested against a board that had never
> been reflashed. Every hypothesis returned bit-identical output, which was read
> as "none of these are the cause" when it meant "no experiment has run yet". The
> signature to watch for is **a run of results that do not vary when the input
> does** — suspect the harness before the theory, and verify the change reached
> the target rather than inferring it from the absence of an error.

**Origin:** Chapter 20 §20.7.

**Mitigation:** the flash step always shows its `Hash of data verified.` line, and
that line is checked before any capture is interpreted.

**Related:** Chapter 19 §19.4's capture harness, which produced exactly 150
samples three runs running; and Chapter 12 §12.9's scripted edit, the fourth in
the project to fail silently.

---

### Rule 7 — A startup artefact is not evidence the thing it introduces works

> The shell was signed off because its banner printed; the banner proves a task
> was created and the TRANSMIT path works, and says nothing about receive. The
> receive path was one byte behind for the shell's entire existence, so pressing
> Enter did nothing until the next keystroke — and every automated test drove it
> with CR **and** LF, the one input shape that hides it. Stack guards and the
> panic handler went unexercised for the same reason: each was confirmed to EXIST
> rather than observed to WORK. **Trigger the mechanism on purpose, or treat it
> as untested.**

**Origin:** Chapter 12 §12.5.

**Lives in:** the `hang`, `fault` and `smash` commands, and the README's
one-sentence defence of them:

> The last three exist on purpose. A recovery path that has never been observed
> to fire is confidence without evidence.

**Arguably the most broadly applicable rule in the book.**

---

### Rule 8 — Never infer direction from the endpoint of a gesture

> The first and last samples of a drag are its two least trustworthy, because
> both sit at a contact transition where the panel is not bridged and the ADC
> reads its rail. A rail reading is near the top of the range, so a drag ending
> anywhere "ends near its maximum" and EVERY axis appears to increase — the test
> can only return one answer. The touch X axis was backwards for three months
> behind exactly that. **Calibrate from labelled points, each a known position
> paired with a reading, and let direction fall out of comparing labels.**

**Origin:** Chapter 19 §19.8.

**And then the follow-up**, which is why Chapter 19 has two sections on
calibration: labelled points beat endpoints, and the four *corners* were still
the wrong four labelled points, because a finger cannot reach the extreme corner
of a bezelled panel. `calib.c`'s four inset targets are the correction.

**Lives in:** `kernel/touch.c`'s recorded corner table and `kernel/calib.c`.

---

### Rule 9 — A cross-check only tests what its samples can distinguish

> The SD pin map was verified against four IO_MUX entries already in the tree.
> All four confirmed the indexing, and all four sat below the anomaly, so every
> one of them agreed with the wrong answer: GPIO23 was read from the UART's
> receive pad. The check was real, it was performed, and **it could not have
> failed. Samples must STRADDLE the thing being verified, not merely agree with
> it.**

**Origin:** Chapter 21 §21.4.

**Applied correctly in:** the VM's 35-case bounds cross-check (Chapter 14 §14.5),
whose cases include "the exact end, one past the end, and lengths chosen to wrap
the address space"; and the interrupt matrix's two-point derivation check, using
sources 14 and 34 on either side of source 22.

**Sibling rule:** *verifying an instrument on the one case that cannot fail is not
verification* (Chapter 23 §23.4) — the same shape with one sample instead of four.

---

## 29.4 The engineering-judgement rules

### Rule 10 — A correct diagnosis does not license a fix of arbitrary scope

> Chrome drawn over a view that repaints every pixel every frame will strobe;
> that diagnosis was reached twice and was right both times. The first fix
> reserved rows for it, which moved the region boundary, the application strips,
> the colour strip and the grid — four files of constants — and produced a screen
> that looked wrong for reasons never found, while every measurement insisted the
> renderer was correct. The second wrote 324 pixels into a buffer that was
> already being sent. **Prefer the fix whose blast radius matches the defect.**

**Origin:** Chapter 24 §24.8.

**Follow-up:** Chapter 24 §24.9 later found the cause and it was not the layout —
which makes this rule's *illustration* partly wrong while leaving the rule intact.
Both are recorded.

---

### Rule 11 — Tolerating a defect is not fixing it

> The note pad's keys are 80 px because the touch mapping reads about 24 px low
> on X; a 24 px key was destroyed by that error and an 80 px key absorbs it. The
> app works, the fault is untouched, and every future element finer than 80 px
> will meet it again. **Record which one you did, at the place a reader would
> otherwise assume the generous version.**

**Origin:** Chapter 26 §26.3.

**The payoff, stated in the report itself:**

> Because the keypad was never called a fix, the fault stayed on the record as
> unresolved, and calibrating it properly stayed on the list instead of quietly
> becoming the way things are. **A workaround that is honestly labelled is a
> debt; one that is called a solution is a defect with good manners.**

The fault *was* subsequently fixed, and the keypad stayed — now for a stated new
reason, "because a decision whose original justification has expired is one
nobody re-examines".

---

### Rule 12 — Reverting two changes together destroys the information about which one mattered

> A relayout and a camera fix were reverted in one commit; the screen looked right
> afterwards, which appeared to convict the layout. Re-applying the geometry
> alone, later, showed it was innocent — the camera had broken concurrently and
> "blank screen" was a correct rendering of the inside of a wall. **If a revert
> must bundle, the bundle is a hypothesis to test later, not a conclusion.**

**Origin:** Chapter 24 §24.9.

The information needed to acquit the layout "had been destroyed by the same
action that produced the evidence".

---

## 29.5 Rules the source carries that the README does not

Several rules exist only as comments, and are worth promoting here.

### Choose a sentinel that cannot be a legal value

- `BLK_FREE` / `BLK_USED` — "implausible as data, and distinct from each other"
- `STACK_GUARD 0x57ACC0DE` and `STACK_FILL 0xEEEEEEEE`
- `MUTEX_FREE = -2`, not −1, because `task_current()` returns −1 before the
  scheduler starts
- `R1 = 0xFF` from an SD card — "bit 7 of a real R1 is always zero, so 'no
  answer' cannot be confused with any legal response"
- The interrupt-integrity patterns `0x11111111`, `0x22222222`, … — "so that a
  stray zero, or a neighbouring register's value, is unmistakable rather than
  plausible"

### A formula whose failure mode is a plausible extreme is worse than one that fails to an obvious value

From `touch.c`, where `z = z1 + 4095 - z2` turns a dead bus into maximum pressure.
Reappeared in `i2c.c`, where an input buffer left off makes every address appear
to ACK.

> because the first requires interpretation and the second announces itself.

### Fetch a constant; do not recall it

Three register-layout errors in one day (Chapter 22). The fix in every case was
to read the vendor header.

> Probing is the right tool when the question is *what does this hardware do*. It
> is the wrong tool when the question is *what is this constant*, and those are
> easy to confuse when both present as "it does not work".

And the corollary, from `window.S`:

> A wrong register number here does not fault — it silently reloads a caller's
> frame with the wrong values, which is the least debuggable failure this project
> could construct.

### Delete the second mechanism rather than debug it

- Two ways for a task to come into existence → one (Chapter 8 §8.2)
- A separate `switch_to()` → the tick path, "exercised constantly, rather than a
  second one exercised rarely" (Chapter 8 §8.3)
- Three arena release paths → one `retire()` (Chapter 16 §16.3)
- A second on-panel command set → `shell_run_line()` into the same `execute()`
  (Chapter 26 §26.9)

### Describe a loop by its bound, not by the bound's value on the day

> Written as "all eight tasks" when `TASK_MAX` was 8. It is now 12, and the check
> is written against `TASK_MAX` rather than a literal, so it followed. The prose
> did not.

### An optimisation with no measured gain has no claim on the tree

> It measured as noise (55.9 → 55.5 ms) and was kept anyway as "correct and free".
> It had also **never actually run** — DMA disabled itself before it mattered —
> so it sat in the tree for a day looking like tested code. **Code that only
> executes when another bug is absent has been tested by nothing.**

The positive counterpart, from the same project: a change kept for a *stated*
reason other than the one it was made for —

> The narrowed hold was kept anyway, because a lock should cover the shared thing
> rather than the whole operation that happens to use it — but it is not an
> optimisation and is not recorded as one.

### Removing an instrument once it has found its bug is how the bug returns unnoticed

From Chapter 24 §24.4. The first/last cell pair is still reported, with a note
saying why. The M2 switch tracing, the `TRACE_PROBES` A/B switch, and
`task_select_probe()` are all retained on the same reasoning.

### A switch that can be flipped is a claim that can be rechecked

From Chapter 18 §18.7, and the reason its wrong conclusion was findable at all.
`fb`, `spiclk`, `touchcfg`, `dfreeze`, `DISPLAY_USE_SPI2`.

### Prefer an oracle that lives on the chip

> The eFuse CRC beat a hand-maintained OUI list. Two independent clocks agreeing
> beat one clock asserting. A hardware acknowledge bit clearing beat a register
> reading back what was written.

### A best-effort policy that silently discards work is indistinguishable from a broken one

Every skip is counted. `g_draw_skipped`, `g_vp_escapes`, `g_refused`,
`g_spurious`, `g_bad_free`, `g_late`, `g_age_rescues`.

> `rescues=0` is the only thing that distinguishes "never needed" from "never
> working".

### Record a limitation where a reader would assume it was addressed

`notes.h` on text entry being a kernel feature; `arena.h` on native tasks never
being confined; `store.c` on the checksum not defending against determined
corruption; `sd.h` on the pin map being assumed until a card answers.

---

## 29.6 The rules, as a checklist

For the reader who wants one page:

**Before writing:**
1. Fetch the constant; do not recall it.
2. Choose sentinels that cannot be legal values.
3. One writer per register, or every writer maintains the shadow.
4. A deadline may only ever move earlier.
5. Clear `PS.EXCM` before calling C from a handler.
6. Delete the second mechanism rather than debug it.

**Before believing a measurement:**
7. Did the experiment demonstrably run?
8. Could this test have failed?
9. Do the samples straddle the boundary, or merely agree with it?
10. Is the instrument reporting itself invalid?
11. Which clock is this, and is it the right one for the question?
12. Does the diagnostic change what it measures? Where is the control?

**Before believing a mechanism works:**
13. Was it *triggered*, or merely confirmed to exist?
14. Is there a counter that must be zero *and* one that must be non-zero?

**Before committing a fix:**
15. Does the blast radius match the defect?
16. Is this a fix or a tolerance? Say which.
17. If this reverts more than one thing, that is a hypothesis, not a conclusion.
18. Did the number improve, and did anyone look at the screen?

---

**Next:** everything this system does not establish.
