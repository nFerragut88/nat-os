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
  /* Shortened from 0x2C200 to reserve the top 32 KB for the vendor blob's
   * .data and .bss (kernel/flash.h, BLOB_DRAM_ADDR). Unlike the irom carve-out
   * this one is NOT free: the heap is whatever is left between _bss_end and
   * the stack, so this costs ~33 KB of heap directly. That is the price of the
   * blob having writable state, and the MMU cannot help with it. */
  dram (rw)  : ORIGIN = 0x3FFB0000, LENGTH = 0x24000   /* 144 KB */

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

  /* Flash mapped EXECUTABLE through the instruction cache.
   *
   * The window is 0x400D0000..0x40400000 -- what ESP-IDF's headers call
   * IRAM0_CACHE, and what an ESP32 application means by "IROM". It is NOT
   * IROM0_CACHE (0x40800000), which is a different window with a different set
   * of MMU entries; that distinction cost a wrong first guess.
   *
   * The flash MMU indexes it at entry offset 64, where drom uses offset 0
   * (soc/hal/esp32/mmu_ll.h, mmu_ll_get_entry_id). boot/boot.c knows both.
   *
   * Why this exists: iram above is 128 KB and that is the ceiling on ALL code
   * this kernel will ever run, forever. .rodata has been mapped from flash for
   * milestones; instructions never were, for no reason beyond nobody needing
   * it. See docs/next_moves/07-irom.md. */
  /* Shortened from 0x330000 to reserve the top 1 MB of this window for the
   * pre-linked vendor 802.11 blob (kernel/flash.h, BLOB_IROM_ADDR). The kernel
   * uses ~21 KB of the 2.3 MB that remains, so this costs nothing real -- but
   * it has to be carved out HERE, or the kernel's own code is free to be placed
   * at addresses the blob will later be mapped over. */
  irom (rx)  : ORIGIN = 0x400D0000, LENGTH = 0x230000
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

  /* Code that runs from flash.
   *
   * Placed BEFORE .text deliberately: the linker assigns each input section
   * exactly once, in the order the output sections appear, so .text's wildcard
   * would claim these objects if it came first.
   *
   * ---- what may go here, and what may never ---------------------------------
   *
   * Executing from flash stalls on a cache miss, and reads nothing at all while
   * the cache is disabled. So anything reachable in either of those conditions
   * has to stay in iram:
   *
   *   vectors.S, panic.c    the fault path, which must work when nothing else does
   *   flash.c, store.c      called WITH THE CACHE OFF -- code in flash could not
   *                         read itself
   *   task.c                the scheduler switch
   *   window.S              window-overflow handlers, reached from any vendor call
   *   appcpu.c              core 1 runs with no cache enabled at all
   *
   * And one reason that is NOT "the cache is off", which is easy to miss
   * because every other entry above is:
   *
   *   anything that could be scheduled DURING A FLASH ERASE. While the chip
   *   is erasing it answers RDSR and little else -- it cannot serve a read,
   *   cache or no cache. flash.c currently makes this safe by holding a
   *   critical section across the whole 125 ms erase, so no other task runs
   *   at all. If that critical section is ever narrowed, this list grows to
   *   include every task that could run during one. See flash_erase_sector().
   *
   * Getting that wrong hangs inside a fault handler with no output, which is
   * the worst failure this kernel can produce. The acceptance test is therefore
   * not "it boots" but "`fault` still prints a panic report".
   *
   * ---- literals travel WITH the code ---------------------------------------
   *
   * `.literal` is selected alongside `.text` for each object. -mtext-section-
   * literals keeps a function's literal pool beside it, and `l32r` reaches
   * backwards only 256 KB from the PC -- so the pool must move when the code
   * does. The note on .flash.rodata below is about the opposite case and still
   * holds: literals must never be left behind in iram while their code moves.
   */
  .flash.text : ALIGN(4)
  {
    _flash_text_start = .;

    /* shell.c first, and alone to begin with: 26,754 bytes, the largest object
     * in the image, on no timing-critical path, and reachable only when a human
     * types. If this placement is wrong the board stops taking commands, which
     * is loud and harmless. */
    *shell.c.o(.literal .literal.* .text .text.*)

    /* kmain.c: 13,057 bytes of boot banner, self-tests and task entry points.
     *
     * Checked rather than assumed. Its 16 symbols are `kmain`, the task entry
     * functions, and helpers -- no ISR context, and `nm` confirms that nothing
     * which must stay in iram (panic, flash, store, task, vectors, window,
     * appcpu, timer, intr) references any of them.
     *
     * The subtle case is the idle task: if the cache were disabled during a
     * flash write and the scheduler then entered a flash-resident task, it
     * would fault. It cannot. flash.c does not disable the cache -- it drives
     * SPI1 directly and wraps every operation in crit_enter/crit_exit, so no
     * other code runs at all for the duration, and its own rodata is pinned to
     * DRAM by .dram.rodata below. */
    *kmain.c.o(.literal .literal.* .text .text.*)

    /* [step 278] wifiapp.c: the wifi view. Same argument shell.c makes for
     * itself -- reachable only when a human taps an icon, on no timing-critical
     * path, no ISR context, and nothing in the pinned set (panic, flash, store,
     * task, vectors, window, appcpu, timer, intr) references it. Its blocking
     * work is a 15 s PBKDF2 that runs with the cache on like everything else.
     *
     * If this placement is wrong the wifi icon faults, which is loud, local and
     * harmless -- the same property that made shell.c a safe first tenant. */
    *wifiapp.c.o(.literal .literal.* .text .text.*)
      *keyboard.c.o(.literal .literal.* .text .text.*)
      *browser.c.o(.literal .literal.* .text .text.*)
      /* [step 298] webfetch.c is irom and wificred.c is NOT, and the difference
       * is the rule step 292 cost a crash to state: code that touches the FLASH
       * BUS must not live on it. wificred.c drives flash_read/erase/write;
       * webfetch.c only calls lwIP. */
      *webfetch.c.o(.literal .literal.* .text .text.*)
      /* [step 333] Reporting only -- called from the net and init paths, never
       * from an ISR, and it does not go near the flash bus (the rule step 316
       * narrowed to "the flash DRIVER must not live on the bus"). Moved to make
       * room for a receive ring that can hold a standard frame. */
      *wpareport.c.o(.literal .literal.* .text .text.*)

    /* [step 278] notes.c and term.c join it, for the same reason and with the
     * same evidence: both are views entered from the launcher, both run only
     * while a human is looking at them, and neither is referenced by anything
     * in the pinned set. They were in iram only because iram was the default,
     * not because anyone decided they belonged there. */
    *notes.c.o(.literal .literal.* .text .text.*)
    *term.c.o(.literal .literal.* .text .text.*)

    . = ALIGN(4);
    _flash_text_end = .;
  } > irom

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

  /* The heap is defined by subtraction, so nothing upstream notices when .bss
   * grows past _heap_end -- the range simply inverts and every allocation
   * fails at run time, several self-tests deep into boot. Observed live on
   * 2026-08-22: a 2 KB overrun produced "LoadProhibited @ 0" in kmain's arena
   * scrub and a panic-during-panic halt, with no link-time warning at all.
   * This turns that class of layout mistake into a link error. Strict, so a
   * zero-byte heap cannot sneak through either: heap_init() treats end==start
   * as "no heap", and the self-test then dereferences the NULL arena base it
   * is handed. */
  ASSERT(_heap_start < _heap_end, ".bss grew into the heap/boot-stack reservation: shrink static DRAM (see APPCPU_CAP_MAX note in appcpu.h) or boot ends in LoadProhibited @ 0")

  /* Keep the toolchain from complaining about unplaced debug sections. */
  /DISCARD/ : { *(.eh_frame) *(.comment) }
}

/* Keeps the two halves of the reservation from drifting apart. If someone
 * lengthens irom, this fails at link time instead of at the moment the blob is
 * mapped over live kernel code. 0x40300000 is BLOB_IROM_ADDR in flash.h. */
ASSERT(ORIGIN(irom) + LENGTH(irom) == 0x40300000,
       "kernel irom must end exactly where the blob region begins (flash.h BLOB_IROM_ADDR)")

ASSERT(ORIGIN(dram) + LENGTH(dram) == 0x3FFD4000,
       "kernel dram must end exactly where the blob data region begins (flash.h BLOB_DRAM_ADDR)")
