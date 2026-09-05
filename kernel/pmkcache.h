/* nat-os — cached pairwise master keys.
 *
 * The PMK is PBKDF2-SHA1(passphrase, ssid, 4096) and measures about fifteen
 * seconds on this part. It is also a PURE FUNCTION of the passphrase and the
 * SSID: the same two inputs give the same 32 bytes every time, on every boot,
 * forever. Deriving it again is not caution, it is arithmetic repeated.
 *
 * WHY ITS OWN SECTOR rather than a field in wificred: store.c's lesson, applied
 * a second time. Extending a versioned record discards every existing one, and
 * the credentials in wificred are passphrases a person typed on a multi-tap
 * keyboard. Costing someone that to save fifteen seconds is a bad trade; a
 * second sector costs 4 KB of a 1.8 MB gap.
 *
 * The two records are also independent in the way that matters: a credential is
 * authoritative and a cached PMK is disposable. If this sector is lost,
 * corrupt, or from an older version, the answer is to derive again — fifteen
 * seconds, not a lost password. Nothing here is ever the only copy of anything.
 *
 * SECRECY: a PMK is as good as the passphrase for joining the network it
 * belongs to. This is plaintext flash, exactly as wificred is, and it is no
 * more exposed than the passphrase already sitting one sector away.
 */

#ifndef NATOS_PMKCACHE_H
#define NATOS_PMKCACHE_H

#include <stdint.h>

#define PMK_LEN     32u
#define PMK_SLOTS    8u

/* Read at boot, before the radio exists — step 291's rule: this drives the
 * flash bus, so it must not be faulted in from a tap. */
void pmkcache_prime(void);

/* Copy the cached PMK for `ssid` into `out` (32 bytes). 1 if found. */
int  pmkcache_get(const char *ssid, unsigned char *out);

/* Store the PMK for `ssid`. Returns 0 on success. Erases and rewrites the
 * sector, so this is for a derivation that just happened, not a hot path. */
int  pmkcache_put(const char *ssid, const unsigned char *pmk);

/* Forget one, for when its passphrase changes. */
void pmkcache_forget(const char *ssid);

#endif /* NATOS_PMKCACHE_H */
