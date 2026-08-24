# Chapter 28b — Two Mysteries, Eleven Theories, and a Novel That Called It

> **This chapter reaches the wrong conclusion, and it is here on purpose.** It is
> the account of a session that spent a day on a fault, eliminated eleven
> theories correctly, found a second genuine defect on the way, and did not find
> the cause. Chapter 28c is the cause. Read them in order: the value of the
> second is mostly in how ordinary the first one looks from inside.

> Sources: `docs/UM-NATOS-029-two-mysteries-and-a-novel-that-called-it.md`
> Code: `kernel/display.c`, `kernel/raycast.c`, `kernel/mutex.c`, `kernel/task.c`, `tools/app_gfx_rogue.vasm`

---

## 28b.0 Why this chapter is lettered

Chapters 28b and 28c were added when reports UM-NATOS-029 and UM-NATOS-030 were
folded into this book. They are lettered rather than numbered so that every
cross-reference in the other thirty-one chapters, in the seven appendices and in
the reports themselves still points where it pointed before — the same reason
the conventions chapter is `00b` rather than `01`.

They belong in Part VI because the interesting content is not the defect. The
defect is one bit and it takes a paragraph (Ch. 18 §18.11). What takes a chapter
is a day of correct reasoning arriving at the wrong place, which is the subject
this part of the book is about.

## 28b.1 The two mysteries, stated precisely

Chapter 25 built a raycaster; Chapter 24 gave it a launcher to open from. From
almost the first day it opened wrong.

**Mystery One — the view garbles, and a failing program repairs it.**

Opening the 3D view shortly after boot produces a scrambled image. It heals by
itself after roughly ten to sixty seconds. It can also be healed on demand, and
reliably, by launching **`gfxrogue`** — a program whose entire purpose is to fail
to draw anything.

`gfxrogue` is `tools/app_gfx_rogue.vasm`: 76 bytes, nineteen VM instructions,
and part of the isolation test suite (Ch. 15 §15.6, Ch. 17 §17.3). It exists to
prove an application cannot draw outside its viewport, so everything it asks for
is a lie:

```
forever:
    paint the ENTIRE screen red      -> clipped to a 180x14 strip
    paint the ENTIRE screen white    -> clipped to the same strip
    paint at (60000, 60000)          -> refused outright, nothing drawn
```

The kernel refuses all three as stated. What a person sees is a small band near
the bottom of the panel flickering red and white; that flickering strip *is* the
containment test passing.

So the mystery is a program that mostly does not draw — and which, with the 3D
view open, usually loses the race for the draw lock and does not draw at all —
repairing the display.

**Mystery Two — uptime alone repairs it.**

The more useful of the two, and it arrived as an aside from the person holding
the board: leave the board running for five to seven minutes and the 3D view
opens correctly the first time. No program, no intervention.

That makes the variable *uptime*, which is a far smaller thing to search than
"drawing". It was filed as a curiosity and not acted on for a day. Chapter 28
§28.10 records what that cost; Chapter 28c §28c.3 records that it was the
diagnosis, stated in full, by someone who was not looking at any instrument.

A third observation from the same session was not reproduced under
instrumentation and is recorded because the distinction matters: at low uptime
the view may not open **at all**, rather than opening wrongly. Those are
different faults, and reporting them as one is how a search gets aimed at the
average of two things.

## 28b.2 A novel described the bug before anyone found it

Alongside the engineering reports, the author has been writing a novel — *The
NatOS Organization: A Novel of System Architecture* — in which the kernel's
subsystems are staff in an office. It lives at `docs/used_medias/um_1.txt` and in
forty chapters beside it. In the first of them, written before any of this
session's debugging, the Scheduler is refusing the Touch Controller a context
switch:

> *"I need a context switch," Touch Controller pleaded over the departmental
> channel. "My PENIRQ line just asserted!"*
>
> *"Impossible," Scheduler replied, not looking up from his lists. "The Display
> Driver has priority inheritance. The 3D raycast renderer is in the middle of a
> DMA transfer. Interrupting now would tear the framebuffer."*

Every clause of that excuse turned out to name a live issue in this session, and
two of the clauses are **false about this kernel** — which the source says
plainly, in both cases, and had said since the day each was written.

| The novel says | The instruments say |
|---|---|
| the renderer holds the display | `dlock hold ms=7965` against ~7,860 ms of uptime — it holds the draw lock essentially without interruption |
| Touch loses the argument | `drawskip` climbing into the thousands; `vp_fill()` uses `display_try_lock()` and simply gives up |
| nobody is contending | true, and misleading. `cont=0` — nothing ever blocks on the lock, because everything else yields rather than waits |
| *"has priority inheritance"* | **false.** nat-os has **ageing**. `mutex.c` contains no mention of priority at all |
| *"in the middle of a DMA transfer"* | **false.** DMA had been disabled since seconds after boot. It was all going out over the FIFO |

The Display Driver's excuse was load-bearing, and it was wrong about both
mechanisms: it was defending a transfer that was not happening, using a
scheduling feature this kernel does not implement.

This is offered as a curiosity rather than as a method. It is a real one, though,
and the reason is not that fiction is insightful. It is that the status line
reported `cont=0` and everyone read that as *no contention problem* rather than
as *nobody is even allowed to contend*. The novel had to describe the argument
between two subsystems in words, and words made the difference visible where a
zero had hidden it.

### 28b.2.1 Ageing is not priority inheritance

The claim was settled by reading `kernel/task.c:996`, which draws the
distinction itself:

```c
    /* Effective priority = base + ageing credit, computed per candidate below.
     * The base priority is never modified: ageing is a property of the
     * SELECTION, not of the task, so a task that finally runs returns to its
     * declared priority automatically rather than needing to be restored. That
     * distinction is what keeps this separate from priority inheritance, which
     * really does change a task's priority and really does have to undo it. */
```

The two solve overlapping problems and are not interchangeable:

| | **Ageing** (implemented, Ch. 9 §9.6) | **Inheritance** (built, never wired, Ch. 30 §30.5) |
|---|---|---|
| What it lifts | any task that has waited too long | the specific task that is *blocking* you |
| Why | regardless of why it waited or who holds what | because it holds a lock you need |
| Undo | none needed — it is a property of the selection | mandatory, and a mechanism that must remember to restore can forget |
| Evidence it runs | `g_age_rescues` = 4,513 decisions in one dump | `git log -S task_boost` never touches `mutex.c` |

The irony is that the mechanism this kernel actually has is the better fit for
the scene the novel wrote. **Inheritance would be close to useless here**,
because it only helps when a high-priority task *blocks* on a lock — and in
nat-os almost nothing blocks. `cont=0` is exactly that fact. Applications call
`display_try_lock()` and walk away; the only task that ever blocks on the panel
is the display task itself (`dblk disp/apps/touch/shell=3630/0/0/0`), and ageing
is what rescues it.

A related check, raised by the same passage and exactly the shape of thing that
looks like a defect: `vp_fill()` takes `display_try_lock()` and then calls
`display_fill_rect()`, which takes `draw_lock()` on the same mutex. That is
**not** a recursive-acquire bug. `mutex_t` carries `owner` and `depth`, and
`mutex_try_lock()` increments `depth` when the caller already owns it:

```c
    if (m->owner == me) {
        m->depth++;
        crit_exit(s);
        return 1;
    }
```

`draw_lock()` further guards its telemetry on `depth == 1u`, so nested
acquisitions cannot corrupt the hold-time measurement — which is what makes the
7,965 ms figure in the table above worth quoting at all. An instrument that
double-counted its own re-entry would have produced a hold time larger than
uptime, and the number would have been discarded as obviously broken. It was not
obviously broken, and that is the whole reason it could be used.

## 28b.3 Eleven theories, all eliminated

Each was tested against hardware, not argued away. Every one of them is still
eliminated; Chapter 28c only changes what they were eliminated *from*.

| # | Theory | How it died |
|---|---|---|
| 1 | DMA fallback to FIFO mid-blit | `dma=320/0` at the time; also §28b.7.1 |
| 2 | Touch task scheduling | identical across all four polling configurations |
| 3 | Application overdraw | killed every application; no change |
| 4 | Panel signal integrity | 40 / 20 / 10 MHz changes appearance only |
| 5 | Chrome column overwriting the view | `APP_VIEW_Y0` 224 == `RAY_VIEW_H` 224; static assert added (Ch. 27 §27.11) |
| 6 | Movement catch-up burst | fixed via `raycast_open()`; symptom unchanged |
| 7 | Touch calibration | fixed; symptom unchanged |
| 8 | Arena / framebuffer overlap | `fb 0x3ffbcd90..0x3ffd7190`, arenas from `0x3ffd75b0` |
| 9 | Panel window desync | `display_resync()` — one CASET/PASET/RAMWR — no change (Ch. 18 §18.10) |
| 10 | Window-setup *frequency* | `resyncn 500` — a burst of them — no change |
| 11 | CPU starvation | `hog` — spins like `task_apps`, draws nothing — no change |

Two further probes bracketed the drawing itself:

- **`fifopoke`** — a single 16-byte transfer forced down the FIFO path. No
  change.
- **`stripn 200`** — `gfxrogue`'s exact clipped rectangle, same colours, same
  window setups, same DMA bursts, issued directly from the shell. **No change.**

`stripn` is the important negative and it deserves its status. It reproduced
everything `gfxrogue` puts on the wire and repaired nothing, which retires the
entire transport-side family of theories in one stroke: whatever `gfxrogue` does
that matters, it is not the bytes.

That conclusion is correct, and it is also where the next four hours went. The
obvious reading — *if not the bytes, then the timing of the drawing* — happens
to be true, which is the worst possible property for a wrong turn to have: the
sessions that followed it were testing a true statement whose mechanism nobody
had guessed.

## 28b.4 The one thing that did work, and the hole it came out of

The tests above cover a two-by-two with a hole in it:

| | burst | continuous |
|---|---|---|
| **draws** | `stripn` ✗ | never tested |
| **doesn't draw** | — | `hog` ✗ |

`gfxrogue` is the missing cell: drawing, forever, interleaved with the renderer.
So `hog` grew a second mode. **`hog draw`** — which spins *and* fills
continuously, using `display_try_lock()` so it behaves like an application —
**repaired the view.**

Credit where it is due: the author identified that gap, not the instruments. The
phrasing was *"gfxrogue is spamming gfx code, the kernel has to let the tasks
take turns"*, offered tentatively, and it was correct where four measured
theories in a row had not been. Chapter 28 §28.10 counts it, and it is one of the
five occasions in this project where a human observation contradicted the
instruments and the human was right.

The result is real and it is not an explanation. It says the repair needs
continuous interleaved drawing. It does not say why the picture is wrong without
it — and the reason `hog draw` works is not the reason it was built to test.
Chapter 28c §28c.3 gives it: 180 pixels is 360 bytes a row, which is over the
64-byte DMA threshold, so continuous drawing adds chances for the display task to
be descheduled mid-wait and brings a spurious timeout forward. It was killing the
DMA engine faster. Every part of the mechanism except the sign was guessed
correctly.

## 28b.5 Render or transport? Three attempts to ask one question

With `hog draw` giving a reliable on/off switch for the first time, the obvious
question became answerable: is the *framebuffer* wrong, or only the picture?

It took three attempts to ask it properly, and the sequence is a compact lesson
in instrument design.

**Attempt one — a summary, and a camera that would not hold still.** `fbsum`
was compared in both states and found large differences: `equal-to-first` 10,304
against 718. This was reported as proof of a render fault. It was nothing of the
sort. **The camera walks continuously** (Ch. 25 §25.7), so two samples fifteen
seconds apart differ by construction. A second run had the numbers flip direction
— 215 against 4,256 — which is what a confound looks like when it stops
flattering the hypothesis.

**Attempt two — freeze the variable, not the subject.** `camfreeze` holds the
viewpoint still while the renderer keeps drawing every frame. It is deliberately
distinct from `dfreeze` (Ch. 25 §25.11), which stops the display task and freezes
the buffer along with it — a control that freezes the thing you are measuring
tells you nothing. With the camera pinned at `cell 3,11 frac 829,362`, frames
advanced 992 → 1061 in six seconds (~11.5 fps), and the framebuffer summary was
identical in both states.

**Attempt three — a checksum instead of a summary.** `fbsum` now emits `fbhash`,
FNV-1a over all 53,760 pixels, because a zero count, an equal-to-first count and
the first pixel of four rows are a *sample*, and two different pictures can share
them. Result: `fbhash=0x5b985ce0`, unchanged for 76 seconds and several hundred
rendered frames.

And the run proved nothing, because the picture never garbled during it. A
constant hash from a frozen camera rendering a correct scene is just a renderer
working. **The instrument was finally right and the bug declined to appear.**

Two things came out of it anyway:

- The camera was **`in open space`** in every sample. The flat-colour signature
  at `raycast.c` — *"buried itself in a wall, and every column rendered as one
  flat colour"* — is a documented failure of this renderer, and it is **not**
  what was happening here. `campos` asks `wall_at()` directly rather than
  inferring the answer from how flat the picture looks, which is the difference
  between a check and a rationalisation.
- Seventy-six seconds with the camera frozen and it never garbled once.
  Suggestive that the fault needs the viewpoint to be moving — but the view had
  already been open ~16 seconds before freezing, inside the window where it heals
  anyway, so this is a lead and not a result.

It was also, at the time, presented as settling render-versus-transport. It does
not settle it, for a reason that only became visible afterwards: a frozen camera
stably rendering a *wrong* scene produces a constant hash too. That retraction is
in Chapter 28 §28.12 and Appendix E §E.4.

## 28b.6 What the session actually found: DMA had been dead the whole time

`dmastat` prints the same two counters the boot self-test does. The difference is
when: the self-test reads them before the scheduler starts, and this reads them
from a system that has been running for minutes.

```
transfers 110507  timeouts 1   <- DMA is OFF, everything is on the FIFO path
```

One timeout, and one is enough. `spi2_dma_tx()` bounded its wait on **wall
clock** at 2,000,000 cycles — about 25 ms at 80 MHz. That is generous for a
480-byte transfer and far too short for the *wait*, because the wait can span a
scheduling round trip. A display task descheduled mid-transfer times out on a DMA
engine that is working perfectly.

A timeout is not a dropped frame. It sets `g_dma_ok = 0` **permanently**, and
every transfer for the rest of the run falls back to the 64-byte FIFO. The
`transfers` counter freezing at 110,507 across captures seven minutes apart is
that fact printed twice: nothing had used DMA since it died.

Which is why a full-view blit cost **55.8 ms** against 21.7 ms to compose and
2.3 ms to march. Getting the picture to the glass was roughly 70% of every frame,
on a path that was supposed to be the slow fallback and had become the only one.

The bound was raised to 40,000,000 cycles, about 500 ms. A genuinely stuck engine
is still caught; it costs half a second once instead of 25 ms. Chapter 18 §18.9
carries the source comment that came out of it, and its best line — *"a
performance number improving was the symptom"* — is this defect in seven words.

### 28b.6.1 A second defect, found here and deliberately left

When `spi2_dma_tx()` returns 0, `spi_tx()` re-sends **the whole buffer** over the
FIFO. The engine may already have shifted some or all of those bytes out. They
reach the panel twice and offset everything after them in the RAMWR stream — a
torn frame, by construction, at exactly the moment of a timeout.

It was left alone deliberately, so that the timeout fix could be tested as a
single variable (Ch. 29, Rule 12). It is fixed now: `spi_tx()` abandons the
transfer on timeout rather than falling through.

Both of those are real defects, correctly diagnosed, correctly fixed. Neither is
the fault this session was looking for, and the raised bound made the device
**worse** — because the spurious timeout was the only thing moving the system off
the broken path. Chapter 28 §28.11 is that shape in general form.

## 28b.7 Instruments that lied, session two

Chapter 28 catalogues the shapes. These are the five instances this session
contributed, kept together because they arrived together.

### 28b.7.1 The boot self-test's `dma=N/0`

`display_init()` prints the DMA counters as part of the boot report, and it read
zero timeouts every time. It runs **before the scheduler starts** — so it
measures the one condition in which nothing can preempt anything, which is
precisely the condition under which the bug cannot occur. That number was read as
proof the timeout never fires, and it retired theory #1 for most of the session.
`dmastat` exists because of it.

This is Chapter 28's Shape 6 exactly: a startup artefact standing in for
behaviour.

### 28b.7.2 Circular reasoning about a guard

Worse than the above, and self-inflicted. The bound was raised to 40,000,000,
`dmastat` was checked, it read **timeouts=0**, and that was taken as evidence the
raise had been unnecessary. The raise is *why* it read zero. The change was
reverted on that reasoning, and the defect stayed in the tree for several more
hours until the original bound was back in place long enough to read `1`.

> **A guard can only be shown unnecessary by measuring it in its absence.**

That is Rule 19 (Ch. 29 §29.6). The next session's report carries it forward
verbatim rather than citing it (Ch. 28c §28c.5), which is what this project does
with a rule it expects to need again before the reader reaches the reference.

### 28b.7.3 A summary mistaken for a checksum

`fbsum` reported a zero count, an equal-to-first count, and the first pixel of
four rows. Two states matching on all six numbers was called "identical
framebuffers". They are a sample; different pictures can share them. It now emits
`fbhash` over every pixel — and even that turned out to prove less than was
claimed for it (§28b.5).

### 28b.7.4 A test harness that skipped its own wait

The uptime comparison script accepted an `EARLY` delay parameter and never used
it. The "early" capture therefore ran at ~12 seconds of uptime, while the kernel
was still booting: `frames 0  columns 0`, and `act/tap/open=1/0/0` showing the
desktop still in launcher mode because the `view3d` command went out before the
shell was reading it. The entire early column of that comparison was void.

Shape 7, and the third time in this project (Ch. 28 §28.8).

### 28b.7.5 An empty column that looked like data

The same harness printed a `frames` field parsed by a matcher that returned the
empty remainder after the bare token `frames`. The column showed `7840` — which
was `equal-to-first` shifted over — and looked like a plausible, unchanging frame
count for thirty-eight consecutive samples.

The tell was there and it is a weak one: a frame count that never moves. On this
system that is either a stopped renderer or a broken parser, and the second is
not the one a reader thinks of first — which is why an instrument should print
*nothing* when it has nothing, rather than the last thing it happened to be
holding.

### 28b.7.6 Carried forward: `raycast_framebuffer()` returns a boolean

From the previous session and still worth its place: a diagnostic used it as a
pointer and stored through `0x00000001`. `raycast_fb_ptr()` now exists for the
pointer, and the naming is the whole defence (Ch. 25 §25.10, Ch. 28 §28.9).

## 28b.8 Instruments added

All read-only or opt-in; none change any existing code path unless invoked.
Appendix C carries them with their arguments.

| Command | Purpose |
|---|---|
| `dmastat` | the DMA counters from a **running** system, not from boot |
| `campos` | camera cell, sub-cell position, heading, and whether `wall_at()` says it is inside geometry |
| `camfreeze` | hold the viewpoint still while the renderer keeps drawing |
| `view3d` | open or close the 3D view from the terminal |
| `hog` / `hog draw` | spin like `task_apps`; `draw` also fills continuously |
| `resyncn` | a burst of window setups, no pixels |
| `stripn` | `gfxrogue`-shaped fills without `gfxrogue` |
| `fbsum` | now emits `fbhash`, FNV-1a over all 53,760 pixels |

`view3d` matters more than it looks. Every previous attempt to catch the startup
fault had failed the same way: the view was opened by hand, seconds passed before
anything could be armed, and the picture had already healed. **A startup failure
needs the instrument to exist before the first frame does** — which is the same
lesson as the boot self-test's `dma=320/0`, arriving from the other direction.

## 28b.9 What the session established, and what it did not

**Established, and still true:**

- DMA times out spuriously within seconds of every boot and is disabled for the
  run. Fixed.
- The renderer holds the draw lock essentially continuously; applications use
  `display_try_lock()` and skip rather than wait, so `cont=0` means *nobody is
  permitted to contend*, not *no contention exists*.
- Continuous interleaved drawing (`hog draw`, `gfxrogue`) repairs the view. A
  burst of the same drawing does not. Neither does spinning without drawing.
- The camera is **not** inside geometry when the view is wrong. That documented
  failure mode is not this one.
- Nothing sent to the panel — window setups, FIFO transfers, identical clipped
  rectangles — repairs the view on its own.

**Not established, as the session closed:**

- Whether the fault was in the renderer or in the transport. Three attempts, no
  answer; the one properly controlled run never reproduced the bug.
- What changes between 30 seconds and 7 minutes of uptime.
- Whether the low-uptime symptom is "opens garbled" or "does not open", which may
  be two faults reported as one.
- Why continuous drawing repairs it. `hog draw` is a reliable switch, not an
  explanation.

Every item in the second list has one answer, and it is one bit wide.

## 28b.10 What this chapter gets wrong, and why it is kept

The report this chapter synthesises ends with a section proposing that the repair
requires *continuous interleaved drawing*, and reasoning about what that implies
for the renderer's interaction with the display task. That reasoning is wrong.
The mechanism is in Chapter 28c §28c.3, and it is not about interleaving at all:
`gfxrogue` and `hog draw` provoke a **timeout**, and the timeout moves the system
off the DMA path onto a path where the defect does not exist.

The eleven eliminations in §28b.3 stand. So do both defects found in §28b.6. What
does not stand is the frame the session put around them — and the reason to keep
the chapter is that from inside, this session looks like good work. It measured
instead of arguing. It recorded negatives. It built instruments and distrusted
them. It got a real fix into the tree.

It also spent a day inside a configuration where the defect was not present,
because the system had moved itself there twelve seconds after every boot, and
nothing in the method as practised had a way to ask *is the machine I am
measuring the machine that has the bug?*

That question is Chapter 28c.

---

**Next:** the answer, in one bit, and the two instruments that finally asked the
question the right way round.
