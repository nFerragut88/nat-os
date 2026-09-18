/* nat-os — MPEG audio frame headers, without decoding anything.
 *
 * Four bytes at the front of every MP3 frame say how long that frame is. So a
 * reader that jumps from header to header either lands on the next 0xFFE sync
 * every time or it does not -- one wrong byte, or one cluster read from the
 * wrong place, and the walk breaks at that frame. That makes the header walk a
 * check on the SD and FAT layers that needs no reference copy of the file, and
 * it is also what the player will use to find frames, measure duration and
 * seek.
 *
 * Tables from ISO/IEC 11172-3 and 13818-3, Layer III only: Layers I and II
 * exist but are not what anyone means by "an MP3".
 */

#ifndef NATOS_MP3HDR_H
#define NATOS_MP3HDR_H

#include <stdint.h>

typedef struct {
    uint32_t version;       /* 10 = MPEG-1, 20 = MPEG-2, 25 = MPEG-2.5 */
    uint32_t bitrate;       /* kbit/s */
    uint32_t rate;          /* samples per second */
    uint32_t channels;      /* 1 or 2 */
    uint32_t samples;       /* per frame: 1152 (MPEG-1) or 576 */
    uint32_t length;        /* bytes, header included */
} mp3hdr_t;

/* Returns 1 and fills *h if the four bytes are a valid Layer III header. */
int mp3hdr_parse(const uint8_t *p, mp3hdr_t *h);

/* Bytes of ID3v2 tag at the front of a file, from its first 10 bytes; 0 when
 * there is no tag. */
uint32_t mp3hdr_id3_size(const uint8_t *p);

/* The total frame count from a Xing/Info header in the frame at `p`, or 0 if
 * there is none or it carries no count. That header is a silent first frame
 * encoders write into VBR files -- the user's files have one (the
 * FF FB D4 00 00... frame step 2's walk started on) -- and it is the only
 * exact source of a VBR file's duration short of reading every frame. */
uint32_t mp3hdr_xing_frames(const uint8_t *p, uint32_t n);

#endif /* NATOS_MP3HDR_H */
