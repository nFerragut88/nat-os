/* nat-os — exception and interrupt vectors.
 *
 * The CPU dispatches to fixed offsets from VECBASE. Each slot is only 64 bytes,
 * so every vector immediately jumps to a real handler in .text. A plain `j` is
 * used rather than loading an address: its ±128 KB range covers the whole
 * kernel, and it avoids needing a literal pool inside a fixed-offset slot.
 *
 * Hardware does NOT save registers on entry. It records the interrupted PC in
 * EPCn and the processor state in EPSn, raises PS.INTLEVEL, and jumps. Every
 * register the handler touches must therefore be saved by the handler.
 *
 * Because the kernel is built -mabi=call0 there are no register windows, so the
 * window overflow/underflow vectors are never taken by ITS OWN code.
 *
 * They were described here as "left as traps", which was the intent and not
 * what the linker script did: nothing was placed before _vecbase + 0x1C0, so
 * slots 0x000..0x180 were zero-filled and a window exception would have
 * executed zeros. PS.WOE is set by the ROM, so the mechanism was armed with
 * nothing behind it.
 *
 * They are now real handlers — see kernel/window.S — which is what lets
 * WINDOWED code run here at all.
 */

/* Slot offsets are imposed by the linker script, not by this file — see the
 * .vectors output section in linker.ld. */

/* ------------------------------------------------------------------ */
/* Vector slots — placed at fixed offsets by the linker script.        */
/* ------------------------------------------------------------------ */

    .section .vectors.level3, "ax"
    .align 4
    .global _vector_level3
_vector_level3:
    j       _handler_level3

    .section .vectors.kernel, "ax"
    .align 4
    .global _vector_kernel
_vector_kernel:
    j       _handler_panic

    .section .vectors.user, "ax"
    .align 4
    .global _vector_user
_vector_user:
    j       _handler_panic

    .section .vectors.double, "ax"
    .align 4
    .global _vector_double
_vector_double:
    j       _handler_double

/* ------------------------------------------------------------------ */
/* Level-3 interrupt handler.                                          */
/* ------------------------------------------------------------------ */
/*
 * Runs on the interrupted context's stack, saves the FULL context there, then
 * asks the scheduler which stack to resume on. Because the stack pointer may
 * come back different, this is also the context switch.
 *
 * Everything is saved, not just the caller-saved set:
 *   a0            return address
 *   a2..a11       caller-saved under call0
 *   a12..a15      callee-saved — the resumed task expects ITS values, not the
 *                 interrupted task's, so they must travel with the frame
 *   SAR           clobbered by any shift
 *   EPC3, EPS3    THE CRITICAL PART — see below
 *
 * EPC3 and EPS3 hold the interrupted PC and processor state, and RFI 3 restores
 * from them. They are single registers, not per-task. Swapping stacks alone
 * would therefore resume the new task's registers but the OLD task's program
 * counter. Saving them into each task's frame and restoring before RFI is what
 * makes a switch actually switch.
 *
 *   LBEG, LEND,   zero-overhead LOOP state — see the note at the save site.
 *   LCOUNT        Omitting these is what broke M2.
 *
 * Frame is 96 bytes: 21 words, padded to keep the stack 16-byte aligned.
 * Layout must match TASK_FRAME_IDX_* in task.h.
 */

    .section .text, "ax"
    .align 4
    .literal_position
    .global _handler_level3
    .type _handler_level3, @function

_handler_level3:
    /* [step 123] The interrupted a1, before the prologue moves it.
     *
     * Three steps have assumed `current_sp + TASK_FRAME_BYTES` is the task's a1
     * at interrupt. It is arithmetically forced, but the thing that matters is
     * whether a WINDOWED FRAME starts there, and that has never been read from
     * the machine. wsr writes a special register and clobbers no AR, so this is
     * free and can be the first instruction. excsave2 is otherwise unused. */
    wsr.excsave2 a1

    /* [step 145] -160, not -112: 112 of frame plus 48 of reserve. The 48 bytes
     * directly below the interrupted sp are a CALL12 frame's extended save area
     * (a4..a11 at -48..-20) and writing the frame through them handed
     * WINDOWSTART back as a10. TASK_FRAME_RESERVE in task.h has the arithmetic. */
    addi     a1, a1, -160
    s32i     a0,  a1, 0
    s32i     a2,  a1, 4
    s32i     a3,  a1, 8
    s32i     a4,  a1, 12
    s32i     a5,  a1, 16
    s32i     a6,  a1, 20
    s32i     a7,  a1, 24
    s32i     a8,  a1, 28
    s32i     a9,  a1, 32
    s32i     a10, a1, 36
    s32i     a11, a1, 40
    s32i     a12, a1, 44
    s32i     a13, a1, 48
    s32i     a14, a1, 52
    s32i     a15, a1, 56
    rsr.sar  a2
    s32i     a2,  a1, 60
    rsr.epc3 a2
    s32i     a2,  a1, 64
    rsr.eps3 a2
    s32i     a2,  a1, 68

    /* Zero-overhead LOOP state. GCC emits LOOP for ordinary counted C loops,
     * so a task can be suspended with LCOUNT non-zero; leaving these out lets a
     * stale LCOUNT branch the handler — or the next task — back to a previous
     * task's LEND. Saved BEFORE any C is called, because task_schedule's own
     * round robin compiles to a LOOP and would otherwise clobber them. */
    rsr.lbeg   a2
    s32i       a2,  a1, 72
    rsr.lend   a2
    s32i       a2,  a1, 76
    rsr.lcount a2
    s32i       a2,  a1, 80

    /* [step 123] park the captured a1 where C can read it */
    rsr.excsave2 a2
    movi     a3, g_ih_a1_raw
    s32i     a2, a3, 0

    /* Per-task window state. See TASK_FRAME_IDX_WBASE in task.h. */
    rsr.windowbase  a2
    s32i       a2,  a1, 84
    rsr.windowstart a2
    s32i       a2,  a1, 88

    /* ==== [step 131] TIER B: save the register file into THIS task's slot ===
     *
     * Placement is forced by step 129: before any call0, while a0..a15 still
     * hold what the tick interrupted. Indexed by g_current, which at this point
     * is still the OUTGOING task -- task_schedule has not run.
     *
     * Step 130 failed because one shared buffer cannot bracket a switch: the
     * restore at .Lresume ran after the scheduler had chosen a different task,
     * so it wrote task A's windows into task B's file. Per-task slots are what
     * make the pair an identity again, and the design needs them regardless.
     *
     * Each block re-derives the slot rather than holding it across ROTW, because
     * a register carrying it would be one of the registers being saved. */
    wsr.excsave2 a0
    movi     a0, g_current
    l32i     a0, a0, 0
    bgez     a0, .Ltb_save
    rsr.excsave2 a0                 /* no outgoing task yet -- first switch */
    j        .Ltb_saved
.Ltb_save:
    rsr.excsave2 a0

    wsr.excsave2 a0
    wsr.excsave3 a1
    movi    a0, g_current
    l32i    a0, a0, 0
    slli    a0, a0, 8
    movi    a1, g_regsave
    add     a0, a0, a1
    s32i    a2,  a0, 8
    s32i    a3,  a0, 12
    s32i    a4,  a0, 16
    s32i    a5,  a0, 20
    s32i    a6,  a0, 24
    s32i    a7,  a0, 28
    s32i    a8,  a0, 32
    s32i    a9,  a0, 36
    s32i    a10, a0, 40
    s32i    a11, a0, 44
    s32i    a12, a0, 48
    s32i    a13, a0, 52
    s32i    a14, a0, 56
    s32i    a15, a0, 60
    rsr.excsave2 a1
    s32i    a1,  a0, 0
    rsr.excsave3 a1
    s32i    a1,  a0, 4
    mov     a1,  a0
    l32i    a0,  a1, 0
    l32i    a1,  a1, 4
    rotw    4

    wsr.excsave2 a0
    wsr.excsave3 a1
    movi    a0, g_current
    l32i    a0, a0, 0
    slli    a0, a0, 8
    movi    a1, g_regsave
    add     a0, a0, a1
    s32i    a2,  a0, 72
    s32i    a3,  a0, 76
    s32i    a4,  a0, 80
    s32i    a5,  a0, 84
    s32i    a6,  a0, 88
    s32i    a7,  a0, 92
    s32i    a8,  a0, 96
    s32i    a9,  a0, 100
    s32i    a10, a0, 104
    s32i    a11, a0, 108
    s32i    a12, a0, 112
    s32i    a13, a0, 116
    s32i    a14, a0, 120
    s32i    a15, a0, 124
    rsr.excsave2 a1
    s32i    a1,  a0, 64
    rsr.excsave3 a1
    s32i    a1,  a0, 68
    mov     a1,  a0
    l32i    a0,  a1, 64
    l32i    a1,  a1, 68
    rotw    4

    wsr.excsave2 a0
    wsr.excsave3 a1
    movi    a0, g_current
    l32i    a0, a0, 0
    slli    a0, a0, 8
    movi    a1, g_regsave
    add     a0, a0, a1
    s32i    a2,  a0, 136
    s32i    a3,  a0, 140
    s32i    a4,  a0, 144
    s32i    a5,  a0, 148
    s32i    a6,  a0, 152
    s32i    a7,  a0, 156
    s32i    a8,  a0, 160
    s32i    a9,  a0, 164
    s32i    a10, a0, 168
    s32i    a11, a0, 172
    s32i    a12, a0, 176
    s32i    a13, a0, 180
    s32i    a14, a0, 184
    s32i    a15, a0, 188
    rsr.excsave2 a1
    s32i    a1,  a0, 128
    rsr.excsave3 a1
    s32i    a1,  a0, 132
    mov     a1,  a0
    l32i    a0,  a1, 128
    l32i    a1,  a1, 132
    rotw    4

    wsr.excsave2 a0
    wsr.excsave3 a1
    movi    a0, g_current
    l32i    a0, a0, 0
    slli    a0, a0, 8
    movi    a1, g_regsave
    add     a0, a0, a1
    s32i    a2,  a0, 200
    s32i    a3,  a0, 204
    s32i    a4,  a0, 208
    s32i    a5,  a0, 212
    s32i    a6,  a0, 216
    s32i    a7,  a0, 220
    s32i    a8,  a0, 224
    s32i    a9,  a0, 228
    s32i    a10, a0, 232
    s32i    a11, a0, 236
    s32i    a12, a0, 240
    s32i    a13, a0, 244
    s32i    a14, a0, 248
    s32i    a15, a0, 252
    rsr.excsave2 a1
    s32i    a1,  a0, 192
    rsr.excsave3 a1
    s32i    a1,  a0, 196
    mov     a1,  a0
    l32i    a0,  a1, 192
    l32i    a1,  a1, 196
    rotw    4

    wsr.excsave2 a0
    wsr.excsave3 a1
    movi     a0, g_current
    l32i     a0, a0, 0
    movi     a1, g_regvalid
    addx4    a1, a0, a1
    movi     a0, 1
    s32i     a0, a1, 0
    rsr.excsave2 a0
    rsr.excsave3 a1

.Ltb_saved:

    /* [X6 experiment] switch-OUT recorder: the raw window state as the task
     * was interrupted, before any bookkeeping narrows it. Paired with the
     * switch-IN recorder at the restore: if a task leaves two bits live and
     * its saved word records one, the drop happened here; if it leaves one
     * and the next task is granted several, the leak is in the restore.
     *
     * [X7 experiment] Extended into a RING SAMPLER: every level-3 interrupt
     * appends {seq, WB, WS} to a 64-entry ring plus the running task's id,
     * so the dump shows the first sample at which foreign bits exist -- with
     * the task that was current at that instant -- rather than only the final
     * state. Stride-12 addressing: addx8 then addx4 gives base + idx*12
     * without a multiply. */
    rsr.windowbase  a3
    rsr.windowstart a4
    movi     a5, g_rout_seq
    l32i     a6, a5, 0
    addi     a6, a6, 1
    s32i     a6, a5, 0
    movi     a5, g_rout_wb
    s32i     a3, a5, 0
    movi     a5, g_rout_ws
    s32i     a4, a5, 0
    extui    a6, a6, 0, 6             /* idx = seq & 63 */
    movi     a5, g_ring
    addx8    a5, a6, a5
    addx4    a5, a6, a5               /* g_ring + idx*12 */
    s32i     a6, a5, 0
    s32i     a3, a5, 4
    s32i     a4, a5, 8
    movi     a5, g_dbg_current
    l32i     a4, a5, 0
    movi     a5, g_ring_task
    addx4    a5, a6, a5               /* g_ring_task + idx*4 */
    s32i     a4, a5, 0

    /* Clear PS.EXCM before calling any C.
     *
     * Hardware sets EXCM on interrupt entry, and while it is set the Xtensa
     * zero-overhead loop is DISABLED: the LEND comparison never fires, so a
     * hardware loop body executes exactly once and falls through to whatever
     * instruction sits at LEND. GCC emits these loops for ordinary counted C
     * loops, which means every C function reachable from this handler is
     * affected, not just the one that exposed it.
     *
     * That is what broke M2. The scheduler's round robin ran a single
     * iteration, and the "no candidate matched" assignment occupying the LEND
     * slot then made every task switch to itself.
     *
     * Safe to clear here: PS.INTLEVEL is still 3, so interrupts stay masked,
     * and EPC3/EPS3 are already saved, so the eventual RFI is unaffected. It
     * also means a fault inside the handler reaches the kernel exception
     * vector and the panic handler, instead of the double-exception vector. */
    rsr.ps   a2
    movi     a3, ~0x10
    and      a2, a2, a3
    wsr.ps   a2
    rsync

    /* ---- [H1 experiment] never switch out of a mid-window-handler context --
     *
     * docs/debug/2026-08-21-wifiinit-storeprohibited.md, hypothesis H1: a tick
     * landing while the interrupted context is INSIDE _WindowOverflow12
     * (its EPS3 has EXCM set) schedules another task out from under a
     * half-finished spill. The frames mid-spill belong to no task yet, so they
     * are absent from g_win_union, and the next rotation spills through
     * whatever stale pointers they hold — measured as OF12 executing with a13
     * near garbage and a StoreProhibited double exception.
     *
     * The change: when the interrupted PS shows EXCM, service the pending
     * sources anyway (NOT servicing the tick would leave CCOMPARE1 un-armed and
     * freeze the scheduler permanently) but skip task_schedule for this one
     * tick. The preempted handler resumes at once; the switch is deferred ten
     * milliseconds, by which time the spill has long completed.
     *
     * Instrumentation doubles as X1-lite: every hit increments
     * g_tick_excm_hits, and the FIRST hit records EPC3/EPS3. hits > 0 proves
     * the preemption happens; a surviving wifiinit with hits == 0 refutes H1
     * outright and points at H2/H3 instead.
     *
     * The decision branches BEFORE intr_dispatch, not after: the defer decision
     * cannot be carried across a call0 call in a caller-saved register, and the
     * first build carried it in a5 -- which intr_dispatch clobbered, so no tick
     * ever switched and the hang detector reset the board every 3 s. Two call
     * sites, no cross-call state.
     */
    l32i     a2, a1, 68                 /* saved EPS3 of the interrupted context */
    bbci     a2, 4, .Lsched             /* PS.EXCM is bit 4 */
    movi     a3, g_tick_excm_hits
    l32i     a4, a3, 0
    addi     a4, a4, 1
    s32i     a4, a3, 0
    movi     a3, g_tick_excm_pc
    l32i     a4, a3, 0
    bnez     a4, .Ldefer                /* keep only the FIRST hit */
    l32i     a4, a1, 64                 /* EPC3 as captured at entry */
    s32i     a4, a3, 0
    movi     a3, g_tick_excm_ps
    l32i     a4, a1, 68
    s32i     a4, a3, 0
.Ldefer:
    /* Service whichever level-3 sources are actually pending.
     *
     * This called timer_isr directly until the interrupt matrix existed, which
     * was correct while CCOMPARE1 was the only source that could reach level 3.
     * It stops being correct the moment a second one is routed: timer_isr
     * advances the tick deadline by a whole interval every time it runs, so a
     * GPIO interrupt arriving here would silently push the clock forward — the
     * same corruption as the yield bug in timer.c, from the other direction.
     *
     * intr_dispatch reads the INTERRUPT register and dispatches on fact. */
    call0    intr_dispatch
    j        .Lresume                   /* [H1] EXCM was set: keep this task */

.Lsched:
    call0    intr_dispatch

    /* Hand the current stack pointer to the scheduler; it returns the stack
     * pointer to resume on, which may belong to a different task. From this
     * instruction onward a1 may no longer be the stack we arrived on. */
    mov      a2, a1
    call0    task_schedule
    mov      a1, a2
.Lresume:

    /* Window state FIRST, before any general register is restored, so that the
     * sixteen registers land at the base this task was saved at rather than at
     * whichever base the previous context left behind.
     *
     * Writing WINDOWBASE changes which PHYSICAL register `a1` refers to, so the
     * frame pointer cannot simply stay in a1 across it. It is parked in a fixed
     * DRAM word instead -- addressed by a literal, so the address survives the
     * base change even though every register does not. EXCSAVE would also work;
     * a plain global is used because nothing else here depends on it. */
    l32i     a2, a1, 84                 /* saved WINDOWBASE */

    /* WINDOWSTART is OR-ed, not assigned, and that distinction is the whole of
     * step 32.
     *
     * The register is global: it describes the entire 64-register file, which
     * holds frames belonging to several tasks at once. A task saved while
     * another was mid-excursion carries that other task's bits in its frame --
     * measured, `disp` was saved claiming seven frames, none of them its own.
     * Writing that value back later resurrects frames whose physical registers
     * have since been reused, and the next rotation spills through whatever a1
     * those stale frames contain. That was step 30's garbage stack pointer.
     *
     * A task may only claim the ONE frame it is being restored into. Everything
     * else in the register file belongs to somebody still using it, so it is
     * left exactly as found. The saved WINDOWSTART word stays in the frame --
     * it is what the invariant counter reads -- but it is not authoritative on
     * the way back in. */
    /* [step 162] the task's own WINDOWSTART; the union is gone (steps 132/140) */
    l32i     a3, a1, 88
    ssl      a2
    /* [step 115] The grant is now this task's own base bit and nothing else.
     *
     * It used to be OR'd with g_win_union -- every other task's recorded live
     * frames -- because tasks left frames in the register file and the restore
     * had to avoid stepping on them. Since task_schedule() sweeps the outgoing
     * task down to one frame, no task has anything left in the file to protect,
     * and a union that is always zero is a union that can only add phantoms.
     *
     * g_win_union is still computed in task.c, as a check: it should now stay
     * zero, and if it does not, some path is parking frames without spilling.
     *
     * [step 115b] RESTORED pending evidence. Removing it broke wintorture in the
     * same build that added the sweep, and those are two changes with one
     * symptom. The union goes back so the sweep can be judged alone. */
    /* [step 162] union deleted -- the restore below rewrites these windows. */

    /* [X6 experiment] What every switch-in actually receives. Captured BEFORE
     * the windowbase write: afterwards the view has rotated and a2/a3 name
     * different physical slots -- the first build of this recorder stored
     * post-rotation garbage (a code address and a stack pointer) and measured
     * nothing. The static model grants (1<<saved_WB) | union, union zero on a
     * real switch; if the recorded word ever holds more, the leak is upstream. */
    movi     a5, g_rin_wb
    s32i     a2, a5, 0
    movi     a5, g_rin_ws
    s32i     a3, a5, 0
    movi     a5, g_rin_seq
    l32i     a4, a5, 0
    addi     a4, a4, 1
    s32i     a4, a5, 0
    movi     a4, g_switch_sp
    s32i     a1, a4, 0

    /* [ROOT CAUSE / FIX] The grant mask in a3 was computed under the
     * OUTGOING window view. WINDOWSTART must therefore be written BEFORE
     * wsr.windowbase rotates the view: afterwards the name a3 resolves to
     * a different physical register (slot newWB*4+3), and the write sources
     * whatever stale value lives in that slot instead.
     *
     * X7 proved this exactly. Restore-history entries 391-397 (all
     * base-1 -> base-1 grants): remapped a3 held 0x00000002, coincidentally
     * equal to the intended mask, so the write looked correct. Entry 398,
     * the first grant following task 5's base-13 excursion: remapped a3
     * held stack pointer 0x3ffbe9a8, and WINDOWSTART -- architecturally 16
     * bits wide -- latched 0xe9a8. That word is the ownerless phantom-frame
     * set every prior experiment kept measuring, and sweeping it is the
     * StoreProhibited.
     *
     * The transient state between the two writes (~2 instructions, both
     * special-register stores back-to-back) has interrupts masked at level
     * 3 and executes no window operation, so no allocation-chain check can
     * observe it. */
    /* [X7 experiment] operand capture retained, now taken pre-write under
     * the unrotated view: must always equal rin_ws. */
    movi     a4, g_rin_junksrc
    s32i     a3, a4, 0
    wsr.windowstart a3
    rsync

    /* [step 147] Leave no scratch behind in the outgoing view.
     *
     * a2..a6 are used above to compute and record the grant. `wsr.windowbase`
     * below rotates the register view, so the epilogue's `l32i a0..a15` reload
     * the INCOMING base's physical registers and never touch these. Whatever the
     * handler last put in them stays in AR[old_base*4 + n] -- and when the
     * outgoing and incoming window sets overlap, that is a register some live
     * frame owns.
     *
     * Step 146 measured the consequence: a3 held the task's own WINDOWSTART
     * (0x0000aa8a), the rotation stranded it, and vendor_torture read its own
     * register and dereferenced a window mask. It was invisible while the grant
     * was `1 << base`, because that residue is a small integer rather than a
     * plausible pointer.
     *
     * The X7 comment above covers the operand hazard -- a3 must be WRITTEN
     * before the rotation. This covers what the rotation leaves BEHIND, which
     * is the same instruction seen from the other side.
     *
     * Reloading from the frame restores the OUTGOING task's real a3..a6, which
     * is what those registers ought to hold. Note the ordering: this cannot be
     * done after `wsr.windowbase`, because by then a1 names a different register
     * and the frame is no longer addressable through it -- the reloads must
     * straddle the rotation in exactly this direction.
     *
     * a2 is unavoidable: it is the operand of the rotation itself, so it still
     * carries the saved WINDOWBASE afterwards. That is a value in 0..15 -- not
     * a plausible pointer, and not a mask -- so it is the least harmful residue
     * available. It is a known remainder, not an oversight. */
    l32i     a3, a1, 8                  /* outgoing a3 */
    l32i     a4, a1, 12                 /* outgoing a4 */
    l32i     a5, a1, 16                 /* outgoing a5 */
    l32i     a6, a1, 20                 /* outgoing a6 */

    wsr.windowbase  a2
    rsync
    /* [X7 experiment] readback: does the clean grant actually COMMIT?
     * Special-register reads are view-independent, so this is valid even
     * though wsr.windowbase has rotated the register view underneath us.
     * verify != written would mean the write itself is being lost or
     * overwritten inside the handler window. */
    rsr.windowstart a5
    movi     a4, g_rin_verify
    s32i     a5, a4, 0
    /* History ring: {seq-tag, junksrc, verify} for the last 16 restores,
     * so the dump shows whether the remapped-operand write ALSO produces
     * the correct value on healthy switches (junksrc coincidentally equal
     * to the intended mask) or diverges everywhere. */
    movi     a5, g_rin_seq
    l32i     a6, a5, 0
    extui    a6, a6, 0, 4             /* idx = seq & 15 */
    movi     a5, g_rjunk
    addx8    a5, a6, a5
    addx4    a5, a6, a5               /* g_rjunk + idx*12 */
    s32i     a6, a5, 0
    movi     a4, g_rin_junksrc
    l32i     a4, a4, 0
    s32i     a4, a5, 4
    movi     a4, g_rin_verify
    l32i     a4, a4, 0
    s32i     a4, a5, 8

    /* ==== [step 162] TIER B restore, with the overflow check disabled ======
     *
     * The ISA settles why every previous attempt failed (step 161). Window
     * overflow is checked on ANY REGISTER REFERENCE, not just ENTRY:
     *
     *     if (ref(r) or ref(s) or ref(t)) and WindowStart[WindowBase+1]
     *        and CWOE = 1  ->  overflow
     *
     * so an `s32i` after a `ROTW` faults whenever the next window's bit is set.
     * And the gate is  CWOE <- if PS.EXCM then 0 else PS.WOE.
     *
     * The prologue's save pass runs BEFORE this handler clears PS.EXCM, so it
     * has always had CWOE = 0 and the check disabled -- which is the entire
     * reason it works and five restore attempts did not. Here EXCM has been
     * cleared, so the check is live.
     *
     * Setting EXCM for the duration restores the prologue's condition. Nothing
     * inside needs exceptions: no ENTRY, no RETW, no call, and any fault in this
     * window would be a bug rather than a mechanism.
     *
     * Step 155 did the opposite by accident -- WINDOWSTART all-ones guarantees
     * WindowStart[WindowBase+1] at every stop, maximising the fault. */
    rsr.ps   a2
    movi     a3, 0x10                   /* PS.EXCM */
    or       a2, a2, a3
    wsr.ps   a2
    rsync

    movi     a0, g_current
    l32i     a0, a0, 0
    bltz     a0, .Ltb_restored
    movi     a1, g_regvalid
    addx4    a1, a0, a1
    l32i     a1, a1, 0
    beqz     a1, .Ltb_restored

    movi    a0, g_current
    l32i    a0, a0, 0
    slli    a0, a0, 8
    movi    a1, g_regsave
    add     a0, a0, a1
    l32i    a2,  a0, 8
    l32i    a3,  a0, 12
    l32i    a4,  a0, 16
    l32i    a5,  a0, 20
    l32i    a6,  a0, 24
    l32i    a7,  a0, 28
    l32i    a8,  a0, 32
    l32i    a9,  a0, 36
    l32i    a10, a0, 40
    l32i    a11, a0, 44
    l32i    a12, a0, 48
    l32i    a13, a0, 52
    l32i    a14, a0, 56
    l32i    a15, a0, 60
    l32i    a1,  a0, 0
    wsr.excsave2 a1
    l32i    a1,  a0, 4
    rsr.excsave2 a0
    rotw    4

    movi    a0, g_current
    l32i    a0, a0, 0
    slli    a0, a0, 8
    movi    a1, g_regsave
    add     a0, a0, a1
    l32i    a2,  a0, 72
    l32i    a3,  a0, 76
    l32i    a4,  a0, 80
    l32i    a5,  a0, 84
    l32i    a6,  a0, 88
    l32i    a7,  a0, 92
    l32i    a8,  a0, 96
    l32i    a9,  a0, 100
    l32i    a10, a0, 104
    l32i    a11, a0, 108
    l32i    a12, a0, 112
    l32i    a13, a0, 116
    l32i    a14, a0, 120
    l32i    a15, a0, 124
    l32i    a1,  a0, 64
    wsr.excsave2 a1
    l32i    a1,  a0, 68
    rsr.excsave2 a0
    rotw    4

    movi    a0, g_current
    l32i    a0, a0, 0
    slli    a0, a0, 8
    movi    a1, g_regsave
    add     a0, a0, a1
    l32i    a2,  a0, 136
    l32i    a3,  a0, 140
    l32i    a4,  a0, 144
    l32i    a5,  a0, 148
    l32i    a6,  a0, 152
    l32i    a7,  a0, 156
    l32i    a8,  a0, 160
    l32i    a9,  a0, 164
    l32i    a10, a0, 168
    l32i    a11, a0, 172
    l32i    a12, a0, 176
    l32i    a13, a0, 180
    l32i    a14, a0, 184
    l32i    a15, a0, 188
    l32i    a1,  a0, 128
    wsr.excsave2 a1
    l32i    a1,  a0, 132
    rsr.excsave2 a0
    rotw    4

    movi    a0, g_current
    l32i    a0, a0, 0
    slli    a0, a0, 8
    movi    a1, g_regsave
    add     a0, a0, a1
    l32i    a2,  a0, 200
    l32i    a3,  a0, 204
    l32i    a4,  a0, 208
    l32i    a5,  a0, 212
    l32i    a6,  a0, 216
    l32i    a7,  a0, 220
    l32i    a8,  a0, 224
    l32i    a9,  a0, 228
    l32i    a10, a0, 232
    l32i    a11, a0, 236
    l32i    a12, a0, 240
    l32i    a13, a0, 244
    l32i    a14, a0, 248
    l32i    a15, a0, 252
    l32i    a1,  a0, 192
    wsr.excsave2 a1
    l32i    a1,  a0, 196
    rsr.excsave2 a0
    rotw    4

.Ltb_restored:
    rsr.ps   a2
    movi     a3, 0xffffffef             /* clear PS.EXCM again */
    and      a2, a2, a3
    wsr.ps   a2
    rsync

    movi     a4, g_switch_sp            /* different physical reg now; reload */
    l32i     a1, a4, 0

    /* Restore EPS3/EPC3 first: after this, RFI will return to whatever task
     * this frame describes. */
    l32i     a2,  a1, 68
    wsr.eps3 a2
    l32i     a2,  a1, 64
    wsr.epc3 a2
    l32i     a2,  a1, 60
    wsr.sar  a2

    /* LOOP state. LCOUNT is written last: it is the register that actually arms
     * the hardware loop, so LBEG and LEND must already describe the right body
     * before it becomes live. */
    l32i       a2,  a1, 72
    wsr.lbeg   a2
    l32i       a2,  a1, 76
    wsr.lend   a2
    l32i       a2,  a1, 80
    wsr.lcount a2

    l32i     a0,  a1, 0
    l32i     a2,  a1, 4
    l32i     a3,  a1, 8
    l32i     a4,  a1, 12
    l32i     a5,  a1, 16
    l32i     a6,  a1, 20
    l32i     a7,  a1, 24
    l32i     a8,  a1, 28
    l32i     a9,  a1, 32
    l32i     a10, a1, 36
    l32i     a11, a1, 40
    l32i     a12, a1, 44
    l32i     a13, a1, 48
    l32i     a14, a1, 52
    l32i     a15, a1, 56
    addi     a1, a1, 160         /* [step 145] frame + TASK_FRAME_RESERVE */
    rfi      3

    .size _handler_level3, . - _handler_level3

/* ------------------------------------------------------------------ */
/* Fault handler — the only debugging channel until JTAG arrives.      */
/* ------------------------------------------------------------------ */
/*
 * Reached on any exception the kernel does not expect. Captures EXCCAUSE and
 * EPC1 into arguments and calls C, which prints them and halts. Without this a
 * fault produces a silent reset and no information at all; with it, the cause
 * code and faulting address are recoverable over UART.
 *
 * This handler deliberately does not attempt to return. Anything that lands
 * here is a kernel bug.
 */

    .align 4
    .global _handler_panic
    .type _handler_panic, @function

/* A DOUBLE exception is not the same event as a first-level one, and reporting
 * it through the same path is what made next_moves/08 steps 35-38 chase their
 * own tail.
 *
 * An exception taken while PS.EXCM is already set writes **DEPC**, not EPC1.
 * EPC1 keeps the FIRST exception's PC. A window overflow/underflow is itself an
 * exception -- it sets EPC1 to the instruction that triggered it and runs the
 * handler with EXCM set and WOE clear -- so a fault INSIDE that handler arrives
 * here with:
 *
 *   EPC1  = the retw/call that caused the spill      (looks like the culprit)
 *   PS    = the window handler's state, EXCM 1 WOE 0 (looks like corruption)
 *   DEPC  = the actual faulting instruction          (never read, until now)
 *
 * Four steps were spent explaining the first two as if they described task
 * state. They describe the window vector. */
    .align 4
    .global _handler_double
    .type _handler_double, @function
_handler_double:
    movi    a1, _panic_stack_top
    movi    a5, g_panic_double
    movi    a6, 1
    s32i    a6, a5, 0
    rsr.epc1     a6
    s32i    a6, a5, 4               /* the FIRST fault's pc, kept for context */
    rsr.exccause a2
    rsr.depc     a3                 /* the SECOND fault's pc -- the real one */
    rsr.ps       a4
    call0   kernel_panic
    .size _handler_double, . - _handler_double

    .align 4
_handler_panic:
    /* Use a known-good stack: the faulting one may be why we are here. */
    movi    a1, _panic_stack_top
    rsr.exccause a2
    rsr.epc1     a3
    rsr.ps       a4
    call0   kernel_panic
.Lpanic_hang:
    j       .Lpanic_hang

    .size _handler_panic, . - _handler_panic

    .section .bss
    .align 16
_panic_stack:
    .space 512
_panic_stack_top:

/* ---- [H1 experiment] evidence, printed by the panic dump and 'intr' ---- */
    .align 4
    .global g_tick_excm_hits
g_tick_excm_hits:
    .space 4
    .global g_tick_excm_pc
g_tick_excm_pc:
    .space 4
    .global g_tick_excm_ps
g_tick_excm_ps:
    .space 4

/* ---- [X6 experiment] switch-in recorder ---- */
    .align 4
    .global g_rin_seq
g_rin_seq:
    .space 4
    .global g_rin_wb
g_rin_wb:
    .space 4
    .global g_rin_ws
g_rin_ws:
    .space 4

/* ---- [X6 experiment] switch-out recorder ---- */
    .align 4
    .global g_rout_seq
g_rout_seq:
    .space 4
    .global g_rout_wb
g_rout_wb:
    .space 4
    .global g_rout_ws
g_rout_ws:
    .space 4

/* ---- [X7 experiment] window-state ring + restore readback ---- */
    .align 4
    .global g_ring
g_ring:
    .space 64 * 12
    .global g_ring_task
g_ring_task:
    .space 64 * 4
    .global g_rin_verify
g_rin_verify:
    .space 4
    .global g_rin_junksrc
g_rin_junksrc:
    .space 4
    .align 4
    .global g_rjunk
g_rjunk:
    .space 16 * 12

/* [step 131] Tier B: one register file per task slot. 12 * 64 * 4 = 3,072 B,
 * the figure costed in UM-NATOS-043 section 5.1. */
    .section .bss, "aw", @nobits
    .align 16
    .global g_regsave
g_regsave:
    .space 3072

/* [step 162] Tier B: one saved register file per task, and whether it has one. */
    .section .bss, "aw", @nobits
    .align 4
    .global g_regvalid
g_regvalid:
    .space 48
