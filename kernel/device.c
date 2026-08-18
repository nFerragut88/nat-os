/* nat-os — the device table. See device.h for why this exists.
 *
 * The two entries below are ports of drivers that already worked from the
 * kernel, chosen on purpose: an abstraction proved only by code written to fit
 * it has been proved of nothing. If the light sensor and the speaker do not fit
 * without changing them, the shape is wrong.
 */

#include "device.h"
#include "adc.h"
#include "audio.h"

static uint32_t g_reads, g_writes, g_refusals;

uint32_t device_reads(void)     { return g_reads; }
uint32_t device_writes(void)    { return g_writes; }
uint32_t device_refusals(void)  { return g_refusals; }

/* ---- light -------------------------------------------------------------- */

/* Channel 0 is the board's LDR. Averaged rather than sampled once: a single
 * conversion on this part is noisy enough that an application would see the
 * noise before it saw the light, and 8 samples is well inside the millisecond
 * budget that DEV_F_SLOW already declares. */
static int light_read(uint32_t chan, uint32_t *out)
{
    if (chan != 0u) {
        return 0;
    }
    uint32_t v = adc1_read_avg(ADC1_CH_LDR, 8u);
    if (v == ADC_INVALID) {
        return 0;                       /* refusal, not a fault */
    }
    *out = v;
    return 1;
}

/* ---- beep --------------------------------------------------------------- */

/* Channel 0 takes (hz << 16) | ticks in one word, because the device interface
 * is deliberately one scalar wide and a tone needs two numbers. Packing is the
 * cost of that narrowness and is documented in app_dev.vasm where a program
 * actually has to build the value.
 *
 * Bounds are the driver's own business: an application may ask for anything,
 * and what it gets is clamped to something the speaker and the ear can survive.
 * Refusing would be defensible; clamping means a program cannot make the device
 * unusable by asking badly. */
static int beep_write(uint32_t chan, uint32_t value)
{
    if (chan != 0u) {
        return 0;
    }
    uint32_t hz    = value >> 16;
    uint32_t ticks = value & 0xFFFFu;

    if (hz < 50u)    { hz = 50u; }
    if (hz > 5000u)  { hz = 5000u; }
    if (ticks == 0u) { ticks = 1u; }
    if (ticks > 100u){ ticks = 100u; }  /* ~1 s ceiling */

    audio_beep(hz, ticks);
    return 1;
}

/* ---- the table ---------------------------------------------------------- */

static const device_t DEVICES[] = {
    { "light", 1u, DEV_F_READ  | DEV_F_SLOW, light_read, 0 },
    { "beep",  1u, DEV_F_WRITE | DEV_F_SLOW, 0,          beep_write },
};

#define DEVICE_COUNT ((int)(sizeof DEVICES / sizeof DEVICES[0]))

_Static_assert(DEVICE_COUNT <= DEVICE_MAX, "device table larger than DEVICE_MAX");

void device_init(void)
{
    g_reads = g_writes = g_refusals = 0;
}

int device_count(void) { return DEVICE_COUNT; }

static const device_t *find(uint32_t id)
{
    return (id < (uint32_t)DEVICE_COUNT) ? &DEVICES[id] : 0;
}

const char *device_name(uint32_t id)
{
    const device_t *d = find(id);
    return d ? d->name : 0;
}

int device_info(uint32_t id, uint32_t *channels, uint32_t *flags)
{
    const device_t *d = find(id);
    if (!d) {
        g_refusals++;
        return 0;
    }
    *channels = d->channels;
    *flags    = d->flags;
    return 1;
}

int device_is_slow(uint32_t id)
{
    const device_t *d = find(id);
    return d && (d->flags & DEV_F_SLOW);
}

int device_read(uint32_t id, uint32_t chan, uint32_t *out)
{
    const device_t *d = find(id);
    /* Channel is checked HERE as well as in the driver. The table knows how
     * many channels a device has, so a driver that forgot the check cannot
     * become a hole -- the same reasoning as SYS BLIT re-deriving its absolute
     * rectangle after clipping, on the principle that the check that matters is
     * the one nearest what actually happens. */
    if (!d || !d->read || chan >= d->channels) {
        g_refusals++;
        return 0;
    }
    if (!d->read(chan, out)) {
        g_refusals++;
        return 0;
    }
    g_reads++;
    return 1;
}

int device_write(uint32_t id, uint32_t chan, uint32_t value)
{
    const device_t *d = find(id);
    if (!d || !d->write || chan >= d->channels) {
        g_refusals++;
        return 0;
    }
    if (!d->write(chan, value)) {
        g_refusals++;
        return 0;
    }
    g_writes++;
    return 1;
}
