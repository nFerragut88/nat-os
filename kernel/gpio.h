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

/* TWO BANKS. Pins 0-31 and pins 32-39 have entirely separate registers, and
 * nothing complains if the wrong one is used — a shift of 39 on a 32-bit
 * register is undefined and in practice reads zero. That is indistinguishable
 * from a device wired correctly but not answering, which is exactly how the
 * touch controller first presented: MISO on GPIO 39 read a constant 0, and
 * MOSI and CS on 32 and 33 were never driven at all. */
#define GPIO_OUT_W1TS_REG     0x3FF44008u   /* pins 0-31  */
#define GPIO_OUT_W1TC_REG     0x3FF4400Cu
#define GPIO_OUT1_W1TS_REG    0x3FF44014u   /* pins 32-39 */
#define GPIO_OUT1_W1TC_REG    0x3FF44018u

#define GPIO_ENABLE_W1TS_REG  0x3FF44024u
#define GPIO_ENABLE_W1TC_REG  0x3FF44028u
#define GPIO_ENABLE1_W1TS_REG 0x3FF44030u
#define GPIO_ENABLE1_W1TC_REG 0x3FF44034u

#define GPIO_IN_REG           0x3FF4403Cu
#define GPIO_IN1_REG          0x3FF44040u

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

/* Touch. GPIO 34-39 are INPUT ONLY on this part, which is why MISO and IRQ sit
 * on 39 and 36 — they can never be driven by mistake. */
#define IO_MUX_GPIO25         0x3FF49024u
#define IO_MUX_GPIO32         0x3FF4901Cu
#define IO_MUX_GPIO33         0x3FF49020u
#define IO_MUX_GPIO36         0x3FF49004u   /* SENSOR_VP */
#define IO_MUX_GPIO39         0x3FF49010u   /* SENSOR_VN */

/* MCU_SEL is bits 14:12, FUN_DRV bits 11:10. Function 2 = GPIO, drive 2. */
#define IO_MUX_GPIO_FUNC      ((2u << 12) | (2u << 10))

/* Same function select, plus FUN_IE. An input pad with FUN_IE clear reads as a
 * constant 0 rather than failing visibly, which is indistinguishable from a
 * device that is present but always answering zero. */
#define IO_MUX_GPIO_IN        ((2u << 12) | (1u << 9))

#define GPIO_REG(a) (*(volatile uint32_t *)(a))

/* Every accessor below picks its bank from the pin number, so a caller never
 * has to know the split exists. */

static inline void gpio_out_init(uint32_t pin, uint32_t io_mux_reg)
{
    GPIO_REG(io_mux_reg)             = IO_MUX_GPIO_FUNC;
    GPIO_REG(GPIO_FUNC_OUT_SEL(pin)) = SIG_GPIO_OUT_IDX;
    if (pin < 32u) {
        GPIO_REG(GPIO_ENABLE_W1TS_REG)  = 1u << pin;
    } else {
        GPIO_REG(GPIO_ENABLE1_W1TS_REG) = 1u << (pin - 32u);
    }
}

static inline void gpio_in_init(uint32_t pin, uint32_t io_mux_reg)
{
    GPIO_REG(io_mux_reg) = IO_MUX_GPIO_IN;
    if (pin < 32u) {
        GPIO_REG(GPIO_ENABLE_W1TC_REG)  = 1u << pin;
    } else {
        GPIO_REG(GPIO_ENABLE1_W1TC_REG) = 1u << (pin - 32u);
    }
}

static inline void gpio_set(uint32_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_OUT_W1TS_REG)  = 1u << pin;
    } else {
        GPIO_REG(GPIO_OUT1_W1TS_REG) = 1u << (pin - 32u);
    }
}

static inline void gpio_clear(uint32_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_OUT_W1TC_REG)  = 1u << pin;
    } else {
        GPIO_REG(GPIO_OUT1_W1TC_REG) = 1u << (pin - 32u);
    }
}

static inline uint32_t gpio_read(uint32_t pin)
{
    if (pin < 32u) {
        return (GPIO_REG(GPIO_IN_REG) >> pin) & 1u;
    }
    return (GPIO_REG(GPIO_IN1_REG) >> (pin - 32u)) & 1u;
}

#endif /* CYDOS_GPIO_H */
