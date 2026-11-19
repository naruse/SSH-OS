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
static s8 there;                /* 0 = not asked yet, 1 = it is there, -1 = it is not */

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
                 memcmp(buf, "SSHOS", 5) == 0 && buf[5] >= 1) ? 1 : -1;
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
