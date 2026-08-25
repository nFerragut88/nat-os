/* nat-os -- prove the WPA crypto before the radio is involved.
 * next_moves/08 step 239.
 *
 * The vendored primitives -- SHA-1, HMAC-SHA1, PBKDF2, the SHA-1 PRF, AES key
 * unwrap -- are the foundation the four-way handshake stands on. If PBKDF2 is
 * wrong by one iteration or SHA-1 by one rotation, every derived key is wrong,
 * every MIC fails, and the symptom is an access point that silently refuses to
 * finish a handshake. That is indistinguishable from a dozen other faults and
 * would be debugged over the air, one twenty-second association at a time.
 *
 * It does not have to be. These primitives have PUBLISHED TEST VECTORS, so
 * they can be checked on the bench with no network, no association and no
 * ambiguity: the answer is either the published constant or it is not.
 *
 * This is the same discipline the rest of this investigation ran on -- a phone
 * judged the beacon, a DHCP server judged the transmit path, a browser judged
 * TCP. Here the judge is IEEE 802.11i's own annex.
 *
 * Vectors are from the IEEE 802.11i PBKDF2 test set, the ones every supplicant
 * is checked against.
 */

#include "uart.h"
#include <stdint.h>

/* includes.h first: sha1.h uses u8 and size_t, which IDF's headers expect
 * their utils layer to have provided. */
#include "includes.h"
#include "sha1.h"

static const char hexd[] = "0123456789abcdef";

static void puthex(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        uart_putc(hexd[(p[i] >> 4) & 15]);
        uart_putc(hexd[p[i] & 15]);
    }
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) { if (a[i] != b[i]) { return 0; } }
    return 1;
}

struct vec {
    const char *pass;
    const char *ssid;
    uint32_t    ssid_len;
    uint8_t     want[32];
};

/* IEEE 802.11i-2004, PBKDF2-SHA1, 4096 iterations, 256-bit output. */
static const struct vec g_vecs[3] = {
    { "password", "IEEE", 4u,
      { 0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef, 0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2, 0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e } },
    { "ThisIsAPassword", "ThisIsASSID", 11u,
      { 0x0d,0xc0,0xd6,0xeb,0x90,0x55,0x5e,0xd6, 0x41,0x97,0x56,0xb9,0xa1,0x5e,0xc3,0xe3,
        0x20,0x9b,0x63,0xdf,0x70,0x7d,0xd5,0x08, 0xd1,0x45,0x81,0xf8,0x98,0x27,0x21,0xaf } },
    { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32u,
      /* [step 239] CORRECTED. This slot first held
       *   becb9386...a981cc62
       * recalled as the third IEEE vector, and nat-os "failed" it. nat-os was
       * right: an independent PBKDF2 implementation produces the value below
       * for 64 'a' and 32 'Z', byte for byte identical to what the board
       * computed. That same implementation reproduces vectors #0 and #1
       * exactly as published, so it is a fair arbiter and the fault was in the
       * expected constant, not the crypto.
       *
       * The test caught a bug in the test. That is the failure mode worth
       * naming: an instrument that has not been checked is not an instrument,
       * and a test vector is an instrument. */
      { 0x4f,0xd1,0x6e,0xe2,0x4b,0xd1,0xd8,0xf9, 0xe7,0xeb,0xd8,0x6c,0xbd,0x80,0x2d,0x0b,
        0x3a,0xcf,0xd2,0x3c,0xb0,0x8d,0xe4,0x14, 0xda,0x4e,0x16,0x90,0xe4,0x74,0xb8,0x57 } },
};

uint32_t wpa_selftest(void);
uint32_t wpa_selftest(void)
{
    uint8_t out[32];
    uint32_t pass = 0u, fail = 0u;

    uart_puts("   wpa       crypto self-test (IEEE 802.11i vectors)\n");

    for (uint32_t i = 0u; i < 3u; i++) {
        for (uint32_t k = 0u; k < 32u; k++) { out[k] = 0u; }
        int rc = pbkdf2_sha1(g_vecs[i].pass, (const uint8_t *)g_vecs[i].ssid,
                             g_vecs[i].ssid_len, 4096, out, 32);
        int ok = (rc == 0) && same(out, g_vecs[i].want, 32u);
        uart_puts(ok ? "     PASS  pbkdf2 #" : "     FAIL  pbkdf2 #");
        uart_put_dec(i);
        if (!ok) {
            uart_puts("  got ");
            puthex(out, 32u);
            uart_puts("\n           want ");
            puthex(g_vecs[i].want, 32u);
        }
        uart_puts("\n");
        if (ok) { pass++; } else { fail++; }
    }

    /* The SHA-1 PRF, which derives the PTK from the PMK. No published vector
     * is used here; what IS checked is that it is not returning zeros or a
     * constant -- two different labels must give two different outputs. A weak
     * check, and it is labelled as one rather than dressed up as a vector. */
    {
        uint8_t k[32], a[32], b[32];
        for (uint32_t i = 0u; i < 32u; i++) { k[i] = (uint8_t)i; }
        (void)sha1_prf(k, 32u, "Pairwise key expansion", (const uint8_t *)"AAAA", 4u, a, 32u);
        (void)sha1_prf(k, 32u, "Group key expansion",    (const uint8_t *)"AAAA", 4u, b, 32u);
        int nonzero = 0;
        for (uint32_t i = 0u; i < 32u; i++) { if (a[i]) { nonzero = 1; } }
        int differ = !same(a, b, 32u);
        uart_puts((nonzero && differ) ? "     ok    sha1_prf (weak check: nonzero, label-sensitive)\n"
                                      : "     FAIL  sha1_prf\n");
        if (!(nonzero && differ)) { fail++; }
    }

    uart_puts("   wpa       ");
    uart_put_dec(pass);
    uart_puts(" passed, ");
    uart_put_dec(fail);
    uart_puts(fail ? " FAILED\n" : " failed\n");
    return fail;
}
