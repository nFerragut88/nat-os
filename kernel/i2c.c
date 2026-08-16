/* nat-os — bit-banged I2C master. See i2c.h. */

#include "i2c.h"
#include "gpio.h"
#include "uart.h"

/* IO_MUX pad registers, and the pull-up bit. Both taken from the vendor's
 * io_mux_reg.h rather than recalled — FUN_PU is BIT(8), FUN_IE is BIT(9), and
 * an afternoon went into learning that this file's kind of constant is not
 * something to remember (UM-NATOS-023 §5.1, UM-NATOS-024 §4). */
#define IO_MUX_GPIO22   0x3FF49080u
#define IO_MUX_GPIO27   0x3FF4902Cu
#define FUN_PU          (1u << 8)
#define FUN_IE          (1u << 9)
#define MCU_SEL_GPIO    (2u << 12)

/* Half a bit period.
 *
 * Deliberately slow. The internal pull-ups are ~45 kOhm, so a released line
 * rises through an RC that a proper 4.7 kOhm bus would not have, and a master
 * that clocks faster than the line can rise reads its own transition times as
 * data. Nothing here needs speed: a sensor read is a few dozen bytes and the
 * caller is a task that sleeps anyway. */
static inline void half_bit(void)
{
    for (volatile int i = 0; i < 60; i++) {
    }
}

/* Open drain, emulated.
 *
 * A line is pulled low by ENABLING the output driver (which is already set to
 * output zero) and released by DISABLING it, letting the pull-up do the rest.
 * Never driven high: two masters, or a slave mid-ACK, would then be shorted
 * against each other rather than merely contending. */
static inline void line_low(uint32_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_ENABLE_W1TS_REG) = 1u << pin;
    } else {
        GPIO_REG(GPIO_ENABLE1_W1TS_REG) = 1u << (pin - 32u);
    }
}

static inline void line_release(uint32_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_ENABLE_W1TC_REG) = 1u << pin;
    } else {
        GPIO_REG(GPIO_ENABLE1_W1TC_REG) = 1u << (pin - 32u);
    }
}

static inline uint32_t line_read(uint32_t pin)
{
    return gpio_read(pin);
}

/* Releases SCL and waits for it to actually be high.
 *
 * This is the clock-stretch handler, and it is the reason a preempted
 * transaction survives: the master does not assume the clock rose because it
 * let go of it. A slave holding SCL down is legal and expected. A line that
 * never rises is a stuck bus or a missing pull-up, and that is a timeout rather
 * than an infinite wait inside a shell command. */
static int scl_release_and_wait(void)
{
    line_release(I2C_PIN_SCL);
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        if (line_read(I2C_PIN_SCL)) {
            half_bit();
            return I2C_OK;
        }
    }
    return I2C_ETIMEOUT;
}

static void pad_init(uint32_t pin, uint32_t mux_reg)
{
    /* Output register holds zero permanently; only the enable is toggled. */
    if (pin < 32u) {
        GPIO_REG(GPIO_OUT_W1TC_REG) = 1u << pin;
        GPIO_REG(GPIO_ENABLE_W1TC_REG) = 1u << pin;
    }
    GPIO_REG(GPIO_FUNC_OUT_SEL(pin)) = SIG_GPIO_OUT_IDX;

    /* FUN_IE because the master must READ these lines — for the ACK bit, for
     * incoming data, and for clock stretching. An open-drain pin whose input
     * buffer is off reads a constant zero, which would look like every device
     * ACKing everything. */
    GPIO_REG(mux_reg) = MCU_SEL_GPIO | FUN_IE | FUN_PU;
}

void i2c_init(void)
{
    pad_init(I2C_PIN_SDA, IO_MUX_GPIO22);
    pad_init(I2C_PIN_SCL, IO_MUX_GPIO27);
    line_release(I2C_PIN_SDA);
    line_release(I2C_PIN_SCL);
    half_bit();
}

static void i2c_start(void)
{
    line_release(I2C_PIN_SDA);
    (void)scl_release_and_wait();
    line_low(I2C_PIN_SDA);          /* SDA falls while SCL is high */
    half_bit();
    line_low(I2C_PIN_SCL);
    half_bit();
}

static void i2c_stop(void)
{
    line_low(I2C_PIN_SDA);
    (void)scl_release_and_wait();
    line_release(I2C_PIN_SDA);      /* SDA rises while SCL is high */
    half_bit();
}

/* Returns I2C_OK if the slave pulled SDA down for the ACK bit. */
static int i2c_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        if ((b >> i) & 1u) {
            line_release(I2C_PIN_SDA);
        } else {
            line_low(I2C_PIN_SDA);
        }
        half_bit();
        if (scl_release_and_wait() != I2C_OK) {
            return I2C_ETIMEOUT;
        }
        line_low(I2C_PIN_SCL);
        half_bit();
    }

    /* Release SDA so the slave can drive the ACK. */
    line_release(I2C_PIN_SDA);
    half_bit();
    if (scl_release_and_wait() != I2C_OK) {
        return I2C_ETIMEOUT;
    }
    int ack = (line_read(I2C_PIN_SDA) == 0u);
    line_low(I2C_PIN_SCL);
    half_bit();

    return ack ? I2C_OK : I2C_ENAK;
}

static int i2c_read_byte(uint8_t *out, int send_ack)
{
    uint8_t v = 0;

    line_release(I2C_PIN_SDA);
    for (int i = 7; i >= 0; i--) {
        if (scl_release_and_wait() != I2C_OK) {
            return I2C_ETIMEOUT;
        }
        v = (uint8_t)((v << 1) | (line_read(I2C_PIN_SDA) & 1u));
        line_low(I2C_PIN_SCL);
        half_bit();
    }

    /* The master's ACK tells the slave whether another byte is wanted. The LAST
     * byte must be NAKed: a slave that is ACKed keeps driving the bus and the
     * following STOP is then issued into a bus somebody else owns. */
    if (send_ack) {
        line_low(I2C_PIN_SDA);
    } else {
        line_release(I2C_PIN_SDA);
    }
    half_bit();
    if (scl_release_and_wait() != I2C_OK) {
        return I2C_ETIMEOUT;
    }
    line_low(I2C_PIN_SCL);
    line_release(I2C_PIN_SDA);
    half_bit();

    *out = v;
    return I2C_OK;
}

int i2c_probe(uint8_t addr7)
{
    i2c_start();
    int r = i2c_write_byte((uint8_t)(addr7 << 1));   /* write bit clear */
    i2c_stop();
    return r;
}

int i2c_write(uint8_t addr7, const uint8_t *data, uint32_t len)
{
    i2c_start();
    int r = i2c_write_byte((uint8_t)(addr7 << 1));
    for (uint32_t i = 0; r == I2C_OK && i < len; i++) {
        r = i2c_write_byte(data[i]);
    }
    i2c_stop();
    return r;
}

int i2c_read(uint8_t addr7, uint8_t *buf, uint32_t len)
{
    i2c_start();
    int r = i2c_write_byte((uint8_t)((addr7 << 1) | 1u));
    for (uint32_t i = 0; r == I2C_OK && i < len; i++) {
        r = i2c_read_byte(&buf[i], (i + 1u) < len);
    }
    i2c_stop();
    return r;
}

int i2c_write_read(uint8_t addr7, const uint8_t *tx, uint32_t txlen,
                   uint8_t *rx, uint32_t rxlen)
{
    i2c_start();
    int r = i2c_write_byte((uint8_t)(addr7 << 1));
    for (uint32_t i = 0; r == I2C_OK && i < txlen; i++) {
        r = i2c_write_byte(tx[i]);
    }

    /* Repeated START, not STOP-then-START. Releasing the bus between the
     * register number and the read lets another master interleave, and on a
     * single-master bus it still costs a slave the right to keep its internal
     * address pointer. */
    if (r == I2C_OK) {
        i2c_start();
        r = i2c_write_byte((uint8_t)((addr7 << 1) | 1u));
        for (uint32_t i = 0; r == I2C_OK && i < rxlen; i++) {
            r = i2c_read_byte(&rx[i], (i + 1u) < rxlen);
        }
    }
    i2c_stop();
    return r;
}

/* ---- verification -------------------------------------------------------
 *
 * A scan on a bus with no pull-up reports every address as present, because a
 * floating SDA that reads low is indistinguishable from every device in the
 * world ACKing at once. That is the failure this checks for, and it checks it
 * per line and in both directions so that "the pull-up works" and "the driver
 * can pull down" are separate results.
 */
void i2c_selftest(void)
{
    static const uint32_t pin[2]  = { I2C_PIN_SDA, I2C_PIN_SCL };
    static const char    *name[2] = { "SDA (gpio22)", "SCL (gpio27)" };
    int ok = 1;

    for (int i = 0; i < 2; i++) {
        line_release(pin[i]);
        half_bit();
        uint32_t released = line_read(pin[i]);

        line_low(pin[i]);
        half_bit();
        uint32_t driven = line_read(pin[i]);

        line_release(pin[i]);
        half_bit();

        uart_puts("   ");
        uart_puts(name[i]);
        uart_puts(released ? "  released=HIGH" : "  released=LOW ");
        uart_puts(driven ? "  driven=HIGH" : "  driven=LOW ");

        if (released && !driven) {
            uart_puts("   ok\n");
        } else if (!released) {
            uart_puts("   NO PULL-UP or shorted low\n");
            ok = 0;
        } else {
            uart_puts("   cannot drive low\n");
            ok = 0;
        }
    }

    uart_puts(ok ? "   bus looks sane; a scan can be believed\n"
                 : "   bus is NOT sane; ignore any scan result\n");
}

void i2c_scan(void)
{
    uint32_t found = 0;

    uart_puts("   scanning 0x08..0x77\n");
    for (uint8_t a = 0x08u; a <= 0x77u; a++) {
        int r = i2c_probe(a);
        if (r == I2C_OK) {
            found++;
            uart_puts("     device at 0x");
            uart_put_hex(a);
            uart_puts("\n");
        } else if (r == I2C_ETIMEOUT) {
            uart_puts("     bus stuck at 0x");
            uart_put_hex(a);
            uart_puts(" - SCL never rose\n");
            return;
        }
    }

    uart_puts("   ");
    uart_put_dec(found);
    uart_puts(" device(s)\n");
    if (found == 0u) {
        uart_puts("   (nothing attached is the expected result on a bare board)\n");
    } else if (found > 100u) {
        uart_puts("   (that many is a stuck SDA, not that many devices)\n");
    }
}
