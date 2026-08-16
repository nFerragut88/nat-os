/* nat-os — eFuse block 0 reads. See efuse.h.
 *
 * The 802.11 MAC needs a hardware address: it goes in every frame this board
 * transmits, and the receive filter matches against it. The ESP32 ships with
 * one burned into eFuse at the factory.
 *
 * Unlike almost everything else in the WiFi path, this is DOCUMENTED. The eFuse
 * controller is in the technical reference manual, the block 0 layout is
 * public, and the decode below is the same one ESP-IDF performs. Nothing here
 * is reverse-engineered.
 *
 * ---- verifying the decode ------------------------------------------------
 *
 * A six-byte address looks equally plausible in any byte order, so the decode
 * needs checking against something.
 *
 * The first attempt checked the top three bytes against a list of Espressif
 * OUIs, on the reasoning that an OUI is a published, assigned value. It
 * reported failure on this board: 5c:01:3b:50:3f:64, no match. The decode was
 * correct and the LIST was wrong -- an OUI assigned after the list was written,
 * or to the module maker rather than to Espressif. The test was measuring my
 * recollection of IEEE registrations, not the hardware.
 *
 * eFuse stores a CRC8 of the address in the same block. Checking against that
 * tests the decode against data on the chip itself: if the bytes are read in
 * the wrong order the CRC cannot match, and no external knowledge is involved.
 * Confirmed here -- 5c:01:3b:50:3f:64 gives 0x08, which is the stored byte,
 * while the reversed order gives 0x8f.
 *
 * The lesson is worth keeping: the better oracle was already on the chip.
 */

#include "efuse.h"

/* Block 0. Word 1 holds MAC[31:0]; word 2 holds MAC[47:32] in its low half and
 * the CRC8 in bits 16..23. */
#define EFUSE_BLK0_RDATA1_REG   0x3FF5A004u
#define EFUSE_BLK0_RDATA2_REG   0x3FF5A008u

void efuse_factory_mac(uint8_t mac[6])
{
    uint32_t low  = *(volatile uint32_t *)EFUSE_BLK0_RDATA1_REG;
    uint32_t high = *(volatile uint32_t *)EFUSE_BLK0_RDATA2_REG;

    mac[0] = (uint8_t)(high >> 8);
    mac[1] = (uint8_t)(high);
    mac[2] = (uint8_t)(low >> 24);
    mac[3] = (uint8_t)(low >> 16);
    mac[4] = (uint8_t)(low >> 8);
    mac[5] = (uint8_t)(low);
}

/* CRC-8, polynomial 0x8C reflected, initial value zero — the variant Espressif
 * burns alongside the address. */
static uint8_t mac_crc8(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (uint8_t)((crc >> 1) ^ 0x8Cu) : (uint8_t)(crc >> 1);
        }
    }
    return crc;
}

uint8_t efuse_mac_crc_stored(void)
{
    return (uint8_t)((*(volatile uint32_t *)EFUSE_BLK0_RDATA2_REG) >> 16);
}

int efuse_mac_valid(const uint8_t mac[6])
{
    return mac_crc8(mac, 6) == efuse_mac_crc_stored();
}
