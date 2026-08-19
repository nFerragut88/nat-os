/* nat-os — CPU and bus clock bring-up. See clock.c for why this exists.
 *
 * Short version: the kernel had always inherited an 80 MHz PLL clock from
 * Espressif's bootloader without knowing it, and UM-NATOS-035's replacement
 * loader does not set one. The board ran at 40 MHz and every cycle-derived
 * measurement in the system agreed that it had not.
 */

#ifndef NATOS_CLOCK_H
#define NATOS_CLOCK_H

#include <stdint.h>

/* Brings the SoC to 80 MHz off the PLL, and records the crystal and bus
 * frequencies where the ROM and the PHY blob look for them.
 *
 * Safe to call when a bootloader has already done it -- the PLL case returns
 * immediately -- which is what lets one kernel image boot correctly from either
 * loader. Call as early as possible: everything timed comes after it.
 *
 * Returns 0 on success, -1 if xtal_mhz is not a value this supports (only 40).
 */
int clock_init(uint32_t xtal_mhz);

/* What the core is actually running at, in MHz. */
uint32_t clock_cpu_mhz(void);

/* Non-zero if THIS code performed the switch, rather than finding it done.
 * The difference is worth reporting: it says which bootloader is underneath. */
int clock_pll_switched(void);

/* RTC_CNTL_SOC_CLK_SEL as read from the hardware: 0 = XTAL, 1 = PLL,
 * 2 = RC_FAST, 3 = APLL. The measurement, not the intent. */
uint32_t clock_source(void);

#endif /* NATOS_CLOCK_H */
