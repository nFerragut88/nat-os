/* nat-os — entry stub.
 *
 * The first instruction of the OS. Runs with whatever state the bootloader
 * left behind, so it establishes its own stack before touching C.
 *
 * Built with -mabi=call0: no ENTRY/RETW, no register windows, no spill
 * exceptions. a1 is the stack pointer and a0 is the return address, exactly
 * as on a conventional architecture. That choice is what makes the context
 * switch later a plain register save rather than window-spill surgery.
 */

    .section .text.start, "ax"
    .align 4
    .global _start
    .type _start, @function

_start:
    /* Stack first — nothing below this line may call C. */
    movi    a1, _stack_top
    /* Xtensa wants the stack 16-byte aligned. */
    movi    a2, ~15
    and     a1, a1, a2

    /* Zero .bss. The bootloader only copies segments that have file content,
     * so uninitialised data arrives as whatever was in RAM at reset. */
    movi    a2, _bss_start
    movi    a3, _bss_end
    movi    a4, 0
.Lbss_loop:
    bgeu    a2, a3, .Lbss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .Lbss_loop
.Lbss_done:

    /* Zero .btdm_bss too.
     *
     * It is a separate NOLOAD section with its own marker pair, placed before
     * .data so the controller's data and bss are each contiguous and the
     * markers bracket all of it. That placement puts it OUTSIDE
     * _bss_start.._bss_end, so the loop above never touched it, and the
     * Bluetooth controller started life with every uninitialised variable
     * holding whatever was in DRAM at reset. It presented as:
     *
     *     exccause 28 (LoadProhibited)  epc 0x401128e5  excvaddr 0x12250025
     *
     * which is semphr_delete_wrapper +0x9 deleting a semaphore whose handle
     * was never assigned -- reset garbage read as a pointer. Not a Bluetooth
     * bug at all: a section that looked zeroed because every other bss is.
     *
     * Unconditional. In a build without -BT no archive contributes here, so
     * start == end and the loop falls straight through. */
    movi    a2, _btdm_bss_start
    movi    a3, _btdm_bss_end
.Lbtdm_loop:
    bgeu    a2, a3, .Lbtdm_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .Lbtdm_loop
.Lbtdm_done:

    /* Into C. kmain must not return. */
    call0   kmain

    /* If it does return anyway, park rather than execute whatever follows. */
.Lhang:
    j       .Lhang

    .size _start, . - _start
