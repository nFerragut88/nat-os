/* nat-os — sample playback: I2S0 -> DAC2 (GPIO26) by DMA. See pcm.h.
 *
 * Every register offset and field position below was read out of the vendor's
 * i2s_reg.h / i2s_struct.h / sens_reg.h / rtc_io_reg.h / dport_reg.h, and the
 * ORDER of the bring-up out of IDF's components/driver/dac/esp32/dac_dma.c and
 * hal/esp32/include/hal/i2s_ll.h. None of it is recalled.
 */

#include "pcm.h"
#include "audio.h"
#include "task.h"
#include "timer.h"
#include "uart.h"
#include "xtensa.h"
#include "generated/sintab.h"

#define REG(a) (*(volatile uint32_t *)(a))

/* ---- DPORT ----------------------------------------------------------------- */
#define DPORT_PERIP_CLK_EN  0x3FF000C0u
#define DPORT_PERIP_RST_EN  0x3FF000C4u
#define DPORT_I2S0          (1u << 4)       /* DPORT_I2S0_CLK_EN / DPORT_I2S0_RST */

/* ---- I2S0 ------------------------------------------------------------------ */
#define I2S0                0x3FF4F000u
#define I2S_CONF            (I2S0 + 0x008u)
#define I2S_INT_RAW         (I2S0 + 0x00Cu)
#define I2S_INT_CLR         (I2S0 + 0x018u)
#define I2S_FIFO_CONF       (I2S0 + 0x020u)
#define I2S_CONF_CHAN       (I2S0 + 0x02Cu)
#define I2S_OUT_LINK        (I2S0 + 0x030u)
#define I2S_OUT_EOF_DES     (I2S0 + 0x038u)
#define I2S_OUTLINK_DSCR    (I2S0 + 0x054u)
#define I2S_LC_CONF         (I2S0 + 0x060u)
#define I2S_CONF1           (I2S0 + 0x0A0u)
#define I2S_CONF2           (I2S0 + 0x0A8u)
#define I2S_CLKM_CONF       (I2S0 + 0x0ACu)
#define I2S_SAMPLE_RATE     (I2S0 + 0x0B0u)

/* conf */
#define C_TX_RESET          (1u << 0)
#define C_TX_FIFO_RESET     (1u << 2)
#define C_TX_START          (1u << 4)
#define C_TX_SLAVE          (1u << 6)
#define C_TX_RIGHT_FIRST    (1u << 8)
#define C_TX_MSB_SHIFT      (1u << 10)
#define C_TX_SHORT_SYNC     (1u << 12)
#define C_TX_MONO           (1u << 14)
#define C_TX_MSB_RIGHT      (1u << 16)
/* fifo_conf */
#define F_DSCR_EN           (1u << 12)
#define F_TX_FIFO_MOD_S     13u             /* 3 bits */
#define F_TX_FIFO_MOD_FORCE (1u << 19)
/* out_link */
#define L_ADDR_M            0xFFFFFu
#define L_STOP              (1u << 28)
#define L_START             (1u << 29)
/* lc_conf */
#define LC_OUT_RST          (1u << 1)
#define LC_AHBM_FIFO_RST    (1u << 2)
#define LC_AHBM_RST         (1u << 3)
#define LC_OUT_AUTO_WRBACK  (1u << 6)
#define LC_OUT_EOF_MODE     (1u << 8)
/* conf2 */
#define C2_LCD_EN           (1u << 5)
#define C2_CAMERA_EN        (1u << 0)
/* clkm_conf */
#define CK_DIV_NUM_S        0u              /* 8 bits */
#define CK_DIV_B_S          8u              /* 6 bits */
#define CK_DIV_A_S          14u             /* 6 bits */
#define CK_CLK_EN           (1u << 20)
#define CK_CLKA_EN          (1u << 21)      /* 0 = PLL_D2, 160 MHz */
/* sample_rate_conf */
#define SR_TX_BCK_DIV_S     0u              /* 6 bits */
#define SR_TX_BITS_S        12u             /* 6 bits */
/* int bits */
#define INT_OUT_EOF         (1u << 12)
#define INT_OUT_DSCR_ERR    (1u << 14)

#define I2S_SCLK_HZ         160000000u      /* PLL_D2, what clka_en=0 selects */
#define BCK_DIV             16u             /* dac_dma.c: DAC_DMA_PERIPH_I2S_BIT_WIDTH */

/* ---- SENS / RTCIO: the DAC itself ----------------------------------------- */
#define SENS_DAC_CTRL1      0x3FF48898u     /* SENS_SAR_DAC_CTRL1_REG */
#define SENS_DAC_CTRL2      0x3FF4889Cu     /* SENS_SAR_DAC_CTRL2_REG */
#define SENS_MEAS_CTRL2     0x3FF488A0u     /* SENS_SAR_MEAS_CTRL2_REG */
#define DAC_DIG_FORCE       (1u << 22)      /* I2S drives the DAC, not the register */
#define DAC_CLK_INV         (1u << 25)
#define DAC_CW_EN2          (1u << 25)      /* in CTRL2: cosine generator -> DAC2 */
#define SAR1_DAC_XPD_FSM_M  0xFu            /* ADC FSM may power the DAC; IDF clears */

#define PAD_DAC2            0x3FF48488u     /* RTC_IO_PAD_DAC2_REG */
#define PDAC_DAC_S          19u             /* 8-bit code, used by the direct path */
#define PDAC_DAC_M          (0xFFu << PDAC_DAC_S)
#define PDAC_XPD_DAC        (1u << 18)
#define PDAC_MUX_SEL        (1u << 17)
#define PDAC_FUN_SEL_M      (3u << 15)
#define PDAC_RUE            (1u << 27)
#define PDAC_RDE            (1u << 28)
#define PDAC_XPD_FORCE      (1u << 10)

#define CPU_HZ              80000000u       /* clock.c; verified in UM-NATOS-036 s11 */

/* ---- the ring -------------------------------------------------------------
 *
 * lldesc_t, as the hardware reads it: size[11:0], length[23:12], eof[30],
 * owner[31]; then the buffer; then the next descriptor. */
typedef struct {
    volatile uint32_t ctrl;
    volatile uint32_t buf;
    volatile uint32_t next;
} desc_t;

#define D_BYTES     (PCM_SAMPLES * 2u)
#define D_CTRL      (D_BYTES | (D_BYTES << 12) | (1u << 30) | (1u << 31))

#define SRAM1 __attribute__((section(".sram1")))
SRAM1 static desc_t   g_ring_desc[PCM_BUFS];
SRAM1 static uint16_t g_ring[PCM_BUFS * PCM_SAMPLES];
static desc_t   *g_desc;
static uint16_t *g_buf;             /* PCM_BUFS * PCM_SAMPLES */
static int       g_running;
static uint32_t  g_rate;

/* Producer state. The DMA is reading buffer `cur` = last_eof + 1; `queued`
 * counts cur and everything after it that holds audio we wrote. The writer
 * fills at cur + queued, which is only safe while queued < PCM_BUFS. */
static uint32_t  g_last_eof;
static uint32_t  g_queued;
static uint32_t  g_fill;            /* samples already in the writer's buffer */

static uint32_t  g_played;
static uint32_t  g_underruns;
static uint32_t  g_blind;
static uint32_t  g_bogus_eof;
static uint32_t  g_start_cc;
static uint32_t  g_last_poll_cc;
static uint32_t  g_cc_hi;           /* CCOUNT wraps every 53 s at 80 MHz */
static uint32_t  g_elapsed_ms;

uint32_t pcm_rate(void)            { return g_rate; }
uint32_t pcm_underruns(void)       { return g_underruns; }
uint32_t pcm_blind(void)           { return g_blind; }
uint32_t pcm_buffers_played(void)  { return g_played; }
int      pcm_running(void)         { return g_running; }

/* 16-bit signed -> the DAC's unsigned 8 bits, in the high byte of the slot. */
static inline uint16_t to_slot(int16_t s)
{
    return (uint16_t)(((uint32_t)(int32_t)s + 32768u) & 0xFF00u);
}

/* Accumulates elapsed time in ms from CCOUNT deltas, so the rate measurement
 * survives CCOUNT wrapping as long as polls are less than 53 s apart. */
static void account_time(uint32_t now)
{
    uint32_t d = now - g_last_poll_cc;
    g_cc_hi += d;
    while (g_cc_hi >= CPU_HZ / 1000u) {
        g_cc_hi -= CPU_HZ / 1000u;
        g_elapsed_ms++;
    }
    g_last_poll_cc = now;
}

uint32_t pcm_rate_actual(void)
{
    if (g_elapsed_ms < 100u) {
        return 0;
    }
    /* samples * 1000 / ms without 64-bit division: this kernel links no
     * libgcc, so a `/` on a uint64_t is a call to nothing (wifimac.c has the
     * scar). Good to ~70 minutes, and +-1 buffer, since `played` counts only
     * buffers the DMA has finished. */
    uint32_t s = g_played * PCM_SAMPLES;
    return (s / g_elapsed_ms) * 1000u + ((s % g_elapsed_ms) * 1000u) / g_elapsed_ms;
}

/* Reads where the DMA has got to and retires what it has finished. */
static void poll(void)
{
    uint32_t now = xt_ccount();
    uint32_t gap = now - g_last_poll_cc;
    account_time(now);

    /* A gap long enough to lap the ring means the index arithmetic below can
     * undercount by whole laps, invisibly. Say so rather than trust it. */
    uint32_t lap_cc = (PCM_BUFS - 1u) * PCM_SAMPLES * (CPU_HZ / g_rate);
    if (gap >= lap_cc) {
        g_blind++;
    }

    uint32_t a = REG(I2S_OUT_EOF_DES);
    uint32_t base = (uint32_t)g_desc;
    if (a < base || a >= base + PCM_BUFS * sizeof(desc_t)
                 || ((a - base) % sizeof(desc_t)) != 0u) {
        /* 0 before the first EOF, which is expected; anything else is not. */
        if (a != 0u) {
            g_bogus_eof++;
        }
        return;
    }
    uint32_t idx = (a - base) / sizeof(desc_t);
    uint32_t adv = (idx + PCM_BUFS - g_last_eof) % PCM_BUFS;
    g_last_eof = idx;
    g_played += adv;

    if (adv >= g_queued) {
        /* The DMA went past everything we had written and is now replaying a
         * stale buffer. Count it, and restart the writer just AFTER the buffer
         * being played, so what we write next is heard next. A partly filled
         * buffer is abandoned: it may be the one now being replayed. */
        g_underruns++;
        g_queued = 1u;
        g_fill   = 0u;
    } else {
        g_queued -= adv;
    }
}

uint32_t pcm_space(void)
{
    if (!g_running) {
        return 0;
    }
    poll();
    if (g_queued >= PCM_BUFS) {
        return 0;
    }
    return (PCM_BUFS - g_queued) * PCM_SAMPLES - g_fill;
}

uint32_t pcm_write(const int16_t *s, uint32_t n)
{
    if (!g_running) {
        return 0;
    }
    poll();
    uint32_t took = 0;
    while (took < n && g_queued < PCM_BUFS) {
        uint32_t w = (g_last_eof + 1u + g_queued) % PCM_BUFS;
        uint16_t *dst = g_buf + w * PCM_SAMPLES + g_fill;
        uint32_t k = PCM_SAMPLES - g_fill;
        if (k > n - took) {
            k = n - took;
        }
        for (uint32_t i = 0; i < k; i++) {
            dst[i] = to_slot(s[took + i]);
        }
        took   += k;
        g_fill += k;
        if (g_fill == PCM_SAMPLES) {
            g_fill = 0;
            g_queued++;
        }
    }
    return took;
}

/* mclk = rate * 2 (bclk) * 16 (bck_div). The divider is integ + b/a with a and
 * b six bits each, chosen here by trying every a -- 63 iterations, once. */
static void set_clock(uint32_t rate)
{
    uint32_t mclk  = rate * 2u * BCK_DIV;
    uint32_t integ = I2S_SCLK_HZ / mclk;
    uint32_t rem   = I2S_SCLK_HZ % mclk;
    uint32_t best_a = 1, best_b = 0;
    uint32_t best_err = 0xFFFFFFFFu;
    /* All 32-bit: rem < mclk <= 3.1 MHz, so rem * 63 < 2^31. */
    for (uint32_t a = 1; a < 64u; a++) {
        uint32_t b = (rem * a + mclk / 2u) / mclk;
        if (b >= a) {
            continue;
        }
        /* |rem/mclk - b/a|, scaled by 64 * mclk */
        int32_t e = (int32_t)(rem * a) - (int32_t)(b * mclk);
        uint32_t err = (uint32_t)(e < 0 ? -e : e) * 64u / a;
        if (err < best_err) {
            best_err = err;
            best_a = a;
            best_b = b;
        }
    }
    /* dac_dma.c goes through i2s_ll_tx_set_mclk(), which writes a throwaway
     * divider first to dodge an inaccuracy on switching rates. Copied. */
    REG(I2S_CLKM_CONF) = CK_CLK_EN | (7u << CK_DIV_NUM_S)
                       | (3u << CK_DIV_B_S) | (47u << CK_DIV_A_S);
    REG(I2S_CLKM_CONF) = CK_CLK_EN | (integ << CK_DIV_NUM_S)
                       | (best_b << CK_DIV_B_S) | (best_a << CK_DIV_A_S);
    REG(I2S_SAMPLE_RATE) = (BCK_DIV << SR_TX_BCK_DIV_S) | (16u << SR_TX_BITS_S);
}

static void dac_pad_claim(void)
{
    /* Stop LEDC first: a tone left running is harmless once RTC owns the pad,
     * but it would resume the instant the pad was released. */
    audio_off();
    uint32_t p = REG(PAD_DAC2);
    p &= ~(PDAC_FUN_SEL_M | PDAC_RUE | PDAC_RDE | PDAC_DAC_M);
    p |= PDAC_MUX_SEL | PDAC_XPD_DAC | PDAC_XPD_FORCE | (0x80u << PDAC_DAC_S);
    REG(PAD_DAC2) = p;
    REG(SENS_MEAS_CTRL2) &= ~SAR1_DAC_XPD_FSM_M;
}

static void dac_pad_release(void)
{
    REG(SENS_DAC_CTRL1) &= ~(DAC_DIG_FORCE | DAC_CLK_INV);
    REG(PAD_DAC2) &= ~(PDAC_XPD_DAC | PDAC_XPD_FORCE);
    /* audio_init() clears MUX_SEL and re-routes LEDC -- the state 027 fought
     * for, restored by the code that established it rather than by a copy. */
    audio_init();
}

int pcm_start(uint32_t rate)
{
    if (rate < PCM_RATE_MIN || rate > PCM_RATE_MAX) {
        return -1;
    }
    if (g_running) {
        pcm_stop();
    }
    g_desc = g_ring_desc;
    g_buf  = g_ring;
    for (uint32_t i = 0; i < PCM_BUFS * PCM_SAMPLES; i++) {
        g_buf[i] = 0x8000u;
    }
    for (uint32_t i = 0; i < PCM_BUFS; i++) {
        g_desc[i].ctrl = D_CTRL;
        g_desc[i].buf  = (uint32_t)(g_buf + i * PCM_SAMPLES);
        g_desc[i].next = (uint32_t)&g_desc[(i + 1u) % PCM_BUFS];
    }

    /* Clock on, then a clean reset, as for LEDC (027 section 3.3): a gated
     * peripheral accepts every write below and does nothing with any of it. */
    REG(DPORT_PERIP_CLK_EN) |= DPORT_I2S0;
    REG(DPORT_PERIP_RST_EN) |= DPORT_I2S0;
    REG(DPORT_PERIP_RST_EN) &= ~DPORT_I2S0;

    /* i2s_ll_enable_clock(): clk_en, and conf2 zeroed. */
    REG(I2S_CLKM_CONF) |= CK_CLK_EN;
    REG(I2S_CONF2) = 0;

    g_rate = rate;
    set_clock(rate);

    /* i2s_ll_enable_builtin_dac(true): LCD mode on, right-first, no MSB shift,
     * no short sync. Then the slot setup dac_dma_periph_init() does. */
    REG(I2S_CONF2) = C2_LCD_EN;
    uint32_t c = REG(I2S_CONF);
    c &= ~(C_TX_SLAVE | C_TX_MSB_SHIFT | C_TX_SHORT_SYNC | C_TX_MSB_RIGHT | C_TX_MONO);
    c |= C_TX_RIGHT_FIRST;
    REG(I2S_CONF) = c;

    uint32_t f = REG(I2S_FIFO_CONF);
    f &= ~(7u << F_TX_FIFO_MOD_S);
    f |= (1u << F_TX_FIFO_MOD_S)        /* 16-bit, single channel: mono */
       | F_TX_FIFO_MOD_FORCE | F_DSCR_EN;
    REG(I2S_FIFO_CONF) = f;
    REG(I2S_CONF_CHAN) = (REG(I2S_CONF_CHAN) & ~7u) | 1u;   /* mono, both slots */

    /* Reset tx, its DMA and its FIFO -- s_dac_dma_periph_reset(). */
    REG(I2S_CONF) |= C_TX_RESET;       REG(I2S_CONF) &= ~C_TX_RESET;
    REG(I2S_LC_CONF) |= LC_OUT_RST | LC_AHBM_RST | LC_AHBM_FIFO_RST;
    REG(I2S_LC_CONF) &= ~(LC_OUT_RST | LC_AHBM_RST | LC_AHBM_FIFO_RST);
    REG(I2S_CONF) |= C_TX_FIFO_RESET;  REG(I2S_CONF) &= ~C_TX_FIFO_RESET;
    REG(I2S_LC_CONF) |= LC_OUT_AUTO_WRBACK | LC_OUT_EOF_MODE;
    REG(I2S_INT_CLR) = 0xFFFFFFFFu;

    /* The DAC: pad to RTC, powered, and switched to the digital (I2S) source. */
    dac_pad_claim();
    REG(SENS_DAC_CTRL1) |= DAC_DIG_FORCE | DAC_CLK_INV;

    /* The whole ring is silence and counts as queued, and the DMA starts on
     * buffer 0 -- as though buffer PCM_BUFS-1 had just finished. */
    g_last_eof  = PCM_BUFS - 1u;
    g_queued    = PCM_BUFS;
    g_fill      = 0;
    g_played    = 0;
    g_underruns = 0;
    g_blind     = 0;
    g_bogus_eof = 0;
    g_cc_hi     = 0;
    g_elapsed_ms = 0;

    REG(I2S_OUT_LINK) = ((uint32_t)&g_desc[0] & L_ADDR_M);
    REG(I2S_OUT_LINK) |= L_START;
    REG(I2S_CONF) |= C_TX_START;

    g_start_cc = g_last_poll_cc = xt_ccount();
    g_running = 1;
    return 0;
}

void pcm_stop(void)
{
    if (!g_running) {
        return;
    }
    poll();                         /* bring the counters up to the moment */
    REG(I2S_CONF) &= ~C_TX_START;
    REG(I2S_OUT_LINK) |= L_STOP;
    REG(I2S_FIFO_CONF) &= ~F_DSCR_EN;
    REG(I2S_LC_CONF) &= ~(LC_OUT_AUTO_WRBACK | LC_OUT_EOF_MODE);
    dac_pad_release();
    g_running = 0;
    g_buf  = 0;
    g_desc = 0;
}

void pcm_dump(void)
{
    uart_puts("   pcm ");
    uart_puts(g_running ? "RUNNING" : "stopped");
    uart_puts("  rate asked=");
    uart_put_dec(g_rate);
    uart_puts(" measured=");
    uart_put_dec(pcm_rate_actual());
    uart_puts("  over ");
    uart_put_dec(g_elapsed_ms);
    uart_puts(" ms\n   buffers played=");
    uart_put_dec(g_played);
    uart_puts(" queued=");
    uart_put_dec(g_queued);
    uart_puts(" underruns=");
    uart_put_dec(g_underruns);
    uart_puts(" blind=");
    uart_put_dec(g_blind);
    uart_puts(" bogus_eof=");
    uart_put_dec(g_bogus_eof);
    uart_puts("\n     i2s clk_en=");
    uart_put_dec((REG(DPORT_PERIP_CLK_EN) >> 4) & 1u);
    uart_puts(" rst=");
    uart_put_dec((REG(DPORT_PERIP_RST_EN) >> 4) & 1u);
    uart_puts("  conf=");      uart_put_hex(REG(I2S_CONF));
    uart_puts(" conf2=");      uart_put_hex(REG(I2S_CONF2));
    uart_puts(" fifo=");       uart_put_hex(REG(I2S_FIFO_CONF));
    uart_puts("\n     clkm=");  uart_put_hex(REG(I2S_CLKM_CONF));
    uart_puts(" srate=");      uart_put_hex(REG(I2S_SAMPLE_RATE));
    uart_puts(" int_raw=");    uart_put_hex(REG(I2S_INT_RAW));
    uart_puts(" outlink=");    uart_put_hex(REG(I2S_OUT_LINK));
    uart_puts("\n     dscr now="); uart_put_hex(REG(I2S_OUTLINK_DSCR));
    uart_puts(" eof_des=");    uart_put_hex(REG(I2S_OUT_EOF_DES));
    uart_puts(" ring=");       uart_put_hex((uint32_t)g_desc);
    uart_puts("\n     pad_dac2="); uart_put_hex(REG(PAD_DAC2));
    uart_puts(" dac_ctrl1=");  uart_put_hex(REG(SENS_DAC_CTRL1));
    uart_puts(" dac_ctrl2=");  uart_put_hex(REG(SENS_DAC_CTRL2));
    uart_puts("\n");
    if (REG(I2S_INT_RAW) & INT_OUT_DSCR_ERR) {
        uart_puts("   OUT_DSCR_ERR is set: the DMA rejected a descriptor\n");
    }
}

/* ---- tests ----------------------------------------------------------------
 *
 * All three play the same kind of signal, so the ear compares like with like.
 * Frequencies default near 3 kHz because UM-NATOS-027 section 4 found 440 Hz
 * inaudible on this speaker: a correct tone the transducer cannot move is
 * indistinguishable from a broken driver, and that was learned once already.
 */

/* Phase accumulator: 32-bit phase, top 8 bits index the 256-entry table. */
static inline int16_t sine_at(uint32_t phase, int32_t amp)
{
    return (int16_t)((SIN_TAB[phase >> 24] * amp) >> 16);
}

/* The CONTROL for the DMA path. Same DAC, same pad, no I2S and no DMA: the
 * CPU writes the 8-bit code straight into PDAC2_DAC at the sample rate, timed
 * off CCOUNT. If this is audible and the DMA path is not, the fault is in I2S
 * or its DMA; if neither is, it is in the DAC or the pad -- one variable apart.
 *
 * Preemptible on purpose, so expect the odd crackle when another task runs:
 * masking interrupts for seconds would stop the tick the watchdog feeds on. */
static void test_direct(uint32_t hz, uint32_t ms)
{
    const uint32_t rate = 22050u;
    audio_off();
    dac_pad_claim();
    REG(SENS_DAC_CTRL1) &= ~(DAC_DIG_FORCE | DAC_CLK_INV);
    REG(SENS_DAC_CTRL2) &= ~DAC_CW_EN2;         /* dac_ll_update_output_value */

    uint32_t step  = ((hz << 16) / rate) << 16;  /* no 64-bit divide; see above */
    uint32_t per   = CPU_HZ / rate;
    uint32_t next  = xt_ccount();
    uint32_t end   = next + ms * (CPU_HZ / 1000u);
    uint32_t phase = 0, late = 0, n = 0;

    while ((int32_t)(xt_ccount() - end) < 0) {
        while ((int32_t)(xt_ccount() - next) < 0) {
        }
        if ((int32_t)(xt_ccount() - next) > (int32_t)(4u * per)) {
            late++;
            next = xt_ccount();                 /* do not try to catch up */
        }
        uint32_t code = (uint32_t)(sine_at(phase, 32000) + 32768) >> 8;
        REG(PAD_DAC2) = (REG(PAD_DAC2) & ~PDAC_DAC_M) | (code << PDAC_DAC_S);
        phase += step;
        next  += per;
        n++;
    }
    uart_puts("   direct: ");
    uart_put_dec(n);
    uart_puts(" samples written, ");
    uart_put_dec(late);
    uart_puts(" times preempted for >4 samples\n");
    REG(PAD_DAC2) = (REG(PAD_DAC2) & ~PDAC_DAC_M) | (0x80u << PDAC_DAC_S);
    dac_pad_release();
}

/* The real path: DMA, fed by pcm_write() from a loop that yields between
 * refills. `sweep` glides hz0 -> hz1 across the duration. */
static void test_dma(uint32_t rate, uint32_t hz0, uint32_t hz1, uint32_t ms)
{
    int r = pcm_start(rate);
    if (r) {
        uart_puts(r == -1 ? "   rate out of range (19600..48000)\n"
                          : "   no memory for the ring\n");
        return;
    }
    int16_t chunk[64];
    uint32_t phase = 0;
    uint32_t total = rate / 10u * (ms / 100u);   /* ms a multiple of 100 */
    uint32_t done  = 0;

    /* hz1 >= hz0. Scaled by 256 so (hz1-hz0) * done stays under 2^32. */
    while (done < total) {
        uint32_t hz = hz0 + (hz1 - hz0) * (done >> 8) / ((total >> 8) + 1u);
        uint32_t step = ((hz << 16) / rate) << 16;
        uint32_t k = pcm_space();
        if (k == 0) {
            task_yield();
            continue;
        }
        if (k > 64u) {
            k = 64u;
        }
        for (uint32_t i = 0; i < k; i++) {
            chunk[i] = sine_at(phase, 30000);
            phase += step;
        }
        done += pcm_write(chunk, k);
    }
    /* Let the tail play out, feeding SILENCE behind it. Merely waiting would
     * let the ring lap and replay its last 186 ms, and count that as an
     * underrun the test itself caused. */
    for (uint32_t i = 0; i < 64u; i++) {
        chunk[i] = 0;
    }
    uint32_t drain = timer_ticks() + 25u;
    while ((int32_t)(timer_ticks() - drain) < 0) {
        if (!pcm_write(chunk, 64u)) {
            task_yield();
        }
    }
    pcm_dump();
    pcm_stop();
}

/* Parses a decimal of any length the uint32_t holds. -1 on anything else. */
static int32_t num(const char *s)
{
    if (!*s) {
        return -1;
    }
    uint32_t v = 0;
    for (; *s && *s != ' '; s++) {
        if (*s < '0' || *s > '9' || v > 100000000u) {
            return -1;
        }
        v = v * 10u + (uint32_t)(*s - '0');
    }
    return (int32_t)v;
}

static char *next_word(char *s)
{
    while (*s && *s != ' ') { s++; }
    while (*s == ' ') { s++; }
    return s;
}

static int word_is(const char *s, const char *w)
{
    while (*w) {
        if (*s++ != *w++) {
            return 0;
        }
    }
    return *s == 0 || *s == ' ';
}

void pcm_shell(char *arg)
{
    char *a1 = next_word(arg);
    char *a2 = next_word(a1);
    int32_t n1 = num(a1), n2 = num(a2);

    if (!*arg) {
        pcm_dump();
    } else if (word_is(arg, "direct")) {
        uint32_t hz = n1 > 0 ? (uint32_t)n1 : 3000u;
        uart_puts("   CPU writing the DAC directly, sine, 3 s - listen\n");
        test_direct(hz, 3000u);
    } else if (word_is(arg, "tone")) {
        uint32_t hz = n1 > 0 ? (uint32_t)n1 : 3000u;
        uint32_t rate = n2 > 0 ? (uint32_t)n2 : 22050u;
        uart_puts("   I2S+DMA sine, 3 s - listen\n");
        test_dma(rate, hz, hz, 3000u);
    } else if (word_is(arg, "sweep")) {
        uint32_t rate = n1 > 0 ? (uint32_t)n1 : 22050u;
        uart_puts("   I2S+DMA sweep 500 -> 6000 Hz, 5 s - listen for a glide\n");
        test_dma(rate, 500u, 6000u, 5000u);
    } else if (word_is(arg, "stop")) {
        pcm_stop();
        uart_puts("   stopped\n");
    } else {
        uart_puts("   pcm                    status and registers\n"
                  "   pcm direct [hz]        CONTROL: CPU writes the DAC, no DMA\n"
                  "   pcm tone [hz] [rate]   I2S+DMA sine, default 3000 Hz @ 22050\n"
                  "   pcm sweep [rate]       I2S+DMA glide 500..6000 Hz\n"
                  "   pcm stop\n");
    }
}
