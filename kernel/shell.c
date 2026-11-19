/* The OSConsole, SSH OS's command line: a line editor and a handful of
 * built-in commands, mostly for looking at the machine while the kernel
 * grows. */
#include "kernel.h"
#include "net.h"
#include "shell.h"

#define LINE_MAX 80
#define BLINK_TICKS 30          /* cursor on/off period: half a second */

void machine_report(void);
void show_font(void);

int shell_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Wait for a key that produces a character, blinking the cursor meanwhile. */
static u8 go_home;

static u8 get_char(void)
{
    u32 next_blink = ticks + BLINK_TICKS;
    int cursor_on = 1;
    int ev;
    u8 c;

    con_cursor_toggle();
    for (;;) {
        ev = kbd_get_event();
        if (ev >= 0 && (c = (u8)(ev & 0xFF)) != 0)
            break;
        if (ev < 0) {
            net_poll();         /* answer the network while nothing else happens */
            if (bar_poll() == BAR_HOME) {
                go_home = 1;    /* the SSH OS menu's Home: as if "desk" had been typed */
                c = '\n';
                break;
            }
            bar_tick();
            bar_idle();         /* sleep until the next interrupt */
        } else {
            bar_work();
        }
        if ((s32)(ticks - next_blink) >= 0) {
            con_cursor_toggle();
            cursor_on = !cursor_on;
            next_blink = ticks + BLINK_TICKS;
        }
    }
    if (cursor_on)
        con_cursor_toggle();
    return c;
}

static void read_line(char *line)
{
    int len = 0;
    u8 c;

    for (;;) {
        c = get_char();
        if (go_home) {
            go_home = 0;
            kputc('\n');
            memcpy(line, "desk", 5);
            return;
        }
        if (c == '\n') {
            kputc('\n');
            line[len] = 0;
            return;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                kputs("\b \b");
            }
            continue;
        }
        if (c >= 32 && c != 127 && len < LINE_MAX - 1) {
            line[len++] = (char)c;
            kputc((char)c);
        }
    }
}

static void cmd_help(void)
{
    kputs("Commands:\n"
          "  help     this list\n"
          "  info     what the kernel knows about the machine\n"
          "  uptime   time since start, counted by the 60 Hz interrupt\n"
          "  keys     show raw key codes as you type (Esc to stop)\n"
          "  mouse    show the mouse position and button (any key to stop)\n"
          "  scsi     list the devices on the SCSI bus\n"
          "  block ID N   show block N of the disk at SCSI ID\n"
          "  speed ID     time reading 128 KB from the disk at SCSI ID\n"
          "  scsi slow    read SCSI data one hand-shaken byte at a time\n"
          "  scsi fast    let the SCSI chip do the handshake (the default)\n"
          "  wtest ID     write test; only runs on an SSH OS scratch disk\n"
          "  disk     the SSH OS disk, where settings are kept: where it is, room left\n"
          "  format ID    make the blank disk at SCSI ID an SSH OS disk\n"
          "  ls, cat NAME, rm NAME    the files on the SSH OS disk\n"
          "  desk     the home screen, with icons for the mouse\n"
          "  ssh USER@HOST   log in to another computer (Command-Q leaves)\n"
          "  term     terminal for a shell on your Mac (needs the helper; Command-Q leaves)\n"
          "  set      show the settings; set NAME VALUE changes one and saves it on the\n"
          "           SSH OS disk (without one, it lasts until restart)\n"
          "  net up       find the network adapter and start the network as set\n"
          "  net static ADDRESS MASK GATEWAY [NAMESERVER]   the same, set by hand\n"
          "  net          show the network settings and counters; net down stops it\n"
          "  net speed    time the adapter: empty polls and full-size sends\n"
          "  ping HOST    send four pings to a name or address (Esc stops)\n"
          "  dns NAME     look up a name\n"
          "  tcp HOST PORT    open a connection and type into it (Esc closes)\n"
          "  tcptest HOST PORT down|echo   timed transfer that checks every byte\n"
          "  bench    time the arithmetic an SSH connection would need\n"
          "  font     show every character of the font\n"
          "  clear    clear the screen\n"
          "  crash    cause a CPU fault on purpose, to see the crash screen\n");
}

static void cmd_uptime(void)
{
    u32 t = ticks;
    u32 tenths = t * 1000 / 6015;       /* the tick is 60.15 Hz */

    kprintf("Up %lu.%lu seconds (%lu ticks).\n", tenths / 10, tenths % 10, t);
    kprintf("Keyboard/mouse packets %lu, bus timeouts %lu, stray interrupts %lu.\n",
            adb_packets, adb_timeouts, irq_stray);
}

static void cmd_keys(void)
{
    int ev;

    kputs("Press keys; each shows as code/character. Esc stops.\n");
    for (;;) {
        ev = kbd_get_event();
        if (ev < 0) {
            cpu_idle();
            continue;
        }
        kprintf("%02lX%s", (u32)(ev >> 8) & 0x7F, (ev & 0x8000) ? "^ " : "v");
        if ((ev & 0xFF) >= 32 && (ev & 0xFF) != 127)
            kprintf("'%c' ", ev & 0xFF);
        else if (!(ev & 0x8000))
            kputc(' ');
        if ((ev & 0xFF) == 0x1B)
            break;
    }
    kputc('\n');
}

static void cmd_mouse(void)
{
    u32 seen = mouse.moves - 1;

    kputs("Move the mouse; any key stops.\n");
    for (;;) {
        int ev = kbd_get_event();

        if (ev >= 0 && (ev & 0xFF))
            break;
        if (seen != mouse.moves) {
            seen = mouse.moves;
            kprintf("\rx %3d  y %3d  button %s   ", mouse.x, mouse.y,
                    mouse.button ? "down" : "up  ");
        }
        cpu_idle();
    }
    kputc('\n');
}

/* ------------------------------------------------------------- SCSI -- */

#define SCRATCH_MAGIC "SSHOS-SCRATCH-DISK"

static u8 blockbuf[4096];       /* eight disk blocks */

/* A number in decimal, or in hex after "0x" or "$". */
int shell_parse_num(const char *s, u32 *out)
{
    u32 v = 0, base = 10, d;

    if (s[0] == '$') {
        base = 16;
        s++;
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (!*s)
        return 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9')
            d = (u32)(*s - '0');
        else if (*s >= 'a' && *s <= 'f')
            d = (u32)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F')
            d = (u32)(*s - 'A' + 10);
        else
            return 0;
        if (d >= base)
            return 0;
        v = v * base + d;
    }
    *out = v;
    return 1;
}

static int parse_id(const char *arg, u8 *id)
{
    u32 v;

    if (!arg || !shell_parse_num(arg, &v) || v > 6) {
        kputs("Give a SCSI ID from 0 to 6.\n");
        return 0;
    }
    *id = (u8)v;
    return 1;
}

static void scsi_complain(int err)
{
    kprintf("Failed: %s", scsi_strerror(err));
    if (err == SCSI_ERR_CHECK)
        kprintf(" (sense %lX, code %02lX/%02lX)", (u32)scsi_sense.key,
                (u32)scsi_sense.asc, (u32)scsi_sense.ascq);
    kputs(".\n");
}

/* Copy a fixed-width text field of an INQUIRY answer. */
static void field(char *dst, const u8 *src, int n)
{
    int i;

    for (i = 0; i < n; i++)
        dst[i] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : ' ';
    dst[n] = 0;
}

static void cmd_scsi(void)
{
    static const char *const types[] = {
        "disk     ", "tape     ", "printer  ", "processor", "WORM     ",
        "CD-ROM   ", "scanner  ", "optical  ",
    };
    u8 inq[36], id, type;
    char vendor[9], product[17], rev[5];
    u32 blocks, bsize;
    int err, found = 0;

    kputs("ID  Type       Vendor    Product           Rev   Size\n");
    for (id = 0; id < 7; id++) {
        err = scsi_inquiry(id, inq);
        if (err == SCSI_ERR_NO_DEVICE)
            continue;
        found++;
        if (err) {
            kprintf("%d   ", id);
            scsi_complain(err);
            continue;
        }
        type = inq[0] & 0x1F;
        field(vendor, inq + 8, 8);
        field(product, inq + 16, 16);
        field(rev, inq + 32, 4);
        kprintf("%d   %s  %s  %s  %s  ", id, type < 8 ? types[type] : "other    ",
                vendor, product, rev);
        if ((type == 0 || type == 5 || type == 7) && scsi_capacity(id, &blocks, &bsize) == 0)
            kprintf("%lu KB (%lu blocks of %lu)", (blocks / 1024) * bsize + (blocks % 1024) * bsize / 1024,
                    blocks, bsize);
        kputc('\n');
    }
    kprintf("7   this Mac\n%d device%s found.\n", found, found == 1 ? "" : "s");
}

static void cmd_block(const char *id_arg, const char *n_arg)
{
    u8 id;
    u32 n, i, j;
    int err;

    if (!parse_id(id_arg, &id))
        return;
    if (!n_arg || !shell_parse_num(n_arg, &n)) {
        kputs("Usage: block ID N\n");
        return;
    }
    err = scsi_read(id, n, 1, blockbuf);
    if (err) {
        scsi_complain(err);
        return;
    }
    for (i = 0; i < 512; i += 16) {
        kprintf("%04lX ", i);
        for (j = 0; j < 16; j++)
            kprintf(" %02lX", (u32)blockbuf[i + j]);
        kputs("  ");
        for (j = 0; j < 16; j++)
            kputc((blockbuf[i + j] >= 32 && blockbuf[i + j] < 127) ? (char)blockbuf[i + j] : '.');
        kputc('\n');
    }
}

static void cmd_speed(const char *id_arg)
{
    enum { CHUNK = 8, TOTAL = 256 };    /* blocks per read, blocks in all */
    u8 id;
    u32 blocks, bsize, b, t0, dt, rate;
    int err;

    if (!parse_id(id_arg, &id))
        return;
    err = scsi_capacity(id, &blocks, &bsize);
    if (err) {
        scsi_complain(err);
        return;
    }
    if (bsize != 512 || blocks < TOTAL) {
        kputs("Needs a disk with 512-byte blocks and at least 128 KB.\n");
        return;
    }
    t0 = ticks;
    while (ticks == t0)                 /* start on a tick boundary */
        cpu_idle();
    t0 = ticks;
    for (b = 0; b < TOTAL; b += CHUNK) {
        err = scsi_read(id, b, CHUNK, blockbuf);
        if (err) {
            scsi_complain(err);
            return;
        }
    }
    dt = ticks - t0;
    if (dt == 0)
        dt = 1;
    rate = (TOTAL * 512UL * 6015) / (dt * 100);        /* bytes per second */
    kprintf("Read %lu KB in %lu ticks: %lu.%lu KB per second (%s reads).\n",
            TOTAL * 512UL / 1024, dt, rate / 1024, (rate % 1024) * 10 / 1024,
            scsi_fast_reads ? "fast" : "slow");
}

/* Write a block, read it back, compare, and put the old contents back. It
 * refuses any disk that is not marked as an SSH OS scratch disk, so it cannot
 * damage a disk holding something else. */
static void cmd_wtest(const char *id_arg)
{
    u8 *old = blockbuf, *pattern = blockbuf + 512, *back = blockbuf + 1024;
    u8 id;
    u32 blocks, bsize, target, i;
    int err, same = 1;

    if (!parse_id(id_arg, &id))
        return;
    err = scsi_read(id, 0, 1, blockbuf);
    if (err == 0)
        err = scsi_capacity(id, &blocks, &bsize);
    if (err) {
        scsi_complain(err);
        return;
    }
    for (i = 0; i < sizeof SCRATCH_MAGIC - 1; i++)
        if (blockbuf[i] != (u8)SCRATCH_MAGIC[i]) {
            kputs("Refused: that is not an SSH OS scratch disk. Nothing was written.\n");
            return;
        }
    target = blocks - 1;
    err = scsi_read(id, target, 1, old);
    for (i = 0; i < 512; i++)
        pattern[i] = (u8)(ticks + i * 7);
    if (err == 0)
        err = scsi_write(id, target, 1, pattern);
    if (err == 0)
        err = scsi_read(id, target, 1, back);
    if (err) {
        scsi_complain(err);
        return;
    }
    for (i = 0; i < 512; i++)
        if (back[i] != pattern[i])
            same = 0;
    err = scsi_write(id, target, 1, old);
    if (err) {
        scsi_complain(err);
        return;
    }
    kprintf("Block %lu written and read back: %s.\n", target,
            same ? "contents match" : "CONTENTS DIFFER");
}

/* ---------------------------------------------------- the SSH OS disk -- */

static void fs_complain(int err)
{
    kprintf("Failed: %s.\n", fs_strerror(err));
}

static void cmd_disk(void)
{
    u32 total, free_kb;
    int err;

    if (!fs_mounted())
        fs_mount();                     /* one may have been made since start-up */
    if (!fs_mounted()) {
        kputs("There is no SSH OS disk, so nothing is kept after a restart. To make one,\n"
              "give the BlueSCSI a blank disk image, find its ID with scsi, and type\n"
              "format ID.\n");
        return;
    }
    err = fs_space(&total, &free_kb);
    if (err) {
        fs_complain(err);
        return;
    }
    kprintf("SSH OS disk at SCSI ID %d: %lu KB, %lu KB free.\n", fs_disk_id(), total, free_kb);
}

static void cmd_format(const char *id_arg)
{
    char answer[LINE_MAX];
    u8 id;
    int kind, err;

    if (!parse_id(id_arg, &id))
        return;
    kind = fs_look(id);
    if (kind == FS_ERR_IN_USE) {
        kputs("Refused: that disk holds something that is not SSH OS's. Nothing was\n"
              "written. Only a blank disk can be made an SSH OS disk.\n");
    } else if (kind < 0) {
        fs_complain(kind);
    } else {
        kprintf("%s Erase it and make it an empty SSH OS disk? Type yes: ",
                kind == FS_OURS ? "The disk at that ID is an SSH OS disk already, with files on it."
                : kind == FS_SCRATCH ? "The disk at that ID is an SSH OS scratch disk."
                : "The disk at that ID is blank.");
        shell_read_line(answer);
        if (!shell_streq(answer, "yes")) {
            kputs("Nothing was written.\n");
        } else if ((err = fs_format(id)) != 0) {
            fs_complain(err);
        } else {
            kputs("Done. ");
        }
    }
    if (fs_mount() == 0)                /* fs_look and fs_format leave it unmounted */
        cmd_disk();
}

static void cmd_ls(void)
{
    char name[13];
    u32 size;
    int i, err;

    for (i = 0; (err = fs_list(i, name, &size)) == 0; i++) {
        kprintf("  %s", name);
        for (err = (int)strlen(name); err < 12; err++)
            kputc(' ');
        kprintf(" %7lu\n", size);
    }
    if (err != FS_ERR_NOT_FOUND)
        fs_complain(err);
    else
        kprintf("%d file%s.\n", i, i == 1 ? "" : "s");
}

static void cmd_cat(const char *name)
{
    u32 len, i;
    int err;

    if (!name) {
        kputs("Usage: cat NAME\n");
        return;
    }
    err = fs_read(name, blockbuf, sizeof blockbuf, &len);
    if (err) {
        fs_complain(err);
        return;
    }
    for (i = 0; i < len; i++)
        if (blockbuf[i] == '\n' || blockbuf[i] >= 32)
            kputc((char)blockbuf[i]);
    if (len && blockbuf[len - 1] != '\n')
        kputc('\n');
}

static void cmd_rm(const char *name)
{
    int err;

    if (!name) {
        kputs("Usage: rm NAME\n");
        return;
    }
    err = fs_delete(name);
    if (err)
        fs_complain(err);
}

/* ---------------------------------------------------------- network -- */

void shell_fail(int net_err)
{
    kprintf("Failed: %s.\n", net_strerror(net_err));
}

void shell_print_ip(u32 ip)
{
    kprintf("%d.%d.%d.%d", (int)(ip >> 24), (int)((ip >> 16) & 255), (int)((ip >> 8) & 255),
            (int)(ip & 255));
}

/* Lets Esc stop a network command that is waiting for an answer. Keys
 * typed ahead for the next command stay in the queue. */
static int esc_pressed(void)
{
    return kbd_escape;
}

static void net_status(void)
{
    if (!net.up) {
        kputs("The network is not started. Try: net up\n");
        return;
    }
    kprintf("Adapter       SCSI ID %d, address %02lX:%02lX:%02lX:%02lX:%02lX:%02lX\n",
            eth_adapter_id(), (u32)net.mac[0], (u32)net.mac[1], (u32)net.mac[2],
            (u32)net.mac[3], (u32)net.mac[4], (u32)net.mac[5]);
    if (!net.ip) {
        kputs("Address       none yet\n");
    } else {
        kputs("Address       ");
        shell_print_ip(net.ip);
        kputs(", mask ");
        shell_print_ip(net.mask);
        if (net.from_dhcp)
            kprintf(" (from DHCP, valid for %lu seconds)\n", net.lease_secs);
        else
            kputs(" (set by hand)\n");
        kputs("Gateway       ");
        shell_print_ip(net.gateway);
        kputs(", name server ");
        shell_print_ip(net.dns);
        kputc('\n');
    }
    kprintf("Frames        received %lu, sent %lu, damaged %lu, ignored %lu\n",
            net_stats.rx_frames, net_stats.tx_frames, net_stats.rx_bad, net_stats.rx_ignored);
    kprintf("Errors        sending %lu, asking the adapter %lu\n", net_stats.tx_errors,
            eth_poll_errors);
}

/* How long the two basic operations take: asking the adapter for frames
 * when there are none, and sending a full-size frame. */
static void net_speed(void)
{
    enum { POLLS = 200, SENDS = 10 };
    u32 t0, dt;
    int i, err = 0;

    if (!net.up || !net.ip) {
        kputs("Start the network first.\n");
        return;
    }
    t0 = ticks;
    while (ticks == t0)
        cpu_idle();
    t0 = ticks;
    for (i = 0; i < POLLS; i++)
        net_poll();
    dt = ticks - t0;
    kprintf("%d polls in %lu ticks: %lu.%lu ms each.\n", POLLS, dt,
            dt * 1000 / (60 * POLLS), (dt * 10000 / (60 * POLLS)) % 10);

    /* UDP port 9 is "discard"; whoever receives these throws them away. */
    t0 = ticks;
    for (i = 0; i < SENDS && err == 0; i++) {
        memset(udp_buffer(), 'J', UDP_MAX_DATA);
        err = udp_send(net.gateway, 4009, 9, UDP_MAX_DATA);
        if (err == NET_ERR_ARP) {       /* first time: the gateway's address is being asked */
            delay_ticks(6);
            net_poll();
            err = 0;
            i--;
            if (ticks - t0 > 300)
                err = NET_ERR_ARP;
        }
    }
    dt = ticks - t0;
    if (err) {
        kprintf("Failed: %s.\n", net_strerror(err));
        return;
    }
    if (dt == 0)
        dt = 1;
    kprintf("%d full frames (1514 bytes) sent in %lu ticks: %lu KB per second.\n", SENDS, dt,
            SENDS * 1514UL * 60 / dt / 1024);
}

/* Start the network the way the "network" setting says: "dhcp", or
 * ADDRESS MASK GATEWAY NAMESERVER. */
static u8 net_quiet;

static int net_up_from_settings(void)
{
    const char *v = config_get("network");
    char part[4][16];
    u32 addr[4] = { 0, 0, 0, 0 };
    int i, n, err;

    err = eth_init();
    if (err)
        return err;
    if (!v || !v[0] || shell_streq(v, "dhcp")) {
        if (!net_quiet)
            kputs("Adapter found. Asking the network for an address...\n");
        return net_dhcp();
    }
    for (i = 0; i < 4; i++) {
        while (*v == ' ')
            v++;
        for (n = 0; *v && *v != ' ' && n < 15; n++)
            part[i][n] = *v++;
        part[i][n] = 0;
        if (n && !net_parse_ip(part[i], &addr[i]))
            return NET_ERR_NO_ROUTE;
    }
    if (!addr[0] || !addr[1])
        return NET_ERR_NO_ROUTE;
    net_set_static(addr[0], addr[1], addr[2], addr[3]);
    return 0;
}

/* Is the address to come from the network (rather than the settings)? */
int shell_network_by_dhcp(void)
{
    const char *v = config_get("network");

    return !v || !v[0] || shell_streq(v, "dhcp");
}

int shell_network_start(void)
{
    int err;

    if (net.up && net.ip)
        return 0;
    net_quiet = 1;
    err = net_up_from_settings();
    net_quiet = 0;
    return err;
}

void shell_read_line(char *line)
{
    read_line(line);
}

/* For programs that need the network: start it if it is not up yet. */
int shell_need_network(void)
{
    int err;

    if (net.up && net.ip)
        return 0;
    err = net_up_from_settings();
    if (err)
        shell_fail(err);
    return err;
}

static void cmd_set(int argc, char **argv)
{
    char value[200];
    int i, n = 0;
    const char *s;

    if (argc < 2) {
        kputs("Settings (change one with: set NAME VALUE):\n");
        for (i = 0; config_name(i) || i < 16; i++) {
            if (!config_name(i))
                continue;
            s = config_get(config_name(i));
            /* Secrets are not shown on the screen. */
            if (shell_streq(config_name(i), "secret") ||
                shell_streq(config_name(i), "wifi_key"))
                s = s[0] ? "(set, hidden)" : "(not set)";
            kprintf("  %s = %s\n", config_name(i), s);
        }
        return;
    }
    for (i = 2; i < argc; i++) {
        if (i > 2 && n < (int)sizeof value - 1)
            value[n++] = ' ';
        for (s = argv[i]; *s && n < (int)sizeof value - 1; s++)
            value[n++] = *s;
    }
    value[n] = 0;
    i = config_store(argv[1], value);
    if (i == -1)
        kputs("That name or value is too long.\n");
    else if (i == 0)
        kputs("Saved on the SSH OS disk.\n");
    else if (i != FS_ERR_NO_DISK)
        kprintf("Changed, but not saved: %s.\n", fs_strerror(i));
}

static void cmd_net(int argc, char **argv)
{
    u32 ip, mask, gateway, dns = 0;
    int err;

    if (argc < 2) {
        net_status();
        return;
    }
    if (shell_streq(argv[1], "speed")) {
        net_speed();
        return;
    }
    if (shell_streq(argv[1], "down")) {
        eth_shutdown();
        kputs("Network stopped.\n");
        return;
    }
    if (shell_streq(argv[1], "up")) {
        err = net_up_from_settings();
    } else if (shell_streq(argv[1], "static")) {
        if (argc < 5 || !net_parse_ip(argv[2], &ip) || !net_parse_ip(argv[3], &mask) ||
            !net_parse_ip(argv[4], &gateway) || (argc > 5 && !net_parse_ip(argv[5], &dns))) {
            kputs("Usage: net static ADDRESS MASK GATEWAY [NAMESERVER]\n");
            return;
        }
        err = eth_init();
        if (err == 0)
            net_set_static(ip, mask, gateway, dns);
    } else {
        kputs("Usage: net, net up, net static ..., net down\n");
        return;
    }
    if (err)
        kprintf("Failed: %s.\n", net_strerror(err));
    net_status();
}

static void cmd_ping(const char *host)
{
    u32 ip, rtt;
    u16 seq;
    int err, got = 0;

    if (!host) {
        kputs("Usage: ping HOST\n");
        return;
    }
    err = net_resolve(host, &ip);
    if (err) {
        kprintf("Failed: %s.\n", net_strerror(err));
        return;
    }
    kputs("Pinging ");
    shell_print_ip(ip);
    kputs("\n");
    for (seq = 1; seq <= 4; seq++) {
        err = net_ping(ip, seq, 2 * 60, &rtt);
        if (err == NET_ERR_ABORTED)
            break;
        if (err) {
            kprintf("  %d: %s\n", seq, net_strerror(err));
            if (err != NET_ERR_TIMEOUT)
                break;
            continue;
        }
        got++;
        /* The clock only counts sixtieths of a second. */
        if (rtt == 0)
            kprintf("  %d: answered in under 17 ms\n", seq);
        else
            kprintf("  %d: answered in about %lu ms\n", seq, (rtt * 1000 + 30) / 60);
    }
    kprintf("%d of %d answered.\n", got, seq > 4 ? 4 : seq);
}

static void cmd_dns(const char *name)
{
    u32 ip;
    int err;

    if (!name) {
        kputs("Usage: dns NAME\n");
        return;
    }
    err = net_resolve(name, &ip);
    if (err) {
        kprintf("Failed: %s.\n", net_strerror(err));
        return;
    }
    kprintf("%s is ", name);
    shell_print_ip(ip);
    kputc('\n');
}

/* --------------------------------------------------------- dispatch -- */

#define MAX_ARGS 6

static void run(char *line)
{
    char *argv[MAX_ARGS] = { 0 };
    int argc = 0;
    const char *cmd;

    /* Split the line into words in place. */
    while (*line && argc < MAX_ARGS) {
        while (*line == ' ')
            line++;
        if (!*line)
            break;
        argv[argc++] = line;
        while (*line && *line != ' ')
            line++;
        if (*line)
            *line++ = 0;
    }
    if (argc == 0)
        return;
    cmd = argv[0];

    if (shell_streq(cmd, "help") || shell_streq(cmd, "?"))
        cmd_help();
    else if (shell_streq(cmd, "info"))
        machine_report();
    else if (shell_streq(cmd, "uptime"))
        cmd_uptime();
    else if (shell_streq(cmd, "keys"))
        cmd_keys();
    else if (shell_streq(cmd, "mouse"))
        cmd_mouse();
    else if (shell_streq(cmd, "scsi") && argv[1] && shell_streq(argv[1], "slow"))
        scsi_fast_reads = 0;
    else if (shell_streq(cmd, "scsi") && argv[1] && shell_streq(argv[1], "fast"))
        scsi_fast_reads = 1;
    else if (shell_streq(cmd, "scsi"))
        cmd_scsi();
    else if (shell_streq(cmd, "block"))
        cmd_block(argv[1], argv[2]);
    else if (shell_streq(cmd, "speed"))
        cmd_speed(argv[1]);
    else if (shell_streq(cmd, "wtest"))
        cmd_wtest(argv[1]);
    else if (shell_streq(cmd, "disk"))
        cmd_disk();
    else if (shell_streq(cmd, "format"))
        cmd_format(argv[1]);
    else if (shell_streq(cmd, "ls"))
        cmd_ls();
    else if (shell_streq(cmd, "cat"))
        cmd_cat(argv[1]);
    else if (shell_streq(cmd, "rm"))
        cmd_rm(argv[1]);
    else if (shell_streq(cmd, "net"))
        cmd_net(argc, argv);
    else if (shell_streq(cmd, "ping"))
        cmd_ping(argv[1]);
    else if (shell_streq(cmd, "dns"))
        cmd_dns(argv[1]);
    else if (shell_streq(cmd, "set"))
        cmd_set(argc, argv);
    else if (shell_streq(cmd, "ssh"))
        cmd_ssh(argc, argv);
    else if (shell_streq(cmd, "desk"))
        cmd_desk();
    else if (shell_streq(cmd, "term")) {
        if (shell_need_network() == 0)
            cmd_term(argc, argv);
    } else if (shell_streq(cmd, "tcp"))
        cmd_tcp(argc, argv);
    else if (shell_streq(cmd, "tcptest"))
        cmd_tcptest(argc, argv);
    else if (shell_streq(cmd, "bench"))
        cmd_bench();
    else if (shell_streq(cmd, "termbench"))
        cmd_bench_term();
    else if (shell_streq(cmd, "font"))
        show_font();
    else if (shell_streq(cmd, "clear"))
        con_clear();
    else if (shell_streq(cmd, "crash"))
        __asm__ volatile("tst.w 0x100001");     /* word access to an odd address */
    else
        kprintf("Unknown command \"%s\". Type help for a list.\n", cmd);
}

/* The "start" setting names a program to go straight into: ssh or term.
 * Any key pressed in the first second keeps the OSConsole instead. */
static void autostart(char *line)
{
    const char *start = config_get("start");
    u32 until;
    int i;

    /* The home screen is where SSH OS starts unless told otherwise, and
     * it starts at once: nothing to wait for. */
    if (!start || !start[0] || shell_streq(start, "desk")) {
        cmd_desk();
        return;
    }
    if (!start || (!shell_streq(start, "term") && !shell_streq(start, "ssh")))
        return;
    kprintf("Starting %s in a moment. Press any key to stay in the OSConsole.\n", start);
    for (until = ticks + 90; (s32)(ticks - until) < 0;) {
        if (kbd_get_event() >= 0) {
            while (kbd_get_event() >= 0)
                ;
            return;
        }
        cpu_idle();
    }
    for (i = 0; start[i] && i < LINE_MAX - 1; i++)
        line[i] = start[i];
    line[i] = 0;
    run(line);
}

void shell_run(void)
{
    char line[LINE_MAX];

    net_abort_check = esc_pressed;
    autostart(line);
    saved_start();              /* the SSH OS disk, unless the home screen found it already */
    for (;;) {
        kputs("> ");
        read_line(line);
        kbd_escape = 0;
        run(line);
    }
}
