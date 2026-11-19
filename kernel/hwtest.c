/* Hardware test: the machine's logic board, drawn as it lies in the case,
 * with a number on each part that can be tried and, beside it, a field for
 * each number: what the part reports and a button to test it. Keyboard and
 * mouse are shown live. Esc goes back.
 */
#include "kernel.h"
#include "hw.h"
#include "net.h"
#include "shell.h"

/* ------------------------------------------------------------ the board -- */

/* Where things are, after a photograph of the Macintosh SE's board: the
 * sockets of the back panel along the top, the four memory modules along
 * the bottom, the expansion slot down the right. */
enum { P_BOX, P_CHIP, P_PINS, P_ROUND, P_GRAY };
static const struct { s16 x, y, w, h; u8 kind; const char *name; u8 upright; } parts[] = {
    {12, 48, 280, 290, P_BOX, 0, 0},            /* the board itself (drawn twice as thick) */
    {43, 40, 17, 20, P_BOX, 0, 0},              /* keyboard and mouse sockets */
    {66, 40, 17, 20, P_BOX, 0, 0},
    {93, 32, 68, 28, P_PINS, 0, 0},             /* floppy drive socket */
    {172, 32, 58, 28, P_PINS, 0, 0},            /* SCSI socket */
    {236, 40, 12, 20, P_BOX, 0, 0},             /* modem and printer */
    {251, 40, 12, 20, P_BOX, 0, 0},
    {268, 40, 18, 20, P_BOX, 0, 0},             /* sound */
    {87, 68, 90, 12, P_PINS, 0, 0},             /* SCSI inside the case */
    {182, 68, 40, 10, P_BOX, 0, 0},             /* the two floppy drives */
    {182, 81, 40, 10, P_BOX, 0, 0},
    {259, 76, 18, 18, P_ROUND, 0, 0},           /* coils */
    {259, 100, 18, 18, P_ROUND, 0, 0},
    {109, 94, 68, 17, P_CHIP, "5380", 0},       /* the SCSI chip */
    {62, 114, 34, 16, P_ROUND, 0, 0},           /* battery */
    {124, 120, 40, 13, P_PINS, 0, 0},           /* to the picture tube's board: power, picture, sound */
    {24, 142, 8, 38, P_CHIP, 0, 0},
    {40, 142, 8, 38, P_CHIP, 0, 0},
    {56, 142, 8, 38, P_CHIP, 0, 0},
    {72, 142, 8, 38, P_CHIP, 0, 0},
    {92, 138, 18, 74, P_CHIP, "VIA", 1},
    {118, 162, 19, 50, P_CHIP, "ROM", 1},
    {144, 162, 19, 50, P_CHIP, "ROM", 1},
    {171, 162, 19, 50, P_CHIP, "IWM", 1},
    {198, 166, 11, 42, P_CHIP, 0, 0},
    {213, 186, 15, 22, P_ROUND, 0, 0},          /* the crystal */
    {232, 144, 19, 68, P_CHIP, "SCC", 1},
    {58, 222, 44, 44, P_GRAY, 0, 0},            /* Apple's own chip, in its square socket */
    {63, 227, 34, 34, P_CHIP, "BBU", 0},
    {119, 233, 106, 24, P_CHIP, "68000", 0},
    {232, 258, 9, 30, P_CHIP, 0, 0},
    {248, 258, 9, 30, P_CHIP, 0, 0},
    {270, 178, 16, 114, P_PINS, 0, 0},          /* the expansion slot */
    {22, 298, 126, 14, P_PINS, 0, 0},           /* memory */
    {22, 316, 126, 14, P_PINS, 0, 0},
    {154, 298, 126, 14, P_PINS, 0, 0},
    {154, 316, 126, 14, P_PINS, 0, 0},
};

/* Where each number sits on the board. */
static const struct { s16 x, y; } marks[7] = {
    {57, 62}, {226, 62}, {72, 68}, {272, 62}, {168, 120}, {141, 281}, {119, 264},
};

static void badge(int x, int y, int n)
{
    char s[2] = {(char)('1' + n), 0};

    gfx_fill(x, y, 12, 12, GFX_BLACK);
    gfx_text(x + 3, y + 1, s, 1, 1, 1);
}

static void draw_board(void)
{
    char one[2] = {0, 0};
    unsigned i, k;

    gfx_begin();
    for (i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        int x = parts[i].x, y = parts[i].y, w = parts[i].w, h = parts[i].h;

        switch (parts[i].kind) {
        case P_CHIP:
            gfx_fill(x, y, w, h, GFX_BLACK);
            break;
        case P_ROUND:
            gfx_round_box(x, y, w, h, GFX_WHITE, 1);
            break;
        case P_GRAY:
            gfx_fill(x, y, w, h, GFX_GRAY);
            gfx_frame(x, y, w, h, 1);
            break;
        default:
            gfx_fill(x, y, w, h, GFX_WHITE);
            gfx_frame(x, y, w, h, i == 0 ? 2 : 1);
            if (parts[i].kind == P_PINS)
                gfx_fill(x + 3, y + 3, w - 6, h - 6, GFX_GRAY);
        }
        if (!parts[i].name)
            continue;
        if (!parts[i].upright) {
            gfx_text(x + (w - gfx_text_width(parts[i].name, 1)) / 2, y + (h - 10) / 2,
                     parts[i].name, 1, 1, 0);
            continue;
        }
        for (k = 0; parts[i].name[k]; k++) {    /* a tall chip: its name one letter under another */
            one[0] = parts[i].name[k];
            gfx_text(x + (w - 6) / 2, y + (h - 10 * (int)strlen(parts[i].name)) / 2 + 10 * (int)k,
                     one, 1, 1, 0);
        }
    }
    for (i = 0; i < 7; i++)
        badge(marks[i].x, marks[i].y, (int)i);
    gfx_end();
}

/* ----------------------------------------------------------- the fields -- */

#define F_X         308
#define F_W         194
#define F_TEXT      (F_X + 20)
#define BTN_X       (F_X + 150)
#define BTN_W       38
#define RESULT_Y    304

enum { F_INPUT, F_SCSI, F_NET, F_SOUND, F_SCREEN, F_MEMORY, F_CPU, F_COUNT };
static const struct { s16 y, h; const char *name, *button; } fields[F_COUNT] = {
    {46, 38, "Keyboard and mouse", 0},
    {88, 38, "SCSI", 0},
    {130, 30, "Network", "Ping"},
    {164, 30, "Sound", "Beep"},
    {198, 30, "Screen", "Test"},
    {232, 30, "Memory", "Test"},
    {266, 30, "Processor", 0},
};

/* One line of what a field says: row 0 or 1. */
static void say(int f, int row, const char *text)
{
    int y = fields[f].y + (fields[f].h > 30 ? 15 + 10 * row : 16);
    int w = (fields[f].button ? BTN_X - 4 : F_X + F_W - 2) - F_TEXT;
    char cut[32];
    u32 n = strlen(text), most = (u32)w / 6;

    if (most > sizeof cut - 1)
        most = sizeof cut - 1;
    memcpy(cut, text, n < most ? n + 1 : most);
    cut[most] = 0;
    gfx_begin();
    gfx_fill(F_TEXT, y, w, 10, GFX_WHITE);
    gfx_text(F_TEXT, y, cut, 0, 1, 0);
    gfx_end();
}

static void draw_field(int f)
{
    char n[2] = {(char)('1' + f), 0};
    int y = fields[f].y, h = fields[f].h;

    gfx_begin();
    gfx_fill(F_X, y, F_W, h, GFX_WHITE);
    gfx_frame(F_X, y, F_W, h, 1);
    gfx_fill(F_X, y, 14, h, GFX_BLACK);
    gfx_text(F_X + 4, y + h / 2 - 5, n, 1, 1, 1);
    gfx_text(F_TEXT, y + 4, fields[f].name, 0, 1, 1);
    if (fields[f].button) {
        gfx_round_box(BTN_X, y + 7, BTN_W, 16, GFX_WHITE, 1);
        gfx_text(BTN_X + (BTN_W - gfx_text_width(fields[f].button, 1)) / 2, y + 10, fields[f].button, 0, 1, 0);
    }
    gfx_end();
}

/* Which field's button is at this point? -1 if none. */
static int button_at(int x, int y)
{
    int f;

    for (f = 0; f < F_COUNT; f++)
        if (fields[f].button && x >= BTN_X && x < BTN_X + BTN_W &&
            y >= fields[f].y + 7 && y < fields[f].y + 23)
            return f;
    return -1;
}

/* What the last test found: two lines under the fields. */
static void result(const char *text)
{
    char line[33];
    u32 n = strlen(text), first = n;

    serial_puts("HWTEST: ");
    serial_puts(text);
    serial_puts("\n");
    if (n > 32)                 /* break at a space */
        for (first = 32; first > 0 && text[first] != ' '; first--)
            ;
    gfx_begin();
    gfx_fill(F_X, RESULT_Y, F_W, 24, GFX_WHITE);
    memcpy(line, text, first);
    line[first] = 0;
    gfx_text(F_X, RESULT_Y, line, 0, 1, 0);
    if (first < n) {
        n -= first + 1;
        memcpy(line, text + first + 1, n > 32 ? 32 : n);
        line[n > 32 ? 32 : n] = 0;
        gfx_text(F_X, RESULT_Y + 12, line, 0, 1, 0);
    }
    gfx_end();
}

static char *number(char *out, u32 v)
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
    return out;
}

static char *words(char *out, const char *s)
{
    while (*s)
        *out++ = *s++;
    *out = 0;
    return out;
}

static void show_scsi(void)
{
    static const char *const types[] = {"disk", "tape", "printer", "processor", "WORM",
                                        "CD-ROM", "scanner", "optical"};
    char line[2][40], *t[2];
    u8 inq[36], id, type;
    int found = 0, row;

    say(F_SCSI, 0, "looking...");
    t[0] = line[0];
    t[1] = line[1];
    line[0][0] = line[1][0] = 0;
    for (id = 0; id < 7; id++) {
        if (scsi_inquiry(id, inq) != 0)
            continue;
        type = inq[0] & 0x1F;
        row = found < 2 ? 0 : 1;        /* two to the first line, the rest to the second */
        if (t[row] != line[row])
            t[row] = words(t[row], "  ");
        if (t[row] - line[row] > 24)
            continue;
        t[row] = number(t[row], id);
        t[row] = words(t[row], " ");
        t[row] = words(t[row], type == 3 && id == eth_adapter_id() ? "network" :
                       type < 8 ? types[type] : "other");
        found++;
    }
    say(F_SCSI, 0, found ? line[0] : "nothing answered");
    say(F_SCSI, 1, line[1]);
}

static void show_network(void)
{
    char text[20];

    if (net.up && net.ip) {
        ip_text(text, net.ip);
        say(F_NET, 0, text);
    } else {
        say(F_NET, 0, "not started");
    }
}

static void show_sound(void)
{
    char text[12];

    number(words(text, "Volume "), (u32)snd_volume());
    say(F_SOUND, 0, text);
}

/* ------------------------------------------------------ the memory test -- */

/* Write a different value into every free long word of memory, read them all
 * back, and then the same with every bit the other way round. Free memory
 * is what lies between the kernel and the stack, which sits just under the
 * screen.
 *
 * It is shown as a map: the machine's memory in 64 squares, which fill in
 * as each is written and go black as it reads back good. A square that read
 * back wrong keeps a cross, which says where the fault is. */
#define MT_X        101
#define MT_Y        (BAR_H + 56)
#define MT_W        310
#define MT_H        186
#define MT_CELLS    64
#define MT_KEY      0x5AA5C33CUL

enum { MT_NEW, MT_WROTE, MT_READ, MT_WROTE2, MT_GOOD, MT_USED, MT_BAD };

static void mt_box(int x, int y, int w, int h, int state)
{
    static const u8 fill[] = {GFX_WHITE, GFX_LIGHT, GFX_GRAY, GFX_DARK, GFX_BLACK, GFX_WHITE, GFX_WHITE};
    int xx, yy;

    gfx_begin();
    gfx_fill(x, y, w, h, GFX_BLACK);
    gfx_fill(x + 1, y + 1, w - 2, h - 2, fill[state]);
    if (state == MT_USED) {
        for (yy = 1; yy < h - 1; yy++)
            for (xx = 1; xx < w - 1; xx++)
                if (((xx + yy) & 3) == 0)
                    gfx_fill(x + xx, y + yy, 1, 1, GFX_BLACK);
    } else if (state == MT_BAD) {
        gfx_text(x + w / 2 - 3, y + h / 2 - 5, "X", 0, 1, 1);
    }
    gfx_end();
}

static void mt_cell(int i, int state)
{
    mt_box(MT_X + 12 + (i % 16) * 18, MT_Y + 28 + (i / 16) * 17, 16, 14, state);
}

/* What the squares mean, in one line under the map. */
static void mt_legend(void)
{
    static const struct { u8 state, x; const char *text; } key[3] = {
        {MT_USED, 12, "in use"}, {MT_READ, 82, "written"}, {MT_GOOD, 158, "read back good"},
    };
    int i;

    for (i = 0; i < 3; i++) {
        mt_box(MT_X + key[i].x, MT_Y + 102, 10, 9, key[i].state);
        gfx_text(MT_X + key[i].x + 15, MT_Y + 102, key[i].text, 0, 1, 0);
    }
}

static char *mt_number(char *out, u32 v)
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
    return out;
}

/* The two lines of words under the map, and the bar: done of whole. */
static void mt_words(const char *bold, const char *right, const char *plain, u32 done, u32 whole)
{
    gfx_begin();
    if (bold) {
        gfx_fill(MT_X + 12, MT_Y + 124, MT_W - 24, 12, GFX_WHITE);
        gfx_text(MT_X + 12, MT_Y + 125, bold, 0, 1, 1);
        gfx_text(MT_X + MT_W - 12 - gfx_text_width(right, 1), MT_Y + 125, right, 0, 1, 0);
    }
    gfx_fill(MT_X + 12, MT_Y + 140, MT_W - 24, 12, GFX_WHITE);
    gfx_text(MT_X + 12, MT_Y + 141, plain, 0, 1, 0);
    gfx_fill(MT_X + 14, MT_Y + 162, (int)(whole ? (MT_W - 28) * (done >> 8) / (whole >> 8) : 0), 8, GFX_BLACK);
    gfx_end();
}

static void test_memory(void)
{
    static const char *const steps[4] = {"Writing a pattern", "Reading it back",
                                         "Writing its opposite", "Reading it back"};
    static u8 wrong[MT_CELLS];
    u32 first = ((u32)__bss_end + 0x1003) & ~3UL;
    u32 last = ((u32)machine.screen - 0x10000) & ~3UL;
    u32 size = (machine.mem_top / MT_CELLS) & ~3UL, total = last - first;
    u32 lo, hi, bad = 0, done, clicks, flip;
    u32 *p, *end;
    char line[48], *t;
    int phase, i, ev, stopped = 0, cellbad;

    gfx_begin();
    gfx_save(MT_X, MT_Y, MT_W + 3, MT_H + 3);
    gfx_fill(MT_X + 3, MT_Y + 3, MT_W, MT_H, GFX_BLACK);
    gfx_fill(MT_X, MT_Y, MT_W, MT_H, GFX_BLACK);
    gfx_fill(MT_X + 1, MT_Y + 1, MT_W - 2, MT_H - 2, GFX_WHITE);
    gfx_fill(MT_X + 1, MT_Y + 1, MT_W - 2, 16, GFX_BLACK);
    gfx_text(MT_X + MT_W / 2 - 33, MT_Y + 4, "Memory test", 1, 1, 1);
    gfx_frame(MT_X + 12, MT_Y + 160, MT_W - 24, 12, 1);
    mt_legend();
    gfx_end();
    memset(wrong, 0, sizeof wrong);
    for (i = 0; i < MT_CELLS; i++) {
        lo = (u32)i * size;
        mt_cell(i, lo + size <= first || lo >= last ? MT_USED : MT_NEW);
    }
    serial_puts("\nHWTEST: memory test\n");

    for (phase = 0; phase < 4 && !stopped; phase++) {
        flip = phase >= 2 ? 0xFFFFFFFFUL : 0;
        mt_words(steps[phase], phase < 2 ? "Pass 1 of 2" : "Pass 2 of 2", "", (u32)phase * total, 4 * total);
        for (i = 0, done = 0; i < MT_CELLS; i++) {
            lo = (u32)i * size;
            hi = lo + size;
            if (lo < first)
                lo = first;
            if (hi > last)
                hi = last;
            if (lo >= hi)
                continue;       /* in use: the kernel, or the stack and the screen */
            p = (u32 *)lo;
            end = (u32 *)hi;
            cellbad = 0;
            if (!(phase & 1)) {
                for (; p < end; p++)
                    *p = ((u32)p ^ MT_KEY) ^ flip;
            } else {
                for (; p < end; p++)
                    if (*p != (((u32)p ^ MT_KEY) ^ flip)) {
                        bad++;
                        cellbad = 1;
                    }
            }
            if (cellbad)
                wrong[i] = 1;
            mt_cell(i, wrong[i] ? MT_BAD : MT_WROTE + phase);
            done += hi - lo;
            t = mt_number(line, done / 1024);
            memcpy(t, " K of ", 7);
            t = mt_number(t + 6, total / 1024);
            memcpy(t, " K", 3);
            mt_words(NULL, NULL, line, (u32)phase * total + done, 4 * total);
            while ((ev = kbd_get_event()) >= 0)
                if (!(ev & 0x8000) && (ev & 0xFF) == 0x1B)
                    stopped = 1;
            if (stopped)
                break;
        }
    }

    if (stopped) {
        mt_words("Stopped", "", "", 0, 4 * total);
        serial_puts("HWTEST: memory test stopped\n");
    } else if (bad) {
        t = mt_number(line, (bad + 255) / 256);
        memcpy(t, " K read back wrong, at the crosses", 35);
        mt_words("MEMORY FAULTY", "", line, 4 * total, 4 * total);
        serial_puts("HWTEST: MEMORY FAULTY\n");
    } else {
        t = mt_number(line, total / 1024);
        memcpy(t, " K written and read back twice", 31);
        mt_words("Memory good", "", line, 4 * total, 4 * total);
        serial_puts("HWTEST: memory good\n");
    }
    kbd_escape = 0;
    for (clicks = mouse.clicks;;) {     /* any key or a click closes it */
        ev = kbd_get_event();
        if ((ev >= 0 && !(ev & 0x8000) && (ev & 0xFF)) || mouse.clicks != clicks)
            break;
        cpu_idle();
    }
    gfx_begin();
    gfx_restore();
    gfx_end();
    kbd_escape = 0;
}

static void wait_key(void)
{
    int ev;

    for (;;) {
        ev = kbd_get_event();
        if (ev >= 0 && !(ev & 0x8000) && (ev & 0xFF))
            return;
        cpu_idle();
    }
}

/* A line of words in a white box near the foot of the screen, readable
 * over any pattern. */
static void screen_note(const char *text)
{
    int w = gfx_text_width(text, 1) + 20, x = (SCREEN_W - w) / 2, y = SCREEN_H - 34;

    gfx_fill(x, y, w, 18, GFX_BLACK);
    gfx_fill(x + 1, y + 1, w - 2, 16, GFX_WHITE);
    gfx_text(x + 10, y + 4, text, 0, 1, 0);
}

static void test_screen(void)
{
    static const int patterns[] = {GFX_BLACK, GFX_WHITE, GFX_GRAY, GFX_LIGHT, GFX_DARK};
    int i, x;

    gfx_pointer(0);             /* the pointer has no place on a test pattern */
    for (i = 0; i < 5; i++) {
        gfx_fill(0, 0, SCREEN_W, SCREEN_H, patterns[i]);
        screen_note("Press any key for the next pattern");
        wait_key();
    }
    gfx_fill(0, 0, SCREEN_W, SCREEN_H, GFX_WHITE);      /* a grid, to judge the geometry */
    for (x = 0; x < SCREEN_W; x += 32)
        gfx_fill(x, 0, 1, SCREEN_H, GFX_BLACK);
    for (x = 0; x < SCREEN_H; x += 32)
        gfx_fill(0, x, SCREEN_W, 1, GFX_BLACK);
    gfx_frame(0, 0, SCREEN_W, SCREEN_H, 1);
    screen_note("Press any key to finish");
    wait_key();
    gfx_pointer(1);
    bar_draw();                 /* the patterns covered the whole screen, bar included */
}

static void draw_all(void)
{
    char text[32], *t;
    int f;

    con_clear();
    gfx_begin();
    gfx_text(F_X, 28, "Hardware test", 0, 1, 1);
    gfx_end();
    draw_board();
    for (f = 0; f < F_COUNT; f++)
        draw_field(f);
    say(F_INPUT, 0, "press any key");
    say(F_INPUT, 1, "move the mouse");
    show_network();
    show_sound();
    say(F_SCREEN, 0, "512 x 342");
    t = number(text, machine.mem_top / 1024);
    words(t, " K");
    say(F_MEMORY, 0, text);
    say(F_CPU, 0, machine.rom_version == 0x0276 ? "68000  ROM SE FDHD" : "68000");
    show_scsi();                /* last: asking every address on the bus takes a few seconds */
    serial_puts("\nHWTEST: ready\n");
}

static void ping_gateway(void)
{
    char text[64], *t;
    u32 rtt;
    int err;

    if (!(net.up && net.ip)) {
        result("No network to ping");
        return;
    }
    result("Pinging the gateway...");
    err = net_ping(net.gateway, 1, 3 * 60, &rtt);
    if (err) {
        t = words(text, "No answer from the gateway: ");
        words(t, net_strerror(err));
    } else {
        /* the time is counted in sixtieths of a second */
        t = words(text, rtt ? "The gateway answered in " : "The gateway answered in under ");
        t = number(t, rtt ? rtt * 1000 / 60 : 17);
        words(t, " ms");
    }
    result(text);
}

void cmd_hwtest(void)
{
    static const char hex[] = "0123456789ABCDEF";
    u32 seen = mouse.moves - 1, clicks = mouse.clicks;
    char text[40], *t;
    int ev, r, what;
    u8 ch, raw;

    draw_all();
    clicks = mouse.clicks;
    for (;;) {
        net_poll();
        if (seen != mouse.moves) {
            seen = mouse.moves;
            t = number(text, (u32)mouse.x);
            t = words(t, ", ");
            t = number(t, (u32)mouse.y);
            words(t, mouse.button ? "  button down" : "  button up");
            say(F_INPUT, 1, text);
        }
        ev = kbd_get_event();
        bar_tick();
        r = bar_poll();
        if (r == BAR_HOME || r == BAR_CONSOLE)
            break;              /* the SSH OS menu: leave */
        if (r != BAR_NONE)
            clicks = mouse.clicks;
        what = -1;
        if (mouse.clicks != clicks) {
            clicks = mouse.clicks;
            what = button_at(mouse.click_x, mouse.click_y);
        }
        if (ev < 0 && what < 0) {
            bar_idle();
            continue;
        }
        bar_work();
        if (ev >= 0 && !(ev & 0x8000)) {
            ch = (u8)(ev & 0xFF);
            raw = (u8)((ev >> 8) & 0x7F);
            if (ch == 0x1B)
                break;
            t = words(text, "key code ");
            *t++ = hex[raw >> 4];
            *t++ = hex[raw & 15];
            *t = 0;
            if (ch >= 32 && ch != 127) {
                t = words(t, " '");
                *t++ = (char)ch;
                words(t, "'");
            }
            say(F_INPUT, 0, text);
            serial_puts(text);
            serial_puts("\n");
            if (ch == 'b' || ch == 'B')
                what = F_SOUND;
            else if (ch == 'm' || ch == 'M')
                what = F_MEMORY;
            else if (ch == 's' || ch == 'S')
                what = F_SCREEN;
            else if (ch == 'p' || ch == 'P')
                what = F_NET;
            else if (ch == '+' || ch == '=' || ch == '-') {
                snd_set_volume(snd_volume() + (ch == '-' ? -1 : 1));
                snd_keep_volume();
                show_sound();
                snd_beep(8);
                bar_draw();
            }
        }
        if (what == F_SOUND) {
            snd_beep(20);
            result("Beep. Did you hear it?");
        } else if (what == F_MEMORY) {
            test_memory();
            clicks = mouse.clicks;
        } else if (what == F_SCREEN) {
            test_screen();
            draw_all();
            clicks = mouse.clicks;
        } else if (what == F_NET) {
            ping_gateway();
        }
    }
    kbd_escape = 0;
}
