/* nat-os — the wifi view. See wifiapp.h for scope and for why the passphrase
 * is the compiled-in one. */

#include "wifiapp.h"
#include "display.h"
#include "desktop.h"
#include "blob.h"
#include "timer.h"
#include "audio.h"
#include "wifi_secrets.h"

/* ---- layout ---------------------------------------------------------------
 *
 * Unlike the shell and the note pad this view has no keyboard, so it stays
 * inside DESK_H and leaves the application band alone. Nothing here needs the
 * extra 64 px, and taking a band that other programs draw in has a cost -- see
 * app_views_suspend() -- which should be paid only by the views that need it.
 */
#define HDR_H     22u
#define ROW_H     20u
#define LIST_Y    (HDR_H + 2u)
#define STAT_H    34u
#define STAT_Y    (DESK_H - STAT_H)
#define LIST_H    (STAT_Y - LIST_Y)
#define MAX_ROWS  (LIST_H / ROW_H)
#define MAX_APS   12u

_Static_assert(MAX_ROWS >= 5u, "a network list under five rows is not a list");
_Static_assert(STAT_Y + STAT_H == DESK_H, "status bar must meet the region end");

#define BG       COLOR_BLACK
#define FG       COLOR_WHITE
#define DIM      COLOR_GREY
#define OK       COLOR_GREEN
#define BUSY     COLOR_YELLOW
#define BAD      COLOR_RED

/* ---- state ---------------------------------------------------------------- */

static wifi_ap_t g_aps[MAX_APS];
static uint32_t  g_count;
static int       g_sel = -1;
static int       g_dirty;
static int       g_was_down;

/* The sweep is spread across frames: one channel per frame, so the view stays
 * responsive and the progress is visible. 0 means idle. */
static uint32_t  g_scan_ch;

/* What the last action did, shown in the status bar. Deliberately a small enum
 * rather than a string: every state here is one the code puts the radio into,
 * and a free-text message invites saying something the radio did not actually
 * report.
 *
 * [step 279] ST_NORADIO and ST_NOSTART are split out of ST_FAILED, which had
 * been carrying three different meanings and rendering all of them as "join
 * failed". A scan that cannot run because the radio was never brought up is
 * not a failed join -- it is a join that was never attempted, and the first
 * version of this file reported it as the former. That is the sixth time this
 * project has found a status line claiming an outcome for work that never ran;
 * the difference here is that the reader was a person, not a log. */
enum { ST_IDLE = 0, ST_NORADIO, ST_STARTING, ST_NOSTART,
       ST_SCANNING, ST_DERIVING, ST_JOINING, ST_JOINED, ST_FAILED };
static int      g_state;
static char     g_joined[33];

/* ---- helpers -------------------------------------------------------------- */

static void put(uint32_t x, uint32_t y, const char *s, uint16_t fg, uint16_t bg)
{
    display_text(x, y, s, fg, bg, 1u);
}

/* Signal as four bars' worth of description rather than a number nobody reads
 * off a phone: -50 is excellent, -90 is the noise floor (step 208 measured
 * exactly that and it is why this scale is what it is). */
static const char *strength(signed char rssi)
{
    if (rssi >= -55) { return "****"; }
    if (rssi >= -70) { return "***-"; }
    if (rssi >= -82) { return "**--"; }
    return "*---";
}

static void draw_row(uint32_t i)
{
    if (i >= g_count || i >= MAX_ROWS) { return; }
    uint32_t y = LIST_Y + i * ROW_H;
    int      on = ((int)i == g_sel);
    uint16_t bg = on ? COLOR_BLUE : BG;

    display_fill_rect(0, y, DISP_W, ROW_H - 1u, bg);
    put(4u, y + 6u, g_aps[i].ssid, FG, bg);
    put(DISP_W - 58u, y + 6u, strength(g_aps[i].rssi), DIM, bg);
    put(DISP_W - 26u, y + 6u, g_aps[i].auth ? "wpa" : "open", DIM, bg);
}

static void draw_status(void)
{
    display_fill_rect(0, STAT_Y, DISP_W, STAT_H, BG);

    const char *msg;
    uint16_t    col;
    switch (g_state) {
    case ST_NORADIO:  msg = "radio off -- tap start";   col = DIM;  break;
    case ST_STARTING: msg = "starting radio -- 90 s";   col = BUSY; break;
    case ST_NOSTART:  msg = "radio did not start";      col = BAD;  break;
    case ST_SCANNING: msg = "scanning";                 col = BUSY; break;
    case ST_DERIVING: msg = "deriving key -- 15 s";     col = BUSY; break;
    case ST_JOINING:  msg = "joining";                  col = BUSY; break;
    case ST_JOINED:   msg = g_joined;                   col = OK;   break;
    case ST_FAILED:   msg = "join failed";              col = BAD;  break;
    default:          msg = "tap a network to join";    col = DIM;  break;
    }
    put(4u, STAT_Y + 8u, msg, col, BG);

    /* One button, whose meaning follows the radio: there is nothing to scan
     * for until the radio is up, and nothing to start once it is. Two buttons
     * would mean one of them is always wrong to press.
     *
     * It sits in the status bar rather than the list so that a list which
     * fills the view cannot push it off. */
    display_fill_rect(DISP_W - 52u, STAT_Y + 4u, 48u, 16u, COLOR_BLUE);
    put(DISP_W - 46u, STAT_Y + 9u, blob_ready() ? "scan" : "start", FG,
        COLOR_BLUE);
}

static void draw_all(void)
{
    display_fill_rect(0, 0, DISP_W, DESK_H, BG);
    display_fill_rect(0, 0, DISP_W, HDR_H, COLOR_BLUE);
    put(6u, 8u, "wifi", FG, COLOR_BLUE);

    if (g_count == 0u && g_state != ST_SCANNING) {
        put(4u, LIST_Y + 6u, "no networks -- tap scan", DIM, BG);
    }
    for (uint32_t i = 0u; i < g_count && i < MAX_ROWS; i++) {
        draw_row(i);
    }
    draw_status();
}

/* ---- the radio ------------------------------------------------------------ */

/* One channel per frame. The whole sweep is thirteen frames, which reads as a
 * list filling in rather than as a pause. */
static void scan_step(void)
{
    const struct blob_entry *e = blob_map();
    if (!e || !blob_ready()) {
        g_state   = ST_NORADIO;      /* NOT ST_FAILED -- see the enum */
        g_scan_ch = 0u;
        g_dirty   = 1;
        return;
    }

    /* [step 280] Scan into a scratch buffer and MERGE, rather than appending
     * straight into the list.
     *
     * Appending showed one network three times. The 2.4 GHz channels overlap by
     * about 20 MHz, so a beacon transmitted on channel 6 is receivable while the
     * radio is parked on 5 and on 7, and the blob reports it each time. Three
     * rows, one access point, and nothing wrong with any of the three readings.
     *
     * Merged on SSID and not on BSSID, deliberately: the BSSID is in the record
     * and is not extracted here, and a person picking a network to join wants
     * the network. A mesh with three radios behind one name is one row, which is
     * also the right answer.
     *
     * The strongest sighting wins, and carries its channel with it -- that is
     * the one the radio would actually associate on. */
    static wifi_ap_t tmp[6];
    uint32_t got = wifi_scan_channel(e->wifi_scan_start, e->wifi_scan_ap_num,
                                     e->wifi_scan_ap_recs, g_scan_ch,
                                     tmp, 6u);
    for (uint32_t k = 0u; k < got; k++) {
        uint32_t i = 0u;
        for (; i < g_count; i++) {
            uint32_t j = 0u;
            while (j < 32u && g_aps[i].ssid[j] && g_aps[i].ssid[j] == tmp[k].ssid[j]) {
                j++;
            }
            if (g_aps[i].ssid[j] == tmp[k].ssid[j]) { break; }   /* both ended */
        }
        if (i < g_count) {
            if (tmp[k].rssi > g_aps[i].rssi) {
                g_aps[i].rssi = tmp[k].rssi;
                g_aps[i].ch   = tmp[k].ch;
            }
        } else if (g_count < MAX_APS) {
            g_aps[g_count++] = tmp[k];
        }
    }

    if (++g_scan_ch > 13u || g_count >= MAX_APS) {
        g_scan_ch = 0u;
        g_state   = ST_IDLE;
    }
    g_dirty = 1;
}

/* Bring the radio up: the whole of what the shell's `wifiinit start` does.
 *
 * This is why the first version of this view did nothing useful. It could scan
 * and it could join, and both of those need a radio that something ELSE had
 * already started -- so the app worked on a board where the shell had been used
 * first and was inert on a board that had just booted. An app that only works
 * after you have used a different app is not an app.
 *
 * BLOCKING, and for a long time: PHY init, the driver's own scan sweep, the
 * association, and PBKDF2 at 4096 iterations. Measured near ninety seconds. The
 * status bar is painted before it starts, and it is painted by task_display --
 * a different task -- so the message survives the whole of it rather than
 * freezing half-drawn.
 *
 * The argument values are not invented here. Each is the one the shell passes
 * for plain `start`, and they are the settled answers to steps 252-257:
 *   rsn_ie_enable(1)      the RSN element goes out            (step 252)
 *   rsn_akm_set(2)        AKM 2 = PSK, not 1 = 802.1X         (step 254)
 *   appie_shape(4, 0)     type RSN(4), flag 0 -- the fix      (step 256/257)
 *   hs_passive(0)         run the four-way handshake for real (step 257)
 *   blob_task_enable(0)   blob task creation still panics     (step 190)
 * Copying them rather than re-deriving them is deliberate: this view must not
 * become a second opinion about how to start the radio. */
static void start_radio(void)
{
    extern void     wifi_start_enable(int on);
    extern uint32_t wifi_bringup(const struct blob_entry *e, int want_null);
    extern void     blob_task_enable(int on);
    extern void     wifi_rsn_ie_enable(int on);
    extern void     wifi_rsn_akm_set(unsigned int t);
    extern void     wifi_appie_shape(unsigned int t, unsigned int f);
    extern void     wifi_hs_passive(int on);
    extern int      phyinit_run_at(uint32_t fn);
    extern int      wifi_joined(void);

    g_state = ST_STARTING;
    draw_status();

    /* blob_map() is safe from here even though this file lives in irom. It
     * disables the cache to rewrite the MMU, but it is itself IRAM-resident and
     * turns the cache back on before returning -- blob.c says so in as many
     * words, and shell.c has called it from flash since step 190. */
    const struct blob_entry *e = blob_map();
    if (!e || blob_init(e) != 0) {
        g_state = ST_NOSTART;
        g_dirty = 1;
        return;
    }
    (void)phyinit_run_at(e->phy_init);

    blob_task_enable(0);
    wifi_start_enable(1);
    wifi_rsn_ie_enable(1);
    wifi_rsn_akm_set(2u);
    wifi_appie_shape(4u, 0u);
    wifi_hs_passive(0);

    (void)wifi_bringup(e, 0);

    if (!blob_ready()) {
        g_state = ST_NOSTART;
        g_dirty = 1;
        return;
    }

    /* The bring-up ends by joining the network in wifi_secrets.h. If that
     * worked, say so: the user asked for a radio and got a connection. */
    if (wifi_joined()) {
        /* The bring-up joins the compiled-in network, so that -- and not the
         * selected row, which does not exist yet -- is what to name. */
        const char *n = WIFI_STA_SSID;
        uint32_t    k = 0u;
        for (; k < 32u && n[k]; k++) { g_joined[k] = n[k]; }
        g_joined[k] = 0;
        g_state = ST_JOINED;
    } else {
        g_state = ST_IDLE;
    }

    /* Either way, fill the list, so what appears next is a set of networks to
     * choose from rather than an empty view with a live radio. */
    g_count   = 0u;
    g_sel     = -1;
    g_scan_ch = 1u;
    g_dirty   = 1;
}

/* Join the selected network with the compiled-in passphrase.
 *
 * BLOCKING, and the status bar is painted before it starts. PBKDF2 at 4096
 * iterations measures ~15 s on this part; a view that redrew afterwards would
 * look like a hang for the whole of it. */
static void join(uint32_t i)
{
    extern void wifi_join_ssid(const char *ssid);
    extern int  wifi_joined(void);

    if (i >= g_count) { return; }
    if (!blob_ready()) { g_state = ST_NORADIO; g_dirty = 1; return; }

    g_state = ST_DERIVING;
    draw_status();

    wifi_join_ssid(g_aps[i].ssid);

    if (wifi_joined()) {
        uint32_t k = 0u;
        for (; k < 32u && g_aps[i].ssid[k]; k++) { g_joined[k] = g_aps[i].ssid[k]; }
        g_joined[k] = 0;
        g_state = ST_JOINED;
    } else {
        g_state = ST_FAILED;
    }
    g_dirty = 1;
}

/* ---- the view ------------------------------------------------------------- */

void wifiapp_open(void)
{
    extern int wifi_joined(void);

    g_sel      = -1;
    g_dirty    = 1;
    g_was_down = 0;

    /* Open showing what is true right now rather than a fixed starting state:
     * this view is entered both before and after the radio exists, and "tap a
     * network to join" on a board with no radio is the same kind of lie the
     * status enum was just split to stop telling. */
    if (!blob_ready())      { g_state = ST_NORADIO; }
    else if (wifi_joined()) { g_state = ST_JOINED;  }
    else                    { g_state = ST_IDLE;    }
    /* Results are kept across opens: a scan costs five seconds of radio time
     * and the list is very likely still true. */
}

void wifiapp_frame(void)
{
    if (g_scan_ch) { scan_step(); }
    if (!g_dirty)  { return; }
    g_dirty = 0;
    draw_all();
}

void wifiapp_touch(uint32_t x, uint32_t y, int down)
{
    if (!down) { g_was_down = 0; return; }
    if (g_was_down) { return; }         /* one press, one action */
    g_was_down = 1;

    audio_click();

    /* The one button: start when the radio is down, scan when it is up. */
    if (y >= STAT_Y && x >= DISP_W - 52u) {
        if (!blob_ready()) {
            start_radio();
        } else {
            g_count   = 0u;
            g_sel     = -1;
            g_scan_ch = 1u;
            g_state   = ST_SCANNING;
            g_dirty   = 1;
        }
        return;
    }

    if (y < LIST_Y || y >= STAT_Y) { return; }

    uint32_t i = (y - LIST_Y) / ROW_H;
    if (i >= g_count || i >= MAX_ROWS) { return; }

    /* First tap selects, second joins. The same discipline the launcher uses
     * (UM-NATOS-021): a confident user double-taps, an unsure one taps once and
     * sees what they picked. Joining takes fifteen seconds and is not something
     * to start by brushing the glass. */
    if (g_sel == (int)i) {
        join(i);
    } else {
        g_sel   = (int)i;
        g_dirty = 1;
    }
}
