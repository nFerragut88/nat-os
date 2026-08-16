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

#define WIFI_MAC_BITMASK_084            0x3FF73084u
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

static dma_list_item *g_rx_chain;
static int            g_rx_ready;

/* Commits the chain to the hardware.
 *
 * open-mac sets bit 0 and spins until the hardware clears it. The spin is
 * bounded here: an unbounded wait on an undocumented acknowledge bit is a hang
 * with no output if the bit never clears, and this kernel has already spent a
 * session on one of those. */
static int update_rx_chain(void)
{
    volatile uint32_t *reg = (volatile uint32_t *)WIFI_MAC_BITMASK_084;
    *reg |= 0x1u;
    for (uint32_t spins = 0; spins < 1000000u; spins++) {
        if (!(*reg & 0x1u)) {
            return 0;
        }
    }
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
        prev = item;
    }
    g_rx_chain = prev;

    *(volatile uint32_t *)WIFI_BASE_RX_DSCR = (uint32_t)prev;
    disable_filters();

    if (update_rx_chain() != 0) {
        return -4;
    }
    g_rx_ready = 1;
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
