/* nat-os — the video view: the card's .nvd files, each with its thumbnail.
 *
 * next_moves/12 steps 3-4. A browser that plays: the list shows every
 * .nvd in the card's root with the 64x36 icon vidconv.py writes into it (step
 * 2), its title and its length. Tap to select, tap again to open a detail
 * screen with the 120x68 cover and a play button (step 4: vplay.c).
 *
 * Took "meter"'s launcher cell, at the user's choice; meter is still a
 * registered program (`run meter`).
 *
 * Pictures are streamed from the file 512 bytes at a time and blitted as they
 * arrive: no icon or cover is ever held whole, because RAM is the one thing
 * this board does not have spare (SRAM1 is 56 of 60 KB full after the MP3
 * player).
 */

#ifndef NATOS_VIDLIST_H
#define NATOS_VIDLIST_H

#include <stdint.h>

void vidlist_open(void);
void vidlist_frame(void);
void vidlist_touch(uint32_t x, uint32_t y, int down);
void vidlist_dump(void);        /* the list as the view parsed it, for `video` */

/* Called as the view is left (its x): stops a playing video and waits for it
 * to be off the panel before the launcher repaints. */
void vidlist_close(void);

/* Non-zero while a video owns the whole panel: kmain must not draw the
 * spectrum strip over its bottom rows. */
int  vidlist_fullscreen(void);

/* Plays video n (1-based) by the play button's own path; the view must be
 * open (`videoopen`). For `video <n>` in the shell. */
void vidlist_play_number(uint32_t n);

#endif /* NATOS_VIDLIST_H */
