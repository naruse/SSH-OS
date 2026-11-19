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

/* The dots the New connection window shows beside what Ping found. */
static const u16 dot_full[8] = {0x3C00, 0x7E00, 0xFF00, 0xFF00, 0xFF00, 0xFF00, 0x7E00, 0x3C00};
static const u16 dot_ring[8] = {0x3C00, 0x4200, 0x8100, 0x8100, 0x8100, 0x8100, 0x4200, 0x3C00};
static const u16 dot_wait[8] = {0x0000, 0x0000, 0x0000, 0xDB00, 0xDB00, 0x0000, 0x0000, 0x0000};

static void draw_choice(int i, int on)
{
    char shown[48];
    u32 n = strlen(choices[i]);
    int y = LIST_Y + i * LIST_ROW;

    /* what fits; a long name is cut at the end */
    memcpy(shown, choices[i], n < 44 ? n + 1 : 44);
    shown[44] = 0;
    gfx_begin();
    gfx_fill(LIST_X, y, LIST_W, LIST_ROW, on ? GFX_BLACK : GFX_WHITE);
    gfx_text(LIST_X + 8, y + 3, shown, on, 1, i == nchoices - 1);
    gfx_end();
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
        bar_tick();
        r = bar_poll();
        if (r == BAR_HOME || r == BAR_CONSOLE) {
            leave = (u8)r;
            return -1;
        }
        if (r != BAR_NONE)
            clicks = mouse.clicks;
        if (mouse.clicks != clicks) {
            clicks = mouse.clicks;
            hit = (mouse.click_y - LIST_Y) / LIST_ROW;
            if (mouse.click_y >= LIST_Y && hit < nchoices && mouse.click_x >= LIST_X &&
                mouse.click_x < LIST_X + LIST_W) {
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
            return at;
        } else if (ch == 0x1B) {
            kbd_escape = 0;
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
#define RESULT_Y    (BAR_H + 146)      /* what Ping found */
#define BTN_Y       (BAR_H + 172)
#define BTN_H       22
#define BTN_W       84
#define BTN_OK_X    (FORM_X + FORM_W - BTN_W)
#define BTN_NO_X    (BTN_OK_X - BTN_W - 12)
#define BTN_PING_X  FORM_X
#define FIELD_CHARS 42          /* how many fit in the field */

static u32 field_first;         /* the first character that shows, of a line longer than the field */

/* The field with the caret before character `at` (lit or not). A long line
 * slides so that the caret is always in sight. */
static void draw_field(const char *line, u32 n, u32 at, int caret)
{
    char shown[FIELD_CHARS + 1];
    u32 k;

    if (at < field_first)
        field_first = at;
    if (at > field_first + FIELD_CHARS)
        field_first = at - FIELD_CHARS;
    if (n <= FIELD_CHARS)
        field_first = 0;
    k = n - field_first;
    if (k > FIELD_CHARS)
        k = FIELD_CHARS;
    memcpy(shown, line + field_first, k);
    shown[k] = 0;
    gfx_begin();
    gfx_fill(FORM_X + 2, FIELD_Y + 2, FORM_W - 4, FIELD_H - 4, GFX_WHITE);
    gfx_text(FORM_X + 8, FIELD_Y + 6, shown, 0, 1, 0);
    if (caret)
        gfx_fill(FORM_X + 8 + (int)(at - field_first) * 6, FIELD_Y + 4, 1, FIELD_H - 8, GFX_BLACK);
    gfx_end();
}

/* The line under the field: what Ping found (result one of KNOCK_, or -1
 * for "checking", or -2 to clear it). */
static void draw_result(int result, u32 ms)
{
    char text[48];

    gfx_begin();
    gfx_fill(FORM_X, RESULT_Y - 2, FORM_W, 14, GFX_WHITE);
    if (result > -2) {
        if (result < 0)
            memcpy(text, "Checking", 9);
        else
            connect_knock_text(result, ms, text);
        gfx_picture(FORM_X + 2, RESULT_Y + 1, result < 0 ? dot_wait :
                    result == KNOCK_ANSWERS ? dot_full : dot_ring, 8, 0, 1);
        gfx_text(FORM_X + 16, RESULT_Y, text, 0, 1, 0);
    }
    gfx_end();
}

static void ping(const char *line)
{
    char said[] = "DESK: ping 0\n";
    u32 ms;
    int r;

    draw_result(-1, 0);
    r = connect_knock(line, &ms);
    draw_result(r, ms);
    said[11] = (char)('0' + r);
    serial_puts(said);
    while (kbd_get_event() >= 0)        /* keys pressed meanwhile were for nobody */
        ;
}

/* Ask who to log in to, in a window of its own: a field to type in, and
 * Cancel and Connect (Esc and Return), and Ping (Tab), which says whether
 * the server is there without connecting. Returns 1 with the answer in line
 * (of size bytes), or 0 to go back. With keep, what line holds is there to
 * be changed. */
static int new_connection(char *line, u32 size, int keep)
{
    static const char title[] = "New connection", label[] = "user@server";
    u32 clicks = mouse.clicks, n, at, blink = ticks;
    int ev, r, caret = 1, x, y;
    u8 ch, raw;

    if (!keep)
        line[0] = 0;
    at = n = strlen(line);
    field_first = 0;
    con_clear();
    gfx_begin();
    gfx_icon(SCREEN_W / 2 - icon_ssh.w / 2, BAR_H + 24, &icon_ssh, 0);
    gfx_text(SCREEN_W / 2 - gfx_text_width(title, 1) / 2, BAR_H + 74, title, 0, 1, 1);
    gfx_text(FORM_X, FIELD_Y - 14, label, 0, 1, 0);
    gfx_fill(FORM_X, FIELD_Y, FORM_W, FIELD_H, GFX_BLACK);
    ask_button(BTN_PING_X, BTN_Y, "Ping", 0);
    ask_button(BTN_NO_X, BTN_Y, "Cancel", 0);
    ask_button(BTN_OK_X, BTN_Y, "Connect", 1);
    gfx_end();
    draw_field(line, n, at, caret);
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
                if (x >= BTN_PING_X && x < BTN_PING_X + BTN_W && n) {
                    ping(line);
                    clicks = mouse.clicks;
                }
            } else if (y >= FIELD_Y && y < FIELD_Y + FIELD_H && x >= FORM_X && x < FORM_X + FORM_W) {
                /* a click in the field puts the caret between the letters there */
                x = (x - FORM_X - 8 + 3) / 6;
                at = field_first + (u32)(x < 0 ? 0 : x);
                if (at > n)
                    at = n;
                caret = 1;
                blink = ticks;
                draw_field(line, n, at, caret);
            }
        }
        if (ticks - blink >= 30) {
            blink = ticks;
            caret = !caret;
            draw_field(line, n, at, caret);
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
        if (raw == 0x3B || raw == 0x3C) {               /* Left, Right: the caret */
            if (raw == 0x3B && at)
                at--;
            if (raw == 0x3C && at < n)
                at++;
            caret = 1;
            blink = ticks;
            draw_field(line, n, at, caret);
            continue;
        }
        if (ch == 0x1B) {
            kbd_escape = 0;
            return 0;
        } else if (ch == '\n') {
            if (n)
                return 1;
            continue;
        } else if (ch == '\t') {
            if (n)
                ping(line);
            clicks = mouse.clicks;
            continue;
        } else if (ch == '\b') {
            if (!at)
                continue;
            memmove(line + at - 1, line + at, n - at + 1);     /* the one before the caret */
            at--;
            n--;
        } else if (ch > ' ' && ch < 0x7F && n + 1 < size) {
            memmove(line + at + 1, line + at, n - at + 1);
            line[at++] = (char)ch;
            n++;
        } else {
            continue;
        }
        caret = 1;
        blink = ticks;
        draw_field(line, n, at, caret);
        draw_result(-2, 0);             /* it was about what stood there before */
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
