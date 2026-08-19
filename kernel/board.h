/* nat-os — which board this build is for.
 *
 * One header per board carries its pin map and what is fitted; this file picks
 * one and then checks that it said everything it had to.
 *
 * ---- why this exists -----------------------------------------------------
 *
 * Before this, seventeen pin numbers lived in five drivers, and each was paired
 * by hand with an IO_MUX register address at the call site:
 *
 *     gpio_out_init(PIN_MOSI, IO_MUX_GPIO13);
 *
 * Nothing checks that those two arguments describe the same pin. Move a pin for
 * a new board, forget the address, and the driver configures the WRONG PAD --
 * and the symptom is a dead signal with every register reading back exactly
 * what was written to it. That is the shape of fault this kernel has already
 * paid days for twice (UM-NATOS-030, UM-NATOS-033), and it was sitting in the
 * one place a board port is guaranteed to touch.
 *
 * gpio_io_mux() in gpio.h derives the address from the pin, so the pairing
 * cannot drift and the board headers below carry pin numbers only. Three
 * separate hand-maintained pin-to-address tables were deleted in the process
 * (audio.c's SPK_MUX[], shell.c's spkhold chain, and the private copies in
 * i2c.c and sd.c); all of them agreed with each other, which is luck rather
 * than a property anyone was maintaining.
 *
 * ---- what a board header does NOT do -------------------------------------
 *
 * It does not make the UI portable. `DISP_W`/`DISP_H` appear about eighty-five
 * times across ten files and the launcher, renderer, note pad and on-panel
 * terminal are all built for a 240x320 colour touchscreen. A different display
 * is a new driver; a much smaller display is a different interface. The feature
 * switches below exist so a board can decline that whole layer rather than
 * pretend to shrink it.
 */

#ifndef NATOS_BOARD_H
#define NATOS_BOARD_H

/* Selected with -DBOARD_CYD or -DBOARD_LORA32 from build.ps1. Defaulting rather
 * than erroring, because every existing report was measured on the CYD and a
 * build that silently changed board would invalidate all of them. */
#if defined(BOARD_LORA32)
#  include "board_lora32.h"
#elif defined(BOARD_CYD)
#  include "board_cyd.h"
#else
#  include "board_cyd.h"
#endif

/* Every board must answer all of these, including with 0. An omission would
 * otherwise read as "not fitted" through the preprocessor's own default, which
 * is a silent wrong answer rather than a build failure. */
#if !defined(BOARD_NAME)        || !defined(BOARD_HAS_DISPLAY) || \
    !defined(BOARD_HAS_TOUCH)   || !defined(BOARD_HAS_SD)      || \
    !defined(BOARD_HAS_AUDIO)   || !defined(BOARD_HAS_LDR)     || \
    !defined(BOARD_HAS_LORA)   || !defined(BOARD_SPARE_PIN)
#  error "board header must define BOARD_NAME, BOARD_SPARE_PIN and every BOARD_HAS_* switch"
#endif

#if BOARD_HAS_DISPLAY && (!defined(BOARD_TFT_MOSI) || !defined(BOARD_TFT_W))
#  error "BOARD_HAS_DISPLAY set without a panel pin map"
#endif
#if BOARD_HAS_LORA && !defined(BOARD_LORA_NSS)
#  error "BOARD_HAS_LORA set without a radio pin map"
#endif

/* ---- who owns a pin -----------------------------------------------------
 *
 * A bring-up test that drives an arbitrary pin can silently break a working
 * subsystem, and the damage shows up somewhere else entirely. Asking the board
 * first turns that into a refusal with a name attached.
 *
 * Returns 0 for a free pin, or a short description of what would break. */
static inline const char *board_pin_owner(uint32_t pin)
{
#if BOARD_HAS_DISPLAY
    if (pin == BOARD_TFT_MOSI || pin == BOARD_TFT_SCLK ||
        pin == BOARD_TFT_CS   || pin == BOARD_TFT_DC   ||
        pin == BOARD_TFT_MISO)                          { return "display"; }
    if (pin == BOARD_TFT_BL)                            { return "backlight"; }
#endif
#if BOARD_HAS_TOUCH
    if (pin == BOARD_TOUCH_CLK  || pin == BOARD_TOUCH_MOSI ||
        pin == BOARD_TOUCH_MISO || pin == BOARD_TOUCH_CS   ||
        pin == BOARD_TOUCH_IRQ)                         { return "touch"; }
#endif
#if BOARD_HAS_SD
    if (pin == BOARD_SD_CS   || pin == BOARD_SD_SCK ||
        pin == BOARD_SD_MISO || pin == BOARD_SD_MOSI)   { return "microSD"; }
#endif
#if BOARD_HAS_AUDIO
    if (pin == BOARD_SPK)                               { return "speaker"; }
#endif
#if BOARD_HAS_LORA
    if (pin == BOARD_LORA_SCK  || pin == BOARD_LORA_MOSI  ||
        pin == BOARD_LORA_MISO || pin == BOARD_LORA_NSS   ||
        pin == BOARD_LORA_RESET|| pin == BOARD_LORA_BUSY  ||
        pin == BOARD_LORA_DIO1)                         { return "radio"; }
#endif
#if defined(BOARD_I2C_SDA)
    if (pin == BOARD_I2C_SDA || pin == BOARD_I2C_SCL)   { return "i2c"; }
#endif
    return 0;
}

static inline int board_pin_claimed(uint32_t pin)
{
    return board_pin_owner(pin) != 0;
}

#endif /* NATOS_BOARD_H */
