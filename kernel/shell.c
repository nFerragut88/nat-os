/* nat-os — minimal console shell. See shell.h.
 *
 * String handling is hand-rolled: there is no libc, and the three primitives
 * this needs are shorter than the argument for pulling one in.
 */

#include "shell.h"
#include "console.h"
#include "app.h"
#include "heap.h"
#include "raycast.h"
#include "critical.h"
#include "timer.h"
#include "task.h"
#include "sd.h"
#include "touch.h"
#include "calib.h"
#include "intr.h"
#include "adc.h"
#include "i2c.h"
#include "audio.h"
#include "desktop.h"
#include "uart.h"
#include "xtensa.h"
#include "vm.h"

#define LINE_MAX 64

static const shell_program_t *g_progs;
static int      g_prog_count;
static char     g_line[LINE_MAX];
static uint32_t g_len;

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return *a == *b;
}

/* Parses a non-negative decimal. Returns -1 on anything else, so a typo is
 * rejected rather than silently read as zero — which would make "kill oops"
 * kill application 0. */
static int parse_int(const char *s)
{
    if (!*s) {
        return -1;
    }
    int v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        v = v * 10 + (*s - '0');
        if (v > 9999) {
            return -1;
        }
        s++;
    }
    return v;
}

/* Splits the line in place at the first space; returns the argument, or "". */
static char *split(char *line)
{
    while (*line && *line != ' ') {
        line++;
    }
    if (!*line) {
        return line;                /* points at the NUL — an empty argument */
    }
    *line++ = 0;
    while (*line == ' ') {
        line++;
    }
    return line;
}

void shell_register(const shell_program_t *table, int count)
{
    g_progs = table;
    g_prog_count = count;
}

static void cmd_help(void)
{
    uart_puts("  commands:\n"
              "    fb [on|off]   framebuffer for the 3D view\n"
              "    ps            list applications\n"
              "    progs         list loadable programs\n"
              "    run <name>    start a program\n"
              "    kill <id>     stop an application, releasing its arena\n"
              "    mem           heap statistics\n"
              "    stacks        per-task stack headroom\n"
              "    fault         take an illegal instruction (panics)\n"
              "    smash         break this task's stack guard (panics)\n"
              "    sd            probe the microSD card\n"
              "    sdread <lba>  read and dump one 512 B block\n"
              "    cal           calibrate the touch panel\n"
              "    calshow       last calibration result and readings\n"
              "    intr          interrupt matrix counters\n"
              "    adc           read every ADC1 channel\n"
              "    ldrscan       watch all ADC channels for movement\n"
              "    i2c           check the bus and scan it\n"
              "    tone <hz>     tone on gpio26; 'tone 0' stops. try 3000, not 440\n"
              "    beep          a short 3 kHz beep\n"
              "    3d [off]      3D view or launcher\n"
              "    taps          dump the touch press log\n"
              "    tapsclear     empty it\n"
              "    hang          wedge the kernel; the watchdog should reset it\n"
              "    help          this\n"
              "  bring-up probes (they poke at live hardware):\n"
              "    irqtest       find the PRO CPU interrupt-enable bit\n"
              "    irqpoke       inject one GPIO edge\n"
              "    adcprobe      sweep the RTC sensor-pad mux\n"
              "    adcconv       prove an ADC conversion really runs\n"
              "    adcdrive      sweep against gpio32 - DRIVES touch MOSI\n"
              "    ldr           watch the light sensor only\n"
              "    findspk       square wave on each candidate speaker pin\n"
              "    spktest       prove the square-wave generator works\n");
}

static void cmd_ps(void)
{
    uart_puts("   id  name        state     arena     insns      published\n");
    for (int id = 0; id < APP_MAX; id++) {
        if (app_state(id) == APP_FREE) {
            continue;
        }
        uart_puts("   ");
        uart_put_dec((unsigned int)id);
        uart_puts("   ");
        uart_puts(app_name(id));
        uart_puts("   ");
        uart_puts(app_state_name(id));
        uart_puts("   ");
        uart_put_dec(app_arena_bytes(id));
        uart_puts(" B   ");
        uart_put_dec(app_instructions(id));
        uart_puts("   ");
        uart_put_dec(app_published(id));
        if (app_state(id) == APP_FAULTED) {
            uart_puts("   [");
            uart_puts(vm_fault_name(app_fault(id)));
            uart_puts(" @");
            uart_put_dec(app_fault_detail(id));
            uart_puts("]");
        }
        uart_puts("\n");
    }
    if (app_live_count() == 0) {
        uart_puts("   (none running)\n");
    }
}

static void cmd_progs(void)
{
    for (int i = 0; i < g_prog_count; i++) {
        uart_puts("   ");
        uart_puts(g_progs[i].name);
        uart_puts("   ");
        uart_put_dec(g_progs[i].len);
        uart_puts(" B image, ");
        uart_put_dec(g_progs[i].arena_bytes);
        uart_puts(" B arena\n");
    }
}

int shell_launch(const char *name)
{
    for (int i = 0; i < g_prog_count; i++) {
        if (str_eq(g_progs[i].name, name)) {
            return app_start(g_progs[i].name, g_progs[i].img, g_progs[i].len,
                             g_progs[i].arena_bytes, g_progs[i].publish_off);
        }
    }
    return -1;
}

static void cmd_run(const char *name)
{
    for (int i = 0; i < g_prog_count; i++) {
        if (str_eq(g_progs[i].name, name)) {
            int id = app_start(g_progs[i].name, g_progs[i].img, g_progs[i].len,
                               g_progs[i].arena_bytes, g_progs[i].publish_off);
            if (id < 0) {
                uart_puts("   cannot start: no free slot or no memory\n");
            } else {
                uart_puts("   started id=");
                uart_put_dec((unsigned int)id);
                uart_puts("\n");
            }
            return;
        }
    }
    uart_puts("   no such program (try 'progs')\n");
}

/* Reads every ADC1 channel, not just the interesting one.
 *
 * Four of these pads belong to the touch controller, so their values are this
 * kernel's own wiring rather than the world — and that is exactly what makes
 * them useful here. A converter that is not really converting returns the same
 * number on every channel, and eight identical readings are obvious in a way
 * that one plausible reading is not.
 *
 * It also tests a claim about the board rather than trusting one: the LDR is
 * SAID to be on GPIO34. Whichever channel moves when the room light changes is
 * the one that actually is. */
static void cmd_adc(void)
{
    static const uint32_t pad[8] = { 36, 37, 38, 39, 32, 33, 34, 35 };

    uart_puts("   ch  gpio  value   note\n");
    for (uint32_t ch = 0; ch < 8u; ch++) {
        uint32_t v = adc1_read_avg(ch, 8u);

        uart_puts("   ");
        uart_put_dec(ch);
        uart_puts("   ");
        uart_put_dec(pad[ch]);
        uart_puts("    ");
        if (v == ADC_INVALID) {
            uart_puts("----   no conversion\n");
            continue;
        }
        uart_put_dec(v);
        uart_puts("    ");
        if (ch == 0u || ch == 3u) {
            uart_puts("touch penirq/miso\n");
        } else if (ch == 4u || ch == 5u) {
            uart_puts("touch mosi/cs\n");
        } else if (ch == ADC1_CH_LDR) {
            uart_puts("<- LDR? cover it and re-run\n");
        } else {
            uart_puts("header\n");
        }
    }
    adc_dump();
    adc_dump_rtcio();
}

static void cmd_mem(void)
{
    uart_puts("   heap free=");
    uart_put_dec(heap_free_bytes());
    uart_puts(" largest=");
    uart_put_dec(heap_largest_free());
    uart_puts(" blocks=");
    uart_put_dec(heap_blocks());
    uart_puts(" high_water=");
    uart_put_dec(heap_high_water());
    uart_puts(" check=");
    uart_put_dec((unsigned int)heap_check());
    uart_puts("\n");
}

static void execute(char *line);   /* defined below */

/* Runs one command line from somewhere other than the UART.
 *
 * execute() splits its argument in place, so a caller's string cannot be passed
 * straight through — it would be modified, and a string literal would fault.
 * The copy is also the length check: a line longer than the shell's own buffer
 * is refused rather than truncated into a different command.
 *
 * This is the whole interface the on-screen shell needs. It deliberately does
 * not bypass execute(): a command typed on the panel takes exactly the same
 * path, with the same parsing and the same output, as one typed over serial. */
void shell_run_line(const char *line)
{
    static char buf[LINE_MAX];
    uint32_t i = 0;

    while (line[i] && i < LINE_MAX - 1u) {
        buf[i] = line[i];
        i++;
    }
    if (line[i]) {
        uart_puts("   line too long\n");
        return;
    }
    buf[i] = 0;
    execute(buf);
}

static void execute(char *line)
{
    while (*line == ' ') {
        line++;
    }
    if (!*line) {
        return;
    }

    char *arg = split(line);

    /* One command, one uninterrupted response. */
    console_lock();

    if (str_eq(line, "help"))       { cmd_help(); }
    else if (str_eq(line, "ps"))    { cmd_ps(); }
    else if (str_eq(line, "progs")) { cmd_progs(); }
    else if (str_eq(line, "mem"))   { cmd_mem(); }
    else if (str_eq(line, "adc"))   { cmd_adc(); }
    else if (str_eq(line, "adcprobe")) { adc_probe_sensor_mux(); }
    else if (str_eq(line, "adcconv")) { adc_probe_convert(); }
    else if (str_eq(line, "adcdrive")) { adc_probe_driven(); }
    else if (str_eq(line, "ldr")) { adc_watch(ADC1_CH_LDR, 120u); }
    else if (str_eq(line, "ldrscan")) { adc_watch_all(40u); }
    else if (str_eq(line, "i2c")) { i2c_selftest(); i2c_scan(); }
    else if (str_eq(line, "tone")) {
        int hz = parse_int(arg);
        if (hz < 0) {
            uart_puts("   tone <hz>, or 'tone 0' to stop\n");
        } else {
            audio_tone((uint32_t)hz);
            uart_puts(hz ? "   sounding\n" : "   off\n");
        }
    }
    else if (str_eq(line, "findspk")) { audio_find_speaker(); }
    else if (str_eq(line, "spktest")) { audio_probe_square(); }
    else if (str_eq(line, "audio")) { audio_dump(); }
    else if (str_eq(line, "bytes")) {
        /* Raw byte counter. Sampled twice from the host with a known wall-clock
         * gap between, this gives true panel throughput without trusting any
         * on-device clock -- which is the assumption currently under suspicion. */
        uart_puts("   bytes=");
        uart_put_dec(display_bytes_written());
        uart_puts("  dma_on=");
        uart_put_dec((unsigned int)display_dma_enabled());
        uart_puts(" xfers=");
        uart_put_dec(display_dma_transfers());
        uart_puts(" timeouts=");
        uart_put_dec(display_dma_timeouts());
        uart_puts("\n   spi clk: init=");
        uart_put_hex(display_spi_clock_reg());
        uart_puts("  live=");
        uart_put_hex(display_spi_clock_live());
        uart_puts("\n   ccount=");
        uart_put_dec(xt_ccount());
        uart_puts("\n");
    }
    else if (str_eq(line, "fillbench")) {
        display_reset_profile();
        uint32_t x0 = display_dma_transfers();
        uint32_t us = display_fill_bench();
        uart_puts("   dma transfers this fill: ");
        uart_put_dec(display_dma_transfers() - x0);
        uart_puts("  (320 spans of 480 B expected)\n   spi_tx branches: dma=");
        uart_put_dec(display_took_dma());
        uart_puts(" fifo=");
        uart_put_dec(display_took_fifo());
        uart_puts("\n");
        uart_puts("   of which: bus busy ");
        uart_put_dec(display_wait_us() / 1000u);
        uart_puts(" ms, dma setup ");
        uart_put_dec(display_setup_us() / 1000u);
        uart_puts(" ms\n");
        uart_puts("   full-screen fill: ");
        uart_put_dec(us / 1000u);
        uart_puts(" ms   (init measured 43 ms single-threaded)\n");
        uint32_t m = display_fill_bench_masked();
        uart_puts("   with interrupts masked: ");
        uart_put_dec(m / 1000u);
        uart_puts(" ms\n");
        desktop_invalidate();
    }
    else if (str_eq(line, "spkhold")) {
        int pin = parse_int(arg);
        uint32_t mux = 0x3FF49028u;             /* gpio26 */
        if (pin == 25) { mux = 0x3FF49024u; }
        else if (pin == 4) { mux = 0x3FF49048u; }
        else if (pin == 16) { mux = 0x3FF4904Cu; }
        else if (pin == 17) { mux = 0x3FF49050u; }
        else if (pin != 26) { pin = 26; }
        uart_puts("   warbling on gpio ");
        uart_put_dec((unsigned int)pin);
        uart_puts(" for 15 s - put the speaker to your ear\n");
        audio_hold((uint32_t)pin, mux, 15u);
        uart_puts("   done\n");
    }
    else if (str_eq(line, "beep")) {
        audio_beep(3000u, 20u);
        uart_puts("   beep\n");
    }
    else if (str_eq(line, "hang")) {
        /* Deliberately wedge the system to prove the hang detector works.
         *
         * An armed watchdog that has never been observed to fire is worse than
         * none: it is confidence without evidence. This masks interrupts and
         * spins, which stops the tick, stops the scheduler, and therefore stops
         * the distinct-task switches the watchdog feeds on. Recovery is the
         * watchdog resetting the board, and nothing else. */
        uart_puts("   wedging the kernel; the watchdog should reset in ~3 s\n");
        crit_enter();
        for (;;) {
        }
    }
    else if (str_eq(line, "fault")) {
        /* Take a real exception, to exercise the panic path deliberately.
         *
         * `hang` proved the watchdog fires; this proves what happens when the
         * kernel stops on purpose rather than by accident. The two interact:
         * a halted panic and a hung kernel look identical to a detector that
         * watches for task switches, so the panic path disarms the watchdog
         * before spinning. Without an easy way to trigger a fault, that
         * interaction stays theoretical. */
        uart_puts("   executing an illegal instruction\n");
        __asm__ volatile ("ill");
    }
    else if (str_eq(line, "smash")) {
        uart_puts("   clobbering this task's guard word; the next switch should panic\n");
        task_smash_guard();
        for (;;) {
        }
    }
    else if (str_eq(line, "stacks")) {
        uart_puts("   id  name        free B  of 2048\n");
        for (int id = 0; id < TASK_MAX; id++) {
            if (!task_exists(id)) {
                continue;
            }
            uart_puts("   ");
            uart_put_dec((unsigned int)id);
            uart_puts("   ");
            uart_puts(task_name(id));
            uart_puts("   ");
            uart_put_dec(task_stack_headroom(id) * 4u);
            uart_puts("\n");
        }
    }
    else if (str_eq(line, "3d")) {
        int on = !str_eq(arg, "off");
        desktop_set_active(!on);
        uart_puts(on ? "   3D view\n" : "   launcher\n");
    }
    else if (str_eq(line, "calshow")) {
        calib_report();
    }
    else if (str_eq(line, "intr")) {
        /* The interrupt matrix, reported rather than printed from the handler.
         * An ISR that writes to the UART changes the timing of the thing it is
         * measuring, and console_lock() inside a level-3 handler would deadlock
         * against a task holding it. */
        uart_puts("   tick  line ");
        uart_put_dec(INTR_LINE_TIMER1);
        uart_puts("  serviced ");
        uart_put_dec(intr_count(INTR_LINE_TIMER1));
        uart_puts("\n   gpio  line ");
        uart_put_dec(INTR_LINE_GPIO);
        uart_puts("  serviced ");
        uart_put_dec(intr_count(INTR_LINE_GPIO));
        uart_puts("\n   penirq edges ");
        uart_put_dec(touch_irq_fires());
        uart_puts("  wakes ");
        uart_put_dec(touch_irq_wakes());
        uart_puts("  waits ");
        uart_put_dec(touch_irq_waits());
        uart_puts("\n   registered as ");
        uart_put_dec((unsigned int)touch_irq_last_reg());
        uart_puts("  armed rb ");
        uart_put_hex(touch_irq_armed_rb());
        uart_puts("  isr saw ");
        uart_put_dec((unsigned int)touch_irq_seen());
        uart_puts("\n   spurious     ");
        uart_put_dec(intr_spurious());
        uart_puts("  masked mask ");
        uart_put_hex(intr_disabled_mask());
        uart_puts("\n");
        if (intr_spurious()) {
            uart_puts("   a line fired with no handler and was masked to stop a hang\n");
        }
        intr_dump();
    }
    else if (str_eq(line, "irqtest")) {
        intr_selftest();
    }
    else if (str_eq(line, "irqpoke")) {
        intr_poke();
    }
    else if (str_eq(line, "cal")) {
        uart_puts("   tap the centre of each cross; four of them\n");
        calib_start();
    }
    else if (str_eq(line, "taps")) {
        /* Dumps the press log. Exists because the same data printed live is
         * unreadable: it appears while a finger is on the glass, which is
         * precisely when nobody has a capture attached. */
        uint32_t n = touch_log_count();
        if (n == 0u) {
            uart_puts("   no presses recorded (tap the screen, then run 'taps')\n");
        } else {
            uart_puts("   #   raw_x  raw_y    x    y    z\n");
            for (uint32_t i = 0; i < n; i++) {
                const touch_log_t *e = touch_log_entry(i);
                uart_puts("   ");
                uart_put_dec(i + 1u);
                uart_puts("   ");
                uart_put_dec(e->raw_x);
                uart_puts("   ");
                uart_put_dec(e->raw_y);
                uart_puts("   ");
                uart_put_dec(e->x);
                uart_puts("   ");
                uart_put_dec(e->y);
                uart_puts("   ");
                uart_put_dec(e->z);
                uart_puts("\n");
            }
        }
    }
    else if (str_eq(line, "tapsclear")) {
        touch_log_clear();
        uart_puts("   press log cleared\n");
    }
    else if (str_eq(line, "sd")) {
        int rc = sd_init();
        uart_puts("   sd_init ");
        uart_puts(rc == 0 ? "OK" : "FAILED");
        uart_puts("  type=");
        uart_puts(sd_type() == SD_TYPE_SDHC ? "SDHC" :
                  sd_type() == SD_TYPE_SDSC ? "SDSC" : "none");
        uart_puts("  last R1=");
        uart_put_hex(sd_last_r1());
        uart_puts("\n");
        if (rc != 0) {
            /* Name the stage that failed. "It did not work" is not a diagnosis,
             * and the stage says whether to suspect an empty slot, the wiring,
             * or the card itself. */
            uart_puts("   stage: ");
            if (rc == SD_ERR_IDLE) {
                uart_puts("CMD0 - no card, or MISO/CS/SCK wrong\n");
            } else if (rc == SD_ERR_IFCOND) {
                uart_puts("CMD8 - pre-2.0 card, or a noisy bus\n");
            } else if (rc == SD_ERR_READY) {
                uart_puts("ACMD41 - card never finished initialising\n");
            } else if (rc == SD_ERR_OCR) {
                uart_puts("CMD58 - addressing mode unknown\n");
            } else if (rc == SD_ERR_BLOCKLEN) {
                uart_puts("CMD16 - block length refused\n");
            } else {
                uart_puts("read\n");
            }
        }
    }
    else if (str_eq(line, "sdread")) {
        int lba = parse_int(arg);
        if (lba < 0) {
            uart_puts("   usage: sdread <lba>\n");
        } else {
            static uint8_t blk[SD_BLOCK_SIZE];
            int rc = sd_read_block((uint32_t)lba, blk);
            if (rc != 0) {
                uart_puts("   read failed, R1=");
                uart_put_hex(sd_last_r1());
                uart_puts("\n");
            } else {
                for (uint32_t r = 0; r < 4u; r++) {
                    uart_puts("   ");
                    for (uint32_t c = 0; c < 16u; c++) {
                        uart_put_hex(blk[r * 16u + c]);
                        uart_putc(' ');
                    }
                    uart_puts("\n");
                }
                /* A FAT-formatted card's block 0 ends 0x55 0xAA. Checking that
                 * verifies the read without depending on what the user chose
                 * to put on the card. */
                uart_puts("   signature=");
                uart_put_hex(blk[510]);
                uart_putc(' ');
                uart_put_hex(blk[511]);
                uart_puts((blk[510] == 0x55u && blk[511] == 0xAAu)
                          ? "  (valid boot sector)\n" : "  (NOT a boot sector)\n");

                /* Decode the MBR partition table.
                 *
                 * This is also the only test that proves the ADDRESSING MODE is
                 * right. Block 0 is byte 0 on a byte-addressed card and block 0
                 * on a block-addressed one, so reading it succeeds either way
                 * and discriminates nothing. A partition starting thousands of
                 * blocks in does discriminate: get the mode wrong and the read
                 * lands 512 times too far into the card, returning something
                 * that is not a filesystem header. */
                if (lba == 0 && blk[510] == 0x55u && blk[511] == 0xAAu) {
                    for (uint32_t p = 0; p < 4u; p++) {
                        const uint8_t *e = &blk[446u + p * 16u];
                        if (e[4] == 0u) {
                            continue;               /* unused entry */
                        }
                        uint32_t start = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                                         ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                        uint32_t count = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                                         ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
                        uart_puts("   partition ");
                        uart_put_dec(p);
                        uart_puts(": type=");
                        uart_put_hex(e[4]);
                        uart_puts(" start=");
                        uart_put_dec(start);
                        uart_puts(" sectors=");
                        uart_put_dec(count);
                        uart_puts("  -> try 'sdread ");
                        uart_put_dec(start);
                        uart_puts("'\n");
                    }
                }

                /* A FAT volume boot record names its type in ASCII, at 0x36 for
                 * FAT12/16 and 0x52 for FAT32. Finding it at a non-zero LBA is
                 * the end-to-end proof: pins, clock, addressing and block
                 * framing all have to be right at once for this to appear. */
                if (lba != 0 && blk[510] == 0x55u && blk[511] == 0xAAu) {
                    uart_puts("   fs type: ");
                    const uint8_t *t = (blk[0x52] == 'F') ? &blk[0x52] : &blk[0x36];
                    for (uint32_t i = 0; i < 8u; i++) {
                        char c = (char)t[i];
                        uart_putc((c >= 32 && c < 127) ? c : '.');
                    }
                    uart_puts("\n");
                }
            }
        }
    }
    else if (str_eq(line, "fb")) {
        if (str_eq(arg, "on") || str_eq(arg, "off")) {
            int want = str_eq(arg, "on");
            if (raycast_set_framebuffer(want) != 0) {
                uart_puts("   cannot allocate framebuffer\n");
            }
        }
        uart_puts("   framebuffer ");
        uart_puts(raycast_framebuffer() ? "on" : "off");
        uart_puts(", ");
        uart_put_dec(raycast_fb_bytes());
        uart_puts(" B, heap free ");
        uart_put_dec(heap_free_bytes());
        uart_puts("\n");
    }
    else if (str_eq(line, "run"))   { cmd_run(arg); }
    else if (str_eq(line, "kill")) {
        int id = parse_int(arg);
        if (id < 0 || id >= APP_MAX) {
            uart_puts("   usage: kill <id>\n");
        } else if (app_state(id) != APP_RUNNING) {
            uart_puts("   not running\n");
        } else {
            app_kill(id);
            uart_puts("   killed ");
            uart_put_dec((unsigned int)id);
            uart_puts(", arena released\n");
        }
    }
    else {
        uart_puts("   unknown command: ");
        uart_puts(line);
        uart_puts("\n");
    }

    console_unlock();
}

void shell_begin(void)
{
    uart_puts("\n  nat-os shell — 'help' for commands\n> ");
}

void shell_poll(void)
{
    int ch;
    while ((ch = uart_getc_nb()) >= 0) {
        if (ch == '\r' || ch == '\n') {
            uart_puts("\n");
            g_line[g_len] = 0;
            execute(g_line);
            g_len = 0;
            uart_puts("> ");
        } else if (ch == 8 || ch == 127) {          /* backspace / delete */
            if (g_len > 0) {
                g_len--;
                uart_puts("\b \b");
            }
        } else if (ch >= 32 && ch < 127 && g_len < LINE_MAX - 1) {
            g_line[g_len++] = (char)ch;
            uart_putc((char)ch);
        }
        /* Anything else — control characters, or input past the line limit —
         * is dropped silently rather than echoed, so a stray paste cannot
         * scroll the console or overrun the buffer. */
    }
}
