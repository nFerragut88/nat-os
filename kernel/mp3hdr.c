/* nat-os — MPEG audio frame headers. See mp3hdr.h. */

#include "mp3hdr.h"

/* Layer III bitrates, kbit/s, by bitrate index 1..14. Index 0 is "free
 * format" and 15 is forbidden; both are refused. */
static const uint16_t BR_V1[15]  = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
static const uint16_t BR_V2[15]  = { 0,  8, 16, 24, 32, 40, 48, 56,  64,  80,  96, 112, 128, 144, 160 };
static const uint16_t SR_V1[3]   = { 44100, 48000, 32000 };

int mp3hdr_parse(const uint8_t *p, mp3hdr_t *h)
{
    if (p[0] != 0xFFu || (p[1] & 0xE0u) != 0xE0u) {
        return 0;                                   /* 11-bit sync */
    }
    uint32_t ver   = (p[1] >> 3) & 3u;              /* 0=2.5 1=reserved 2=2 3=1 */
    uint32_t layer = (p[1] >> 1) & 3u;              /* 1 = Layer III */
    uint32_t bri   = p[2] >> 4;
    uint32_t sri   = (p[2] >> 2) & 3u;
    uint32_t pad   = (p[2] >> 1) & 1u;
    uint32_t mode  = p[3] >> 6;                     /* 3 = mono */

    if (ver == 1u || layer != 1u || bri == 0u || bri == 15u || sri == 3u) {
        return 0;
    }
    int v1 = (ver == 3u);
    h->version  = v1 ? 10u : (ver == 2u ? 20u : 25u);
    h->bitrate  = v1 ? BR_V1[bri] : BR_V2[bri];
    h->rate     = SR_V1[sri] >> (v1 ? 0 : (ver == 2u ? 1 : 2));
    h->channels = (mode == 3u) ? 1u : 2u;
    h->samples  = v1 ? 1152u : 576u;
    /* length = samples/8 * bitrate / rate + padding (Layer III slots are bytes) */
    h->length   = (h->samples / 8u) * h->bitrate * 1000u / h->rate + pad;
    return 1;
}

uint32_t mp3hdr_id3_size(const uint8_t *p)
{
    if (p[0] != 'I' || p[1] != 'D' || p[2] != '3') {
        return 0;
    }
    /* Four 7-bit "syncsafe" bytes, then the 10-byte header, then a 10-byte
     * footer if flag bit 4 says there is one. */
    uint32_t s = ((uint32_t)(p[6] & 0x7Fu) << 21) | ((uint32_t)(p[7] & 0x7Fu) << 14)
               | ((uint32_t)(p[8] & 0x7Fu) << 7)  |  (uint32_t)(p[9] & 0x7Fu);
    return s + 10u + ((p[5] & 0x10u) ? 10u : 0u);
}
