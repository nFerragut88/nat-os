/* nat-os — minimal console shell. See shell.h.
 *
 * String handling is hand-rolled: there is no libc, and the three primitives
 * this needs are shorter than the argument for pulling one in.
 */

#include "shell.h"
#include "panic.h"
#include "blob.h"
#include "blobcall.h"
extern uint32_t wincollide_runs(void);
extern uint32_t wincollide_bad(void);
#include "wifi_osi_table.h"
#include "wifi_init_cfg.h"
#include "console.h"
#include "app.h"
#include "heap.h"
#include "raycast.h"
#include "critical.h"
#include "timer.h"
#include "task.h"
#include "store.h"
#include "sd.h"
#include "touch.h"
#include "calib.h"
#include "intr.h"
#include "adc.h"
#include "i2c.h"
#include "audio.h"
#include "desktop.h"
#include "uart.h"
#include "vm.h"
#include "window.h"
#include "phyinit.h"
#include "wifi_osi_impl.h"
#include "wifimac.h"
#include "efuse.h"
#include "xtensa.h"
#include "vmarg.h"
#include "gpio.h"      /* GPIO_REG, for spidump */
#include "spi3.h"
#include "board.h"
#include "device.h"
#include "term.h"

uint32_t osi_add_probe(uint32_t a, uint32_t b);
uint32_t osi_add_probe(uint32_t a, uint32_t b) { return a + b; }

#define LINE_MAX 64

static const shell_program_t *g_progs;
static int      g_prog_count;
static char     g_line[LINE_MAX];
static uint32_t g_len;

/* Always refuses. Used only by `storetest` to drive the deferral bound to its
 * limit -- the case the bound exists for, and the one that cannot be produced
 * on demand by anything real. */
static int storetest_refuse(void) { return 0; }

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
/* Full 32-bit hex, for register addresses and values. parse_int is decimal
 * and caps at 9999, which is right for slot numbers and useless for these.
 * Returns 0 on a malformed string and sets *ok, because 0 is a legitimate
 * register value and cannot double as the error. */
static uint32_t parse_hex(const char *s, int *ok)
{
    uint32_t v = 0;
    int n = 0;
    *ok = 0;
    if (s[0] == (char)48 && (s[1] == (char)120 || s[1] == (char)88)) {
        s += 2;                       /* skip an 0x prefix */
    }
    while (*s) {
        uint32_t d;
        if      (*s >= (char)48 && *s <= (char)57)  { d = (uint32_t)(*s - 48); }
        else if (*s >= (char)97 && *s <= (char)102) { d = (uint32_t)(*s - 97) + 10u; }
        else if (*s >= (char)65 && *s <= (char)70)  { d = (uint32_t)(*s - 65) + 10u; }
        else { return 0; }
        v = (v << 4) | d;
        if (++n > 8) { return 0; }
        s++;
    }
    if (!n) { return 0; }
    *ok = 1;
    return v;
}

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

/* The `hog` command's task. Created on first use rather than at boot, because a
 * task that exists only to compete for the CPU should not be in the table of a
 * system nobody is debugging. See the command for what it is testing.
 *
 * The shape is copied from task_apps deliberately, down to the yield: spin while
 * armed, yield when not, so a disarmed hog costs what an idle apps task costs
 * and the two states are comparable. */
static volatile int g_hog_on;
static volatile int g_hog_draw;         /* also issue gfxrogue's fill */
static volatile uint32_t g_hog_w = APP_VIEW_W;   /* 180 -> DMA; <=32 -> FIFO */
static int g_hog_task = -1;
static volatile uint32_t g_hog_spins;
static volatile uint32_t g_hog_fills;

static void hog_task(void)
{
    for (;;) {
        if (g_hog_on) {
            /* Roughly the work app_tick(2000) does, without doing any of it.
             * The count is volatile so the loop cannot be optimised away. */
            for (int i = 0; i < 2000; i++) {
                g_hog_spins++;
            }

            /* `hog draw` adds the one thing the earlier tests between them
             * never reproduced: drawing CONTINUOUSLY.
             *
             * stripn issued 200 fills as a burst and stopped; hog spun forever
             * and drew nothing. gfxrogue does both at once, and the difference
             * is not cosmetic. The raycaster holds the draw lock essentially
             * without interruption -- 7,965 ms held out of 7,860 ms of uptime,
             * with cont=0 because applications use display_try_lock() and give
             * up rather than wait. So the only moment anything else can draw is
             * the sliver between one blit and the next, and a burst simply
             * loses that race a few hundred times and quits.
             *
             * A program that never stops asking gets into that gap over and
             * over, forever. That is the shape being tested here. try_lock
             * rather than a blocking take, so this behaves like an application
             * and cannot stall the renderer by holding the panel. */
            /* Width is the variable, and it selects the TRANSPORT.
             *
             * spi_tx() sends anything over 64 bytes by DMA and anything smaller
             * through the FIFO, so a fill's width decides which path carries
             * it. Every program that repairs the 3D view issues wide fills:
             * gfxrogue and `hog draw` are both 180 px, 360 bytes a row, DMA.
             * The `draw` application does NOT repair it, and its block is 20 px
             * -- 40 bytes a row, FIFO.
             *
             * That is a single-variable difference between a program that fixes
             * the view and one that does not, so it is worth being able to set
             * it: `hog draw 20` is gfxrogue's shape at draw's width. */
            if (g_hog_draw && display_try_lock()) {
                display_fill_rect(0, APP_VIEW_Y0, g_hog_w, APP_VIEW_H,
                                  (g_hog_fills & 1u) ? COLOR_WHITE : COLOR_RED);
                display_unlock();
                g_hog_fills++;
            }
        } else {
            task_yield();
        }
    }
}

/* Splits the line in place at the first space; returns the argument, or "". */
/* One line per eight registers, address first -- the format tools/idf_ref
 * prints, so one host script parses both. */
static void regdump_range(const char *tag, uint32_t base, uint32_t words)
{
    for (uint32_t i = 0; i < words; i += 8u) {
        uart_puts("REG ");
        uart_puts(tag);
        uart_puts(" ");
        uart_put_hex(base + i * 4u);
        for (uint32_t j = 0; j < 8u && (i + j) < words; j++) {
            uart_puts(" ");
            uart_put_hex(*(volatile uint32_t *)(base + (i + j) * 4u));
        }
        uart_puts("\n");
    }
}

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
              "    dev [id ch [v]] device table; read or write a channel\n"
              "    perms [a d on|off] what each application may touch\n"
              "    light [thresh] one light reading, beep if dark\n"
              "    tone <hz>     tone on gpio26; 'tone 0' stops. try 3000, not 440\n"
              "    beep          a short 3 kHz beep\n"
              "    3d [off]      3D view or launcher\n"
              "    taps          dump the touch press log\n"
              "    tapsclear     empty it\n"
              "    hang          wedge the kernel; the watchdog should reset it\n"
              "    wintest [n]   run windowed-ABI code, recursion depth n\n"
              "    vendorcall [n] call a -mabi=windowed compiled object\n"
              "    romcall       call crc32_le in the ESP32 ROM\n"
              "    ositest       exercise the WiFi OSI table end to end\n"
              "    macinit       bring up the WiFi MAC (needs phyinit first)\n"
              "    maclive       which MAC registers move on their own\n"
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

/* Start a program and grant it exactly what its table entry declares.
 *
 * Every launch path goes through here, because a path that started a program
 * WITHOUT granting would produce one that silently cannot reach hardware, and a
 * path that granted the wrong entry would hand it somebody else's capabilities.
 * Both are quiet failures, and the second is the more dangerous. */
static int launch_entry(const shell_program_t *p)
{
    int id = app_start(p->name, p->img, p->len, p->arena_bytes, p->publish_off);
    if (id >= 0) {
        device_grant((uint32_t)id, p->perms);
    }
    return id;
}

int shell_launch(const char *name)
{
    for (int i = 0; i < g_prog_count; i++) {
        if (str_eq(g_progs[i].name, name)) {
            return launch_entry(&g_progs[i]);
        }
    }
    return -1;
}

static void cmd_run(const char *name)
{
    for (int i = 0; i < g_prog_count; i++) {
        if (str_eq(g_progs[i].name, name)) {
            int id = launch_entry(&g_progs[i]);
            if (id < 0) {
                uart_puts("   cannot start: no free slot or no memory\n");
            } else {
                uart_puts("   started id=");
                uart_put_dec((unsigned int)id);
                uart_puts(g_progs[i].perms ? " perms=" : " perms=none\n");
                if (g_progs[i].perms) {
                    uart_put_hex(g_progs[i].perms);
                    uart_puts("\n");
                }
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
    else if (str_eq(line, "spkhold")) {
        int pin = parse_int(arg);
        /* This used to carry its own pin-to-IO_MUX chain -- a THIRD copy of the
         * same table, after audio.c's and the per-driver constants. All gone;
         * gpio_io_mux() derives it, and a pin it does not know is refused
         * rather than silently turned into GPIO26. */
        if (pin < 0 || !gpio_io_mux((uint32_t)pin)) { pin = 26; }
        uart_puts("   warbling on gpio ");
        uart_put_dec((unsigned int)pin);
        uart_puts(" for 15 s - put the speaker to your ear\n");
        audio_hold((uint32_t)pin, 15u);
        uart_puts("   done\n");
    }
    else if (str_eq(line, "beep")) {
        /* Through the DEVICE TABLE, not straight to audio_beep().
         *
         * This used to call the driver directly, which made it a diagnostic for
         * a path no application can take -- and a self-test that exercises a
         * different route from the real one is how a broken system passes its
         * own checks. Now it is `dev 1 0 <packed>` with the packing done for
         * you, so using it also tests the model.
         *
         * Defaults are the old fixed values, so `beep` on its own still means
         * what it always meant. */
        char *chz = arg;
        char *ctk = split(chz);
        int hz = *chz ? parse_int(chz) : 3000;
        int tk = *ctk ? parse_int(ctk) : 20;
        if (hz <= 0 || tk <= 0) {
            uart_puts("   usage: beep [hz] [ticks]\n");
        } else {
            uint32_t packed = ((uint32_t)hz << 16) | ((uint32_t)tk & 0xFFFFu);
            uart_puts(device_write(DEVICE_CALLER_KERNEL, 1u, 0u, packed)
                      ? "   beep\n" : "   refused\n");
        }
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
    else if (str_eq(line, "blobphy")) {
        /* Initialise the PHY *inside the blob*, then report.
         *
         * Not the kernel's copy. Until now a -WiFi build linked libphy into
         * IRAM and calibrated that; the blob carries its own, with its own
         * .bss, so initialising one and transmitting through the other would
         * present as a PHY that reported success and a radio that stayed
         * silent -- the exact shape of failure this project has already spent
         * sessions on.
         *
         * UM-NATOS-036 records that this call panics with StoreProhibited
         * inside phy_enter_critical under OUR bootloader, which is why -WiFi
         * forces Espressif's. This is the same call against a different copy
         * of the same code, from a blob-free kernel. Whether that changes
         * anything is exactly the open question. */
        const struct blob_entry *e = blob_map();
        if (!e) {
            uart_puts("   no valid image -- run build_blob.ps1 -Flash\n");
        } else if (blob_init(e) != 0) {
            uart_puts("   loader refused; not calling anything\n");
        } else {
            uart_puts("   blob loaded. calling its register_chipv7_phy at ");
            uart_put_hex(e->phy_init);
            uart_puts("\n");
            int r = phyinit_run_at(e->phy_init);
            uart_puts("   phyinit   rc=");
            uart_put_dec((unsigned int)r);
            uart_puts("  result=");
            uart_put_hex(phyinit_result());
            uart_puts("\n   phystack  ");
            uart_put_dec(phy_stack_used());
            uart_puts(" of ");
            uart_put_dec(phy_stack_size());
            uart_puts(" bytes used\n");
            uart_puts("   IT RETURNED. that is not evidence the radio works.\n");
        }
    }
    else if (str_eq(line, "blobtx")) {
        /* next_moves/08 step 5: actually transmit through the vendor path.
         *
         * Maps and loads the blob, initialises ITS PHY, then hands a beacon
         * frame to esp_wifi_80211_tx.
         *
         * Expected to fail, and the failure is the information. esp_wifi_
         * 80211_tx is reached through libpp and the OS adapter table, and it
         * normally runs after esp_wifi_init()/esp_wifi_start() have built
         * queues, timers and an interface. None of that has happened here, and
         * net80211_host.c's event stubs accept registrations and never call
         * back. A live PHY is necessary and is nowhere near sufficient.
         *
         * The frame is a beacon with a distinctive SSID so that if anything
         * does reach the air, a laptop scan can name it rather than leaving us
         * to argue about RF plots. */
        static uint8_t frame[64];
        uint8_t mac[6];
        efuse_factory_mac(mac);

        uint32_t n = 0;
        frame[n++] = 0x80; frame[n++] = 0x00;          /* beacon            */
        frame[n++] = 0x00; frame[n++] = 0x00;          /* duration          */
        for (int i = 0; i < 6; i++) { frame[n++] = 0xFF; }   /* addr1 bcast */
        for (int i = 0; i < 6; i++) { frame[n++] = mac[i]; } /* addr2 = us  */
        for (int i = 0; i < 6; i++) { frame[n++] = mac[i]; } /* addr3 bssid */
        frame[n++] = 0x00; frame[n++] = 0x00;          /* seq/frag          */
        for (int i = 0; i < 8; i++) { frame[n++] = 0x00; }   /* timestamp   */
        frame[n++] = 0x64; frame[n++] = 0x00;          /* interval 100 TU   */
        frame[n++] = 0x01; frame[n++] = 0x00;          /* capability: ESS   */
        frame[n++] = 0x00; frame[n++] = 0x09;          /* SSID IE, 9 bytes  */
        frame[n++] = 'N'; frame[n++] = 'A'; frame[n++] = 'T';
        frame[n++] = 'O'; frame[n++] = 'S'; frame[n++] = '-';
        frame[n++] = 'B'; frame[n++] = 'L'; frame[n++] = 'B';
        frame[n++] = 0x01; frame[n++] = 0x04;          /* rates IE          */
        frame[n++] = 0x82; frame[n++] = 0x84;
        frame[n++] = 0x8B; frame[n++] = 0x96;
        frame[n++] = 0x03; frame[n++] = 0x01; frame[n++] = 0x06; /* DS ch 6 */

        const struct blob_entry *e = blob_map();
        if (!e) {
            uart_puts("   no valid image\n");
        } else if (blob_init(e) != 0) {
            uart_puts("   loader refused\n");
        } else {
            int prc = phyinit_run_at(e->phy_init);
            uart_puts("   phyinit   rc=");
            uart_put_dec((unsigned int)(prc < 0 ? -prc : prc));
            uart_puts(prc == 0 ? " (or already done this boot)\n" : "\n");

            uart_puts("   frame     ");
            uart_put_dec(n);
            uart_puts(" bytes, beacon, ssid NATOS-BLB, channel 6\n");
            /* `blobtx force` installs the OS adapter table directly into the
             * blob's own g_osi_funcs_p (0x3ffd4e08) rather than waiting for
             * esp_wifi_init_internal to do it.
             *
             * AFTER blob_init, not before: blob_init copies .data over the
             * blob's DRAM, so an earlier poke is simply overwritten -- which
             * is what the first attempt did.
             *
             * The question it asks is narrow: is the adapter table all that
             * transmit needs, or does it need the rest of a started driver? */
            if (str_eq(arg, "force")) {
                *(volatile uint32_t *)0x3ffd4e08u = (uint32_t)wifi_osi_table();
                uart_puts("   forced g_osi_funcs_p = ");
                uart_put_hex((uint32_t)wifi_osi_table());
                uart_puts("\n");
            }
            uart_puts("   calling esp_wifi_80211_tx at ");
            uart_put_hex(e->wifi_80211_tx);
            uart_puts("\n");

            uint32_t r = phy_stack_call(e->wifi_80211_tx, 0u,
                                        (uint32_t)frame, n, 0u);
            uart_puts("   tx        returned ");
            uart_put_hex(r);
            uart_puts("\n   phystack  ");
            uart_put_dec(phy_stack_used());
            uart_puts(" of ");
            uart_put_dec(phy_stack_size());
            uart_puts(" bytes used\n");
            uart_puts("   RETURNING IS NOT RADIATING. scan for NATOS-BLB.\n");
        }
    }
    else if (str_eq(line, "osi") || str_eq(line, "osiused")) {
        /* next_moves/08. Hand the OS adapter table to the blob, then report
         * which of its entries the driver actually reaches.
         *
         * Every entry is an instrumented stub (kernel/wifi_osi_table.c), so
         * this is a measurement rather than a guess: the driver names its own
         * requirements, in call order, and the entries that never appear are
         * ones nobody has to write. */
        if (str_eq(line, "osiused")) {
            uart_puts("   entries called so far (order, count):\n");
            uint32_t shown = 0;
            for (uint32_t i = 0; i < wifi_osi_entries(); i++) {
                if (wifi_osi_calls(i) == 0u) { continue; }
                uart_puts("     ");
                uart_put_dec(wifi_osi_order(i));
                uart_puts("  ");
                uart_puts(wifi_osi_name(i));
                uart_puts("  x");
                uart_put_dec(wifi_osi_calls(i));
                uart_puts("\n");
                shown++;
            }
            uart_puts("   blob_call: ");
            uart_put_dec(blob_call_count());
            uart_puts(" entries, contended ");
            uart_put_dec(blob_call_contended());
            uart_puts("  (scheduler stayed live)\n");
            uart_puts("   intr clamped to CRIT_LEVEL: ");
            uart_put_dec(wifi_osi_intr_clamped());
            uart_puts("   (must be 0)\n");
            if (!shown) { uart_puts("     none -- nothing has called in yet\n"); }
            else {
                uart_puts("   ");
                uart_put_dec(shown);
                uart_puts(" of ");
                uart_put_dec(wifi_osi_entries());
                uart_puts(" entries used\n");
            }
        } else {
            const struct blob_entry *e = blob_map();
            if (!e) {
                uart_puts("   no valid image\n");
            } else if (blob_init(e) != 0) {
                uart_puts("   loader refused\n");
            } else {
                int prc = phyinit_run_at(e->phy_init);
                uart_puts("   phyinit   rc=");
                uart_put_dec((unsigned int)(prc < 0 ? -prc : prc));
                uart_puts("\n   table     ");
                uart_put_dec(wifi_osi_entries());
                uart_puts(" entries, version 8, magic 0xdeadbeaf\n");
                uart_puts("   registering at ");
                uart_put_hex(e->osi_register);
                uart_puts("\n");

                /* `osi null` registers a NULL table on purpose.
                 *
                 * wifi_osi_funcs_register validates version and magic, so NULL
                 * MUST be rejected. If it returns 0 for both, the argument is
                 * not reaching the blob at all -- which would explain why
                 * esp_wifi_init_internal behaves identically for a real config
                 * and a NULL one, and would move the fault from the config to
                 * phy_stack_call. */
                uint32_t r = phy_stack_call(e->osi_register,
                                            str_eq(arg, "null") ? 0u
                                                : (uint32_t)wifi_osi_table(),
                                            0u, 0u, 0u);
                uart_puts("   osi_reg   returned ");
                uart_put_hex(r);
                uart_puts("\n");
                /* NOT "accepted - version and magic matched". That wording
                 * was invented here and then cited as evidence the table
                 * layout was right. `osi null` returns 0 as well, so this
                 * function does not validate what it is given -- or at least
                 * not that. A return of 0 means only that it returned 0. */
                uart_puts(r == 0u ? "   returned ESP_OK (it also returns ESP_OK for NULL,\n"
                                    "     so this does NOT confirm the table)\n"
                                  : "   non-zero: the blob refused something\n");
                uart_puts("   run 'osiused' to see what it called.\n");
            }
        }
    }
    else if (str_eq(line, "wifiinit")) {
        /* next_moves/08. esp_wifi_init_internal() -- the step that installs
         * the OS adapter table.
         *
         * The table is delivered through cfg->osi_funcs, NOT by
         * wifi_osi_funcs_register(), which validates one but does not install
         * it. That is why transmit kept faulting on a null pointer at offset
         * 0x54 with a perfectly good table already registered.
         *
         * This is also the first call that should make the instrumented stubs
         * fire. Run 'osiused' afterwards: whatever it names is what actually
         * needs a real body, and whatever it does not name is work nobody has
         * to do. */
        const struct blob_entry *e = blob_map();
        if (!e) {
            uart_puts("   no valid image\n");
        } else if (blob_init(e) != 0) {
            uart_puts("   loader refused\n");
        } else {
            int prc = phyinit_run_at(e->phy_init);
            uart_puts("   phyinit   rc=");
            uart_put_dec((unsigned int)(prc < 0 ? -prc : prc));
            uart_puts("\n   config    ");
            uart_put_dec(wifi_init_cfg_size());
            uart_puts(" bytes, magic 0x1f2f3f4f\n   osi_funcs -> ");
            /* What the config ACTUALLY carries, not what a helper returns.
             * The old print showed wifi_osi_table() regardless of what was
             * handed over, which is how a stale reading survives a change. */
            {
                extern char g_wifi_osi_funcs;
                const uint32_t *cfg = (const uint32_t *)wifi_init_cfg();
                uart_put_hex(cfg[0]);
                uart_puts("   (counting table ");
                uart_put_hex((uint32_t)wifi_osi_table());
                uart_puts(", real table ");
                uart_put_hex((uint32_t)&g_wifi_osi_funcs);
                uart_puts(")");
            }
            uart_puts("\n   calling esp_wifi_init_internal at ");
            uart_put_hex(e->wifi_init);
            uart_puts("\n");

            /* `wifiinit null` passes a NULL config on purpose.
             *
             * The disassembly shows exactly one site returning 0x102 -- the
             * config-is-NULL path -- but that path returns before registering
             * the OS adapter, so it cannot explain 8 adapter calls. objdump
             * has already been caught losing instruction sync in this blob, so
             * "only one site" is worth only as much as the decode. Comparing
             * the two runs answers it without trusting either. */
            /* `wifiinit nvs` flips nvs_enable to IDF's shipped value.
             *
             * It is the one field that unambiguously diverges: nat-os has no
             * NVS at all, so it was set to 0 on the reasoning that the driver
             * should not reach for a key/value store that does not exist. If
             * the driver validates it rather than merely honouring it, that is
             * a config rejection with a one-field cause. */
            int want_null = str_eq(arg, "null");
            /* `wifiinit task` opts into blob task creation, which currently
             * panics -- two contexts in windowed code. Kept reachable because
             * it is the reproducer for the next piece of work. */
            blob_task_enable(str_eq(arg, "task"));
            /* [step 252] 'startnoie' is 'start' with the RSN IE suppressed:
             * the A/B that says whether the association request goes out at
             * all. One variable, and the only one. */
            {
                extern void wifi_rsn_ie_enable(int on);
                extern void wifi_rsn_akm_set(unsigned int t);
                int noie = str_eq(arg, "startnoie");
                /* [step 254] 'start8021x' keeps the element well formed and
                 * changes ONE BYTE of it to an AKM this access point must
                 * refuse. A status code proves the element is being parsed;
                 * silence proves it is not. */
                int x8021 = str_eq(arg, "start8021x");
                wifi_start_enable(str_eq(arg, "start") || noie || x8021);
                wifi_rsn_ie_enable(!noie);
                wifi_rsn_akm_set(x8021 ? 1u : 2u);
            }
            if (str_eq(arg, "nvs")) { wifi_init_cfg_nvs(1); }
            /* blob_call, not phy_stack_call: the driver has reached
             * _task_create_pinned_to_core, and a task created inside a masked
             * call can never run. Exclusion moves to a mutex; the scheduler
             * keeps running. */
            uint32_t r = wifi_bringup(e, want_null);
            uart_puts("   init      returned ");
            uart_put_hex(r);
            uart_puts(r == 0u ? "  (ESP_OK)\n" : "  (an esp_err_t, not OK)\n");

            uint32_t used = 0;
            for (uint32_t i = 0; i < wifi_osi_entries(); i++) {
                if (wifi_osi_calls(i)) { used++; }
            }
            {
                extern uint32_t g_osi_last, g_osi_hits;
                uart_puts("   real osi forwarded calls: ");
                uart_put_dec(g_osi_hits);
                uart_puts("   last impl at ");
                uart_put_hex(g_osi_last);
                uart_puts("\n");
            }
            uart_puts("   osi       ");
            uart_put_dec(used);
            uart_puts(" of ");
            uart_put_dec(wifi_osi_entries());
            uart_puts(" adapter entries were called\n");
            {
                uint32_t cl = wifi_osi_intr_clamped();
                uart_puts("   intr clamped to CRIT_LEVEL: ");
                uart_put_dec(cl);
                uart_puts(cl ? "  *** the blob wants an interrupt that would\n"
                               "       fire mid-erase; see UM-NATOS-038 7.2\n"
                             : "  (must stay zero)\n");
            }
            uart_puts("   sequence: ");
            for (uint32_t k = 0; k < wifi_osi_trace_len(); k++) {
                uart_puts(wifi_osi_name(wifi_osi_trace_idx(k)));
                if (wifi_osi_trace_arg(k)) {
                    uart_puts("(");
                    uart_put_dec(wifi_osi_trace_arg(k));
                    uart_puts(")");
                }
                uart_puts(k + 1u < wifi_osi_trace_len() ? " -> " : "\n");
            }
            uart_puts("   run 'osiused' for the list, in call order.\n");
        }
    }
    else if (str_eq(line, "osiclamp")) {
        /* Exercise the interrupt-priority clamp.
         *
         * The counter reads 0 today only because _set_intr has never been
         * reached -- init fails before interrupt setup. A tripwire nobody has
         * seen trip is untested code, and this project has a standing rule
         * about mechanisms with no exerciser.
         *
         * _set_intr is entry 2 of the table, so byte offset 8. It is WINDOWED,
         * so the call goes out through rom_call4 -- the call0 -> windowed
         * bridge for a four-argument function -- not called directly. */
        const uint32_t *tbl = (const uint32_t *)wifi_osi_table();
        uint32_t fn = tbl[2];
        uint32_t before = wifi_osi_intr_clamped();

        uart_puts("   asking _set_intr at ");
        uart_put_hex(fn);
        uart_puts(" for priority 7 (above CRIT_LEVEL 3)\n");
        (void)rom_call4(fn, 0u, 0u, 0u, 7u);
        uint32_t hi = wifi_osi_intr_clamped();

        uart_puts("   then for priority 2 (at or below, must NOT clamp)\n");
        (void)rom_call4(fn, 0u, 0u, 0u, 2u);
        uint32_t lo = wifi_osi_intr_clamped();

        uart_puts("   clamped count: ");
        uart_put_dec(before);
        uart_puts(" -> ");
        uart_put_dec(hi);
        uart_puts(" -> ");
        uart_put_dec(lo);
        uart_puts("\n");
        uart_puts(((hi == before + 1u) && (lo == hi))
                  ? "   PASS - clamps above CRIT_LEVEL, leaves the rest alone\n"
                  : "   FAIL - the clamp does not behave as documented\n");
    }
    else if (str_eq(line, "argtest")) {
        /* Does phy_stack_call actually deliver its arguments?
         *
         * next_moves/08 step 9 turns on this, and it cannot be settled with a
         * vendor function whose validation is being inferred -- that is how
         * "accepted, version and magic matched" got invented.
         *
         * crc32_le is in ROM at a fixed address, windowed, PURE, and its
         * result depends on every argument. rom_call3 is the control: it is a
         * bridge already known to work, so if both fail the fault is
         * elsewhere, and if only phy_stack_call fails it is the bridge. */
        static const char m1[] = "nat-os";
        static const char m2[] = "nat-os-xyz";

        uint32_t a1 = rom_call3(ESP_ROM_CRC32_LE, 0u, (uint32_t)m1, 6u);
        uint32_t b1 = phy_stack_call(ESP_ROM_CRC32_LE, 0u, (uint32_t)m1, 6u, 0u);
        uint32_t a2 = rom_call3(ESP_ROM_CRC32_LE, 0u, (uint32_t)m2, 10u);
        uint32_t b2 = phy_stack_call(ESP_ROM_CRC32_LE, 0u, (uint32_t)m2, 10u, 0u);

        uart_puts("   rom_call3       6B=");
        uart_put_hex(a1);
        uart_puts("  10B=");
        uart_put_hex(a2);
        uart_puts("\n   phy_stack_call  6B=");
        uart_put_hex(b1);
        uart_puts("  10B=");
        uart_put_hex(b2);
        uart_puts("\n");
        uart_puts((a1 != a2) ? "   rom_call3      : args distinguish inputs\n"
                             : "   rom_call3      : SAME for both -- args lost\n");
        uart_puts((b1 != b2) ? "   phy_stack_call : args distinguish inputs\n"
                             : "   phy_stack_call : SAME for both -- ARGS LOST\n");
        uart_puts((a1 == b1 && a2 == b2)
                  ? "   both bridges agree\n"
                  : "   BRIDGES DISAGREE - one of them is wrong\n");
    }
    else if (str_eq(line, "wincollide")) {
        /* TWO tasks inside windowed code at once. Built to FAIL.
         *
         * wintorture proved a single windowed task survives preemption by
         * call0 tasks -- 6/6, switch counter as control. Step 13 then panicked
         * with two windowed contexts, but that was one observation inside a
         * WiFi bring-up, with a blob, a driver and a mutex all in frame.
         *
         * This isolates the claim: two nat-os tasks, both calling the same
         * windowed function, nothing else involved. If the hazard is real this
         * must produce wrong checksums or a panic. If it PASSES, the step-13
         * diagnosis is wrong and the scheduler work is not justified.
         *
         * A concurrency test that passes first time usually is not testing
         * anything, so this is expected to fail and is worth nothing until it
         * does. */
        extern unsigned int vendor_torture(unsigned int, unsigned int);
        static volatile uint32_t bad, runs, live;
        static const uint32_t WANT = 8u * 7u * (1u+2u+3u+4u+5u+6u+7u+8u) / 8u * 0u; /* computed below */
        (void)WANT;

        /* expected checksum, same derivation as wintorture */
        uint32_t want = 0;
        for (uint32_t d = 1; d <= 8u; d++) {
            for (uint32_t i = 0; i < 6u; i++) { want += (d * 7u) + i; }
        }

        static uint32_t g_want;
        g_want = want;
        bad = 0; runs = 0; live = 0;

        /* Both entries are call0 (kernel tasks) and reach the windowed
         * function through rom_call3, which does NOT mask interrupts -- so
         * both can genuinely be inside windowed code at the same time. */
        static volatile uint32_t *pbad = &bad, *pruns = &runs, *plive = &live;
        (void)pbad; (void)pruns; (void)plive;

        uart_puts("   spawning two tasks, both entering windowed code\n");
        uart_puts("   expected checksum ");
        uart_put_dec(g_want);
        uart_puts("\n");

        extern void wincollide_entry(void);
        int t1 = task_create("wcol-a", wincollide_entry);
        int t2 = task_create("wcol-b", wincollide_entry);
        if (t1 < 0 || t2 < 0) {
            uart_puts("   could not create both tasks\n");
        } else {
            for (uint32_t i = 0; i < 40u; i++) { task_sleep(10u); }
            uart_puts("   runs=");
            uart_put_dec(wincollide_runs());
            uart_puts("  wrong=");
            uart_put_dec(wincollide_bad());
            uart_puts("\n");
            uart_puts(wincollide_bad()
                      ? "   FAILED as expected - two windowed contexts corrupt each other\n"
                      : "   no corruption seen; either the hazard is elsewhere or the\n"
                        "   two tasks never overlapped inside the window\n");
        }
    }
    else if (str_eq(line, "dramtest")) {
        /* Is the blob DRAM reservation actually usable memory?
         *
         * It was reserved from a region the kernel had never touched: the heap
         * ends at 0x3ffd3000 and the boot stack at 0x3ffd4000, so everything
         * above that was declared usable by linker.ld and never once written.
         * "linker.ld says it is dram" is not evidence that it is RAM.
         *
         * Walks up in 1 KB steps, writing and reading back a pattern derived
         * from the address, reporting the LAST address that worked. If the
         * board resets partway, the last printed address is the answer. */
        uart_puts("   probing ");
        uart_put_hex(BLOB_DRAM_ADDR);
        uart_puts(" .. ");
        uart_put_hex(BLOB_DRAM_ADDR + BLOB_DRAM_SIZE);
        uart_puts("\n");
        uint32_t ok_to = 0, bad = 0;
        for (uint32_t a = BLOB_DRAM_ADDR; a < BLOB_DRAM_ADDR + BLOB_DRAM_SIZE; a += 1024u) {
            volatile uint32_t *p = (volatile uint32_t *)a;
            uint32_t want = a ^ 0xA5A5A5A5u;
            *p = want;
            if (*p != want) { bad = a; break; }
            ok_to = a;
        }
        uart_puts("   highest address that stored and read back: ");
        uart_put_hex(ok_to);
        uart_puts("\n");
        if (bad) {
            uart_puts("   FIRST BAD: ");
            uart_put_hex(bad);
            uart_puts("  -- the reservation is not all RAM\n");
        } else {
            uart_puts("   all 1 KB steps stored and verified\n");
        }
    }
    else if (str_eq(line, "blob")) {
        /* next_moves/08 step 4. Maps the vendor image, validates it, and runs
         * the loader half -- .data copy and .bss zero.
         *
         * Does NOT call into it. Mapping and calling are separate on purpose:
         * if the map is wrong, a call jumps into whatever bytes are there and
         * the board dies with no report. Every number below is readable before
         * anything is executed. */
        const struct blob_entry *e = blob_map();
        if (!e) {
            uart_puts("   no valid image at ");
            uart_put_hex(BLOB_FLASH_ADDR);
            uart_puts("\n   flash one with: vendor/net80211/build_blob.ps1 -Flash\n");
            uart_puts("   (an erased region reads 0xFF, which is why this is a\n");
            uart_puts("    clean negative rather than a crash)\n");
        } else {
            uart_puts("   magic     N802  version ");
            uart_put_dec(e->version);
            uart_puts("\n   image     ");
            uart_put_dec(e->image_size);
            uart_puts(" bytes  of ");
            uart_put_dec(BLOB_FLASH_SIZE);
            uart_puts(" reserved\n   .data     ");
            uart_put_dec(e->data_size);
            uart_puts(" B from ");
            uart_put_hex(e->data_lma);
            uart_puts(" to ");
            uart_put_hex(e->data_vma);
            uart_puts("\n   .bss      ");
            uart_put_dec(e->bss_end - e->bss_start);
            uart_puts(" B at ");
            uart_put_hex(e->bss_start);
            uart_puts("\n   tx entry  ");
            uart_put_hex(e->wifi_80211_tx);
            uart_puts("\n");

            /* `blob map` stops here: MMU programmed and image validated, but
             * nothing written. Separating the two is what tells a bad map
             * apart from a bad load. */
            if (str_eq(arg, "map")) {
                uart_puts("   mapped and validated; loader NOT run\n");
                return;
            }
            int rc = blob_init(e);
            uart_puts("   loader    rc=");
            uart_put_dec((unsigned int)(rc < 0 ? -rc : rc));
            uart_puts(rc == 0 ? " (.data copied, .bss zeroed)\n"
                              : " REFUSED - a range was outside the reservation\n");
            if (rc == 0) {
                /* Verify the WHOLE load, not one word. A copy loop that
                 * returns is not evidence it copied anything useful, and a
                 * single matching word would also match if the loop had run
                 * exactly once. */
                uint32_t bad_d = 0, bad_b = 0;
                const volatile uint32_t *im = (const volatile uint32_t *)e->data_lma;
                const volatile uint32_t *dv = (const volatile uint32_t *)e->data_vma;
                for (uint32_t i = 0; i < e->data_size / 4u; i++) {
                    if (dv[i] != im[i]) { bad_d++; }
                }
                const volatile uint32_t *bz = (const volatile uint32_t *)e->bss_start;
                for (uint32_t i = 0; i < (e->bss_end - e->bss_start) / 4u; i++) {
                    if (bz[i] != 0u) { bad_b++; }
                }
                uart_puts("   verify    .data ");
                uart_put_dec(e->data_size / 4u);
                uart_puts(" words, ");
                uart_put_dec(bad_d);
                uart_puts(" mismatched;  .bss ");
                uart_put_dec((e->bss_end - e->bss_start) / 4u);
                uart_puts(" words, ");
                uart_put_dec(bad_b);
                uart_puts(" non-zero\n");
                uart_puts((bad_d == 0u && bad_b == 0u)
                          ? "   LOAD VERIFIED\n" : "   LOAD IS WRONG\n");
            }
            uart_puts("   NOT CALLED. mapping is not running.\n");
        }
    }
    else if (str_eq(line, "nestfault")) {
        /* NA-007. Panics, then faults inside the panic handler.
         *
         * Two things must be true afterwards, and the second is the one that
         * matters: the guard must announce PANIC DURING PANIC rather than
         * looping, and the next boot must report THIS panic -- the guard
         * message -- not the StoreProhibited the handler caused. If the next
         * boot blames address zero, the record was overwritten and the guard
         * did not do its job. */
        uart_puts("   panicking, then faulting inside the handler\n");
        uart_puts("   after the reset, check that the reported fault is the\n");
        uart_puts("   guard below and NOT a StoreProhibited at 0x00000000\n");
        g_panic_nest_test = 1;
        kernel_panic_msg("nestfault: re-entry guard exerciser", 0x5eed);
    }
    else if (str_eq(line, "smash")) {
        uart_puts("   clobbering this task's guard word; the next switch should panic\n");
        task_smash_guard();
        for (;;) {
        }
    }
    else if (str_eq(line, "waketest")) {
        /* NA-005 regression.
         *
         * The bug lives in a dormant path -- touch_irq_wait() has no caller --
         * so it cannot be reproduced by touching the screen. What CAN be tested
         * is the primitive the fix rests on: a wake delivered while the task is
         * still RUNNING must survive to the sleep that follows.
         *
         * Two measurements, and the second one is the point. If only the
         * "armed" case were measured, a task_sleep_armed() that returned
         * immediately for some unrelated reason would pass. The plain case is
         * the negative control: it must sleep the FULL time, because that is
         * exactly the old, broken behaviour. A test where both numbers are
         * small is a test that is measuring nothing. */
        const uint32_t N = 30u;         /* 300 ms -- a full sleep is unmistakable */

        task_arm_wake();
        task_wake(task_current());      /* wake a task that is still running */
        uint32_t t0 = timer_ticks();
        task_sleep_armed(N);
        uint32_t armed = timer_ticks() - t0;

        task_arm_wake();
        task_wake(task_current());
        t0 = timer_ticks();
        task_sleep(N);                  /* clears on entry -- must NOT be cut short */
        uint32_t plain = timer_ticks() - t0;

        uart_puts("   armed  ");
        uart_put_dec(armed);
        uart_puts(" ticks  (expected 0-1, the wake survived)\n   plain  ");
        uart_put_dec(plain);
        uart_puts(" ticks  (expected ");
        uart_put_dec(N);
        uart_puts(", control: the wake was discarded)\n");
        uart_puts((armed <= 1u && plain >= N - 1u)
                  ? "   PASS - a wake to a running task survives task_sleep_armed()\n"
                  : "   FAIL - see NA-005 in docs/audit-spec.md\n");
    }
    else if (str_eq(line, "storetest")) {
        /* Exercise the save-deferral bound, because a mechanism with no
         * exerciser is untested code and this project has a long record of
         * those reporting success.
         *
         * Registers a predicate that ALWAYS refuses -- the worst case the
         * bound exists for, a radio that never goes idle or a predicate with a
         * bug -- then calls the periodic path enough times to cross
         * STORE_DEFER_MAX and watches what happens.
         *
         * What must be true:
         *   - the first STORE_DEFER_MAX-1 calls return 1 (deferred)
         *   - call STORE_DEFER_MAX returns 0 and store_forced() rises
         *   - the record is actually written, not merely reported as written
         *
         * The last point is why store_dirty() is checked at the end rather
         * than trusting the return code. */
        /* Something must be pending or the periodic path short-circuits. */
        store_slot_set(STORE_KERNEL_BANK, 0u, timer_ticks());
        uart_puts("   dirty before: ");
        uart_put_dec((unsigned int)store_dirty());
        uart_puts("\n");

        uint32_t d0 = store_deferrals(), f0 = store_forced();
        store_set_may_save(storetest_refuse);

        uint32_t deferred = 0, wrote = 0, err = 0;
        int first_write_at = -1;
        for (uint32_t i = 0; i < STORE_DEFER_MAX + 2u; i++) {
            int rc = store_save_if_allowed();
            if (rc == 1) {
                deferred++;
            } else if (rc == 0) {
                wrote++;
                if (first_write_at < 0) {
                    first_write_at = (int)i;
                }
                /* A write clears dirty, so make it dirty again or every later
                 * call short-circuits and the test stops testing anything. */
                store_slot_set(STORE_KERNEL_BANK, 0u, timer_ticks() + i);
            } else {
                err++;
            }
        }
        store_set_may_save(0);          /* always allow again */

        uart_puts("   calls ");
        uart_put_dec(STORE_DEFER_MAX + 2u);
        uart_puts(": deferred=");
        uart_put_dec(deferred);
        uart_puts(" wrote=");
        uart_put_dec(wrote);
        uart_puts(" errors=");
        uart_put_dec(err);
        uart_puts("\n   first write at call ");
        uart_put_dec((unsigned int)first_write_at);
        uart_puts("  (expected ");
        uart_put_dec(STORE_DEFER_MAX - 1u);
        uart_puts(")\n   deferrals +");
        uart_put_dec(store_deferrals() - d0);
        uart_puts("   forced +");
        uart_put_dec(store_forced() - f0);
        uart_puts("\n");
        uart_puts((first_write_at == (int)(STORE_DEFER_MAX - 1u)
                   && (store_forced() - f0) > 0u && err == 0u)
                  ? "   PASS - refused until the bound, then forced through\n"
                  : "   FAIL - the bound did not behave as documented\n");
    }
    else if (str_eq(line, "jitter")) {
        /* next_moves/04, step one: get the number before touching anything.
         *
         * That file's own instruction is "measure before changing", and its
         * named suspect is store_save() -- a flash sector erase inside
         * crit_enter/crit_exit, which is tens of milliseconds where nothing
         * runs at all. If that dominates the tail then the honest fix is a
         * policy ("do not erase flash while a control loop is running"), which
         * is free, rather than a scheduler change, which is not.
         *
         *   jitter          print the distribution
         *   jitter reset    zero it
         *   jitter save     time a real store_save() and print the tail again
         *
         * The histogram counts every ready-to-running wait, in ticks, which is
         * the same quantity `fair maxwait` reports as a high-water mark. A
         * maximum cannot distinguish one long wait per hour from one per
         * second; those are the same number and completely different systems.
         */
        static const char *LBL[8] = { "0", "1", "2-3", "4-7",
                                      "8-15", "16-31", "32-63", "64+" };
        if (str_eq(arg, "reset")) {
            task_wait_hist_reset();
            uart_puts("   histogram and maxwait zeroed\n");
        } else {
            if (str_eq(arg, "save")) {
                /* Time the suspect directly, rather than inferring it from the
                 * tail. store_save() erases a sector with interrupts masked;
                 * this is how long nothing else in the system can run. */
                uint32_t t0 = xt_ccount();
                int rc = store_save();
                uint32_t dt = xt_ccount() - t0;
                uart_puts("   store_save() returned ");
                uart_put_dec((unsigned int)rc);
                uart_puts(", took ");
                uart_put_dec(dt / 80000u);
                uart_puts(" ms (");
                uart_put_dec(dt);
                uart_puts(" cycles at 80 MHz)\n");
                uart_puts("   during which interrupts were masked and NOTHING\n"
                          "   else in the system ran.\n");
                /* The ticks LOST to it, which is the actual point.
                 *
                 * The histogram below cannot see this stall. `waiting` only
                 * advances on a scheduler decision, decisions happen on ticks,
                 * and the tick is masked for the whole erase -- so the wait it
                 * causes is never counted by the instrument built to count
                 * waits. That is visible as an empty 8-15 bucket while the
                 * erase lasts 12-14 ticks.
                 *
                 * timer_late_count() does see it, because the timer notices its
                 * own missed deadlines after the fact. An instrument sharing a
                 * dependency with the fault is the failure this project keeps
                 * rediscovering; here it was predicted and is reported rather
                 * than tripped over. */
                uart_puts("   timer deadlines missed/skipped, cumulative: ");
                uart_put_dec(timer_late_count());
                uart_puts("\n");
            }
            uint32_t total = 0;
            for (uint32_t i = 0; i < 8u; i++) {
                total += task_wait_hist(i);
            }
            uart_puts("   ready-to-running wait, ticks (1 tick = 10 ms)\n");
            uart_puts("   bucket   count      share\n");
            for (uint32_t i = 0; i < 8u; i++) {
                uint32_t c = task_wait_hist(i);
                uart_puts("   ");
                uart_puts(LBL[i]);
                uart_puts("\t   ");
                uart_put_dec(c);
                uart_puts("\t   ");
                /* Per-mille rather than percent: the tail is the point and at
                 * whole percent the interesting buckets all read 0. */
                uart_put_dec(total ? (c * 1000u) / total : 0u);
                uart_puts("/1000\n");
            }
            uart_puts("   samples ");
            uart_put_dec(total);
            uart_puts("   worst ");
            uart_put_dec(task_max_wait());
            uart_puts(" ticks\n");
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
    else if (str_eq(line, "wintest")) {
        int d = parse_int(arg);
        if (d < 0) { d = 20; }
        uart_puts("   calling a WINDOWED function, depth ");
        uart_put_dec((unsigned int)d);
        uart_puts("\n");
        uint32_t got = win_call_probe((uint32_t)d);
        uart_puts("   returned ");
        uart_put_dec(got);
        uart_puts(got == (uint32_t)d ? "  CORRECT" : "  WRONG");
        uart_puts("\n");
    }
    else if (str_eq(line, "wintorture")) {
        /* Does a context switch corrupt live windowed frames?
         *
         * This is the premise behind phy_stack_call masking interrupts, and it
         * has never actually been tested -- vendor_probe completes in
         * microseconds and is essentially never preempted.
         *
         * vendor_torture holds 8 live windowed frames and spins at the bottom
         * for N ms, so many ticks elapse with them live. rom_call3 is used
         * rather than phy_stack_call precisely BECAUSE it leaves interrupts
         * enabled. The checksum is verified on the way out: a switch that
         * mishandles the window gives a wrong number, not a crash.
         *
         * The switch count is the control. If it does not rise, no preemption
         * happened and a PASS means nothing. */
        extern unsigned int vendor_torture(unsigned int, unsigned int);
        int ms = parse_int(arg);
        if (ms < 0) { ms = 60; }

        /* depth 8, 6 locals each: 8*(7d + 0..5) summed on unwind. */
        uint32_t want = 0;
        for (uint32_t d = 1; d <= 8u; d++) {
            for (uint32_t i = 0; i < 6u; i++) { want += (d * 7u) + i; }
        }

        uint32_t s0 = task_switch_count(task_current());
        uint32_t got = rom_call3((uint32_t)&vendor_torture, 8u, (uint32_t)ms, 0u);
        uint32_t sw = task_switch_count(task_current()) - s0;

        uart_puts("   spun ");
        uart_put_dec((unsigned int)ms);
        uart_puts(" ms with 8 windowed frames live, interrupts ENABLED\n");
        uart_puts("   switches during the call: ");
        uart_put_dec(sw);
        uart_puts(sw ? "  (preemption really happened)\n"
                     : "  -- NONE, so this proves nothing\n");
        uart_puts("   checksum ");
        uart_put_dec(got);
        uart_puts(" expected ");
        uart_put_dec(want);
        uart_puts(got == want ? "  CORRECT\n" : "  WRONG - frames were corrupted\n");
    }
    else if (str_eq(line, "x20")) {
        extern void x20_run(void);
        extern uint32_t g_x20_outcome;
        extern uint32_t g_x20_pre[3];
        extern uint32_t g_x20_post_ws;
        extern uint32_t g_x20_recovered[4];
        uart_puts("   [X20] call8 -> entry(+2) -> WS:=own-bit -> retw.n\n");
        x20_run();
        uart_puts("   SURVIVED. outcome=");
        uart_put_dec(g_x20_outcome);
        uart_puts(" pre(ps/ws/wb)=");
        uart_put_hex(g_x20_pre[0]);
        uart_putc('/');
        uart_put_hex(g_x20_pre[1]);
        uart_putc('/');
        uart_put_dec(g_x20_pre[2]);
        uart_puts(" wiped=");
        uart_put_hex(g_x20_post_ws);
        uart_puts(" rec(a2/a3/e4/e5)=");
        uart_put_hex(g_x20_recovered[0]);
        uart_putc('/');
        uart_put_hex(g_x20_recovered[1]);
        uart_putc('/');
        uart_put_hex(g_x20_recovered[2]);
        uart_putc('/');
        uart_put_hex(g_x20_recovered[3]);
        uart_putc('\n');
    }
    else if (str_eq(line, "vendorcall")) {
        int d = parse_int(arg);
        if (d < 0) { d = 20; }
        uart_puts("   calling a -mabi=windowed object built by the vendor compiler\n");
        uint32_t got = win_call_vendor((uint32_t)d, 1u);
        uart_puts("   vendor_probe(");
        uart_put_dec((unsigned int)d);
        uart_puts(",1) = ");
        uart_put_dec(got);
        uart_puts("\n");
    }
    else if (str_eq(line, "romcall")) {
        /* The first Espressif code this kernel has ever executed.
         *
         * crc32_le lives in the chip ROM at a fixed address, is windowed ABI,
         * and is a pure function -- no heap, no clocks, no calibration data,
         * none of the environment a real blob wants. It is therefore the
         * smallest possible test of "can vendor code run here", and its answer
         * is checkable against a CRC computed on the host: garbage from a
         * broken call does not accidentally equal a CRC32. */
        static const char msg[] = "123456789";
        uint32_t r = rom_call3(ESP_ROM_CRC32_LE, 0u, (uint32_t)msg, 9u);
        uart_puts("   crc32_le(0,");
        uart_puts(msg);
        uart_puts(",9) = ");
        uart_put_hex(r);
        uart_puts("\n");
    }
#if BOARD_HAS_WIFI
    /* Blob-dependent. These reach libphy/libpp through the windowed
     * bridge; a build with BOARD_HAS_WIFI 0 links no vendor archive at
     * all and these commands do not exist. See docs/blob-free.md. */
    else if (str_eq(line, "phyver")) {
        /* First call into Espressif's RADIO blob.
         *
         * phy_version_print() takes no arguments and touches no RF hardware;
         * it reports through phy_printf, which is our windowed shim writing to
         * a buffer. So this exercises the whole chain -- call0 -> window bridge
         * -> blob -> windowed shim -> buffer -> call0 -- without going near the
         * radio. If the blob is linked and running it states its own version. */
        extern void phy_version_print(void);
        phy_host_log_len = 0; phy_host_log_buf[0] = 0;
        rom_call3((uint32_t)&phy_version_print, 0u, 0u, 0u);
        uart_puts("   libphy says: ");
        uart_puts(phy_host_log_buf);
        uart_puts("\n");
    }
    else if (str_eq(line, "phyinit")) {
        /* The first thing in this project that touches the radio.
         *
         * Printed BEFORE the call as well as after, deliberately: if the board
         * hangs or resets inside register_chipv7_phy, the difference between
         * "never started" and "started and did not return" is the only clue
         * that would survive a reset. It is how the watchdog reset on the
         * first attempt was traced to the critical-section shim rather than to
         * the PHY -- the call had already returned 0. */
        if (phyinit_attempted()) {
            uart_puts("   already initialised this boot; a second call faults\n");
        } else {
            uart_puts("   ungating the radio clock, calling register_chipv7_phy...\n");
            int r = phyinit_run();
            uart_puts("   returned ");
            uart_put_dec((unsigned int)r);
            uart_puts("\n   phy log: ");
            uart_puts(phy_host_log_buf);
            uart_puts("\n");
        }
    }
#endif /* BOARD_HAS_WIFI */
    else if (str_eq(line, "efusedump")) {
        /* Raw block 0. The decode below it was wrong on the first attempt and
         * the OUI check caught it, so this prints the source bytes rather than
         * inviting another guess at the byte order. */
        for (uint32_t w = 0; w < 8; w++) {
            uart_puts("   rdata");
            uart_put_dec(w);
            uart_puts(" = ");
            uart_put_hex(*(volatile uint32_t *)(0x3FF5A000u + w * 4u));
            uart_puts("\n");
        }
    }
    else if (str_eq(line, "macaddr")) {
        uint8_t mac[6];
        efuse_factory_mac(mac);
        uart_puts("   factory mac ");
        for (int i = 0; i < 6; i++) {
            if (i) {
                uart_puts(":");
            }
            /* Two hex digits, zero-padded. uart_put_hex prints a full word
             * with an 0x prefix, which is not a MAC address. */
            static const char hex[] = "0123456789abcdef";
            char b[3];
            b[0] = hex[(mac[i] >> 4) & 0xF];
            b[1] = hex[mac[i] & 0xF];
            b[2] = 0;
            uart_puts(b);
        }
        uart_puts(efuse_mac_valid(mac)
                  ? "   crc matches - decode confirmed\n"
                  : "   CRC MISMATCH - the byte order is wrong\n");
    }
#if BOARD_HAS_WIFI
    /* Blob-dependent. These reach libphy/libpp through the windowed
     * bridge; a build with BOARD_HAS_WIFI 0 links no vendor archive at
     * all and these commands do not exist. See docs/blob-free.md. */
    else if (str_eq(line, "macrst")) {
        /* Arm the MAC reset for the next macinit.
         *
         * UM-NATOS-028 §3 ends on this as the strongest untried lead: this
         * kernel has only ever UNGATED the WiFi peripheral and never RESET it,
         * so the MAC runs in whatever state the ROM bootloader left it in. That
         * would plausibly receive while refusing to transmit, which is exactly
         * the symptom -- 178 frames of 178 reported complete, nothing heard.
         *
         * Sequence: macrst, macinit, chan 1, macrx, probe. If receive still
         * works AND probes get answered, that is the answer. If receive breaks,
         * that is also informative and is why this is opt-in. */
        wifimac_reset_next(!str_eq(arg, "off"));
        uart_puts(str_eq(arg, "off")
                  ? "   MAC reset disarmed\n"
                  : "   MAC reset armed; run 'macinit' next\n"
                    "   (only DPORT_WIFIMAC_RST bit 2 -- the baseband and front\n"
                    "    end are left alone so the PHY calibration survives)\n");
    }
    else if (str_eq(line, "macinit")) {
        /* Liveness is sampled BEFORE the write as well as after.
         *
         * Without the before-reading, "words changed" after init proves only
         * that words change -- not that the write caused it. The pair is the
         * measurement; either number alone is an anecdote. */
        uint32_t addr0 = 0, addr1 = 0;
        uint32_t live0 = wifimac_liveness(&addr0);
        uart_puts("   before: ctrl=");
        uart_put_hex(*(volatile uint32_t *)WIFIMAC_CTRL_REG);
        uart_puts("  moving words=");
        uart_put_dec(live0);
        uart_puts("\n");

        int r = wifimac_init();
        if (r == -1) {
            uart_puts("   refused: run 'phyinit' first (it must return 0)\n");
        } else if (r == -2) {
            uart_puts("   already initialised this boot\n");
        } else {
            uint32_t live1 = wifimac_liveness(&addr1);
            uart_puts("   after:  ctrl=");
            uart_put_hex(wifimac_ctrl_after());
            uart_puts(" (was ");
            uart_put_hex(wifimac_ctrl_before());
            uart_puts(")  moving words=");
            uart_put_dec(live1);
            if (addr1) {
                uart_puts("  first at ");
                uart_put_hex(addr1);
            }
            uart_puts("\n   ");
            uart_puts(live1 > live0
                      ? "MAC IS RUNNING - counters advance that did not before\n"
                      : "no new movement; the MAC is readable but may be idle\n");
            /* Read only. Clearing bit 31 here killed receive outright; see the
             * note in wifimac_init(). The value is reported because nothing
             * else says what the ROM bootloader left in this register. */
            uart_puts("   bitmask_084 ");
            uart_put_hex(wifimac_bm084_before());
            uart_puts((wifimac_bm084_before() & 0x80000000u)
                      ? "  (bit 31 SET at boot -- left alone)\n"
                      : "  (bit 31 clear at boot)\n");
            if (wifimac_reset_done()) {
                uart_puts("   MAC was RESET first: DPORT_WIFI_RST_EN ");
                uart_put_hex(wifimac_rst_before());
                uart_puts(" -> ");
                uart_put_hex(wifimac_rst_after());
                uart_puts("\n");
            }
        }
    }
    else if (str_eq(line, "chan")) {
        int ch = parse_int(arg);
        if (ch < 1) {
            uart_puts("   usage: chan <1..13>\n");
        } else {
            int r = wifimac_set_channel((uint32_t)ch);
            uart_puts(r == 0 ? "   tuned to channel " : "   refused, code ");
            uart_put_dec(r == 0 ? (unsigned)ch : (unsigned)(-r));
            uart_puts("\n");
        }
    }
    else if (str_eq(line, "macrx")) {
        int r = wifimac_rx_start();
        if (r == -1)      { uart_puts("   run macinit first\n"); }
        else if (r == -2) { uart_puts("   already armed\n"); }
        else if (r == -3) { uart_puts("   out of DRAM for rx buffers\n"); }
        else if (r == -4) { uart_puts("   hardware never acknowledged the chain\n"); }
        else              { uart_puts("   receiver armed, promiscuous\n"); }
    }
    else if (str_eq(line, "beacon")) {
        /* Default SSID is deliberately identifiable rather than generic: the
         * whole point is that a phone can see it and nobody has to wonder
         * whether they are looking at someone else's network. */
        const char *ssid = *arg ? arg : "nat-os";
        int r = wifimac_beacon_start(ssid);
        if (r == -1)      { uart_puts("   run macrx first\n"); }
        else if (r == -2) { uart_puts("   tune a channel first, e.g. 'chan 1'\n"); }
        else {
            uart_puts("   beaconing \"");
            uart_puts(ssid);
            uart_puts("\" every 100 ms, ");
            uart_put_dec(wifimac_beacon_len());
            uart_puts(" byte frame\n   look for it in a phone's wifi list; "
                      "'txstat' for what the hardware says\n");
        }
    }
    else if (str_eq(line, "beaconoff")) {
        wifimac_beacon_stop();
        uart_puts("   stopped\n");
    }
#endif /* BOARD_HAS_WIFI */
    else if (str_eq(line, "fifopoke")) {
        /* One tiny draw, deliberately small enough to take the FIFO path.
         *
         * spi_tx() sends anything over 64 bytes by DMA and anything smaller
         * through the FIFO. The raycaster blits in 480-byte chunks, so it is
         * the only heavy DMA user; application draws are small text and rects
         * and frequently go through the FIFO instead.
         *
         * If a garbled view comes good after this, the DMA engine was in a
         * state an intervening FIFO transfer clears -- which is what starting a
         * program does dozens of times a second, and would finally explain the
         * repair. 8 pixels is 16 bytes, drawn on the bottom edge inside the
         * spectrum band, which repaints every frame anyway. */
        display_fill_rect(0, DISP_H - 2u, 8u, 1u, COLOR_BLACK);
        uart_puts("   16-byte FIFO transfer issued (8 px, bottom edge)\n");
    }
    else if (str_eq(line, "resync")) {
        display_resync();
        uart_puts("   panel window re-issued; no pixels drawn\n");
    }
    else if (str_eq(line, "touchoff")) {
        /* Stop touch DELIVERING events, without stopping it sampling.
         *
         * The unattended sweep logged four taps and two opens at 390 s with
         * nobody present, and maxy moving 254 -> 270 -- slot 2's bottom edge,
         * so a third program was launched. ping and pong hold slots 0 and 1.
         * A program drawing continuously is the one thing known to repair the
         * 3D view, so "leave it five minutes and it fixes itself" may just be
         * "leave it long enough for a phantom tap to open something".
         *
         * With this set, presses are still read and still latched into the
         * touch counters -- so a phantom press stays visible -- but nothing
         * routes them, so none can select an icon, launch a program, or steer
         * the camera. If the view still heals with this on, the touch theory
         * is dead and uptime is real. */
        extern volatile int g_touch_events_off;
        g_touch_events_off = !str_eq(arg, "on");
        uart_puts(g_touch_events_off
                  ? "   touch events suppressed; sampling and telemetry still live\n"
                  : "   touch events delivered again\n");
    }
    else if (str_eq(line, "ztrack")) {
        /* One pressure line per report, for as long as it takes.
         *
         * The phantom presses arrived at ~374 s and ~390 s in two independent
         * runs, which is consistent enough to be a mechanism rather than an
         * accident. The leading candidate is thermal: a small board reaches
         * equilibrium in about that time, and a pressure baseline that walks
         * with temperature would start crossing a fixed threshold at a
         * repeatable moment.
         *
         * What settles it is the BASELINE, not the presses. If `min` climbs
         * steadily toward `thr` over six minutes, the hypothesis is confirmed
         * before a single phantom press occurs; if `min` sits flat and `max`
         * spikes once from nowhere, it is an electrical event and thermal drift
         * is dead. The two look nothing alike, which is what makes this worth
         * running rather than reasoning about. */
        extern volatile int g_ztrack;
        g_ztrack = !str_eq(arg, "off");
        uart_puts(g_ztrack ? "   ZTRK lines on\n" : "   ZTRK lines off\n");
    }
    else if (str_eq(line, "panelid")) {
        /* Does MISO work at all?
         *
         * Everything about reading the panel back depends on this one answer,
         * and it must be established against a KNOWN value before anything is
         * concluded from an unknown one. 0xD3 (Read ID4) returns a dummy byte
         * then 0x00 0x93 0x41 on every ILI9341 ever made. If those three bytes
         * come back, the read path works and the framebuffer can be compared
         * against the glass. If everything reads 0x00, MISO is dead -- which is
         * precisely how the touch controller presented on an output-only pin,
         * and a constant zero is the most convincing wrong answer available.
         *
         * 0x04 (Read Display ID) is printed alongside as a second opinion; some
         * modules answer one and not the other. */
        uint8_t id4[5], id[5];
        display_panel_read(0xD3u, id4, 5u);
        display_panel_read(0x04u, id,  5u);

        uart_puts("   0xD3 ->");
        for (int i = 0; i < 5; i++) { uart_puts(" "); uart_put_hex(id4[i]); }
        uart_puts("\n   0x04 ->");
        for (int i = 0; i < 5; i++) { uart_puts(" "); uart_put_hex(id[i]); }

        int found = 0;
        for (int i = 0; i < 4; i++) {
            if (id4[i] == 0x93u && id4[i + 1] == 0x41u) { found = 1; }
        }
        int all_zero = 1;
        for (int i = 0; i < 5; i++) {
            if (id4[i] || id[i]) { all_zero = 0; }
        }
        uart_puts(found     ? "\n   0x93 0x41 present -- MISO WORKS, readback is viable\n"
                : all_zero  ? "\n   everything zero -- MISO is dead or the pad is misconfigured\n"
                            : "\n   answered, but not the ILI9341 signature; check clock and dummy count\n");
    }
    else if (str_eq(line, "lrtest")) {
        /* Is display_blit()'s offset CONSTANT, or does it accumulate per row?
         *
         * Established so far, all by measurement rather than reading:
         *   - the panel's x axis is correct   (red at x=0 appears on the left)
         *   - display_fill_rect() lands correctly
         *   - the 3D view's FRAMEBUFFER is correct (dumped; close button at
         *     buffer x=220, the right edge)
         *   - on the glass that button appears at the LEFT, and a narrow strip
         *     at the left keeps whatever was underneath
         *   - display_blit() lands a few pixels RIGHT of display_fill_rect()
         *
         * The last two only fit together if the error GROWS. A constant offset
         * of a few pixels cannot carry a button from x=220 round to x=0; 224
         * rows of a few pixels each can. The 3D view is 224 rows and is the only
         * caller of the contiguous path, which is why it is the only place this
         * ever showed.
         *
         * So: blit a tall band of VERTICAL bars. Vertical bars are the right
         * test object because the failure mode writes itself across them --
         *
         *   bars stay vertical, whole band offset -> constant, one-time
         *   bars SLANT                            -> accumulates per row, and
         *                                            the slope is the per-row
         *                                            error, readable by eye
         *
         * A reference band drawn with fill_rect sits at the bottom, so "where
         * should they be" is on the same screen as "where they are". */
        extern volatile int g_display_frozen;
        g_display_frozen = 1;

        /* Measure the per-transfer error DIRECTLY, instead of inferring it.
         *
         * Bands of bars showed that something drifts, but not how much or per
         * what. This draws MARKERS: a short white bar at x=0..15 drawn three
         * ways, stacked, with a fill_rect marker as the ruler.
         *
         *   ref   fill_rect, 16 px wide            -- the zero mark
         *   one   ONE blit row, full width         -- is a SINGLE transfer OK?
         *   many  row 24 of a 24-row blit          -- has it drifted by then?
         *
         * If `one` sits under `ref`, a single contiguous transfer is correct and
         * the error is per-transfer accumulation. If `one` is already offset,
         * every transfer is wrong and row count is irrelevant. The gap between
         * `one` and `many` is the accumulated error over 24 transfers, readable
         * against the 16 px marker width.
         *
         * This is the smallest experiment that distinguishes the two, and its
         * answer is a number rather than an impression. */
        /* 12 transfers rather than 24: the buffer pair here and the txwatch
         * sampler together overflowed DRAM, and this half still measures
         * exactly what it did -- where the LAST of N contiguous transfers
         * lands, against a 16 px ruler. Only N changed. */
        enum { MANY_H = 12u };
        static uint16_t one_row[DISP_W];
        static uint16_t many[DISP_W * MANY_H];

        for (uint32_t col = 0; col < DISP_W; col++) {
            one_row[col] = (col < 16u) ? COLOR_WHITE : COLOR_BLACK;
        }
        for (uint32_t row = 0; row < MANY_H; row++) {
            for (uint32_t col = 0; col < DISP_W; col++) {
                /* Only the LAST row carries the marker, so what is measured is
                 * where the final transfer landed, not a smear of all of them. */
                many[row * DISP_W + col] =
                    (row == MANY_H - 1u && col < 16u) ? COLOR_GREEN : COLOR_BLACK;
            }
        }

        display_lock();
        display_fill_rect(0u, 0u, DISP_W, DISP_H, COLOR_BLACK);

        /* Ruler: ticks every 16 px so an offset can be counted, not estimated. */
        for (uint32_t t = 0; t < DISP_W; t += 16u) {
            display_fill_rect(t, 60u, 1u, 8u, COLOR_GREY);
        }

        /* ref -- fill_rect, the zero mark */
        display_fill_rect(0u, 70u, 16u, 8u, COLOR_RED);

        /* one -- a single full-width blit row */
        display_blit(0u, 82u, DISP_W, 1u, one_row, DISP_W);

        /* many -- the last row of a MANY_H-row contiguous blit */
        display_blit(0u, 94u, DISP_W, MANY_H, many, DISP_W);

        display_text(4u, 140u, "RED=FILLRECT",  COLOR_WHITE, COLOR_BLACK, 1u);
        display_text(4u, 155u, "WHT=1 BLIT ROW", COLOR_WHITE, COLOR_BLACK, 1u);
        display_text(4u, 170u, "GRN=24TH ROW",  COLOR_WHITE, COLOR_BLACK, 1u);
        display_text(4u, 195u, "TICKS=16PX",    COLOR_WHITE, COLOR_BLACK, 1u);
        display_unlock();

        uart_puts("   top band  : 48 rows, display_blit contiguous path\n"
                  "   bottom band: same bars, display_fill_rect\n"
                  "   Each bar is 40 px: R G B Y C M, left to right.\n"
                  "   Do the top band's bars stay VERTICAL, or slant?\n"
                  "   'dfreeze off' when done.\n");
    }
    else if (str_eq(line, "panelpull")) {
        /* Is anything connected to MISO at all?
         *
         * `panelid` establishes that the read returns zeros. It cannot say WHY,
         * and the two candidates need different responses: a module whose SDO
         * is not populated is a dead end, while a line held low by something is
         * a bug worth chasing. A negative read cannot separate them, because an
         * unconnected pin and a pin driven low both read 0x00.
         *
         * A weak internal pull loses to anything driving the net and wins on a
         * floating one, so reading the SAME command three ways is decisive:
         *
         *   up 0xFF / down 0x00  -> nothing drives MISO. The pad is fine and
         *                           the panel is not attached to it.
         *   both 0x00            -> something holds it low; "not populated" is
         *                           the wrong explanation.
         *   same real bytes      -> the panel answers and the zeros were a
         *                           framing or timing fault after all.
         *
         * Eight bytes rather than five, because the second candidate in
         * next_moves/05 §5.1 is a wrong dummy-clock count -- and a framing
         * error shows as 0x93 0x41 sitting at an unexpected offset rather than
         * as silence. Reading past where the signature should be costs nothing
         * and tests that at the same time.
         *
         * GPIO12 is MTDI, a strapping pin. The pull is applied for microseconds
         * inside the read and cleared before it returns; pad hold is never
         * enabled. See display.c. */
        static const struct { const char *name; int pull; } WAYS[] = {
            { "none", DISPLAY_PULL_NONE },
            { "up  ", DISPLAY_PULL_UP   },
            { "down", DISPLAY_PULL_DOWN },
        };

        uint8_t got[3][8];
        for (int w = 0; w < 3; w++) {
            display_panel_read_pull(0xD3u, got[w], 8u, WAYS[w].pull);
            uart_puts("   0xD3 pull=");
            uart_puts(WAYS[w].name);
            /* The IO_MUX value the pad ACTUALLY held, read back from the
             * register. Without it these three passes are only assumed to be
             * three different configurations -- and if the pull bits never
             * landed, all three would be identical by construction and the
             * verdict below would be drawn from an experiment whose variable
             * never varied. FUN_PU is bit 8, FUN_PD bit 7. */
            uart_puts(" pad=");
            uart_put_hex(display_panel_pad());
            uart_puts(" ->");
            for (int i = 0; i < 8; i++) {
                uart_puts(" ");
                uart_put_hex(got[w][i]);
            }
            uart_puts("\n");
        }

        /* Interpretation, stated by the kernel rather than left to a reader
         * squinting at three rows of hex. */
        int follows = 1, all_zero = 1, sig = 0;
        for (int i = 0; i < 8; i++) {
            if (got[1][i] != 0xFFu || got[2][i] != 0x00u) { follows = 0; }
            for (int w = 0; w < 3; w++) {
                if (got[w][i]) { all_zero = 0; }
            }
            if (i < 7 && got[0][i] == 0x93u && got[0][i + 1] == 0x41u) { sig = 1; }
        }

        if (sig) {
            uart_puts("   0x93 0x41 present -- MISO WORKS\n");
        } else if (follows) {
            uart_puts("   reads follow the pull -- NOTHING DRIVES MISO.\n"
                      "   The pad and the read path are fine; the panel's SDO\n"
                      "   is not connected on this module. Readback is not\n"
                      "   available on this board and no software change helps.\n");
        } else if (all_zero) {
            uart_puts("   zero regardless of pull -- something HOLDS the line\n"
                      "   low. Not simply unpopulated; worth chasing.\n");
        } else {
            uart_puts("   mixed -- see the rows above; the line is doing\n"
                      "   something, so check clock and dummy count next.\n");
        }
    }
    else if (str_eq(line, "view3d")) {
        /* Open or close the 3D view from the terminal.
         *
         * Every attempt to catch the garbling in the act has failed the same
         * way: the view is opened by hand, some seconds pass before anything
         * can be armed, and by the time a measurement starts the picture has
         * already healed. The failure is a STARTUP failure, so the instrument
         * has to exist before the first frame does, and that needs the moment
         * of opening to be ours rather than the user's.
         *
         * Mirrors the launcher's own DESK_ACTION_3D branch exactly, including
         * raycast_open() -- an open path that skipped that reset is what made a
         * reopened view spend its first seconds inside a wall, and a diagnostic
         * that opens the view differently from the real thing would be
         * measuring a different bug. */
        if (str_eq(arg, "off")) {
            desktop_set_active(1);
            uart_puts("   back to the launcher\n");
        } else {
            raycast_open();
            desktop_set_active(0);
            uart_puts("   3D view open\n");
        }
    }
    else if (str_eq(line, "camfreeze")) {
        raycast_cam_freeze(!str_eq(arg, "off"));
        uart_puts(raycast_cam_frozen()
                  ? "   camera held still; the renderer keeps drawing\n"
                  : "   camera walking again\n");
    }
    else if (str_eq(line, "campos")) {
        /* Where the camera actually is, and whether it is inside a wall.
         *
         * fbsum established that the framebuffer differs between the garbled
         * and the good state, which puts the fault in the renderer rather than
         * the panel. This is the next question down: the flat-colour signature
         * is what a camera inside geometry produces, but "flat" was read off
         * the pixels. Read it off the map instead. */
        uart_puts("   cell ");
        uart_put_dec(raycast_cam_x());
        uart_puts(",");
        uart_put_dec(raycast_cam_y());
        uart_puts("  frac ");
        uart_put_dec(raycast_cam_frac_x());
        uart_puts(",");
        uart_put_dec(raycast_cam_frac_y());
        uart_puts("/1000  heading ");
        uart_put_dec(raycast_heading());
        uart_puts("  angle ");
        uart_put_dec(raycast_angle());
        uart_puts(raycast_cam_in_wall() ? "\n   INSIDE A WALL\n"
                                        : "\n   in open space\n");
        uart_puts("   frames ");
        uart_put_dec(raycast_frames());
        uart_puts("  columns ");
        uart_put_dec(raycast_columns());
        uart_puts("\n");
    }
    else if (str_eq(line, "dmastat")) {
        /* The DMA counters, read while the system is RUNNING.
         *
         * They were only ever printed by display_init()'s boot report, which
         * runs before the scheduler starts -- so the reassuring dma=N/0 was
         * measured in the single condition where no task can be preempted, and
         * was taken as evidence that timeouts do not happen. It is evidence
         * that they do not happen at boot.
         *
         * A timeout is not a dropped frame. It disables DMA for the rest of the
         * run and every transfer after it falls back to the FIFO, so one trip
         * changes the behaviour of everything that draws, permanently. */
        uart_puts("   transfers ");
        uart_put_dec(display_dma_transfers());
        uart_puts("  timeouts ");
        uart_put_dec(display_dma_timeouts());
        /* Report the FLAG, not the timeout count.
         *
         * This line used to infer the transport from display_dma_timeouts(),
         * which is only one of the two ways DMA gets switched off -- `dmaoff`
         * is the other. So after forcing the FIFO path by hand it cheerfully
         * printed "DMA still active", which is the exact failure this file
         * catalogues elsewhere: a diagnostic reporting what it assumed rather
         * than what is true. It nearly invalidated a transport A/B test. */
        uart_puts(display_dma_enabled() ? "  (DMA active)\n"
                                        : "  <- DMA is OFF, everything is on the FIFO path\n");
        if (!display_dma_enabled() && !display_dma_timeouts()) {
            uart_puts("   (forced off by hand, not by a timeout)\n");
        }
    }
    else if (str_eq(line, "hog")) {
        /* An application's SCHEDULING, with none of its drawing.
         *
         * resyncn and stripn put byte-identical traffic on the wire and changed
         * nothing, which clears the panel, the bus, the DMA engine and the
         * controller's window state. So whatever gfxrogue does to repair the
         * view, it does not do it by drawing -- and the difference between
         * gfxrogue and stripn is not the pixels, it is who emits them.
         *
         * task_apps does not sleep while an application is live:
         *
         *     for (;;) { app_tick(2000);
         *                if (app_live_count() == 0) task_yield(); }
         *
         * so a running program turns that task into a permanent CPU hog at
         * NORMAL priority, and ageing promotes it over the HIGH-priority
         * display task every TASK_AGE_TICKS. stripn ran in the shell task,
         * which yields on every poll -- it reproduced the drawing exactly and
         * the scheduling not at all.
         *
         * This is the other half. It spins the same way and touches nothing:
         * no display, no SPI, no memory outside its own counter. If a garbled
         * view comes good with this running, the repair is scheduling and the
         * drawing was never relevant. */
        char *width = split(arg);
        int w = parse_int(width);
        g_hog_w    = (w > 0 && w <= (int)APP_VIEW_W) ? (uint32_t)w : APP_VIEW_W;
        g_hog_on   = !str_eq(arg, "off");
        g_hog_draw = str_eq(arg, "draw");
        if (g_hog_on && g_hog_task < 0) {
            g_hog_task = task_create("hog", hog_task);
            if (g_hog_task >= 0) {
                task_set_priority(g_hog_task, TASK_PRIO_NORMAL);
            }
        }
        if (g_hog_task < 0) {
            uart_puts("   could not create the task\n");
        } else if (!g_hog_on) {
            uart_puts("   yielding again, ");
            uart_put_dec(g_hog_fills);
            uart_puts(" fills issued in total\n");
        } else if (g_hog_draw) {
            uart_puts("   spinning AND filling continuously, width ");
            uart_put_dec(g_hog_w);
            uart_puts(g_hog_w * 2u > 64u ? " px -> DMA path\n"
                                         : " px -> FIFO path\n");
        } else {
            uart_puts("   spinning at NORMAL priority, drawing nothing"
                      " ('hog draw' to also fill)\n");
        }
    }
    else if (str_eq(line, "resyncn")) {
        /* A BURST of window setups. `resync` issues one and changed nothing,
         * but gfxrogue -- the one program that reliably repairs the view --
         * does not issue one either. It issues a few hundred a second, forever,
         * because every display_fill_rect() begins with a fresh CASET/PASET/
         * RAMWR. Nothing else in the system re-establishes the window at that
         * rate: the raycaster sets it ONCE per 224 DMA bursts.
         *
         * So the quantity under test is frequency, not occurrence. This is the
         * pure form of it -- window setups and CS cycles with not one pixel
         * written -- which separates "the controller's idea of where pixels go
         * needs refreshing often" from "something about the pixel writes". */
        int n = parse_int(arg);
        if (n < 1) { n = 500; }
        for (int i = 0; i < n; i++) {
            display_resync();
        }
        uart_puts("   ");
        uart_put_dec((unsigned int)n);
        uart_puts(" window setups issued; no pixels drawn\n");
    }
    else if (str_eq(line, "stripn")) {
        /* gfxrogue's draw, without gfxrogue.
         *
         * Same rectangle its fill clips down to (180x14 at y=224, the slot-0
         * strip), same alternating red and white, issued directly from the
         * shell. No VM, no scheduler slot, no application lifecycle -- just the
         * pixels and the window setups that accompany them.
         *
         * Paired with resyncn this pins the mechanism down. If stripn repairs a
         * garbled view and resyncn does not, the repair needs the pixel writes
         * and the DMA bursts that carry them. If resyncn repairs it too, the
         * window setup alone is enough. If NEITHER repairs it and gfxrogue
         * still does, then it is not the drawing at all and the answer is
         * somewhere in the app path -- scheduling, timing, or the display task
         * being interrupted. */
        int n = parse_int(arg);
        if (n < 1) { n = 200; }
        for (int i = 0; i < n; i++) {
            display_fill_rect(0, APP_VIEW_Y0, APP_VIEW_W, APP_VIEW_H,
                              (i & 1) ? COLOR_WHITE : COLOR_RED);
        }
        uart_puts("   ");
        uart_put_dec((unsigned int)n);
        uart_puts(" gfxrogue-shaped fills issued (");
        uart_put_dec(APP_VIEW_W);
        uart_puts("x");
        uart_put_dec(APP_VIEW_H);
        uart_puts(" at y=");
        uart_put_dec(APP_VIEW_Y0);
        uart_puts(")\n");
    }
    else if (str_eq(line, "dfreeze")) {
        extern volatile int g_display_frozen;
        g_display_frozen = !str_eq(arg, "off");
        uart_puts(g_display_frozen ? "   display task frozen; nothing repaints\n"
                                   : "   display task running\n");
    }
    else if (str_eq(line, "dev")) {
        /* The device table from the terminal.
         *
         *   dev                     list every device
         *   dev <id> <chan>         read
         *   dev <id> <chan> <value> write
         *
         * app_dev.vasm proved a PROGRAM can reach a peripheral through the
         * model, which was the point of building it. It is a poor way to ask a
         * one-off question though: it has to be launched, it occupies a slot,
         * and it polls forever to answer something asked once. The table is a
         * kernel structure and the shell can read it directly.
         *
         * Deliberately the same entry points -- device_read/device_write, not
         * the drivers underneath. A diagnostic that bypassed the table would
         * report on a path no application can take, which is how a self-test
         * ends up passing for a broken system. */
        char *cid   = arg;
        char *cchan = split(cid);
        char *cval  = split(cchan);

        if (!*cid) {
            /* The last column samples CHANNEL 0, which is not always a
             * meaningful thing to read: i2c's channel 0 is a reserved bus
             * address and refuses by design. Labelled `ch0` rather than `value`
             * so a correct refusal does not read as a broken device. */
            uart_puts("   id  name      chans  flags     ch0\n");
            for (uint32_t i = 0; i < (uint32_t)device_count(); i++) {
                uint32_t chans = 0, flags = 0;
                device_info(i, &chans, &flags);
                uart_puts("   ");
                uart_put_dec(i);
                uart_puts("   ");
                const char *nm = device_name(i);
                uint32_t w = 0;
                while (nm && nm[w]) { uart_putc(nm[w]); w++; }
                while (w < 10u)     { uart_putc(' ');   w++; }
                uart_put_dec(chans);
                uart_puts("      ");
                uart_putc((flags & DEV_F_READ)    ? 'r' : '-');
                uart_putc((flags & DEV_F_WRITE)   ? 'w' : '-');
                uart_putc((flags & DEV_F_SLOW)    ? 's' : '-');
                uart_putc((flags & DEV_F_CONSUME) ? 'c' : '-');
                uart_putc((flags & DEV_F_XFER)    ? 'x' : '-');
                uart_puts("     ");
                /* Never sample a device whose read CONSUMES. Listing the table
                 * used to pop a keypress off `keys`, which is a diagnostic
                 * quietly altering the thing it reports. */
                if (flags & DEV_F_CONSUME) {
                    uart_puts("(consumes)");
                } else if (flags & DEV_F_READ) {
                    uint32_t v = 0;
                    if (device_read(DEVICE_CALLER_KERNEL, i, 0, &v)) {
                        uart_put_dec(v);
                    } else {
                        uart_puts("refused");
                    }
                } else {
                    uart_puts("-");
                }
                uart_puts("\n");
            }
            uart_puts("   reads=");
            uart_put_dec(device_reads());
            uart_puts(" writes=");
            uart_put_dec(device_writes());
            uart_puts(" refusals=");
            uart_put_dec(device_refusals());
            uart_puts("\n   usage: dev <id> <chan> [value]\n");
        } else {
            int id   = parse_int(cid);
            int chan = *cchan ? parse_int(cchan) : 0;
            if (id < 0 || chan < 0) {
                uart_puts("   usage: dev <id> <chan> [value]\n");
            } else if (*cval) {
                int v = parse_int(cval);
                if (v < 0) {
                    uart_puts("   value must be a non-negative decimal\n");
                } else {
                    uart_puts(device_write(DEVICE_CALLER_KERNEL, (uint32_t)id,
                                           (uint32_t)chan, (uint32_t)v)
                              ? "   written\n" : "   refused\n");
                }
            } else {
                uint32_t v = 0;
                if (device_read(DEVICE_CALLER_KERNEL, (uint32_t)id,
                                (uint32_t)chan, &v)) {
                    uart_puts("   ");
                    uart_put_dec(v);
                    uart_puts("\n");
                } else {
                    uart_puts("   refused\n");
                }
            }
        }
    }
    else if (str_eq(line, "perms")) {
        /* What each running application may touch, and the ability to change it
         * while it runs.
         *
         *   perms                     list every live application
         *   perms <app> <dev> on|off  grant or revoke one device
         *
         * The listing exists because a capability nobody can see is a capability
         * nobody audits. The grants come from the PROGRAMS table, which is a
         * source file -- so without this, checking what a running program holds
         * means reading kmain.c and trusting that the build on the board matches
         * it. This asks the kernel.
         *
         * The mutator exists to make refusals TESTABLE: revoke a device from a
         * program that is using it and the denial counter moves while the
         * program keeps running, which is the whole claim this feature makes.
         *
         * Note what is deliberately absent: a program cannot grant itself
         * anything. There is no `sys device` operation that reaches
         * device_grant(). Every grant comes from the kernel side -- this command
         * or the launch table. */
        char *capp = arg;
        char *cdev = split(capp);
        char *cset = split(cdev);

        if (!*capp) {
            uart_puts("   app  name        devices\n");
            for (int id = 0; id < APP_MAX; id++) {
                if (app_state(id) != APP_RUNNING) {
                    continue;
                }
                uart_puts("   ");
                uart_put_dec((unsigned int)id);
                uart_puts("    ");
                const char *nm = app_name(id);
                uint32_t w = 0;
                while (nm && nm[w]) { uart_putc(nm[w]); w++; }
                while (w < 12u)     { uart_putc(' ');   w++; }

                uint32_t p = device_perms((uint32_t)id);
                if (!p) {
                    uart_puts("(none)");
                } else {
                    /* Names, not a hex mask. A mask is exactly the kind of thing
                     * that gets misread on the wrong day. */
                    int first = 1;
                    for (uint32_t d = 0; d < (uint32_t)device_count(); d++) {
                        if (!((p >> d) & 1u)) {
                            continue;
                        }
                        if (!first) { uart_puts(" "); }
                        first = 0;
                        const char *dn = device_name(d);
                        while (dn && *dn) { uart_putc(*dn++); }
                    }
                }
                uart_puts("\n");
            }
            uart_puts("   denials=");
            uart_put_dec(device_denials());
            uart_puts("\n   usage: perms <app> <dev> on|off\n");
        } else if (!*cdev || !*cset) {
            uart_puts("   usage: perms <app> <dev> on|off\n");
        } else {
            int aid = parse_int(capp);
            int did = parse_int(cdev);
            int on  = str_eq(cset, "on");
            if (aid < 0 || aid >= APP_MAX || did < 0 || did >= device_count()) {
                uart_puts("   no such application or device\n");
            } else if (!on && !str_eq(cset, "off")) {
                uart_puts("   last argument must be on or off\n");
            } else {
                uint32_t p = device_perms((uint32_t)aid);
                p = on ? (p | (1u << did)) : (p & ~(1u << did));
                device_grant((uint32_t)aid, p);
                uart_puts(on ? "   granted " : "   revoked ");
                const char *dn = device_name((uint32_t)did);
                while (dn && *dn) { uart_putc(*dn++); }
                uart_puts(" for app ");
                uart_put_dec((unsigned int)aid);
                uart_puts("\n");
            }
        }
    }
    else if (str_eq(line, "light")) {
        /* One reading, and a beep if it is dark.
         *
         * This was app_dev's resident loop, which seized the speaker and beeped
         * at the room whenever a shadow fell across the board. A demo should
         * demonstrate and then get out of the way; a thing you want on demand
         * belongs at the prompt.
         *
         * Both halves go through the device table, so this exercises the same
         * path an application takes rather than a private shortcut. */
        char *cth = arg;
        int threshold = *cth ? parse_int(cth) : 400;
        uint32_t v = 0;
        if (!device_read(DEVICE_CALLER_KERNEL, 0u, 0u, &v)) {
            uart_puts("   light sensor refused\n");
        } else {
            uart_puts("   light = ");
            uart_put_dec(v);
            if (threshold > 0 && v < (uint32_t)threshold) {
                uint32_t packed = (1000u << 16) | 20u;
                device_write(DEVICE_CALLER_KERNEL, 1u, 0u, packed);
                uart_puts("  (dark, below ");
                uart_put_dec((unsigned int)threshold);
                uart_puts(" -- beeped)");
            }
            uart_puts("\n   usage: light [threshold]\n");
        }
    }
    else if (str_eq(line, "keys")) {
        /* Drain whatever the on-panel keypad has settled.
         *
         * Reading is destructive and the queue is shared, so this command
         * COMPETES with any application polling for keys. That is worth knowing
         * rather than hiding: it is the same arrangement the touchscreen has,
         * and this kernel has no concept of focus to arbitrate it. */
        uint32_t pending = 0;
        device_read(DEVICE_CALLER_KERNEL, 4u, 1u, &pending);
        uart_puts("   pending ");
        uart_put_dec(pending);
        uart_puts("  dropped ");
        uart_put_dec(term_keys_dropped());
        uart_puts("  queued-ever ");
        uart_put_dec(term_keys_queued());
        uart_puts("\n   ");
        if (!pending) {
            uart_puts("(nothing typed on the panel keypad)");
        }
        uint32_t ch = 0, n = 0;
        while (device_read(DEVICE_CALLER_KERNEL, 4u, 0u, &ch) && n < 64u) {
            uart_putc('[');
            uart_putc((char)ch);
            uart_putc(']');
            n++;
        }
        uart_puts("\n");
    }
    else if (str_eq(line, "devw")) {
        /* devw <id> <chan> <byte> [byte] [byte]  -- through DEV_OP_XFER_OUT.
         *
         * Takes a device id rather than being I2C-specific, because the point
         * of the model is that the transport is a table entry. Three bytes is
         * deliberate: this is a diagnostic for the transfer PATH, and a shell
         * command parsing an arbitrary byte list would be more parser than
         * test. */
        char *cid = arg;
        char *cch = split(cid);
        char *c1  = split(cch);
        char *c2  = split(c1);
        char *c3  = split(c2);
        int id   = parse_int(cid);
        int chan = *cch ? parse_int(cch) : -1;
        uint8_t buf[3];
        uint32_t n = 0;
        int bad = 0;
        if (*c1) { int v = parse_int(c1); if (v < 0 || v > 255) { bad = 1; } else { buf[n++] = (uint8_t)v; } }
        if (*c2) { int v = parse_int(c2); if (v < 0 || v > 255) { bad = 1; } else { buf[n++] = (uint8_t)v; } }
        if (*c3) { int v = parse_int(c3); if (v < 0 || v > 255) { bad = 1; } else { buf[n++] = (uint8_t)v; } }

        if (id < 0 || chan < 0 || n == 0u || bad) {
            uart_puts("   usage: devw <id> <chan> <byte> [byte] [byte]\n");
        } else {
            uart_puts(device_xfer_out(DEVICE_CALLER_KERNEL, (uint32_t)id,
                                      (uint32_t)chan, buf, n)
                      ? "   written\n" : "   refused\n");
        }
    }
    else if (str_eq(line, "devr")) {
        /* devr <id> <chan> <count>  -- through DEV_OP_XFER_IN. */
        char *cid = arg;
        char *cch = split(cid);
        char *cn  = split(cch);
        int id   = parse_int(cid);
        int chan = *cch ? parse_int(cch) : -1;
        int want = *cn  ? parse_int(cn)  : 1;
        uint8_t buf[DEVICE_XFER_MAX];

        if (id < 0 || chan < 0 || want <= 0 || want > (int)DEVICE_XFER_MAX) {
            uart_puts("   usage: devr <id> <chan> <count 1..64>\n");
        } else if (!device_xfer_in(DEVICE_CALLER_KERNEL, (uint32_t)id,
                                   (uint32_t)chan, buf, (uint32_t)want)) {
            uart_puts("   refused\n");
        } else {
            uart_puts("  ");
            for (int i = 0; i < want; i++) {
                uart_puts(" ");
                uart_put_hex(buf[i]);
            }
            uart_puts("\n");
        }
    }
    else if (str_eq(line, "keyinject")) {
        /* keyinject <char> [count]  -- type without fingers.
         *
         * The keypad needs a person, the terminal view, and correct multi-tap
         * timing. Anything downstream of it -- the `keys` device, the VM event
         * path -- would otherwise be testable only by asking somebody to tap,
         * which is three ways for a test to fail for reasons unrelated to what
         * it is testing. One of them already did. */
        char *cc = arg;
        char *cn = split(cc);
        int n = *cn ? parse_int(cn) : 1;
        if (!*cc || n < 1 || n > 16) {
            uart_puts("   usage: keyinject <char> [count 1..16]\n");
        } else {
            for (int i = 0; i < n; i++) {
                term_key_inject((uint32_t)(uint8_t)cc[0]);
            }
            uart_puts("   injected ");
            uart_put_dec((unsigned int)n);
            uart_puts(" x '");
            uart_putc(cc[0]);
            uart_puts("'\n");
        }
    }
    else if (str_eq(line, "i2cscan")) {
        /* Sweep the bus THROUGH THE DEVICE TABLE.
         *
         * i2c.c already has i2c_scan(), which prints its own report and talks
         * to the driver directly. This one reads device 3 channel by channel,
         * exactly as an application would, so what it reports is what a program
         * would find. A scanner that used a private path could agree with the
         * hardware and still disagree with every program on the board. */
        uart_puts("   sweeping 0x08..0x77 through device 3\n");
        int found = 0;
        for (uint32_t a = 8u; a <= 119u; a++) {
            uint32_t present = 0;
            if (device_read(DEVICE_CALLER_KERNEL, 3u, a, &present) && present) {
                uart_puts("   0x");
                uart_put_hex(a);
                uart_puts("  answered\n");
                found++;
            }
        }
        if (!found) {
            uart_puts("   nothing answered. With no pull-ups fitted SDA floats"
                      " high and every\n   address can appear to answer, so a"
                      " silent bus is the honest result\n   for an empty"
                      " header.\n");
        }
    }
    else if (str_eq(line, "vmargtest")) {
        /* Drive known-BAD arguments through the harness and require each to be
         * refused, with the right fault code.
         *
         * A harness that has never rejected anything and a harness that is
         * never reached look identical from outside, and this kernel has been
         * caught by that shape before -- an audio self-check that could not fail
         * by construction (UM-NATOS-027), a viewport counter that only moved in
         * one drawing mode (UM-NATOS-028). The cases below are the ones the four
         * hand-written sites were each reasoned about individually to survive.
         *
         * Runs against a fabricated vm_t over a local buffer: no arena, no
         * application, nothing to clean up, and it can be run at any time. */
        static uint8_t sandbox[64];
        vm_t t;
        for (uint32_t i = 0; i < sizeof sandbox; i++) {
            sandbox[i] = (uint8_t)(i + 1u);     /* no NUL until forced below */
        }
        t.base = (uint32_t)sandbox;
        t.size = sizeof sandbox;
        t.pc   = 0;

        vm_span_t s;
        char sbuf[16];
        int pass = 0, total = 0;

        #define WANT(desc, expr, expect_ok, expect_fault)                      \
            do {                                                              \
                total++;                                                      \
                t.fault = VM_FAULT_NONE;                                      \
                int r = (expr);                                               \
                int good = (r == (expect_ok)) &&                              \
                           (t.fault == (expect_fault));                       \
                if (good) { pass++; }                                         \
                uart_puts(good ? "   ok   " : "   FAIL ");                    \
                uart_puts(desc);                                              \
                if (!good) {                                                  \
                    uart_puts("  (rc=");                                      \
                    uart_put_dec((unsigned int)r);                            \
                    uart_puts(" fault=");                                     \
                    uart_puts(vm_fault_name(t.fault));                        \
                    uart_puts(")");                                           \
                }                                                             \
                uart_puts("\n");                                              \
            } while (0)

        WANT("span inside the arena is accepted",
             vmarg_span(&t, 0, 64, 1, &s), 1, VM_FAULT_NONE);
        WANT("span one byte past the end is refused",
             vmarg_span(&t, 0, 65, 1, &s), 0, VM_FAULT_BOUNDS);
        WANT("offset past the end is refused",
             vmarg_span(&t, 65, 1, 1, &s), 0, VM_FAULT_BOUNDS);
        WANT("off+len WRAPPING is refused (the whole point)",
             vmarg_span(&t, 0xFFFFFFF0u, 0x20u, 1, &s), 0, VM_FAULT_BOUNDS);
        WANT("misaligned offset is refused",
             vmarg_span(&t, 3, 4, 4, &s), 0, VM_FAULT_ALIGN);
        WANT("items within the ceiling are accepted",
             vmarg_items(&t, 0, 16, 2, 32, 2, &s), 1, VM_FAULT_NONE);
        WANT("count over the ceiling is refused BEFORE multiplying",
             vmarg_items(&t, 0, 0x80000000u, 2, 32, 2, &s), 0, VM_FAULT_BOUNDS);
        WANT("count*elem that would wrap is refused",
             vmarg_items(&t, 0, 0x40000001u, 4, 1024, 1, &s), 0, VM_FAULT_BOUNDS);
        /* NA-004. The case a GENEROUS ceiling used to let through: count is
         * under max_items, so the check above passes, and count*elem then wraps
         * to something small that fits the arena. Before the guard this
         * returned a valid 4-byte span for a request of 4 GB. */
        WANT("count under a huge ceiling that STILL wraps is refused",
             vmarg_items(&t, 0, 0x40000001u, 4, 0xFFFFFFFFu, 1, &s), 0,
             VM_FAULT_BOUNDS);
        WANT("zero count yields an empty span, not a fault",
             vmarg_items(&t, 0, 0, 4, 16, 1, &s), 1, VM_FAULT_NONE);
        WANT("unterminated string running off the end is refused",
             vmarg_string(&t, 56, sbuf, sizeof sbuf), 0, VM_FAULT_BOUNDS);

        sandbox[10] = 0;                /* now there IS a terminator */
        WANT("terminated string is accepted",
             vmarg_string(&t, 0, sbuf, sizeof sbuf), 1, VM_FAULT_NONE);
        WANT("over-long string truncates rather than faulting",
             vmarg_string(&t, 16, sbuf, 8), 1, VM_FAULT_NONE);
        #undef WANT

        uart_puts("   ");
        uart_put_dec((unsigned int)pass);
        uart_puts("/");
        uart_put_dec((unsigned int)total);
        uart_puts(" passed   checks=");
        uart_put_dec(vmarg_checks());
        uart_puts(" rejects=");
        uart_put_dec(vmarg_rejects());
        uart_puts(vmarg_rejects() ? "\n" : "  <- ZERO REJECTS, the test is inert\n");
    }
    else if (str_eq(line, "spi3")) {
        /* Bring up SPI3, with no radio attached and nothing wired.
         *
         * Phase 0 of the Ark idea is "two boards, an SX1262, one packet", and
         * the temptation is to write the radio driver first and find out
         * everything at once. That is how M2 cost nine build cycles: bring up
         * four uncertain things together and a black screen tells you nothing
         * about which one is wrong.
         *
         * So the peripheral goes first, alone, and it is verified without a
         * radio at all. The GPIO matrix can tie a peripheral input to a
         * constant, which gives two tests that drive no pin:
         *
         *   MISO tied high -> every byte back must be 0xFF
         *   MISO tied low  -> every byte back must be 0x00
         *
         * BOTH have to pass. Either alone is worthless: a driver that never
         * reads anything returns a constant and would sail through whichever
         * test happened to expect that constant. This is the "one counter that
         * must be non-zero beside one that must be zero" shape, applied to a
         * bus.
         *
         * The pattern sent is 5A A5 00 FF 12 34 56 78, chosen so that a driver
         * handing back its own transmit buffer fails both halves rather than
         * accidentally matching.
         *
         *   spi3            the two constant tests
         *   spi3 loop [pin] additionally loop MOSI back into MISO on one pad
         *
         * The loopback is the first test that proves a bit actually left the
         * chip. It needs a pin it is safe to drive; 27 is broken out on this
         * board and claimed by nothing in the kernel. */
        spi3_init();

        int hi = spi3_selftest_const(1);
        int lo = spi3_selftest_const(0);

        uart_puts("   MISO tied high -> ");
        uart_puts(hi ? "0xFF as expected  PASS\n" : "WRONG  FAIL\n");
        uart_puts("   MISO tied low  -> ");
        uart_puts(lo ? "0x00 as expected  PASS\n" : "WRONG  FAIL\n");

        if (hi && lo) {
            uart_puts("   peripheral, clock gate, W registers and capture path\n"
                      "   all live. Nothing has left the chip yet.\n");
        } else {
            uart_puts("   SPI3 is not reading. Everything below is meaningless\n"
                      "   until this passes.\n");
        }

        if (str_eq(arg, "loop") || (*arg && !str_eq(arg, "loop"))) {
            /* Split BEFORE comparing. `arg` is the whole tail, so for
             * "spi3 loop 27" it is "loop 27" -- comparing that against
             * "loop" fails, cpin never advanced, and parse_int() got a
             * word instead of a number. The guard below then never ran
             * and the usage line printed instead, which looked like the
             * pin had been rejected when it had merely been misread. */
            char *cpin = arg;
            char *rest = split(cpin);
            if (str_eq(cpin, "loop")) {
                cpin = rest;
            }
            int pin = *cpin ? parse_int(cpin) : (int)BOARD_SPARE_PIN;
            if (pin < 0 || pin > 39) {
                uart_puts("   usage: spi3 loop [pin]\n");
            } else if (board_pin_claimed((uint32_t)pin)) {
                /* Refuse rather than drive it. This defaulted to 27,
                 * which is I2C SCL on this board -- the loopback left
                 * SCL driven by the SPI peripheral, `i2c` then reported
                 * "bus is NOT sane", and it looked exactly like a
                 * regression from the board refactor landing in the
                 * same hour. The board header knows what is spoken
                 * for, so ask it instead of picking and finding out. */
                uart_puts("   GPIO");
                uart_put_dec((unsigned int)pin);
                uart_puts(" is claimed by this board (");
                uart_puts(board_pin_owner((uint32_t)pin));
                uart_puts("). Driving it would break that.\n");
            } else {
                int ok = spi3_selftest_loopback((uint8_t)pin);
                uart_puts("   loopback on GPIO");
                uart_put_dec((unsigned int)pin);
                uart_puts(ok ? " -> pattern returned intact  PASS\n"
                             : " -> pattern did NOT return  FAIL\n");
                if (ok) {
                    uart_puts("   a byte left the chip and came back. The bus works.\n");
                }
            }
        }

        uart_puts("   transfers=");
        uart_put_dec(spi3_transfers());
        uart_puts(" timeouts=");
        uart_put_dec(spi3_timeouts());
        uart_puts("\n");
    }
    else if (str_eq(line, "spidump")) {
        /* Read the SPI2 configuration back off the chip.
         *
         * Every theory about why the DMA path displaces pixels has been argued
         * from what the code WRITES. This prints what the peripheral actually
         * HOLDS, which is the only version that matters -- and this driver has
         * already been caught twice writing a real but wrong neighbouring bit
         * that the hardware accepted without complaint (UM-NATOS-030's
         * OUTLINK_START, and DMA_OUT_RST landing on the inbound channel).
         *
         * Expected, after spi2_dma_init():
         *   DMA_CONF  bits 10 and 12 set (OUTDSCR_BURST, OUT_DATA_BURST),
         *             nothing else -- in particular NOT bit 16 (DMA_CONTINUE),
         *             which would make a transfer restart itself and is the one
         *             bit that would explain an offset growing per transfer.
         *   USER      USR_MOSI set; USR_COMMAND / USR_ADDR / USR_DUMMY clear,
         *             since any of those prepend bits to every transaction. */
        uart_puts("   DMA_CONF  = "); uart_put_hex(GPIO_REG(0x3FF64100u));
        uart_puts("\n   USER      = "); uart_put_hex(GPIO_REG(0x3FF6401Cu));
        uart_puts("\n   USER1     = "); uart_put_hex(GPIO_REG(0x3FF64020u));
        uart_puts("\n   USER2     = "); uart_put_hex(GPIO_REG(0x3FF64024u));
        uart_puts("\n   CTRL      = "); uart_put_hex(GPIO_REG(0x3FF64008u));
        uart_puts("\n   CTRL2     = "); uart_put_hex(GPIO_REG(0x3FF64014u));
        uart_puts("\n   CLOCK     = "); uart_put_hex(GPIO_REG(0x3FF64018u));
        uart_puts("\n   MOSI_DLEN = "); uart_put_hex(GPIO_REG(0x3FF64028u));
        uart_puts("\n   PIN       = "); uart_put_hex(GPIO_REG(0x3FF64034u));
        uart_puts("\n   DMA_INT_RAW = "); uart_put_hex(GPIO_REG(0x3FF64114u));
        uart_puts("\n");
    }
    else if (str_eq(line, "dmaoff")) {
        /* One variable: the transport. `dmaoff` puts every transfer on the
         * FIFO, `dmaoff on` gives DMA back. Nothing else changes -- same
         * framebuffer, same window, same renderer, same everything. */
        display_force_fifo(!str_eq(arg, "on"));
        uart_puts(display_dma_enabled() ? "   DMA path enabled\n"
                                        : "   FIFO path forced; DMA disabled\n");
    }
    else if (str_eq(line, "fbpattern")) {
        /* A KNOWN image, through the identical blit path.
         *
         * "Garbled" is a description; a displacement is a number. Eight colour
         * bands in a fixed order, twenty-eight rows each, with a white square
         * in the top-left corner: if the bands come back in the wrong order the
         * stream is being reordered, if they are shifted the window or the byte
         * count is off, and if the square is not in the corner the origin is
         * wrong. Any of those is measurable by describing the panel, which is
         * the only instrument available now that MISO has been shown dead.
         *
         * Run `dfreeze` first so the renderer does not immediately paint over
         * it. This writes into the raycaster's own framebuffer and calls the
         * same display_blit() the renderer calls, so nothing about the path
         * under test is being substituted -- a pattern drawn some other way
         * would be testing some other code. */
        uint16_t *fb = raycast_fb_ptr();
        if (!fb) {
            uart_puts("   no framebuffer (fb must be on)\n");
        } else {
            static const uint16_t BANDS[8] = {
                COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW,
                COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE, COLOR_GREY
            };
            for (uint32_t y = 0; y < RAY_VIEW_H; y++) {
                uint16_t c = BANDS[(y / 28u) & 7u];
                for (uint32_t x = 0; x < RAY_VIEW_W; x++) {
                    fb[y * RAY_VIEW_W + x] = c;
                }
            }
            /* Corner marker: black square, top-left, 16x16. */
            for (uint32_t y = 0; y < 16u; y++) {
                for (uint32_t x = 0; x < 16u; x++) {
                    fb[y * RAY_VIEW_W + x] = COLOR_BLACK;
                }
            }
            display_blit(0, 0, RAY_VIEW_W, RAY_VIEW_H, fb, RAY_VIEW_W);
            uart_puts("   pattern blitted: 8 bands of 28 rows, top to bottom --\n"
                      "   RED GREEN BLUE YELLOW CYAN MAGENTA WHITE GREY,\n"
                      "   black 16x16 square in the TOP-LEFT corner\n");
        }
    }
    else if (str_eq(line, "fbdump")) {
        /* The framebuffer itself, over the wire, as an image.
         *
         * Every render-vs-transport test so far has failed the same way: the
         * only instrument that can see the PICTURE is a person, and the only
         * instrument that can see the BUFFER reports summaries. fbhash proves
         * two frames are identical; it cannot say whether either is a maze or
         * noise, and a frozen camera stably rendering a wrong scene produces a
         * perfectly constant hash. MISO is dead so the panel cannot be read
         * back. That leaves sending the buffer out and looking at it.
         *
         * Every second pixel and every second row: 120x112, small enough to
         * cross a 115200 line in a few seconds and far more than enough to tell
         * a rendered corridor from garbage. */
        uint16_t *fb = raycast_fb_ptr();
        if (!fb) {
            uart_puts("   no framebuffer\n");
        } else {
            /* Held across the WHOLE dump.
             *
             * The reporter prints a status line every couple of hundred ticks.
             * Without the lock it lands in the middle of the hex, shifts a row,
             * and the image reconstructed on the host acquires colour garbage
             * that came from the console rather than from the framebuffer --
             * an instrument manufacturing the very fault it was built to
             * photograph. The first dump taken without this looked convincingly
             * corrupt and could not be trusted for exactly that reason.
             *
             * Each row is also prefixed with its own index, so the host can
             * verify it received the row it thinks it did rather than inferring
             * position from arrival order. */
            static const char HEX[] = "0123456789abcdef";
            console_lock();
            uart_puts("FBDUMP 120 112\n");
            for (uint32_t y = 0; y < RAY_VIEW_H; y += 2u) {
                uart_put_dec(y / 2u);
                uart_putc(':');
                for (uint32_t x = 0; x < RAY_VIEW_W; x += 2u) {
                    uint16_t p = fb[y * RAY_VIEW_W + x];
                    uart_putc(HEX[(p >> 12) & 0xFu]);
                    uart_putc(HEX[(p >> 8)  & 0xFu]);
                    uart_putc(HEX[(p >> 4)  & 0xFu]);
                    uart_putc(HEX[p & 0xFu]);
                }
                uart_putc('\n');
            }
            uart_puts("FBEND\n");
            console_unlock();
        }
    }
    else if (str_eq(line, "fbsum")) {
        /* What is actually IN the framebuffer, sampled without touching the
         * panel. The renderer runs at full rate while the picture is wrong, and
         * transport is proven clean, so the remaining question is whether the
         * buffer holds a rendered scene or something else. */
        uint16_t *fb = raycast_fb_ptr();
        if (!fb) {
            uart_puts("   no framebuffer\n");
        } else {
            uint32_t n = RAY_VIEW_W * RAY_VIEW_H;
            uint32_t zero = 0, same = 0;
            uint16_t first = fb[0];
            /* FNV-1a over every pixel, because the counts above are a SAMPLE
             * and two different pictures can share them. Comparing summaries
             * across a state change and calling them identical is exactly the
             * mistake this whole investigation has been making; a hash over all
             * 53,760 pixels either matches or it does not. */
            uint32_t hash = 2166136261u;
            for (uint32_t i = 0; i < n; i++) {
                if (fb[i] == 0u)     { zero++; }
                if (fb[i] == first)  { same++; }
                hash = (hash ^ (uint32_t)(fb[i] & 0xFFu)) * 16777619u;
                hash = (hash ^ (uint32_t)(fb[i] >> 8))    * 16777619u;
            }
            uart_puts("   fbhash=");
            uart_put_hex(hash);
            uart_puts("  pixels=");
            uart_put_dec(n);
            uart_puts("  zero=");
            uart_put_dec(zero);
            uart_puts("  equal-to-first=");
            uart_put_dec(same);
            /* Does any application's arena overlap the live framebuffer?
             *
             * The framebuffer is a 107 KB heap allocation and an arena is
             * another heap allocation. If the allocator ever hands out a block
             * that overlaps the framebuffer, a program writing to its OWN
             * memory scribbles straight into the rendered image -- which is
             * what a garbled view that depends on which programs are running
             * would look like. Unverified either way; this reports it. */
            uart_puts("\n   fb spans ");
            uart_put_hex((uint32_t)fb);
            uart_puts(" .. ");
            uart_put_hex((uint32_t)fb + n * 2u);
            uart_puts("\n");
            for (int id = 0; id < APP_MAX; id++) {
                if (app_state(id) == APP_FREE) {
                    continue;
                }
                uint32_t b = app_arena_base(id);
                uint32_t e = b + app_arena_bytes(id);
                int overlap = (b < (uint32_t)fb + n * 2u) && (e > (uint32_t)fb);
                uart_puts("   app ");
                uart_put_dec((unsigned)id);
                uart_puts(" arena ");
                uart_put_hex(b);
                uart_puts(" .. ");
                uart_put_hex(e);
                uart_puts(overlap ? "  *** OVERLAPS THE FRAMEBUFFER ***\n"
                                  : "  clear\n");
            }
            uart_puts("   rows 0/60/120/180 first px: ");
            for (uint32_t r = 0; r < 4u; r++) {
                uart_put_hex(fb[(r * 60u) * RAY_VIEW_W + 8u]);
                uart_puts(" ");
            }
            uart_puts("\n");
        }
    }
    else if (str_eq(line, "blittest")) {
        /* Splits "the picture is wrong" into its two possible causes.
         *
         * A STATIC pattern through the exact call the raycaster uses -- same
         * buffer, same width, same stride, same contiguous path. If this shows
         * clean vertical bars with a white block in the top-right corner, the
         * blit is sound and the raycaster's composition is at fault. If it is
         * torn, the transport is, and the renderer was never the problem.
         *
         * The white block sits where the close button goes, so "the X is
         * missing" gets an answer at the same time. */
        uint16_t *fb = raycast_fb_ptr();
        if (!fb) {
            uart_puts("   no framebuffer (try 'fb on')\n");
        } else {
            /* Address first. The first run of this panicked with
             * StoreProhibited writing into the buffer, so where it points and
             * how big it is matter more than the pattern. */
            uart_puts("   fb=");
            uart_put_hex((uint32_t)fb);
            uart_puts("  need=");
            uart_put_dec(RAY_VIEW_W * RAY_VIEW_H * 2u);
            uart_puts(" bytes  heap free=");
            uart_put_dec(heap_free_bytes());
            uart_puts("\n");
            desktop_set_active(1);          /* stop the renderer overwriting it */
            static const uint16_t bars[6] = {
                COLOR_RED, COLOR_GREEN, COLOR_BLUE,
                COLOR_YELLOW, COLOR_WHITE, COLOR_BLACK };
            for (uint32_t y = 0; y < RAY_VIEW_H; y++) {
                for (uint32_t x = 0; x < RAY_VIEW_W; x++) {
                    fb[y * RAY_VIEW_W + x] = bars[(x / 40u) % 6u];
                }
            }
            for (uint32_t y = 2; y < 20u; y++) {
                for (uint32_t x = RAY_VIEW_W - 20u; x < RAY_VIEW_W - 2u; x++) {
                    fb[y * RAY_VIEW_W + x] = COLOR_WHITE;
                }
            }
            display_blit(0, 0, RAY_VIEW_W, RAY_VIEW_H, fb, RAY_VIEW_W);
            uart_puts("   6 vertical bars + white block top-right, ");
            uart_put_dec(RAY_VIEW_W);
            uart_puts("x");
            uart_put_dec(RAY_VIEW_H);
            uart_puts(" via the raycaster's own blit call\n");
        }
    }
    else if (str_eq(line, "spiclk")) {
        /* Live, while the view is on screen. The driver's own note says a panel
         * clocked too fast shows noise on the glass with every counter
         * reporting success, so the only usable instrument is someone watching
         * it change. */
        int n = parse_int(arg);
        if (n < 0 || n > 2) {
            uart_puts("   usage: spiclk 0|1|2   (0=40MHz default, 1=20MHz, 2=10MHz)\n");
        } else {
            uart_puts("   panel clock reg = ");
            uart_put_hex(display_spi_clock_preset((uint32_t)n));
            uart_puts("\n");
        }
    }
    else if (str_eq(line, "touchcfg")) {
        /* touchcfg <prio 0-2> <sleep ticks, 0=yield>
         *
         * Both knobs at runtime, because touch went from NORMAL+yield to
         * HIGH+sleep in a single step and the 3D view started tearing. Which
         * half caused it is the question, and reflashing per guess has already
         * cost two rounds. */
        extern volatile uint32_t g_touch_sleep_ticks;
        extern int kmain_touch_task_id(void);
        char *second = split(arg);
        int prio = parse_int(arg);
        int naps = parse_int(second);
        if (prio >= 0 && prio <= 2 && naps >= 0) {
            task_set_priority(kmain_touch_task_id(), prio);
            g_touch_sleep_ticks = (uint32_t)naps;
            uart_puts("   touch prio=");
            uart_put_dec((unsigned)prio);
            uart_puts(" sleep=");
            uart_put_dec((unsigned)naps);
            uart_puts(naps ? " ticks\n" : " (yield)\n");
        } else {
            uart_puts("   usage: touchcfg <prio 0..2> <sleep ticks, 0=yield>\n");
        }
    }
#if BOARD_HAS_WIFI
    else if (str_eq(line, "hwinit")) {
        /* One stage per invocation. This is the first code to call into libpp,
         * the MAC blob, and if it takes the board down the stage number is the
         * only thing that will survive. */
        /* 0..3 bring the MAC up and let it listen, and were all tried in
         * UM-NATOS-028 §3 without changing anything. 4..6 are the transmit
         * side, and every one is already in the image -- checked with nm before
         * being named, because referencing an UNLINKED symbol changes which
         * objects the linker pulls and has broken working code here before. */
        static const char *names[7] = {
            "ic_mac_init", "hal_init", "ic_enable_rx", "hal_mac_tsf_reset",
            "hal_mac_rate_autoack_init", "hal_attenna_init",
            "hal_mac_disable_low_rate" };
        int step = *arg ? parse_int(arg) : wifimac_hw_stage();
        if (step < 0 || step > 6) {
            uart_puts("   usage: hwinit [0..6]\n"
                      "     0 ic_mac_init   1 hal_init   2 ic_enable_rx\n"
                      "     3 hal_mac_tsf_reset\n"
                      "     4 hal_mac_rate_autoack_init   <- transmit needs a rate\n"
                      "     5 hal_attenna_init            6 hal_mac_disable_low_rate\n");
        } else {
            uart_puts("   calling ");
            uart_puts(names[step]);
            uart_puts(" ...\n");
            int r = wifimac_hwinit_step((uint32_t)step);
            uart_puts(r == 0 ? "   returned\n" : "   refused: run macinit first\n");
            uart_puts("   next stage would be ");
            uart_put_dec((unsigned)wifimac_hw_stage());
            uart_puts("\n");
        }
    }
#endif /* BOARD_HAS_WIFI */
#if BOARD_HAS_WIFI
    /* Blob-dependent. These reach libphy/libpp through the windowed
     * bridge; a build with BOARD_HAS_WIFI 0 links no vendor archive at
     * all and these commands do not exist. See docs/blob-free.md. */
    else if (str_eq(line, "txpwr")) {
        /* One step per invocation, so the probe test after each says which
         * step mattered. "txpwr" alone just reports. */
        if (str_eq(arg, "init")) {
            uart_puts("   tx_pwctrl_init -> ");
            uart_put_dec((unsigned)wifimac_txpwr_init());
        } else if (str_eq(arg, "cal")) {
            uart_puts("   tx_pwctrl_cal -> ");
            uart_put_dec((unsigned)wifimac_txpwr_cal());
        } else if (*arg) {
            int v = parse_int(arg);
            uart_puts("   phy_set_most_tpw -> ");
            uart_put_dec((unsigned)wifimac_txpwr_set((uint32_t)v));
        } else {
            uart_puts("   usage: txpwr init | cal | <quarter-dBm, e.g. 78>");
        }
        uart_puts("\n   most_tpw now = ");
        uart_put_hex(wifimac_txpwr_get());
        uart_puts("\n");
    }
    else if (str_eq(line, "phycap")) {
        /* Capture the PHY's analog programming, with core 1 watching.
         *
         * UM-NATOS-034 §27 captured 5,095 regi2c transactions on the reference
         * board and could not capture nat-os's, because register_chipv7_phy()
         * is synchronous and a single-core kernel has nobody left to sample.
         * appcpu.c starts core 1 for exactly this and nothing else.
         *
         * Output format is byte-identical to tools/idf_ref's, so one parser
         * reads both.
         *
         * ---- the rate difference, stated before the result ------------------
         *
         * The reference runs at 240 MHz, nat-os at 80. Five words is ~140
         * cycles either way, so the sampling period is ~0.6 us there and
         * ~1.75 us here, against transactions observed ~1 us apart. **nat-os
         * will miss more than the reference does.**
         *
         * So a transaction-for-transaction diff is not available and is not
         * what this is for. What survives the rate difference is which analog
         * BLOCKS get programmed at all, and roughly how much traffic each
         * receives -- and if the reference programs a block nat-os never
         * touches, no sampling rate hides that.
         */
        if (appcpu_start() == 0) {
            uart_puts("   core 1 started\n");
            for (volatile uint32_t d = 0; d < 400000u; d++) {
            }
        }
        if (appcpu_alive() != 0xC0DEA11Eu) {
            uart_puts("   core 1 did not arrive; refusing to report a capture\n");
            uart_puts("   that nothing was taking.\n");
        } else {
            appcpu_arm(1);
            int rc = phyinit_run();
            appcpu_arm(0);

            uint32_t n = appcpu_cap_count();
            /* console_lock across the dump, for the reason regdump gives: the
             * reporter emits two kilobytes every 200 ms and this dump runs for
             * minutes.
             *
             * PRECAUTIONARY, not a proven fix. One capture came back 4,099
             * taken and 3,454 parseable; the next came back 4,076 and 4,076
             * with this code NOT yet applied. So interleaving is a plausible
             * explanation for the first and is not a demonstrated one, and the
             * lock is here because the hazard is real rather than because it
             * was shown to be the cause. */
            console_lock();
            uart_puts("   phyinit_run returned ");
            uart_put_dec((unsigned int)rc);
            uart_puts("\n");
            uart_puts("I2CCAP tag=natos n=");
            uart_put_dec(n);
            uart_puts("\n");
            uint32_t t0 = n ? appcpu_cap_ts(0) : 0u;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t v = appcpu_cap_val(i);
                uart_puts("I2C ");
                uart_put_dec(i);
                uart_puts(" ");
                uart_put_dec(appcpu_cap_ts(i) - t0);
                uart_puts(" ");
                uart_put_hex(v);
                uart_puts(" ");
                uart_put_hex(v & 0xFFu);
                uart_puts(" ");
                uart_put_hex((v >> 8) & 0xFFu);
                uart_puts(" ");
                uart_put_hex((v >> 16) & 0xFFu);
                uart_puts("\n");
            }
            uart_puts("I2CCAPEND\n");
            console_unlock();
        }
    }
    else if (str_eq(line, "appcpu")) {
        /* Step one: is core 1 alive at all?
         *
         * Deliberately separate from the capture. Starting a second core on a
         * kernel that has never had one is the risky half; sampling a
         * peripheral word is the easy half. Proving the first works before
         * building on it means a failure has one candidate cause instead of
         * two. */
        int rc = appcpu_start();
        uart_puts(rc == 1 ? "   core 1 was already running\n"
                          : "   core 1 released from stall and reset\n");
        for (volatile uint32_t d = 0; d < 400000u; d++) {
        }
        uint32_t a = appcpu_alive();
        uart_puts("   alive word : ");
        uart_put_hex(a);
        uart_puts(a == 0xC0DEA11Eu ? "   CORE 1 REACHED C\n" : "   no arrival\n");
        uint32_t s1 = appcpu_spins();
        for (volatile uint32_t d = 0; d < 400000u; d++) {
        }
        uint32_t s2 = appcpu_spins();
        uart_puts("   spins      : ");
        uart_put_dec(s2 - s1);
        uart_puts(s2 > s1 ? "   and it is EXECUTING\n" : "   not advancing\n");
    }
    else if (str_eq(line, "phyi2c")) {
        /* Does register_chipv7_phy touch the regi2c host at all?
         *
         * UM-NATOS-034 §27. The capture on the reference board watched one word
         * -- 0x3FF4E010, the one `i2cprobe` found -- and saw 78 transactions,
         * every single one to block 0x66, the BBPLL. That is ESP-IDF's own CPU
         * clock code, not the radio.
         *
         * Which leaves two readings, and they point in opposite directions:
         *
         *   (a) the PHY does not use regi2c for the RF front end at all, and
         *       programs it through the memory-mapped PHY/baseband blocks --
         *       which §26 already diffed and found equivalent; or
         *   (b) it does, through a host word this instrument was not watching.
         *
         * A transaction register holds its last transaction. So a snapshot of
         * the whole host block either side of PHY init answers it: if any word
         * other than 0x3FF4E010 moves, reading (b) is live and the capture must
         * be widened. If none does, reading (a) stands and the analog path is
         * not reached this way.
         *
         * Cheap, decisive, and it does not need the sampling rate that a
         * continuous capture across 64 words would fail to sustain -- 64 words
         * is ~7.5 us per pass and the observed transactions are ~1 us apart.
         */
        enum { NW = 64 };
        static uint32_t pre[NW], post[NW];
        const uint32_t BASE = 0x3FF4E000u;

        for (uint32_t i = 0; i < NW; i++) {
            pre[i] = *(volatile uint32_t *)(BASE + i * 4u);
        }

        int rc = phyinit_run();

        for (uint32_t i = 0; i < NW; i++) {
            post[i] = *(volatile uint32_t *)(BASE + i * 4u);
        }

        uart_puts("   phyinit_run returned ");
        uart_put_dec((unsigned int)rc);
        uart_puts("\n   regi2c host block 0x3FF4E000, 64 words:\n");

        uint32_t moved = 0;
        for (uint32_t i = 0; i < NW; i++) {
            if (pre[i] != post[i]) {
                uart_puts("   0x");
                uart_put_hex(BASE + i * 4u);
                uart_puts("  ");
                uart_put_hex(pre[i]);
                uart_puts(" -> ");
                uart_put_hex(post[i]);
                uart_puts("   blk=");
                uart_put_hex(post[i] & 0xFFu);
                uart_puts(" reg=");
                uart_put_hex((post[i] >> 8) & 0xFFu);
                uart_puts(" data=");
                uart_put_hex((post[i] >> 16) & 0xFFu);
                uart_puts("\n");
                moved++;
            }
        }
        if (!moved) {
            uart_puts("   NOTHING moved. register_chipv7_phy does not reach the\n");
            uart_puts("   analog blocks through this host -- so the RF path is\n");
            uart_puts("   configured somewhere §26 already compared.\n");
        } else {
            uart_puts("   ");
            uart_put_dec(moved);
            uart_puts(" word(s) moved\n");
        }
    }
    else if (str_eq(line, "i2cprobe")) {
        /* Find the regi2c HOST registers, by doing a transaction and seeing
         * what moved.
         *
         * ---- why ------------------------------------------------------------
         *
         * UM-NATOS-034 §26 established that the entire memory-mapped state of
         * the MAC, PHY and baseband is equivalent between a stack that
         * transmits and one that does not -- 2,048 PHY registers, one
         * difference, matched, no change. So whatever differs is not in the
         * register space, and the obvious candidate is the analog front end,
         * which is not memory-mapped at all. It is reached over an internal
         * I2C bus through rom_i2c_writeReg, the same mechanism kernel/clock.c
         * uses for the BBPLL because no register path exists.
         *
         * Those analog registers cannot be dumped. But the I2C HOST that
         * reaches them is ordinary memory-mapped hardware, so the traffic can
         * be watched even though the destinations cannot.
         *
         * ---- why this is a search and not a constant --------------------------
         *
         * `ANA_CONFIG_REG 0x6000E044` is the only address ESP-IDF publishes,
         * and there is no `DR_REG_*_BASE` anywhere in soc.h covering
         * 0x3FF4Exxx -- the block is undocumented. Rather than assume the alias
         * arithmetic and the extent, this does a real transaction and reports
         * which words changed.
         *
         * rom_i2c_readReg, not writeReg: a read still drives the host through a
         * full transaction, and cannot disturb a PLL this kernel is running on.
         *
         * Usage: i2cprobe [hex base]   default 0x3FF4E000, 256 words
         */
        enum { NW = 256 };
        static uint32_t before[NW], after[NW];

        int bok = 0;
        uint32_t base = parse_hex(arg, &bok);
        if (!bok) {
            base = 0x3FF4E000u;
        }

        for (uint32_t i = 0; i < NW; i++) {
            before[i] = *(volatile uint32_t *)(base + i * 4u);
        }

        /* I2C_BBPLL block 0x66, host id 4, register 0 -- the same block
         * clock.c programs, so it is known to exist and known to answer. */
        uint32_t val = rom_call3(0x40004148u, 0x66u, 4u, 0u);

        for (uint32_t i = 0; i < NW; i++) {
            after[i] = *(volatile uint32_t *)(base + i * 4u);
        }

        uart_puts("   rom_i2c_readReg(0x66,4,0) returned ");
        uart_put_hex(val);
        uart_puts("\n   scanning ");
        uart_put_hex(base);
        uart_puts(" for ");
        uart_put_dec(NW);
        uart_puts(" words\n");

        uint32_t moved = 0;
        for (uint32_t i = 0; i < NW; i++) {
            if (before[i] != after[i]) {
                uart_puts("   0x");
                uart_put_hex(base + i * 4u);
                uart_puts("  ");
                uart_put_hex(before[i]);
                uart_puts(" -> ");
                uart_put_hex(after[i]);
                uart_puts("\n");
                moved++;
            }
        }
        if (!moved) {
            uart_puts("   nothing moved in this range.\n");
            uart_puts("   Either the host is elsewhere, or its registers are\n");
            uart_puts("   write-only/self-clearing and a before/after snapshot\n");
            uart_puts("   cannot see them -- which is the same blind spot\n");
            uart_puts("   UM-NATOS-034 §18 was built to close.\n");
        } else {
            uart_puts("   ");
            uart_put_dec(moved);
            uart_puts(" words moved\n");
        }
    }
    else if (str_eq(line, "coexprio")) {
        /* coex_bt_high_prio(), reimplemented from librtc.a's bt_bb.o.
         *
         * ---- why this exists ----------------------------------------------
         *
         * UM-NATOS-034 §22.5 left one candidate: ESP-IDF calls
         * coex_bt_high_prio() unconditionally on ESP32, immediately after PHY
         * init, and nat-os never did. §22.5 also asserted it lived in
         * libcoexist.a and would therefore need a third vendor archive.
         *
         * That assertion was wrong. The symbol is not in libcoexist.a at all --
         * it is defined in esp_phy/lib/esp32/librtc.a, object bt_bb.o, and it
         * is 251 bytes of pure register read-modify-write with no calls and no
         * loops. So it does not need linking. It needs transcribing.
         *
         * ---- the addresses -------------------------------------------------
         *
         * Its literal pool holds peripheral addresses in the 0x60000000 alias.
         * On ESP32 the peripheral bus at 0x3FF40000 is mirrored at 0x60000000,
         * so 0x60031300 is 0x3FF71300 and so on. Translated:
         *
         *   0x3FF5C080   BT baseband
         *   0x3FF710D0   BT/PHY block
         *   0x3FF71300   BT/PHY block
         *   0x3FF73D30 } WiFi MAC -- the SAME block as this kernel's transmit
         *   0x3FF73D38 } config (0x3FF73D1C / 0x3FF73D20) and the status
         *   0x3FF73D40 } registers traced in UM-NATOS-034 §19-§20
         *   0x3FF441C4   GPIO
         *   0x3FF5D040   BT baseband
         *
         * Three of the eight are WiFi MAC transmit-block registers. A function
         * named for Bluetooth coexistence turns out to configure the very block
         * this investigation has been staring at, which is why it was worth
         * transcribing rather than dismissing by its name.
         *
         * Order preserved exactly as disassembled. Every step is a read, a
         * mask, and a write back.
         */
        volatile uint32_t *A = (volatile uint32_t *)0x3FF5C080u;
        volatile uint32_t *B = (volatile uint32_t *)0x3FF710D0u;
        volatile uint32_t *C = (volatile uint32_t *)0x3FF71300u;
        volatile uint32_t *D = (volatile uint32_t *)0x3FF73D30u;
        volatile uint32_t *E = (volatile uint32_t *)0x3FF73D38u;
        volatile uint32_t *F = (volatile uint32_t *)0x3FF73D40u;
        volatile uint32_t *G = (volatile uint32_t *)0x3FF441C4u;
        volatile uint32_t *H = (volatile uint32_t *)0x3FF5D040u;

        uart_puts("   before: D=");
        uart_put_hex(*D);
        uart_puts(" E=");
        uart_put_hex(*E);
        uart_puts(" F=");
        uart_put_hex(*F);
        uart_puts("\n");

        *A = *A & 0xFFFFFF3Fu;                       /* clear bits 6,7        */
        *B = *B & 0xFFFFFF0Fu;                       /* clear bits 4..7       */
        *C = *C & 0xFFFFFF0Fu;                       /* clear bits 4..7       */
        *D = *D | 1u;                                /* set bit 0             */
        *D = (*D & 0xF0FFFFFFu) | 0x01000000u;       /* field 27:24 := 1      */
        *D = (*D & 0xFF0FFFFFu) | 0x00400000u;       /* field 23:20 := 4      */
        *E = (*E & 0x0000FFFFu) | 0x0C800000u;       /* high half := 0x0C80   */
        *E = (*E & 0xFFFF0000u) | 10u;               /* low half  := 10       */
        *F = (*F & 0xFFFFFFC7u) | 16u;               /* field 5:3 := 2        */
        *G = *G | 64u;                               /* set bit 6             */
        *B = *B | 1u;                                /* set bit 0             */
        *C = *C | 1u;                                /* set bit 0             */
        *H = *H & 0x7FFFFFFFu;                       /* clear bit 31          */

        uart_puts("   after : D=");
        uart_put_hex(*D);
        uart_puts(" E=");
        uart_put_hex(*E);
        uart_puts(" F=");
        uart_put_hex(*F);
        uart_puts("\n   coex_bt_high_prio sequence applied\n");
    }
    else if (str_eq(line, "txwatch")) {
        /* Ask the MAC, rather than ourselves, whether a frame went out.
         *
         * ---- why this exists ---------------------------------------------
         *
         * UM-NATOS-034 §18 traced ESP-IDF transmitting and found 0x3FF73DB8
         * cycling 0x000 -> 0x210 -> 0x230 -> 0x020 -> 0x000 once per frame,
         * with three counters stepping alongside it. It read as a control
         * register with a self-clearing "go" bit that nat-os never writes,
         * and §18.5 proposed writing that sequence in wifimac_tx().
         *
         * THAT WAS WRONG. The block is READ-ONLY -- 0x3FF73DAC, 0x3FF73DB0 and
         * 0x3FF73DB8 reject 0xFFFFFFFF, 0x55555555 and 0x230 alike, while
         * 0x3FF73D1C and 0x3FF73D20, which wifimac_tx() does write, accept all
         * three. Bit 5 self-clears because the HARDWARE clears it. nat-os does
         * not write these because it cannot.
         *
         * Which makes them worth far more than the lead they replaced.
         *
         * Every transmit counter this kernel owns is its own bookkeeping --
         * `hardware=`, `completions reaped=`, `chain acks=` -- and this project
         * has a rule about that, paid for repeatedly:
         *
         *     a successful completion count is not evidence.
         *
         * These are not our counters. 0x3FF73D84/88/8C are inside the MAC and
         * step once per frame on a radio that is demonstrably transmitting. So:
         *
         *   they move  -> the MAC believes it transmitted, and the fault is
         *                 downstream of it, in the RF/PHY path
         *   they don't -> the MAC never started, and the fault is upstream, in
         *                 how nat-os arms it
         *
         * Either answer halves a search space that has been whole for four
         * sessions, and neither can be forged by nat-os's own code.
         *
         * ---- method -------------------------------------------------------
         *
         * wifimac_tx() posts to the hardware and returns; the transmit happens
         * after it. So sampling in a tight loop immediately afterwards catches
         * the state machine if there is one. 64 samples is a few hundred
         * microseconds -- the whole cycle takes ~20 of ESP-IDF's 1.9 us samples,
         * so this is comfortably wider than the event.
         *
         * Reads only. This command cannot perturb what it measures. */
        static const uint32_t WATCH[] = {
            0x3FF73D84u, 0x3FF73D88u, 0x3FF73D8Cu,   /* the three counters */
            0x3FF73DACu, 0x3FF73DB0u, 0x3FF73DB8u,   /* the state block    */
        };
        /* 384 samples.
         *
         * Originally 64, which produced a wrong answer once already: on
         * ESP-IDF the state reaches 0x258 quickly and only completes --
         * 0x258 -> 0x220 -> 0x020 -> 0x000 -- about 320 us later, and 64
         * samples is roughly 100 us, so the window ended before the event
         * did. At ~1.56 us per sample (measured below via ccount), 384
         * covers ~600 us -- nearly twice the event, with room to spare.
         *
         * That same error UM-NATOS-034 A18 made with one frame per window,
         * and it is worth stating twice: a measurement that ends before
         * the event cannot be told apart from an event that never happens,
         * unless somebody checks the timescales.
         *
         * Halved again from 768 purely for DRAM: snap plus the display
         * buffers overflowed .bss by ~12.9 KB and nothing else could give
         * it back without degrading an instrument further. */
        enum { NW = 6, NS = 384 };
        static uint32_t snap[NS][NW];
        uint32_t t0, t1;

        for (uint32_t w = 0; w < NW; w++) {
            snap[0][w] = *(volatile uint32_t *)WATCH[w];
        }

        /* `txwatch idle` samples WITHOUT posting a frame.
         *
         * The control, and it is not optional. The first run of this command
         * showed the MAC stepping through a whole state cycle and it read as
         * evidence that the transmit reached the hardware. It is only evidence
         * of that if the cycle does NOT happen when nothing is transmitted --
         * and this MAC has a receiver armed, a TSF timer running and beacon
         * timing to service, any of which could produce state changes on its
         * own.
         *
         * UM-NATOS-034 §5 needed a second board to learn this lesson about
         * transmit. It costs one argument to learn it here. */
        int idle = str_eq(arg, "idle");
        if (!idle) {
            wifimac_probe_request();
        }

        __asm__ __volatile__("rsr.ccount %0" : "=r"(t0));
        for (uint32_t i = 1; i < NS; i++) {
            for (uint32_t w = 0; w < NW; w++) {
                snap[i][w] = *(volatile uint32_t *)WATCH[w];
            }
        }
        __asm__ __volatile__("rsr.ccount %0" : "=r"(t1));

        uart_puts(idle ? "   NOTHING posted (control); MAC status sampled "
                        : "   one frame posted; MAC status sampled ");
        uart_put_dec(NS);
        uart_puts("x over ");
        /* Printed, so the window can be COMPARED with ESP-IDF's rather than
         * assumed comparable. 80 MHz, so cycles/80 is microseconds. */
        uart_put_dec((t1 - t0) / 80u);
        uart_puts(" us\n");
        uart_puts("   sample  addr        from      to\n");
        uint32_t changes = 0;
        for (uint32_t i = 1; i < NS; i++) {
            for (uint32_t w = 0; w < NW; w++) {
                if (snap[i][w] != snap[i - 1][w]) {
                    uart_puts("   ");
                    uart_put_dec(i);
                    uart_puts("\t0x");
                    uart_put_hex(WATCH[w]);
                    uart_puts("  ");
                    uart_put_hex(snap[i - 1][w]);
                    uart_puts("  ");
                    uart_put_hex(snap[i][w]);
                    uart_puts("\n");
                    changes++;
                }
            }
        }
        if (changes == 0u) {
            uart_puts("   NOTHING MOVED.\n");
            uart_puts("   The MAC's own counters did not step. On a radio that\n");
            uart_puts("   transmits these advance once per frame, so this says\n");
            uart_puts("   the transmit never started -- the fault is upstream of\n");
            uart_puts("   the RF path, in how this kernel arms the hardware.\n");
        } else {
            uart_puts("   ");
            uart_put_dec(changes);
            uart_puts(" changes -- the MAC moved. Compare the shape against\n");
            uart_puts("   UM-NATOS-034 §18: 0x210 -> 0x230 -> 0x020 -> 0x000.\n");
        }
    }
    else if (str_eq(line, "probe")) {
        /* Ten, spaced by the caller re-running this: APs answer probe requests
         * but not reliably on the first try, and a single unanswered one would
         * be weak evidence either way. */
        for (int i = 0; i < 10; i++) {
            wifimac_probe_request();
            task_sleep(2u);
        }
        uart_puts("   sent 10 probe requests\n   frames addressed to us=");
        uart_put_dec(wifimac_rx_to_us());
        uart_puts("  (any non-zero means something HEARD us)\n");
    }
    else if (str_eq(line, "regdump")) {
        /* The same ranges, in the same format, as tools/idf_ref prints.
         *
         * Phase B of UM-NATOS-034: both stacks call the same PHY blob, so
         * whatever it does internally is identical. The difference between
         * a radio that transmits and one that does not has to show in the
         * registers the surrounding code touches -- a layer both sides can
         * dump and a host can diff.
         *
         * Printed TWICE by the caller, a second apart, so counters can be
         * filtered: anything that moves between one board's own two dumps
         * is free-running and gets discarded. Without that the diff is
         * almost entirely TSF timer.
         *
         * console_lock across the whole dump. The reporter emits two
         * kilobytes every 200 ms, and a status line landing in the middle
         * of the hex is exactly how fbdump manufactured a render bug that
         * did not exist (UM-NATOS-030 §5.1). */
        console_lock();
        regdump_range("dport", 0x3FF00000u, 64u);
        regdump_range("rtc",   0x3FF48000u, 64u);
        regdump_range("mac",   0x3FF73000u, 1280u);
        /* The PHY and baseband blocks -- see UM-NATOS-034 §26. Ranges must
         * match tools/idf_ref exactly or reg_diff.py cannot pair them. */
        /* regi2c host block -- see UM-NATOS-034 §27. */
        regdump_range("i2c",   0x3FF4E000u, 64u);
        regdump_range("bb0",   0x3FF5C000u, 512u);
        regdump_range("bb1",   0x3FF5D000u, 512u);
        regdump_range("phy",   0x3FF71000u, 1024u);
        uart_puts("REGEND\n");
        console_unlock();
    }
    else if (str_eq(line, "wifireg")) {
        /* Read or write one register, by address.
         *
         *   wifireg <addr>          read
         *   wifireg <addr> <value>  write, then read back
         *
         * The differential in UM-NATOS-034 §13 produced a shortlist of about
         * ninety addresses where the working stack holds something and this
         * one holds zero. Hardcoding each as its own command and reflashing
         * between them would take an afternoon; this sweeps the list from the
         * prompt, one register at a time, which is the same discipline for a
         * fraction of the cost.
         *
         * Deliberately unguarded as to WHICH address. It is a diagnostic on a
         * board that reboots in three seconds, and refusing addresses would
         * mean guessing in advance which ones matter -- which is the thing the
         * differential exists to stop doing. */
        char *ca = arg;
        char *cv = split(ca);
        int aok = 0, vok = 0;
        uint32_t addr = parse_hex(ca, &aok);
        if (!aok) {
            uart_puts("   usage: wifireg <hex addr> [hex value]\n");
        } else if (*cv) {
            uint32_t val = parse_hex(cv, &vok);
            if (!vok) {
                uart_puts("   value must be hex\n");
            } else {
                uart_puts("   "); uart_put_hex(addr);
                uart_puts(" was "); uart_put_hex(*(volatile uint32_t *)addr);
                *(volatile uint32_t *)addr = val;
                uart_puts(" now "); uart_put_hex(*(volatile uint32_t *)addr);
                uart_puts("\n");
            }
        } else {
            uart_puts("   "); uart_put_hex(addr);
            uart_puts(" = "); uart_put_hex(*(volatile uint32_t *)addr);
            uart_puts("\n");
        }
    }
    else if (str_eq(line, "machw")) {
        /* Program the MAC hardware with this chip's own address.
         *
         * The differential against ESP-IDF found 0x3FF73008/000C holding the
         * reference's MAC and nat-os holding zero -- this driver has run with
         * an address of 00:00:00:00:00:00 since it was written. Run after
         * macinit, before beaconing. */
        uint8_t mac[6];
        uint32_t lo = 0, hi = 0;
        efuse_factory_mac(mac);
        wifimac_get_hw_addr(&lo, &hi);
        uart_puts("   before: "); uart_put_hex(lo);
        uart_puts(" "); uart_put_hex(hi); uart_puts("\n");
        wifimac_set_hw_addr(mac);
        wifimac_get_hw_addr(&lo, &hi);
        uart_puts("   after : "); uart_put_hex(lo);
        uart_puts(" "); uart_put_hex(hi); uart_puts("\n");
    }
    else if (str_eq(line, "wifipd")) {
        /* The WiFi power domain. `wifipd` reads, `wifipd on` applies.
         *
         * MUST run BEFORE macinit: the sequence pulses WIFIMAC_RST, so
         * applying it to a live MAC undoes the bring-up and would read as
         * a new failure rather than as this command doing its job.
         *
         * Reading first because the answer may already be visible. If
         * WIFI_FORCE_ISO is asserted, the WiFi domain is ISOLATED -- its
         * outputs clamped -- which is UM-NATOS-034 stated as a register:
         * the digital side works, completions climb, nothing reaches the
         * analog side. */
        uint32_t pwc = 0, iso = 0, clk = 0;
        wifimac_power_domain_read(&pwc, &iso, &clk);
        uart_puts("   DIG_PWC  = "); uart_put_hex(pwc);
        uart_puts("   WIFI_FORCE_PD(17) = ");
        uart_puts((pwc & (1u << 17)) ? "SET  <-- powered down\n"
                                     : "clear\n");
        uart_puts("   DIG_ISO  = "); uart_put_hex(iso);
        uart_puts("   WIFI_FORCE_ISO(28) = ");
        uart_puts((iso & (1u << 28)) ? "SET  <-- ISOLATED\n"
                                     : "clear\n");
        uart_puts("   WIFI_CLK = "); uart_put_hex(clk); uart_puts("\n");

        if (str_eq(arg, "on")) {
            wifimac_power_domain_on();
            wifimac_power_domain_read(&pwc, &iso, &clk);
            uart_puts("   applied. now PWC="); uart_put_hex(pwc);
            uart_puts(" ISO="); uart_put_hex(iso);
            uart_puts(" CLK="); uart_put_hex(clk);
            uart_puts("\n   run phyinit / macinit / chan / macrx now\n");
        } else {
            uart_puts("   read only. 'wifipd on' applies it (before macinit).\n");
        }
    }
    else if (str_eq(line, "lmacinit")) {
        /* The lower MAC. Run macinit first, then this, then try to transmit.
         *
         * Separate from macinit on purpose: naming lmacInit changed the link,
         * and the canary for that (phyinit returning 0) has to be checkable
         * before any of the newly-linked code runs. */
        uart_puts("   lmacInit ...\n");
        wifimac_lmac_init();
        for (uint32_t ac = 0; ac < 4u; ac++) {
            wifimac_lmac_init_ac(ac);
        }
        uart_puts("   lmacInit + lmacInitAc(0..3) returned\n");
    }
    else if (str_eq(line, "wifitx")) {
        /* The two registers the vendor's init writes and this driver never has.
         *
         *   wifitx            read them, change nothing
         *   wifitx cca <0-3>  CCA mode, bits 31:30 of 0x3FF73C58
         *   wifitx aifs <0-15> arbitration spacing, bits 27:24 of TX_CONFIG
         *   wifitx cw <n>     contention window, bits 21:12
         *
         * Separate subcommands on purpose. UM-NATOS-034 narrowed transmit to
         * "the MAC retires the frame and nothing radiates", and the standing
         * rule from reverting two changes together is that it destroys the
         * information about which one mattered. One poke, one measurement.
         *
         * Reading first is not politeness. Nobody has ever looked at what these
         * hold, and the reset value is itself evidence -- an AIFS of zero would
         * mean the arbitration spacing has been degenerate this whole time. */
        char *what = arg;
        char *val  = split(what);

        if (!*what) {
            uint32_t cca = wifimac_cca_get();
            uint32_t cfg = wifimac_txcfg_get();
            uart_puts("   CCA  0x3FF73C58 = ");
            uart_put_hex(cca);
            uart_puts("   mode(31:30)=");
            uart_put_dec((cca >> 30) & 3u);
            uart_puts("\n   TXCFG            = ");
            uart_put_hex(cfg);
            uart_puts("   aifs(27:24)=");
            uart_put_dec((cfg >> 24) & 0xFu);
            uart_puts("  cw(21:12)=");
            uart_put_dec((cfg >> 12) & 0x3FFu);
            uart_puts("\n   usage: wifitx cca|aifs|cw <value>\n");
        } else {
            int v = parse_int(val);
            if (v < 0) {
                uart_puts("   value must be a non-negative decimal\n");
            } else if (str_eq(what, "cca")) {
                wifimac_cca_set((uint32_t)v);
                uart_puts("   CCA now ");
                uart_put_hex(wifimac_cca_get());
                uart_puts("\n");
            } else if (str_eq(what, "aifs")) {
                wifimac_aifs_set((uint32_t)v);
                uart_puts("   TXCFG now ");
                uart_put_hex(wifimac_txcfg_get());
                uart_puts("\n");
            } else if (str_eq(what, "cw")) {
                wifimac_cw_set((uint32_t)v);
                uart_puts("   TXCFG now ");
                uart_put_hex(wifimac_txcfg_get());
                uart_puts("\n");
            } else {
                uart_puts("   usage: wifitx cca|aifs|cw <value>\n");
            }
        }
    }
    else if (str_eq(line, "txstat")) {
        uart_puts("   tx handed to hardware=");
        uart_put_dec(wifimac_tx_sent());
        uart_puts("  completions reaped=");
        uart_put_dec(wifimac_tx_done());
        uart_puts("\n   a rising completion count is the MAC saying the frame "
                  "actually went out\n   chain acks=");
        uart_put_dec(wifimac_chain_calls());
        uart_puts("  forced=");
        uart_put_dec(wifimac_tx_forced());
        uart_puts("\n");
    }
    else if (str_eq(line, "scan")) {
        /* Every distinct beacon source seen since the receiver was armed.
         * Several networks, each with a rising beacon count, is what proves
         * descriptors are being recycled -- four buffers can hold four frames,
         * so anything beyond that had to come from reuse. */
        uint32_t n = wifimac_net_count();
        uart_puts("   frames=");
        uart_put_dec(wifimac_rx_frames());
        uart_puts("  recycled=");
        uart_put_dec(wifimac_rx_recycled());
        uart_puts("  networks=");
        uart_put_dec(n);
        uart_puts("\n");
        static const char hx[] = "0123456789abcdef";
        for (uint32_t i = 0; i < n; i++) {
            uint8_t bssid[6];
            const char *ssid;
            uint32_t seen;
            if (wifimac_net_info(i, bssid, &ssid, &seen) != 0) {
                continue;
            }
            uart_puts("   ");
            for (int b = 0; b < 6; b++) {
                char t[4];
                t[0] = ':';
                t[1] = hx[(bssid[b] >> 4) & 0xF];
                t[2] = hx[bssid[b] & 0xF];
                t[3] = 0;
                uart_puts(t + (b ? 0 : 1));
            }
            uart_puts("  x");
            uart_put_dec(seen);
            uart_puts("  \"");
            uart_puts(ssid);
            uart_puts("\"\n");
        }
        if (!n) {
            uart_puts("   nothing yet - run macrx then chan 1\n");
        }
    }
    else if (str_eq(line, "macstat")) {
        uart_puts("   irq fires=");
        uart_put_dec(wifimac_irq_fires());
        uart_puts("  last status=");
        uart_put_hex(wifimac_irq_status());
        uart_puts("\n   descriptors filled=");
        uart_put_dec(wifimac_rx_filled());
        uart_puts("  next dscr=");
        uart_put_hex(wifimac_rx_next_dscr());
        uart_puts("\n");
        /* static: wifi_frame_info_t is ~64 bytes and the shell task has a 2 KB
         * stack that 'stacks' showed down to 380 B free with these on it. The
         * shell is single-threaded, so this costs nothing but .bss. */
        static wifi_frame_info_t fi;
        if (wifimac_frame_info(&fi) == 0) {
            static const char hx[] = "0123456789abcdef";
            static const char *sub[16] = {
                "assoc-req","assoc-resp","reassoc-req","reassoc-resp",
                "probe-req","probe-resp","?","?","BEACON","atim",
                "disassoc","auth","deauth","action","?","?" };
            uart_puts("   802.11 frame: ");
            uart_puts(fi.fc_type == 0u ? sub[fi.fc_subtype]
                      : (fi.fc_type == 1u ? "control" : "data"));
            uart_puts("  len=");
            uart_put_dec(fi.length);
            uart_puts("\n   bssid ");
            for (int i = 0; i < 6; i++) {
                char b[4];
                b[0] = i ? ':' : ' ';
                b[1] = hx[(fi.addr3[i] >> 4) & 0xF];
                b[2] = hx[fi.addr3[i] & 0xF];
                b[3] = 0;
                uart_puts(b + (i ? 0 : 1));
            }
            if (fi.ssid[0]) {
                uart_puts("   ssid \"");
                uart_puts(fi.ssid);
                uart_puts("\"");
            }
            uart_puts("\n");
        }
        uint32_t len = 0;
        static uint8_t buf[64];
        int n = wifimac_rx_peek(&len, buf, sizeof buf);
        if (n < 0) {
            uart_puts("   no frame captured yet\n");
        } else {
            uart_puts("   first frame len=");
            uart_put_dec(len);
            uart_puts("  bytes:");
            static const char hex[] = "0123456789abcdef";
            for (int i = 0; i < n; i++) {
                char b[4];
                b[0] = ' ';
                b[1] = hex[(buf[i] >> 4) & 0xF];
                b[2] = hex[buf[i] & 0xF];
                b[3] = 0;
                uart_puts(b);
            }
            uart_puts("\n");
        }
    }
    else if (str_eq(line, "macirq")) {
        wifimac_irq_enable();
        uart_puts("   wifi mac source 0 -> cpu line 27, handler installed\n   fires=");
        uart_put_dec(wifimac_irq_fires());
        uart_puts("  last status=");
        uart_put_hex(wifimac_irq_status());
        uart_puts("\n   line disabled mask=");
        uart_put_hex(intr_disabled_mask());
        uart_puts("\n   zero fires is expected until receive exists\n");
    }
    else if (str_eq(line, "maclive")) {
        /* static, like the other buffers here: execute() is one long if/else
         * chain, so GCC sums EVERY branch's locals into a single frame and the
         * shell task's 2 KB was down to 380 B free. */
        static uint32_t addrs[16], khz[16];
        uint32_t n = wifimac_movers(addrs, khz, 16u);
        uart_puts("   moving words=");
        uart_put_dec(n);
        uart_puts("  (delta is per millisecond)\n");
        uint32_t shown = n < 16u ? n : 16u;
        for (uint32_t i = 0; i < shown; i++) {
            uart_puts("   ");
            uart_put_hex(addrs[i]);
            uart_puts("  ");
            uart_put_dec(khz[i]);
            uart_puts(" kHz");
            /* ~1000/ms is 1 MHz, the 802.11 TSF rate. Flagged rather than
             * asserted: the tolerance is wide because the 1 ms window is a
             * busy-wait an interrupt can stretch. */
            if (khz[i] > 900u && khz[i] < 1100u) {
                uart_puts("   <- 1 MHz, looks like the TSF timer");
            }
            uart_puts("\n");
        }
    }
#endif /* BOARD_HAS_WIFI */
    else if (str_eq(line, "tickrate")) {
        /* How many ticks pass per real second.
         *
         * Should be 100 at a 10 ms tick. Anything higher means timer_ticks()
         * is counting something other than time -- which is the whole reason
         * task_sleep was returning early. Busy-waits on CCOUNT rather than
         * sleeping, for the obvious reason. */
        uint32_t t0 = timer_ticks();
        uint32_t c0 = xt_ccount();
        while ((xt_ccount() - c0) < 80000000u) {
            /* one second at ~80 MHz */
        }
        uart_puts("   ticks in one real second: ");
        uart_put_dec(timer_ticks() - t0);
        uart_puts("   (10 ms tick -> expect 100)\n");
    }
    else if (str_eq(line, "sleeptest")) {
        /* Does task_sleep actually sleep? Found while bringing up the MAC:
         * a 500 ms sleep returned in about a microsecond, and two independent
         * clocks agreed on that, so it is not a measurement artefact. */
        uint32_t t0 = timer_ticks();
        uint32_t c0 = xt_ccount();
        task_sleep(50u);                        /* 50 ticks = 500 ms */
        uint32_t cyc = xt_ccount() - c0;
        uint32_t t1 = timer_ticks();
        uart_puts("   task_sleep(50) -> ");
        uart_put_dec(cyc / 80000u);
        uart_puts(" ms, ");
        uart_put_dec(t1 - t0);
        uart_puts(" ticks (expected ~500 ms / 50 ticks)\n   raw cycles=");
        uart_put_dec(cyc);
        uart_puts("  ticks ");
        uart_put_dec(t0);
        uart_puts(" -> ");
        uart_put_dec(t1);
        uart_puts("\n");
    }
#if BOARD_HAS_WIFI
    /* Blob-dependent. These reach libphy/libpp through the windowed
     * bridge; a build with BOARD_HAS_WIFI 0 links no vendor archive at
     * all and these commands do not exist. See docs/blob-free.md. */
    else if (str_eq(line, "mactsf")) {
        /* Two independent clocks over half a second. The MAC's counter has no
         * connection to the CPU's CCOUNT, so agreement to a fraction of a
         * percent is not something a misread register or a noisy address can
         * produce -- it is the difference between "a number changed" and "this
         * is a 1 MHz timebase". */
        uint32_t cycles = 0;
        uint32_t ticks = wifimac_tsf_check(500u, &cycles);
        uint32_t ms = cycles / 80000u;
        uart_puts("   tsf advanced ");
        uart_put_dec(ticks);
        uart_puts(" over ");
        uart_put_dec(ms);
        uart_puts(" ms of cpu time");
        /* Guarded, and the guard earns its place: the first run of this
         * panicked with IntegerDivideByZero because the command was typed
         * during boot, before the scheduler existed. task_sleep() returns
         * immediately when there is no current task, so the interval was a
         * handful of cycles and ms rounded to zero. */
        if (ms == 0u) {
            uart_puts("\n   no interval elapsed - is the scheduler running yet?\n");
        } else {
            uart_puts(" -> ");
            uart_put_dec(ticks / ms);
            uart_puts(" kHz\n");
        }
    }
    else if (str_eq(line, "ositest")) {
        /* Drives the OSI vtable exactly as libpp will. osi_selftest is
         * WINDOWED, so it is entered through rom_call3 -- calling it directly
         * from here would be the IllegalInstruction this project has already
         * met once. Each body it reaches then crosses BACK to call0 via
         * w2c_callN, so a pass exercises both directions per call. */
        extern uint32_t osi_selftest(void);
        uint32_t got = rom_call3((uint32_t)&osi_selftest, 0, 0, 0);
        uart_puts("   osi vtable checks = ");
        uart_put_hex(got);
        uart_puts(got == 0x3Fu ? "  ALL PASS\n" : "  INCOMPLETE (want 0x3F)\n");
        uart_puts("   pools: sem ");
        uart_put_dec(osi_impl_sems_used());
        uart_puts("  queue ");
        uart_put_dec(osi_impl_queues_used());
        uart_puts("  timer ");
        uart_put_dec(osi_impl_timers_used());
        uart_puts("\n   timer service task id ");
        uart_put_dec((unsigned int)osi_impl_service_start());
        uart_puts("\n");
    }
#endif /* BOARD_HAS_WIFI */
    else if (str_eq(line, "bridgetest")) {
        /* Proves windowed code can call BACK into this call0 kernel.
         * osi_add lives here, in call0; the loop calling it is windowed. */
        extern uint32_t osi_add_probe(uint32_t a, uint32_t b);
        uint32_t got = win_call_bridge((uint32_t)&osi_add_probe, 10u);
        uart_puts("   sum 0..9 across the ABI boundary = ");
        uart_put_dec(got);
        uart_puts(got == 45u ? "  CORRECT" : "  WRONG");
        uart_puts("\n");
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
