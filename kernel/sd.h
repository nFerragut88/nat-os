/* nat-os — microSD over SPI.
 *
 * The first storage in this project that a person can remove, carry away, and
 * fill on another machine. Flash holds what the kernel was built with; this
 * holds what the user brought.
 *
 * ---- why SPI mode and not SDIO -------------------------------------------
 *
 * The card supports a 4-bit parallel protocol that is several times faster.
 * This driver uses the 1-bit SPI mode instead, because:
 *
 *   - it is the mode every card must support, so a card that fails here is
 *     faulty rather than unsupported
 *   - the CYD wires the slot to ordinary GPIOs, not to the ESP32's dedicated
 *     SDMMC pins, so the fast path is not available on this board anyway
 *   - this project has now brought up three SPI devices, and reusing a shape
 *     that is understood beats learning a protocol whose failures are new
 *
 * ---- bit-banged first ------------------------------------------------------
 *
 * As with the display (UM-NATOS-015 §3), the first version bit-bangs. Card
 * initialisation is a one-time cost measured in milliseconds and is required to
 * run at 400 kHz or slower, which no hardware peripheral makes easier. Sector
 * reads can move to SPI3 once something is proven to be reading the right
 * bytes; moving first would mean debugging a protocol and a peripheral at the
 * same time, which is how the display cost three commits to one defect.
 *
 * ---- pins ------------------------------------------------------------------
 *
 * Taken from the ESP32-2432S028R board design, which puts the slot on the VSPI
 * default pads. These are ASSUMED until a card answers CMD0 — a wrong pin map
 * and an absent card are the same silence, so sd_init() reports which stage
 * failed rather than a single boolean.
 */

#ifndef NATOS_SD_H
#define NATOS_SD_H

#include <stdint.h>

#define SD_PIN_CS    5u
#define SD_PIN_SCK  18u
#define SD_PIN_MISO 19u
#define SD_PIN_MOSI 23u

#define SD_BLOCK_SIZE 512u

/* Distinct codes per failure stage. "No card" and "wrong wiring" and "card
 * present but refuses the voltage range" are three different problems, and a
 * driver that returns -1 for all of them makes the user guess. */
typedef enum {
    SD_OK           =  0,
    SD_ERR_IDLE     = -1,   /* no answer to CMD0 — absent card, or bad wiring */
    SD_ERR_IFCOND   = -2,   /* CMD8 rejected — pre-2.0 card, or a bad bus     */
    SD_ERR_READY    = -3,   /* ACMD41 never completed initialisation          */
    SD_ERR_OCR      = -4,   /* CMD58 failed, so addressing mode is unknown    */
    SD_ERR_BLOCKLEN = -5,   /* CMD16 refused on a byte-addressed card         */
    SD_ERR_READ     = -6,   /* CMD17 refused                                  */
    SD_ERR_TOKEN    = -7,   /* card never sent a data token                   */
} sd_err_t;

typedef enum {
    SD_TYPE_NONE = 0,
    SD_TYPE_SDSC,           /* byte-addressed, needs a 512-byte block length */
    SD_TYPE_SDHC,           /* block-addressed; CMD17 takes an LBA directly  */
} sd_type_t;

/* Runs the power-up and identification sequence. Returns SD_OK or one of the
 * codes above. Safe to call with no card inserted: it fails at SD_ERR_IDLE
 * after a bounded wait rather than hanging. */
int sd_init(void);

/* Reads one 512-byte block. `lba` is a block index on SDHC and is converted to
 * a byte offset internally on SDSC, so callers always speak in blocks. */
int sd_read_block(uint32_t lba, uint8_t *dst);

sd_type_t sd_type(void);

/* The R1 response byte from the last command, and the stage that failed.
 * Reported because SD failures are almost always diagnosable from R1 alone —
 * bit 2 is "illegal command", bit 0 is "still idle" — and losing it means
 * guessing from a return code that has already collapsed the detail. */
uint32_t sd_last_r1(void);
uint32_t sd_init_attempts(void);

#endif /* NATOS_SD_H */
