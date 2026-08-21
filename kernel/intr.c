/* nat-os — the interrupt matrix. See intr.h. */

#include "intr.h"
#include "timer.h"
#include "xtensa.h"
#include "uart.h"

/* The PRO CPU's map registers are one word per source, in the silicon's source
 * order, starting at the MAC source. The address is therefore computed rather
 * than tabulated — but the INDEX still has to be right, which is why intr.h
 * names the sources instead of letting callers pass a number.
 *
 * Checked against two independently known registers: TG0_T0 is source 14 and
 * maps to 0x3FF0013C, UART0 is source 34 and maps to 0x3FF0018C. Both fall out
 * of this base. */
#define DPORT_PRO_MAP_BASE  0x3FF00104u
#define DPORT_PRO_MAP(src)  (DPORT_PRO_MAP_BASE + 4u * (src))

#define DPORT_REG(a) (*(volatile uint32_t *)(a))

static intr_handler_fn g_handler[32];
static uint32_t        g_count[32];
static uint32_t        g_spurious;
static uint32_t        g_disabled;

void intr_install(uint32_t line, intr_handler_fn fn)
{
    if (line < 32u) {
        g_handler[line] = fn;
    }
}

void intr_route(uint32_t source, uint32_t line, intr_handler_fn fn)
{
    if (line >= 32u) {
        return;
    }

    /* Handler first. The matrix write can make the line fire immediately if the
     * peripheral is already asserting, and a line that fires before its handler
     * exists is a spurious interrupt that the defence in intr_dispatch() would
     * then permanently disable — the routing would appear to have failed. */
    g_handler[line] = fn;

    DPORT_REG(DPORT_PRO_MAP(source)) = line;
    xt_enable_interrupt(line);
}

/* Called from _handler_level3 with interrupts masked at level 3.
 *
 * The Level-3 vector is shared. Before this file, the handler assumed it could
 * only have been the tick and called timer_isr() unconditionally — which was
 * true when one source existed and becomes a clock corruption the moment a
 * second one is added, because timer_isr() advances the tick deadline by a full
 * interval every time it runs. Dispatching on the INTERRUPT register instead of
 * on an assumption is the whole change.
 */
void intr_dispatch(void)
{
    uint32_t pending = xt_get_interrupt() & xt_get_intenable();

    /* The tick first, and by name. It is not a matrix source — CCOMPARE1 is
     * internal to the core — so it has no handler in the table. */
    if (pending & (1u << INTR_LINE_TIMER1)) {
        g_count[INTR_LINE_TIMER1]++;
        timer_isr();
        pending &= ~(1u << INTR_LINE_TIMER1);
    }

    while (pending) {
        uint32_t line = 31u - (uint32_t)__builtin_clz(pending);
        pending &= ~(1u << line);

        if (g_handler[line]) {
            g_count[line]++;
            g_handler[line]();
            continue;
        }

        /* An enabled, pending, unhandled LEVEL-triggered line is a hang, not a
         * glitch: nothing clears the condition, so returning re-enters this
         * handler immediately and the machine never runs task code again. There
         * is no output from that state and no watchdog reach — the watchdog is
         * fed by a task, and tasks have stopped running.
         *
         * So the line is masked instead. The kernel loses that interrupt and
         * keeps running, which is recoverable and inspectable ('intr' in the
         * shell reports it) rather than a silent brick. */
        g_spurious++;
        g_disabled |= (1u << line);
        xt_disable_interrupt(line);
    }
}

uint32_t intr_count(uint32_t line)  { return (line < 32u) ? g_count[line] : 0u; }
uint32_t intr_spurious(void)        { return g_spurious; }
uint32_t intr_disabled_mask(void)   { return g_disabled; }

/* ---- register read-back -------------------------------------------------
 *
 * Every value below was written by this file or by gpio.h, and each was
 * believed correct on the strength of a datasheet reading. Zero interrupts
 * arrived anyway. Reading the registers back distinguishes "programmed wrongly"
 * from "programmed correctly and not firing", which no amount of re-reading the
 * source can do — this project has already spent two commits disproving
 * hypotheses about firmware that had never been flashed.
 */
#define GPIO_PIN36_REG      0x3FF44118u   /* 0x3FF44088 + 4 * 36 */
#define GPIO_STATUS1        0x3FF44050u
#define GPIO_PCPU_INT1      0x3FF44078u   /* STATUS1 masked by the PRO enable */
#define IO_MUX_SENSOR_VP    0x3FF49004u

static void row(const char *name, uint32_t v)
{
    uart_puts("     ");
    uart_puts(name);
    uart_puts(" = ");
    uart_put_hex(v);
    uart_puts("\n");
}

void intr_dump(void)
{
    uart_puts("   programmed state:\n");
    row("intenable       ", xt_get_intenable());
    row("interrupt       ", xt_get_interrupt());
    {
        /* [H1 experiment] ticks deferred because they landed inside a window
         * handler (EPS3.EXCM set). Zero here with a surviving wifiinit is the
         * H1 refutation condition. */
        extern volatile uint32_t g_tick_excm_hits, g_tick_excm_pc, g_tick_excm_ps;
        row("tick-excm hits  ", g_tick_excm_hits);
        if (g_tick_excm_hits) {
            row("tick-excm pc    ", g_tick_excm_pc);
            row("tick-excm ps    ", g_tick_excm_ps);
        }
    }
    row("dport map src22 ", DPORT_REG(DPORT_PRO_MAP(INTR_SRC_GPIO_PRO)));
    row("gpio_pin36      ", DPORT_REG(GPIO_PIN36_REG));
    row("gpio_status1    ", DPORT_REG(GPIO_STATUS1));
    row("io_mux_sensor_vp", DPORT_REG(IO_MUX_SENSOR_VP));

    /* The per-CPU masked status registers sit between STATUS1 and PIN0, and
     * their exact order is the thing in question — the first attempt at this
     * dump read 0x78 and reported a confident zero from what is probably the
     * APP CPU's copy. Printing the range and letting the value identify the
     * register is the same move as the calibration's pair check: do not ask the
     * hardware to confirm a guess, ask it what is there.
     *
     * GPIO 36 is bank-1 bit 4, so the register showing 0x10 is the one that
     * matters, and whether it shows it at all is the actual finding. */
    for (uint32_t a = 0x3FF44064u; a <= 0x3FF44084u; a += 4u) {
        uart_puts("     [");
        uart_put_hex(a);
        uart_puts("] = ");
        uart_put_hex(DPORT_REG(a));
        uart_puts("\n");
    }
    uart_puts("     expect: one of the above = 0x10 if the pin reaches the PRO CPU\n");
}

/* ---- which enable bit reaches THIS CPU ----------------------------------
 *
 * GPIO_PINn_INT_ENA is five bits wide and the datasheet's field order is the
 * one thing here that reading the source cannot settle. Bit 13 was chosen from
 * memory of the ESP-IDF constant, and the evidence says it delivers the pin
 * somewhere other than this CPU's ordinary interrupt: real taps latch
 * GPIO_STATUS1 bit 4, exactly one masked-status register reflects it, and the
 * CPU's INTERRUPT register never shows line 23.
 *
 * So the bits are tried instead of argued about. GPIO_STATUS1_W1TS sets a
 * status bit directly, which is indistinguishable from a pin edge to everything
 * downstream — the part that is already proven working. That makes this a test
 * of the enable path alone, and it needs no finger, which matters because the
 * alternative is asking someone to tap the panel five times while watching a
 * serial log.
 */
#define GPIO_STATUS1_W1TS_A  0x3FF44054u
#define GPIO_STATUS1_W1TC_A  0x3FF44058u
#define IRQ_PIN_BIT          4u          /* GPIO 36 in the 32-39 bank */

void intr_selftest(void)
{
    uint32_t saved = DPORT_REG(GPIO_PIN36_REG);
    int found = -1;

    uart_puts("   injecting an edge for each INT_ENA bit:\n");

    for (uint32_t bit = 13u; bit <= 17u; bit++) {
        DPORT_REG(GPIO_PIN36_REG)     = 0u;                  /* disable */
        DPORT_REG(GPIO_STATUS1_W1TC_A) = 1u << IRQ_PIN_BIT;  /* clear stale */

        uint32_t before = g_count[INTR_LINE_GPIO];

        DPORT_REG(GPIO_PIN36_REG)      = (2u << 7) | (1u << bit);
        DPORT_REG(GPIO_STATUS1_W1TS_A) = 1u << IRQ_PIN_BIT;  /* inject */

        for (volatile int i = 0; i < 2000; i++) {
        }

        uint32_t after = g_count[INTR_LINE_GPIO];

        uart_puts("     bit ");
        uart_put_dec(bit);
        uart_puts(after != before ? "  -> SERVICED\n" : "  -> nothing\n");
        if (after != before && found < 0) {
            found = (int)bit;
        }

        DPORT_REG(GPIO_PIN36_REG)      = 0u;
        DPORT_REG(GPIO_STATUS1_W1TC_A) = 1u << IRQ_PIN_BIT;
    }

    if (found >= 0) {
        uart_puts("   PRO CPU enable is bit ");
        uart_put_dec((uint32_t)found);
        uart_puts("; restoring with it\n");
        DPORT_REG(GPIO_PIN36_REG) = (2u << 7) | (1u << (uint32_t)found);
    } else {
        uart_puts("   no bit reached this CPU; restoring previous config\n");
        DPORT_REG(GPIO_PIN36_REG) = saved;
    }
    DPORT_REG(GPIO_STATUS1_W1TC_A) = 1u << IRQ_PIN_BIT;
}

/* Injects a single edge without touching the pin's configuration, so it lands
 * while the system is in its ordinary state — unlike intr_selftest(), which
 * reconfigures as it goes. Lets the wake path be tested against a waiting task
 * without needing a finger, which matters because "did the handler find a
 * waiter" is a race that a human tapping cannot be asked to hit repeatedly. */
void intr_poke(void)
{
    DPORT_REG(GPIO_STATUS1_W1TS_A) = 1u << IRQ_PIN_BIT;
}
