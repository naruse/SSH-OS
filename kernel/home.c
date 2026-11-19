/* home: a control panel for Home Assistant.
 *
 * Shows lights and switches as tiles, twelve to a page (N and P turn the
 * page). Click a tile, or select it with the arrow keys and press Space, to
 * switch it; a light that can be dimmed has a bar to click on, and + and -
 * change it from the keyboard.
 *
 * It talks to Home Assistant's own web interface (the REST API) in plain
 * HTTP on the home network, signing in with a long-lived access token from
 * the settings. Two kinds of request are used:
 *
 *   POST /api/template         Home Assistant fills in a small template and
 *                              returns "#" and the number of devices, then
 *                              one line for each device on this page:
 *                                  entity|name|state|brightness|dimmable
 *                              so the SE never has to read the large
 *                              description of everything in the house.
 *   POST /api/services/...     switch something, or set a brightness.
 */
#include "kernel.h"
#include "net.h"
#include "shell.h"
#include "hw.h"

#define MAX_TILES   12
#define COLS        3
#define TILE_W      160
#define TILE_H      70
#define TILE_GAP_X  8
#define TILE_GAP_Y  6
#define FIRST_X     8
#define FIRST_Y     26
#define BAR_X       48          /* brightness bar, inside the tile */
#define BAR_Y       50
#define BAR_W       102
#define LEVEL_H       10
#define STATUS_Y    331
#define REFRESH_TICKS (10 * 60)

struct tile {
    char id[64];
    char name[18];
    u8 on, dimmable, available, is_light;
    u8 brightness;              /* 0..255, as Home Assistant counts it */
};

static struct tile tiles[MAX_TILES];
static int ntiles, selected;
static int page, total;         /* which twelve are shown, out of how many */
static char reply[4096];
static char request[1400];
static char ha_host[64];
static u32 ha_ip;
static u16 ha_port;
static const char *ha_token;
static char status_text[86];

static const u16 icon_bulb[16] = {
    0x07C0, 0x0820, 0x1010, 0x2008, 0x2008, 0x2008, 0x2008, 0x1010,
    0x0820, 0x0440, 0x0440, 0x07C0, 0x0440, 0x07C0, 0x0380, 0x0000,
};
static const u16 icon_power[16] = {
    0x0180, 0x0180, 0x0DB0, 0x1998, 0x318C, 0x6186, 0x6186, 0x6006,
    0x6006, 0x6006, 0x300C, 0x1818, 0x0E70, 0x07E0, 0x0000, 0x0000,
};

static void note(const char *s)
{
    serial_puts("HOME: ");
    serial_puts(s);
    serial_puts("\n");
}

static void copy_text(char *dst, const char *src, int max)
{
    int n = 0;

    while (src[n] && n < max - 1) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

/* ------------------------------------------------------------ drawing -- */

static void tile_place(int i, int *x, int *y)
{
    *x = FIRST_X + (i % COLS) * (TILE_W + TILE_GAP_X);
    *y = FIRST_Y + (i / COLS) * (TILE_H + TILE_GAP_Y);
}

static void draw_tile(int i)
{
    struct tile *t = &tiles[i];
    char line[20];
    int x, y, white, filled, pct, n;

    tile_place(i, &x, &y);
    white = t->on && t->available;      /* a lit tile is black with white ink */
    gfx_begin();
    gfx_fill(x - 3, y - 3, TILE_W + 6, TILE_H + 6, GFX_WHITE);
    if (i == selected)
        gfx_frame(x - 3, y - 3, TILE_W + 6, TILE_H + 6, 2);
    gfx_round_box(x, y, TILE_W, TILE_H, !t->available ? GFX_LIGHT : white ? GFX_BLACK : GFX_WHITE, 1);
    gfx_picture(x + 8, y + 10, t->is_light ? icon_bulb : icon_power, 16, white, 2);
    if (!t->available)
        gfx_fill(x + 46, y + 8, 110, 30, GFX_WHITE);    /* keep the words readable */
    gfx_text(x + BAR_X, y + 10, t->name, white, 1, 1);

    if (!t->available) {
        copy_text(line, "Not available", sizeof line);
    } else if (!t->on) {
        copy_text(line, "Off", sizeof line);
    } else if (t->dimmable) {
        pct = (t->brightness * 100 + 127) / 255;
        n = 0;
        line[n++] = 'O'; line[n++] = 'n'; line[n++] = ' '; line[n++] = ' ';
        if (pct >= 100)
            line[n++] = '1';
        if (pct >= 10)
            line[n++] = (char)('0' + pct / 10 % 10);
        line[n++] = (char)('0' + pct % 10);
        line[n++] = '%';
        line[n] = 0;
    } else {
        copy_text(line, "On", sizeof line);
    }
    gfx_text(x + BAR_X, y + 26, line, white, 1, 0);

    if (t->dimmable && t->available) {
        /* The bar: an outline, filled from the left in proportion. */
        filled = t->on ? (t->brightness * (BAR_W - 4) + 127) / 255 : 0;
        if (white) {
            gfx_fill(x + BAR_X, y + BAR_Y, BAR_W, LEVEL_H, GFX_WHITE);
            gfx_fill(x + BAR_X + 1, y + BAR_Y + 1, BAR_W - 2, LEVEL_H - 2, GFX_BLACK);
            gfx_fill(x + BAR_X + 2, y + BAR_Y + 2, filled, LEVEL_H - 4, GFX_WHITE);
        } else {
            gfx_frame(x + BAR_X, y + BAR_Y, BAR_W, LEVEL_H, 1);
            gfx_fill(x + BAR_X + 2, y + BAR_Y + 2, filled, LEVEL_H - 4, GFX_BLACK);
        }
    }
    gfx_end();
}

static void draw_status(const char *text)
{
    copy_text(status_text, text, sizeof status_text);
    gfx_begin();
    gfx_fill(0, STATUS_Y - 2, SCREEN_W, SCREEN_H - STATUS_Y + 2, GFX_WHITE);
    gfx_fill(0, STATUS_Y - 3, SCREEN_W, 1, GFX_BLACK);
    gfx_text(8, STATUS_Y, status_text, 0, 1, 0);
    gfx_end();
    note(text);
}

static void draw_all(void)
{
    static const char title[] = "Home";
    int i, w = gfx_text_width(title, 1) + 16;

    gfx_begin();
    gfx_fill(0, 0, SCREEN_W, SCREEN_H, GFX_WHITE);
    /* A title bar in the manner of the Macintosh: stripes, name in the middle. */
    for (i = 3; i < 17; i += 2)
        gfx_fill(4, i, SCREEN_W - 8, 1, GFX_BLACK);
    gfx_fill(0, 19, SCREEN_W, 1, GFX_BLACK);
    gfx_fill((SCREEN_W - w) / 2, 1, w, 18, GFX_WHITE);
    gfx_text((SCREEN_W - w) / 2 + 8, 5, title, 0, 1, 1);
    gfx_end();
    for (i = 0; i < ntiles; i++)
        draw_tile(i);
    draw_status(status_text);
}

/* --------------------------------------------------- Home Assistant -- */

static char *append(char *p, char *end, const char *s)
{
    while (*s && p < end)
        *p++ = *s++;
    *p = 0;
    return p;
}

/* A name as Home Assistant sends it (UTF-8) in the letters this machine
 * has: accents are dropped, anything else unknown becomes '?'. */
static void plain_name(char *dst, const char *src, int n, int max)
{
    static const char latin[] =         /* U+00C0 .. U+00FF */
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    int out = 0, i = 0;
    unsigned char c, c2;

    while (i < n && out < max - 1) {
        c = (unsigned char)src[i++];
        if (c < 0x80) {
            dst[out++] = (c >= 32 && c < 127) ? (char)c : ' ';
        } else if ((c == 0xC3 || c == 0xC2) && i < n) {
            c2 = (unsigned char)src[i++];
            if (c == 0xC3 && c2 >= 0x80 && c2 <= 0xBF)
                dst[out++] = latin[c2 - 0x80];
            else
                dst[out++] = (c2 == 0xA0) ? ' ' : '?';
        } else {
            while (i < n && ((unsigned char)src[i] & 0xC0) == 0x80)
                i++;                    /* skip the rest of a longer character */
            dst[out++] = '?';
        }
    }
    dst[out] = 0;
}

static int ha_post(const char *path, const char *body, int *status)
{
    int err = http_request("POST", ha_ip, ha_port, ha_host, path, ha_token, body, reply,
                           sizeof reply, NULL, status);

    if (err)
        return err;
    return 0;
}

static const char *why_failed(int err, int status)
{
    if (err)
        return net_strerror(err);
    if (status == 401 || status == 403)
        return "Home Assistant refused the access token";
    if (status == 404)
        return "that address answers, but not as Home Assistant";
    return "Home Assistant reported an error";
}

/* Ask for the list of devices and their states. Returns 0 or shows why not. */
static int refresh(void)
{
    static const char each[] =
        "{{ s.entity_id }}|{{ s.name }}|{{ s.state }}|{{ s.attributes.brightness | default(0, true) }}|"
        "{{ (s.attributes.supported_color_modes | default(['onoff'])) != ['onoff'] }}\\n";
    const char *list = config_get("ha_entities");
    char *p = request, *end = request + sizeof request - 4, *line, *next, *field[5];
    char item[64], slice[40];
    int err, status, n = 0, f, k;
    struct tile *t;

    /* "[12:24] %}" for the second page, and so on. */
    p = slice;
    *p++ = '[';
    for (k = page * MAX_TILES, f = 1000; f; f /= 10)
        if (k >= f || f == 1)
            *p++ = (char)('0' + k / f % 10);
    *p++ = ':';
    for (k = (page + 1) * MAX_TILES, f = 1000; f; f /= 10)
        if (k >= f || f == 1)
            *p++ = (char)('0' + k / f % 10);
    append(p, slice + sizeof slice - 1, "] %}");
    p = request;

    /* The template is sent as JSON; it uses only single quotes, so nothing
     * in it needs escaping. */
    p = append(p, end, "{\"template\":\"");
    if (list && list[0]) {
        p = append(p, end, "{% set all = [");
        while (*list) {
            while (*list == ' ' || *list == ',')
                list++;
            for (k = 0; *list && *list != ',' && *list != ' ' && k < (int)sizeof item - 1; k++)
                item[k] = (*list == '\'' || *list == '"' || *list == '\\') ? '_' : *list, list++;
            item[k] = 0;
            if (k) {
                p = append(p, end, "'");
                p = append(p, end, item);
                p = append(p, end, "',");
            }
        }
        p = append(p, end, "] %}#{{ all | count }}\\n{% for e in all");
        p = append(p, end, slice);
        p = append(p, end, "{% set s = states[e] %}{% if s %}");
        p = append(p, end, each);
        p = append(p, end, "{% endif %}{% endfor %}\"}");
    } else {
        p = append(p, end, "{% set all = states.light | list + states.switch | list %}"
                           "#{{ all | count }}\\n{% for s in all");
        p = append(p, end, slice);
        p = append(p, end, each);
        p = append(p, end, "{% endfor %}\"}");
    }
    if (p >= end) {
        draw_status("The list of devices in the settings is too long.");
        return -1;
    }

    err = ha_post("/api/template", request, &status);
    if (err || status != 200) {
        char msg[86];

        p = append(msg, msg + sizeof msg - 1, "No news from Home Assistant: ");
        append(p, msg + sizeof msg - 1, why_failed(err, status));
        draw_status(msg);
        return -1;
    }

    for (line = reply; *line && n < MAX_TILES; line = next) {
        for (next = line; *next && *next != '\n'; next++)
            ;
        if (*next)
            *next++ = 0;
        if (line[0] == '#') {                   /* how many devices there are in all */
            for (k = 0, p = line + 1; *p >= '0' && *p <= '9'; p++)
                k = k * 10 + (*p - '0');
            total = k;
            continue;
        }
        field[0] = line;
        for (f = 1; f < 5; f++) {
            for (p = field[f - 1]; *p && *p != '|'; p++)
                ;
            if (!*p)
                break;
            *p = 0;
            field[f] = p + 1;
        }
        if (f < 5 || !field[0][0])
            continue;
        t = &tiles[n++];
        memset(t, 0, sizeof *t);
        copy_text(t->id, field[0], sizeof t->id);
        plain_name(t->name, field[1], (int)strlen(field[1]), sizeof t->name);
        t->is_light = field[0][0] == 'l' && field[0][1] == 'i';
        t->on = field[2][0] == 'o' && field[2][1] == 'n' && !field[2][2];
        t->available = !(field[2][0] == 'u' && field[2][1] == 'n');     /* unavailable, unknown */
        for (k = 0, p = field[3]; *p >= '0' && *p <= '9'; p++)
            k = k * 10 + (*p - '0');
        t->brightness = (u8)(k > 255 ? 255 : k);
        t->dimmable = field[4][0] == 'T';
    }
    ntiles = n;
    if (selected >= ntiles)
        selected = ntiles ? ntiles - 1 : 0;
    return 0;
}

/* Switch tile i, or with level >= 0 set its brightness (0..255). */
static void act(int i, int level)
{
    struct tile *t = &tiles[i];
    char *p = request, *end = request + sizeof request - 1;
    char num[4];
    int err, status, n = 0;

    if (i < 0 || i >= ntiles || !t->available)
        return;
    p = append(p, end, "{\"entity_id\":\"");
    p = append(p, end, t->id);
    p = append(p, end, "\"");
    if (level >= 0) {
        if (level >= 100) num[n++] = (char)('0' + level / 100);
        if (level >= 10) num[n++] = (char)('0' + level / 10 % 10);
        num[n++] = (char)('0' + level % 10);
        num[n] = 0;
        p = append(p, end, ",\"brightness\":");
        p = append(p, end, num);
    }
    append(p, end, "}");
    serial_puts("HOME: sending ");
    serial_puts(request);
    serial_puts("\n");
    err = ha_post(level > 0 ? "/api/services/light/turn_on" :
                  level == 0 ? "/api/services/light/turn_off" :
                  "/api/services/homeassistant/toggle", request, &status);
    if (err || status != 200) {
        char msg[86];

        p = append(msg, msg + sizeof msg - 1, "Could not switch it: ");
        append(p, msg + sizeof msg - 1, why_failed(err, status));
        draw_status(msg);
        return;
    }
    /* Show the result at once; the next refresh confirms it. */
    if (level > 0) {
        t->on = 1;
        t->brightness = (u8)level;
    } else if (level == 0) {
        t->on = 0;
    } else {
        t->on = !t->on;
        if (t->on && t->dimmable && t->brightness == 0)
            t->brightness = 255;
    }
    draw_tile(i);
    draw_status(t->on ? "Switched on." : "Switched off.");
}

static int pages(void)
{
    return total > MAX_TILES ? (total + MAX_TILES - 1) / MAX_TILES : 1;
}

/* The usual line at the bottom: what to do, and which page this is. */
static void draw_hint(void)
{
    char text[86], *p = text, *end = text + sizeof text - 1;

    if (!ntiles) {
        draw_status("Home Assistant has no lights or switches to show.");
        return;
    }
    p = append(p, end, "Click a tile, or arrow keys and Space. Q leaves.");
    if (pages() > 1) {
        p = append(p, end, "  Page ");
        *p++ = (char)('1' + page);
        p = append(p, end, " of ");
        *p++ = (char)('0' + (pages() > 9 ? 9 : pages()));
        append(p, end, ": N next, P previous.");
    }
    draw_status(text);
}

static void select_tile(int i)
{
    int old = selected;

    if (i < 0 || i >= ntiles || i == selected)
        return;
    selected = i;
    draw_tile(old);
    draw_tile(selected);
}

/* A click at (x, y): which tile, and is it on that tile's brightness bar? */
static void click(int x, int y)
{
    int i, tx, ty, level;

    for (i = 0; i < ntiles; i++) {
        tile_place(i, &tx, &ty);
        if (x < tx || x >= tx + TILE_W || y < ty || y >= ty + TILE_H)
            continue;
        select_tile(i);
        if (tiles[i].dimmable && y >= ty + BAR_Y - 3 && y < ty + BAR_Y + LEVEL_H + 3 &&
            x >= tx + BAR_X - 4) {
            level = (x - (tx + BAR_X)) * 255 / (BAR_W - 1);
            if (level < 8)
                level = 0;
            if (level > 255)
                level = 255;
            act(i, level);
        } else {
            act(i, -1);
        }
        return;
    }
}

static int same_tile(const struct tile *a, const struct tile *b)
{
    const u8 *p = (const u8 *)a, *q = (const u8 *)b;
    u32 n = sizeof *a;

    while (n--)
        if (*p++ != *q++)
            return 0;
    return 1;
}

void cmd_home(int argc, char **argv)
{
    static struct tile before[MAX_TILES];
    u32 seen_clicks, next_refresh;
    int ev, i, level, quit = 0, err, was;
    char ch;
    u8 raw;

    (void)argc;
    (void)argv;
    if (config_host_port("ha", ha_host, sizeof ha_host, &ha_port, 8123) != 0) {
        kputs("Set Home Assistant's address first, for example: set ha 192.168.1.5:8123\n");
        return;
    }
    ha_token = config_get("ha_token");
    if (!ha_token || !ha_token[0]) {
        kputs("No access token. Put ha_token = ... in config.txt and rebuild the disk.\n");
        return;
    }
    err = net_resolve(ha_host, &ha_ip);
    if (err) {
        shell_fail(err);
        return;
    }

    ntiles = 0;
    selected = 0;
    page = 0;
    total = 0;
    mouse.x = SCREEN_W / 2;
    mouse.y = SCREEN_H / 2;
    copy_text(status_text, "Asking Home Assistant...", sizeof status_text);
    draw_all();
    gfx_pointer(1);
    if (refresh() == 0) {
        draw_all();
        draw_hint();
    }
    seen_clicks = mouse.clicks;
    next_refresh = ticks + REFRESH_TICKS;

    while (!quit) {
        net_poll();
        if (mouse.clicks != seen_clicks) {
            seen_clicks = mouse.clicks;
            click(mouse.click_x, mouse.click_y);
            next_refresh = ticks + 90;          /* confirm with Home Assistant shortly */
        }
        while (!quit && (ev = kbd_get_event()) >= 0) {
            if (ev & 0x8000)
                continue;
            ch = (char)(ev & 0xFF);
            raw = (u8)((ev >> 8) & 0x7F);
            if (ch == 'q' || ch == 'Q' || ch == 0x1B) {
                quit = 1;               /* keys typed after this belong to the shell */
            } else if (raw == 0x3B) {           /* arrows */
                select_tile(selected - 1);
            } else if (raw == 0x3C) {
                select_tile(selected + 1);
            } else if (raw == 0x3E) {
                select_tile(selected - COLS);
            } else if (raw == 0x3D) {
                select_tile(selected + COLS);
            } else if (ch == ' ' || ch == '\n') {
                act(selected, -1);
                next_refresh = ticks + 90;
            } else if ((ch == '+' || ch == '=' || ch == '-') && selected < ntiles &&
                       tiles[selected].dimmable) {
                level = tiles[selected].on ? tiles[selected].brightness : 0;
                level += (ch == '-') ? -26 : 26;        /* a tenth */
                act(selected, level < 0 ? 0 : level > 255 ? 255 : level);
                next_refresh = ticks + 90;
            } else if (ch == 'r' || ch == 'R') {
                next_refresh = ticks;
            } else if (((ch == 'n' || ch == 'N') && page + 1 < pages() && page < 8) ||
                       ((ch == 'p' || ch == 'P') && page > 0)) {
                page += (ch == 'n' || ch == 'N') ? 1 : -1;
                selected = 0;
                draw_status("Asking Home Assistant...");
                if (refresh() == 0) {
                    draw_all();
                    draw_hint();
                }
                next_refresh = ticks + REFRESH_TICKS;
            }
        }
        if (!quit && (s32)(ticks - next_refresh) >= 0) {
            memcpy(before, tiles, sizeof tiles);
            was = ntiles;
            if (refresh() == 0) {
                for (i = 0; i < MAX_TILES; i++) {
                    if (i < ntiles) {
                        /* Redraw only what changed, so the screen stays calm. */
                        if (i >= was || !same_tile(&before[i], &tiles[i]))
                            draw_tile(i);
                    } else if (i < was) {       /* a device that went away */
                        int x, y;

                        tile_place(i, &x, &y);
                        gfx_begin();
                        gfx_fill(x - 3, y - 3, TILE_W + 6, TILE_H + 6, GFX_WHITE);
                        gfx_end();
                    }
                }
                note("refreshed");
            }
            next_refresh = ticks + REFRESH_TICKS;
        }
        cpu_idle();
    }

    kbd_escape = 0;
    con_clear();
}
