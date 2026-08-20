/* nat-os — PHY bring-up. See phyinit.c. */
#ifndef NATOS_PHYINIT_H
#define NATOS_PHYINIT_H
#include <stdint.h>
int      phyinit_run(void);        /* returns register_chipv7_phy's result */

/* Same bring-up, against a register_chipv7_phy at a RUNTIME address, which is
 * how the loaded blob's own copy gets initialised. The blob carries its own
 * libphy with its own .bss, so the kernel must calibrate that copy rather than
 * one it linked itself. See phyinit.c and next_moves/08. */
int      phyinit_run_at(uint32_t fn);
uint32_t phyinit_result(void);
int      phyinit_attempted(void);
#endif
