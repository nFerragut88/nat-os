/* nat-os — tones on DAC2. See audio.h. */

#include "audio.h"
#include "timer.h"
#include "gpio.h"
#include "xtensa.h"
#include "uart.h"
#include "i2c.h"
#include "touch.h"
#include "adc.h"

#define REG(a) (*(volatile uint32_t *)(a))

/* Every offset and field below came from the vendor's sens_reg.h and
 * rtc_io_reg.h, fetched rather than recalled. That is the practice UM-NATOS-024
 * argues for after one misremembered bit position cost an afternoon, and this
 * driver has more single-bit enables than any other in the tree. */
#define SENS_BASE               0x3FF48800u
#define SENS_SAR_DAC_CTRL1_REG  (SENS_BASE + 0x0098u)
#define SENS_SAR_DAC_CTRL2_REG  (SENS_BASE + 0x009Cu)

#define SW_TONE_EN      (1u << 16)      /* run the cosine generator      */
#define SW_FSTEP_S      0u              /* 16 bits: the frequency step   */
#define SW_FSTEP_M      0xFFFFu

#define DAC_CW_EN2      (1u << 25)      /* route the generator to DAC2   */
#define DAC_INV2_S      22u             /* 2 = invert, which centres it  */
#define DAC_SCALE2_S    18u             /* 0 = full amplitude, 3 = 1/8   */
#define DAC_DC2_S       8u              /* 8-bit DC offset               */

#define RTCIO_BASE              0x3FF48400u
#define RTC_IO_PAD_DAC2_REG     (RTCIO_BASE + 0x88u)

#define PDAC2_DAC_S     19u             /* 8-bit level when not generating */
#define PDAC2_XPD_DAC   (1u << 18)      /* power the DAC up               */
#define PDAC2_MUX_SEL   (1u << 17)      /* pad belongs to RTC, not GPIO    */
#define PDAC2_FUN_SEL_S 15u

/* RTC8M nominal. The generator advances a 16-bit phase accumulator by SW_FSTEP
 * once per RTC8M cycle, so a full cycle of the wave takes 65536/fstep ticks:
 *
 *      hz = RTC8M * fstep / 65536      ->      fstep = hz * 65536 / RTC8M
 *
 * With RTC8M at 8 MHz that is fstep = hz / 122, so one step is about 122 Hz and
 * the whole audible range fits comfortably inside the 16-bit field. The
 * oscillator is untrimmed, so treat the result as nominal. */
#define RTC8M_HZ        8000000u

static uint32_t g_tones;
static uint32_t g_toggles;   /* edges the square-wave loop actually produced */
static uint32_t g_off_tick;     /* tick at which a beep ends, 0 = not beeping */
static uint32_t g_current;

uint32_t audio_tones(void) { return g_tones; }

void audio_init(void)
{
    /* Hand the pad to the RTC subsystem and power the DAC. Until MUX_SEL is
     * set the pin is an ordinary GPIO and the DAC drives nothing — the same
     * class of mistake as the ADC pads, where the converter ran correctly and
     * was attached to nothing (UM-NATOS-024 §3). */
    uint32_t p = REG(RTC_IO_PAD_DAC2_REG);
    p |= PDAC2_MUX_SEL | PDAC2_XPD_DAC;
    p &= ~(3u << PDAC2_FUN_SEL_S);      /* RTC function 0 */
    REG(RTC_IO_PAD_DAC2_REG) = p;

    audio_off();
}

void audio_tone(uint32_t hz)
{
    if (hz == 0u) {
        audio_off();
        return;
    }

    uint32_t fstep = (hz * 65536u) / RTC8M_HZ;
    if (fstep == 0u) {
        fstep = 1u;             /* below one step; the lowest tone available */
    }
    if (fstep > SW_FSTEP_M) {
        fstep = SW_FSTEP_M;
    }

    REG(SENS_SAR_DAC_CTRL1_REG) = SW_TONE_EN | (fstep << SW_FSTEP_S);

    /* INV2 = 2 inverts the MSB, which centres the wave on mid-scale. Without
     * it the output sits against a rail for half its cycle, which a speaker
     * reproduces as a click and a DC offset rather than as a tone.
     *
     * SCALE2 = 1 is half amplitude. Full scale into a small speaker with no
     * volume control is louder than anything this device needs to say. */
    REG(SENS_SAR_DAC_CTRL2_REG) = DAC_CW_EN2
                                | (2u << DAC_INV2_S)
                                | (1u << DAC_SCALE2_S)
                                | (0u << DAC_DC2_S);

    g_current = hz;
    g_tones++;
}

void audio_off(void)
{
    REG(SENS_SAR_DAC_CTRL1_REG) = 0u;           /* stop the generator */
    REG(SENS_SAR_DAC_CTRL2_REG) = 0u;           /* unroute it from DAC2 */

    /* Park the pad at mid-scale rather than zero.
     *
     * Zero is a rail, and stepping from a centred wave to a rail is a step the
     * speaker reproduces as a click on every tone-off. Mid-scale is where the
     * wave already was. */
    uint32_t p = REG(RTC_IO_PAD_DAC2_REG);
    p &= ~(0xFFu << PDAC2_DAC_S);
    p |= (128u << PDAC2_DAC_S);
    REG(RTC_IO_PAD_DAC2_REG) = p;

    g_off_tick = 0;
    g_current  = 0;
}

void audio_beep(uint32_t hz, uint32_t ticks)
{
    audio_tone(hz);
    g_off_tick = timer_ticks() + (ticks ? ticks : 1u);
}

void audio_service(void)
{
    /* Signed comparison, so a tick counter that wraps does not leave a tone
     * running until it wraps again — the same reasoning as the comparator
     * deadline in timer.c (UM-NATOS-014 §10). */
    if (g_off_tick && (int32_t)(timer_ticks() - g_off_tick) >= 0) {
        audio_off();
    }
}

void audio_click(void)
{
    /* Two ticks is about 20 ms: long enough to be heard as a click, short
     * enough that holding a key down does not become a tone. */
    audio_beep(2200u, 2u);
}

/* ---- finding the speaker pin --------------------------------------------
 *
 * The board's connector is labelled SPEAK and the trace behind it goes to some
 * GPIO. Which one is a claim about the hardware, and the last two such claims
 * in this project — the LDR on GPIO34 and the PRO CPU interrupt enable bit —
 * were settled by measuring rather than by repeating them (UM-NATOS-024 §6,
 * UM-NATOS-023 §5.1). One was right and one was wrong.
 *
 * A square wave rather than the DAC, for two reasons. It swings the full supply
 * instead of the DAC's half-scale sine, so an amplifier that is present but
 * weakly driven still makes a noise; and it needs no RTC mux, so a pin that is
 * silent under this is silent because nothing is connected to it, not because
 * some enable was missed.
 *
 * Timed from CCOUNT rather than a delay loop, so the pitch does not depend on
 * what the compiler did to the loop body.
 */
/* Round two, after the first seven were all silent.
 *
 * GPIO25 leads it, and should probably have led round one. It is DAC1 — the
 * OTHER analogue output — and the reason it was left out is that this kernel
 * already uses it as the touch controller's SPI clock. "The pin is busy" is a
 * fact about this software, not about where the board routed its speaker, and
 * excluding a candidate for a reason that belongs to the wrong layer is how a
 * search misses the answer.
 *
 * The rest are the SD and touch pins. Driving them disturbs those peripherals
 * for a second and nothing more. The display's pins are deliberately NOT here:
 * garbage clocked into the ILI9341 is interpreted as commands, and the panel
 * can be left inverted, asleep or misconfigured until the next reflash. A
 * silent speaker is worth a second of confused touch; it is not worth a broken
 * screen.
 *
 * Flash (6-11) and UART (1, 3) are excluded absolutely — the first would crash
 * mid-instruction and the second would cut the channel carrying the results. */
static const uint32_t SPK_PINS[] = { 25u, 5u, 18u, 19u, 23u, 32u, 33u };
static const uint32_t SPK_MUX[]  = {
    0x3FF49024u,    /* 25 — DAC1, and this kernel's touch clock              */
    0x3FF4906Cu,    /* 5  — SD                                              */
    0x3FF49070u,    /* 18 — SD                                              */
    0x3FF49074u,    /* 19 — SD                                              */
    0x3FF4908Cu,    /* 23 — SD                                              */
    0x3FF4901Cu,    /* 32 — touch MOSI                                      */
    0x3FF49020u,    /* 33 — touch CS                                        */
};
#define SPK_COUNT (sizeof SPK_PINS / sizeof SPK_PINS[0])

void audio_square(uint32_t pin, uint32_t mux, uint32_t hz, uint32_t ms)
{
    gpio_out_init(pin, mux);

    uint32_t half = 40000000u / (hz ? hz : 1000u);  /* ~80 MHz / 2 / hz */
    uint32_t end  = xt_ccount() + ms * 80000u;
    uint32_t next = xt_ccount();
    int level = 0;

    while ((int32_t)(xt_ccount() - end) < 0) {
        if ((int32_t)(xt_ccount() - next) >= 0) {
            level = !level;
            if (level) {
                gpio_set(pin);
            } else {
                gpio_clear(pin);
            }
            next += half;
            g_toggles++;
        }
    }
    gpio_clear(pin);
}

void audio_find_speaker(void)
{
    uart_puts("   sounding 1 kHz on each candidate for ~1 s.\n");
    uart_puts("   say which one you heard.\n");

    for (uint32_t i = 0; i < SPK_COUNT; i++) {
        uart_puts("     gpio ");
        uart_put_dec(SPK_PINS[i]);
        uart_puts(" ...\n");
        audio_square(SPK_PINS[i], SPK_MUX[i], 1000u, 900u);

        /* A gap, so two adjacent pins are not heard as one continuous tone. */
        uint32_t g = xt_ccount() + 40000000u;
        while ((int32_t)(xt_ccount() - g) < 0) {
        }
    }
    uart_puts("   done\n");

    /* Put back everything this borrowed. touch_init() restores the clock, chip
     * select and data pins to the states the driver expects to find them in;
     * without it the panel answers nothing and looks like a dead controller. */
    touch_init();
    i2c_init();
    audio_init();
}

/* ---- does the square wave exist at all? --------------------------------
 *
 * Fourteen pins have now been driven and none made a sound, and that is also
 * exactly what a generator that never toggles would look like. Asking someone
 * to listen for a signal whose existence has never been checked is the same
 * mistake as chasing PENIRQ through four layers on the assumption that the edge
 * was real (UM-NATOS-023 §5).
 *
 * GPIO32 is the one pin this kernel can both DRIVE and MEASURE — it is the
 * touch controller's MOSI and ADC1 channel 4 — so the board can check this
 * without ears and without hardware nobody has.
 *
 * Two independent things are established here. The toggle count says the timing
 * loop ran and produced edges at the rate it intended. The ADC readings say
 * those edges reached the pad as real voltage.
 */
void audio_probe_square(void)
{
    uint32_t before = g_toggles;

    /* Drive at 1 kHz for 200 ms: 200 cycles, so 400 edges expected. */
    audio_square(32u, 0x3FF4901Cu, 1000u, 200u);
    uint32_t edges = g_toggles - before;

    uart_puts("   edges produced = ");
    uart_put_dec(edges);
    uart_puts("  expected ~400\n");

    /* Now the pad itself: hold each level and measure it. */
    gpio_out_init(32u, 0x3FF4901Cu);
    gpio_set(32u);
    uint32_t hi = adc1_read_avg(4u, 4u);
    gpio_clear(32u);
    uint32_t lo = adc1_read_avg(4u, 4u);

    uart_puts("   pad measured high = ");
    uart_put_dec(hi);
    uart_puts("  low = ");
    uart_put_dec(lo);
    uart_puts("\n");

    if (edges < 300u) {
        uart_puts("   THE GENERATOR IS BROKEN - the silence proves nothing\n");
    } else if (hi < lo + 300u) {
        uart_puts("   edges are timed but the pad does not move - drive is broken\n");
    } else {
        uart_puts("   generator and pad both work; the silence is the hardware\n");
    }

    touch_init();
    audio_init();
}
