/* The BlueSCSI's WiFi commands: which network it is on and how strong the
 * signal is, which networks are in range, and joining one. They are an
 * addition of the BlueSCSI firmware to the network adapter it imitates
 * (its lib/SCSI2SD/src/firmware/network.h): SCSI command 0x1C with the
 * request in the second byte and the data length in the fourth and fifth.
 * An adapter without them (the Snow emulator's) refuses the command, and
 * every function here then returns an error.
 *
 * NOT TRIED ON A REAL BLUESCSI YET: written from its source code.
 */
#include "kernel.h"

#define CMD_WIFI        0x1C
#define WIFI_SCAN       1       /* start looking; answers one byte */
#define WIFI_COMPLETE   2       /* one byte: 1 when the look is over */
#define WIFI_RESULTS    3       /* two bytes of length, then entries */
#define WIFI_INFO       4       /* the same, with the one entry we are on */
#define WIFI_JOIN       5       /* we send: name, password, channel */

#define ENTRY_SIZE      74      /* name 64, station address 6, signal, channel, flags, unused */
#define JOIN_SIZE       130     /* name 64, password 64, channel, unused */

static u8 buf[2 + WIFI_MAX_NETWORKS * ENTRY_SIZE + JOIN_SIZE];

#ifdef WIFI_FAKE
/* Built with -DWIFI_FAKE (make test-wifi), a made-up adapter answers in
 * place of the real one, so that the network panel's list and joining can
 * be seen and tested in the emulator. It follows the same layouts. */
static u8 fake_polls;
static char fake_on[WIFI_NAME_MAX + 1] = "HomeNet";

static void fake_entry(u8 *e, const char *name, int signal, u8 channel, u8 flags)
{
    u32 n = strlen(name);

    memset(e, 0, ENTRY_SIZE);
    memcpy(e, name, n);
    e[70] = (u8)signal;
    e[71] = channel;
    e[72] = flags;
}

static int command(u8 what, u8 dir, u32 len, u32 *moved)
{
    (void)dir;
    *moved = len;
    switch (what) {
    case WIFI_SCAN:
        fake_polls = 0;
        buf[0] = 1;
        return 0;
    case WIFI_COMPLETE:
        buf[0] = ++fake_polls >= 3;
        return 0;
    case WIFI_RESULTS:
        buf[0] = (u8)((4 * ENTRY_SIZE) >> 8);
        buf[1] = (u8)(4 * ENTRY_SIZE);
        fake_entry(buf + 2, "HomeNet", -50, 6, 1);
        fake_entry(buf + 2 + ENTRY_SIZE, "Guest", -64, 11, 0);
        fake_entry(buf + 2 + 2 * ENTRY_SIZE, "Vecinos 2.4", -75, 1, 1);
        fake_entry(buf + 2 + 3 * ENTRY_SIZE, "Printer-setup", -86, 3, 0);
        *moved = 2 + 4 * ENTRY_SIZE;
        return 0;
    case WIFI_INFO:
        buf[0] = 0;
        buf[1] = ENTRY_SIZE;
        fake_entry(buf + 2, fake_on, fake_on[0] == 'G' ? -64 : -50, 6, 1);
        return 0;
    case WIFI_JOIN:
        memcpy(fake_on, buf, WIFI_NAME_MAX);
        return 0;
    }
    return -1;
}
#else
static int command(u8 what, u8 dir, u32 len, u32 *moved)
{
    u8 cdb[6] = {CMD_WIFI, what, 0, (u8)(len >> 8), (u8)len, 0};
    int id = eth_adapter_id();

    if (id < 0)
        return -1;
    return scsi_exec((u8)id, cdb, 6, dir, buf, len, moved) == 0 ? 0 : -1;
}
#endif

static void take_entry(struct wifi_network *n, const u8 *e)
{
    int i;

    for (i = 0; i < WIFI_NAME_MAX && e[i]; i++)
        n->name[i] = (e[i] >= 32 && e[i] < 127) ? (char)e[i] : '?';
    n->name[i] = 0;
    n->signal = (s8)e[70];
    n->channel = e[71];
    n->secured = e[72] & 1;
}

/* The network the adapter is on. 0, or -1 if it cannot say or is on none. */
int wifi_current(struct wifi_network *n)
{
    u32 moved = 0;

    if (command(WIFI_INFO, SCSI_DATA_IN, 2 + ENTRY_SIZE, &moved) != 0 || moved < 2 + ENTRY_SIZE ||
        ((buf[0] << 8) | buf[1]) < ENTRY_SIZE || !buf[2])
        return -1;
    take_entry(n, buf + 2);
    return 0;
}

int wifi_scan_start(void)
{
    u32 moved = 0;

    return (command(WIFI_SCAN, SCSI_DATA_IN, 1, &moved) == 0 && moved == 1 && buf[0] == 1) ? 0 : -1;
}

/* 1 when the look started by wifi_scan_start is over, 0 while it lasts, -1 on error. */
int wifi_scan_done(void)
{
    u32 moved = 0;

    if (command(WIFI_COMPLETE, SCSI_DATA_IN, 1, &moved) != 0 || moved != 1)
        return -1;
    return buf[0] == 1;
}

/* The networks found: how many were put in list (at most max), or -1. */
int wifi_scan_results(struct wifi_network *list, int max)
{
    u32 moved = 0, len;
    int n = 0;

    if (max > WIFI_MAX_NETWORKS)
        max = WIFI_MAX_NETWORKS;
    if (command(WIFI_RESULTS, SCSI_DATA_IN, 2 + (u32)max * ENTRY_SIZE, &moved) != 0 || moved < 2)
        return -1;
    len = ((u32)buf[0] << 8) | buf[1];
    if (len > moved - 2)
        len = moved - 2;
    for (; n < max && (u32)(n + 1) * ENTRY_SIZE <= len; n++)
        take_entry(&list[n], buf + 2 + n * ENTRY_SIZE);
    return n;
}

/* Ask the adapter to move to another network. It answers at once; whether
 * that worked shows in wifi_current() some seconds later. */
int wifi_join(const char *name, const char *password)
{
    u32 moved = 0, i;
    int r;

    memset(buf, 0, JOIN_SIZE);
    for (i = 0; i < 63 && name[i]; i++)
        buf[i] = (u8)name[i];
    for (i = 0; i < 63 && password[i]; i++)
        buf[64 + i] = (u8)password[i];
    r = command(WIFI_JOIN, SCSI_DATA_OUT, JOIN_SIZE, &moved);
    memset(buf, 0, JOIN_SIZE);              /* do not keep the password about */
    return r;
}

/* Signal strength as 1 (weak) to 4 (full), from the figure in dBm. */
int wifi_bars(int signal)
{
    return signal >= -55 ? 4 : signal >= -67 ? 3 : signal >= -78 ? 2 : 1;
}

/* After a restart: if the settings name a network (the last one joined
 * from the panel) and the adapter is on another or on none, ask it to go
 * back. Returns 1 if it got there, 0 if there was nothing to do or it did
 * not. Waits up to ten seconds, and not at all on an adapter without these
 * commands. */
int wifi_restore(void)
{
    const char *name = config_get("wifi"), *key = config_get("wifi_key");
    struct wifi_network now;
    int tries;

    if (!name || !name[0] || (wifi_current(&now) == 0 && strcmp(now.name, name) == 0))
        return 0;
    if (wifi_join(name, key ? key : "") != 0)
        return 0;
    for (tries = 0; tries < 10; tries++) {
        delay_ticks(60);
        if (wifi_current(&now) == 0 && strcmp(now.name, name) == 0)
            return 1;
    }
    return 0;
}
