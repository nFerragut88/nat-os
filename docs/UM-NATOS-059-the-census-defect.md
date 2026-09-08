# UM-NATOS-059 — The Census Defect

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-07 · Status: **A program written in NatScript, launched from an icon, owning the screen and reading a sensor — and five bugs that were all the same bug.**

---

## 1. Abstract

```
                       before          after
application slots           1               4
a program's canvas    180 x 14      240 x 202
```

This report covers `next_moves/08` steps **367–371**. UM-NATOS-058 covers
355–366.

`tools/app_meter.nat` is ninety lines of NatScript. Tapping its icon gives it
the main screen; it reads the light sensor ten times a second and sweeps a trace
of what it read. **No kernel change was needed to make it work** — that was the
point of the sixteen steps before it.

Getting there needed four limits removed, and **all four were the same defect**:
a statement that was true when it was written and became false when the system
grew by one. §3 is that pattern, and it is the report.

---

## 2. What changed

| step | |
|---|---|
| 367 | `ARENA_MAX` is `APP_MAX + 1`. The kernel had been renting an application's slot |
| 368 | `ping` and `pong` stop starting at boot. Four usable slots, from one |
| 369 | An application can own the main region: 240 x 202 instead of 180 x 14 |
| 370 | `app_meter.nat` — NatScript, an icon, the region, a sensor |
| 371 | Every manifest bound to the bytes it was written for |

---

## 3. The census defect

Four independent bugs in five steps, and one shape.

### 3.1 `ARENA_MAX 4`

`APP_MAX` was 4. So was `ARENA_MAX`. They read as *one arena per application*
and they were not: `kmain.c` takes one at boot for the kernel's own VM task and
never releases it. Four arenas meant **three** applications — and with `ping`
and `pong` running from boot, **one**.

`run paint` while anything else ran failed with *"no free slot or no memory"*
against 28,728 bytes of free heap.

Nobody chose that. Two independent limits happened to be the same number and one
of them silently had a tenant.

```c
#define ARENA_MAX (APP_MAX + 1)
```

with a `_Static_assert` naming the `+1`. The bare `4` was not wrong so much as
**unexplained** — it stated a quantity where a relationship was meant.

### 3.2 `else { raycast_frame(); }`

Reported by the user as: *"why is paint opening up 3D view? a bit strange"*.

```c
if      (desktop_active()) desktop_frame();
else if (desktop_notes())  notes_frame();
...
else                       raycast_frame();      /* <- */
```

Correct for as long as `MODE_3D` was the only mode not named above it. **That is
not a branch. It is a claim that no seventh mode will ever exist**, and it was
wrong within an hour of one being added: the raycaster rendered over the
program's region.

The chain names `desktop_3d()` now and has no `else`. A mode added later and
forgotten shows a stale region — a visible bug — rather than inheriting the
raycaster, which looks like a feature misbehaving.

### 3.3 `if (down && !desktop_active())`

The same assumption, one file over, in the touch path: steer the raycaster from
any touch in the top 224 rows of *any* view. Invisible while nothing else used
that region, wrong the moment something did. It asks for `desktop_3d()` too.

### 3.4 `cmd_run`'s list of reasons

Step 364 had already fixed a diagnostic that named two causes without saying
which — *"no free slot or no memory"* — after it cost a wasted reflash chasing
the wrong one.

**Three steps later it came back.** Adding the manifest check gave `launch` a
third way to fail, and `cmd_run` did not know:

```
refused: tamper does not match the manifest it was built with (...)
cannot start tamper: the heap could not find 512 bytes; 3232
```

The second line is false and is the first thing a reader would act on.

`cmd_run` *enumerated* its callee's failure modes, which is wrong every time one
is added. The failure now says whether it has already spoken; the caller stays
quiet.

### 3.5 The shape

| what it said | when it stopped being true |
|---|---|
| `ARENA_MAX 4` | the kernel took one |
| `else` = the 3D view | a seventh mode existed |
| `!desktop_active()` = steering | a program owned the region |
| two reasons a launch fails | there were three |

**Each was a census taken at the time of writing, hardened into a constant, a
default branch, or a list.** None of them was wrong on the day it was written.
Every one was wrong on the day the count changed, and none of them said what
they depended on — which is why nothing caught them and a person had to.

The correction is the same in all four: **state the relationship, not the
tally.** `APP_MAX + 1` with an assert. A branch per mode with no `else`. A
callee that reports whether it has explained itself.

This is the same family as step 356's permission bitmap, which mirrored
`device.c`'s table order by hand with nothing checking the two lists against
each other — available for two hundred steps and never fired.

---

## 4. An application can own the screen

A VM program's canvas was **180 x 14**: enough for one line of text. Every
NatScript program written for it was really a serial-console program with a
status line.

Tapping a program icon now hands it **240 x 202**, nineteen times the area.

### 4.1 The safety property is the top 22 rows

The program's viewport starts *below* the bar carrying its name and the X, so it
cannot paint over the way out — and `desktop_chrome_touch()` checks that button
before anything else. A program that hangs, fills its canvas, or draws a
convincing fake bar inside its own region still cannot take the real one away.

That is the argument `app.h` already made about the strips' close button —
*"the one control the user needs in order to escape a misbehaving program is the
one control that program cannot touch"* — applied to a region nineteen times the
size. **The rule scaled; only the numbers changed.**

### 4.2 Ownership has to be given back

Three ways a program stops owning the region, all of which had to be handled or
the desktop sits on a canvas nobody owns with the way back drawn over a dead
screen: the X, the program stopping, and another view opening.

`apply_view()` is one function deciding what viewport a program should have,
because three call sites deciding it independently is how a program ends up
drawing in two places or in none.

---

## 5. `app_meter`

Ninety lines of NatScript, and it uses everything the language picked up over
the previous sixteen steps and nothing else:

| | |
|---|---|
| `permissions { light }` | the manifest, resolved to a device **by name** at load |
| `every 100ms` | the poll; `light` is slow, so a read ends the slice |
| `format(line, "light ", v, ...)` | the readout |
| `v * GH / peak` | **signed** division — the fix the board itself found at 363 |
| `screen.fill` on 240 x 202 | the region from §4 |

Measured on the board rather than inferred from the constants — the program was
asked to print its own canvas:

```
act/tap/open=0/2/1          the icon was tapped, one view opened
[meter] canvas 240x202      what screen.width/height returned
started id=0 perms=light    the manifest resolved by name
```

The same source printed `180x14` when launched from the shell an hour earlier,
which is the other half of the same check.

**Two decisions inside the program.** The scale is *discovered*: nobody knows
what an LDR reads in somebody's room, so `peak` grows to fit, because a fixed
divisor either flattens the trace against the floor or clips it against the
ceiling depending on the weather. And the trace *sweeps* rather than scrolls —
scrolling means keeping the history and repainting it every frame; sweeping is
one column cleared and one drawn, which over SPI is the difference between a
smooth trace and a visible repaint.

`pong` lost its icon to make room. The grid holds nine and cannot grow — 224
pixels will not fit a fourth row — so an application with an icon costs another
one its place. `run pong` still works.

---

## 6. VM-08, and the part that cannot be built here

`device.h` has carried one sentence since permissions were written:

> *"A permission grant is only meaningful if the image it applies to cannot be
> swapped for another, and nat-os has no image identity."*

**A content hash cannot deliver that.** Programs are `static const` arrays
*inside the kernel image*; a hash stored in that same image is rewritten by
anyone who can rewrite the program it describes. What the sentence requires is a
root of trust the attacker cannot reach — ESP32 secure boot with a key burned
into **eFuses**, one-time programmable and unrecoverable if wrong. That is not
something to do to somebody's only board while passing through.

So VM-08 stays open, and the header now says why rather than saying nothing.

### 6.1 What was built instead

The narrower property, which is real: **the manifest is bound to the bytes it
was written for.** FNV-1a over each image *and* its permission names, recomputed
at load, compared before anything is resolved or granted.

It catches a stale generated header, a table entry pointing at another program's
array, and a manifest edited without rebuilding. Mechanical failures with no
attacker in them — and each one would let a program run under permissions
reviewed for different code. It becomes load-bearing the moment a program
arrives from anywhere but the kernel image.

**FNV-1a deliberately.** 32 bits is the right size for detecting drift and the
wrong size for resisting an adversary. Reaching for SHA-256 would dress a drift
check as a signature, which is the failure this project has spent three reports
on.

### 6.2 Proved, not asserted

`run tamper` is a permanent table entry carrying `counter`'s image and
`squares`' id:

```
   launch    refused: tamper does not match the manifest it was built with
                      (id 0xa68614fb, expected 0x23068aee)
```

If it ever starts, the check has stopped working and every other id in the table
is decoration.

---

## 7. The instrument, again

UM-NATOS-058 §7 recorded four rounds lost to broken measurement rather than a
broken system. This stretch added three more, and the honest reading is that
this is now the dominant failure mode of the work rather than an anecdote.

| | |
|---|---|
| the USB link | dropped repeatedly. Five flashes failed with *"Could not open COM5"* seconds after a check that had just seen the port — so *"paint is still opening the 3D view"* was reported against an image that never contained the fix |
| the retry loop | ran the flash **eight times**, because its success check matched the wrong output line. It succeeded on the first; the other seven were redundant writes to a part with a finite erase count |
| the step log | 369 shipped with **no entry at all**: the command that wrote it failed to parse and nothing checked. Written at 370 and labelled late rather than backdated |

The board was working every time. The pattern from 058 holds and has got
sharper: **measure the measurer first**, and check that the thing which was
supposed to record the work actually did.

---

## 8. What remains

1. **VM-08 proper** (§6) — secure boot and eFuses, deliberately not done.
2. **`APP_MAX` 4**, pinned by geometry rather than memory: the strip band is
   224 to 288, exactly four at 16 pixels each.
3. **`ping`'s hardcoded peer id.** It sends to application 1, written into its
   bytecode. The boot path used to guarantee that ordering and no longer does;
   a send to the wrong slot is refused rather than misdelivered, so it degrades
   to silence.
4. **Per-task stack sizing.** 368's measurement is the argument against doing it
   from a snapshot: `net` showed 332 bytes used, and the log records it hitting
   1,384 under lwIP load. It would return 8–10 KB to a heap with 28 KB free.
5. **The null-`sp` window fault** — identified at 351, never reproduced.

**A language, a compiler, a test suite, a manifest bound to its image, and a
program somebody could have written running full-screen on hardware that had
none of it eighteen steps ago.**
