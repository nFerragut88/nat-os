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

  /* [next_moves/11 step 4] SRAM1's upper 60 KB, the first RAM above the
   * old "ROM-reserved" line to be claimed.
   *
   * UM-NATOS-004 avoided all of 0x3FFE0000.. on a belief it flagged as untested.
   * next_moves/08 step 396 measured it with a write-and-verify probe: the ROM
   * touches 0x3FFE0000/1000/3000 and 0x3FFF0000 (flash writes, PHY bring-up),
   * deterministically, and 0x3FFF1000..0x40000000 survived all of it -- a
   * flash write, a PHY bring-up and a full WPA2 join, twice, byte for byte.
   * This region is exactly that surviving block and nothing more.
   *
   * Untested there, so untrusted here: BT, deep sleep, OTA, coredump. Only
   * data that can be rebuilt goes in it -- decoder state, never the record. */
  sram1 (rw) : ORIGIN = 0x3FFF1000, LENGTH = 0xF000    /* 60 KB */

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
      /* [step 390] The HTML reader and its self-test, with the view
       * they serve. Nothing here runs with interrupts masked or
       * touches the flash controller, which is the only thing that
       * keeps a file OUT of irom -- see the flash.c note below. */
      *html.c.o(.literal .literal.* .text .text.*)
      *htmltest.c.o(.literal .literal.* .text .text.*)
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
      /* [step 341] The touch calibration UI. It draws and it reads the panel;
       * it reaches flash only through store.c, which stays in iram -- and step
       * 316 established that callers are irrelevant, only the driver executing
       * while the bus is taken matters. Moved to make room for wifiprefs. */
      *calib.c.o(.literal .literal.* .text .text.*)

      /* [step 352] ALL OF lwIP, 40 KB of it.
       *
       * iram had 848 bytes free of 128 KB and had already refused kernel
       * instrumentation twice (328, 341). lwIP is the largest thing in it and
       * has no business being there: every one of its entry points runs on the
       * net task -- netif_wifi_input() from the drain loop, sys_check_timeouts()
       * from the tick, the tcp/udp callbacks from those two. None of it runs
       * from an interrupt handler, and none of it touches the flash bus, which
       * is the only rule that actually constrains placement (316).
       *
       * A flash erase masks interrupts, so no task switch can land inside one:
       * lwIP cannot be executing while the bus is taken. */
      *lwip_*.c.o(.literal .literal.* .text .text.*)

      /* [step 374] The WPA crypto, 12,648 bytes, for the same reason and by the
       * same rule.
       *
       * PBKDF2, SHA-1, AES and the key-wrap unwrap run on the NET TASK during
       * association -- four thousand rounds of it, which is why a join takes
       * fifteen seconds. None of it is reachable from an interrupt handler and
       * none of it touches the flash bus.
       *
       * It is compiled WINDOWED rather than call0 (see build.ps1), and that is
       * orthogonal to where it lives: the ABI decides how it is called, the
       * linker decides where it is fetched from.
       *
       * Listed one file at a time rather than as *wpa*.o, deliberately.
       * wpa_cb.o and wpa_hs.o are NOT here: the blob calls those, and what
       * context the blob calls them from is not something this project has
       * established. A wildcard would have swept them in on the strength of
       * their names. */
      *wpa_sha1-internal.c.o(.literal .literal.* .text .text.*)
      *wpa_sha1.c.o(.literal .literal.* .text .text.*)
      *wpa_sha1-pbkdf2.c.o(.literal .literal.* .text .text.*)
      *wpa_sha1-prf.c.o(.literal .literal.* .text .text.*)
      *wpa_aes-internal.c.o(.literal .literal.* .text .text.*)
      *wpa_aes-internal-dec.c.o(.literal .literal.* .text .text.*)
      *wpa_aes-unwrap.c.o(.literal .literal.* .text .text.*)
      *wpatest.c.o(.literal .literal.* .text .text.*)
      *wpatest.o(.literal .literal.* .text .text.*)

      /* [step 374] The netif glue and the TCP server, 3,225 bytes.
       *
       * The lwIP block above already names netif_wifi_input() as running on the
       * net task from the drain loop -- so the file it lives in was being held
       * in iram by nothing but not having been listed. tcpsrv is reached from
       * lwIP's accept and receive callbacks, which are the same task.
       *
       * This is the last of what can move under the rule. What remains in iram
       * is the scheduler, the vectors, the flash driver, the display DMA path,
       * panic, and the OSI shim the blob calls from a context this project has
       * not established -- and the last of those is where the next 8 KB is, for
       * anyone willing to establish it. */
      *netif_wifi.c.o(.literal .literal.* .text .text.*)
      *tcpsrv.c.o(.literal .literal.* .text .text.*)

      /* [step 374] The last 435 bytes, and the smallest thing that could
       * supply them: program-to-program messaging, reached only from `sys send`
       * and `sys recv` on an application task.
       *
       * The obvious candidate was wifimac.c, five kilobytes and called from
       * nothing but shell commands. It also contains wifimac_isr(), routed
       * through intr_route() -- an interrupt handler executing from flash is
       * the one placement this kernel actually cannot survive. Checked rather
       * than assumed, which is the whole of why this file lists objects one at
       * a time. */
      *ipc.c.o(.literal .literal.* .text .text.*)

      /* [step 380] The wifi bring-up configuration, 3,519 bytes.
       *
       * Runs on the net task through the job system, calls into the blob under
       * a mutex, and registers no interrupt handler -- the same three checks
       * every entry above had to pass. Added because 27 bytes of new
       * diagnostic pushed iram over again, which is the standing condition of
       * this build rather than a surprise. */
      *wifi_init_cfg.c.o(.literal .literal.* .text .text.*)

    /* [step 278] notes.c and term.c join it, for the same reason and with the
     * same evidence: both are views entered from the launcher, both run only
     * while a human is looking at them, and neither is referenced by anything
     * in the pinned set. They were in iram only because iram was the default,
     * not because anyone decided they belonged there. */
    *notes.c.o(.literal .literal.* .text .text.*)
    *term.c.o(.literal .literal.* .text .text.*)

    /* [next_moves/11 step 1] pcm.c: sample playback. Polled, not interrupt
     * driven, so nothing in it runs in ISR context, and nothing in the pinned
     * set calls it. The DMA reads its ring from DRAM without the CPU, so a
     * cache miss here delays a refill, never a sample. */
    *pcm.c.o(.literal .literal.* .text .text.*)

    /* [next_moves/11 step 2] fat.c: shell- and player-driven, no ISR path,
     * nothing pinned calls it. Its speed is the SD bus's, not the cache's. */
    *fat.c.o(.literal .literal.* .text .text.*)
    *mp3hdr.c.o(.literal .literal.* .text .text.*)

    /* [next_moves/11 step 4] mp3.c, i.e. minimp3: 19 KB, run by the decoder
     * task only. In flash first because iram is the scarcer resource; whether
     * cache misses cost real decode time is a measurement, not an assumption,
     * and moving it is a one-line change if they do. */
    *mp3.c.o(.literal .literal.* .text .text.*)
    /* [next_moves/11 step 6] the music view; human-driven, like browser.c */
    *player.c.o(.literal .literal.* .text .text.*)
    /* [next_moves/12 step 3] the video browser; human-driven, like player.c */
    *vidlist.c.o(.literal .literal.* .text .text.*)
    /* [next_moves/12 step 4] video playback, run by the media task. From
     * flash like mp3.c: its time goes to the SD bus and the panel. */
    *vplay.c.o(.literal .literal.* .text .text.*)

    /* [next_moves/12 step 10] audio.c, the tone generator: 1,978 bytes, moved
     * to make room for spi3.c's DMA diagnostics, which iram refused by 55
     * bytes. It programs LEDC and returns; audio_service() is a deadline check
     * the display task makes once a frame, and pcm.c -- already in flash --
     * is its only other caller. Nothing here runs in ISR context or touches
     * the flash bus, which step 316 established is the only disqualifier.
     *
     * The DMA data path itself stays in iram: sd.c and spi3.c are what this
     * step is trying to make cheaper, and measuring them through a cache would
     * confuse the measurement with the cache's mood. */
    *audio.c.o(.literal .literal.* .text .text.*)

    /* [next_moves/12 step 11] efuse.c, 146 bytes, read once during the boot
     * banner and never again. iram refused the SPI3 re-attach by 7 bytes. */
    *efuse.c.o(.literal .literal.* .text .text.*)

    /* [next_moves/12 step 11] adc.c, 2,489 bytes: adc_init() at boot and the
     * shell's four probe commands are its only callers outside itself -- grep
     * confirms nothing in the kernel samples it periodically. Room for the SD
     * driver's DMA diagnostics, which iram then refused by 15 bytes. */
    *adc.c.o(.literal .literal.* .text .text.*)

    /* [next_moves/13 step 1] Six more of the kernel's own files, moved to make
     * room for the Bluetooth controller's 35,680 resident bytes.
     *
     * The budget was estimated with `nm`, which sizes SYMBOLS and so missed
     * every literal pool: it said 61 KB of iram were free when the true figure
     * was closer to 20. These six are what covered the difference, and every
     * one of them meets step 316's rule -- nothing here runs with the cache
     * off, and nothing here can be reached during a flash erase, because
     * flash.c masks interrupts for the whole 125 ms (see the note above).
     *
     *   vm.c       the bytecode interpreter; applications are not timing work
     *   desktop.c  the launcher, drawn when a human is looking at it
     *   touch.c    the panel's controller, polled at the tick
     *   i2c.c      bit-banged, and I2C tolerates a stretched clock
     *   device.c   the device table, reached from VM syscalls
     *   raycast.c  the 3D view. It WILL be slower from flash; it is the only
     *              one of the six with a frame rate, and 16 fps has margin
     *
     * NOT moved, deliberately: display.c, sd.c and spi3.c, which are the video
     * path this project spent a day measuring, and task.c, panic.c, flash.c and
     * store.c, which are pinned for cause. */
    *vm.c.o(.literal .literal.* .text .text.*)
    *desktop.c.o(.literal .literal.* .text .text.*)
    *touch.c.o(.literal .literal.* .text .text.*)
    *i2c.c.o(.literal .literal.* .text .text.*)
    *device.c.o(.literal .literal.* .text .text.*)
    *raycast.c.o(.literal .literal.* .text .text.*)

    /* [next_moves/13 step 1] Espressif's PHY archive, which was the single
     * largest thing in IRAM for no reason: 27,131 bytes of phy_chip_v7, _cal
     * and _ana.
     *
     * Its calibration runs ONCE, from a task, through phy_stack_call() with the
     * cache on, and the hooks the blobs call afterwards come from the same
     * context. Nothing here is reached with the cache off or during a flash
     * erase, which is step 316's whole test.
     *
     * Its .iram1 sections are a different matter and stay resident -- they are
     * the radio's interrupt-time routines (ram_disable_agc, wr_bt_tx_atten and
     * the rest), and the linker places them in an orphan .iram1 section of
     * 8,941 bytes that this script deliberately does not touch.
     *
     * This is what made room for Bluetooth, and the WiFi build gets the same
     * 27 KB back. */
    *libphy_natos.a:(.literal .literal.* .text .text.*)
    *libpp_natos.a:(.literal .literal.* .text .text.*)

    /* [next_moves/13 step 1] The Bluetooth controller's non-resident code:
     * 96 KB of it, which is why this lives in flash rather than iram. Only its
     * .iram1 sections are interrupt-time; the rest is init, HCI and link
     * management, reached from the controller's own task with the cache on. */
    *libbtdm_app_natos.a:(.literal .literal.* .text .text.* .rodata .rodata.*)
    *libbt_natos.a:(.literal .literal.* .text .text.* .rodata .rodata.*)
    *libcoexist_natos.a:(.literal .literal.* .text .text.* .rodata .rodata.*)

    . = ALIGN(4);
    _flash_text_end = .;
  } > irom

  /* ---- the Bluetooth controller's resident code (next_moves/13 step 1) ----
   *
   * 39,686 bytes of it, and none of it can live in flash: these are the
   * controller's interrupt-time routines, which run while the radio is mid-
   * packet and cannot stall on a cache miss. That is the single largest claim
   * on iram this project has ever accepted, and the measurement that allowed
   * it is 61,119 bytes free in a blob-free build.
   *
   * BEFORE .text, because the linker assigns an input section exactly once and
   * .text's wildcard would otherwise claim these objects first. */
  .btdm_iram : ALIGN(4)
  {
    _btdm_iram_start = .;
    *libbtdm_app_natos.a:(.iram1 .iram1.*)
    *libbt_natos.a:(.iram1 .iram1.*)
    *libcoexist_natos.a:(.iram1 .iram1.*)
    . = ALIGN(4);
    _btdm_iram_end = .;
  } > iram

  /* ---- the radio's own resident code (next_moves/13 step 1) ---------------
   *
   * libphy keeps its interrupt-time routines in .iram1 and in dozens of
   * .phyiram.N sections -- ram_disable_agc, wr_bt_tx_atten and the rest, the
   * code that runs while a packet is in the air. Those stay in IRAM; it is only
   * the calibration that went to flash.
   *
   * Gathered into ONE output section, and that is not tidiness. The linker
   * gives every orphan section its own image segment, and esptool refuses an
   * image with more than sixteen: thirty-one .phyiram sections produced
   * "Invalid segment count 18 (max 16)", which reads like a linker script
   * problem because it is one. */
  .phy_iram : ALIGN(4)
  {
    _phy_iram_start = .;
    *(.iram1 .iram1.*)
    *(.phyiram .phyiram.*)
    *(.wifi0iram .wifi0iram.* .wifirxiram .wifirxiram.*)
    *(.wifiorslpiram .wifiorslpiram.* .wifislpiram .wifislpiram.*)
    *(.wifislprxiram .wifislprxiram.* .wifiextrairam .wifiextrairam.*)
    *(.coexiram .coexiram.*)
    . = ALIGN(4);
    _phy_iram_end = .;
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

  /* BEFORE .data, and that is the point: .data's wildcard claims every
   * input .data section it sees first, and 1,230 bytes of the controller's
   * own data went there in the first version -- outside the markers that
   * bracket it. The controller walks that range itself, so a marker pair
   * around a fragment is worse than none.
   */
  /* ---- the Bluetooth controller's data (next_moves/13 step 1) -------------
   *
   * The blob's writable state, and the five pairs of marker symbols it insists
   * on. They are not decoration: the controller walks _btdm_data_start.._end
   * and _btdm_bss_start.._end itself, to hand that memory to its own
   * allocator. Markers that bracket the wrong bytes mean the controller
   * scribbling on whatever happens to be there, which is the kind of fault
   * that presents as something else entirely.
   *
   * The _nimble_ pair exists because libbt.a references it whether or not a
   * NimBLE host is present. Empty here, and bracketing nothing, which is the
   * truth: there is no host stack in this image. */
  .btdm_data : ALIGN(4)
  {
    _bt_data_start = .;
    _btdm_data_start = .;
    /* .dram1.N, not .data: Espressif's archives name their writable data that
     * way, and the first version of this section came out EMPTY (0x0) while
     * three dozen .dram1 orphans each took an image segment -- which is what
     * esptool's "Invalid segment count" was really complaining about. The
     * markers then bracketed nothing, which is worse than an error: the
     * controller walks that range itself. */
    *libbtdm_app_natos.a:(.data .data.* .dram1 .dram1.*)
    *libbt_natos.a:(.data .data.* .dram1 .dram1.*)
    *libcoexist_natos.a:(.data .data.* .dram1 .dram1.*)
    *libphy_natos.a:(.data .data.* .dram1 .dram1.*)
    *libpp_natos.a:(.data .data.* .dram1 .dram1.*)
    *(.dram1 .dram1.*)
    . = ALIGN(4);
    _btdm_data_end = .;
    _bt_data_end = .;
    _nimble_data_start = .;
    _nimble_data_end = .;
  } > dram

  .btdm_bss (NOLOAD) : ALIGN(4)
  {
    _bt_bss_start = .;
    _btdm_bss_start = .;
    *libbtdm_app_natos.a:(.bss .bss.* COMMON)
    *libbt_natos.a:(.bss .bss.* COMMON)
    *libcoexist_natos.a:(.bss .bss.* COMMON)
    . = ALIGN(4);
    _btdm_bss_end = .;
    _bt_bss_end = .;
    _nimble_bss_start = .;
    _nimble_bss_end = .;
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

  /* SRAM1: NOLOAD, so nothing here is in the image and the contents at boot
   * are whatever the silicon holds. Users zero what they need. Placed with
   * __attribute__((section(".sram1"))). */
  .sram1 (NOLOAD) : ALIGN(16)
  {
    _sram1_start = .;
    *(.sram1 .sram1.*)
    . = ALIGN(4);
    _sram1_end = .;
  } > sram1

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
