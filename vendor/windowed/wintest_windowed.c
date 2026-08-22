/* Compiled -mabi=windowed. NOT part of the kernel.
 *
 * Stands in for a vendor blob: this is exactly the ABI every precompiled
 * Espressif library uses, produced by the same compiler that would consume one.
 * If the kernel can call this, it can call theirs.
 *
 * Written to be awkward on purpose. A leaf function returning a constant would
 * link and run without ever rotating a register window, and would prove
 * nothing:
 *
 *   - it recurses, so ENTRY runs deep enough to exhaust the physical registers
 *     and force overflow exceptions
 *   - it keeps a local array live across the recursive call, so the compiler
 *     must actually use callee-saved registers rather than optimise the frame
 *     away
 *   - it returns a checksum over every level, so a window handler that reloads
 *     the wrong register produces a WRONG NUMBER rather than merely not
 *     crashing. That distinction is the whole point of the test.
 */

unsigned int vendor_probe(unsigned int depth, unsigned int seed);

unsigned int vendor_probe(unsigned int depth, unsigned int seed)
{
    unsigned int local[6];

    for (unsigned int i = 0; i < 6u; i++) {
        local[i] = seed + i;
    }

    if (depth == 0u) {
        return seed;
    }

    unsigned int deeper = vendor_probe(depth - 1u, seed + 1u);

    /* Reading the locals AFTER the call is what forces them to survive it. */
    unsigned int sum = deeper;
    for (unsigned int i = 0; i < 6u; i++) {
        sum += local[i];
    }
    return sum;
}

/* ---- windowed -> call0, proved -----------------------------------------
 *
 * The OSI table's bodies are windowed and need the call0 kernel underneath
 * them. This exercises that direction the same way vendor_probe exercises the
 * other: it recurses, so window overflow is unavoidable, and it crosses the ABI
 * boundary on every level. The return value is a checksum, so a bridge that
 * corrupts a register produces a wrong number rather than merely surviving.
 */
extern unsigned int w2c_call2(unsigned int fn, unsigned int a, unsigned int b);

unsigned int vendor_bridge_probe(unsigned int fn_add, unsigned int depth)
{
    unsigned int acc = 0;
    for (unsigned int i = 0; i < depth; i++) {
        /* Each iteration leaves windowed code, runs a call0 function, returns. */
        acc = w2c_call2(fn_add, acc, i);
    }
    return acc;
}

/* ---- preemption torture ------------------------------------------------
 *
 * vendor_probe above proves window OVERFLOW handling works: recurse deep
 * enough to exhaust the physical registers and the checksum still comes back
 * right. What it cannot prove is anything about PREEMPTION, because it
 * completes in microseconds and is almost never interrupted.
 *
 * The question that matters for the WiFi driver is different: can a context
 * switch happen while windowed frames are live? nat-os's level-3 handler saves
 * a0..a15 and NOT WINDOWBASE/WINDOWSTART, and does not spill the window --
 * which is why phy_stack_call masks interrupts for the whole call.
 *
 * So this holds a modest number of live frames (shallow enough to fit a task
 * stack) and then SPINS at the bottom for a requested number of milliseconds,
 * guaranteeing many ticks elapse with those frames live. The locals are read
 * after the spin and after the recursive call, so a switch that mishandles the
 * register window produces a WRONG CHECKSUM rather than merely not crashing.
 */
unsigned int vendor_torture(unsigned int depth, unsigned int spin_ms);

unsigned int vendor_torture(unsigned int depth, unsigned int spin_ms)
{
    unsigned int local[6];

    for (unsigned int i = 0; i < 6u; i++) {
        local[i] = (depth * 7u) + i;
    }

    if (depth == 0u) {
        unsigned int t0, now;
        __asm__ volatile ("rsr.ccount %0" : "=r"(t0));
        for (;;) {
            __asm__ volatile ("rsr.ccount %0" : "=r"(now));
            if ((now - t0) > (spin_ms * 80000u)) { break; }   /* 80 MHz */
        }
        return 0u;
    }

    unsigned int deeper = vendor_torture(depth - 1u, spin_ms);

    unsigned int sum = deeper;
    for (unsigned int i = 0; i < 6u; i++) {
        sum += local[i];
    }
    return sum;
}

/* ---- does win_spill_all actually reduce the window to one frame? --------
 *
 * next_moves/08 step 31. The whole per-task window design rests on a task
 * having exactly ONE live frame when it is switched away from, and the thing
 * meant to guarantee that on the voluntary path is win_spill_all(). That has
 * never been measured directly -- only inferred from whether the WiFi driver
 * survived, which is a test with far too much else in it.
 *
 * This holds `depth` live windowed frames, reads WINDOWSTART, spills, and reads
 * it again. Two numbers, no driver, no blob. */
extern volatile unsigned int g_spill_ws_before;
extern volatile unsigned int g_spill_ws_after;
extern volatile unsigned int g_spill_walked, g_spill_bad, g_spill_bad_a0, g_spill_bad_at;
extern void win_spill_all(void);

unsigned int vendor_spilltest(unsigned int depth);

unsigned int vendor_spilltest(unsigned int depth)
{
    unsigned int local[6];
    for (unsigned int i = 0; i < 6u; i++) { local[i] = (depth * 7u) + i; }

    if (depth == 0u) {
        unsigned int ws;
        __asm__ volatile ("rsr.windowstart %0" : "=r"(ws));
        g_spill_ws_before = ws;
        win_spill_all();
        __asm__ volatile ("rsr.windowstart %0" : "=r"(ws));
        g_spill_ws_after = ws;

        /* [step 98] Did the spill write CORRECT save areas?
         *
         * WINDOWSTART going 7 -> 1 says frames left the register file. It does
         * not say what was written for them. _WindowUnderflow8 restores a0 from
         * [sp-16], so after a spill that word must be a windowed return encoding
         * -- bit 31 set -- for every frame on the chain.
         *
         * Walks the chain from this frame's own sp upward through the saved a1
         * links, checking each a0. No blob involved: if this fails, the spill is
         * wrong for our own code too. */
        {
            unsigned int sp;
            __asm__ volatile ("mov %0, a1" : "=r"(sp));
            g_spill_bad = 0u;
            g_spill_walked = 0u;
            for (unsigned int k = 0; k < 12u; k++) {
                if (sp < 0x3ff00000u || sp >= 0x40000000u) { break; }
                unsigned int a0 = ((volatile unsigned int *)(sp - 16u))[0];
                unsigned int a1 = ((volatile unsigned int *)(sp - 12u))[0];
                g_spill_walked++;
                if ((a0 >> 30) == 0u) { g_spill_bad++; if (!g_spill_bad_a0) { g_spill_bad_a0 = a0; g_spill_bad_at = sp; } }
                if (a1 <= sp) { break; }        /* chain must ascend */
                sp = a1;
            }
        }
        return 0u;
    }

    unsigned int deeper = vendor_spilltest(depth - 1u);
    unsigned int sum = deeper;
    for (unsigned int i = 0; i < 6u; i++) { sum += local[i]; }
    return sum;
}


/* ---- a windowed place to wait -------------------------------------------
 *
 * next_moves/08 step 106. The waiting on a blocking adapter entry happens here,
 * in real nested windowed frames, so a context switch landing anywhere in it
 * finds a1 belonging to a windowed frame with a proper save area beneath it.
 *
 * It also keeps the register window genuinely rotating while the driver waits --
 * frames spilling and refilling -- which is the state the blob expects of a task
 * blocked on a queue, rather than a frozen register file. */
unsigned int osi_windowed_idle(unsigned int depth, unsigned int spin);

unsigned int osi_windowed_idle(unsigned int depth, unsigned int spin)
{
    unsigned int local[4];
    for (unsigned int i = 0; i < 4u; i++) { local[i] = depth * 3u + i; }

    if (depth == 0u) {
        unsigned int t0, now;
        __asm__ volatile ("rsr.ccount %0" : "=r"(t0));
        for (;;) {
            __asm__ volatile ("rsr.ccount %0" : "=r"(now));
            if ((now - t0) > spin) { break; }
        }
        return 0u;
    }
    unsigned int deeper = osi_windowed_idle(depth - 1u, spin);
    unsigned int sum = deeper;
    for (unsigned int i = 0; i < 4u; i++) { sum += local[i]; }
    return sum;
}


/* ---- do registers survive a windowed call made while UNPINNED? -----------
 *
 * next_moves/08 step 107. Step 106 found `a6` holding a pointer across
 * `call8 osi_windowed_idle` and coming back as 0x1000 -- a caller-saved register
 * in a windowed frame that did not survive.
 *
 * wintorture and wincollide have never tested this: both reach windowed code
 * through rom_call3, which takes blob_lock and therefore PINS. The scheduler
 * refuses to switch away, so no spill of the caller's frame ever happens. The
 * unpinned case has no coverage at all.
 *
 * This is that case, with no blob, no adapter and no queue: hold magics in
 * locals across a windowed call, unpinned so a tick can land, and report which
 * survived. If they do not, the defect is general and ours.
 */
extern volatile int g_pinned;
extern volatile unsigned int g_unpin_before, g_unpin_after, g_unpin_bad, g_unpin_rounds;

unsigned int vendor_unpintest(unsigned int rounds);

unsigned int vendor_unpintest(unsigned int rounds)
{
    unsigned int bad = 0u;
    int me = g_pinned;
    if (me < 0) { me = 0; }             /* run anyway; the pin is the variable */

    for (unsigned int r = 0; r < rounds; r++) {
        volatile unsigned int m1 = 0xA1A1A1A1u;
        volatile unsigned int m2 = 0xB2B2B2B2u;
        unsigned int k1 = 0xC3C3C3C3u;  /* these two are the ones GCC will try */
        unsigned int k2 = 0xD4D4D4D4u;  /* to keep in registers across the call */

        g_pinned = -1;                  /* UNPIN: a tick may now switch us out */
        (void)osi_windowed_idle(4u, 60000u);
        g_pinned = me;                  /* REPIN before anything else */

        if (m1 != 0xA1A1A1A1u || m2 != 0xB2B2B2B2u) { bad |= 1u; }
        if (k1 != 0xC3C3C3C3u) { bad |= 2u; g_unpin_after = k1; }
        if (k2 != 0xD4D4D4D4u) { bad |= 4u; g_unpin_after = k2; }
        if (bad) { g_unpin_rounds = r; break; }
    }
    g_unpin_bad = bad;
    return bad;
}
