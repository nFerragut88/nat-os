/* cyd-os — Milestone 0 link map.
 *
 * Everything lives in internal RAM. The 2nd-stage bootloader reads the image
 * header, copies each segment to the address named here, and jumps to ENTRY.
 * Nothing executes from flash yet, so no cache/MMU configuration is needed —
 * that gets added later when the kernel outgrows 128 KB of IRAM.
 *
 * Two ESP32 rules constrain this layout:
 *   - IRAM is fetch-only for instructions and requires aligned 32-bit access,
 *     so read-only data must NOT live there; .rodata goes to DRAM.
 *   - DRAM above ~0x3FFE0000 is used by the ROM for its own stack and data.
 *     Staying below that avoids being overwritten during boot.
 */

MEMORY
{
  /* Internal SRAM0 — instruction RAM. */
  iram (rwx) : ORIGIN = 0x40080000, LENGTH = 0x20000   /* 128 KB */

  /* Internal SRAM2 — data RAM, below the ROM's reserved region. */
  dram (rw)  : ORIGIN = 0x3FFB0000, LENGTH = 0x2C200   /* ~176 KB */
}

ENTRY(_start)

SECTIONS
{
  /* Exception and interrupt vectors.
   *
   * The CPU dispatches to fixed offsets from VECBASE, so each slot is placed
   * by absolute position rather than by order. VECBASE itself must be
   * 1024-byte aligned, which is why this section leads the image.
   *
   * Offsets are architectural (Xtensa LX6):
   *   0x180 level 2      0x1C0 level 3      0x200 level 4
   *   0x240 level 5      0x280 debug        0x2C0 NMI
   *   0x300 kernel exc   0x340 user exc     0x3C0 double exc
   *
   * Only the slots the kernel actually uses are populated; the rest are left
   * as gap. A stray dispatch into a gap is a bug, and the resulting fault is
   * caught by the panic vectors.
   */
  .vectors : ALIGN(1024)
  {
    _vecbase = .;

    . = _vecbase + 0x1C0;
    KEEP(*(.vectors.level3))

    . = _vecbase + 0x300;
    KEEP(*(.vectors.kernel))

    . = _vecbase + 0x340;
    KEEP(*(.vectors.user))

    . = _vecbase + 0x3C0;
    KEEP(*(.vectors.double))

    . = _vecbase + 0x400;
  } > iram

  .text : ALIGN(4)
  {
    /* Entry stub first so the image entry point is predictable. */
    KEEP(*(.text.start))
    *(.literal .literal.*)
    *(.text .text.*)
    . = ALIGN(4);
  } > iram

  .rodata : ALIGN(4)
  {
    *(.rodata .rodata.*)
    . = ALIGN(4);
  } > dram

  .data : ALIGN(4)
  {
    *(.data .data.*)
    . = ALIGN(4);
  } > dram

  .bss (NOLOAD) : ALIGN(4)
  {
    _bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    _bss_end = .;
  } > dram

  /* Kernel stack grows down from the top of DRAM. */
  _stack_top = ORIGIN(dram) + LENGTH(dram);

  /* Keep the toolchain from complaining about unplaced debug sections. */
  /DISCARD/ : { *(.eh_frame) *(.comment) }
}
