/* ssh: a Secure Shell client, so the SE can log in to another computer by
 * itself, with nothing in between.
 *
 * What it speaks (RFC 4253 and friends), chosen to be what every current
 * OpenSSH server offers and what this CPU can manage:
 *   key exchange   curve25519-sha256, with the "strict" extension
 *   server's key   ssh-ed25519
 *   packets        chacha20-poly1305@openssh.com
 *   login          an Ed25519 key from the setting "ssh_key", or a password
 *
 * All the arithmetic is done here by the 68000, which makes connecting
 * slow: about 12 seconds for each of the two halves of the key exchange,
 * about as long again to check the server's signature, and once more to
 * sign with our own key. The plan is to hand those four calculations to the
 * BlueSCSI's processor; they are x25519(), ed25519_verify() and
 * ed25519_sign().
 *
 * The screen is the terminal in vt.c, which says it is an xterm, so
 * full-screen programs work.
 *
 * Settings:
 *   ssh        = user@host:port   where "ssh" without arguments connects
 *   ssh_key    = ...              our key, from tools/sshkey.py
 *   ssh_hosts  = host SHA256:...  servers whose key is known (pairs); keys
 *                                 accepted at the keyboard are remembered
 *                                 in HOSTS.TXT on the SSH OS disk instead
 *   ssh_term   = xterm-256color   what kind of terminal to say we are (TERM)
 *   ssh_lang   = en_US.UTF-8      the language setting (LANG) asked of the
 *                                 server, so that it sends and takes accented
 *                                 letters as UTF-8; this is the default
 */
#include "kernel.h"
#include "net.h"
#include "shell.h"

#define RX_MAX          9000    /* longest packet accepted, all included */
#define TX_PAYLOAD      1024    /* longest packet contents we ever send */
#define CHAN_PACKET     4096    /* most output the server may put in one packet */
#define CHAN_WINDOW     16384   /* and how far it may run ahead of us: small, so
                                   that Control-C takes effect soon */
#define SHOW_EVERY      6               /* ticks, at least, between looks at text that pours in */
#define WAIT            (30 * 60)       /* ticks to wait for an answer */

#define MSG_DISCONNECT          1
#define MSG_IGNORE              2
#define MSG_UNIMPLEMENTED       3
#define MSG_DEBUG               4
#define MSG_SERVICE_REQUEST     5
#define MSG_SERVICE_ACCEPT      6
#define MSG_EXT_INFO            7
#define MSG_KEXINIT             20
#define MSG_NEWKEYS             21
#define MSG_KEX_ECDH_INIT       30
#define MSG_KEX_ECDH_REPLY      31
#define MSG_USERAUTH_REQUEST    50
#define MSG_USERAUTH_FAILURE    51
#define MSG_USERAUTH_SUCCESS    52
#define MSG_USERAUTH_BANNER     53
#define MSG_USERAUTH_PK_OK      60
#define MSG_GLOBAL_REQUEST      80
#define MSG_REQUEST_FAILURE     82
#define MSG_CHANNEL_OPEN        90
#define MSG_CHANNEL_OPEN_CONFIRMATION 91
#define MSG_CHANNEL_OPEN_FAILURE 92
#define MSG_CHANNEL_WINDOW_ADJUST 93
#define MSG_CHANNEL_DATA        94
#define MSG_CHANNEL_EXTENDED_DATA 95
#define MSG_CHANNEL_EOF         96
#define MSG_CHANNEL_CLOSE       97
#define MSG_CHANNEL_REQUEST     98
#define MSG_CHANNEL_SUCCESS     99
#define MSG_CHANNEL_FAILURE     100

#define ERR_SSH (-100)          /* our own failure; ssh.why says what */

static const char our_version[] = "SSH-2.0-SSHOS_" SSHOS_VERSION;
static const char name_ed25519[] = "ssh-ed25519";
static const char name_cipher[] = "chacha20-poly1305@openssh.com";

static struct {
    int c;                      /* the TCP connection */
    const char *why;            /* why the connection failed or ended */
    u8 first_kex;               /* still in the first key exchange */
    u8 strict;                  /* both sides do "strict" key exchange */
    u8 enc_in, enc_out;         /* packets in that direction are encrypted */
    u32 seq_in, seq_out;        /* packet counters */
    u8 key_in[64], key_out[64];
    u8 session_id[32];
    u8 host_key[32];            /* the server's public key */
    u32 rx_have, rx_len;        /* bytes of the next packet read so far; its length field once known */
    const u8 *payload;          /* the packet just received */
    u32 payload_len;
    u8 have_pair;               /* a one-time key pair is ready */
    u8 secret[32], q_c[32];
    char server_version[256];
    u32 server_version_len;
    /* the one channel: our shell */
    u32 remote_chan, remote_window, remote_packet, consumed;
    u8 chan_open, chan_reply, chan_closed, close_sent, have_exit;
    u32 exit_status;
    u8 log;                     /* setting "termlog": copy the shell's output to the serial port */
    u8 quiet;                   /* what the channel says is not for the screen (a command of ours) */
    u8 by_password;             /* logged in by typing the password */
    u8 own_key_used;            /* the key that was tried was this Macintosh's own */
} ssh;

static u8 rx[RX_MAX];
static u8 tx[5 + TX_PAYLOAD + 16 + 16];
static u32 tx_len;
static u8 tx_overflow;
static u8 my_kexinit[400];
static u32 my_kexinit_len;
static u8 scratch[TX_PAYLOAD + 40];

static int fail(const char *why)
{
    if (!ssh.why)
        ssh.why = why;
    return ERR_SSH;
}

static int net_fail(int err)
{
    if (!ssh.why)
        ssh.why = err == NET_ERR_CLOSED ? "the server closed the connection" : net_strerror(err);
    return ERR_SSH;
}

/* ------------------------------------------------------ building packets -- */

static void put_u8(u8 v)
{
    if (tx_len < TX_PAYLOAD)
        tx[5 + tx_len++] = v;
    else
        tx_overflow = 1;
}

static void put_start(u8 type)
{
    tx_len = 0;
    tx_overflow = 0;
    put_u8(type);
}

static void put_u32(u32 v)
{
    put_u8((u8)(v >> 24));
    put_u8((u8)(v >> 16));
    put_u8((u8)(v >> 8));
    put_u8((u8)v);
}

static void put_bytes(const void *data, u32 n)
{
    const u8 *p = data;

    while (n--)
        put_u8(*p++);
}

static void put_string(const void *data, u32 n)
{
    put_u32(n);
    put_bytes(data, n);
}

static void put_text(const char *s)
{
    put_string(s, strlen(s));
}

static void be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* The 12 bytes that select the key stream of packet number seq. */
static void make_nonce(u8 *nonce, u32 seq)
{
    memset(nonce, 0, 8);
    be32(nonce + 8, seq);
}

/* Send the packet built with put_...: add the length and padding, and
 * encrypt and sign it once keys are in use. */
static int packet_send(void)
{
    u8 nonce[12], polykey[32];
    u32 pad, total, n;
    int err;

    if (tx_overflow)
        return fail("a packet of ours grew too long");
    /* Padding of 4 to 11 bytes brings the packet to a multiple of 8; the
     * length field counts towards that only while nothing is encrypted. */
    pad = 8 - ((tx_len + 1 + (ssh.enc_out ? 0 : 4)) & 7);
    if (pad < 4)
        pad += 8;
    total = 1 + tx_len + pad;
    be32(tx, total);
    tx[4] = (u8)pad;
    rng_bytes(tx + 5 + tx_len, pad);
    n = 4 + total;
    if (ssh.enc_out) {
        /* by the BlueSCSI if it can and the packet is worth the trip */
        if (copro_seal(ssh.seq_out, ssh.key_out, tx, total) != 0) {
            make_nonce(nonce, ssh.seq_out);
            chacha20(ssh.key_out + 32, nonce, 0, tx, 4);
            memset(polykey, 0, sizeof polykey);
            chacha20(ssh.key_out, nonce, 0, polykey, sizeof polykey);
            chacha20(ssh.key_out, nonce, 1, tx + 4, total);
            poly1305(tx + n, polykey, tx, n);
        }
        n += 16;
    }
    ssh.seq_out++;
    err = tcp_write(ssh.c, tx, n, WAIT);
    return err ? net_fail(err) : 0;
}

/* ------------------------------------------------------- reading packets -- */

static const u8 *rp, *rend;
static u8 rbad;                 /* a packet ended before what we wanted from it */

static void get_start(void)
{
    rp = ssh.payload + 1;
    rend = ssh.payload + ssh.payload_len;
    rbad = 0;
}

static u8 get_u8(void)
{
    if (rp >= rend) {
        rbad = 1;
        return 0;
    }
    return *rp++;
}

static u32 get_u32(void)
{
    u32 v;

    if (rend - rp < 4) {
        rbad = 1;
        rp = rend;
        return 0;
    }
    v = ((u32)rp[0] << 24) | ((u32)rp[1] << 16) | ((u32)rp[2] << 8) | rp[3];
    rp += 4;
    return v;
}

static const u8 *get_string(u32 *len)
{
    const u8 *p;

    *len = get_u32();
    if ((u32)(rend - rp) < *len) {
        rbad = 1;
        rp = rend;
        *len = 0;
    }
    p = rp;
    rp += *len;
    return p;
}

static int same_text(const u8 *p, u32 len, const char *s)
{
    return len == strlen(s) && memcmp(p, s, len) == 0;
}

/* Is name one of the comma-separated names in list? */
static int list_has(const u8 *list, u32 len, const char *name)
{
    u32 i = 0, start;

    while (i <= len) {
        start = i;
        while (i < len && list[i] != ',')
            i++;
        if (same_text(list + start, i - start, name))
            return 1;
        i++;
    }
    return 0;
}

/* Print text that came from the other side: only what is safe to show. */
static void show_text(const u8 *p, u32 len)
{
    while (len--) {
        if (*p == '\n' || (*p >= 32 && *p < 127))
            kputc((char)*p);
        p++;
    }
}

/* Read what has arrived and see whether a whole packet is there: 1 = yes,
 * it is ssh.payload; 0 = not yet; otherwise an error. */
static int packet_poll(void)
{
    u8 nonce[12], polykey[32], tag[16], head[4], diff;
    u32 want, len, pad;
    int n, i;

    for (;;) {
        want = ssh.rx_len ? 4 + ssh.rx_len + (ssh.enc_in ? 16 : 0) : 4;
        if (ssh.rx_have < want) {
            n = tcp_recv(ssh.c, rx + ssh.rx_have, (u16)(want - ssh.rx_have));
            if (n < 0)
                return net_fail(n);
            if (n == 0)
                return 0;
            ssh.rx_have += (u32)n;
            continue;
        }
        make_nonce(nonce, ssh.seq_in);
        if (!ssh.rx_len) {
            memcpy(head, rx, 4);
            if (ssh.enc_in)
                chacha20(ssh.key_in + 32, nonce, 0, head, 4);
            len = ((u32)head[0] << 24) | ((u32)head[1] << 16) | ((u32)head[2] << 8) | head[3];
            if (len < 8 || len > RX_MAX - 4 - 16 || ((len + (ssh.enc_in ? 0 : 4)) & 7))
                return fail("the server sent a packet this version cannot take");
            ssh.rx_len = len;
            continue;
        }
        len = ssh.rx_len;
        if (ssh.enc_in) {
            /* by the BlueSCSI if it can and the packet is worth the trip */
            i = copro_open(ssh.seq_in, ssh.key_in, rx, len);
            if (i == 0)
                return fail("a packet arrived damaged or forged (check sum wrong)");
            if (i < 0) {
                memset(polykey, 0, sizeof polykey);
                chacha20(ssh.key_in, nonce, 0, polykey, sizeof polykey);
                poly1305(tag, polykey, rx, 4 + len);
                diff = 0;
                for (i = 0; i < 16; i++)
                    diff |= tag[i] ^ rx[4 + len + i];
                if (diff)
                    return fail("a packet arrived damaged or forged (check sum wrong)");
                chacha20(ssh.key_in, nonce, 1, rx + 4, len);
            }
        }
        pad = rx[4];
        if (pad + 2 > len)
            return fail("the server sent a malformed packet");
        ssh.payload = rx + 5;
        ssh.payload_len = len - 1 - pad;
        ssh.seq_in++;
        ssh.rx_have = 0;
        ssh.rx_len = 0;
        return 1;
    }
}

/* The next packet that is not housekeeping: its type, or 0 if none has
 * arrived and timeout_ticks is 0, or an error. */
static int packet_get(u32 timeout_ticks)
{
    struct timeout t;
    u32 len;
    const u8 *text;
    int r;

    timeout_start(&t, timeout_ticks);
    for (;;) {
        r = packet_poll();
        if (r < 0)
            return r;
        if (r == 1) {
            switch (ssh.payload[0]) {
            case MSG_IGNORE:
            case MSG_DEBUG:
            case MSG_UNIMPLEMENTED:
            case MSG_EXT_INFO:
                /* Strict key exchange: nothing may be slipped in before the
                 * first keys are in use. */
                if (ssh.first_kex && ssh.strict)
                    return fail("unexpected packet during the key exchange");
                continue;
            case MSG_DISCONNECT:
                get_start();
                get_u32();
                text = get_string(&len);
                kputs("\nThe server says: ");
                show_text(text, len);
                kputc('\n');
                return fail("the server ended the connection");
            default:
                return ssh.payload[0];
            }
        }
        if (!timeout_ticks)
            return 0;
        r = net_wait_step(&t);
        if (r)
            return net_fail(r);
    }
}

/* Wait for one particular packet. */
static int packet_expect(u8 type, const char *otherwise)
{
    int r = packet_get(WAIT);

    if (r < 0)
        return r;
    if (r != type)
        return fail(otherwise);
    get_start();
    return 0;
}

/* -------------------------------------------------------------- base64 -- */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Without the = padding, as OpenSSH prints key fingerprints. */
static void base64_encode(char *out, const u8 *in, u32 n)
{
    u32 i, v;

    for (i = 0; i < n; i += 3) {
        v = (u32)in[i] << 16;
        if (i + 1 < n)
            v |= (u32)in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        *out++ = b64[(v >> 18) & 63];
        *out++ = b64[(v >> 12) & 63];
        if (i + 1 < n)
            *out++ = b64[(v >> 6) & 63];
        if (i + 2 < n)
            *out++ = b64[v & 63];
    }
    *out = 0;
}

/* Returns the number of bytes, or -1 if s is not base64 or does not fit. */
static int base64_decode(u8 *out, int max, const char *s)
{
    u32 v = 0;
    int bits = 0, n = 0, i;

    for (; *s && *s != '='; s++) {
        for (i = 0; i < 64 && b64[i] != *s; i++)
            ;
        if (i == 64)
            return -1;
        v = (v << 6) | (u32)i;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max)
                return -1;
            out[n++] = (u8)(v >> bits);
        }
    }
    return n;
}

/* -------------------------------------------------------- key exchange -- */

static u8 busy_turns;

/* Called from inside the long calculations. */
static void busy(void)
{
    net_poll();
    bar_tick();
    if ((++busy_turns & 7) == 0)
        kputc('.');
    /* Esc (or whatever else stops a wait for the network) stops these too. */
    if (kbd_escape || (net_abort_check && net_abort_check()))
        crypto_stop = 1;
}

static void hash_string(struct sha256 *h, const void *data, u32 n)
{
    u8 len[4];

    be32(len, n);
    sha256_update(h, len, 4);
    sha256_update(h, data, n);
}

/* A number as SSH writes it: 4 bytes of length, then the bytes, most
 * significant first, without leading zeros but never looking negative. */
static u32 make_mpint(u8 *out, const u8 *bytes32)
{
    u32 skip = 0, n;

    while (skip < 32 && bytes32[skip] == 0)
        skip++;
    n = 32 - skip;
    if (n && (bytes32[skip] & 0x80)) {
        out[4] = 0;
        memcpy(out + 5, bytes32 + skip, n);
        n++;
    } else {
        memcpy(out + 4, bytes32 + skip, n);
    }
    be32(out, n);
    return 4 + n;
}

/* One 64-byte key: SHA-256 of the shared secret, the exchange hash, a letter
 * and the session's name, and then the same again for the second half. */
static void derive_key(u8 *key64, const u8 *k_mpint, u32 k_len, const u8 *h, char letter)
{
    struct sha256 s;

    sha256_init(&s);
    sha256_update(&s, k_mpint, k_len);
    sha256_update(&s, h, 32);
    sha256_update(&s, &letter, 1);
    sha256_update(&s, ssh.session_id, 32);
    sha256_finish(&s, key64);
    sha256_init(&s);
    sha256_update(&s, k_mpint, k_len);
    sha256_update(&s, h, 32);
    sha256_update(&s, key64, 32);
    sha256_finish(&s, key64 + 32);
}

/* Make the one-time key pair for a key exchange. Done before connecting
 * the first time, so the server is not kept waiting for it. */
static void make_pair(void)
{
    static const u8 base[32] = {9};

    rng_bytes(ssh.secret, 32);
    if (copro_x25519(ssh.q_c, ssh.secret, base) != 0)
        x25519(ssh.q_c, ssh.secret, base);
    ssh.have_pair = 1;
}

/* Is this server's key the one we know for it? 1 yes, 0 never seen,
 * -1 a different key is on record. The setting holds pairs: host, print. */
static int known_host(const char *host, const char *print)
{
    const char *p = config_get("ssh_hosts");
    u32 hl = strlen(host), pl = strlen(print), n;
    int is_host, found = 0;

    while (p && *p) {
        while (*p == ' ' || *p == ',')
            p++;
        for (n = 0; p[n] && p[n] != ' ' && p[n] != ','; n++)
            ;
        is_host = n == hl && memcmp(p, host, n) == 0;
        p += n;
        while (*p == ' ')
            p++;
        for (n = 0; p[n] && p[n] != ' ' && p[n] != ','; n++)
            ;
        if (is_host) {
            if (n == pl && memcmp(p, print, n) == 0)
                return 1;
            found = -1;
        }
        p += n;
    }
    /* Not in the settings: one accepted at the keyboard before, perhaps. */
    return found ? found : hosts_known(host, print);
}

/* Edit a line of text in a field: the line (len bytes of at most max, kept
 * closed with a 0), the caret before byte *atp, and one key: its character
 * c, or 0 and its key number raw for the Left and Right arrows. Typing and
 * Delete happen at the caret. Accented letters are UTF-8 and count as one.
 * Returns 1 if the text changed, 2 if only the caret moved, 0 if nothing. */
int kbd_line_edit(char *line, int *lenp, int *atp, int max, u8 c, u8 raw)
{
    int len = *lenp, at = *atp, n = 0, from;
    char add[3];

    if (!c && (raw == 0x3B || raw == 0x3C)) {
        if (raw == 0x3B) {
            if (at == 0)
                return 0;
            for (at--; at > 0 && ((u8)line[at] & 0xC0) == 0x80; at--)
                ;
        } else {
            if (at >= len)
                return 0;
            for (at++; at < len && ((u8)line[at] & 0xC0) == 0x80; at++)
                ;
        }
        *atp = at;
        return 2;
    }
    if (c == '\b') {
        if (at == 0)
            return 0;
        for (from = at - 1; from > 0 && ((u8)line[from] & 0xC0) == 0x80; from--)
            ;
        memmove(line + from, line + at, (u32)(len - at));
        len -= at - from;
        line[len] = 0;
        *lenp = len;
        *atp = from;
        return 1;
    }
    if (c < 32 || c == 127)
        return 0;
    if (c < 0x80) {
        add[n++] = (char)c;
    } else {
        u32 cp = c >= 0xA0 ? c : charset_extra[c - 0x80];

        if (cp >= 0x800) {
            add[n++] = (char)(0xE0 | (cp >> 12));
            add[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        } else {
            add[n++] = (char)(0xC0 | (cp >> 6));
        }
        add[n++] = (char)(0x80 | (cp & 0x3F));
    }
    if (len + n > max - 1)
        return 0;
    memmove(line + at + n, line + at, (u32)(len - at));
    memcpy(line + at, add, (u32)n);
    len += n;
    line[len] = 0;
    *lenp = len;
    *atp = at + n;
    return 1;
}

/* Take one typed character into a line of max bytes: Delete removes the
 * last one, anything printable is added (accented letters as UTF-8).
 * Returns 1 if a character was added, -1 if one was removed, 0 if nothing
 * changed. */
int kbd_line_key(char *line, int *lenp, int max, u8 c)
{
    int len = *lenp;

    if (c == '\b') {
        /* step back over one whole character */
        while (len > 0 && ((u8)line[len - 1] & 0xC0) == 0x80)
            len--;
        if (len == 0)
            return 0;
        *lenp = len - 1;
        return -1;
    }
    if (c < 32 || c == 127 || len > max - 4)
        return 0;
    if (c < 0x80) {
        line[len++] = (char)c;
    } else {
        u32 cp = c >= 0xA0 ? c : charset_extra[c - 0x80];

        if (cp >= 0x800) {
            line[len++] = (char)(0xE0 | (cp >> 12));
            line[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        } else {
            line[len++] = (char)(0xC0 | (cp >> 6));
        }
        line[len++] = (char)(0x80 | (cp & 0x3F));
    }
    *lenp = len;
    return 1;
}

/* Wait for a line typed at the keyboard, keeping the network answered.
 * With echo off nothing shows (for passwords). Returns the length, or -1 if
 * Esc was pressed. Accented letters are stored as UTF-8. */
int kbd_ask_line(char *line, int max, int echo)
{
    int len = 0, ev, r;
    u8 c;

    for (;;) {
        net_poll();
        ev = kbd_get_event();
        if (ev < 0) {
            bar_tick();
            bar_idle();
            continue;
        }
        c = (u8)(ev & 0xFF);
        if ((ev & 0x8000) || !c)
            continue;
        if (c == 0x1B) {
            kbd_escape = 0;
            kputc('\n');
            return -1;
        }
        if (c == '\n') {
            kputc('\n');
            line[len] = 0;
            return len;
        }
        r = kbd_line_key(line, &len, max, c);
        if (echo && r < 0)
            kputs("\b \b");
        else if (echo && r > 0)
            kputc((char)c);
    }
}

/* Decide whether to trust the server's key. */
static int check_host_key(const char *host, const u8 *blob, u32 blob_len)
{
    struct sha256 s;
    u8 digest[32];
    char print[7 + 44];
    int known;

    sha256_init(&s);
    sha256_update(&s, blob, blob_len);
    sha256_finish(&s, digest);
    memcpy(print, "SHA256:", 7);
    base64_encode(print + 7, digest, 32);

    known = known_host(host, print);
    if (known == 1)
        return 0;
    if (known < 0) {
        kprintf("\nWARNING: the key of %s is NOT the one on record for it.\n"
                "It is now %s\n"
                "Either the server was set up again, or something is in between,\n"
                "pretending to be it. Not connecting.\n", host, print);
        return fail("the server's key has changed");
    }
    if (!ask_trust(host, print))
        return fail("the server's key was not accepted");
    if (hosts_add(host, print) == 0)
        kputs("Remembered on the SSH OS disk: it will not ask again.\n");
    else
        kprintf("There is no SSH OS disk to remember that on. To skip this question, add\n"
                "to config.txt:\n  ssh_hosts = %s %s\n", host, print);
    return 0;
}

/* Agree on keys with the server. The first time this also swaps version
 * lines and decides whether the server is who we think. On a later round
 * (asked for by the server) its KEXINIT packet is the one just received. */
static int key_exchange(const char *host)
{
    struct sha256 h;
    u8 k[32], k_mpint[37], hash[32], new_in[64], new_out[64], sig[64];
    const u8 *list, *ks, *qs, *p, *server_key;
    u32 len, ks_len, qs_len, k_len, n, i;
    int r, first = ssh.first_kex, guess_wrong;
    u8 any = 0, follows;

    if (first) {
        /* Version lines. The server may send other lines before its own. */
        r = tcp_write(ssh.c, our_version, sizeof our_version - 1, WAIT);
        if (r == 0)
            r = tcp_write(ssh.c, "\r\n", 2, WAIT);
        if (r)
            return net_fail(r);
        for (n = 0;; n++) {
            char ch;

            if (n > 40)
                return fail("that is not an SSH server");
            for (len = 0;;) {
                r = tcp_read(ssh.c, &ch, 1, WAIT);
                if (r < 0)
                    return net_fail(r);
                if (ch == '\n')
                    break;
                if (len < sizeof ssh.server_version - 1)
                    ssh.server_version[len++] = ch;
            }
            if (len && ssh.server_version[len - 1] == '\r')
                len--;
            if (len >= 4 && memcmp(ssh.server_version, "SSH-", 4) == 0)
                break;
        }
        ssh.server_version[len] = 0;
        ssh.server_version_len = len;
        if (memcmp(ssh.server_version, "SSH-2.0-", 8) != 0 &&
            memcmp(ssh.server_version, "SSH-1.99-", 9) != 0)
            return fail("the server only speaks an old version of SSH");
        kputs("Server: ");
        show_text((const u8 *)ssh.server_version + 8, len - 8);
        kputc('\n');
    }

    /* Our list of what we can do: one choice of each. */
    put_start(MSG_KEXINIT);
    rng_bytes(scratch, 16);
    put_bytes(scratch, 16);
    put_text("curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-c-v00@openssh.com");
    put_text(name_ed25519);
    put_text(name_cipher);
    put_text(name_cipher);
    put_text("hmac-sha2-256");          /* not used: the cipher has its own check sum */
    put_text("hmac-sha2-256");
    put_text("none");
    put_text("none");
    put_text("");
    put_text("");
    put_u8(0);
    put_u32(0);
    memcpy(my_kexinit, tx + 5, tx_len);
    my_kexinit_len = tx_len;
    r = packet_send();
    if (r)
        return r;

    /* The server's list. */
    if (first) {
        r = packet_expect(MSG_KEXINIT, "the server did not start a key exchange");
        if (r)
            return r;
    }
    sha256_init(&h);
    hash_string(&h, our_version, sizeof our_version - 1);
    hash_string(&h, ssh.server_version, ssh.server_version_len);
    hash_string(&h, my_kexinit, my_kexinit_len);
    hash_string(&h, ssh.payload, ssh.payload_len);

    get_start();
    for (i = 0; i < 16; i++)
        get_u8();
    list = get_string(&len);
    if (!list_has(list, len, "curve25519-sha256") &&
        !list_has(list, len, "curve25519-sha256@libssh.org"))
        return fail("the server has no key exchange this version knows (curve25519-sha256)");
    if (first)
        ssh.strict = (u8)list_has(list, len, "kex-strict-s-v00@openssh.com");
    if (first && ssh.strict && ssh.seq_in != 1)
        return fail("unexpected packet during the key exchange");
    guess_wrong = !(len >= 17 && memcmp(list, "curve25519-sha256", 17) == 0);
    list = get_string(&len);
    if (!list_has(list, len, name_ed25519))
        return fail("the server has no ssh-ed25519 key, the only kind this version knows");
    if (!same_text(list, len < 11 ? len : 11, name_ed25519))
        guess_wrong = 1;
    list = get_string(&len);
    if (!list_has(list, len, name_cipher))
        return fail("the server lacks the chacha20-poly1305 cipher this version needs");
    list = get_string(&len);
    if (!list_has(list, len, name_cipher))
        return fail("the server lacks the chacha20-poly1305 cipher this version needs");
    get_string(&len);
    get_string(&len);
    list = get_string(&len);
    if (!list_has(list, len, "none"))
        return fail("the server insists on compression");
    list = get_string(&len);
    if (!list_has(list, len, "none"))
        return fail("the server insists on compression");
    get_string(&len);
    get_string(&len);
    follows = get_u8();
    if (rbad)
        return fail("the server sent a malformed packet");

    if (!ssh.have_pair) {
        kputs("Making a one-time key");
        make_pair();
        kputc('\n');
    }
    put_start(MSG_KEX_ECDH_INIT);
    put_string(ssh.q_c, 32);
    r = packet_send();
    if (r)
        return r;
    if (follows && guess_wrong) {       /* the server guessed, wrongly: skip its guess */
        r = packet_get(WAIT);
        if (r < 0)
            return r;
    }
    r = packet_expect(MSG_KEX_ECDH_REPLY, "the server did not answer the key exchange");
    if (r)
        return r;
    ks = get_string(&ks_len);
    qs = get_string(&qs_len);
    p = get_string(&len);
    if (rbad || qs_len != 32)
        return fail("the server sent a malformed packet");

    /* The server's key and its signature, each wrapped with their type. */
    rend = p + len;
    rp = p;
    list = get_string(&len);
    if (!same_text(list, len, name_ed25519))
        rbad = 1;
    p = get_string(&len);
    if (rbad || len != 64)
        return fail("the server signed in a way this version does not know");
    memcpy(sig, p, 64);
    rp = ks;
    rend = ks + ks_len;
    list = get_string(&len);
    if (!same_text(list, len, name_ed25519))
        rbad = 1;
    server_key = get_string(&len);
    if (rbad || len != 32)
        return fail("the server's key is of a kind this version does not know");
    if (!first && memcmp(server_key, ssh.host_key, 32) != 0)
        return fail("the server changed its key in mid-connection");

    kputs("Working out the shared secret");
    if (copro_x25519(k, ssh.secret, qs) != 0)
        x25519(k, ssh.secret, qs);
    kputc('\n');
    if (crypto_stop)
        return fail("stopped");
    memset(ssh.secret, 0, sizeof ssh.secret);
    ssh.have_pair = 0;
    for (i = 0; i < 32; i++)
        any |= k[i];
    if (!any)
        return fail("the server sent an unusable key exchange value");

    k_len = make_mpint(k_mpint, k);
    hash_string(&h, ks, ks_len);
    hash_string(&h, ssh.q_c, 32);
    hash_string(&h, qs, 32);
    sha256_update(&h, k_mpint, k_len);
    sha256_finish(&h, hash);

    kputs("Checking the server's signature");
    r = copro_ed25519_verify(sig, hash, 32, server_key);
    if (r < 0)
        r = ed25519_verify(sig, hash, 32, server_key);
    kputc('\n');
    if (crypto_stop)
        return fail("stopped");
    if (!r)
        return fail("the server's signature is wrong: it does not hold the key it showed");
    if (first) {
        memcpy(ssh.host_key, server_key, 32);
        memcpy(ssh.session_id, hash, 32);
        /* ks points into the receive buffer; the question below leaves
         * that alone, as no packet is read while it waits. */
        r = check_host_key(host, ks, ks_len);
        if (r)
            return r;
    }

    derive_key(new_out, k_mpint, k_len, hash, 'C');
    derive_key(new_in, k_mpint, k_len, hash, 'D');
    memset(k, 0, sizeof k);
    memset(k_mpint, 0, sizeof k_mpint);

    put_start(MSG_NEWKEYS);
    r = packet_send();
    if (r)
        return r;
    memcpy(ssh.key_out, new_out, 64);
    ssh.enc_out = 1;
    if (ssh.strict)
        ssh.seq_out = 0;

    r = packet_expect(MSG_NEWKEYS, "the server did not switch to the new keys");
    if (r)
        return r;
    memcpy(ssh.key_in, new_in, 64);
    ssh.enc_in = 1;
    if (ssh.strict)
        ssh.seq_in = 0;
    ssh.first_kex = 0;
    memset(new_in, 0, sizeof new_in);
    memset(new_out, 0, sizeof new_out);
    return 0;
}

/* ---------------------------------------------------------------- login -- */

#define AUTH_NO     0
#define AUTH_YES    1
#define AUTH_KEY_OK 2

static char methods[80];        /* what the server last said may still work */

/* The server's answer to a login attempt. */
static int auth_answer(void)
{
    const u8 *text;
    u32 len;
    int r;

    for (;;) {
        r = packet_get(WAIT);
        if (r < 0)
            return r;
        get_start();
        switch (r) {
        case MSG_USERAUTH_BANNER:
            text = get_string(&len);
            show_text(text, len);
            if (len && text[len - 1] != '\n')
                kputc('\n');
            break;
        case MSG_USERAUTH_SUCCESS:
            return AUTH_YES;
        case MSG_USERAUTH_PK_OK:
            return AUTH_KEY_OK;
        case MSG_USERAUTH_FAILURE:
            text = get_string(&len);
            if (len > sizeof methods - 1)
                len = sizeof methods - 1;
            memcpy(methods, text, len);
            methods[len] = 0;
            return AUTH_NO;
        default:
            return fail("unexpected answer while logging in");
        }
    }
}

static void auth_start(const char *user, const char *method)
{
    put_start(MSG_USERAUTH_REQUEST);
    put_text(user);
    put_text("ssh-connection");
    put_text(method);
}

static void put_key_blob(const u8 *pub)
{
    put_u32(4 + 11 + 4 + 32);
    put_text(name_ed25519);
    put_string(pub, 32);
}

static int authenticate(const char *user, const char *host)
{
    const char *setting = config_get("ssh_key");
    static char password[100];
    u8 key[64], sig[64];
    int r, tries, have_key = 0;

    put_start(MSG_SERVICE_REQUEST);
    put_text("ssh-userauth");
    r = packet_send();
    if (r == 0)
        r = packet_expect(MSG_SERVICE_ACCEPT, "the server does not offer logging in");
    if (r)
        return r;

    if (setting && setting[0]) {
        if (base64_decode(key, sizeof key, setting) != 64)
            kputs("The setting ssh_key is not a key from tools/sshkey.py; not using it.\n");
        else
            have_key = 1;
    }
    /* Otherwise the key this Macintosh made for itself, if it has. */
    if (!have_key && own_key_get(key)) {
        have_key = 1;
        ssh.own_key_used = 1;
    }

    /* Ask first whether the server would take our key: signing is slow. */
    if (have_key) {
        auth_start(user, "publickey");
        put_u8(0);
        put_text(name_ed25519);
        put_key_blob(key + 32);
    } else {
        auth_start(user, "none");
    }
    r = packet_send();
    if (r == 0)
        r = auth_answer();
    if (r == AUTH_KEY_OK) {
        /* What is signed: the session's name, then the request itself. */
        auth_start(user, "publickey");
        put_u8(1);
        put_text(name_ed25519);
        put_key_blob(key + 32);
        if (tx_overflow)
            return fail("the user name is too long");
        be32(scratch, 32);
        memcpy(scratch + 4, ssh.session_id, 32);
        memcpy(scratch + 36, tx + 5, tx_len);
        kputs("Signing in with the key");
        if (copro_ed25519_sign(sig, scratch, 36 + tx_len, key, key + 32) != 0)
            ed25519_sign(sig, scratch, 36 + tx_len, key, key + 32);
        kputc('\n');
        if (crypto_stop)
            return fail("stopped");
        put_u32(4 + 11 + 4 + 64);
        put_text(name_ed25519);
        put_string(sig, 64);
        r = packet_send();
        if (r == 0)
            r = auth_answer();
    }
    memset(key, 0, sizeof key);
    if (r != AUTH_NO)
        return r == AUTH_YES ? 0 : r < 0 ? r : fail("unexpected answer while logging in");
    if (have_key && !ssh.own_key_used)
        kputs("The server does not accept the key in ssh_key for this user.\n");

    for (tries = 0; tries < 3 && list_has((const u8 *)methods, strlen(methods), "password"); tries++) {
        r = ask_password(user, host, password, sizeof password);
        if (r < 0)
            return fail("stopped at the password");
        auth_start(user, "password");
        put_u8(0);
        put_text(password);
        memset(password, 0, sizeof password);
        r = packet_send();
        memset(tx, 0, sizeof tx);
        if (r == 0)
            r = auth_answer();
        if (r == AUTH_YES) {
            ssh.by_password = 1;
            return 0;
        }
        if (r != AUTH_NO)
            return r < 0 ? r : fail("the server wants the password changed; do that from another computer");
        kputs("Not accepted.\n");
    }
    if (tries == 0)
        kprintf("The server only accepts: %s\n", methods);
    return fail("could not log in");
}

/* -------------------------------------------------------------- session -- */

static void to_terminal(char c)
{
    if (c == '\n')
        vt_write((const u8 *)"\r", 1);
    vt_write((const u8 *)&c, 1);
}

/* Act on one packet of the logged-in connection. */
static int handle(int type, const char *host)
{
    const u8 *text;
    u32 len, n;
    u8 want_reply;
    int r = 0;

    get_start();
    switch (type) {
    case MSG_KEXINIT:
        /* What the key exchange prints goes onto the terminal, where the
         * program's own text is, not to wherever the console last wrote. */
        kputc_divert(to_terminal);
        kputs("\n[The server asks for fresh keys; this takes a while.]\n");
        r = key_exchange(host);
        kputc_divert(NULL);
        return r;
    case MSG_GLOBAL_REQUEST:
        get_string(&len);
        if (get_u8()) {
            put_start(MSG_REQUEST_FAILURE);
            r = packet_send();
        }
        break;
    case MSG_CHANNEL_OPEN:              /* we offer nothing the server could open */
        get_string(&len);
        n = get_u32();
        put_start(MSG_CHANNEL_OPEN_FAILURE);
        put_u32(n);
        put_u32(1);
        put_text("");
        put_text("");
        r = packet_send();
        break;
    case MSG_CHANNEL_OPEN_CONFIRMATION:
        get_u32();
        ssh.remote_chan = get_u32();
        ssh.remote_window = get_u32();
        ssh.remote_packet = get_u32();
        ssh.chan_open = 1;
        break;
    case MSG_CHANNEL_OPEN_FAILURE:
        return fail("the server refused to open a session");
    case MSG_CHANNEL_WINDOW_ADJUST:
        get_u32();
        ssh.remote_window += get_u32();
        break;
    case MSG_CHANNEL_EXTENDED_DATA:
    case MSG_CHANNEL_DATA:
        get_u32();
        if (type == MSG_CHANNEL_EXTENDED_DATA)
            get_u32();
        text = get_string(&len);
        if (rbad)
            break;
        if (!ssh.quiet)
            vt_write(text, len);
        ssh.consumed += len;
        if (ssh.consumed >= CHAN_WINDOW / 2 && !ssh.chan_closed) {
            put_start(MSG_CHANNEL_WINDOW_ADJUST);
            put_u32(ssh.remote_chan);
            put_u32(ssh.consumed);
            ssh.consumed = 0;
            r = packet_send();
        }
        break;
    case MSG_CHANNEL_EOF:
        break;
    case MSG_CHANNEL_CLOSE:
        ssh.chan_closed = 1;
        break;
    case MSG_CHANNEL_REQUEST:
        get_u32();
        text = get_string(&len);
        want_reply = get_u8();
        if (same_text(text, len, "exit-status")) {
            ssh.exit_status = get_u32();
            ssh.have_exit = 1;
        }
        if (want_reply && !rbad) {
            put_start(MSG_CHANNEL_FAILURE);
            put_u32(ssh.remote_chan);
            r = packet_send();
        }
        break;
    case MSG_CHANNEL_SUCCESS:
        ssh.chan_reply = 1;
        break;
    case MSG_CHANNEL_FAILURE:
        ssh.chan_reply = 2;
        break;
    default:                            /* say so, as the rules ask, and carry on */
        put_start(MSG_UNIMPLEMENTED);
        put_u32(ssh.seq_in - 1);
        r = packet_send();
        break;
    }
    if (rbad)
        return fail("the server sent a malformed packet");
    return r;
}

/* Handle packets until *flag is set. */
static int wait_for(const u8 *flag, const char *host)
{
    int r;

    while (!*flag) {
        r = packet_get(WAIT);
        if (r < 0)
            return r;
        r = handle(r, host);
        if (r)
            return r;
        if (ssh.chan_closed)
            return fail("the server closed the session");
    }
    return 0;
}

/* Run one command on the server, in a session of its own, and wait for it
 * to end. What it prints is not shown. 0 = it ran and said all went well,
 * 1 = it did not; below 0 the connection itself failed. */
static int run_command(const char *host, const char *command)
{
    int r, ok;

    ssh.quiet = 1;
    put_start(MSG_CHANNEL_OPEN);
    put_text("session");
    put_u32(0);
    put_u32(CHAN_WINDOW);
    put_u32(CHAN_PACKET);
    r = packet_send();
    if (r == 0)
        r = wait_for(&ssh.chan_open, host);
    if (r == 0) {
        put_start(MSG_CHANNEL_REQUEST);
        put_u32(ssh.remote_chan);
        put_text("exec");
        put_u8(1);
        put_text(command);
        r = tx_overflow ? fail("the command is too long") : packet_send();
    }
    /* until the server closes it: the answer to "exec", the command's
     * verdict and the end all come by themselves */
    while (r == 0 && !ssh.chan_closed) {
        r = packet_get(WAIT);
        if (r >= 0)
            r = handle(r, host);
    }
    ok = ssh.chan_reply == 1 && ssh.have_exit && ssh.exit_status == 0;
    if (r == 0) {
        put_start(MSG_CHANNEL_CLOSE);
        put_u32(ssh.remote_chan);
        r = packet_send();
    }
    /* the one channel is free again, for the shell */
    ssh.chan_open = ssh.chan_reply = ssh.chan_closed = ssh.have_exit = 0;
    ssh.consumed = 0;
    ssh.quiet = 0;
    return r < 0 ? r : r ? r : ok ? 0 : 1;
}

/* Logged in by password: offer to add this Macintosh's own key to the
 * account, so that next time there is nothing to type. The key is made the
 * first time it is wanted and kept on the SSH OS disk; the password is not
 * kept anywhere. Whatever goes wrong here, the log-in itself stands: only
 * a broken connection is passed on. */
static int offer_key(const char *user, const char *host)
{
    static char who[112], command[280];
    const char *setting = config_get("ssh_key"), *force = config_get("ssh_offer_key");
    u8 key[64], blob[51];
    char b64key[72];
    int have = 0, r;
    u32 n;

    if (!(ssh.by_password || (force && force[0] == '1')) || !fs_mounted())
        return 0;
    n = strlen(user);
    if (n + strlen(host) + 2 > sizeof who)
        return 0;
    memcpy(who, user, n);
    who[n] = '@';
    memcpy(who + n + 1, host, strlen(host) + 1);
    if (own_key_refused(who))
        return 0;
    /* a key from the settings, if there is one, is the one to add */
    if (setting && setting[0] && base64_decode(key, sizeof key, setting) == 64 && ssh.by_password)
        have = 1;
    else
        have = own_key_get(key);
    if (!ask_keyless(user, host, !have)) {
        own_key_refuse(who);
        memset(key, 0, sizeof key);
        return 0;
    }
    if (!have) {
        kputs("Making this Macintosh's own key");
        rng_bytes(key, 32);
        if (copro_ed25519_public(key + 32, key) != 0)
            ed25519_public(key + 32, key);
        kputc('\n');
        if (crypto_stop) {
            memset(key, 0, sizeof key);
            return fail("stopped");
        }
        if (own_key_keep(key) != 0) {
            kputs("The key could not be kept on the SSH OS disk; the password stays.\n");
            memset(key, 0, sizeof key);
            return 0;
        }
    }
    /* the public half as OpenSSH writes it: "ssh-ed25519 AAAA... name" */
    be32(blob, 11);
    memcpy(blob + 4, name_ed25519, 11);
    be32(blob + 15, 32);
    memcpy(blob + 19, key + 32, 32);
    memset(key, 0, sizeof key);
    base64_encode(b64key, blob, sizeof blob);
    {
        static const char *const part[] = {
            "umask 077; mkdir -p ~/.ssh; grep -qF '", 0, "' ~/.ssh/authorized_keys 2>/dev/null || "
            "echo 'ssh-ed25519 ", 0, " SSH-OS' >> ~/.ssh/authorized_keys",
        };
        char *t = command;
        int i;

        for (i = 0; i < 5; i++) {
            const char *p = part[i] ? part[i] : b64key;

            while (*p)
                *t++ = *p++;
        }
        *t = 0;
    }
    kputs("Adding the key to your account on the server\n");
    r = run_command(host, command);
    if (r < 0)
        return r;
    kputs(r == 0 ? "Done: no password next time.\n" :
          "The server would not take it; the password stays.\n");
    return 0;
}

static int open_shell(const char *host)
{
    const char *lang = config_get("ssh_lang"), *term = config_get("ssh_term");
    int r;

    put_start(MSG_CHANNEL_OPEN);
    put_text("session");
    put_u32(0);
    put_u32(CHAN_WINDOW);
    put_u32(CHAN_PACKET);
    r = packet_send();
    if (r == 0)
        r = wait_for(&ssh.chan_open, host);
    if (r)
        return r;

    put_start(MSG_CHANNEL_REQUEST);
    put_u32(ssh.remote_chan);
    put_text("pty-req");
    put_u8(1);
    put_text(term && term[0] ? term : "xterm-256color");
    put_u32(CON_COLS);
    put_u32(CON_ROWS);
    put_u32(0);
    put_u32(0);
    put_string("", 1);                  /* terminal modes: none, just the end mark */
    r = packet_send();
    if (r == 0)
        r = wait_for(&ssh.chan_reply, host);
    if (r)
        return r;
    if (ssh.chan_reply != 1)
        return fail("the server refused to give us a terminal");

    /* Ask for UTF-8. A server that does not allow this just ignores it. */
    if (!lang)
        lang = "en_US.UTF-8";
    if (lang[0]) {
        put_start(MSG_CHANNEL_REQUEST);
        put_u32(ssh.remote_chan);
        put_text("env");
        put_u8(0);
        put_text("LANG");
        put_text(lang);
        r = packet_send();
        if (r)
            return r;
    }

    ssh.chan_reply = 0;
    put_start(MSG_CHANNEL_REQUEST);
    put_u32(ssh.remote_chan);
    put_text("shell");
    put_u8(1);
    r = packet_send();
    if (r == 0)
        r = wait_for(&ssh.chan_reply, host);
    if (r)
        return r;
    if (ssh.chan_reply != 1)
        return fail("the server refused to start a shell");
    return 0;
}

/* The bytes a key sends: into out, returning how many (at most 4). */
static int key_bytes(u8 *out, int ev)
{
    static const struct {
        u8 raw;
        char seq[5];
    } special[] = {
        {0x3E, "\x1b[A"}, {0x3D, "\x1b[B"}, {0x3C, "\x1b[C"}, {0x3B, "\x1b[D"},
        {0x73, "\x1b[H"}, {0x77, "\x1b[F"}, {0x74, "\x1b[5~"}, {0x79, "\x1b[6~"},
        {0x75, "\x1b[3~"},
    };
    u8 raw = (u8)((ev >> 8) & 0x7F), c = (u8)(ev & 0xFF);
    u32 cp;
    int i, n = 0;

    /* The SE's own keyboard has no Page Up or Page Down, Home or End:
     * Command with an arrow stands in for them. */
    if (kbd_modifiers() & KBD_COMMAND)
        raw = raw == 0x3E ? 0x74 : raw == 0x3D ? 0x79 : raw == 0x3B ? 0x73 : raw == 0x3C ? 0x77 : raw;
    for (i = 0; i < (int)(sizeof special / sizeof special[0]); i++)
        if (special[i].raw == raw) {
            n = (int)strlen(special[i].seq);
            memcpy(out, special[i].seq, (u32)n);
            /* Full-screen programs ask for the arrows, Home and End as Esc O x. */
            if (n == 3 && vt_app_keys())
                out[1] = 'O';
            return n;
        }
    if (!c)
        return 0;
    if (ev & KBD_EV_META)
        out[n++] = 0x1B;
    if (c == '\n') {
        out[n++] = '\r';
    } else if (c == '\b') {
        out[n++] = 0x7F;
    } else if (c < 0x80) {
        out[n++] = c;
    } else {
        cp = c >= 0xA0 ? c : charset_extra[c - 0x80];
        if (cp >= 0x800) {
            out[n++] = (u8)(0xE0 | (cp >> 12));
            out[n++] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        } else {
            out[n++] = (u8)(0xC0 | (cp >> 6));
        }
        out[n++] = (u8)(0x80 | (cp & 0x3F));
    }
    return n;
}

/* The shell: show what arrives, send what is typed. Returns when either
 * side ends it. */
static int session(const char *host)
{
    u8 keys[120];
    int r = 0, ev, n, quit = 0, busy_now;
    u32 shown = ticks, wait = SHOW_EVERY, streak = 0;

    /* Text is drawn when no more is waiting, or SHOW_EVERY ticks after the
     * last time: what a long listing sends between two looks scrolls by
     * without being drawn at all. */
    /* Scrolling goes a line at a time, each one drawn, which is what looks
     * smooth. Only when packets come one on the heels of another, with
     * never a moment to spare, are the steps made bigger to keep up. */
    vt_hold = 1;
    vt_step = 1;
    while (!quit && !ssh.chan_closed) {
        busy_now = 0;
        net_poll();
        r = packet_get(0);
        if (r < 0)
            break;
        if (r > 0) {
            busy_now = 1;
            if (streak < 255)
                streak++;
            vt_step = streak < 6 ? 1 : streak < 12 ? 2 : streak < 24 ? 4 : 0;
            r = handle(r, host);
            if (r)
                break;
            if (ticks - shown >= wait) {
                shown = ticks;
                vt_show();
                wait = (ticks - shown) * 3 / 2;         /* a third of the time drawing, no more */
                if (wait < SHOW_EVERY)
                    wait = SHOW_EVERY;
                shown = ticks;
            }
        } else {
            vt_show();
            shown = ticks;
            wait = SHOW_EVERY;
            streak = 0;
        }

        /* Keys. Command-Q or Control-] leaves. */
        n = 0;
        while (n < (int)sizeof keys - 8 && (ev = kbd_get_event()) >= 0) {
            if (ev & 0x8000)
                continue;
            if (((kbd_modifiers() & KBD_COMMAND) && ((ev & 0xFF) == 'q' || (ev & 0xFF) == 'Q')) ||
                (ev & 0xFF) == 0x1D) {
                quit = 1;
                break;
            }
            n += key_bytes(keys + n, ev);
        }
        n += vt_reply(keys + n, (int)sizeof keys - n);  /* answers to what programs asked */
        if (n && (u32)n <= ssh.remote_window && !ssh.chan_closed) {
            busy_now = 1;
            put_start(MSG_CHANNEL_DATA);
            put_u32(ssh.remote_chan);
            put_string(keys, (u32)n);
            ssh.remote_window -= (u32)n;
            r = packet_send();
            if (r)
                break;
        }
        bar_tick();
        n = bar_poll();
        if (n == BAR_HOME || n == BAR_CONSOLE)
            quit = 1;           /* the SSH OS menu: end the session */
        if (!busy_now) {
            vt_cursor(1);
            bar_idle();
        } else {
            bar_work();
        }
    }
    vt_hold = 0;
    vt_step = 0;
    vt_show();
    vt_cursor(0);
    if (r < 0)
        return r;
    if (quit)
        ssh.why = "closed from the keyboard";
    return 0;
}

/* user@host:port, any part but the host optional. */
static int parse_target(const char *s, char *user, int user_max, char *host, int host_max,
                        u16 *port)
{
    const char *at = NULL, *p;
    u32 v = 0;
    int n = 0;

    for (p = s; *p; p++)
        if (*p == '@')
            at = p;
    user[0] = 0;
    if (at) {
        if (at - s >= user_max || at == s)
            return -1;
        memcpy(user, s, (u32)(at - s));
        user[at - s] = 0;
        s = at + 1;
    }
    while (*s && *s != ':') {
        if (n >= host_max - 1)
            return -1;
        host[n++] = *s++;
    }
    host[n] = 0;
    *port = 22;
    if (*s == ':') {
        for (s++; *s >= '0' && *s <= '9'; s++)
            v = v * 10 + (u32)(*s - '0');
        if (*s || v == 0 || v > 65535)
            return -1;
        *port = (u16)v;
    }
    return n ? 0 : -1;
}

/* Keep a connection that worked, for the home screen's list. */
static void remember_target(const char *user, const char *host, u16 port)
{
    char t[40 + 64 + 8];
    u32 n = strlen(user), l = strlen(host), div;

    memcpy(t, user, n);
    t[n++] = '@';
    memcpy(t + n, host, l);
    n += l;
    if (port != 22) {
        t[n++] = ':';
        for (div = 10000; div > 1 && port / div == 0; div /= 10)
            ;
        for (; div; div /= 10)
            t[n++] = (char)('0' + port / div % 10);
    }
    t[n] = 0;
    recent_add(t);
}

void (*ssh_login_hook)(void);
void (*ssh_end_hook)(void);
u8 ssh_ended_well;

void cmd_ssh(int argc, char **argv)
{
    char user[40], host[64];
    const char *target = argc > 1 ? argv[1] : config_get("ssh"), *setting;
    int (*abort_check)(void) = net_abort_check;
    u32 ip, started;
    u16 port;
    int r;

    if (!target || parse_target(target, user, sizeof user, host, sizeof host, &port) != 0 ||
        !user[0]) {
        kputs("Usage: ssh USER@HOST or ssh USER@HOST:PORT. Without arguments it uses the\n"
              "setting \"ssh\". Esc stops while connecting; Command-Q ends a session.\n");
        return;
    }
    if (shell_need_network() != 0)
        return;
    r = net_resolve(host, &ip);
    if (r) {
        shell_fail(r);
        return;
    }

    ssh_ended_well = 0;
    memset(&ssh, 0, sizeof ssh);
    ssh.first_kex = 1;
    setting = config_get("termlog");
    ssh.log = setting && setting[0] == '1';
    crypto_idle = busy;
    crypto_stop = 0;
    started = ticks;

    kputs(copro_present() ? "Making a one-time key"
                          : "Making a one-time key (each of these steps takes this machine a while)");
    make_pair();
    if (crypto_stop) {
        crypto_idle = NULL;
        crypto_stop = 0;
        kbd_escape = 0;
        kputs("\nStopped.\n");
        return;
    }
    kprintf("\nConnecting to %s port %d...\n", host, port);
    ssh.c = tcp_connect(ip, port, 10 * 60);
    if (ssh.c < 0) {
        crypto_idle = NULL;
        shell_fail(ssh.c);
        return;
    }
    r = key_exchange(host);
    if (r == 0)
        r = authenticate(user, host);
    if (r == 0)
        r = offer_key(user, host);
    if (r == 0)
        r = open_shell(host);
    if (r == 0) {
        kprintf("Logged in after %lu seconds.\n", (unsigned long)((ticks - started) / 60));
        remember_target(user, host, port);
        if (ssh_login_hook)
            ssh_login_hook();
        /* Esc belongs to the other side now, not to our waiting calls. */
        net_abort_check = NULL;
        setting = config_get("option");         /* "meta": Option never types accents here */
        kbd_option_meta = setting && strcmp(setting, "meta") == 0;
        vt_reset();
        vt_log = ssh.log;
        r = session(host);
        vt_log = 0;
        vt_end();
        kbd_option_meta = 0;
        net_abort_check = abort_check;
    }
    kbd_escape = 0;
    crypto_idle = NULL;
    crypto_stop = 0;

    if (r == 0 && ssh.c >= 0) {         /* a polite goodbye */
        put_start(MSG_DISCONNECT);
        put_u32(11);
        put_text("");
        put_text("");
        packet_send();
        tcp_close(ssh.c);
    } else {
        tcp_abort(ssh.c);
    }
    if (ssh_end_hook)
        ssh_end_hook();
    ssh_ended_well = r == 0 && !ssh.why && !(ssh.have_exit && ssh.exit_status);
    if (ssh.have_exit && ssh.exit_status)
        kprintf("\n[The shell ended with status %lu.]\n", (unsigned long)ssh.exit_status);
    kprintf("\n[Connection to %s ended%s%s.]\n", host, ssh.why ? ": " : "", ssh.why ? ssh.why : "");
    memset(&ssh, 0, sizeof ssh);
}
