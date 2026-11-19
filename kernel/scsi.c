/* Polled driver for the SE's NCR 5380 SCSI chip.
 *
 * No interrupts; the SCSI interrupt stays masked at the VIA (via.c). There
 * are two ways of moving bytes:
 *
 *   - By hand: the REQ/ACK handshake of every byte is done with register
 *     writes. Slow (about 16 KB per second) but with nothing left to chance.
 *     Used for command, status and message bytes and for data going out.
 *   - "Pseudo-DMA" for data coming in: the chip does the handshake itself
 *     and raises DRQ for each byte it has; we poll DRQ and fetch the byte
 *     through a second address. Several times faster. We never read without
 *     seeing DRQ first, so this too makes no assumption about how fast the
 *     device is. scsi_fast_reads = 0 turns it off, to compare on a real
 *     machine if a device misbehaves.
 *
 * The Mac is always device 7. A command runs start to finish: get the bus,
 * select the device, then do whatever the device asks for (it decides the
 * order: command bytes, data, status, a closing message) until it lets go
 * of the bus. We select without ATN, as the Mac's ROM does, so a device is
 * never allowed to disconnect half-way and come back later.
 *
 * The sequence follows the 5380 data sheet and Linux's NCR5380.c.
 */
#include "kernel.h"
#include "hw.h"

/* Registers. The same number means different registers for read and write. */
#define R_CDR   0       /* read: the data bus as it is now */
#define R_ICR   1       /* read/write: initiator command */
#define R_MR    2       /* read/write: mode */
#define R_TCR   3       /* read/write: target command (the phase we expect) */
#define R_CSR   4       /* read: bus control lines */
#define R_BSR   5       /* read: bus and status */
#define R_RPI   7       /* read: clears the chip's interrupt and parity flags */
#define W_ODR   0       /* write: byte to put on the data bus */
#define W_SER   4       /* write: select enable (IDs that may interrupt us) */
#define W_SDIR  7       /* write: start a DMA receive (any value) */

/* Reading here fetches the chip's latched byte and acknowledges it: the
 * address line that selects this range is wired to the chip's DACK pin. */
#define DMA_READ (*(volatile u8 *)(SCSI_BASE + 0x260UL))

#define ICR_RST     0x80
#define ICR_AIP     0x40        /* read: arbitration in progress */
#define ICR_LOST    0x20        /* read: lost arbitration */
#define ICR_ACK     0x10
#define ICR_BSY     0x08
#define ICR_SEL     0x04
#define ICR_DATA    0x01        /* drive the data bus from the output register */

#define MR_ARBITRATE 0x01
#define MR_DMA       0x02

#define BSR_DRQ         0x40    /* the chip holds a byte for us (DMA mode) */
#define BSR_PHASE_MATCH 0x08    /* the bus is in the phase named in the TCR */

#define CSR_BSY     0x40
#define CSR_REQ     0x20
#define CSR_SEL     0x02
#define PHASE_OF(csr) (((csr) >> 2) & 7)    /* MSG, C/D, I/O */

/* Bus phases, as MSG:C/D:I/O. Written to the TCR as they are. */
#define PH_DATA_OUT 0
#define PH_DATA_IN  1
#define PH_COMMAND  2
#define PH_STATUS   3
#define PH_MSG_OUT  6
#define PH_MSG_IN   7
#define PH_IO       1           /* set in phases where the device sends */

#define OWN_ID_MASK 0x80        /* the Mac is device 7 */

#define MSG_COMPLETE    0x00
#define MSG_EXTENDED    0x01
#define MSG_DISCONNECT  0x04
#define MSG_NOP         0x08

#define ST_GOOD     0x00
#define ST_CHECK    0x02
#define ST_BUSY     0x08

#define SELECT_TICKS    16      /* 250 ms: the standard selection timeout */
#define BUS_TICKS       30      /* wait for a free bus */
#define REQ_TICKS       600     /* 10 s: longest a device may think */
#define DISCARD_LIMIT   0x100000UL

struct scsi_sense scsi_sense;
u32 scsi_bus_resets;
u8 scsi_fast_reads = 1;

#define RD(reg) (*(volatile u8 *)(SCSI_BASE + (reg) * 0x10UL))

/* Writes go through an explicit move.b. Left to itself the compiler may
 * store a zero with clr.b, which on the 68000 reads the address first, and
 * a read cycle at a write address is not something to try on this chip. */
#define WR(reg, val) \
    __asm__ volatile("move.b %1,%0" \
                     : "=m" (*(volatile u8 *)(SCSI_BASE + (reg) * 0x10UL + 1)) \
                     : "di" ((u8)(val)))

/* A short pause for the bus to settle. The SCSI delays needed here are 2.4
 * microseconds at most; one register read takes about 2 on the SE. */
static void settle(void)
{
    (void)RD(R_CSR);
    (void)RD(R_CSR);
    (void)RD(R_CSR);
    (void)RD(R_CSR);
}

void scsi_reset_bus(void)
{
    WR(R_ICR, ICR_RST);
    delay_ticks(1);             /* far longer than the 25 microseconds required */
    WR(R_ICR, 0);
    WR(R_MR, 0);
    WR(R_TCR, 0);
    (void)RD(R_RPI);
    delay_ticks(30);            /* devices need time before they answer again */
    scsi_bus_resets++;
}

/* Call with interrupts enabled, after timer_calibrate(). */
void scsi_init(void)
{
    WR(R_ICR, 0);
    WR(R_MR, 0);
    WR(R_TCR, 0);
    WR(W_SER, 0);
    (void)RD(R_RPI);
    if (RD(R_CSR) & (CSR_BSY | CSR_SEL))
        scsi_reset_bus();       /* something was left holding the bus */
}

/* Take the bus and select a device. 0, or a negative error. */
static int select_target(u8 id)
{
    struct timeout t;

    WR(R_TCR, 0);
    WR(R_MR, 0);
    WR(R_ICR, 0);

    timeout_start(&t, BUS_TICKS);
    while (RD(R_CSR) & (CSR_BSY | CSR_SEL))
        if (timeout_expired(&t))
            return SCSI_ERR_BUS_BUSY;

    /* Arbitration: the chip waits for a free bus and puts our ID on it. */
    WR(W_ODR, OWN_ID_MASK);
    WR(R_MR, MR_ARBITRATE);
    timeout_start(&t, BUS_TICKS);
    while (!(RD(R_ICR) & ICR_AIP))
        if (timeout_expired(&t)) {
            WR(R_MR, 0);
            return SCSI_ERR_BUS_BUSY;
        }
    settle();
    if (RD(R_ICR) & ICR_LOST) { /* cannot happen to ID 7, the highest */
        WR(R_MR, 0);
        return SCSI_ERR_BUS_BUSY;
    }

    /* Selection: SEL with both IDs on the data bus, then let go of BSY and
     * wait for the device to take it. */
    WR(R_ICR, ICR_SEL | ICR_BSY);
    settle();
    WR(W_ODR, OWN_ID_MASK | (u8)(1 << id));
    WR(R_ICR, ICR_SEL | ICR_BSY | ICR_DATA);
    WR(R_MR, 0);
    WR(W_SER, 0);
    settle();
    WR(R_ICR, ICR_SEL | ICR_DATA);
    settle();

    timeout_start(&t, SELECT_TICKS);
    while (!(RD(R_CSR) & CSR_BSY))
        if (timeout_expired(&t)) {
            WR(R_ICR, 0);
            return SCSI_ERR_NO_DEVICE;
        }
    settle();
    WR(R_ICR, 0);               /* release SEL and the data bus */
    return 0;
}

/* Wait until the device asks for the next byte. Returns the bus phase, or a
 * negative error. */
static int wait_req(void)
{
    struct timeout t;
    u16 quick = 2000;
    u8 csr;

    /* Nearly always the device is ready at once; only start a timeout,
     * which costs a function call per turn, when it is not. */
    do {
        csr = RD(R_CSR);
        if (csr & CSR_REQ)
            goto ready;
        if (!(csr & CSR_BSY) && !(RD(R_CSR) & CSR_BSY))
            return SCSI_ERR_BUS_FREE;
    } while (--quick);

    timeout_start(&t, REQ_TICKS);
    for (;;) {
        csr = RD(R_CSR);
        if (csr & CSR_REQ)
            break;
        if (!(csr & CSR_BSY) && !(RD(R_CSR) & CSR_BSY))
            return SCSI_ERR_BUS_FREE;
        if (timeout_expired(&t))
            return SCSI_ERR_TIMEOUT;
    }
ready:
    /* The phase lines are stable once REQ is up; read them again rather
     * than trust a value sampled while they might have been changing. */
    return PHASE_OF(RD(R_CSR));
}

/* Wait for the device to drop REQ after our ACK. */
static int wait_req_low(void)
{
    struct timeout t;
    u16 quick = 2000;

    do {
        if (!(RD(R_CSR) & CSR_REQ))
            return 0;
    } while (--quick);

    timeout_start(&t, REQ_TICKS);
    while (RD(R_CSR) & CSR_REQ)
        if (timeout_expired(&t))
            return SCSI_ERR_TIMEOUT;
    return 0;
}

/* Read up to count data bytes with the chip doing the handshake. Stops
 * early when the device leaves the data phase. Returns the number read or a
 * negative error.
 *
 * In this mode the chip accepts each byte as soon as the device offers it,
 * so when we stop while the device still has more to send, one further byte
 * has already been taken and is lost. Callers only stop early when they are
 * about to throw the rest away anyway.
 *
 * Not inlined: on its own the compiler can keep the whole loop in registers. */
static __attribute__((noinline)) s32 dma_read(u8 *buf, u32 count)
{
    struct timeout t;
    /* The two addresses are kept in registers, which makes the loop for
     * each byte about half as long as with the addresses spelled out. */
    volatile u8 *bsr_reg, *dma_reg;
    u8 *p = buf, *end = buf + count;
    s32 result = 0;
    u16 quick;
    u8 bsr;

    __asm__("" : "=a" (bsr_reg) : "0" (&RD(R_BSR)));
    __asm__("" : "=a" (dma_reg) : "0" (&DMA_READ));

    WR(R_TCR, PH_DATA_IN);
    WR(R_MR, MR_DMA);
    WR(W_SDIR, 0);
    while (p < end) {
        if (*bsr_reg & BSR_DRQ) {
            *p++ = *dma_reg;
            continue;
        }
        /* No byte yet. Either the device is slow or the data has ended. */
        quick = 2000;
        timeout_start(&t, REQ_TICKS);
        for (;;) {
            bsr = RD(R_BSR);
            if (bsr & BSR_DRQ)
                break;
            if (!(bsr & BSR_PHASE_MATCH)) {
                /* Phase changed. A phase never changes back, so this is
                 * final, but look once more in case the lines were read
                 * while they were moving. */
                bsr = RD(R_BSR);
                if (bsr & BSR_DRQ)
                    break;
                if (!(bsr & BSR_PHASE_MATCH))
                    goto out;
            }
            if (!(RD(R_CSR) & CSR_BSY) && !(RD(R_CSR) & CSR_BSY)) {
                result = SCSI_ERR_BUS_FREE;
                goto out;
            }
            if (quick)
                quick--;
            else if (timeout_expired(&t)) {
                result = SCSI_ERR_TIMEOUT;
                goto out;
            }
        }
    }
out:
    WR(R_MR, 0);                /* back to doing the handshake by hand */
    (void)RD(R_RPI);            /* clear the "phase changed" flag the chip raised */
    return result < 0 ? result : (s32)(p - buf);
}

/* Move up to count bytes in one phase, one handshake per byte. Stops early
 * when the device changes phase. With buf NULL, incoming bytes are dropped
 * and outgoing ones are zeros. Returns the number moved or a negative error. */
static s32 transfer(u8 phase, u8 *buf, u32 count)
{
    u32 n = 0;
    int ph;
    u8 b;

    if (phase == PH_DATA_IN && buf && scsi_fast_reads)
        return dma_read(buf, count);

    WR(R_TCR, phase);           /* the chip only drives the bus in the phase named here */
    while (n < count) {
        ph = wait_req();
        if (ph < 0)
            return ph;
        if (ph != phase)
            break;
        if (phase & PH_IO) {
            b = RD(R_CDR);
            if (buf)
                buf[n] = b;
            WR(R_ICR, ICR_ACK);
        } else {
            b = buf ? buf[n] : 0;
            WR(W_ODR, b);
            WR(R_ICR, ICR_DATA);
            WR(R_ICR, ICR_DATA | ICR_ACK);
        }
        ph = wait_req_low();
        WR(R_ICR, 0);
        if (ph < 0)
            return ph;
        n++;
    }
    return (s32)n;
}

int scsi_command(u8 id, const u8 *cdb, u8 cdb_len, u8 dir, void *buf, u32 len, u32 *moved)
{
    struct timeout t;
    u32 done = 0;
    u8 cdb_sent = 0, have_status = 0, status = 0, msg;
    int ph, err;
    s32 n;

    if (moved)
        *moved = 0;
    if (id > 6)
        return SCSI_ERR_NO_DEVICE;
    err = select_target(id);
    if (err)
        return err;

    for (;;) {
        ph = wait_req();
        if (ph == SCSI_ERR_BUS_FREE && have_status)
            goto finished;      /* left without the closing message; accept it */
        if (ph < 0) {
            err = ph;
            goto fail;
        }
        switch (ph) {
        case PH_COMMAND:
            if (cdb_sent) {     /* it wants more command bytes than we have */
                err = SCSI_ERR_PHASE;
                goto fail;
            }
            n = transfer(PH_COMMAND, (u8 *)cdb, cdb_len);
            cdb_sent = 1;
            break;

        case PH_DATA_IN:
        case PH_DATA_OUT:
            if (dir == (ph == PH_DATA_IN ? SCSI_DATA_IN : SCSI_DATA_OUT) && done < len) {
                n = transfer((u8)ph, (u8 *)buf + done, len - done);
                if (n > 0)
                    done += (u32)n;
            } else {
                /* More than the caller has room for, or the wrong way
                 * round: keep the handshake going so the command can end. */
                n = transfer((u8)ph, NULL, DISCARD_LIMIT);
                if (n == (s32)DISCARD_LIMIT)
                    n = SCSI_ERR_PHASE;
            }
            break;

        case PH_STATUS:
            n = transfer(PH_STATUS, &status, 1);
            have_status = 1;
            break;

        case PH_MSG_IN:
            n = transfer(PH_MSG_IN, &msg, 1);
            if (n < 1)
                break;
            if (msg == MSG_COMPLETE)
                goto finished;
            if (msg == MSG_DISCONNECT) {
                err = SCSI_ERR_PHASE;       /* we never allowed that */
                goto fail;
            }
            if (msg == MSG_EXTENDED) {      /* length byte, then that many more */
                n = transfer(PH_MSG_IN, &msg, 1);
                if (n == 1)
                    n = transfer(PH_MSG_IN, NULL, msg ? msg : 256);
            }
            break;                          /* anything else needs no action */

        case PH_MSG_OUT:
            msg = MSG_NOP;
            n = transfer(PH_MSG_OUT, &msg, 1);
            break;

        default:
            n = SCSI_ERR_PHASE;
            break;
        }
        if (n < 0) {
            err = (int)n;
            goto fail;
        }
    }

finished:
    WR(R_ICR, 0);
    timeout_start(&t, 2);       /* the device now lets go of the bus */
    while ((RD(R_CSR) & CSR_BSY) && !timeout_expired(&t))
        ;
    if (moved)
        *moved = done;
    return have_status ? status : SCSI_ERR_PHASE;

fail:
    WR(R_ICR, 0);
    if (RD(R_CSR) & CSR_BSY)
        scsi_reset_bus();       /* the device is stuck mid-command */
    if (moved)
        *moved = done;
    return err;
}

/* Ask the device why the last command failed. */
static void request_sense(u8 id)
{
    static const u8 cdb[6] = { 0x03, 0, 0, 0, 14, 0 };
    u8 data[14];
    u32 got = 0;

    scsi_sense.key = scsi_sense.asc = scsi_sense.ascq = 0xFF;
    if (scsi_command(id, cdb, 6, SCSI_DATA_IN, data, sizeof data, &got) != ST_GOOD || got < 3)
        return;
    scsi_sense.key = data[2] & 0x0F;
    scsi_sense.asc = got > 12 ? data[12] : 0;
    scsi_sense.ascq = got > 13 ? data[13] : 0;
}

int scsi_exec(u8 id, const u8 *cdb, u8 cdb_len, u8 dir, void *buf, u32 len, u32 *moved)
{
    int tries, st = SCSI_ERR_STATUS;

    for (tries = 0; tries < 4; tries++) {
        st = scsi_command(id, cdb, cdb_len, dir, buf, len, moved);
        if (st == ST_GOOD)
            return 0;
        if (st < 0)
            return st;
        if (st == ST_CHECK) {
            request_sense(id);
            if (scsi_sense.key == 6)    /* "unit attention": it was reset; just ask again */
                continue;
            return SCSI_ERR_CHECK;
        }
        if (st == ST_BUSY) {
            delay_ticks(6);
            continue;
        }
        return SCSI_ERR_STATUS;
    }
    return st == ST_CHECK ? SCSI_ERR_CHECK : SCSI_ERR_STATUS;
}

int scsi_inquiry(u8 id, u8 *buf36)
{
    static const u8 cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    u32 got = 0;
    int err;

    memset(buf36, 0, 36);
    err = scsi_exec(id, cdb, 6, SCSI_DATA_IN, buf36, 36, &got);
    if (err == 0 && got < 5)
        err = SCSI_ERR_PHASE;
    return err;
}

int scsi_capacity(u8 id, u32 *blocks, u32 *block_size)
{
    static const u8 cdb[10] = { 0x25 };
    u8 d[8];
    u32 got = 0;
    int err;

    err = scsi_exec(id, cdb, 10, SCSI_DATA_IN, d, 8, &got);
    if (err)
        return err;
    if (got < 8)
        return SCSI_ERR_PHASE;
    /* The device reports the number of its last block. */
    *blocks = (((u32)d[0] << 24) | ((u32)d[1] << 16) | ((u32)d[2] << 8) | d[3]) + 1;
    *block_size = ((u32)d[4] << 24) | ((u32)d[5] << 16) | ((u32)d[6] << 8) | d[7];
    return 0;
}

static int read_write(u8 opcode, u8 dir, u8 id, u32 block, u16 count, void *buf)
{
    u8 cdb[10];
    u32 len = (u32)count * 512, got = 0;
    int err;

    cdb[0] = opcode;
    cdb[1] = 0;
    cdb[2] = (u8)(block >> 24);
    cdb[3] = (u8)(block >> 16);
    cdb[4] = (u8)(block >> 8);
    cdb[5] = (u8)block;
    cdb[6] = 0;
    cdb[7] = (u8)(count >> 8);
    cdb[8] = (u8)count;
    cdb[9] = 0;
    err = scsi_exec(id, cdb, 10, dir, buf, len, &got);
    if (err == 0 && got != len)
        err = SCSI_ERR_PHASE;
    return err;
}

int scsi_read(u8 id, u32 block, u16 count, void *buf)
{
    return read_write(0x28, SCSI_DATA_IN, id, block, count, buf);
}

int scsi_write(u8 id, u32 block, u16 count, const void *buf)
{
    return read_write(0x2A, SCSI_DATA_OUT, id, block, count, (void *)buf);
}

const char *scsi_strerror(int err)
{
    switch (err) {
    case 0:                     return "no error";
    case SCSI_ERR_NO_DEVICE:    return "no device at that ID";
    case SCSI_ERR_BUS_BUSY:     return "SCSI bus is busy";
    case SCSI_ERR_TIMEOUT:      return "device stopped answering";
    case SCSI_ERR_BUS_FREE:     return "device dropped off the bus";
    case SCSI_ERR_PHASE:        return "unexpected answer from the device";
    case SCSI_ERR_CHECK:        return "device reported an error";
    case SCSI_ERR_STATUS:       return "device busy or returned a bad status";
    }
    return "unknown error";
}
