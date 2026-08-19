/* nat-os — tones on GPIO26, via LEDC.
 *
 * The first output this system has that is not the screen.
 *
 * ---- why LEDC ------------------------------------------------------------
 *
 * LEDC is hardware PWM: given a divider it generates the waveform continuously
 * with no CPU involvement at all — no interrupt, no DMA, no sample buffer, no
 * task. Once started it runs until stopped.
 *
 * The DAC's cosine generator was tried first and abandoned. See audio.c: it
 * produced no sound, and claiming the pad for the RTC subsystem muted every
 * other attempt on the same pin for the rest of the search.
 *
 * That is what makes tones appropriate now and sample playback not. PCM needs a
 * clock at 8 kHz against a 100 Hz tick, which means either a dedicated timer
 * interrupt or I2S with DMA — and this kernel's interrupt matrix has never
 * delivered a peripheral interrupt end to end (UM-NATOS-023 §7). Tones need
 * none of that machinery, so they cost nothing that is not already proven.
 *
 * ---- the pin ---------------------------------------------------------------
 *
 * GPIO26, which is where this board's SPEAK connector goes. GPIO25 would have
 * been the alternative and is the touch controller's clock, so it is not
 * available anyway.
 *
 * ---- frequency, and what the speaker will actually reproduce ---------------
 *
 * LEDC counts APB at 80 MHz, which this project has measured, so the pitch is
 * EXACT rather than nominal — unlike the DAC path, which was clocked from an
 * untrimmed RC oscillator.
 *
 * Being exact is not the same as being audible. **440 Hz on this speaker is
 * inaudible**; 3 kHz is clear. That cost a whole round of debugging, because a
 * correct tone at a frequency the transducer cannot move is indistinguishable
 * from a broken driver. Everything here that is meant to be heard sits near
 * 3 kHz, which is also roughly where the ear is most sensitive.
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
void audio_square(uint32_t pin, uint32_t hz, uint32_t ms);
void audio_find_speaker(void);

/* Proves the square-wave generator works, using a pin the board can measure.
 * Run this before believing a negative result from audio_find_speaker(). */
void audio_probe_square(void);

/* A long, warbling, 3 kHz square wave on one pin — built to be findable by ear
 * rather than merely present. See audio.c. */
void audio_hold(uint32_t pin, uint32_t seconds);

void audio_dump(void);          /* LEDC state, including its clock gate */

uint32_t audio_tones(void);     /* tones started, for telemetry */

#endif /* NATOS_AUDIO_H */
