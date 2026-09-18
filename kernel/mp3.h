/* nat-os — MP3 decoding: minimp3 on its own task, in SRAM1, on the FPU.
 *
 * Step 4 of docs/next_moves/11 is only the measurement: can this 80 MHz core
 * decode these files in real time at all? `mp3 bench` decodes N frames and
 * reports the cost in the DECODER TASK'S OWN CYCLES, against how long those
 * frames last. Wall-clock would fold in the ~75% of the CPU the display task
 * currently takes (step 3c) and answer a different question.
 *
 * ---- three things this file is the only owner of ---------------------------
 *
 *   SRAM1 (0x3FFF1000, 60 KB, linker.ld). Decoder state, scratch, buffers and
 *   the task's stack all live there, so the decoder costs the heap nothing.
 *
 *   The FPU. CPENABLE is set once, by the decoder task. nat-os does NOT save
 *   FPU registers on a task switch, which is only correct while one task uses
 *   them, so build.ps1 fails any build in which an object other than mp3.c
 *   contains an FPU instruction.
 *
 *   __divsf3. See mp3.c for why it is here and what it does not handle.
 */

#ifndef NATOS_MP3_H
#define NATOS_MP3_H

#include <stdint.h>

void mp3_shell(char *arg);

/* ---- the player, for the music view (next_moves/11 step 6) ------------------
 *
 * Nothing here blocks: the view calls these from the touch and display tasks,
 * which must never wait on a decoder. mp3_play() is refused while a song is
 * still stopping, so a view that wants to switch tracks requests a stop and
 * starts the next one when mp3_busy() goes false. */

#define MP3_E_NOCARD  (-101)
#define MP3_E_NOTASK  (-102)
#define MP3_E_BUSY    (-103)

enum {
    MP3_ST_IDLE,        /* nothing has been played */
    MP3_ST_STARTING,    /* submitted, first frame not yet out */
    MP3_ST_PLAYING,
    MP3_ST_PAUSED,
    MP3_ST_FINISHED,    /* reached the end of the file */
    MP3_ST_STOPPED,     /* stopped on request */
    MP3_ST_ERROR,
};

typedef struct {
    int      state;     /* MP3_ST_* */
    int      err;       /* when state is ERROR: see mp3_error_text() */
    uint32_t seq;       /* changes with every song started */
    uint32_t pos_s;     /* seconds played */
    uint32_t total_s;   /* length; 0 while unknown */
    int      exact;     /* total_s came from the file's Xing/Info header */
} mp3_status_t;

int  mp3_play(const char *path);    /* 0, or MP3_E_* */
void mp3_request_stop(void);
void mp3_set_pause(int on);
int  mp3_busy(void);
void mp3_status(mp3_status_t *st);
const char *mp3_error_text(int e);

/* Output volume in sixteenths, 0 (silent) .. MP3_VOL_MAX (the file's own
 * level). Applied to each sample before the DAC; takes effect within a frame. */
#define MP3_VOL_MAX 16u
void     mp3_set_volume(uint32_t v);
uint32_t mp3_volume(void);

#endif /* NATOS_MP3_H */
