/* nat-os - the rest of what libpp.a wants from its host. Compiled windowed.
 *
 * The OSI table (wifi_osi.c) is the big contract. This file is the remainder:
 * globals the blob expects the host to own, callbacks that belong to the 802.11
 * layer open-mac replaces, and a handful of stubs.
 *
 * SIZES ARE MEASURED, NOT GUESSED. g_ic and g_wifi_menuconfig are COMMON
 * symbols in libnet80211.a, so their sizes are recorded in the archive:
 *
 *     nm -S libnet80211.a  ->  0000027c C g_ic
 *                             00000060 C g_wifi_menuconfig
 *
 * 636 and 96 bytes. Defining them any smaller would let the blob write past the
 * end of a host allocation, which is the kind of fault that surfaces somewhere
 * else entirely, hours later. This project has already spent a day on one of
 * those (UM-NATOS-015 §5.9).
 */

#include <stdint.h>
#include <stddef.h>

/* ---- globals the blob expects the host to define ------------------------ */

/* Interface control block. 636 bytes per libnet80211.a. uint32_t array rather
 * than uint8_t so the alignment matches what the blob will assume. */
uint32_t g_ic[636 / 4];

uint32_t g_wifi_menuconfig[96 / 4];

void    *g_wifi_global_lock;
void    *g_intr_lock_mux;
uint32_t g_wifi_mac_time_delta;

/* ---- mesh: not built here ----------------------------------------------- */

uint32_t g_mesh_is_root;
uint32_t g_mesh_is_started;
uint32_t g_mesh_init_ps_type;
void    *esp_mesh_quick_funcs;

int esp_mesh_get_running_active_duty_cycle(void)
{
    return 0;
}

/* ---- the 802.11 layer open-mac replaces --------------------------------- */
/*
 * libpp calls up into net80211 for these. open-mac's whole purpose is to
 * provide that layer, so these are the seam between the two -- stubs here, and
 * the place open-mac's implementation would attach.
 */
int  esp_wifi_get_mode(void *mode)             { (void)mode; return 0; }
void ieee80211_hostapd_beacon_txcb(void *a, void *b) { (void)a; (void)b; }
void sta_reset_beacon_timeout(void *a)         { (void)a; }
int  wifi_sta_rx_probe_req(void *a, void *b)   { (void)a; (void)b; return 0; }
int  wl_is_ap_no_lr(void)                      { return 1; }
int  wifi_set_rx_policy(int policy)            { (void)policy; return 0; }
int  wifi_nvs_get_sta_listen_interval(void *v) { (void)v; return 0; }
void wifi_rf_phy_enable(void)                  { }
void wifi_rf_phy_disable(void)                 { }
void wifi_log(int level, const char *fmt, ...) { (void)level; (void)fmt; }

/* ---- odds and ends ------------------------------------------------------ */

void abort(void)
{
    /* The blob calls this when it has decided it cannot continue. Spinning is
     * deliberate: the hang detector notices a task that stops switching and
     * resets the board, which is a louder and more debuggable outcome than
     * returning to a caller that believes abort() does not return. */
    for (;;) {
    }
}

void *phy_memset(void *d, int c, unsigned int n)
{
    unsigned char *p = d;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return d;
}

int phy_memcmp(const void *a, const void *b, unsigned int n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++; y++;
    }
    return 0;
}

/* ---- soft-float helpers libpp needs that libphy did not ----------------- */
/*
 * Same approach as phy_host.c: ROM routines reached through function pointers,
 * so no linker script defines anything and nothing the kernel owns can be
 * displaced. That distinction cost a boot panic once already.
 */
#define ROM_EXTENDSFDF2  0x40002C34u
#define ROM_TRUNCDFSF2   0x40002B90u
#define ROM_UDIVDI3      0x4000CFB0u
#define ROM_UMODDI3      0x4000D0DCu

typedef unsigned long long (*fn_ull2)(unsigned long long, unsigned long long);
typedef double             (*fn_ext)(float);
typedef float              (*fn_trunc)(double);

unsigned long long phy_udivdi3(unsigned long long a, unsigned long long b)
{
    return ((fn_ull2)ROM_UDIVDI3)(a, b);
}

unsigned long long phy_umoddi3(unsigned long long a, unsigned long long b)
{
    return ((fn_ull2)ROM_UMODDI3)(a, b);
}

double phy_extendsfdf2(float a) { return ((fn_ext)ROM_EXTENDSFDF2)(a); }

/* Not in ROM. Counting bits is cheaper to write than to find. */
int phy_popcountsi2(unsigned int x)
{
    int n = 0;
    while (x) { x &= x - 1u; n++; }
    return n;
}

/* Straight to ROM, and it has to be.
 *
 * The obvious body -- return (float)(double)x -- makes the compiler emit calls
 * to __floatundidf and __truncdfsf2, which are two of the helpers this file
 * exists to supply. It compiled, then failed to link against itself. */
#define ROM_FLOATUNDISF  0x4000C8B0u
#define ROM_FLOATUNDIDF  0x4000C978u

typedef float  (*fn_ull2f)(unsigned long long);
typedef double (*fn_ull2d)(unsigned long long);

float phy_floatundisf(unsigned long long x)
{
    return ((fn_ull2f)ROM_FLOATUNDISF)(x);
}

double phy_floatundidf(unsigned long long x)
{
    return ((fn_ull2d)ROM_FLOATUNDIDF)(x);
}


/* ---- WPA supplicant callback table -- next_moves/08 step 205 ----
 * Folded into this file rather than its own: adding vendor/windowed/wpa_cb.c
 * as a NEW translation unit panicked inside esp_wifi_init_internal at a
 * garbage PC, identically whether the kernel-side block was fifteen lines or
 * four, so it was the unit's presence and not its caller's size. The stub
 * itself disassembles clean -- entry/l32r/retw, no libgcc window-spill helper
 * -- so the code was never the problem. See UM-NATOS-042 section 9.2.
 *
 * ESP-IDF's esp_wifi_init() WRAPPER calls esp_supplicant_init(), which calls
 * esp_wifi_register_wpa_cb_internal(). That wrapper is open-source IDF code
 * and is NOT in the blob -- the symbol is an export with no caller anywhere
 * in 180k instructions. nat-os calls esp_wifi_init_internal() directly, so
 * g_ic->wpa_cb (+0x1b4) has been NULL since the driver first initialised.
 *
 * A NULL table faults in cannel_scan_connect_state, which checks the
 * FUNCTION and not the table. An all-zero table faults in
 * wifi_station_start, which checks the TABLE and not the function. Neither
 * 'leave it NULL' nor 'hand it zeros' is right, so every slot points at one
 * stub that records where it was called FROM -- the driver names the entries
 * it needs instead of a static scan guessing them. */
uint32_t g_wpa_calls;      /* total calls through the table */
uint32_t g_wpa_ra[12];     /* raw a0 of the first twelve, encoded call-size */
/* [step 220] ZERO, was 1.
 *
 * Step 205 chose 1 because the entries it knew about returned bool and true is
 * 1. But struct wpa_funcs also contains POINTER-returning entries --
 * wpa_ap_init, wpa_ap_get_wpa_ie, wpa_config_parse_string, wpa3_build_sae_msg
 * -- and a WPA3/SAE-capable access point reaches one of them. Measured against
 * an iPhone hotspot:
 *
 *     exccause 28 LoadProhibited   epc 0x4000c2af (a ROM copy routine)
 *     excvaddr 0x00000001
 *
 * The driver took the 1 as a pointer and handed it to memcpy. NULL is the
 * value every one of these entries is checked against; 1 is checked by
 * nothing, and it is a valid-looking non-zero for the bool ones by accident
 * rather than by design. */
uint32_t g_wpa_ret;                     /* 0 -- NULL, and false */

/* The one entry known to need TRUE. wifi_station_start calls wpa_cb + 0
 * unguarded and a false there refuses the interface. */
int wpa_cb_true_stub(void);
int wpa_cb_true_stub(void)
{
    if (g_wpa_calls < 12u) { g_wpa_ra[g_wpa_calls] = (uint32_t)__builtin_return_address(0); }
    g_wpa_calls++;
    return 1;
}

int wpa_cb_stub(void);
int wpa_cb_stub(void)
{
    uint32_t ra = (uint32_t)__builtin_return_address(0);
    if (g_wpa_calls < 12u) {
        g_wpa_ra[g_wpa_calls] = ra;
    }
    g_wpa_calls++;
    return (int)g_wpa_ret;
}

/* The TABLE and the code that fills and reports it are CALL0 and live in
 * kernel/wifi_osi_impl.c. Only this stub is windowed, because only this stub is
 * called by the blob (callx8). Calling wpa_cb_table_fill() from wifi_bringup()
 * -- call0 code reaching straight into a windowed function -- is what produced
 * the IllegalInstruction at a garbage PC inside esp_wifi_init_internal, three
 * times, identically, while I was blaming the layout band for it. The rule this
 * project enforces by directory is the rule: only the ADDRESS crosses. */

/* ---- wpa_sta_connect -- next_moves/08 step 219 --------------------------
 *
 * The one supplicant entry that stalls an association, implemented.
 *
 * Step 218 measured where the driver stops: cnx_connect_next_ap loads
 * wpa_cb + 8 and calls it, and offset 8 is wpa_sta_connect. Our recording stub
 * returned 1 and did nothing, so nothing drove the next step and the driver sat
 * silent for thirty seconds -- no CONNECTED, no DISCONNECTED, no reason code.
 *
 * ESP-IDF's version, read from the source rather than guessed:
 *
 *     int wpa_sta_connect(uint8_t *bssid) {
 *         ret = wpa_config_profile(bssid);
 *         if (ret == 0) { ret = wpa_config_bss(bssid); if (ret) return ret; }
 *         else if (authmode == NONE_AUTH) esp_set_assoc_ie(bssid, NULL, 0, 0);
 *         return esp_wifi_sta_connect_internal(bssid);
 *     }
 *
 * wpa_config_profile and wpa_config_bss are the WPA half -- RSN IE parsing, PMK
 * derivation, the four-way handshake state. That is the subsystem this project
 * does not have. The LAST line is what moves the driver, and for an open
 * network it is very nearly the whole function.
 *
 * So this is that line and nothing else. An open network should associate. A
 * protected one should NOT -- but it should now fail with a reason code
 * instead of silence, which is worth having either way.
 *
 * Windowed, because the blob calls it with callx8 and it calls back into the
 * blob. Only the ADDRESS crosses, exactly as phy_wakeup_init does. */

uint32_t g_sta_connect_fn;     /* set from the entry table at bring-up */
uint32_t g_sta_connect_calls;
uint32_t g_sta_connect_rc = 0xFFFFFFFFu;

void wpa_install_rsn_ie(void);   /* defined below */
void wpa_hs_arm(void *bssid);
extern uint32_t g_appie_fn;
int wpa_sta_connect_impl(void *bssid);
int wpa_sta_connect_impl(void *bssid)
{
    typedef int (*sta_conn_fn)(void *);
    g_sta_connect_calls++;
    if (!g_sta_connect_fn) {
        return 1;                       /* nothing to call; behave as before */
    }
    /* [step 236] Declare the RSN IE before the association is driven, which is
     * the order ESP-IDF uses: wpa_config_bss() installs it, then
     * esp_wifi_sta_connect_internal() runs. */
    wpa_install_rsn_ie();

    /* [step 241] Derive the PMK and arm the handshake, once, here -- this is
     * windowed code and the crypto is windowed, so PBKDF2 is a direct call.
     *
     * PBKDF2 at 4096 iterations is NOT free: step 240 measured three of them
     * taking most of a minute. One is done, and only when the passphrase or
     * SSID has changed, because recomputing it per association would add tens
     * of seconds to every connect. */
    wpa_hs_arm(bssid);

    int rc = ((sta_conn_fn)g_sta_connect_fn)(bssid);
    g_sta_connect_rc = (uint32_t)rc;
    return rc;
}

/* ---- the RX data path -- next_moves/08 step 222 -------------------------
 *
 * esp_wifi_internal_reg_rxcb registers a callback the driver invokes for every
 * received DATA frame, already converted from 802.11 to Ethernet II: six bytes
 * destination, six source, two ethertype, then the payload. Management frames
 * do not come this way -- those are the driver's own business, which is why
 * scanning has worked all along without any of this.
 *
 * WINDOWED, because the driver calls it with callx8 from its own task, the
 * same reason wpa_sta_connect_impl is.
 *
 * IT MUST FREE THE BUFFER. esp_wifi_internal_free_rx_buffer returns the
 * driver's eb to its pool; not calling it leaks until the pool empties and
 * reception stops -- a failure that would look like the radio going deaf
 * rather than like a missing free. So the free happens first, before anything
 * else can go wrong, and the copy is taken before that.
 *
 * Deliberately does NOT parse. This step establishes that data frames arrive
 * at all and shows what they are; ARP and IP come after that is in evidence.
 */

uint32_t g_rx_frames;              /* total data frames seen */
uint32_t g_rx_bytes;
uint32_t g_rx_free_fn;             /* esp_wifi_internal_free_rx_buffer */
extern void net_rx_enqueue(void);   /* call0; address only */
#define RX_KEEP 6u
#define RX_SNAP 80u
uint32_t g_rx_len[RX_KEEP];
uint8_t  g_rx_snap[RX_KEEP][RX_SNAP];

int nat_rx_cb(void *buffer, unsigned short len, void *eb);
int nat_rx_cb(void *buffer, unsigned short len, void *eb)
{
    typedef void (*free_fn)(void *);
    const uint8_t *p = (const uint8_t *)buffer;

    if (g_rx_frames < RX_KEEP && p) {
        uint32_t n = len < RX_SNAP ? len : RX_SNAP;
        g_rx_len[g_rx_frames] = len;
        for (uint32_t i = 0u; i < n; i++) { g_rx_snap[g_rx_frames][i] = p[i]; }
    }
    g_rx_frames++;
    g_rx_bytes += len;

    /* [step 226] Hand it to the call0 side, which owns the parsing. The
     * crossing is w2c_call2 because this is windowed code and net_rx_enqueue
     * is not -- the boundary this project enforces by directory, and the one
     * step 205 lost three builds to ignoring. */
    if (p && len) {
        (void)w2c_call2((uint32_t)&net_rx_enqueue, (uint32_t)p, (uint32_t)len);
    }

    if (eb && g_rx_free_fn) { ((free_fn)g_rx_free_fn)(eb); }
    return 0;                                   /* ESP_OK */
}

/* ---- the RSN information element -- next_moves/08 step 236 --------------
 *
 * Step 219 measured WPA2 failing at reason 203, ASSOC_FAIL: the access point
 * rejects an association request that carries no RSN information element. That
 * IE is what ESP-IDF's wpa_config_bss() builds and wpa_config_assoc_ie()
 * installs with esp_wifi_set_appie_internal(WIFI_APPIE_RSN, ...).
 *
 * And for WPA2-PSK with CCMP it is a CONSTANT. No PMK, no PBKDF2, no crypto of
 * any kind -- it is a declaration of what this station intends to negotiate,
 * not proof that it can:
 *
 *   30 14                element 48 (RSN), length 20
 *   01 00                version 1
 *   00 0F AC 04          group cipher            CCMP
 *   01 00  00 0F AC 04   one pairwise cipher     CCMP
 *   01 00  00 0F AC 02   one AKM                 PSK
 *   00 00                RSN capabilities
 *
 * So this is a bounded experiment rather than a supplicant. If the association
 * now SUCCEEDS and fails later at the four-way handshake, step 219's reading
 * was right and what remains is exactly the crypto. If it still fails at 203,
 * the reading was wrong and the IE was never the obstacle.
 *
 * A full port is a much larger job: esp_wpa_main.c alone drags in utils/eloop,
 * several ESP-IDF headers and the whole ap/hostapd side even for a station,
 * across thirty-odd files plus a crypto layer. Worth knowing the answer to
 * this question before committing to that. */

/* [step 237] NOT const, and 4-byte aligned. As `static const uint8_t[22]` this
 * landed in .rodata, which on the ESP32 is FLASH mapped through the data cache
 * at 0x3F4xxxxx -- and handing that pointer to the blob produced
 *
 *     exccause 3 LoadStoreError   epc 0x40310458   excvaddr 0x3f40b334
 *
 * a fault inside the driver reading our own IE. Flash-mapped DROM does not
 * tolerate the access widths and alignments that RAM does, and a 22-byte array
 * has no alignment guarantee at all.
 *
 * Dropping const puts it in .data -- RAM -- and the alignment attribute makes
 * the width question moot. The general rule, which this project has now paid
 * for once: a buffer handed to the blob must live in RAM. */
static uint8_t g_rsn_ie[22] __attribute__((aligned(4))) = {
    0x30, 0x14,
    0x01, 0x00,
    0x00, 0x0F, 0xAC, 0x04,
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
    0x00, 0x00
};

uint32_t g_appie_fn;
uint32_t g_appie_rc = 0xFFFFFFFFu;

void wpa_install_rsn_ie(void);

void wpa_install_rsn_ie(void)
{
    typedef int (*appie_fn)(int, const void *, unsigned short, int);
    if (!g_appie_fn) { return; }
    /* WIFI_APPIE_RSN is 4, from esp_wifi_driver.h. The trailing 1 is what
     * IDF passes for this call site. */
    g_appie_rc = (uint32_t)((appie_fn)g_appie_fn)(4, g_rsn_ie,
                                                  (unsigned short)sizeof g_rsn_ie, 1);
}


/* ---- arming the handshake -- next_moves/08 step 241 ---------------------- */

#include "includes.h"   /* u8/size_t: sha1.h expects the utils layer */
#include "sha1.h"

extern void wpa_hs_set_pmk(const unsigned char *pmk);
extern void wpa_hs_set_addrs(const unsigned char *ap, const unsigned char *own);
extern uint32_t g_hs_tx_fn, g_hs_setkey_fn, g_hs_ptkdone_fn, g_hs_authdone_fn;

/* Filled by the kernel from the entry table before the connect. */
uint32_t g_hs_e_tx, g_hs_e_setkey, g_hs_e_ptkdone, g_hs_e_authdone, g_hs_e_getmac;
const char *g_hs_ssid;
const char *g_hs_pass;

static unsigned char g_hs_pmk[32];
static uint32_t g_hs_pmk_ready;

void wpa_hs_arm(void *bssid);
void wpa_hs_arm(void *bssid)
{
    typedef int (*getmac_fn_t)(unsigned char, unsigned char *);
    /* [step 241] NOT an aggregate initializer. GCC emits a call to memcpy for
     * `own[6] = {0,...}` -- a language-level copy, not a builtin, so
     * -fno-builtin does not stop it -- and memcpy is CALL0 while this file is
     * windowed. Measured: call8 memcpy, faulting at 0x40000000.
     *
     * The general hazard, which applies to every windowed file in this tree:
     * aggregate initializers and struct assignments can emit calls to call0
     * string functions that no source line mentions. */
    unsigned char own[6];
    own[0] = 0; own[1] = 0; own[2] = 0;
    own[3] = 0; own[4] = 0; own[5] = 0;

    g_hs_tx_fn       = g_hs_e_tx;
    g_hs_setkey_fn   = g_hs_e_setkey;
    g_hs_ptkdone_fn  = g_hs_e_ptkdone;
    g_hs_authdone_fn = g_hs_e_authdone;

    if (g_hs_e_getmac) { (void)((getmac_fn_t)g_hs_e_getmac)(0, own); }

    if (!g_hs_pmk_ready && g_hs_ssid && g_hs_pass) {
        unsigned int sl = 0u;
        while (g_hs_ssid[sl]) { sl++; }
        if (pbkdf2_sha1(g_hs_pass, (const unsigned char *)g_hs_ssid, sl,
                        4096, g_hs_pmk, 32) == 0) {
            g_hs_pmk_ready = 1u;
        }
    }
    if (g_hs_pmk_ready) { wpa_hs_set_pmk(g_hs_pmk); }
    wpa_hs_set_addrs((const unsigned char *)bssid, own);
}
