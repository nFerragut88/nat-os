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

/* ---- slots 3 and 4, named -- next_moves/08 step 246 ---------------------
 *
 * WHAT IS BEING MEASURED: whether the driver ever tells the supplicant that
 * the station associated.
 *
 * Steps 237-245 all rest on one reading: reason 39 means "ASSOCIATED, and
 * then no handshake". That reading was never measured -- it was inferred from
 * 203 becoming 39, which is rule 11's exact trap. And reason 39 is
 * WIFI_REASON_TIMEOUT, NOT WIFI_REASON_HANDSHAKE_TIMEOUT (204), which is the
 * code ESP-IDF's driver reports for a station that associates and then cannot
 * complete EAPOL. If the association really completed, 204 is the code that
 * should have appeared.
 *
 * struct wpa_funcs slot 3 is wpa_sta_connected_cb(bssid) and slot 4 is
 * wpa_sta_disconnected_cb(reason). Both have been recording stubs sharing one
 * body, so a call to either was indistinguishable from a call to any other
 * slot. Named, they answer the question from the DRIVER'S OWN path:
 *
 *   conn >= 1   the driver declared the station associated. "Associated, no
 *               EAPOL" stands, and the packet capture is the right next move.
 *   conn == 0   it never associated. Reason 39 is an association timeout, the
 *               access point never had a station to send message one TO, and
 *               steps 237-245 have been debugging the wrong half.
 *
 * The reason also arrives here as an ARGUMENT rather than at a guessed +39
 * into the event payload, so the two cross-check each other.
 *
 * Instrumentation only: both return void in the header, nothing downstream
 * consumes a value, and no behaviour changes. */
uint32_t g_wpa_conn_cb, g_wpa_disc_cb, g_wpa_disc_reason = 0xFFFFFFFFu;

void wpa_sta_connected_cb_impl(unsigned char *bssid);
void wpa_sta_connected_cb_impl(unsigned char *bssid)
{
    (void)bssid;
    g_wpa_conn_cb++;
}

void wpa_sta_disconnected_cb_impl(unsigned char reason);
void wpa_sta_disconnected_cb_impl(unsigned char reason)
{
    g_wpa_disc_cb++;
    g_wpa_disc_reason = (uint32_t)reason;
}

/* ---- every slot, named -- next_moves/08 step 247 ------------------------
 *
 * Step 246 measured that the station never associates, and left twelve
 * wpa_cb hits recorded as RETURN ADDRESSES: which blob call site, never which
 * supplicant entry. One stub served all 128 slots, so it could not know its
 * own index.
 *
 * Now each of the first 32 slots has its own trampoline, which records its
 * index and its caller and then returns exactly what the shared pair returned
 * before -- 1 for the six bool entries that must be true, 0 (NULL, and false)
 * for everything else. Same values, same ABI, one more fact per call.
 *
 * The addresses cross to the call0 side as DATA, in g_wpa_slot_fn below. A
 * call0 caller reaching in to ask for them would be the exact violation that
 * cost three identical IllegalInstructions at step 219.
 *
 * Slots 32..127 keep the shared stub: nothing has ever called one, and 96
 * more trampolines to prove it is not worth the text. */
uint32_t g_wpa_slot_mask;      /* bit i: slot i was called at least once */
uint8_t  g_wpa_slot_seq[24];   /* the first 24 slots, IN CALL ORDER */

static int wpa_cb_slot(uint32_t slot, int ret, uint32_t ra)
{
    g_wpa_slot_mask |= (1u << slot);
    if (g_wpa_calls < 24u) { g_wpa_slot_seq[g_wpa_calls] = (uint8_t)slot; }
    if (g_wpa_calls < 12u) { g_wpa_ra[g_wpa_calls] = ra; }
    g_wpa_calls++;
    return ret;
}

/* r is the return value, and it is per entry for the reason step 220 paid for
 * twice: 1 everywhere crashed against a WPA3 access point, 0 everywhere hung
 * before set_mode. */
#define WPA_SLOT(n, r)                                                        \
    static int wpa_cb_s##n(void);                                             \
    static int wpa_cb_s##n(void)                                              \
    {                                                                         \
        return wpa_cb_slot(n, r, (uint32_t)__builtin_return_address(0));      \
    }

WPA_SLOT( 0, 1) WPA_SLOT( 1, 1) WPA_SLOT( 2, 0) WPA_SLOT( 3, 0)
WPA_SLOT( 4, 0) WPA_SLOT( 5, 0) WPA_SLOT( 6, 0) WPA_SLOT( 7, 0)
WPA_SLOT( 8, 1) WPA_SLOT( 9, 1) WPA_SLOT(10, 1) WPA_SLOT(11, 0)
WPA_SLOT(12, 1) WPA_SLOT(13, 0) WPA_SLOT(14, 0) WPA_SLOT(15, 0)
WPA_SLOT(16, 0) WPA_SLOT(17, 0) WPA_SLOT(18, 0) WPA_SLOT(19, 0)
WPA_SLOT(20, 0) WPA_SLOT(21, 0) WPA_SLOT(22, 0) WPA_SLOT(23, 0)
WPA_SLOT(24, 0) WPA_SLOT(25, 0) WPA_SLOT(26, 0) WPA_SLOT(27, 0)
WPA_SLOT(28, 0) WPA_SLOT(29, 0) WPA_SLOT(30, 0) WPA_SLOT(31, 0)

/* Addresses only, as DATA. .data rather than .rodata is deliberate: this array
 * is read by the call0 side, and step 236 measured that flash-mapped rodata
 * does not tolerate every access width. */
uint32_t g_wpa_slot_fn[32] = {
    (uint32_t)&wpa_cb_s0,  (uint32_t)&wpa_cb_s1,  (uint32_t)&wpa_cb_s2,
    (uint32_t)&wpa_cb_s3,  (uint32_t)&wpa_cb_s4,  (uint32_t)&wpa_cb_s5,
    (uint32_t)&wpa_cb_s6,  (uint32_t)&wpa_cb_s7,  (uint32_t)&wpa_cb_s8,
    (uint32_t)&wpa_cb_s9,  (uint32_t)&wpa_cb_s10, (uint32_t)&wpa_cb_s11,
    (uint32_t)&wpa_cb_s12, (uint32_t)&wpa_cb_s13, (uint32_t)&wpa_cb_s14,
    (uint32_t)&wpa_cb_s15, (uint32_t)&wpa_cb_s16, (uint32_t)&wpa_cb_s17,
    (uint32_t)&wpa_cb_s18, (uint32_t)&wpa_cb_s19, (uint32_t)&wpa_cb_s20,
    (uint32_t)&wpa_cb_s21, (uint32_t)&wpa_cb_s22, (uint32_t)&wpa_cb_s23,
    (uint32_t)&wpa_cb_s24, (uint32_t)&wpa_cb_s25, (uint32_t)&wpa_cb_s26,
    (uint32_t)&wpa_cb_s27, (uint32_t)&wpa_cb_s28, (uint32_t)&wpa_cb_s29,
    (uint32_t)&wpa_cb_s30, (uint32_t)&wpa_cb_s31
};

/* ---- the sniffer -- next_moves/08 step 250 ------------------------------
 *
 * Promiscuous mode has been ON since step 197 and NO CALLBACK WAS EVER
 * REGISTERED, so every frame the radio decoded outside the data path was
 * decoded and thrown away. Step 245 concluded that telling "the AP never
 * answers" from "the driver discards it" needed a packet capture on another
 * machine. It does not. The radio is already listening.
 *
 * wifi_promiscuous_pkt_t is rx_ctrl followed by the raw 802.11 frame. On
 * ESP32 rx_ctrl is SEVEN 32-bit words -- counted from the bitfields in
 * esp_wifi_types.h, not guessed:
 *
 *   0  rssi:8 rate:5 :1 sig_mode:2 :16
 *   1  mcs:7 cwb:1 :16 smoothing:1 not_sounding:1 :1 aggregation:1 stbc:2
 *      fec_coding:1 sgi:1
 *   2  noise_floor:8 ampdu_cnt:8 channel:4 secondary_channel:4 :8
 *   3  timestamp:32
 *   4  :32
 *   5  :31 ant:1
 *   6  sig_len:12 :12 rx_state:8
 *
 * so the frame starts at +28 and sig_len is the low 12 bits of word 6.
 *
 * CHECKED, not assumed, in the same spirit as the scan record at step 206:
 * sig_len must be a plausible frame length and the frame control's protocol
 * version bits must be zero. A frame failing either is counted as a layout
 * miss rather than decoded, so a wrong offset announces itself instead of
 * printing convincing rubbish.
 *
 * WHAT IT KEEPS. Beacons and probe responses are the overwhelming majority
 * of the air and say nothing about our association -- step 249 measured five
 * of them and nothing else. They are counted and dropped. What is kept is
 * everything else: authentication, association request and response,
 * deauthentication, disassociation, action.
 *
 * WINDOWED, and it must do almost nothing: this runs in the driver's own
 * receive path for every frame on the channel. Nine bytes are copied and it
 * returns. The buffer is the driver's and is NOT freed here -- unlike the
 * data path's rxcb, the promiscuous callback does not own it. */

#define SNF_MAX 24u

uint32_t g_snf_total, g_snf_drop_bcn, g_snf_layout, g_snf_kept;
uint8_t  g_snf_fc[SNF_MAX];       /* frame control byte 0: type and subtype */
uint8_t  g_snf_ch[SNF_MAX];
signed char g_snf_rssi[SNF_MAX];
uint8_t  g_snf_a1[SNF_MAX][3];    /* receiver  -- last three bytes */
uint8_t  g_snf_a2[SNF_MAX][3];    /* sender    -- last three bytes */
/* [step 251] Six bytes of body, from offset 24 -- the fixed parameters, and
 * the same six serve every frame this keeps:
 *
 *   AUTH        alg[2]  seq[2]  STATUS[2]
 *   ASSOC_RESP  capab[2]        STATUS[2]  aid[2]
 *   DEAUTH / DISASSOC           reason[2]
 *
 * Status 0 is success. A rejection is a frame from the access point
 * addressed to us exactly like an acceptance, which is why step 250 could
 * not tell them apart and refused to guess. */
uint8_t  g_snf_body[SNF_MAX][6];

void natos_sniff_cb(void *buf, int type);
void natos_sniff_cb(void *buf, int type)
{
    const unsigned char *b = (const unsigned char *)buf;
    if (!b) { return; }
    g_snf_total++;
    if (type != 0) { return; }              /* WIFI_PKT_MGMT only */

    const uint32_t *w = (const uint32_t *)buf;
    uint32_t siglen = w[6] & 0xFFFu;
    const unsigned char *f = b + 28;

    /* The two self-checks. A management frame is at least 24 bytes of header
     * plus a 4-byte FCS, and the protocol version must be 0. */
    if (siglen < 28u || siglen > 1600u || (f[0] & 0x03u) != 0u) {
        g_snf_layout++;
        return;
    }

    uint32_t sub = (uint32_t)(f[0] >> 4);    /* subtype */
    if (sub == 8u || sub == 5u || sub == 4u) {
        g_snf_drop_bcn++;                    /* beacon, probe resp, probe req */
        return;
    }

    if (g_snf_kept < SNF_MAX) {
        uint32_t i = g_snf_kept;
        g_snf_fc[i]   = f[0];
        g_snf_ch[i]   = (uint8_t)((w[2] >> 16) & 0xFu);
        g_snf_rssi[i] = (signed char)(w[0] & 0xFFu);
        g_snf_a1[i][0] = f[7];  g_snf_a1[i][1] = f[8];  g_snf_a1[i][2] = f[9];
        g_snf_a2[i][0] = f[13]; g_snf_a2[i][1] = f[14]; g_snf_a2[i][2] = f[15];
        /* [step 251] siglen carries the FCS, so 30 is the smallest frame that
         * really has six body bytes. Short ones leave zeroes, which the
         * report shows as such rather than inventing a status. */
        for (uint32_t q = 0u; q < 6u; q++) {
            g_snf_body[i][q] = (siglen >= 30u) ? f[24u + q] : 0u;
        }
    }
    g_snf_kept++;
}

/* ---- wpa_cb slot 21: wpa_sta_rx_mgmt -- next_moves/08 step 249 ----------
 *
 * Seven calls on every failing connect, and until now only a count.
 *
 *   int wpa_sta_rx_mgmt(u8 type, u8 *frame, size_t len, u8 *sender,
 *                       int8_t rssi, u8 channel, u64 current_tsf);
 *
 * The first argument is the management frame subtype and the fourth is who
 * sent it. Those two say whether the ACCESS POINT IS ANSWERING:
 *
 *   11 auth / 1 assoc resp from the AP's BSSID   it is talking to us, and
 *                                                the failure is later
 *   8 beacon / 5 probe resp only                 it is not answering at all
 *
 * Six parameters are declared and the u64 TSF is not. The windowed ABI passes
 * the first six words in a2..a7 and the rest on the caller's stack, and a
 * callee never pops arguments, so ignoring the tail is safe. Declaring a u64
 * would split it across a7 and the stack for no gain -- nothing here reads it.
 *
 * Return 0, exactly as the recording stub did. This step changes no
 * behaviour; it only reads what was already crossing.
 *
 * WINDOWED: the blob calls it through the table with callx8. */
uint32_t g_mgmt_calls;
uint8_t  g_mgmt_type[12];
uint8_t  g_mgmt_ch[12];
signed char g_mgmt_rssi[12];
uint8_t  g_mgmt_src[12][3];     /* last three bytes -- enough to tell APs apart */

int wpa_sta_rx_mgmt_impl(unsigned char type, unsigned char *frame,
                         unsigned int len, unsigned char *sender,
                         signed char rssi, unsigned char channel);
int wpa_sta_rx_mgmt_impl(unsigned char type, unsigned char *frame,
                         unsigned int len, unsigned char *sender,
                         signed char rssi, unsigned char channel)
{
    (void)frame; (void)len;
    if (g_mgmt_calls < 12u) {
        uint32_t i = g_mgmt_calls;
        g_mgmt_type[i] = type;
        g_mgmt_ch[i]   = channel;
        g_mgmt_rssi[i] = rssi;
        if (sender) {
            g_mgmt_src[i][0] = sender[3];
            g_mgmt_src[i][1] = sender[4];
            g_mgmt_src[i][2] = sender[5];
        }
    }
    g_mgmt_calls++;
    return 0;
}

/* ---- wpa_cb slot 15: wpa_parse_wpa_ie -- next_moves/08 step 248 ---------
 *
 * Step 247 measured this entry being called THREE TIMES on the failing
 * connect, once per attempt, while it was a recording stub that returned 0 --
 * success -- and wrote nothing. The driver read back a zeroed wifi_wpa_ie_t:
 * proto 0, pairwise 0, group 0, key_mgmt 0. "This access point offers no
 * security at all", three times, about a WPA2 network.
 *
 * The driver hands us the access point's RSN information element, taken from
 * its beacon, and a struct to fill in. This fills it.
 *
 * Layout from wifi_wpa_ie_t in esp_wifi_driver.h, and the field CONVENTIONS
 * from wpa_parse_wpa_ie_wrapper() in esp_wpa_main.c, which is the function
 * this replaces. They are not the same convention for every field, and that
 * is the part worth reading twice:
 *
 *   proto            SUPPLICANT bitmask   WPA_PROTO_RSN = BIT(1) = 2
 *   pairwise_cipher  PUBLIC enum          WIFI_CIPHER_TYPE_CCMP = 4
 *   group_cipher     PUBLIC enum          mapped, same as pairwise
 *   key_mgmt         SUPPLICANT bitmask   WPA_KEY_MGMT_PSK = BIT(1) = 2
 *   capabilities     raw RSN capabilities, straight from the element
 *
 * So two of the five are mapped through cipher_type_map_supp_to_public and
 * two are not. Handing the driver a supplicant bitmask where it expects the
 * public enum would put TKIP (3) where CCMP (4) belongs.
 *
 * WINDOWED: the blob calls it through the table with callx8.
 *
 * SCOPE: RSN (WPA2) only, which is this project's scope everywhere else. A
 * WPA1 vendor element or anything malformed returns -1 rather than a
 * confident zero -- the whole failure this step exists to undo. */

struct natos_wpa_ie {
    int proto;
    int pairwise_cipher;
    int group_cipher;
    int key_mgmt;
    int capabilities;
    unsigned int num_pmkid;
    const unsigned char *pmkid;
    int mgmt_group_cipher;
    unsigned char rsnxe_capa;
};

uint32_t g_pie_calls, g_pie_ok, g_pie_bad, g_pie_len;
uint32_t g_pie_group, g_pie_pair, g_pie_akm, g_pie_caps;

/* 00-0F-AC-xx selectors, to the supplicant's WPA_CIPHER_* bits. */
static uint32_t rsn_cipher_bit(const unsigned char *p)
{
    if (p[0] != 0x00u || p[1] != 0x0Fu || p[2] != 0xACu) { return 0u; }
    switch (p[3]) {
    case 0u:  return 0u;         /* "use the group cipher" */
    case 1u:  return 1u << 7;    /* WEP40         */
    case 2u:  return 1u << 1;    /* TKIP          */
    case 4u:  return 1u << 3;    /* CCMP          */
    case 5u:  return 1u << 8;    /* WEP104        */
    case 6u:  return 1u << 5;    /* AES-128-CMAC  */
    default:  return 0u;
    }
}

/* cipher_type_map_supp_to_public, from wpa.c, for the cases an RSN element
 * can actually produce. */
static int cipher_to_public(uint32_t m)
{
    if (m == (1u << 3))              { return 4; }   /* CCMP        */
    if (m == (1u << 1))              { return 3; }   /* TKIP        */
    if (m == ((1u << 3)|(1u << 1)))  { return 5; }   /* TKIP+CCMP   */
    if (m == (1u << 5))              { return 6; }   /* AES-CMAC128 */
    if (m == (1u << 7))              { return 1; }   /* WEP40       */
    if (m == (1u << 8))              { return 2; }   /* WEP104      */
    if (m == 0u)                     { return 0; }   /* NONE        */
    return 12;                                       /* UNKNOWN     */
}

static uint32_t rsn_akm_bit(const unsigned char *p)
{
    if (p[0] != 0x00u || p[1] != 0x0Fu || p[2] != 0xACu) { return 0u; }
    switch (p[3]) {
    case 1u:  return 1u << 0;    /* IEEE8021X        */
    case 2u:  return 1u << 1;    /* PSK  <- WPA2-PSK */
    case 5u:  return 1u << 7;    /* IEEE8021X_SHA256 */
    case 6u:  return 1u << 8;    /* PSK_SHA256       */
    case 8u:  return 1u << 10;   /* SAE              */
    default:  return 0u;
    }
}

static uint32_t le16(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

int wpa_parse_wpa_ie_impl(const unsigned char *ie, unsigned int len,
                          struct natos_wpa_ie *d);
int wpa_parse_wpa_ie_impl(const unsigned char *ie, unsigned int len,
                          struct natos_wpa_ie *d)
{
    g_pie_calls++;
    if (!ie || !d || len < 2u) { g_pie_bad++; return -1; }

    /* Field by field, NOT a struct assignment or an aggregate: either can
     * emit a call to call0 memcpy from this windowed file, which is the
     * hazard wpa_hs_arm() paid for at step 241. */
    d->proto = 0; d->pairwise_cipher = 0; d->group_cipher = 0;
    d->key_mgmt = 0; d->capabilities = 0; d->num_pmkid = 0u;
    d->pmkid = 0; d->mgmt_group_cipher = 0; d->rsnxe_capa = 0u;

    /* The driver may hand over the whole element or just its body. Sniffed
     * rather than assumed: an RSN body begins with version 1 as 01 00, so a
     * leading 48 can only be the element id. */
    const unsigned char *p = ie;
    unsigned int n = len;
    if (p[0] == 48u) {
        if ((unsigned int)p[1] + 2u > len) { g_pie_bad++; return -1; }
        n = (unsigned int)p[1];
        p += 2;
    }
    g_pie_len = n;

    if (n < 6u)                     { g_pie_bad++; return -1; }
    if (le16(p) != 1u)              { g_pie_bad++; return -1; }  /* version */

    unsigned int o = 2u;
    d->proto = 2;                                   /* WPA_PROTO_RSN */

    uint32_t grp = rsn_cipher_bit(&p[o]);
    o += 4u;
    d->group_cipher = cipher_to_public(grp);
    g_pie_group = grp;

    uint32_t pair = 0u;
    if (o + 2u <= n) {
        uint32_t cnt = le16(&p[o]);
        o += 2u;
        for (uint32_t i = 0u; i < cnt; i++) {
            if (o + 4u > n) { g_pie_bad++; return -1; }
            pair |= rsn_cipher_bit(&p[o]);
            o += 4u;
        }
    }
    if (pair == 0u) { pair = grp; }     /* an absent list means "use group" */
    d->pairwise_cipher = cipher_to_public(pair);
    g_pie_pair = pair;

    uint32_t akm = 0u;
    if (o + 2u <= n) {
        uint32_t cnt = le16(&p[o]);
        o += 2u;
        for (uint32_t i = 0u; i < cnt; i++) {
            if (o + 4u > n) { g_pie_bad++; return -1; }
            akm |= rsn_akm_bit(&p[o]);
            o += 4u;
        }
    }
    d->key_mgmt = (int)akm;             /* bitmask, NOT mapped */
    g_pie_akm = akm;

    if (o + 2u <= n) {
        d->capabilities = (int)le16(&p[o]);
        g_pie_caps = (uint32_t)d->capabilities;
    }

    /* The PMKID list and the group management cipher are deliberately not
     * parsed: this station does no PMKSA caching and no management frame
     * protection, so num_pmkid stays 0 and pmkid stays NULL -- which is what
     * the wrapper this replaces leaves them as too. */
    g_pie_ok++;
    return 0;
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
/* [step 253] TWO BYTES OF HEADROOM, and they are the whole point.
 *
 * ESP-IDF does NOT hand esp_wifi_set_appie_internal a pointer to the RSN
 * element. set_assoc_ie() in rsn_supp/wpa.c:2520 does this:
 *
 *     sm->assoc_wpa_ie     = assoc_buf + 2;          <- element lives at +2
 *     sm->assoc_wpa_ie_len = ASSOC_IE_LEN - 2;
 *     wpa_config_assoc_ie(sm->proto, assoc_buf, sm->assoc_wpa_ie_len);
 *                                    ^^^^^^^^^ the START of the buffer
 *
 * and only afterwards does wpa_gen_wpa_ie() write the element, at +2. So the
 * buffer the driver is given has two bytes in front of the element that are
 * not part of it, and the length is the buffer CAPACITY rather than the
 * element size -- set_appie is called before the element even exists, so it
 * cannot be the element size.
 *
 * nat-os pointed straight at the element and passed 22. If the driver reads
 * the element from ptr+2, it saw 01 00 -- the RSN version field -- as an
 * element id and length. That is a malformed information element, and an
 * access point that cannot parse an association request DISCARDS IT
 * SILENTLY, which is exactly the measurement of step 252: authentication
 * accepted, association request transmitted, no response of any kind.
 *
 * ASSOC_IE_LEN is 24 + 2 + PMKID_LEN + RSN_SELECTOR_LEN = 46, and the buffer
 * is two longer, both from wpa.c. The shape is reproduced rather than
 * reasoned about. */
#define NATOS_ASSOC_IE_LEN (24 + 2 + 16 + 4)     /* ESP-IDF ASSOC_IE_LEN = 46 */

static uint8_t g_rsn_ie[NATOS_ASSOC_IE_LEN + 2] __attribute__((aligned(4))) = {
    0x00, 0x00,                       /* headroom -- NOT part of the element */
    0x30, 0x14,
    0x01, 0x00,
    0x00, 0x0F, 0xAC, 0x04,
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
    0x00, 0x00
    /* the rest is zero, and is the headroom wpa_gen_wpa_ie would have used */
};

uint32_t g_appie_fn;
uint32_t g_appie_rc = 0xFFFFFFFFu;

void wpa_install_rsn_ie(void);

/* [step 252] The A/B switch. Written from call0 as DATA -- a call0 caller
 * reaching into this windowed file would be the step-219 violation. */
uint32_t g_rsn_ie_enable = 1u;

/* [step 254] The AKM suite TYPE byte, 00-0F-AC-xx, at element offset 19.
 * 2 is PSK and is what a WPA2-PSK network wants; 1 is 802.1X, which this
 * access point must REFUSE. The point is the refusal: a well-formed element
 * the AP dislikes earns a status code, and silence would mean the element is
 * not being read as an element at all. */
uint32_t g_rsn_akm_type = 2u;

/* [step 255] The two remaining candidates from step 252, one constant each.
 *
 *   type  WIFI_APPIE_RSN is 4 and is what wpa_config_assoc_ie() uses for an
 *         RSN network. WIFI_APPIE_ASSOC_REQ is 1 and is what esp_set_assoc_ie()
 *         uses -- a different call, on the same frame, in the same connect.
 *   flag  the trailing argument, 1 at IDF's RSN call site and 0 at every
 *         other set_appie call site in the tree. Nothing names what it means.
 */
uint32_t g_appie_type = 4u;
/* [step 257] ZERO, and this is the fix. Step 256 measured that the trailing
 * argument to esp_wifi_set_appie_internal must be 0 for the association to
 * complete: with 1 the access point never answers the association request,
 * with 0 it answers status 0 and sends EAPOL. ESP-IDF passes 1 at its own RSN
 * call site; why that works there is NOT understood and is not guessed at. */
uint32_t g_appie_flag = 0u;

void wpa_install_rsn_ie(void)
{
    typedef int (*appie_fn)(int, const void *, unsigned short, int);
    if (!g_appie_fn) { return; }
    /* [step 252] Suppressed by 'wifiinit startnoie'. Before the RSN IE went
     * in, this router answered 203 ASSOC_FAIL -- a refusal the access point
     * has to transmit. After it, silence. Putting the variable back tells us
     * whether the association request is going out at all. */
    if (!g_rsn_ie_enable) { g_appie_rc = 0x4E4F4945u;   /* "NOIE" -- suppressed, not failed */ return; }
    /* WIFI_APPIE_RSN is 4, from esp_wifi_driver.h. The trailing 1 is what
     * IDF passes for this call site. */
    /* [step 253] ELIMINATED -- passing the buffer START and the CAPACITY, the
     * way set_assoc_ie() does, PANICS THE DRIVER:
     *
     *     exccause 20 InstFetchProhibited   epc 0x00006898
     *     a0 0x8036cc9c  -- returning into the blob
     *
     * immediately after connect, inside the association path. A jump to a
     * garbage address is memory corruption, not a parse failure, so the
     * driver does more with this buffer than copy it and the two-byte
     * headroom is not simply headroom. Whatever set_appie means by its
     * length, it is not 'capacity', and the reading that produced this is
     * wrong.
     *
     * Back to the element and its own length, which is step 237s form: the
     * access point stays silent, but the board stays up, and silence is a
     * measurement while a panic is not. The buffer keeps its headroom so the
     * next attempt can move the pointer without touching the layout. */
    /* [step 254] Patched in place: the buffer is .data and the element is at
     * +2, so this is byte 2 + 19 = 21. Everything else -- length, version,
     * both cipher suites, the counts, the capabilities -- is untouched. */
    g_rsn_ie[2u + 19u] = (unsigned char)g_rsn_akm_type;

    g_appie_rc = (uint32_t)((appie_fn)g_appie_fn)((int)g_appie_type,
                                                  g_rsn_ie + 2,
                                                  (unsigned short)22,
                                                  (int)g_appie_flag);
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
/* [step 292] NOT static. The call0 side clears this directly, because a write
 * crosses no ABI and a call does. See the removal below. */
uint32_t g_hs_pmk_ready;

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

    /* [step 243] The PMK is NOT derived here. It used to be, and that was the
     * bug: wpa_sta_connect is called BY THE DRIVER from cnx_connect_next_ap,
     * and PBKDF2 at 4096 iterations measured ~18 seconds on this part. Eighteen
     * seconds inside the driver's connect callback means the association is
     * long dead before the access point would send message one -- which is
     * exactly what was observed: no CONNECTED, no DISCONNECTED, no EAPOL, and a
     * handshake that never ran.
     *
     * Step 240 measured that cost and wrote down that it "should not be
     * recomputed per association". It was then put in the per-association path
     * anyway. Measuring a thing does not help if the measurement is not used.
     *
     * wpa_hs_derive_pmk() now runs once at bring-up, before any connect. */
    if (g_hs_pmk_ready) { wpa_hs_set_pmk(g_hs_pmk); }
    wpa_hs_set_addrs((const unsigned char *)bssid, own);
}

/* [step 243] Derive the PMK once, at bring-up, off the driver's connect path.
 * Windowed because the crypto is; reached from call0 through blob_call with no
 * arguments, the same way the self-test is. */
/* [step 278] Let a new SSID force a fresh derivation. g_hs_pmk_ready exists so
 * the 15 s PBKDF2 runs once per boot; joining a DIFFERENT network needs a
 * different key, so the view clears it deliberately rather than the guard
 * being weakened for everyone. */
/* [step 292] g_hs_pmk_ready_reset() WAS HERE, and it was the crash.
 *
 * This file is compiled WINDOWED. wifi_join_ssid(), which called it, is call0.
 * A call0 `call0` into a windowed function reaches a `retw` that pops a
 * register window nobody pushed -- so it returned onto a garbage a0 (0x30 in
 * the dump) and executed it: exccause 0, IllegalInstruction, at epc 0x4009d36e,
 * which is inside this twelve-byte function.
 *
 * The line below it in the same block did it correctly:
 *
 *     (void)g_hs_pmk_ready_reset();                              <- direct
 *     (void)blob_call((uint32_t)&wpa_hs_derive_pmk, 0,0,0,0);    <- bridged
 *
 * Sixth time this project has paid for that ABI crossing, and the first where
 * the correct form was one line away in the same expression block.
 *
 * Not replaced with a bridged call. The whole function existed to zero one
 * word, and a memory write has no calling convention: the flag is extern now
 * and the call0 side assigns it. The cheapest fix for an ABI bug is to not
 * make the call. */

int wpa_hs_derive_pmk(void);
int wpa_hs_derive_pmk(void)
{
    if (g_hs_pmk_ready) { return 0; }
    if (!g_hs_ssid || !g_hs_pass) { return -1; }
    unsigned int sl = 0u;
    while (g_hs_ssid[sl]) { sl++; }
    if (pbkdf2_sha1(g_hs_pass, (const unsigned char *)g_hs_ssid, sl,
                    4096, g_hs_pmk, 32) != 0) {
        return -1;
    }
    g_hs_pmk_ready = 1u;
    return 0;
}
