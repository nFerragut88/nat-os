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
    addi     a1, a1, -112
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

    /* Per-task window state. See TASK_FRAME_IDX_WBASE in task.h. */
    rsr.windowbase  a2
    s32i       a2,  a1, 84
    rsr.windowstart a2
    s32i       a2,  a1, 88

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
    movi     a3, 1
    ssl      a2
    sll      a3, a3                     /* 1 << saved WINDOWBASE */
    /* Every OTHER task's live frames, by recorded ownership rather than by
     * whatever happens to be set. See g_win_mask in task.c: the hardware bit is
     * "a frame lives here" with no owner, so reading it back cannot tell a live
     * frame from an abandoned one. The union does. */
    movi     a6, g_win_union
    l32i     a6, a6, 0
    or       a3, a3, a6

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
    addi     a1, a1, 112
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
