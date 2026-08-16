/* nat-os — grid raycaster.
 *
 * Renders a first-person view of a tile map as vertical columns, which is the
 * shape this display driver is already good at: no framebuffer, one composed
 * column blitted per screen x (UM-NATOS-015 §4).
 *
 * Camera-plane formulation. A ray is dir + plane * cameraX, where dir is a unit
 * vector and plane is perpendicular to it, so the distance marched along that
 * ray is ALREADY the perpendicular distance to the camera plane. That removes
 * the fisheye correction entirely — no cosine per column, and no reciprocals,
 * which matters because 64-bit division would need libgcc and this kernel links
 * with -nostdlib.
 *
 * Fixed point throughout, 16.16, in units of one map cell. The ESP32's Xtensa
 * core has a hardware 32-bit divide, so the one division per column is a single
 * instruction; 64-bit arithmetic is avoided rather than emulated.
 */

#ifndef NATOS_RAYCAST_H
#define NATOS_RAYCAST_H

#include <stdint.h>

#define RAY_VIEW_W 240u
/* The 3D view fills the launcher region MINUS the chrome bar at its foot.
 *
 * It briefly filled the whole region, and the close button drawn on top of it
 * flickered badly: the raycaster repaints every pixel every frame, so the X was
 * visible only in the gap between one repaint and the next. Drawing chrome over
 * something that repaints continuously cannot be made to work by ordering the
 * draws — the view has to not own those rows. */
#define RAY_VIEW_H 208u

void raycast_init(void);

/* ---- framebuffer, switchable at runtime --------------------------------
 *
 * Off: each column is blitted as it is computed — 120 address-window setups per
 * frame, and the window overhead measured at roughly five times the pixel data
 * it carried.
 *
 * On: columns are composed into DRAM and the view goes out as ONE window.
 *
 * Both paths stay live deliberately. UM-NATOS-010 §7.2 argued against a
 * framebuffer, and that argument is sound for the UI — the panel already holds
 * that image, so a host copy is redundant. It does not transfer to a renderer
 * that composes every pixel from scratch each frame, because there is no
 * existing copy for it to duplicate. Keeping the switch means the difference
 * stays a measurement rather than an assertion, and the claim can be rechecked
 * whenever the display path changes underneath it.
 *
 * The buffer is 240x168x2 = 80,640 B, allocated from the heap on enable and
 * released on disable. Returns 0 on success. */
int  raycast_set_framebuffer(int on);
int  raycast_framebuffer(void);
uint32_t raycast_fb_bytes(void);

/* Advances the camera one step and draws one frame. Walks forward until a wall
 * is close, then turns until clear. */
void raycast_frame(void);

/* Nudges the heading. Positive turns right. Used by the touch task so the view
 * can be steered. */
void raycast_turn(int32_t units);

/* Where a frame's time actually goes, in microseconds. */
uint32_t raycast_us_march(void);
uint32_t raycast_us_compose(void);
uint32_t raycast_us_blit(void);

uint32_t raycast_frames(void);
uint32_t raycast_columns(void);

#endif /* NATOS_RAYCAST_H */
