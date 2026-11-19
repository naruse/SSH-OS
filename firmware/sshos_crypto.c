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
 *   request 05  OPEN     write packet number (4), key (64), then an SSH
 *                        packet as it arrived: length (4, still encrypted),
 *                        body, check sum (16)
 *   request 06  SEAL     write packet number (4), key (64), then a packet
 *                        to send: length (4), body
 *   request 10  RESULT   read 1 byte, 1 = done or 0 = refused, then the
 *                        answer of the request before: 32, 32, 64 bytes;
 *                        for VERIFY one byte, 1 = genuine; for OPEN one
 *                        byte, 1 = genuine, then the body in clear; for
 *                        SEAL the packet encrypted, with its check sum
 *
 * OPEN and SEAL are the packets of a session once logged in, in the form
 * chacha20-poly1305@openssh.com gives them: the key is the 64 bytes SSH
 * derives for that direction (the second half hides the length, the first
 * the body and makes the check sum's key), the number is the packet's
 * place in the connection. Nothing is remembered between requests.
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

static unsigned char answer[SSHOS_CRYPTO_MAX];
static unsigned int answer_len;

/* One packet either way. in: number (4), key (64), packet. */
static void packet(int seal, const unsigned char *in, unsigned int len)
{
    const unsigned char *key = in + 4, *p = in + SSHOS_PACKET_HEAD;
    unsigned char nonce[12], polykey[32], tag[16], diff = 0;
    unsigned int n, i;

    if (len < SSHOS_PACKET_HEAD + 4 + (seal ? 0 : 16))
        return;
    n = len - SSHOS_PACKET_HEAD - (seal ? 0 : 16);      /* the length and the body */
    if (n + 18 > sizeof answer)
        return;
    for (i = 0; i < 8; i++)
        nonce[i] = 0;
    for (i = 0; i < 4; i++)
        nonce[8 + i] = in[i];
    for (i = 0; i < 32; i++)
        polykey[i] = 0;
    chacha20(key, nonce, 0, polykey, 32);
    if (seal) {
        unsigned char *out = answer + 1;

        for (i = 0; i < n; i++)
            out[i] = p[i];
        chacha20(key + 32, nonce, 0, out, 4);
        chacha20(key, nonce, 1, out + 4, n - 4);
        poly1305(out + n, polykey, out, n);
        answer_len = 1 + n + 16;
    } else {
        poly1305(tag, polykey, p, n);
        for (i = 0; i < 16; i++)
            diff |= (unsigned char)(tag[i] ^ p[n + i]);
        answer[1] = (unsigned char)(diff == 0);
        answer_len = 2;
        if (!diff) {
            for (i = 4; i < n; i++)
                answer[2 + i - 4] = p[i];
            chacha20(key, nonce, 1, answer + 2, n - 4);
            answer_len = 2 + n - 4;
        }
    }
    for (i = 0; i < 32; i++)
        polykey[i] = 0;
    answer[0] = 1;
}

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
    case SSHOS_OPEN:
        packet(0, in, len);
        return;
    case SSHOS_SEAL:
        packet(1, in, len);
        return;
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
