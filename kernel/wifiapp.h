/* nat-os — the wifi view.
 *
 * A launcher app for the radio: what is on the air, what we are joined to, and
 * one tap to join. It replaces "squares", which was a VM program; this cannot
 * be one, because reaching the radio means calling the vendor blob and a
 * bytecode application has no path to it by design.
 *
 * Scope, decided before it was written: the passphrase is the one compiled into
 * kernel/wifi_secrets.h. Tapping a network joins it with that password, which
 * is right for the network this board lives on and honest about the rest — a
 * wrong password fails the four-way handshake and says so. Typing a passphrase
 * needs the multi-tap keyboard and is a separate job.
 *
 * The cost that shapes the interface: the PMK is PBKDF2 at 4096 iterations and
 * measures ~15 s on this part (next_moves/08 step 243, which moved it OFF the
 * driver's connect path for exactly this reason). Joining a network the board
 * has not already derived a key for therefore blocks for that long, and the
 * view says so rather than appearing to hang.
 */

#ifndef NATOS_WIFIAPP_H
#define NATOS_WIFIAPP_H

#include <stdint.h>

/* One scan result, in the only fields a person picking a network needs. The
 * record the blob returns is 84 bytes of which this is the useful part; the
 * offsets it is read from were measured out of the blob at step 212, not taken
 * from a header. */
typedef struct {
    char         ssid[33];
    signed char  rssi;      /* dBm, negative */
    unsigned char ch;
    unsigned char auth;     /* 0 = open, else some WPA flavour */
} wifi_ap_t;

/* Scan ONE channel and return how many results were written. One channel per
 * call so the caller can paint progress across a sweep rather than freeze for
 * five seconds. */
uint32_t wifi_scan_channel(uint32_t scan_fn, uint32_t num_fn, uint32_t recs_fn,
                           uint32_t ch, wifi_ap_t *out, uint32_t max);

/* Runs a requested radio bring-up. Called from the NET task every pass, not
 * from the touch task -- the bring-up blocks for ~90 s and the task that reads
 * the glass must stay answerable for all of it. */
void wifiapp_service(void);

void wifiapp_open(void);                              /* entering the app */
void wifiapp_frame(void);                             /* per-frame redraw */
void wifiapp_touch(uint32_t x, uint32_t y, int down); /* routed by kmain  */

#endif /* NATOS_WIFIAPP_H */
