# Chapter 24 — The Launcher, and Four Defects It Found by Existing

> Sources: `docs/UM-NATOS-021-launcher.md`
> Code: `kernel/desktop.c`, `kernel/desktop.h`, `kernel/app.h`, `kernel/shell.c`

---

## 24.1 The first thing that exists for a user

> The first thing in this project that exists for a user rather than for a test.
> Everything before it was verified by reading serial output; this is verified by
> someone touching the glass and a program starting.

It is about 250 lines, and its value turned out to be less in what it does than
in what it **demanded**:

| exposed | defect | chapter |
|---|---|---|
| horizontal position | touch X axis inverted since the driver was written | 19 §19.8 |
| position validity | PENIRQ answers presence, not validity; rail samples | 19 §19.6 |
| a task slot | nine `task_create` calls against a `TASK_MAX` of 8 — no idle task | §24.5 |
| display throughput | applications and renderer blocking each other on the draw lock | 11 §11.9 |

> **None was found by inspection. All four were found by building something that
> depended on them.**

That is the strongest argument in this book for building the consumer rather than
more infrastructure. Four defects, all latent for weeks, all in code that had
been reviewed and had passing self-tests, and all surfaced by 250 lines whose own
logic is trivial.

## 24.2 A launcher, not a window manager

```c
 * It is a LAUNCHER: an icon grid, a cursor, and double-tap to start a program.
 * It is not a window manager. Applications still render into the fixed
 * horizontal strips app.c assigns by slot index (UM-NATOS-016 §2), because
 * there is no focus model, no z-order, and no way for an application to own the
 * screen and give it back.
 *
 * That boundary is deliberate rather than unfinished. Dynamic viewports need an
 * arbitration policy for who owns which pixels, and building one before there
 * is a user interface to demand it would be designing against a guess — the
 * same reason focus arbitration was deferred when viewports could not overlap.
 * A launcher is the consumer that makes the question concrete.
```

"The consumer that makes the question concrete" — but not the one that answers
it. That distinction keeps a known gap on the record rather than letting it
become a decision by default.

## 24.3 Why a cursor on a touchscreen

```c
 * A cursor is an indirection: you can already point at what you want. It earns
 * its place here because this panel is resistive and noisy — a single reading
 * can land tens of pixels from the finger — and because an icon large enough to
 * hit reliably by direct tap would leave room for very few of them.
 *
 * So the interaction is hybrid, not modal: a single tap MOVES the cursor to it,
 * and a double-tap OPENS whatever the cursor is on. A confident user taps
 * twice and it opens; an unsure one taps once, sees where the cursor landed,
 * and corrects. **Neither has to be told which mode they are in.**
```

Two measured reasons for a design decision that would otherwise look like
inherited desktop convention.

## 24.4 Two defects of its own

### Status text drawn over a control

> The launch message was drawn at `DESK_H - 10`, which is inside the bottom-left
> cell's label. So "started" appeared over `pong`'s name — and `g_msg_sel` was
> never cleared, so it stayed there permanently.
>
> It was reported as *"it selected Pong, which was a bit off"*, and that reading
> was entirely reasonable. **The interface was asserting something false about
> its own state, indefinitely.**

The fix is recorded in the source as a rule rather than a patch:

```c
/* The bottom of the region is a status strip, not grid.
 *
 * It used to be neither: the launch message was drawn at DESK_H-10, which is
 * inside the bottom-left cell's label. So "started" appeared over pong's name
 * and stayed there permanently, and the obvious reading — that pong was what
 * had been selected — was wrong but entirely reasonable. Text that overlaps a
 * control is not a cosmetic problem; it makes the interface lie about its own
 * state. */
#define STATUS_H 14u
#define GRID_H  (DESK_H - STATUS_H)
```

Plus a second decision on the same line:

```c
/* How long a launch message stays up. It expires rather than persisting,
 * because a status line that never clears stops describing the present and
 * becomes decoration. */
#define MSG_TICKS 200u                  /* ~2 s */
```

### The selection was read at the worst possible moment

> Selection followed *every* sample while the finger was down, so the **last**
> sample before release decided it — and release is precisely when a resistive
> panel produces its worst data.

Measured, one tap on the top-right icon:

```
first sample -> cell 2      (correct)
last sample  -> cell 3      (wrong)
```

> **The correct target was read, recorded, and discarded microseconds later.**

The fix:

> Selection is now latched from the **first** sample of a press and not moved
> again until the finger lifts. Later samples describe a finger being *lifted*,
> which is not an instruction. Drag no longer moves the cursor; tap again to
> correct.

And the note about the instrument:

> This was diagnosed by an instrument built for the theory and then nearly
> deleted when the theory looked wrong. The first/last cell pair is still
> reported, with a note saying why: **removing an instrument once it has found
> its bug is how the bug returns unnoticed.**

This is the same defect as Chapter 19 §19.6, seen from the consumer's side: the
release sample reads the ADC rail. One cause, and by the time both were found it
had produced *three* symptoms across two files and three months.

## 24.5 There was no idle task

> `kmain` makes **nine** `task_create` calls; `TASK_MAX` was **8**. The ninth is
> the idle task, so it failed: `task_create()` returns −1 when the table is full,
> `task_set_idle(-1)` bounds-checks and quietly does nothing, and the return
> value was never printed or checked.

Two real costs:

> `WAITI` never executed, so the CPU never slept. And with every task asleep the
> scheduler found nothing runnable and fell through to "resume the interrupted
> context" — **which resumes a SLEEPING task, the exact invariant blocking exists
> to enforce.**

And the part that stings:

> The evidence had been printed and read. The `stacks` table added hours earlier
> listed ids 0–7 with no idle task; it was looked at and not registered. **That
> is the failure UM-NATOS-019 is entirely about, committed while writing the
> report about it.**

The fixes are structural: `TASK_MAX` is now 12, and `must_create()` panics rather
than returning −1 (Chapter 9 §9.9). `task.h` records the whole thing at the
constant:

```c
/* 12, not 8. The kernel creates nine tasks and TASK_MAX was 8, so the ninth —
 * the idle task — failed to be created and nobody noticed: task_create()
 * returns -1, task_set_idle(-1) quietly does nothing, and the return value was
 * never checked or printed. The system ran without an idle task for as long as
 * priorities have existed, which cost the WAITI power-saving path entirely and
 * meant that with every task asleep the scheduler resumed a SLEEPING task
 * rather than idling. */
#define TASK_MAX          12
```

## 24.6 Icons, close buttons, and ownership

### Icons are a bitmap, not nine drawing routines

> 8 × 8 monochrome glyphs, one byte per row, drawn at 3× into 24 × 24. Nine
> bespoke drawing functions would be more code than nine constants and would make
> every icon a place a bug can hide; a bitmap is also editable by anyone who can
> count to eight, which matters more than elegance for something whose only
> requirement is being recognisable at 24 pixels.

And a detail that is not micro-optimisation:

> Each row is drawn as one rectangle per **run** of set pixels rather than one
> per pixel. That is not micro-optimisation: every primitive takes the draw lock,
> and UM-NATOS-014 §10 established that the cost of that lock is the number of
> acquisitions rather than the time held.

Chapter 11's finding, applied by a completely unrelated piece of code, correctly.

### The close button is outside the viewport, and that is the point

`app.h` carries the argument at the constant:

```c
/* ---- where an application may draw, and where it may not ----------------
 *
 * Exported because the close button depends on the exact boundary. The kernel
 * reserves a column at the right of every strip and draws the X there, so the
 * viewport handed to the application STOPS short of it.
 *
 * That is isolation, not layout. If the close button lived inside the viewport
 * an application could paint over it, draw a decoy elsewhere, or simply fill
 * its strip and hide the way out. Putting it outside means the one control the
 * user needs in order to escape a misbehaving program is the one control that
 * program cannot touch — the same argument as the viewport itself
 * (UM-NATOS-016 §2), applied to a pixel the user owns rather than one the
 * application does. */
#define APP_VIEW_Y0     224u
#define APP_VIEW_PITCH   16u
#define APP_VIEW_H       14u
#define APP_NAME_W       44u
#define APP_CLOSE_W      16u
#define APP_CHROME_W    (APP_NAME_W + APP_CLOSE_W)
#define APP_VIEW_W      (DISP_W - APP_CHROME_W)
```

> **the one control a user needs in order to escape a program is the one control
> that program cannot touch.**

The touch path enforces the same boundary:

> close buttons see the press first, and a consumed press is not passed on, so
> closing a program cannot also select an icon underneath it.

### A button for something you cannot name

> The area below the launcher was reported as *"strange space … why is there two
> x boxes in there"*, and the answer was worse than the question implied.
>
> Those were `ping` and `pong`, which `kmain` starts at boot to keep the IPC
> counters live. They exchange **messages**, not pixels. So two programs were
> running correctly, producing no visible output, and **the only evidence of
> their existence was two buttons whose function was to destroy them.** Tapping
> one would have been a guess about what it deleted.

```c
/* The kernel's column: the program's NAME and its close button.
 *
 * The name is there because without it the area is unreadable. Four empty
 * strips with two X floating in them is what a user actually saw, and the
 * honest reading of that is "what are those" — the programs starting at boot
 * exchange messages rather than drawing, so nothing identified them. A close
 * button for something you cannot name is worse than no close button. */
```

The cost is stated rather than hidden:

> the kernel column grew from 16 px to 60 px, so applications lost about a fifth
> of their strip width to a label they can neither draw nor overwrite. If a
> program later needs the full width, this is the decision to revisit.

### The running marker, again

> The mark for "this program is running" was a four-pixel dot in the corner of
> the icon. It is now an underline beneath the icon's label.
>
> The dot had already failed once: it compared only the first character of the
> program name, so `paint`, `ping` and `pong` marked each other, and **nobody
> noticed by looking at the screen** — three wrong four-pixel dots read as a
> rendering artefact. It was found while writing §9 of this report.
>
> **A mark too small to be read wrongly is also too small to be read at all.**

## 24.7 Chrome over a repainting view

### The problem

> The 3D view's close button was first drawn immediately after the view, every
> frame, by the same routine that draws the application buttons. It strobed badly
> enough that the view read as broken.
>
> The raycaster **repaints every pixel every frame**. Anything drawn over it
> therefore survives only until the next frame begins, and drawing it later in
> the sequence cannot help — "later" ends about sixty milliseconds afterwards.
> **The button and the frame beneath it are not two draws to be ordered. They
> have to be one draw.**

### The fix

`desktop_overlay_into()` writes the button into the view's framebuffer, in that
buffer's own coordinates, and the raycaster calls it immediately before blitting.

> It is a pure function: it writes pixels and calls nothing. The frame and the
> button reach the panel in a single transfer, and there is no moment at which
> one exists without the other.

```c
/* Stamps the close button into a view's framebuffer, in the buffer's own
 * coordinates. Pure: it writes pixels and calls nothing.
```

And the invariant that keeps drawing and hit-testing in step:

> The hit test did not change. The stamped button lands on exactly the
> coordinates `desktop_chrome_touch()` already tested, which is worth stating
> because **a control drawn by one mechanism and tested by another is a standing
> invitation for the two to drift.**

### Three owners, made explicit

The `fb off` path has no buffer to stamp into, and the note pad has no
framebuffer at all. All three cases are now named in one comment:

```c
    /* Who draws the close button depends on who owns the region:
     *
     *   3D view, framebuffer on   the raycaster stamps it into the buffer, so
     *                             it and the frame arrive in one transfer
     *   3D view, framebuffer off  drawn here, and it flickers; fb off is a
     *                             diagnostic mode
     *   note pad                  the app draws it in its own header
     *
     * The middle case is the only one this branch is for. It used to be the
     * only case considered, which is why the note pad's button was invisible:
     * present, hit-testable, and drawn by nobody. */
    if (g_mode == MODE_3D && !raycast_framebuffer()) {
        draw_close(DISP_W - 20u, 2u, 18u, 18u, COLOR_WHITE);
    }
```

Chapter 26 §26.7 is the note pad's side of that: *"present, hit-testable, and
drawn by nobody"* — a button that worked and was invisible.

And the reasoning for accepting the flicker in the diagnostic mode:

> An invisible exit is worse than a visible strobing one: the way out of the view
> would exist and be unfindable.

## 24.8 The same diagnosis, at twenty times the scope

*This section is about a fix that was reverted, and it is here because the
mistake was not in the reasoning.*

> The first attempt reached exactly the conclusion above — chrome cannot be drawn
> over a view that repaints continuously — and implemented it by **reserving
> rows** at the foot of the region for a chrome bar the views would not touch.
>
> That is a correct solution. It also meant moving the region boundary, and
> therefore the application strips, and therefore the colour strip, and the
> launcher grid was resized to use the space freed by removing things that had
> been on screen at boot. **Four files of constants moved together.**

The result was a screen that looked wrong, and it was reverted without the cause
being understood. And every measurement said the renderer was fine:

> the wall band landed at the right rows, the composed framebuffer held a dark
> ceiling, an orange wall and a dark floor at the expected offsets,
> `display_blit` was verified including its contiguous fast path, and eleven
> self-tests passed throughout. Three rounds of instrumenting the raycaster
> produced three rounds of evidence that the raycaster was fine.

The comparison with the fix that worked:

> The fix in §24.7 does the same job by writing 324 pixels into a buffer that was
> about to be sent anyway.
>
> **A correct diagnosis does not license a fix of arbitrary scope.** The two
> attempts share a diagnosis and differ by a factor of twenty in what they touch.

## 24.9 The cause, found later — and it was not the layout

*Revision 1.3. §24.8 said the cause was never found. It has been, and the
conclusion a reader would have drawn from §24.8 was wrong.*

> The geometry change was applied **on its own** to current code — nothing else
> those commits touched — and the 3D view rendered correctly: the framebuffer
> allocated, frames climbed, the camera crossed the map. **Geometry alone breaks
> nothing.**
>
> What broke was the camera. The relayout raised the frame rate, movement was
> per-frame at the time, and the camera outran its turn probe and buried itself
> in a wall. **"Blank screen" and "a bunch of vertical lines" are precisely what
> a wall at zero distance looks like: full-height columns of flat colour.**
>
> Which is why every instrument insisted the renderer was fine. **It was fine.**
> It was correctly drawing the inside of a wall. A taller region made the symptom
> worse rather than better, because wall height scales with `RAY_VIEW_H` — so the
> layout looked more guilty the more of it there was.

### The actual lesson

> The revert bundled the layout change **and** the camera fix into one commit.
> The screen looked right afterwards, which appeared to convict the layout, and
> the information needed to acquit it had been destroyed by the same action that
> produced the evidence.

> **Reverting two changes together destroys the information about which one
> mattered. If a revert must bundle, the bundle is a hypothesis to test later,
> not a conclusion — and it should be recorded as one.**

> §24.8 recorded it as a conclusion. This section is the correction.

## 24.10 The screen budget, and six assertions

The screen budget is real and was the other half of the difficulty. Before the
relayout it was **312 of 320 rows allocated with 8 spare**, and the four claims
on it live in four files:

| rows | claimed by | file |
|---|---|---|
| launcher and full-region views | `DESK_H`, `RAY_VIEW_H` | `desktop.h`, `raycast.h` |
| application strips | `APP_VIEW_Y0/PITCH/H` | `app.h` |
| colour strip | `SPEC_Y/SPEC_H` | `kmain.c` |

> Growing one forces a cascade through the others with no single owner to check
> it.

The assertions are the mechanical part of the lesson:

```c
/* The screen layout is agreed across four files — desktop.h sizes the launcher
 * region, raycast.h sizes the 3D view, app.h places the application strips, and
 * kmain.c places the colour strip. Nothing enforced that agreement, and a
 * session spent reshuffling it produced a screen that looked broken in ways no
 * test could see: every self-test passed throughout.
 *
 * These do not check that the layout is GOOD. They check that its pieces do not
 * overlap or fall off the panel, which is the part a compiler can know. */
_Static_assert(RAY_VIEW_H <= DESK_H,
               "the 3D view must fit inside the launcher region");
_Static_assert(APP_VIEW_Y0 >= DESK_H,
               "application strips must start at or below the launcher region");
_Static_assert(APP_VIEW_Y0 + APP_MAX * APP_VIEW_PITCH <= DISP_H,
               "application strips must fit on the panel");
_Static_assert(APP_CHROME_W < DISP_W,
               "the kernel column must leave an application something to draw in");
```

plus two in `kmain.c` for the colour strip:

```c
_Static_assert(SPEC_Y >= APP_VIEW_Y0 + APP_MAX * APP_VIEW_PITCH,
               /* ... */);
_Static_assert(SPEC_Y + SPEC_H <= DISP_H,
               /* ... */);
```

*"These do not check that the layout is GOOD. They check that its pieces do not
overlap or fall off the panel, which is the part a compiler can know"* is a
precise statement of what a static assertion is for.

They also enabled the experiment that acquitted the layout in §24.9: *"they are
what let the experiment build safely instead of silently overlapping."*

### The layout that resulted

```
launcher / views    0..223     was 0..167
application strips  224..287   was 168..279 — now 14 rows each
colour strip        288..319   unchanged

launcher cells      80 x 70    was 80 x 51
icon glyphs         32 x 32    was 24 x 24
notes text area     10 lines   was about 4
3D view             224 rows   was 168
```

> The cost is the application strips losing more than half their height. `ping`
> and `pong` draw nothing, so it costs nothing today; a program that draws will
> notice.

## 24.11 A guard narrowed to an assertion

A late correction, from Chapter 27's session, and it belongs here because it is
about this file.

The chrome column was, for a while, believed to be painting over the full-width
3D view (Chapter 27 §27.6 — and it genuinely was, at the earlier geometry). The
first fix returned from `desktop_chrome()` outright in `MODE_3D`. Once the
geometry changed, that guard became both unnecessary and harmful:

```c
void desktop_chrome(void)
{
    /* The strips are BELOW a full-width view, and that is load-bearing.
     *
     * APP_VIEW_Y0 is 224 and RAY_VIEW_H is 224, so slot 0 begins exactly where
     * the 3D view ends and nothing here can touch it. An earlier fix returned
     * from this function outright in MODE_3D, on the theory that the chrome was
     * painting over the view. The geometry says otherwise, and the cost of the
     * blanket guard was that the strip band was never repainted while the view
     * was open -- so a program starting behind the view left a stale band that
     * only looked right once it had painted over every pixel itself. That is
     * the "wrong for ten to thirty seconds, then fine" report.
     *
     * The assertion below is the part worth keeping: it fails the build if the
     * view ever grows tall enough to reach the strips, rather than leaving a
     * future overlap to be rediscovered from the glass. */
    _Static_assert(APP_VIEW_Y0 >= RAY_VIEW_H,
                   "app strips must stay below the 3D view, or chrome overwrites it");
```

A runtime guard replaced by a compile-time assertion — *"which is the check that
should have been written first, since it answers the overlap question at compile
time instead of from the glass."*

## 24.12 Verification

```
tap left icon   raw_x=3408 -> x=0,   cell 0
tap right icon  raw_x=920  -> x=196, cell 2
seven consecutive taps, z 1662..2261, no rail samples
x spanning 0..187 across all three columns
first and last sample of each press agree
```

Five separate claims in five lines: the axis maps correctly at both ends, taps
are firm enough to pass the pressure gate, no rail samples got through, the
horizontal range spans the grid, and the latching fix holds.

> All three columns selectable, double-tap opens the named program, and the
> status strip reports which. Confirmed on hardware by the user.

## 24.13 The launcher costs nothing when idle

```c
 * It repaints only when something changed, so an idle desktop pushes **zero**
 * pixels — unlike the raycaster, which repaints unconditionally because every
 * frame differs. If the screen looks frozen, that is correct: it is waiting for
 * input.
```

A frozen-looking screen that is *supposed* to be frozen, in a project whose worst
debugging session was caused by a frozen screen that was not. Hence the last
sentence, which exists so nobody re-diagnoses it.

## 24.14 Metrics

| Quantity | Value |
|---|---|
| Status strip | 14 px, message expires ~2 s |
| Double-tap window | 60 ticks (~600 ms) |
| Minimum press | 2 ticks, rejects contact chatter |
| SPI cost when idle | 0 bytes |
| Grid | 3 × 3, cells 80 × 70 |
| Icon glyphs | 8 × 8, drawn at 4× |
| Screen rows allocated, before / after | 312 of 320 / 320 of 320 |
| Layout assertions | 6, spanning four files |
| Overlay cost per frame | 324 pixels into a buffer already being sent |
| Reverted attempt | ~200 lines, four files |
| Kernel-owned column | 60 px of 240 |
| Application viewport width | 180 px, was 240 |
| **Defects exposed in other subsystems** | **4** |
| Defects of its own | 2 |

## 24.15 What this does not establish

- **The screen is now fully allocated.** Every row is claimed; there is no spare.
  The next feature wanting rows must take them from something named in §24.10,
  and the assertions will refuse a build that overlaps.
- **Application strips are 14 rows.** No program currently draws into one, so
  nothing has tested whether that is enough to be useful.
- **No focus, no windows, no z-order.**
- **No way to stop a program from the launcher itself.** `kill` exists only in
  the shell — which since Chapter 26 is *on* the grid, so it no longer needs a
  host, but is still a command typed into another app.
- **The `fb off` path still flickers.**
- **Nothing tests the overlay.** The button's position is agreed between
  `desktop_overlay_into()` and `desktop_chrome_touch()` by two matching
  constants, and no assertion ties them together — "the exact drift the layout
  assertions were added to prevent elsewhere".
- **The running marker is per-name, not per-instance.**
- **Faulted programs cannot be dismissed.** A program that faults keeps its slot
  and shows no X, because it is not running; it lingers until `kill`.
- **The chrome column is fixed width.** 60 px regardless of name length.
- **Double-tap timing is unmeasured.** 600 ms and the 2-tick minimum press were
  reasoned, not derived. "The counters exist to settle them and nobody has."
- **The icons are guesses at 24 pixels.** Judged by one person. "`blit` and
  `rogue` are the two most likely to read as noise."
- **One user.** Every judgement about whether the interaction feels right came
  from a single person on a single panel.

---

**Next:** what the launcher hands the region to.
