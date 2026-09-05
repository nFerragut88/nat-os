/* nat-os — one blocking job at a time. See job.h for why this exists. */

#include "job.h"
#include "timer.h"

/* volatile: submitted from the touch task, run and cleared on the worker.
 * Step 303 lost updates across exactly this boundary with a plain int. */
static volatile job_fn      g_fn;
static volatile const char *g_name = "";
static volatile uint32_t    g_t0;
static volatile int         g_running;
static volatile uint32_t    g_seq;

int job_submit(job_fn fn, const char *name)
{
    if (!fn)                { return -1; }
    if (g_fn || g_running)  { return -1; }   /* one at a time -- see job.h */
    g_name = name ? name : "working";
    g_t0   = timer_ticks();
    g_fn   = fn;                             /* published LAST: the worker
                                              * tests this, so everything it
                                              * will read is already set */
    return 0;
}

int         job_busy(void)    { return (g_fn != 0) || g_running; }
const char *job_name(void)    { return job_busy() ? (const char *)g_name : ""; }
uint32_t    job_elapsed(void) { return job_busy() ? (timer_ticks() - g_t0) : 0u; }
uint32_t    job_seq(void)     { return g_seq; }

void job_service(void)
{
    job_fn fn = g_fn;
    if (!fn) { return; }

    /* Cleared BEFORE the call, and g_running set, so that a job which itself
     * submits work does not deadlock against its own slot -- and so that a job
     * which faults leaves the queue empty rather than a permanently occupied
     * one. Step 289's guard never expired; this one cannot outlive its call. */
    g_fn      = 0;
    g_running = 1;

    fn();

    g_running = 0;
    g_seq++;                /* completion is observable without polling state */
}
