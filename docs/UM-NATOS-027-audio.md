# UM-NATOS-027 — Audio, and Three Ways to Be Silent

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-16 · Status: **Working on hardware**

---

## 1. Abstract

The first output this system has that is not the screen.

The feature is small: hardware PWM on GPIO26, a tone, a beep, and a click on
every keypress. The report is mostly about the debugging, because getting from
"a speaker is plugged into the connector labelled SPEAK" to a sound took three
separate faults stacked on top of one another, and **each one made the next one
invisible**.

The immediate use is not novelty. Multi-tap's worst property is that a press
registering is invisible — UM-NATOS-022 §3.4 records that the press which "did
not register" is usually one that did, and then replaced the letter you wanted.
A click resolves that using none of the 224 rows every other part of the
interface is competing for.

## 2. Why tones and not sample playback

Sample playback needs a clock at 8 kHz against a 100 Hz tick, which means either
a dedicated timer interrupt or I²S with DMA. **This kernel's interrupt matrix
has never delivered a peripheral interrupt end to end** (UM-NATOS-023 §7), so
PCM would mean debugging audio and interrupts simultaneously, and silence is a
far less legible symptom than a counter that fails to advance.

LEDC needs none of that machinery. Given a divider it generates the waveform in
hardware — no CPU, no interrupt, no DMA, no task, no buffer — so tones cost
nothing that is not already proven.

## 3. The three faults

### 3.1 The driver muted its own pin

The first implementation used the DAC's cosine generator, which requires setting
`MUX_SEL` on the pad to hand it to the RTC subsystem. **Once RTC owns a pad, the
digital GPIO matrix cannot drive it.**

So `audio_init()`, running at boot, muted GPIO26 for every subsequent
experiment. The DAC did not work *and* the square-wave probes aimed at the same
pin were writing to a pad that was not listening. Fifteen pins were swept in two
rounds, all silent, and the mute was this file's own initialisation.

### 3.2 The check that could not fail

Fourteen silent pins is also exactly what a broken generator looks like, so the
generator was checked — correctly, in principle. `spktest` drives GPIO32, the
one pin this kernel can both drive and measure, and confirmed 351 edges against
~400 expected with the pad swinging 1829 against 0.

**GPIO32 has no RTC mux.** It is structurally incapable of exhibiting §3.1's
fault. The test passed, and its conclusion — *"generator and pad both work; the
silence is the hardware"* — was reported with confidence and was wrong, sending
the search toward connectors and amplifiers.

> Verifying an instrument on the one case that cannot fail is not verification.
> The limitation is now printed by the probe itself, so the next person to run
> it is told what it does not cover before they read the result.

### 3.3 A peripheral that was switched off

With the pad released, LEDC still produced nothing — while **every register read
back byte-for-byte correct**: `timer0_conf = 0x021631ca` matching the computed
divider exactly, `SIG_OUT_EN` set, duty at 50%, GPIO26 routed to signal 71.

LEDC comes out of reset clock-gated. A gated peripheral accepts register writes
and does nothing.

That is the third appearance of this shape in one project:

| symptom | reality |
|---|---|
| ADC returning the same value on every channel | converting, attached to nothing (UM-NATOS-024 §3) |
| GPIO edge latched, CPU never interrupted | delivered to the APP CPU, which is halted (UM-NATOS-023 §5.1) |
| LEDC configured perfectly, silent | clock gated off |

**A read-back cannot detect any of them.** The display and touch never needed a
clock gate touched because SPI2 and GPIO are ungated by the bootloader; LEDC is
the first peripheral this kernel has had to switch on itself.

## 4. And then the frequency

With all three fixed, 440 Hz was still inaudible. 3 kHz was immediately clear.

440 Hz is unremarkable for a speaker in general and beyond this one. **A correct
tone at a frequency the transducer cannot move is indistinguishable from a
broken driver**, and it cost a full round of debugging after the driver was
already working.

Everything meant to be heard now sits near 3 kHz, which is also roughly where
human hearing is most sensitive — so the same drive is louder for free.

## 5. What actually broke the deadlock

Not a probe. The user said **"it works fine on my other project."**

One sentence eliminated the entire hardware half of the search space — which was
precisely the half §3.2 had concluded was at fault. Every tool built that
evening was aimed at the wrong hypothesis, and none of them could have
disconfirmed it, because they all tested the same layer.

> The question worth asking earlier is not "what else can I measure" but "what
> already works that this can be compared against". A working reference is
> stronger evidence than any amount of instrumentation on the broken thing.

## 6. Verification

```
tone 3000        -> audible
tone 0           -> stops
open shell, tap keys  -> a click on every accepted press
open notes, tap keys  -> the same
```

## 7. Metrics

| Quantity | Value |
|---|---|
| Pin | GPIO26 (SPEAK connector) |
| Generator | LEDC high-speed timer 0, channel 0 |
| Clock | APB, 80 MHz — pitch is exact |
| Duty resolution | 10 bits, fixed 50% |
| Frequency range | ~78 Hz to APB/1024; **usable from ~1 kHz on this speaker** |
| Click | 3 kHz, 2 ticks (~20 ms) |
| CPU cost while sounding | none |
| Pins swept before the fault was found | 15 |
| Faults stacked | 3 |

## 8. What this does not establish

- **No sample playback.** §2. Tones only; there is no PCM path and no way to
  play a recorded sound.
- **No volume control.** Duty is fixed at 50%. Varying it changes timbre and
  loudness together on a square wave, and nothing here has tried.
- **One channel.** LEDC has sixteen; one timer and one channel are configured.
- **The speaker's response is uncharacterised.** 440 Hz is known inaudible and
  3 kHz known clear. Nothing between has been measured, so "usable from ~1 kHz"
  is an estimate, not a measurement.
- **No applications can make a sound**, like every other peripheral here — there
  is no syscall (UM-NATOS-022 §2).
- **Clicks are unconditional.** Every accepted keypress clicks, with no way to
  silence it, which is a preference nobody has been asked for.
- **The DAC is untouched now.** Whether the cosine generator could have been
  made to work was never established — it was abandoned once LEDC worked, so
  §3.1 records a fault in this driver, not a verdict on the peripheral.

## 9. References

- UM-NATOS-022 §3.4 — the invisible keypress this exists to fix
- UM-NATOS-023 §5.1, §7 — the halted-CPU delivery, and why PCM waits on interrupts
- UM-NATOS-024 §3 — the ADC attached to nothing; the same shape as §3.3
- `kernel/audio.c` — the driver, and the probes with their limits recorded
