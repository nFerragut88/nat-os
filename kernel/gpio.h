/* cyd-os — minimal GPIO.
 *
 * Only what the display needs: drive a pin high or low, and read one back.
 * No interrupts, no pull-ups, no analogue, no pins above 31 — the CYD's display
 * signals are all in the low bank, and a register set that stops at what is
 * actually used cannot be wrong about the rest.
 *
 * Two layers have to agree before a pin does anything:
 *
 *   1. IO_MUX selects which peripheral owns the pad. Function 2 is plain GPIO
 *      on every pin this kernel touches.
 *   2. The GPIO matrix selects which signal drives it. 256 (SIG_GPIO_OUT_IDX)
 *      means "whatever GPIO_OUT says" rather than a peripheral output.
 *
 * Getting either wrong leaves the pin floating, which on a backlight looks
 * exactly like a broken display.
 */

#ifndef CYDOS_GPIO_H
#define CYDOS_GPIO_H

#include <stdint.h>

#define GPIO_OUT_W1TS_REG     0x3FF44008u
#define GPIO_OUT_W1TC_REG     0x3FF4400Cu
#define GPIO_ENABLE_W1TS_REG  0x3FF44024u
#define GPIO_IN_REG           0x3FF4403Cu

/* GPIO_FUNCn_OUT_SEL_CFG_REG, one per pin. */
#define GPIO_FUNC_OUT_SEL(n)  (0x3FF44530u + 4u * (n))
#define SIG_GPIO_OUT_IDX      256u

/* IO_MUX pad registers are not in pin order — the table is the silicon's, not
 * ours, so it is written out rather than computed. Only the pins used here. */
#define IO_MUX_GPIO2          0x3FF49040u
#define IO_MUX_GPIO12         0x3FF49034u
#define IO_MUX_GPIO13         0x3FF49038u
#define IO_MUX_GPIO14         0x3FF49030u
#define IO_MUX_GPIO15         0x3FF4903Cu
#define IO_MUX_GPIO21         0x3FF4907Cu

/* MCU_SEL is bits 14:12, FUN_DRV bits 11:10. Function 2 = GPIO, drive 2. */
#define IO_MUX_GPIO_FUNC      ((2u << 12) | (2u << 10))

#define GPIO_REG(a) (*(volatile uint32_t *)(a))

static inline void gpio_out_init(uint32_t pin, uint32_t io_mux_reg)
{
    GPIO_REG(io_mux_reg)              = IO_MUX_GPIO_FUNC;
    GPIO_REG(GPIO_FUNC_OUT_SEL(pin))  = SIG_GPIO_OUT_IDX;
    GPIO_REG(GPIO_ENABLE_W1TS_REG)    = 1u << pin;
}

static inline void gpio_set(uint32_t pin)   { GPIO_REG(GPIO_OUT_W1TS_REG) = 1u << pin; }
static inline void gpio_clear(uint32_t pin) { GPIO_REG(GPIO_OUT_W1TC_REG) = 1u << pin; }

static inline uint32_t gpio_read(uint32_t pin)
{
    return (GPIO_REG(GPIO_IN_REG) >> pin) & 1u;
}

#endif /* CYDOS_GPIO_H */
