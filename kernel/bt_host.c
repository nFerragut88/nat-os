/* nat-os — the call0 side of the Bluetooth controller's host services.
 *
 * ---- what this is ----------------------------------------------------------
 *
 * Espressif's BT controller is four archives of WINDOWED code that expect an
 * ESP-IDF underneath them: FreeRTOS queues and tasks, esp_timer, the interrupt
 * allocator, a capability heap, the PHY. vendor/windowed/bt_osi.c answers those
 * symbols in the ABI the blob calls with, and forwards the real work here,
 * where the kernel lives.
 *
 * The split is the same one the WiFi bring-up established: the blob's ABI on
 * one side, nat-os on the other, and exactly one bridge between them
 * (kernel/window.S). vendor/phy/README.md records what happens when a windowed
 * function is called from call0 by accident -- an IllegalInstruction in
 * padding, from code that linked cleanly.
 *
 * ---- what is NOT here ------------------------------------------------------
 *
 * The queues, semaphores, event groups and software timers are already built,
 * in kernel/wifi_osi_impl.c, because the WiFi blob needed the same things and
 * that file is compiled into every build. This one adds only what Bluetooth
 * asks for and WiFi did not: an interrupt allocator with an argument, a
 * microsecond clock, esp_timer's shape over the existing timers, and the
 * controller bring-up itself.
 *
 * [next_moves/13 step 1]
 */

#include <stdint.h>
#include "bt.h"
#include "heap.h"
#include "intr.h"
#include "timer.h"
#include "uart.h"
#include "xtensa.h"
#include "critical.h"
#include "phyinit.h"
#include "task.h"
#include "window.h"

extern uint32_t osi_impl_evt_wait(void *h, uint32_t bits, int clear, int all,
                                  uint32_t ticks);

/* ---- counters, because a shim that is never called is a shim that is wrong --
 *
 * The WiFi bring-up's hardest hours went on calls that silently did nothing
 * (UM-NATOS-038). Every service here counts its uses, so `bt` can show which
 * parts of the controller actually ran rather than which parts compiled. */
static uint32_t g_n_intr, g_n_timer_arm, g_n_malloc, g_n_free, g_n_log;
static uint32_t g_bytes_live;

uint32_t bt_host_intr_allocs(void) { return g_n_intr; }
uint32_t bt_host_timer_arms(void)  { return g_n_timer_arm; }
uint32_t bt_host_mallocs(void)     { return g_n_malloc; }
uint32_t bt_host_frees(void)       { return g_n_free; }
uint32_t bt_host_bytes_live(void)  { return g_bytes_live; }
uint32_t bt_host_logs(void)        { return g_n_log; }

/* ---- the heap ---------------------------------------------------------------
 *
 * heap_alloc() has no size record of its own, so a four-byte header carries it:
 * the controller frees with free(), which takes no size, and the live-bytes
 * counter is the only way to see how much of this board's 22 KB the controller
 * has taken. That number decides whether Bluetooth fits at all, so it is not
 * optional. */
/* The largest single block, and the largest request SEEN -- including the ones
 * refused. The controller answers ESP_ERR_NO_MEM without saying what it wanted,
 * so the only way to learn its appetite is to record every ask. */
static uint32_t g_biggest_ask, g_refused;

uint32_t bt_host_biggest_ask(void) { return g_biggest_ask; }
uint32_t bt_host_refused(void)     { return g_refused; }

void *bt_host_malloc(uint32_t bytes)
{
    if (bytes > g_biggest_ask) {
        g_biggest_ask = bytes;
    }
    /* 0x4000 was the first cap, and it was MY bug: the controller asks for one
     * block larger than that, was refused, and reported ESP_ERR_NO_MEM against
     * a heap with 29 KB free. A cap exists only to stop a corrupt length from
     * taking the whole heap, so it belongs just above what the heap can serve. */
    if (bytes == 0u || bytes > 0x8000u) {
        g_refused++;
        return 0;
    }
    uint32_t *p = (uint32_t *)heap_alloc(bytes + 4u);
    if (!p) {
        g_refused++;
        uart_puts("   [bt] heap refused ");
        uart_put_dec(bytes);
        uart_puts(" B, largest free ");
        uart_put_dec(heap_largest_free());
        uart_puts("\n");
        return 0;
    }
    p[0] = bytes;
    g_n_malloc++;
    g_bytes_live += bytes;
    return (void *)(p + 1);
}

void bt_host_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    uint32_t *p = (uint32_t *)ptr - 1;
    g_bytes_live -= p[0];
    g_n_free++;
    heap_free(p);
}

/* Printed at the moment of failure, with the heap's state beside it: a queue
 * the controller wanted and could not have is the whole of ESP_ERR_NO_MEM, and
 * after the fact the heap looks merely empty. */
void bt_host_queue_fail(uint32_t len, uint32_t item_size)
{
    uart_puts("   [bt] queue refused: ");
    uart_put_dec(len);
    uart_puts(" x ");
    uart_put_dec(item_size);
    uart_puts(" B = ");
    uart_put_dec(len * item_size);
    uart_puts(" B, largest free ");
    uart_put_dec(heap_largest_free());
    uart_puts("\n");
}

/* The shim trace's printer. See BT_TR in vendor/windowed/bt_osi.c -- built only
 * with -BTTrace, because it prints inside the controller's init and changes its
 * timing completely. uart_puts is polled, so it works with interrupts masked,
 * which is the whole point: the freeze being diagnosed stops the tick. */
void bt_host_trace(uint32_t str)
{
    const char *s = (const char *)str;
    uart_puts("  . ");
    for (uint32_t i = 0; i < 48u && s[i]; i++) {
        uart_putc(s[i]);
    }
    uart_puts("\n");
}

/* ---- the BT peripheral's clock and reset ------------------------------------
 *
 * periph_module_enable() was `{ (void)module; }` -- a stub that was not even
 * counted, which is the one kind this project is not allowed to have. The
 * controller's init then ran with the Bluetooth clock still gated and the BT
 * resets still asserted, so every register it polled read back a constant and
 * it spun until the Timer Group 0 watchdog restarted the chip:
 *
 *     rst:0x7 (TG0WDT_SYS_RESET)
 *
 * right after "handing the controller its configuration". Exactly the failure
 * wifi_osi_impl.c already has a long comment about -- _wifi_clock_enable was
 * empty there too, and the symptom was a radio that reported success and did
 * nothing. Same mistake, different peripheral, found the same way.
 *
 * Only libbt_natos.a references these two symbols (the blob proper pokes its
 * own registers once the clock is live), so this is the whole of the host's
 * responsibility for BT clock gating.
 *
 * Constants are Espressif's, from soc/esp32/dport_reg.h. The two registers are
 * the ones this kernel already uses for the WiFi side -- see DP_WIFI_CLK_EN and
 * DP_CORE_RST_EN in wifi_osi_impl.c, and the bit list in wifimac.c:
 *
 *   DPORT_WIFI_CLK_EN_REG  0x3FF000CC  bits 0x30800 = DPORT_WIFI_CLK_BT_EN_M
 *   DPORT_CORE_RST_EN_REG  0x3FF000D0  bits 4..10   = the seven BT reset bits
 *                                                     (BT, BTMAC, RW_BTMAC,
 *                                                     RW_BTLP and the three
 *                                                     _REG_ variants)
 *
 * Enabling CLEARS the reset bits rather than pulsing them. That matters: a
 * pulse would reset the BT baseband, and wifimac.c records what that costs --
 * register_chipv7_phy's calibration, the ten-second one, undone. Clearing a
 * bit that is already clear does nothing, so this cannot disturb the PHY that
 * came up two lines earlier. Bit 3 (BTBB, the baseband) is deliberately not in
 * the mask for the same reason.
 *
 * The module number is printed because IDF's periph_module_t numbering is not
 * something this tree can check against a header it does not have -- so rather
 * than assert a value, the shim says which number actually arrived and applies
 * the BT bits, which in a -BT image is the only thing any caller can mean. */
#define DP_WIFI_CLK_EN   0x3FF000CCu
#define DP_CORE_RST_EN   0x3FF000D0u
#define DP_CLK_BT_EN     0x00030800u
#define DP_BT_RST_BITS   0x000007F0u

static uint32_t g_periph_on;
static uint32_t g_periph_off;
static uint32_t g_periph_last = 0xFFFFFFFFu;

uint32_t bt_host_periph_ons(void)  { return g_periph_on; }
uint32_t bt_host_periph_offs(void) { return g_periph_off; }
uint32_t bt_host_periph_last(void) { return g_periph_last; }

void bt_host_periph(uint32_t module, uint32_t on)
{
    volatile uint32_t *clk = (volatile uint32_t *)DP_WIFI_CLK_EN;
    volatile uint32_t *rst = (volatile uint32_t *)DP_CORE_RST_EN;

    g_periph_last = module;

    if (on) {
        g_periph_on++;
        *clk |= DP_CLK_BT_EN;
        *rst &= ~DP_BT_RST_BITS;
    } else {
        g_periph_off++;
        *clk &= ~DP_CLK_BT_EN;
        *rst |= DP_BT_RST_BITS;
    }

    uart_puts("   [bt] periph module ");
    uart_put_dec(module);
    uart_puts(on ? " enabled, clk now " : " disabled, clk now ");
    uart_put_hex(*clk);
    uart_puts(" rst ");
    uart_put_hex(*rst);
    uart_puts("\n");
}

/* ---- the microsecond clock --------------------------------------------------
 *
 * esp_timer_get_time() is a 64-bit microsecond count, and this kernel has a
 * 10 ms tick and a 32-bit cycle counter that wraps every 53 seconds. Ticks are
 * the only thing that is monotonic for longer than a minute, so the coarse
 * answer is the honest one: ticks * 10,000, with the sub-tick left at zero.
 *
 * 10 ms of resolution where the caller may expect 1 µs is a real limitation and
 * the FIRST thing to suspect if the controller misbehaves around sleep or
 * scheduling. It is written down here rather than discovered later.
 *
 * The multiply is done by hand in 16-bit halves: this kernel links no libgcc,
 * so a 64-bit multiply would be an undefined reference at best (next_moves/11
 * step 2 cost a build to that). */
void bt_host_us(uint32_t *out)
{
    uint32_t t = timer_ticks();
    uint32_t lo16 = t & 0xFFFFu, hi16 = t >> 16;
    uint32_t p0 = lo16 * 10000u;            /* fits: 65535 * 10000 < 2^32 */
    uint32_t p1 = hi16 * 10000u;
    uint32_t lo = p0 + (p1 << 16);
    uint32_t hi = (p1 >> 16) + ((p0 + (p1 << 16)) < p0 ? 1u : 0u);
    out[0] = lo;
    out[1] = hi;
}

/* ---- interrupts -------------------------------------------------------------
 *
 * esp_intr_alloc() hands over a handler that takes an argument; intr_route()
 * takes a function of no arguments. So each allocation keeps the pair and a
 * trampoline per line supplies it. Four lines is more than the controller asks
 * for (BT uses two) and the table is checked rather than assumed. */
#define BT_INTR_MAX 4u

typedef struct {
    void (*fn)(void *);
    void *arg;
    uint32_t source;
    uint32_t line;
    uint32_t used;
} bt_intr_t;

static bt_intr_t g_intr[BT_INTR_MAX];

/* The lines Bluetooth is given for MATRIX sources.
 *
 * LEVEL 3 ONLY. Lines 5 and 7 were in this list first, and they are level 1 in
 * the silicon: the kernel dispatches level 3 from _handler_level3 and treats a
 * level-1 arrival as a fault, so the first edge on either took the board down
 * with exccause 4.
 *
 * The list was then { 11, 15, 22, 29 }, and three of those four were wrong in a
 * way that had not yet been reached:
 *
 *   15  is INTR_LINE_TIMER1 -- the SCHEDULER TICK. intr_route() installs by
 *       overwriting g_handler[line], so the controller's second allocation
 *       would have replaced the kernel's clock with a BT handler. Nothing
 *       would have reported it; the board would simply have stopped keeping
 *       time, in a build that had just been given Bluetooth to blame.
 *   11  is the internal PROFILING interrupt and 29 the internal SW1 -- see
 *       BT_INTERNAL below. Handing them out to matrix sources collides with
 *       the only lines the internal sources are allowed to use.
 *
 * What is actually free at level 3, once the kernel's own 15, 23 (GPIO) and 27
 * (WiFi MAC) are set aside, is line 22. One line, stated honestly rather than
 * padded out to four with lines that belong to someone else. Stage 1 allocates
 * no matrix source at all, so this is not yet exercised; stage 2 will need
 * RWBT and RWBLE and will have to share 22 between them, which means a
 * trampoline that calls both handlers. That is a stage-2 problem and it is
 * better to meet it as a refusal here than as a silent theft of line 15. */
#define BT_MATRIX_LINES 1u
static const uint32_t BT_LINES[BT_MATRIX_LINES] = { 22u };

/* ---- IDF's INTERNAL interrupt sources --------------------------------------
 *
 * esp_intr_alloc() takes negative source numbers for the six interrupts that
 * are internal to the Xtensa core and do NOT pass through the DPORT matrix:
 *
 *     -1  ETS_INTERNAL_TIMER0       line 6    level 1
 *     -2  ETS_INTERNAL_TIMER1       line 15   level 3   (the kernel's tick)
 *     -3  ETS_INTERNAL_TIMER2       line 16   level 5
 *     -4  ETS_INTERNAL_SW0          line 7    level 1
 *     -5  ETS_INTERNAL_SW1          line 29   level 3
 *     -6  ETS_INTERNAL_PROFILING    line 11   level 3
 *
 * The line is fixed by the silicon, not chosen. Passing one of these to
 * intr_route() was the bug that froze the board for thirty seconds: the source
 * number went into DPORT_PRO_MAP(src) as unsigned, and -5 addressed twenty
 * bytes BELOW the map array, writing a line number into DPORT's clock and
 * reset block. intr_route() now refuses out-of-range sources; this table is
 * how the request gets answered correctly instead.
 *
 * Only level 3 is serviceable here, and -2 is the scheduler's own line, so
 * exactly two of the six can be honoured: SW1 and PROFILING. The rest are
 * refused out loud. A refusal the controller cannot see is still better than a
 * handler on a line the kernel never dispatches, which is what the level-1
 * entries in the old BT_LINES produced. */
/* Defined further down this file, beside the controller's xt_ints_on/off. */
void intr_enable_mask(uint32_t mask);

#define BT_INTERNAL_MIN 1u
#define BT_INTERNAL_MAX 6u

static const uint32_t BT_INTERNAL_LINE[BT_INTERNAL_MAX] = {
    6u,     /* -1 timer0,    level 1, not dispatched    */
    15u,    /* -2 timer1,    level 3, THE KERNEL'S TICK */
    16u,    /* -3 timer2,    level 5, not dispatched    */
    7u,     /* -4 sw0,       level 1, not dispatched    */
    29u,    /* -5 sw1,       level 3, usable            */
    11u,    /* -6 profiling, level 3, usable            */
};

/* Which of them this kernel will actually install. Indexed as above. */
static const uint8_t BT_INTERNAL_OK[BT_INTERNAL_MAX] = { 0, 0, 0, 0, 1, 1 };

static void bt_intr_tramp0(void) { if (g_intr[0].used) { g_intr[0].fn(g_intr[0].arg); } }
static void bt_intr_tramp1(void) { if (g_intr[1].used) { g_intr[1].fn(g_intr[1].arg); } }
static void bt_intr_tramp2(void) { if (g_intr[2].used) { g_intr[2].fn(g_intr[2].arg); } }
static void bt_intr_tramp3(void) { if (g_intr[3].used) { g_intr[3].fn(g_intr[3].arg); } }

static const intr_handler_fn BT_TRAMPS[BT_INTR_MAX] = {
    bt_intr_tramp0, bt_intr_tramp1, bt_intr_tramp2, bt_intr_tramp3
};

/* Returns a handle (1-based index) or 0.
 *
 * `source` is IDF's esp_intr_alloc source: 0..68 for a peripheral that comes
 * through the DPORT matrix, or -1..-6 for one of the core's internal
 * interrupts, which must NOT be routed and whose line is fixed. The two cases
 * are genuinely different and conflating them cost thirty seconds of frozen
 * board per attempt -- see BT_INTERNAL_LINE above. */
uint32_t bt_host_intr_alloc(uint32_t source, uint32_t fn, uint32_t arg)
{
    int32_t src = (int32_t)source;
    uint32_t internal = 0;
    uint32_t line;

    if (src < 0) {
        uint32_t idx = (uint32_t)(-src) - 1u;   /* -1 -> 0 */
        if ((uint32_t)(-src) < BT_INTERNAL_MIN ||
            (uint32_t)(-src) > BT_INTERNAL_MAX) {
            uart_puts("   [bt] internal source ");
            uart_put_dec((uint32_t)(-src));
            uart_puts(" is not one of IDF's six -- refused\n");
            return 0;
        }
        if (!BT_INTERNAL_OK[idx]) {
            /* Said out loud, with the reason, because the controller gets back
             * only a failure code and this is the sort of thing that otherwise
             * reappears a week later as "Bluetooth does not work". */
            uart_puts("   [bt] internal source -");
            uart_put_dec((uint32_t)(-src));
            uart_puts(" wants line ");
            uart_put_dec(BT_INTERNAL_LINE[idx]);
            uart_puts(BT_INTERNAL_LINE[idx] == INTR_LINE_TIMER1
                      ? " -- that is the scheduler tick, refused\n"
                      : " -- not level 3, this kernel cannot dispatch it\n");
            return 0;
        }
        internal = 1u;
        line = BT_INTERNAL_LINE[idx];
    } else if (source >= INTR_SRC_COUNT) {
        uart_puts("   [bt] source ");
        uart_put_dec(source);
        uart_puts(" is outside 0..68 -- refused\n");
        return 0;
    } else {
        line = 0xFFFFFFFFu;     /* chosen from BT_LINES below */
    }

    for (uint32_t i = 0; i < BT_INTR_MAX; i++) {
        if (g_intr[i].used) {
            continue;
        }
        if (!internal) {
            if (i >= BT_MATRIX_LINES) {
                uart_puts("   [bt] out of level-3 matrix lines for source ");
                uart_put_dec(source);
                uart_puts(" -- refused\n");
                return 0;
            }
            line = BT_LINES[i];
        }

        g_intr[i].fn = (void (*)(void *))fn;
        g_intr[i].arg = (void *)arg;
        g_intr[i].source = source;
        g_intr[i].line = line;
        g_intr[i].used = 1u;

        uart_puts("   [bt] ");
        if (internal) {
            uart_puts("internal source -");
            uart_put_dec((uint32_t)(-src));
            uart_puts(" on its fixed line ");
        } else {
            uart_puts("source ");
            uart_put_dec(source);
            uart_puts(" routed to line ");
        }
        uart_put_dec(line);
        uart_puts("\n");

        if (internal) {
            /* No matrix write: there is no map register for these. Install the
             * handler and enable the line directly. */
            intr_install(line, BT_TRAMPS[i]);
            intr_enable_mask(1u << line);
        } else {
            intr_route(source, line, BT_TRAMPS[i]);
        }
        g_n_intr++;
        return i + 1u;
    }
    return 0;                   /* refuse: a silent overwrite loses a driver */
}

/* ---- handlers installed by LINE, not by source ------------------------------
 *
 * The controller uses two different ways in. esp_intr_alloc() asks the host to
 * pick a line for a peripheral source; xt_set_interrupt_handler() names the
 * line itself and expects a handler to appear on it. The second was stubbed in
 * the first version -- counted and refused, on the grounds that the source was
 * unknown -- and the result was a KERNEL PANIC during controller init:
 *
 *     exccause 4 (Level1Interrupt) at 0x4009a2c4
 *
 * which is the CPU taking an interrupt that has no handler behind it. The
 * controller had installed its handler, been refused, enabled the line anyway
 * through xt_ints_on, and the first edge brought the board down. Refusing a
 * request is only honest if the caller can see the refusal, and a blob cannot.
 *
 * Eight slots, each with its own trampoline, because the kernel's handlers take
 * no argument and the controller's take one. */
#define BT_XT_MAX 8u

static struct {
    void (*fn)(void *);
    void *arg;
    uint32_t line;
    uint32_t used;
} g_xt[BT_XT_MAX];

static uint32_t g_n_xt;

uint32_t bt_host_xt_handlers(void) { return g_n_xt; }

static void xt_t0(void) { if (g_xt[0].used) { g_xt[0].fn(g_xt[0].arg); } }
static void xt_t1(void) { if (g_xt[1].used) { g_xt[1].fn(g_xt[1].arg); } }
static void xt_t2(void) { if (g_xt[2].used) { g_xt[2].fn(g_xt[2].arg); } }
static void xt_t3(void) { if (g_xt[3].used) { g_xt[3].fn(g_xt[3].arg); } }
static void xt_t4(void) { if (g_xt[4].used) { g_xt[4].fn(g_xt[4].arg); } }
static void xt_t5(void) { if (g_xt[5].used) { g_xt[5].fn(g_xt[5].arg); } }
static void xt_t6(void) { if (g_xt[6].used) { g_xt[6].fn(g_xt[6].arg); } }
static void xt_t7(void) { if (g_xt[7].used) { g_xt[7].fn(g_xt[7].arg); } }

static const intr_handler_fn XT_TRAMPS[BT_XT_MAX] = {
    xt_t0, xt_t1, xt_t2, xt_t3, xt_t4, xt_t5, xt_t6, xt_t7
};

uint32_t bt_host_set_handler(uint32_t line, uint32_t fn, uint32_t arg)
{
    if (line >= 32u) {
        return 0;
    }
    /* A line asked for twice is the same handler being re-installed, which the
     * controller does; replace rather than consume a second slot. */
    for (uint32_t i = 0; i < BT_XT_MAX; i++) {
        if (g_xt[i].used && g_xt[i].line == line) {
            g_xt[i].fn = (void (*)(void *))fn;
            g_xt[i].arg = (void *)arg;
            return 1;
        }
    }
    for (uint32_t i = 0; i < BT_XT_MAX; i++) {
        if (g_xt[i].used) {
            continue;
        }
        g_xt[i].fn = (void (*)(void *))fn;
        g_xt[i].arg = (void *)arg;
        g_xt[i].line = line;
        g_xt[i].used = 1u;
        /* Printed, not just recorded: which LINE the controller asks for
         * decides whether this kernel can service it at all. Lines have fixed
         * priority levels in the silicon and this kernel dispatches level 3
         * only (intr.h), so a level-1 request is a panic waiting for its first
         * edge -- which is exactly what exccause 4 was. */
        uart_puts("   [bt] handler wanted on CPU line ");
        uart_put_dec(line);
        uart_puts(line == 11u || line == 15u || line == 22u || line == 29u
                  ? " (level 3, serviceable)\n"
                  : " (NOT level 3 -- this kernel cannot dispatch it)\n");
        intr_install(line, XT_TRAMPS[i]);
        g_n_xt++;
        return 1;
    }
    return 0;
}

void bt_host_intr_free(uint32_t handle)
{
    if (handle == 0u || handle > BT_INTR_MAX) {
        return;
    }
    g_intr[handle - 1u].used = 0u;
}

/* ---- logging ----------------------------------------------------------------
 *
 * The controller logs through esp_log_write and esp_rom_printf. Format strings
 * are not expanded: this kernel has no printf, the ROM's is windowed and takes
 * varargs across an ABI boundary, and a format bug inside a blob's log call is
 * a crash rather than a message. The string is printed as it stands, which is
 * enough to see WHERE the controller is complaining. */
void bt_host_log(uint32_t str)
{
    const char *s = (const char *)str;
    if (!s) {
        return;
    }
    g_n_log++;
    uart_puts("   [bt] ");
    for (uint32_t i = 0; i < 160u && s[i]; i++) {
        uart_putc(s[i]);
    }
    uart_puts("\n");
}

/* ---- the PHY ----------------------------------------------------------------
 *
 * phyinit_run() is the WiFi bring-up's sequence and it is what register_chipv7_phy
 * needs for either radio -- the PHY is shared hardware, and the blob's own
 * esp_phy_enable is what the BT controller calls. Done once; a second call
 * returns the first result rather than re-running calibration. */
static int g_phy_done, g_phy_rc;

int bt_host_phy_enable(void)
{
    if (!g_phy_done) {
        /* phyinit_run() itself is behind BOARD_HAS_WIFI, because a -WiFi build
         * links its own libphy; phyinit_run_at() takes the entry as a PARAMETER
         * precisely so a build that links a different copy can use it. The copy
         * here is the ROM's, which esp32.rom.ld already provides. */
        extern int register_chipv7_phy(const void *, void *, int);
        g_phy_rc = phyinit_run_at((uint32_t)&register_chipv7_phy);
        g_phy_done = 1;
    }
    return g_phy_rc;
}

void bt_host_phy_report(void)
{
    uart_puts("   phy: ");
    if (!g_phy_done) {
        uart_puts("not run\n");
        return;
    }
    uart_puts(g_phy_rc == 0 ? "calibrated ok\n" : "FAILED rc=");
    if (g_phy_rc != 0) {
        uart_put_dec((uint32_t)g_phy_rc);
        uart_puts("\n");
    }
}

/* ---- the controller's own task ----------------------------------------------
 *
 * The blob asks for a task with a stack size, a priority and an ARGUMENT; this
 * kernel's entry functions take none. One static pair and a trampoline covers
 * it, because the controller creates exactly one task -- and a second request
 * is refused rather than quietly handed the first one's argument.
 *
 * The stack is static and sized from the controller's own request (3,584 bytes
 * by default, rounded up): task_create_with_stack exists precisely because a
 * vendor task once asked for more than the pool's fixed size (task.h). */
#define BT_TASK_WORDS 1280u             /* 5,120 bytes */

static uint32_t g_bt_stack[BT_TASK_WORDS];
static void (*g_bt_entry)(void *);
static void *g_bt_arg;
static int g_bt_task_made;

/* Did the controller's task ever run, and did it come back?
 *
 * blobcall.c has exactly these three counters for the WiFi blob (g_bt_reached,
 * g_bt_running, g_bt_returned, added at step 179) and the reason is the same:
 * when the init context blocks waiting for a worker, "reached != running" says
 * the worker is stuck before its first instruction and "running with no
 * return" says it is inside the blob. Without them, a dead worker and a
 * deadlocked semaphore look identical from the init side -- which is precisely
 * the wrong conclusion this file drew once already. */
static uint32_t g_bt_reached, g_bt_running, g_bt_returned;

uint32_t bt_host_task_reached(void)  { return g_bt_reached; }
uint32_t bt_host_task_running(void)  { return g_bt_running; }
uint32_t bt_host_task_returned(void) { return g_bt_returned; }

static void bt_task_tramp(void)
{
    g_bt_reached++;
    if (g_bt_entry) {
        g_bt_running++;
        /* Through the bridge. NOT g_bt_entry(g_bt_arg).
         *
         * The controller's task function is WINDOWED blob code and this
         * trampoline is call0, so calling it directly is the ABI crossing
         * vendor/phy/README.md describes -- and the symptom is not a clean
         * fault. The first window overflow in the callee spills relative to
         * the caller's sp fetched from [a1-12], a slot only a windowed
         * prologue writes; a call0 frame leaves whatever was there, the spill
         * goes to a wild address, and the second fault vectors to the
         * double-exception handler. No panic, no output, nothing.
         *
         * That is what the board was doing for thirty seconds: the controller
         * blocked in esp_bt_controller_init waiting for this task to answer
         * its queue, and this task had already died silently on its first
         * windowed call. The trace ended on xQueueSemaphoreTake and looked
         * like a deadlock in the semaphore, which was the wrong end of it.
         *
         * rom_call4 rather than phy_stack_call: it builds the windowed base
         * frame on the CURRENT stack, and this task already has its own
         * BT_TASK_WORDS of it. phy_stack_call would move the task onto the
         * shared 6 KB _phy_stack, which phyinit.c pins the scheduler to
         * protect -- and a task that blocks (this one does, constantly) must
         * never be switched away from with its sp on that buffer. */
        rom_call4((uint32_t)g_bt_entry, (uint32_t)g_bt_arg, 0, 0, 0);
        g_bt_returned++;
    }
    /* The controller's task is not meant to return. If it does, sleeping
     * forever is the containable answer: this kernel has nowhere to return. */
    for (;;) {
        task_sleep(100u);
    }
}

uint32_t bt_host_task_create(uint32_t *a)
{
    /* a = { fn, arg, stack_bytes, prio, name } */
    if (g_bt_task_made || !a || !a[0]) {
        return 0;
    }
    if (a[2] > BT_TASK_WORDS * 4u) {
        uart_puts("   [bt] task wants ");
        uart_put_dec(a[2]);
        uart_puts(" bytes of stack, have ");
        uart_put_dec(BT_TASK_WORDS * 4u);
        uart_puts(" -- refused\n");
        return 0;               /* a short stack is a corruption, not a slowdown */
    }
    g_bt_entry = (void (*)(void *))a[0];
    g_bt_arg   = (void *)a[1];
    int id = task_create_with_stack("btctrl", bt_task_tramp,
                                    g_bt_stack, BT_TASK_WORDS);
    if (id < 0) {
        return 0;
    }
    g_bt_task_made = 1;
    return (uint32_t)(id + 1);  /* non-zero handle */
}

/* ---- event groups, one argument wide ---------------------------------------
 *
 * osi_impl_evt_wait takes five arguments and the windowed bridge carries three,
 * so the caller packs them. */
uint32_t bt_host_evt_wait(uint32_t *a)
{
    if (!a) {
        return 0;
    }
    return osi_impl_evt_wait((void *)a[0], a[1], (int)a[2], (int)a[3], a[4]);
}

/* ---- critical sections ------------------------------------------------------
 *
 * crit_enter() returns the previous interrupt state and crit_exit() needs it
 * back; FreeRTOS's vPortExitCritical carries no such value. So the state is
 * kept here, one level deep, which is all the controller uses -- and deeper
 * nesting is counted rather than silently losing the outer state. */
static uint32_t g_crit_state;
static uint32_t g_crit_depth;
static uint32_t g_crit_overflow;

/* crit_enter() is a static inline in critical.h, so it has no ADDRESS -- and
 * the windowed bridge works by taking one. These two wrappers exist to have
 * addresses, which is the whole of their reason for being. */
uint32_t bt_crit_enter(void)
{
    uint32_t state = crit_enter();
    if (g_crit_depth == 0u) {
        g_crit_state = state;
    } else {
        g_crit_overflow++;
    }
    g_crit_depth++;
    return 0;
}

void bt_crit_exit(void)
{
    if (g_crit_depth == 0u) {
        return;
    }
    g_crit_depth--;
    if (g_crit_depth == 0u) {
        crit_exit(g_crit_state);
    }
}

uint32_t bt_host_crit_overflows(void) { return g_crit_overflow; }

/* ---- interrupt masks and a microsecond delay ------------------------------- */

/* ---- which lines this kernel is ALLOWED to enable ---------------------------
 *
 * The controller calls xt_ints_on(mask) with its own idea of which CPU lines
 * matter, and this used to enable every bit of it. The result, the moment the
 * controller's task got far enough to turn its interrupts on:
 *
 *     *** KERNEL PANIC ***  exccause 4 (Level1Interrupt)  epc 0x4008ebed
 *
 * nat-os dispatches LEVEL 3 ONLY, from _handler_level3. A level-1 line that is
 * enabled with nothing behind it is not a lost interrupt, it is a panic on the
 * first arrival -- the same wall the original BT_LINES hit when it contained
 * lines 5 and 7.
 *
 * So the mask is filtered to the level-3 lines, and what was dropped is
 * recorded. On the ESP32 those are 11, 15, 22, 23, 27 and 29; 15, 23 and 27
 * belong to the tick, GPIO and the WiFi MAC, which leaves the three BT may
 * use. Enabling a line the kernel cannot service is strictly worse than not
 * enabling it: one loses an interrupt, the other loses the board. */
#define BT_ENABLE_OK ((1u << 11) | (1u << 22) | (1u << 29))

static uint32_t g_mask_refused;         /* bits asked for and not granted */
static uint32_t g_mask_calls;

uint32_t bt_host_mask_refused(void) { return g_mask_refused; }
uint32_t bt_host_mask_calls(void)   { return g_mask_calls; }

void intr_enable_mask(uint32_t mask)
{
    uint32_t allow = mask & BT_ENABLE_OK;
    uint32_t deny  = mask & ~BT_ENABLE_OK;

    g_mask_calls++;
    if (deny) {
        if ((g_mask_refused | deny) != g_mask_refused) {
            /* Only the first time each bit is seen: the controller calls this
             * repeatedly and a print per call would bury the run. */
            uart_puts("   [bt] refusing to enable lines 0x");
            uart_put_hex(deny);
            uart_puts(" -- not level 3, this kernel would panic on arrival\n");
        }
        g_mask_refused |= deny;
    }
    if (allow) {
        xt_set_intenable(xt_get_intenable() | allow);
    }
}

void intr_disable_mask(uint32_t mask)
{
    xt_set_intenable(xt_get_intenable() & ~mask);
}

void bt_host_delay_us(uint32_t us)
{
    uint32_t start = xt_ccount();
    uint32_t want = us * 80u;           /* 80 MHz, per the clock this runs at */
    while ((xt_ccount() - start) < want) {
    }
}
