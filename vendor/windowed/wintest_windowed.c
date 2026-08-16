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
