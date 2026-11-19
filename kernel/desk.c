/* desk: the home screen. Below the bar (bar.c, barmenu.c) there is an icon
 * for each thing SSH OS can do. Everything works with the mouse and with
 * the keyboard.
 *
 *   Left/Right, Return   choose an icon and open it
 *   J                    the SSH OS menu (Up/Down, Return; Esc closes)
 *   W                    the network panel
 *   M                    what uses the memory
 *   + and -              volume up and down (V: one step louder, then off)
 *
 * The screen is drawn once and then left alone: only the bar's figures are
 * redrawn, once a second, and whatever a click changes.
 *
 * SSH opens on a list of the connections used before (kept on the SSH OS
 * disk, saved.c) with "New connection" under them: Up/Down and Return or a
 * click choose, Delete forgets the one chosen, Esc goes back.
 */
#include "kernel.h"
#include "hw.h"
#include "net.h"
#include "shell.h"

static const struct {
    const char *label;
    const struct icon *icon;
    int x, y;                   /* top left of the icon */
} apps[] = {
    {"SSH", &icon_ssh, 104, 100},
    {"Hardware test", &icon_hardware, 236, 96},
    {"Settings", &icon_settings, 362, 100},
};
#define APPS ((int)(sizeof apps / sizeof apps[0]))

static int selected;
static u8 net_tried;
static u8 leave;                /* BAR_HOME or BAR_CONSOLE chosen in the menu while in a list */

static void note(const char *s)
{
    serial_puts("DESK: ");
    serial_puts(s);
    serial_puts("\n");
}

static int label_x(int i)
{
    return apps[i].x + apps[i].icon->w / 2 - gfx_text_width(apps[i].label, 1) / 2;
}

static void draw_app(int i)
{
    const struct icon *ic = apps[i].icon;
    int on = i == selected, lx = label_x(i), ly = apps[i].y + ic->h + 12;

    gfx_begin();
    gfx_fill(apps[i].x - 8, apps[i].y - 8, ic->w + 16, ic->h + 16, on ? GFX_BLACK : GFX_WHITE);
    gfx_icon(apps[i].x, apps[i].y, ic, on);
    gfx_fill(lx - 4, ly - 2, gfx_text_width(apps[i].label, 1) + 8, 14, on ? GFX_BLACK : GFX_WHITE);
    gfx_text(lx, ly, apps[i].label, on, 1, 0);
    gfx_end();
}

/* Which icon is at this point, or -1. */
static int app_at(int x, int y)
{
    int i;

    for (i = 0; i < APPS; i++)
        if (x >= apps[i].x - 24 && x < apps[i].x + apps[i].icon->w + 24 &&
            y >= apps[i].y - 8 && y < apps[i].y + apps[i].icon->h + 28)
            return i;
    return -1;
}

static void draw_home(void)
{
    int i;

    con_clear();
    for (i = 0; i < APPS; i++)
        draw_app(i);
    bar_draw();
}

void cmd_hwtest(void);

/* ------------------------------------------------- the list of connections -- */

#define LIST_X      96
#define LIST_W      320
#define LIST_Y      (BAR_H + 96)
#define LIST_ROW    16
#define LIST_MAX    (RECENT_MAX + 2)

static const char *choices[LIST_MAX];
static int nchoices;
static u8 has_setting;          /* the first choice is the setting "ssh", not a saved one */

/* Is each one alive? Every saved connection is tried when the list opens,
 * one after the other: a connection to its SSH port, dropped as soon as it
 * is answered. That says more than a ping would: the machine is up, the way
 * to it is open, and something there is listening for SSH. */
enum { ALIVE_WAITING, ALIVE_CHECKING, ALIVE_YES, ALIVE_NO_ANSWER, ALIVE_NO_SSH, ALIVE_NO_NAME,
       ALIVE_OFFLINE };
static u8 alive[LIST_MAX];
static u16 alive_ms[LIST_MAX];
static int probing = -1, probe_conn = -1;
static u32 probe_started;

static const u16 dot_full[8] = {0x3C00, 0x7E00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0x7E00, 0x3C00};
static const u16 dot_ring[8] = {0x3C00, 0x4200, 0x8100, 0x8100, 0x8100, 0x8100, 0x4200, 0x3C00};
static const u16 dot_wait[8] = {0x0000, 0x0000, 0x0000, 0xDB00, 0xDB00, 0x0000, 0x0000, 0x0000};

static void draw_choice(int i, int on)
{
    char shown[40], status[12];
    u32 n = strlen(choices[i]);
    int y = LIST_Y + i * LIST_ROW, last = i == nchoices - 1, k = 0;
    u32 v;

    /* what fits; a long name is cut at the end */
    memcpy(shown, choices[i], n < 34 ? n + 1 : 34);
    shown[34] = 0;
    gfx_begin();
    gfx_fill(LIST_X, y, LIST_W, LIST_ROW, on ? GFX_BLACK : GFX_WHITE);
    gfx_text(LIST_X + 24, y + 3, shown, on, 1, last);
    if (!last && alive[i] != ALIVE_OFFLINE) {
        gfx_picture(LIST_X + 8, y + 4, alive[i] == ALIVE_YES ? dot_full :
                    alive[i] <= ALIVE_CHECKING ? dot_wait : dot_ring, 8, on, 1);
        status[0] = 0;
        if (alive[i] == ALIVE_YES) {
            v = alive_ms[i];
            if (v >= 1000) status[k++] = (char)('0' + v / 1000 % 10);
            if (v >= 100) status[k++] = (char)('0' + v / 100 % 10);
            if (v >= 10) status[k++] = (char)('0' + v / 10 % 10);
            status[k++] = (char)('0' + v % 10);
            memcpy(status + k, " ms", 4);
        } else if (alive[i] == ALIVE_NO_ANSWER) {
            memcpy(status, "no answer", 10);
        } else if (alive[i] == ALIVE_NO_SSH) {
            memcpy(status, "no SSH", 7);
        } else if (alive[i] == ALIVE_NO_NAME) {
            memcpy(status, "not found", 10);
        }
        gfx_text(LIST_X + LIST_W - 8 - gfx_text_width(status, 1), y + 3, status, on, 1, 0);
    }
    gfx_end();
}

static void probe_stop(void)
{
    if (probe_conn >= 0)
        tcp_abort(probe_conn);
    probe_conn = -1;
    probing = -1;
}

/* One step of the checking; at is the line that is lit. */
static void probe_step(int at)
{
    char host[64];
    const char *s, *p;
    u32 ip, port = 0;
    int i, n = 0;

    if (probing >= 0) {
        if (!tcp_connecting(probe_conn)) {
            if (tcp_is_open(probe_conn)) {
                u32 ms = (ticks - probe_started) * 1000 / 60;

                alive[probing] = ALIVE_YES;
                alive_ms[probing] = (u16)(ms > 9999 ? 9999 : ms ? ms : 1);
            } else {
                alive[probing] = ALIVE_NO_SSH;
            }
        } else if (ticks - probe_started > 4 * 60) {
            alive[probing] = ALIVE_NO_ANSWER;
        } else {
            return;
        }
        i = probing;
        probe_stop();
        draw_choice(i, i == at);
        return;
    }
    for (i = 0; i < nchoices - 1 && alive[i] != ALIVE_WAITING; i++)
        ;
    if (i >= nchoices - 1)
        return;
    /* user@host:port */
    s = choices[i];
    for (p = s; *p; p++)
        if (*p == '@')
            s = p + 1;
    while (*s && *s != ':' && n < (int)sizeof host - 1)
        host[n++] = *s++;
    host[n] = 0;
    if (*s == ':')
        for (s++; *s >= '0' && *s <= '9'; s++)
            port = port * 10 + (u32)(*s - '0');
    alive[i] = ALIVE_CHECKING;
    draw_choice(i, i == at);
    if (net_resolve(host, &ip) != 0) {
        alive[i] = ALIVE_NO_NAME;
    } else if ((probe_conn = tcp_open(ip, (u16)(port ? port : 22))) < 0) {
        alive[i] = ALIVE_NO_ANSWER;
        probe_conn = -1;
    } else {
        probing = i;
        probe_started = ticks;
        return;
    }
    draw_choice(i, i == at);
}

static void build_choices(void)
{
    const char *setting = config_get("ssh");
    int i;

    nchoices = 0;
    has_setting = 0;
    if (setting && setting[0]) {
        for (i = 0; i < recent_count(); i++)
            if (strcmp(recent_get(i), setting) == 0)
                break;
        if (i == recent_count()) {
            choices[nchoices++] = setting;
            has_setting = 1;
        }
    }
    for (i = 0; i < recent_count(); i++)
        choices[nchoices++] = recent_get(i);
    choices[nchoices++] = "New connection...";
    probe_stop();
    memset(alive, net.up && net.ip ? ALIVE_WAITING : ALIVE_OFFLINE, sizeof alive);
}

static void draw_choices(int at)
{
    int i;

    con_clear();
    gfx_begin();
    gfx_icon(SCREEN_W / 2 - icon_ssh.w / 2, BAR_H + 24, &icon_ssh, 0);
    gfx_text(SCREEN_W / 2 - gfx_text_width("Connect to", 1) / 2, BAR_H + 74, "Connect to", 0, 1, 1);
    gfx_end();
    for (i = 0; i < nchoices; i++)
        draw_choice(i, i == at);
}

/* Which connection? Returns its place in choices, or -1 to go back. The
 * last one is always "New connection". */
static int choose_target(void)
{
    u32 clicks = mouse.clicks;
    int at = 0, ev, r, hit;
    u8 ch, raw;

    build_choices();
    if (nchoices == 1)
        return 0;               /* nothing to choose from yet: straight to the question */
    draw_choices(at);
    note("connections");
    for (;;) {
        net_poll();
        probe_step(at);
        bar_tick();
        r = bar_poll();
        if (r == BAR_HOME || r == BAR_CONSOLE) {
            leave = (u8)r;
            probe_stop();
            return -1;
        }
        if (r != BAR_NONE)
            clicks = mouse.clicks;
        if (mouse.clicks != clicks) {
            clicks = mouse.clicks;
            hit = (mouse.click_y - LIST_Y) / LIST_ROW;
            if (mouse.click_y >= LIST_Y && hit < nchoices && mouse.click_x >= LIST_X &&
                mouse.click_x < LIST_X + LIST_W) {
                probe_stop();
                return hit;
            }
        }
        ev = kbd_get_event();
        if (ev < 0) {
            bar_idle();
            continue;
        }
        bar_work();
        if (ev & 0x8000)
            continue;
        ch = (u8)(ev & 0xFF);
        raw = (u8)((ev >> 8) & 0x7F);
        if (raw == 0x3D || raw == 0x3E) {               /* Down, Up */
            draw_choice(at, 0);
            at = raw == 0x3D ? (at + 1) % nchoices : (at + nchoices - 1) % nchoices;
            draw_choice(at, 1);
        } else if (ch == '\n') {
            probe_stop();
            return at;
        } else if (ch == 0x1B) {
            kbd_escape = 0;
            probe_stop();
            return -1;
        } else if (ch == '\b' && at < nchoices - 1 && !(has_setting && at == 0)) {
            recent_remove(at - has_setting);            /* forget this one */
            build_choices();
            if (at >= nchoices)
                at = nchoices - 1;
            draw_choices(at);
        }
    }
}

/* --------------------------------------------------------- a new connection -- */

#define FORM_X      116
#define FORM_W      280
#define FIELD_Y     (BAR_H + 116)
#define FIELD_H     22
#define BTN_Y       (BAR_H + 156)
#define BTN_H       22
#define BTN_W       84
#define BTN_OK_X    (FORM_X + FORM_W - BTN_W)
#define BTN_NO_X    (BTN_OK_X - BTN_W - 12)
#define FIELD_CHARS 42          /* how many fit in the field */

static void draw_field(const char *line, u32 n, int caret)
{
    /* The end of a long line is what shows. */
    const char *shown = n > FIELD_CHARS ? line + n - FIELD_CHARS : line;
    int w = gfx_text_width(shown, 1);

    gfx_begin();
    gfx_fill(FORM_X + 2, FIELD_Y + 2, FORM_W - 4, FIELD_H - 4, GFX_WHITE);
    gfx_text(FORM_X + 8, FIELD_Y + 6, shown, 0, 1, 0);
    if (caret)
        gfx_fill(FORM_X + 9 + w, FIELD_Y + 4, 1, FIELD_H - 8, GFX_BLACK);
    gfx_end();
}

/* Ask who to log in to, in a window of its own: a field to type in, and
 * Cancel and Connect (Esc and Return). Returns 1 with the answer in line
 * (of size bytes), or 0 to go back. With keep, what line holds is there to
 * be changed. */
static int new_connection(char *line, u32 size, int keep)
{
    static const char title[] = "New connection", label[] = "user@server";
    u32 clicks = mouse.clicks, n, blink = ticks;
    int ev, r, caret = 1, x, y;
    u8 ch;

    if (!keep)
        line[0] = 0;
    n = strlen(line);
    con_clear();
    gfx_begin();
    gfx_icon(SCREEN_W / 2 - icon_ssh.w / 2, BAR_H + 24, &icon_ssh, 0);
    gfx_text(SCREEN_W / 2 - gfx_text_width(title, 1) / 2, BAR_H + 74, title, 0, 1, 1);
    gfx_text(FORM_X, FIELD_Y - 14, label, 0, 1, 0);
    gfx_fill(FORM_X, FIELD_Y, FORM_W, FIELD_H, GFX_BLACK);
    ask_button(BTN_NO_X, BTN_Y, "Cancel", 0);
    ask_button(BTN_OK_X, BTN_Y, "Connect", 1);
    gfx_end();
    draw_field(line, n, caret);
    note("new connection");
    for (;;) {
        net_poll();
        bar_tick();
        r = bar_poll();
        if (r == BAR_HOME || r == BAR_CONSOLE) {
            leave = (u8)r;
            return 0;
        }
        if (r != BAR_NONE)
            clicks = mouse.clicks;
        if (mouse.clicks != clicks) {
            clicks = mouse.clicks;
            x = mouse.click_x;
            y = mouse.click_y;
            if (y >= BTN_Y && y < BTN_Y + BTN_H) {
                if (x >= BTN_NO_X && x < BTN_NO_X + BTN_W)
                    return 0;
                if (x >= BTN_OK_X && x < BTN_OK_X + BTN_W && n)
                    return 1;
            }
        }
        if (ticks - blink >= 30) {
            blink = ticks;
            caret = !caret;
            draw_field(line, n, caret);
        }
        ev = kbd_get_event();
        if (ev < 0) {
            bar_idle();
            continue;
        }
        bar_work();
        if (ev & 0x8000)
            continue;
        ch = (u8)(ev & 0xFF);
        if (ch == 0x1B) {
            kbd_escape = 0;
            return 0;
        } else if (ch == '\n') {
            if (n)
                return 1;
            continue;
        } else if (ch == '\b') {
            if (!n)
                continue;
            line[--n] = 0;
        } else if (ch > ' ' && ch < 0x7F && n + 1 < size) {
            line[n++] = (char)ch;
            line[n] = 0;
        } else {
            continue;
        }
        caret = 1;
        blink = ticks;
        draw_field(line, n, caret);
    }
}

/* Leave the home screen for a program with a text screen, then come back. */
static void run_app(int i)
{
    char line[120];
    int which, failed;
    u32 n;

    if (i == 0) {
        note("ssh");
        /* A connection that fails comes back to where it was asked for:
         * the list, or the New connection window with what was typed. */
        for (;;) {
            which = choose_target();
            if (which < 0)
                return;
            line[0] = 0;
            if (which < nchoices - 1) {
                n = strlen(choices[which]);
                if (n < sizeof line)
                    memcpy(line, choices[which], n + 1);
                if (!line[0] || !connect_run(line))
                    break;
                continue;
            }
            failed = 0;
            while (new_connection(line, sizeof line, failed) && (failed = connect_run(line)) != 0)
                ;
            break;
        }
        note("back from ssh");
    } else if (i == 1) {
        note("hardware test");
        con_clear();
        cmd_hwtest();
    } else {
        leave = (u8)settings_run();
    }
    while (kbd_get_event() >= 0)
        ;
    kbd_escape = 0;
}

/* The SSH OS disk (settings, connections) and the network. Start-up does
 * this under its last picture; it happens once. */
void desk_network_start(void)
{
    if (net_tried)
        return;
    net_tried = 1;
    saved_start();
    /* Start-up should not stand still for long when there is no network:
     * ask for an address once (2 seconds), twice after joining a network.
     * Anything that needs the network later asks again, with patience. */
    net_dhcp_tries = 1;
    shell_network_start();
    if (wifi_restore()) {               /* the network joined last time */
        note("back on the saved network");
        net_stop();
        net_dhcp_tries = 2;
        shell_network_start();
    }
    net_dhcp_tries = 4;
    wifi_refresh();
    note(net.up && net.ip ? "network up" : "no network");
}

void cmd_desk(void)
{
    u32 seen_clicks = mouse.clicks, polled = 0;
    int ev, quit = 0, redraw = 1, hit, r;
    u8 ch, raw;

    selected = 0;
    note("home");

    while (!quit) {
        int worked = redraw;

        if (redraw) {
            draw_home();
            seen_clicks = mouse.clicks;
            redraw = 0;
            while (kbd_get_event() >= 0)
                ;
        }

        desk_network_start();           /* unless start-up did it already */
        /* Nothing here waits for the network, and one look at the adapter
         * costs several hundredths of a second, so look twice a second and
         * leave the processor asleep the rest of the time. */
        if (ticks - polled >= 30) {
            polled = ticks;
            net_poll();
            worked = 1;
            /* No address yet (the WiFi was not up when we started, say):
             * keep asking on the side, without holding up keys and mouse. */
            if (net.up && !net.ip && shell_network_by_dhcp()) {
                r = net_dhcp_step();
                if (r < 0) {
                    net_dhcp_begin();
                } else if (r == 1) {
                    wifi_refresh();
                    bar_draw();
                    note("network up, late");
                }
            }
        }
        bar_tick();

        r = bar_poll();                         /* the bar's own clicks */
        if (r == BAR_CONSOLE || leave == BAR_CONSOLE)
            quit = 1;
        leave = 0;
        if (r != BAR_NONE)
            seen_clicks = mouse.clicks;

        if (mouse.clicks != seen_clicks) {
            seen_clicks = mouse.clicks;
            worked = 1;
            if ((hit = app_at(mouse.click_x, mouse.click_y)) >= 0) {
                selected = hit;
                run_app(hit);
                redraw = 1;
            }
        }

        while (!quit && !redraw && (ev = kbd_get_event()) >= 0) {
            if (ev & 0x8000)
                continue;
            ch = (u8)(ev & 0xFF);
            raw = (u8)((ev >> 8) & 0x7F);
            worked = 1;
            if (raw == 0x3B || raw == 0x3C || ch == '\t') {             /* Left, Right */
                int old = selected;

                selected = raw == 0x3B ? (selected + APPS - 1) % APPS : (selected + 1) % APPS;
                draw_app(old);
                draw_app(selected);
            } else if (ch == '\n') {
                run_app(selected);
                redraw = 1;
            } else if (ch == 'j' || ch == 'J') {
                if (bar_menu() == BAR_CONSOLE)
                    quit = 1;
                seen_clicks = mouse.clicks;
            } else if (ch == 'w' || ch == 'W') {
                bar_panel();
                seen_clicks = mouse.clicks;
            } else if (ch == 'm' || ch == 'M') {
                bar_memory();
                seen_clicks = mouse.clicks;
            } else if (ch == 'v' || ch == 'V') {
                bar_volume_step();
            } else if (ch == '+' || ch == '=') {
                bar_volume_change(1);
            } else if (ch == '-') {
                bar_volume_change(-1);
            }
        }
        if (worked)
            bar_work();
        if (!quit && !redraw)
            bar_idle();
    }

    con_clear();
    kbd_escape = 0;
    kputs("OSConsole. Type help for the commands; type desk, or choose Home in the\n"
          "SSH OS menu, to go back.\n");
}
