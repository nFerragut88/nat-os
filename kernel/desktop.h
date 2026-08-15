/* cyd-os — a touch-driven launcher.
 *
 * The first thing in this kernel that exists for a user rather than for a test.
 * Everything before it was verified by reading serial output; this is verified
 * by someone touching the glass and an application starting.
 *
 * ---- what it is, and what it deliberately is not ------------------------
 *
 * It is a LAUNCHER: an icon grid, a cursor, and double-tap to start a program.
 * It is not a window manager. Applications still render into the fixed
 * horizontal strips app.c assigns by slot index (UM-CYDOS-016 §2), because
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

#ifndef CYDOS_DESKTOP_H
#define CYDOS_DESKTOP_H

#include <stdint.h>

/* The launcher owns the same region the 3D view does. Only one of them draws at
 * a time — see desktop_active(). */
#define DESK_H  168u

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

/* Counters, for the reporter — a launcher that never sees a tap and a touch
 * path that never delivers one look identical on the glass. */
uint32_t desktop_taps(void);
uint32_t desktop_opens(void);

#endif /* CYDOS_DESKTOP_H */
