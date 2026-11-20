/* Apple Desktop Bus (keyboard and mouse) through the SE's ADB transceiver.
 *
 * The transceiver is a small microcontroller that does the bus timing. The
 * CPU talks to it with:
 *   - two state lines on VIA port B (ST1:ST0): 0 = command byte, 1 = even
 *     data byte, 2 = odd data byte, 3 = idle;
 *   - the VIA shift register, one byte at a time, with an interrupt per byte;
 *   - a flag line (port B bit 3, active low) read together with each byte.
 *     On the first data byte it means "no reply", on the second "another
 *     device wants service", on any later byte "end of data".
 *
 * While idle, the transceiver keeps repeating the last Talk command about
 * every 11 ms and reports on its own when the device has data. So normal
 * operation is: send "Talk register 0" to the keyboard once, then just take
 * what arrives; when the mouse asks for service, switch the Talk to it.
 *
 * The state machine follows Linux's drivers/macintosh/via-macii.c, which
 * drives the same chip on the Mac II family and the SE.
 */
#include "kernel.h"
#include "hw.h"

#define ST_MASK     (PB_ADB_ST1 | PB_ADB_ST0)
#define ST_CMD      0x00
#define ST_EVEN     PB_ADB_ST0
#define ST_ODD      PB_ADB_ST1
#define ST_IDLE     ST_MASK

#define ADB_KEYBOARD 2          /* default bus addresses */
#define ADB_MOUSE    3
#define TALK_R0(addr) (u8)(((addr) << 4) | 0x0C)
#define IS_TALK(cmd)  (((cmd) & 0x0C) == 0x0C)

volatile u32 adb_packets, adb_timeouts, adb_restarts;
static u8 quiet;                /* ticks since the transceiver last had anything to say */

static enum { IDLE, SENDING, READING } state;
static u8 reply[9];             /* command byte + up to 8 data bytes */
static u8 reply_len;
static u8 bus_timeout, srq_asserted;
static u8 sending_cmd;           /* command byte now being shifted out */
static u8 last_cmd;              /* last command the transceiver accepted */
static u8 last_talk_cmd, last_poll_cmd;
static u8 pending_cmd, have_pending;

static void set_state(u8 st)
{
    VIA(VIA_ORB) = (u8)((VIA(VIA_ORB) & ~ST_MASK) | st);
}

/* Switch the shift register to input and read it, which also clears the
 * shift-register interrupt flag. */
static u8 shift_in(void)
{
    VIA(VIA_ACR) &= (u8)~ACR_SR_OUT;
    return VIA(VIA_SR);
}

static void start_command(u8 cmd)
{
    VIA(VIA_ACR) |= ACR_SR_OUT;
    VIA(VIA_SR) = cmd;
    set_state(ST_CMD);
    state = SENDING;
    sending_cmd = cmd;
    bus_timeout = 0;
    srq_asserted = 0;
}

static void deliver(void)
{
    u8 addr = reply[0] >> 4;

    adb_packets++;
    if (reply_len < 3 || !IS_TALK(reply[0]) || (reply[0] & 3) != 0)
        return;                 /* only register 0 with two data bytes */
    if (addr == ADB_KEYBOARD) {
        kbd_key(reply[1]);
        if (reply[2] != 0xFF)
            kbd_key(reply[2]);
    } else if (addr == ADB_MOUSE) {
        mouse_packet(reply[1], reply[2]);
    }
}

/* Decide whether the transceiver should poll a different device next. It
 * repeats the last Talk by itself, so there is only something to do when
 * another device asked for service during the last transfer. */
static void queue_poll(void)
{
    u8 addr = last_poll_cmd >> 4;
    u8 cmd;

    if (srq_asserted && last_cmd == last_poll_cmd)
        addr = (addr == ADB_KEYBOARD) ? ADB_MOUSE : ADB_KEYBOARD;
    cmd = TALK_R0(addr);
    if (cmd == last_cmd)
        return;
    pending_cmd = cmd;
    have_pending = 1;
}

void adb_interrupt(void)
{
    u8 status = VIA(VIA_ORB) & (ST_MASK | PB_ADB_INT);
    u8 x;

    quiet = 0;
    switch (state) {
    case IDLE:
        /* Unrequested report: the transceiver polled and the device had data. */
        reply_len = 0;
        bus_timeout = 0;
        srq_asserted = 0;
        x = VIA(VIA_SR);
        if (status & PB_ADB_INT)
            break;              /* flag not asserted: nothing to read */
        state = READING;
        reply[0] = x;           /* the Talk command it was answering */
        reply_len = 1;
        set_state(ST_EVEN);
        break;

    case SENDING:
        if (status == ST_CMD) {
            /* Flag asserted after our command byte: a report was already
             * on its way, and its first byte (the command) was lost because
             * the shift register was an output. Read the report, then send
             * our command again. */
            pending_cmd = sending_cmd;
            have_pending = 1;
            state = READING;
            reply[0] = last_talk_cmd;
            reply_len = 1;
            (void)shift_in();
            set_state(ST_EVEN);
            break;
        }
        last_cmd = sending_cmd;             /* the transceiver took it */
        if (IS_TALK(last_cmd)) {
            last_talk_cmd = last_cmd;
            if ((last_cmd & 0x0F) == 0x0C)
                last_poll_cmd = last_cmd;
        }
        if (IS_TALK(last_cmd)) {
            state = READING;
            reply[0] = last_cmd;
            reply_len = 1;
            (void)shift_in();
        } else {
            state = IDLE;
            break;
        }
        set_state(ST_EVEN);     /* ask for the first data byte */
        break;

    case READING:
        x = VIA(VIA_SR);
        if (!(status & PB_ADB_INT)) {
            if (status == ST_EVEN && reply_len == 1) {
                bus_timeout = 1;            /* device did not answer */
            } else if (status == ST_ODD && reply_len == 2) {
                srq_asserted = 1;           /* another device wants service */
            } else {
                state = IDLE;               /* end of data */
                if (bus_timeout)
                    adb_timeouts++;
                else
                    deliver();
                break;
            }
        }
        if (reply_len < sizeof reply)
            reply[reply_len++] = x;
        VIA(VIA_ORB) ^= ST_MASK;            /* even <-> odd: ask for the next byte */
        break;
    }

    if (state == IDLE) {
        if (!have_pending)
            queue_poll();
        if (have_pending) {
            have_pending = 0;
            start_command(pending_cmd);
        } else {
            (void)shift_in();
            set_state(ST_IDLE);
        }
    }
}

/* Called on every tick that brings no interrupt from the transceiver.
 * Everything above is moved along by those interrupts, one a byte: if one
 * is lost or comes too late (the machine was busy with interrupts off, on
 * the SCSI bus for one) the exchange stops half way, no further interrupt
 * comes, and keyboard and mouse are dead for good. So an exchange that
 * has stood still for a fifth of a second is given up and the keyboard
 * asked again. */
#define STUCK_TICKS 12

void adb_watch(void)
{
    if (quiet < 255)
        quiet++;
    if (state == IDLE || quiet < STUCK_TICKS)
        return;
    quiet = 0;
    adb_restarts++;
    have_pending = 0;
    (void)shift_in();
    set_state(ST_IDLE);
    start_command(TALK_R0(ADB_KEYBOARD));
}

/* Called from via_init with all VIA interrupts disabled. */
void adb_init(void)
{
    VIA(VIA_DDRB) = (u8)((VIA(VIA_DDRB) | ST_MASK) & ~PB_ADB_INT);
    set_state(ST_IDLE);
    VIA(VIA_ACR) = (u8)((VIA(VIA_ACR) & ~ACR_SR_MASK) | ACR_SR_EXT_IN);
    (void)VIA(VIA_SR);          /* drop any stale byte and its interrupt flag */
    state = IDLE;
    last_poll_cmd = TALK_R0(ADB_KEYBOARD);
}

/* Send the first Talk to the keyboard; the transceiver repeats it from then
 * on. Call once, after interrupts are enabled. */
void adb_start_polling(void)
{
    cpu_irq_disable();
    if (state == IDLE)
        start_command(TALK_R0(ADB_KEYBOARD));
    else {
        pending_cmd = TALK_R0(ADB_KEYBOARD);
        have_pending = 1;
    }
    cpu_irq_enable();
}
