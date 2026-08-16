/* nat-os — eFuse block 0 reads. See efuse.c. */
#ifndef NATOS_EFUSE_H
#define NATOS_EFUSE_H

#include <stdint.h>

/* Reads the factory-programmed MAC address into mac[6], most significant byte
 * first (the order it is written and transmitted in). */
void efuse_factory_mac(uint8_t mac[6]);

/* Non-zero if the address matches the CRC8 burned beside it in eFuse.
 *
 * This is what makes the read self-verifying, and it beats checking the OUI
 * against a list of Espressif prefixes: the CRC lives on the chip, so a wrong
 * byte order cannot pass, and no assumption about who owns the OUI is
 * involved. The OUI check was tried first and produced a false failure on this
 * board -- see the note in efuse.c. */
int     efuse_mac_valid(const uint8_t mac[6]);
uint8_t efuse_mac_crc_stored(void);

#endif /* NATOS_EFUSE_H */
