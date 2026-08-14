/* cyd-os — exception and interrupt vectors.
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
 * window overflow/underflow vectors are never taken. They are left as traps
 * rather than omitted: if one is somehow reached it means an ABI assumption has
 * broken, and stopping loudly beats executing whatever bytes follow.
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
    j       _handler_panic

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
 * Frame is 80 bytes: 18 words, padded to keep the stack 16-byte aligned.
 * Layout must match TASK_FRAME_IDX_* in task.h.
 */

    .section .text, "ax"
    .align 4
    .literal_position
    .global _handler_level3
    .type _handler_level3, @function

_handler_level3:
    addi     a1, a1, -80
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

    /* Tick bookkeeping and comparator re-arm. */
    call0    timer_isr

    /* Hand the current stack pointer to the scheduler; it returns the stack
     * pointer to resume on, which may belong to a different task. From this
     * instruction onward a1 may no longer be the stack we arrived on. */
    mov      a2, a1
    call0    task_schedule
    mov      a1, a2

    /* Restore EPS3/EPC3 first: after this, RFI will return to whatever task
     * this frame describes. */
    l32i     a2,  a1, 68
    wsr.eps3 a2
    l32i     a2,  a1, 64
    wsr.epc3 a2
    l32i     a2,  a1, 60
    wsr.sar  a2
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
    addi     a1, a1, 80
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
