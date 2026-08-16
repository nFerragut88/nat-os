/* nat-os — a touch-driven launcher.
 *
 * The first thing in this kernel that exists for a user rather than for a test.
 * Everything before it was verified by reading serial output; this is verified
 * by someone touching the glass and an application starting.
 *
 * ---- what it is, and what it deliberately is not ------------------------
 *
 * It is a LAUNCHER: an icon grid, a cursor, and double-tap to start a program.
 * It is not a window manager. Applications still render into the fixed
 * horizontal strips app.c assigns by slot index (UM-NATOS-016 §2), because
 * there is no focus model, no z-order, and no way for an application to own the
 * screen and give it back.
 *
 * That boundary is deliberate rather than unfinished. Dynamic viewports need an
 * arbitration policy for who owns which pixels, and building one before there
 * is a user interface to demand it would be designing against a guess — the
 * same reason focus arbitration was deferred when viewports could not overlap.
 * A launcher is the consumer that makes the question concrete.
 *
 * ---- why a cursor on a touchscreen ---------------------------------------
 *
 * A cursor is an indirection: you can already point at what you want. It earns
 * its place here because this panel is resistive and noisy — a single reading
 * can land tens of pixels from the finger — and because an icon large enough to
 * hit reliably by direct tap would leave room for very few of them.
 *
 * So the interaction is hybrid, not modal: a single tap MOVES the cursor to it,
 * and a double-tap OPENS whatever the cursor is on. A confident user taps
 * twice and it opens; an unsure one taps once, sees where the cursor landed,
 * and corrects. Neither has to be told which mode they are in.
 */

#ifndef NATOS_DESKTOP_H
#define NATOS_DESKTOP_H

#include <stdint.h>
#include "app.h"

/* The launcher owns the same region the 3D view does. Only one of them draws at
 * a time — see desktop_active(). */
/* The launcher owns everything above the application strips.
 *
 * It was 168, leaving a band of application strips and a colour strip visible
 * at boot whether or not anything was running — two rows of empty boxes and an
 * animation nobody asked for. Neither earns permanent screen space: the strips
 * appear when a program is running, and the colour artwork is now something you
 * open from the menu. */
#define DESK_H  224u

/* The foot of the region, owned by the kernel whichever view is open. It holds
 * the close button, and no view draws into it — which is what stops the button
 * flickering against a view that repaints every frame. */
#define DESK_BAR_H  16u
#define DESK_BAR_Y  (DESK_H - DESK_BAR_H)

/* A launchable entry. `prog` is looked up in the shell's program table by name,
 * so the desktop holds no image pointers of its own and cannot fall out of step
 * with what is actually loadable — the defect that made PROGRAMS[4] silently
 * become a different program than the one the launcher named. */
typedef struct {
    const char *label;      /* shown under the icon, kept short by the cell */
    const char *prog;       /* program name, or 0 for a built-in action     */
    uint16_t    colour;
    int         action;     /* DESK_ACTION_*, only when prog == 0           */
} desk_icon_t;

#define DESK_ACTION_NONE   0
#define DESK_ACTION_3D     1    /* hand the region to the raycaster */
#define DESK_ACTION_ART    2    /* hand the region to the colour artwork */

void desktop_init(void);

/* Draws one frame. Called from the display task in place of raycast_frame()
 * whenever the launcher is the active owner of the region. */
void desktop_frame(void);

/* Feeds a touch sample. `down` is zero on release, which is what closes a tap:
 * a double-tap is two press-release pairs, not two samples that happen to be
 * near each other while a finger is dragging. */
void desktop_touch(uint32_t x, uint32_t y, int down);

/* Non-zero while the launcher owns the region. Cleared by opening the 3D view
 * and set again by a touch in the top-left corner, which is the way back. */
int  desktop_active(void);

/* Non-zero when the colour artwork owns the region. The artwork used to be
 * drawn permanently along the bottom of the screen; it is now something you
 * open, which is the only reason it needs to be asked about. */
int  desktop_art(void);

/* Force the launcher on or off without touching the glass. Exists so the
 * renderer can be measured from the console: with the launcher active the
 * raycaster does not run at all, and its timers read zero. */
void desktop_set_active(int on);

/* Counters, for the reporter — a launcher that never sees a tap and a touch
 * path that never delivers one look identical on the glass. */
/* Draws the close buttons: one per running application in the column app.h
 * reserves outside every viewport, plus one for the 3D view. Called every
 * frame, whichever owner is drawing the region above. */
void desktop_chrome(void);

/* Offers a touch to the close buttons first. Returns non-zero if it was
 * consumed, in which case nothing else should see it. */
int  desktop_chrome_touch(uint32_t x, uint32_t y);

uint32_t desktop_closes(void);
uint32_t desktop_taps(void);

/* First and last sample of the most recent press, and the cell each landed in.
 * Present to distinguish "the touch layer is wrong" from "we sampled at the
 * wrong moment", which are indistinguishable from the chair. */
uint32_t desktop_first_x(void);
uint32_t desktop_first_y(void);
int      desktop_first_cell(void);
uint32_t desktop_last_x(void);
uint32_t desktop_last_y(void);
int      desktop_last_cell(void);
int      desktop_sel(void);
uint32_t desktop_opens(void);

#endif /* NATOS_DESKTOP_H */
