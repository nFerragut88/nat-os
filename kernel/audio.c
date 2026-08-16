/* nat-os — tones on GPIO26. See audio.h. */

#include "audio.h"
#include "timer.h"
#include "gpio.h"
#include "xtensa.h"
#include "uart.h"
#include "i2c.h"
#include "touch.h"
#include "adc.h"

#define REG(a) (*(volatile uint32_t *)(a))

/* ---- what happened to the DAC -------------------------------------------
 *
 * This driver was first written around the DAC's hardware cosine generator,
 * which is elegant — a continuous waveform with no CPU at all — and produced no
 * sound. Worse, `audio_init()` set MUX_SEL on GPIO26 to hand the pad to the RTC
 * subsystem, and once RTC owns a pad the digital GPIO matrix cannot drive it.
 * So the DAC did not work AND every square-wave probe aimed at the same pin was
 * writing to a pad that was not listening. Fifteen pins were swept, all silent,
 * and the mute was this file's own boot-time init.
 *
 * It survived a deliberate check because that check ran on GPIO32 — a pin with
 * no RTC mux, which therefore could not exhibit the fault. Verifying an
 * instrument on the one case that structurally cannot fail is not verification,
 * and the conclusion drawn from it ("the silence is the hardware") sent the
 * search in exactly the wrong direction.
 *
 * LEDC replaces it, and is the better fit regardless. It is hardware PWM: set a
 * divider and it generates the waveform continuously with no CPU, no interrupt
 * and no DMA — the properties the cosine generator was chosen for — and it is
 * clocked from APB at 80 MHz, which this project has measured, so the pitch is
 * exact rather than nominal against an untrimmed RC oscillator.
 *
 * Offsets and field positions came from the vendor's ledc_reg.h and
 * gpio_sig_map.h, fetched rather than recalled.
 */
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
#define DUTY_RES_S      0u              /* 5 bits: log2 of the period     */
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

#define RTCIO_BASE              0x3FF48400u
#define RTC_IO_PAD_DAC1_REG     (RTCIO_BASE + 0x84u)
#define RTC_IO_PAD_DAC2_REG     (RTCIO_BASE + 0x88u)
#define PDAC_XPD_DAC    (1u << 18)
#define PDAC_MUX_SEL    (1u << 17)

static uint32_t g_tones;
static uint32_t g_toggles;      /* edges the bit-banged probes produced */
static uint32_t g_off_tick;     /* tick a beep ends at, 0 = not beeping */
static uint32_t g_current;

uint32_t audio_tones(void) { return g_tones; }

/* Returns a DAC pad to the digital GPIO matrix. See the note above: this is the
 * defect that muted GPIO26 for an entire investigation. */
static void release_dac_pad(uint32_t pin)
{
    if (pin != 25u && pin != 26u) {
        return;
    }
    volatile uint32_t *pad = (volatile uint32_t *)
        ((pin == 25u) ? RTC_IO_PAD_DAC1_REG : RTC_IO_PAD_DAC2_REG);
    *pad &= ~(PDAC_MUX_SEL | PDAC_XPD_DAC);
}

/* Ungates the peripheral's clock and releases it from reset.
 *
 * LEDC comes out of reset clock-gated, and a gated peripheral ACCEPTS REGISTER
 * WRITES AND DOES NOTHING — every value reads back exactly as written while no
 * hardware acts on any of it. That is the third time this project has met that
 * shape: the ADC converting nothing while every SENS register read correctly
 * (UM-NATOS-024 §3), the GPIO edge delivered to a halted CPU (UM-NATOS-023
 * §5.1), and now this.
 *
 * The display and touch never needed it because SPI2 and GPIO are ungated by
 * the bootloader before this kernel runs. LEDC is the first peripheral here
 * that this kernel has to switch on itself. */
static void ledc_clock_enable(void)
{
    REG(0x3FF000C0u) |=  (1u << 11);    /* DPORT_PERIP_CLK_EN: LEDC */
    REG(0x3FF000C4u) &= ~(1u << 11);    /* DPORT_PERIP_RST_EN: LEDC out of reset */
}

void audio_init(void)
{
    ledc_clock_enable();
    release_dac_pad(SPK_PIN);

    /* Pad to plain GPIO output, then route LEDC channel 0 onto it through the
     * GPIO matrix. Both layers must agree or the pin stays idle — the rule
     * gpio.h opens with, and the one this file broke. */
    gpio_out_init(SPK_PIN, SPK_MUX_REG);
    GPIO_REG(GPIO_FUNC_OUT_SEL(SPK_PIN)) = LEDC_HS_SIG_OUT0;

    REG(LEDC_CONF_REG) = 1u;            /* high-speed domain runs from APB */

    REG(LEDC_HSCH0_HPOINT_REG) = 0u;
    REG(LEDC_HSCH0_DUTY_REG)   = (DUTY_PERIOD / 2u) << DUTY_S;   /* 50% */
    REG(LEDC_HSCH0_CONF1_REG)  = DUTY_START;
    REG(LEDC_HSCH0_CONF0_REG)  = 0u;    /* timer 0, output disabled */

    audio_off();
}

void audio_tone(uint32_t hz)
{
    if (hz == 0u) {
        audio_off();
        return;
    }

    /* f = APB / (div/256 * 2^res)  ->  div = APB * 256 / (f * 2^res)
     *
     * Divided by hz FIRST so the numerator cannot overflow: APB * 256 is
     * 2.05e10 and does not fit in 32 bits. */
    uint32_t div = ((APB_HZ / hz) << 8) / DUTY_PERIOD;
    if (div < 256u) {
        div = 256u;                     /* divider 1.0 — the ceiling */
    }
    if (div > 0x3FFFFu) {
        div = 0x3FFFFu;                 /* about 78 Hz — the floor */
    }

    REG(LEDC_HSTIMER0_CONF_REG) = TICK_SEL_APB
                                | (div << DIV_NUM_S)
                                | (DUTY_RES << DUTY_RES_S);
    REG(LEDC_HSTIMER0_CONF_REG) |= TIMER0_RST;      /* latch the divider */
    REG(LEDC_HSTIMER0_CONF_REG) &= ~TIMER0_RST;

    REG(LEDC_HSCH0_DUTY_REG)  = (DUTY_PERIOD / 2u) << DUTY_S;
    REG(LEDC_HSCH0_CONF1_REG) = DUTY_START;
    REG(LEDC_HSCH0_CONF0_REG) = SIG_OUT_EN;         /* timer 0, output on */

    g_current = hz;
    g_tones++;
}

void audio_off(void)
{
    /* Output off, pin parked LOW. A pin left high across a speaker is a DC path
     * through the coil for as long as nothing else happens. */
    REG(LEDC_HSCH0_CONF0_REG) = 0u;
    GPIO_REG(GPIO_FUNC_OUT_SEL(SPK_PIN)) = SIG_GPIO_OUT_IDX;
    gpio_clear(SPK_PIN);
    GPIO_REG(GPIO_FUNC_OUT_SEL(SPK_PIN)) = LEDC_HS_SIG_OUT0;

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
    /* Signed comparison, so a wrapping tick counter cannot leave a tone
     * sounding until it wraps again — the reasoning of timer.c's deadline. */
    if (g_off_tick && (int32_t)(timer_ticks() - g_off_tick) >= 0) {
        audio_off();
    }
}

void audio_click(void)
{
    /* Two ticks is about 20 ms: heard as a click rather than a note, and short
     * enough that holding a key does not become a tone. 3 kHz because that is
     * near the ear's most sensitive region, so it carries at low volume. */
    audio_beep(3000u, 2u);
}

/* ---- bring-up probes ----------------------------------------------------
 *
 * Kept because each answers a question that would otherwise be asked again.
 */
static const uint32_t SPK_PINS[] = { 26u, 25u, 4u, 16u, 17u, 0u, 22u, 27u };
static const uint32_t SPK_MUX[]  = {
    0x3FF49028u,    /* 26 — the speaker on this board                       */
    0x3FF49024u,    /* 25 — DAC1, and this kernel's touch clock             */
    0x3FF49048u,    /* 4  — RGB LED red                                     */
    0x3FF4904Cu,    /* 16 — RGB LED green                                   */
    0x3FF49050u,    /* 17 — RGB LED blue                                    */
    0x3FF49044u,    /* 0  — strapping pin, safe to toggle after boot        */
    0x3FF49080u,    /* 22 — i2c SDA                                         */
    0x3FF4902Cu,    /* 27 — i2c SCL                                         */
};
#define SPK_COUNT (sizeof SPK_PINS / sizeof SPK_PINS[0])

void audio_square(uint32_t pin, uint32_t mux, uint32_t hz, uint32_t ms)
{
    release_dac_pad(pin);
    gpio_out_init(pin, mux);

    uint32_t half = 40000000u / (hz ? hz : 1000u);
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
    for (uint32_t i = 0; i < SPK_COUNT; i++) {
        uart_puts("     gpio ");
        uart_put_dec(SPK_PINS[i]);
        uart_puts(" ...\n");
        audio_square(SPK_PINS[i], SPK_MUX[i], 1000u, 900u);
        uint32_t g = xt_ccount() + 40000000u;
        while ((int32_t)(xt_ccount() - g) < 0) {
        }
    }
    uart_puts("   done\n");
    touch_init();
    i2c_init();
    audio_init();
}

/* Warbling, 3 kHz, and long. A steady tone at the edge of hearing vanishes into
 * the noise floor; an alternating one does not, which is why alarms warble. */
void audio_hold(uint32_t pin, uint32_t mux, uint32_t seconds)
{
    release_dac_pad(pin);
    gpio_out_init(pin, mux);

    uint32_t end  = xt_ccount() + seconds * 80000000u;
    uint32_t hz   = 3000u;
    uint32_t swap = xt_ccount() + 16000000u;
    uint32_t next = xt_ccount();
    uint32_t half = 40000000u / hz;
    int level = 0;

    while ((int32_t)(xt_ccount() - end) < 0) {
        if ((int32_t)(xt_ccount() - swap) >= 0) {
            hz    = (hz == 3000u) ? 2200u : 3000u;
            half  = 40000000u / hz;
            swap += 16000000u;
        }
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
    touch_init();
    i2c_init();
    audio_init();
}

/* Proves the bit-banged generator toggles and that its edges reach a pad.
 *
 * Runs on GPIO32, the one pin this kernel can both drive and measure. Its
 * limitation is recorded here rather than learned twice: GPIO32 has no RTC mux,
 * so a pass says nothing whatever about a DAC pad. */
void audio_probe_square(void)
{
    uint32_t before = g_toggles;
    audio_square(32u, 0x3FF4901Cu, 1000u, 200u);
    uint32_t edges = g_toggles - before;

    uart_puts("   edges produced = ");
    uart_put_dec(edges);
    uart_puts("  expected ~400\n");

    gpio_out_init(32u, 0x3FF4901Cu);
    gpio_set(32u);
    uint32_t hi = adc1_read_avg(4u, 4u);
    gpio_clear(32u);
    uint32_t lo = adc1_read_avg(4u, 4u);

    uart_puts("   pad high = ");
    uart_put_dec(hi);
    uart_puts("  low = ");
    uart_put_dec(lo);
    uart_puts("\n   NOTE: gpio32 has no RTC mux, so this cannot detect a DAC\n"
              "   pad held by the RTC subsystem - the fault that mattered\n");

    touch_init();
    audio_init();
}

/* Reads back what LEDC was told, and whether its clock is on. A gated
 * peripheral's registers read back perfectly, so the clock bit is the value
 * that actually distinguishes "configured" from "running". */
void audio_dump(void)
{
    uart_puts("     perip_clk_en LEDC = ");
    uart_put_dec((REG(0x3FF000C0u) >> 11) & 1u);
    uart_puts("   perip_rst LEDC = ");
    uart_put_dec((REG(0x3FF000C4u) >> 11) & 1u);
    uart_puts("\n     ledc_conf   = ");
    uart_put_hex(REG(LEDC_CONF_REG));
    uart_puts("\n     timer0_conf = ");
    uart_put_hex(REG(LEDC_HSTIMER0_CONF_REG));
    uart_puts("\n     ch0_conf0   = ");
    uart_put_hex(REG(LEDC_HSCH0_CONF0_REG));
    uart_puts("\n     ch0_duty    = ");
    uart_put_hex(REG(LEDC_HSCH0_DUTY_REG));
    uart_puts("\n     gpio26 func = ");
    uart_put_dec(GPIO_REG(GPIO_FUNC_OUT_SEL(SPK_PIN)) & 0x1FFu);
    uart_puts("  (71 = LEDC ch0)\n");
}
