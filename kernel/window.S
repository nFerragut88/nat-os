/* nat-os — Xtensa register-window exception handlers.
 *
 * This kernel is built -mabi=call0 and every line of it uses plain call/ret, so
 * none of this ever runs for nat-os's own code. It exists to make WINDOWED code
 * callable — which is the single gate in front of every precompiled Espressif
 * library, WiFi included (UM-NATOS-003 §5.1).
 *
 * ---- why this is safe to add ---------------------------------------------
 *
 * These vectors are currently EMPTY. The linker script places nothing before
 * _vecbase + 0x1C0, so slots 0x000..0x180 are zero-filled, and a window
 * exception today would execute zeros. vectors.S says they are "left as traps
 * rather than omitted"; that was the intent and not what the linker script
 * does.
 *
 * PS.WOE is already SET — the ROM leaves it that way and task.c preserves it
 * when fabricating task frames — so the mechanism is armed and only the
 * handlers are missing.
 *
 * Filling empty vectors is therefore purely additive. Code that never triggers
 * a window exception cannot tell the difference, which is the whole reason this
 * can go in without risking anything that already works.
 *
 * ---- what these do --------------------------------------------------------
 *
 * The Xtensa has 64 physical registers presented as a sliding 16-register
 * window. ENTRY rotates the window forward; when it runs out of physical
 * registers the hardware raises an overflow exception, and the handler spills
 * the oldest window to its stack frame. RETW raises an underflow, and the
 * handler reloads. The spill slots are at fixed negative offsets from the
 * frame pointer, which is why every one of these is a fixed sequence of s32e /
 * l32e and nothing else.
 *
 * Taken verbatim from Espressif's xtensa_vectors.S rather than derived. A wrong
 * register number here does not fault — it silently reloads a caller's frame
 * with the wrong values, which is the least debuggable failure this project
 * could construct. This is the third time today that fetching a definition
 * beat recalling one (UM-NATOS-024 §4).
 *
 * Each vector slot is 64 bytes. The longest handler here is 13 instructions,
 * so all six fit with room to spare.
 */

    .section .vectors.window.of4, "ax"
    .align 4
    .global _WindowOverflow4
_WindowOverflow4:
    s32e    a0, a5, -16             /* save a0 to call[j+1]'s stack frame */
    s32e    a1, a5, -12
    s32e    a2, a5,  -8
    s32e    a3, a5,  -4
    rfwo                            /* rotate back to call[i] */

    .section .vectors.window.uf4, "ax"
    .align 4
    .global _WindowUnderflow4
_WindowUnderflow4:
    l32e    a0, a5, -16
    l32e    a1, a5, -12
    l32e    a2, a5,  -8
    l32e    a3, a5,  -4
    rfwu

    .section .vectors.window.of8, "ax"
    .align 4
    .global _WindowOverflow8
_WindowOverflow8:
    s32e    a0, a9, -16
    l32e    a0, a1, -12             /* a0 <- call[j-1]'s sp */
    s32e    a1, a9, -12
    s32e    a2, a9,  -8
    s32e    a3, a9,  -4
    s32e    a4, a0, -32
    s32e    a5, a0, -28
    s32e    a6, a0, -24
    s32e    a7, a0, -20
    rfwo

    .section .vectors.window.uf8, "ax"
    .align 4
    .global _WindowUnderflow8
_WindowUnderflow8:
    l32e    a0, a9, -16
    l32e    a1, a9, -12
    l32e    a2, a9,  -8
    l32e    a7, a1, -12
    l32e    a3, a9,  -4
    l32e    a4, a7, -32
    l32e    a5, a7, -28
    l32e    a6, a7, -24
    l32e    a7, a7, -20
    rfwu

    .section .vectors.window.of12, "ax"
    .align 4
    .global _WindowOverflow12
_WindowOverflow12:
    s32e    a0,  a13, -16
    l32e    a0,  a1,  -12
    s32e    a1,  a13, -12
    s32e    a2,  a13,  -8
    s32e    a3,  a13,  -4
    s32e    a4,  a0,  -48
    s32e    a5,  a0,  -44
    s32e    a6,  a0,  -40
    s32e    a7,  a0,  -36
    s32e    a8,  a0,  -32
    s32e    a9,  a0,  -28
    s32e    a10, a0,  -24
    s32e    a11, a0,  -20
    rfwo

    .section .vectors.window.uf12, "ax"
    .align 4
    .global _WindowUnderflow12
_WindowUnderflow12:
    l32e    a0,  a13, -16
    l32e    a1,  a13, -12
    l32e    a2,  a13,  -8
    l32e    a11, a1,  -12
    l32e    a3,  a13,  -4
    l32e    a4,  a11, -48
    l32e    a5,  a11, -44
    l32e    a6,  a11, -40
    l32e    a7,  a11, -36
    l32e    a8,  a11, -32
    l32e    a9,  a11, -28
    l32e    a10, a11, -24
    l32e    a11, a11, -20
    rfwu

/* ---- proof that windowed code runs ---------------------------------------
 *
 * A windowed function, hand-written because the build compiles every file
 * -mabi=call0 and one exception would be a build-system change to test a
 * kernel change.
 *
 * win_probe(n) returns n by recursing n deep. Each ENTRY rotates the window;
 * there are 64 physical registers, so at CALL8 depth the hardware runs out
 * after 8 frames and MUST take an overflow. Returning back up must take the
 * matching underflows. A depth of 20 therefore exercises both, several times,
 * and the return value is a checksum over every one of them: any handler that
 * reloads the wrong register returns the wrong number rather than crashing.
 *
 * That is the property worth having. A test that only proves "did not crash"
 * would pass with handlers that corrupt a register nobody happened to read.
 */
    .section .text, "ax"
    .align 4
    .global win_probe
    .type win_probe, @function
win_probe:
    entry   a1, 32
    beqz    a2, .Lbase
    addi    a10, a2, -1             /* callee's a2 = our a10 under CALL8 */
    call8   win_probe
    addi    a2, a10, 1              /* result comes back in a10 */
    retw
.Lbase:
    movi    a2, 0
    retw
    .size win_probe, . - win_probe

/* call0 -> windowed bridge.
 *
 * CALL8 rotates the window by 8, so this caller's a8..a15 become the callee's
 * a0..a7 and are clobbered; a0..a7 survive. A call0 function may therefore call
 * a windowed one directly, provided it treats the upper half as volatile and
 * passes its argument in a10.
 *
 * a0 is saved by hand: CALL8 overwrites the callee's a0 with the return
 * address, and this function's own a0 lives in the half that does not rotate. */
    .align 4
    .global win_call_probe
    .type win_call_probe, @function
win_call_probe:
    addi    a1, a1, -16
    s32i    a0, a1, 0
    mov     a10, a2
    call8   win_probe
    mov     a2, a10
    l32i    a0, a1, 0
    addi    a1, a1, 16
    ret
    .size win_call_probe, . - win_call_probe
