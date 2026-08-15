# UM-CYDOS-017 — Touchscreen, and a Verification Method That Failed Three Times

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-15 · Status: **Complete, verified on hardware**

---

## 1. Abstract

The CYD's XPT2046 resistive touchscreen works: contact is detected through the
controller's PENIRQ line, and both axes are mapped to panel coordinates by
measurement rather than by reasoning.

The driver took four attempts, and only one of the four failures was in the
driver. Two were in `gpio.h`, and the fourth — the one worth reading this report
for — was in **how the results were being read**. §5 documents a serial capture
that rebooted the board and erased the evidence before printing it, and the two
conclusions recorded as fact on the strength of it.

This is the third time in two sessions that an instrument reported its own
reading invalid and the number beside it was believed anyway. §6 treats that as
the finding.

## 2. Hardware

| Signal | GPIO | Note |
|---|---|---|
| CLK | 25 | |
| MOSI | 32 | second GPIO bank |
| MISO | 39 | input-only pin, second bank |
| CS | 33 | second bank |
| IRQ (PENIRQ) | 36 | input-only pin, second bank |

Pins and calibration extremes are the board's, taken from the vendor driver.

**A separate bus from the display, which matters.** The display driver leaves CS
asserted between a window command and its pixel stream (UM-CYDOS-015 §4), so a
shared bus would mean either interleaving touch traffic into an open window or
serialising every touch read behind a 387 ms full-screen fill. Neither is
acceptable for a pointer.

Bit-banged, for the same reason the display is: it needs only GPIO registers, so
a failure can only be the panel, the pins, or the protocol.

## 3. Two defects that made a broken driver look plausible

### 3.1 `gpio.h` only ever handled pins 0–31

Pins 32–39 have entirely separate registers — `OUT1`, `ENABLE1`, `IN1` — and
**nothing complains when the wrong bank is used**. A shift of 39 on a 32-bit
register is undefined and reads zero in practice.

So MISO on GPIO 39 read a constant 0, and MOSI and CS on 32 and 33 were never
driven at all. Every accessor now selects its bank from the pin number, so a
caller never has to know the split exists.

This bug was latent from the moment `gpio.h` was written for the display, whose
pins are all below 32. It would have broken **any** peripheral above pin 31.

### 3.2 Silence read as maximum pressure

Pressure was computed as `z = z1 + 4095 - z2`. A controller returning zeros on
both channels yields **4095** — full scale. A dead bus therefore presented as a
permanent hard press, and the first run reported `s/e=10/10`: every sample a
touch, on a screen nobody was touching.

An all-zero reading is now rejected outright. The general point is worth
keeping: **a formula whose failure mode is a plausible extreme is worse than one
that fails to an obvious value**, because the first requires interpretation and
the second announces itself.

### 3.3 The first conversion is the one that matters

The control bytes originally set `PD1:PD0 = 00`, powering down the ADC and its
reference between every conversion. The first conversion after power-up is
specified as inaccurate — and `Z1`, the channel that decides whether a touch
exists, is the first conversion after CS goes low on every single read.

The burst now uses `PD=11` to keep the reference powered, with a throwaway
conversion absorbing the inaccurate reading, and a final `PD=00` to power down
and re-enable PENIRQ.

## 4. Detection: PENIRQ, not pressure

Both routes work. Measured under a real touch:

```
irq=17   z1max=1123   z2min=2992   zmax=2065
```

PENIRQ asserted 17 times, matching the event count exactly, and computed
pressure peaked at 2065 against a threshold of 300.

**PENIRQ is used** because it is a direct electrical indication from the panel,
needing no ADC, no threshold and no calibration. It is read *before* CS is
asserted, since the pin is only meaningful while the controller is idle.

Pressure is still recorded despite no longer being the detector. A channel that
never moves is itself evidence, and hiding it would only make the next person
repeat the measurement.

## 5. The capture was destroying its own data

### 5.1 Symptom

Three consecutive runs, across different builds and different user actions,
reported **exactly 150 samples**.

A counter that lands on an identical value every time is not accumulating. It is
measuring a fixed window.

### 5.2 Cause

Opening a serial port asserts DTR/RTS, which is wired to the ESP32's reset and
boot pins. Configuring them *after* opening still glitches EN. Every capture
described as "no reset" was rebooting the board, waiting for it to boot, and
then reading roughly twelve seconds of a system nobody was touching.

Setting `dtr` and `rts` **before** `open()` fixes it. The same read then reports
647,406 samples and no boot banner.

### 5.3 What it invalidated

Two statements were recorded as findings on data that had already been erased:

- *"pressure never exceeds 1 under a firm press"* — actual value **1123**
- *"PENIRQ never asserts across two full drags"* — actual count **17**

Both were measured on a board that had rebooted seconds earlier. The `PD=11` fix
of §3.3 had already worked when it was declared a failure.

Worse, the user reported that dots were following their finger, and that report
was overridden as "almost certainly leftovers" on the strength of the broken
measurement. **The human observation was the better evidence.**

### 5.4 Why it was hard to see

The reboot was invisible in every way that was being checked. Output looked
normal, because a booting kernel prints a full banner and then healthy report
lines. Values looked plausible, because they were real readings — of the wrong
interval. Only the sample counter carried the contradiction, and it was printed
in the same line as the values that were being trusted.

## 6. The finding

Three times across two sessions, an instrument reported its own reading invalid
and the value beside it was believed anyway:

| Instrument | What it said | What was believed instead |
|---|---|---|
| Marker block (UM-CYDOS-016 §3.4) | Frozen — the system is halted | The colour bars, which a frozen screen renders identically |
| `s/e=150`, three runs | This counter is not accumulating | The pressure values printed beside it |
| Boot banner in the capture | The board just restarted | The counters, as though continuous |

The peripherals were largely fine. The verification method was what kept
failing, and it failed in the same shape each time: **a signal that the
measurement was invalid, sitting next to a number that looked reasonable.**

What broke the deadlock, every time, was the same move — **latch the quantity so
timing cannot lie about it**, then feed the system a controlled input rather than
interpret an uncontrolled one. §7 is that method applied deliberately.

## 7. Calibration by controlled input

Watching a trail follow a finger can say *"the dots go the wrong way"*. It cannot
distinguish a **swapped** axis from a **flipped** one, and several build cycles
were spent guessing between those two before that was noticed.

Instrumenting the raw span of each channel answers both at once. Two drags, each
along one screen axis:

| Drag | `rx` span | `ry` span |
|---|---|---|
| Left → right | **486–3536** (3050) | 2499–2862 (363) |
| Top → bottom | 675–1518 (843) | **432–3204** (2772) |

The mirror image is the result. Each drag swept one channel across most of its
range while the other barely moved, which is precisely what a correct axis
assignment looks like.

Direction falls out of the endpoints: the horizontal drag ended at `rx=3527`
against a maximum of 3536, so raw X increases left to right. The vertical drag
ran from `ry=586` to `ry=2171`, so raw Y increases downward. Neither axis needs
a flip.

The original code had the axes transposed, on the reasoning that a portrait
display must swap a landscape-calibrated panel. **That reasoning was wrong**, and
no amount of looking at the screen would have said so.

## 8. Metrics

| Quantity | Value |
|---|---|
| Detection | PENIRQ, GPIO 36 |
| Poll rate | ~100 Hz (once per tick) |
| Conversions per read | 11, including one throwaway |
| Peak pressure measured | 2065 (threshold 300) |
| `z1` under touch | 1123, against 0 idle |
| `z2` under touch | 2992, against ~4090 idle |
| Raw span, horizontal drag | 3050 counts |
| Raw span, vertical drag | 2772 counts |
| Axis flips required | 0 |
| Native tasks | 10 |
| Image size | 21,888 B |
| Attempts before working | 4 |
| Conclusions retracted | 2 |

## 9. What this does not establish

- **No multi-touch.** A resistive panel cannot report two points; a second
  finger produces a reading somewhere between them.
- **No IRQ-driven wakeup.** PENIRQ is polled, not wired to an interrupt. Doing
  so would let the system idle until touched instead of sampling at 100 Hz.
- **No debounce or gesture recognition.** Every sample is independent; there is
  no tap, hold, drag or double-tap concept.
- **No touch access from applications.** There is no syscall, so bytecode cannot
  read the pointer. That is the natural counterpart to the display syscalls of
  UM-CYDOS-016 and would make applications interactive.
- **Calibration is the vendor's, not this board's.** The extremes were taken
  from the vendor driver and the measured spans are consistent with them, but no
  per-unit calibration pass has been run.
- **The cursor leaves a black square** where it has been, because nothing
  records what was underneath. Fixing that needs either a panel read-back this
  hardware does not reliably support, or damage tracking the display layer does
  not have.

## 10. References

- UM-CYDOS-015 — the display driver, and the shared-bus problem avoided here
- UM-CYDOS-016 §3.4 — the frozen marker, the first instance of the §6 pattern
- `kernel/gpio.h` — the two-bank split of §3.1
- `kernel/touch.c` — PENIRQ detection, PD handling, and the measured mapping
