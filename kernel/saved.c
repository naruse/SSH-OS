/* What SSH OS remembers on its disk besides the settings (config.c), each
 * a small text file with one item a line:
 *
 *   HOSTS.TXT    servers whose key has been accepted: "host SHA256:..."
 *   RECENT.TXT   the last SSH connections, newest first: "user@host:port"
 *   KEY.TXT      this Macintosh's own SSH key, made here: 128 hex digits
 *   NOKEY.TXT    accounts where adding that key was offered and turned down
 *
 * Both are also kept in memory, so without an SSH OS disk they still work
 * until the next restart.
 */
#include "kernel.h"

#define HOSTS_FILE  "HOSTS.TXT"
#define RECENT_FILE "RECENT.TXT"
#define KEY_FILE    "KEY.TXT"
#define NOKEY_FILE  "NOKEY.TXT"
#define NOKEY_MAX   1024
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

/* Take every line that is exactly `line`, or (with_more) that begins with
 * it and a space, out of a file kept in memory; write it back if any went. */
static void strike(char *text, u32 *lenp, const char *file, const char *line, int with_more)
{
    u32 n = strlen(line), at = 0, end, len = *lenp;
    int any = 0;

    while (at < len) {
        for (end = at; end < len && text[end] != '\n'; end++)
            ;
        if ((end - at == n || (with_more && end - at > n && text[at + n] == ' ')) &&
            memcmp(text + at, line, n) == 0) {
            end = end < len ? end + 1 : len;
            memmove(text + at, text + end, len - end);
            len -= end - at;
            any = 1;
        } else {
            at = end + 1;
        }
    }
    *lenp = len;
    if (any)
        fs_write(file, text, len);
}

static char nokey[NOKEY_MAX];   /* accounts where adding our own key was turned down */
static u32 nokey_len;

/* Forget a connection, and with it what this Macintosh keeps about it: the
 * server's fingerprint (unless another connection still goes there), so
 * that it is shown and asked about again, and a "no" to adding our key to
 * that account, so that this is offered again. */
void recent_remove(int i)
{
    char who[RECENT_LEN], *host, *p;
    int k, others = 0;
    u32 n;

    if (i < 0 || i >= nrecent)
        return;
    memcpy(who, recent[i], RECENT_LEN);
    who[RECENT_LEN - 1] = 0;
    drop(i);
    recent_save();

    for (p = who; *p && *p != ':'; p++)         /* without the port: "user@host" */
        ;
    *p = 0;
    strike(nokey, &nokey_len, NOKEY_FILE, who, 0);
    for (host = who; *host && *host != '@'; host++)
        ;
    if (!*host)
        return;
    host++;
    n = strlen(host);
    for (k = 0; k < nrecent; k++) {
        for (p = recent[k]; *p && *p != '@'; p++)
            ;
        if (*p && memcmp(p + 1, host, n) == 0 && (p[1 + n] == 0 || p[1 + n] == ':'))
            others++;
    }
    if (!others)
        strike(hosts, &hosts_len, HOSTS_FILE, host, 1);
}

/* ----------------------------------------------------------- our own key -- */


/* This Macintosh's own key (32 bytes of secret, then the public half), if
 * one has been made and is on the disk. 1 = there it is. */
int own_key_get(u8 *key64)
{
    char text[130];
    u32 len, i;
    int hi, lo;

    if (!fs_mounted() || fs_read(KEY_FILE, text, sizeof text, &len) != 0 || len < 128)
        return 0;
    for (i = 0; i < 64; i++) {
        hi = text[2 * i];
        lo = text[2 * i + 1];
        hi = hi >= '0' && hi <= '9' ? hi - '0' : hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 : -1;
        lo = lo >= '0' && lo <= '9' ? lo - '0' : lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 : -1;
        if (hi < 0 || lo < 0)
            return 0;
        key64[i] = (u8)(hi << 4 | lo);
    }
    memset(text, 0, sizeof text);
    return 1;
}

/* Keep it. 0 if it reached the disk. */
int own_key_keep(const u8 *key64)
{
    static const char hex[] = "0123456789abcdef";
    char text[129];
    int i, r;

    if (!fs_mounted())
        return -1;
    for (i = 0; i < 64; i++) {
        text[2 * i] = hex[key64[i] >> 4];
        text[2 * i + 1] = hex[key64[i] & 15];
    }
    text[128] = '\n';
    r = fs_write(KEY_FILE, text, 129);
    memset(text, 0, sizeof text);
    return r;
}

/* Was adding the key to this account ("user@host") turned down before? */

int own_key_refused(const char *who)
{
    u32 n = strlen(who), at = 0, end;

    while (at < nokey_len) {
        for (end = at; end < nokey_len && nokey[end] != '\n'; end++)
            ;
        if (end - at == n && memcmp(nokey + at, who, n) == 0)
            return 1;
        at = end + 1;
    }
    return 0;
}

void own_key_refuse(const char *who)
{
    u32 n = strlen(who);

    if (own_key_refused(who) || nokey_len + n + 1 > NOKEY_MAX)
        return;
    memcpy(nokey + nokey_len, who, n);
    nokey_len += n + 1;
    nokey[nokey_len - 1] = '\n';
    fs_write(NOKEY_FILE, nokey, nokey_len);
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
    if (fs_read(NOKEY_FILE, nokey, NOKEY_MAX - 1, &len) == 0) {
        if (len && nokey[len - 1] != '\n')
            nokey[len++] = '\n';
        nokey_len = len;
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
