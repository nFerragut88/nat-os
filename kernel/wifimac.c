/* nat-os — WiFi MAC bring-up. See wifimac.h and vendor/phy/MAC-NEXT.md.
 *
 * open-mac's entire MAC initialisation is one line:
 *
 *     MAC_CTRL_REG = MAC_CTRL_REG & 0xffffe800;
 *
 * That is not an oversimplification -- it really is the whole thing, and it is
 * why finishing the environment underneath it was worth the effort. The 0x17ff
 * being cleared is undocumented; Espressif publishes nothing about this
 * register, and the mask comes from esp32-open-mac's reverse engineering.
 *
 * ---- why this file is mostly measurement -------------------------------
 *
 * The write is trivial and a readback proving the bits cleared would prove
 * almost nothing. This kernel has now been caught three separate times by a
 * peripheral whose registers read back exactly as written while the hardware
 * did nothing at all -- LEDC clock-gated behind DPORT_PERIP_CLK_EN bit 11
 * (UM-NATOS-027 §3.3) being the clearest. A successful store is not evidence.
 *
 * So the real content here is wifimac_liveness(): read the register window,
 * wait, read it again, and report which words moved on their own. A gated
 * block is stable; a running MAC has free-running counters in it. That
 * distinction cannot be faked by a write that went nowhere.
 *
 * It also avoids guessing WHICH register is the counter. Espressif's MAC
 * register map is not public and the addresses circulating for it are
 * reverse-engineered. Rather than assert that some specific offset is the TSF
 * timer, this asks the hardware which addresses change and reports them.
 */

#include "wifimac.h"
#include "phyinit.h"
#include "xtensa.h"
#include "task.h"
#include "intr.h"
#include "heap.h"
#include "uart.h"
#include "window.h"
#include "efuse.h"
#include "timer.h"

/* Clock gating for the MAC, which is separate from the WiFi/BT common clock
 * phyinit already ungated (0x3C9).
 *
 * 0x406 is DPORT_WIFI_CLK_WIFI_EN from Espressif's dport_reg.h. Setting it
 * alongside the common bits is what ESP-IDF's periph_module_enable() does for
 * PERIPH_WIFI_MODULE. Nothing else in this kernel uses DPORT_WIFI_CLK_EN_REG,
 * so this is additive and cannot disturb the display, SD or touch paths --
 * they are on PERIP_CLK_EN, a different register entirely. */
#define DPORT_WIFI_CLK_EN_REG   0x3FF000CCu
#define DPORT_WIFI_CLK_WIFI_EN  0x00000406u

/* ---- the reset this kernel has never performed --------------------------
 *
 * UM-NATOS-028 §3 ends on this: nat-os has only ever UNGATED the WiFi
 * peripheral -- turned its clock on -- and never RESET it. The MAC therefore
 * runs in whatever state the ROM bootloader left it in, and a block
 * half-initialised that way would plausibly receive while refusing to
 * transmit. Receive is mostly listen-and-DMA; transmit needs the queues, the
 * rate control and the PHY handoff all correctly armed. That asymmetry is
 * exactly the symptom: 178 frames of 178 reported complete, zero answers.
 *
 * DPORT_WIFI_RST_EN_REG, bit 2 is the MAC.
 *
 *   bit 0  WIFIBB    baseband
 *   bit 1  FE        front end
 *   bit 2  WIFIMAC   <- this one
 *   bit 3  BTBB
 *   bit 4  BTMAC
 *
 * ONLY bit 2. Resetting the baseband or the front end would undo
 * register_chipv7_phy's calibration -- the ten-second one that currently works
 * and that receive depends on -- to test a hypothesis about the MAC. If the MAC
 * reset alone is not enough, widening it is a later and much more expensive
 * experiment, not a free extra.
 *
 * Note also that ESP-IDF's periph_module_reset() does NOTHING for
 * PERIPH_WIFI_MODULE: get_rst_en_mask() returns 0 for it. So the lead as
 * originally written down -- "call periph_module_reset(0x19)" -- would have
 * been a no-op that looked like a test. The bit has to be pulsed directly. */
#define DPORT_WIFI_RST_EN_REG   0x3FF000D0u
#define DPORT_WIFIMAC_RST       (1u << 2)

/* Used twice and far apart: bit 0 commits the rx descriptor chain, and bit 31
 * is open-mac's AP/beacon-mode flag which wifimac_init() clears. Defined here
 * because the earlier of those two uses is near the top of the file. */
#define WIFI_MAC_BITMASK_084    0x3FF73084u

#define MAC_INIT_MASK           0xFFFFE800u

/* 256 words at a time rather than one 4 KB snapshot: the window is 4 KB and a
 * buffer that size in .bss is real DRAM permanently spent on a diagnostic. */
/* ~80 MHz, per kmain.c TICK_INTERVAL_CYCLES. Used only to turn measured
 * cycles into a frequency, so an error here scales every rate uniformly and
 * would not change which registers are counters. */
#define CPU_KHZ     80000u

#define SCAN_WORDS  256u
#define SCAN_CHUNKS 4u

static uint32_t g_snap[SCAN_WORDS];

static int      g_attempted;
static uint32_t g_channel;
static uint32_t g_before, g_after;

/* The MAC-reset experiment. See the register note above. */
static int      g_reset_next;
static int      g_reset_done;
static uint32_t g_rst_before, g_rst_after;
static uint32_t g_bm084_before, g_bm084_after;

uint32_t wifimac_bm084_before(void) { return g_bm084_before; }
uint32_t wifimac_bm084_after(void)  { return g_bm084_after; }

void wifimac_reset_next(int on) { g_reset_next = on; }
int  wifimac_reset_done(void)   { return g_reset_done; }
uint32_t wifimac_rst_before(void) { return g_rst_before; }
uint32_t wifimac_rst_after(void)  { return g_rst_after; }

int      wifimac_attempted(void)   { return g_attempted; }
uint32_t wifimac_ctrl_before(void) { return g_before; }
uint32_t wifimac_ctrl_after(void)  { return g_after; }

/* Busy-waits on the cycle counter rather than sleeping.
 *
 * task_sleep would yield, and a counter that advances across a scheduling
 * gap proves less than one that advances across a known, short, uninterrupted
 * span -- the point is to observe hardware moving while software does not.
 *
 * The caller measures the ACTUAL elapsed cycles rather than trusting this to
 * deliver them; at ~80 MHz, 240,000 cycles is 3 ms, not the 1 ms an earlier
 * comment here claimed. */
static void spin_cycles(uint32_t cycles)
{
    uint32_t start = xt_ccount();
    while ((xt_ccount() - start) < cycles) {
        /* nothing */
    }
}

int wifimac_init(void)
{
    /* The PHY must be up first. The MAC without a calibrated radio underneath
     * it is not a meaningful thing to initialise, and the failure would appear
     * later and elsewhere. */
    if (!phyinit_attempted() || phyinit_result() != 0) {
        return -1;
    }
    if (g_attempted) {
        return -2;
    }

    *(volatile uint32_t *)DPORT_WIFI_CLK_EN_REG |= DPORT_WIFI_CLK_WIFI_EN;

    /* Pulse the MAC out of reset, if asked. Opt-in via wifimac_reset_next()
     * rather than unconditional, because this is a hypothesis under test and
     * the previous behaviour -- receive working -- is the thing it might
     * break. Both orders are then reachable without a reflash. */
    if (g_reset_next) {
        volatile uint32_t *rst = (volatile uint32_t *)DPORT_WIFI_RST_EN_REG;
        g_rst_before = *rst;
        *rst = g_rst_before | DPORT_WIFIMAC_RST;
        /* Held briefly. A reset asserted and released in consecutive stores may
         * not span a peripheral clock edge, and a reset that did not take is
         * indistinguishable from one that did nothing. */
        for (volatile int i = 0; i < 1000; i++) {
        }
        *rst = g_rst_before & ~DPORT_WIFIMAC_RST;
        g_rst_after = *rst;
        g_reset_done = 1;
    }

    /* ram_tx_pwctrl_bg_init() was called here and had to be removed.
     *
     * It is not something open-mac calls, and merely REFERENCING it pulled
     * further objects out of libphy.a, after which register_chipv7_phy died
     * with IllegalInstruction inside set_rx_gain_testchip_70 -- a function on
     * the calibration path that this kernel does not touch and had been
     * working for days.
     *
     * The lesson is about the link, not the radio: with a blob this tangled,
     * naming a symbol changes which objects are pulled in and can break code
     * that was already correct. Reference only what open-mac references. */

    /* WIFI_MAC_BITMASK_084 is READ here and deliberately NOT written.
     *
     * open-mac clears bit 31 in wifi_hw_start_openmac() and sets it only in
     * filters_set_ap_mode(), which reads as an AP/beacon-mode flag -- so
     * clearing it looked free, and this kernel had never touched it in either
     * direction.
     *
     * It is not free. Clearing it here KILLED RECEIVE: descriptors filled
     * dropped to 0 and stayed there across eight seconds, and the rx chain
     * acknowledge count collapsed from 157 to 1. Beacons that had been arriving
     * continuously stopped arriving at all.
     *
     * Which means bit 31 is not what its use in filters_set_ap_mode() suggests,
     * or the write has a side effect at this point in nat-os's sequence that it
     * does not have in open-mac's. Either way the inference was wrong, and the
     * measurement is the only reason that is known.
     *
     * The read stays because the VALUE is informative -- it says what the ROM
     * bootloader left behind, which nothing else reports. */
    g_bm084_before = *(volatile uint32_t *)WIFI_MAC_BITMASK_084;
    g_bm084_after  = g_bm084_before;

    volatile uint32_t *ctrl = (volatile uint32_t *)WIFIMAC_CTRL_REG;
    g_before = *ctrl;
    *ctrl = g_before & MAC_INIT_MASK;
    g_after = *ctrl;

    g_attempted = 1;
    return 0;
}

/* Which words move, and at what frequency.
 *
 * The count alone says the MAC is alive; the RATE says what each mover is. A
 * counter running at 1 MHz is the 802.11 TSF timer -- and the TSF is the one
 * register in this whole region whose identity can be established from
 * behaviour rather than from a reverse-engineered address list.
 *
 * The window is MEASURED, not assumed. The first version of this asserted
 * "240000 cycles is 1 ms", which is true at 240 MHz and false here: nat-os
 * runs at ~80 MHz (kmain.c TICK_INTERVAL_CYCLES), so every rate it printed was
 * three times too high. Reading the cycle counter at both ends fixes that AND
 * removes a second error -- the busy-wait is preemptible, so a tick landing
 * inside it stretches the window and deflates the apparent rate. Dividing by
 * elapsed cycles rather than intended ones makes both problems disappear.
 *
 * Returns how many movers were found; fills up to `max` entries with the
 * address and the rate in kHz. */
uint32_t wifimac_movers(uint32_t *addrs, uint32_t *khz, uint32_t max)
{
    uint32_t found = 0;

    for (uint32_t chunk = 0; chunk < SCAN_CHUNKS; chunk++) {
        volatile uint32_t *base =
            (volatile uint32_t *)(WIFIMAC_BASE + chunk * SCAN_WORDS * 4u);

        uint32_t t0 = xt_ccount();
        for (uint32_t i = 0; i < SCAN_WORDS; i++) {
            g_snap[i] = base[i];
        }
        spin_cycles(240000u);
        uint32_t elapsed = xt_ccount() - t0;
        if (!elapsed) {
            elapsed = 1;
        }

        for (uint32_t i = 0; i < SCAN_WORDS; i++) {
            uint32_t now = base[i];
            if (now != g_snap[i]) {
                if (found < max) {
                    /* Magnitude only: a decrementing counter reports its rate,
                     * and the sign is not what identifies it. */
                    uint32_t d = now - g_snap[i];
                    if (d > 0x80000000u) {
                        d = (uint32_t)(0u - d);
                    }

                    /* d * CPU_KHZ / elapsed, with both sides scaled by 1000 so
                     * it stays in 32 bits. The obvious 64-bit form needs
                     * __udivdi3, which a freestanding call0 build has no
                     * libgcc to supply -- it linked as a call to nothing.
                     *
                     * Dividing elapsed by 1000 first costs under 0.4% at the
                     * ~240k cycles this samples over, which is far inside the
                     * accuracy needed to tell 1 MHz from 133 kHz. */
                    uint32_t denom = elapsed / 1000u;
                    addrs[found] = (uint32_t)&base[i];
                    khz[found] = denom ? (d * (CPU_KHZ / 1000u)) / denom : 0u;
                }
                found++;
            }
        }
    }
    return found;
}

uint32_t wifimac_liveness(uint32_t *first)
{
    uint32_t changed = 0, first_addr = 0;

    for (uint32_t chunk = 0; chunk < SCAN_CHUNKS; chunk++) {
        volatile uint32_t *base =
            (volatile uint32_t *)(WIFIMAC_BASE + chunk * SCAN_WORDS * 4u);

        for (uint32_t i = 0; i < SCAN_WORDS; i++) {
            g_snap[i] = base[i];
        }

        spin_cycles(240000u);           /* ~3 ms at ~80 MHz */

        for (uint32_t i = 0; i < SCAN_WORDS; i++) {
            if (base[i] != g_snap[i]) {
                changed++;
                if (!first_addr) {
                    first_addr = (uint32_t)&base[i];
                }
            }
        }
    }

    if (first) {
        *first = first_addr;
    }
    return changed;
}

uint32_t wifimac_tsf_check(uint32_t ms, uint32_t *cycles)
{
    /* Busy-waits rather than sleeping, and that is a workaround.
     *
     * This first used task_sleep(ms/10), which returned immediately: both
     * clocks agreed that ~1 us had passed for a requested 500 ms. Two
     * independent measurements agreeing on "no time elapsed" is not a
     * measurement error, so task_sleep from shell-task context is not
     * sleeping -- see the note on the 'sleeptest' command. That is a kernel
     * bug, tracked separately; this function must not depend on it.
     *
     * Spinning on CCOUNT still gives what the check needs: a long interval
     * timed by a clock with no connection to the MAC's. */
    uint32_t c0 = xt_ccount();
    uint32_t t0 = wifimac_tsf();
    spin_cycles(ms * (CPU_KHZ / 1000u) * 1000u);
    uint32_t t1 = wifimac_tsf();
    uint32_t c1 = xt_ccount();

    if (cycles) {
        *cycles = c1 - c0;
    }
    return t1 - t0;
}

/* ---- interrupt ----------------------------------------------------------
 *
 * Routing only. Nothing has been observed to arrive here yet, and it will not
 * until receive is enabled -- an idle MAC has nothing to report. The value of
 * doing it now is that the path is in place and COUNTED, so when RX is turned
 * on the question "did the hardware raise anything" has an answer rather than
 * needing new code written under a fault.
 *
 * The clear is the part to be wary of. Line 27 is level-triggered, so a handler
 * that returns without clearing the source at the peripheral re-enters
 * immediately and forever -- a hang with no output. Writing the status back to
 * a presumed clear register is the conventional shape, but 0x3ff73c4c is a
 * reverse-engineered address and this code cannot prove it does anything.
 *
 * What makes that acceptable is that intr_dispatch() already defends against
 * exactly this: a line that keeps re-asserting gets shut off and recorded in
 * intr_disabled_mask(). So the failure mode is a disabled interrupt and a
 * counter to read, not a dead board. That defence was written for a different
 * peripheral and is being relied on here deliberately.
 */
#define WIFI_DMA_INT_STATUS  0x3FF73C48u
#define WIFI_DMA_INT_CLR     0x3FF73C4Cu

static volatile uint32_t g_irq_fires;
static volatile uint32_t g_irq_last_status;

uint32_t wifimac_irq_fires(void)  { return g_irq_fires; }
uint32_t wifimac_irq_status(void) { return g_irq_last_status; }

static void wifimac_isr(void)
{
    uint32_t status = *(volatile uint32_t *)WIFI_DMA_INT_STATUS;
    g_irq_last_status = status;
    *(volatile uint32_t *)WIFI_DMA_INT_CLR = status;
    g_irq_fires++;
}

void wifimac_irq_enable(void)
{
    intr_route(INTR_SRC_WIFI_MAC, INTR_LINE_WIFI_MAC, wifimac_isr);
}

/* ---- receive chain ------------------------------------------------------
 *
 * Now built from esp32-open-mac's actual source rather than from recollection.
 * The descriptor layout below is theirs verbatim, because a bitfield order
 * guessed wrong would hand the DMA engine a length where it expects a flag and
 * corrupt memory silently -- the one class of error this kernel has no defence
 * against.
 *
 * Ten buffers of 1600 bytes is what open-mac uses. This uses four: 6.4 KB
 * against 16 KB, on a board with no PSRAM and a display that already owns a
 * framebuffer. Fewer buffers means frames are dropped under burst load, not
 * that reception fails -- an acceptable trade here, and stated so it is not
 * mistaken for a faithful port.
 */
#define RX_BUFFERS      4u
#define RX_BUFFER_BYTES 1600u

/* WIFI_MAC_BITMASK_084 moved to the top of the file: wifimac_init() clears its
 * bit 31 and is defined well above here. */
#define WIFI_BASE_RX_DSCR               0x3FF73088u
#define WIFI_NEXT_RX_DSCR               0x3FF7308Cu
#define WIFI_LAST_RX_DSCR               0x3FF73090u
#define WIFI_MAC_ADDR_ACK_ENABLE_SLOT_0 0x3FF73064u
#define WIFI_BSSID_FILTER_ADDR_SLOT_0   0x3FF73000u

typedef struct dma_list_item {
    uint16_t size     : 12;
    uint16_t length   : 12;
    uint8_t  _unknown : 6;
    uint8_t  has_data : 1;
    uint8_t  owner    : 1;
    void    *packet;
    struct dma_list_item *next;
} __attribute__((packed)) dma_list_item;

static void wifi_rx_task(void);

static dma_list_item *g_rx_chain;       /* head: oldest descriptor awaiting data */
static dma_list_item *g_rx_last;        /* tail: where recycled items are appended */
static int            g_rx_ready;

/* Totals and the most recent decode. Kept here rather than walking the chain on
 * demand: once recycling works a descriptor is only briefly filled, so anything
 * that reads the chain later would usually find it empty and report nothing
 * received -- exactly backwards. */
static uint8_t  g_beacon_frame[128];
static uint32_t g_beacon_len, g_beacon_due;
static int      g_beacon_on;

static uint8_t           g_my_mac[6];
static uint32_t          g_rx_to_us;
static uint32_t          g_rx_to_us_subtype;
static uint32_t          g_rx_frames;
static uint32_t          g_rx_recycled;
static wifi_frame_info_t g_rx_latest;

/* Distinct networks seen, which is the readable proof that reception is
 * CONTINUING rather than having captured a handful of frames once. */
#define NET_MAX 8
static struct { uint8_t bssid[6]; char ssid[33]; uint32_t seen; } g_nets[NET_MAX];
static uint32_t g_net_count;

/* Commits the chain to the hardware.
 *
 * open-mac sets bit 0 and spins until the hardware clears it. The spin is
 * bounded here: an unbounded wait on an undocumented acknowledge bit is a hang
 * with no output if the bit never clears, and this kernel has already spent a
 * session on one of those. */
static uint32_t g_chain_spins_max;      /* worst acknowledge wait seen */
static uint32_t g_chain_calls;

uint32_t wifimac_chain_spins(void) { return g_chain_spins_max; }
uint32_t wifimac_chain_calls(void) { return g_chain_calls; }

static int update_rx_chain(void)
{
    volatile uint32_t *reg = (volatile uint32_t *)WIFI_MAC_BITMASK_084;
    *reg |= 0x1u;
    g_chain_calls++;
    for (uint32_t spins = 0; spins < 1000000u; spins++) {
        if (!(*reg & 0x1u)) {
            if (spins > g_chain_spins_max) {
                g_chain_spins_max = spins;
            }
            return 0;
        }
    }
    g_chain_spins_max = 1000000u;
    return -1;                          /* never acknowledged */
}

/* Promiscuous: accept everything. The address and BSSID filters are turned OFF
 * rather than programmed with this board's MAC, because the first thing worth
 * proving is that ANY frame arrives. Filtering to our own address first would
 * make "nothing received" ambiguous between a broken receiver and a correct
 * one with nothing addressed to it. */
static void disable_filters(void)
{
    for (uint32_t slot = 0; slot < 2u; slot++) {
        volatile uint32_t *ack =
            (volatile uint32_t *)(WIFI_MAC_ADDR_ACK_ENABLE_SLOT_0 + 8u * slot);
        *ack &= ~0x10000u;
        volatile uint32_t *bssid =
            (volatile uint32_t *)(WIFI_BSSID_FILTER_ADDR_SLOT_0 + 8u * slot);
        *bssid &= ~0x10000u;
        /* open-mac's set_some_kind_of_rx_policy(slot, false). Its purpose is
         * not documented there either -- the name is theirs. Cleared because
         * that is what their working receive path does. */
        volatile uint32_t *policy =
            (volatile uint32_t *)(0x3FF730D8u + 4u * slot);
        *policy &= ~0x110u;
    }
}

int wifimac_rx_start(void)
{
    if (!g_attempted) {
        return -1;                      /* macinit has not run */
    }
    if (g_rx_ready) {
        return -2;
    }

    dma_list_item *prev = 0;
    for (uint32_t i = 0; i < RX_BUFFERS; i++) {
        dma_list_item *item = heap_alloc(sizeof(dma_list_item));
        uint8_t *buf = heap_alloc(RX_BUFFER_BYTES);
        if (!item || !buf) {
            return -3;                  /* out of DRAM; nothing armed */
        }
        item->has_data = 0;
        item->owner    = 1;             /* hardware owns it */
        item->size     = RX_BUFFER_BYTES;
        item->length   = RX_BUFFER_BYTES;
        item->packet   = buf;
        item->next     = prev;
        if (!prev) {
            g_rx_last = item;       /* first built is the tail: its next is NULL */
        }
        prev = item;
    }
    g_rx_chain = prev;

    *(volatile uint32_t *)WIFI_BASE_RX_DSCR = (uint32_t)prev;
    disable_filters();

    if (update_rx_chain() != 0) {
        return -4;
    }
    efuse_factory_mac(g_my_mac);
    g_rx_ready = 1;
    /* NORMAL, deliberately, and this was HIGH for a while.
     *
     * Raising it to HIGH took beacons from 3 Hz to 9.4 Hz, which looked like a
     * clean win because the raycaster blit did not move. It measured the wrong
     * thing. The TOUCH task is NORMAL, and with two flat-out HIGH tasks instead
     * of one it stopped being scheduled except when ageing rescued it -- about
     * every 300 ms. The panel went from responsive to intermittent, which no
     * counter in this kernel reports and which the user noticed immediately.
     *
     * Background radio work does not outrank user input. Beacon timing is worth
     * something; it is not worth the touchscreen, and especially not while
     * transmit does not reach the air at all.
     *
     * Receive is unaffected in practice: at NORMAL it still drained 49 frames
     * in 8 seconds, far above the beacon and scan rates anything here needs. */
    int rxid = task_create("wifirx", wifi_rx_task);
    (void)rxid;
    return 0;
}

/* How many descriptors the hardware has filled. Read from the descriptors
 * themselves rather than from a driver counter, so it reports what the DMA
 * engine actually did and not what this code believes it asked for. */
uint32_t wifimac_rx_filled(void)
{
    uint32_t n = 0;
    for (dma_list_item *it = g_rx_chain; it; it = it->next) {
        if (it->has_data) {
            n++;
        }
    }
    return n;
}

uint32_t wifimac_rx_next_dscr(void)
{
    return *(volatile uint32_t *)WIFI_NEXT_RX_DSCR;
}

/* First filled descriptor's length and a few bytes of its payload, so a
 * received frame can be recognised as 802.11 rather than merely counted. */
int wifimac_rx_peek(uint32_t *len, uint8_t *out, uint32_t max)
{
    for (dma_list_item *it = g_rx_chain; it; it = it->next) {
        if (it->has_data) {
            uint32_t n = it->length;
            *len = n;
            if (n > max) {
                n = max;
            }
            const uint8_t *p = (const uint8_t *)it->packet;
            for (uint32_t i = 0; i < n; i++) {
                out[i] = p[i];
            }
            return (int)n;
        }
    }
    return -1;
}

/* ---- channel ------------------------------------------------------------
 *
 * Arming the receiver was not enough: the chain was accepted, the DMA engine
 * took the base pointer, and nothing ever arrived. Nothing had tuned the radio.
 *
 * open-mac's sequence, and the ORDER is theirs, not a rearrangement:
 *
 *     deinit_mac(); chip_v7_set_chan_nomac(ch, 0);
 *     disable_wifi_agc(); init_mac(); enable_wifi_agc();
 *
 * The MAC is taken down around the retune and the AGC is bracketed across the
 * MAC coming back up. Reordering those would be guessing at a sequence that
 * exists because someone watched the hardware misbehave without it.
 *
 * The three PHY calls are WINDOWED -- they come out of libphy, which this
 * project cannot rebuild -- so each goes through rom_call3(). Calling one
 * directly from call0 is the IllegalInstruction this project has already met.
 */
static void deinit_mac(void)
{
    volatile uint32_t *ctrl = (volatile uint32_t *)WIFIMAC_CTRL_REG;
    *ctrl |= 0x17FFu;
    for (uint32_t spins = 0; spins < 1000000u; spins++) {
        if (!(*ctrl & 0x2000u)) {
            return;
        }
    }
    /* Bounded, like update_rx_chain: an unbounded wait on an undocumented
     * status bit is a silent hang, and the caller can still proceed. */
}

int wifimac_set_channel(uint32_t ch)
{
    extern int chip_v7_set_chan_nomac(int, int);
    extern int disable_wifi_agc(void);
    extern int enable_wifi_agc(void);

    if (!g_attempted) {
        return -1;                      /* macinit has not run */
    }
    if (ch < 1u || ch > 13u) {
        return -2;
    }

    /* Announced step by step. The first attempt panicked with StoreProhibited
     * inside ram_chip_i2c_readReg -- the PHY's analog RF register access -- and
     * with all three calls in a row there was no way to tell which one had
     * reached it. A panic prints nothing about where it came from; these do. */
    phy_stack_prime();          /* so the panic can report the high water */
    uart_puts("   deinit_mac...");
    deinit_mac();
    uart_puts(" ok\n   set_chan_nomac...");
    phy_stack_call((uint32_t)&chip_v7_set_chan_nomac, ch, 0u);
    uart_puts(" ok\n   disable_agc...");
    phy_stack_call((uint32_t)&disable_wifi_agc, 0u, 0u);
    uart_puts(" ok\n   init_mac...");
    *(volatile uint32_t *)WIFIMAC_CTRL_REG &= MAC_INIT_MASK;
    uart_puts(" ok\n   enable_agc...");
    phy_stack_call((uint32_t)&enable_wifi_agc, 0u, 0u);
    uart_puts(" ok\n");

    g_channel = ch;
    return 0;
}

uint32_t wifimac_channel(void) { return g_channel; }

/* ---- reading a captured frame ------------------------------------------
 *
 * The DMA buffer does not start with 802.11. The hardware prepends a receive
 * control header -- RSSI, rate, channel, timestamp -- and the frame follows it.
 * Measured at 40 bytes on this silicon by finding where the beacon actually
 * began: frame control 0x80, then a broadcast address1 of ff:ff:ff:ff:ff:ff,
 * which is unmistakable and cannot land at the right offset by chance.
 *
 * Taken as a measured constant rather than a documented one, because that is
 * what it is. If a future capture decodes as nonsense, this offset is the first
 * thing to re-derive.
 */
#define RX_CTRL_BYTES 40u

/* Decodes one descriptor's payload into out. Split from the old version, which
 * walked the chain looking for a filled descriptor -- that worked only because
 * nothing was ever recycled, so filled descriptors stayed filled forever. With
 * recycling in place a descriptor is filled only briefly, and a chain walk run
 * a moment later would find nothing and report that no frame had arrived. */
static void decode_frame(const dma_list_item *it, wifi_frame_info_t *out)
{
    const uint8_t *p = (const uint8_t *)it->packet + RX_CTRL_BYTES;

    out->fc_type    = (uint8_t)((p[0] >> 2) & 0x3u);
    out->fc_subtype = (uint8_t)((p[0] >> 4) & 0xFu);
    out->length     = it->length;
    for (int i = 0; i < 6; i++) {
        out->addr1[i] = p[4 + i];
        out->addr2[i] = p[10 + i];
        out->addr3[i] = p[16 + i];
    }

    /* Beacon: 24-byte header, 12 fixed bytes (timestamp, interval,
     * capability), then tagged parameters. Tag 0 is the SSID and the standard
     * requires it first. */
    out->ssid[0] = 0;
    if (out->fc_type == 0u && out->fc_subtype == 8u) {
        const uint8_t *tag = p + 24 + 12;
        if (tag[0] == 0u) {
            uint32_t n = tag[1];
            if (n > sizeof(out->ssid) - 1u) {
                n = sizeof(out->ssid) - 1u;
            }
            for (uint32_t i = 0; i < n; i++) {
                uint8_t c = tag[2 + i];
                out->ssid[i] = (c >= 32u && c < 127u) ? (char)c : '?';
            }
            out->ssid[n] = 0;
        }
    }
}

static void note_network(const wifi_frame_info_t *fi)
{
    if (!fi->ssid[0]) {
        return;                     /* only beacons name themselves */
    }
    for (uint32_t i = 0; i < g_net_count; i++) {
        int same = 1;
        for (int b = 0; b < 6; b++) {
            if (g_nets[i].bssid[b] != fi->addr3[b]) {
                same = 0;
                break;
            }
        }
        if (same) {
            g_nets[i].seen++;
            return;
        }
    }
    if (g_net_count < NET_MAX) {
        uint32_t i = g_net_count++;
        for (int b = 0; b < 6; b++) {
            g_nets[i].bssid[b] = fi->addr3[b];
        }
        uint32_t j = 0;
        for (; fi->ssid[j] && j < sizeof(g_nets[i].ssid) - 1u; j++) {
            g_nets[i].ssid[j] = fi->ssid[j];
        }
        g_nets[i].ssid[j] = 0;
        g_nets[i].seen = 1;
    }
}

/* Hands a descriptor back to the hardware, per open-mac's rs_recycle_dma_item.
 *
 * length is restored to size and has_data cleared -- `owner` is deliberately
 * NOT touched, because open-mac does not touch it either and this is not a
 * place to improvise. The item goes on the TAIL, so buffers rotate rather than
 * one being reused while the others sit idle. */
static void recycle_item(dma_list_item *item)
{
    item->length   = item->size;
    item->has_data = 0;
    item->next     = 0;

    if (g_rx_chain) {
        g_rx_last->next = item;
        g_rx_last = item;
        update_rx_chain();
    } else {
        /* The chain drained completely: the hardware has no descriptor at all,
         * so it needs a new base address rather than an appended link. */
        g_rx_chain = item;
        g_rx_last  = item;
        *(volatile uint32_t *)WIFI_BASE_RX_DSCR = (uint32_t)item;
        update_rx_chain();
    }
    g_rx_recycled++;
}

/* Drains filled descriptors and returns them to the hardware.
 *
 * Bounded per call, as open-mac bounds it: without a limit a busy channel can
 * keep this loop fed indefinitely and starve everything else on a cooperative
 * scheduler. */
uint32_t wifimac_rx_service(void)
{
    uint32_t handled = 0;

    while (g_rx_chain && g_rx_chain->has_data && handled < 10u) {
        dma_list_item *item = g_rx_chain;
        dma_list_item *next = item->next;

        g_rx_chain = next;          /* detach before touching the payload */
        item->next = 0;

        if (item->length > RX_CTRL_BYTES + 24u) {
            decode_frame(item, &g_rx_latest);
            note_network(&g_rx_latest);
            g_rx_frames++;

            /* Anything unicast to OUR address is proof the transmitter
             * reached the air: no station sends to this MAC unless it first
             * heard from it. A completion bit cannot establish that, and
             * neither can beaconing -- a radio cannot hear itself. */
            int mine = 1;
            for (int b = 0; b < 6; b++) {
                if (g_rx_latest.addr1[b] != g_my_mac[b]) {
                    mine = 0;
                    break;
                }
            }
            if (mine) {
                g_rx_to_us++;
                g_rx_to_us_subtype = g_rx_latest.fc_subtype;
            }
        }

        recycle_item(item);
        handled++;
    }
    return handled;
}

int wifimac_frame_info(wifi_frame_info_t *out)
{
    if (!g_rx_frames) {
        return -1;
    }
    *out = g_rx_latest;
    return 0;
}

uint32_t wifimac_rx_frames(void)   { return g_rx_frames; }
uint32_t wifimac_rx_recycled(void) { return g_rx_recycled; }
uint32_t wifimac_net_count(void)   { return g_net_count; }

int wifimac_net_info(uint32_t i, uint8_t bssid[6], const char **ssid,
                     uint32_t *seen)
{
    if (i >= g_net_count) {
        return -1;
    }
    for (int b = 0; b < 6; b++) {
        bssid[b] = g_nets[i].bssid[b];
    }
    *ssid = g_nets[i].ssid;
    *seen = g_nets[i].seen;
    return 0;
}

/* Polls, because the MAC interrupt has never fired (see MAC-STATE.md). A task
 * rather than a shell command: reception has to continue whether or not anyone
 * is typing, and four descriptors fill in milliseconds on a busy channel. */
static void wifi_rx_task(void)
{
    for (;;) {
        wifimac_rx_service();

        /* Reaping lives here rather than in a task of its own: it is a couple
         * of register accesses and there is no reason to spend a task slot or
         * a 2 KB stack on it. */
        wifimac_tx_reap();

        /* Beacons, if enabled, paced against the CLOCK rather than against
         * this loop's iteration count.
         *
         * Counting iterations was the first attempt and it beaconed roughly
         * once every four seconds instead of ten times a second: the loop rate
         * is not fixed, because draining a descriptor calls update_rx_chain()
         * which spins on a hardware acknowledge, and the task competes with a
         * HIGH-priority display. A beacon interval that drifts with load is
         * not an interval.
         *
         * 100 ms is what the frame itself advertises (100 TU), so a client
         * that gives up after missed beacons gets what it was promised. */
        if (g_beacon_on) {
            uint32_t now = timer_ticks();
            if ((int32_t)(now - g_beacon_due) >= 0) {
                g_beacon_due = now + 10u;               /* 10 ticks = 100 ms */
                if (!wifimac_tx_busy()) {
                    wifimac_tx(g_beacon_frame, g_beacon_len, 0u);   /* 1 Mbps */
                }
            }
        }

        task_sleep(1u);
    }
}

int wifimac_beacon_start(const char *ssid)
{
    uint8_t mac[6];
    if (!g_rx_ready) {
        return -1;                  /* the rx task is what sends them */
    }
    if (!g_channel) {
        return -2;                  /* a beacon must advertise a real channel */
    }
    efuse_factory_mac(mac);
    g_beacon_len = wifimac_build_beacon(g_beacon_frame, mac, ssid, g_channel);
    g_beacon_due = timer_ticks();
    g_beacon_on = 1;
    return 0;
}

void wifimac_beacon_stop(void) { g_beacon_on = 0; }
uint32_t wifimac_beacon_len(void) { return g_beacon_len; }

/* ---- transmit -----------------------------------------------------------
 *
 * Registers and ordering taken from esp32-open-mac's transmit_80211_frame.
 * The ORDER is theirs and is not rearranged: TX_CONFIG |= 0xa first, then the
 * descriptor address into PLCP0, then the PLCP/duration words, then two more
 * TX_CONFIG bits, and only at the very end the 0xc0000000 in PLCP0 that
 * actually starts the transmission. Sequences like this exist because someone
 * watched the hardware misbehave without them.
 *
 * The base/offset pairs index BACKWARDS: MAC_TX_PLCP0_OS is -2 on a uint32_t
 * array, so slot n sits 8 bytes BELOW the base. Slot 0 is the base itself,
 * which is the only slot used here.
 */
#define MAC_TX_PLCP0_BASE    0x3FF73D20u     /* -8  per slot */
#define WIFI_TX_CONFIG_BASE  0x3FF73D1Cu     /* -8  per slot */
#define MAC_TX_PLCP1_BASE    0x3FF74258u     /* -60 per slot */
#define MAC_TX_PLCP2_BASE    0x3FF7425Cu
#define MAC_TX_DURATION_BASE 0x3FF74268u
#define WIFI_TXQ_CLR_STATE_COMPLETE 0x3FF73CC4u
#define WIFI_TXQ_GET_STATE_COMPLETE 0x3FF73CC8u

#define TX_SLOT 0u

/* ---- two registers the vendor init writes and this driver never has -------
 *
 * Recovered by disassembling hal_mac.o rather than by guessing. Both functions
 * are LEAF functions -- no calls, just a read-modify-write -- so they can be
 * replicated here as direct pokes with no link change at all. That matters: the
 * other route to them is referencing the symbols, and hal_mac.o is already in
 * the image only because --gc-sections kept the nine functions nat-os calls.
 *
 *   hal_mac_tx_set_cca(mode):
 *       reg 0x3FF73C58 -> (val & 0x3FFFFFFF) | (mode << 30)
 *
 *   hal_mac_tx_config_edca(p):
 *       reg 0x3FF73D1C - qid*8 -> AIFS into bits 27:24, CW into bits 21:12
 *
 * WHY THIS IS THE SUSPECT. wifimac_tx() was verified line by line against
 * open-mac and is clean; the queue really is armed (the 0xC0000000 into PLCP0
 * at the end of it is exactly hal_mac_txq_enable). Yet UM-NATOS-034 showed the
 * MAC reporting 302 completions while a receiver 30 cm away heard nothing.
 *
 * A completion is a MAC-level event: the descriptor was retired. It says
 * nothing about whether the medium-access layer ever granted a transmit
 * opportunity. CCA is what tells the MAC whether the channel is busy, and this
 * driver has never written that register in its life; AIFS is the arbitration
 * spacing, and bits 27:24 have only ever held their reset value.
 *
 * A MAC that believes the channel is permanently busy would behave EXACTLY as
 * observed -- accept the frame, arm the queue, retire the descriptor, and never
 * key the radio.
 *
 * Stated as a hypothesis because that is what it is. The bit positions come
 * from the disassembly and are solid; what the values MEAN comes from 802.11
 * convention and is not. Hence a sweep rather than a single write. */
#define WIFI_TX_CCA_REG  0x3FF73C58u

/* Defined below, next to the transmit path it belongs to. */
static volatile uint32_t *tx_reg(uint32_t base, uint32_t stride_bytes);

uint32_t wifimac_cca_get(void)
{
    return *(volatile uint32_t *)WIFI_TX_CCA_REG;
}

void wifimac_cca_set(uint32_t mode)
{
    volatile uint32_t *r = (volatile uint32_t *)WIFI_TX_CCA_REG;
    *r = (*r & 0x3FFFFFFFu) | ((mode & 3u) << 30);
}

uint32_t wifimac_txcfg_get(void)
{
    return *tx_reg(WIFI_TX_CONFIG_BASE, 8u);
}

void wifimac_aifs_set(uint32_t aifs)
{
    volatile uint32_t *r = tx_reg(WIFI_TX_CONFIG_BASE, 8u);
    *r = (*r & 0xF0FFFFFFu) | ((aifs & 0xFu) << 24);
}

void wifimac_cw_set(uint32_t cw)
{
    volatile uint32_t *r = tx_reg(WIFI_TX_CONFIG_BASE, 8u);
    *r = (*r & 0xFFC00FFFu) | ((cw & 0x3FFu) << 12);
}

/* Static, not heap: the descriptor address is handed to hardware masked to its
 * low 20 bits, and it must be 4-byte aligned. A static in .bss satisfies both
 * without depending on what the allocator happens to return. */
static dma_list_item g_tx_item __attribute__((aligned(4)));
static uint8_t       g_tx_buf[256] __attribute__((aligned(4)));
static uint32_t      g_tx_seq;
static uint32_t      g_tx_sent, g_tx_done;

/* Every frame reuses one descriptor and one buffer, so a second must not be
 * handed over while the first is still in flight -- that would rewrite a
 * buffer the DMA engine is reading. Measured: with beacons at 10 Hz and no
 * guard, 421 frames were sent against 273 completions reaped.
 *
 * The deadline is what keeps the guard from being worse than the problem. A
 * completion that never arrives would otherwise stop transmission permanently,
 * and this project has already shipped one wait that could hang forever. */
static int      g_tx_pending;
static uint32_t g_tx_started;
static uint32_t g_tx_forced;

#define TX_PENDING_MAX_TICKS 5u         /* 50 ms; airtime here is under 1 ms */

int wifimac_tx_busy(void)
{
    if (!g_tx_pending) {
        return 0;
    }
    if ((int32_t)(timer_ticks() - g_tx_started) >= (int32_t)TX_PENDING_MAX_TICKS) {
        g_tx_pending = 0;               /* stale; let the caller proceed */
        g_tx_forced++;
        return 0;
    }
    return 1;
}

uint32_t wifimac_tx_forced(void) { return g_tx_forced; }

/* ---- lmacInit, and why this is behind a command rather than in macinit ----
 *
 * open-mac's wifi_hw_start_openmac() calls this and nat-os never has. It is the
 * lower MAC: the layer that owns the transmit queues and the arbitration state
 * machine above the registers UM-NATOS-034 §9 eliminated.
 *
 * THIS REFERENCE CHANGES THE LINK, and that is the whole risk. lmacInit lives
 * in lmac.o, which is not currently in the image; naming it pulls the object
 * in, and lmac.o needs 47 symbols that are not present either. Measured, those
 * resolve into pp.o, trc.o, wdev.o, esf_buf.o, hal_mac_tx.o and rate_control.o
 * -- 305,888 bytes against a 142,016-byte image, before the second round of
 * cascade. One of the 47 is tx_pwctrl_background, in the same object as the
 * calibration that broke last time this was attempted.
 *
 * So it is deliberately NOT wired into macinit. Flashing must stay safe, and
 * the canary -- phyinit returning 0 -- has to be checkable BEFORE anything
 * calls into the new code. A build that links but crashes on boot would take
 * the working receiver with it and tell us nothing.
 *
 * Windowed, like every vendor entry point, so it goes through phy_stack_call.
 */
extern void lmacInit(void);

/* ---- the WiFi power domain -----------------------------------------------
 *
 * open-mac calls esp_wifi_power_domain_on() and nat-os never has. UM-NATOS-034
 * §10.4 recorded that as unreachable, because the symbol is not in libpp or
 * libphy -- which is true and was the wrong conclusion. It is not a blob
 * function at all. It is ESP-IDF, it is open source, and it is six register
 * operations on documented registers:
 *
 *     CLEAR RTC_CNTL_DIG_PWC_REG.WIFI_FORCE_PD
 *     delay 10 us
 *     enable the WiFi/BT common clock
 *     SET then CLEAR DPORT_CORE_RST_EN_REG.MODEM_RESET_FIELD_WHEN_PU
 *     CLEAR RTC_CNTL_DIG_ISO_REG.WIFI_FORCE_ISO
 *
 * ---- why the isolation bit is the suspect --------------------------------
 *
 * WIFI_FORCE_ISO clamps the signals crossing OUT of the WiFi power domain, so
 * a powered-down domain cannot float its outputs into the rest of the chip.
 * With it still asserted the digital side behaves perfectly -- registers accept
 * writes, the MAC takes frames, descriptors retire, completion counters climb
 * -- and nothing reaches the analog side.
 *
 * That is the symptom of UM-NATOS-034 stated register by register, and it fits
 * the one asymmetry nobody has explained: receive works, so the domain has
 * power; only the outbound path is dead.
 *
 * Hypothesis, not fact. But better sourced than §9's CCA guess: that was
 * semantics inferred from disassembly, this is Espressif's own source for the
 * exact function open-mac calls.
 *
 * ---- ORDER MATTERS ---------------------------------------------------------
 *
 * MODEM_RESET_FIELD_WHEN_PU includes WIFIMAC_RST. This RESETS the MAC, so it
 * must run BEFORE macinit, not after. Running it on a live MAC would undo the
 * bring-up and look like a new failure. */
#define RTC_CNTL_DIG_PWC_REG      0x3FF48084u
#define RTC_CNTL_WIFI_FORCE_PD    (1u << 17)
#define RTC_CNTL_DIG_ISO_REG      0x3FF48088u
#define RTC_CNTL_WIFI_FORCE_ISO   (1u << 28)
#define DPORT_CORE_RST_EN_REG     0x3FF000D0u
#define DPORT_WIFI_CLK_EN_REG     0x3FF000CCu
/* WIFIBB | WIFIMAC | BTBB | BTMAC | RW_BTMAC = bits 0,2,3,4,9 */
#define MODEM_RESET_FIELD_WHEN_PU 0x0000021Du
#define DPORT_WIFI_CLK_COMMON     0x000003C9u

#define PDREG(a) (*(volatile uint32_t *)(a))

void wifimac_power_domain_read(uint32_t *pwc, uint32_t *iso, uint32_t *clk)
{
    *pwc = PDREG(RTC_CNTL_DIG_PWC_REG);
    *iso = PDREG(RTC_CNTL_DIG_ISO_REG);
    *clk = PDREG(DPORT_WIFI_CLK_EN_REG);
}

void wifimac_power_domain_on(void)
{
    PDREG(RTC_CNTL_DIG_PWC_REG) &= ~RTC_CNTL_WIFI_FORCE_PD;

    /* The 10 us the reference implementation waits, by cycle count. */
    uint32_t t0 = xt_ccount();
    while ((xt_ccount() - t0) < 800u) {
    }

    PDREG(DPORT_WIFI_CLK_EN_REG) |= DPORT_WIFI_CLK_COMMON;

    PDREG(DPORT_CORE_RST_EN_REG) |=  MODEM_RESET_FIELD_WHEN_PU;
    PDREG(DPORT_CORE_RST_EN_REG) &= ~MODEM_RESET_FIELD_WHEN_PU;

    PDREG(RTC_CNTL_DIG_ISO_REG) &= ~RTC_CNTL_WIFI_FORCE_ISO;
}

extern void lmacInitAc(uint32_t ac);

void wifimac_lmac_init(void)
{
    phy_stack_call((uint32_t)&lmacInit, 0u, 0u);
}

/* The four EDCA access categories. open-mac calls lmacInit and lmacInitAc
 * together; which categories it arms is not recorded, so all four are offered
 * and the caller sweeps. Free: lmacInitAc came into the image with lmacInit. */
void wifimac_lmac_init_ac(uint32_t ac)
{
    phy_stack_call((uint32_t)&lmacInitAc, ac, 0u);
}


static volatile uint32_t *tx_reg(uint32_t base, uint32_t stride_bytes)
{
    return (volatile uint32_t *)(base - stride_bytes * TX_SLOT);
}

int wifimac_tx(const uint8_t *payload, uint32_t len, uint32_t rate)
{
    if (!g_attempted || !g_channel) {
        return -1;                  /* no MAC, or the radio was never tuned */
    }
    if (len < 24u || len > sizeof(g_tx_buf)) {
        return -2;
    }

    for (uint32_t i = 0; i < len; i++) {
        g_tx_buf[i] = payload[i];
    }

    /* Sequence control, bytes 22..23 of the 802.11 header. Every frame from a
     * station carries an incrementing sequence number; a receiver that sees the
     * same one twice treats the second as a retransmission and drops it, which
     * would look exactly like transmit not working. */
    g_tx_buf[22] = (uint8_t)((g_tx_seq & 0x0Fu) << 4);
    g_tx_buf[23] = (uint8_t)((g_tx_seq & 0xFF0u) >> 4);
    g_tx_seq = (g_tx_seq + 1u) & 0xFFFu;

    g_tx_item.owner    = 1;
    g_tx_item.has_data = 1;
    g_tx_item.length   = len;
    g_tx_item.size     = len + 32u;     /* open-mac's headroom, not padding */
    g_tx_item.packet   = g_tx_buf;
    g_tx_item.next     = 0;

    uint32_t is_ht = (rate >= 0x10u);

    *tx_reg(WIFI_TX_CONFIG_BASE, 8u) |= 0xAu;

    *tx_reg(MAC_TX_PLCP0_BASE, 8u) =
        (((uint32_t)&g_tx_item) & 0xFFFFFu) | 0x00600000u;

    *tx_reg(MAC_TX_PLCP1_BASE, 60u) =
        0x10000000u | (len & 0xFFFu) | ((rate & 0x1Fu) << 12) |
        ((is_ht & 1u) << 25);           /* crypto key slot 0: nothing to OR in */
    *tx_reg(MAC_TX_PLCP2_BASE, 60u)    = 0x00000020u;
    *tx_reg(MAC_TX_DURATION_BASE, 60u) = 0;

    *tx_reg(WIFI_TX_CONFIG_BASE, 8u) |= 0x02000000u;
    *tx_reg(WIFI_TX_CONFIG_BASE, 8u) |= 0x00003000u;

    /* Last, and only now: this bit is what puts it on the air. */
    *tx_reg(MAC_TX_PLCP0_BASE, 8u) |= 0xC0000000u;

    g_tx_sent++;
    g_tx_pending = 1;
    g_tx_started = timer_ticks();
    return 0;
}

/* Reaps completed transmissions.
 *
 * This is the self-check that matters. A transmit call that returns 0 has only
 * proved that some register stores did not fault -- the same weak evidence this
 * project has been caught by three times. The hardware setting a completion bit
 * is the MAC saying it actually put the frame out. */
uint32_t wifimac_tx_reap(void)
{
    uint32_t st = *(volatile uint32_t *)WIFI_TXQ_GET_STATE_COMPLETE;
    if (!st) {
        return 0;
    }
    uint32_t slot = 31u - (uint32_t)__builtin_clz(st);
    *(volatile uint32_t *)WIFI_TXQ_CLR_STATE_COMPLETE |= (1u << slot);
    g_tx_pending = 0;
    g_tx_done++;

    /* Transmit power control needs servicing, and open-mac does it here --
     * tx_pwctrl_background(1, 0) on every fourth completion. That call is not
     * decoration: 20 probe requests produced 20 completions and zero answers
     * from any access point, so the MAC was reporting success while nothing
     * useful reached the air. A completion bit says the frame left the queue,
     * not that it left the antenna with any power behind it.
     *
     * Windowed, so it goes through phy_stack_call. That masks interrupts for
     * its duration, which also makes it safe against the shell entering the
     * PHY on the same private stack. */
    extern int tx_pwctrl_background(int, int);
    if ((g_tx_done & 3u) == 0u) {
        phy_stack_call((uint32_t)&tx_pwctrl_background, 1u, 0u);
    }
    return st;
}

uint32_t wifimac_tx_sent(void) { return g_tx_sent; }
uint32_t wifimac_tx_done(void) { return g_tx_done; }

/* ---- a beacon, so the result is visible from another device -------------
 *
 * Building a beacon rather than an arbitrary frame is deliberate: a completion
 * bit proves the MAC accepted the frame, but only a second radio proves it
 * reached the air. A beacon shows up in any phone's WiFi list by name, which is
 * evidence this kernel cannot fake to itself.
 */
uint32_t wifimac_build_beacon(uint8_t *out, const uint8_t mac[6],
                              const char *ssid, uint32_t channel)
{
    uint32_t n = 0;

    out[n++] = 0x80; out[n++] = 0x00;               /* beacon, no flags */
    out[n++] = 0x00; out[n++] = 0x00;               /* duration */
    for (int i = 0; i < 6; i++) { out[n++] = 0xFF; }        /* addr1 broadcast */
    for (int i = 0; i < 6; i++) { out[n++] = mac[i]; }      /* addr2 source */
    for (int i = 0; i < 6; i++) { out[n++] = mac[i]; }      /* addr3 bssid */
    out[n++] = 0x00; out[n++] = 0x00;               /* seq: wifimac_tx fills it */

    for (int i = 0; i < 8; i++) { out[n++] = 0x00; }        /* timestamp */
    out[n++] = 0x64; out[n++] = 0x00;               /* interval, 100 TU */
    out[n++] = 0x01; out[n++] = 0x00;               /* capability: ESS */

    out[n++] = 0x00;                                /* tag 0: SSID */
    uint32_t len_at = n++;
    uint32_t sl = 0;
    while (ssid[sl] && sl < 32u) {
        out[n++] = (uint8_t)ssid[sl++];
    }
    out[len_at] = (uint8_t)sl;

    /* Supported rates. 1 and 2 Mbps, both flagged basic (bit 7). A beacon
     * advertising no rates is malformed and some clients will not list it. */
    out[n++] = 0x01; out[n++] = 0x02; out[n++] = 0x82; out[n++] = 0x84;

    out[n++] = 0x03; out[n++] = 0x01;               /* tag 3: DS parameter */
    out[n++] = (uint8_t)channel;

    return n;
}

uint32_t wifimac_rx_to_us(void)         { return g_rx_to_us; }
uint32_t wifimac_rx_to_us_subtype(void) { return g_rx_to_us_subtype; }

/* A probe request, which is the cleanest question this kernel can ask the air.
 *
 * A beacon is a statement; nothing has to answer it, so silence proves nothing.
 * A broadcast probe request with a wildcard SSID obliges every AP in range to
 * send a probe RESPONSE addressed to this station -- and one arriving is
 * unforgeable evidence that the transmitter works, because a radio cannot hear
 * itself and nobody sends to this MAC without having heard it first.
 */
int wifimac_probe_request(void)
{
    uint8_t f[64];
    uint32_t n = 0;

    f[n++] = 0x40; f[n++] = 0x00;                   /* probe request */
    f[n++] = 0x00; f[n++] = 0x00;                   /* duration */
    for (int i = 0; i < 6; i++) { f[n++] = 0xFF; }  /* addr1 broadcast */
    for (int i = 0; i < 6; i++) { f[n++] = g_my_mac[i]; }    /* addr2 us */
    for (int i = 0; i < 6; i++) { f[n++] = 0xFF; }  /* addr3 broadcast bssid */
    f[n++] = 0x00; f[n++] = 0x00;                   /* seq, filled by tx */

    f[n++] = 0x00; f[n++] = 0x00;                   /* SSID tag, wildcard */
    f[n++] = 0x01; f[n++] = 0x04;                   /* supported rates */
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96;

    return wifimac_tx(f, n, 0u);                    /* 1 Mbps */
}

/* ---- transmit power ----------------------------------------------------
 *
 * The MAC completes every frame and no access point answers a probe request,
 * so the transmitter is enabled with nothing behind it. register_chipv7_phy
 * calibrates the radio; ESP-IDF's esp_phy_enable() does more than that, and
 * the extra is where power control gets armed.
 *
 * Every function reached here is ALREADY in the image -- confirmed with nm
 * before writing the call. That matters more than it sounds: referencing
 * ram_tx_pwctrl_bg_init, which was NOT linked, pulled fresh objects out of
 * libphy.a and broke PHY calibration entirely. Calling something already
 * present cannot change which objects link, so the worst case here is that
 * nothing improves.
 *
 * Exposed as separate steps rather than one sequence because which of them
 * matters is unknown, and a single combined call would not say which one did
 * anything.
 */
/* Measured result: NONE of these changed anything.
 *
 * most_tpw already reads 0x28 -- 40 quarter-dBm, 10 dBm -- straight out of
 * register_chipv7_phy, so the transmitter was never running at zero power and
 * this was never the explanation. phy_set_most_tpw(78) returned success and
 * left the value at 0x28, so it does not take either.
 *
 * A negative result worth keeping: it rules out the most plausible cause and
 * points at the real one, which is that the MAC's transmit machinery was never
 * initialised at all. See vendor/phy/MAC-STATE.md.
 */
int wifimac_txpwr_init(void)
{
    extern int tx_pwctrl_init(void);
    if (!g_attempted) {
        return -1;
    }
    phy_stack_call((uint32_t)&tx_pwctrl_init, 0u, 0u);
    return 0;
}

/* FAULTS. LoadProhibited at 0x40095613, inside tx_pwctrl_cal itself.
 *
 * Kept rather than deleted, because the fault is the finding: this routine
 * expects calibration state that register_chipv7_phy alone does not leave
 * behind, which is the same story the whole transmit path tells. Do not call
 * it without reading the note at the bottom of vendor/phy/MAC-STATE.md. */
int wifimac_txpwr_cal(void)
{
    extern int tx_pwctrl_cal(void);
    if (!g_attempted) {
        return -1;
    }
    phy_stack_call((uint32_t)&tx_pwctrl_cal, 0u, 0u);
    return 0;
}

/* tpw is Espressif's "target power word", quarter-dBm. 78 is 19.5 dBm, near
 * the part's maximum. */
int wifimac_txpwr_set(uint32_t tpw)
{
    extern int phy_set_most_tpw(int);
    if (!g_attempted) {
        return -1;
    }
    phy_stack_call((uint32_t)&phy_set_most_tpw, tpw, 0u);
    return 0;
}

uint32_t wifimac_txpwr_get(void)
{
    extern int phy_get_most_tpw(void);
    return phy_stack_call((uint32_t)&phy_get_most_tpw, 0u, 0u);
}

/* ---- the MAC hardware init chain ---------------------------------------
 *
 * The part of open-mac's hwinit() that nat-os had never run. Receive works
 * without it because the RX path is a descriptor chain and a DMA engine;
 * transmit does not, because the transmit machinery -- queues, access
 * categories, RF switch timing -- is built here and nowhere else.
 *
 * All four live in libpp, the MAC blob, which until now was linked but never
 * referenced. Every one is WINDOWED and goes through phy_stack_call.
 *
 * Separate steps on purpose. Referencing a single unlinked libphy symbol
 * previously broke PHY calibration outright, and this pulls in far more than
 * one symbol, so each stage is callable and reportable on its own. If the
 * board dies, WHICH call it died in is the first thing worth knowing.
 */
static int g_hw_stage;

int wifimac_hwinit_step(uint32_t step)
{
    extern int ic_mac_init(void);
    extern int hal_init(void);
    extern int ic_enable_rx(void);
    extern int hal_mac_tsf_reset(int);

    /* Steps 4..6: the transmit side of the chain.
     *
     * The four above were tried in UM-NATOS-028 §3 and changed nothing, but
     * they are all about bringing the MAC up and letting it listen. None of
     * them arms anything transmit needs.
     *
     * These three do, and every one of them is ALREADY IN THE IMAGE -- checked
     * with nm before being named, which is the rule bought expensively when
     * referencing ram_tx_pwctrl_bg_init pulled fresh objects out of libphy.a
     * and killed register_chipv7_phy. Adding a call to something already linked
     * cannot change which objects the linker pulls.
     *
     *   hal_mac_rate_autoack_init  transmit needs a RATE. A MAC whose rate
     *                              control was never initialised would
     *                              plausibly accept a frame, report it
     *                              complete, and put nothing on the air --
     *                              which is the symptom exactly.
     *   hal_attenna_init           Espressif's spelling. The antenna path.
     *   hal_mac_disable_low_rate   sets which rates are permitted at all.
     *
     * lmacInit is the other candidate and is NOT linked, so calling it would
     * change the link. Deliberately not attempted here. */
    extern int hal_mac_rate_autoack_init(void);
    extern int hal_attenna_init(void);
    extern int hal_mac_disable_low_rate(void);

    if (!g_attempted) {
        return -1;                  /* macinit has not run */
    }

    switch (step) {
    case 0: phy_stack_call((uint32_t)&ic_mac_init, 0u, 0u);       break;
    case 1: phy_stack_call((uint32_t)&hal_init, 0u, 0u);          break;
    case 2: phy_stack_call((uint32_t)&ic_enable_rx, 0u, 0u);      break;
    case 3: phy_stack_call((uint32_t)&hal_mac_tsf_reset, 0u, 0u); break;
    case 4: phy_stack_call((uint32_t)&hal_mac_rate_autoack_init, 0u, 0u); break;
    case 5: phy_stack_call((uint32_t)&hal_attenna_init, 0u, 0u);  break;
    case 6: phy_stack_call((uint32_t)&hal_mac_disable_low_rate, 0u, 0u); break;
    default: return -2;
    }
    if ((int)step >= g_hw_stage) {
        g_hw_stage = (int)step + 1;
    }
    return 0;
}

int wifimac_hw_stage(void) { return g_hw_stage; }
