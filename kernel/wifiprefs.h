/* nat-os — what the user chose about the radio, kept across reboots.
 *
 * Two settings, and both exist because the alternative is asking the user the
 * same question on every boot:
 *
 *   the preferred network   which SSID to associate with automatically. Until
 *                           now the bring-up joined the one compiled into
 *                           wifi_secrets.h, which is right for the board this
 *                           was developed on and wrong for anybody else's.
 *
 *   enabled                 whether to bring the radio up at all.
 *
 * WHAT "DISABLED" ACTUALLY SAVES, stated plainly because the honest answer is
 * narrower than the feature sounds. The blob entry table has esp_wifi_init,
 * _start, _connect and _disconnect, and **no _stop and no _deinit**. So the
 * radio can be told to leave a network; it cannot be told to give its memory
 * back.
 *
 *   disabled at boot   the driver is never initialised, so it never allocates.
 *                      That is the whole saving and it is real: the heap goes
 *                      from ~30 KB to the full ~38 KB, and BLOB_DRAM's 32 KB
 *                      window is never populated.
 *
 *   disabled while up  the station disconnects and nothing auto-joins again,
 *                      but the driver's allocations are not returned until the
 *                      next boot. The setting is honoured immediately; the
 *                      memory comes back when the board restarts.
 *
 * Its own sector, for the reason store.c taught twice (285b, 315a): extending a
 * versioned record discards every existing one, and the credentials next door
 * are passphrases somebody typed on a multi-tap keyboard.
 */

#ifndef NATOS_WIFIPREFS_H
#define NATOS_WIFIPREFS_H

#include <stdint.h>

/* Read at boot, before the radio exists -- step 291's rule: this drives the
 * flash bus, so it must not be faulted in from a tap. */
void wifiprefs_prime(void);

/* Is the radio wanted? Defaults to ENABLED on a board that has never been
 * told: a wifi view whose radio is off by default would be a wifi view that
 * appears broken to anyone who has not read this header. */
int  wifiprefs_enabled(void);
void wifiprefs_set_enabled(int on);

/* The network to associate with automatically, or "" if none has been chosen.
 * Set when a join succeeds, so choosing a network IS the act of preferring it
 * -- there is no second confirmation to forget to give. */
const char *wifiprefs_network(void);
void        wifiprefs_set_network(const char *ssid);

/* Has the user ever chosen a network -- joined one, or forgotten one -- through
 * the view?
 *
 * [step 347] This decides whether the compiled-in credentials in
 * wifi_secrets.h still apply. They are a FACTORY DEFAULT: right for a board
 * nobody has configured, and wrong the moment somebody has. Without this, a
 * forgotten network was rejoined on the next boot from the binary, so "forget"
 * meant "until you reboot" -- which is not what the word means. */
int  wifiprefs_chosen(void);
void wifiprefs_set_chosen(void);

#endif /* NATOS_WIFIPREFS_H */
