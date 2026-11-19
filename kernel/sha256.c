/* SHA-256 and HMAC-SHA-256 (FIPS 180-4, RFC 2104).
 *
 * Used to prove to the terminal helper that this machine knows the shared
 * secret without sending the secret itself. Written for clarity, not speed:
 * it hashes a few hundred bytes once per connection.
 */
#include "kernel.h"

static const u32 K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL,
    0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL,
    0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL, 0xa2bfe8a1UL, 0xa81a664bUL,
    0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL,
    0x5b9cca4fUL, 0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL,
};

#define ROR(x, n) ((u32)(((x) >> (n)) | ((x) << (32 - (n)))))

static void block(u32 *h, const u8 *p)
{
    u32 w[64], a, b, c, d, e, f, g, hh, t1, t2;
    int i;

    for (i = 0; i < 16; i++, p += 4)
        w[i] = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    for (; i < 64; i++)
        w[i] = w[i - 16] + w[i - 7] +
               (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
               (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(struct sha256 *s)
{
    static const u32 start[8] = {
        0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
        0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL,
    };

    memcpy(s->h, start, sizeof start);
    s->total = 0;
    s->used = 0;
}

void sha256_update(struct sha256 *s, const void *data, u32 len)
{
    const u8 *p = data;

    s->total += len;
    while (len) {
        s->buf[s->used++] = *p++;
        len--;
        if (s->used == 64) {
            block(s->h, s->buf);
            s->used = 0;
        }
    }
}

void sha256_finish(struct sha256 *s, u8 *out32)
{
    u32 bits = s->total << 3, bits_hi = s->total >> 29;
    u8 pad = 0x80;
    u8 len[8];
    int i;

    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56)
        sha256_update(s, &pad, 1);
    for (i = 0; i < 4; i++) {
        len[i] = (u8)(bits_hi >> (24 - 8 * i));
        len[4 + i] = (u8)(bits >> (24 - 8 * i));
    }
    sha256_update(s, len, 8);
    for (i = 0; i < 8; i++) {
        out32[4 * i] = (u8)(s->h[i] >> 24);
        out32[4 * i + 1] = (u8)(s->h[i] >> 16);
        out32[4 * i + 2] = (u8)(s->h[i] >> 8);
        out32[4 * i + 3] = (u8)s->h[i];
    }
}

void hmac_sha256(const void *key, u32 key_len, const void *msg, u32 msg_len, u8 *out32)
{
    struct sha256 s;
    u8 k[64], pad[64], inner[32];
    int i;

    memset(k, 0, sizeof k);
    if (key_len > 64) {
        sha256_init(&s);
        sha256_update(&s, key, key_len);
        sha256_finish(&s, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, msg, msg_len);
    sha256_finish(&s, inner);
    for (i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5C;
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, inner, 32);
    sha256_finish(&s, out32);
}
