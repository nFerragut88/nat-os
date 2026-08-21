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

    /* Hand the current stack pointer to the scheduler; it returns the stack
     * pointer to resume on, which may belong to a different task. From this
     * instruction onward a1 may no longer be the stack we arrived on. */
    mov      a2, a1
    call0    task_schedule
    mov      a1, a2

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
    rsr.windowstart a6
    or       a3, a3, a6                 /* keep every other task's live frames */

    movi     a4, g_switch_sp
    s32i     a1, a4, 0
    wsr.windowbase  a2
    rsync
    wsr.windowstart a3
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
