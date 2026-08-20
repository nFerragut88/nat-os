/* Pre-link layout for the vendor 802.11 blob — next_moves/08 step 3.
 *
 * The blob is linked ONCE at build time to fixed addresses so the kernel needs
 * no runtime linker: no relocation processing, no symbol resolution, no ELF
 * loader. Installing becomes read-file, write-flash, map, call.
 *
 * These addresses are NOT free parameters. They must agree with
 * kernel/flash.h (BLOB_IROM_ADDR, BLOB_DRAM_ADDR) and with the hole carved out
 * of kernel/linker.ld. Change one and the others must move with it; the
 * asserts at the bottom of this file and in flash.c exist to make a
 * disagreement a build failure rather than a corrupted board.
 */

MEMORY
{
  /* Mapped from flash 0x220000 by the flash MMU, 16 pages of 64 KB. */
  birom (rx) : ORIGIN = 0x40300000, LENGTH = 0x100000

  /* Carved out of the top of the kernel's dram. Costs heap, one-for-one. */
  bdram (rw) : ORIGIN = 0x3FFD4000, LENGTH = 0x8000
}

SECTIONS
{
  /* The entry table is FIRST so the kernel can find it at a known address --
   * BLOB_IROM_ADDR itself -- without parsing anything. It carries the magic,
   * the version, and the addresses the loader needs to finish the install. */
  .blob_entry : ALIGN(16)
  {
    _blob_start = .;
    KEEP(*(.blob_entry))
  } > birom

  .text : ALIGN(4)
  {
    *(.literal .literal.* .text .text.*)
  } > birom

  .rodata : ALIGN(4)
  {
    *(.rodata .rodata.* .sdata2 .sdata2.*)
    _blob_text_end = .;
  } > birom

  /* .data lives in DRAM at run time but its initial values ship inside the
   * flash image, so it is placed with AT> to give it a load address in birom
   * and a run address in bdram. The kernel copies it at install time -- the
   * MMU maps instructions, it does not populate writable memory. */
  .data : ALIGN(4)
  {
    _blob_data_vma = .;
    *(.data .data.* .sdata .sdata.*)
    . = ALIGN(4);
    _blob_data_end = .;
  } > bdram AT> birom
  _blob_data_lma = LOADADDR(.data);

  .bss (NOLOAD) : ALIGN(4)
  {
    _blob_bss_start = .;
    *(.bss .bss.* .sbss .sbss.* COMMON)
    . = ALIGN(4);
    _blob_bss_end = .;
  } > bdram

  /* The whole image that must be written to flash: everything with a load
   * address in birom, which is text + rodata + the .data initialisers. */
  _blob_image_end = _blob_data_lma + (_blob_data_end - _blob_data_vma);

  /* Sizes as absolute symbols. C cannot subtract two addresses in a static
   * initializer -- it is not a constant expression -- so blob_entry.c reads
   * these by taking their ADDRESS, which is the value computed here. */
  _blob_image_size = _blob_image_end - _blob_start;
  _blob_data_size  = _blob_data_end  - _blob_data_vma;

  /DISCARD/ : { *(.eh_frame) *(.comment) *(.xt.lit*) *(.xt.prop*) }
}

ASSERT(_blob_image_end - _blob_start <= LENGTH(birom),
       "blob image is larger than the 1 MB flash reservation (flash.h BLOB_FLASH_SIZE)")
ASSERT(_blob_bss_end - ORIGIN(bdram) <= LENGTH(bdram),
       "blob .data + .bss exceed the DRAM reservation (flash.h BLOB_DRAM_SIZE)")
ASSERT(ORIGIN(birom) == 0x40300000, "blob text base must match flash.h BLOB_IROM_ADDR")
ASSERT(ORIGIN(bdram) == 0x3FFD4000, "blob data base must match flash.h BLOB_DRAM_ADDR")
