/* Tests of small kernel pieces on the build machine: SHA-256 / HMAC (the
 * digests are printed and compared with Python's by tests/misc_test.py), the
 * settings parser, and the keyboard's key-to-character step (with the
 * argument "keys": each input line is a list of key transitions in hex, the
 * output line the events they produce; "meta" before the list turns on the
 * setting option = meta).   make test-misc-host */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SSHOS_TYPES_H
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

/* kbd.c reports key moments to the random number pool; not under test here. */
void rng_stir(u32 value)
{
    (void)value;
}

#include "../kernel/sha256.c"
#include "../kernel/config.c"
#include "../kernel/kbd.c"
#include "../kernel/wifi.c"

/* A stand-in for the BlueSCSI's WiFi commands, written from the layouts in
 * its firmware's network.h, for wifi.c to talk to. */
static int fake_adapter = 4, fake_scan_polls, fake_wifi_off;
static char fake_joined[64], fake_key[64];

int eth_adapter_id(void)
{
    return fake_adapter;
}

static void fake_entry(u8 *e, const char *name, int signal, int channel, int flags)
{
    memset(e, 0, 74);
    strcpy((char *)e, name);
    e[70] = (u8)(s8)signal;
    e[71] = (u8)channel;
    e[72] = (u8)flags;
}

int scsi_exec(u8 id, const u8 *cdb, u8 cdb_len, u8 dir, void *data, u32 len, u32 *moved)
{
    u8 *out = data, reply[2 + 3 * 74];
    u32 want = ((u32)cdb[3] << 8) | cdb[4], n = 0;

    if (id != 4 || cdb_len != 6 || cdb[0] != 0x1C || want != len || fake_wifi_off)
        return SCSI_ERR_CHECK;
    switch (cdb[1]) {
    case 1:
        fake_scan_polls = 0;
        reply[0] = 1;
        n = 1;
        break;
    case 2:
        reply[0] = (u8)(++fake_scan_polls >= 3);
        n = 1;
        break;
    case 3:
        if (fake_scan_polls < 3 || want < 2)
            return SCSI_ERR_CHECK;
        reply[0] = 0;
        reply[1] = 3 * 74;
        fake_entry(reply + 2, "HomeNet", -52, 6, 1);
        fake_entry(reply + 2 + 74, "Guest", -70, 11, 0);
        fake_entry(reply + 2 + 148, "A name of exactly thirty-two ch. and then some more", -88, 1, 1);
        n = 2 + 3 * 74;
        break;
    case 4:
        reply[0] = 0;
        reply[1] = 74;
        fake_entry(reply + 2, fake_joined[0] ? fake_joined : "HomeNet", -52, 6, 1);
        n = 2 + 74;
        break;
    case 5:
        if (dir != SCSI_DATA_OUT || len != 130)
            return SCSI_ERR_CHECK;
        memcpy(fake_joined, out, 64);
        memcpy(fake_key, out + 64, 64);
        *moved = len;
        return 0;
    default:
        return SCSI_ERR_CHECK;
    }
    if (dir != SCSI_DATA_IN)
        return SCSI_ERR_CHECK;
    if (n > len)
        n = len;                /* as the firmware does: never more than asked for */
    memcpy(out, reply, n);
    *moved = n;
    return 0;
}

static void wifi_checks(void);

/* A stand-in SSH OS disk that holds one file, there only when have_disk is set. */
static char stored_name[32], stored_text[600];
static int have_disk;

int fs_mounted(void)
{
    return have_disk;
}

int fs_read(const char *name, void *buf, u32 max, u32 *len)
{
    u32 n = (u32)strlen(stored_text);

    *len = 0;
    if (!have_disk || strcmp(name, stored_name) != 0)
        return have_disk ? FS_ERR_NOT_FOUND : FS_ERR_NO_DISK;
    if (n > max)
        return FS_ERR_TOO_BIG;
    memcpy(buf, stored_text, n);
    *len = n;
    return 0;
}

int fs_write(const char *name, const void *buf, u32 len)
{
    if (!have_disk || len >= sizeof stored_text)
        return FS_ERR_NO_DISK;
    snprintf(stored_name, sizeof stored_name, "%s", name);
    memcpy(stored_text, buf, len);
    stored_text[len] = 0;
    return 0;
}

static u32 delays;

void delay_ticks(u32 nticks)
{
    delays += nticks;
}


const char config_text[] =
    "# a comment\n"
    "network = dhcp\r\n"
    "  server=10.1.2.3:2323   \n"
    "\n"
    "secret =  two words  # not a comment here\n"
    "broken line without equals\n"
    "averyveryverylongnamethatdoesnotfit = x\n"
    "ha = homeassistant.local\n"
    "empty =\n"
    "network = 10.0.0.2 255.0.0.0 10.0.0.1 1.1.1.1\n"
    "last = no newline at the end";
const u32 config_text_len = sizeof config_text - 1;

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void digest(const char *name, const u8 *key, size_t klen, const u8 *msg, size_t mlen)
{
    u8 out[32];
    struct sha256 s;
    size_t i;

    if (key) {
        hmac_sha256(key, (u32)klen, msg, (u32)mlen, out);
    } else {
        sha256_init(&s);
        for (i = 0; i < mlen; i += 7)       /* in awkward pieces */
            sha256_update(&s, msg + i, (u32)(mlen - i < 7 ? mlen - i : 7));
        sha256_finish(&s, out);
    }
    printf("%s ", name);
    for (i = 0; i < 32; i++)
        printf("%02x", out[i]);
    printf("\n");
}

static int keys(void)
{
    char line[1024], *p;
    int ev, n;
    unsigned raw;

    while (fgets(line, sizeof line, stdin)) {
        shift_down = control_down = caps_on = option_down = command_down = pending = 0;
        kbd_option_meta = 0;
        p = line;
        if (strncmp(p, "meta", 4) == 0) {
            kbd_option_meta = 1;
            p += 4;
        }
        while (sscanf(p, " %x%n", &raw, &n) == 1) {
            kbd_key((u8)raw);
            p += n;
            /* Read as we go: the queue is small. Releases and keys without
             * a character are not interesting. */
            while ((ev = kbd_get_event()) >= 0)
                if (!(ev & 0x8000) && (ev & 0xFF))
                    printf("%s%02x ", (ev & KBD_EV_META) ? "M" : "", ev & 0xFF);
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    static u8 big[1000], key[200];
    char host[32];
    u16 port = 0;
    size_t i;

    if (argc > 1 && strcmp(argv[1], "keys") == 0)
        return keys();
    for (i = 0; i < sizeof big; i++)
        big[i] = (u8)(i * 7 + 1);
    for (i = 0; i < sizeof key; i++)
        key[i] = (u8)(255 - i);
    /* Lengths around the 55/56/64-byte padding boundaries matter most. */
    for (i = 0; i <= 130; i++) {
        char name[16];

        snprintf(name, sizeof name, "sha%zu", i);
        digest(name, NULL, 0, big, i);
    }
    digest("sha1000", NULL, 0, big, 1000);
    for (i = 0; i <= 200; i += 8) {
        char name[16];

        snprintf(name, sizeof name, "hmac%zu", i);
        digest(name, key, i, big, 16 + i);
    }
    digest("hmac64", key, 64, big, 16);
    digest("hmac65", key, 65, big, 16);

    config_init();
    CHECK(strcmp(config_get("network"), "10.0.0.2 255.0.0.0 10.0.0.1 1.1.1.1") == 0);
    CHECK(strcmp(config_get("server"), "10.1.2.3:2323") == 0);
    CHECK(strcmp(config_get("secret"), "two words  # not a comment here") == 0);
    CHECK(strcmp(config_get("ha"), "homeassistant.local") == 0);
    CHECK(strcmp(config_get("empty"), "") == 0);
    CHECK(strcmp(config_get("last"), "no newline at the end") == 0);
    CHECK(config_get("broken") == NULL && config_get("averyveryverylongnamethatdoesnotfit") == NULL);
    CHECK(config_get("x") == NULL);
    CHECK(config_host_port("server", host, sizeof host, &port, 1) == 0 &&
          strcmp(host, "10.1.2.3") == 0 && port == 2323);
    CHECK(config_host_port("ha", host, sizeof host, &port, 8123) == 0 &&
          strcmp(host, "homeassistant.local") == 0 && port == 8123);
    CHECK(config_host_port("empty", host, sizeof host, &port, 1) != 0);
    CHECK(config_host_port("nothing", host, sizeof host, &port, 1) != 0);
    config_set("server", "h:0");
    CHECK(config_host_port("server", host, sizeof host, &port, 1) != 0);
    config_set("server", "h:99999");
    CHECK(config_host_port("server", host, sizeof host, &port, 1) != 0);
    config_set("server", "h:12x");
    CHECK(config_host_port("server", host, sizeof host, &port, 1) != 0);
    CHECK(config_set("thisnameistoolongforit", "v") != 0);
    CHECK(config_set("wifi", "HomeNet") == 0 && config_set("wifi_key", "") == 0);
    for (i = 0; i < 40; i++) {              /* fill the table; must fail cleanly, not overflow */
        char name[8];

        snprintf(name, sizeof name, "k%zu", i);
        config_set(name, "v");
    }
    CHECK(config_get("k39") == NULL && config_get("network") != NULL);

    /* Settings changed for good: kept in a file, which holds those and no others. */
    CHECK(config_store("ha", "10.9.8.7") == FS_ERR_NO_DISK);        /* changed all the same */
    CHECK(strcmp(config_get("ha"), "10.9.8.7") == 0);
    have_disk = 1;
    CHECK(config_load() == FS_ERR_NOT_FOUND);
    CHECK(config_store("empty", "now full") == 0);
    CHECK(strcmp(stored_name, "SETTINGS.TXT") == 0);
    CHECK(strcmp(stored_text, "# Settings changed on the SE. SSH OS rewrites this file.\n"
                              "ha = 10.9.8.7\nempty = now full\n") == 0);
    CHECK(config_store("thisnameistoolongforit", "v") == -1);
    snprintf(stored_text, sizeof stored_text, "# by hand\nserver = fromdisk:1\nempty=\n");
    CHECK(config_load() == 0);
    CHECK(strcmp(config_get("server"), "fromdisk:1") == 0 && config_get("empty")[0] == 0);
    CHECK(config_store("ha", "x") == 0);                            /* what was loaded is kept */
    CHECK(strstr(stored_text, "server = fromdisk:1\n") && strstr(stored_text, "ha = x\n") &&
          strstr(stored_text, "empty = \n") && !strstr(stored_text, "network"));

    wifi_checks();
    /* Going back to the saved network after a restart. */
    CHECK(wifi_restore() == 0 && delays == 0);      /* wifi_checks left an adapter without WiFi */
    fake_adapter = 4;
    fake_wifi_off = 0;
    fake_joined[0] = 0;
    CHECK(wifi_restore() == 0 && fake_joined[0] == 0 && delays == 0);   /* there already */
    CHECK(config_set("wifi", "Guest") == 0 && config_set("wifi_key", "pass word") == 0);
    CHECK(wifi_restore() == 1 && delays == 60);             /* asked, and it moved */
    CHECK(strcmp(fake_joined, "Guest") == 0 && strcmp(fake_key, "pass word") == 0);
    CHECK(wifi_restore() == 0 && delays == 60);             /* nothing more to do */
    return failures ? 1 : 0;
}

static void wifi_checks(void)
{
    struct wifi_network now, list[WIFI_MAX_NETWORKS];
    int n;

    CHECK(wifi_current(&now) == 0 && strcmp(now.name, "HomeNet") == 0 && now.signal == -52 &&
          now.channel == 6 && now.secured == 1);
    CHECK(wifi_scan_results(list, WIFI_MAX_NETWORKS) == -1);    /* not asked for yet */
    CHECK(wifi_scan_start() == 0);
    CHECK(wifi_scan_done() == 0 && wifi_scan_done() == 0 && wifi_scan_done() == 1);
    n = wifi_scan_results(list, WIFI_MAX_NETWORKS);
    CHECK(n == 3);
    CHECK(strcmp(list[1].name, "Guest") == 0 && list[1].signal == -70 && list[1].channel == 11 &&
          list[1].secured == 0);
    CHECK(strlen(list[2].name) == WIFI_NAME_MAX && list[2].signal == -88);   /* cut, not overrun */
    CHECK(wifi_scan_results(list, 2) == 2);                     /* room for two only */
    CHECK(wifi_bars(-40) == 4 && wifi_bars(-60) == 3 && wifi_bars(-70) == 2 && wifi_bars(-90) == 1);
    CHECK(wifi_join("Guest", "") == 0 && strcmp(fake_joined, "Guest") == 0 && fake_key[0] == 0);
    CHECK(wifi_join("HomeNet", "a password") == 0 && strcmp(fake_key, "a password") == 0);
    CHECK(wifi_current(&now) == 0 && strcmp(now.name, "HomeNet") == 0);
    fake_wifi_off = 1;                                          /* an adapter without these commands */
    CHECK(wifi_current(&now) == -1 && wifi_scan_start() == -1 && wifi_scan_done() == -1 &&
          wifi_scan_results(list, 3) == -1 && wifi_join("x", "y") == -1);
    fake_wifi_off = 0;
    fake_adapter = -1;                                          /* no adapter at all */
    CHECK(wifi_current(&now) == -1 && wifi_scan_start() == -1);
}
