/* nat-os — MP3 decoding. See mp3.h. */

#include <stddef.h>
#include <stdint.h>
#include "mp3.h"
#include "mp3hdr.h"
#include "fat.h"
#include "task.h"
#include "console.h"
#include "uart.h"
#include "pcm.h"
#include "timer.h"

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

#define IN_BYTES     4096u              /* 2 KB kept ahead + a top-up; > 2 max frames */
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

/* ---- the job the shell hands the decoder task -----------------------------
 *
 * One task, one job at a time: minimp3's scratch is static (vendor README), so
 * two decodes at once would share it. BENCH decodes N frames and reports; PLAY
 * streams a whole file to the DAC and keeps its status live for `mp3`. */

enum { JOB_IDLE, JOB_PENDING, JOB_RUNNING, JOB_DONE };
enum { KIND_BENCH, KIND_PLAY };

static struct {
    volatile int state;
    volatile int stop;                  /* set by `mp3 stop`, read by the task */
    int      kind;
    char     path[96];
    uint32_t frames_wanted;             /* bench only */

    /* results, and for PLAY the live status */
    int      err;                       /* fat_err_t, or MP3_ERR_* below */
    uint32_t id3;
    uint32_t hz, channels, kbps_min, kbps_max;
    uint32_t frames, samples;           /* samples per channel */
    uint32_t skipped;                   /* frame_bytes > 0 but no samples */
    uint32_t dec_ms, dec_cc;            /* decoder's own CPU, decode only */
    uint32_t worst_cc;                  /* slowest single frame */
    uint32_t read_ms, read_cc;          /* decoder's own CPU, SD reads */
    uint32_t bytes, size;
    int32_t  peak;
    uint32_t abs_sum_hi, abs_sum_lo;    /* sum |sample| as 64 bits, by hand */
    uint32_t nsamp;
    uint32_t cpen;
    uint32_t start_tick;                /* play: when the first sample was queued */
    uint32_t end_tick;                  /* play: when it stopped; 0 while playing */
    uint32_t full_waits;                /* play: times the ring was full, so it slept */
} g_job;

#define MP3_ERR_NOFRAME  1
#define MP3_ERR_RATE     2              /* sample rate the DAC cannot produce */
#define MP3_ERR_PCM      3

static int g_task = -1;

/* The file and the input window, shared by both jobs. */
static uint32_t g_fill, g_pos;
static int      g_eof;

static void add_ms(uint32_t *ms, uint32_t *cc, uint32_t d)
{
    *cc += d;
    while (*cc >= CPU_HZ / 1000u) {
        *cc -= CPU_HZ / 1000u;
        (*ms)++;
    }
}

static int open_track(void)
{
    int rc = fat_open(&g_file, g_job.path);
    if (rc) {
        return rc;
    }
    g_job.size = g_file.size;
    /* Seek past the ID3 tag: these files carry up to 776 KB of cover art. */
    uint8_t t[10];
    g_job.id3 = (fat_read(&g_file, t, 10u) == 10) ? mp3hdr_id3_size(t) : 0u;
    if (fat_seek(&g_file, g_job.id3) != FAT_OK) {
        return FAT_ERR_CHAIN;
    }
    mp3dec_init(&g_dec);
    g_fill = g_pos = 0;
    g_eof = 0;
    g_job.kbps_min = 0xFFFFu;
    return 0;
}

/* Keeps at least 2 KB ahead -- more than any one frame (1,441 bytes at
 * 320 kbps / 32 kHz) -- by sliding what is left down and topping up. */
static int refill(void)
{
    if (g_eof || g_fill - g_pos >= 2048u) {
        return 0;
    }
    memmove(g_in, g_in + g_pos, g_fill - g_pos);
    g_fill -= g_pos;
    g_pos = 0;
    uint32_t c0 = task_cpu_cycles();
    int32_t got = fat_read(&g_file, g_in + g_fill, IN_BYTES - g_fill);
    add_ms(&g_job.read_ms, &g_job.read_cc, task_cpu_cycles() - c0);
    if (got < 0) {
        return got;
    }
    if (got == 0) {
        g_eof = 1;
    }
    g_fill += (uint32_t)got;
    g_job.bytes += (uint32_t)got;
    return 0;
}

/* Decodes the next frame into g_pcm. Returns samples per channel, 0 at the end
 * of the file, or a negative fat_err_t. Skips frames that yield nothing. */
static int next_frame(mp3dec_frame_info_t *info)
{
    for (;;) {
        int rc = refill();
        if (rc) {
            return rc;
        }
        if (g_fill == g_pos) {
            return 0;
        }
        uint32_t c0 = task_cpu_cycles();
        int n = mp3dec_decode_frame(&g_dec, g_in + g_pos, (int)(g_fill - g_pos), g_pcm, info);
        uint32_t d = task_cpu_cycles() - c0;

        if (info->frame_bytes == 0) {
            if (g_eof) {
                return 0;
            }
            g_pos = g_fill;             /* nothing in what is buffered: read on */
            continue;
        }
        g_pos += (uint32_t)info->frame_bytes;
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
        g_job.hz = (uint32_t)info->hz;
        g_job.channels = (uint32_t)info->channels;
        if ((uint32_t)info->bitrate_kbps < g_job.kbps_min) { g_job.kbps_min = (uint32_t)info->bitrate_kbps; }
        if ((uint32_t)info->bitrate_kbps > g_job.kbps_max) { g_job.kbps_max = (uint32_t)info->bitrate_kbps; }
        return n;
    }
}

/* Evidence the output is audio: its peak, and its mean level. Zeros or a stuck
 * value would show here long before anyone listens. */
static void measure(const int16_t *s, uint32_t total)
{
    for (uint32_t i = 0; i < total; i++) {
        int32_t v = s[i];
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
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

static void bench(void)
{
    g_job.err = open_track();
    if (g_job.err) {
        return;
    }
    mp3dec_frame_info_t info;
    while (g_job.frames < g_job.frames_wanted) {
        int n = next_frame(&info);
        if (n <= 0) {
            if (n < 0) {
                g_job.err = n;
                return;
            }
            break;
        }
        measure(g_pcm, (uint32_t)n * (uint32_t)info.channels);
    }
    g_job.err = g_job.frames ? 0 : MP3_ERR_NOFRAME;
}

/* Queues n mono samples, sleeping a tick whenever the ring is full.
 *
 * The sleep is what makes running at HIGH priority safe (task.h: strict
 * priority is only safe because tasks sleep). The ring holds 170 ms and a tick
 * is 10 ms, so the task wakes with the ring still nearly full, decodes the
 * next frame (~11 ms of CPU for 24 ms of audio) and sleeps again. */
static void queue(const int16_t *s, uint32_t n)
{
    uint32_t w = 0;
    while (w < n && !g_job.stop) {
        uint32_t k = pcm_write(s + w, n - w);
        w += k;
        if (k == 0) {
            g_job.full_waits++;
            task_sleep(1u);
        }
    }
}

static void play(void)
{
    g_job.err = open_track();
    if (g_job.err) {
        return;
    }
    mp3dec_frame_info_t info;
    int started = 0;

    while (!g_job.stop) {
        int n = next_frame(&info);
        if (n <= 0) {
            if (n < 0) {
                g_job.err = n;
            }
            break;
        }
        if (!started) {
            /* The DAC's clock cannot go below ~19.6 kHz (pcm.h). Refuse rather
             * than play a 16 kHz file at the wrong speed. */
            if ((uint32_t)info.hz < PCM_RATE_MIN || (uint32_t)info.hz > PCM_RATE_MAX) {
                g_job.err = MP3_ERR_RATE;
                return;
            }
            if (pcm_start((uint32_t)info.hz) != 0) {
                g_job.err = MP3_ERR_PCM;
                return;
            }
            started = 1;
            g_job.start_tick = timer_ticks();
        }
        /* Stereo to mono, in place: output i reads inputs 2i and 2i+1, which
         * are never behind it, so nothing is overwritten before it is read. */
        if (info.channels == 2) {
            for (int i = 0; i < n; i++) {
                g_pcm[i] = (int16_t)(((int32_t)g_pcm[2 * i] + g_pcm[2 * i + 1]) >> 1);
            }
        }
        measure(g_pcm, (uint32_t)n);
        queue(g_pcm, (uint32_t)n);
    }

    if (started) {
        /* A ring's worth of silence behind the last frame, so the DMA plays
         * out the tail instead of looping stale buffers. Skipped on stop:
         * whoever pressed stop wants silence now, not in 170 ms. */
        if (!g_job.stop) {
            static const int16_t zero[64] = { 0 };
            for (uint32_t i = 0; i < PCM_BUFS * PCM_SAMPLES / 64u; i++) {
                queue(zero, 64u);
            }
        }
        pcm_stop();
        /* Stop the clock here, so a status read long after the song ended does
         * not divide the song's CPU by minutes of silence (step 5: it did). */
        g_job.end_tick = timer_ticks();
    }
    if (!g_job.err && !g_job.frames) {
        g_job.err = MP3_ERR_NOFRAME;
    }
}

static void mp3_task(void)
{
    fpu_enable();
    for (;;) {
        if (g_job.state == JOB_PENDING) {
            g_job.state = JOB_RUNNING;
            fpu_enable();               /* cheap, and nothing may have cleared it */
            g_job.cpen = fpu_enabled();
            if (g_job.kind == KIND_PLAY) {
                play();
            } else {
                bench();
            }
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

static const char *err_text(int e)
{
    switch (e) {
    case MP3_ERR_NOFRAME: return "no frame decoded";
    case MP3_ERR_RATE:    return "sample rate outside what the DAC can play (19.6-48 kHz)";
    case MP3_ERR_PCM:     return "pcm_start refused";
    default:              return fat_strerror(e);
    }
}

static uint32_t mean_abs(void)
{
    /* sum / n without 64-bit division: shift both down until the sum fits. */
    uint32_t n = g_job.nsamp ? g_job.nsamp : 1u;
    uint32_t hi = g_job.abs_sum_hi, lo = g_job.abs_sum_lo, sh = 0;
    while (hi) {
        lo = (lo >> 1) | (hi << 31);
        hi >>= 1;
        sh++;
    }
    return (lo / n) << sh;
}

static void report_bench(void)
{
    if (g_job.err && g_job.err != MP3_ERR_NOFRAME) {
        uart_puts("   ");
        uart_puts(err_text(g_job.err));
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
    if (g_job.err == MP3_ERR_NOFRAME) {
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
    uart_put_dec(mean_abs());
    uart_puts("   (of 32767)\n");
}

/* `mp3` while playing (or after): where it is, and whether it is keeping up.
 * The two numbers that say "stutter" are underruns (the DMA replayed a stale
 * buffer) and the decoder's CPU against the wall clock since it started. */
static void report_play(void)
{
    const char *st = (g_job.state == JOB_RUNNING) ? "PLAYING" :
                     (g_job.state == JOB_DONE)    ? "finished" : "idle";
    uart_puts("   ");
    uart_puts(st);
    uart_puts("  ");
    uart_puts(g_job.path);
    uart_puts("\n");
    if (g_job.err) {
        uart_puts("   error: ");
        uart_puts(err_text(g_job.err));
        uart_puts("\n");
    }
    if (!g_job.hz) {
        return;
    }
    uint32_t pos_s = g_job.samples / g_job.hz;
    uart_puts("   at ");
    uart_put_dec(pos_s / 60u);
    uart_puts(":");
    if (pos_s % 60u < 10u) { uart_putc('0'); }
    uart_put_dec(pos_s % 60u);
    uart_puts("  (");
    uart_put_dec(g_job.size ? (g_job.id3 + g_job.bytes) / (g_job.size / 100u + 1u) : 0u);
    uart_puts("% of the file)  ");
    uart_put_dec(g_job.hz);
    uart_puts(" Hz ");
    uart_put_dec(g_job.channels);
    uart_puts(" ch  ");
    uart_put_dec(g_job.kbps_min);
    uart_puts("-");
    uart_put_dec(g_job.kbps_max);
    uart_puts(" kbps\n   underruns=");
    uart_put_dec(pcm_underruns());
    uart_puts(" blind=");
    uart_put_dec(pcm_blind());
    uart_puts("  ring-full waits=");
    uart_put_dec(g_job.full_waits);
    uart_puts("  dac rate measured=");
    uart_put_dec(pcm_rate_actual());
    uint32_t now = g_job.end_tick ? g_job.end_tick : timer_ticks();
    uint32_t wall_ms = (now - g_job.start_tick) * 10u;
    uart_puts("\n   decode CPU ");
    put_ms(g_job.dec_ms);
    uart_puts(" + SD ");
    put_ms(g_job.read_ms);
    uart_puts(" over ");
    put_ms(wall_ms);
    uart_puts(" wall = ");
    uart_put_dec(wall_ms ? (g_job.dec_ms + g_job.read_ms) * 100u / wall_ms : 0u);
    uart_puts("%   worst frame ");
    uart_put_dec(g_job.worst_cc / (CPU_HZ / 1000000u));
    uart_puts(" us\n   output peak=");
    uart_put_dec((uint32_t)g_job.peak);
    uart_puts(" mean |s|=");
    uart_put_dec(mean_abs());
    uart_puts("\n");
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

/* Hands a job to the decoder task, creating it on first use. */
static int submit(int kind, const char *path, uint32_t frames)
{
    if (!fat_mounted() && fat_mount() != FAT_OK) {
        uart_puts("   card not mounted\n");
        return 0;
    }
    if (g_task < 0) {
        g_task = task_create_with_stack("mp3", mp3_task, g_stack, STACK_WORDS);
        if (g_task < 0) {
            uart_puts("   task table full\n");
            return 0;
        }
        /* AUDIO, above the display (task.h). At HIGH it got 23% of the CPU
         * and underran 465 times in 30 s. Safe only because queue() sleeps
         * whenever the ring is full -- see there. */
        task_set_priority(g_task, TASK_PRIO_AUDIO);
    }
    if (g_job.state == JOB_PENDING || g_job.state == JOB_RUNNING) {
        uart_puts("   busy -- 'mp3 stop' first\n");
        return 0;
    }
    uint8_t *z = (uint8_t *)&g_job;
    for (uint32_t i = 0; i < sizeof g_job; i++) {
        z[i] = 0;
    }
    uint32_t i = 0;
    for (; path[i] && i < sizeof g_job.path - 1u; i++) {
        g_job.path[i] = path[i];
    }
    g_job.path[i] = 0;
    g_job.kind = kind;
    g_job.frames_wanted = frames;
    g_job.state = JOB_PENDING;
    return 1;
}

/* Waits for the job without holding the console: tasks that print would
 * otherwise block on it, and their time would be lost from the comparison. */
static void wait_done(void)
{
    console_unlock();
    while (g_job.state == JOB_PENDING || g_job.state == JOB_RUNNING) {
        task_sleep(5u);
    }
    console_lock();
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
        if (submit(KIND_BENCH, p, frames)) {
            uart_puts("   decoding on task 'mp3'...\n");
            wait_done();
            report_bench();
            g_job.state = JOB_IDLE;
        }
    } else if (sub[0] == 'p' && sub[1] == 'l') {            /* play */
        if (!*rest) {
            uart_puts("   mp3 play <file>\n");
            return;
        }
        if (submit(KIND_PLAY, rest, 0)) {
            uart_puts("   playing on task 'mp3' -- 'mp3' for status, 'mp3 stop' to stop\n");
        }
    } else if (sub[0] == 's' && sub[1] == 't') {            /* stop */
        if (g_job.state == JOB_RUNNING || g_job.state == JOB_PENDING) {
            g_job.stop = 1;
            wait_done();
        }
        report_play();
    } else if (!*sub) {
        if (g_job.kind == KIND_PLAY) {
            report_play();
        } else {
            uart_puts("   no song has been played\n");
        }
    } else {
        uart_puts("   mp3                      status of what is playing\n"
                  "   mp3 play <file>          play it (a name ending * matches a prefix)\n"
                  "   mp3 stop\n"
                  "   mp3 bench <frames> <f>   decode N frames; cost in the decoder's own CPU\n");
    }
}
