/* nat-os — MP3 decoding. See mp3.h. */

#include <stddef.h>
#include <stdint.h>
#include "mp3.h"
#include "mp3hdr.h"
#include "fat.h"
#include "task.h"
#include "console.h"
#include "uart.h"

void *memmove(void *dst, const void *src, size_t n);    /* kstring.c */

#define SRAM1 __attribute__((section(".sram1")))
#define CPU_HZ 80000000u

/* ---- the decoder ------------------------------------------------------------
 *
 * minimp3 is included here and nowhere else, so this is the only object with
 * FPU instructions in it (build.ps1 checks). Its warnings are its own business;
 * silenced for the include only, so nat-os's stay visible. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_STATIC_SCRATCH SRAM1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "minimp3.h"
#pragma GCC diagnostic pop

/* ---- __divsf3 ---------------------------------------------------------------
 *
 * Float division. GCC calls this rather than emitting an instruction sequence,
 * and it normally comes from libgcc, which nat-os does not link. minimp3 divides
 * in exactly one place (L3_pow_43, for escaped Huffman values >= 129), where
 * the denominator is an integer >= 64.
 *
 * So: an initial reciprocal from the float's bit pattern, three Newton steps,
 * then a multiply. Relative error ~1e-7, all on the FPU. **Not IEEE**: no
 * correct rounding, and zero, infinities, NaNs and denormals in `b` are not
 * handled. That is fine for minimp3's one call site and wrong for general use,
 * which the build's FPU check exists to keep from happening unnoticed. */
float __divsf3(float a, float b);
float __divsf3(float a, float b)
{
    union { float f; uint32_t u; } v = { b };
    uint32_t sign = v.u & 0x80000000u;
    v.u &= 0x7FFFFFFFu;                         /* |b| */
    float m = v.f;
    v.u = 0x7EF311C3u - v.u;                    /* ~1/|b| to a few bits */
    float r = v.f;
    r = r * (2.0f - m * r);
    r = r * (2.0f - m * r);
    r = r * (2.0f - m * r);
    float q = a * r;
    return sign ? -q : q;
}

/* ---- memory: all of it in SRAM1 ------------------------------------------- */

#define IN_BYTES     8192u              /* >= 5 max-size frames at 48 kHz */
#define STACK_WORDS  1536u              /* 6 KB; minimp3 without its scratch */

SRAM1 static mp3dec_t  g_dec;
SRAM1 static int16_t   g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
SRAM1 static uint8_t   g_in[IN_BYTES];
SRAM1 static uint32_t  g_stack[STACK_WORDS];
SRAM1 static fat_file_t g_file;

/* ---- the FPU ----------------------------------------------------------------
 *
 * CPENABLE bit 0 lets coprocessor 0 (the FPU) execute. With it clear, the first
 * FP instruction raises Coprocessor0Disabled (EXCCAUSE 32), which is loud, so
 * a missing enable cannot pass as a wrong answer. It is a per-core register,
 * not per-task: set here, it is set for everyone -- which is why the build
 * check matters. */
static void fpu_enable(void)
{
    uint32_t v = 1u;
    __asm__ volatile ("wsr %0, cpenable\n\trsync" :: "r"(v));
}

static uint32_t fpu_enabled(void)
{
    uint32_t v;
    __asm__ volatile ("rsr %0, cpenable" : "=r"(v));
    return v;
}

/* ---- the job the shell hands the decoder task ----------------------------- */

enum { JOB_IDLE, JOB_PENDING, JOB_RUNNING, JOB_DONE };

static struct {
    volatile int state;
    char     path[96];
    uint32_t frames_wanted;

    /* results */
    int      err;                       /* fat_err_t, or 1 = no frame decoded */
    uint32_t id3;
    uint32_t hz, channels, kbps_min, kbps_max;
    uint32_t frames, samples;           /* samples per channel */
    uint32_t skipped;                   /* frame_bytes > 0 but no samples */
    uint32_t dec_ms, dec_cc;            /* decoder's own CPU, decode only */
    uint32_t worst_cc;                  /* slowest single frame */
    uint32_t read_ms, read_cc;          /* decoder's own CPU, SD reads */
    uint32_t bytes;
    int32_t  peak;
    uint32_t abs_sum_hi, abs_sum_lo;    /* sum |sample| as 64 bits, by hand */
    uint32_t nsamp;
    uint32_t cpen;
} g_job;

static int g_task = -1;

static void add_ms(uint32_t *ms, uint32_t *cc, uint32_t d)
{
    *cc += d;
    while (*cc >= CPU_HZ / 1000u) {
        *cc -= CPU_HZ / 1000u;
        (*ms)++;
    }
}

static void bench(void)
{
    g_job.err = fat_open(&g_file, g_job.path);
    if (g_job.err) {
        return;
    }

    /* Seek past the ID3 tag: these files carry up to 776 KB of cover art. */
    uint8_t t[10];
    g_job.id3 = (fat_read(&g_file, t, 10u) == 10) ? mp3hdr_id3_size(t) : 0u;
    if (fat_seek(&g_file, g_job.id3) != FAT_OK) {
        g_job.err = FAT_ERR_CHAIN;
        return;
    }

    mp3dec_init(&g_dec);
    uint32_t fill = 0, pos = 0;
    int eof = 0;
    g_job.kbps_min = 0xFFFFu;

    while (g_job.frames < g_job.frames_wanted) {
        /* Keep at least 2 KB ahead -- more than any one frame (1,441 bytes at
         * 320 kbps / 32 kHz) -- by sliding what is left down and topping up. */
        if (!eof && fill - pos < 2048u) {
            memmove(g_in, g_in + pos, fill - pos);
            fill -= pos;
            pos = 0;
            uint32_t c0 = task_cpu_cycles();
            int32_t got = fat_read(&g_file, g_in + fill, IN_BYTES - fill);
            add_ms(&g_job.read_ms, &g_job.read_cc, task_cpu_cycles() - c0);
            if (got < 0) {
                g_job.err = got;
                return;
            }
            if (got == 0) {
                eof = 1;
            }
            fill += (uint32_t)got;
            g_job.bytes += (uint32_t)got;
        }
        if (fill == pos) {
            break;
        }

        mp3dec_frame_info_t info;
        uint32_t c0 = task_cpu_cycles();
        int n = mp3dec_decode_frame(&g_dec, g_in + pos, (int)(fill - pos), g_pcm, &info);
        uint32_t d = task_cpu_cycles() - c0;

        if (info.frame_bytes == 0) {
            if (eof) {
                break;
            }
            /* Nothing found in what is buffered: drop it and read on. */
            pos = fill;
            continue;
        }
        pos += (uint32_t)info.frame_bytes;
        if (n == 0) {
            g_job.skipped++;
            continue;
        }

        add_ms(&g_job.dec_ms, &g_job.dec_cc, d);
        if (d > g_job.worst_cc) {
            g_job.worst_cc = d;
        }
        g_job.frames++;
        g_job.samples += (uint32_t)n;
        g_job.hz = (uint32_t)info.hz;
        g_job.channels = (uint32_t)info.channels;
        if ((uint32_t)info.bitrate_kbps < g_job.kbps_min) { g_job.kbps_min = (uint32_t)info.bitrate_kbps; }
        if ((uint32_t)info.bitrate_kbps > g_job.kbps_max) { g_job.kbps_max = (uint32_t)info.bitrate_kbps; }

        /* Evidence the output is audio: its peak, and its mean level. Zeros
         * or a stuck value would show here long before anyone listens. */
        uint32_t total = (uint32_t)n * (uint32_t)info.channels;
        for (uint32_t i = 0; i < total; i++) {
            int32_t s = g_pcm[i];
            uint32_t a = (uint32_t)(s < 0 ? -s : s);
            if ((int32_t)a > g_job.peak) {
                g_job.peak = (int32_t)a;
            }
            uint32_t lo = g_job.abs_sum_lo + a;
            if (lo < g_job.abs_sum_lo) {
                g_job.abs_sum_hi++;
            }
            g_job.abs_sum_lo = lo;
        }
        g_job.nsamp += total;
    }
    g_job.err = g_job.frames ? 0 : 1;
}

static void mp3_task(void)
{
    fpu_enable();
    for (;;) {
        if (g_job.state == JOB_PENDING) {
            g_job.state = JOB_RUNNING;
            fpu_enable();               /* cheap, and nothing may have cleared it */
            g_job.cpen = fpu_enabled();
            bench();
            g_job.state = JOB_DONE;
        }
        task_sleep(2u);
    }
}

/* ---- shell ------------------------------------------------------------------ */

static void put_ms(uint32_t ms)
{
    uart_put_dec(ms);
    uart_puts(" ms");
}

static void report(void)
{
    if (g_job.err && g_job.err != 1) {
        uart_puts("   ");
        uart_puts(fat_strerror(g_job.err));
        uart_puts("\n");
        return;
    }
    uart_puts("   cpenable=");
    uart_put_hex(g_job.cpen);
    uart_puts("  id3 skipped=");
    uart_put_dec(g_job.id3);
    uart_puts("  bytes read=");
    uart_put_dec(g_job.bytes);
    uart_puts("\n");
    if (g_job.err == 1) {
        uart_puts("   no frame decoded\n");
        return;
    }
    uint32_t audio_ms = g_job.samples / (g_job.hz / 1000u);
    uart_puts("   ");
    uart_put_dec(g_job.frames);
    uart_puts(" frames  ");
    uart_put_dec(g_job.hz);
    uart_puts(" Hz  ");
    uart_put_dec(g_job.channels);
    uart_puts(" ch  ");
    uart_put_dec(g_job.kbps_min);
    uart_puts("-");
    uart_put_dec(g_job.kbps_max);
    uart_puts(" kbps  skipped=");
    uart_put_dec(g_job.skipped);
    uart_puts("\n   audio        ");
    put_ms(audio_ms);
    uart_puts("\n   decode CPU   ");
    put_ms(g_job.dec_ms);
    uart_puts("   = ");
    uart_put_dec(audio_ms ? g_job.dec_ms * 100u / audio_ms : 0u);
    uart_puts("% of real time   per frame avg ");
    uart_put_dec(g_job.frames ? g_job.dec_ms * 1000u / g_job.frames : 0u);
    uart_puts(" us, worst ");
    uart_put_dec(g_job.worst_cc / (CPU_HZ / 1000000u));
    uart_puts(" us\n   SD read CPU  ");
    put_ms(g_job.read_ms);
    uart_puts("   = ");
    uart_put_dec(audio_ms ? g_job.read_ms * 100u / audio_ms : 0u);
    uart_puts("% of real time\n   output peak=");
    uart_put_dec((uint32_t)g_job.peak);
    uart_puts("  mean |s|=");
    /* sum / n without 64-bit division: n < 2^24 here, so shift both down. */
    uint32_t n = g_job.nsamp ? g_job.nsamp : 1u;
    uint32_t hi = g_job.abs_sum_hi, lo = g_job.abs_sum_lo, sh = 0;
    while (hi) {
        lo = (lo >> 1) | (hi << 31);
        hi >>= 1;
        sh++;
    }
    uart_put_dec((lo / n) << sh);
    uart_puts("   (of 32767)\n");
}

static uint32_t parse_u32(const char *s, const char **end)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s++ - '0');
    }
    *end = s;
    return v;
}

void mp3_shell(char *arg)
{
    char *sub = arg;
    char *rest = arg;
    while (*rest && *rest != ' ') { rest++; }
    if (*rest) {
        *rest++ = 0;
        while (*rest == ' ') { rest++; }
    }

    if (sub[0] == 'b' && sub[1] == 'e') {                   /* bench */
        const char *p;
        uint32_t frames = parse_u32(rest, &p);
        while (*p == ' ') { p++; }
        if (!frames || !*p) {
            uart_puts("   mp3 bench <frames> <file>\n");
            return;
        }
        if (!fat_mounted() && fat_mount() != FAT_OK) {
            uart_puts("   card not mounted\n");
            return;
        }
        if (g_task < 0) {
            g_task = task_create_with_stack("mp3", mp3_task, g_stack, STACK_WORDS);
            if (g_task < 0) {
                uart_puts("   task table full\n");
                return;
            }
        }
        if (g_job.state == JOB_PENDING || g_job.state == JOB_RUNNING) {
            uart_puts("   busy\n");
            return;
        }
        /* Fresh results; the path copied because `arg` is the shell's line. */
        uint8_t *z = (uint8_t *)&g_job;
        for (uint32_t i = 0; i < sizeof g_job; i++) {
            z[i] = 0;
        }
        uint32_t i = 0;
        for (; p[i] && i < sizeof g_job.path - 1u; i++) {
            g_job.path[i] = p[i];
        }
        g_job.path[i] = 0;
        g_job.frames_wanted = frames;
        g_job.state = JOB_PENDING;

        uart_puts("   decoding on task 'mp3'...\n");
        /* Wait without holding the console: tasks that print would block on
         * it and their time would be lost from the comparison. */
        console_unlock();
        while (g_job.state != JOB_DONE) {
            task_sleep(5u);
        }
        console_lock();
        g_job.state = JOB_IDLE;
        report();
    } else {
        uart_puts("   mp3 bench <frames> <file>   decode N frames; cost in the decoder's\n"
                  "                               own CPU time against the audio's length\n");
    }
}
