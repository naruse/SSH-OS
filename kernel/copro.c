/* Handing the slow sums of SSH to the BlueSCSI.
 *
 * A BlueSCSI whose firmware has SSH OS's addition (firmware/sshos_crypto.c)
 * does in thousandths of a second what takes the 68000 a quarter of a
 * minute: the two key agreements and the two signature sums of a log-in.
 * Each function here asks it and returns 0 with the answer, or -1 if there
 * is no such BlueSCSI or it did not answer; the caller then does the sum
 * itself, as SSH OS always did. So one SSH OS runs with either firmware.
 *
 * The request goes out in one SCSI command and the answer comes back in a
 * second (see the list in firmware/sshos_crypto.c).
 */
#include "kernel.h"
#define SSHOS_KERNEL            /* the arithmetic is declared by kernel.h here */
#include "../firmware/sshos_crypto.h"

static u8 buf[SSHOS_CRYPTO_MAX];
static s8 there;                /* 0 = not asked yet, -1 = it is not there, else its version */

static void note(const char *s)
{
    serial_puts("COPRO: ");
    serial_puts(s);
    serial_puts("\n");
}

#ifdef COPRO_FAKE
/* Built with -DCOPRO_FAKE (make test-copro), the firmware's side is linked
 * into SSH OS and answers in the BlueSCSI's place: the same code, the same
 * bytes each way, without the cable. Nothing is quicker for it, of course. */
#include "../firmware/sshos_crypto.c"

static int command(u8 request, u8 dir, u32 len, u32 *moved)
{
    (void)dir;
    if (request == SSHOS_HELLO)
        *moved = sshos_crypto_hello(buf);
    else if (request == SSHOS_RESULT)
        *moved = sshos_crypto_result(buf, len);
    else {
        sshos_crypto_job(request, buf, len);
        *moved = len;
    }
    return 0;
}
#else
static int command(u8 request, u8 dir, u32 len, u32 *moved)
{
    u8 cdb[6] = {SSHOS_CRYPTO_CMD, request, 0, (u8)(len >> 8), (u8)len, 0};
    int id = eth_adapter_id();

    if (id < 0)
        return -1;
    return scsi_exec((u8)id, cdb, 6, dir, buf, len, moved) == 0 ? 0 : -1;
}
#endif

/* Is there a BlueSCSI that does sums? Asked once. */
int copro_present(void)
{
    u32 moved = 0;

    if (!there) {
        memset(buf, 0, 8);
        there = (command(SSHOS_HELLO, SCSI_DATA_IN, 8, &moved) == 0 && moved == 8 &&
                 memcmp(buf, "SSHOS", 5) == 0 && buf[5] >= 1 && buf[5] < 100) ? (s8)buf[5] : -1;
        note(there > 0 ? "the BlueSCSI does the sums" : "none: the 68000 does the sums");
    }
    return there > 0;
}

/* Send what is in buf (len bytes) as that request and fetch the answer:
 * want bytes after the one that says it was done. They are left at buf + 1.
 * The caller has asked copro_present() before filling buf: asking uses buf. */
static int ask(u8 request, u32 len, u32 want)
{
    u32 moved = 0;

    if (command(request, SCSI_DATA_OUT, len, &moved) != 0 || moved != len)
        return -1;
    memset(buf, 0, 1 + want);
    if (command(SSHOS_RESULT, SCSI_DATA_IN, 1 + want, &moved) != 0 || moved != 1 + want || buf[0] != 1)
        return -1;
    if (request < SSHOS_OPEN)
        note("a sum done by the BlueSCSI");
    return 0;
}

static void wipe(void)
{
    memset(buf, 0, sizeof buf);
}

int copro_x25519(u8 *out32, const u8 *scalar32, const u8 *point32)
{
    int r;

    if (!copro_present())
        return -1;
    memcpy(buf, scalar32, 32);
    memcpy(buf + 32, point32, 32);
    r = ask(SSHOS_X25519, 64, 32);
    if (r == 0)
        memcpy(out32, buf + 1, 32);
    wipe();
    return r;
}

int copro_ed25519_public(u8 *pub32, const u8 *seed32)
{
    int r;

    if (!copro_present())
        return -1;
    memcpy(buf, seed32, 32);
    r = ask(SSHOS_PUBLIC, 32, 32);
    if (r == 0)
        memcpy(pub32, buf + 1, 32);
    wipe();
    return r;
}

int copro_ed25519_sign(u8 *sig64, const u8 *msg, u32 len, const u8 *seed32, const u8 *pub32)
{
    int r;

    if (!copro_present())
        return -1;
    if (len > sizeof buf - 64)
        return -1;
    memcpy(buf, seed32, 32);
    memcpy(buf + 32, pub32, 32);
    memcpy(buf + 64, msg, len);
    r = ask(SSHOS_SIGN, 64 + len, 64);
    if (r == 0)
        memcpy(sig64, buf + 1, 64);
    wipe();
    return r;
}

/* 1 = genuine, 0 = not, -1 = could not ask. */
int copro_ed25519_verify(const u8 *sig64, const u8 *msg, u32 len, const u8 *pub32)
{
    int r;

    if (!copro_present())
        return -1;
    if (len > sizeof buf - 96)
        return -1;
    memcpy(buf, sig64, 64);
    memcpy(buf + 64, pub32, 32);
    memcpy(buf + 96, msg, len);
    r = ask(SSHOS_VERIFY, 96 + len, 1);
    if (r == 0)
        r = buf[1] == 1;
    wipe();
    return r;
}

/* ---------------------------------------------------------------- packets -- */

/* The packets of a session, once logged in. Short ones are not worth the
 * two trips down the cable: the 68000 is done with a key press before the
 * BlueSCSI has been asked. (With the stand-in everything goes this way, to
 * try it.) */
#ifdef COPRO_FAKE
#define PACKET_MIN  0
#else
#define PACKET_MIN  256
#endif

static u8 packets_noted;

static int packets_here(u32 bytes)
{
    return bytes >= PACKET_MIN && bytes + SSHOS_PACKET_HEAD + 20 <= sizeof buf &&
           copro_present() && there >= 2;
}

static void packet_head(u32 seq, const u8 *key64)
{
    buf[0] = (u8)(seq >> 24);
    buf[1] = (u8)(seq >> 16);
    buf[2] = (u8)(seq >> 8);
    buf[3] = (u8)seq;
    memcpy(buf + 4, key64, 64);
}

/* A packet as it arrived: p is its length field (4 bytes, still encrypted),
 * then len bytes of body, then the check sum. 1 = genuine, and the body is
 * now in clear; 0 = damaged or forged; -1 = not asked, do it here. */
int copro_open(u32 seq, const u8 *key64, u8 *p, u32 len)
{
    int r;

    if (!packets_here(len))
        return -1;
    packet_head(seq, key64);
    memcpy(buf + SSHOS_PACKET_HEAD, p, 4 + len + 16);
    r = ask(SSHOS_OPEN, SSHOS_PACKET_HEAD + 4 + len + 16, 1 + len);
    if (r != 0) {
        /* refused, or a forgery: then only two bytes came back */
        r = (buf[0] == 1 && buf[1] == 0) ? 0 : -1;
    } else {
        memcpy(p + 4, buf + 2, len);
        r = 1;
        if (!packets_noted) {
            packets_noted = 1;
            note("packets go through the BlueSCSI");
        }
    }
    wipe();
    return r;
}

/* A packet to send: p is its length field and total bytes of body, in
 * clear, with room for the check sum after. 0 = it is encrypted and has
 * its check sum; -1 = not asked, do it here. */
int copro_seal(u32 seq, const u8 *key64, u8 *p, u32 total)
{
    int r;

    if (!packets_here(total))
        return -1;
    packet_head(seq, key64);
    memcpy(buf + SSHOS_PACKET_HEAD, p, 4 + total);
    r = ask(SSHOS_SEAL, SSHOS_PACKET_HEAD + 4 + total, 4 + total + 16);
    if (r == 0)
        memcpy(p, buf + 1, 4 + total + 16);
    wipe();
    return r;
}
