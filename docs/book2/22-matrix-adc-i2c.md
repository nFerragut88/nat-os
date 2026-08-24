# Chapter 22 — The Interrupt Matrix, the ADC, and I²C

> Sources: `docs/UM-NATOS-023-interrupts.md`, `docs/UM-NATOS-024-adc.md`, `docs/UM-NATOS-025-i2c.md`
> Code: `kernel/intr.c`, `kernel/intr.h`, `kernel/adc.c`, `kernel/i2c.c`, `kernel/gpio.h`

---

These three drivers were built in one session and belong in one chapter, because
they share a theme that the display and touch chapters only touched: **a register
that reads back correctly is not evidence that anything happened.** Between them
they produced three distinct instances of it, and Chapter 23 adds a fourth.

---

# Part A — The Interrupt Matrix

## 22.1 Everything was polled, and nothing said so

> Until this work, **every peripheral in this kernel was polled**, and nothing
> said so. The Level-3 vector served exactly one source — CCOMPARE1, which is
> internal to the Xtensa core and reaches the CPU without passing through any
> routing at all.
>
> That is a hard wall for anything serviced on the hardware's schedule rather
> than ours.

## 22.2 Sources are not lines

```c
 * On the ESP32 a peripheral cannot reach the CPU by itself. Each of ~70
 * peripheral interrupt SOURCES is routed, through a DPORT map register, onto
 * one of 32 CPU interrupt LINES. Those are different numbering spaces and
 * confusing them is the classic failure: writing the source number into
 * INTENABLE enables an unrelated line, and the peripheral stays silent while
 * every register involved reads back exactly as intended.
 *
 *   source  (0..69)  what raised it     — GPIO, I2S0, UART1, TG0_T0 ...
 *   line    (0..31)  what the CPU sees  — fixed priority level, fixed type
 *
 * A line's priority level and edge/level type are properties of the silicon,
 * not choices. Line 23 is used here because it is level-triggered at level 3,
 * so it shares the existing Level-3 vector and its proven context save rather
 * than needing a second one.
 */
```

Reusing the existing vector is the same economy as choosing CCOMPARE1 in
Chapter 7: fewer new things to be wrong about.

The map register address is computed, and the derivation is checked against two
independently known points:

```c
/* The PRO CPU's map registers are one word per source, in the silicon's source
 * order, starting at the MAC source. The address is therefore computed rather
 * than tabulated — but the INDEX still has to be right, which is why intr.h
 * names the sources instead of letting callers pass a number.
 *
 * Checked against two independently known registers: TG0_T0 is source 14 and
 * maps to 0x3FF0013C, UART0 is source 34 and maps to 0x3FF0018C. Both fall out
 * of this base. */
#define DPORT_PRO_MAP_BASE  0x3FF00104u
#define DPORT_PRO_MAP(src)  (DPORT_PRO_MAP_BASE + 4u * (src))
```

Two samples, on either side of the sources this kernel uses — which is the
straddling discipline of Chapter 21 §21.4, applied correctly this time.

## 22.3 The vector could only ever have been right by accident

`_handler_level3` called `timer_isr` unconditionally.

> Correct while CCOMPARE1 was the only source that could reach level 3, and a
> **clock corruption** the moment a second one is routed: `timer_isr()` advances
> the tick deadline by a whole interval every time it runs, so any GPIO interrupt
> would silently push the clock into the future.
>
> That is the same defect as the `task_yield()` bug, arrived at from the opposite
> direction, and it would have been just as quiet — sleeps running long, timeouts
> running loose, and every frame rate measured against a clock that drifts.

The fix is one line of assembly — `call0 intr_dispatch` instead of
`call0 timer_isr` — and the vector records why:

```asm
    /* Service whichever level-3 sources are actually pending.
     *
     * This called timer_isr directly until the interrupt matrix existed, which
     * was correct while CCOMPARE1 was the only source that could reach level 3.
     * It stops being correct the moment a second one is routed: timer_isr
     * advances the tick deadline by a whole interval every time it runs, so a
     * GPIO interrupt arriving here would silently push the clock forward — the
     * same corruption as the yield bug in timer.c, from the other direction.
     *
     * intr_dispatch reads the INTERRUPT register and dispatches on fact. */
    call0    intr_dispatch
```

**"Dispatches on fact"** rather than on an assumption about who could have fired.

## 22.4 The defence against a hang

```c
void intr_dispatch(void)
{
    uint32_t pending = xt_get_interrupt() & xt_get_intenable();

    /* The tick first, and by name. It is not a matrix source — CCOMPARE1 is
     * internal to the core — so it has no handler in the table. */
    if (pending & (1u << INTR_LINE_TIMER1)) {
        g_count[INTR_LINE_TIMER1]++;
        timer_isr();
        pending &= ~(1u << INTR_LINE_TIMER1);
    }

    while (pending) {
        uint32_t line = 31u - (uint32_t)__builtin_clz(pending);
        pending &= ~(1u << line);

        if (g_handler[line]) {
            g_count[line]++;
            g_handler[line]();
            continue;
        }

        /* An enabled, pending, unhandled LEVEL-triggered line is a hang, not a
         * glitch: nothing clears the condition, so returning re-enters this
         * handler immediately and the machine never runs task code again. There
         * is no output from that state and no watchdog reach — the watchdog is
         * fed by a task, and tasks have stopped running.
         *
         * So the line is masked instead. The kernel loses that interrupt and
         * keeps running, which is recoverable and inspectable ('intr' in the
         * shell reports it) rather than a silent brick. */
        g_spurious++;
        g_disabled |= (1u << line);
        xt_disable_interrupt(line);
    }
}
```

Note the masking with `INTENABLE` — the warning attached to `xt_get_interrupt()`
in Chapter 7 §7.6 is honoured here.

And routing installs the handler *first*:

```c
    /* Handler first. The matrix write can make the line fire immediately if the
     * peripheral is already asserting, and a line that fires before its handler
     * exists is a spurious interrupt that the defence in intr_dispatch() would
     * then permanently disable — the routing would appear to have failed. */
    g_handler[line] = fn;

    DPORT_REG(DPORT_PRO_MAP(source)) = line;
    xt_enable_interrupt(line);
```

A two-line ordering that prevents the safety mechanism from sabotaging the thing
it protects.

## 22.5 Four ways to deliver an edge to nobody

> The theme: at every stage, an edge was generated correctly, routed correctly,
> and consumed by something that could not act on it. **No register ever read
> back wrong.** Only counters distinguished these.

### 1. Delivered to a halted CPU

`GPIO_PINn_INT_ENA` is a 5-bit field at 17:13 selecting which CPU and which kind
of interrupt receives the pin. Bit 13 was chosen as the low bit of the field,
matching the vendor header's `PRO_CPU_INTR_ENA` being `BIT(0)`.

> Real taps latched `GPIO_STATUS1` bit 4. The map register read 23. `INTENABLE`
> had bit 23. The CPU's `INTERRUPT` register never showed it.
>
> The pin was being delivered to the **APP CPU, which this kernel never starts**
> — arriving perfectly at a core that was halted.

The answer was *measured*, not read:

```c
void intr_selftest(void)
{
    uint32_t saved = DPORT_REG(GPIO_PIN36_REG);
    int found = -1;

    uart_puts("   injecting an edge for each INT_ENA bit:\n");

    for (uint32_t bit = 13u; bit <= 17u; bit++) {
        DPORT_REG(GPIO_PIN36_REG)     = 0u;                  /* disable */
        DPORT_REG(GPIO_STATUS1_W1TC_A) = 1u << IRQ_PIN_BIT;  /* clear stale */

        uint32_t before = g_count[INTR_LINE_GPIO];

        DPORT_REG(GPIO_PIN36_REG)      = (2u << 7) | (1u << bit);
        DPORT_REG(GPIO_STATUS1_W1TS_A) = 1u << IRQ_PIN_BIT;  /* inject */

        for (volatile int i = 0; i < 2000; i++) {
        }

        uint32_t after = g_count[INTR_LINE_GPIO];

        uart_puts("     bit ");
        uart_put_dec(bit);
        uart_puts(after != before ? "  -> SERVICED\n" : "  -> nothing\n");
        if (after != before && found < 0) {
            found = (int)bit;
        }
        /* ... restore ... */
    }

    if (found >= 0) {
        uart_puts("   PRO CPU enable is bit ");
        uart_put_dec((uint32_t)found);
        uart_puts("; restoring with it\n");
        DPORT_REG(GPIO_PIN36_REG) = (2u << 7) | (1u << (uint32_t)found);
    }
    /* ... */
}
```

> **The measured answer is bit 15.** `intr_selftest()` settles it by injecting an
> edge through `GPIO_STATUS1_W1TS` once per candidate bit and reporting which the
> handler actually saw. Running `irqtest` re-derives the constant on any board
> rather than trusting the comment that records it.

`gpio.h` carries the constant with the whole story attached:

```c
/* GPIO_PINn_REG: INT_TYPE is bits 9:7, and INT_ENA is a 5-bit field at 17:13
 * selecting which CPU and which kind of interrupt the pin is delivered to.
 *
 * Bit 15 is THIS CPU's ordinary interrupt, and that number is measured rather
 * than read. The obvious choice — bit 13, the low bit of the field, matching
 * the vendor header's PRO_CPU_INTR_ENA being BIT(0) — produced a pin that
 * latched its edge in GPIO_STATUS1 and never reached the processor. It was
 * being delivered to the APP CPU, which this kernel never starts, so the edge
 * arrived correctly at a core that was halted.
 *
 * That failure is worth naming because every register involved read back
 * exactly as intended. */
#define GPIO_PIN_INT_ENA_PRO  (1u << 15)
```

Software injection mattered for a second reason:

> it isolates the status→CPU path from the pad→status path. The alternative was
> asking someone to tap a panel five times while watching a serial log.

### 2. The driver interrupting itself

> `touch.c` already knew that "a conversion in progress drives PENIRQ regardless
> of whether anything is touching". That is as true of the *interrupt* as of the
> *level*, and the driver had not been read that way.
>
> Every `touch_read()` manufactured its own edges — **3,917 across a handful of
> taps, none caused by a finger.** Each one set the "edge arrived with nobody
> waiting" latch, so the idle wait returned immediately, forever. The driver had
> interrupted itself into a busy loop.

Fixed by masking the pin for the duration of the burst:

```c
    /* Mask PENIRQ for the duration of the burst.
     *
     * The comment above says a conversion drives this pin regardless of whether
     * anything is touching, and that is exactly as true of the INTERRUPT as of
     * the level. Every read therefore manufactured its own edges: 3900 of them
     * across a handful of taps, none caused by a finger, each one indexing the
     * "an edge arrived with nobody waiting" latch and making the idle wait
     * return immediately forever. The driver was interrupting itself into a
     * busy loop.
     *
     * Masking here means the only edges that survive are the ones a finger
     * caused. An edge genuinely missed during the burst costs nothing: the burst
     * only runs while the task is awake and already sampling. */
    gpio_int_disable(PIN_IRQ);
```

The knowledge was already in the file. What was missing was reading it as a
statement about the *interrupt* rather than about the *level*.

### 3. Re-arming against a pin a finger is holding down

> The mask was then released at the end of `touch_read()`, which looked right and
> was the same bug moved. The pad is still **LOW** at that point if a finger is
> on it, and arming falling-edge detection against an already-low pin latches an
> edge immediately.
>
> Result: 100 edges, 0 wakes — every one self-inflicted, every one while the task
> was awake with no waiter registered.

```c
    /* Deliberately does NOT re-arm. Arming happens in touch_irq_wait().
     *
     * Re-arming here looked right and was the second version of this bug. The
     * pad is still held LOW by a finger at this point, and enabling falling-edge
     * detection against an already-low pin latches an edge immediately — so
     * every read produced an interrupt, always while the task was awake and had
     * no waiter registered. The counters said 100 edges and 0 wakes, which is
     * exactly what a self-inflicted edge looks like from the outside.
     *
     * The interrupt is only ever useful while the task is idle and the pen is
     * up. Arming it anywhere else can only manufacture events. */
```

The generalisation — *arm an edge detector only in the state where an edge is
meaningful* — is now enforced by having exactly one arming site.

`gpio_int_enable()` clears stale status first, for the third variant of the same
hazard:

```c
/* Enables interrupts on a pin. ...
 *
 * Clears any stale status first. A pin that was already asserting before its
 * enable bit was set has a status bit set, and enabling into that state fires
 * the handler for an edge that happened before anyone was listening. */
static inline void gpio_int_enable(uint32_t pin, uint32_t type)
{
    gpio_int_clear(pin);
    GPIO_REG(GPIO_PIN_REG(pin)) = GPIO_PIN_INT_TYPE(type) | GPIO_PIN_INT_ENA_PRO;
}
```

### 4. A diagnostic that was itself wrong

> The first register dump read `0x3FF44078` for the PRO CPU's masked status and
> reported a confident zero. The per-CPU status registers sit between `STATUS1`
> and `PIN0` in an order taken from memory, and that address is most likely the
> *other* CPU's copy.
>
> Printing the whole range and letting the value identify the register found bit
> 4 set at `0x3FF44074` — which is what pointed at §1.

> **A diagnostic derived from the same faulty recollection as the code it is
> checking will agree with the code and both will be wrong. Dump the range, not
> the address you believe in.**

The dump now does exactly that:

```c
    /* The per-CPU masked status registers sit between STATUS1 and PIN0, and
     * their exact order is the thing in question — the first attempt at this
     * dump read 0x78 and reported a confident zero from what is probably the
     * APP CPU's copy. Printing the range and letting the value identify the
     * register is the same move as the calibration's pair check: do not ask the
     * hardware to confirm a guess, ask it what is there.
     *
     * GPIO 36 is bank-1 bit 4, so the register showing 0x10 is the one that
     * matters, and whether it shows it at all is the actual finding. */
    for (uint32_t a = 0x3FF44064u; a <= 0x3FF44084u; a += 4u) {
        uart_puts("     [");
        uart_put_hex(a);
        uart_puts("] = ");
        uart_put_hex(DPORT_REG(a));
        uart_puts("\n");
    }
    uart_puts("     expect: one of the above = 0x10 if the pin reaches the PRO CPU\n");
```

Printing the *expectation* alongside the data is what makes the output
self-interpreting.

## 22.6 What is verified, and what is switched off

```
irqtest   -> bit 15 SERVICED          injected edge reaches the handler
intr      -> tick line 15 serviced N  dispatch does not disturb the clock
             spurious 0               nothing masked defensively
             armed rb 0x00008100      INT_TYPE=falling + enable, read back
                                      from inside the armed window
```

That last line is a methodological point:

> sampling `GPIO_PIN36` from the shell returned zero three times running against
> a window covering 93% of the cycle. That is not chance — **the shell's sampling
> is correlated with the touch task being awake.** Only the armed task could
> answer without the measurement being correlated with its own subject.

An observer whose scheduling is coupled to the thing it observes cannot sample it.

### And a finger has never once woken the touch task

> With everything above confirmed, real taps produced 24 touch events and 30 low
> PENIRQ readings while the armed edge detector latched nothing. **That gap is
> unexplained.**
>
> The touch task is therefore back to polling — byte for byte its previous
> behaviour — and `touch_irq_wait()` is intact, one line from reinstatement.
>
> Shipping touch input on an interrupt whose end-to-end path has never been
> observed to work would trade a mechanism that demonstrably works for one that
> only should.

The remaining suspects, all untested:

- the pad→status edge path may need `FUN_IE` or a GPIO-matrix input selection
  that plain reads do not require — *`gpio_read()` working does not prove the
  edge detector is fed*
- the arm/disarm boundary may be clearing a finger's edge
- GPIO 34–39 are input-only RTC pads and may route their edges differently

## 22.7 Why the matrix was built first, and whether that was right

> The alternative was ADC — smaller, and with an onboard LDR to prove it. The
> matrix was chosen because it **cannot be bolted on later without revisiting
> every driver**, and because PENIRQ offered a real, low-rate, forgiving consumer
> already on the board.
>
> That reasoning half survives. The infrastructure is real and the next
> peripheral inherits it. But the "forgiving consumer" turned out to be the
> hardest part, and an ADC would have proved the same matrix against something
> with no edge semantics to get wrong.

---

# Part B — The ADC

## 22.8 A driver whose output cannot be checked for plausibility

> Every driver before this one talked to a device that answers in protocol. A
> display either shows the pattern or it does not; a flash read either returns
> the magic number or it does not; a touch controller either responds on the bus
> or is silent. Each of those has an internal check.
>
> **This one returns a number meaning "how much light", and nothing in the system
> can check it for plausibility.** 619 is a perfectly good ADC reading. So is
> 351. So is the same value on all eight channels, which is what a converter that
> is not converting anything returns.

> That is what shapes this report: the driver is ordinary, and the verification
> is the work.

## 22.9 ADC1, and a constraint that became an advantage

> ADC2 is unusable on this part whenever WiFi is running, which is the standard
> reason to avoid it. **That reason does not apply here.** The `-mabi=call0`
> build cannot link Espressif's precompiled radio libraries at all, so WiFi will
> never run and ADC2 is permanently free.

That reasoning was correct when written and was overtaken two days later by
Chapter 27. ADC2 remains untouched.

ADC1 is still first "because the board's own light sensor is on it and a sensor
already soldered down needs no wiring to be wrong about".

The file opens by declaring its own risk:

```c
 * Register offsets below are the risky part of this file. The last driver this
 * kernel gained lost an afternoon to a five-bit field whose order was recalled
 * rather than measured (UM-NATOS-023 §5.1), so adc_dump() exists from the first
 * commit rather than being added when something goes wrong, and the shell reads
 * every channel rather than the one that is supposed to be interesting.
```

Building the diagnostic *first*, on the strength of yesterday's lesson. It still
was not enough — §22.10.

## 22.10 The defect was one bit

`SENS_SAR1_EN_PAD_FORCE` is `BIT(31)`. It was written as bit 27, from memory.

> Bit 27 is not a spare bit. `SAR1_EN_PAD` is **twelve** bits at shift 19, so 27
> sits inside it: setting it selected a channel that does not exist, and left pad
> selection with the hardware FSM. The FSM then measured whatever it liked,
> identically, forever.

The symptom was a complete set of reassurances:

| observation | what it seemed to prove |
|---|---|
| all 8 channels ≈ 619 | plausible mid-scale readings |
| `DONE` asserted every time | conversions completing |
| 0 timeouts | the converter is alive |
| every register read back as written | the configuration is correct |

> **A wrong bit in the right register is invisible to a read-back.** That is the
> one failure mode the read-back discipline cannot catch, and it is worth naming
> because that discipline had been working well enough to feel sufficient.

The constant now carries the whole story:

```c
/* BIT(31), and this was the whole defect.
 *
 * It was written as bit 27 from memory. Bit 27 is not a separate field: SAR1_EN_PAD
 * is TWELVE bits at shift 19, so 27 is inside it, and setting it selected a
 * channel that does not exist while leaving pad selection with the FSM. The FSM
 * then measured whatever it liked, identically, forever -- which is why all eight
 * channels read ~619 and no pad mux bit changed anything.
 *
 * Every other offset and field in this file was recalled correctly. This one was
 * not, and nothing in the read-back could show it, because a wrong bit in a
 * correct register reads back exactly as written. The value came from the
 * vendor's sens_reg.h, fetched rather than remembered. */
#define SAR1_EN_PAD_FORCE   (1u << 31)
```

*"Every other offset and field in this file was recalled correctly"* is the
uncomfortable part: recall has a high hit rate, which is exactly what makes it
dangerous.

There is a second trap in the same driver, closed in advance:

```c
    /* DATA_INV is not optional and is the classic way this comes out looking
     * broken-but-alive: without it every reading is its own complement, so a
     * dark sensor reads high and covering it makes the number go up. */
```

## 22.11 Three probes, none of which found it

> Three probes were built before the answer arrived, and each is kept because
> each answers a question that would otherwise be asked again:
>
> - **`adcconv`** — proves a conversion *actually runs*. `DONE` goes low when
>   START is cleared and high 13 spins later. This killed the plausible theory
>   that the poll was returning stale latched data.
> - **`adcdrive`** — sweeps against **GPIO32, a pin this kernel drives**. Every
>   earlier probe depended on GPIO36 idling high, which is a belief about the
>   board; this commands the input voltage instead. It also settled `DATA_INV`
>   empirically: driving the pin high *raises* the reading, so the inversion as
>   configured is correct.
> - **`ldrscan`** — watches all eight channels at once.
>
> None of them found it. What found it was **fetching the vendor's `sens_reg.h`
> instead of recalling it**, at which point every other offset and field in the
> file turned out to be correct. It was one line.

The lesson is a taxonomy of tools:

> This was the third register-layout error in a day and the first fixed by
> reading the definitions rather than probing around them. **Probing is the right
> tool when the question is *what does this hardware do*. It is the wrong tool
> when the question is *what is this constant*, and those are easy to confuse
> when both present as "it does not work".**

`adcdrive` is worth noting separately: it is the only probe that *commands* the
input rather than observing it. Chapter 19 §19.5 identified that move as the one
that broke every previous deadlock — "feed the system a controlled input rather
than interpret an uncontrolled one".

## 22.12 The diagnostic that hid the answer

> The RTC IO dump skipped registers reading zero, to make the block's shape
> legible. The two registers that mattered — the pad muxes at `+0x7c` and `+0x80`
> — **were zero precisely because they were the thing not yet configured.**
>
> The filter removed exactly the registers the dump existed to find. A bit sweep
> then ran against `+0x00`, which is `RTC_GPIO_OUT` and governs nothing here, and
> reported thirty-two failures that meant nothing at all.

> **A diagnostic that suppresses the uninteresting cannot find something whose
> signature is *being uninteresting*.**

It prints zeros now. And a nice recovery of the information the filter was
introduced to provide:

> The block's shape was recoverable anyway: the ten-register run at
> `+0x94..+0xb8` can only be the ten touch pads, which fixes every other offset
> in the block by counting.

## 22.13 Verification, with a control group

A hand passed over the board, all eight channels watched at once:

```
ch  gpio   min   max  spread
 0   36    176   188     12   (touch pin)
 1   37    552   560      8
 2   38      0     0      0
 3   39    226   240     14   (touch pin)
 4   32      0     0      0   (touch pin)
 5   33   1777  1824     47   (touch pin)
 6   34    273   538    265   <- the sensor
 7   35    621   625      4
```

> **265 counts against a control group that never exceeded 47.** The four touch
> pins are kept in the scan for exactly this reason: **they are pins that should
> not respond to light, so they measure the noise floor while the experiment
> runs.**

A control group measured *simultaneously with* the experiment, on the same
hardware, in the same conditions. That is the strongest form of this project's
"one number that must move, one that must not" idiom.

And a claim settled rather than repeated:

> "The LDR is on GPIO34" was an assertion about the board, propagated through
> comments; `ldrscan` tested it. It happened to be right. **The point is that it
> is no longer an assumption.**

---

# Part C — I²C

## 22.14 Two pins, most sensors

> The ADC turned one pin into one sensor. This turns two pins into **most**
> sensors — temperature, humidity, pressure, accelerometers, magnetometers,
> real-time clocks, small displays — and they share the bus, so the pin cost does
> not grow with the sensor count. On a board with three spare pins that matters
> more than it would elsewhere.

| Signal | GPIO | Why |
|---|---|---|
| SDA | 22 | brought out to a header, unclaimed |
| SCL | 27 | brought out to a header, unclaimed |

The pin budget, which is the reason there are only two:

> Everything else on this board is spoken for: display (2, 12–15, 21), touch (25,
> 32, 33, 36, 39), SD (5, 18, 19, 23), light sensor (34), speaker (26), RGB LED
> (4, 16, 17).

## 22.15 Bit-banged, and why that is *safe* rather than merely consistent

This part has two hardware I²C controllers. Neither is used.

> The weak reason is consistency: the display and touch drivers are both
> bit-banged, so the technique is proven in this kernel and its failure modes are
> understood.
>
> The real reason is that **clock stretching is in the I²C specification**. A
> slave may hold SCL low to buy itself time, and every master is required to
> tolerate it. A bus that is legal to stall is therefore a bus on which a
> *preempted master* is legal too — a task switch in the middle of a transaction
> stretches the clock rather than corrupting the transfer.

> That is a guarantee from the protocol, not a consequence of this code being
> careful.

This is a genuinely elegant argument and it is the only place in the project
where a *protocol property* substitutes for a *kernel mechanism*. A bit-banged
SPI transaction preempted mid-byte would be fine too (SPI has no timing floor),
but a bit-banged I²S or UART would not be — which is why Chapter 23 puts audio
behind hardware PWM rather than bit-banging a waveform.

The property lives in one function:

```c
/* Releases SCL and waits for it to actually be high.
 *
 * This is the clock-stretch handler, and it is the reason a preempted
 * transaction survives: the master does not assume the clock rose because it
 * let go of it. A slave holding SCL down is legal and expected. A line that
 * never rises is a stuck bus or a missing pull-up, and that is a timeout rather
 * than an infinite wait inside a shell command. */
static int scl_release_and_wait(void)
{
    line_release(I2C_PIN_SCL);
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        if (line_read(I2C_PIN_SCL)) {
            half_bit();
            return I2C_OK;
        }
    }
    return I2C_ETIMEOUT;
}
```

## 22.16 Open drain, emulated

```c
/* Open drain, emulated.
 *
 * A line is pulled low by ENABLING the output driver (which is already set to
 * output zero) and released by DISABLING it, letting the pull-up do the rest.
 * Never driven high: two masters, or a slave mid-ACK, would then be shorted
 * against each other rather than merely contending. */
static inline void line_low(uint32_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_ENABLE_W1TS_REG) = 1u << pin;
    } else {
        GPIO_REG(GPIO_ENABLE1_W1TS_REG) = 1u << (pin - 32u);
    }
}

static inline void line_release(uint32_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_ENABLE_W1TC_REG) = 1u << pin;
    } else {
        GPIO_REG(GPIO_ENABLE1_W1TC_REG) = 1u << (pin - 32u);
    }
}
```

The output register is written once at init and never again; only the *enable* is
toggled. Nothing is ever driven high.

### `FUN_IE`, and the third instance of "silence reads as data"

```c
    /* FUN_IE because the master must READ these lines — for the ACK bit, for
     * incoming data, and for clock stretching. An open-drain pin whose input
     * buffer is off reads a constant zero, which would look like every device
     * ACKing everything. */
    GPIO_REG(mux_reg) = MCU_SEL_GPIO | FUN_IE | FUN_PU;
```

Same class as the touch controller reading silence as maximum pressure. Third
appearance of the pattern in this book, and the general rule from Chapter 19
§19.3 applies unchanged.

The constants are fetched, not recalled, with an explicit reference to yesterday:

```c
/* IO_MUX pad registers, and the pull-up bit. Both taken from the vendor's
 * io_mux_reg.h rather than recalled — FUN_PU is BIT(8), FUN_IE is BIT(9), and
 * an afternoon went into learning that this file's kind of constant is not
 * something to remember (UM-NATOS-023 §5.1, UM-NATOS-024 §4). */
```

## 22.17 The pull-up compromise

> The internal pull-ups are used: roughly **45 kΩ**, against the 2.2–10 kΩ a real
> bus wants.
>
> This is a stated compromise, not a design. Rise times are slow, so the clock is
> slow to match — **a master that clocks faster than the line can rise reads its
> own transition times as data.**

```c
/* Half a bit period.
 *
 * Deliberately slow. The internal pull-ups are ~45 kOhm, so a released line
 * rises through an RC that a proper 4.7 kOhm bus would not have, and a master
 * that clocks faster than the line can rise reads its own transition times as
 * data. Nothing here needs speed: a sensor read is a few dozen bytes and the
 * caller is a task that sleeps anyway. */
static inline void half_bit(void)
{
    for (volatile int i = 0; i < 60; i++) {
    }
}
```

## 22.18 The failure that looks like success

> **A bus with no pull-up reports every address as present.**
>
> SDA floats, reads low, and a floating low is indistinguishable from an ACK. A
> scan then returns 112 devices, which is not a subtle wrong answer — but the
> individual result at any one address is entirely plausible, and code that
> probes a single expected sensor would find it and proceed.

The self-test checks this **before any scan is believed**, per line and in both
directions:

```c
void i2c_selftest(void)
{
    static const uint32_t pin[2]  = { I2C_PIN_SDA, I2C_PIN_SCL };
    static const char    *name[2] = { "SDA (gpio22)", "SCL (gpio27)" };
    int ok = 1;

    for (int i = 0; i < 2; i++) {
        line_release(pin[i]);
        half_bit();
        uint32_t released = line_read(pin[i]);

        line_low(pin[i]);
        half_bit();
        uint32_t driven = line_read(pin[i]);

        line_release(pin[i]);
        half_bit();

        /* ... print ... */

        if (released && !driven) {
            uart_puts("   ok\n");
        } else if (!released) {
            uart_puts("   NO PULL-UP or shorted low\n");
            ok = 0;
        } else {
            uart_puts("   cannot drive low\n");
            ok = 0;
        }
    }

    uart_puts(ok ? "   bus looks sane; a scan can be believed\n"
                 : "   bus is NOT sane; ignore any scan result\n");
}
```

```
SDA (gpio22)  released=HIGH  driven=LOW    ok
SCL (gpio27)  released=HIGH  driven=LOW    ok
bus looks sane; a scan can be believed
scanning 0x08..0x77
0 device(s)
(nothing attached is the expected result on a bare board)
```

> Both halves matter. A line that reads high when released and high when driven
> means the driver is not working; a line that reads low both times means there
> is no pull-up. **Only one combination is a working line, and it is the one that
> gets the word `ok`.**

Two readings, four outcomes, three of which are named failures. That is a
properly designed test.

The scan also interprets its own result rather than reporting it neutrally:

```c
    if (found == 0u) {
        uart_puts("   (nothing attached is the expected result on a bare board)\n");
    } else if (found > 100u) {
        uart_puts("   (that many is a stuck SDA, not that many devices)\n");
    }
```

Telling the reader what the expected result *is* turns a number into a
conclusion.

## 22.19 The protocol details that matter

Two, both about not corrupting somebody else's bus.

**The last byte must be NAKed:**

```c
    /* The master's ACK tells the slave whether another byte is wanted. The LAST
     * byte must be NAKed: a slave that is ACKed keeps driving the bus and the
     * following STOP is then issued into a bus somebody else owns. */
```

**Repeated START, not STOP-then-START:**

```c
    /* Repeated START, not STOP-then-START. Releasing the bus between the
     * register number and the read lets another master interleave, and on a
     * single-master bus it still costs a slave the right to keep its internal
     * address pointer. */
    if (r == I2C_OK) {
        i2c_start();
        r = i2c_write_byte((uint8_t)((addr7 << 1) | 1u));
        /* ... */
    }
```

## 22.20 Metrics

### Interrupt matrix

| Quantity | Value |
|---|---|
| Peripheral interrupts routed before this | 0 |
| CPU line used | 23, level 3, level-triggered |
| GPIO source | 22 |
| PRO CPU enable bit | 15 (**measured**, not read) |
| Self-inflicted edges before masking | 3,917 |
| Assembly changed | 1 call |
| Distinct delivery failures found | 4 |
| **Finger-driven wakes observed** | **0** |

### ADC

| Quantity | Value |
|---|---|
| Channels | 8, ADC1 |
| Resolution | 12-bit |
| Attenuation | 11 dB, all channels |
| Conversion time | ~13 poll iterations |
| Light sensor | channel 6, GPIO34, **confirmed** |
| Sensor swing (hand over board) | 265 counts |
| Control-group maximum | 47 counts |
| Lines of driver | ~150 |
| **Wrong bits** | **1** |

### I²C

| Quantity | Value |
|---|---|
| Pins | SDA 22, SCL 27 |
| Pull-ups | internal, ~45 kΩ |
| Devices found on a bare bus | 0 |
| **Bytes ever transferred to a real device** | **0** |

## 22.21 What these do not establish

**Matrix.** No peripheral interrupt has yet driven anything — proven by injection
only. `task_wake()` has never woken a task. One line, one handler; no sharing, no
priority among sources. Nothing is re-entrant. The APP CPU is still never
started.

**ADC.** The reading is uncalibrated — counts, not volts, with factory eFuse
calibration data unread. The sensor's usable range is narrow (273–538 of 0–4095,
and the low end was a hand rather than darkness). It is polled. No application
can reach it. **Only channel 6 has been proven to track anything** — "the other
seven return distinct, stable, plausible values, and plausible is exactly what
this whole report is about not trusting." ADC2 is untouched despite being
permanently free.

**I²C.** **No byte has ever been transferred.** START, STOP, the ACK bit,
repeated START and the read path are all correct by inspection and have never
moved data.

> The first real device will be the first test of §22.19's logic, and the most
> likely defects are the ones inspection is worst at: a half-bit of timing, or an
> ACK sampled on the wrong clock edge.

Clock stretching has never happened, so the handler that justifies the whole
bit-banging argument is unexercised. Speed is unmeasured. No multi-master
arbitration detection. No bus recovery (the nine-clock flush is not implemented).
No application access.

---

**Next:** the first output that is not the screen, and three faults where each one
made the next invisible.
