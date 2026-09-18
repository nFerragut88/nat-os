/* nat-os — sample playback on GPIO26: I2S0 feeding the built-in DAC by DMA.
 *
 * The second output this system has that is not the screen, and the first that
 * can play a recorded sound. audio.c makes tones; this makes waveforms. It is
 * the floor an MP3 player stands on (docs/next_moves/11).
 *
 * ---- why the objection in UM-NATOS-027 section 2 no longer applies --------
 *
 * 027 declined PCM because it needs "a clock at 8 kHz against a 100 Hz tick",
 * meaning a timer interrupt or I2S with DMA, and interrupts were unproven. The
 * answer here is I2S with DMA and NO interrupt at all:
 *
 *   - I2S0 generates the sample clock in hardware and its DMA engine walks a
 *     ring of descriptors on its own, forever, feeding the DAC one sample per
 *     word-select period. No CPU is involved per sample.
 *   - The producer finds out how far the DMA has got by READING a register
 *     (OUT_EOF_DES_ADDR: the last descriptor it finished) whenever it gets
 *     around to it. So the only timing requirement on software is "refill the
 *     ring before it laps you", which is a question of ring length against the
 *     scheduler's worst gap -- tens of milliseconds, not 45 microseconds.
 *
 * ---- the DAC, and the pad it takes -----------------------------------------
 *
 * DAC2 is GPIO26, the SPEAK connector. Driving it means handing the pad to the
 * RTC subsystem (MUX_SEL), and UM-NATOS-027 section 3.1 is the record of what
 * that does to LEDC: once RTC owns the pad, the GPIO matrix cannot drive it. So
 * tones and samples are exclusive, and pcm_stop() gives the pad back by calling
 * audio_init(), the one function known to leave it the way LEDC needs it.
 *
 * DAC1 is GPIO25, the touch controller's clock. It is NEVER powered here. The
 * I2S peripheral feeds both DACs' digital inputs; only a pad with XPD_DAC set
 * drives a voltage, so the touch clock is untouched as long as nobody sets it.
 *
 * ---- format ----------------------------------------------------------------
 *
 * Mono, 16-bit slots, the DAC taking the HIGH byte of each: 8 bits of
 * resolution, unsigned, 0x80 at rest. That is IDF's own dac_dma.c
 * configuration (tx_fifo_mod 1, tx_chan_mod 1, bck_div 16), copied field for
 * field rather than invented. Callers hand pcm_write() signed 16-bit and the
 * conversion happens here.
 *
 * The clock is PLL_D2 (160 MHz) / (32 * rate) with a fractional divider. The
 * integer part must fit 8 bits, so rates BELOW ~19.6 kHz ARE NOT AVAILABLE --
 * IDF states the same floor. A 16 kHz MP3 will need upsampling.
 */

#ifndef NATOS_PCM_H
#define NATOS_PCM_H

#include <stdint.h>

/* Ring geometry. 16 x 512 samples = 16 KB: 170 ms at 48 kHz, the rate the
 * user's MP3s are at. Step 1 had 8 buffers from the heap -- 186 ms at 22,050
 * Hz but only 85 ms at 48 kHz, less than one store_save() (125 ms, interrupts
 * masked). Since step 5 of next_moves/11 the ring lives in SRAM1, statically:
 * 16 KB is most of the heap, and SRAM1 holds nothing that outlives a reboot. */
#define PCM_BUFS     16u
#define PCM_SAMPLES  512u

#define PCM_RATE_MIN 19600u
#define PCM_RATE_MAX 48000u

/* Claims the pad, allocates the ring, fills it with silence and starts the
 * DMA. Returns 0, or -1 for a rate out of range, -2 for no memory. */
int  pcm_start(uint32_t rate);

/* Stops the DMA, releases the pad back to LEDC, frees the ring. */
void pcm_stop(void);

int  pcm_running(void);

/* Non-blocking. Converts and queues up to n signed 16-bit mono samples and
 * returns how many it took, which is 0 when the ring is full. Callers loop:
 * write what fits, yield, write the rest. */
uint32_t pcm_write(const int16_t *s, uint32_t n);

/* Samples the ring can take right now. Polls the DMA as a side effect. */
uint32_t pcm_space(void);

/* ---- instruments -----------------------------------------------------------
 *
 * Each exists to answer one question the ear cannot answer reliably.
 *
 *   rate_actual  samples the DMA consumed per second of CCOUNT since start --
 *                whether the clock divider produced the rate that was asked
 *                for, measured rather than computed
 *   underruns    times the DMA reached a buffer nobody had refilled, so it
 *                replayed stale audio; the number a stutter is made of
 *   blind        polls so far apart that the ring could have lapped more than
 *                once, so the underrun count itself cannot be trusted for
 *                that interval. Non-zero means "the counters above are a
 *                lower bound", which is a different statement from "fine".
 */
uint32_t pcm_rate(void);          /* requested */
uint32_t pcm_rate_actual(void);
uint32_t pcm_underruns(void);
uint32_t pcm_blind(void);
uint32_t pcm_buffers_played(void);

void pcm_dump(void);

/* The shell's `pcm` command. Parses its own arguments because the shell's
 * parse_int() stops at 9999 and a sample rate does not. */
void pcm_shell(char *arg);

#endif /* NATOS_PCM_H */
