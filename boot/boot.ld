/* nat-os — second-stage bootloader link map.
 *
 * ---- the constraint that decides every address here ----------------------
 *
 * This program copies nat-os into memory. If any part of it lived where nat-os
 * lands, it would overwrite itself mid-copy, and the failure would be a hang
 * with no message. So it must sit entirely OUTSIDE the kernel's regions:
 *
 *     nat-os IRAM   0x40080000 .. 0x400A0000
 *     nat-os DRAM   0x3FFB0000 .. 0x3FFDC200
 *
 * The two windows below are the ones Espressif's own bootloader uses for
 * exactly this reason -- a loader segment beneath the kernel and a data segment
 * above it -- read out of its bootloader.ld rather than chosen.
 *
 * ---- no DROM, deliberately ------------------------------------------------
 *
 * There is no flash-mapped region here and there must not be. This runs with
 * the cache DISABLED -- turning it on is part of its job -- so any rodata it
 * placed in flash would be unreadable at the moment it was needed. Everything
 * goes to DRAM instead, which is why .rodata is named explicitly below.
 */

ENTRY(boot_start)

MEMORY
{
  /* Below the kernel's IRAM. Espressif calls this the APP CPU cache region;
   * nothing uses it during a single-core boot. */
  iram (rwx) : ORIGIN = 0x40078000, LENGTH = 0x8000    /* 32 KB */

  /* Above the kernel's DRAM top of 0x3FFDC200, below the ROM's own data. */
  dram (rw)  : ORIGIN = 0x3FFF0000, LENGTH = 0x6000    /* 24 KB */
}

SECTIONS
{
  .text : ALIGN(4)
  {
    *(.literal .text .literal.* .text.*)
    . = ALIGN(4);
  } > iram

  .data : ALIGN(4)
  {
    *(.data .data.*)
    *(.rodata .rodata.*)      /* in DRAM, for the cache reason above */
    . = ALIGN(4);
  } > dram

  .bss (NOLOAD) : ALIGN(4)
  {
    _bss_start = ABSOLUTE(.);
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    _bss_end = ABSOLUTE(.);
  } > dram

  _stack_top = ORIGIN(dram) + LENGTH(dram);
}
