/* ESP32-2432S028R — the "Cheap Yellow Display".
 *
 * The board this kernel was written on and every report up to UM-NATOS-033 was
 * measured on. ESP32-D0WD (Xtensa LX6), 240x320 ILI9341 over SPI2, XPT2046
 * touch, microSD, an LDR, a speaker and an RGB LED.
 *
 * Every number here was read off the vendor project's own TFT_eSPI User_Setup.h
 * or measured on the board, never recalled. See UM-NATOS-015 §3 for why that
 * distinction earned its own paragraph: a wrong pin and a wrong init sequence
 * produce an identical symptom, and the difference between reading and
 * remembering was the difference between a first-try success and an afternoon.
 */

#ifndef NATOS_BOARD_CYD_H
#define NATOS_BOARD_CYD_H

#define BOARD_NAME "ESP32-2432S028R (CYD)"

/* ---- what is fitted ----------------------------------------------------- */
#define BOARD_HAS_DISPLAY 1
#define BOARD_HAS_TOUCH   1
#define BOARD_HAS_SD      1
#define BOARD_HAS_AUDIO   1
#define BOARD_HAS_LDR     1
#define BOARD_HAS_LORA    0

/* ---- WiFi, and the only vendor binaries in this project ------------------
 *
 * OFF by default, and that is a statement rather than a convenience.
 *
 * Everything else in this kernel is code from this project: the scheduler, the
 * heap, arenas, the VM, the display, touch, SD, SPI3, ADC, I2C, audio, flash,
 * persistence, IPC, the renderer. Thirty-three of thirty-seven source files
 * link nothing anyone else wrote.
 *
 * WiFi is the exception. It needs libphy.a for analog RF calibration -- VCO and
 * PLL tuning, filter calibration, I/Q imbalance, temperature compensation --
 * and that cannot be reimplemented from public information. Not "is hard":
 * Espressif has never published the RF characterisation it encodes. Even
 * esp32-open-mac, a dedicated reverse-engineering effort, keeps the PHY blob
 * and replaces only the MAC above it.
 *
 * So WiFi on this chip cannot be clean, and -- the part that decides it --
 * SUCCEEDING AT TRANSMIT WOULD NOT MAKE IT CLEAN. After however many months,
 * the image still links 1.4 MB of somebody else's binary. The work buys
 * function, never independence.
 *
 * Build with -WiFi to get it back. The research is not deleted; UM-NATOS-027,
 * 028 and 034 are among the better records in the project and the drivers still
 * compile. It is simply not what this kernel is by default. */
#define BOARD_HAS_WIFI    0

/* ---- panel: ILI9341 on SPI2 (HSPI), IO_MUX direct ------------------------
 *
 * RST is NOT wired to a GPIO on this board; it follows the ESP32's own reset,
 * so the only reset available after boot is the panel's 0x01 SWRESET.
 *
 * MISO is wired and reads all zeros on this module. `panelid` gets
 * 00 00 00 00 00 from both 0xD3 and 0x04, and `panelpull` exists to separate
 * "SDO not populated" from "something holds the line low" -- untested, because
 * the board was busy. UM-NATOS-030 §7. */
#define BOARD_TFT_MOSI    13u
#define BOARD_TFT_SCLK    14u
#define BOARD_TFT_CS      15u
#define BOARD_TFT_DC       2u
#define BOARD_TFT_BL      21u
#define BOARD_TFT_MISO    12u
#define BOARD_TFT_W      240u
#define BOARD_TFT_H      320u

/* ---- touch: XPT2046, bit-banged on its own pins -------------------------
 *
 * Not on SPI2 with the panel: the controller wants ~2 MHz and the panel is
 * driven at 40, and the two share no pins on this board anyway. */
#define BOARD_TOUCH_CLK   25u
#define BOARD_TOUCH_MOSI  32u
#define BOARD_TOUCH_MISO  39u   /* input only */
#define BOARD_TOUCH_CS    33u
#define BOARD_TOUCH_IRQ   36u   /* input only */

/* ---- microSD, bit-banged -------------------------------------------------
 *
 * These are SPI3's IO_MUX pins. That matters the moment anything else wants
 * SPI3 as a peripheral: "SPI3 is unclaimed" is true of the peripheral and false
 * of its default pins. Route SPI3 through the GPIO matrix instead. */
#define BOARD_SD_CS        5u
#define BOARD_SD_SCK      18u
#define BOARD_SD_MISO     19u
#define BOARD_SD_MOSI     23u

/* ---- I2C, bit-banged ----------------------------------------------------- */
#define BOARD_I2C_SDA     22u
#define BOARD_I2C_SCL     27u

/* ---- speaker and light sensor -------------------------------------------- */
#define BOARD_SPK         26u
#define BOARD_LDR_ADC_CH   6u   /* ADC1 channel 6 = GPIO34 */

/* A pin that is safe for a bring-up test to drive.
 *
 * GPIO17 is the blue leg of the RGB LED. Driving it blinks an LED and breaks
 * nothing -- and it is VISIBLE, so a test that claims to have driven a pin can
 * be checked by looking at the board.
 *
 * This constant exists because of a real mistake: the SPI3 loopback defaulted to
 * GPIO27, which is I2C SCL on this board, and left it driven by the SPI
 * peripheral. `i2c` then reported "bus is NOT sane" and looked exactly like a
 * regression from the board-header refactor happening at the same moment. The
 * pin was chosen while writing the very file that declares SCL is there. */
#define BOARD_SPARE_PIN   17u

/* ---- free pins -----------------------------------------------------------
 *
 * Genuinely little. 22 and 27 carry I2C, 35 is input-only, 21 is the backlight,
 * 0 is a strapping pin. Anything needing more than a couple of signals wants a
 * different board rather than a clever plan -- which is why the LoRa work
 * targets a board with the radio already fitted rather than wiring a module to
 * this one. */

#endif /* NATOS_BOARD_CYD_H */
