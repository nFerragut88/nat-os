/* nat-os — video playback. See vplay.h; the file format is specified in
 * tools/vidconv.py's docstring and the offsets below are read from it. */

#include "vplay.h"
#include "fat.h"
#include "pcm.h"
#include "display.h"
#include "task.h"
#include "timer.h"

#define CPU_HZ 80000000u

/* How long either wait on the audio clock may go without finishing. Five
 * seconds is far longer than a frame (143 ms) or a drain (the ring is 371 ms)
 * and far shorter than a person's patience. */
#define STALL_TICKS 500u

/* .nvd header (vidconv.py HEADER) */
#define NV_VERSION   4u
#define NV_W         8u
#define NV_H         10u
#define NV_FPS_NUM   12u
#define NV_FPS_DEN   14u
#define NV_PIX       16u
#define NV_FRAMES    20u
#define NV_FRAME_B   24u
#define NV_RATE      28u
#define NV_AFMT      32u
#define NV_AUD_MAX   36u
#define NV_STRIDE    40u
#define NV_FIRST     44u
#define PIX_PAL8     1u
#define PIX_RGB565   2u
#define AUD_U8_MONO  1u

static vplay_status_t g_st;
static uint16_t g_pal[256];
static fat_file_t g_fa, g_fv;           /* the audio cursor and the video cursor */

/* Per-file values the loop needs. */
static uint32_t g_first, g_stride, g_frame_b, g_rate, g_fnum, g_fden;
static int g_mute;

void vplay_set_mute(int on) { g_mute = on ? 1 : 0; }
int  vplay_muted(void)      { return g_mute; }

void vplay_status(vplay_status_t *st)
{
    *st = g_st;
}

const char *vplay_error_text(int e)
{
    switch (e) {
    case VPLAY_E_FORMAT: return "not a playable .nvd";
    case VPLAY_E_PCM:    return "audio output refused";
    case VPLAY_E_STALL:  return "audio clock stopped";
    default:             return fat_strerror(e);
    }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void add_ms(uint32_t *ms, uint32_t *cc, uint32_t d)
{
    *cc += d;
    while (*cc >= CPU_HZ / 1000u) {
        *cc -= CPU_HZ / 1000u;
        (*ms)++;
    }
}

/* Audio sample (in the file) where frame i starts: the same rounding
 * vidconv.py uses to cut the audio, so chunk boundaries agree exactly. */
static uint32_t a_of(uint32_t i)
{
    return (i * g_rate * g_fden + g_fnum / 2u) / g_fnum;
}

/* Queues chunk i's audio. Blocks (sleeping) while the ring is full. Returns
 * 0, or a fat error. u8 -> s16 because pcm_write() takes signed 16-bit; it
 * goes back to 8 bits on the way into the ring. */
static int queue_audio(uint32_t i, volatile int *stop)
{
    uint8_t h[16];
    uint32_t base = g_first + i * g_stride;
    if (fat_seek(&g_fa, base) != FAT_OK || fat_read(&g_fa, h, 16u) != 16) {
        return FAT_ERR_CHAIN;
    }
    if (h[0] != 'N' || h[1] != 'V' || h[2] != 'C' || h[3] != 'K' || rd32(h + 4) != i) {
        return VPLAY_E_FORMAT;
    }
    uint32_t left = rd32(h + 8);
    if (fat_seek(&g_fa, base + 16u + g_frame_b) != FAT_OK) {
        return FAT_ERR_CHAIN;
    }
    uint8_t raw[128];
    int16_t s[128];
    while (left && !*stop) {
        uint32_t n = left < sizeof raw ? left : sizeof raw;
        if (fat_read(&g_fa, raw, n) != (int32_t)n) {
            return FAT_ERR_CHAIN;
        }
        for (uint32_t k = 0; k < n; k++) {
            s[k] = (int16_t)(((int32_t)raw[k] - 128) << 8);
        }
        uint32_t w = 0;
        while (w < n && !*stop) {
            uint32_t k = pcm_write(s + w, n - w);
            w += k;
            if (k == 0) {
                task_sleep(1u);
            }
        }
        left -= n;
    }
    return 0;
}

/* Reads frame i and puts it on the panel, `rows` rows at a time. */
static int draw_frame(uint32_t i, uint32_t x, uint32_t y, uint32_t pix,
                      uint8_t *a, uint16_t *b, uint32_t rows,
                      uint32_t *read_cc, uint32_t *draw_cc, volatile int *stop)
{
    uint32_t w = g_st.w, h = g_st.h;
    uint32_t c0 = task_cpu_cycles();
    if (fat_seek(&g_fv, g_first + i * g_stride + 16u) != FAT_OK) {
        return FAT_ERR_CHAIN;
    }
    *read_cc += task_cpu_cycles() - c0;
    for (uint32_t r = 0; r < h; r += rows) {
        /* Checked per batch, not per frame: leaving the view stops the video
         * and then repaints the launcher, and a frame finishing after that
         * would land on top of the icons. */
        if (*stop) {
            return 0;
        }
        uint32_t n = (h - r < rows) ? h - r : rows;
        uint32_t t0 = task_cpu_cycles();
        if (pix == PIX_PAL8) {
            if (fat_read(&g_fv, a, n * w) != (int32_t)(n * w)) {
                return FAT_ERR_CHAIN;
            }
            uint32_t t1 = task_cpu_cycles();
            *read_cc += t1 - t0;
            for (uint32_t k = 0; k < n * w; k++) {
                b[k] = g_pal[a[k]];
            }
            display_blit(x, y + r, w, n, b, w);
            *draw_cc += task_cpu_cycles() - t1;
        } else {
            if (fat_read(&g_fv, b, n * w * 2u) != (int32_t)(n * w * 2u)) {
                return FAT_ERR_CHAIN;
            }
            uint32_t t1 = task_cpu_cycles();
            *read_cc += t1 - t0;
            display_blit(x, y + r, w, n, b, w);
            *draw_cc += task_cpu_cycles() - t1;
        }
    }
    return 0;
}

int vplay_run(const char *path, uint32_t y, volatile int *stop,
              uint8_t *a, uint32_t a_bytes, uint16_t *b, uint32_t b_words)
{
    uint8_t *z = (uint8_t *)&g_st;
    for (uint32_t k = 0; k < sizeof g_st; k++) {
        z[k] = 0;
    }

    int rc = fat_open(&g_fv, path);
    if (rc) {
        return g_st.err = rc;
    }
    if (a_bytes < 1024u || fat_read(&g_fv, a, 1024u) != 1024) {
        return g_st.err = VPLAY_E_FORMAT;
    }
    if (a[0] != 'N' || a[1] != 'V' || a[2] != 'I' || a[3] != 'D' || rd16(a + NV_VERSION) != 1u) {
        return g_st.err = VPLAY_E_FORMAT;
    }
    uint32_t pix = a[NV_PIX];
    g_st.w = rd16(a + NV_W);
    g_st.h = rd16(a + NV_H);
    g_fnum = g_st.fps_num = rd16(a + NV_FPS_NUM);
    g_fden = g_st.fps_den = rd16(a + NV_FPS_DEN);
    g_st.frames = rd32(a + NV_FRAMES);
    g_frame_b = rd32(a + NV_FRAME_B);
    g_rate = rd32(a + NV_RATE);
    uint32_t afmt = a[NV_AFMT];
    g_stride = rd32(a + NV_STRIDE);
    g_first = rd32(a + NV_FIRST);
    uint32_t bpp = (pix == PIX_PAL8) ? 1u : 2u;

    /* Everything the loop will trust, checked once. A bad header must not
     * become a blit off the panel or a divide by zero. */
    if ((pix != PIX_PAL8 && pix != PIX_RGB565) || !g_st.w || g_st.w > DISP_W
        || !g_st.h || g_st.h > DISP_H - y || g_frame_b != g_st.w * g_st.h * bpp
        || !g_fnum || !g_fden || g_stride % 512u || g_first % 512u
        || g_stride < 16u + g_frame_b
        || (afmt == AUD_U8_MONO && (g_rate < PCM_RATE_MIN || g_rate > PCM_RATE_MAX))) {
        return g_st.err = VPLAY_E_FORMAT;
    }
    for (uint32_t k = 0; k < 256u; k++) {
        g_pal[k] = rd16(a + 512u + 2u * k);
    }

    /* Rows per batch: as many as the smaller of the two borrowed buffers
     * holds (indices in `a`, RGB565 in `b`). */
    uint32_t rows = b_words / g_st.w;
    if (pix == PIX_PAL8 && a_bytes / g_st.w < rows) {
        rows = a_bytes / g_st.w;
    }
    if (rows == 0u) {
        return g_st.err = VPLAY_E_FORMAT;
    }

    int audio = (afmt == AUD_U8_MONO) && !g_mute;
    uint32_t spf = 0, ring = PCM_BUFS * PCM_SAMPLES, k_ahead = 0;
    if (audio) {
        /* See vplay.h for why (K + 1) frames of audio must fit the ring. */
        spf = (g_rate * g_fden + g_fnum - 1u) / g_fnum;
        k_ahead = (spf < ring) ? ring / spf : 1u;
        k_ahead = (k_ahead > 1u) ? k_ahead - 1u : 1u;
        if (pcm_start(g_rate) != 0) {
            return g_st.err = VPLAY_E_PCM;
        }
        rc = fat_open(&g_fa, path);
        if (rc) {
            pcm_stop();
            return g_st.err = rc;
        }
    }
    g_st.lookahead = k_ahead;

    /* The ring starts FULL of silence, so the file's first sample reaches the
     * DAC after that much has played. */
    uint32_t offset0 = ring;
    uint32_t x = (DISP_W - g_st.w) / 2u;
    uint32_t read_cc = 0, draw_cc = 0, aud_cc = 0;
    uint32_t start_tick = timer_ticks();
    uint32_t queued = 0;                        /* chunks whose audio is queued */

    for (uint32_t i = 0; i < g_st.frames && !*stop; i++) {
        g_st.frame = i;

        /* Keep the audio K chunks ahead of the picture. */
        while (audio && queued < g_st.frames && queued <= i + k_ahead && !*stop) {
            uint32_t c0 = task_cpu_cycles();
            rc = queue_audio(queued, stop);
            add_ms(&g_st.audio_ms, &aud_cc, task_cpu_cycles() - c0);
            if (rc) {
                g_st.err = rc;
                goto out;
            }
            queued++;
        }

        if (audio) {
            /* Wait for frame i's moment; if frame i+1's has already come,
             * this one is too late to be worth drawing.
             *
             * BOUNDED. This waits on the DAC, and if the DAC ever stops
             * advancing the wait never ends -- the task sleeps at 0% CPU,
             * looking idle, while `mp3 stop` blocks in wait_done() and the
             * shell stops answering. Observed once; the cause is not known,
             * so the wait is bounded rather than assumed safe. */
            uint32_t guard = timer_ticks() + STALL_TICKS;
            while (!*stop && pcm_samples_played() < offset0 + a_of(i)) {
                if ((int32_t)(timer_ticks() - guard) >= 0) {
                    g_st.err = VPLAY_E_STALL;
                    goto out;
                }
                task_sleep(1u);
            }
            if (pcm_samples_played() >= offset0 + a_of(i + 1u)) {
                g_st.dropped++;
                continue;
            }
        } else {
            /* No audio: the tick is the clock. */
            uint32_t due = start_tick + (i * 100u * g_fden) / g_fnum;
            while (!*stop && (int32_t)(timer_ticks() - due) < 0) {
                task_sleep(1u);
            }
        }

        uint32_t f0 = task_cpu_cycles();
        uint32_t rb = read_cc, db = draw_cc;
        rc = draw_frame(i, x, y, pix, a, b, rows, &read_cc, &draw_cc, stop);
        if (rc) {
            g_st.err = rc;
            goto out;
        }
        uint32_t fus = (task_cpu_cycles() - f0) / (CPU_HZ / 1000000u);
        if (fus > g_st.worst_frame_us) {
            g_st.worst_frame_us = fus;
        }
        /* Fold the cycle counts into ms as we go, so no counter wraps. */
        uint32_t rd = read_cc - rb, dd = draw_cc - db;
        read_cc = rb; draw_cc = db;
        add_ms(&g_st.read_ms, &read_cc, rd);
        add_ms(&g_st.draw_ms, &draw_cc, dd);
        g_st.shown++;
        g_st.wall_ms = (timer_ticks() - start_tick) * 10u;
    }

    /* Let the last frame's sound finish before the DAC is switched off --
     * feeding SILENCE behind it. Merely waiting let the ring run dry and
     * replay stale buffers: the first full play of the example counted 2
     * underruns, both here. The same lesson pcm.c's test and mp3.c's play()
     * learned. */
    uint32_t drain_guard = timer_ticks() + STALL_TICKS;
    while (audio && !*stop && pcm_samples_played() < offset0 + a_of(g_st.frames)) {
        static const int16_t quiet[64] = { 0 };
        if (!pcm_write(quiet, 64u)) {
            task_sleep(1u);
        }
        if ((int32_t)(timer_ticks() - drain_guard) >= 0) {
            g_st.err = VPLAY_E_STALL;           /* same bound as the sync wait */
            break;
        }
    }
out:
    g_st.wall_ms = (timer_ticks() - start_tick) * 10u;
    if (audio) {
        pcm_stop();
    }
    return g_st.err;
}
