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
#include "store.h"
#include "i2c.h"

static uint32_t g_reads, g_writes, g_refusals;

uint32_t device_reads(void)     { return g_reads; }
uint32_t device_writes(void)    { return g_writes; }
uint32_t device_refusals(void)  { return g_refusals; }

/* ---- light -------------------------------------------------------------- */

/* Channel 0 is the board's LDR. Averaged rather than sampled once: a single
 * conversion on this part is noisy enough that an application would see the
 * noise before it saw the light, and 8 samples is well inside the millisecond
 * budget that DEV_F_SLOW already declares. */
static int light_read(uint32_t caller, uint32_t chan, uint32_t *out)
{
    (void)caller;                       /* the light is the same for everyone */
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
static int beep_write(uint32_t caller, uint32_t chan, uint32_t value)
{
    (void)caller;                       /* one speaker, first come first served */
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

/* ---- store -------------------------------------------------------------- */

/* Channels 0..STORE_SLOTS_PER_BANK-1 are persistent words; the channel after
 * them commits.
 *
 * A slot write lands in the in-RAM record and reaches flash on the next save,
 * because an erase per write costs tens of milliseconds with interrupts masked
 * and would spend a sector rated for a hundred thousand cycles in an afternoon.
 * A program that wants its value on the glass NOW writes the commit channel and
 * pays for it; one that does not gets durability within the minute for free,
 * from the periodic save the kernel already performs.
 *
 * Slots are banked by CALLER, so this driver is the reason device_t grew a
 * caller argument. Reading the commit channel reports whether anything is
 * waiting, which is the only way a program can tell.
 */
#define STORE_COMMIT_CHAN STORE_SLOTS_PER_BANK

static uint32_t store_bank(uint32_t caller)
{
    /* DEVICE_CALLER_KERNEL is APP_MAX, which is exactly STORE_KERNEL_BANK.
     * Asserted rather than assumed: they are defined in different headers for
     * different reasons and could drift apart without either looking wrong. */
    _Static_assert(DEVICE_CALLER_KERNEL == STORE_KERNEL_BANK,
                   "the kernel's device-caller id must match its slot bank");
    return caller;
}

static int store_dev_read(uint32_t caller, uint32_t chan, uint32_t *out)
{
    if (chan == STORE_COMMIT_CHAN) {
        *out = (uint32_t)store_dirty();
        return 1;
    }
    return store_slot_get(store_bank(caller), chan, out);
}

static int store_dev_write(uint32_t caller, uint32_t chan, uint32_t value)
{
    if (chan == STORE_COMMIT_CHAN) {
        (void)value;                    /* any value means "commit now" */
        return store_save() == 0;
    }
    return store_slot_set(store_bank(caller), chan, value);
}

/* ---- i2c ---------------------------------------------------------------- */

/* Probe only, for now.
 *
 * The channel IS the 7-bit address, so a program discovers what is plugged into
 * the expansion header by reading channels 8..119 and seeing which answer. That
 * is a whole capability and it fits the one-word interface exactly, which is
 * why it lands before the transfer operations do.
 *
 * i2c_write() and i2c_read() take BUFFERS and do not fit. Adding a fifth
 * operation for them is a real decision about the interface, and it should be
 * driven by a device that actually needs it rather than guessed at now -- the
 * whole reason the model was kept narrow in the first place. vmarg_items is
 * already there when that day comes.
 *
 * Addresses below 8 and above 119 are reserved by the specification and are
 * refused rather than probed: driving a reserved address is a way to confuse a
 * bus, and a program sweeping 0..127 should not be able to do it by accident. */
#define I2C_ADDR_MIN 8u
#define I2C_ADDR_MAX 119u

static int i2c_dev_read(uint32_t caller, uint32_t chan, uint32_t *out)
{
    (void)caller;                       /* one bus, shared by everyone */
    if (chan < I2C_ADDR_MIN || chan > I2C_ADDR_MAX) {
        return 0;
    }
    *out = (i2c_probe((uint8_t)chan) == I2C_OK) ? 1u : 0u;
    return 1;
}

/* ---- the table ---------------------------------------------------------- */

static const device_t DEVICES[] = {
    { "light", 1u, DEV_F_READ  | DEV_F_SLOW, light_read, 0 },
    { "beep",  1u, DEV_F_WRITE | DEV_F_SLOW, 0,          beep_write },
    /* Slow because the commit channel erases a flash sector. Marking the whole
     * device slow costs a slot-read its quantum, which is the safe direction to
     * be wrong in: a device that under-declares its cost lets a program starve
     * the renderer without either of them doing anything visibly wrong. */
    { "store", STORE_SLOTS_PER_BANK + 1u, DEV_F_READ | DEV_F_WRITE | DEV_F_SLOW,
      store_dev_read, store_dev_write },
    /* 128 channels because the channel IS the address; the driver refuses the
     * reserved ranges. Slow: a probe is a full start/address/ack/stop on a
     * bit-banged bus, and a program sweeping every address must not do it on
     * the renderer's time. */
    { "i2c",   128u, DEV_F_READ | DEV_F_SLOW, i2c_dev_read, 0 },
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

int device_read(uint32_t caller, uint32_t id, uint32_t chan, uint32_t *out)
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
    if (!d->read(caller, chan, out)) {
        g_refusals++;
        return 0;
    }
    g_reads++;
    return 1;
}

int device_write(uint32_t caller, uint32_t id, uint32_t chan, uint32_t value)
{
    const device_t *d = find(id);
    if (!d || !d->write || chan >= d->channels) {
        g_refusals++;
        return 0;
    }
    if (!d->write(caller, chan, value)) {
        g_refusals++;
        return 0;
    }
    g_writes++;
    return 1;
}
