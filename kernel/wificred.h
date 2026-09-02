/* nat-os — saved WiFi credentials.
 *
 * A passphrase the user typed, kept across reboots.
 *
 * WHY NOT store.c: the persistence record is a fixed, versioned, checksummed
 * struct, and store.c REJECTS a record whose version does not match — so adding
 * fields to it discards every existing record, and with it the touch
 * calibration. Making someone recalibrate the screen to gain a saved password
 * is a bad trade, and an avoidable one: flash.h reserves 0x200000 for the
 * record and 0x201000 for the message sector, and the blob does not begin until
 * 0x220000. The 120 KB between them is unused. One sector of it is taken here,
 * with its own magic, version and checksum, so the two records fail and
 * migrate independently.
 *
 * NOT SECURE, and worth saying so rather than implying otherwise. The
 * passphrase is stored in plaintext in flash, readable by anyone who can read
 * the chip. That is the same guarantee as the compiled-in WIFI_STA_PASS it
 * replaces — which lives in the binary in plaintext — so nothing is lost, but
 * nothing is gained either. Encrypting it would need a key, and there is
 * nowhere on this part to keep one that an attacker with the flash cannot also
 * read; eFuse key blocks and flash encryption are the real answer and are a
 * separate piece of work.
 */

#ifndef NATOS_WIFICRED_H
#define NATOS_WIFICRED_H

#include <stdint.h>

#define WIFICRED_SSID_MAX  33u      /* 32 + NUL */
#define WIFICRED_PASS_MAX  64u      /* 63 + NUL, the WPA2 maximum */
#define WIFICRED_SLOTS      8u

/* Copy the saved passphrase for `ssid` into `pass`. Returns 1 if one was
 * found, 0 otherwise; `pass` is left untouched when 0 is returned. */
int wificred_get(const char *ssid, char *pass, uint32_t max);

/* Save (or replace) the passphrase for `ssid`. Returns 0 on success.
 *
 * Erases and rewrites the sector, which masks interrupts for tens of
 * milliseconds — call it on a user action, never from a frame path. */
int wificred_put(const char *ssid, const char *pass);

/* How many credentials are stored. Reads the cache, not the flash. */
uint32_t wificred_count(void);

#endif /* NATOS_WIFICRED_H */
