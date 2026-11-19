/* What SSH OS remembers on its disk besides the settings (config.c), each
 * a small text file with one item a line:
 *
 *   HOSTS.TXT    servers whose key has been accepted: "host SHA256:..."
 *   RECENT.TXT   the last SSH connections, newest first: "user@host:port"
 *
 * Both are also kept in memory, so without an SSH OS disk they still work
 * until the next restart.
 */
#include "kernel.h"

#define HOSTS_FILE  "HOSTS.TXT"
#define RECENT_FILE "RECENT.TXT"
#define HOSTS_MAX   4096
#define RECENT_LEN  112

static char hosts[HOSTS_MAX];
static u32 hosts_len;
static char recent[RECENT_MAX][RECENT_LEN];
static int nrecent;
static u8 started;

static void note(const char *s)
{
    serial_puts("DISK: ");
    serial_puts(s);
    serial_puts("\n");
}

/* ------------------------------------------------------------ known hosts -- */

int hosts_known(const char *host, const char *print)
{
    u32 hl = strlen(host), pl = strlen(print), at = 0, end, sp;
    int found = 0;

    while (at < hosts_len) {
        for (end = at; end < hosts_len && hosts[end] != '\n'; end++)
            ;
        for (sp = at; sp < end && hosts[sp] != ' '; sp++)
            ;
        if (sp - at == hl && memcmp(hosts + at, host, hl) == 0) {
            if (sp < end && end - sp - 1 == pl && memcmp(hosts + sp + 1, print, pl) == 0)
                return 1;
            found = -1;
        }
        at = end + 1;
    }
    return found;
}

int hosts_add(const char *host, const char *print)
{
    u32 hl = strlen(host), pl = strlen(print), need = hl + pl + 2, first;

    if (need > HOSTS_MAX)
        return -1;
    /* Out of room: the oldest lines go. */
    while (hosts_len + need > HOSTS_MAX) {
        for (first = 0; first < hosts_len && hosts[first] != '\n'; first++)
            ;
        first = first < hosts_len ? first + 1 : hosts_len;
        memmove(hosts, hosts + first, hosts_len - first);
        hosts_len -= first;
    }
    memcpy(hosts + hosts_len, host, hl);
    hosts[hosts_len + hl] = ' ';
    memcpy(hosts + hosts_len + hl + 1, print, pl);
    hosts_len += need;
    hosts[hosts_len - 1] = '\n';
    return fs_write(HOSTS_FILE, hosts, hosts_len);
}

/* ----------------------------------------------------- recent connections -- */

int recent_count(void)
{
    return nrecent;
}

const char *recent_get(int i)
{
    return (i >= 0 && i < nrecent) ? recent[i] : "";
}

static void recent_save(void)
{
    static char text[RECENT_MAX * RECENT_LEN];
    u32 n = 0, l;
    int i;

    for (i = 0; i < nrecent; i++) {
        l = strlen(recent[i]);
        memcpy(text + n, recent[i], l);
        n += l;
        text[n++] = '\n';
    }
    fs_write(RECENT_FILE, text, n);
}

static void drop(int i)
{
    for (; i + 1 < nrecent; i++)
        memcpy(recent[i], recent[i + 1], RECENT_LEN);
    nrecent--;
}

/* Put a connection at the top of the list, in memory only. */
static void put_first(const char *target, u32 len)
{
    int i;

    if (len == 0 || len >= RECENT_LEN)
        return;
    for (i = 0; i < nrecent; i++)
        if (strlen(recent[i]) == len && memcmp(recent[i], target, len) == 0) {
            drop(i);
            break;
        }
    if (nrecent == RECENT_MAX)
        nrecent--;
    for (i = nrecent; i > 0; i--)
        memcpy(recent[i], recent[i - 1], RECENT_LEN);
    memcpy(recent[0], target, len);
    recent[0][len] = 0;
    nrecent++;
}

void recent_add(const char *target)
{
    /* Nothing to write if it is at the top already. */
    if (nrecent && strcmp(recent[0], target) == 0)
        return;
    put_first(target, strlen(target));
    recent_save();
}

void recent_remove(int i)
{
    if (i < 0 || i >= nrecent)
        return;
    drop(i);
    recent_save();
}

/* ------------------------------------------------------------------ start -- */

void saved_start(void)
{
    static char text[RECENT_MAX * RECENT_LEN];
    u32 len, at, end;
    int lines[RECENT_MAX], n = 0;

    if (started)
        return;
    started = 1;
    if (fs_mount() != 0) {
        note("no SSH OS disk");
        return;
    }
    note("SSH OS disk found");
    if (config_load() == 0) {
        snd_from_settings();
        mouse_from_settings();
    }
    if (fs_read(HOSTS_FILE, hosts, HOSTS_MAX - 1, &len) == 0) {
        if (len && hosts[len - 1] != '\n')      /* edited by hand, perhaps */
            hosts[len++] = '\n';
        hosts_len = len;
    }
    if (fs_read(RECENT_FILE, text, sizeof text - 1, &len) == 0) {
        /* The file has the newest first; put_first wants the oldest first. */
        for (at = 0; at < len && n < RECENT_MAX; at = end + 1) {
            for (end = at; end < len && text[end] != '\n' && text[end] != '\r'; end++)
                ;
            lines[n++] = (int)at;
            text[end] = 0;
        }
        while (n--)
            put_first(text + lines[n], strlen(text + lines[n]));
    }
}
