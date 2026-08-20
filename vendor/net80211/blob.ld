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

  /* Read-only data, mapped through the DATA cache. Required, not cosmetic:
   * rodata in the instruction window serves 32-bit accesses only, and vendor
   * code reads bytes out of its own rodata. See kernel/flash.h. */
  bdrom (r)  : ORIGIN = 0x3F700000, LENGTH = 0x40000
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

  /* Every code section class the vendor archives use, not just .text.
   *
   * They ship .iram1, .phyiram and six .wifi*iram variants -- 357,940 bytes of
   * it -- because ESP-IDF places that code in real IRAM. Naming only .text
   * left all of it out of the image, and the region read back as ZEROS: the
   * first call into the blob died with IllegalInstruction at 0x4036e728,
   * which is inside that gap.
   *
   * --orphan-handling=error (see build_blob.ps1) is what stops this class of
   * mistake recurring: the linker now refuses to place a section nobody named
   * rather than quietly putting it somewhere plausible.
   *
   * CAVEAT, corrected. These are called *iram because ESP-IDF keeps them in
   * RAM so they stay executable while the flash cache is off -- ESP-IDF leaves
   * interrupts ENABLED during flash operations, which is why its WiFi ISRs are
   * IRAM_ATTR. That reasoning does NOT transfer: nat-os's
   * flash_erase_sector() holds crit_enter() across the whole erase, so nothing
   * at level <= CRIT_LEVEL runs and nothing fetches. Flash placement is
   * therefore safe against the fault, and the cost is 125 ms of DELAY instead.
   *
   * What remains real: CRIT_LEVEL is 3, so an interrupt allocated at level 4-7
   * is not masked and would fetch mid-erase. Whatever the OSI table's
   * intr_alloc chooses for the MAC decides this, so choose it deliberately. */
  .text : ALIGN(4)
  {
    *(.literal .literal.* .text .text.*)
    /* Globs, not exact names. These arrive per-function as .wifi0iram.37,
     * .rodata_wlog_warning.59 and so on, so a bare-name pattern matches
     * nothing that actually exists. */
    *(.iram1*)
    *(.phyiram*)
    *(.wifi0iram*)
    *(.wifiextrairam*)
    *(.wifiorslpiram*)
    *(.wifirxiram*)
    *(.wifislpiram*)
    *(.wifislprxiram*)
    _blob_text_end = .;
  } > birom

  /* LMA fixed at image offset 0x80000 (BLOB_RODATA_OFF), 64 KB aligned, so
   * the flash page and the DROM virtual page correspond with no arithmetic.
   * The ASSERT below is what catches code growing past it. */
  .rodata : AT(0x40300000 + 0x80000)
  {
    . = ALIGN(4);
    *(.rodata .rodata.* .sdata2 .sdata2.*)
    *(.rodata_wlog_*)
  } > bdrom
  _blob_rodata_lma  = LOADADDR(.rodata);
  _blob_rodata_vma  = ADDR(.rodata);
  _blob_rodata_size = SIZEOF(.rodata);

  /* .data lives in DRAM at run time but its initial values ship inside the
   * flash image, so it is placed with AT> to give it a load address in birom
   * and a run address in bdram. The kernel copies it at install time -- the
   * MMU maps instructions, it does not populate writable memory. */
    /* ALIGN the LMA. .rodata ends on a byte boundary -- it is full of
   * byte-granular wlog strings -- so an unaligned .data load address reaches
   * the loader, whose word-copy guard then refuses the whole image (rc=7).
   * That guard caught this; it is not decoration. */
  .data : AT(ALIGN(LOADADDR(.rodata) + SIZEOF(.rodata), 4))
  {
    . = ALIGN(4);
    _blob_data_vma = .;
    *(.data .data.* .sdata .sdata.*)
    *(.dram1*)
    . = ALIGN(4);
    _blob_data_end = .;
  } > bdram
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

  /* Named explicitly so --orphan-handling=error does not trip on them. */
  /DISCARD/ : {
    *(.eh_frame) *(.comment) *(.xt.lit*) *(.xt.prop*)
    *(.debug_*) *(.xtensa.info) *(.note*)
  }
}

ASSERT(_blob_image_end - _blob_start <= LENGTH(birom),
       "blob image is larger than the 1 MB flash reservation (flash.h BLOB_FLASH_SIZE)")
ASSERT(_blob_bss_end - ORIGIN(bdram) <= LENGTH(bdram),
       "blob .data + .bss exceed the DRAM reservation (flash.h BLOB_DRAM_SIZE)")
ASSERT(ORIGIN(birom) == 0x40300000, "blob text base must match flash.h BLOB_IROM_ADDR")
ASSERT(ORIGIN(bdram) == 0x3FFD4000, "blob data base must match flash.h BLOB_DRAM_ADDR")
ASSERT(ORIGIN(bdrom) == 0x3F700000, "blob rodata base must match flash.h BLOB_DROM_ADDR")
ASSERT(LOADADDR(.rodata) >= _blob_text_end,
       "code has grown past BLOB_RODATA_OFF and would overlap rodata in the image")
ASSERT(SIZEOF(.rodata) <= LENGTH(bdrom),
       "blob .rodata exceeds the DROM reservation (flash.h BLOB_DROM_SIZE)")
