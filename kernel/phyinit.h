/* nat-os — PHY bring-up. See phyinit.c. */
#ifndef NATOS_PHYINIT_H
#define NATOS_PHYINIT_H
#include <stdint.h>
int      phyinit_run(void);        /* returns register_chipv7_phy's result */
uint32_t phyinit_result(void);
int      phyinit_attempted(void);
#endif
