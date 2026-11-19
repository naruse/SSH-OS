/* The BlueSCSI's side of the crypto hand-off (firmware/sshos_crypto.c),
 * built for this computer and asked the way SSH OS asks it: published
 * examples must come back right, a forged signature must be refused, and
 * so must a request of the wrong length. Used by "make test-fw-host". */
#include <stdio.h>
#include <string.h>
#include "../firmware/sshos_crypto.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", what, ok ? "ok" : "WRONG");
    if (!ok)
        failures++;
}

static int unhex(unsigned char *out, const char *hex)
{
    int n = 0;
    unsigned v;

    for (; hex[0] && hex[1]; hex += 2, n++) {
        sscanf(hex, "%2x", &v);
        out[n] = (unsigned char)v;
    }
    return n;
}

int main(void)
{
    unsigned char in[SSHOS_CRYPTO_MAX], out[80], want[64], seed[32], pub[32];
    unsigned int n;

    n = sshos_crypto_hello(out);
    check("hello", n == 8 && memcmp(out, "SSHOS", 5) == 0 && out[5] == SSHOS_CRYPTO_VERSION);

    /* RFC 7748, section 5.2, the first example */
    unhex(in, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    unhex(in + 32, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    unhex(want, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    sshos_crypto_job(SSHOS_X25519, in, 64);
    n = sshos_crypto_result(out, sizeof out);
    check("x25519", n == 33 && out[0] == 1 && memcmp(out + 1, want, 32) == 0);
    n = sshos_crypto_result(out, sizeof out);
    check("an answer is given out once", n == 1 && out[0] == 0);

    /* RFC 8032, section 7.1, test 2: a message of one byte */
    unhex(seed, "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
    unhex(pub, "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    unhex(want, "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
                "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    memcpy(in, seed, 32);
    sshos_crypto_job(SSHOS_PUBLIC, in, 32);
    n = sshos_crypto_result(out, sizeof out);
    check("public key", n == 33 && out[0] == 1 && memcmp(out + 1, pub, 32) == 0);

    memcpy(in, seed, 32);
    memcpy(in + 32, pub, 32);
    in[64] = 0x72;
    sshos_crypto_job(SSHOS_SIGN, in, 65);
    n = sshos_crypto_result(out, sizeof out);
    check("signature", n == 65 && out[0] == 1 && memcmp(out + 1, want, 64) == 0);

    memcpy(in, want, 64);
    memcpy(in + 64, pub, 32);
    in[96] = 0x72;
    sshos_crypto_job(SSHOS_VERIFY, in, 97);
    n = sshos_crypto_result(out, sizeof out);
    check("a genuine signature passes", n == 2 && out[0] == 1 && out[1] == 1);
    in[96] = 0x73;
    sshos_crypto_job(SSHOS_VERIFY, in, 97);
    n = sshos_crypto_result(out, sizeof out);
    check("a forged one does not", n == 2 && out[0] == 1 && out[1] == 0);

    sshos_crypto_job(SSHOS_X25519, in, 63);
    n = sshos_crypto_result(out, sizeof out);
    check("a short request is refused", n == 1 && out[0] == 0);
    sshos_crypto_job(0x55, in, 64);
    n = sshos_crypto_result(out, sizeof out);
    check("an unknown request is refused", n == 1 && out[0] == 0);

    printf(failures ? "FAILED\n" : "All right.\n");
    return failures != 0;
}
