/* cyd-os — minimal console shell. See shell.h.
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
#include "uart.h"
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
              "    help          this\n");
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
    uart_puts("\n  cyd-os shell — 'help' for commands\n> ");
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
