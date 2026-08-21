/* nat-os — bringing Espressif's PHY up. See vendor/phy/README.md.
 *
 * This is the first code in the project that touches the radio. Everything
 * before it — the window handlers, the ROM calls, phy_version_print — was
 * provably inert: it could not affect hardware even if it was wrong. This can.
 *
 * register_chipv7_phy() takes three arguments, so it goes through rom_call3()
 * unchanged. What it needs beyond that is:
 *
 *   - the WiFi/BT common clock, ungated in DPORT. Without it the PHY's
 *     registers are dead and it hangs waiting on a peripheral that is not
 *     running — the same shape as the LEDC clock gate in UM-NATOS-027 §3.3,
 *     which is the third time that pattern has appeared in this project.
 *   - 128 bytes of initialisation data. This is Espressif's own default table,
 *     copied from phy_init_data.h; the 107 listed values are theirs and the
 *     remaining 21 are the zeros C fills in.
 *   - a calibration buffer it writes into. Normally this is persisted to flash
 *     and reused; here it is .bss, so every run pays for a full calibration.
 *
 * PHY_RF_CAL_FULL is chosen deliberately over PARTIAL or NONE: the other two
 * expect calibration data from a previous run, and there has never been one.
 */

#include "phyinit.h"
#include "window.h"
#include "blobcall.h"
#include "task.h"
#include "uart.h"
#include "efuse.h"

#define DPORT_WIFI_CLK_EN_REG        0x3FF000CCu
#define DPORT_WIFI_CLK_WIFI_BT_COMMON 0x000003C9u

#define PHY_RF_CAL_FULL 2u

/* Espressif's default PHY initialisation parameters.
 *
 * ---- indices 44..49, and how they were wrong ------------------------------
 *
 * These six are the TRANSMIT POWER table, in units of 0.25 dBm, one per rate
 * group. In ESP-IDF's phy_init_data.h they are written as
 *
 *     LIMIT(CONFIG_ESP_PHY_MAX_TX_POWER * 4, 40, 78)
 *     LIMIT(CONFIG_ESP_PHY_MAX_TX_POWER * 4, 40, 72)   ... and so on
 *
 * with `#define LIMIT(val, low, high) ((val < low) ? low : (val > high) ? high
 * : val)` and a default max of 20 dBm. So the values ESP-IDF actually passes
 * are 80 clamped to each ceiling: **78, 72, 66, 60, 56, 52**.
 *
 * This table originally held `40, 40, 40, 40, 40, 40` -- the macro's LOW bound,
 * copied six times instead of evaluated. nat-os was transmitting between 3 and
 * 9.5 dB below the reference at every rate, with 10.0 dBm where ESP-IDF uses
 * 19.5.
 *
 * Found by diffing the arguments rather than the registers. Every other byte of
 * the 128 matches exactly; these six were the whole difference.
 */
static const uint8_t g_phy_init_data[128] = {
    3, 3, 0x05, 0x09, 0x06, 0x05, 0x03, 0x06, 0x05, 0x04, 0x06, 0x04,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x05, 0x09, 0x06, 0x05, 0x03, 0x06, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0xfc, 0xfe, 0xf0,
    0xf0, 0xf0, 0xe0, 0xe0, 0xe0, 0x18, 0x18, 0x18, 78, 72, 66, 60,
    56, 52, 0, 1, 1, 2, 2, 3, 4, 5, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

/* esp_phy_calibration_data_t: version[4] + mac[6] + opaque[1894]. */
static uint8_t g_phy_cal_data[1904];

static uint32_t g_phy_result;
static int      g_phy_attempted;

uint32_t phyinit_result(void)    { return g_phy_result; }
int      phyinit_attempted(void) { return g_phy_attempted; }

/* The address is a PARAMETER, so this compiles with no Espressif symbol at
 * link time and works for either copy of libphy: the one a -WiFi build links
 * into IRAM, or the one inside the loaded blob. The blob's copy is the one
 * that matters now -- it has its own .bss, so initialising the kernel's copy
 * and transmitting through the blob's would calibrate a PHY nobody uses. */
int phyinit_run_at(uint32_t fn)
{

    /* Once per boot, and the guard is not a nicety.
     *
     * A second call faults with exccause 29 (StoreProhibited) inside the blob.
     * ESP-IDF calls this exactly once and so must anything else: the PHY keeps
     * state across the call, and handing it a fresh calibration buffer while it
     * believes it is already initialised is not a supported thing to do.
     *
     * Found by doing it, on the assumption that a function returning 0 could be
     * called twice. */
    if (g_phy_attempted) {
        return -1;
    }

    /* Prime the PHY stack here, once, rather than at each call site.
     *
     * The panic handler reports its high-water mark, and an UNPRIMED stack is
     * all zeros -- which the scan reads as "entirely used" and prints as
     * 6144 of 6144. That is not a big number, it is a missing measurement, and
     * it appeared in a real panic report during transmit bring-up looking
     * exactly like a stack exhaustion that had not happened. */
    phy_stack_prime();

    /* Ungate the radio's clock before anything reaches its registers. */
    *(volatile uint32_t *)DPORT_WIFI_CLK_EN_REG |= DPORT_WIFI_CLK_WIFI_BT_COMMON;

    /* The calibration buffer's MAC field, which ESP-IDF fills and this did not.
     *
     *     ESP_ERROR_CHECK(esp_efuse_mac_get_default(sta_mac));
     *     memcpy(cal_data->mac, sta_mac, 6);
     *     register_chipv7_phy(init_data, cal_data, calibration_mode);
     *
     * esp_phy_calibration_data_t is version[4] then mac[6] then the opaque
     * remainder, so the six bytes go at offset 4. nat-os was passing a zeroed
     * .bss buffer, which means a MAC of 00:00:00:00:00:00.
     *
     * Its role is to tie stored calibration to the chip that produced it, so
     * with PHY_RF_CAL_FULL -- which recalibrates regardless -- it may well not
     * matter. Filled anyway: it costs six bytes, it is unambiguously what the
     * working stack does, and leaving a known difference in place while hunting
     * an unknown one is how this investigation has previously wasted sessions. */
    {
        uint8_t mac[6];
        efuse_factory_mac(mac);
        for (int i = 0; i < 6; i++) {
            g_phy_cal_data[4 + i] = mac[i];
        }
    }

    g_phy_attempted = 1;
    /* phy_stack_call, NOT rom_call3.
     *
     * rom_call3 runs the callee on the caller's stack. phy_stack_prime() above
     * prepares a private 6 KB stack precisely because the PHY needs one, and
     * calling through rom_call3 meant that stack was primed, measured, and
     * never used -- the PHY ran on whichever task called in. From the 2 KB
     * shell task that faulted inside phy_enter_critical's ENTRY, spilling
     * below a stack pointer that was not a stack. */
    /* Bracket the write. Step 65 bisected the corruption of g_tasks[].sp to this
     * stage; this says whether it happens inside the call itself. Sampling the
     * SAVED sp, not the live one -- no switch can occur here, so the stored
     * value should be untouched across the call. */
    {
        extern uint32_t _phy_stack[], _phy_stack_top[];
        uart_puts("   [phy] &tasks[5].sp ");
        uart_put_hex(task_sp_addr(5));
        uart_puts("   _phy_stack ");
        uart_put_hex((uint32_t)_phy_stack);
        uart_puts("..");
        uart_put_hex((uint32_t)_phy_stack_top);
        uart_puts("\n");
    }

    int      me      = task_current();
    uint32_t sp_pre  = task_saved_sp(me);

    /* PINNED, not merely masked.
     *
     * phy_stack_call raises INTLEVEL to 3 and the blob lowers it again -- not
     * through the adapter (phy_enter/exit_critical run 0/0 times) but by writing
     * PS directly, as IDF's PHY does. Measured at step 69: the tick that caught
     * a task on the private stack was taken with eps3 intlevel 0.
     *
     * So the mask is advisory. The pin is not: it lives in task_schedule(),
     * which decides which task runs, and no amount of PS writing on the blob's
     * side can reach it. The tick still fires; the scheduler simply declines to
     * switch away, so nothing is ever saved with an sp on _phy_stack.
     *
     * blob_lock() also excludes a second context from the shared 6 KB buffer,
     * which the mask never did either. */
    blob_lock();
    g_phy_result = phy_stack_call(fn,
                                  (uint32_t)g_phy_init_data,
                                  (uint32_t)g_phy_cal_data,
                                  PHY_RF_CAL_FULL,
                                  0u);
    blob_unlock();

    {
        uint32_t sp_post = task_saved_sp(me);
        if (sp_post != sp_pre) {
            uart_puts("   [phy] saved sp of task ");
            uart_put_dec((unsigned int)me);
            uart_puts(" changed across the call: ");
            uart_put_hex(sp_pre);
            uart_puts(" -> ");
            uart_put_hex(sp_post);
            uart_puts(", phy used ");
            uart_put_dec(phy_stack_used());
            uart_puts(" of ");
            uart_put_dec(phy_stack_size());
            uart_puts(" B\n");
        } else {
            uart_puts("   [phy] saved sp unchanged across the call\n");
        }
        {
            extern uint32_t g_crit_enters, g_crit_exits;
            extern uint32_t g_crit_lowered, g_crit_from_lvl, g_crit_to_lvl;
            uart_puts("   [phy] crit enter/exit ");
            uart_put_dec(g_crit_enters);
            uart_puts("/");
            uart_put_dec(g_crit_exits);
            if (g_crit_lowered) {
                uart_puts("   LOWERED intlevel ");
                uart_put_dec(g_crit_from_lvl);
                uart_puts(" -> ");
                uart_put_dec(g_crit_to_lvl);
            } else {
                uart_puts("   never lowered the level");
            }
            uart_puts("\n");
        }
    }
    return (int)g_phy_result;
}

/* ---- PHY stack instrumentation -----------------------------------------
 *
 * See window.S for why the PHY runs on a stack of its own. These make its
 * usage measurable, because the interesting failure -- a StoreProhibited
 * inside a window-overflow spill -- gives a fault address that cannot
 * distinguish "ran out of stack" from "something else entirely".
 */
extern uint32_t _phy_stack[], _phy_stack_top[];

#define PHY_STACK_FILL 0xEEEEEEEEu

void phy_stack_prime(void)
{
    for (uint32_t *p = _phy_stack; p < _phy_stack_top; p++) {
        *p = PHY_STACK_FILL;
    }
}

uint32_t phy_stack_size(void)
{
    return (uint32_t)((char *)_phy_stack_top - (char *)_phy_stack);
}

/* Scans from the LOW end: the stack grows down, so the first word still
 * holding the fill pattern marks the deepest point ever reached. */
uint32_t phy_stack_used(void)
{
    uint32_t *p = _phy_stack;
    while (p < _phy_stack_top && *p == PHY_STACK_FILL) {
        p++;
    }
    return (uint32_t)((char *)_phy_stack_top - (char *)p);
}

#if BOARD_HAS_WIFI
/* The original entry point, for a build that links libphy into the kernel. */
int phyinit_run(void)
{
    extern int register_chipv7_phy(const void *, void *, int);
    return phyinit_run_at((uint32_t)&register_chipv7_phy);
}
#endif
