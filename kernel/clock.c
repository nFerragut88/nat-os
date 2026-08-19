/* nat-os — CPU and bus clock bring-up.
 *
 * ---- why this file exists, which is not a happy story ---------------------
 *
 * For the whole life of this project the SoC clock was configured by whoever
 * loaded the kernel. Espressif's second-stage bootloader switches the SoC to
 * the 320 MHz PLL and divides it to 80 MHz before jumping to the application;
 * nat-os inherited that and never knew it depended on it.
 *
 * UM-NATOS-035 replaced that bootloader. The replacement copies segments and
 * jumps -- it does not touch the clock -- so the kernel came up on the bare
 * 40 MHz crystal. **The board ran at half speed and nothing said so.**
 *
 * Nothing said so because every timing figure in this kernel is derived from
 * CCOUNT, and CCOUNT counts CPU cycles. Halve the clock and you halve the
 * cycles per second, so a duration measured in cycles and printed as
 * milliseconds reports the same number for twice the wall-clock time. The
 * display's "fullscreen=44 ms" was identical before and after, and it was 44 ms
 * of cycles against 88 ms of reality.
 *
 * That is this project's own standing rule arriving from a new direction:
 *
 *     a successful measurement is not evidence, if the measurement and the
 *     fault share a dependency.
 *
 * The loud symptom was elsewhere. register_chipv7_phy() hangs, because the PHY
 * asks the ROM what the crystal frequency is, and the answer is kept in an RTC
 * retention register that the bootloader is expected to fill in. Ours left it
 * at zero. A radio calibration handed a crystal frequency of zero does not
 * return.
 *
 * ---- why it lives in the kernel and not in the bootloader ------------------
 *
 * It could have gone back into the bootloader, which is where Espressif put it
 * and where the regression came from. Three reasons it is here instead:
 *
 *   - The analog PLL is programmed over an undocumented internal I2C bus. The
 *     only way in is rom_i2c_writeReg(), a WINDOWED ROM function, and calling
 *     windowed code needs the window-overflow handlers -- which are in the
 *     kernel, installed at VECBASE, and would have to be duplicated into a
 *     2.7 KB loader whose entire job is three memcpys.
 *
 *   - Choosing a CPU frequency is an operating system's decision. A loader's
 *     job is to put the kernel in memory.
 *
 *   - It makes nat-os independent of what loaded it. That is strictly better
 *     than what existed before this bug, and it is the property whose absence
 *     caused it: the kernel had a hard requirement that nothing in the kernel
 *     stated, checked, or owned.
 *
 * ---- on using a ROM function ----------------------------------------------
 *
 * rom_i2c_writeReg is in the ESP32's mask ROM. It is silicon, not a blob: it
 * cannot be replaced by anyone, including Espressif, and UM-NATOS-035 §10
 * already argued that counting it would mean counting the instruction decoder.
 * ESP-IDF itself calls the same ROM routine on this chip -- there is no
 * register-level path, because the analog I2C master is not in the TRM.
 *
 * Every constant below was read out of the SDK headers named beside it. None
 * were recalled.
 */

#include "clock.h"
#include "window.h"
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

/* soc/rtc_cntl_reg.h */
#define RTC_CNTL_OPTIONS0_REG   0x3FF48000u
#define RTC_CNTL_CLK_CONF_REG   0x3FF48070u
#define RTC_CNTL_REG_           0x3FF4807Cu   /* yes, that is its name        */
#define RTC_XTAL_FREQ_REG       0x3FF480B0u   /* = RTC_CNTL_STORE4_REG        */
#define RTC_APB_FREQ_REG        0x3FF480B4u   /* = RTC_CNTL_STORE5_REG        */

#define RTC_CNTL_BB_I2C_FORCE_PD      (1u << 6)
#define RTC_CNTL_BBPLL_I2C_FORCE_PD   (1u << 8)
#define RTC_CNTL_BBPLL_FORCE_PD       (1u << 10)

#define RTC_CNTL_SOC_CLK_SEL_S  27
#define RTC_CNTL_SOC_CLK_SEL_M  (3u << RTC_CNTL_SOC_CLK_SEL_S)
#define SOC_CLK_SEL_XTAL        0u
#define SOC_CLK_SEL_PLL         1u

#define RTC_CNTL_DIG_DBIAS_WAK_S 11
#define RTC_CNTL_DIG_DBIAS_WAK_M (7u << RTC_CNTL_DIG_DBIAS_WAK_S)
#define DIG_DBIAS_80M_160M       4u            /* RTC_CNTL_DBIAS_1V10        */

/* soc/dport_reg.h */
#define DPORT_CPU_PER_CONF_REG  0x3FF0003Cu
#define DPORT_CPUPERIOD_SEL_M   3u
#define CPUPERIOD_SEL_80M       0u

/* soc/uart_reg.h — UART0 */
#define UART0_CLKDIV_REG        0x3FF40014u
#define UART_CLKDIV_FRAG_S      20

/* soc/regi2c_bbpll.h */
#define I2C_BBPLL               0x66u
#define I2C_BBPLL_HOSTID        4u
#define I2C_BBPLL_IR_CAL_DELAY    0u
#define I2C_BBPLL_IR_CAL_EXT_CAP  1u
#define I2C_BBPLL_OC_LREF         2u
#define I2C_BBPLL_OC_DIV_7_0      3u
#define I2C_BBPLL_OC_ENB_FCAL     4u
#define I2C_BBPLL_OC_DCUR         5u
#define I2C_BBPLL_BBADC_DSMP      9u
#define I2C_BBPLL_OC_ENB_VCON    10u
#define I2C_BBPLL_ENDIV5         11u
#define I2C_BBPLL_BBADC_CAL_7_0  12u

/* hal/clk_tree_ll.h */
#define BBPLL_IR_CAL_DELAY_VAL    0x18u
#define BBPLL_IR_CAL_EXT_CAP_VAL  0x20u
#define BBPLL_OC_ENB_FCAL_VAL     0x9Au
#define BBPLL_OC_ENB_VCON_VAL     0x00u
#define BBPLL_BBADC_CAL_7_0_VAL   0x00u
#define BBPLL_ENDIV5_VAL_320M     0x43u
#define BBPLL_BBADC_DSMP_VAL_320M 0x84u

/* esp32.rom.ld */
#define ROM_I2C_WRITEREG        0x400041A4u

static uint32_t g_cpu_mhz = 40u;    /* until proven otherwise */
static int      g_switched;

static void regi2c_write(uint32_t reg, uint32_t data)
{
    rom_call4(ROM_I2C_WRITEREG, I2C_BBPLL, I2C_BBPLL_HOSTID, reg, data);
}

/* Busy-wait in CPU cycles at the CURRENT clock.
 *
 * Deliberately not the kernel's timer_*: this runs before the clock is what
 * everything else assumes, so anything that converts cycles to microseconds
 * using a compiled-in frequency would be wrong here by exactly the factor this
 * function is helping to change. */
static void spin_cycles(uint32_t n)
{
    uint32_t start, now;
    __asm__ __volatile__("rsr.ccount %0" : "=r"(start));
    do {
        __asm__ __volatile__("rsr.ccount %0" : "=r"(now));
    } while ((now - start) < n);
}

uint32_t clock_cpu_mhz(void)   { return g_cpu_mhz; }
int      clock_pll_switched(void) { return g_switched; }

uint32_t clock_source(void)
{
    return (REG(RTC_CNTL_CLK_CONF_REG) & RTC_CNTL_SOC_CLK_SEL_M)
           >> RTC_CNTL_SOC_CLK_SEL_S;
}

/* Rescales UART0's divisor for a changed APB frequency.
 *
 * The divisor is 20.4 fixed point: whole cycles in bits 19:0, sixteenths in
 * bits 23:20. Scaling the existing value by the frequency ratio keeps whatever
 * baud rate is in use without this file needing to know what it is -- and the
 * one in use is the one the ROM negotiated with esptool, which is not
 * necessarily the one a constant here would have claimed.
 *
 * Done in sixteenths throughout so the fractional part is carried rather than
 * dropped; dropping it is a 6% baud error, which is inside the range where a
 * terminal shows *mostly* correct text.
 */
static void uart_rescale(uint32_t num, uint32_t den)
{
    uint32_t v      = REG(UART0_CLKDIV_REG);
    uint32_t whole  = v & 0xFFFFFu;
    uint32_t frac   = (v >> UART_CLKDIV_FRAG_S) & 0xFu;
    uint32_t six16  = (whole * 16u + frac) * num / den;

    REG(UART0_CLKDIV_REG) = (six16 / 16u) | ((six16 % 16u) << UART_CLKDIV_FRAG_S);
}

int clock_init(uint32_t xtal_mhz)
{
    /* Already on the PLL means a bootloader did this. Espressif's does; ours
     * does not. Returning early rather than redoing it is what lets the same
     * kernel boot correctly from either, which is the entire point of moving
     * this out of the loader. */
    if (clock_source() == SOC_CLK_SEL_PLL) {
        g_cpu_mhz = 80u;
        /* Still record the crystal. Espressif's bootloader writes it, but a
         * third loader might not, and the PHY reads it rather than measuring
         * it -- a zero here is a hang inside a blob with no diagnostic. */
        REG(RTC_XTAL_FREQ_REG) = (xtal_mhz & 0xFFFFu) | ((xtal_mhz & 0xFFFFu) << 16);
        return 0;
    }

    /* Only 40 MHz is supported, and it is checked rather than assumed. The
     * divider table below is the 40 MHz row of ESP-IDF's; a 26 MHz part would
     * need the other row, and silently applying the wrong one produces a PLL
     * that locks to the wrong frequency -- every timing in the system wrong by
     * a ratio, which is exactly the failure this file was written to fix. */
    if (xtal_mhz != 40u) {
        return -1;
    }

    /* 1. Tell the ROM what the crystal is. This is the register whose being
     *    zero hangs register_chipv7_phy(). Two 16-bit copies, per
     *    clk_ll_xtal_store_freq_mhz(). */
    REG(RTC_XTAL_FREQ_REG) = (xtal_mhz & 0xFFFFu) | ((xtal_mhz & 0xFFFFu) << 16);

    /* 2. Raise the digital regulator before asking the core to go faster.
     *    Wrong order is a brownout on a board with no way to report one. */
    REG(RTC_CNTL_REG_) = (REG(RTC_CNTL_REG_) & ~RTC_CNTL_DIG_DBIAS_WAK_M)
                       | (DIG_DBIAS_80M_160M << RTC_CNTL_DIG_DBIAS_WAK_S);

    /* 3. Power up the BBPLL and its I2C interface. */
    REG(RTC_CNTL_OPTIONS0_REG) &= ~(RTC_CNTL_BB_I2C_FORCE_PD
                                  | RTC_CNTL_BBPLL_FORCE_PD
                                  | RTC_CNTL_BBPLL_I2C_FORCE_PD);

    regi2c_write(I2C_BBPLL_IR_CAL_DELAY,   BBPLL_IR_CAL_DELAY_VAL);
    regi2c_write(I2C_BBPLL_IR_CAL_EXT_CAP, BBPLL_IR_CAL_EXT_CAP_VAL);
    regi2c_write(I2C_BBPLL_OC_ENB_FCAL,    BBPLL_OC_ENB_FCAL_VAL);
    regi2c_write(I2C_BBPLL_OC_ENB_VCON,    BBPLL_OC_ENB_VCON_VAL);
    regi2c_write(I2C_BBPLL_BBADC_CAL_7_0,  BBPLL_BBADC_CAL_7_0_VAL);

    /* 4. 320 MHz from a 40 MHz crystal. div_ref=0, div7_0=32, div10_8=0,
     *    lref=0, dcur=6, bw=3 -- the 40 MHz case of clk_ll_bbpll_set_config(). */
    regi2c_write(I2C_BBPLL_ENDIV5,     BBPLL_ENDIV5_VAL_320M);
    regi2c_write(I2C_BBPLL_BBADC_DSMP, BBPLL_BBADC_DSMP_VAL_320M);
    regi2c_write(I2C_BBPLL_OC_LREF,    (0u << 7) | (0u << 4) | 0u);
    regi2c_write(I2C_BBPLL_OC_DIV_7_0, 32u);
    regi2c_write(I2C_BBPLL_OC_DCUR,    (3u << 6) | 6u);

    /* 5. Let it lock. ESP-IDF waits 1100 us with a 150 kHz slow clock; at the
     *    40 MHz we are still running, that is 44,000 cycles. Generous on
     *    purpose -- this happens once per boot and a PLL sampled before lock
     *    gives a clock that is wrong rather than absent. */
    spin_cycles(44000u * 2u);

    /* 6. 320/4 = 80 MHz, then hand the SoC over. CPUPERIOD first: selecting the
     *    divider after the source would run the core at 320 MHz for however
     *    many cycles the next instruction takes. */
    REG(DPORT_CPU_PER_CONF_REG) = (REG(DPORT_CPU_PER_CONF_REG)
                                   & ~DPORT_CPUPERIOD_SEL_M) | CPUPERIOD_SEL_80M;

    REG(RTC_CNTL_CLK_CONF_REG) = (REG(RTC_CNTL_CLK_CONF_REG) & ~RTC_CNTL_SOC_CLK_SEL_M)
                               | (SOC_CLK_SEL_PLL << RTC_CNTL_SOC_CLK_SEL_S);

    /* 7. APB just doubled and UART0 did not hear about it. Until this line the
     *    terminal is at half the baud it thinks -- so any diagnostic printed
     *    between step 6 and here would arrive as garbage, which is why there
     *    are none. */
    uart_rescale(80u, 40u);

    /* 8. Record the APB frequency where the ROM and the PHY look for it.
     *    clk_ll_apb_store_freq_hz(): value is Hz >> 12, two 16-bit copies. */
    {
        uint32_t v = (80u * 1000000u) >> 12;
        REG(RTC_APB_FREQ_REG) = (v & 0xFFFFu) | ((v & 0xFFFFu) << 16);
    }

    g_cpu_mhz  = 80u;
    g_switched = 1;
    return 0;
}
