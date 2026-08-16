/* nat-os — Milestone 0 link map.
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

  /* Flash mapped read-only through the data cache.
   *
   * Nothing is copied here: the 2nd-stage bootloader recognises load addresses
   * in 0x3F400000–0x3F800000 as flash-mapped and programs the flash MMU to
   * point at the image data in place, then enables the cache before jumping to
   * the entry point. Reads then go through the cache transparently.
   *
   * The 0x20 offset is not arbitrary. The MMU maps in 64 KB pages, and the
   * mapped page begins at the start of the image — which is occupied by the
   * 24-byte image header plus an 8-byte segment header. Starting the region
   * 32 bytes in makes the virtual address agree with the flash offset, which is
   * the congruence esptool and the bootloader both assume. */
  drom (r)   : ORIGIN = 0x3F400020, LENGTH = 0x400000 - 0x20
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

    /* Register-window vectors, 0x000..0x180.
     *
     * These were previously left EMPTY — the section jumped straight to 0x1C0,
     * so a window exception executed zero-filled space. Nothing in this kernel
     * takes them (it is -mabi=call0 throughout), but PS.WOE is set by the ROM,
     * so the mechanism was armed with no handlers behind it.
     *
     * Filling them is what makes windowed-ABI code callable at all — see
     * kernel/window.S. Purely additive: code that never raises a window
     * exception cannot observe the difference. */
    . = _vecbase + 0x000;
    KEEP(*(.vectors.window.of4))
    . = _vecbase + 0x040;
    KEEP(*(.vectors.window.uf4))
    . = _vecbase + 0x080;
    KEEP(*(.vectors.window.of8))
    . = _vecbase + 0x0C0;
    KEEP(*(.vectors.window.uf8))
    . = _vecbase + 0x100;
    KEEP(*(.vectors.window.of12))
    . = _vecbase + 0x140;
    KEEP(*(.vectors.window.uf12))

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

  /* Read-only data that must survive a disabled flash cache.
   *
   * UM-NATOS-011 §4 records the hazard: writing flash requires the cache to be
   * turned off, and while it is off EVERY access to flash-mapped .rodata
   * faults. That includes the panic handler's own message strings — a fault
   * during a flash write would reach a panic handler that then faults trying to
   * describe itself, which is the worst failure mode available to a kernel with
   * no debugger.
   *
   * Selecting by object file rather than by attributing individual strings:
   * string literals are awkward to place by hand, and a rule that catches an
   * object's rodata wholesale cannot be defeated by someone adding a new
   * message later and forgetting the annotation. */
  .dram.rodata : ALIGN(4)
  {
    *panic.o(.rodata .rodata.*)
    *uart.o(.rodata .rodata.*)
    *watchdog.o(.rodata .rodata.*)

    /* flash.o and store.o run with the flash chip busy: any flash-mapped read
     * here would hit a chip that cannot answer. */
    *flash.o(.rodata .rodata.*)
    *store.o(.rodata .rodata.*)
    . = ALIGN(4);
  } > dram

  /* Everything else lives in flash rather than DRAM.
   *
   * It sat in DRAM because IRAM cannot serve the unaligned byte reads that
   * string handling performs. Flash mapped through the data cache has no such
   * restriction, so the constraint that forced the original placement does not
   * apply here (UM-NATOS-010 §7.3).
   *
   * Literal pools are deliberately NOT moved. `-mtext-section-literals` keeps
   * them beside the code in IRAM because `l32r` addresses backwards within
   * 256 KB of the PC; a literal in flash would be out of range and the link
   * would fail — or worse, not fail. */
  .flash.rodata : ALIGN(16)
  {
    _rodata_start = .;
    *(.rodata .rodata.*)
    . = ALIGN(4);
    _rodata_end = .;
  } > drom

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

  /* Boot stack reservation.
   *
   * The boot context is abandoned once kmain hands off to the scheduler, and
   * every task thereafter runs on its own stack from .bss. The reservation is
   * therefore only needed until handoff — but it is kept permanently rather
   * than reclaimed, because the heap must not be adjacent to a stack that grew
   * further than expected before handoff. 4 KB is far more than the boot path
   * uses and costs 2% of DRAM. */
  _boot_stack_size = 4K;

  /* Heap: everything between the end of .bss and the boot stack reservation.
   * Placed by the linker rather than sized by hand so it absorbs changes in
   * .bss automatically — a fixed heap base silently shrinks when a static
   * array grows, which is the sort of thing that shows up as a mysterious
   * allocation failure much later. */
  _heap_start = ALIGN(_bss_end, 8);
  _heap_end   = _stack_top - _boot_stack_size;

  /* Keep the toolchain from complaining about unplaced debug sections. */
  /DISCARD/ : { *(.eh_frame) *(.comment) }
}
