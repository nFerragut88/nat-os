/* cyd-os — grid raycaster.
 *
 * Renders a first-person view of a tile map as vertical columns, which is the
 * shape this display driver is already good at: no framebuffer, one composed
 * column blitted per screen x (UM-CYDOS-015 §4).
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

#ifndef CYDOS_RAYCAST_H
#define CYDOS_RAYCAST_H

#include <stdint.h>

#define RAY_VIEW_W 240u
#define RAY_VIEW_H 168u

void raycast_init(void);

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

#endif /* CYDOS_RAYCAST_H */
