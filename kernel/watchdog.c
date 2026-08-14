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

unsigned int watchdog_rtc_config(void)
{
    return REG(RTC_CNTL_WDTCONFIG0);
}
