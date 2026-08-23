/* nat-os — native task control and round-robin scheduling.
 *
 * Switching happens inside the level-3 timer interrupt. The handler saves the
 * full context onto the interrupted task's own stack, hands the stack pointer
 * to task_schedule(), and resumes on whatever stack it gets back. Because the
 * frame carries EPC3 and EPS3, restoring it restores the return address and
 * processor state of a *different* task — which is the whole trick.
 *
 * A new task is started by fabricating a frame that looks exactly as though it
 * had been interrupted at its entry point. No special "first switch" path is
 * needed, and the boot context needs no fabrication at all: its stack pointer
 * arrives as the argument on the first call.
 */

#include "task.h"
#include "window.h"
#include "timer.h"
#include "uart.h"
#include "critical.h"
#include "watchdog.h"
#include "blobcall.h"
#include "panic.h"
#include "xtensa.h"

/* Written into the lowest stack word; if it changes, the task overflowed. */
#define STACK_GUARD 0x57ACC0DEu

/* Fill pattern, so untouched stack is distinguishable from used stack and
 * headroom can be measured rather than guessed. */
#define STACK_FILL  0xEEEEEEEEu

_Static_assert(TASK_FRAME_BYTES >= TASK_FRAME_WORDS * 4,
               "frame must hold every saved word");
_Static_assert((TASK_FRAME_BYTES % 16) == 0,
               "Xtensa requires a 16-byte aligned stack");

/* The task table, fenced.
 *
 * next_moves/08 step 62: g_tasks[5].sp holds a value the scheduler never saved,
 * and after creation the scheduler is the only thing that assigns it. So
 * something outside writes here. One struct rather than three objects, because
 * separate statics may be placed anywhere and the point is adjacency. */
static struct {
    uint32_t lo;
    task_t   t[TASK_MAX];
    uint32_t hi;
} g_ttab = { 0xA5A5A5A5u, {{0}}, 0x5A5A5A5Au };

#define g_tasks (g_ttab.t)

volatile uint32_t g_ttab_lo_seen, g_ttab_hi_seen;   /* first clobbered value */
volatile int      g_ttab_side = -1;                 /* 0 = below, 1 = above */
static uint32_t g_stacks[TASK_MAX][TASK_STACK_WORDS];

/* Scratch for _handler_level3's window-state restore: the frame pointer has to
 * live somewhere base-independent while WINDOWBASE is written. See vectors.S. */
volatile uint32_t g_switch_sp;

/* First task whose saved sp fell outside its own stack. See task_schedule(). */
volatile int      g_phytop_task = -1;   /* who was saved at _phy_stack_top */
volatile uint32_t g_phytop_epc, g_phytop_a0;

static uint32_t   g_out_ws[TASK_MAX], g_out_base[TASK_MAX];

/* [step 115] Spill-on-preemption outcome. g_pspill_pre_ws is what the task held
 * when the tick landed; g_pspill_post_ws is what the sweep left. The design says
 * post must always be a single bit, and that bit the task's own base. */
volatile uint32_t g_pspill_count, g_pspill_pre_ws, g_pspill_post_ws, g_pspill_worst;
volatile int      g_pspill_task = -1;
volatile uint32_t g_pspill_bad;      /* sweeps that did NOT reduce to one frame */

/* [step 118] The sweep's own output, audited.
 *
 * Steps 115-117 all judged the sweep by whether wintorture passed. It does not
 * pass, and pass/fail cannot say WHICH save area is wrong. This walks the chain
 * the sweep just wrote, from the task's stack pointer upward, and checks each
 * frame's a0 is a windowed return encoding -- the same walk step 112 used on the
 * blocking path, which settled in one run what three steps of inference had not.
 *
 * Latched on the FIRST sweep, so it describes the event rather than the last of
 * many. */
volatile uint32_t g_pspill_have, g_pspill_walked, g_pspill_badframes;
volatile uint32_t g_pspill_bad_a0, g_pspill_bad_at, g_pspill_sp, g_pspill_wb;

/* [step 119] Where did the seven frames actually GO?
 *
 * The borrowed-stack design rests on one claim: each frame overflows through
 * the a1 held in ITS OWN window, so parents write to the task's stack wherever
 * the sweeping context's a1 points. Step 118 showed the chain from the task's
 * sp reaches none of them, so the claim is what gets tested.
 *
 * bs_enc / bs_sp census the borrowed stack for windowed return encodings and
 * for words that look like task stack pointers. If the parents landed there,
 * the premise is refuted and the borrowed stack is precisely the wrong idea.
 *
 * link / a0slot are [task_sp-12] and [task_sp-16] raw -- the words the restore's
 * first underflow will read -- so "never written" and "written wrong" can be
 * told apart instead of inferred. */
volatile uint32_t g_pspill_bs_enc, g_pspill_bs_sp, g_pspill_link, g_pspill_a0slot;

/* [step 123] The interrupted a1 as the machine had it, written by
 * _handler_level3's first instruction, plus what the frame arithmetic claims.
 * If these agree, `current_sp + TASK_FRAME_BYTES` is sound and three steps of
 * assumption are retired; if they differ, every walk that started there was
 * reading the wrong address. */
volatile uint32_t g_ih_a1_raw, g_ih_a1_calc, g_ih_ws, g_ih_wb, g_ih_bitset;
volatile uint32_t g_ih_a1_latched;
/* [step 141] The `frames :` line predicts the grant by recomputing what
 * _handler_level3 does. That duplication is not optional -- the vector has no
 * room to walk a task table -- but it is drift-prone, and step 140 caught it
 * drifting: the handler was changed to take the grant from the frame and this
 * kept reporting `granted 0x00000008`, a number nothing had written.
 *
 * So the prediction is now CHECKED. The handler already records the grant it
 * actually wrote, in g_rin_ws. One switch later, that value is compared against
 * what was predicted for the same task. A mismatch names the task and both
 * numbers, and means this file's model of the restore has diverged from the
 * restore -- which makes every `frames :` line since the divergence fiction.
 *
 * Lagged by one switch because g_rin_ws is written by the handler AFTER
 * task_schedule returns. Comparing it in the same call would compare this
 * event's prediction against the previous event's grant, which is the mistake
 * step 135 made and step 123 made before it. */
static int      g_pred_task = -1;
static uint32_t g_pred_grant;
volatile int      g_grant_drift_task = -1;
volatile uint32_t g_grant_drift_pred, g_grant_drift_real;

volatile int      g_lost_task = -1;
volatile uint32_t g_lost_had, g_lost_grant, g_lost_bits;

static uint32_t   g_term_addr[TASK_MAX], g_term_val[TASK_MAX];
volatile int      g_term_hit = -1, g_term_by = -1;
volatile uint32_t g_term_was, g_term_now;

volatile uint32_t g_ovlp_seen, g_ovlp_frame, g_ovlp_slot;
volatile int      g_ovlp_task = -1;

volatile int      g_a0bad_out_task = -1, g_a0bad_in_task = -1;
volatile uint32_t g_a0bad_out_val, g_a0bad_in_val;

volatile int      g_badsp_task = -1;
volatile uint32_t g_badsp_val, g_badsp_lo, g_badsp_hi;
volatile uint32_t g_badsp_osi = 0xFFFFFFFFu, g_badsp_tick;
volatile uint32_t g_badsp_eps;

/* ---- who owns which window position ------------------------------------
 *
 * next_moves/08 steps 49-50. WINDOWSTART is 16 bits of "a frame lives here"
 * with no owner field, and nat-os needs owners because more than one task holds
 * frames in the register file at once: a task pinned inside windowed code keeps
 * its frames live while every other task is switched away from and resumed
 * around it.
 *
 * Assignment on restore destroys those frames. OR never clears, so bits
 * accumulate until one names a frame `entry` never created. Both were measured.
 * The only way out is to record what the hardware does not.
 *
 * A task is the ONLY thing that can change WINDOWSTART while it runs, so the
 * bits it owns are whatever is set at its switch-out that no other task has
 * claimed. The union of every task's mask is then the true set of live frames,
 * and a restore may assign exactly that -- keeping other tasks' frames and
 * dropping bits nobody owns. */
static uint32_t g_win_mask[TASK_MAX];
static uint8_t  g_win_base[TASK_MAX];   /* the base each claim was made at */
volatile uint32_t g_win_union;          /* read by _handler_level3 */

/* [step 138] Set by the WINDOWSTART wipes in window.S -- phy_stack_call and
 * x20_windowed -- which overwrite registers belonging to other tasks' frames
 * and then disown the bits. Without this, g_win_mask still claims those frames
 * and the restore hands them back: a bit for a window whose contents are gone,
 * which is exactly step 124's phantom. */
volatile uint32_t g_win_disowned;
uint32_t g_disown_hits;

/* [X5 experiment] last multi-frame sighting at a park point. See
 * spill_before_parking. 0xFFFFFFFF = never seen (WS is 16-bit, WB <= 15). */
/* [X8 DIAGNOSTIC CLAMP -- NOT A FIX] count of parks where the forced
 * win_spill_call0() was skipped. Every skip is also a full sbp sighting, so
 * skipped <= sightings. Compared against the unmodified-image death
 * signature (tick 463, epc 0x4008b8af, IllegalInstruction) to discriminate
 * H-A (sweep corrupts genuinely multi-frame windowed tasks) from H-B
 * (independent wake-path/osi-glue defect). */
volatile uint32_t g_sbp_ws = 0xFFFFFFFFu;
volatile uint32_t g_sbp_wb = 0xFFFFFFFFu;
volatile int      g_sbp_task = -1;
volatile uint32_t g_sbp_skipped = 0;

/* [X8 experiment] window state the sweep LEFT BEHIND at the last multi-frame
 * park. A healthy sweep reduces ws to a single bit at wb; anything else
 * quantifies the failure shape per task. */
volatile uint32_t g_sbp_post_ws = 0xFFFFFFFFu;
volatile uint32_t g_sbp_post_wb = 0xFFFFFFFFu;

/* [X7 experiment] non-static mirror of g_current, written at the single
 * assignment in task_schedule. The save-path ring sampler reads this to
 * record which task each sampled window state belongs to; g_current itself
 * is static and has no symbol assembly can name. */
volatile int g_dbg_current = -1;

uint32_t task_win_union(void) { return g_win_union; }
uint32_t task_win_mask(int id) { return (id >= 0 && id < TASK_MAX) ? g_win_mask[id] : 0u; }
uint32_t task_win_base(int id) { return (id >= 0 && id < TASK_MAX) ? g_win_base[id] : 0u; }

/* Tasks switched away from with more than one live windowed frame. See
 * task_schedule(). Zero is the design; anything else is the bug. */
static uint32_t g_multiframe_count, g_multiframe_worst, g_multiframe_ws;
static int      g_multiframe_task = -1;

uint32_t task_multiframe_count(void) { return g_multiframe_count; }
uint32_t task_multiframe_worst(void) { return g_multiframe_worst; }
uint32_t task_multiframe_ws(void)    { return g_multiframe_ws; }
int      task_multiframe_task(void)  { return g_multiframe_task; }

/* [X11] EXCM contamination watch. A stored resume PS with EXCM set makes the
 * first windowed instruction (entry/retw) trap IllegalInstruction on resume.
 * Interrupt entry sets EXCM in the live ps; any save path that copies THAT
 * value into EPS3 instead of a clean ps poisons its victim. Zero is the
 * design; anything else names the moment of contamination. */
volatile uint32_t g_excm_count;
volatile int      g_excm_task = -1;
volatile uint32_t g_excm_seq, g_excm_ps;

/* [X13] WOE-clear watch. entry/retw trap IllegalInstruction with WOE clear
 * exactly as with EXCM set; the X11 watch only covered bit 4. A stored resume
 * ps without bit 18 names the same class of poison from the other side. */
volatile uint32_t g_woec_count;
volatile int      g_woec_task = -1;
volatile uint32_t g_woec_seq, g_woec_ps;

/* [X12] .text integrity watch over the window-bridge helpers.
 *
 * Cause-0 traps have landed on opcodes that are legal and aligned in the ELF
 * image (w2c_call1+3 s32i.n twice-built, w2c_call2+0x17 retw.n twice). A legal
 * opcode cannot trap IllegalInstruction under any live ps state -- windowed
 * instructions trap only via WOE/EXCM and plain stores never do -- so the
 * leading explanation is that IRAM no longer holds what the loader copied:
 * something rewrites kernel .text at runtime. Snapshot the region on the first
 * scheduler pass and re-compare every tick; a mismatch names the writer's
 * reach directly. Region is symbol-derived so it survives address shifts. */
extern uint32_t rom_call4(uint32_t fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
#define TXT_WATCH_WORDS 128u
static uint32_t g_txt_base[TXT_WATCH_WORDS];
static int      g_txt_ready;
volatile uint32_t g_txt_bad_ticks;
volatile uint32_t g_txt_seq, g_txt_exp, g_txt_act;
volatile int      g_txt_off = -1;

/* -1 means "no task is running yet": the boot context is about to be abandoned
 * and its stack pointer must NOT be saved, because doing so would overwrite the
 * fabricated frame of whichever task occupies that slot.
 *
 * An earlier design adopted the boot context as task 0 instead, capturing its
 * stack pointer on the first interrupt. That created two ways for a task to
 * come into existence — fabricated and captured — and only the fabricated path
 * worked: tasks 1 and 2 ran, task 0 never resumed. Deleting the second path was
 * cheaper than debugging it, and leaves one code path to be correct about. */
/* [step 131] Not static: the interrupt prologue indexes the Tier B register-file
 * slots by it, and task_current() is a call0 function that cannot be called from
 * there. A variable rather than an accessor is the only option, not a
 * preference. */
int g_current = -1;

/* ---- CPU time accounting ------------------------------------------------
 *
 * Cycles each task has actually RUN, as opposed to how much wall-clock passed
 * while it was trying to.
 *
 * Every timing figure in this kernel used to be a pair of xt_ccount() reads
 * around some work, which counts every tick, every other task's slice and every
 * interrupt that landed in the middle as though it were the work itself. The
 * measured cost of a full-screen fill was 43 ms before the scheduler existed
 * and 249-362 ms from a task afterwards — the same bytes to the same panel,
 * differing by eight times in nothing but bookkeeping.
 *
 * Charged at the switch, which is the only place that knows a task stopped
 * running. Time spent inside the level-3 handler is charged to whichever task
 * it interrupted; that is a small and deliberate inaccuracy, and the
 * alternative is accounting inside the ISR that measures the ISR.
 *
 * MEASURED, and the correction is not uniform. A full-screen fill from the
 * SHELL task read 249-362 ms wall-clock and 63-79 ms of work — the shell runs
 * at NORMAL and is preempted constantly, so nearly all of that was other tasks.
 * The raycaster's blit did not move at all: 55.5 ms either way, because the
 * display task runs at HIGH and is barely interrupted, so wall-clock was
 * already very close to its work.
 *
 * Which means the fix matters most exactly where the old numbers were least
 * trusted, and changes nothing where they were quietly correct. A conclusion
 * drawn from one task's timings could not have been carried to another's. */
static uint32_t g_run_cycles[TASK_MAX];
static uint32_t g_slice_start;      /* ccount when the running task got the CPU */

/* -1 until a task registers as idle. Kept out of the round robin and used only
 * when nothing else can run. */
static int g_idle_id = -1;

/* Fairness telemetry. g_max_wait is the worst wait ever observed by a task that
 * eventually ran, in ticks; g_age_rescues counts decisions that went to a task
 * only because ageing had lifted it. If the second is zero the policy is inert,
 * which is itself worth knowing. */
static uint32_t g_max_wait;

/* The TAIL, not the maximum.
 *
 * next_moves/04 asks for a histogram rather than a high-water mark, and the
 * reason is in its own text: `maxwait=36` is the worst wait ever OBSERVED,
 * which is not the worst possible and says nothing about how often a long wait
 * happens. A control loop cares about the shape of the distribution -- one
 * 36-tick wait per hour and one per second are the same number here and
 * completely different systems.
 *
 * Buckets are in ticks, matching `waiting`: 0, 1, 2-3, 4-7, 8-15, 16-31,
 * 32-63, 64+. Doubling widths because the interesting region is the tail and a
 * linear scale spends all its resolution on the head. */
#define WAIT_BUCKETS 8
static uint32_t g_wait_hist[WAIT_BUCKETS];

static uint32_t wait_bucket(uint32_t w)
{
    uint32_t b = 0;
    while (w > 1u && b < WAIT_BUCKETS - 1u) {
        w >>= 1;
        b++;
    }
    return (w == 0u) ? 0u : b + 1u < WAIT_BUCKETS ? b + 1u : WAIT_BUCKETS - 1u;
}

uint32_t task_wait_hist(uint32_t i)
{
    return (i < WAIT_BUCKETS) ? g_wait_hist[i] : 0u;
}

void task_wait_hist_reset(void)
{
    for (int i = 0; i < WAIT_BUCKETS; i++) {
        g_wait_hist[i] = 0u;
    }
    g_max_wait = 0u;
}
static uint32_t g_age_rescues;

/* Consecutive scheduler decisions spent pinned inside blob code. ~2 s at
 * 100 Hz; register_chipv7_phy, the longest known blob call, is well under. */
#define BLOB_PIN_MAX_TICKS 200u
static uint32_t g_blob_pin_ticks;
static uint32_t g_sleep_clamped;

uint32_t task_max_wait(void)    { return g_max_wait; }
uint32_t task_age_rescues(void)  { return g_age_rescues; }
uint32_t task_blob_pin_ticks(void) { return g_blob_pin_ticks; }
uint32_t task_sleep_clamped(void) { return g_sleep_clamped; }

/* Wakes any sleeping task whose deadline has passed. Called from the scheduler,
 * which runs every tick, so a sleep resolves to one tick. */
static void wake_sleepers(void)
{
    uint32_t now = timer_ticks();
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_SLEEPING &&
            (int32_t)(now - g_tasks[i].wake_tick) >= 0) {
            g_tasks[i].state = TASK_READY;
        }
    }
}

int task_create(const char *name, task_entry_fn entry)
{
    /* Slot id and pool index are the same thing here, so the pool stack cannot
     * be chosen until the slot is. task_create_with_stack() takes NULL to mean
     * "use this slot's pool stack" rather than duplicating the search. */
    return task_create_with_stack(name, entry, 0, 0u);
}

/* Seed the overflow probe's scratch so it cannot report its own uninitialised
 * state. _WindowOverflow8/12 write EXCSAVE 6/7 on every overflow; until the
 * first one they hold whatever was there at reset, and a zero satisfies the
 * "not a stack address" filter perfectly. That is what `base 0x00000000 from
 * frame sp 0x00000000` has been reporting since step 48 -- the probe firing on
 * nothing at all. See next_moves/08 step 73. */
void win_probe_seed(void);
void win_probe_seed(void)
{
    uint32_t sentinel = 0xFFFFFFFFu;
    __asm__ volatile ("wsr.excsave6 %0" :: "r"(sentinel));
    __asm__ volatile ("wsr.excsave7 %0" :: "r"(sentinel));
}

/* Where a windowed chain goes if it ever unwinds past a task's entry.
 *
 * Reaching this is a bug by construction -- a task entry must not return -- but
 * it is a bug that now announces itself at the moment it happens, instead of
 * spilling through whatever the padding at the top of the stack happened to
 * hold. See the terminator in task_create_with_stack(). */
static void task_chain_end(void)
{
    kernel_panic_msg("windowed chain unwound past a task entry", 0u);
}

int task_create_with_stack(const char *name, task_entry_fn entry,
                           uint32_t *stack_in, uint32_t words_in)
{
    for (int id = 0; id < TASK_MAX; id++) {
        if (g_tasks[id].state != TASK_UNUSED) {
            continue;
        }

        uint32_t *stack = stack_in ? stack_in : g_stacks[id];
        uint32_t  words = stack_in ? words_in : (uint32_t)TASK_STACK_WORDS;
        if (words < (uint32_t)TASK_FRAME_WORDS + 8u) {
            return -1;              /* too small to hold even the first frame */
        }

        for (uint32_t i = 0; i < words; i++) {
            stack[i] = STACK_FILL;
        }
        stack[0] = STACK_GUARD;

        /* Frame sits at the top of the stack, 16-byte aligned -- less the 16
         * bytes reserved for the windowed chain terminator.
         *
         * [step 90] Those 16 bytes are not spare. `_WindowUnderflow8` reads a
         * caller's frame from `[a9-16..a9-4]`, so terminating the chain means
         * the 16 bytes BELOW the top of the stack must hold a valid save area.
         * But the handler pops its 112-byte frame and leaves `a1 = top`, so the
         * task's own entry function then allocates downward straight through
         * that same region -- measured: task 6 zeroed its own terminator while
         * task 6 was running.
         *
         * Reserving them moves the task's usable stack down by 16 bytes, so its
         * frames end at `top-17` and the terminator below `top` is never in the
         * task's way. */
        uint32_t top = (uint32_t)&stack[words];
        top &= ~15u;
        uint32_t term_top = top;
        top -= 16u;
        uint32_t *frame = (uint32_t *)(top - TASK_FRAME_BYTES);

        for (int i = 0; i < TASK_FRAME_WORDS; i++) {
            frame[i] = 0;
        }

        /* Resume at the entry point, with interrupts admitted. PS is taken
         * from the running kernel with INTLEVEL forced to 0, so the task
         * inherits the same execution mode rather than a guessed one — the
         * ROM leaves WOE and CALLINC set, and fabricating a different PS would
         * put the task in a subtly different state from its creator. */
        frame[TASK_FRAME_IDX_EPC3] = (uint32_t)entry;
        frame[TASK_FRAME_IDX_EPS3] = xt_get_ps() & ~0xFu;
        frame[TASK_FRAME_IDX_SAR]  = 0;

        /* A new task starts with exactly one live frame -- its own -- at the
         * base its creator happens to be running at. Any base is correct for
         * call0 code, which never rotates the window; what matters is that
         * WINDOWSTART claims that one frame and nothing else, so the task does
         * not inherit a claim on frames belonging to whoever created it. */
        {
            uint32_t wb;
            __asm__ volatile ("rsr.windowbase %0" : "=r"(wb));
            frame[TASK_FRAME_IDX_WBASE]  = wb;
            /* CLAIM NOTHING.
             *
             * A new task has no windowed frames -- it starts in call0 code, and
             * the first windowed call it makes sets its own bit, because that is
             * what `entry` does. Seeding a bit here asserts a frame exists
             * before one does, and step 50 measured where that goes: the blob
             * task is created from INSIDE the driver's own excursion, so the
             * creator's base is in the middle of the creator's live window, and
             * the claim lands on somebody else's registers.
             *
             * The base is still recorded, because the restore needs somewhere to
             * put the sixteen registers; that is harmless, since every task's
             * registers travel through memory on each switch. It is the BIT that
             * was the lie. */
            frame[TASK_FRAME_IDX_WSTART] = 0u;
        }

        g_tasks[id].sp         = (uint32_t)frame;
        g_tasks[id].state      = TASK_READY;
        g_tasks[id].prio       = TASK_PRIO_NORMAL;
        g_tasks[id].base_prio  = TASK_PRIO_NORMAL;
        g_tasks[id].wake_tick  = 0;
        g_tasks[id].woken      = 0;
        g_tasks[id].switches   = 0;
        g_tasks[id].waiting    = 0;
        g_tasks[id].name       = name;
        /* TERMINATE THE WINDOWED CHAIN.
         *
         * next_moves/08 step 88. _WindowUnderflow8 recovers a caller's frame
         * with `l32e a1, a9, -12` and then dereferences it with
         * `l32e a7, a1, -12`. At the outermost frame of a task there is no
         * caller, and nothing had ever written that save area -- so the handler
         * read the padding above the initial context frame and followed it.
         * Measured: a9 exactly `top & ~15`, recovering 0x3ffd8f78 (the blob's
         * .bss) as a return address and 0x190 as a stack pointer, then faulting.
         *
         * The 16 bytes at [top-16..top-1] are frame offsets 96..111. The frame
         * proper is TASK_FRAME_WORDS = 23 words = 92 bytes, so this is padding
         * the initial frame never uses and cannot collide with.
         *
         * a1 points at the top itself, which keeps `[a1-12]` inside the task's
         * own stack and readable, so the second dereference cannot fault. a0 is
         * a well-formed windowed return encoding -- CALLINC 1, so `retw` accepts
         * it -- aimed at a trap, because a chain that unwinds this far is wrong
         * and should say so rather than continue.
         *
         * This is a GUARD, not the cure. It converts a wild read outside the
         * task into a contained, named failure; whatever makes the chain unwind
         * this far is still open (step 88, candidate 2). */
        {
            uint32_t top_addr = term_top;
            uint32_t *t = (uint32_t *)top_addr;
            t[-4] = (1u << 30) | ((uint32_t)&task_chain_end & 0x3FFFFFFFu);
            t[-3] = top_addr;      /* a1: readable, and [a1-12] is too */

            t[-2] = 0u;
            t[-1] = 0u;
            /* [step 90] Remember it, so the per-switch check below can tell
             * "somebody wrote here" from "this was never written". */
            g_term_addr[id] = top_addr;
            g_term_val[id]  = t[-4];
        }

        g_tasks[id].stack_base  = stack;
        g_tasks[id].stack_words = words;
        return id;
    }
    return -1;
}

/* ---- switch tracing -------------------------------------------------- */
/*
 * Prints the frame the handler is about to restore. Ticks 1-3 restore frames
 * fabricated by task_create; tick 4 is the first restore of a frame the
 * handler itself saved. Dumping both means the saved frame can be read against
 * a known-good fabricated one instead of against expectations.
 *
 * This runs at interrupt level 3 and blocks on the UART for the length of the
 * dump, which is far longer than a tick period. Ticks are missed as a result
 * and timer_late_count() will climb — that is expected and harmless here,
 * because the question is what the frame CONTAINS, not when it arrives.
 */
/* Switch tracing. Set to 0 for a quiet boot; raise it to watch the first N
 * switches when touching the handler or the frame layout. Retained rather than
 * deleted because it is what turned M2's silence into a sequence. */
#define TRACE_SWITCHES 0

/* A/B switch. With the in-loop probes compiled in, the selection loop is
 * correct; with them out, switch 4 returned 2 -> 2 while the task table said
 * every task was READY. Same source otherwise. Flip this to reproduce. */
#define TRACE_PROBES 0

#if TRACE_SWITCHES > 0

static uint32_t g_trace_n;

static const char *const FRAME_REGS[TASK_FRAME_WORDS] = {
    "a0 ", "a2 ", "a3 ", "a4 ", "a5 ", "a6 ", "a7 ", "a8 ", "a9 ",
    "a10", "a11", "a12", "a13", "a14", "a15", "sar", "epc", "eps",
    "lbg", "lnd", "lct"
};

static void trace_frame(int from, int to, uint32_t in_sp, const uint32_t *frame,
                        int fabricated)
{
    uart_puts("\n-- switch ");
    uart_put_dec(g_trace_n);
    uart_puts(": ");
    uart_put_dec((unsigned int)from);
    uart_puts(" -> ");
    uart_put_dec((unsigned int)to);
    uart_puts(fabricated ? "  (fabricated frame)\n" : "  (SAVED frame)\n");

    uart_puts("   in_sp=");
    uart_put_hex(in_sp);
    uart_puts("  frame@");
    uart_put_hex((uint32_t)frame);
    uart_puts("  base=");
    uart_put_hex((uint32_t)g_tasks[to].stack_base);
    uart_puts("\n");

    /* The whole task table. If the round robin stops offering a task, the
     * question is whether the scheduler skipped it or whether its state field
     * stopped saying READY — and those are different bugs. */
    uart_puts("   table:");
    for (int i = 0; i < TASK_MAX; i++) {
        uart_puts(" [");
        uart_put_dec((unsigned int)i);
        uart_puts("] st=");
        uart_put_dec((unsigned int)g_tasks[i].state);
        uart_puts(" sp=");
        uart_put_hex(g_tasks[i].sp);
        uart_puts(" sw=");
        uart_put_dec(g_tasks[i].switches);
    }
    uart_puts("\n");

    for (int i = 0; i < TASK_FRAME_WORDS; i++) {
        uart_puts("   ");
        uart_puts(FRAME_REGS[i]);
        uart_putc('=');
        uart_put_hex(frame[i]);
        if ((i % 3) == 2) {
            uart_putc('\n');
        }
    }
    uart_puts("\n");
}

#endif /* TRACE_SWITCHES > 0 */

/* Test hook. Byte-for-byte the selection loop as it was WITHOUT the volatile
 * workaround, so GCC is free to emit a zero-overhead LOOP again. Called from
 * kmain before timer_start(), i.e. single-threaded with no interrupt source
 * armed and no context switching in existence.
 *
 * This separates two very different explanations for the M2 defect:
 *   - wrong answers here  => the loop is mis-executed on its own, and interrupts
 *                            were never involved
 *   - correct answers here => the loop is fine in isolation and something about
 *                            interrupt context corrupts it
 */
int task_select_probe(int current)
{
    int next = current;
    for (int i = 1; i <= TASK_MAX; i++) {
        int candidate = (current + i) % TASK_MAX;
        if (g_tasks[candidate].state == TASK_READY) {
            next = candidate;
            break;
        }
    }
    return next;
}

/* Called from _handler_level3. Must not be static — assembly names it. */
uint32_t task_schedule(uint32_t current_sp)
{
    /* On the very first switch there is no task to save — the interrupted
     * context is the boot path, which is deliberately discarded. */
    /* Seed the overflow probe once, here rather than in kmain.c -- kmain is
     * flash-resident and step 25 measured that adding to it walks into the
     * layout band. */
    {
        static int seeded;
        if (!seeded) { seeded = 1; win_probe_seed(); }
    }

    uint32_t now = xt_ccount();
    if (g_current >= 0) {
        /* Catch the write, not its consequence.
         *
         * A saved sp of exactly _phy_stack_top has been reported for many steps
         * and three fixes, and every attempt to identify the writer by reading
         * the code has been wrong. This records it: the task, and the EPC3 in
         * the frame being saved -- the instruction that was actually executing.
         *
         * Read from current_sp, which the handler has just written and is known
         * good, rather than from the suspect value later. */
        {
            extern uint32_t _phy_stack_top[];
            if (current_sp == (uint32_t)_phy_stack_top && g_phytop_task < 0) {
                g_phytop_task = g_current;
                g_phytop_epc  = ((const uint32_t *)current_sp)[TASK_FRAME_IDX_EPC3];
                g_phytop_a0   = ((const uint32_t *)current_sp)[0];
            }
        }

        /* Fence check, once per switch, before anything trusts the table. */
        if (g_ttab_side < 0) {
            if (g_ttab.lo != 0xA5A5A5A5u) {
                g_ttab_side = 0; g_ttab_lo_seen = g_ttab.lo;
            } else if (g_ttab.hi != 0x5A5A5A5Au) {
                g_ttab_side = 1; g_ttab_hi_seen = g_ttab.hi;
            }
        }

        /* [step 82] Is a0 already broken when the task is SAVED?
         *
         * a0 = 0x0000000d at the faulting retw is neither a code address (a
         * call0 return) nor a windowed encoding (bit 31 set). Anything in
         * [1, 0x3fffffff] is therefore anomalous whichever ABI the task was
         * running. Catching it here says whether the value was already wrong
         * before the switch, or arrives wrong on the way back in -- the two
         * have different culprits and no amount of argument separates them. */
        {
            uint32_t a0_out = ((const uint32_t *)current_sp)[0];
            if (a0_out != 0u && a0_out < 0x40000000u && g_a0bad_out_task < 0) {
                g_a0bad_out_task = g_current;
                g_a0bad_out_val  = a0_out;
            }
        }

        /* [step 84] Does a switch frame land on the frame w2c_call2 is
         * watching?
         *
         * current_sp IS the switch frame's base, and the handler writes 112
         * bytes upward from it. g_slotwatch[0] is the windowed frame whose
         * [sp+0] was stamped. If the two ranges overlap, this catches it by
         * address -- no reading of a byte pattern, no resemblance argument.
         * If it never fires, step 83's switch-frame hypothesis is wrong and the
         * writer is something else. */
        {
            extern volatile uint32_t g_slotwatch[9];
            uint32_t watched = g_slotwatch[0];
            if (watched != 0u && g_ovlp_seen == 0u
                && current_sp <= watched && (current_sp + 112u) > watched) {
                g_ovlp_seen  = 1u;
                g_ovlp_frame = current_sp;
                g_ovlp_slot  = watched;
                g_ovlp_task  = g_current;
            }
        }

        /* [step 90] Who overwrites a task's chain terminator?
         *
         * It sits at a fixed address per task, written once at creation, and
         * after that it has NO legitimate writer. So any change names its
         * writer: latch the first one together with the task that was running
         * at that instant. Step 89 measured the value as an address inside the
         * blob's .bss, 96 bytes above the blob task's own stack pointer.
         *
         * Checked here, once per switch, for every task -- so the culprit is
         * identified by task rather than inferred from a byte pattern, which is
         * the move that has worked every time it was used in this
         * investigation. */
        if (g_term_hit < 0) {
            for (int t = 0; t < TASK_MAX; t++) {
                if (!g_term_addr[t]) { continue; }
                uint32_t now = ((const uint32_t *)g_term_addr[t])[-4];
                if (now != g_term_val[t]) {
                    g_term_hit     = t;
                    g_term_by      = g_current;
                    g_term_was     = g_term_val[t];
                    g_term_now     = now;
                    break;
                }
            }
        }

        /* [step 118] Audit the sweep, once, on the first event.
         *
         * Runs here because the sweep is in _handler_level3 immediately above
         * and this is the first C to execute afterwards. current_sp + 112 is
         * the task's own stack pointer -- the frame is TASK_FRAME_BYTES below
         * it -- and the innermost windowed frame's base save area sits at
         * [task_sp-16 .. task_sp-4], inside the frame's padding above offset
         * 88, so the walk starts on ground the handler did not touch. */
        if (g_pspill_count && !g_pspill_have) {
            uint32_t sp = (uint32_t)current_sp + TASK_FRAME_BYTES;
            g_pspill_have = 1u;
            g_pspill_sp   = sp;

            /* The two words the restore's first underflow reads, raw. */
            g_pspill_a0slot = ((volatile uint32_t *)(sp - 16u))[0];
            g_pspill_link   = ((volatile uint32_t *)(sp - 12u))[0];

            /* Census the borrowed stack. */
            {
                extern uint32_t _phy_stack[];
                const volatile uint32_t *bs = (const volatile uint32_t *)_phy_stack;
                uint32_t enc = 0u, spl = 0u;
                for (uint32_t k = 0; k < 256u; k++) {
                    uint32_t w = bs[k];
                    if ((w >> 30) != 0u && (w & 0x3FFFFFFFu) < 0x00400000u) { enc++; }
                    if (w >= 0x3FFB0000u && w < 0x3FFC0000u) { spl++; }
                }
                g_pspill_bs_enc = enc;
                g_pspill_bs_sp  = spl;
            }
            g_pspill_wb   = ((const uint32_t *)current_sp)[TASK_FRAME_IDX_WBASE] & 15u;
            for (uint32_t k = 0; k < 12u; k++) {
                if (sp < 0x3ff00000u || sp >= 0x40000000u) { break; }
                uint32_t fa0 = ((volatile uint32_t *)(sp - 16u))[0];
                uint32_t fa1 = ((volatile uint32_t *)(sp - 12u))[0];
                g_pspill_walked++;
                if ((fa0 >> 30) == 0u) {          /* not a windowed return encoding */
                    g_pspill_badframes++;
                    if (!g_pspill_bad_at) { g_pspill_bad_a0 = fa0; g_pspill_bad_at = sp; }
                }
                if (fa1 <= sp) { break; }         /* the chain must ascend */
                sp = fa1;
            }
        }

        /* [step 123] the comparison, latched on the first multiframe switch-out
         * so it describes a case where windowed frames are actually live. */
        {
            const uint32_t *fr = (const uint32_t *)current_sp;
            uint32_t ws   = fr[TASK_FRAME_IDX_WSTART];
            uint32_t base = fr[TASK_FRAME_IDX_WBASE] & 15u;
            if (!g_ih_ws && (ws & (ws - 1u))) {
                g_ih_ws      = ws;
                g_ih_wb      = base;
                g_ih_bitset  = (ws >> base) & 1u;
                g_ih_a1_calc = (uint32_t)current_sp + TASK_FRAME_BYTES;
                /* [step 123 fix] latch raw HERE, in the same event.
                 * _handler_level3 rewrites g_ih_a1_raw on every interrupt, so
                 * reading it at panic time compared this event's `calc` against
                 * some later tick's `raw` -- and duly reported DIFFER, with a
                 * raw that was not even inside this task's stack. The handler
                 * has already written it for THIS interrupt by the time we run,
                 * so copying it now pins both to one event. */
                g_ih_a1_latched = g_ih_a1_raw;
            }
        }

        /* [step 94] What this task actually HELD on the way out. */
        g_out_ws[g_current]   = ((const uint32_t *)current_sp)[TASK_FRAME_IDX_WSTART];

        /* ---- [step 115] SPILL ON PREEMPTION -- attempted, reverted ----------
         *
         * The sweep went here: if the outgoing task's saved WINDOWSTART had
         * more than one bit, call win_spill_call0() and rewrite the saved word
         * to 1 << base, so every task leaves with one frame and the restore
         * needs no cross-task union.
         *
         * It broke `wintorture` -- LoadProhibited at 0x40080155, inside the
         * window vectors. The same build also removed g_win_union from the
         * restore grant, which is two changes with one symptom, so the union
         * was put back on its own: wintorture still panicked. The sweep is the
         * cause, and the grant simplification is untested rather than wrong.
         *
         * Not a refutation of spill-on-preemption -- a placement problem. This
         * site runs AFTER _handler_level3 has done `addi a1, a1, -112` and
         * built the switch frame on the task's own stack, so at spill time a1
         * no longer holds the interrupted task's stack pointer and the frame
         * occupies the 112 bytes directly below it. Spilling with the task's
         * real sp instead is not a fix either: win_spill_call0 writes at
         * sp-32-12, which lands inside that frame.
         *
         * So the sweep needs somewhere the task's sp is intact AND nothing is
         * written below it -- the handler prologue before the frame is built,
         * which in turn needs the spill's register clobbers (a2..a11) dealt
         * with first. That is an assembly change to the interrupt prologue and
         * wants its own step, tested against wintorture with the switch count
         * as the control.
         *
         * Measured while it was in: during `wifiinit`, sweeps=0. The blob task
         * is pinned whenever it holds more than one frame and unpinned only in
         * the step-113 leaf spin, where it holds exactly one -- so the sweep
         * had nothing to do on the path it was built for. wintorture is a
         * different workload and did trigger it. */
        g_out_base[g_current] = ((const uint32_t *)current_sp)[TASK_FRAME_IDX_WBASE] & 15u;

        g_tasks[g_current].sp = current_sp;
        g_run_cycles[g_current] += now - g_slice_start;

        /* The invariant the two-word window save rests on, MEASURED.
         *
         * Saving only WINDOWBASE/WINDOWSTART -- rather than a whole spilled
         * window -- is only sound while every task is switched away from with
         * exactly ONE live frame. The pin is supposed to guarantee that for
         * involuntary switches and the blocking path's spill for voluntary
         * ones. Both were assumed. Step 30 died on a garbage stack pointer
         * inside the restore, which is what an invalid saved WINDOWSTART looks
         * like from the other end, so the assumption is worth checking rather
         * than believing.
         *
         * Counted, not enforced: refusing the switch here would be a second
         * guess layered on the first. This names the task instead. */
        /* Poll the overflow probe every tick.
         *
         * Sampling it only at the stub's spill boundary never fired: the bad
         * overflow happens after the task blocks, while another context is
         * rotating the window, so neither side of the spill sees it. The
         * scheduler runs on every tick regardless of who is executing, which is
         * the one vantage point that does. */
        {
            extern volatile uint32_t g_of_bad_base, g_of_bad_frame, g_of_bad_when;
            uint32_t base, frame;
            __asm__ volatile ("rsr.excsave6 %0" : "=r"(base));
            __asm__ volatile ("rsr.excsave7 %0" : "=r"(frame));
            if (g_of_bad_when == 0u && base != 0xFFFFFFFFu && base < 0x3ff00000u) {
                g_of_bad_base  = base;
                g_of_bad_frame = frame;
                g_of_bad_when  = 3u;          /* 3 = caught by the scheduler */
            }
        }

        /* [step 138] An excursion destroyed frames it did not own. Believe it. */
        if (g_win_disowned) {
            for (int t = 0; t < TASK_MAX; t++) { g_win_mask[t] = 0u; }
            g_win_union   = 0u;
            g_win_disowned = 0u;
            g_disown_hits++;
        }

        /* Claim what this task changed while it ran. */
        {
            /* A task owns the bit at its OWN base, and only if the hardware
             * says a frame really lives there.
             *
             * Two earlier rules failed here and both failures are informative.
             * "Everything set that nobody else claimed" credited a call0 task
             * with the driver's frames (step 53). "1 << base unconditionally"
             * claimed a frame for tasks that have none, because a call0 task is
             * not in windowed code at all (step 54).
             *
             * WINDOWSTART already answers the question the bookkeeping was
             * trying to guess: the bit at the task's base is set exactly when a
             * windowed frame lives there. A call0 task owns nothing; a task that
             * spilled to one frame before blocking owns that one. Frames deeper
             * than the base belong to whoever is RUNNING and are unwound or
             * spilled before that task can reach a switch. */
            uint32_t ws_out = ((const uint32_t *)current_sp)[TASK_FRAME_IDX_WSTART];
            uint32_t base   = ((const uint32_t *)current_sp)[TASK_FRAME_IDX_WBASE] & 15u;
            g_win_mask[g_current] = ((ws_out >> base) & 1u) ? (1u << base) : 0u;
            g_win_base[g_current] = (uint8_t)base;
        }

        uint32_t ws = ((const uint32_t *)current_sp)[TASK_FRAME_IDX_WSTART];
        uint32_t bits = 0u;
        for (uint32_t m = ws; m; m &= m - 1u) { bits++; }
        if (bits > 1u) {
            g_multiframe_count++;
            g_multiframe_task = g_current;
            g_multiframe_ws   = ws;
            if (bits > g_multiframe_worst) { g_multiframe_worst = bits; }
        }

        /* [X11] see the block comment at the declaration. */
        {
            extern volatile uint32_t g_rout_seq;
            uint32_t eps = ((const uint32_t *)current_sp)[TASK_FRAME_IDX_EPS3];
            if (eps & 0x10u) {
                g_excm_count++;
                g_excm_task = g_current;
                g_excm_seq  = g_rout_seq;
                g_excm_ps   = eps;
            }
            /* [X13] see the block comment at the declarations. */
            if ((eps & 0x40000u) == 0u) {
                g_woec_count++;
                g_woec_task = g_current;
                g_woec_seq  = g_rout_seq;
                g_woec_ps   = eps;
            }
        }
    }
    g_slice_start = now;

    /* [X12] see the block comment at the declarations. */
    {
        extern volatile uint32_t g_rout_seq;
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)&rom_call4;
        if (!g_txt_ready) {
            for (uint32_t i = 0; i < TXT_WATCH_WORDS; i++) { g_txt_base[i] = p[i]; }
            g_txt_ready = 1;
        } else {
            for (uint32_t i = 0; i < TXT_WATCH_WORDS; i++) {
                uint32_t noww = p[i];
                if (noww != g_txt_base[i]) {
                    g_txt_bad_ticks++;
                    if (g_txt_off < 0) {
                        g_txt_off = (int)i;
                        g_txt_seq = g_rout_seq;
                        g_txt_exp = g_txt_base[i];
                        g_txt_act = noww;
                    }
                    break;
                }
            }
        }
    }


    /* Round robin from the one after current, so no task can starve another.
     * With g_current == -1 the first candidate is 0, so the first switch enters
     * whichever task was created first.
     *
     * The idle task is skipped here and used only as a fallback below. Leaving
     * it in the rotation would hand it an equal share of the CPU, which is a
     * seventh of the machine spent deliberately doing nothing. */
    wake_sleepers();

    /* Highest priority wins; equal priorities round-robin.
     *
     * Scanning from the task after the current one and taking a candidate only
     * on STRICTLY greater priority gives both at once: among equals the first
     * one met wins, and because the scan starts past the current task, that is
     * a different task each time. */
    /* Effective priority = base + ageing credit, computed per candidate below.
     * The base priority is never modified: ageing is a property of the
     * SELECTION, not of the task, so a task that finally runs returns to its
     * declared priority automatically rather than needing to be restored. That
     * distinction is what keeps this separate from priority inheritance, which
     * really does change a task's priority and really does have to undo it. */
    int next = -1;
    int best = -1;
    /* Plain counted loop. GCC is free to emit a zero-overhead LOOP here, and
     * does; that is correct now that _handler_level3 clears PS.EXCM before
     * calling C. This loop previously needed a `volatile` counter to force
     * ordinary branches, which worked but treated the symptom. */
    for (int i = 1; i <= TASK_MAX; i++) {
        int candidate = (g_current + i) % TASK_MAX;
        if (candidate == g_idle_id) {
            continue;
        }
#if TRACE_SWITCHES > 0 && TRACE_PROBES
        if (g_trace_n < TRACE_SWITCHES) {
            uart_puts("\n   probe i=");
            uart_put_dec((unsigned int)i);
            uart_puts(" cand=");
            uart_put_dec((unsigned int)candidate);
            uart_puts(" st=");
            uart_put_dec((unsigned int)g_tasks[candidate].state);
            uart_puts(g_tasks[candidate].state == TASK_READY ? " MATCH" : " skip");
        }
#endif
        if (g_tasks[candidate].state == TASK_READY) {
            uint32_t credit = g_tasks[candidate].waiting / TASK_AGE_TICKS;
            if (credit > TASK_AGE_MAX) {
                credit = TASK_AGE_MAX;
            }
            int eff = (int)g_tasks[candidate].prio + (int)credit;
            if (eff > best) {
                best = eff;
                next = candidate;
            }
        }
    }
    /* Nothing else runnable — fall back to idle. Resuming the interrupted task
     * is NOT an acceptable answer once blocking exists: that task may be the
     * one that just blocked, and running a blocked task defeats the whole
     * mechanism. */
    if (next < 0 && g_idle_id >= 0 && g_tasks[g_idle_id].state == TASK_READY) {
        next = g_idle_id;
    }

    if (next < 0) {
        /* No runnable task and no idle task. Before blocking existed this could
         * only happen at boot; it can now also mean every task is blocked with
         * nobody left to wake them, which is a deadlock the kernel cannot
         * resolve. Resuming the interrupted context is the least-bad answer and
         * at least keeps the console alive to say so. */
        return current_sp;
    }

#if TRACE_SWITCHES > 0
    /* switches == 0 means this task has never run, so its frame is still the
     * one task_create fabricated. Anything else is a frame the handler saved. */
    int fabricated = (g_tasks[next].switches == 0);
    int from = g_current;
#endif

    /* Liveness for the hang detector: a tick that resumes the SAME task is not
     * evidence the system is healthy — it is exactly what a monopoly looks
     * like. Only a switch between distinct tasks counts. */
    /* Ageing bookkeeping, done once the decision is final.
     *
     * Every READY task that was NOT chosen waits one more tick; the chosen one
     * resets. Sleeping and blocked tasks are untouched — a task waiting on a
     * deadline or a mutex is not being treated unfairly by the scheduler, and
     * crediting it would let it barge ahead the moment it becomes runnable. */
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state != TASK_READY) {
            continue;
        }
        if (i == next) {
            if (g_tasks[i].waiting > g_max_wait) {
                g_max_wait = g_tasks[i].waiting;
            }
            g_wait_hist[wait_bucket(g_tasks[i].waiting)]++;
            if (g_tasks[i].waiting >= TASK_AGE_TICKS) {
                g_age_rescues++;    /* it only ran because ageing lifted it */
            }
            g_tasks[i].waiting = 0;
        } else {
            g_tasks[i].waiting++;
        }
    }

    /* A task inside the blob's windowed code is NOT preemptible.
     *
     * nat-os cannot preserve a windowed frame set across a context switch, and
     * five attempts to teach it how have failed (next_moves/08 steps 14-18).
     * The cheaper guarantee is to make the situation not arise: while a task is
     * executing windowed vendor code, decline to switch away from it.
     *
     * This is NOT the old masked-interrupt model. Interrupts stay enabled --
     * the tick fires, ISRs run, the timer keeps time. Only the SWITCH is
     * withheld, which is the narrow thing that actually breaks.
     *
     * It is bounded because it ends the moment the task blocks: the adapter's
     * blocking entries spill their window and clear this first, precisely when
     * it becomes safe to switch away. So the CPU is monopolised for the length
     * of a blob call that is making progress, and released the instant one
     * stops.
     *
     * The watchdog is fed deliberately here. It is normally fed by evidence of
     * a task switch, and this is the one case where declining to switch is
     * correct rather than a symptom -- without this the hang detector would
     * reset a blob call that is working. */
    if (blob_pinned_task() >= 0 && blob_pinned_task() == g_current) {
        next = g_current;

        /* BOUNDED. Feeding the watchdog at the decline is what stops the hang
         * detector resetting a blob call that is working -- and it is also
         * what would let a wedged one run forever, which is exactly what
         * happened the first time this was tried: init pinned the caller,
         * created the blob's task, waited for it, and the pin prevented the
         * task it was waiting for from ever running.
         *
         * So the feeding stops after a bound. Past it the pin still holds --
         * switching away would corrupt the window either way -- but the
         * watchdog is left alone and the hang detector does its job. A blob
         * call that has not blocked or returned in this long is not making
         * progress. */
        /* Per pin, not per run of pinned ticks. Each new pin starts a fresh
         * budget; two tasks alternating short blob calls therefore never
         * accumulate one another's ticks. */
        static uint32_t last_seq;
        uint32_t seq = blob_pin_seq();
        if (seq != last_seq) {
            last_seq = seq;
            g_blob_pin_ticks = 0;
        }

        if (++g_blob_pin_ticks < BLOB_PIN_MAX_TICKS) {
            watchdog_feed();
        }
    } else {
        g_blob_pin_ticks = 0;
    }

    watchdog_liveness(next != g_current);

    /* A broken guard is fatal, not cosmetic.
     *
     * It used to print "BROKEN" beside the telemetry and carry on, which means
     * continuing to schedule tasks whose stacks have already written into a
     * neighbour's. Every number printed after that point is suspect, including
     * the ones that would be used to diagnose it. Stopping at the switch that
     * noticed keeps the damage bounded and the report honest. */
    int broken = task_stack_broken();
    if (broken >= 0) {
        kernel_panic_msg("stack guard overwritten", (unsigned int)broken);
    }

    /* DISTINCT switches only.
     *
     * This counted every tick, including the ones where next == g_current and
     * nothing switched -- and two lines above, the same function passes
     * `next != g_current` to watchdog_liveness(), so the distinction was already
     * known here and simply not used.
     *
     * It made wintorture's control meaningless: "switches during the call: 6
     * (preemption really happened)" was six ticks of a pinned task resuming
     * itself. That sentence is why "windowed frames survive preemption" was
     * treated as measured since step 14. See next_moves/08 step 54. */
    /* The union is what _handler_level3 assigns on the way back in. Computed
     * here, in C, because the vector has no room to walk a table. */
    {
        uint32_t u = 0u;
        for (int t = 0; t < TASK_MAX; t++) { u |= g_win_mask[t]; }

        /* A SAME-TASK resume must not disturb the window.
         *
         * next == g_current still runs the whole restore path, including the
         * WINDOWSTART write -- and a task pinned inside windowed code is resumed
         * as itself on every tick. Any rule that narrows the mask therefore
         * deletes the RUNNING task's deep frames one tick after it creates them,
         * which is why both attempts at a precise rule regressed wintorture
         * while the imprecise OR survived: the OR never dropped what was live.
         *
         * Handing the live register back makes the assignment a no-op in that
         * case, without a branch in the vector. */
        if (next == g_current) {
            uint32_t live;
            __asm__ volatile ("rsr.windowstart %0" : "=r"(live));
            u = live;
        } else {
            /* A REAL task change: nobody else needs a live bit.
             *
             * Every non-running task has all sixteen of its registers in memory,
             * saved by this handler on the way out and restored on the way back
             * in. A bit left set for it does not preserve anything -- the
             * registers at that position now belong to whoever is running -- but
             * it does tell the hardware a frame lives there, and an overflow that
             * reaches it spills through registers that are somebody else's. That
             * is the phantom frame of steps 48-56.
             *
             * So on a real switch the incoming task claims its own base and
             * nothing else. This is the assignment that regressed the suite
             * twice -- but both times the damage was done on SAME-task resumes,
             * which the branch above now leaves untouched. */
            u = 0u;
        }
        g_win_union = u;
    }

    if (next != g_current) {
        g_tasks[next].switches++;
    }
    g_tasks[next].resumes++;
    g_current = next;
    g_dbg_current = next;            /* [X7 experiment] mirror for the ring */

    /* [X8b DIAGNOSTIC -- NOT A FIX] heartbeat from the scheduler itself.
     * Dots keep flowing while ticks fire and scheduling cycles; silence means
     * the machine wedged below the scheduler (interrupts dead or a spin with
     * INTLEVEL raised). Discriminates "stuck in taskland" from "machine
     * dead" for the X8 clamp hang. */
    {
        static uint32_t hb;
        if ((++hb & 63u) == 0u) {
            uart_putc('.');
            if ((hb & 1023u) == 0u) {
                uart_putc('\n');
            }
        }
    }

#if TRACE_SWITCHES > 0
    if (g_trace_n < TRACE_SWITCHES) {
        g_trace_n++;
        trace_frame(from, next, current_sp,
                    (const uint32_t *)g_tasks[next].sp, fabricated);
    }
#endif

    /* The stack pointer this returns is loaded straight into a1 by
     * _handler_level3, which then restores sixteen registers through it. A bad
     * value faults INSIDE the handler -- measured, `l32i.n a8, a1, 28` at
     * 0x400891b0 -- one restore after the corruption, with nothing left to say
     * where it came from.
     *
     * Checked here instead, where the owning task is still known. Recorded and
     * not corrected: substituting a plausible sp would hide the bug and hand the
     * handler a stack that is not the task's. */
    {
        uint32_t sp = g_tasks[next].sp;
        uint32_t base = (uint32_t)g_tasks[next].stack_base;
        if (base != 0u) {
            uint32_t lo = base;
            uint32_t hi = base + g_tasks[next].stack_words * 4u;
            /* The private PHY stack is a legitimate place for a task to be
             * executing, so a saved sp inside it is expected rather than wrong.
             * phy_stack_call switches onto it deliberately, and the save and
             * restore around a tick are symmetric -- step 70.
             *
             * Everything else outside the owning task's stack is still worth
             * catching. That property has never actually been violated, which is
             * the point of keeping the check rather than deleting it. */
            extern uint32_t _phy_stack[], _phy_stack_top[];
            int on_phy = (sp >= (uint32_t)_phy_stack && sp <= (uint32_t)_phy_stack_top);

            if ((sp < lo || sp > hi) && !on_phy && g_badsp_task < 0) {
                extern volatile uint32_t g_woe_prev_hit;
                g_badsp_task = next;
                g_badsp_val  = sp;
                g_badsp_lo   = lo;
                g_badsp_hi   = hi;
                /* Which adapter entry the blob had most recently reached when
                 * the table first went bad. The write is targeted (step 63), so
                 * bracketing it to one entry is the whole remaining question. */
                g_badsp_osi  = g_woe_prev_hit;
                g_badsp_tick = timer_ticks();
                /* The PS in force at the instant the tick was taken. EPS3 is
                 * what the handler saved from the interrupted context, so its
                 * INTLEVEL says whether phy_stack_call's rsil was actually in
                 * effect. If it reads 3, the mask held and the tick fired
                 * anyway; if it reads 0, the mask was never in force. */
                g_badsp_eps  = ((const uint32_t *)sp)[TASK_FRAME_IDX_EPS3];

                /* Say so IMMEDIATELY, once.
                 *
                 * The panic dump only appears on an exception, so bisecting
                 * which stage corrupts the table meant provoking a fault to read
                 * the result -- and kernel_panic_msg takes a different path that
                 * does not print it. Announcing at the latch makes any command
                 * self-reporting. Polled UART, one line, guarded by the same
                 * stickiness as the record. */
                uart_puts("\n[!] task ");
                uart_put_dec((unsigned int)next);
                uart_puts(" sp ");
                uart_put_hex(sp);
                uart_puts(" left its stack\n");
                uart_puts("    eps3 ");
                uart_put_hex(g_badsp_eps);
                uart_puts("  intlevel ");
                uart_put_dec(g_badsp_eps & 0xFu);
                uart_puts("\n");
            }
        }
    }

    /* [step 82] ...and the same check on the way IN. */
    {
        uint32_t a0_in = ((const uint32_t *)g_tasks[next].sp)[0];
        if (a0_in != 0u && a0_in < 0x40000000u && g_a0bad_in_task < 0) {
            g_a0bad_in_task = next;
            g_a0bad_in_val  = a0_in;
        }
    }

    /* [step 94] ...and what the restore is about to GRANT it.
     *
     * The handler assigns `1 << saved_base | g_win_union`. Anything the task
     * held at switch-out that is not in that grant is a frame the hardware will
     * no longer believe in -- its window position free for another context, its
     * registers reused, and an unwind that reaches it reading whatever now lives
     * at that stack address. Step 93's account predicts this is non-zero for the
     * blob task; if it is always zero, that account is wrong.
     *
     * Latched once, with the task, so it names rather than suggests. */
    /* [step 141] Did the LAST prediction match what the handler wrote? */
    if (g_pred_task >= 0 && g_grant_drift_task < 0) {
        extern volatile uint32_t g_rin_ws;
        if (g_rin_ws != g_pred_grant) {
            g_grant_drift_task = g_pred_task;
            g_grant_drift_pred = g_pred_grant;
            g_grant_drift_real = g_rin_ws;
        }
    }

    {
        uint32_t base_in = ((const uint32_t *)g_tasks[next].sp)[TASK_FRAME_IDX_WBASE] & 15u;
        g_pred_task  = next;
        g_pred_grant = (1u << base_in) | g_win_union;
    }

    if (g_lost_task < 0 && g_out_ws[next] != 0u) {
        uint32_t base_in = ((const uint32_t *)g_tasks[next].sp)[TASK_FRAME_IDX_WBASE] & 15u;
        uint32_t grant   = (1u << base_in) | g_win_union;
        uint32_t lost    = g_out_ws[next] & ~grant;
        if (lost != 0u) {
            g_lost_task  = next;
            g_lost_had   = g_out_ws[next];
            g_lost_grant = grant;
            g_lost_bits  = lost;
        }
    }

    return g_tasks[next].sp;
}

int task_current(void) { return g_current; }

uint32_t task_switch_count(int id)
{
    return (id >= 0 && id < TASK_MAX) ? g_tasks[id].switches : 0;
}

/* The saved stack pointer of a task that is not running.
 *
 * Diagnostic. A blocked task resuming with a zero sp is the whole question in
 * next_moves/08 step 26, and this is the only way to see whether the damage is
 * in the task table or happens on the way back out. */
/* The ADDRESS of a task's saved sp field.
 *
 * Step 67. Every conclusion since step 62 assumes this field is where the code
 * thinks it is, and the check that would catch a wrong assumption reads it back
 * through the same expression. This lets the assumption be tested against the
 * PHY stack's actual extent instead. */
uint32_t task_sp_addr(int id)
{
    if (id < 0 || id >= TASK_MAX) { return 0u; }
    return (uint32_t)&g_tasks[id].sp;
}

uint32_t task_saved_sp(int id)
{
    if (id < 0 || id >= TASK_MAX) { return 0u; }
    return g_tasks[id].sp;
}

uint32_t task_stack_span(int id, uint32_t *words)
{
    if (id < 0 || id >= TASK_MAX) { return 0u; }
    if (words) { *words = g_tasks[id].stack_words; }
    return (uint32_t)g_tasks[id].stack_base;
}

int task_stack_intact(int id)
{
    if (id < 0 || id >= TASK_MAX || g_tasks[id].stack_base == 0) {
        return 1;   /* no stack of ours to check */
    }
    return g_tasks[id].stack_base[0] == STACK_GUARD;
}

/* First task whose guard word has been overwritten, or -1 if all are intact.
 *
 * Checked on every context switch rather than by the reporter, because the
 * reporter only covered three of the eight tasks and a broken guard means the
 * damage has ALREADY happened — a check that runs seconds later reports a
 * corruption whose cause is long gone. Eight word loads per tick is not a cost
 * worth optimising against that. */
/* Test hook: clobber the running task's guard word so the next context switch
 * finds it. Exists for the same reason `hang` and `fault` do — an enforcement
 * path that has never been seen to fire is an assumption, not a mechanism. */
/* Cycles the CALLING task has run, including the slice it is in right now.
 *
 * This is the clock to measure work with. Two reads around a section give the
 * cycles that section actually consumed, with every preemption in between
 * excluded — which is the whole difference from xt_ccount().
 *
 * The critical section is not optional: without it a tick landing between the
 * two loads returns an accumulated total from before the switch added to a
 * slice start from after it, which reads as a wildly negative interval. */
uint32_t task_cpu_cycles(void)
{
    uint32_t crit = crit_enter();
    uint32_t v = (g_current >= 0)
               ? g_run_cycles[g_current] + (xt_ccount() - g_slice_start)
               : xt_ccount();
    crit_exit(crit);
    return v;
}

uint32_t task_cpu_cycles_of(int id)
{
    return (id >= 0 && id < TASK_MAX) ? g_run_cycles[id] : 0u;
}

void task_smash_guard(void)
{
    if (g_current >= 0 && g_tasks[g_current].stack_base) {
        g_tasks[g_current].stack_base[0] = 0xDEADBEEFu;
    }
}

int task_exists(int id)
{
    return id >= 0 && id < TASK_MAX && g_tasks[id].state != TASK_UNUSED;
}

const char *task_name(int id)
{
    if (id < 0 || id >= TASK_MAX || g_tasks[id].name == 0) {
        return "?";
    }
    return g_tasks[id].name;
}

int task_stack_broken(void)
{
    for (int id = 0; id < TASK_MAX; id++) {
        if (g_tasks[id].stack_base && g_tasks[id].stack_base[0] != STACK_GUARD) {
            return id;
        }
    }
    return -1;
}

/* The task closest to overflowing. Reported so the margin is a number somebody
 * has seen, rather than an assumption that 2 KB was enough. */
int task_stack_tightest(void)
{
    int worst = -1;
    uint32_t least = 0xFFFFFFFFu;
    for (int id = 0; id < TASK_MAX; id++) {
        if (!g_tasks[id].stack_base) {
            continue;
        }
        uint32_t free_words = task_stack_headroom(id);
        if (free_words < least) {
            least = free_words;
            worst = id;
        }
    }
    return worst;
}

uint32_t task_stack_headroom(int id)
{
    if (id < 0 || id >= TASK_MAX || g_tasks[id].stack_base == 0) {
        return 0;
    }
    uint32_t untouched = 0;
    /* Walk up from just above the guard until the fill pattern stops. Bounded
     * by THIS task's stack, not by the pool constant -- a task on a supplied
     * stack has a different size, and reading past it would report headroom
     * from whatever follows in DRAM. */
    for (uint32_t i = 1u; i < g_tasks[id].stack_words; i++) {
        if (g_tasks[id].stack_base[i] != STACK_FILL) {
            break;
        }
        untouched++;
    }
    return untouched;
}

void task_set_idle(int id)
{
    if (id >= 0 && id < TASK_MAX) {
        g_idle_id = id;
    }
}

/* Leave the register file holding exactly ONE frame for this task.
 *
 * next_moves/08 step 33. WINDOWSTART is global and describes frames belonging to
 * several tasks at once, so a per-task copy cannot be written back: assigning it
 * resurrects other tasks' frames, OR-ing it discards your own. Both were tried
 * and both fail. The only way the per-task word becomes meaningful is if it is
 * always one bit -- which means a task must not park with more than one live
 * frame.
 *
 * Done HERE, in task context, and not in _handler_level3 where five attempts
 * failed (steps 14-18). It does not need to be in the handler: the pin means an
 * involuntary switch never lands while a task holds live windowed frames, so
 * every switch out of windowed code is voluntary and passes through here.
 *
 * The test is cheap and skips the common case: `ws & (ws - 1)` is non-zero only
 * when more than one frame is live anywhere in the file. A plain call0 task with
 * nothing windowed in flight pays one register read. */
static void spill_before_parking(void)
{
    uint32_t ws;
    __asm__ volatile ("rsr.windowstart %0" : "=r"(ws));
    if (ws & (ws - 1u)) {
        /* [X5 experiment] Every sight of more than one live frame at a park
         * point. Overwritten each time, so after a fault these describe the
         * LAST sighting -- if the faulting sweep rotated over a bit that does
         * not belong to the sweeping task, that bit is the phantom frame and
         * its owner is whoever parked without cleaning it. */
        extern volatile uint32_t g_sbp_ws, g_sbp_wb;
        extern volatile int      g_sbp_task;
        {
            uint32_t wb;
            __asm__ volatile ("rsr.windowbase %0" : "=r"(wb));
            g_sbp_wb   = wb;
            g_sbp_ws   = ws;
            g_sbp_task = g_current;
        }
        /* [X8 experiment] Clamp reverted after its run showed a livelock
         * instead of the baseline fault: the forced sweep is load-bearing.
         * Kept instead: outcome instrumentation -- what the sweep actually
         * left behind, per park. */
        win_spill_call0();
        {
            uint32_t wb2, ws2;
            __asm__ volatile ("rsr.windowbase %0"  : "=r"(wb2));
            __asm__ volatile ("rsr.windowstart %0" : "=r"(ws2));
            g_sbp_post_wb = wb2;
            g_sbp_post_ws = ws2;
        }
    }
}

void task_block(void)
{
    spill_before_parking();
    if (g_current < 0) {
        return;                     /* boot context has nothing to block */
    }
    g_tasks[g_current].state = TASK_BLOCKED;
    /* Deliberately does NOT yield. The caller marks itself blocked while still
     * holding a critical section — so the decision to block and the record of
     * what it is waiting for cannot be split by a tick — then leaves the
     * critical section and yields. Yielding here instead would either fire the
     * tick with the wait-list half-updated, or be silently deferred until the
     * caller's crit_exit(), which reads as a bug at the call site. */
}

int task_state_of(int id)
{
    return (id >= 0 && id < TASK_MAX) ? (int)g_tasks[id].state : (int)TASK_UNUSED;
}

void task_unblock(int id)
{
    if (id >= 0 && id < TASK_MAX && g_tasks[id].state == TASK_BLOCKED) {
        g_tasks[id].state = TASK_READY;
    }
}

void task_wake(int id)
{
    if (id < 0 || id >= TASK_MAX) {
        return;
    }
    /* Deliberately takes SLEEPING as well as BLOCKED, which is the whole
     * difference from task_unblock().
     *
     * A task waiting on hardware wants two things at once: to be released the
     * instant the device speaks, and to be released anyway if it never does. A
     * plain block gives the first without the second, and a task blocked on a
     * peripheral that has failed is deaf forever. A plain sleep gives the second
     * without the first.
     *
     * Sleeping with a deadline and letting an interrupt cut it short gives both,
     * and the failure mode is the good one: if the interrupt never arrives, the
     * deadline still fires and the caller falls back to polling. That is how the
     * touch task survives a PENIRQ that does not work — it degrades to exactly
     * what it did before this existed rather than to nothing.
     *
     * Safe from an ISR. It only promotes a state to READY; wake_tick is left
     * alone, and the scheduler's sleep sweep ignores a task that is no longer
     * SLEEPING. */
    /* NA-005. The flag is set for ANY live task, including one that is still
     * RUNNING -- which is the case this used to lose.
     *
     * A waiter arms its interrupt inside a critical section, leaves it, and
     * then calls task_sleep(). An edge arriving in that gap found the task
     * still READY, so the old `state == BLOCKED || state == SLEEPING` guard
     * matched nothing, no flag was set, and the sleep that followed ran its
     * full timeout with the event already gone. touch.c's ISR does not latch
     * in that case either, because a waiter WAS registered.
     *
     * Recording the wake regardless costs nothing for existing callers, since
     * task_sleep() clears the flag on entry and can never see a stale one. It
     * is task_sleep_armed() that keeps it, for callers that armed something
     * first. */
    if (g_tasks[id].state != TASK_UNUSED) {
        g_tasks[id].woken = 1;
    }
    if (g_tasks[id].state == TASK_BLOCKED ||
        g_tasks[id].state == TASK_SLEEPING) {
        g_tasks[id].state = TASK_READY;
        /* Records that this was a DELIBERATE release, not a deadline expiring.
         * task_sleep re-arms until its deadline arrives, so without this it
         * would sleep the caller again and the early wake would be lost. Set
         * only here; wake_sleepers() leaves it alone, which is what makes the
         * two cases distinguishable. */
    }
}

void task_set_priority(int id, int prio)
{
    if (id < 0 || id >= TASK_MAX) {
        return;
    }
    if (prio < 0) { prio = 0; }
    if (prio >= TASK_PRIO_LEVELS) { prio = TASK_PRIO_LEVELS - 1; }
    g_tasks[id].base_prio = (uint8_t)prio;
    g_tasks[id].prio      = (uint8_t)prio;
}

int task_priority(int id)
{
    return (id >= 0 && id < TASK_MAX) ? (int)g_tasks[id].prio : -1;
}

void task_boost(int id, int prio)
{
    if (id >= 0 && id < TASK_MAX && prio > (int)g_tasks[id].prio) {
        g_tasks[id].prio = (uint8_t)prio;
    }
}

void task_unboost(int id)
{
    if (id >= 0 && id < TASK_MAX) {
        g_tasks[id].prio = g_tasks[id].base_prio;
    }
}

void task_arm_wake(void)
{
    if (g_current < 0) {
        return;
    }
    uint32_t crit = crit_enter();
    g_tasks[g_current].woken = 0;
    crit_exit(crit);
}

void task_sleep(uint32_t ticks)
{
    spill_before_parking();
    /* Discards any wake that arrived before this call. Correct for a plain
     * delay, which is what almost every caller wants -- a stale flag would
     * otherwise make the very next sleep return immediately. A caller that
     * armed something first wants the opposite and uses task_sleep_armed(). */
    task_arm_wake();
    task_sleep_armed(ticks);
}

void task_sleep_armed(uint32_t ticks)
{
    if (g_current < 0) {
        return;                     /* no context to sleep */
    }

    /* Loops, because task_yield() does not switch — it ARMS a switch.
     *
     * It writes CCOMPARE1 to ccount + 64 and returns; the context change
     * happens when that comparator fires, roughly sixty cycles later. So the
     * caller keeps running past this function for a short window, and whatever
     * it does in that window happens DURING what it believes is a sleep.
     *
     * Measured directly: task_sleep(50) — half a second — returned to its
     * caller in 107 cycles, which is the cost of the function body and nothing
     * else. The sleep did happen; it just started after the caller had already
     * read the clock and concluded no time had passed. Two separate
     * measurements this session were wrong because of it, including the first
     * TSF check, which is what exposed it.
     *
     * Re-arming until the deadline genuinely arrives closes that window and
     * costs one extra pass in the normal case.
     *
     * The `woken` flag is what keeps this from breaking touch_irq_wait. That
     * caller wants a sleep an interrupt can cut short, so "the deadline has not
     * arrived" must not be confused with "task_wake released me deliberately" —
     * without the flag, this loop would put the task straight back to sleep and
     * turn every early wake into a full-length one. */
    /* NA-001. Clamp, and say so.
     *
     * Every deadline comparison in this kernel is the wrap-safe
     * (int32_t)(now - deadline) >= 0, which is correct and is why tick wrap
     * works -- but it only spans half the range. deadline = now + 0xFFFFFFFF is
     * now - 1, so the very first test reads (int32_t)(1) >= 0 and the function
     * returns WITHOUT SLEEPING. A caller asking to sleep forever got no sleep
     * at all, which is the opposite of what it asked for.
     *
     * Clamped rather than refused, which is a deliberate departure from this
     * project's rule 6. Rule 6 is about transfers, where a silent shortening
     * loses data. Here the caller's intent for any absurd value is "a very long
     * time", and TASK_SLEEP_MAX is 248 days at 100 Hz -- longer than this board
     * will run. Refusing would also mean changing a void signature that has 19
     * call sites, for no gain.
     *
     * But a clamp that hides a caller's arithmetic bug is still a lie, so it is
     * counted. task_sleep_clamped() should be zero forever; if it is not,
     * somebody computed a sleep that overflowed. */
    if (ticks > TASK_SLEEP_MAX) {
        g_sleep_clamped++;
        ticks = TASK_SLEEP_MAX;
    }

    uint32_t deadline = timer_ticks() + ticks;

    for (;;) {
        uint32_t crit = crit_enter();
        if (g_tasks[g_current].woken) {
            g_tasks[g_current].woken = 0;
            crit_exit(crit);
            return;                 /* released early, on purpose */
        }
        if ((int32_t)(timer_ticks() - deadline) >= 0) {
            crit_exit(crit);
            return;                 /* the time really has passed */
        }
        g_tasks[g_current].wake_tick = deadline;
        g_tasks[g_current].state     = TASK_SLEEPING;
        crit_exit(crit);
        task_yield();
    }
}

void task_yield(void)
{
    /* Bring the comparator deadline forward so the tick — and therefore the
     * switch — happens almost immediately. Reuses the preemption path exactly
     * instead of introducing a second way to switch, which would be a second
     * thing that can be subtly wrong.
     *
     * ONLY EVER EARLIER, NEVER LATER. Writing ccount + 64 unconditionally means
     * a loop that yields faster than 64 cycles pushes the deadline ahead of
     * CCOUNT on every pass, so the comparator is never reached and the timer
     * interrupt stops firing altogether. That is not a slowdown — it is a total
     * system freeze, because the tick is what drives every context switch, and
     * the task doing the yielding spins forever waiting for a clock it is
     * itself preventing.
     *
     * It cost a full debugging session: `while (timer_ticks() < until)
     * task_yield();` in the display task halted the entire kernel, and looked
     * exactly like a hung display driver. */
    /* Every voluntary switch point funnels through here, so closing the class
     * costs one line and needs no search for the path that was missed. The
     * blocking primitives already spill; this covers the bare yields -- idle
     * loops in the shell and the app host today, whatever calls it tomorrow.
     *
     * The guard means a task with nothing windowed in flight pays one register
     * read, which is what the idle loops that call this hardest will pay. */
    spill_before_parking();

    uint32_t soon = xt_ccount() + 64u;
    if ((int32_t)(soon - xt_get_ccompare1()) < 0) {
        xt_set_ccompare1(soon);
    }
}
