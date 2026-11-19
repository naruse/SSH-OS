/* Network adapter driver: the BlueSCSI's WiFi, which presents itself on the
 * SCSI bus as a DaynaPORT SCSI/Link Ethernet adapter.
 *
 * The adapter cannot interrupt us. Received frames wait inside it until we
 * ask with a read command, so eth_poll() has to be called regularly.
 *
 * Commands used (all six bytes, length in bytes 3 and 4):
 *   09  read 18 bytes: the adapter's Ethernet address, then counters
 *   0E  switch frame reception on (last byte 80) or off (00)
 *   0A  send one frame; the data is the frame itself
 *   08  read received frames. The answer is a 6-byte header (frame length,
 *       three zero bytes, flags) followed by the frame. A zero length means
 *       nothing is waiting; flag 10 means there is more.
 *
 * After "more", the BlueSCSI wants another read command, while the Snow
 * emulator sends the following frames in the same answer. eth_poll() copes
 * with both.
 */
#include "kernel.h"
#include "net.h"

#define FRAME_MAX   1518        /* longest frame, with its 4-byte checksum */
#define READ_LEN    (6 + FRAME_MAX)
#define RX_FRAMES   8           /* most frames taken in per poll */

static s8 adapter_id = -1;
static u8 rxbuf[RX_FRAMES * READ_LEN];

u32 eth_poll_errors;

int eth_adapter_id(void)
{
    return adapter_id;
}

/* Look for the adapter, read its address and switch it on. */
int eth_init(void)
{
    static const u8 get_address[6] = { 0x09, 0, 0, 0, 18, 0 };
    static const u8 enable[6] = { 0x0E, 0, 0, 0, 0, 0x80 };
    u8 inq[36], info[18], id;
    u32 got = 0;
    int err;

    adapter_id = -1;
    net_stop();
    for (id = 0; id < 7; id++) {
        if (scsi_inquiry(id, inq) != 0)
            continue;
        if (inq[8] == 'D' && inq[9] == 'a' && inq[10] == 'y' && inq[11] == 'n' && inq[12] == 'a')
            break;
    }
    if (id == 7)
        return NET_ERR_NO_ADAPTER;

    err = scsi_exec(id, get_address, 6, SCSI_DATA_IN, info, sizeof info, &got);
    if (err || got < 6)
        return NET_ERR_NO_ADAPTER;
    err = scsi_exec(id, enable, 6, SCSI_DATA_NONE, NULL, 0, NULL);
    if (err)
        return NET_ERR_NO_ADAPTER;

    adapter_id = (s8)id;
    eth_poll_errors = 0;
    net_start(info);
    return 0;
}

void eth_shutdown(void)
{
    static const u8 disable[6] = { 0x0E, 0, 0, 0, 0, 0x00 };

    if (adapter_id >= 0)
        scsi_exec((u8)adapter_id, disable, 6, SCSI_DATA_NONE, NULL, 0, NULL);
    adapter_id = -1;
    net_stop();
}

int eth_send(const u8 *frame, u16 len)
{
    u8 cdb[6] = { 0x0A, 0, 0, 0, 0, 0 };

    if (adapter_id < 0)
        return -1;
    cdb[3] = (u8)(len >> 8);
    cdb[4] = (u8)len;
    return scsi_exec((u8)adapter_id, cdb, 6, SCSI_DATA_OUT, (void *)frame, len, NULL);
}

int eth_poll(void)
{
    static const u8 cdb[6] = { 0x08, 0, 0, READ_LEN >> 8, READ_LEN & 0xFF, 0xC0 };
    u32 got, off;
    u16 len;
    int frames = 0, reads, err;
    u8 more;

    if (adapter_id < 0)
        return 0;
    for (reads = 0; reads < RX_FRAMES; reads++) {
        got = 0;
        err = scsi_exec((u8)adapter_id, cdb, 6, SCSI_DATA_IN, rxbuf, sizeof rxbuf, &got);
        if (err) {
            eth_poll_errors++;
            return err;
        }
        more = 0;
        for (off = 0; off + 6 <= got; off += 6 + len) {
            len = (u16)(((u16)rxbuf[off] << 8) | rxbuf[off + 1]);
            if (len == 0 || len > got - off - 6)
                break;          /* nothing waiting, or a cut-off answer */
            more = rxbuf[off + 5] & 0x10;
            /* Handling a frame may send one (a ping reply, say). That is
             * safe here: the read command is over and rxbuf is ours. */
            net_input(rxbuf + off + 6, len);
            frames++;
        }
        if (!more)
            break;
    }
    return frames;
}
