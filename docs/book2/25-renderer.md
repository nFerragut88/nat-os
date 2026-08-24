# Chapter 25 — The Renderer

> Sources: `docs/UM-NATOS-015-display.md` §5.8, `docs/UM-NATOS-021-launcher.md` §6.7
> Code: `kernel/raycast.c`, `kernel/raycast.h`, `tools/gen_sintab.py`

---

## 25.1 What it is, and why it is in this book

A grid raycaster: one ray per screen column, fixed-point throughout, face
shading, wall texture, a camera that wanders a maze on its own.

It is the most demanding consumer in the system and it is the reason four
separate defects elsewhere became visible. From Chapter 18:

> Driving the display from a raycaster rather than a status screen put a very
> different load on this layer and found two things.

From Chapter 11:

> A raycaster took the display lock once per column and spent 25 seconds per
> frame on 37 ms of work.

From Chapter 9, it is the only HIGH-priority task besides touch, and the reason
strict priority had to be reasoned about at all.

It also carries an admission about its own placement:

> That is recorded here for want of a better home. It is not a display-driver
> property, and if the renderer grows much further it wants its own report rather
> than a subsection of the driver's.

This chapter is that report.

## 25.2 The map

```c
#define MAP_W 16
#define MAP_H 16

/* 1 is wall, 0 is floor. A ring with a few interior blocks, so walking it
 * produces corridors and openings rather than one empty box. */
static const uint8_t MAP[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1},
    {1,0,1,1,0,1,0,1,0,1,1,1,0,1,0,1},
    /* ... */
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};
```

The map shape is a design decision with a stated purpose: corridors and openings
rather than one empty box, so the camera's navigation has something to fail at
(§25.6).

## 25.3 Fixed point, and an overflow avoided by construction

```c
#define FP        16
#define ONE       FP_ONE
#define STEP_SHIFT 3                    /* march 1/8 of a cell per step */
#define MAX_STEPS  160                  /* 20 cells before giving up    */

/* tan(30 deg) in 16.16 — half of a 60 degree field of view. */
#define PLANE_SCALE 37837
```

16.16 fixed point. No floating point anywhere — the ESP32 has an FPU but the
kernel is `-nostdlib` and the soft-float helpers would have to come from libgcc
or the ROM, which Chapter 2 §2.10 shows is a link-order minefield.

The marcher steps incrementally rather than scaling a ray by a growing parameter,
and the reason is arithmetic:

```c
        /* March by a fixed fraction of a cell. Stepping incrementally avoids
         * multiplying the ray by a growing t, which would overflow 32 bits
         * within a few cells. */
        int32_t sx = rayX >> STEP_SHIFT;
        int32_t sy = rayY >> STEP_SHIFT;
```

The scaling in the camera-plane arithmetic is the same defence, applied
differently:

```c
    int32_t planeX = (-dirY / 256) * (PLANE_SCALE / 256);
    int32_t planeY = ( dirX / 256) * (PLANE_SCALE / 256);
    /* ... */
        int32_t rayX = dirX + (planeX / 256) * (cameraX / 256);
        int32_t rayY = dirY + (planeY / 256) * (cameraX / 256);
```

Both operands pre-scaled by 256 so the product stays inside 32 bits. Chapter 19
§19.10 notes the contrast: the calibration arithmetic does *not* need this,
"nothing here approaches the limits that forced the scaled multiply in
raycast.c", and says so at the point where a reader might copy the pattern
unnecessarily.

The sine table is generated on the host (`tools/gen_sintab.py`) into
`kernel/generated/sintab.h`, and lives in `.rodata` — which since Chapter 4 costs
zero DRAM.

## 25.4 The march

```c
    for (uint32_t c = 0; c < RAY_COLS; c++) {
        uint32_t x = c * RAY_COLW;
        /* -1 at the left edge, +1 at the right. */
        int32_t cameraX = (int32_t)((2 * x * ONE) / RAY_VIEW_W) - ONE;

        int32_t rayX = dirX + (planeX / 256) * (cameraX / 256);
        int32_t rayY = dirY + (planeY / 256) * (cameraX / 256);

        int32_t sx = rayX >> STEP_SHIFT;
        int32_t sy = rayY >> STEP_SHIFT;

        int32_t  fx = g_px, fy = g_py;
        int      hit = 0, steps = 0;
        /* ... */

        t0 = task_cpu_cycles();
        while (steps < MAX_STEPS) {
            fx += sx;
            fy += sy;
            steps++;
            if (wall_at(fx, fy)) {
                hit = 1;
                hx = fx >> FP;
                hy = fy >> FP;
                /* ... face and texture recovery ... */
                break;
            }
        }

        t_march += task_cpu_cycles() - t0;
```

Note `task_cpu_cycles()` rather than `xt_ccount()` — Chapter 9 §9.8's clock,
used correctly here.

`MAX_STEPS` bounds the march at 20 cells. A ray that finds nothing produces
`hit = 0` and a zero-height wall rather than running forever, which matters
because the map is finite and a ray parallel to a corridor can miss everything.

### Perspective, without a divide per pixel

```c
        /* Perpendicular distance, in 16.16 cells. */
        int32_t dist = (int32_t)steps << (FP - STEP_SHIFT);
        if (dist < (ONE / 8)) {
            dist = ONE / 8;             /* never divide by ~zero */
        }

        int32_t h = hit ? (int32_t)((RAY_VIEW_H * ONE) / dist) : 0;
        if (h > (int32_t)RAY_VIEW_H) {
            h = RAY_VIEW_H;
        }

        int32_t top = ((int32_t)RAY_VIEW_H - h) / 2;
        int32_t bot = top + h;
```

One division per column, not per pixel. `dist` is clamped away from zero — the
guard that turns Chapter 24 §24.9's "camera inside a wall" from a divide-by-zero
into a full-height column of flat colour, which is exactly what was observed.

### Distance falloff

```c
        /* Distance falloff. Full brightness up close, floor of 40 so a far wall
         * stays visible rather than fading into the ceiling. */
        uint32_t shade = 255u;
        if (dist > ONE) {
            uint32_t d = (uint32_t)(dist >> FP);
            shade = (d >= 12u) ? 40u : (255u - d * 18u);
        }
```

A floor rather than a linear fade to black, because a wall that fades into the
ceiling colour stops being a wall.

## 25.5 One ray per column, and an estimate wrong by forty

`RAY_COLW` went from 2 to 1, doubling horizontal detail. The constant carries
both the old reasoning and the measurement that overturned it:

```c
/* One ray per screen column.
 *
 * Was 2, which halved the cast cost at the price of halving horizontal detail.
 * That trade made sense when the renderer's cost was unknown; measurement says
 * it is not: marching is 0.2 ms of a 50 ms frame and the blit is 41.9 ms, so
 * the frame is bus-bound and casting twice as many rays is close to free. */
#define RAY_COLW  1u
#define RAY_COLS  (RAY_VIEW_W / RAY_COLW)
```

The estimate was wrong by a factor of forty — predicted 0.2 ms, cost about 9 ms —
for **two reasons worth recording**:

> - the 0.2 ms march figure was sampled with the camera **against a wall**, where
>   rays terminate immediately. **A cost measured at the cheapest moment is not a
>   cost.**
> - compose doubled although the pixel count did not: the per-row loop now runs
>   once per column, 240 × 168 iterations instead of 120 × 168 writing two pixels
>   each. **Same pixels, twice the loop.**

Both are general. The first is a sampling error — the same class as Chapter 19
§19.9's "measured in one place and written down as a property of the panel". The
second is a reminder that loop *iterations* and *work* are different quantities.

The superseded comment above it is left in the file, which is how the history
survives:

```c
/* Two pixels per row, so one composed buffer covers a 2-pixel-wide column.
 *
 * The data volume is identical either way — the panel receives the same 80,640
 * bytes. What halves is the number of address-window setups, and those measured
 * at ~450 us each against only ~94 us of pixel data per column. The overhead was
 * five times the payload. */
```

That "~450 µs each" figure is itself one of the numbers Chapter 18 §18.7 later
identified as wall-clock contaminated by preemption. Two superseded measurements
stacked in one comment block, both left standing, both labelled by what came
after — which is the project's documentation style in miniature.

## 25.6 Face shading and wall texture

### Faces, recovered rather than tracked

```c
                /* Which face was crossed, recovered by comparing the cell one
                 * step back. A DDA would know this for free; a fixed-step march
                 * does not, and one subtraction is cheaper than changing the
                 * marcher.
                 *
                 * If both coordinates changed cell in the same step the choice
                 * is arbitrary and X wins. At 1/8 of a cell per step that is a
                 * corner hit, where either answer is defensible. */
                face_x = (((fx - sx) >> FP) != hx);
```

The design problem:

> Every wall face was equally bright, so two walls meeting at a corner differed
> only by cell hue and the corner read as a colour change rather than as
> geometry. Faces crossed through a vertical boundary now render at full
> brightness and the others at 5/8.

One subtraction per hit, one multiply per column, "unmeasurable against a 41.9 ms
blit". Chapter 18 §18.7's finding — *detail is cheap here and pixels are
expensive* — used to decide what was worth adding.

### Texture, positioned in world space

```c
                /* Where along the wall face the ray landed, as a fraction of a
                 * cell. The face runs along the axis that did NOT change cell,
                 * so that is the coordinate to take the fraction of. */
                wall_u = (uint32_t)((face_x ? fy : fx) & (ONE - 1));
```

> Four panels per cell with a narrow dark seam between them, positioned in
> **world space** so perspective compresses them as a wall recedes. **A flat
> colour gives the eye no scale; evenly spaced marks that converge do.** The same
> trick as the face shading, one level finer.
>
> Computed once per column, so it costs a shift and a compare.

And a cost decision stated as one:

> Vertical only, and that is a cost decision rather than an aesthetic one:
> horizontal courses would need per-pixel work inside the compose loop, which is
> the one place in this renderer where a cost multiplies by 168.

## 25.7 Navigation, and per-cell decisions

The camera's wander was rewritten from reactive probing to a heading chosen once
per cell:

> no amount of probe tuning could traverse a maze — in corridors one cell wide a
> look-ahead probe is blocked almost always, so the camera turned continuously
> and never committed to a direction. **Distinct cells visited in 20 s went from
> 4, pacing in one pocket, to 12 across the map.**

The tuning constants carry their own history, which is where two "runs into
walls" reports are recorded:

```c
/* ---- walk and steer ------------------------------------------------------
 *
 * MOVE_SHIFT  distance per tick, as a shift of the unit direction vector.
 *             6 is 1/64 of a cell per tick, so about 1.5 cells a second once
 *             MOVE_CATCHUP_MAX stopped throwing most of the ticks away.
 * LOOK_SHIFT  how far ahead a wall triggers a turn. 0 is a FULL cell — half a
 *             cell was not enough warning at this speed, which is what "runs
 *             into walls before turning" meant the second time.
 * STOP_SHIFT  how far ahead a wall stops the walk. 3 is an eighth of a cell,
 *             so the camera keeps advancing through a turn and only halts when
 *             it is genuinely about to embed.
 * TURN_UNITS  angle per tick while a turn is wanted, out of 256 for a full
 *             circle. 1 is 1.4 degrees — a sweep, where the 3 it replaced was
 *             a 4.2-degree jerk applied only on contact. */
#define MOVE_SHIFT  6
#define LOOK_SHIFT  0
#define STOP_SHIFT  3
#define TURN_UNITS  1
```

### Movement per tick, not per frame — and a guard that fired constantly

```c
/* Ticks of movement a single frame may apply.
 *
 * This was 4, and it was the reason the camera crawled AND turned late. At
 * 9 fps about eleven ticks pass between frames, so the clamp discarded two
 * thirds of both the walking and the turning, every frame — a stall guard that
 * fired constantly during normal running.
 *
 * 20 is 200 ms, longer than any gap between frames the renderer produces, so it
 * now only bites on a genuine stall. The guard is still needed: without it, a
 * pause of a second would apply a second of movement in one step and walk
 * straight through a wall, because the collision probe only checks the
 * destination of each step and not the path to it. */
#define MOVE_CATCHUP_MAX 20u
```

Three things in one comment: what the guard is for, why the old value was wrong,
and why removing it entirely would be worse. **"A stall guard that fired
constantly during normal running"** is a category of bug worth naming — a limit
set for an exceptional case that turns out to bind in the ordinary one.

Movement is decoupled from frame rate:

```c
    for (uint32_t step = 0; step < elapsed; step++) {
        navigate_step();
    }
```

which is the fix for Chapter 24 §24.9's actual cause: *"the relayout raised the
frame rate, movement was per-frame at the time, and the camera outran its turn
probe and buried itself in a wall."*

## 25.8 Where the frame goes

```c
/* Split the frame cost three ways: ray marching, column composition, and the
 * SPI transfer. Guessing which dominates has been wrong often enough in this
 * project that it is cheaper to measure. */
static uint32_t g_us_march, g_us_compose, g_us_blit;
```

```
march      2.6 ms      one ray per screen column
compose   13.9 ms      240 x 168 pixels into DRAM
blit      41.9 ms      one 80,640-byte window
                       ~9.1 fps
```

> **The frame is bus-bound**: 72% of it is pushing pixels down SPI at 40 MHz. So
> detail is cheap here and pixels are expensive, and that shapes what is worth
> adding.

Three separate timers rather than one total, and the justification —
*"guessing which dominates has been wrong often enough in this project that it is
cheaper to measure"* — is the project's own record cited as evidence.

## 25.9 The framebuffer, twice measured

Covered in Chapter 18 §18.7 and worth restating from this side because the switch
lives here.

```c
/* NULL when the direct path is in use. */
static uint16_t *g_fb;
```

- **First measurement:** `fb off` 126,650 µs, `fb on` 131,411 µs. No difference.
  80,640 B for nothing. Defaulted **off**.
- **Both measurements were contaminated** — by a tick that stalled for up to
  183 ms (Chapter 7 §7.9) and by draw-lock contention that dominated the frame
  (Chapter 11 §11.9).
- **Second measurement, both faults fixed:** one window per frame beats 240.
  Defaulted **on**.

> §5.7's *conclusion* was wrong; its *method* was right, and is why the error was
> findable — a switch that can be flipped is a claim that can be rechecked.

The framebuffer's existence also enables the overlay of Chapter 24 §24.7: the
close button is stamped into it so the frame and the button arrive in one
transfer. With `fb off` there is nothing to stamp into, which is why that path
flickers.

## 25.10 Two accessor names, one of which caused a panic

From Chapter 27 §27.8's tally of instruments that lied:

> **`raycast_framebuffer()`.** Returns a boolean. Named like an accessor.
> Panicked a diagnostic that trusted the name.

The diagnostic used its return value as a pointer and stored through address
`0x00000001`:

> (The first attempt at this panicked, because `raycast_framebuffer()` returns a
> *boolean* and the test used it as a pointer, storing through address
> `0x00000001`. `raycast_fb_ptr()` now exists so nobody repeats that. **It was a
> fast and educational panic.**)

Two functions now, with unambiguous names. The lesson is about naming rather than
about pointers: an accessor named for a *thing* should return the thing.

## 25.11 `dfreeze`, and the measurement that finally said something

The renderer's most useful diagnostic is one that stops it.

```c
/* Stops every drawer in the display task, leaving whatever is on the panel and
 * in the framebuffer exactly as it stands.
 *
 * This is a measurement tool. The raycaster rewrites the entire framebuffer
 * every frame as the camera moves, so comparing the buffer before and after
 * some event always differs for innocent reasons. With the renderer frozen,
 * ANY change to the buffer was made by something else -- which is precisely
 * the open question about why launching a program repairs a garbled view. */
volatile int g_display_frozen = 0;
```

Chapter 27 §27.10 is the experiment it enabled, and the result is the first
positive finding in a nine-theory investigation:

```
BEFORE launch    equal-to-first=25088   rows 67e0 67e0 67e0 67e0
CONTROL          equal-to-first=25088   rows 67e0 67e0 67e0 67e0
AFTER launch     equal-to-first=25088   rows 67e0 67e0 67e0 67e0
```

> **Launching a program does not alter one sampled byte of the framebuffer.**

with the control row doing the real work:

> The **control** row is the load-bearing one. Taken with nothing done in
> between, it proves the freeze holds the buffer still and the sampler is
> stable — without it, "no change" would be indistinguishable from a broken
> measurement.

A control group measured on the *same* instrument in the *same* run, which is the
same discipline as the ADC's four touch pins in Chapter 22 §22.13.

## 25.12 Metrics

| Quantity | Value |
|---|---|
| Map | 16 × 16 cells, walls and corridors |
| Fixed point | 16.16 |
| March step | 1/8 cell, `MAX_STEPS` 160 (20 cells) |
| Field of view | 60° (`PLANE_SCALE` = tan 30° in 16.16) |
| Rays per frame | 240, one per column |
| View | 240 × 224 |
| Framebuffer | 80,640 B, on by default |
| Frame cost, march / compose / blit | 2.6 / 13.9 / 41.9 ms |
| Frame rate | ~9.1 fps |
| Share of frame on the bus | 72% |
| Cost of face shading and wall seams | unmeasurable — 9.1 fps before and after |
| Distinct cells visited in 20 s, before / after navigation rewrite | 4 / 12 |
| `RAY_COLW` change, predicted / actual cost | 0.2 ms / ~9 ms |
| Overlay cost per frame | 324 pixels |

## 25.13 What this does not establish

- **Wall texture is vertical only.** No horizontal courses, because those need
  per-pixel work where costs multiply by 168.
- **Face shading is by orientation only.** No light source, no falloff across a
  face. "It is a depth cue, not lighting."
- **The corner case in face recovery is arbitrary.** When a march step crosses
  both boundaries at once, X is chosen because something must be. Nothing
  measures how often that happens or whether it is visible.
- **No sprites, no entities, no floor or ceiling texture.**
- **No player control.** The camera wanders; touch steers left/right only, and
  did so backwards for three days (Chapter 19 §19.8).
- **No damage tracking.** Every frame repaints every pixel unconditionally, which
  is why the launcher can idle at zero SPI bytes and this cannot.
- **Nothing measures whether the shading is legible.** Judged by one person on
  one panel.
- ~~**The startup glitch is open.**~~ **Closed, and it was not this chapter's
  fault** — see §25.14.

## 25.14 The startup glitch was a display bug, not a renderer bug

The "garbled for ten to thirty seconds, repaired by launching an unrelated
program" fault is closed (UM-NATOS-030, Ch. 18 §18.11). The cause was one bit in
`kernel/display.c`: `DMA_OUTLINK_START` was defined as `(1u << 30)`, which is
`OUTLINK_RESTART`.

Nothing in `raycast.c` was wrong. It is worth stating plainly, because this
chapter spent its whole life as the prime suspect and the suspicion was
structural rather than evidential: the renderer is the heaviest consumer in the
system, it was the place the fault was *visible*, and a fault that appears in one
consumer looks like that consumer's fault.

It was, precisely, the opposite. Every transfer wider than 32 pixels was
affected system-wide — the launcher, full-screen clears, longer text, the panic
screen. The renderer issues 224 DMA transfers per frame inside a single window,
more than everything else combined, so it expressed the defect hardest and
first. **The renderer was the best instrument for a bug it did not contain.**

Three of this chapter's observations are explained by it, and all three had been
filed as separate mysteries:

| observation | actual mechanism |
|---|---|
| Garbled on open, good later | The wall-clock timeout trips on a preempted display task at ~12 s, permanently disabling DMA — and the FIFO path never touches `SPI_DMA_OUT_LINK_REG`, so the picture becomes correct |
| `gfxrogue` "repairs" it | Its 180 px fill is over the 64-byte DMA threshold, so it adds chances to be descheduled mid-wait and brings that timeout forward. It was killing the DMA engine faster, not repairing anything |
| The `draw` application never repairs it | Its block is 20 px — 40 bytes a row, below the threshold, entirely FIFO, incapable of provoking the timeout. This was the observation that fitted no theory for hours, and it is the one that confirms this one |

The frame numbers in §25.12 were all measured on the FIFO fallback and are
therefore honest measurements of the wrong configuration. With DMA working the
full-view blit is **31.4 ms** rather than 55.8 ms.

### The renderer as an instrument

Two of this chapter's diagnostics were built during that investigation and are
worth keeping in the renderer's own record, because they are the shape a
control should have:

- **`camfreeze`** holds the viewpoint still while the renderer keeps drawing
  every frame. It is deliberately *not* `dfreeze` (§25.11), which stops the
  display task and freezes the buffer with it: a control that also freezes the
  thing being measured answers nothing. With the camera pinned at `cell 3,11
  frac 829,362` the frame counter still advanced 992 → 1061 in six seconds.
- **`campos`** reports the camera's cell, sub-cell position, heading, and
  whether `wall_at()` says it is inside geometry. That last field exists because
  this renderer has a documented failure mode — buried in a wall, every column
  one flat colour — which is diagnosed by *asking the map*, not by judging how
  flat the picture looks.

And one caution, since this chapter is the one a reader would come to for it:
`fbhash` over all 53,760 pixels held constant for 76 seconds under `camfreeze`,
and that settled less than was claimed for it at the time. **A frozen camera
stably rendering a *wrong* scene produces a constant hash too.** What finally
separated render from transport was `fbdump` — the buffer extracted and looked
at, pristine, while the panel was garbled (Ch. 28c §28c.4).

---

**Next:** the two applications that take text from a person.
