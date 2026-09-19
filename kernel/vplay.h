/* nat-os — video playback: a .nvd file to the panel and the DAC.
 *
 * next_moves/12 step 4. Runs on the MP3 player's task (AUDIO priority) as a
 * second kind of job, and borrows that player's buffers: only one of the two
 * can play at a time, and SRAM1 has 2.5 KB left.
 *
 * ---- the clock is the DAC ----------------------------------------------------
 *
 * Frame i is shown when the DAC has played up to frame i's first audio sample
 * (pcm_samples_played()). A frame whose NEXT frame's time has already come is
 * not drawn -- it is dropped, and counted -- so falling behind costs frames,
 * never sync.
 *
 * ---- two cursors through one file --------------------------------------------
 *
 * Audio must be queued ahead of the picture or the ring runs dry, and a frame
 * (32 KB) cannot be held in RAM to wait for its sound. The .nvd layout makes
 * the answer cheap: every chunk is the same size, so one cursor reads audio K
 * chunks ahead while another reads the frame being shown, each by arithmetic.
 *
 * K is bounded by the ring: when chunk i+K's audio is queued, the ring must
 * still hold everything from frame i onward, so (K + 1) frames of audio must
 * fit in PCM_BUFS x PCM_SAMPLES. At 22,050 Hz and 10 fps that is K = 2 -- with
 * 3 the play position would already be past frame i when its turn came, and
 * every frame would be dropped.
 */

#ifndef NATOS_VPLAY_H
#define NATOS_VPLAY_H

#include <stdint.h>

typedef struct {
    uint32_t frame;         /* the frame being played */
    uint32_t frames;        /* in the file */
    uint32_t shown;         /* frames drawn */
    uint32_t dropped;       /* frames skipped because their time had passed */
    uint32_t fps_num, fps_den;
    uint32_t w, h;
    uint32_t read_ms;       /* this task's CPU reading frames from the card */
    uint32_t draw_ms;       /* ... converting through the palette and blitting */
    uint32_t audio_ms;      /* ... reading and queueing audio */
    uint32_t worst_frame_us;/* slowest single frame, read + draw */
    uint32_t wall_ms;       /* from the first frame to now or the end */
    uint32_t lookahead;     /* K, the audio chunks queued ahead */
    int      err;           /* 0, or VPLAY_E_* / a fat_err_t */
} vplay_status_t;

#define VPLAY_E_FORMAT  (-201)  /* not a .nvd this player understands */
#define VPLAY_E_PCM     (-202)  /* the DAC refused the rate */

/* Plays `path` with its frames centred horizontally at row `y`. Blocks until
 * the end of the file or until *stop goes non-zero. Borrows `a` (bytes) and
 * `b` (16-bit words) as work buffers for as long as it runs. */
int vplay_run(const char *path, uint32_t y, volatile int *stop,
              uint8_t *a, uint32_t a_bytes, uint16_t *b, uint32_t b_words);

/* A snapshot, safe to take from another task while a video plays. */
void vplay_status(vplay_status_t *st);

const char *vplay_error_text(int e);

#endif /* NATOS_VPLAY_H */
