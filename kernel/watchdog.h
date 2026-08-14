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

#endif /* CYDOS_WATCHDOG_H */
