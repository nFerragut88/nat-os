/* nat-os -- the WPA2 four-way handshake. next_moves/08 step 241.
 *
 * WINDOWED, and it has to be: the driver calls wpa_sta_rx_eapol through the
 * wpa_cb table with callx8, and this calls back into the blob --
 * esp_wifi_set_sta_key_internal takes NINE arguments where the call0 bridges
 * carry four. Windowed all the way through means no bridge anywhere on the
 * path, which is why the crypto moved to the windowed ABI at step 240.
 *
 * ---- what a four-way handshake is ---------------------------------------
 *
 * Both sides already share the PMK -- derived from the passphrase and the SSID
 * by PBKDF2, which is why an attacker who knows the SSID can precompute. The
 * handshake proves each side HAS it without either sending it, and derives a
 * fresh session key:
 *
 *   1  AP  -> STA   ANonce                        (no MIC: nothing shared yet)
 *   2  STA -> AP    SNonce + RSN IE + MIC         (the MIC proves we have PMK)
 *   3  AP  -> STA   GTK (wrapped) + MIC           (its MIC proves the AP does)
 *   4  STA -> AP    MIC                           (acknowledgement)
 *
 * The PTK is PRF-384(PMK, "Pairwise key expansion", MACs and nonces in sorted
 * order) and splits into KCK (MICs), KEK (unwrapping the GTK) and TK (the
 * actual traffic key handed to the hardware).
 *
 * Sorted order matters and is easy to get wrong: min(AA,SA) || max(AA,SA) ||
 * min(ANonce,SNonce) || max(ANonce,SNonce). Both ends must sort identically or
 * the PTKs differ and every MIC fails -- which looks exactly like a wrong
 * password.
 *
 * ---- scope --------------------------------------------------------------
 *
 * WPA2-PSK with CCMP, key descriptor version 2 (HMAC-SHA1 MIC, AES key wrap).
 * No TKIP, no WPA1, no WPA3/SAE, no PMKSA caching, no roaming, no rekeying
 * beyond the initial GTK. Those are what ESP-IDF's 3,017-line wpa.c handles
 * and what this deliberately does not.
 */

#include "includes.h"
#include "sha1.h"
#include "aes_wrap.h"

/* ---- blob entries, addresses only --------------------------------------- */

uint32_t g_hs_tx_fn, g_hs_setkey_fn, g_hs_ptkdone_fn, g_hs_authdone_fn;

typedef int (*tx_fn_t)(int, void *, unsigned short);
typedef int (*setkey_fn_t)(int, u8 *, int, int, u8 *, unsigned int,
                           u8 *, unsigned int, int);
typedef int (*ptkdone_fn_t)(u8 *);
typedef int (*authdone_fn_t)(void);

/* ---- state -------------------------------------------------------------- */

static u8  g_pmk[32];
static u8  g_ptk[48];            /* KCK[16] KEK[16] TK[16] */
static u8  g_snonce[32];
static u8  g_aa[6];              /* the access point */
static u8  g_sa[6];              /* us */
static u8  g_replay[8];

uint32_t g_hs_have_pmk, g_hs_state, g_hs_msg1, g_hs_msg3, g_hs_done;
uint32_t g_hs_mic_bad, g_hs_unwrap_bad, g_hs_tx_err, g_hs_last_keyinfo;

#define KCK (g_ptk + 0)
#define KEK (g_ptk + 16)
#define TK  (g_ptk + 32)

/* EAPOL-Key layout, from the 802.1X header onward. */
#define O_VER      0
#define O_TYPE     1
#define O_LEN      2
#define O_DESC     4
#define O_KEYINFO  5
#define O_KEYLEN   7
#define O_REPLAY   9
#define O_NONCE    17
#define O_IV       49
#define O_RSC      65
#define O_ID       73
#define O_MIC      81
#define O_KDLEN    97
#define O_KD       99

#define KI_PAIRWISE 0x0008u
#define KI_INSTALL  0x0040u
#define KI_ACK      0x0080u
#define KI_MIC      0x0100u
#define KI_SECURE   0x0200u
#define KI_ENCRYPT  0x1000u

static u8 g_out[256];

static uint32_t be16(const u8 *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static void put16(u8 *p, uint32_t v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

/* The RSN IE we advertise; must match what went into the association request
 * or the AP rejects msg2 with "IE differs" (reason 17). */
static u8 g_rsn[22] __attribute__((aligned(4))) = {
    0x30,0x14, 0x01,0x00, 0x00,0x0F,0xAC,0x04,
    0x01,0x00, 0x00,0x0F,0xAC,0x04, 0x01,0x00, 0x00,0x0F,0xAC,0x02, 0x00,0x00
};

void wpa_hs_set_pmk(const u8 *pmk);
void wpa_hs_set_pmk(const u8 *pmk)
{
    for (uint32_t i = 0u; i < 32u; i++) { g_pmk[i] = pmk[i]; }
    g_hs_have_pmk = 1u;
    g_hs_state = 0u;
}

void wpa_hs_set_addrs(const u8 *ap, const u8 *own);
void wpa_hs_set_addrs(const u8 *ap, const u8 *own)
{
    for (uint32_t i = 0u; i < 6u; i++) { g_aa[i] = ap[i]; g_sa[i] = own[i]; }
}

/* PTK = PRF-384(PMK, "Pairwise key expansion", min||max||min||max). */
static void derive_ptk(const u8 *anonce)
{
    u8 data[76];
    const u8 *lo_m, *hi_m, *lo_n, *hi_n;

    lo_m = (os_memcmp(g_aa, g_sa, 6) < 0) ? g_aa : g_sa;
    hi_m = (os_memcmp(g_aa, g_sa, 6) < 0) ? g_sa : g_aa;
    lo_n = (os_memcmp(anonce, g_snonce, 32) < 0) ? anonce : g_snonce;
    hi_n = (os_memcmp(anonce, g_snonce, 32) < 0) ? g_snonce : anonce;

    os_memcpy(&data[0],  lo_m, 6);
    os_memcpy(&data[6],  hi_m, 6);
    os_memcpy(&data[12], lo_n, 32);
    os_memcpy(&data[44], hi_n, 32);

    (void)sha1_prf(g_pmk, 32, "Pairwise key expansion", data, sizeof data, g_ptk, 48);
}

/* Build and send an EAPOL-Key reply. `kd`/`kdlen` is the key data (msg2 carries
 * our RSN IE; msg4 carries none). The MIC covers the whole frame with the MIC
 * field zeroed, which is why it is computed last. */
static void send_eapol(uint32_t key_info, const u8 *nonce, const u8 *kd, uint32_t kdlen)
{
    uint32_t body = 95u + kdlen;              /* after the 4-byte 802.1X header */
    uint32_t total = 4u + body;
    u8 *e = g_out + 14;                       /* leave room for the Ethernet hdr */

    for (uint32_t i = 0u; i < 14u + total; i++) { g_out[i] = 0u; }

    /* Ethernet II: to the AP, from us, EAPOL. */
    os_memcpy(&g_out[0], g_aa, 6);
    os_memcpy(&g_out[6], g_sa, 6);
    put16(&g_out[12], 0x888Eu);

    e[O_VER]  = 2u;
    e[O_TYPE] = 3u;                           /* EAPOL-Key */
    put16(&e[O_LEN], body);
    e[O_DESC] = 2u;                           /* RSN descriptor */
    put16(&e[O_KEYINFO], key_info);
    put16(&e[O_KEYLEN], 16u);                 /* CCMP pairwise key length */
    os_memcpy(&e[O_REPLAY], g_replay, 8);
    if (nonce) { os_memcpy(&e[O_NONCE], nonce, 32); }
    put16(&e[O_KDLEN], kdlen);
    if (kd && kdlen) { os_memcpy(&e[O_KD], kd, kdlen); }

    u8 mic[20];
    (void)hmac_sha1(KCK, 16, e, total, mic);
    os_memcpy(&e[O_MIC], mic, 16);

    if (g_hs_tx_fn) {
        if (((tx_fn_t)g_hs_tx_fn)(0, g_out, (unsigned short)(14u + total)) != 0) {
            g_hs_tx_err++;
        }
    }
}

/* Pull the GTK out of msg3's key data, which arrives AES-key-wrapped under the
 * KEK and holds a sequence of KDEs. The GTK KDE is 00-0F-AC type 1. */
static int extract_gtk(const u8 *kd, uint32_t kdlen, u8 *gtk, uint32_t *gtk_len, uint32_t *keyid)
{
    static u8 plain[128];
    if (kdlen < 16u || (kdlen % 8u) != 0u || kdlen - 8u > sizeof plain) { return -1; }
    if (aes_unwrap(KEK, 16, (int)((kdlen - 8u) / 8u), kd, plain) != 0) { return -1; }

    uint32_t n = kdlen - 8u, i = 0u;
    while (i + 6u <= n) {
        uint32_t t = plain[i], l = plain[i + 1u];
        if (t != 0xDDu || l < 4u || i + 2u + l > n) { break; }
        if (plain[i+2] == 0x00u && plain[i+3] == 0x0Fu && plain[i+4] == 0xACu
            && plain[i+5] == 0x01u && l >= 6u) {
            *keyid = plain[i + 6u] & 0x03u;
            *gtk_len = l - 6u;
            if (*gtk_len > 32u) { *gtk_len = 32u; }
            os_memcpy(gtk, &plain[i + 8u], *gtk_len);
            return 0;
        }
        i += 2u + l;
    }
    return -1;
}

/* ---- the entry the driver calls ----------------------------------------- */

int wpa_sta_rx_eapol_impl(u8 *src, u8 *buf, u32 len);
/* [step 256] PASSIVE MODE. Count the frame and touch nothing else.
 *
 * Step 255 found a0 = sta_rx_eapol + 0x16c in two different panics, which
 * says the driver was delivering EAPOL when they happened -- and an access
 * point sends message one only to a station that has ASSOCIATED. If that is
 * real, those arms associate and the crash is in this file, which has never
 * executed once in its life.
 *
 * Passive separates the two: no PTK derivation, no MIC, no transmit, no key
 * install. If the panic goes with it, the handshake is the fault and the
 * association is fine. If the panic stays, a0 was lying and step 255 said so
 * in advance. */
uint32_t g_hs_passive;

int wpa_sta_rx_eapol_impl(u8 *src, u8 *buf, u32 len)
{
    if (g_hs_passive) {
        g_hs_msg1++;                 /* it ARRIVED, and that is the result */
        g_hs_last_keyinfo = 0xEEEEu;   /* marker: passive, not decoded */
        return 0;
    }
    if (!g_hs_have_pmk || !buf || len < O_KD) { return 0; }
    if (buf[O_TYPE] != 3u) { return 0; }              /* not EAPOL-Key */

    uint32_t ki = be16(&buf[O_KEYINFO]);
    uint32_t kdlen = be16(&buf[O_KDLEN]);
    g_hs_last_keyinfo = ki;
    if (src) { os_memcpy(g_aa, src, 6); }
    os_memcpy(g_replay, &buf[O_REPLAY], 8);

    if (!(ki & KI_PAIRWISE)) { return 0; }             /* group rekey: not handled */

    if (!(ki & KI_MIC)) {
        /* ---- message 1: ANonce, no MIC ---- */
        g_hs_msg1++;
        for (uint32_t i = 0u; i < 32u; i++) {
            g_snonce[i] = (u8)(lwip_rand_u32() >> ((i & 3u) * 8u));
        }
        derive_ptk(&buf[O_NONCE]);
        g_hs_state = 1u;
        send_eapol(0x0002u | KI_PAIRWISE | KI_MIC, g_snonce, g_rsn, sizeof g_rsn);
        return 0;
    }

    /* ---- anything with a MIC must be verified before it is believed ---- */
    {
        u8 want[20], saved[16];
        uint32_t total = 4u + 95u + kdlen;
        if (total > len) { return 0; }
        os_memcpy(saved, &buf[O_MIC], 16);
        os_memset(&buf[O_MIC], 0, 16);
        (void)hmac_sha1(KCK, 16, buf, total, want);
        os_memcpy(&buf[O_MIC], saved, 16);
        if (os_memcmp(want, saved, 16) != 0) {
            g_hs_mic_bad++;                 /* wrong PMK, or a forged frame */
            return 0;
        }
    }

    /* ---- message 3: install the keys, then acknowledge ---- */
    g_hs_msg3++;

    u8 gtk[32]; uint32_t gtk_len = 0u, keyid = 0u;
    int have_gtk = 0;
    if ((ki & KI_ENCRYPT) && kdlen) {
        have_gtk = (extract_gtk(&buf[O_KD], kdlen, gtk, &gtk_len, &keyid) == 0);
        if (!have_gtk) { g_hs_unwrap_bad++; }
    }

    send_eapol(0x0002u | KI_PAIRWISE | KI_MIC | KI_SECURE, 0, 0, 0u);

    if (g_hs_setkey_fn) {
        /* Explicit, not an initializer -- see the note in wifi_glue.c: an
         * aggregate initializer here becomes a call to call0 memcpy. */
        u8 seq[6];
        seq[0] = 0; seq[1] = 0; seq[2] = 0;
        seq[3] = 0; seq[4] = 0; seq[5] = 0;
        /* pairwise: CCMP(3), for this AP, index 0, tx, PAIRWISE|TX|RX */
        (void)((setkey_fn_t)g_hs_setkey_fn)(3, g_aa, 0, 1, seq, 6, TK, 16,
                                            (1<<5) | (1<<3) | (1<<2));
        if (have_gtk) {
            (void)((setkey_fn_t)g_hs_setkey_fn)(3, 0, (int)keyid, 0, seq, 6,
                                                gtk, gtk_len, (1<<4) | (1<<2));
        }
    }
    if (g_hs_ptkdone_fn)  { (void)((ptkdone_fn_t)g_hs_ptkdone_fn)(g_aa); }
    if (g_hs_authdone_fn) { (void)((authdone_fn_t)g_hs_authdone_fn)(); }

    g_hs_state = 3u;
    g_hs_done++;
    return 0;
}

/* wpa_cb slot 6: true while a handshake is in flight. Answering true when no
 * handshake is running would have the driver wait for one that never starts. */
uint32_t g_hs_in4way_calls;

int wpa_sta_in_4way_impl(void);
int wpa_sta_in_4way_impl(void)
{
    /* [step 242] BOUNDED. Answering "yes, still in a handshake" forever is how
     * the first attempt took the board down: the driver task runs at nat-os
     * priority HIGH, it polls this, and an answer that never changes makes it
     * spin -- starving the shell and the poll loop until TG0WDT_SYS_RESET.
     *
     * Nothing could be reported from that state, which is the worst property a
     * failure can have. A bounded answer turns a hang into a driver-side
     * timeout with a reason code, which is diagnosable.
     *
     * The count is generous: a real handshake completes in a few exchanges. */
    if (g_hs_state != 1u) { return 0; }
    if (++g_hs_in4way_calls > 4000u) { return 0; }
    return 1;
}
