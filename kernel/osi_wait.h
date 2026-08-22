/* nat-os — how long a "forever" OSI wait actually waits.
 *
 * ---- why this file exists ------------------------------------------------
 *
 * `OSI_FOREVER_CAP` was `400u` in kernel/wifi_osi_impl.c, commented "~4 s at
 * the current tick", and the unit was correct there: the loop it bounds counts
 * ticks, which is why the same variable is compared against `ticks` two lines
 * above it.
 *
 * Step 111 rebuilt the blocking path in windowed code and carried the digits
 * across without the unit. The stub's loop counts spins of 120000 cycles, not
 * ticks, so `rounds >= 400u` meant 600 ms where the constant it was copied from
 * meant 4 s -- the same number, a 6.7x shorter wait, and a comment on the copy
 * claiming it was "unchanged in meaning". Nothing surfaced it because the
 * driver was waiting on a queue nothing could ever post to, where no timeout is
 * distinguishable from any other.
 *
 * It became a second copy as well as a wrong one: after step 111 the blocking
 * path no longer reads `OSI_FOREVER_CAP`, so changing that constant no longer
 * changes the blocking path. That is the exact knob step 103 turned to prove
 * `excvaddr` was `spent` (UM-NATOS-042 section 7). The experiment would now read
 * as a null result for a reason that has nothing to do with the hypothesis.
 *
 * So the wait is expressed once, in milliseconds -- the only unit that means
 * anything to the driver being waited on -- and each counter derives its own
 * bound from it. A period change in either loop now moves both bounds, and a
 * duration change moves neither loop's period.
 *
 * ---- what may live here --------------------------------------------------
 *
 * Macros only. This header is included by call0 kernel code AND by windowed
 * vendor code in vendor/windowed/, where a call0 static inline would be an ABI
 * boundary crossing with no bridge -- the bit-31 fault this project has hit
 * four times. Macros have no ABI. Nothing else may be added here.
 */
#ifndef NATOS_OSI_WAIT_H
#define NATOS_OSI_WAIT_H

/* The one number with a meaning. Everything else is derived. */
#define OSI_FOREVER_CAP_MS   4000u

#define OSI_CPU_HZ           80000000u
#define OSI_CYCLES_PER_MS    (OSI_CPU_HZ / 1000u)

/* The two loop periods. OSI_TICK_CYCLES must equal kmain.c's
 * TICK_INTERVAL_CYCLES; kmain.c asserts that rather than trusting it. */
#define OSI_TICK_CYCLES      800000u
#define OSI_SPIN_CYCLES      120000u

/* `spent` in osi_impl_queue_recv() and friends -- counts ticks. */
#define OSI_FOREVER_CAP      (OSI_FOREVER_CAP_MS * OSI_CYCLES_PER_MS / OSI_TICK_CYCLES)

/* The windowed blocking path bounds itself by ELAPSED CYCLES, not by a count
 * of anything.
 *
 * It first used a round count derived from OSI_SPIN_CYCLES, on the assumption
 * that a round costs one spin. Measured, a round costs ~19 ms against a 1.5 ms
 * spin: it also runs win_spill_all(), and the task is unpinned across the wait,
 * so a scheduling round trip lands in the middle of every one. The nominal 4 s
 * was really ~51 s -- the same class of error as the ticks-for-spins mix-up
 * this header was written to end, arrived at from the other direction.
 *
 * A count cannot express a duration unless the thing counted has a fixed cost.
 * Reading the cycle counter needs no such assumption.
 *
 * Safe against ccount wraparound (~53.7 s at 80 MHz) for any cap below that,
 * because unsigned (now - start) is correct across a single wrap -- which the
 * assert below enforces. */
#define OSI_FOREVER_CAP_CYCLES  (OSI_FOREVER_CAP_MS * OSI_CYCLES_PER_MS)

/* The derivations multiply before dividing, to keep the precision that
 * (ms / 1000) * Hz throws away and that (cycles_per_ms / period) truncates to
 * zero outright. That costs headroom: past this bound the product wraps and the
 * cap silently becomes small, which is precisely the class of failure this file
 * was written to end. */
_Static_assert(OSI_FOREVER_CAP_MS <= 50000u,
               "OSI_FOREVER_CAP_MS * OSI_CYCLES_PER_MS overflows uint32");
_Static_assert(OSI_FOREVER_CAP > 0u,
               "the tick period exceeds the whole wait; the cap rounds to zero");
_Static_assert(OSI_FOREVER_CAP_MS < 53000u,
               "cap approaches the ccount wrap period; (now-start) stops being sound");

#endif
