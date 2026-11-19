/* The terminal: what a program at the other end of an SSH connection draws
 * on. It understands what xterm does, as far as a black-and-white screen of
 * 85 x 32 characters can show it:
 *
 *   - moving the cursor, erasing, inserting and deleting characters and
 *     lines, scrolling regions, the second ("alternate") screen that
 *     full-screen programs switch to and leave again
 *   - bold, underline and inverse. There are no colours: a coloured
 *     background is shown as inverse, a coloured text as plain
 *   - UTF-8, with the characters our font has; the VT100 line-drawing set
 *   - the questions programs ask the terminal (where is the cursor, what
 *     are you), answered through vt_reply()
 *
 * The screen's contents are kept here as well as on the glass, so they can
 * be shifted, redrawn and brought back.
 */
#include "kernel.h"

#define COLS CON_COLS
#define ROWS CON_ROWS
#define NPAR 16

struct page {
    u8 ch[ROWS][COLS];
    u8 at[ROWS][COLS];
    u8 row[ROWS];               /* which of those is each row of the screen: scrolling turns
                                 * this list round and moves no text */
};

enum { GROUND, ESC, ESC_ONE, CSI, STRING, STRING_ESC };

static struct page pages[2];
static struct {
    struct page *p;
    int cx, cy;                 /* cursor */
    int top, bot;               /* scrolling region, rows */
    u8 wrap;                    /* the last column was written: the next character wraps */
    u8 bold, under, inv, darkbg;
    u8 state;
    u8 mark;                    /* < = > ? right after Esc [ */
    u8 inter;                   /* a character from space to / before the last one */
    u16 par[NPAR];
    u8 sub[NPAR];               /* this number came after a colon */
    u8 npar, digits;
    u8 more;                    /* bytes still to come of a UTF-8 character */
    u32 cp;
    u8 set[2], shift;           /* 1 = that character set is the line-drawing one */
    u8 last;                    /* the last character shown, for "repeat" */
    u8 app_keys, autowrap, insert, origin, show_cursor, alt;
    u8 cursor_on;               /* the cursor block is on the glass */
    struct {                    /* the remembered cursor, one for each screen */
        int x, y;
        u8 bold, under, inv, dark;
    } saved[2];
    u8 reply[48];
    u8 nreply;
} vt;

u8 vt_log;                      /* copy what is shown to the serial port */

static u8 attr_now(void)
{
    return (u8)((vt.bold ? CON_BOLD : 0) | (vt.under ? CON_UNDERLINE : 0) |
                ((vt.inv ^ vt.darkbg) ? CON_INVERSE : 0));
}

/* What an erased cell looks like: only the background counts. */
static u8 attr_blank(void)
{
    return vt.darkbg ? CON_INVERSE : 0;
}

static void reply(const char *s)
{
    while (*s && vt.nreply < sizeof vt.reply)
        vt.reply[vt.nreply++] = (u8)*s++;
}

static void reply_num(u32 v)
{
    char t[8];
    int n = 0;

    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 8);
    while (n && vt.nreply < sizeof vt.reply)
        vt.reply[vt.nreply++] = (u8)t[--n];
}

/* ------------------------------------------------------------- the glass -- */

/* Nothing is drawn as it arrives. What changes is noted, a span to each
 * row, and drawn when the bytes in hand have all been read (vt_write's
 * end): a cell written five times is drawn once, cells are drawn in fours
 * (con_draw_cells), and a page of text that scrolls by is moved on the
 * screen once, not line by line. */
static u8 dirty_from[ROWS], dirty_to[ROWS];     /* to <= from: nothing to draw */
static u8 scrolled;             /* the whole screen is to move up by this many rows first */

static void mark(int row, int from, int to)
{
    if (from < 0)
        from = 0;
    if (to > COLS)
        to = COLS;
    if (from >= to)
        return;
    if (dirty_to[row] <= dirty_from[row]) {
        dirty_from[row] = (u8)from;
        dirty_to[row] = (u8)to;
        return;
    }
    if (from < dirty_from[row])
        dirty_from[row] = (u8)from;
    if (to > dirty_to[row])
        dirty_to[row] = (u8)to;
}

/* Move the screen as far as the text has moved; the notes then fit it. */
static void settle(void)
{
    if (scrolled)
        con_scroll(0, ROWS - 1, scrolled);
    scrolled = 0;
}

static void flush(void)
{
    int r;

    settle();
    for (r = 0; r < ROWS; r++)
        if (dirty_to[r] > dirty_from[r]) {
            con_draw_cells(r, dirty_from[r], dirty_to[r], vt.p->ch[vt.p->row[r]], vt.p->at[vt.p->row[r]]);
            dirty_from[r] = dirty_to[r] = 0;
        }
}

static void blank(int row, int from, int n)
{
    u8 a = attr_blank();

    if (from < 0)
        from = 0;
    if (from + n > COLS)
        n = COLS - from;
    if (n <= 0)
        return;
    memset(vt.p->ch[vt.p->row[row]] + from, ' ', (u32)n);
    memset(vt.p->at[vt.p->row[row]] + from, a, (u32)n);
    mark(row, from, from + n);
}

/* Draw part of a row again from what is kept. */
static void redraw(int row, int from, int to)
{
    mark(row, from, to);
}

static void redraw_all(void)
{
    int r;

    for (r = 0; r < ROWS; r++)
        redraw(r, 0, COLS);
}

/* Move rows top..bot up by n (down if n is negative); new rows are blank. */
static void scroll(int top, int bot, int n)
{
    int height = bot - top + 1, r, count = n < 0 ? -n : n;
    u8 keep[ROWS];

    if (height <= 0 || n == 0)
        return;
    if (count > height)
        count = height;
    /* The rows themselves stay where they are in memory: only the list of
     * which is where is turned round. The ones that drop off one end come
     * in at the other, to be blanked below. */
    if (n > 0) {
        memcpy(keep, vt.p->row + top, (u32)count);
        memmove(vt.p->row + top, vt.p->row + top + count, (u32)(height - count));
        memcpy(vt.p->row + bot + 1 - count, keep, (u32)count);
    } else {
        memcpy(keep, vt.p->row + bot + 1 - count, (u32)count);
        memmove(vt.p->row + top + count, vt.p->row + top, (u32)(height - count));
        memcpy(vt.p->row + top, keep, (u32)count);
    }
    /* The screen: the usual case, the whole of it moving up, waits for the
     * end of what is being read; anything else is done now. The notes of
     * what is still to draw move with their rows either way, and the rows
     * that come in are clear on the screen already. */
    if (n > 0 && top == 0 && bot == ROWS - 1 && scrolled + count < ROWS) {
        scrolled = (u8)(scrolled + count);
    } else {
        settle();
        con_scroll(top, bot, n > 0 ? count : -count);
    }
    if (n > 0) {
        memmove(dirty_from + top, dirty_from + top + count, (u32)(height - count));
        memmove(dirty_to + top, dirty_to + top + count, (u32)(height - count));
    } else {
        memmove(dirty_from + top + count, dirty_from + top, (u32)(height - count));
        memmove(dirty_to + top + count, dirty_to + top, (u32)(height - count));
    }
    for (r = 0; r < count; r++) {
        int row = n > 0 ? bot - r : top + r;

        memset(vt.p->ch[vt.p->row[row]], ' ', COLS);
        memset(vt.p->at[vt.p->row[row]], attr_blank(), COLS);
        dirty_from[row] = dirty_to[row] = 0;
        if (attr_blank())
            mark(row, 0, COLS);
    }
}

static void line_feed(void)
{
    if (vt.cy == vt.bot)
        scroll(vt.top, vt.bot, 1);
    else if (vt.cy < ROWS - 1)
        vt.cy++;
}

static void reverse_line_feed(void)
{
    if (vt.cy == vt.top)
        scroll(vt.top, vt.bot, -1);
    else if (vt.cy > 0)
        vt.cy--;
}

static void go(int col, int row)
{
    vt.cx = col < 0 ? 0 : col >= COLS ? COLS - 1 : col;
    vt.cy = row < 0 ? 0 : row >= ROWS ? ROWS - 1 : row;
    vt.wrap = 0;
}

static void show(u8 c)
{
    u8 a = attr_now();

    if (vt.wrap) {
        vt.wrap = 0;
        vt.cx = 0;
        line_feed();
    }
    if (vt.insert && vt.cx < COLS - 1) {
        memmove(vt.p->ch[vt.p->row[vt.cy]] + vt.cx + 1, vt.p->ch[vt.p->row[vt.cy]] + vt.cx, (u32)(COLS - 1 - vt.cx));
        memmove(vt.p->at[vt.p->row[vt.cy]] + vt.cx + 1, vt.p->at[vt.p->row[vt.cy]] + vt.cx, (u32)(COLS - 1 - vt.cx));
        redraw(vt.cy, vt.cx + 1, COLS);
    }
    vt.p->ch[vt.p->row[vt.cy]][vt.cx] = c;
    vt.p->at[vt.p->row[vt.cy]][vt.cx] = a;
    mark(vt.cy, vt.cx, vt.cx + 1);
    vt.last = c;
    if (vt_log)
        serial_putc((char)c);
    if (vt.cx < COLS - 1)
        vt.cx++;
    else if (vt.autowrap)
        vt.wrap = 1;
}

/* ------------------------------------------------------------ characters -- */

/* A Unicode character as one of ours, or '?'. */
static u8 our_char(u32 cp)
{
    int i;

    if ((cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF))
        return (u8)cp;
    if (cp >= 0x2500 && cp < 0x25A0)
        return charset_box[cp - 0x2500];    /* double and heavy lines as plain ones */
    for (i = 0; i < 32; i++)
        if (charset_extra[i] == cp)
            return (u8)(0x80 + i);
    if (cp >= 0x2800 && cp <= 0x28FF)       /* Braille, which programs draw spinners with */
        return cp == 0x2800 ? ' ' : 0xB7;
    switch (cp) {
    case 0x2018: case 0x2019: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: return '-';
    case 0x2015: return our_char(0x2014);
    case 0x203A: case 0x276F: case 0x25B6: case 0x25B8: case 0x25BA: case 0x279C: case 0x27A4:
    case 0x2B62: return '>';
    case 0x2039: case 0x276E: case 0x25C0: case 0x25C2: case 0x25C4: return '<';
    case 0x25B2: case 0x25B4: case 0x23F6: return '^';
    case 0x25BC: case 0x25BE: case 0x23F7: return 'v';
    case 0x2714: case 0x2705: return our_char(0x2713);
    case 0x2717: case 0x2718: case 0x2715: case 0x2716: case 0x274C: return 'x';
    case 0x25CB: case 0x25E6: case 0x25EF: case 0x25CC: return 'o';
    case 0x25C6: case 0x25C7: case 0x2666: case 0x2731: case 0x2733: case 0x273B:
    case 0x2219: case 0x22C5: case 0x25AA: case 0x25A0: case 0x25A1: case 0x25AB: case 0x2605:
    case 0x2606: case 0x23FA: case 0x25C9: case 0x2B24: return our_char(0x25CF);
    case 0x26A0: case 0x2757: return '!';
    case 0x21B5: case 0x23CE: return our_char(0x2190);
    case 0x21E7: return our_char(0x2191);
    case 0x2264: return '<';
    case 0x2265: return '>';
    case 0x2260: return '#';
    case 0x2003: case 0x2002: case 0x2009: case 0x200A: case 0x202F: case 0x3000: return ' ';
    case 0x23BF: return our_char(0x2514);
    }
    return '?';
}

/* Characters that take no room of their own (accents put on the one before,
 * marks that only change how the neighbours look). */
static int no_width(u32 cp)
{
    return (cp >= 0x300 && cp < 0x370) || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x2060 || cp == 0xFEFF ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

/* Characters that programs count as two columns wide. */
static int two_wide(u32 cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
           (cp >= 0x20000 && cp <= 0x3FFFD);
}

static void show_unicode(u32 cp)
{
    if (no_width(cp))
        return;
    show(our_char(cp));
    if (two_wide(cp))
        show(' ');
}

/* The VT100 line-drawing set, which stands in for the small letters. */
static u8 line_drawing(u8 c)
{
    static const u16 map[32] = {
        0x25C6, 0x2592, '?', '?', '?', '?', 0xB0, 0xB1, '?', '?', 0x2518, 0x2510, 0x250C, 0x2514,
        0x253C, 0x2500, 0x2500, 0x2500, 0x2500, 0x2500, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502,
        0x2264, 0x2265, 'p', 0x2260, 0xA3, 0xB7, ' ',
    };

    return (c >= 0x60 && c < 0x7F) ? our_char(map[c - 0x60]) : c;
}

/* ---------------------------------------------------------------- orders -- */

static void save_cursor(void)
{
    vt.saved[vt.alt].x = vt.cx;
    vt.saved[vt.alt].y = vt.cy;
    vt.saved[vt.alt].bold = vt.bold;
    vt.saved[vt.alt].under = vt.under;
    vt.saved[vt.alt].inv = vt.inv;
    vt.saved[vt.alt].dark = vt.darkbg;
}

static void restore_cursor(void)
{
    go(vt.saved[vt.alt].x, vt.saved[vt.alt].y);
    vt.bold = vt.saved[vt.alt].bold;
    vt.under = vt.saved[vt.alt].under;
    vt.inv = vt.saved[vt.alt].inv;
    vt.darkbg = vt.saved[vt.alt].dark;
}

static void clear_page(void)
{
    int r;

    for (r = 0; r < ROWS; r++)
        blank(r, 0, COLS);
}

/* Switch to the second screen (cleared first, if asked) or back. The cursor
 * stays where it is, as in xterm. */
static void second_screen(int on, int clear)
{
    if ((u8)on == vt.alt && !(on && clear))
        return;
    vt.alt = (u8)on;
    vt.p = &pages[on ? 1 : 0];
    if (on && clear) {
        memset(vt.p->ch, ' ', sizeof vt.p->ch);
        memset(vt.p->at, 0, sizeof vt.p->at);
    }
    vt.wrap = 0;
    redraw_all();
}

static void set_mode(int on)
{
    int i;

    for (i = 0; i < vt.npar; i++) {
        if (vt.mark == '?') {
            switch (vt.par[i]) {
            case 1: vt.app_keys = (u8)on; break;
            case 6: vt.origin = (u8)on; go(0, on ? vt.top : 0); break;
            case 7: vt.autowrap = (u8)on; vt.wrap = 0; break;
            case 25: vt.show_cursor = (u8)on; break;
            case 47: case 1047: second_screen(on, 1); break;
            case 1048: if (on) save_cursor(); else restore_cursor(); break;
            case 1049:
                if (on && !vt.alt)
                    save_cursor();
                second_screen(on, 1);
                if (!on)
                    restore_cursor();
                break;
            }
        } else if (!vt.mark && vt.par[i] == 4) {
            vt.insert = (u8)on;
        }
    }
}

/* Is this colour, as a background, light enough to pass for our white? */
static int light_16(int c)
{
    return c == 7 || c == 15;
}

static int light_256(int c)
{
    if (c < 16)
        return light_16(c);
    if (c >= 232)
        return c >= 250;
    c -= 16;
    return c / 36 + (c / 6) % 6 + c % 6 >= 13;
}

/* The numbers of an extended colour (38 or 48), starting after it. Returns
 * how many numbers it took and sets *light. */
static int colour(int i, int *light)
{
    int kind, n;

    *light = 1;
    if (i >= vt.npar)
        return 0;
    kind = vt.par[i];
    if (kind == 5) {
        if (i + 1 < vt.npar)
            *light = light_256(vt.par[i + 1]);
        return 2;
    }
    if (kind == 2) {
        /* 2;r;g;b, or with colons 2:space:r:g:b */
        for (n = 1; i + n < vt.npar && vt.sub[i + n] && n < 5; n++)
            ;
        if (n < 4)
            n = 4;
        if (i + n - 1 < vt.npar)
            *light = (vt.par[i + n - 3] * 2 + vt.par[i + n - 2] * 5 + vt.par[i + n - 1]) / 8 >= 200;
        return n;
    }
    return 1;
}

static void looks(void)
{
    int i, light, c;

    if (vt.npar == 0) {
        vt.bold = vt.under = vt.inv = vt.darkbg = 0;
        return;
    }
    for (i = 0; i < vt.npar; i++) {
        c = vt.par[i];
        if (vt.sub[i])
            continue;                       /* a detail of the one before: no use to us */
        switch (c) {
        case 0: vt.bold = vt.under = vt.inv = vt.darkbg = 0; break;
        case 1: vt.bold = 1; break;
        case 4: vt.under = !(i + 1 < vt.npar && vt.sub[i + 1] && vt.par[i + 1] == 0); break;
        case 7: vt.inv = 1; break;
        case 21: vt.under = 1; break;
        case 22: vt.bold = 0; break;
        case 24: vt.under = 0; break;
        case 27: vt.inv = 0; break;
        case 38: i += colour(i + 1, &light); break;
        case 48: i += colour(i + 1, &light); vt.darkbg = (u8)!light; break;
        case 49: vt.darkbg = 0; break;
        default:
            if (c >= 40 && c <= 47)
                vt.darkbg = (u8)!light_16(c - 40);
            else if (c >= 100 && c <= 107)
                vt.darkbg = (u8)!light_16(c - 100 + 8);
        }
    }
}

static void shift_cells(int n, int insert)
{
    u8 *c = vt.p->ch[vt.p->row[vt.cy]], *a = vt.p->at[vt.p->row[vt.cy]];
    int rest = COLS - vt.cx;

    if (n > rest)
        n = rest;
    if (insert) {
        memmove(c + vt.cx + n, c + vt.cx, (u32)(rest - n));
        memmove(a + vt.cx + n, a + vt.cx, (u32)(rest - n));
        memset(c + vt.cx, ' ', (u32)n);
        memset(a + vt.cx, attr_blank(), (u32)n);
    } else {
        memmove(c + vt.cx, c + vt.cx + n, (u32)(rest - n));
        memmove(a + vt.cx, a + vt.cx + n, (u32)(rest - n));
        memset(c + COLS - n, ' ', (u32)n);
        memset(a + COLS - n, attr_blank(), (u32)n);
    }
    redraw(vt.cy, vt.cx, COLS);
}

/* The order that ended with the character c, after Esc [. */
static void order(u8 c)
{
    int n = vt.npar ? vt.par[0] : 0, n1 = n ? n : 1, m = vt.npar > 1 ? vt.par[1] : 0, r;

    if (vt.inter) {
        if (vt.inter == '!' && c == 'p')    /* soft reset */
            vt_reset_modes();
        return;
    }
    if (vt.mark && c != 'h' && c != 'l' && c != 'c' && c != 'n')
        return;
    switch (c) {
    case 'A': go(vt.cx, vt.cy - n1 < vt.top && vt.cy >= vt.top ? vt.top : vt.cy - n1); break;
    case 'B': case 'e':
        go(vt.cx, vt.cy + n1 > vt.bot && vt.cy <= vt.bot ? vt.bot : vt.cy + n1); break;
    case 'C': case 'a': go(vt.cx + n1, vt.cy); break;
    case 'D': go(vt.cx - n1, vt.cy); break;
    case 'E': go(0, vt.cy + n1); break;
    case 'F': go(0, vt.cy - n1); break;
    case 'G': case '`': go(n1 - 1, vt.cy); break;
    case 'd': go(vt.cx, n1 - 1 + (vt.origin ? vt.top : 0)); break;
    case 'H': case 'f': go((m ? m : 1) - 1, n1 - 1 + (vt.origin ? vt.top : 0)); break;
    case 'I':
        while (n1-- && vt.cx < COLS - 1)
            go((vt.cx | 7) + 1, vt.cy);
        break;
    case 'Z':
        while (n1-- && vt.cx > 0)
            go((vt.cx - 1) & ~7, vt.cy);
        break;
    case 'J':
        if (n == 0) {
            blank(vt.cy, vt.cx, COLS - vt.cx);
            for (r = vt.cy + 1; r < ROWS; r++)
                blank(r, 0, COLS);
        } else if (n == 1) {
            for (r = 0; r < vt.cy; r++)
                blank(r, 0, COLS);
            blank(vt.cy, 0, vt.cx + 1);
        } else if (n == 2 || n == 3) {
            clear_page();
        }
        vt.wrap = 0;
        break;
    case 'K':
        if (n == 0)
            blank(vt.cy, vt.cx, COLS - vt.cx);
        else if (n == 1)
            blank(vt.cy, 0, vt.cx + 1);
        else if (n == 2)
            blank(vt.cy, 0, COLS);
        vt.wrap = 0;
        break;
    case 'L':
        if (vt.cy >= vt.top && vt.cy <= vt.bot) {
            scroll(vt.cy, vt.bot, -n1);
            go(0, vt.cy);
        }
        break;
    case 'M':
        if (vt.cy >= vt.top && vt.cy <= vt.bot) {
            scroll(vt.cy, vt.bot, n1);
            go(0, vt.cy);
        }
        break;
    case '@': shift_cells(n1, 1); vt.wrap = 0; break;
    case 'P': shift_cells(n1, 0); vt.wrap = 0; break;
    case 'X': blank(vt.cy, vt.cx, n1); vt.wrap = 0; break;
    case 'S': scroll(vt.top, vt.bot, n1); break;
    case 'T': scroll(vt.top, vt.bot, -n1); break;
    case 'b':
        if (vt.last)
            while (n1-- > 0)
                show(vt.last);
        break;
    case 'r':
        if (m == 0 || m > ROWS)
            m = ROWS;
        if (n1 < m) {
            vt.top = n1 - 1;
            vt.bot = m - 1;
            go(0, vt.origin ? vt.top : 0);
        }
        break;
    case 'm': looks(); break;
    case 'h': set_mode(1); break;
    case 'l': set_mode(0); break;
    case 's': save_cursor(); break;
    case 'u': restore_cursor(); break;
    case 'n':
        if (n == 5) {
            reply("\x1b[0n");
        } else if (n == 6) {
            reply("\x1b[");
            reply_num((u32)(vt.cy - (vt.origin ? vt.top : 0) + 1));
            reply(";");
            reply_num((u32)vt.cx + 1);
            reply("R");
        }
        break;
    case 'c':
        if (vt.mark == '>')
            reply("\x1b[>0;95;0c");
        else if (!vt.mark && n == 0)
            reply("\x1b[?1;2c");
        break;
    }
}

/* ------------------------------------------------------------ the outside -- */

void vt_reset_modes(void)
{
    vt.bold = vt.under = vt.inv = vt.darkbg = 0;
    vt.app_keys = vt.insert = vt.origin = 0;
    vt.autowrap = vt.show_cursor = 1;
    vt.top = 0;
    vt.bot = ROWS - 1;
    vt.set[0] = vt.set[1] = vt.shift = 0;
    vt.wrap = 0;
}

/* A fresh, empty screen. */
void vt_reset(void)
{
    u8 keep = vt.cursor_on;
    int i;

    if (keep)
        vt_cursor(0);
    memset(&vt, 0, sizeof vt);
    vt.p = &pages[0];
    vt_reset_modes();
    memset(pages, 0, sizeof pages);
    memset(pages[0].ch, ' ', sizeof pages[0].ch);
    memset(pages[1].ch, ' ', sizeof pages[1].ch);
    for (i = 0; i < ROWS; i++)
        pages[0].row[i] = pages[1].row[i] = (u8)i;
    memset(dirty_from, 0, sizeof dirty_from);
    memset(dirty_to, 0, sizeof dirty_to);
    scrolled = 0;
    con_clear();
}

/* Show or hide the cursor block. Programs can ask for it to stay hidden. */
void vt_cursor(int on)
{
    if (!vt.p)
        return;
    if (on && !vt.show_cursor)
        on = 0;
    if (vt.cursor_on != (u8)on) {
        flush();                /* the block goes on what is really there */
        con_invert(vt.cx, vt.cy);
        vt.cursor_on = (u8)on;
    }
}

/* Do the arrow keys send their "application" form just now? */
int vt_app_keys(void)
{
    return vt.app_keys;
}

/* What the terminal has to say back to the program; empties it. */
int vt_reply(u8 *out, int max)
{
    int n = vt.nreply < max ? vt.nreply : max;

    memcpy(out, vt.reply, (u32)n);
    memmove(vt.reply, vt.reply + n, (u32)(vt.nreply - n));
    vt.nreply = (u8)(vt.nreply - n);
    return n;
}

/* Leave the screen ready for plain text below what is on it. */
void vt_end(void)
{
    if (!vt.p)
        return;
    vt_cursor(0);
    if (vt.alt)
        second_screen(0, 0);
    flush();
    con_goto(0, vt.cy);
    if (vt.cx > 0 || vt.wrap)
        con_putc('\n');
}

void vt_write(const u8 *p, u32 n)
{
    u8 b;

    if (!vt.p)
        vt_reset();
    vt_cursor(0);
    while (n--) {
        b = *p++;
        if (vt.state == STRING) {           /* a title or the like: until Bell or Esc \ */
            if (b == 7)
                vt.state = GROUND;
            else if (b == 0x1B)
                vt.state = STRING_ESC;
            continue;
        }
        if (vt.state == STRING_ESC) {
            vt.state = b == '\\' ? GROUND : b == 0x1B ? STRING_ESC : STRING;
            continue;
        }
        /* Plain letters, one after another on a row, are most of what comes:
         * they go straight into the row, with none of the care below. */
        if (b >= 0x20 && b < 0x7F && vt.state == GROUND && !vt.more && !vt.wrap && !vt.insert &&
            !vt.set[vt.shift] && !vt_log && vt.cx < COLS - 1) {
            u8 *ch = vt.p->ch[vt.p->row[vt.cy]], *at = vt.p->at[vt.p->row[vt.cy]], a = attr_now();
            int x = vt.cx, from = x;

            for (;;) {
                ch[x] = b;
                at[x] = a;
                x++;
                if (!n || x >= COLS - 1 || *p < 0x20 || *p >= 0x7F)
                    break;
                b = *p++;
                n--;
            }
            vt.cx = x;
            vt.last = b;
            mark(vt.cy, from, x);
            continue;
        }
        /* Control characters act at once, even in the middle of an order. */
        if (b < 32) {
            vt.more = 0;
            switch (b) {
            case 0x1B: vt.state = ESC; break;
            case '\r': go(0, vt.cy); break;
            case '\n': case 0x0B: case 0x0C:
                line_feed();
                vt.wrap = 0;
                if (vt_log) {
                    serial_putc('\r');
                    serial_putc('\n');
                }
                break;
            case '\b': if (vt.cx > 0) go(vt.cx - 1, vt.cy); break;
            case '\t': go(vt.cx < COLS - 1 ? (vt.cx | 7) + 1 : vt.cx, vt.cy); break;
            case 7: snd_beep(6); break;
            case 0x0E: vt.shift = 1; break;
            case 0x0F: vt.shift = 0; break;
            }
            continue;
        }
        switch (vt.state) {
        case ESC:
            vt.state = GROUND;
            switch (b) {
            case '[':
                vt.state = CSI;
                vt.mark = vt.inter = vt.npar = vt.digits = 0;
                memset(vt.par, 0, sizeof vt.par);
                memset(vt.sub, 0, sizeof vt.sub);
                break;
            case ']': case 'P': case '^': case '_': case 'X': vt.state = STRING; break;
            case '(': case ')': case '*': case '+': case '#': case '%': case ' ':
                vt.state = ESC_ONE;
                vt.inter = b;
                break;
            case '7': save_cursor(); break;
            case '8': restore_cursor(); break;
            case 'D': line_feed(); break;
            case 'E': go(0, vt.cy); line_feed(); break;
            case 'M': reverse_line_feed(); break;
            case 'c': vt_reset(); break;
            }
            continue;
        case ESC_ONE:
            vt.state = GROUND;
            if (vt.inter == '(')
                vt.set[0] = b == '0';
            else if (vt.inter == ')')
                vt.set[1] = b == '0';
            vt.inter = 0;
            continue;
        case CSI:
            if (b >= '0' && b <= '9') {
                /* npar is the place of the number being read until the end */
                vt.digits = 1;
                if (vt.par[vt.npar] < 6000)
                    vt.par[vt.npar] = (u16)(vt.par[vt.npar] * 10 + (b - '0'));
            } else if (b == ';' || b == ':') {
                vt.digits = 1;
                if (vt.npar < NPAR - 1) {
                    vt.npar++;
                    vt.sub[vt.npar] = b == ':';
                }
            } else if (b >= '<' && b <= '?') {
                vt.mark = b;
            } else if (b >= 0x20 && b < 0x30) {
                vt.inter = b;
            } else if (b >= 0x40 && b < 0x7F) {
                vt.state = GROUND;
                if (vt.digits)
                    vt.npar++;              /* now it is how many numbers there are */
                order(b);
            } else {
                vt.state = GROUND;
            }
            continue;
        }
        /* Text. */
        if (vt.more) {
            if ((b & 0xC0) == 0x80) {
                vt.cp = (vt.cp << 6) | (b & 0x3F);
                if (--vt.more == 0)
                    show_unicode(vt.cp);
                continue;
            }
            vt.more = 0;        /* cut short: show that, then take b as it comes */
            show('?');
        }
        if (b == 127) {
            ;
        } else if (b < 0x80) {
            show(vt.set[vt.shift] ? line_drawing(b) : b);
        } else if (b >= 0xC2 && b < 0xE0) {
            vt.more = 1;
            vt.cp = b & 0x1F;
        } else if (b >= 0xE0 && b < 0xF0) {
            vt.more = 2;
            vt.cp = b & 0x0F;
        } else if (b >= 0xF0 && b < 0xF5) {
            vt.more = 3;
            vt.cp = b & 0x07;
        } else {
            show('?');
        }
    }
    flush();
}
