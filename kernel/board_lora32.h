/* An ESP32 (LX6) board with an SX1262 fitted — a relay node.
 *
 * ============================ NOT VERIFIED ============================
 *
 * NOTHING IN THIS FILE HAS BEEN MEASURED. No board of this kind has been
 * connected to this kernel. Every pin below is a placeholder, and a placeholder
 * that looks like a pin map is exactly the sort of thing that gets trusted
 * later by someone who did not read this paragraph.
 *
 * Fill these in from the board's own schematic when it arrives, the way
 * board_cyd.h was: read, not recalled. A wrong pin and a wrong init sequence
 * produce the same symptom, and this kernel has burned days on that twice.
 *
 * ======================================================================
 *
 * ---- what a relay node is, and is not ------------------------------------
 *
 * Deliberately headless. A node bolted to a mast has nobody standing in front
 * of it, and the UI this kernel grew -- a 3x3 launcher of 80x51 cells, a 3D
 * view wanting an 80,640-byte framebuffer, a note pad, an on-panel terminal --
 * assumes a 240x320 colour touchscreen and cannot be squeezed onto the 128x64
 * monochrome OLED these boards typically carry. It should not be: the port is
 * "do not compile the UI", not "make it smaller".
 *
 * What such a node needs is what 23 of this kernel's source files already
 * provide without knowing a panel exists -- heap, arenas, IPC, the device
 * model, persistence, SD, SPI3, I2C, timers, the argument harness -- plus a
 * radio and a serial console.
 */

#ifndef NATOS_BOARD_LORA32_H
#define NATOS_BOARD_LORA32_H

#define BOARD_NAME "ESP32 + SX1262 (UNVERIFIED)"

/* ---- what is fitted ----------------------------------------------------- */
#define BOARD_HAS_DISPLAY 0     /* an SSD1306 may be present; not driven yet */
#define BOARD_HAS_TOUCH   0
#define BOARD_HAS_SD      0     /* several of these boards have a slot       */
#define BOARD_HAS_AUDIO   0
#define BOARD_HAS_LDR     0
#define BOARD_HAS_LORA    1

/* ---- SX1262 on SPI3, routed through the GPIO matrix ----------------------
 *
 * Seven signals. BUSY must be honoured before every command -- it is the one
 * thing an SX126x driver cannot skip and the usual reason a first bring-up
 * returns nothing but zeros.
 *
 * DIO1 is the interrupt line. It can be omitted and the status polled instead,
 * at the cost of latency; whether it is worth a pin depends on the board. */
#define BOARD_LORA_SCK    0xFFu
#define BOARD_LORA_MOSI   0xFFu
#define BOARD_LORA_MISO   0xFFu
#define BOARD_LORA_NSS    0xFFu
#define BOARD_LORA_RESET  0xFFu
#define BOARD_LORA_BUSY   0xFFu
#define BOARD_LORA_DIO1   0xFFu

/* Region. 902-928 MHz is the licence-free ISM band in the US; 868 is Europe.
 * 433 is amateur band in the US and not licence-free at useful power, which is
 * a legal question rather than a tuning one. */
#define BOARD_LORA_BAND_KHZ 915000u

/* No pin has been established as safe to drive on this board yet. 0xFF makes
 * the bring-up tests refuse rather than pick something and find out. */
#define BOARD_SPARE_PIN   0xFFu

#endif /* NATOS_BOARD_LORA32_H */
