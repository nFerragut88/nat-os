/* cyd-os — watchdog control.
 *
 * The ESP32 has three watchdogs that matter at boot: one in the RTC controller
 * and one in each timer group. The second-stage bootloader arms the RTC
 * watchdog so a kernel that hangs during startup gets rebooted rather than
 * sitting dead. It expects the application to take ownership — feed it, or turn
 * it off.
 *
 * cyd-os did neither for three milestones. The consequence was RTCWDT_RTC_RESET
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
        }
    }
#else
    (void)switched;
#endif
}

unsigned int watchdog_feeds(void)   { return g_feeds; }
unsigned int watchdog_starved(void) { return g_starved; }

unsigned int watchdog_rtc_config(void)
{
    return REG(RTC_CNTL_WDTCONFIG0);
}
