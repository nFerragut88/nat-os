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
    /* [step 126] Refuse to store through a frame pointer that is not DRAM.
     *
     * Step 124 measured the sweep spilling a window whose a1 was 0xeeeeeeee --
     * stack fill, not a pointer. WINDOWSTART said a frame lived there; nothing
     * real did. The handler wrote through the poison and the matching underflow
     * later read the garbage back, which is the epc 0x6eeeeeee panic.
     *
     * `bbsi ..., 31` is the whole test, and it is exact for this machine: DRAM
     * is 0x3FFxxxxx with bit 31 clear, and every fill/poison value this kernel
     * uses has it set. It costs ONE instruction because the skip target is the
     * rfwo that was already there -- which matters, since these vectors are
     * 64-byte slots and an earlier attempt to grow one failed to link.
     *
     * Skipping the stores is correct for a phantom: there is nothing to save.
     * rfwo still clears the WINDOWSTART bit, so the window stops being claimed.
     */
    bbsi    a5, 31, .Lof4_skip
    s32e    a0, a5, -16             /* save a0 to call[j+1]'s stack frame */
    s32e    a1, a5, -12
    s32e    a2, a5,  -8
    s32e    a3, a5,  -4
.Lof4_skip:
    rfwo                            /* rotate back to call[i] */

    .section .vectors.window.uf4, "ax"
    .align 4
    .global _WindowUnderflow4
_WindowUnderflow4:
    l32e    a0, a5, -16
    /* next_moves/08 step 42. Record what this handler recovered as the return
     * address, and the save area it came from. EXCSAVE because these vectors
     * have no stack, no literal pool and 64 bytes of room, and because they run
     * with PS.EXCM set where nothing else is safe. Levels 4 and 5 are unused by
     * this kernel. Overwritten on every underflow, so what survives to a panic
     * is the last one -- which is the one that returned into the fault. */
    wsr.excsave4 a0
    wsr.excsave5 a5
    l32e    a1, a5, -12
    l32e    a2, a5,  -8
    l32e    a3, a5,  -4
    rfwu

    .section .vectors.window.of8, "ax"
    .align 4
    .global _WindowOverflow8
_WindowOverflow8:
    /* [step 126] Refuse to store through a frame pointer that is not DRAM.
     *
     * Step 124 measured the sweep spilling a window whose a1 was 0xeeeeeeee --
     * stack fill, not a pointer. WINDOWSTART said a frame lived there; nothing
     * real did. The handler wrote through the poison and the matching underflow
     * later read the garbage back, which is the epc 0x6eeeeeee panic.
     *
     * `bbsi ..., 31` is the whole test, and it is exact for this machine: DRAM
     * is 0x3FFxxxxx with bit 31 clear, and every fill/poison value this kernel
     * uses has it set. It costs ONE instruction because the skip target is the
     * rfwo that was already there -- which matters, since these vectors are
     * 64-byte slots and an earlier attempt to grow one failed to link.
     *
     * Skipping the stores is correct for a phantom: there is nothing to save.
     * rfwo still clears the WINDOWSTART bit, so the window stops being claimed.
     */
    bbsi    a9, 31, .Lof8_skip
    s32e    a0, a9, -16
    l32e    a0, a1, -12             /* a0 <- call[j-1]'s sp */
    s32e    a1, a9, -12
    s32e    a2, a9,  -8
    s32e    a3, a9,  -4
    bbsi    a0, 31, .Lof8_skip      /* the recovered caller sp, same test */
    s32e    a4, a0, -32
    s32e    a5, a0, -28
    s32e    a6, a0, -24
    s32e    a7, a0, -20
.Lof8_skip:
    rfwo

    .section .vectors.window.uf8, "ax"
    .align 4
    .global _WindowUnderflow8
_WindowUnderflow8:
    l32e    a0, a9, -16
    /* next_moves/08 step 42. Record what this handler recovered as the return
     * address, and the save area it came from. EXCSAVE because these vectors
     * have no stack, no literal pool and 64 bytes of room, and because they run
     * with PS.EXCM set where nothing else is safe. Levels 4 and 5 are unused by
     * this kernel. Overwritten on every underflow, so what survives to a panic
     * is the last one -- which is the one that returned into the fault. */
    wsr.excsave4 a0
    wsr.excsave5 a9
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
    /* [step 126] Refuse to store through a frame pointer that is not DRAM.
     *
     * Step 124 measured the sweep spilling a window whose a1 was 0xeeeeeeee --
     * stack fill, not a pointer. WINDOWSTART said a frame lived there; nothing
     * real did. The handler wrote through the poison and the matching underflow
     * later read the garbage back, which is the epc 0x6eeeeeee panic.
     *
     * `bbsi ..., 31` is the whole test, and it is exact for this machine: DRAM
     * is 0x3FFxxxxx with bit 31 clear, and every fill/poison value this kernel
     * uses has it set. It costs ONE instruction because the skip target is the
     * rfwo that was already there -- which matters, since these vectors are
     * 64-byte slots and an earlier attempt to grow one failed to link.
     *
     * Skipping the stores is correct for a phantom: there is nothing to save.
     * rfwo still clears the WINDOWSTART bit, so the window stops being claimed.
     */
    bbsi    a13, 31, .Lof12_skip
    s32e    a0,  a13, -16
    l32e    a0,  a1,  -12
    s32e    a1,  a13, -12
    s32e    a2,  a13,  -8
    s32e    a3,  a13,  -4
    bbsi    a0, 31, .Lof12_skip     /* the recovered caller sp, same test */
    s32e    a4,  a0,  -48
    s32e    a5,  a0,  -44
    s32e    a6,  a0,  -40
    s32e    a7,  a0,  -36
    s32e    a8,  a0,  -32
    s32e    a9,  a0,  -28
    s32e    a10, a0,  -24
    s32e    a11, a0,  -20
.Lof12_skip:
    rfwo

    .section .vectors.window.uf12, "ax"
    .align 4
    .global _WindowUnderflow12
_WindowUnderflow12:
    l32e    a0,  a13, -16
    /* next_moves/08 step 42. Record what this handler recovered as the return
     * address, and the save area it came from. EXCSAVE because these vectors
     * have no stack, no literal pool and 64 bytes of room, and because they run
     * with PS.EXCM set where nothing else is safe. Levels 4 and 5 are unused by
     * this kernel. Overwritten on every underflow, so what survives to a panic
     * is the last one -- which is the one that returned into the fault. */
    wsr.excsave4 a0
    wsr.excsave5 a13
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
 * CALL8 rotates the window by 8, so this caller's a8..a15 physically BECOME the
 * callee's a0..a7 and come back holding whatever the callee left in them.
 * Arguments therefore move up by eight and the result returns in a10.
 *
 * ---- and a12..a15 must be saved by hand ----------------------------------
 *
 * Under the call0 ABI a12..a15 are CALLEE-SAVED: a C caller is entitled to keep
 * a live value in one across a call and find it intact afterwards. Under the
 * windowed ABI that same register range is the outgoing-argument area and is
 * volatile. A bridge between the two therefore has to honour the call0 contract
 * itself, because neither compiler will.
 *
 * Found by evidence rather than inspection. The first version saved only a0,
 * and the shell printed the depth argument as 1073433976 — the C caller had
 * parked it in a callee-saved register that CALL8 then destroyed. The RESULT
 * was correct, so a test checking only the return value would have passed and
 * shipped a bridge that silently corrupts its caller.
 *
 * a0 is saved for a different reason: CALL8 writes the return address into the
 * callee's a0, and this function's own a0 is in the half that does not rotate. */
    .align 4
    .global win_call_probe
    .type win_call_probe, @function
win_call_probe:
    addi    a1, a1, -32
    s32i    a0,  a1, 0
    s32i    a12, a1, 4              /* see the note above: CALL8 eats these */
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    mov     a10, a2
    call8   win_probe
    mov     a2, a10
    l32i    a0,  a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    ret
    .size win_call_probe, . - win_call_probe

/* Same bridge, for the two-argument windowed function in vendor/windowed/.
 *
 * That object is compiled -mabi=windowed by the same compiler that produces
 * Espressif's libraries, so it stands in for a vendor blob: if this call works,
 * the mechanism for calling theirs works.
 *
 * Under CALL8 the callee's a2..a7 are this caller's a10..a15, so arguments move
 * up by eight and the result comes back in a10. Anything a call0 caller needs
 * to keep must live in a0..a7, which do not rotate. */
    .align 4
    .global win_call_vendor
    .type win_call_vendor, @function
win_call_vendor:
    addi    a1, a1, -32
    s32i    a0,  a1, 0
    s32i    a12, a1, 4              /* callee-saved under call0, volatile under */
    s32i    a13, a1, 8              /* windowed — this bridge owes the caller   */
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    mov     a10, a2                 /* depth */
    mov     a11, a3                 /* seed  */
    call8   vendor_probe
    mov     a2, a10
    l32i    a0,  a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    ret
    .size win_call_vendor, . - win_call_vendor

/* Generic call0 -> windowed bridge for a three-argument function at a runtime
 * address.
 *
 * This is the one that matters. Espressif's ROM holds hundreds of routines at
 * fixed addresses, all windowed ABI, and every one of them becomes reachable
 * through this: no linking, no relocation, no environment — just an address.
 * The same shape works for a function inside a linked blob.
 *
 * CALLX8 takes its target from a register that must survive the window
 * rotation, so it is read from a2, which is in the half that does not rotate.
 * Arguments move to a10..a12 and the result returns in a10.
 *
 *   uint32_t rom_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);
 */
    .align 4
/* Four-argument windowed call.
 *
 * rom_call3 was enough until the clock bring-up needed rom_i2c_writeReg, which
 * takes (block, host_id, reg_add, data). Identical to rom_call3 with one more
 * argument moved into the callee's window; kept as its own symbol rather than
 * generalising, because the register moves are the whole body and a shared
 * version would be longer than both.
 */
/* .align 4 is REQUIRED, not tidiness. call0 encodes its target as an offset in
 * units of four bytes, so a misaligned entry point cannot be reached by a direct
 * call and the linker rejects it with "dangerous relocation: call0: misaligned
 * call target".
 *
 * These two were the only globals in this file without it. It stayed latent
 * because the surrounding layout happened to land them on 4-byte boundaries;
 * moving shell.c into flash changed the layout and the link failed immediately.
 * A latent alignment bug is exactly the kind that surfaces when something
 * unrelated moves. */
    .global rom_call4
    .type rom_call4, @function
    .align 4
rom_call4:
    addi    a1, a1, -32

    /* Base save area, for the reason spelled out in win_spill_call0 and in
     * phy_stack_call: _WindowOverflow8/12 recover the caller's sp with
     * `l32e a0, a1, -12` and spill through it. A windowed `entry` writes that
     * slot; this function is call0 and must write it by hand, or a spill that
     * unwinds this far stores through whatever was already at [sp-12] -- from
     * inside a window vector, with PS.EXCM set. */
    addi    a8, a1, 32
    addi    a9, a1, -12
    s32i    a8, a9, 0
    /* [step 92] and the a0 slot, which the underflow also reads. */
    movi    a10, win_chain_trap
    addi    a11, a1, -16
    s32i    a10, a11, 0

    s32i    a0,  a1, 0
    s32i    a12, a1, 4              /* callee-saved under call0; CALL8 eats them */
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    mov     a10, a3
    mov     a11, a4
    mov     a12, a5
    mov     a13, a6
    callx8  a2
    mov     a2,  a10
    l32i    a0,  a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    ret
    .size rom_call4, . - rom_call4

/* ---- pushing live windows out to memory, from TASK context ------------
 *
 * Six nested CALL12s consume 72 registers against a 64-register file, so the
 * hardware raises overflows and _WindowOverflow* writes each oldest live frame
 * to the stack pointer recorded IN that frame -- the frame's own owner's
 * stack, which is exactly where it belongs.
 *
 * next_moves/08 step 14 called this from inside _handler_level3 and it broke
 * the single-task case: rotating the window mid-context-switch, while a1 is
 * the frame under construction, does not work. Here it runs in ordinary task
 * context, on a normal stack, outside any handler.
 *
 * Windowed by necessity -- ENTRY is what does the work. */
    .section .text, "ax"
    .align 4
    .global win_spill_all
    .type win_spill_all, @function
win_spill_all:
    entry   a1, 48
    call12  .Lws1
    retw
    .align 4
.Lws1:
    entry   a1, 48
    call12  .Lws2
    retw
    .align 4
.Lws2:
    entry   a1, 48
    call12  .Lws3
    retw
    .align 4
.Lws3:
    entry   a1, 48
    call12  .Lws4
    retw
    .align 4
.Lws4:
    entry   a1, 48
    call12  .Lws5
    retw
    .align 4
.Lws5:
    entry   a1, 48
    retw
    .size win_spill_all, . - win_spill_all

/* ---- spilling from call0 code -----------------------------------------
 *
 * win_spill_all is windowed, so call0 kernel code cannot simply call it -- that
 * is the CALL8-into-call0 mismatch this file has recorded four times, from the
 * other direction. This is the same shape as rom_call3 with no target and no
 * lock: save what CALL8 eats, rotate into the spill, come back.
 *
 * Exists so task_block()/task_sleep() can leave the register file holding
 * exactly one frame for this task before it parks. See next_moves/08 step 34. */
    .align 4
    .global win_spill_call0
    .type win_spill_call0, @function
win_spill_call0:
    addi    a1, a1, -32

    /* Write this frame's BASE SAVE AREA before rotating into windowed code.
     *
     * _WindowOverflow8/12 recover the caller's stack pointer with
     * `l32e a0, a1, -12` and then spill through it. A windowed callee's `entry`
     * writes that slot as part of its prologue; a call0 function does not, so a
     * call0 frame that windowed code rotates over hands the overflow handler
     * whatever happens to be at [sp-12] -- and the handler then stores through
     * it, inside an exception context with PS.EXCM set.
     *
     * That is the fault: StoreProhibited to 0x000004a2 in step 30, and the
     * IllegalInstruction at win_spill_all's retw in step 36, both with
     * ps 0x00030210 -- EXCM 1, WOE 0, UM 0, the state of a window vector rather
     * than of task code.
     *
     * phy_stack_call already does exactly this and says so in its own comment;
     * this bridge was written from rom_call3's shape and inherited the gap.
     * s32i takes no negative offset, hence the second addi. */
    addi    a8, a1, 32              /* the caller's sp */
    addi    a9, a1, -12
    s32i    a8, a9, 0

    s32i    a0,  a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    call8   win_spill_all
    l32i    a0,  a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    ret
    .size win_spill_call0, . - win_spill_call0

/* Where an underflow lands if it unwinds into a call0 bridge's frame.
 *
 * next_moves/08 step 92. The bridges write [sp-12] -- the caller's sp, which
 * _WindowOverflow8/12 recover with `l32e a0, a1, -12` -- but never [sp-16].
 * _WindowUnderflow8 reads BOTH: `l32e a0, a9, -16` then `l32e a1, a9, -12`. So
 * the a0 slot was uninitialised and the handler recovered whatever the blob had
 * left there. Measured: 0x3ffd8f78, an address inside the blob's .bss, used as a
 * return address.
 *
 * The address of this label doubles as its own return encoding: kernel code
 * lives at 0x4008xxxx, so bits 31:30 are already 01 -- CALLINC 1 -- and `retw`
 * accepts it without any arithmetic. */
    .align 4
    .global win_chain_trap
    .type win_chain_trap, @function
win_chain_trap:
    movi    a2, .Lchainmsg
    movi    a3, 0
    call0   kernel_panic_msg
1:  j       1b
    .size win_chain_trap, . - win_chain_trap

    .section .rodata
    .align 4
.Lchainmsg:
    .asciz "windowed chain unwound into a call0 bridge frame"
    .section .text
    .align 4

    .global rom_call3
    .type rom_call3, @function
    .align 4
rom_call3:
    addi    a1, a1, -48

    /* Base save area, for the reason spelled out in win_spill_call0 and in
     * phy_stack_call: _WindowOverflow8/12 recover the caller's sp with
     * `l32e a0, a1, -12` and spill through it. A windowed `entry` writes that
     * slot; this function is call0 and must write it by hand, or a spill that
     * unwinds this far stores through whatever was already at [sp-12] -- from
     * inside a window vector, with PS.EXCM set. */
    addi    a8, a1, 48
    addi    a9, a1, -12
    s32i    a8, a9, 0
    /* [step 92] and the a0 slot, which the underflow also reads. */
    movi    a10, win_chain_trap
    addi    a11, a1, -16
    s32i    a10, a11, 0

    s32i    a0,  a1, 0
    s32i    a12, a1, 4              /* callee-saved under call0; CALL8 eats them */
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16

    /* Take the blob lock for the duration of this excursion into windowed
     * code. EXCLUSION, not a marker: a pin that only records the current task
     * is overwritten by the next caller, so both enter and collide.
     *
     * blob_lock pins as a side effect, so the scheduler will not switch away
     * either. Blocking adapter entries spill and release it, so a wait does
     * not deadlock against the task it is waiting for. */
    /* a2..a5 are the target and its arguments, and call0 CLOBBERS a2..a11 --
     * they have to be stacked across the lock call. (The spill below is a
     * CALL8, which only eats a8..a15, so they survive that one untouched.) */
    s32i    a2,  a1, 24
    s32i    a3,  a1, 28
    s32i    a4,  a1, 32
    s32i    a5,  a1, 36
    call0   blob_lock
    l32i    a2,  a1, 24
    l32i    a3,  a1, 28
    l32i    a4,  a1, 32
    l32i    a5,  a1, 36

    /* NO SPILL HERE, and its absence is load-bearing.
     *
     * Step 16 spilled the previously-running task's live frames out to its own
     * stack before rotating over them. That was compensating for the absence of
     * exclusion: without a lock, another task really could be sitting on live
     * windowed frames when this one rotated the window.
     *
     * The lock above removes that case at the source. The only way to hold live
     * windowed frames is to hold the lock, and a task that released it has
     * already unwound them, so there is never anything here to spill.
     *
     * Keeping the spill anyway was not merely redundant, it WEDGED: called from
     * a freshly spawned task, win_spill_all never returned, the pin then starved
     * every other task, and the board died on the hang detector three seconds
     * later. Bisected -- removing this one call turned wincollide from a
     * watchdog reset into 156 runs with zero wrong checksums.
     *
     * win_spill_all itself stays; the blocking adapter entries call it for a
     * different reason (reducing a blocked task to ONE live frame), from inside
     * windowed code where frames genuinely are live. */

    mov     a10, a3
    mov     a11, a4
    mov     a12, a5
    callx8  a2
    mov     a2,  a10
    s32i    a2,  a1, 20             /* preserve the result across a call0 */
    call0   blob_unlock
    l32i    a2,  a1, 20
    l32i    a0,  a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 48
    ret
    .size rom_call3, . - rom_call3

/* ---- windowed -> call0 -------------------------------------------------
 *
 * The other direction, and the one the OSI table needs.
 *
 * libpp calls the OSI entries, so those are windowed. Their bodies want the
 * kernel's heap, mutexes and scheduler, which are call0. A windowed function
 * cannot simply CALL8 a call0 one: the window rotates forward, the callee uses
 * the rotated registers as its own, and its RET does not rotate back. That is
 * the fault that put IllegalInstruction at 0x4008a810 the first time.
 *
 * These bridges do it properly. ENTRY establishes a windowed frame, the call0
 * callee is invoked with CALLX0 under its own convention, and RETW unwinds the
 * window. a0 must be saved by hand: after ENTRY it holds the return address
 * RETW needs, and CALLX0 overwrites it.
 *
 * Arguments arrive in a2.. as usual for a windowed callee (the caller's a10..),
 * and a2 is both the call0 first argument and the return register, which is why
 * the shuffles below are as short as they are.
 */
    .section .text, "ax"
    .align 4
    .global w2c_call0f
    .type w2c_call0f, @function

w2c_call0f:                         /* uint32_t w2c_call0f(fn) */
    entry   a1, 32
    /* [step 85] a0 in a12, not on the stack -- see w2c_call2. [sp+0] upward is
     * the call0 callee's argument/spill area and it writes there legitimately;
     * a12..a15 are callee-saved under call0, so a12 is both correct and free. */
    mov     a12, a0
    mov     a8, a2
    callx0  a8
    mov     a0, a12
    /* [step 79] What came BACK, and from where. Same address + different value
     * means the callee wrote into this frame; different address means the
     * reload sourced from a moved a1. */
    /* [step 79, corrected] STICKY, and self-contained.
     *
     * The first attempt wrote singleton globals at entry and at the reload and
     * compared them. Those are different invocations whenever another task
     * calls through here in between -- which it does: the pair came back as
     * 0x3ffb91c0 and 0x3ffb2760, two different task stacks. Comparing them
     * measured nothing.
     *
     * No pairing is needed. At the reload, a1 IS this frame's pointer and
     * [a1+0] is the slot it just read, so a0 and a1 together are self-contained
     * evidence. Latch only the ILLEGAL case -- callinc 0, which is what makes
     * the retw fault -- and only the first, so the record belongs to the
     * fatal crossing and nothing overwrites it afterwards. */
    extui   a9, a0, 30, 2           /* a0[31:30], the callinc field */
    bnez    a9, 1f                  /* legal encoding: nothing to record */
    movi    a10, g_a0trace
    l32i    a11, a10, 0
    bnez    a11, 1f                 /* already latched the first one */
    movi    a11, 1
    s32i    a11, a10, 0
    s32i    a0, a10, 4              /* the illegal a0 */
    s32i    a1, a10, 8              /* the frame it was read from */
    rsr.windowbase a11
    s32i    a11, a10, 12
1:
    retw
    .size w2c_call0f, . - w2c_call0f

    .align 4
    .global w2c_call1
    .type w2c_call1, @function
w2c_call1:                          /* uint32_t w2c_call1(fn, a) */
    entry   a1, 32
    /* [step 85] a0 in a12, not on the stack -- see w2c_call2. [sp+0] upward is
     * the call0 callee's argument/spill area and it writes there legitimately;
     * a12..a15 are callee-saved under call0, so a12 is both correct and free. */
    mov     a12, a0
    mov     a8, a2
    mov     a2, a3
    callx0  a8
    mov     a0, a12
    retw
    .size w2c_call1, . - w2c_call1

    .align 4
    .global w2c_call2
    .type w2c_call2, @function
w2c_call2:                          /* uint32_t w2c_call2(fn, a, b) */
    entry   a1, 32
    /* [X18] capture at ENTRY too, plus a crossing counter: the exit-side
     * samples are overwritten by every healthy pass, so without these we
     * cannot tell whether the dumped state belongs to the fatal crossing. */
    rsr.ps  a9
    movi    a11, g_win_in
    s32i    a9, a11, 0
    rsr.windowstart a9
    s32i    a9, a11, 4
    rsr.windowbase  a9
    s32i    a9, a11, 8
    movi    a10, g_win_seq
    l32i    a9, a10, 0
    addi    a9, a9, 1
    s32i    a9, a10, 0
    /* [step 85] a0 lives in a12 across the callx0, NOT on the stack.
     *
     * This saved a0 at [a1+0]. Measured: the word comes back as 13 and the four
     * words at [a1+0..12] hold 0x0000000d / 0x00000700 / 0x3ffb0c78 / 0 --
     * identical across runs and unchanged when the save was moved to [a1+4], so
     * the readback follows the writer, not our store. 0x700 is 1792, which is
     * BLOB_TASK_STACK_WORDS, and 0x3ffb0c78 is inside the blob task's stack:
     * that is the call0 callee's own data.
     *
     * [sp+0] upward is the call0 caller-provided argument/spill area, so the
     * callee is entitled to it and this bridge was never entitled to store
     * there. a12..a15 are callee-saved under call0, so the callee must preserve
     * them -- which makes a12 both correct and free. */
    mov     a12, a0
    /* [step 83] Stamp the slot and remember where it is.
     *
     * a0 is written exactly once in this function -- by the reload -- so a0 at
     * the retw IS [a1+0]. Step 82 measured that as 13, which means this word is
     * overwritten while the task is parked. Recording the address lets the
     * corrupting store be identified by destination rather than by argument. */
    movi    a9, g_slotwatch
    s32i    a1, a9, 0               /* [X: slot moved to sp+4 for this run] */
    s32i    a0, a9, 4               /* what we put there */
    /* [step 79] What was SAVED, and where. a0 = 13 at the retw (callinc 0) is
     * why that instruction is illegal; this says whether 13 was already in the
     * slot at entry, or arrived while the call0 callee ran. */
    mov     a8, a2
    mov     a2, a3
    mov     a3, a4
    /* [X19] bracket the vendor call: state immediately before callx0. */
    rsr.ps  a9
    movi    a11, g_win_mid
    s32i    a9, a11, 0
    rsr.windowstart a9
    s32i    a9, a11, 4
    rsr.windowbase  a9
    s32i    a9, a11, 8
    callx0  a8
    mov     a0, a12
    /* [step 83] Did the slot survive? Compare against what was stamped, and
     * latch the first divergence together with the frame address. */
    movi    a9, g_slotwatch
    l32i    a10, a9, 0              /* the frame we stamped */
    bne     a10, a1, 2f             /* different frame: another invocation */
    l32i    a10, a9, 4              /* what we stamped */
    beq     a10, a0, 2f             /* unchanged: nothing to report */
    l32i    a11, a9, 8
    bnez    a11, 2f                 /* already latched the first */
    movi    a11, 1
    s32i    a11, a9, 8
    s32i    a10, a9, 12             /* stamped value */
    s32i    a0,  a9, 16             /* what came back */
    s32i    a1,  a9, 20             /* the frame */
    /* [step 83b] The neighbours. 13 is WINDOWBASE, and _handler_level3 writes
     * WINDOWBASE at offset 84 of its 112-byte switch frame with WINDOWSTART at
     * 88. If a switch frame overlaps this slot, [sp+4] holds a windowstart-
     * shaped value and the signature is unmistakable. Four words distinguish a
     * switch frame from a four-word window spill from a single store. */
    l32i    a10, a1, 0
    s32i    a10, a9, 24
    l32i    a10, a1, 8
    s32i    a10, a9, 28
    l32i    a10, a1, 12
    s32i    a10, a9, 32
2:
    /* Diagnostic: a0 and a1 as they are on the way out. The blocking adapter
     * path returns through here after the task has been switched away and
     * back, so these are the two values that say whether the context came back
     * intact or came back as zeros. */
    movi    a9, g_win_a0
    s32i    a0, a9, 0
    s32i    a1, a9, 4
    /* [X16] PS as it stands immediately before the retw.n that traps.
     * Splits "WOE was cleared somewhere between resume and here" from
     * "retw.n trapped under a clean PS" (which would mean the exccause
     * itself is lying). */
    rsr.ps  a10
    movi    a11, g_win_ps
    s32i    a10, a11, 0
    /* [X17] Window state in the same instant: is the caller-frame bit set,
     * i.e. will this retw.n need an underflow refill at all? */
    rsr.windowstart a10
    s32i    a10, a11, 4
    rsr.windowbase  a10
    s32i    a10, a11, 8
    /* [step 80] RING, not a singleton.
     *
     * Every previous capture here was a global overwritten by the next healthy
     * crossing, so the dumped value need not belong to the fatal one -- that
     * error was made twice in this file (steps 78, 79). Eight entries of
     * {seq, a0, a1, WINDOWSTART} recorded at the retw itself, so the newest
     * entry IS the faulting crossing: identifiable rather than assumed. Same
     * shape as X7's restore history, the one instrument here that has not lied.
     *
     * idx*16 with no multiply: addx8 twice. */
    movi    a10, g_xseq
    l32i    a9, a10, 0
    addi    a9, a9, 1
    s32i    a9, a10, 0
    extui   a11, a9, 0, 3           /* idx = seq & 7 */
    movi    a10, g_xring
    addx8   a10, a11, a10
    addx8   a10, a11, a10           /* g_xring + idx*16 */
    s32i    a9, a10, 0
    s32i    a0, a10, 4              /* a0 exactly as the retw will see it */
    s32i    a1, a10, 8
    rsr.windowstart a9
    s32i    a9, a10, 12

    retw
    .size w2c_call2, . - w2c_call2

    .align 4
    .global w2c_call3
    .type w2c_call3, @function
w2c_call3:                          /* uint32_t w2c_call3(fn, a, b, c) */
    entry   a1, 32
    /* [step 85] a0 in a12, not on the stack -- see w2c_call2. [sp+0] upward is
     * the call0 callee's argument/spill area and it writes there legitimately;
     * a12..a15 are callee-saved under call0, so a12 is both correct and free. */
    mov     a12, a0
    mov     a8, a2
    mov     a2, a3
    mov     a3, a4
    mov     a4, a5
    callx0  a8
    mov     a0, a12
    /* [step 97] Did a12 survive the call0 callee?
     *
     * Step 85 moved the windowed a0 into a12 because the call0 ABI makes
     * a12..a15 callee-saved. That is an assumption about every function reached
     * through this bridge, and it has never been tested. A restored a0 whose top
     * two bits are zero is not a windowed return encoding, which means a12 did
     * not survive -- latched once, with the callee's address, so the offender is
     * named rather than inferred. */
    extui   a9, a0, 30, 2
    bnez    a9, 3f                  /* valid encoding: a12 survived */
    movi    a10, g_a12bad
    l32i    a11, a10, 0
    bnez    a11, 3f                 /* already latched */
    movi    a11, 1
    s32i    a11, a10, 0
    s32i    a0,  a10, 4             /* what a12 came back as */
    s32i    a8,  a10, 8             /* the callee we had just run */
3:
    retw
    .size w2c_call3, . - w2c_call3

/* ---- [X20 experiment] retw.n with a cleared caller bit -------------------
 *
 * Failure 2's last measured state: a retw.n whose target WINDOWSTART bit had
 * been erased by the wake-side single-bit grant (vectors.S .Lresume), with
 * PS.WOE=1 EXCM=0 -- yet no window exception ever ran (excsave4/5 stayed
 * task-9-stale). Per ISA that retw.n must raise WindowUnderflow8. Crossings
 * 1-8 never hit the condition because their caller bit was still set.
 *
 * This probe asks the silicon directly, isolated from tasks and grants:
 *   x20_run (call0 wrapper) --call8--> x20_windowed
 * entry rotates +2 exactly like the bridges do; x20_windowed then wipes
 * WINDOWSTART down to its own bit only -- precisely what grant #4103 did to
 * bit 11 -- plants a call8 save image below its sp in _WindowUnderflow8's
 * exact load layout, records pre/post state, and executes retw.n.
 *
 * Outcome A (silicon quirk): cause 0 AT this retw.n -- panic names exccause
 * 0 inside x20_windowed. Mechanism closed.
 * Outcome B (ISA holds): uf8 fires, refills the planted image, control
 * lands on x20_landing; excsave4/5 name the handler run and g_x20_recovered
 * carries the planted magics 0x5a1/0x5a2 as proof. The fatal path then
 * differs somewhere else and the hunt continues there.
 *
 * Isolated and removable; everything is tagged X20.
 */

    .section .text, "ax"
    .align 4
    .global x20_run
    .type x20_run, @function
x20_run:
    /* call0 wrapper, win_spill_call0's shape: CALL8 eats a8..a15, so save
     * what matters and write this frame's base save area first. That chain
     * word doubles as the refill's T pointer: uf8 fetches caller a4..a7 via
     * [fake_a1 - 12], which lands here -- it reads stale bytes from inside
     * the shell frame, harmless, nothing dereferences them before landing
     * restores the real registers. */
    addi    a1, a1, -32
    addi    a8, a1, 32              /* the shell caller's sp */
    addi    a9, a1, -12
    s32i    a8, a9, 0               /* [a1_R-12] = C_sp: doubles as T */
    s32i    a0,  a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16

    movi    a9, g_x20_outcome
    movi    a10, 1                  /* entered */
    s32i    a10, a9, 0

    call8   x20_windowed

x20_landing:
    /* Reached only if the underflow fired: the refilled a0 points here.
     * If the wipe somehow failed and rotation was clean we also arrive
     * here -- the magics recorded below tell the two cases apart.
     * a1 = planted fake a1 = this frame's adjusted sp. */
    movi    a9, g_x20_outcome
    movi    a10, 2                  /* landed */
    s32i    a10, a9, 0
    movi    a9, g_x20_recovered
    s32i    a2, a9, 0               /* expect 0x5a1 iff the image was consumed */
    s32i    a3, a9, 4               /* expect 0x5a2 */
    rsr.excsave4 a10
    s32i    a10, a9, 8              /* handler-recovered return address */
    rsr.excsave5 a10
    s32i    a10, a9, 12             /* handler-recovered save-area base */

    l32i    a0,  a1, 0              /* normal epilogue: real state restored */
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    ret
    .size x20_run, . - x20_run

    .align 4
    .global x20_windowed
    .type x20_windowed, @function
x20_windowed:
    entry   a1, 48
    s32i    a0, a1, 0

    /* pre-wipe record */
    rsr.ps  a9
    movi    a10, g_x20_pre
    s32i    a9, a10, 0
    rsr.windowstart a9
    s32i    a9, a10, 4
    rsr.windowbase  a9
    s32i    a9, a10, 8

    /* plant the call8 refill image below this frame:
     *   [-16] landing   <- refills a0
     *   [-12] fake a1   <- this+48 == x20_run's adjusted sp
     *   [ -8] 0x5a1     <- refills a2
     *   [ -4] 0x5a2     <- refills a3
     * The a4..a7 leg goes through [fake_a1 - 12] = x20_run's base save
     * area chain word, already written by the wrapper.
     */
    movi    a9, x20_landing
    addi    a6, a1, -16
    s32i    a9, a6, 0
    addi    a9, a1, 48
    s32i    a9, a6, 4
    movi    a9, 0x5a1
    s32i    a9, a6, 8
    movi    a9, 0x5a2
    s32i    a9, a6, 12

    /* THE WIPE: WINDOWSTART := own bit only -- grant #4103 semantics */
    rsr.windowbase  a9
    movi    a10, 1
    ssl     a9
    sll     a10, a10
    wsr.windowstart a10
    rsync
    /* [step 174] who wrote WINDOWSTART. Counter + last value, per site. */
    movi    a9, g_wsw
    l32i    a11, a9, 8
    addi    a11, a11, 1
    s32i    a11, a9, 8
    s32i    a10, a9, 12
    movi    a9, g_x20_post_ws
    s32i    a10, a9, 0

    /* [step 138] Tell the bookkeeping what the wipe just did.
     *
     * The comment above is right that these bits must not be put back -- the
     * registers behind them are gone. But g_win_mask[] still records those
     * frames as owned, so the next restore ORs them straight back in via
     * g_win_union, re-marking frames whose contents this excursion destroyed.
     * That is the phantom of step 124: a bit set for a window holding nothing.
     *
     * A flag rather than the array itself, because g_win_mask is static to
     * task.c and the clearing wants to happen where the scheduler can see it. */
    movi    a9, g_win_disowned
    movi    a10, 1
    s32i    a10, a9, 0


    l32i    a0, a1, 0
    retw.n                          /* THE TEST INSTRUCTION */
    .size x20_windowed, . - x20_windowed

    .align 4
    .global win_call_bridge
    .type win_call_bridge, @function
win_call_bridge:                    /* call0 -> windowed -> call0, round trip */
    addi    a1, a1, -32
    s32i    a0,  a1, 0
    s32i    a12, a1, 4
    s32i    a13, a1, 8
    s32i    a14, a1, 12
    s32i    a15, a1, 16
    mov     a10, a2
    mov     a11, a3
    call8   vendor_bridge_probe
    mov     a2, a10
    l32i    a0,  a1, 0
    l32i    a12, a1, 4
    l32i    a13, a1, 8
    l32i    a14, a1, 12
    l32i    a15, a1, 16
    addi    a1, a1, 32
    ret
    .size win_call_bridge, . - win_call_bridge

/* ---- calling the PHY on a stack big enough for it ----------------------
 *
 * chip_v7_set_chan_nomac faulted with StoreProhibited at a `call8` deep inside
 * ram_chip_i2c_readReg. The store that failed was not the RF access -- it was
 * the WINDOW OVERFLOW spill that the call8 triggered, writing register state to
 * a stack that had no room left.
 *
 * nat-os gives every task 2 KB (TASK_STACK_WORDS). That is generous for call0
 * kernel code, which this kernel is, and far too little for windowed blob code:
 * each nested call8 can spill four registers, and the PHY nests deeply. ESP-IDF
 * gives its WiFi tasks 4 KB and more. Raising every task to match would cost
 * 24 KB of DRAM on a board with about 21 KB of heap left.
 *
 * So the PHY gets its own stack for the duration of the call, and only the
 * tasks that actually enter the blob pay for it. The panic handler already does
 * exactly this with _panic_stack, for the same reason: the stack you are on may
 * be the thing that is wrong.
 *
 *   uint32_t phy_stack_call(uint32_t fn, uint32_t a, uint32_t b,
 *                           uint32_t c, uint32_t d);
 *
 * Saves the caller's stack pointer INTO the new stack rather than a register,
 * because call8 destroys a12..a15 and there is nowhere else that survives.
 */
    .section .text, "ax"
    .align 4
    .global phy_stack_call
    .type phy_stack_call, @function
phy_stack_call:
    movi    a8, _phy_stack_top
    addi    a8, a8, -64
    s32i    a1,  a8, 0              /* the stack we came from */
    s32i    a0,  a8, 4
    s32i    a12, a8, 8              /* callee-saved under call0; call8 eats them */
    s32i    a13, a8, 12
    s32i    a14, a8, 16
    s32i    a15, a8, 20

    /* THE BASE FRAME MUST LOOK LIKE A WINDOWED ONE.
     *
     * This is what the StoreProhibited actually was, and it is an ABI
     * requirement that call0 code has no reason to know about.
     *
     * _WindowOverflow8 spills a4..a7 relative to the CALLER'S stack pointer,
     * which it fetches with `l32e a0, a1, -12`. Every windowed frame is
     * required to hold its caller's sp in that slot; ENTRY does not write it,
     * the caller's prologue does. phy_stack_call is call0 and wrote nothing
     * there, so the handler loaded a fresh .bss zero and spilled to 0 - 32.
     *
     * That is also why the faulting PC looked like the CALL8 itself. The spill
     * faults inside the overflow handler, which runs with PS.EXCM set, so the
     * second fault vectors to the double-exception handler while EPC1 still
     * holds the instruction that caused the ORIGINAL window overflow.
     *
     * The slot points at the very top of the private stack, so the 32 bytes
     * the handler writes below it (top-32 .. top-20) land above this frame's
     * own saves (top-64 .. top-32) and collide with nothing.
     *
     * Two earlier theories were wrong and are recorded because the measurements
     * that killed them were the useful part: it was not stack DEPTH -- the
     * fault came 272 bytes into 6144 -- and it was not stale WINDOWSTART bits,
     * since declaring a single live frame did not change it either. */
    /* The caller's REAL stack pointer, not a marker.
     *
     * This wrote `_phy_stack_top` -- a plausible-looking value chosen so
     * _WindowOverflow8/12 would find something sane at [sp-12] and stop
     * spilling through a .bss zero. It fixed that StoreProhibited and created a
     * quieter bug in the mirror direction: _WindowUnderflow8/12 restores a1 from
     * this same slot, so a frame underflowing through the base frame comes back
     * with a1 = _phy_stack_top, and a switch landing there saves the task
     * pointing at the top of a shared buffer it does not own.
     *
     * Measured for many steps as `bad sp: task 5 sp 0x3ffc0000`, and the value
     * is what identified it -- a1 after the switch would be _phy_stack_top - 64,
     * never the top exactly.
     *
     * a1 still holds the caller's sp here: step 59 moved the switch after the
     * mask, and this runs before both. So the slot can hold something true
     * rather than something merely unobservable. */
    addi    a10, a8, -12            /* s32i takes no negative offset */
    s32i    a1,  a10, 0


    /* Mask interrupts for the duration.
     *
     * Not for atomicity — to stop a context switch happening while windowed
     * frames are live. nat-os's level-3 handler saves a0..a15 and nothing else:
     * it does not save WINDOWBASE or WINDOWSTART and does not spill the window.
     * A switch in the middle of blob code therefore leaves another task running
     * with this one's frames still marked live in the register file.
     *
     * CONSEQUENCE, and it is a prerequisite for the WiFi driver rather than a
     * detail. Masking here stops the tick, so no task switches happen, so
     * watchdog_liveness() is never fed -- any blob call over 3 s is reset as
     * if the kernel had hung. More seriously, blob code that BLOCKS can never
     * make progress: esp_wifi_init_internal asks the OS adapter table for
     * semaphores and tasks, and _semphr_take() cannot return while the
     * scheduler is frozen. register_chipv7_phy is fine here only because it
     * asks the OS for nothing.
     *
     * The fix is to make the context switch window-aware rather than to widen
     * this mask: spill the window and save WINDOWBASE/WINDOWSTART, but only
     * when WINDOWSTART shows more than one live frame -- which in a call0
     * kernel means only during a blob excursion. See next_moves/08. */
    /* CONDITIONAL now, not unconditional.
     *
     * PS is saved either way, so the restore below is correct in both cases:
     * if the mask was skipped, the saved PS equals the current one and writing
     * it back is a no-op.
     *
     * g_phy_call_mask = 1 keeps the original behaviour, which register_chipv7_phy
     * still wants -- it is a self-contained calibration routine that asks the
     * OS for nothing, and freezing the scheduler around it costs nothing.
     *
     * g_phy_call_mask = 0 lets the scheduler keep running, which is required
     * for anything that BLOCKS: a task created inside a masked call can never
     * run, and a _semphr_take waiting on it can never return. Safety then rests
     * on a mutex instead -- see blob_call() -- because the invariant that
     * actually matters is ONE CONTEXT INSIDE WINDOWED CODE AT A TIME, not
     * "no preemption". wintorture measured that distinction: 6/6 correct with
     * live windowed frames across real context switches.
     *
     * The private stack is the second reason for the mutex. _phy_stack is a
     * single shared buffer, so two contexts here would corrupt each other
     * regardless of window state. */
    rsr.ps  a9
    s32i    a9,  a8, 24                 /* a8, not a1 -- see the switch below */
    movi    a10, g_phy_call_mask        /* a10, not a8 -- a8 is still the frame */
    l32i    a10, a10, 0
    beqz    a10, .Lphy_no_mask
    rsil    a9, 3
.Lphy_no_mask:

    /* SWITCH LAST, after the mask is in force.
     *
     * This used to run before the masking, and the gap was a real race: for the
     * half-dozen instructions between them, a1 pointed at `_phy_stack` while the
     * timer could still fire. A tick landing there saves the task with a stack
     * pointer outside its own stack -- measured, `task 5 sp 0x3ffc0000 outside
     * 0x3ffb9e88..0x3ffba688`, and 0x3ffc0000 is exactly _phy_stack_top.
     *
     * A task parked on the shared PHY stack has no guard protecting its frames,
     * window bookkeeping keyed to a base unrelated to where those frames live,
     * and a second context free to overwrite them. The handler then faults
     * restoring through it, one switch after the damage (step 57).
     *
     * Latent since PHY init first worked. It needed a path that keeps running
     * long enough afterwards for the bad saved sp to be restored through, and
     * `wifiinit task` is the first one. */
    mov     a1,  a8                     /* switch */

    /* Declare exactly one live frame: this one.
     *
     * THIS is what the StoreProhibited was. A CALL8 that runs out of physical
     * registers raises a window overflow, and the handler spills the OLDEST
     * live frame to the stack pointer recorded in it. Those bits survive from
     * whatever windowed code ran before — a different task, a previous
     * excursion — and by then their stack pointers are stale or garbage. The
     * spill writes there and faults.
     *
     * The measurement that found it: the fault was 272 bytes into a 6144-byte
     * stack. Depth was never the cause, which is what the private stack had
     * been introduced to fix.
     *
     * WINDOWSTART is deliberately NOT restored afterwards. The bits describe
     * frames whose registers this excursion has already overwritten, so
     * putting them back would re-mark frames whose contents are gone. A call0
     * kernel's correct steady state is one live frame, and that is what this
     * leaves behind. */
    rsr.windowbase  a9
    movi    a11, 1
    ssl     a9
    sll     a11, a11
    wsr.windowstart a11
    rsync
    /* [step 174] who wrote WINDOWSTART. Counter + last value, per site. */
    movi    a9, g_wsw
    l32i    a10, a9, 16
    addi    a10, a10, 1
    s32i    a10, a9, 16
    s32i    a11, a9, 20


    /* [step 138] Tell the bookkeeping what the wipe just did.
     *
     * The comment above is right that these bits must not be put back -- the
     * registers behind them are gone. But g_win_mask[] still records those
     * frames as owned, so the next restore ORs them straight back in via
     * g_win_union, re-marking frames whose contents this excursion destroyed.
     * That is the phantom of step 124: a bit set for a window holding nothing.
     *
     * A flag rather than the array itself, because g_win_mask is static to
     * task.c and the clearing wants to happen where the scheduler can see it. */
    movi    a9, g_win_disowned
    movi    a11, 1
    s32i    a11, a9, 0

    mov     a10, a3
    mov     a11, a4
    mov     a12, a5
    /* Fourth argument. a6 is untouched by everything above -- the prologue
     * only uses a8..a11 -- and a13 is already saved and restored with the
     * other callee-saved registers, so this costs one instruction and nothing
     * else. Callees taking fewer arguments simply ignore it.
     * esp_wifi_80211_tx(ifx, buffer, len, seq) is what needs it. */
    mov     a13, a6
    callx8  a2
    mov     a2,  a10

    /* Back to one live frame, then unmask. */
    rsr.windowbase  a9
    movi    a11, 1
    ssl     a9
    sll     a11, a11
    wsr.windowstart a11
    rsync
    /* [step 174] who wrote WINDOWSTART. Counter + last value, per site. */
    movi    a9, g_wsw
    l32i    a10, a9, 24
    addi    a10, a10, 1
    s32i    a10, a9, 24
    s32i    a11, a9, 28


    /* [step 138] Tell the bookkeeping what the wipe just did.
     *
     * The comment above is right that these bits must not be put back -- the
     * registers behind them are gone. But g_win_mask[] still records those
     * frames as owned, so the next restore ORs them straight back in via
     * g_win_union, re-marking frames whose contents this excursion destroyed.
     * That is the phantom of step 124: a bit set for a window holding nothing.
     *
     * A flag rather than the array itself, because g_win_mask is static to
     * task.c and the clearing wants to happen where the scheduler can see it. */
    movi    a9, g_win_disowned
    movi    a11, 1
    s32i    a11, a9, 0

    l32i    a9,  a1, 24
    wsr.ps  a9
    rsync

    l32i    a0,  a1, 4
    l32i    a12, a1, 8
    l32i    a13, a1, 12
    l32i    a14, a1, 16
    l32i    a15, a1, 20
    l32i    a1,  a1, 0              /* switch back, last */
    ret
    .size phy_stack_call, . - phy_stack_call

    .section .bss
    .align 16
    .global _phy_stack
    .global _phy_stack_top
_phy_stack:
    .space 6144
_phy_stack_top:

/* Whether phy_stack_call masks interrupts. Default 1: the historical
 * behaviour, correct for calls that never ask the OS for anything. */
    .section .data
    .align 4
    .global g_phy_call_mask
g_phy_call_mask:
    .word 1

/* [step 79] {a0_saved, slot_addr, a0_reloaded, reload_addr} for w2c_call2. */
    .align 4
    .global g_a0trace
g_a0trace:
    .word 0, 0, 0, 0

/* [step 80] 8 x {seq, a0, a1, ws} recorded at w2c_call2's retw. */
    .align 4
    .global g_xseq
g_xseq:
    .word 0
    .global g_xring
g_xring:
    .space 128

/* [step 97] {latched, a0-from-a12, callee} -- did a12 survive a call0 callee? */
    .align 4
    .global g_a12bad
g_a12bad:
    .word 0, 0, 0

/* [step 83] {frame, stamped, latched, was, now, frame_at_fault} */
    .align 4
    .global g_slotwatch
g_slotwatch:
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0

/* [X20 experiment] probe state, zero-init. Read back by the "x20" shell
 * command; layout mirrors what each store site above documents. */
    .section .bss
    .align 4
    .global g_x20_outcome
g_x20_outcome:
    .space 4                        /* 1 = entered wrapper, 2 = landed */
    .global g_x20_pre
g_x20_pre:
    .space 12                       /* ps, ws, wb at probe entry */
    .global g_x20_post_ws
g_x20_post_ws:
    .space 4                        /* wiped word */
    .global g_x20_recovered
g_x20_recovered:
    .space 16                       /* a2, a3, excsave4, excsave5 at landing */

/* [step 174] Four sites write WINDOWSTART outside ENTRY/RETW. Each records a
 * count and the value it last wrote, so the panic dump can say who touched it
 * most recently rather than leaving it to inference.
 *   0: _handler_level3 restore   1: x20_windowed wipe
 *   2: phy_stack_call pre        3: phy_stack_call post */
    .section .bss, "aw", @nobits
    .align 4
    .global g_wsw
g_wsw:
    .space 32
