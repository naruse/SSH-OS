/* The "bench" command: how fast is this machine at the arithmetic of an SSH
 * connection? Connecting needs two X25519, one signature check and, when
 * logging in with a key, one signature. Once connected, every byte goes
 * through ChaCha20 and Poly1305.
 */
#include "kernel.h"
#include "shell.h"

/* The first test of RFC 7748, section 5.2. */
static const u8 rfc_scalar[32] = {
    0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d, 0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
    0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18, 0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4,
};
static const u8 rfc_point[32] = {
    0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb, 0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
    0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b, 0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c,
};
static const u8 rfc_answer[32] = {
    0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90, 0x8e, 0x94, 0xea, 0x4d, 0xf2, 0x8d, 0x08, 0x4f,
    0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7, 0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52,
};

static u8 buf[1024];

static u32 next_tick(void)
{
    u32 t = ticks;

    while (ticks == t)
        ;
    return ticks;
}

static void rate(const char *what, u32 bytes, u32 dt)
{
    u32 tenths;         /* tenths of a KB per second; the tick is 60.15 Hz */

    if (dt == 0)
        dt = 1;
    tenths = bytes / 1024 * 6015 / (dt * 10);
    kprintf("%s: %lu KB in %lu ticks, %lu.%lu KB per second.\n",
            what, bytes / 1024, dt, tenths / 10, tenths % 10);
}

void cmd_bench(void)
{
    struct sha256 s;
    u8 out[32], pub[32], sig[64];
    u32 t0, dt, tenths, i;
    int ok = 1;

    kputs("Timing the arithmetic of an SSH connection; give it a minute.\n");

    t0 = next_tick();
    x25519(out, rfc_scalar, rfc_point);
    dt = ticks - t0;
    for (i = 0; i < 32; i++)
        if (out[i] != rfc_answer[i])
            ok = 0;
    tenths = dt * 1000 / 6015;
    kprintf("X25519: %lu.%lu seconds (%lu ticks), answer %s.\n",
            tenths / 10, tenths % 10, dt, ok ? "correct" : "WRONG");

    for (i = 0; i < sizeof buf; i++)
        buf[i] = (u8)i;

    /* A key pair, a signature and its check; each must agree with the others. */
    t0 = next_tick();
    ed25519_public(pub, rfc_scalar);
    dt = ticks - t0;
    tenths = dt * 1000 / 6015;
    kprintf("Ed25519 public key from the secret: %lu.%lu seconds.\n", tenths / 10, tenths % 10);
    t0 = next_tick();
    ed25519_sign(sig, buf, 100, rfc_scalar, pub);
    dt = ticks - t0;
    tenths = dt * 1000 / 6015;
    kprintf("Ed25519 signature: %lu.%lu seconds.\n", tenths / 10, tenths % 10);
    t0 = next_tick();
    ok = ed25519_verify(sig, buf, 100, pub);
    dt = ticks - t0;
    sig[5] ^= 1;
    if (ed25519_verify(sig, buf, 100, pub))
        ok = 0;
    tenths = dt * 1000 / 6015;
    kprintf("Ed25519 signature check: %lu.%lu seconds, answer %s.\n",
            tenths / 10, tenths % 10, ok ? "correct" : "WRONG");

    t0 = next_tick();
    sha256_init(&s);
    for (i = 0; i < 32; i++)
        sha256_update(&s, buf, sizeof buf);
    sha256_finish(&s, out);
    rate("SHA-256", 32 * sizeof buf, ticks - t0);

    t0 = next_tick();
    for (i = 0; i < 16; i++)
        chacha20(rfc_scalar, rfc_point, i * 16, buf, sizeof buf);
    rate("ChaCha20", 16 * sizeof buf, ticks - t0);

    t0 = next_tick();
    for (i = 0; i < 16; i++)
        poly1305(out, rfc_scalar, buf, sizeof buf);
    rate("Poly1305", 16 * sizeof buf, ticks - t0);
    {
        /* the example of RFC 8439, section 2.5.2 */
        static const u8 key[32] = {
            0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06, 0xa8,
            0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd, 0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b,
        };
        static const u8 tag[16] = {
            0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6, 0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27, 0xa9,
        };

        poly1305(out, key, (const u8 *)"Cryptographic Forum Research Group", 34);
        kprintf("Poly1305 answer %s.\n", memcmp(out, tag, 16) == 0 ? "correct" : "WRONG");
    }
    kputs("BENCH-DONE\n");
}

/* How fast the terminal draws: text that scrolls, text that stays in place,
 * and text with a change of style every few letters, as full-screen
 * programs send it. The same figures as above, to set beside the ciphers. */
/* Feed the terminal a made-up screenful, a kilobyte at a time as SSH
 * would hand it over. */
static u8 feed[1024];

static u32 feed_all(u32 total)
{
    u32 t0 = next_tick(), left;

    for (left = total; left; left -= left < sizeof feed ? left : sizeof feed)
        vt_write(feed, left < sizeof feed ? left : sizeof feed);
    return ticks - t0;
}

/* The copying in copy.S against a byte at a time, for every mix of odd and
 * even ends, short and long, apart and overlapping either way. */
static void copy_check(void)
{
    static u8 a[700], b[700];
    static const u16 lens[] = { 0, 1, 2, 3, 4, 5, 7, 47, 48, 49, 95, 96, 97, 143, 144, 145, 300, 401 };
    u32 i, k, from, to, bad = 0, n, done = 0;

    for (k = 0; k < sizeof lens / sizeof lens[0]; k++)
        for (from = 100; from < 104; from++)
            for (to = 60; to < 150; to += (to >= 96 && to < 108) ? 1 : 9) {
                n = lens[k];
                for (i = 0; i < sizeof a; i++)
                    a[i] = b[i] = (u8)(i * 7 + k);
                if (to <= from)
                    for (i = 0; i < n; i++)
                        b[to + i] = b[from + i];
                else
                    for (i = n; i-- > 0;)
                        b[to + i] = b[from + i];
                memmove(a + to, a + from, n);
                for (i = 0; i < sizeof a; i++)
                    if (a[i] != b[i])
                        bad++;
                for (i = 0; i < sizeof a; i++)
                    a[i] = (u8)(i * 5 + 1), b[i] = 0;
                memcpy(b + to, a + from, n);
                for (i = 0; i < sizeof a; i++)
                    if (b[i] != (i >= to && i < to + n ? a[from + i - to] : 0))
                        bad++;
                done++;
            }
    kprintf("Copying: %lu cases, %lu bytes wrong.\n", done, bad);
}

void cmd_bench_term(void)
{
    u32 t0, dt, i, n;

    for (i = 0; i < 80; i++)
        buf[i] = (u8)('A' + i % 26 + (i & 32));

    /* lines of 80 letters, each ending in a new line: 1 KB is 12 of them */
    for (i = 0; i < sizeof feed; i++)
        feed[i] = i % 82 == 80 ? '\r' : i % 82 == 81 ? '\n' : buf[i % 82];
    vt_reset();
    dt = feed_all(32 * 1024);
    vt_end();
    con_clear();
    rate("Terminal, lines that scroll", 32 * 1024, dt);

    /* the same with nothing drawn until the end: the reading alone */
    vt_reset();
    vt_hold = 1;
    dt = feed_all(32 * 1024);
    vt_hold = 0;
    vt_show();
    vt_end();
    con_clear();
    rate("Terminal, reading only", 32 * 1024, dt);

    /* the same, drawn only now and then, as an SSH session does it */
    vt_reset();
    vt_hold = 1;
    t0 = next_tick();
    for (i = 0, n = t0, dt = 6; i < 32; i++) {
        vt_write(feed, sizeof feed);
        if (ticks - n >= dt) {
            n = ticks;
            vt_show();
            dt = (ticks - n) * 3 / 2;
            if (dt < 6)
                dt = 6;
            n = ticks;
        }
    }
    vt_hold = 0;
    vt_show();
    dt = ticks - t0;
    vt_end();
    con_clear();
    rate("Terminal, lines that pour in", 32 * 1024, dt);

    /* the same lines written over each other at the top */
    for (i = 0; i < sizeof feed; i++)
        feed[i] = i % 83 == 80 ? '\033' : i % 83 == 81 ? '[' : i % 83 == 82 ? 'E' : buf[i % 83];
    memcpy(feed, "\033[H", 3);
    vt_reset();
    dt = feed_all(32 * 1024);
    vt_end();
    con_clear();
    rate("Terminal, text in place", 32 * 1024, dt);

    /* a change of style every four letters, as full-screen programs send */
    for (i = 0; i < sizeof feed; i++)
        feed[i] = "\033[1mabcd\033[0mefgh"[i % 16];
    memcpy(feed, "\033[H", 3);
    vt_reset();
    dt = feed_all(32 * 1024);
    vt_end();
    con_clear();
    rate("Terminal, styled text in place", 32 * 1024, dt);

    /* the parts: the screen alone, one character at a time and a row at a time */
    t0 = next_tick();
    for (i = 0; i < 12; i++)
        for (n = 0; n < 32; n++)
            con_draw(0, (int)n, buf, 85, 0);
    rate("Screen only, rows of 85", 12 * 32 * 85, ticks - t0);
    t0 = next_tick();
    for (i = 0; i < 12 * 32 * 85; i++)
        con_draw((int)(i % 85), (int)(i / 85 % 32), buf + i % 80, 1, 0);
    rate("Screen only, one at a time", 12 * 32 * 85, ticks - t0);
    t0 = next_tick();
    for (i = 0; i < 31; i++)
        con_scroll(0, 31, 1);
    dt = ticks - t0;
    kprintf("Scrolling 31 lines: %lu ticks.\n", dt);
    t0 = next_tick();
    for (i = 0; i < 31; i++)
        con_scroll(0, 31, -1);
    kprintf("Scrolling 31 lines back: %lu ticks.\n", ticks - t0);
    copy_check();
    con_clear();
    kputs("BENCH-DONE\n");
}
