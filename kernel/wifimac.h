/* nat-os — WiFi MAC bring-up. See wifimac.c. */
#ifndef NATOS_WIFIMAC_H
#define NATOS_WIFIMAC_H

#include <stdint.h>

/* The one register open-mac's init_mac() touches. */
#define WIFIMAC_BASE      0x3FF73000u
#define WIFIMAC_CTRL_REG  0x3FF73CB8u

/* Ungates the MAC clock and performs open-mac's init_mac(). Returns 0 on
 * success, negative if the preconditions are not met (PHY not up, or already
 * done this boot). Captures before/after values for wifimac_report(). */
int  wifimac_init(void);

int      wifimac_attempted(void);
uint32_t wifimac_ctrl_before(void);
uint32_t wifimac_ctrl_after(void);

/* Reads the MAC register window twice and reports how many words differ.
 *
 * This is the evidence that the peripheral is RUNNING rather than merely
 * readable. A clock-gated block returns a stable value -- usually zero -- from
 * every address; a live MAC has free-running counters in it, and those show up
 * as words that change between two passes with nothing driving them.
 *
 * Returns the number of differing words; `first` receives the address of the
 * lowest one, or 0 if none changed. */
uint32_t wifimac_liveness(uint32_t *first);

/* Which words move and by how much per millisecond. A mover advancing ~1000
 * per ms is a 1 MHz counter -- the 802.11 TSF timer runs at exactly that, so
 * this identifies it from behaviour rather than from a guessed address. */
uint32_t wifimac_movers(uint32_t *addrs, uint32_t *khz, uint32_t max);

/* The 802.11 TSF timer, identified by behaviour: 0x3ff73c00 is the one word in
 * the MAC window that advances at exactly 1 MHz across repeated samples. */
#define WIFIMAC_TSF_REG   0x3FF73C00u

static inline uint32_t wifimac_tsf(void)
{
    return *(volatile uint32_t *)WIFIMAC_TSF_REG;
}

/* Counts TSF ticks against the kernel's own cycle counter over `ms`
 * milliseconds. Returns the TSF delta; `cycles` receives the cycles elapsed.
 * A genuine 1 MHz counter yields delta ~= ms * 1000 -- a long-interval match
 * no drifting or noisy register can produce by accident. */
uint32_t wifimac_tsf_check(uint32_t ms, uint32_t *cycles);

/* Routes the MAC's interrupt onto a CPU line and counts what arrives. Safe to
 * call before receive exists: an idle MAC simply never raises anything. */
void     wifimac_irq_enable(void);
uint32_t wifimac_irq_fires(void);
uint32_t wifimac_irq_status(void);

#endif /* NATOS_WIFIMAC_H */
