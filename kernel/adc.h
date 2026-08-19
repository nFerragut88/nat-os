/* nat-os — SAR ADC1, one-shot reads.
 *
 * The first driver in this kernel that measures something about the world
 * rather than about itself. Everything before it was a bus with a device on the
 * end that answers in protocol; this returns a number that means "how much
 * light", and nothing in the system can check it for plausibility.
 *
 * ---- why ADC1 and not ADC2 ------------------------------------------------
 *
 * On this part ADC2 is unusable whenever WiFi is running, which is the standard
 * reason to avoid it. That reason does not apply here: the -mabi=call0 build
 * cannot link Espressif's precompiled radio libraries at all (UM-NATOS-003
 * §5.1), so WiFi will never run and ADC2 is permanently free.
 *
 * ADC1 is still first because the board's own light sensor is on it, and a
 * sensor already soldered down needs no wiring to be wrong about.
 *
 * ---- channels -------------------------------------------------------------
 *
 * ADC1 channel N is a fixed pad:
 *
 *      ch0 = GPIO36   ch1 = GPIO37   ch2 = GPIO38   ch3 = GPIO39
 *      ch4 = GPIO32   ch5 = GPIO33   ch6 = GPIO34   ch7 = GPIO35
 *
 * Channels 0 and 3 are the touch controller's PENIRQ and MISO, and 4 and 5 are
 * its MOSI and CS — reading those is measuring this kernel's own wiring, not
 * the world. They are kept as a control group rather than hidden.
 *
 * **Channel 6 is the light sensor, confirmed on hardware.** A hand passed over
 * the board moved it 265 counts (273..538) while the four touch pins moved 12,
 * 14, 0 and 47 and the two header pins moved 8 and 4. That was a claim about
 * the board until `ldrscan` measured it; the claim happened to be right, and
 * the point is that it is no longer a claim.
 */

#ifndef NATOS_ADC_H
#define NATOS_ADC_H

#include <stdint.h>
#include "board.h"

#define ADC1_CH_LDR   BOARD_LDR_ADC_CH   /* GPIO34 on the CYD */
#define ADC_INVALID   0xFFFFFFFFu

/* Attenuation, which sets the input range. 11 dB is the widest the part offers
 * and the only one that reaches the supply rail; the narrower settings are more
 * accurate over less range, and nothing here yet cares about accuracy. */
#define ADC_ATTEN_0DB   0u      /* ~0 - 1.1 V */
#define ADC_ATTEN_2_5DB 1u
#define ADC_ATTEN_6DB   2u
#define ADC_ATTEN_11DB  3u      /* ~0 - 3.1 V */

void adc_init(void);

/* One conversion. Returns 0-4095, or ADC_INVALID if the hardware never reported
 * done — which is a different outcome from a reading of zero, and has to be,
 * because a converter that is not running reads zero forever and this project
 * has already shipped one driver whose silence read as a valid measurement
 * (UM-NATOS-017 §3.2). */
uint32_t adc1_read(uint32_t channel);

/* Averaged, discarding the first conversion. The SAR's first sample after a
 * channel change is the one the datasheet does not stand behind, and the touch
 * controller taught this lesson expensively. */
uint32_t adc1_read_avg(uint32_t channel, uint32_t samples);

uint32_t adc_timeouts(void);    /* conversions that never reported done */
void     adc_dump(void);        /* register read-back, for when it does not work */
void     adc_dump_rtcio(void);  /* the RTC pad block, read-only */
void     adc_probe_sensor_mux(void); /* find the analog mux bit empirically */
void     adc_probe_convert(void);    /* does a conversion actually run? */
void     adc_probe_driven(void);     /* sweep against a pin we control */
void     adc_watch(uint32_t channel, uint32_t rounds); /* min/max over time */
void     adc_watch_all(uint32_t rounds);  /* which channel moves? */

#endif /* NATOS_ADC_H */
