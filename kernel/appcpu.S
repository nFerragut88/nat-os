/* nat-os — the APP CPU's entry point.
 *
 * nat-os is a single-core kernel and intends to stay one. This exists for one
 * job: UM-NATOS-034 §27 needs to sample the regi2c host while
 * register_chipv7_phy() runs, and that call is synchronous, so on one core
 * there is nobody left to watch it.
 *
 * Core 1 is therefore not a scheduling resource here. It is an instrument. It
 * runs this, touches its own memory and the peripheral bus, and nothing else.
 *
 * ---- what the hardware hands us -------------------------------------------
 *
 * The APP CPU comes out of reset via ROM code that reads DPORT_APPCPU_CTRL_D
 * and jumps to it. On arrival a1 is meaningless, interrupts are in an unknown
 * state, and nothing has been initialised. So: interrupts off first, then a
 * stack, then C.
 *
 * The stack address is fetched from a global rather than built from a linker
 * symbol, so that core 0 owns the decision and this file stays trivial.
 *
 * ---- and one thing it must not do -----------------------------------------
 *
 * No flash. The APP CPU has its own cache and nothing has enabled it, so any
 * flash-mapped address -- which on this kernel means every string literal,
 * since .rodata lives in DROM -- would fault or hang. Everything reachable from
 * here is in IRAM and DRAM, and appcpu.c keeps it that way deliberately.
 */
    .section .text.appcpu_entry, "ax"
    .global  appcpu_entry
    .type    appcpu_entry, @function
    .align   4

appcpu_entry:
    rsil    a2, 15                  /* interrupts off before anything else   */

    movi    a1, g_appcpu_sp         /* address of the slot...                */
    l32i    a1, a1, 0               /* ...and the stack top inside it        */
    movi    a0, 0                   /* no return address; this never returns */

    call0   appcpu_main

.appcpu_hang:                       /* appcpu_main does not return           */
    j       .appcpu_hang
    .size   appcpu_entry, . - appcpu_entry
