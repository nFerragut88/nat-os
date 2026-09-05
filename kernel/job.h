/* nat-os — one blocking job at a time, off the tasks that serve the user.
 *
 * WHY THIS EXISTS. Every long operation in the wifi and web views was built by
 * hand, three times, and each rebuild reintroduced a bug the previous one had
 * already fixed:
 *
 *   step 281  the bring-up ran on the TOUCH task; ninety seconds with nothing
 *             reading the glass, and the exit button dead for all of it
 *   step 288  the sweep ran on the DISPLAY task; 400 ms a channel, and a second
 *             context inside the vendor blob
 *   step 289  a guard against a double request had no expiry, so one request
 *             that never finished disabled the view permanently
 *   step 307  the view could not tell that a bring-up was in progress, because
 *             "in progress" was not a thing anything recorded
 *
 * Every one of those is the same missing idea: **the system runs one blocking
 * job at a time, somewhere that is not the user interface, and everyone can ask
 * whether it is running and for how long.**
 *
 * ONE AT A TIME IS THE POINT, not a limitation. There is one radio, one vendor
 * blob that admits one caller (blobcall.c), and one lwIP that is single-context
 * under NO_SYS. A second concurrent job would be a bug in every case this
 * system has; refusing it here is cheaper than each caller inventing a flag to
 * prevent it, which is what g_busy, g_want_start and g_want_join were.
 *
 * JOBS ARE SYNCHRONOUS. The function runs to completion on the worker task.
 * That is what the bring-up, the join and a credential write are. An operation
 * that is genuinely asynchronous -- the web fetch, which lives in lwIP's
 * callbacks -- keeps its own state machine and is not forced through here.
 */

#ifndef NATOS_JOB_H
#define NATOS_JOB_H

#include <stdint.h>

typedef void (*job_fn)(void);

/* Queue `fn` to run on the worker task. Returns 0 if accepted, -1 if a job is
 * already queued or running.
 *
 * `name` is shown to the user, so it is a short verb phrase: "starting radio",
 * "joining". A caller that passes something else has misunderstood what it is
 * for. */
int         job_submit(job_fn fn, const char *name);

/* Is anything queued or running? This is the question g_busy was invented to
 * answer, once, for everyone. */
int         job_busy(void);

const char *job_name(void);         /* the running job's name, or "" */
uint32_t    job_elapsed(void);      /* ticks since it was submitted */

/* Bumped every time a job completes. A view can compare this against what it
 * last saw and repaint, without polling any particular piece of state -- the
 * same reason step 303 replaced a dirty flag with a sequence. */
uint32_t    job_seq(void);

/* Runs the queued job. Called from the worker task and nowhere else. */
void        job_service(void);

#endif /* NATOS_JOB_H */
