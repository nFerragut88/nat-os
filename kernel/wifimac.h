/* nat-os — WiFi MAC bring-up. See wifimac.c. */
#ifndef NATOS_WIFIMAC_H
#define NATOS_WIFIMAC_H

#include <stdint.h>

/* The one register open-mac's init_mac() touches. */
#define WIFIMAC_BASE      0x3FF73000u
#define WIFIMAC_CTRL_REG  0x3FF73CB8u

/* Ungates the MAC clock and performs open-mac's init_mac(). Returns 0 on
 * success, negative if the preconditions are not met (PHY not up, or already
 * done this boot). Captures before/after values for wifimac_report(). */
int  wifimac_init(void);

int      wifimac_attempted(void);
uint32_t wifimac_ctrl_before(void);
uint32_t wifimac_ctrl_after(void);

/* Arm the MAC reset for the NEXT wifimac_init().
 *
 * nat-os has only ever ungated the WiFi peripheral, never reset it, so the MAC
 * runs in whatever state the ROM bootloader left it in -- which would plausibly
 * receive while refusing to transmit. Opt-in rather than unconditional, because
 * receive currently works and is the thing this might break. See wifimac.c. */
void     wifimac_reset_next(int on);
int      wifimac_reset_done(void);
uint32_t wifimac_rst_before(void);
uint32_t wifimac_rst_after(void);

/* Reads the MAC register window twice and reports how many words differ.
 *
 * This is the evidence that the peripheral is RUNNING rather than merely
 * readable. A clock-gated block returns a stable value -- usually zero -- from
 * every address; a live MAC has free-running counters in it, and those show up
 * as words that change between two passes with nothing driving them.
 *
 * Returns the number of differing words; `first` receives the address of the
 * lowest one, or 0 if none changed. */
uint32_t wifimac_liveness(uint32_t *first);

/* Which words move and by how much per millisecond. A mover advancing ~1000
 * per ms is a 1 MHz counter -- the 802.11 TSF timer runs at exactly that, so
 * this identifies it from behaviour rather than from a guessed address. */
uint32_t wifimac_movers(uint32_t *addrs, uint32_t *khz, uint32_t max);

/* The 802.11 TSF timer, identified by behaviour: 0x3ff73c00 is the one word in
 * the MAC window that advances at exactly 1 MHz across repeated samples. */
#define WIFIMAC_TSF_REG   0x3FF73C00u

static inline uint32_t wifimac_tsf(void)
{
    return *(volatile uint32_t *)WIFIMAC_TSF_REG;
}

/* Counts TSF ticks against the kernel's own cycle counter over `ms`
 * milliseconds. Returns the TSF delta; `cycles` receives the cycles elapsed.
 * A genuine 1 MHz counter yields delta ~= ms * 1000 -- a long-interval match
 * no drifting or noisy register can produce by accident. */
uint32_t wifimac_tsf_check(uint32_t ms, uint32_t *cycles);

/* Routes the MAC's interrupt onto a CPU line and counts what arrives. Safe to
 * call before receive exists: an idle MAC simply never raises anything. */
void     wifimac_irq_enable(void);
uint32_t wifimac_irq_fires(void);
uint32_t wifimac_irq_status(void);

/* A decoded 802.11 frame header from the first filled descriptor. */
typedef struct {
    uint8_t  fc_type, fc_subtype;
    uint8_t  addr1[6], addr2[6], addr3[6];
    uint32_t length;
    char     ssid[33];          /* beacons only; empty otherwise */
} wifi_frame_info_t;

int wifimac_frame_info(wifi_frame_info_t *out);

/* Builds the receive descriptor chain and arms the receiver in promiscuous
 * mode. Returns 0, or negative if macinit has not run (-1), it is already
 * armed (-2), DRAM ran out (-3), or the hardware never acknowledged the chain
 * (-4). */
int      wifimac_rx_start(void);

uint32_t wifimac_rx_filled(void);

/* Drains filled descriptors and hands them back to the hardware. Called
 * continuously by the 'wifirx' task that rx_start creates; exposed so the
 * shell can force a pass. Returns how many were handled. */
uint32_t wifimac_rx_service(void);

uint32_t wifimac_chain_spins(void);
uint32_t wifimac_chain_calls(void);
uint32_t wifimac_rx_frames(void);
uint32_t wifimac_rx_recycled(void);

/* Distinct networks seen, which is what shows reception is CONTINUING rather
 * than having captured a few frames once. */
uint32_t wifimac_net_count(void);
int      wifimac_net_info(uint32_t i, uint8_t bssid[6], const char **ssid,
                          uint32_t *seen);
uint32_t wifimac_rx_next_dscr(void);
int      wifimac_rx_peek(uint32_t *len, uint8_t *out, uint32_t max);

/* Tunes the radio, using open-mac's exact deinit/retune/AGC sequence. Returns
 * 0, -1 if macinit has not run, -2 if the channel is outside 1..13. */
int      wifimac_set_channel(uint32_t ch);
uint32_t wifimac_channel(void);

/* Transmits a raw 802.11 frame. rate is a wifi_phy_rate_t; 0 is 1 Mbps long
 * preamble, the most robust. Returns 0, -1 if the radio is not tuned, -2 on a
 * bad length. Sequence control is filled in for you. */
int      wifimac_tx(const uint8_t *payload, uint32_t len, uint32_t rate);

/* Reaps a completed transmission and returns the raw status word, or 0. The
 * hardware setting this is the only real evidence a frame went out. */
uint32_t wifimac_tx_reap(void);
/* Non-zero while a frame is still in flight. The single TX descriptor and
 * buffer must not be reused until then. Self-clears after 50 ms so a missing
 * completion cannot stop transmission for good. */
int      wifimac_tx_busy(void);
uint32_t wifimac_tx_forced(void);
/* Frames seen addressed to THIS station -- the only unforgeable proof that
 * transmit reaches the air, since a radio cannot hear itself. */
uint32_t wifimac_rx_to_us(void);
uint32_t wifimac_rx_to_us_subtype(void);

/* Broadcast probe request with a wildcard SSID. Every AP in range is obliged
 * to answer one; a beacon obliges nobody, so silence after a beacon proves
 * nothing while silence after this is informative. */
int      wifimac_probe_request(void);

/* Transmit power. Each is a separate step because which one matters is
 * unknown, and a combined call would not say which did anything. Every symbol
 * reached is already linked -- see the note in wifimac.c about what happened
 * when an unlinked one was referenced. */
int      wifimac_txpwr_init(void);
int      wifimac_txpwr_cal(void);
int      wifimac_txpwr_set(uint32_t tpw);
uint32_t wifimac_txpwr_get(void);

/* One stage of open-mac's MAC hardware init, from libpp. 0=ic_mac_init,
 * 1=hal_init, 2=ic_enable_rx, 3=hal_mac_tsf_reset. Stepwise because this pulls
 * a large blob into the link and a crash needs to name its stage. */
int      wifimac_hwinit_step(uint32_t step);
int      wifimac_hw_stage(void);

uint32_t wifimac_tx_sent(void);
uint32_t wifimac_tx_done(void);

uint32_t wifimac_build_beacon(uint8_t *out, const uint8_t mac[6],
                              const char *ssid, uint32_t channel);

/* Beacons the given SSID every 100 ms from the rx task. Needs the receiver
 * armed (-1) and the radio tuned (-2), because a beacon must name a real
 * channel. */
int      wifimac_beacon_start(const char *ssid);
void     wifimac_beacon_stop(void);
uint32_t wifimac_beacon_len(void);

#endif /* NATOS_WIFIMAC_H */
