# Chapter 19 — Touch: Four Attempts, and an Axis Inverted for Three Months

> Sources: `docs/UM-NATOS-017-touch.md`
> Code: `kernel/touch.c`, `kernel/touch.h`, `kernel/gpio.h`, `kernel/calib.c`

---

## 19.1 The summary

The XPT2046 resistive touchscreen works: contact is detected through PENIRQ
*and* pressure, and both axes are mapped to panel coordinates by measurement
rather than by reasoning.

> The driver took four attempts, and **only one of the four failures was in the
> driver.** Two were in `gpio.h`, and the fourth — the one worth reading this
> report for — was in **how the results were being read.**

And then, three months later, a fifth:

> The horizontal axis had been **inverted since the driver was written**, and the
> calibration not only missed it but concluded the opposite — because it inferred
> direction from the last sample of a drag, which is the one sample guaranteed to
> be invalid.

## 19.2 Hardware

| Signal | GPIO | Note |
|---|---|---|
| CLK | 25 | |
| MOSI | 32 | second GPIO bank |
| MISO | 39 | input-only pin, second bank |
| CS | 33 | second bank |
| IRQ (PENIRQ) | 36 | input-only pin, second bank |

**A separate bus from the display, and that matters:**

> The display driver leaves CS asserted between a window command and its pixel
> stream, so a shared bus would mean either interleaving touch traffic into an
> open window or serialising every touch read behind a 387 ms full-screen fill.
> Neither is acceptable for a pointer.

Bit-banged, for the same reason the display is: it needs only GPIO registers, so
a failure can only be the panel, the pins, or the protocol.

`gpio.h` notes why MISO and IRQ are where they are:

```c
/* Touch. GPIO 34-39 are INPUT ONLY on this part, which is why MISO and IRQ sit
 * on 39 and 36 — they can never be driven by mistake. */
```

## 19.3 Two defects in `gpio.h` that made a broken driver look plausible

### `gpio.h` only ever handled pins 0–31

> Pins 32–39 have entirely separate registers — `OUT1`, `ENABLE1`, `IN1` — and
> **nothing complains when the wrong bank is used**. A shift of 39 on a 32-bit
> register is undefined and reads zero in practice.
>
> So MISO on GPIO 39 read a constant 0, and MOSI and CS on 32 and 33 were never
> driven at all.

The scar is recorded in the header:

```c
/* TWO BANKS. Pins 0-31 and pins 32-39 have entirely separate registers, and
 * nothing complains if the wrong one is used — a shift of 39 on a 32-bit
 * register is undefined and in practice reads zero. That is indistinguishable
 * from a device wired correctly but not answering, which is exactly how the
 * touch controller first presented: MISO on GPIO 39 read a constant 0, and
 * MOSI and CS on 32 and 33 were never driven at all. */
#define GPIO_OUT_W1TS_REG     0x3FF44008u   /* pins 0-31  */
#define GPIO_OUT_W1TC_REG     0x3FF4400Cu
#define GPIO_OUT1_W1TS_REG    0x3FF44014u   /* pins 32-39 */
#define GPIO_OUT1_W1TC_REG    0x3FF44018u
```

Every accessor now selects its bank from the pin number:

```c
/* Every accessor below picks its bank from the pin number, so a caller never
 * has to know the split exists. */

static inline void gpio_set(uint32_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_OUT_W1TS_REG)  = 1u << pin;
    } else {
        GPIO_REG(GPIO_OUT1_W1TS_REG) = 1u << (pin - 32u);
    }
}
```

> This bug was latent from the moment `gpio.h` was written for the display, whose
> pins are all below 32. It would have broken **any** peripheral above pin 31.

And the file's opening comment is candid about the claim it originally made:

```c
 * The claim that this file needed no pins above 31 was already false when it
 * was written; the touch controller sits on 32, 33, 36 and 39, and the two-bank
 * split below is the scar from discovering that.
```

The interrupt registers have the same split, and there it is *dangerous* rather
than merely annoying:

```c
 * Two banks again, and here the split is dangerous rather than merely annoying:
 * a status bit left set holds the shared source asserted, so failing to clear
 * the right bank's bit hangs the machine in the handler instead of dropping an
 * event.
```

### Silence read as maximum pressure

Pressure is computed as `z = z1 + 4095 - z2`. A controller returning zeros on
both channels yields **4095** — full scale.

> A dead bus therefore presented as a permanent hard press, and the first run
> reported `s/e=10/10`: every sample a touch, on a screen nobody was touching.

An all-zero reading is now rejected outright:

```c
    /* Pressure from the two Z channels. A light touch gives a small z1 and a
     * large z2; the difference is what separates contact from noise.
     *
     * A controller that is not answering returns 0 on every channel, and that
     * formula turns 0,0 into 4095 — maximum pressure. Silence would read as a
     * permanent hard press, which is how a dead bus first presented here. An
     * all-zero reading is therefore rejected outright: no touch produces a
     * z1 of exactly zero. */
    int answered = (z1 != 0u) || (z2 != 0u);
    uint32_t z = answered ? ((z1 + 4095u) - z2) : 0u;
```

The general rule, which recurs in the I²C driver (Chapter 22 §22.7):

> **a formula whose failure mode is a plausible extreme is worse than one that
> fails to an obvious value**, because the first requires interpretation and the
> second announces itself.

### The first conversion is the one that matters

```c
/* Control bytes. Bit 7 starts a conversion; bits 6:4 select the channel; bit 3
 * clear selects 12-bit; bit 2 clear selects differential mode, which cancels
 * the supply variation single-ended mode is sensitive to.
 *
 * Bits 1:0 are PD1:PD0, the power-down mode, and they were the defect. With
 * PD=00 the ADC and its reference power down between every conversion, and the
 * first conversion after power-up is specified as inaccurate. Z1 is the first
 * conversion after CS goes low on every single read, so the one channel that
 * decides whether a touch exists was always the throwaway sample: z1 never
 * exceeded 1 across 150 samples including a firm held press.
 *
 * PD=11 keeps the reference and ADC powered for the whole burst. The final
 * command uses PD=00 to power down again and re-enable PENIRQ, which is what
 * makes the IRQ line usable later. */
#define PD_ON   0x03u
#define CMD_Y   (0x90u | PD_ON)
#define CMD_X   (0xD0u | PD_ON)
#define CMD_Z1  (0xB0u | PD_ON)
#define CMD_Z2  (0xC0u | PD_ON)
#define CMD_IDLE 0x90u              /* PD=00: power down, PENIRQ back on */
```

The burst absorbs the inaccurate reading with an explicit throwaway:

```c
    /* Throwaway. Powers the reference up and absorbs the inaccurate first
     * conversion so it cannot land on a channel whose value is load-bearing. */
    (void)convert(CMD_Z1);

    uint32_t z1 = convert(CMD_Z1);
    uint32_t z2 = convert(CMD_Z2);
```

and again per axis:

```c
    /* Four conversions per axis, kept individually. */
    uint32_t sx[4], sy[4];
    for (int i = 0; i < 4; i++) {
        sx[i] = convert(CMD_X);
    }
    for (int i = 0; i < 4; i++) {
        sy[i] = convert(CMD_Y);
    }
    (void)convert(CMD_IDLE);        /* power down, re-enable PENIRQ */
    uint32_t rx = (sx[1] + sx[2] + sx[3]) / 3u;   /* first discarded */
    uint32_t ry = (sy[1] + sy[2] + sy[3]) / 3u;
```

Eleven conversions per read, of which two are deliberately discarded.

## 19.4 The capture was destroying its own data

*This is the section the whole report is named for.*

### The symptom

> Three consecutive runs, across different builds and different user actions,
> reported **exactly 150 samples**.
>
> A counter that lands on an identical value every time is not accumulating. It
> is measuring a fixed window.

### The cause

> Opening a serial port asserts DTR/RTS, which is wired to the ESP32's reset and
> boot pins. Configuring them *after* opening still glitches EN. Every capture
> described as "no reset" was rebooting the board, waiting for it to boot, and
> then reading roughly twelve seconds of a system nobody was touching.
>
> Setting `dtr` and `rts` **before** `open()` fixes it. The same read then
> reports 647,406 samples and no boot banner.

### What it invalidated

Two statements were recorded as findings on data that had already been erased:

- *"pressure never exceeds 1 under a firm press"* — actual value **1123**
- *"PENIRQ never asserts across two full drags"* — actual count **17**

Both measured on a board that had rebooted seconds earlier. The `PD=11` fix had
already worked when it was declared a failure.

And the worst part:

> the user reported that dots were following their finger, and that report was
> overridden as "almost certainly leftovers" on the strength of the broken
> measurement. **The human observation was the better evidence.**

### Why it was hard to see

> The reboot was invisible in every way that was being checked. Output looked
> normal, because a booting kernel prints a full banner and then healthy report
> lines. Values looked plausible, because they were real readings — of the wrong
> interval. **Only the sample counter carried the contradiction, and it was
> printed in the same line as the values that were being trusted.**

### It happened again, with new tools

> Investigating the inverted axis needed a way to read board state while a finger
> was on the glass. Two scripts were written for it, both described as "no
> reset", and **both reset the board**.
>
> So the same failure this section documents was reproduced, by someone who had
> just read this section, in tools written specifically to avoid it. Two rounds
> of four-corner taps were destroyed between being recorded and being read.

The fix was known; what was missing was a *check*:

> What is new is the verification — uptime is now sampled twice across a
> connection and confirmed to increase, rather than the absence of a reset being
> assumed. **Knowing the failure mode was not enough; a check was needed.**

## 19.5 The finding

Three times across two sessions, an instrument reported its own reading invalid
and the value beside it was believed anyway:

| Instrument | What it said | What was believed instead |
|---|---|---|
| Marker block | Frozen — the system is halted | The colour bars, which a frozen screen renders identically |
| `s/e=150`, three runs | This counter is not accumulating | The pressure values printed beside it |
| Boot banner in the capture | The board just restarted | The counters, as though continuous |

> The peripherals were largely fine. **The verification method was what kept
> failing**, and it failed in the same shape each time: a signal that the
> measurement was invalid, sitting next to a number that looked reasonable.
>
> What broke the deadlock, every time, was the same move — **latch the quantity
> so timing cannot lie about it**, then feed the system a controlled input rather
> than interpret an uncontrolled one.

Latching is now a habit in this driver:

```c
/* Extremes since boot. Confirming what the pressure channels do under a finger
 * needs a capture to coincide with the press, which is not something that can
 * be arranged reliably. Latching the extremes makes the question answerable at
 * any later moment instead. */
```

and in the press log:

```c
        /* Record the FIRST sample of each press. A held finger yields hundreds
         * of samples and only the moment of contact is a known screen
         * position; recording every sample would fill the log with the drift
         * of a finger settling. */
        if (!g_was_down && g_log_count < TOUCH_LOG_MAX) {
            touch_log_t *e = &g_log[g_log_count++];
            e->raw_x = rx;
            e->raw_y = ry;
            e->x     = out->x;
            e->y     = out->y;
            e->z     = out->z;
        }
```

Chapter 28 counts these alongside four more.

## 19.6 PENIRQ answers a different question than it was asked

### The original reasoning, which was sound

```
irq=17   z1max=1123   z2min=2992   zmax=2065
```

PENIRQ asserted 17 times, matching the event count exactly, and computed pressure
peaked at 2065 against a threshold of 300.

> **PENIRQ is used** because it is a direct electrical indication from the panel,
> needing no ADC, no threshold and no calibration. It is read *before* CS is
> asserted, since the pin is only meaningful while the controller is idle.

Pressure was kept anyway:

> A channel that never moves is itself evidence, and hiding it would only make
> the next person repeat the measurement.

### And the conclusion was still wrong

> The reasoning above is sound and the conclusion was still wrong, which is an
> uncomfortable combination worth stating precisely.
>
> PENIRQ reports whether a finger is **there**. It says nothing about whether the
> position sample taken alongside it is **valid**. Those are different questions,
> and this section answered the second by measuring the first.

Measured on the launcher, within a single tap:

```
z=7     raw_x=4095   ->  x=0     approach, input floating
z=2025  raw_x=1280   ->  x=167   the actual finger
z=20    raw_x=4095   ->  x=0     release
```

> A factor of a hundred separates the populations, and `Z_THRESHOLD` — 300,
> defined in `touch.c` and unused since it was written — sits cleanly between
> them.

And the consequence, invisible until something depended on horizontal position:

> **4095 is near the top of the range, so a rail reading maps to one edge of the
> screen.** Every spurious sample votes for the same place. In the launcher that
> place was the leftmost column, and it looked exactly like a broken axis rather
> than a valid-sample problem.

The fix is one expression, and the source records both halves of the reasoning:

```c
    /* PENIRQ says a finger is NEAR; pressure says it has actually landed.
     *
     * Both are now required, and the reason is not that PENIRQ was wrong.
     *
     * UM-NATOS-017 §4 chose PENIRQ over pressure with pressure working fine —
     * it measured a peak of 2065 against a threshold of 300 — on the grounds
     * that PENIRQ is a direct electrical signal needing no ADC, no threshold
     * and no calibration. That reasoning is still correct for the question it
     * answered.
     *
     * It answered the wrong question. PENIRQ reports whether a finger is
     * THERE; it says nothing about whether the position sample taken alongside
     * it is VALID. ...
     *
     * The rail maps to x=0, so every spurious sample votes for the leftmost
     * column — which is exactly what the launcher did once the axis inversion
     * stopped masking it. Two populations separated by a factor of a hundred,
     * and a threshold that has been sitting unused in this file the whole
     * time. */
    out->down  = pen && (z > Z_THRESHOLD);
```

A constant defined at the start of the project and first used in revision 1.2 —
which means the *knowledge* was there and the *connection* was not.

## 19.7 Calibration by controlled input

Watching a trail follow a finger can say *"the dots go the wrong way"*. It cannot
distinguish a **swapped** axis from a **flipped** one, and several build cycles
were spent guessing between those two before that was noticed.

Instrumenting the raw span of each channel answers both at once. Two drags, each
along one screen axis:

| Drag | `rx` span | `ry` span |
|---|---|---|
| Left → right | **486–3536** (3050) | 2499–2862 (363) |
| Top → bottom | 675–1518 (843) | **432–3204** (2772) |

> The mirror image is the result. Each drag swept one channel across most of its
> range while the other barely moved, which is precisely what a correct axis
> assignment looks like.

The original code had the axes transposed, "on the reasoning that a portrait
display must swap a landscape-calibrated panel. **That reasoning was wrong**, and
no amount of looking at the screen would have said so."

## 19.8 The direction test was decided by its worst sample

*The conclusion above about direction is wrong, and the method that produced it
is why.*

Raw X does not increase left to right. It decreases. Measured by tapping four
labelled corners:

| corner | `raw_x` | `raw_y` |
|---|---|---|
| top-left | **3360** | 416 |
| top-right | **591** | 376 |
| bottom-left | 3258 | **3518** |
| bottom-right | 376 | **3462** |

> Left reads ~3300, right reads ~480. The axis assignment is correct — `raw_x`
> really is horizontal — but its **sign** is backwards, and has been since the
> driver was written.

The span measurement was never the problem. The *inference* was:

> *"the horizontal drag ended at `rx=3527` against a maximum of 3536, so raw X
> increases left to right"*
>
> That reads the **last sample of a drag**, which is the release sample — and
> release samples read the ADC rail. A rail reading is 4095, near the top of any
> range, so a drag that ends anywhere at all "ends near its maximum", and
> **every axis appears to increase.** The test could only ever return one answer.

And:

> The same corrupted sample later decided the launcher's icon selection, three
> months apart, in a different file. **One defect, two symptoms.**

### The rule

> **Never infer direction from the endpoint of a gesture.** A drag produces a
> cloud of samples whose first and last are the two least trustworthy, because
> both sit at a contact transition. Inferring direction from precisely those two
> is the method's flaw, not a slip in applying it.
>
> Four **labelled** taps have no such moment. Each is a known screen position
> paired with a raw reading, and the direction of every axis falls out of
> comparing labels rather than trusting an endpoint.

The corner table is now recorded in `touch.c` *as the calibration*, rather than
the two derived limits it produces, so a future re-calibration can check the
derivation instead of repeating the guess.

### Why nothing noticed for three months

> A backwards horizontal axis had no consumer:
>
> - the application strips are full width, so horizontal position never mattered
> - the raycaster steers from the left and right thirds of the view, so a
>   reversed axis makes it turn the other way — **which reads as a control
>   preference**
> - the containment tests check that a touch is confined to the right viewport,
>   which is a question about `y`
>
> The launcher is the first thing in this project that asks **where across the
> screen** a finger is. It failed on the first tap.

## 19.9 The corners were the wrong four points

The labelled-points method is right, and it still chose the four places on the
panel where a labelled point is least trustworthy.

> **A finger cannot reach the extreme corner of a bezelled panel.** Every corner
> reading was therefore short of the true extreme, the derived range came out
> narrower than reality, and — because the range is the *divisor* — every raw step
> counted for too many pixels. The mapping magnified position about the centre of
> the screen.

| | corner-derived span | measured span | error |
|---|---|---|---|
| X | 3000 | 3694 | 23% |
| Y | 3140 | 3484 | 11% |

The consequence is what made it hard to characterise:

| true screen x | old mapping returned | error |
|---|---|---|
| 30 | ~1 | −29 px |
| 120 | ~112 | −8 px |
| 210 | ~222 | +12 px |

> The error is proportional to distance from centre **and changes sign**.
> UM-NATOS-022 §3.2 described this as "about 24 px low on X", which is a fair
> reading of one symptom at one place on the screen and is not what was wrong.
> **No fixed offset could have corrected a magnification**, which is why every
> attempt to nudge the constants moved the fault somewhere else rather than
> removing it.

Chapter 26 §26.4 records the note pad's own correction of that description, and
it is the sharpest statement of the trap:

> The number 24 was real. It was measured in one place, on keys near the middle,
> and then written down as though it were a property of the panel rather than of
> where it happened to be measured. **A single sample of a position-dependent
> quantity looks exactly like a constant.**

## 19.10 The fix: four inset targets

`kernel/calib.c` — four targets inset 30 px from the edges, tapped on the device.

```c
/* nat-os — touch calibration by tapping targets. See calib.h.
 *
 * Four targets, INSET from the edges. That inset is the entire point.
 *
 * ... A finger cannot reach the extreme corner of a
 * bezelled panel, so every corner reading was short of the true extreme, the
 * derived range came out narrower than reality, and every mapped coordinate
 * landed inward of the finger — about 24 px on X by the time it reached the
 * middle of the screen. That was enough to make a 24 px keyboard key type its
 * neighbour.
 *
 * Targets at 30 px in are reachable, so the readings are real positions rather
 * than the closest a fingertip could get to an unreachable one. The mapping is
 * then interpolated outward to the edges instead of extrapolated inward from a
 * guess.
 */

#define INSET   30u
#define TARGETS 4u

/* Screen positions of the targets, in the order they are asked for. Two on each
 * side of both axes, so each axis is fitted from an average of two readings and
 * a single bad tap cannot define the whole scale. */
static const uint32_t TX[TARGETS] = { INSET, DISP_W - INSET, INSET,          DISP_W - INSET };
static const uint32_t TY[TARGETS] = { INSET, INSET,          DISP_H - INSET, DISP_H - INSET };
```

### The pair check, which caught the first run

Two targets share each screen coordinate, so every reading has a partner it
should nearly match:

```c
/* The four pairs that must agree: two targets share a screen x, two share a
 * screen y, so each reading has a partner that should nearly match it. */
static const uint32_t PAIR_A[4] = { 0u, 1u, 0u, 2u };
static const uint32_t PAIR_B[4] = { 2u, 3u, 1u, 3u };
static const char *const PAIR_AXIS[4] = { "x", "x", "y", "y" };

/* How far two readings that should match may differ before the run is refused.
 * The usable raw span is about 3000 and a fingertip is worth a few hundred of
 * it, so 900 admits a sloppy tap and rejects a tap on the wrong cross. */
#define PAIR_TOLERANCE 900u
```

```c
    /* Check the pairs before fitting anything.
     *
     * Averaging two readings guards against one sloppy tap, but it also hides a
     * tap on the wrong cross: 3453 and 353 average to 1903, which looks like a
     * perfectly ordinary number and is a measurement of nothing. The averaged
     * fit then fails its own sanity check and reports "readings too close
     * together", which is true of the averages and says nothing about the
     * mistake that produced them.
     *
     * Two targets share each screen coordinate, so every reading has a partner
     * it should nearly match. Comparing them first turns a vague rejection into
     * a specific one. */
    g_disagree = -1;
    for (int p = 0; p < 4; p++) {
        const uint32_t *v = (p < 2) ? g_raw_x : g_raw_y;
        uint32_t a = v[PAIR_A[p]], b = v[PAIR_B[p]];
        uint32_t d = (a > b) ? a - b : b - a;
        if (d > PAIR_TOLERANCE) {
            g_disagree = p;
            break;
        }
    }
```

> The first run on hardware was refused by this check — two targets had been
> tapped on the wrong cross, and 3453 and 353 average to 1903, an entirely
> ordinary-looking number that measures nothing.

The rejection message is *specific*, which is the difference between a check and
a complaint:

```c
            uart_puts("REJECTED - targets ");
            uart_put_dec(PAIR_A[g_disagree] + 1u);
            uart_puts(" and ");
            uart_put_dec(PAIR_B[g_disagree] + 1u);
            uart_puts(" share a screen ");
            uart_puts(PAIR_AXIS[g_disagree]);
            uart_puts(" but read far apart.\n");
            uart_puts("             tap the cross that is SHOWING, one at a time.\n");
```

### The outcome is held, not just printed

```c
/* The outcome of the last run, kept rather than only printed.
 *
 * The result line is emitted the instant the fourth target is tapped, which is
 * exactly when nobody has a capture attached — the same way the touch log's
 * values were lost before it was changed to hold them. A number that has
 * already scrolled past is not a measurement. */
static int      g_last_ok = -1;     /* -1 never run, 0 rejected, 1 installed */
```

> That is §19.4's failure in a new costume — **the third time in this project a
> measurement was written into a stream nobody was reading** — so `calshow`
> reports the four readings, the outcome, and the calibration in use, and can be
> asked at any later time.

### The result is read back, not assumed

```c
    touch_set_calibration(xmin, xmax, ymin, ymax);

    /* Read back what was actually installed rather than what was computed:
     * touch_set_calibration() refuses a degenerate range, and reporting the
     * request instead of the result would hide that. */
    uint32_t ax, bx, ay, by;
    touch_get_calibration(&ax, &bx, &ay, &by);
    /* ... */
    uart_puts(ax == xmin ? "   (installed)\n" : "   (REFUSED, kept previous)\n");
```

And the refusal itself has a self-preservation argument:

```c
void touch_set_calibration(uint32_t xmin, uint32_t xmax,
                           uint32_t ymin, uint32_t ymax)
{
    /* Refuse a degenerate or inverted range rather than installing it. A bad
     * calibration makes the panel unusable, which also makes it impossible to
     * run the calibration routine again — the failure would be
     * self-perpetuating. */
    if (xmax <= xmin + 100u || ymax <= ymin + 100u) {
        return;
    }
    /* ... */
}
```

### Persisted and restored

```
touch cal    : restored x 141..3835 y 243..3727
```

Record version 3 (Chapter 20 §20.7), restored at boot before anything can be
touched.

## 19.11 The mapping

```c
static uint32_t map_axis(uint32_t raw, uint32_t lo, uint32_t hi, uint32_t span)
{
    if (raw <= lo) {
        return 0;
    }
    if (raw >= hi) {
        return span - 1u;
    }
    return ((raw - lo) * span) / (hi - lo);
}
```

```c
        out->x = map_axis(rx, g_cal_x_min, g_cal_x_max, DISP_W);
#if TOUCH_X_INVERTED
        out->x = (DISP_W - 1u) - out->x;
#endif
        out->y = map_axis(ry, g_cal_y_min, g_cal_y_max, DISP_H);
```

The calibration values are runtime, not compile-time:

```c
/* Runtime, not compile-time, so a calibration routine can replace them and a
 * saved result can be restored at boot. The values below are the defaults from
 * the corner measurement described above — usable, and wrong by about 24 px on
 * X for the reason in the next paragraph. */
static uint32_t g_cal_x_min = 380u;     /* right edge — the LOW end of raw_x */
static uint32_t g_cal_x_max = 3380u;    /* left edge                         */
static uint32_t g_cal_y_min = 380u;     /* top                               */
static uint32_t g_cal_y_max = 3520u;    /* bottom                            */
```

Note the comments on `x_min` and `x_max`: *right edge* is the LOW end. The
inversion is documented at the point where somebody would otherwise misread the
names.

## 19.12 Input reaches applications

Covered in Chapter 17 §17.7. The measurement:

```
touch g/w = 81/109211        last = ...->191,174
```

Two defects it exposed are recorded there and in Chapter 16 §16.4: launching by
index, and the kernel drawing a cursor over application viewports.

The second is worth repeating here because it is an ownership argument rather
than a safety one:

> A kernel-drawn touch cursor painted over whatever strip the finger was in —
> exactly what the kernel forbids applications from doing to each other. Not
> unsafe, since the kernel is trusted, but **wrong about ownership**: those
> pixels belong to whoever owns the strip.
>
> The cursor is removed. An application that wants pointer feedback draws it
> itself, through `SYS TOUCH` and `SYS FILL`, in its own coordinates and inside
> its own viewport — which is the entire point of the syscall.

## 19.13 Metrics

| Quantity | Value |
|---|---|
| Detection | PENIRQ (GPIO 36) **and** pressure |
| Poll rate | 100 Hz, at HIGH priority, real 10 ms sleep |
| Conversions per read | 11, including two throwaways |
| Peak pressure measured | 2065 (threshold 300) |
| `z1` under touch | 1123, against 0 idle |
| `z2` under touch | 2992, against ~4090 idle |
| Raw span, horizontal drag | 3050 counts |
| Raw span, vertical drag | 2772 counts |
| `raw_x` at the left edge | ~3300 |
| `raw_x` at the right edge | ~480 |
| Direction of `raw_x` | **decreases** left → right |
| Rail samples per tap, before the pressure gate | ~2 of 3 |
| Pressure, real contact vs approach/release | ~2000 vs ~10 |
| Months the X axis was inverted | ~3 |
| Attempts before working | 4 |
| Conclusions retracted | 2 |
| Tools written to avoid §19.4's failure that reproduced it | 2 of 2 |
| Calibration error, corner-derived | 23% on X, 11% on Y |
| Touch samples per reporter interval, before / after the sleep fix | ~15 / ~150 |

## 19.14 What this does not establish

- **No multi-touch.** A resistive panel cannot report two points.
- **No IRQ-driven wakeup — but no longer for want of an interrupt.** PENIRQ is
  routed to CPU line 23 and its handler runs (Chapter 22). What has never been
  observed is a *finger* causing that to happen.

  > The gap moved from "not wired" to "wired and unexplained", which is progress
  > of a sort and is not the same as working.
- **No debounce or gesture recognition.** Every sample is independent.
- **No pointer-up event.** An application cannot distinguish a held finger from a
  rapid sequence of contacts.
- **One pointer, shared.** All applications read the same snapshot; nothing
  arbitrates focus.
- **The calibration is this board's, but only this board's**, run once, with one
  finger. The vendor extremes remain the compiled-in defaults and are known to be
  wrong by 23% on X.
- **The calibration is still linear.** Nothing measures panel non-linearity
  *between* the target positions.
- **Nothing detects that a calibration has gone stale.**
- **`Z_THRESHOLD` is a single number chosen from one measurement**, on this panel,
  on this day, with this finger. A lighter touch has not been tested.
- **No filtering beyond the gate.** No median-of-N, no hysteresis.
- **Nothing re-checks the axis at runtime.**

---

**Next:** state that survives a power cycle, and a read defect that pointed at
the wrong register for a day.
