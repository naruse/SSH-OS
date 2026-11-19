/* The slow sums of SSH, done by the BlueSCSI for the Macintosh.
 *
 * The SE's 68000 needs 12 to 20 seconds for each of the four big
 * calculations of an SSH log-in; the BlueSCSI's processor needs a few
 * thousandths of one. This is the part of the BlueSCSI firmware that does
 * them when SSH OS asks, over the SCSI cable they already share: a command
 * of our own on the network adapter, next to the firmware's WiFi commands.
 *
 *   SCSI command 0x1D, six bytes: 1D, request, 0, length high, length low, 0
 *
 *   request 00  HELLO    read 8 bytes: "SSHOS", version, 0, 0. A firmware
 *                        without this answers nothing, and SSH OS then
 *                        does the sums itself.
 *   request 01  X25519   write scalar (32), point (32)
 *   request 02  PUBLIC   write seed (32): the Ed25519 public key for it
 *   request 03  SIGN     write seed (32), public key (32), message
 *   request 04  VERIFY   write signature (64), public key (32), message
 *   request 10  RESULT   read 1 byte, 1 = done or 0 = refused, then the
 *                        answer of the request before: 32, 32, 64 bytes,
 *                        or for VERIFY one byte, 1 = genuine
 *
 * The arithmetic is SSH OS's own (kernel/x25519.c, ed25519.c, sha512.c),
 * the same files the SE runs and the tests check; nothing here depends on
 * the BlueSCSI. In the firmware, network.c hands command 0x1D to
 * sshos_crypto_job() and sshos_crypto_result().
 *
 * The same file is built into SSH OS itself with -DCOPRO_FAKE, where it
 * stands in for the BlueSCSI so that the hand-off can be tested in the
 * emulator (make test-copro).
 */
#include "sshos_crypto.h"

static unsigned char answer[1 + 64];
static unsigned int answer_len;

/* Take one request. in/len is what the Macintosh wrote. */
void sshos_crypto_job(unsigned char request, const unsigned char *in, unsigned int len)
{
    answer[0] = 0;
    answer_len = 1;
    switch (request) {
    case SSHOS_X25519:
        if (len != 64)
            return;
        x25519(answer + 1, in, in + 32);
        answer_len = 1 + 32;
        break;
    case SSHOS_PUBLIC:
        if (len != 32)
            return;
        ed25519_public(answer + 1, in);
        answer_len = 1 + 32;
        break;
    case SSHOS_SIGN:
        if (len < 64)
            return;
        ed25519_sign(answer + 1, in + 64, len - 64, in, in + 32);
        answer_len = 1 + 64;
        break;
    case SSHOS_VERIFY:
        if (len < 96)
            return;
        answer[1] = (unsigned char)(ed25519_verify(in, in + 96, len - 96, in + 64) == 1);
        answer_len = 1 + 1;
        break;
    default:
        return;
    }
    answer[0] = 1;
}

/* Hand over the answer to the request before; returns its length. */
unsigned int sshos_crypto_result(unsigned char *out, unsigned int max)
{
    unsigned int n = answer_len < max ? answer_len : max, i;

    for (i = 0; i < n; i++) {
        out[i] = answer[i];
        answer[i] = 0;          /* it may be a secret: it is given out once */
    }
    answer_len = 1;
    return n;
}

/* The eight bytes of HELLO. */
unsigned int sshos_crypto_hello(unsigned char *out)
{
    static const unsigned char hello[8] = {'S', 'S', 'H', 'O', 'S', SSHOS_CRYPTO_VERSION, 0, 0};
    int i;

    for (i = 0; i < 8; i++)
        out[i] = hello[i];
    return 8;
}
