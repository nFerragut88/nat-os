/* nat-os — the music view: songs on the SD card, and the buttons to play them.
 *
 * Replaces "ping" on the launcher (next_moves/11 step 6). Built the way the web
 * and wifi views are: a native view, not a VM program, because it drives the
 * decoder task and the card -- neither of which a bytecode application can
 * reach, by design.
 *
 *   header        "music" and the red x, top right, like every other view
 *   list          the .mp3 files in the card's root, 9 rows, with up/down
 *                 buttons. Tap a song to select it; tap it again to play it.
 *                 Two taps rather than one for the browser's reason: this
 *                 panel's default calibration can put the reported point a row
 *                 away from the finger, and a single tap that starts a song
 *                 would start the wrong one.
 *   now playing   the name, elapsed / total, a progress bar
 *   buttons       previous, play/pause, stop, next
 *
 * When a song ends the next one starts, and the last wraps to the first.
 * Playing does not depend on the view being open: player_service() runs from
 * the display task every frame regardless of which view is up.
 */

#ifndef NATOS_PLAYER_H
#define NATOS_PLAYER_H

#include <stdint.h>

void player_open(void);
void player_frame(void);
void player_touch(uint32_t x, uint32_t y, int down);

/* Auto-advance and queued starts. Cheap; call every display frame. */
void player_service(void);

/* What the view believes, printed: the list, the selection, the song, and the
 * decoder's state -- the same variables the draw and hit-test code read, since
 * the panel cannot be read back (MISO, 05). */
void player_dump(void);

/* Starts song n (1-based) through the SAME path the buttons take -- queue,
 * stop what is playing, start when the decoder is free -- so the shell can
 * test the view's transport rather than a parallel one (`music <n>`). */
void player_play_number(uint32_t n);

#endif /* NATOS_PLAYER_H */
