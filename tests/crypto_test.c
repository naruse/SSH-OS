/* Runs the kernel's X25519, ChaCha20, SHA-512, Poly1305 and Ed25519 on the
 * build machine. Each input line is "x SCALAR POINT", "c KEY NONCE COUNTER
 * LENGTH", "f OP A B" (one operation modulo 2^255 - 19 on raw 256-bit
 * values), "h MESSAGE" (SHA-512), "p KEY MESSAGE" (Poly1305), "e SEED
 * MESSAGE" (public key and signature), "v PUBLIC SIGNATURE MESSAGE" (1 if
 * genuine) or "d POINT" (the point written out again, or 0 if those 32 bytes
 * are not a point), all in hex with "-" for an empty message; the
 * answer is printed in hex. tests/crypto_test.py supplies the lines and
 * compares the answers with its own arithmetic.   make test-crypto-host */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SSHOS_KERNEL_H
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

struct sha512 {
    u32 h[8][2];
    u32 total;
    u8  buf[128];
    u8  used;
};
void sha512_init(struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, u32 len);
void sha512_finish(struct sha512 *s, u8 *out64);

#include "../kernel/x25519.c"
#include "../kernel/sha512.c"
#include "../kernel/poly1305.c"
#include "../kernel/ed25519.c"

static int idle_calls;
static void count_idle(void)
{
    idle_calls++;
}

static void unhex(u8 *out, const char *s, size_t n)
{
    unsigned v;
    size_t i;

    for (i = 0; i < n; i++) {
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (u8)v;
    }
}

static void print_hex(const u8 *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

int main(void)
{
    static char line[9000], m[8200];
    char a[130], b[130], op;
    unsigned long counter, len;
    u8 k[32], p[32], out[64], nonce[12], sig[64], *data;
    struct sha512 sh;
    size_t mlen, i;

    crypto_idle = count_idle;
    while (fgets(line, sizeof line, stdin)) {
        if (sscanf(line, "d %64s", a) == 1) {
            struct ge point;

            unhex(p, a, 32);
            if (ge_frombytes(&point, p)) {
                ge_tobytes(out, &point);
                print_hex(out, 32);
            } else {
                printf("0\n");
            }
            continue;
        }
        m[0] = 0;
        if (sscanf(line, "h %8199s", m) == 1 || sscanf(line, "p %64s %8199s", a, m) == 2 ||
            sscanf(line, "e %64s %8199s", a, m) == 2 || sscanf(line, "v %64s %128s %8199s", a, b, m) == 3) {
            /* the message in a block of exactly its size, so an overrun is caught */
            mlen = m[0] == '-' ? 0 : strlen(m) / 2;
            data = malloc(mlen ? mlen : 1);
            unhex(data, m, mlen);
            switch (line[0]) {
            case 'h':
                sha512_init(&sh);
                for (i = 0; i < mlen; i += 37)          /* in awkward pieces */
                    sha512_update(&sh, data + i, (u32)(mlen - i < 37 ? mlen - i : 37));
                sha512_finish(&sh, out);
                print_hex(out, 64);
                break;
            case 'p':
                unhex(k, a, 32);
                poly1305(out, k, data, (u32)mlen);
                print_hex(out, 16);
                break;
            case 'e':
                unhex(k, a, 32);
                ed25519_public(p, k);
                ed25519_sign(sig, data, (u32)mlen, k, p);
                for (i = 0; i < 32; i++)
                    printf("%02x", p[i]);
                print_hex(sig, 64);
                break;
            case 'v':
                unhex(p, a, 32);
                unhex(sig, b, 64);
                printf("%d\n", ed25519_verify(sig, data, (u32)mlen, p));
                break;
            }
            free(data);
            continue;
        }
        if (sscanf(line, "x %64s %64s", a, b) == 2) {
            unhex(k, a, 32);
            unhex(p, b, 32);
            x25519(out, k, p);
            print_hex(out, 32);
            /* in place, as a caller might: the answer over the point */
            x25519(p, k, p);
            if (memcmp(p, out, 32) != 0) {
                fprintf(stderr, "in-place result differs\n");
                return 1;
            }
        } else if (sscanf(line, "f %c %64s %64s", &op, a, b) == 3) {
            /* one field operation on raw values below 2^256 */
            u16 fa[LIMBS], fb[LIMBS];
            int i;

            unhex(k, a, 32);
            unhex(p, b, 32);
            for (i = 0; i < LIMBS; i++) {
                fa[i] = (u16)(k[2 * i] | (k[2 * i + 1] << 8));
                fb[i] = (u16)(p[2 * i] | (p[2 * i + 1] << 8));
            }
            switch (op) {
            case 'a': fe_add(fa, fa, fb); break;
            case 's': fe_sub(fa, fa, fb); break;
            case 'm': fe_mul(fa, fa, fb); break;
            case 'q': fe_sqr(fa, fa); break;
            case 'k': fe_mul_a24(fa, fa); break;
            case 'i': fe_invert(fa, fa); break;
            }
            fe_tobytes(out, fa);
            print_hex(out, 32);
        } else if (sscanf(line, "c %64s %24s %lx %lx", a, b, &counter, &len) == 4) {
            unhex(k, a, 32);
            unhex(nonce, b, 12);
            data = calloc(len ? len : 1, 1);    /* exact size, so an overrun is caught */
            chacha20(k, nonce, (u32)counter, data, (u32)len);
            print_hex(data, len);
            free(data);
        } else {
            fprintf(stderr, "bad line: %s", line);
            return 1;
        }
    }
    if (!idle_calls) {
        fprintf(stderr, "crypto_idle was never called\n");
        return 1;
    }
    return 0;
}
