/* nat-os — WiFi MAC bring-up. See wifimac.h and vendor/phy/MAC-NEXT.md.
 *
 * open-mac's entire MAC initialisation is one line:
 *
 *     MAC_CTRL_REG = MAC_CTRL_REG & 0xffffe800;
 *
 * That is not an oversimplification -- it really is the whole thing, and it is
 * why finishing the environment underneath it was worth the effort. The 0x17ff
 * being cleared is undocumented; Espressif publishes nothing about this
 * register, and the mask comes from esp32-open-mac's reverse engineering.
 *
 * ---- why this file is mostly measurement -------------------------------
 *
 * The write is trivial and a readback proving the bits cleared would prove
 * almost nothing. This kernel has now been caught three separate times by a
 * peripheral whose registers read back exactly as written while the hardware
 * did nothing at all -- LEDC clock-gated behind DPORT_PERIP_CLK_EN bit 11
 * (UM-NATOS-027 §3.3) being the clearest. A successful store is not evidence.
 *
 * So the real content here is wifimac_liveness(): read the register window,
 * wait, read it again, and report which words moved on their own. A gated
 * block is stable; a running MAC has free-running counters in it. That
 * distinction cannot be faked by a write that went nowhere.
 *
 * It also avoids guessing WHICH register is the counter. Espressif's MAC
 * register map is not public and the addresses circulating for it are
 * reverse-engineered. Rather than assert that some specific offset is the TSF
 * timer, this asks the hardware which addresses change and reports them.
 */

#include "wifimac.h"
#include "phyinit.h"
#include "xtensa.h"
#include "task.h"

/* Clock gating for the MAC, which is separate from the WiFi/BT common clock
 * phyinit already ungated (0x3C9).
 *
 * 0x406 is DPORT_WIFI_CLK_WIFI_EN from Espressif's dport_reg.h. Setting it
 * alongside the common bits is what ESP-IDF's periph_module_enable() does for
 * PERIPH_WIFI_MODULE. Nothing else in this kernel uses DPORT_WIFI_CLK_EN_REG,
 * so this is additive and cannot disturb the display, SD or touch paths --
 * they are on PERIP_CLK_EN, a different register entirely. */
#define DPORT_WIFI_CLK_EN_REG   0x3FF000CCu
#define DPORT_WIFI_CLK_WIFI_EN  0x00000406u

#define MAC_INIT_MASK           0xFFFFE800u

/* 256 words at a time rather than one 4 KB snapshot: the window is 4 KB and a
 * buffer that size in .bss is real DRAM permanently spent on a diagnostic. */
/* ~80 MHz, per kmain.c TICK_INTERVAL_CYCLES. Used only to turn measured
 * cycles into a frequency, so an error here scales every rate uniformly and
 * would not change which registers are counters. */
#define CPU_KHZ     80000u

#define SCAN_WORDS  256u
#define SCAN_CHUNKS 4u

static uint32_t g_snap[SCAN_WORDS];

static int      g_attempted;
static uint32_t g_before, g_after;

int      wifimac_attempted(void)   { return g_attempted; }
uint32_t wifimac_ctrl_before(void) { return g_before; }
uint32_t wifimac_ctrl_after(void)  { return g_after; }

/* Busy-waits on the cycle counter rather than sleeping.
 *
 * task_sleep would yield, and a counter that advances across a scheduling
 * gap proves less than one that advances across a known, short, uninterrupted
 * span -- the point is to observe hardware moving while software does not.
 *
 * The caller measures the ACTUAL elapsed cycles rather than trusting this to
 * deliver them; at ~80 MHz, 240,000 cycles is 3 ms, not the 1 ms an earlier
 * comment here claimed. */
static void spin_cycles(uint32_t cycles)
{
    uint32_t start = xt_ccount();
    while ((xt_ccount() - start) < cycles) {
        /* nothing */
    }
}

int wifimac_init(void)
{
    /* The PHY must be up first. The MAC without a calibrated radio underneath
     * it is not a meaningful thing to initialise, and the failure would appear
     * later and elsewhere. */
    if (!phyinit_attempted() || phyinit_result() != 0) {
        return -1;
    }
    if (g_attempted) {
        return -2;
    }

    *(volatile uint32_t *)DPORT_WIFI_CLK_EN_REG |= DPORT_WIFI_CLK_WIFI_EN;

    volatile uint32_t *ctrl = (volatile uint32_t *)WIFIMAC_CTRL_REG;
    g_before = *ctrl;
    *ctrl = g_before & MAC_INIT_MASK;
    g_after = *ctrl;

    g_attempted = 1;
    return 0;
}

/* Which words move, and at what frequency.
 *
 * The count alone says the MAC is alive; the RATE says what each mover is. A
 * counter running at 1 MHz is the 802.11 TSF timer -- and the TSF is the one
 * register in this whole region whose identity can be established from
 * behaviour rather than from a reverse-engineered address list.
 *
 * The window is MEASURED, not assumed. The first version of this asserted
 * "240000 cycles is 1 ms", which is true at 240 MHz and false here: nat-os
 * runs at ~80 MHz (kmain.c TICK_INTERVAL_CYCLES), so every rate it printed was
 * three times too high. Reading the cycle counter at both ends fixes that AND
 * removes a second error -- the busy-wait is preemptible, so a tick landing
 * inside it stretches the window and deflates the apparent rate. Dividing by
 * elapsed cycles rather than intended ones makes both problems disappear.
 *
 * Returns how many movers were found; fills up to `max` entries with the
 * address and the rate in kHz. */
uint32_t wifimac_movers(uint32_t *addrs, uint32_t *khz, uint32_t max)
{
    uint32_t found = 0;

    for (uint32_t chunk = 0; chunk < SCAN_CHUNKS; chunk++) {
        volatile uint32_t *base =
            (volatile uint32_t *)(WIFIMAC_BASE + chunk * SCAN_WORDS * 4u);

        uint32_t t0 = xt_ccount();
        for (uint32_t i = 0; i < SCAN_WORDS; i++) {
            g_snap[i] = base[i];
        }
        spin_cycles(240000u);
        uint32_t elapsed = xt_ccount() - t0;
        if (!elapsed) {
            elapsed = 1;
        }

        for (uint32_t i = 0; i < SCAN_WORDS; i++) {
            uint32_t now = base[i];
            if (now != g_snap[i]) {
                if (found < max) {
                    /* Magnitude only: a decrementing counter reports its rate,
                     * and the sign is not what identifies it. */
                    uint32_t d = now - g_snap[i];
                    if (d > 0x80000000u) {
                        d = (uint32_t)(0u - d);
                    }

                    /* d * CPU_KHZ / elapsed, with both sides scaled by 1000 so
                     * it stays in 32 bits. The obvious 64-bit form needs
                     * __udivdi3, which a freestanding call0 build has no
                     * libgcc to supply -- it linked as a call to nothing.
                     *
                     * Dividing elapsed by 1000 first costs under 0.4% at the
                     * ~240k cycles this samples over, which is far inside the
                     * accuracy needed to tell 1 MHz from 133 kHz. */
                    uint32_t denom = elapsed / 1000u;
                    addrs[found] = (uint32_t)&base[i];
                    khz[found] = denom ? (d * (CPU_KHZ / 1000u)) / denom : 0u;
                }
                found++;
            }
        }
    }
    return found;
}

uint32_t wifimac_liveness(uint32_t *first)
{
    uint32_t changed = 0, first_addr = 0;

    for (uint32_t chunk = 0; chunk < SCAN_CHUNKS; chunk++) {
        volatile uint32_t *base =
            (volatile uint32_t *)(WIFIMAC_BASE + chunk * SCAN_WORDS * 4u);

        for (uint32_t i = 0; i < SCAN_WORDS; i++) {
            g_snap[i] = base[i];
        }

        spin_cycles(240000u);           /* ~3 ms at ~80 MHz */

        for (uint32_t i = 0; i < SCAN_WORDS; i++) {
            if (base[i] != g_snap[i]) {
                changed++;
                if (!first_addr) {
                    first_addr = (uint32_t)&base[i];
                }
            }
        }
    }

    if (first) {
        *first = first_addr;
    }
    return changed;
}

uint32_t wifimac_tsf_check(uint32_t ms, uint32_t *cycles)
{
    /* Busy-waits rather than sleeping, and that is a workaround.
     *
     * This first used task_sleep(ms/10), which returned immediately: both
     * clocks agreed that ~1 us had passed for a requested 500 ms. Two
     * independent measurements agreeing on "no time elapsed" is not a
     * measurement error, so task_sleep from shell-task context is not
     * sleeping -- see the note on the 'sleeptest' command. That is a kernel
     * bug, tracked separately; this function must not depend on it.
     *
     * Spinning on CCOUNT still gives what the check needs: a long interval
     * timed by a clock with no connection to the MAC's. */
    uint32_t c0 = xt_ccount();
    uint32_t t0 = wifimac_tsf();
    spin_cycles(ms * (CPU_KHZ / 1000u) * 1000u);
    uint32_t t1 = wifimac_tsf();
    uint32_t c1 = xt_ccount();

    if (cycles) {
        *cycles = c1 - c0;
    }
    return t1 - t0;
}
