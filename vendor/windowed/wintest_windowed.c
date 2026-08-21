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
        return 0u;
    }

    unsigned int deeper = vendor_spilltest(depth - 1u);
    unsigned int sum = deeper;
    for (unsigned int i = 0; i < 6u; i++) { sum += local[i]; }
    return sum;
}
