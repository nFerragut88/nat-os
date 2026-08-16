/* nat-os — tones on DAC2.
 *
 * The first output this system has that is not the screen.
 *
 * ---- why the cosine generator ---------------------------------------------
 *
 * The ESP32's DAC has a hardware cosine-wave generator: given a frequency step,
 * it drives the DAC continuously with no CPU involvement at all — no interrupt,
 * no DMA, no sample buffer, no task. Once started it runs until stopped.
 *
 * That is what makes tones appropriate now and sample playback not. PCM needs a
 * clock at 8 kHz against a 100 Hz tick, which means either a dedicated timer
 * interrupt or I2S with DMA — and this kernel's interrupt matrix has never
 * delivered a peripheral interrupt end to end (UM-NATOS-023 §7). Tones need
 * none of that machinery, so they cost nothing that is not already proven.
 *
 * ---- the pin ---------------------------------------------------------------
 *
 * DAC2 is GPIO26. DAC1 is GPIO25, which is the touch controller's clock, so it
 * is not available: a speaker there would buzz on every touch read and load the
 * SPI clock line. There is exactly one usable DAC pin on this board.
 *
 * ---- frequency accuracy ----------------------------------------------------
 *
 * The generator is clocked from RTC8M, an internal RC oscillator that is not
 * trimmed and drifts with temperature — typically within a few percent, which
 * is inaudible as wrong for a beep and would be unusable for music. The
 * conversion in audio.c is therefore nominal, and `tone` in the shell takes Hz
 * so the error can be heard against a known pitch rather than assumed away.
 */

#ifndef NATOS_AUDIO_H
#define NATOS_AUDIO_H

#include <stdint.h>

void audio_init(void);

/* Starts a continuous tone. 0 stops it. Frequency is nominal — see above. */
void audio_tone(uint32_t hz);
void audio_off(void);

/* Starts a tone that stops itself after `ticks`, without blocking.
 *
 * Blocking would be the obvious implementation and is wrong here: this is
 * called from the touch path for key feedback, and a 30 ms busy wait per
 * keypress is 30 ms the touch task is not sampling. audio_service() ends it. */
void audio_beep(uint32_t hz, uint32_t ticks);

/* Called once per frame. Ends any beep whose deadline has passed. */
void audio_service(void);

/* Key feedback: short, quiet, and high enough not to be confused with a beep
 * that means something. */
void audio_click(void);

/* Bit-bangs a full-swing square wave on any pin, for finding the one the
 * board's SPEAK connector is actually wired to. See audio.c. */
void audio_square(uint32_t pin, uint32_t mux, uint32_t hz, uint32_t ms);
void audio_find_speaker(void);

/* Proves the square-wave generator works, using a pin the board can measure.
 * Run this before believing a negative result from audio_find_speaker(). */
void audio_probe_square(void);

uint32_t audio_tones(void);     /* tones started, for telemetry */

#endif /* NATOS_AUDIO_H */
