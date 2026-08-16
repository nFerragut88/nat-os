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
