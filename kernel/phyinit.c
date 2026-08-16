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
#include "uart.h"

#define DPORT_WIFI_CLK_EN_REG        0x3FF000CCu
#define DPORT_WIFI_CLK_WIFI_BT_COMMON 0x000003C9u

#define PHY_RF_CAL_FULL 2u

/* Espressif's default PHY initialisation parameters. */
static const uint8_t g_phy_init_data[128] = {
    3, 3, 0x05, 0x09, 0x06, 0x05, 0x03, 0x06, 0x05, 0x04, 0x06, 0x04,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x05, 0x09, 0x06, 0x05, 0x03, 0x06, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0xfc, 0xfe, 0xf0,
    0xf0, 0xf0, 0xe0, 0xe0, 0xe0, 0x18, 0x18, 0x18, 40, 40, 40, 40,
    40, 40, 0, 1, 1, 2, 2, 3, 4, 5, 0, 0,
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

int phyinit_run(void)
{
    extern int register_chipv7_phy(const void *, void *, int);

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

    g_phy_attempted = 1;
    g_phy_result = rom_call3((uint32_t)&register_chipv7_phy,
                             (uint32_t)g_phy_init_data,
                             (uint32_t)g_phy_cal_data,
                             PHY_RF_CAL_FULL);
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
