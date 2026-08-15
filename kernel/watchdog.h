/* cyd-os — watchdog control. See watchdog.c for why these are disabled
 * rather than fed, and what has to change before re-enabling them. */
#ifndef CYDOS_WATCHDOG_H
#define CYDOS_WATCHDOG_H

/* Disable the RTC watchdog and both timer-group watchdogs. Call early in
 * kmain, before anything that might take longer than the boot watchdog's
 * window. */
void watchdog_disable_all(void);

/* Read back the RTC watchdog config register — non-zero means still armed.
 * Used to confirm the write actually took, rather than assuming it. */
unsigned int watchdog_rtc_config(void);

/* ---- hang detection ----------------------------------------------------
 *
 * Arms TIMG0's watchdog to reset the system after `ms` without a feed.
 *
 * UM-CYDOS-009 §8 planned to feed this from the idle task. That plan is no
 * longer sound: with priorities, idle is only selected when nothing else is
 * runnable, and the application host never sleeps — so idle may never run on a
 * perfectly healthy system, and feeding from there would reset a working
 * kernel.
 *
 * The liveness signal used instead is the scheduler switching between DISTINCT
 * tasks. That is precisely what every hang in this project destroyed: the
 * task_yield freeze stopped the tick, the LOOP defect pinned the scheduler to
 * one task, and a stuck DMA wait would monopolise the display task. All three
 * present as "no distinct switches", and none of them present as "idle stopped
 * running".
 *
 * Compile-time disable is deliberate: a watchdog whose feeder is wrong reboots
 * a working system, and that must be recoverable without a debugger. */
#define WATCHDOG_ENABLE 1

void watchdog_arm(unsigned int ms);
void watchdog_feed(void);

/* Called from the scheduler once per tick. Feeds only if the system has
 * switched between distinct tasks since the last check. */
void watchdog_liveness(int switched);

unsigned int watchdog_feeds(void);
unsigned int watchdog_starved(void);

#endif /* CYDOS_WATCHDOG_H */
