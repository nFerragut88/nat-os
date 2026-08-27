/* nat-os — watchdog control.
 *
 * The ESP32 has three watchdogs that matter at boot: one in the RTC controller
 * and one in each timer group. The second-stage bootloader arms the RTC
 * watchdog so a kernel that hangs during startup gets rebooted rather than
 * sitting dead. It expects the application to take ownership — feed it, or turn
 * it off.
 *
 * nat-os did neither for three milestones. The consequence was RTCWDT_RTC_RESET
 * roughly every second once M2 kept the CPU busy, which presented as a "stuck
 * scheduler" and cost three build cycles chasing bugs that did not exist. The
 * reset reason said so on the first line of every boot.
 *
 * Each watchdog's configuration register is write-protected: the key must be
 * written to the protect register first, and re-locking afterwards prevents a
 * stray store from silently re-enabling one.
 *
 * These are disabled outright rather than fed. A watchdog is only useful once
 * something is responsible for feeding it; until the scheduler owns that duty,
 * an armed watchdog reboots working code. Re-enable it in M3 or later, with the
 * idle task feeding it — at which point it becomes a real hang detector.
 */

#include "watchdog.h"
#include "uart.h"
#include "task.h"
#include "blobcall.h"
#include "timer.h"

#define REG(a) (*(volatile unsigned int *)(a))

/* Write-protect key, shared by all three watchdog blocks. */
#define WDT_WKEY               0x50D83AA1u

#define RTC_CNTL_WDTCONFIG0    0x3FF4808Cu
#define RTC_CNTL_WDTWPROTECT   0x3FF480A4u

#define TIMG0_WDTCONFIG0       0x3FF5F048u
#define TIMG0_WDTWPROTECT      0x3FF5F064u

#define TIMG1_WDTCONFIG0       0x3FF60048u
#define TIMG1_WDTWPROTECT      0x3FF60064u

/* TIMG0 watchdog, used as the hang detector. Chosen over the RTC watchdog
 * because its timeout is expressed in APB-clock ticks through a prescaler,
 * which is a number this kernel already knows, rather than in RTC slow-clock
 * cycles whose frequency is only approximately known. */
#define TIMG0_WDTCONFIG1       0x3FF5F04Cu   /* prescaler            */
#define TIMG0_WDTCONFIG2       0x3FF5F050u   /* stage 0 timeout      */
#define TIMG0_WDTFEED          0x3FF5F060u

#define WDT_EN                 (1u << 31)
#define WDT_STG0_RESET_SYSTEM  (3u << 29)
#define WDT_SYS_RESET_LEN      (7u << 15)
#define WDT_CPU_RESET_LEN      (7u << 18)

/* [step 264] A BREADCRUMB THAT SURVIVES THE RESET.
 *
 * A watchdog reset is the one failure in this kernel that leaves nothing --
 * no exception, no register dump, no LAST FAULT record, just a reboot. Step
 * 263 measured it at 3 runs in 8, so it is the blocker for everything, and
 * four steps of argument about it have produced no mechanism because every
 * observation died with the board.
 *
 * RTC slow memory does not die with it. TG0WDT_SYS_RESET resets the digital
 * core; the RTC domain keeps its contents. Three words written per tick cost
 * nothing and are readable on the next boot.
 *
 * The magic is checked so an unprogrammed or power-cycled RTC reads as absent
 * rather than as garbage -- the same reasoning blob_map() uses for a flash
 * region that was never written. */
#define RTC_SLOW_MEM  0x50000000u
#define BC_MAGIC      0x6E617462u        /* 'natb' */

/* [step 265] hist packs the EIGHT most recent task ids, four bits each, most
 * recent in the low nibble. Step 264 showed the tick landing in disp three
 * times of three, and could not tell a monopoly from a coincidence of timing:
 * a HIGH-priority task that draws continuously is what a naive prior already
 * predicts. Eight in a row all reading 6 is a monopoly. A mixture is not.
 *
 * lock is who holds the panel mutex, which is the thing UM-NATOS-029 measured
 * the display holding for essentially all of uptime. */
/* [step 267] crit is the blob's critical-section depth. Until this step the
 * blob's _wifi_int_disable was a stub, so it had none; now it does, and a
 * reset with a NON-ZERO depth says the blob entered a critical region and the
 * board died inside it. */
struct bc { unsigned int magic, seq, task, tick, hist, lock, crit; };
static volatile struct bc *const g_bc = (volatile struct bc *)RTC_SLOW_MEM;

/* The PREVIOUS boot's final breadcrumb, snapshotted before this boot
 * overwrites it. */
static struct bc g_bc_prev;
static int g_bc_had_prev;

void watchdog_breadcrumb_init(void);
void watchdog_breadcrumb_init(void)
{
    if (g_bc->magic == BC_MAGIC) {
        g_bc_prev.seq  = g_bc->seq;
        g_bc_prev.task = g_bc->task;
        g_bc_prev.tick = g_bc->tick;
        g_bc_prev.hist = g_bc->hist;
        g_bc_prev.lock = g_bc->lock;
        g_bc_prev.crit = g_bc->crit;
        g_bc_had_prev  = 1;
    }
    g_bc->magic = BC_MAGIC;
    g_bc->seq = 0u;
    g_bc->task = 0xFFFFFFFFu;
    g_bc->tick = 0u;
    g_bc->hist = 0u;
    g_bc->lock = 0xFFFFFFFFu;
    g_bc->crit = 0u;
}

int watchdog_breadcrumb_prev(unsigned int *seq, unsigned int *task,
                             unsigned int *tick);
int watchdog_breadcrumb_prev(unsigned int *seq, unsigned int *task,
                             unsigned int *tick)
{
    if (!g_bc_had_prev) { return 0; }
    *seq = g_bc_prev.seq; *task = g_bc_prev.task; *tick = g_bc_prev.tick;
    return 1;
}

unsigned int watchdog_breadcrumb_hist(void);
unsigned int watchdog_breadcrumb_hist(void) { return g_bc_prev.hist; }
unsigned int watchdog_breadcrumb_lock(void);
unsigned int watchdog_breadcrumb_lock(void) { return g_bc_prev.lock; }
unsigned int watchdog_breadcrumb_crit(void);
unsigned int watchdog_breadcrumb_crit(void) { return g_bc_prev.crit; }

static unsigned int g_feeds;
static unsigned int g_starved;

static void disable_one(unsigned int wprotect, unsigned int config0)
{
    REG(wprotect) = WDT_WKEY;   /* unlock */
    REG(config0)  = 0;          /* clear enable and every stage action */
    REG(wprotect) = 0;          /* re-lock */
}

void watchdog_disable_all(void)
{
    disable_one(RTC_CNTL_WDTWPROTECT, RTC_CNTL_WDTCONFIG0);
    disable_one(TIMG0_WDTWPROTECT,    TIMG0_WDTCONFIG0);
    disable_one(TIMG1_WDTWPROTECT,    TIMG1_WDTCONFIG0);
}

void watchdog_arm(unsigned int ms)
{
#if WATCHDOG_ENABLE
    REG(TIMG0_WDTWPROTECT) = WDT_WKEY;

    /* APB is 80 MHz; a prescaler of 40,000 gives a 2 kHz tick, so the timeout
     * is milliseconds times two. Chosen so the timeout fits comfortably in the
     * 32-bit stage register at any duration worth using. */
    REG(TIMG0_WDTCONFIG1) = 40000u << 16;
    REG(TIMG0_WDTCONFIG2) = ms * 2u;

    REG(TIMG0_WDTCONFIG0) = WDT_EN | WDT_STG0_RESET_SYSTEM |
                            WDT_SYS_RESET_LEN | WDT_CPU_RESET_LEN;

    REG(TIMG0_WDTFEED)     = 1u;
    REG(TIMG0_WDTWPROTECT) = 0;
#else
    (void)ms;
#endif
}

void watchdog_disarm(void)
{
#if WATCHDOG_ENABLE
    REG(TIMG0_WDTWPROTECT) = WDT_WKEY;
    REG(TIMG0_WDTCONFIG0)  = 0;
    REG(TIMG0_WDTWPROTECT) = 0;
#endif
}

void watchdog_feed(void)
{
#if WATCHDOG_ENABLE
    REG(TIMG0_WDTWPROTECT) = WDT_WKEY;
    REG(TIMG0_WDTFEED)     = 1u;
    REG(TIMG0_WDTWPROTECT) = 0;
    g_feeds++;
#endif
}

/* Called every tick from the scheduler. `switched` is non-zero if this tick
 * resumed a DIFFERENT task than the one it interrupted.
 *
 * The window is deliberately much shorter than the watchdog timeout: a healthy
 * system switches many times a second, so requiring one switch per window is a
 * low bar that only a genuine monopoly fails. */
#define LIVENESS_WINDOW_TICKS 100u      /* ~1 s */

void watchdog_liveness(int switched)
{
#if WATCHDOG_ENABLE
    /* [step 264] Three stores per tick. Whatever the board was running when
     * the watchdog fired is in RTC memory on the next boot. */
    {
        extern int display_lock_owner(void);
        unsigned int me = (unsigned int)task_current();
        g_bc->seq++;
        g_bc->task = me;
        g_bc->tick = (unsigned int)timer_ticks();
        g_bc->hist = (g_bc->hist << 4) | (me & 0xFu);
        g_bc->lock = (unsigned int)display_lock_owner();
        {
            extern unsigned int g_wint_depth;
            g_bc->crit = g_wint_depth;
        }
    }

    static unsigned int ticks;
    static unsigned int seen;

    if (switched) {
        seen++;
    }

    if (++ticks >= LIVENESS_WINDOW_TICKS) {
        ticks = 0;
        if (seen) {
            seen = 0;
            watchdog_feed();
        } else {
            /* No distinct switch for a whole window. Deliberately does NOT
             * feed: the watchdog is the only thing that can recover this, and
             * feeding on the way past would defeat the entire mechanism. */
            g_starved++;
            /* [step 264] Step 259 SAY WHO is REMOVED. It answered its
             * question -- starved stayed 0 across a reset, so this is not
             * a monopoly -- and the RTC breadcrumb written every tick says
             * more, earlier, and survives the reset. iram paid for the swap.
             */
        }
    }
#else
    (void)switched;
#endif
}

unsigned int watchdog_feeds(void)   { return g_feeds; }
unsigned int watchdog_starved(void) { return g_starved; }

/* [step 259] The TIMG0 watchdog's OWN configuration, read back.
 *
 * Feeds were advancing and starved was 0 in the run that reset, so the hang
 * detector was doing its job and the watchdog fired anyway. That points at
 * the register rather than the scheduler: TIMG0 is Espressif hardware and the
 * blob is Espressif code, and nothing stops it reconfiguring a timer group
 * nat-os also uses. A pure read; if this ever differs from what
 * watchdog_arm() wrote, somebody else owns the watchdog. */
unsigned int watchdog_timg0_config(void) { return REG(TIMG0_WDTCONFIG0); }
unsigned int watchdog_timg0_timeout(void) { return REG(TIMG0_WDTCONFIG2); }

unsigned int watchdog_rtc_config(void)
{
    return REG(RTC_CNTL_WDTCONFIG0);
}
