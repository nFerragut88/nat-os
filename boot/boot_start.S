/* Entry from the ROM's first-stage loader.
 *
 * The ROM has copied our segments and jumped here. Two things must happen
 * before C is safe: a stack of our own, and a zeroed .bss.
 *
 * Our own stack, because the ROM's lives in DRAM this code is about to be
 * careless around, and because depending on somebody else's stack pointer is
 * the kind of assumption that holds until it does not.
 *
 * .bss zeroed by hand because nothing else will. The ROM loads only the
 * segments the image header names, and .bss is NOLOAD -- it exists in the link
 * map and not in the file, so on entry it holds whatever the last boot left
 * there.
 */
    .section .text.boot_start, "ax"
    .global  boot_start
    .type    boot_start, @function
    .align   4

boot_start:
    movi    a1, _stack_top          /* our stack, not the ROM's */

    movi    a2, _bss_start
    movi    a3, _bss_end
    movi    a4, 0
.bss_loop:
    bgeu    a2, a3, .bss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .bss_loop
.bss_done:

    call0   boot_main

.hang:                              /* boot_main never returns */
    j       .hang
    .size   boot_start, . - boot_start
