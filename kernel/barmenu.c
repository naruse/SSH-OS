/* What the bar does when clicked, in whatever program is running: the
 * SSH OS menu, the About window, the network panel and the volume.
 *
 * Each of these opens over the screen as it is, keeps the pixels it covers
 * and puts them back when it closes, so the program underneath need not
 * know. While one is open the program waits; the network is still answered.
 *
 * A program's main loop calls bar_poll(), which returns at once unless the
 * bar was clicked.
 */
#include "kernel.h"
#include "hw.h"
#include "net.h"
#include "shell.h"

static const char *const menu_items[] = {"About SSH OS", "Home", "OSConsole", "Restart", "Shut down"};
#define MENU_ITEMS  5
#define MENU_W      110
#define MENU_ROW    15

static u32 seen_clicks;         /* clicks up to here have been dealt with */

static void note(const char *s)
{
    serial_puts("DESK: ");
    serial_puts(s);
    serial_puts("\n");
}

static void put_num(char *out, u32 v, int digits)
{
    while (digits--) {
        out[digits] = (char)('0' + v % 10);
        v /= 10;
    }
}

static void put_count(char *out, u32 v)
{
    char rev[12];
    int n = 0;

    do {
        rev[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *out++ = rev[--n];
    *out = 0;
}

/* A white box with a black edge and a shadow. The screen under it is kept
 * first; close_box() puts it back. One box at a time. */
static void open_box(int x, int y, int w, int h)
{
    gfx_save(x, y, w + 3, h + 3);
    gfx_fill(x + 3, y + 3, w, h, GFX_BLACK);
    gfx_fill(x, y, w, h, GFX_BLACK);
    gfx_fill(x + 1, y + 1, w - 2, h - 2, GFX_WHITE);
}

static void close_box(void)
{
    gfx_begin();
    gfx_restore();
    gfx_end();
    bar_highlight(0, 0);
    seen_clicks = mouse.clicks;
    while (kbd_get_event() >= 0)
        ;
    kbd_escape = 0;
}

/* One turn of waiting while something is open: 1 = a click (at mouse.click_x,
 * click_y), 2 = a key (in *ch and *raw), 0 = neither yet. */
static int event(u8 *ch, u8 *raw)
{
    static u32 polled;
    int ev;

    bar_tick();
    if (ticks - polled >= 30) {
        polled = ticks;
        net_poll();
    }
    if (mouse.clicks != seen_clicks) {
        seen_clicks = mouse.clicks;
        bar_work();
        return 1;
    }
    while ((ev = kbd_get_event()) >= 0) {
        if (ev & 0x8000)
            continue;
        *ch = (u8)(ev & 0xFF);
        *raw = (u8)((ev >> 8) & 0x7F);
        if (*ch || *raw == 0x3D || *raw == 0x3E) {
            bar_work();
            return 2;
        }
    }
    bar_idle();
    return 0;
}

static void label(int x, int y, const char *name, const char *value)
{
    gfx_text(x, y, name, 0, 1, 0);
    gfx_text(x + 66, y, value, 0, 1, 0);
}

/* ----------------------------------------------------------------- About -- */

static void about(void)
{
    int x = 96, y = 76, w = 320, h = 166;
    char text[40];
    u32 kb = (u32)(__bss_end - __kernel_start) / 1024;
    u8 ch, raw;

    gfx_begin();
    open_box(x, y, w, h);
    gfx_fill(x + 1, y + 1, w - 2, 16, GFX_BLACK);
    gfx_text(x + w / 2 - 36, y + 4, "About SSH OS", 1, 1, 1);
    gfx_icon(x + 22, y + 34, &icon_logo48, 0);
    gfx_text(x + 92, y + 30, "SSH OS " SSHOS_VERSION, 0, 2, 1);
    label(x + 92, y + 62, "Author", SSHOS_AUTHOR);
    label(x + 92, y + 76, "Source", SSHOS_SOURCE);
    label(x + 92, y + 100, "Computer", "Macintosh SE");
    put_num(text, machine.mem_top / 1024, 4);
    memcpy(text + 4, " K", 3);
    label(x + 92, y + 114, "Memory", text);
    put_num(text, kb, 3);
    memcpy(text + 3, " K in use", 10);
    label(x + 92, y + 128, "System", text);
    label(x + 92, y + 142, "ROM", machine.rom_version == 0x0276 ? "SE FDHD (0276)" : "other");
    gfx_end();
    note("about open");
    while (!event(&ch, &raw))
        ;
    close_box();
}

/* ------------------------------------------------------ Restart, Shut down -- */

static void restart(void)
{
    note("restart");
    cpu_irq_disable();
    /* Start the ROM again as after power-on: its first two long words are
     * the stack pointer and the address to begin at. */
    __asm__ volatile("movea.l %0,%%a0\n\tmovea.l (%%a0),%%sp\n\tmovea.l 4(%%a0),%%a0\n\tjmp (%%a0)"
                     : : "g"(machine.rom_base));
    for (;;)
        ;
}

static void shut_down(void)
{
    const char *a = "You can switch off your Macintosh now.";
    const char *b = "Any key starts it again.";
    u8 ch, raw;

    note("shut down");
    gfx_pointer(0);
    net_stop();
    gfx_fill(0, 0, SCREEN_W, SCREEN_H, GFX_GRAY);
    gfx_fill(99, 133, 320, 70, GFX_BLACK);
    gfx_fill(96, 130, 320, 70, GFX_BLACK);
    gfx_fill(97, 131, 318, 68, GFX_WHITE);
    gfx_text(256 - gfx_text_width(a, 1) / 2, 150, a, 0, 1, 1);
    gfx_text(256 - gfx_text_width(b, 1) / 2, 172, b, 0, 1, 0);
    while (kbd_get_event() >= 0)
        ;
    seen_clicks = mouse.clicks;
    for (;;) {
        int ev = kbd_get_event();

        if ((ev >= 0 && !(ev & 0x8000) && (ev & 0xFF)) || mouse.clicks != seen_clicks)
            break;
        cpu_idle();
    }
    (void)ch;
    (void)raw;
    restart();
}

/* ------------------------------------------------------------- the menu -- */

static void draw_item(int i, int on)
{
    int y = BAR_H + 3 + i * MENU_ROW;

    gfx_begin();
    gfx_fill(2, y, MENU_W - 4, MENU_ROW, on ? GFX_BLACK : GFX_WHITE);
    gfx_text(10, y + 3, menu_items[i], on, 1, 0);
    gfx_end();
}

static int item_at(int x, int y)
{
    int i = (y - BAR_H - 3) / MENU_ROW;

    return (x >= 0 && x < MENU_W && y >= BAR_H + 3 && i < MENU_ITEMS) ? i : -1;
}

int bar_menu(void)
{
    u32 moves = mouse.moves - 1;
    int at = -1, hit, i, got, chosen = -1;
    u8 ch = 0, raw = 0;

    bar_highlight(1, 0);
    gfx_begin();
    open_box(0, BAR_H, MENU_W, MENU_ITEMS * MENU_ROW + 6);
    gfx_end();
    for (i = 0; i < MENU_ITEMS; i++)
        draw_item(i, 0);
    note("menu open");

    for (;;) {
        if (mouse.moves != moves) {             /* the menu follows the pointer */
            moves = mouse.moves;
            hit = item_at(mouse.x, mouse.y);
            if (hit != at) {
                if (at >= 0)
                    draw_item(at, 0);
                if (hit >= 0)
                    draw_item(hit, 1);
                at = hit;
            }
        }
        got = event(&ch, &raw);
        if (got == 1) {
            chosen = item_at(mouse.click_x, mouse.click_y);
            break;
        }
        if (got == 2 && (raw == 0x3D || raw == 0x3E)) {         /* Down, Up */
            if (at >= 0)
                draw_item(at, 0);
            at = raw == 0x3D ? (at + 1) % MENU_ITEMS : (at + MENU_ITEMS - 1 + (at < 0)) % MENU_ITEMS;
            draw_item(at, 1);
        } else if (got == 2) {
            if (ch == '\n')
                chosen = at;
            break;
        }
    }
    close_box();
    switch (chosen) {
    case 0:
        about();
        break;
    case 1:
        note("home");
        return BAR_HOME;
    case 2:
        note("console");
        return BAR_CONSOLE;
    case 3:
        restart();
        break;
    case 4:
        shut_down();
        break;
    }
    return BAR_USED;
}

/* ---------------------------------------------------- the network panel -- */

#define PANEL_X     262
#define PANEL_W     246
#define LIST_Y      (BAR_H + 96)
#define LIST_ROW    14
#define LIST_MAX    8
#define PANEL_H     (LIST_Y - BAR_H + LIST_MAX * LIST_ROW + 24)

static struct wifi_network found[WIFI_MAX_NETWORKS];
static int nfound;
static u8 scan;                 /* 1 looking, 2 have the list, 3 the adapter cannot */

/* The last line of the panel, for hints and for the password. */
static void panel_message(const char *a, const char *b)
{
    int y = LIST_Y + (scan == 2 && nfound > 0 ? nfound : 1) * LIST_ROW + 4;

    gfx_begin();
    gfx_fill(PANEL_X + 1, y - 1, PANEL_W - 2, 12, GFX_WHITE);
    gfx_text(PANEL_X + 10, y, a, 0, 1, 0);
    gfx_text(PANEL_X + 10 + gfx_text_width(a, 1), y, b, 0, 1, 0);
    gfx_end();
}

static void draw_panel(void)
{
    static const struct icon *const fans[4] = {&icon_wifi1, &icon_wifi2, &icon_wifi3, &icon_wifi4};
    int x = PANEL_X, y = BAR_H, w = PANEL_W, up = net.up && net.ip, i;
    char text[48], count[12];

    gfx_begin();
    gfx_fill(x + 1, y + 1, w - 2, PANEL_H - 2, GFX_WHITE);
    if (up && wifi_now.name[0]) {
        gfx_text(x + 10, y + 8, wifi_now.name, 0, 1, 1);
        memcpy(text, "-00 dBm, channel 00", 20);
        put_num(text + 1, (u32)(-wifi_now.signal), 2);
        put_num(text + 17, wifi_now.channel, 2);
        label(x + 10, y + 24, "Signal", text);
    } else {
        gfx_text(x + 10, y + 8, up ? "Connected" : "Not connected", 0, 1, 1);
    }
    if (up) {
        ip_text(text, net.ip);
        label(x + 10, y + 38, "Address", text);
        ip_text(text, net.gateway);
        label(x + 10, y + 52, "Gateway", text);
        put_count(text, net_stats.rx_frames);
        put_count(count, net_stats.tx_frames);
        memcpy(text + strlen(text), " in, ", 6);
        memcpy(text + strlen(text), count, strlen(count) + 1);
        memcpy(text + strlen(text), " out", 5);
        label(x + 10, y + 66, "Packets", text);
    } else {
        gfx_text(x + 10, y + 38, "No network adapter answered, or", 0, 1, 0);
        gfx_text(x + 10, y + 52, "it has no address yet.", 0, 1, 0);
    }
    gfx_fill(x + 1, LIST_Y - 8, w - 2, 1, GFX_BLACK);
    if (scan == 2 && nfound > 0) {
        for (i = 0; i < nfound; i++) {
            int ry = LIST_Y + i * LIST_ROW;

            gfx_icon(x + 10, ry, fans[wifi_bars(found[i].signal) - 1], 0);
            memcpy(text, found[i].name, 25);
            text[25] = 0;
            gfx_text(x + 34, ry + 1, text, 0, 1, strcmp(found[i].name, wifi_now.name) == 0);
            gfx_text(x + w - 60, ry + 1, found[i].secured ? "password" : "open", 0, 1, 0);
        }
        gfx_end();
        panel_message("Click a network to join it", "");
        return;
    }
    gfx_text(x + 10, LIST_Y + 1, scan == 3 ? "This adapter cannot list networks" :
             scan == 2 ? "No networks found" : "Looking for networks...", 0, 1, 0);
    if (scan == 3)
        gfx_text(x + 10, LIST_Y + LIST_ROW + 4, "(a real BlueSCSI can)", 0, 1, 0);
    gfx_end();
}

/* Join the network in that row; its password is typed into the panel. */
static void join(int i)
{
    static char password[64];
    char dots[34];
    int len = 0, got, tries;
    u8 ch = 0, raw = 0;

    password[0] = 0;
    if (found[i].secured) {
        for (;;) {
            memset(dots, 0x9B, sizeof dots);            /* a bullet for each character */
            dots[len < 30 ? len : 30] = '_';
            dots[(len < 30 ? len : 30) + 1] = 0;
            panel_message("Password: ", dots);
            while ((got = event(&ch, &raw)) == 0)
                ;
            if (got == 1 || ch == 0x1B) {
                memset(password, 0, sizeof password);
                return;
            }
            if (ch == '\n')
                break;
            if (ch == '\b' && len > 0)
                len--;
            else if (ch >= 32 && ch < 127 && len < (int)sizeof password - 1)
                password[len++] = (char)ch;
            password[len] = 0;
        }
    }
    panel_message("Joining ", found[i].name);
    note("joining");
    if (wifi_join(found[i].name, password) != 0) {
        panel_message("The adapter refused.", "");
    } else {
        /* It changes over on its own; ask now and then whether it is there. */
        for (tries = 0; tries < 20; tries++) {
            delay_ticks(60);
            bar_tick();
            if (wifi_current(&wifi_now) == 0 && strcmp(wifi_now.name, found[i].name) == 0)
                break;
        }
        note(tries < 20 ? "joined" : "not joined");
        if (tries < 20) {
            config_store("wifi", found[i].name);        /* to come back to it after a restart */
            config_store("wifi_key", password);
            net_stop();         /* our address belonged to the old network: start again */
            shell_network_start();
        }
        wifi_refresh();
        bar_highlight(0, 1);
        draw_panel();
        panel_message(tries < 20 ? "Joined." : "Not joined: wrong password, or out of range.", "");
    }
    memset(password, 0, sizeof password);
    delay_ticks(90);
}

void bar_panel(void)
{
    u32 checked = ticks, count;
    int got, done, row;
    u8 ch = 0, raw = 0;

    wifi_refresh();
    scan = wifi_scan_start() == 0 ? 1 : 3;
    nfound = 0;
    bar_highlight(0, 1);
    gfx_begin();
    open_box(PANEL_X, BAR_H, PANEL_W, PANEL_H);
    gfx_end();
    draw_panel();
    count = net_stats.rx_frames + net_stats.tx_frames;
    note(scan == 3 ? "network panel open, no list" : "network panel open");

    for (;;) {
        if (scan == 1 && ticks - checked >= 30) {       /* fetch the list once it is there */
            checked = ticks;
            done = wifi_scan_done();
            if (done) {
                nfound = done > 0 ? wifi_scan_results(found, LIST_MAX) : -1;
                scan = nfound >= 0 ? 2 : 3;
                draw_panel();
            }
        } else if (net_stats.rx_frames + net_stats.tx_frames != count) {
            count = net_stats.rx_frames + net_stats.tx_frames;
            draw_panel();
        }
        got = event(&ch, &raw);
        if (got == 2)
            break;
        if (got == 1) {
            row = (mouse.click_y - LIST_Y) / LIST_ROW;
            if (scan == 2 && mouse.click_x >= PANEL_X && mouse.click_y >= LIST_Y && row < nfound) {
                join(row);
                seen_clicks = mouse.clicks;
                continue;
            }
            break;
        }
    }
    close_box();
}

static void volume_to(int v)
{
    if (v != snd_volume()) {
        snd_set_volume(v);
        snd_keep_volume();
    }
    serial_puts("DESK: volume ");
    serial_putc((char)('0' + snd_volume()));
    serial_puts("\n");
    snd_beep(8);
    bar_draw();
}

void bar_volume_step(void)
{
    volume_to((snd_volume() + 1) % 8);      /* one step louder each time, then off */
}

void bar_volume_change(int by)
{
    int v = snd_volume() + by;

    volume_to(v < 0 ? 0 : v > 7 ? 7 : v);
}

/* Has the bar been clicked? If so, do what was asked. Returns BAR_NONE, or
 * BAR_USED when something was opened and closed again, or BAR_HOME or
 * BAR_CONSOLE when the menu asks the program to go there. */
int bar_poll(void)
{
    int x, y;

    if (mouse.clicks == seen_clicks)
        return BAR_NONE;
    seen_clicks = mouse.clicks;
    x = mouse.click_x;
    y = mouse.click_y;
    if (y >= BAR_H)
        return BAR_NONE;
    if (x < BAR_TITLE_W)
        return bar_menu();
    if (x >= BAR_NET_X && x < BAR_CLOCK_X - 4)
        bar_panel();
    else if (x >= BAR_VOL_LESS - 1 && x < BAR_VOL_LESS + BAR_VOL_BTN + 1)
        bar_volume_change(-1);
    else if (x >= BAR_VOL_MORE - 1 && x < BAR_VOL_MORE + BAR_VOL_BTN + 2)
        bar_volume_change(1);
    else if (x >= BAR_VOL_X && x < BAR_VOL_MORE - 1)
        bar_volume_step();
    else
        return BAR_NONE;
    return BAR_USED;
}
