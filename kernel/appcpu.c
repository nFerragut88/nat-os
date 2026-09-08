/* nat-os — starting the second core, as an instrument.
 *
 * See appcpu.S for what core 1 runs and why it must not touch flash.
 *
 * ---- why a single-core kernel is starting a second core -------------------
 *
 * UM-NATOS-034 §27 captured 5,095 regi2c transactions on the reference board:
 * the PHY's analog calibration, which is invisible to every other instrument in
 * this project because the analog registers are not memory-mapped. The
 * comparison needs the same capture from nat-os, and register_chipv7_phy() runs
 * synchronously — on one core there is nobody to sample while it runs.
 *
 * This is not a step toward SMP. Core 1 gets no scheduler, no interrupts, no
 * heap and no locks. It reads a peripheral word into an array and stops.
 *
 * ---- the risk, and why it is small ----------------------------------------
 *
 * Two cores sharing DRAM is where multicore bugs live. This avoids the whole
 * class by construction: core 1 writes only g_i2c_* and g_appcpu_alive, core 0
 * reads them and writes only the arm flag. There is no lock because there is
 * nothing to contend for, and there is no cache coherency question because
 * internal SRAM on the ESP32 is not cached.
 *
 * If core 1 wedges, core 0 is unaffected — they share no control flow.
 */

#include "appcpu.h"
#include "heap.h"    /* [step 374] the capture buffers are allocated on arm */
#include "uart.h"
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

/* soc/dport_reg.h */
#define DPORT_APPCPU_CTRL_A_REG   0x3FF0002Cu   /* bit 0 RESETTING   */
#define DPORT_APPCPU_CTRL_B_REG   0x3FF00030u   /* bit 0 CLKGATE_EN  */
#define DPORT_APPCPU_CTRL_C_REG   0x3FF00034u   /* bit 0 RUNSTALL    */
#define DPORT_APPCPU_CTRL_D_REG   0x3FF00038u   /* boot address      */
#define APPCPU_BIT0               (1u << 0)

/* soc/rtc_cntl_reg.h — the stall value is split across two registers, and both
 * halves must be cleared or the core stays stalled with no other symptom. */
#define RTC_CNTL_OPTIONS0_REG     0x3FF48000u
#define RTC_CNTL_SW_CPU_STALL_REG 0x3FF480ACu
#define SW_STALL_APPCPU_C0_M      (0x3u  << 0)
#define SW_STALL_APPCPU_C1_M      (0x3Fu << 20)

/* Core 1's stack. 1 KB is generous for a function with no locals worth the
 * name; it is here rather than in the linker script because core 1 is a
 * diagnostic and should leave no trace in the memory map when unused. */
static uint32_t g_appcpu_stack[256];

/* Read by appcpu.S. Not a linker symbol, so the assembly stays trivial. */
uint32_t g_appcpu_sp;

volatile uint32_t g_appcpu_alive;      /* core 1 sets this on arrival    */
volatile uint32_t g_appcpu_spins;      /* proof it is running, not stuck */
volatile uint32_t g_appcpu_run;        /* core 0 arms and disarms        */

/* ---- the capture ---------------------------------------------------------
 *
 * The regi2c host, one 32-bit word per analog block:
 *   bits  7:0  block, 15:8 register, 23:16 data.
 *
 * Five words, because §27.2 established the hard way that watching one is
 * watching the CPU clock and missing the entire radio.
 */
#define I2C_HOST_BASE  0x3FF4E000u
#define I2C_WORDS      5u

/* [step 374] The capture buffers are allocated when ARMED, not at link time.
 *
 * As static arrays these cost 49,152 bytes of DRAM for an instrument that is
 * off in every boot nobody is debugging the radio in -- and they are why the
 * -WiFi build did not link: dram overflowed by 27,828 bytes, and this was 48 KB
 * of it.
 *
 * appcpu.h records the same failure happening once already: at 8192 entries the
 * arrays pushed _bss_end past _heap_end, heap_init() saw an inverted range and
 * returned a zero-byte heap, and a self-test faulted on an arena base that had
 * never existed. The fix then was to shrink them. The fix now is to stop paying
 * for them when they are not in use, which keeps the instrument at full size
 * for the one job it exists to do.
 *
 * CORE 1 WRITES THESE. appcpu_main() runs on the other CPU with no lock, so the
 * pointers are only ever installed while g_appcpu_run is 0 -- disarm first,
 * then free -- and the capture loop reads them once per iteration through a
 * volatile pointer rather than caching them. */
uint32_t *volatile g_i2c_val;
uint32_t *volatile g_i2c_ts;
volatile uint32_t g_i2c_n;

static inline uint32_t ccount_now(void)
{
    uint32_t c;
    __asm__ __volatile__("rsr.ccount %0" : "=r"(c));
    return c;
}

void appcpu_main(void)
{
    volatile uint32_t *reg = (volatile uint32_t *)I2C_HOST_BASE;
    uint32_t last[I2C_WORDS];

    g_appcpu_alive = 0xC0DEA11Eu;

    for (uint32_t w = 0; w < I2C_WORDS; w++) {
        last[w] = reg[w];
    }

    for (;;) {
        g_appcpu_spins++;
        if (!g_appcpu_run) {
            continue;
        }
        uint32_t *vals = g_i2c_val;
        uint32_t *tss  = g_i2c_ts;
        if (!vals || !tss) {
            continue;               /* armed flag set before the buffers were */
        }
        for (uint32_t w = 0; w < I2C_WORDS; w++) {
            uint32_t v = reg[w];
            if (v != last[w]) {
                if (g_i2c_n < APPCPU_CAP_MAX) {
                    vals[g_i2c_n] = v;
                    tss[g_i2c_n]  = ccount_now();
                    g_i2c_n++;
                }
                last[w] = v;
            }
        }
    }
}

/* ---- starting it ---------------------------------------------------------
 *
 * Order taken from ESP-IDF's start_other_core(): unstall, then clock, then
 * runstall, then a reset pulse, and the boot address LAST. The ROM code the
 * core runs after reset polls CTRL_D, so setting it earlier is not merely
 * unnecessary — the reset would clear the wait it is meant to satisfy.
 *
 * Cache_Flush(1) / Cache_Read_Enable(1) are deliberately not called. They exist
 * so core 1 can execute from flash, and nothing it runs here is in flash.
 */
int appcpu_start(void)
{
    extern void appcpu_entry(void);

    if (g_appcpu_alive == 0xC0DEA11Eu) {
        return 1;                        /* already up; starting twice resets it */
    }

    g_appcpu_sp = (uint32_t)&g_appcpu_stack[256];

    REG(RTC_CNTL_OPTIONS0_REG)     &= ~SW_STALL_APPCPU_C0_M;
    REG(RTC_CNTL_SW_CPU_STALL_REG) &= ~SW_STALL_APPCPU_C1_M;

    REG(DPORT_APPCPU_CTRL_B_REG) |=  APPCPU_BIT0;   /* clock on      */
    REG(DPORT_APPCPU_CTRL_C_REG) &= ~APPCPU_BIT0;   /* not stalled   */
    REG(DPORT_APPCPU_CTRL_A_REG) |=  APPCPU_BIT0;   /* assert reset  */
    REG(DPORT_APPCPU_CTRL_A_REG) &= ~APPCPU_BIT0;   /* release       */

    REG(DPORT_APPCPU_CTRL_D_REG) = (uint32_t)&appcpu_entry;

    return 0;
}

uint32_t appcpu_alive(void)  { return g_appcpu_alive; }
uint32_t appcpu_spins(void)  { return g_appcpu_spins; }
/* [step 374] Arming allocates; disarming frees. The order matters in both
 * directions because core 1 is reading these without a lock:
 *
 *   arming    buffers first, THEN the run flag
 *   disarming run flag first, THEN the buffers
 *
 * The capture loop also refuses to run on a null pointer, so a failed
 * allocation disarms rather than faulting the other CPU -- and says so, because
 * an instrument that quietly recorded nothing would be worse than one that
 * refused. */
void appcpu_arm(int on)
{
    if (!on) {
        g_appcpu_run = 0u;
        uint32_t *v = g_i2c_val, *t = g_i2c_ts;
        g_i2c_val = 0; g_i2c_ts = 0;
        if (v) { heap_free(v); }
        if (t) { heap_free(t); }
        return;
    }
    if (g_appcpu_run) { return; }               /* already capturing */

    uint32_t bytes = APPCPU_CAP_MAX * (uint32_t)sizeof(uint32_t);
    uint32_t *v = (uint32_t *)heap_alloc(bytes);
    uint32_t *t = (uint32_t *)heap_alloc(bytes);
    if (!v || !t) {
        if (v) { heap_free(v); }
        if (t) { heap_free(t); }
        uart_puts("   appcpu: no room to arm the capture (needs 2 x ");
        uart_put_dec(bytes);
        uart_puts(" bytes)\n");
        return;
    }
    g_i2c_n   = 0u;
    g_i2c_val = v;
    g_i2c_ts  = t;
    g_appcpu_run = 1u;
}

uint32_t appcpu_cap_count(void) { return g_i2c_n; }
uint32_t appcpu_cap_val(uint32_t i)
{
    uint32_t *v = g_i2c_val;
    return (v && i < g_i2c_n) ? v[i] : 0u;
}
uint32_t appcpu_cap_ts(uint32_t i)
{
    uint32_t *t = g_i2c_ts;
    return (t && i < g_i2c_n) ? t[i] : 0u;
}
