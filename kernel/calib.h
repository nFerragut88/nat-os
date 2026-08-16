/* nat-os — touch calibration by tapping targets.
 *
 * Replaces a calibration derived from tapping the corners of the glass, which
 * a finger cannot reach on a bezelled panel — see calib.c for why that made
 * every mapped coordinate land inward of the finger.
 *
 * While running it owns the whole panel and takes every touch. It is not a
 * launcher view: it is entered from the shell, because a calibration you can
 * only reach by tapping accurately is no use when tapping accurately is the
 * thing that is broken.
 */

#ifndef NATOS_CALIB_H
#define NATOS_CALIB_H

#include <stdint.h>

void calib_start(void);
int  calib_running(void);

/* Fed the RAW channel values, not mapped coordinates: mapping is what is being
 * calibrated, so using it here would fit the result to its own error. */
void calib_touch(uint32_t raw_x, uint32_t raw_y, int down);

/* Implemented in store.c — writes the result to the persistent record so it
 * survives a reboot. Declared here rather than in store.h so the dependency
 * reads in the direction the data flows. */
void calib_persist(uint32_t xmin, uint32_t xmax, uint32_t ymin, uint32_t ymax);

#endif /* NATOS_CALIB_H */
