/* nat-os — minimal GPIO.
 *
 * Drive a pin high or low, read one back, and — since PENIRQ became the first
 * peripheral interrupt this kernel routes — raise an interrupt on an edge. No
 * pull-ups and no analogue: a register set that stops at what is actually used
 * cannot be wrong about the rest.
 *
 * The claim that this file needed no pins above 31 was already false when it
 * was written; the touch controller sits on 32, 33, 36 and 39, and the two-bank
 * split below is the scar from discovering that.
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

#ifndef NATOS_GPIO_H
#define NATOS_GPIO_H

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

/* ---- pin interrupts ----------------------------------------------------
 *
 * A pin raises the single GPIO peripheral interrupt source, which the matrix
 * routes to a CPU line (see intr.h). The source is shared by all 40 pins, so
 * the handler must ask GPIO_STATUS which one it was.
 *
 * Two banks again, and here the split is dangerous rather than merely annoying:
 * a status bit left set holds the shared source asserted, so failing to clear
 * the right bank's bit hangs the machine in the handler instead of dropping an
 * event. gpio_int_clear() picks the bank from the pin, like every other
 * accessor in this file. */
#define GPIO_STATUS_REG       0x3FF44044u   /* pins 0-31  */
#define GPIO_STATUS_W1TC_REG  0x3FF4404Cu
#define GPIO_STATUS1_REG      0x3FF44050u   /* pins 32-39 */
#define GPIO_STATUS1_W1TC_REG 0x3FF44058u

/* GPIO_PINn_REG: INT_TYPE is bits 9:7, and INT_ENA is a 5-bit field at 17:13
 * selecting which CPU and which kind of interrupt the pin is delivered to.
 *
 * Bit 15 is THIS CPU's ordinary interrupt, and that number is measured rather
 * than read. The obvious choice — bit 13, the low bit of the field, matching
 * the vendor header's PRO_CPU_INTR_ENA being BIT(0) — produced a pin that
 * latched its edge in GPIO_STATUS1 and never reached the processor. It was
 * being delivered to the APP CPU, which this kernel never starts, so the edge
 * arrived correctly at a core that was halted.
 *
 * That failure is worth naming because every register involved read back
 * exactly as intended. `intr_selftest()` settled it by injecting an edge
 * through GPIO_STATUS1_W1TS once for each candidate bit and reporting which one
 * the handler actually saw; it is still in intr.c, and re-running `irqtest`
 * re-derives this constant on any board rather than trusting this comment. */
#define GPIO_PIN_REG(n)       (0x3FF44088u + 4u * (n))
#define GPIO_PIN_INT_TYPE(t)  ((t) << 7)
#define GPIO_PIN_INT_ENA_PRO  (1u << 15)

#define GPIO_INT_DISABLED     0u
#define GPIO_INT_RISING       1u
#define GPIO_INT_FALLING      2u
#define GPIO_INT_ANY_EDGE     3u
#define GPIO_INT_LOW_LEVEL    4u
#define GPIO_INT_HIGH_LEVEL   5u

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

/* Clears a pin's pending interrupt. Must be called by the handler BEFORE it
 * returns — see the bank note above. */
static inline void gpio_int_clear(uint32_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_STATUS_W1TC_REG)  = 1u << pin;
    } else {
        GPIO_REG(GPIO_STATUS1_W1TC_REG) = 1u << (pin - 32u);
    }
}

static inline uint32_t gpio_int_pending(uint32_t pin)
{
    if (pin < 32u) {
        return (GPIO_REG(GPIO_STATUS_REG) >> pin) & 1u;
    }
    return (GPIO_REG(GPIO_STATUS1_REG) >> (pin - 32u)) & 1u;
}

/* Enables interrupts on a pin. The pad must already be configured as an input;
 * this only sets the trigger condition and the PRO CPU's enable bit.
 *
 * Clears any stale status first. A pin that was already asserting before its
 * enable bit was set has a status bit set, and enabling into that state fires
 * the handler for an edge that happened before anyone was listening. */
static inline void gpio_int_enable(uint32_t pin, uint32_t type)
{
    gpio_int_clear(pin);
    GPIO_REG(GPIO_PIN_REG(pin)) = GPIO_PIN_INT_TYPE(type) | GPIO_PIN_INT_ENA_PRO;
}

static inline void gpio_int_disable(uint32_t pin)
{
    GPIO_REG(GPIO_PIN_REG(pin)) = 0u;
    gpio_int_clear(pin);
}

#endif /* NATOS_GPIO_H */
