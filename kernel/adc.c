/* nat-os — SAR ADC1. See adc.h.
 *
 * The SAR ADC sits in the RTC domain, not the digital one, so none of the
 * peripheral conventions from the rest of this kernel apply. It is controlled
 * through the SENS register block, and the awkward part is that most of its
 * registers select between an RTC controller, a digital controller and the ULP
 * — three masters for one converter, of which exactly one must be told it is in
 * charge before anything happens.
 *
 * Register offsets below are the risky part of this file. The last driver this
 * kernel gained lost an afternoon to a five-bit field whose order was recalled
 * rather than measured (UM-NATOS-023 §5.1), so adc_dump() exists from the first
 * commit rather than being added when something goes wrong, and the shell reads
 * every channel rather than the one that is supposed to be interesting.
 */

#include "adc.h"
#include "uart.h"
#include "gpio.h"

#define SENS_BASE                   0x3FF48800u

#define SENS_SAR_READ_CTRL_REG      (SENS_BASE + 0x0000u)
#define SENS_SAR_MEAS_WAIT2_REG     (SENS_BASE + 0x000Cu)
#define SENS_SAR_START_FORCE_REG    (SENS_BASE + 0x002Cu)
#define SENS_SAR_ATTEN1_REG         (SENS_BASE + 0x0034u)
#define SENS_SAR_MEAS_START1_REG    (SENS_BASE + 0x0054u)

#define REG(a) (*(volatile uint32_t *)(a))

/* SENS_SAR_READ_CTRL_REG */
#define SAR1_DIG_FORCE      (1u << 27)  /* 1 = digital controller owns ADC1 */
#define SAR1_DATA_INV       (1u << 28)  /* the reading comes out inverted */
#define SAR1_SAMPLE_BIT_S   16u         /* 3 = 12-bit */

/* SENS_SAR_MEAS_WAIT2_REG */
#define FORCE_XPD_SAR_S     18u         /* 3 = keep the SAR powered up */

/* SENS_SAR_START_FORCE_REG */
#define SAR1_BIT_WIDTH_S    0u          /* 3 = 12-bit */

/* SENS_SAR_MEAS_START1_REG */
#define MEAS1_DATA_MASK     0xFFFFu
#define MEAS1_DONE          (1u << 16)
#define MEAS1_START         (1u << 17)
#define MEAS1_START_FORCE   (1u << 18)  /* software starts it, not the FSM */
#define SAR1_EN_PAD_S       19u         /* one bit per channel */
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

static uint32_t g_timeouts;

void adc_init(void)
{
    /* Take the converter away from the digital controller and the FSM.
     *
     * DATA_INV is not optional and is the classic way this comes out looking
     * broken-but-alive: without it every reading is its own complement, so a
     * dark sensor reads high and covering it makes the number go up. That is a
     * plausible-looking signal that responds to light in the wrong direction,
     * and it would be easy to "calibrate" around rather than notice. */
    uint32_t rc = REG(SENS_SAR_READ_CTRL_REG);
    rc &= ~SAR1_DIG_FORCE;
    rc |= SAR1_DATA_INV;
    rc &= ~(3u << SAR1_SAMPLE_BIT_S);
    rc |= (3u << SAR1_SAMPLE_BIT_S);        /* 12-bit */
    REG(SENS_SAR_READ_CTRL_REG) = rc;

    uint32_t sf = REG(SENS_SAR_START_FORCE_REG);
    sf &= ~(3u << SAR1_BIT_WIDTH_S);
    sf |= (3u << SAR1_BIT_WIDTH_S);         /* 12-bit */
    REG(SENS_SAR_START_FORCE_REG) = sf;

    /* Power the SAR and keep it powered. Left to the FSM it powers down between
     * conversions, and the first conversion after a power-up is the one the
     * datasheet does not stand behind — exactly the defect that made the touch
     * controller's pressure channel useless for months (UM-NATOS-017 §3.3). */
    uint32_t w = REG(SENS_SAR_MEAS_WAIT2_REG);
    w &= ~(3u << FORCE_XPD_SAR_S);
    w |= (3u << FORCE_XPD_SAR_S);
    REG(SENS_SAR_MEAS_WAIT2_REG) = w;

    /* Widest range on every channel. Two bits each, so all eight is 0xFFFF. */
    REG(SENS_SAR_ATTEN1_REG) = 0xFFFFFFFFu;

    /* Software owns pad selection and conversion start. */
    uint32_t m = REG(SENS_SAR_MEAS_START1_REG);
    m |= SAR1_EN_PAD_FORCE | MEAS1_START_FORCE;
    m &= ~MEAS1_START;
    REG(SENS_SAR_MEAS_START1_REG) = m;
}

uint32_t adc1_read(uint32_t channel)
{
    if (channel > 7u) {
        return ADC_INVALID;
    }

    uint32_t m = REG(SENS_SAR_MEAS_START1_REG);
    m |= SAR1_EN_PAD_FORCE | MEAS1_START_FORCE;
    m &= ~(0xFFFu << SAR1_EN_PAD_S);   /* the field is 12 bits, not 8 */
    m |= (1u << (SAR1_EN_PAD_S + channel));
    m &= ~MEAS1_START;
    REG(SENS_SAR_MEAS_START1_REG) = m;

    /* Rising edge on START is what begins the conversion, so it has to be
     * cleared and set rather than just set. */
    REG(SENS_SAR_MEAS_START1_REG) = m | MEAS1_START;

    /* Bounded wait. An unbounded one on a converter that is not running is a
     * hang inside a shell command, and the whole point of ADC_INVALID is that
     * "did not answer" stays distinguishable from "answered zero". */
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        uint32_t s = REG(SENS_SAR_MEAS_START1_REG);
        if (s & MEAS1_DONE) {
            return s & MEAS1_DATA_MASK;
        }
    }

    g_timeouts++;
    return ADC_INVALID;
}

uint32_t adc1_read_avg(uint32_t channel, uint32_t samples)
{
    if (samples == 0u) {
        return ADC_INVALID;
    }

    (void)adc1_read(channel);           /* discarded: first after a pad change */

    uint32_t sum = 0, taken = 0;
    for (uint32_t i = 0; i < samples; i++) {
        uint32_t v = adc1_read(channel);
        if (v == ADC_INVALID) {
            return ADC_INVALID;         /* one bad sample poisons the average */
        }
        sum += v;
        taken++;
    }
    return sum / taken;
}

uint32_t adc_timeouts(void) { return g_timeouts; }

void adc_dump(void)
{
    uart_puts("     read_ctrl    = ");
    uart_put_hex(REG(SENS_SAR_READ_CTRL_REG));
    uart_puts("\n     meas_wait2   = ");
    uart_put_hex(REG(SENS_SAR_MEAS_WAIT2_REG));
    uart_puts("\n     start_force  = ");
    uart_put_hex(REG(SENS_SAR_START_FORCE_REG));
    uart_puts("\n     atten1       = ");
    uart_put_hex(REG(SENS_SAR_ATTEN1_REG));
    uart_puts("\n     meas_start1  = ");
    uart_put_hex(REG(SENS_SAR_MEAS_START1_REG));
    uart_puts("\n     timeouts     = ");
    uart_put_dec(g_timeouts);
    uart_puts("\n");
}

/* ---- the RTC IO block ---------------------------------------------------
 *
 * Every channel returning the same value with every SENS register reading back
 * as programmed says the converter is running and is not attached to any pad.
 * On this part the ADC pads live in the RTC domain and must be switched to
 * their analogue function there; the digital IO_MUX that the rest of this
 * kernel uses does not reach them.
 *
 * Which register does that is exactly the kind of recalled-offset question that
 * cost UM-NATOS-023 an afternoon, so this dumps the block instead. Reading is
 * safe; writing blind here is not, because the same block holds the 32 kHz
 * crystal pads and the pull-ups on pins this kernel is already using. */
void adc_dump_rtcio(void)
{
    uart_puts("   rtcio 0x3FF48400:\n");
    for (uint32_t off = 0x00u; off <= 0xC8u; off += 4u) {
        uint32_t v = REG(0x3FF48400u + off);
        if (v == 0u) {
            continue;               /* skip the empty ones; the shape is the point */
        }
        uart_puts("     +0x");
        uart_put_hex(off);
        uart_puts(" = ");
        uart_put_hex(v);
        uart_puts("\n");
    }
}

/* ---- find the mux bit by trying, not by remembering ---------------------
 *
 * RTC_IO_SENSOR_PADS at +0x00 governs GPIO36-39 and currently reads zero, so
 * those pads are on the digital mux and the converter cannot see them. Which
 * bit moves them is a field-order question, and this project's record on field
 * order from memory is now 0 for 2 (UM-NATOS-023 §5.1, §5.4).
 *
 * There is an unmistakable oracle available. GPIO36 is PENIRQ, which idles
 * HIGH at the supply rail, so a channel 0 that is genuinely connected reads
 * near full scale. Anything still reading the ~619 that every channel returns
 * when nothing is attached is not connected.
 *
 * Safe to sweep: this register reaches only pads 36-39, all four of which are
 * input-only on this part, so no combination of bits can make the chip drive
 * against something. The register is restored afterwards.
 */
void adc_probe_sensor_mux(void)
{
    /* +0x7c, established by counting the ten touch pads at +0x94..+0xb8 back to
     * their base — the block's own shape, not a recalled offset. The first
     * version of this swept +0x00, which is RTC_GPIO_OUT and governs nothing
     * here. */
    volatile uint32_t *sensor_pads = (volatile uint32_t *)(0x3FF48400u + 0x7Cu);
    uint32_t saved = *sensor_pads;
    uint32_t base  = adc1_read_avg(0u, 4u);

    uart_puts("   ch0 (GPIO36, idles HIGH) with no mux = ");
    uart_put_dec(base);
    uart_puts("\n   sweeping RTC_IO_SENSOR_PADS bits:\n");

    for (uint32_t bit = 0; bit < 32u; bit++) {
        *sensor_pads = (1u << bit);
        uint32_t v = adc1_read_avg(0u, 4u);

        /* Only report a bit that changed something. Thirty-two lines of the
         * same number is the noise this is trying to see through. */
        uint32_t diff = (v > base) ? v - base : base - v;
        if (diff > 200u) {
            uart_puts("     bit ");
            uart_put_dec(bit);
            uart_puts("  -> ");
            uart_put_dec(v);
            uart_puts(v > 3000u ? "   <- rail, this is it\n" : "   (changed)\n");
        }
    }

    *sensor_pads = saved;
    uart_puts("   restored\n");
}

/* ---- is a conversion actually happening? --------------------------------
 *
 * Eight identical values, DONE always set, and no timeouts is also what you get
 * from a converter that ran once and never again: the poll in adc1_read() finds
 * DONE already high from the previous conversion and returns its stale data
 * immediately, without a conversion having taken place at all.
 *
 * That reads as success from every angle — a plausible number, a DONE flag, no
 * timeout — which is why it is worth testing rather than assuming. If DONE does
 * not go low when START is cleared, the poll is meaningless and the value is a
 * fossil. */
void adc_probe_convert(void)
{
    uint32_t m = REG(SENS_SAR_MEAS_START1_REG);
    m |= SAR1_EN_PAD_FORCE | MEAS1_START_FORCE;
    m &= ~(0xFFu << SAR1_EN_PAD_S);
    m |= (1u << (SAR1_EN_PAD_S + 0u));      /* channel 0, GPIO36, idles HIGH */

    REG(SENS_SAR_MEAS_START1_REG) = m & ~MEAS1_START;
    uint32_t after_clear = REG(SENS_SAR_MEAS_START1_REG);

    REG(SENS_SAR_MEAS_START1_REG) = m | MEAS1_START;
    uint32_t after_start = REG(SENS_SAR_MEAS_START1_REG);

    uint32_t spins = 0;
    uint32_t s = 0;
    for (; spins < 100000u; spins++) {
        s = REG(SENS_SAR_MEAS_START1_REG);
        if (s & MEAS1_DONE) {
            break;
        }
    }

    uart_puts("   after START cleared = ");
    uart_put_hex(after_clear);
    uart_puts(after_clear & MEAS1_DONE ? "  DONE STILL SET\n" : "  done clear\n");
    uart_puts("   after START set     = ");
    uart_put_hex(after_start);
    uart_puts(after_start & MEAS1_DONE ? "  DONE already set\n" : "  converting\n");
    uart_puts("   spins to DONE       = ");
    uart_put_dec(spins);
    uart_puts("\n   final               = ");
    uart_put_hex(s);
    uart_puts("  data ");
    uart_put_dec(s & MEAS1_DATA_MASK);
    uart_puts("\n");
}

/* ---- an oracle this kernel controls -------------------------------------
 *
 * Every probe so far has depended on GPIO36 idling high, which is a belief
 * about the board. GPIO32 is the touch controller's MOSI, which this kernel
 * DRIVES, and it is ADC1 channel 4 — so the input voltage can be commanded
 * rather than assumed, and "the converter sees the pad" becomes a question with
 * a self-contained answer.
 *
 * Its RTC pad register is TOUCH_PAD9 at +0xb8. That address is not recalled: it
 * is the last of the ten-register run the block dump showed at +0x94..+0xb8,
 * and TOUCH9 is GPIO32.
 *
 * Driving MOSI is safe here. Nothing is mid-transaction — the shell and the
 * touch task never run at once — and MOSI idles in whatever state the last
 * transfer left it, so both levels are ones the pin takes in normal use.
 */
void adc_probe_driven(void)
{
    volatile uint32_t *touch9 = (volatile uint32_t *)(0x3FF48400u + 0xB8u);
    uint32_t saved = *touch9;

    gpio_set(32u);
    uint32_t hi_before = adc1_read_avg(4u, 4u);
    gpio_clear(32u);
    uint32_t lo_before = adc1_read_avg(4u, 4u);

    uart_puts("   ch4 (GPIO32, driven) before any mux:  high=");
    uart_put_dec(hi_before);
    uart_puts("  low=");
    uart_put_dec(lo_before);
    uart_puts("\n   sweeping TOUCH_PAD9 (+0xb8) bits:\n");

    int found = 0;
    for (uint32_t bit = 0; bit < 32u; bit++) {
        *touch9 = saved | (1u << bit);

        gpio_set(32u);
        uint32_t hi = adc1_read_avg(4u, 4u);
        gpio_clear(32u);
        uint32_t lo = adc1_read_avg(4u, 4u);

        /* The signature of a connected pad is not a particular value, it is the
         * two levels DIFFERING. A pad that is not connected returns the same
         * number whatever the pin is doing. */
        uint32_t spread = (hi > lo) ? hi - lo : lo - hi;
        if (spread > 300u) {
            found = 1;
            uart_puts("     bit ");
            uart_put_dec(bit);
            uart_puts("  high=");
            uart_put_dec(hi);
            uart_puts("  low=");
            uart_put_dec(lo);
            uart_puts("   <- the pad is connected\n");
        }
    }

    *touch9 = saved;
    gpio_set(32u);
    if (!found) {
        uart_puts("     no bit connected the pad\n");
    }
    uart_puts("   restored\n");
}

/* Watches one channel and reports the extremes it saw.
 *
 * The end-to-end claim for this driver is that a number here tracks something
 * physical, and a single reading cannot show that — 351 is as consistent with a
 * light sensor as with a disconnected pin. A spread taken while the light is
 * deliberately changed is the smallest measurement that distinguishes them.
 *
 * min/max rather than a live trace for the reason the touch log and the
 * calibration both had to learn: a value printed as it happens is a value
 * printed when nobody is capturing. */
void adc_watch(uint32_t channel, uint32_t rounds)
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0, n = 0;

    for (uint32_t i = 0; i < rounds; i++) {
        uint32_t v = adc1_read_avg(channel, 4u);
        if (v == ADC_INVALID) {
            continue;
        }
        if (v < lo) { lo = v; }
        if (v > hi) { hi = v; }
        n++;
        for (volatile int d = 0; d < 20000; d++) {
        }
    }

    uart_puts("   ch");
    uart_put_dec(channel);
    uart_puts("  samples ");
    uart_put_dec(n);
    uart_puts("  min ");
    uart_put_dec(lo);
    uart_puts("  max ");
    uart_put_dec(hi);
    uart_puts("  spread ");
    uart_put_dec(hi - lo);
    uart_puts(hi - lo > 200u ? "   <- it is tracking something\n"
                             : "   (flat: not a sensor, or the light did not change)\n");
}

/* Watches every channel at once and reports which one moved.
 *
 * "The LDR is on GPIO34" is a claim about the board that this driver has been
 * repeating without evidence. Watching all eight while the light changes tests
 * it instead: whichever channel has a large spread is the light sensor, and if
 * none does then the board does not have one where it is said to.
 *
 * Four of these pads are the touch controller's and will move on their own,
 * which is useful rather than confusing — a channel that moves when nothing is
 * touching it is the control group. */
void adc_watch_all(uint32_t rounds)
{
    uint32_t lo[8], hi[8];
    for (uint32_t c = 0; c < 8u; c++) {
        lo[c] = 0xFFFFFFFFu;
        hi[c] = 0;
    }

    for (uint32_t i = 0; i < rounds; i++) {
        for (uint32_t c = 0; c < 8u; c++) {
            uint32_t v = adc1_read_avg(c, 2u);
            if (v == ADC_INVALID) {
                continue;
            }
            if (v < lo[c]) { lo[c] = v; }
            if (v > hi[c]) { hi[c] = v; }
        }
        for (volatile int d = 0; d < 20000; d++) {
        }
    }

    static const uint32_t pad[8] = { 36, 37, 38, 39, 32, 33, 34, 35 };
    uart_puts("   ch  gpio   min   max  spread\n");
    for (uint32_t c = 0; c < 8u; c++) {
        uart_puts("   ");
        uart_put_dec(c);
        uart_puts("   ");
        uart_put_dec(pad[c]);
        uart_puts("    ");
        uart_put_dec(lo[c]);
        uart_puts("   ");
        uart_put_dec(hi[c]);
        uart_puts("   ");
        uart_put_dec(hi[c] - lo[c]);
        /* 150 against a control group that measured 0, 4, 8, 12, 14 and 47
         * with a hand passing over the board. The sensor moved 265. The
         * threshold is set from that measurement rather than from a guess about
         * what a big number looks like. */
        if (hi[c] - lo[c] > 150u) {
            uart_puts("   <- moved a lot");
        }
        if (c == 0u || c == 3u || c == 4u || c == 5u) {
            uart_puts("   (touch pin)");
        }
        uart_puts("\n");
    }
}
