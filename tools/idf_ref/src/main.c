/* A reference that is doing NAT-OS'S JOB, not its own.
 *
 * The first version of this was a SoftAP. It proved nat-os's receiver works
 * (UM-NATOS-034 §12) and then produced a register differential that was mostly
 * useless: 342 differences, of which the great majority said only "a fully
 * configured access point and a minimal promiscuous sniffer are configured
 * differently". True, and not a bug. Applying them wholesale hung the board
 * (§14).
 *
 * A difference is not a defect until both sides are doing the same job. So this
 * version matches nat-os as closely as ESP-IDF allows:
 *
 *     promiscuous mode, no association, fixed channel, and a raw 802.11 frame
 *     pushed out by hand every 100 ms
 *
 * which is exactly what nat-os does. What is left in the diff after that should
 * be suspicious rather than merely different.
 *
 * The frame is a PROBE REQUEST because that is what nat-os's `probe` sends, it
 * is broadcast so any receiver logs it, and an access point in range answers it
 * -- which makes "did this actually radiate" checkable from three directions.
 */
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REF_CHANNEL 6

/* The multi-minute register sweep. Off for the link test, which needs this
 * board beaconing steadily rather than busy for five minutes first. */
#define REF_TRACE_SWEEP 0

/* Which half of the two-board link test this build is for.
 *
 *   1 = AP.  Beacons NATOS-CTRL-6 continuously so nat-os's `scan` can list it.
 *            Direction A -- "can nat-os hear this board?"
 *   0 = STA + promiscuous, receive only. Direction B -- "can this board hear
 *            nat-os?"
 *
 * They must be separate builds, and finding that out cost a wrong verdict.
 * With AP mode and promiscuous enabled together, this board heard FOUR frames
 * in forty seconds and could not hear an access point that nat-os hears
 * continuously -- ESP-IDF's promiscuous receive is all but disabled in AP mode.
 * A negative from a deaf receiver is not evidence, and the first run of
 * link_test.py reported one as though it were.
 */
#define REF_LINK_AP 0

/* A probe request with a wildcard SSID and a basic rate set. Hand-built so the
 * bytes on the air are ours rather than the stack's, the same way nat-os builds
 * its own. Addresses are filled in at run time from the chip's own MAC. */
static uint8_t probe_req[] = {
    0x40, 0x00,                             /* frame control: mgmt, probe req */
    0x00, 0x00,                             /* duration                       */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,     /* addr1  destination: broadcast  */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     /* addr2  source: filled below    */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,     /* addr3  bssid: broadcast        */
    0x00, 0x00,                             /* sequence control               */
    0x00, 0x00,                             /* IE 0: SSID, length 0, wildcard */
    0x01, 0x04, 0x82, 0x84, 0x8b, 0x96,     /* IE 1: rates 1, 2, 5.5, 11 Mbps */
};

/* ---- the register dump, for the differential ----------------------------
 *
 * Ranges match tools/serial/reg_diff.py and nat-os's `regdump` exactly, so one
 * host script parses both. Printed twice a second apart, so the host can throw
 * away anything that moves on its own -- the TSF timer and the statistics -- and
 * compare only stable state.
 */
static void dump_range(const char *tag, uint32_t base, uint32_t words)
{
    for (uint32_t i = 0; i < words; i += 8) {
        printf("REG %s %08x", tag, (unsigned)(base + i * 4));
        for (uint32_t j = 0; j < 8 && (i + j) < words; j++) {
            printf(" %08x",
                   (unsigned)(*(volatile uint32_t *)(base + (i + j) * 4)));
        }
        printf("\n");
    }
}

static void dump_all(void)
{
    dump_range("dport", 0x3FF00000u, 64u);
    dump_range("rtc",   0x3FF48000u, 64u);
    dump_range("mac",   0x3FF73000u, 1280u);
    /* The PHY and baseband blocks.
     *
     * Never dumped before, by any instrument in this investigation, which is
     * the gap UM-NATOS-034 §26 is about: §20 bounded the fault to below the MAC
     * and the search then continued everywhere except below the MAC.
     *
     * Extents chosen from the addresses coex_bt_high_prio itself writes --
     * 0x3FF5C080, 0x3FF5D040, 0x3FF710D0, 0x3FF71300 -- widened to cover the
     * blocks they sit in. */
    dump_range("bb0",   0x3FF5C000u, 512u);
    dump_range("bb1",   0x3FF5D000u, 512u);
    dump_range("phy",   0x3FF71000u, 1024u);
    printf("REGEND\n");
}

/* ==== the transmit tracer ==================================================
 *
 * UM-NATOS-034 §13 ended on a specific limitation:
 *
 *     the snapshot diff compares destinations, not routes.
 *
 * Applying all ~30 stable register differences wholesale changed nothing, which
 * eliminated the whole shortlist and left the same question. Two shapes of
 * cause survive a snapshot untouched:
 *
 *   ORDER      -- both firmwares reach the same state by different paths, and
 *                 the hardware cares which.
 *   TRANSIENTS -- a write to a self-clearing bit. A "go" bit reads back zero a
 *                 microsecond later, so it is invisible to any number of
 *                 snapshots taken afterwards, no matter how carefully filtered.
 *
 * The second is the more likely and the more embarrassing: a trigger nat-os
 * never writes would explain every observation in this project -- completions
 * counted, canary intact, receive unaffected, nothing on the air.
 *
 * ---- how it catches them --------------------------------------------------
 *
 * Core 1 does nothing but read a small window of MAC registers into DRAM, as
 * fast as the bus allows, with no comparison and no branching beyond the loop.
 * Core 0 transmits one frame into the middle of that. Afterwards the buffer is
 * differenced offline and printed as an ordered list of changes.
 *
 * Sixteen registers at a time, because resolution is the whole point:
 *
 *     16 words x ~8 cycles = ~128 cycles = ~0.55 us per snapshot at 240 MHz
 *     1024 snapshots        = ~0.7 ms of continuous coverage
 *
 * A whole-block sweep would have been ~11 us per snapshot, which is slower than
 * the events being hunted. The window is swept across the block over successive
 * runs instead.
 *
 * ---- what it cannot do ----------------------------------------------------
 *
 * A write whose value is overwritten within one snapshot period is still lost.
 * The tracer narrows that blind spot by roughly twenty times versus polling the
 * whole block; it does not close it.
 *
 * It is worth stating why that is acceptable. The same tracer will run on
 * nat-os, and what is being compared is two traces, not a trace against the
 * truth. A blind spot present in both instruments cannot manufacture a
 * difference -- it can only hide one. This is the same argument the two-board
 * receiver differential rests on, and it is the reason that one settled a
 * question three months of single-board tests could not.
 */
#define TRACE_WORDS  16u            /* 64 bytes per window                    */
#define TRACE_SNAPS  2048u          /* ~3.9 ms of coverage -- see below       */

/* MEASURED, not estimated. The comment above originally predicted ~0.55 us per
 * sample from 8 cycles per word. The first run reported 1.90 us: a read from
 * the MAC peripheral bus costs about 28 cycles, not 8.
 *
 * That matters more than a wrong guess usually would. At 1.9 us the tracer is
 * still roughly six times finer than sweeping the whole block would have been,
 * so the design holds -- but the blind spot is three times wider than claimed,
 * and anything faster than ~2 us can still be missed. Recorded here rather than
 * quietly corrected, because the width of this instrument's blind spot is the
 * one number a reader needs to judge a negative result from it.
 */

static uint32_t g_trace[TRACE_SNAPS][TRACE_WORDS];
static volatile uint32_t g_trace_base = 0x3FF73000u;
static volatile int      g_trace_arm;
static volatile int      g_trace_done;
static uint32_t          g_trace_t0, g_trace_t1;

static inline uint32_t ccount(void)
{
    uint32_t c;
    __asm__ __volatile__("rsr.ccount %0" : "=r"(c));
    return c;
}

/* IRAM, and no function calls inside the loop. An instruction-cache miss in the
 * middle of the capture would be a gap in the trace that looks exactly like the
 * hardware going quiet. */
static IRAM_ATTR void trace_capture(void)
{
    volatile uint32_t *base = (volatile uint32_t *)g_trace_base;

    g_trace_t0 = ccount();
    for (uint32_t s = 0; s < TRACE_SNAPS; s++) {
        for (uint32_t w = 0; w < TRACE_WORDS; w++) {
            g_trace[s][w] = base[w];
        }
    }
    g_trace_t1 = ccount();
}

static void trace_task(void *arg)
{
    (void)arg;
    for (;;) {
        while (!g_trace_arm) {
            vTaskDelay(1);
        }
        trace_capture();
        g_trace_arm  = 0;
        g_trace_done = 1;
    }
}

/* Prints only what changed, which is what makes the output readable at all:
 * 16,384 samples collapse to the handful of words that actually moved.
 *
 * Format is fixed for tools/serial/tx_trace.py:
 *     TRACE base=<hex> words=<n> snaps=<n> cycles=<n>
 *     T <snapshot> <addr> <old> <new>
 *     TRACEEND changes=<n>
 */
static void trace_dump(int do_tx)
{
    printf("TRACE base=%08x words=%u snaps=%u cycles=%u mode=%s\n",
           (unsigned)g_trace_base, (unsigned)TRACE_WORDS,
           (unsigned)TRACE_SNAPS, (unsigned)(g_trace_t1 - g_trace_t0),
           do_tx ? "tx" : "idle");

    uint32_t changes = 0;
    for (uint32_t s = 1; s < TRACE_SNAPS; s++) {
        for (uint32_t w = 0; w < TRACE_WORDS; w++) {
            if (g_trace[s][w] != g_trace[s - 1][w]) {
                printf("T %u %08x %08x %08x\n", (unsigned)s,
                       (unsigned)(g_trace_base + w * 4),
                       (unsigned)g_trace[s - 1][w], (unsigned)g_trace[s][w]);
                changes++;
            }
        }
    }
    printf("TRACEEND changes=%u\n", (unsigned)changes);
}

static void spin_us(uint32_t us)
{
    uint32_t start = ccount();
    while ((ccount() - start) < us * 240u) {
    }
}

/* A traced burst.
 *
 * The first version sent ONE frame and the second pass of the sweep came back
 * empty at the only address that had shown anything. That is not noise, it is
 * the design being wrong: esp_wifi_80211_tx() hands the frame to the WiFi task
 * and returns. When the hardware is actually touched is decided by a scheduler
 * this code does not control, so a single frame lands inside a 2 ms window only
 * when it happens to.
 *
 * Eight frames spread across the window instead. The point is not to transmit
 * more; it is that the probability of the window being empty for a reason that
 * has nothing to do with the registers drops from "often" to "rarely".
 *
 * This also changes what a silent window MEANS, which is the whole value of it.
 * With one frame, silence was ambiguous. With eight, silence at an address is
 * evidence the transmit path does not touch it.
 */
static void traced_tx(uint32_t base, int do_tx)
{
    g_trace_base = base;
    g_trace_done = 0;
    g_trace_arm  = 1;

    /* Spin, not vTaskDelay: at these timescales a tick is four orders of
     * magnitude too coarse. ~30 us is enough for core 1 to be in the loop. */
    spin_us(30);

    for (int i = 0; i < 8; i++) {
        if (do_tx) {
            esp_wifi_80211_tx(WIFI_IF_STA, probe_req, sizeof probe_req, true);
        }
        spin_us(400);
    }

    while (!g_trace_done) {
    }
    trace_dump(do_tx);
}

/* ==== link test: does each board hear the other? ==========================
 *
 * UM-NATOS-034 §5 established that a second board 30 cm away hears a real
 * access point but never hears nat-os transmitting, and every conclusion since
 * — including §20's — rests on it. The receiver in that test was validated
 * against an access point. **It has never been pointed at this board.**
 *
 * That leaves an assumption load-bearing and unmeasured: that a receiver which
 * hears an AP would also hear a nearby ESP32. If it would not, §5 is a
 * statement about the receiver rather than the transmitter, and a great deal of
 * the record needs revisiting.
 *
 * So: two directions, both measured.
 *
 *   A. this board -> nat-os. AP mode with an unmistakable SSID, beaconing
 *      continuously. nat-os's `scan` lists beacon sources by BSSID and SSID,
 *      so no change is needed on that side, and the result cannot be a
 *      counter artefact — either the SSID string appears or it does not.
 *
 *   B. nat-os -> this board. The tally below.
 *
 * ---- why a tally and not a frame count -----------------------------------
 *
 * `tools/serial/wifi_sweep.py` once keyed its detector on a frame-count delta
 * and reported a hit at every step, because a real access point delivers
 * ~1.5 frames/s and the counter always moves. The lesson was written down:
 *
 *     key on the transmitter's source MAC, never on frame volume.
 *
 * This keeps a per-source-MAC count. nat-os's address either appears in it or
 * it does not, and no amount of ambient traffic can put it there.
 */
#define REF_SSID "NATOS-CTRL-6"

#define SRC_MAX 24
static uint8_t  g_src[SRC_MAX][6];
static uint32_t g_src_n[SRC_MAX];
static uint32_t g_src_count;
static uint32_t g_frames;
static uint32_t g_bad_fcs;

static void sniff(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    if (!p || p->rx_ctrl.sig_len < 16) {
        return;
    }
    g_frames++;
    /* rx_state != 0 means the hardware flagged a problem with this frame --
     * most usefully an FCS failure. Counted separately, because "malformed
     * transmission" and "no transmission at all" are the two possibilities
     * UM-NATOS-034 exists to tell apart, and until the FCSFAIL filter below was
     * opened this receiver could not see the difference. */
    if (p->rx_ctrl.rx_state != 0) {
        g_bad_fcs++;
    }

    /* addr2 -- the transmitter. Bytes 10..15 of an 802.11 header. */
    const uint8_t *a2 = p->payload + 10;

    for (uint32_t i = 0; i < g_src_count; i++) {
        if (memcmp(g_src[i], a2, 6) == 0) {
            g_src_n[i]++;
            return;
        }
    }
    if (g_src_count < SRC_MAX) {
        memcpy(g_src[g_src_count], a2, 6);
        g_src_n[g_src_count] = 1;
        g_src_count++;
    }
}

static void link_report(void)
{
    printf("LINK frames=%u sources=%u badfcs=%u\n",
           (unsigned)g_frames, (unsigned)g_src_count, (unsigned)g_bad_fcs);
    for (uint32_t i = 0; i < g_src_count; i++) {
        printf("LINKSRC %02x:%02x:%02x:%02x:%02x:%02x %u\n",
               g_src[i][0], g_src[i][1], g_src[i][2],
               g_src[i][3], g_src[i][4], g_src[i][5], (unsigned)g_src_n[i]);
    }
    printf("LINKEND\n");
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* AP mode, for the link test.
     *
     * nat-os's `scan` tracks BEACON sources only -- it lists BSSIDs and SSIDs.
     * A probe request from this board would therefore be invisible to it no
     * matter how well the receiver works, so direction A would fail for a
     * reason that has nothing to do with radios.
     *
     * An AP beacons ~10x a second with a name in every frame. The result is a
     * string appearing on nat-os's console or not appearing, which is about as
     * hard to misread as evidence gets. Raw injection still works alongside it
     * for direction B. */
#if REF_LINK_AP
    wifi_config_t ap = { 0 };
    memcpy(ap.ap.ssid, REF_SSID, sizeof(REF_SSID));
    ap.ap.ssid_len = sizeof(REF_SSID) - 1;
    ap.ap.channel  = REF_CHANNEL;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 1;
    ap.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
#else
    /* STA, never associated -- the configuration in which this board's
     * promiscuous receive actually works. See REF_LINK_AP. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
#endif

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(sniff));

    /* Accept EVERYTHING, including frames that fail their FCS.
     *
     * WIFI_PROMIS_FILTER_MASK_FCSFAIL is documented "do not open it in
     * general", and it is off by default -- so until this line existed, this
     * receiver silently discarded every frame that arrived corrupt.
     *
     * That is not a detail. UM-NATOS-034 §1 states the entire reason the second
     * receiver was built:
     *
     *     An AP that ignores a malformed frame looks exactly like a radio that
     *     never transmitted.
     *
     * A promiscuous receiver with default filtering has that same blindness,
     * and §21 used one. "nat-os heard 0 times" could therefore have meant
     * "nat-os radiates nothing" OR "nat-os radiates something malformed" --
     * completely different faults, in completely different places.
     *
     * With this open, the two are distinguishable. */
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    wifi_promiscuous_filter_t cfilt = { .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_ctrl_filter(&cfilt));

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(REF_CHANNEL, WIFI_SECOND_CHAN_NONE));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(REF_LINK_AP ? WIFI_IF_AP : WIFI_IF_STA, mac));
    memcpy(&probe_req[10], mac, 6);          /* addr2: this chip */

    printf("REF-RAW mode=promiscuous channel=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           REF_CHANNEL, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Transmit for a while before dumping: the registers wanted are those of a
     * radio that is actively transmitting, not one that has just been set up. */
    for (int i = 0; i < 30; i++) {
        esp_wifi_80211_tx(WIFI_IF_STA, probe_req, sizeof probe_req, true);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    dump_all();
    /* A SECOND dump, a second later, so the host can discard anything that
     * moves on its own. Without two, reg_diff.py refuses to run -- correctly:
     * an unfiltered diff of a 3,456-register dump is noise wearing a result's
     * clothes. This was lost when the link test replaced the sweep. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    dump_all();

    /* Core 1, pinned. The WiFi stack and its blobs live on core 0, so the
     * tracer is not competing with the thing it is measuring for anything but
     * the peripheral bus. */
    xTaskCreatePinnedToCore(trace_task, "trace", 4096, NULL,
                            configMAX_PRIORITIES - 1, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Sweep the window across the whole MAC register block.
     *
     * The first run swept only 0x3FF73000..0x3FF73400, on the grounds that every
     * register this project has found interesting falls inside it. Fifteen of
     * those sixteen windows were completely silent -- not "only counters moved",
     * but no bit changed in 16,384 samples. Whatever the transmit path touches
     * is mostly somewhere else, and 0x3FF73C40 and 0x3FF73C68 from UM-NATOS-034
     * were outside the swept range entirely.
     *
     * 64 windows, the full 4 KB. Silence is only evidence if the search was
     * wide enough to have found something.
     *
     * Each position gets its own burst, so cross-window ordering is not directly
     * observed -- it is reconstructed from the snapshot index, which assumes the
     * sequence repeats. The second pass tests that assumption rather than
     * trusting it. */
    /* Idle and transmit captures INTERLEAVED per window, not split into passes.
     *
     * UM-NATOS-034 §19 found out the hard way why this matters. The MAC is
     * receiving real ambient traffic throughout -- a nearby access point
     * delivers frames continuously -- and that background produces state
     * changes indistinguishable from transmit ones. §18 had no control at all
     * and misidentified background activity as the transmit signature.
     *
     * A control taken minutes later would be a weaker one, because ambient
     * traffic varies. Back-to-back on the same window is as close to "same
     * conditions, one variable" as this experiment can get.
     *
     * This is the first version of the nat-os/ESP-IDF comparison with a control
     * on BOTH sides. */
#if REF_TRACE_SWEEP
    for (int pass = 0; pass < 2; pass++) {
        printf("TRACEPASS %d\n", pass);
        for (uint32_t w = 0; w < 64; w++) {
            uint32_t base = 0x3FF73000u + w * TRACE_WORDS * 4u;
            traced_tx(base, 0);                /* control first */
            vTaskDelay(pdMS_TO_TICKS(40));
            traced_tx(base, 1);                /* same window, with a burst */
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
    printf("TRACESWEEPEND\n");
#endif

    /* Link test. Beacons go out on their own once the AP is started -- that is
     * direction A, and nat-os's `scan` should list REF_SSID. Raw probe requests
     * are injected alongside so the receiver has two frame types to catch, and
     * link_report() prints every source MAC heard, which is direction B.
     *
     * WIFI_IF_AP, not WIFI_IF_STA: the interface has to be one that exists in
     * the current mode, and this board is now an AP. */
    for (;;) {
#if REF_LINK_AP
        for (int i = 0; i < 10; i++) {
            esp_wifi_80211_tx(WIFI_IF_AP, probe_req, sizeof probe_req, true);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
#else
        /* Receive only. A transmitter that also listens would put its own
         * frames in its own tally, which is noise with no upside here. */
        vTaskDelay(pdMS_TO_TICKS(1000));
#endif
        link_report();
    }
}
