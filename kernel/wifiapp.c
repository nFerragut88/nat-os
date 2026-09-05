/* nat-os — the wifi view. See wifiapp.h for scope and for why the passphrase
 * is the compiled-in one. */

#include "wifiapp.h"
#include "display.h"
#include "desktop.h"
#include "blob.h"
#include "timer.h"
#include "wifi_secrets.h"
#include "keyboard.h"
#include "wificred.h"

/* The bound DHCP address, or 0. Declared here rather than in each block that
 * wants it: draw_status() reads it twice, in different scopes. */
extern uint32_t netif_wifi_ip(void);

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
/* [step 285] SPEC_Y, not DESK_H. The view took the application band when it
 * grew a keyboard, and having taken it, the list may as well use it: eleven
 * rows instead of eight, on a screen where the whole point is choosing from a
 * list. */
#define VIEW_H    SPEC_Y
#define STAT_Y    (VIEW_H - STAT_H)
#define LIST_H    (STAT_Y - LIST_Y)
#define MAX_ROWS  (LIST_H / ROW_H)
#define MAX_APS   12u

_Static_assert(MAX_ROWS >= 5u, "a network list under five rows is not a list");
_Static_assert(STAT_Y + STAT_H == VIEW_H, "status bar must meet the region end");

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
/* [step 303] A SEQUENCE, not a flag, and volatile.
 *
 * This is written by the net task -- scan_step(), join(), start_radio() -- and
 * read by the display task, and the read used to be:
 *
 *     if (!g_dirty) { return; }
 *     g_dirty = 0;                 <- a set landing here is LOST
 *     draw_all();
 *
 * A sweep result arriving in that window was never drawn, and nothing set the
 * flag again, so the view stayed stale until it was re-entered -- which is
 * exactly how it was reported: "it only works when I exit out and reopen".
 *
 * A counter cannot lose an update. Producers bump it, the display task records
 * what it last painted, and a bump that happens during a paint is simply seen
 * on the next frame. */
static volatile uint32_t g_dirty;
static uint32_t          g_drawn;
static int       g_was_down;

/* The sweep is spread across frames: one channel per frame, so the view stays
 * responsive and the progress is visible. 0 means idle. */
static uint32_t  g_scan_ch;
static uint32_t  g_swept;       /* channels visited in the sweep just run */
static int       g_retried;    /* [step 305] one automatic re-sweep, no more */

/* [step 289] The row to paint white for one flash, and when it started. This
 * replaces the click: audio said "the press landed" without saying WHERE, and
 * on a list the where is the whole message. */
static int       g_flash_row = -1;
static uint32_t  g_flash_tick;

/* When the in-flight request started. A request that never finishes must not
 * silently disable the view -- see wifiapp_frame(). */
static uint32_t  g_req_tick;

/* [step 293] The driver's disconnect count AS IT WAS when we joined.
 *
 * g_wpa_disc_cb counts every disconnect since boot, so testing it against zero
 * would report a link as lost because of a drop that happened before this join
 * and was already recovered from. The question is not "has this board ever
 * disconnected" but "has it disconnected since it connected THIS time". */
static uint32_t  g_disc_at_join;

/* [step 281] Set by the touch handler, consumed by wifiapp_service() on the net
 * task. See wifiapp_service() for why the bring-up must not run on the task
 * that reads the glass. */
static int       g_want_start;

/* [step 284] The selected row a join was asked for, or -1. Same reason as
 * g_want_start: join() blocks for the ~15 s of PBKDF2 and enters the blob, and
 * neither may happen on the task that reads the glass. */
static int       g_want_join = -1;

/* [step 302] The next paint must clear the whole view. Set on entering, and
 * nowhere else: a repaint that blanks the screen first is a flash, not an
 * update, and scan_step() marks the view dirty once per channel -- thirteen
 * full blank-and-repaint cycles per sweep. */
static int       g_full = 1;

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
       ST_SCANNING, ST_DERIVING, ST_JOINING, ST_JOINED, ST_FAILED,
       ST_ASKPASS };  /* [step 285] typing a passphrase for g_ask */
static int      g_state;
static char     g_joined[33];

/* The row a passphrase is being typed for, and the passphrase once it is. */
static int      g_ask = -1;
static char     g_pass[WIFICRED_PASS_MAX];

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
    int      fl = ((int)i == g_flash_row);
    uint16_t bg = fl ? COLOR_WHITE : (on ? COLOR_BLUE : BG);
    uint16_t fg = fl ? COLOR_BLACK : FG;
    uint16_t dm = fl ? COLOR_BLACK : DIM;

    display_fill_rect(0, y, DISP_W, ROW_H - 1u, bg);
    put(4u, y + 6u, g_aps[i].ssid, fg, bg);

    /* [step 295] A green dot for a network whose passphrase is already saved.
     *
     * The user tapped a known network, was not asked for a password, and read
     * that as the feature failing -- when it was the feature working: ask once,
     * remember forever. There was nothing on screen that distinguished "I know
     * this one" from "I did not respond".
     *
     * Reads the primed RAM cache, never flash, so this is safe on a draw path
     * (step 291 explains at length why that distinction matters here). */
    if (wificred_has(g_aps[i].ssid)) {
        display_fill_rect(DISP_W - 70u, y + 7u, 6u, 6u, fl ? COLOR_BLACK : OK);
    }
    put(DISP_W - 58u, y + 6u, strength(g_aps[i].rssi), dm, bg);
    put(DISP_W - 26u, y + 6u, g_aps[i].auth ? "wpa" : "open", dm, bg);
}

/* Two decimal digits into a caller's buffer, returning where it stopped.
 * There is no snprintf here and this view should not be the thing that adds
 * one. */
static uint32_t num(char *b, uint32_t at, uint32_t v)
{
    if (v >= 10u) { b[at++] = (char)('0' + (v / 10u) % 10u); }
    b[at++] = (char)('0' + v % 10u);
    return at;
}

static void draw_status(void)
{
    display_fill_rect(0, STAT_Y, DISP_W, STAT_H, BG);

    /* [step 281] The scan says where it has got to and what it has found.
     *
     * The serial link to this board drops often enough that a run cannot be
     * relied on to be observed, so a scan that finds nothing must be legible
     * FROM THE GLASS: "scanning ch 7 -- 0 found" and "no networks -- tap scan"
     * are different reports, and the first one distinguishes a sweep that is
     * running and finding nothing from one that never started. */
    static char scanmsg[28];
    if (g_state == ST_SCANNING) {
        uint32_t at = 0u;
        const char *p = "scanning ch ";
        while (*p) { scanmsg[at++] = *p++; }
        at = num(scanmsg, at, g_scan_ch);
        p = " -- ";
        while (*p) { scanmsg[at++] = *p++; }
        at = num(scanmsg, at, g_count);
        p = " found";
        while (*p) { scanmsg[at++] = *p++; }
        scanmsg[at] = 0;
    }

    /* [step 283] JOINED shows the address, because "joined" and "on the
     * network" are not the same claim and this view had been making the
     * stronger one. An association with no DHCP lease is exactly the state the
     * board is in today, and it looked identical to success. */
    /* [step 293] A dropped link must stop reading as a connection.
     *
     * netif_wifi_ip() returns lwIP's address, and lwIP keeps it when the radio
     * goes away -- so the view reported an address that had stopped working,
     * in green, which is the strongest claim it can make. The driver's own
     * disconnect callback is the truth here. */
    static char joinmsg[40];
    if (g_state == ST_JOINED) {
        extern uint32_t g_wpa_disc_cb;
        int lost = (g_wpa_disc_cb != g_disc_at_join);
        uint32_t ip = lost ? 0u : netif_wifi_ip();
        uint32_t at = 0u;
        for (uint32_t k = 0u; k < 20u && g_joined[k]; k++) { joinmsg[at++] = g_joined[k]; }
        joinmsg[at++] = ' ';
        if (ip) {
            for (uint32_t b = 0u; b < 4u; b++) {
                uint32_t v = (ip >> (8u * b)) & 0xFFu;
                if (v >= 100u) { joinmsg[at++] = (char)('0' + v / 100u); }
                if (v >= 10u)  { joinmsg[at++] = (char)('0' + (v / 10u) % 10u); }
                joinmsg[at++] = (char)('0' + v % 10u);
                if (b < 3u) { joinmsg[at++] = '.'; }
            }
        } else {
            const char *p2 = lost ? "-- link lost" : "-- no IP";
            while (*p2) { joinmsg[at++] = *p2++; }
        }
        joinmsg[at] = 0;
    }

    const char *msg;
    uint16_t    col;
    switch (g_state) {
    case ST_NORADIO:  msg = "radio off -- tap start";   col = DIM;  break;
    case ST_STARTING: msg = "starting radio -- 30 s";   col = BUSY; break;
    case ST_NOSTART:  msg = "radio did not start";      col = BAD;  break;
    case ST_SCANNING: msg = scanmsg;                    col = BUSY; break;
    case ST_DERIVING: msg = "deriving key -- 15 s";     col = BUSY; break;
    case ST_JOINING:  msg = "joining";                  col = BUSY; break;
    case ST_JOINED:   msg = joinmsg;
                      col = netif_wifi_ip() ? OK : BUSY;                break;
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
    /* [step 286] 28 px tall, was 16. A 16-pixel target at the very bottom of a
     * resistive panel is where the calibration is worst and a fingertip is
     * widest; the hit test below has always accepted the whole strip, so the
     * button now looks like the area it actually occupies. */
    display_fill_rect(DISP_W - 60u, STAT_Y + 3u, 56u, 28u, COLOR_BLUE);
    put(DISP_W - 50u, STAT_Y + 14u, blob_ready() ? "scan" : "start", FG,
        COLOR_BLUE);

    /* [step 296] `forget`, and ONLY when a selected network has something to
     * forget. A button that is always present but usually does nothing teaches
     * people to ignore it; one that appears exactly when it applies explains
     * itself by appearing.
     *
     * It is how the password screen is reached for a network already known --
     * to correct a wrong passphrase, or to hand the board to a different
     * network with the same name. Without it the store could be taught and
     * never untaught. */
    if (g_sel >= 0 && (uint32_t)g_sel < g_count &&
        wificred_has(g_aps[g_sel].ssid)) {
        display_fill_rect(DISP_W - 124u, STAT_Y + 3u, 60u, 28u, COLOR_RED);
        put(DISP_W - 118u, STAT_Y + 14u, "forget", FG, COLOR_RED);
    }
}

static void draw_all(void)
{
    if (g_full) { display_fill_rect(0, 0, DISP_W, VIEW_H, BG); g_full = 0; }
    display_fill_rect(0, 0, DISP_W, HDR_H, COLOR_BLUE);
    put(6u, 8u, "wifi", FG, COLOR_BLUE);

    /* [step 281] The way out, DRAWN.
     *
     * desktop_chrome_touch() has always accepted the top-right 22x22 corner as
     * "leave this view", and it is checked before anything else -- so the exit
     * worked the whole time. This header painted over it, which made a working
     * button invisible and left the view feeling like a trap.
     *
     * Step 277 recorded nearly trapping the user in the shell by removing that
     * handler. This is the same trap reached from the other side: the handler
     * was left alone and the pixels were taken instead. Worth stating plainly,
     * because "the button still works" is not a defence when nobody can see it. */
    display_fill_rect(DISP_W - 22u, 0, 22u, HDR_H, COLOR_RED);
    put(DISP_W - 14u, 8u, "x", FG, COLOR_RED);

    /* [step 286] The empty list says WHY it is empty.
     *
     * It used to read "no networks -- tap scan" in every empty case, including
     * the one where the radio had never been switched on -- so the reason lived
     * in the status bar and the symptom lived here, and reading one without the
     * other gave the wrong answer. Reported three times as "not seeing any
     * networks", each time from a different cause, and I asked twice for the
     * status bar instead of putting the two together. An interface that needs
     * you to cross-reference two places to understand one fact is the bug. */
    /* Only the list, not the screen. Rows and the empty-state text share this
     * region and each must be able to replace the other. */
    display_fill_rect(0, LIST_Y, DISP_W, STAT_Y - LIST_Y, BG);

    if (g_count == 0u) {
        const char *why;
        if (!blob_ready())              { why = "radio off -- tap start"; }
        else if (g_state == ST_SCANNING){ why = "scanning..."; }
        else if (g_state == ST_NOSTART) { why = "radio did not start"; }
        else if (g_state == ST_JOINED)  { why = "connected -- scan for others"; }
        else                            { why = "none found -- tap scan"; }
        put(4u, LIST_Y + 6u, why, DIM, BG);

        /* [step 288] What the last sweep actually DID. "Inconsistent" is a
         * rate, and a rate needs numbers: thirteen channels visited with eleven
         * refused is a driver that would not look; thirteen with none refused
         * is an empty band. Both used to print the same four words. */
        if (g_swept) {
            extern uint32_t g_scan_refused;
            static char sm[32];
            uint32_t at = 0u;
            const char *q;
            at = num(sm, at, g_swept);
            q = " ch, "; while (*q) { sm[at++] = *q++; }
            at = num(sm, at, g_scan_refused);
            q = " refused"; while (*q) { sm[at++] = *q++; }
            sm[at] = 0;
            put(4u, LIST_Y + 20u, sm, DIM, BG);
        }
    }
    for (uint32_t i = 0u; i < g_count && i < MAX_ROWS; i++) {
        draw_row(i);
    }
    draw_status();
}

/* The passphrase screen: which network, what has been typed, and the keyboard.
 *
 * The passphrase is shown in the clear rather than masked. On a device held in
 * one hand there is no shoulder to look over that is not already looking at the
 * screen, and multi-tap entry without seeing the result is close to impossible
 * -- the whole cycling mechanism depends on watching the letter change. Masking
 * would trade a real usability cost for a theoretical secrecy gain on a device
 * that stores the same string in plaintext flash anyway (wificred.h). */
static void draw_ask(void)
{
    display_fill_rect(0, 0, DISP_W, KB_TOP, BG);
    display_fill_rect(0, 0, DISP_W, HDR_H, COLOR_BLUE);
    put(6u, 8u, "password", FG, COLOR_BLUE);
    display_fill_rect(DISP_W - 22u, 0, 22u, HDR_H, COLOR_RED);
    put(DISP_W - 14u, 8u, "x", FG, COLOR_RED);

    if (g_ask >= 0 && (uint32_t)g_ask < g_count) {
        put(4u, HDR_H + 8u, g_aps[g_ask].ssid, DIM, BG);
    }

    /* The field, drawn as a field: an empty line and a typed line look
     * different, which is what tells you the keyboard is connected to
     * something. */
    display_fill_rect(4u, HDR_H + 26u, DISP_W - 8u, 14u, 0x2104u);
    const char *t = keyboard_text();
    put(6u, HDR_H + 29u, t[0] ? t : "type it, then join", t[0] ? FG : DIM, 0x2104u);

    keyboard_draw();
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
        g_dirty++;
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

    g_swept++;
    if (++g_scan_ch > 13u || g_count >= MAX_APS) {
        /* [step 305] One automatic retry when the driver refused channels and
         * nothing was found.
         *
         * "It does not work the first time" and works the second is a fact
         * about the radio, not about the user: a scan issued while the driver
         * is busy -- associating, or parked on the access point -- is REFUSED
         * (288), and the refusals themselves disturb it enough that the next
         * sweep goes through. Making the user perform that second sweep by hand
         * is asking them to work around a retry the code should be doing.
         *
         * Bounded to one. A second failure is a real answer and gets reported
         * with its numbers rather than looped over. */
        extern uint32_t g_scan_refused;
        if (g_count == 0u && g_scan_refused && !g_retried) {
            g_retried      = 1;
            g_swept        = 0u;
            g_scan_refused = 0u;
            g_scan_ch      = 1u;
            g_dirty++;
            return;
        }
        g_scan_ch = 0u;
        /* [step 286] Not unconditionally ST_IDLE. The bring-up joins and sets
         * ST_JOINED, and then starts this sweep -- so ending it with ST_IDLE
         * erased a successful join from the display a few seconds after it
         * appeared. The scan finishing says nothing about whether we are
         * associated, and it was overwriting the one thing that does. */
        if (g_state == ST_SCANNING) { g_state = ST_IDLE; }
    }
    g_dirty++;
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
 * Copying them rather than re-deriving them is deliberate: this view must not
 * become a second opinion about how to start the radio. */
static void start_radio(void)
{
    extern void     wifi_start_enable(int on);
    extern uint32_t wifi_bringup(const struct blob_entry *e, int want_null);
    extern void     wifi_rsn_ie_enable(int on);
    extern void     wifi_rsn_akm_set(unsigned int t);
    extern void     wifi_appie_shape(unsigned int t, unsigned int f);
    extern void     wifi_hs_passive(int on);
    extern int      phyinit_run_at(uint32_t fn);
    extern int      wifi_joined(void);

    g_state = ST_STARTING;      /* held for the duration; gates scan_step() */
    g_dirty++;

    /* blob_map() is safe from here even though this file lives in irom. It
     * disables the cache to rewrite the MMU, but it is itself IRAM-resident and
     * turns the cache back on before returning -- blob.c says so in as many
     * words, and shell.c has called it from flash since step 190. */
    const struct blob_entry *e = blob_map();
    if (!e || blob_init(e) != 0) {
        g_state = ST_NOSTART;
        g_dirty++;
        return;
    }
    (void)phyinit_run_at(e->phy_init);

    /* [step 291] wifi_start_enable() enables the blob's task as a side effect,
     * and that is deliberate -- step 214: "Starting the driver REQUIRES its
     * task, so enabling one enables the other."
     *
     * This used to read blob_task_enable(0) on the line above, copied from the
     * shell's pre-214 arrangement along with everything else in this block. It
     * was DEAD -- overwritten one line later -- and the comment beside it
     * claimed blob task creation still panics, which stopped being true at step
     * 214. A no-op line asking for something that would break the radio, next to
     * a comment stating the opposite of the current design: exactly the kind of
     * thing copying a settled block without re-reading it produces. */
    wifi_start_enable(1);
    wifi_rsn_ie_enable(1);
    wifi_rsn_akm_set(2u);
    wifi_appie_shape(4u, 0u);
    wifi_hs_passive(0);
    {   /* [step 304] This view sweeps on entry and shows the result, so the
         * driver's own five-second sweep would gather evidence the user is
         * about to gather again. */
        extern void wifi_bringup_quick(int on);
        wifi_bringup_quick(1);
    }

    (void)wifi_bringup(e, 0);

    if (!blob_ready()) {
        g_state = ST_NOSTART;
        g_dirty++;
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
        { extern uint32_t g_wpa_disc_cb; g_disc_at_join = g_wpa_disc_cb; }
        g_state = ST_JOINED;
    } else {
        g_state = ST_IDLE;
    }

    /* [step 293] Sweep ONLY if we did not just join.
     *
     * This used to sweep unconditionally, "so what appears next is a set of
     * networks to choose from". A station has ONE radio: a passive sweep retunes
     * it away from the access point for SWEEP_DWELL (400 ms) per channel, five
     * seconds across the band -- and it was being run immediately after
     * associating and binding an address.
     *
     * The board showed a green "ivory-billed 192.168.1.140" and answered
     * nothing: no ping, no ARP entry, no HTTP. The address was real when it was
     * printed and the link was gone by the time anyone used it.
     *
     * Scanning while connected is a genuine trade, not a bug, and it stays
     * available on the scan button. Doing it to a user who did not ask, seconds
     * after connecting them, is not a trade -- it is just losing the link. */
    g_count = 0u;
    g_sel   = -1;
    if (g_state != ST_JOINED) { g_scan_ch = 1u; }
    g_dirty++;
}

/* Join the selected network with the compiled-in passphrase.
 *
 * BLOCKING, and the status bar is painted before it starts. PBKDF2 at 4096
 * iterations measures ~15 s on this part; a view that redrew afterwards would
 * look like a hang for the whole of it. */
static void join(uint32_t i)
{
    extern void wifi_join_ssid_pass(const char *ssid, const char *pass);
    extern int  wifi_joined(void);

    if (i >= g_count) { return; }
    if (!blob_ready()) { g_state = ST_NORADIO; g_dirty++; return; }

    g_state = ST_DERIVING;      /* the display task is already painting this */

    wifi_join_ssid_pass(g_aps[i].ssid, g_pass[0] ? g_pass : 0);

    if (wifi_joined()) {
        uint32_t k = 0u;
        for (; k < 32u && g_aps[i].ssid[k]; k++) { g_joined[k] = g_aps[i].ssid[k]; }
        g_joined[k] = 0;
        { extern uint32_t g_wpa_disc_cb; g_disc_at_join = g_wpa_disc_cb; }
        g_state = ST_JOINED;
    } else {
        g_state = ST_FAILED;
    }
    g_dirty++;
}

/* Run a requested bring-up. Called from the NET TASK, not the touch task.
 *
 * [step 281] The first version called start_radio() straight out of
 * wifiapp_touch(), which runs on the touch task -- so for the ninety seconds of
 * bring-up the task that reads the glass was sitting inside wifi_bringup(). The
 * status bar said "starting radio -- 90 s" while the exit button, the scan
 * button and every other press went unread. Reported as "the x button isn't
 * working", and it was not: nothing was listening.
 *
 * A status line that asks the user to wait, displayed by a system that has
 * stopped accepting input, is worse than no status line -- it invites exactly
 * the presses it cannot answer.
 *
 * The net task is the right place: it has nothing to service until the radio
 * exists, its stack frame is shallow where the shell's is deep (the shell runs
 * this same sequence with 236 bytes to spare, per UM-NATOS-052 7), and blocking
 * it blocks nothing a user can see. The display task keeps painting and the
 * touch task keeps listening, so the view stays alive and answerable throughout
 * -- including the way out.
 *
 * Leaving the view mid-bring-up is therefore allowed, and the bring-up
 * continues without it. That is deliberate: the radio is the system's, not this
 * view's, and ninety seconds of work should not be thrown away by a back
 * button. */
void wifiapp_service(void)
{
    if (g_want_start) {
        g_want_start = 0;
        start_radio();
        return;
    }
    if (g_want_join >= 0) {
        uint32_t i = (uint32_t)g_want_join;
        g_want_join = -1;
        join(i);
        return;
    }

    /* [step 288] The SWEEP runs here too, not on the display task.
     *
     * Two reasons, and the second is the one that matters. It blocks for
     * SWEEP_DWELL -- 400 ms -- per channel, so a thirteen-channel sweep froze
     * the display for over five seconds. And it was the last place the DISPLAY
     * task entered the blob, which is the hazard step 281c guarded around
     * rather than removed. With this moved, exactly ONE task enters the blob,
     * which is what blobcall.c has assumed all along:
     *
     *     "Today there is exactly one caller, so this should stay zero."
     *
     * A guard that keeps two callers apart is worth less than not having two. */
    if (g_scan_ch) { scan_step(); }
}

/* ---- the view ------------------------------------------------------------- */

void wifiapp_open(void)
{
    extern int wifi_joined(void);

    g_sel      = -1;
    g_dirty++;

    /* [step 305] The finger that opened this view IS STILL DOWN.
     *
     * The launcher opens on a press, and the touch task keeps reporting that
     * same press to whoever owns the screen next -- which is now this view.
     * Clearing g_was_down here armed the handler for a press the user had
     * already spent, so the first thing they did after entering was acted on at
     * the coordinates of the ICON they tapped, and their next real tap was the
     * one that appeared to be first.
     *
     * Setting it swallows the held press. It clears on release, which is the
     * event that actually means "the user is done with that tap". */
    g_was_down = 1;

    /* Open showing what is true right now rather than a fixed starting state:
     * this view is entered both before and after the radio exists, and "tap a
     * network to join" on a board with no radio is the same kind of lie the
     * status enum was just split to stop telling. */
    g_ask = -1;
    if (!blob_ready())      { g_state = ST_NORADIO; }
    else if (wifi_joined()) { g_state = ST_JOINED;  }
    else                    { g_state = ST_IDLE;    }
    g_full = 1;

    /* [step 302] Sweep on entry when there is a radio, nothing is connected and
     * the list is empty.
     *
     * Reported as having to leave the view, come back and tap scan before
     * anything could be joined. That WAS the design: start_radio() clears the
     * list and 293 stopped it sweeping afterwards, so entering showed nothing
     * until the user asked. Asking was a step with no decision in it -- there
     * is nothing else to do with an empty list.
     *
     * NOT when already joined. Scanning retunes the radio off the access point
     * (293) and would drop the connection the user came here to keep. The scan
     * button stays, for choosing a different network on purpose.
     *
     * Results are otherwise kept across opens: a sweep costs five seconds of
     * radio time and the list is very likely still true. */
    if (blob_ready() && !wifi_joined() && g_count == 0u && !g_scan_ch) {
        extern uint32_t g_scan_refused;
        g_sel          = -1;
        g_swept        = 0u;
        g_scan_refused = 0u;
        g_retried      = 0;
        g_scan_ch      = 1u;
        g_state        = ST_SCANNING;
    }
}

void wifiapp_frame(void)
{
    /* [step 289] The flash is a few frames of white, then gone. */
    if (g_flash_row >= 0 && (timer_ticks() - g_flash_tick) > 8u) {
        g_flash_row = -1;
        g_dirty++;
    }

    /* [step 289] A request that never finishes must not disable the view.
     *
     * Every press began with "if STARTING or DERIVING, return" -- so if a join
     * or a bring-up did not complete, the view ignored every press from then
     * on, forever, while still clicking to say the press had landed. Reported
     * as "when I tap a network nothing happens", which is exactly what it did.
     *
     * A guard against a second request became a guard against ever using the
     * view again. It expires now and says so: 120 s is well past the 15 s a key
     * derivation takes and the ~90 s of a bring-up, so anything still pending
     * then is not going to finish. */
    if ((g_state == ST_DERIVING || g_state == ST_STARTING) &&
        (timer_ticks() - g_req_tick) > 12000u) {
        g_state = ST_FAILED;
        g_dirty++;
    }

    /* [step 283] DHCP binds seconds after the join, so the address arrives
     * after the paint that reported the join. Watch for it changing. */
    if (g_state == ST_JOINED) {
        static uint32_t shown;
        uint32_t now = netif_wifi_ip();
        if (now != shown) { shown = now; g_dirty++; }
    }

    /* [step 281] Do NOT enter the blob from here while a bring-up is in flight.
     *
     * wifiapp_frame() runs on the DISPLAY task and scan_step() calls into the
     * vendor blob; the bring-up runs on the NET task and does the same. Moving
     * the bring-up off the touch task fixed a dead exit button and created this:
     * two tasks able to be inside windowed vendor code at once, which is the
     * hazard this project has paid for five times.
     *
     * blobcall.c says so in its own words -- "Today there is exactly one caller,
     * so this should stay zero; if it does not, something has started entering
     * the blob from a second context and the assumptions above are worth
     * re-reading." I made the second caller, so the guard belongs here, at the
     * caller I added, rather than as a change to the exclusion everything else
     * depends on.
     *
     * The cost is that the sweep pauses for the duration of a bring-up. The
     * bring-up ends by starting a fresh sweep anyway, so nothing is lost. */
    if (g_state == ST_ASKPASS) {
        if (keyboard_tick()) { g_dirty++; }
        uint32_t seq = g_dirty;
        if (seq == g_drawn) { return; }
        g_drawn = seq;
        draw_ask();
        return;
    }

    uint32_t seq = g_dirty;
    if (seq == g_drawn) { return; }
    g_drawn = seq;
    draw_all();
}

void wifiapp_touch(uint32_t x, uint32_t y, int down)
{
    if (!down) { g_was_down = 0; return; }
    if (g_was_down) { return; }         /* one press, one action */
    g_was_down = 1;

    /* [step 285] The passphrase screen owns the glass while it is up. Note the
     * keyboard gives its own feedback -- the live key is drawn blue for as long
     * as another tap can still change that character -- so nothing here needs
     * to add to it. [step 289] This used to warn against firing audio_click()
     * twice per key; there is no audio in this view any more. */
    if (g_state == ST_ASKPASS) {
        int r = keyboard_touch(x, y);
        if (r == KB_EDIT) { g_dirty++; return; }
        if (r != KB_SUBMIT) { return; }

        /* Save first, then join. A passphrase that turns out to be wrong is
         * still the one the user meant to type, and losing it to a failed join
         * means typing it again on a multi-tap keyboard to find out why. */
        uint32_t k = 0u;
        const char *t = keyboard_text();
        for (; k + 1u < WIFICRED_PASS_MAX && t[k]; k++) { g_pass[k] = t[k]; }
        g_pass[k] = 0;

        if (g_ask >= 0 && (uint32_t)g_ask < g_count && g_pass[0]) {
            (void)wificred_put(g_aps[g_ask].ssid, g_pass);
            g_scan_ch   = 0u;
            g_want_join = g_ask;
            g_state     = ST_DERIVING;
            g_req_tick  = timer_ticks();
        } else {
            g_state = ST_IDLE;          /* nothing typed: back to the list */
        }
        g_ask   = -1;
        g_dirty++;
        return;
    }


    /* [step 296] forget, checked before the scan/start button beside it. */
    if (y >= STAT_Y && x >= DISP_W - 124u && x < DISP_W - 60u &&
        g_sel >= 0 && (uint32_t)g_sel < g_count &&
        wificred_has(g_aps[g_sel].ssid)) {
        (void)wificred_forget(g_aps[g_sel].ssid);
        g_dirty++;
        return;                 /* the dot goes; a double tap now asks */
    }

    /* The one button: start when the radio is down, scan when it is up. */
    if (y >= STAT_Y && x >= DISP_W - 60u) {
        if (g_state == ST_STARTING || g_state == ST_DERIVING) {
            /* Already going. A second press must not queue a second request. */
        } else if (!blob_ready()) {
            g_scan_ch    = 0u;      /* the sweep stops HERE, before the request */
            g_want_start = 1;       /* the net task picks this up */
            g_state      = ST_STARTING;
            g_req_tick   = timer_ticks();
            g_dirty++;
        } else {
            extern uint32_t g_scan_refused;
            g_count        = 0u;
            g_sel          = -1;
            g_swept        = 0u;
            g_scan_refused = 0u;
            g_scan_ch      = 1u;
            g_state        = ST_SCANNING;
            g_dirty++;
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
        if (g_state == ST_STARTING || g_state == ST_DERIVING) { return; }
        g_scan_ch = 0u;             /* the sweep stops HERE, before the request */

        /* [step 285] A network typed once is not typed again. */
        g_pass[0] = 0;
        if (wificred_get(g_aps[i].ssid, g_pass, sizeof g_pass)) {
            g_want_join = (int)i;   /* the net task picks this up */
            g_state     = ST_DERIVING;
            g_req_tick  = timer_ticks();
        } else {
            g_ask   = (int)i;
            g_state = ST_ASKPASS;
            keyboard_reset("join");
        }
        g_dirty++;
    } else {
        g_sel        = (int)i;
        g_flash_row  = (int)i;      /* [step 289] white, where the finger went */
        g_flash_tick = timer_ticks();
        g_dirty++;
    }
}
