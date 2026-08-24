# Chapter 23 — Audio, and Three Ways to Be Silent

> Sources: `docs/UM-NATOS-027-audio.md`
> Code: `kernel/audio.c`, `kernel/audio.h`, `kernel/gpio.h`

---

## 23.1 The smallest feature in the book, and the longest debugging story

> The first output this system has that is not the screen.
>
> The feature is small: hardware PWM on GPIO26, a tone, a beep, and a click on
> every keypress. The report is mostly about the debugging, because getting from
> "a speaker is plugged into the connector labelled SPEAK" to a sound took three
> separate faults stacked on top of one another, and **each one made the next one
> invisible.**

## 23.2 Why it exists at all

Not novelty:

> Multi-tap's worst property is that a press registering is invisible —
> UM-NATOS-022 §3.4 records that the press which "did not register" is usually
> one that did, and then replaced the letter you wanted. A click resolves that
> using none of the 224 rows every other part of the interface is competing for.

Feedback that costs no screen space is worth a great deal on a device where
Chapter 24 §24.7 records that **every row of the panel is allocated with none
spare**.

## 23.3 Tones, not sample playback

> Sample playback needs a clock at 8 kHz against a 100 Hz tick, which means
> either a dedicated timer interrupt or I²S with DMA. **This kernel's interrupt
> matrix has never delivered a peripheral interrupt end to end**, so PCM would
> mean debugging audio and interrupts simultaneously, and silence is a far less
> legible symptom than a counter that fails to advance.
>
> LEDC needs none of that machinery. Given a divider it generates the waveform in
> hardware — no CPU, no interrupt, no DMA, no task, no buffer — so tones cost
> nothing that is not already proven.

The same staging principle as bit-banging the display first, applied to a
different axis: build the thing whose failure has *one* candidate.

## 23.4 The three faults

### 1. The driver muted its own pin

The first implementation used the DAC's cosine generator, which requires setting
`MUX_SEL` on the pad to hand it to the RTC subsystem.

> **Once RTC owns a pad, the digital GPIO matrix cannot drive it.**
>
> So `audio_init()`, running at boot, muted GPIO26 for every subsequent
> experiment. The DAC did not work *and* the square-wave probes aimed at the same
> pin were writing to a pad that was not listening. Fifteen pins were swept in
> two rounds, all silent, and the mute was this file's own initialisation.

A driver's `init()` sabotaging every subsequent diagnostic of itself is a
particularly nasty shape, because the diagnostics look independent and are not.

### 2. The check that could not fail

Fourteen silent pins is also exactly what a broken generator looks like, so the
generator was checked — correctly, in principle. `spktest` drives GPIO32, the one
pin this kernel can both drive and measure, and confirmed 351 edges against ~400
expected with the pad swinging 1829 against 0.

> **GPIO32 has no RTC mux.** It is structurally incapable of exhibiting §1's
> fault. The test passed, and its conclusion — *"generator and pad both work; the
> silence is the hardware"* — was reported with confidence and was wrong, sending
> the search toward connectors and amplifiers.

> **Verifying an instrument on the one case that cannot fail is not
> verification.** The limitation is now printed by the probe itself, so the next
> person to run it is told what it does not cover before they read the result.

This is the third member of the family with Chapter 21 §21.4 (four IO_MUX entries
all on the same side of the anomaly) and Chapter 19 §19.8 (a direction test that
could only return one answer). All three were real tests, correctly performed,
structurally unable to fail.

The mitigation here is the most direct of the three: **the probe prints its own
blind spot.**

### 3. A peripheral that was switched off

With the pad released, LEDC still produced nothing — while **every register read
back byte-for-byte correct**:

```
timer0_conf = 0x021631ca   matching the computed divider exactly
SIG_OUT_EN  set
duty        50%
GPIO26      routed to signal 71
```

> LEDC comes out of reset clock-gated. **A gated peripheral accepts register
> writes and does nothing.**

The fix is two lines:

```c
    REG(0x3FF000C0u) |=  (1u << 11);    /* DPORT_PERIP_CLK_EN: LEDC */
    REG(0x3FF000C4u) &= ~(1u << 11);    /* DPORT_PERIP_RST_EN: LEDC out of reset */
```

with the observation that made it findable:

```c
 * LEDC comes out of reset clock-gated, and a gated peripheral ACCEPTS REGISTER
 * WRITES and does nothing. ... [SPI2 and GPIO are ungated by]
 * the bootloader before this kernel runs. LEDC is the first peripheral here
 * [that this kernel has had to switch on itself].
```

### The shape, three times in one project

> | symptom | reality |
> |---|---|
> | ADC returning the same value on every channel | converting, attached to nothing |
> | GPIO edge latched, CPU never interrupted | delivered to the APP CPU, which is halted |
> | LEDC configured perfectly, silent | clock gated off |
>
> **A read-back cannot detect any of them.** The display and touch never needed a
> clock gate touched because SPI2 and GPIO are ungated by the bootloader; LEDC is
> the first peripheral this kernel has had to switch on itself.

Chapter 27 §27.2 adds a fourth and answers it differently — by looking for
free-running counters rather than by reading registers at all.

## 23.5 And then the frequency

With all three fixed, **440 Hz was still inaudible. 3 kHz was immediately clear.**

> 440 Hz is unremarkable for a speaker in general and beyond this one. **A
> correct tone at a frequency the transducer cannot move is indistinguishable
> from a broken driver**, and it cost a full round of debugging after the driver
> was already working.
>
> Everything meant to be heard now sits near 3 kHz, which is also roughly where
> human hearing is most sensitive — so the same drive is louder for free.

The shell's help text carries the warning where somebody will actually read it:

```
    tone <hz>     tone on gpio26; 'tone 0' stops. try 3000, not 440
```

## 23.6 What actually broke the deadlock

Not a probe.

> The user said **"it works fine on my other project."**
>
> One sentence eliminated the entire hardware half of the search space — which
> was precisely the half §2 had concluded was at fault. Every tool built that
> evening was aimed at the wrong hypothesis, and none of them could have
> disconfirmed it, because they all tested the same layer.

> **The question worth asking earlier is not "what else can I measure" but "what
> already works that this can be compared against". A working reference is
> stronger evidence than any amount of instrumentation on the broken thing.**

That is the second time in this book a human observation outranked the
instruments — Chapter 19 §19.4's *"the dots are following my finger"*, overridden
as "almost certainly leftovers" and correct — and Chapter 27 §27.6 adds a third
where a user contradicted a confident hardware diagnosis and was right.

## 23.7 The driver

```c
#define LEDC_BASE               0x3FF59000u
#define LEDC_HSCH0_CONF0_REG    (LEDC_BASE + 0x0000u)
#define LEDC_HSCH0_HPOINT_REG   (LEDC_BASE + 0x0004u)
#define LEDC_HSCH0_DUTY_REG     (LEDC_BASE + 0x0008u)
#define LEDC_HSCH0_CONF1_REG    (LEDC_BASE + 0x000Cu)
#define LEDC_HSTIMER0_CONF_REG  (LEDC_BASE + 0x0140u)
#define LEDC_CONF_REG           (LEDC_BASE + 0x0190u)

#define TICK_SEL_APB    (1u << 25)      /* timer counts APB, not REF_TICK */
#define TIMER0_RST      (1u << 24)
#define DIV_NUM_S       5u              /* 18 bits, 10.8 fixed point      */
#define SIG_OUT_EN      (1u << 2)
#define DUTY_START      (1u << 31)
#define DUTY_S          4u              /* the duty field carries 1/16ths */

#define LEDC_HS_SIG_OUT0    71u         /* GPIO matrix signal index */

#define SPK_PIN         26u
#define SPK_MUX_REG     0x3FF49028u

/* 10-bit period: deep enough that 50% duty is exactly 50%, shallow enough that
 * the divider stays inside 18 bits down to about 80 Hz. */
#define DUTY_RES        10u
#define DUTY_PERIOD     (1u << DUTY_RES)

#define APB_HZ          80000000u
```

Offsets fetched rather than recalled, and the file says so:

```c
 * Offsets and field positions came from the vendor's ledc_reg.h and
 * gpio_sig_map.h, fetched rather than recalled.
```

Starting a tone:

```c
    REG(LEDC_HSTIMER0_CONF_REG) = TICK_SEL_APB
                                | /* ... divider ... */;
    REG(LEDC_HSTIMER0_CONF_REG) |= TIMER0_RST;      /* latch the divider */
    REG(LEDC_HSTIMER0_CONF_REG) &= ~TIMER0_RST;

    REG(LEDC_HSCH0_DUTY_REG)  = (DUTY_PERIOD / 2u) << DUTY_S;
    REG(LEDC_HSCH0_CONF1_REG) = DUTY_START;
    REG(LEDC_HSCH0_CONF0_REG) = SIG_OUT_EN;         /* timer 0, output on */
```

Clocked from APB at 80 MHz, which this project has *measured* (Chapter 7 §7.8),
so the pitch is exact rather than nominal against an untrimmed RC oscillator. The
report makes that point explicitly as a reason to prefer LEDC over the DAC.

### And why LEDC was the better choice anyway

```c
 * LEDC replaces it, and is the better fit regardless. It is hardware PWM: set a
 * divider and it generates the waveform continuously with no CPU, no interrupt
 * and no DMA — the properties the cosine generator was chosen for — and it is
 * clocked from APB at 80 MHz, which this project has measured, so the pitch is
 * exact rather than nominal against an untrimmed RC oscillator.
```

An accidental improvement: the workaround for a fault turned out to be a better
design than the thing it replaced.

## 23.8 Verification

```
tone 3000        -> audible
tone 0           -> stops
open shell, tap keys  -> a click on every accepted press
open notes, tap keys  -> the same
```

The click is 3 kHz for 2 ticks (~20 ms), and it fires on every *accepted* press —
which is the property that makes it useful feedback rather than noise. A press
rejected as chatter does not click, so the click means "this one counted".

## 23.9 Metrics

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
| **Faults stacked** | **3** |

## 23.10 What this does not establish

- **No sample playback.** Tones only; there is no PCM path.
- **No volume control.** Duty is fixed at 50%. "Varying it changes timbre and
  loudness together on a square wave, and nothing here has tried."
- **One channel.** LEDC has sixteen; one timer and one channel are configured.
- **The speaker's response is uncharacterised.** 440 Hz is known inaudible and
  3 kHz known clear. Nothing between has been measured, so "usable from ~1 kHz"
  is an estimate.
- **No applications can make a sound.** No syscall.
- **Clicks are unconditional.** Every accepted keypress clicks, with no way to
  silence it, "which is a preference nobody has been asked for".
- **The DAC is untouched now.** Whether the cosine generator could have been made
  to work was never established:

  > it was abandoned once LEDC worked, so §23.4 records a fault in this driver,
  > not a verdict on the peripheral.

  A careful distinction, and the right one: the DAC was never shown to be broken,
  only never shown to work.

---

**Part IV ends here.** Nine drivers: display, touch, flash, microSD, the
interrupt matrix, ADC, I²C, audio, and — in Chapter 27 — an 802.11 receiver. Four
of them produced a register that read back perfectly while the hardware did
nothing, and each was caught by a different technique: a control group, an
injected edge, a moving-word scan, and a person saying "it works on my other
project".

**Part V** is what a user actually sees.
