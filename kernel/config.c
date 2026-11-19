/* Settings: plain text, one "name = value" per line, # for comments.
 *
 * They come from two places. The settings file on the build machine
 * (config.txt) is copied into the kernel when it is built; see cfgdata.S.
 * Settings changed on the SE itself (the OSConsole's "set", the volume, the
 * WiFi network) are kept in SETTINGS.TXT on the SSH OS disk and read after
 * the built-in ones, so they win. Without an SSH OS disk a change lasts
 * until the next restart.
 */
#include "kernel.h"

#define ENTRIES     16
#define NAME_MAX    16
#define VALUE_MAX   256        /* room for a long value, such as a key */

extern const char config_text[];
extern const u32 config_text_len;

static struct {
    char name[NAME_MAX];
    char value[VALUE_MAX];
    u8 saved;                   /* 1 = belongs in SETTINGS.TXT */
} entries[ENTRIES];

#define SAVED_FILE "SETTINGS.TXT"
static char filebuf[ENTRIES * (NAME_MAX + VALUE_MAX + 4) + 80];

static int same_name(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

const char *config_get(const char *name)
{
    int i;

    for (i = 0; i < ENTRIES; i++)
        if (entries[i].name[0] && same_name(entries[i].name, name))
            return entries[i].value;
    return NULL;
}

/* 0, or -1 when the name or value is too long or the table is full. */
int config_set(const char *name, const char *value)
{
    int i, slot = -1;

    if (!name[0] || strlen(name) >= NAME_MAX || strlen(value) >= VALUE_MAX)
        return -1;
    for (i = 0; i < ENTRIES; i++) {
        if (entries[i].name[0] && same_name(entries[i].name, name)) {
            slot = i;
            break;
        }
        if (!entries[i].name[0] && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return -1;
    memcpy(entries[slot].name, name, strlen(name) + 1);
    memcpy(entries[slot].value, value, strlen(value) + 1);
    return 0;
}

const char *config_name(int i)
{
    return (i >= 0 && i < ENTRIES && entries[i].name[0]) ? entries[i].name : NULL;
}

/* "host:port" or "host" into its parts; the host is copied to host_out. */
int config_host_port(const char *name, char *host_out, int host_max, u16 *port, u16 default_port)
{
    const char *v = config_get(name);
    u32 p = 0;
    int n = 0;

    if (!v || !v[0])
        return -1;
    while (v[n] && v[n] != ':') {
        if (n >= host_max - 1)
            return -1;
        host_out[n] = v[n];
        n++;
    }
    host_out[n] = 0;
    *port = default_port;
    if (v[n] == ':') {
        for (n++; v[n] >= '0' && v[n] <= '9'; n++)
            p = p * 10 + (u32)(v[n] - '0');
        if (v[n] || p == 0 || p > 65535)
            return -1;
        *port = (u16)p;
    }
    return n ? 0 : -1;
}

static int slot_of(const char *name)
{
    int i;

    for (i = 0; i < ENTRIES; i++)
        if (entries[i].name[0] && same_name(entries[i].name, name))
            return i;
    return -1;
}

/* Take settings from a text. Lines that make no sense are skipped. */
static void parse(const char *p, const char *end, int saved)
{
    char name[NAME_MAX], value[VALUE_MAX];
    int n;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        n = 0;
        while (p < end && *p != '=' && *p != '\n' && *p != '\r' && *p != '#' && *p != ' ' &&
               *p != '\t') {
            if (n < NAME_MAX - 1)
                name[n++] = *p;
            else
                n = NAME_MAX;               /* too long: poison the line */
            p++;
        }
        if (n < NAME_MAX)
            name[n] = 0;
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p < end && *p == '=' && n > 0 && n < NAME_MAX) {
            p++;
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            n = 0;
            while (p < end && *p != '\n' && *p != '\r') {
                if (n < VALUE_MAX - 1)
                    value[n++] = *p;
                p++;
            }
            while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t'))
                n--;
            value[n] = 0;
            if (config_set(name, value) == 0 && saved)
                entries[slot_of(name)].saved = 1;
        }
        while (p < end && *p != '\n')
            p++;
        p++;
    }
}

void config_init(void)
{
    parse(config_text, config_text + config_text_len, 0);
}

/* Read the settings kept on the SSH OS disk, once it is found. */
int config_load(void)
{
    u32 len;
    int err = fs_read(SAVED_FILE, filebuf, sizeof filebuf, &len);

    if (err == 0)
        parse(filebuf, filebuf + len, 1);
    return err;
}

/* Change a setting for good: as config_set, and written to the SSH OS
 * disk. Returns 0, -1 as config_set, or an FS_ERR_ value when it could not
 * be written (it is still changed until the next restart). */
int config_store(const char *name, const char *value)
{
    static const char head[] = "# Settings changed on the SE. SSH OS rewrites this file.\n";
    u32 n = sizeof head - 1, l;
    int i;

    if (config_set(name, value) != 0)
        return -1;
    entries[slot_of(name)].saved = 1;
    if (!fs_mounted())
        return FS_ERR_NO_DISK;
    memcpy(filebuf, head, n);
    for (i = 0; i < ENTRIES; i++) {
        if (!entries[i].name[0] || !entries[i].saved)
            continue;
        l = strlen(entries[i].name);
        memcpy(filebuf + n, entries[i].name, l);
        n += l;
        memcpy(filebuf + n, " = ", 3);
        n += 3;
        l = strlen(entries[i].value);
        memcpy(filebuf + n, entries[i].value, l);
        n += l;
        filebuf[n++] = '\n';
    }
    return fs_write(SAVED_FILE, filebuf, n);
}
