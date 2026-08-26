/* nat-os -- WPA crypto self-test, WINDOWED half. next_moves/08 step 240.
 *
 * Runs the vectors and PRINTS NOTHING. uart_puts is call0; this file is
 * windowed because it calls the crypto, which is windowed because the
 * handshake needs to reach nine-argument blob functions without a bridge.
 *
 * Windowed code calling call0 directly is the violation this project enforces
 * by directory, and it was made here and measured: a NULL dereference at
 * excvaddr 0, because call8 leaves the return address in a8 while a call0
 * callee returns through a0. Step 205 lost three builds to the same mistake
 * in the other direction.
 *
 * So results go into globals and kernel/wpareport.c -- call0 -- prints them.
 * Only the DATA crosses, which is the same rule as "only the address crosses".
 */

#include "includes.h"
#include "sha1.h"
#include "aes_wrap.h"

uint32_t g_wpat_pass, g_wpat_fail;
/* [step 258] aes_unwrap against RFC 3394. */
uint32_t g_wpat_unwrap_ok, g_wpat_unwrap_rc;
uint8_t  g_wpat_got[3][32];
uint8_t  g_wpat_want[3][32];
uint32_t g_wpat_ok[3];
uint32_t g_wpat_prf_ok;

struct vec { const char *pass; const char *ssid; uint32_t ssid_len; uint8_t want[32]; };

/* IEEE 802.11i-2004 PBKDF2-SHA1, 4096 iterations, 256-bit output.
 *
 * Vector #2's expected value was CORRECTED at step 239: it first held a
 * misremembered constant and nat-os "failed" it. nat-os was right -- an
 * independent PBKDF2 reproduces the value below byte for byte, and reproduces
 * #0 and #1 exactly as published, which is what makes it an arbiter. The test
 * caught a bug in the test. */
static const struct vec g_vecs[3] = {
    { "password", "IEEE", 4u,
      { 0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef, 0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2, 0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e } },
    { "ThisIsAPassword", "ThisIsASSID", 11u,
      { 0x0d,0xc0,0xd6,0xeb,0x90,0x55,0x5e,0xd6, 0x41,0x97,0x56,0xb9,0xa1,0x5e,0xc3,0xe3,
        0x20,0x9b,0x63,0xdf,0x70,0x7d,0xd5,0x08, 0xd1,0x45,0x81,0xf8,0x98,0x27,0x21,0xaf } },
    { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32u,
      { 0x4f,0xd1,0x6e,0xe2,0x4b,0xd1,0xd8,0xf9, 0xe7,0xeb,0xd8,0x6c,0xbd,0x80,0x2d,0x0b,
        0x3a,0xcf,0xd2,0x3c,0xb0,0x8d,0xe4,0x14, 0xda,0x4e,0x16,0x90,0xe4,0x74,0xb8,0x57 } },
};

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) { if (a[i] != b[i]) { return 0; } }
    return 1;
}

int wpa_selftest(void);
int wpa_selftest(void)
{
    g_wpat_pass = 0u; g_wpat_fail = 0u;

    for (uint32_t i = 0u; i < 3u; i++) {
        for (uint32_t k = 0u; k < 32u; k++) {
            g_wpat_got[i][k] = 0u;
            g_wpat_want[i][k] = g_vecs[i].want[k];
        }
        int rc = pbkdf2_sha1(g_vecs[i].pass, (const uint8_t *)g_vecs[i].ssid,
                             g_vecs[i].ssid_len, 4096, g_wpat_got[i], 32);
        int ok = (rc == 0) && same(g_wpat_got[i], g_vecs[i].want, 32u);
        g_wpat_ok[i] = (uint32_t)ok;
        if (ok) { g_wpat_pass++; } else { g_wpat_fail++; }
    }

    /* The SHA-1 PRF that derives the PTK. A WEAK check and labelled as one:
     * no published vector, only that it is nonzero and label-sensitive. */
    {
        uint8_t k[32], a[32], b[32];
        for (uint32_t i = 0u; i < 32u; i++) { k[i] = (uint8_t)i; }
        (void)sha1_prf(k, 32u, "Pairwise key expansion", (const uint8_t *)"AAAA", 4u, a, 32u);
        (void)sha1_prf(k, 32u, "Group key expansion",    (const uint8_t *)"AAAA", 4u, b, 32u);
        int nz = 0;
        for (uint32_t i = 0u; i < 32u; i++) { if (a[i]) { nz = 1; } }
        g_wpat_prf_ok = (uint32_t)(nz && !same(a, b, 32u));
        if (!g_wpat_prf_ok) { g_wpat_fail++; }
    }
    /* [step 258] RFC 3394 section 4.1 -- AES key unwrap, 128-bit KEK.
     *
     * THE ONE PRIMITIVE ON THE HANDSHAKE PATH THAT WAS NEVER PROVED. Step 240
     * vendored the crypto and checked PBKDF2 against three published vectors
     * and sha1_prf against a weak self-consistency test. aes_unwrap got
     * neither, and it is now the only thing standing between nat-os and a
     * completed WPA2 association: step 257 measured m1=1, m3=1, micbad=0,
     * unwrap=1 -- every MIC correct, the group key unreadable.
     *
     * Blaming the protocol before proving the primitive is how a week gets
     * spent in the wrong file.
     *
     *   KEK        000102030405060708090A0B0C0D0E0F
     *   plaintext  00112233445566778899AABBCCDDEEFF          (2 blocks)
     *   wrapped    1FA68B0A8112B447 AEF34BD8FB5A7B82 9D3E862371D2CFE5
     *
     * n is the number of 64-bit blocks in the PLAINTEXT, so 2 here, and the
     * ciphertext is n+1 blocks -- the same arithmetic extract_gtk does with
     * (kdlen - 8) / 8. */
    {
        static const uint8_t kek[16] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
        };
        static const uint8_t wrapped[24] = {
            0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,
            0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,
            0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5
        };
        static const uint8_t want[16] = {
            0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
            0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF
        };
        uint8_t got[16];
        for (uint32_t i = 0u; i < 16u; i++) { got[i] = 0u; }
        int rc = aes_unwrap(kek, 16u, 2, wrapped, got);
        g_wpat_unwrap_rc = (uint32_t)rc;
        g_wpat_unwrap_ok = (uint32_t)((rc == 0) && same(got, want, 16u));
        if (g_wpat_unwrap_ok) { g_wpat_pass++; } else { g_wpat_fail++; }
    }

    return (int)g_wpat_fail;
}
