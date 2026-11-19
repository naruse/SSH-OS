/* Text console drawn straight into video memory.
 *
 * 85 x 32 characters in a 6 x 10 pixel cell, black on white, below the bar
 * that bar.c keeps at the top of the screen. This version is
 * plain C and favours being obviously correct; the hot paths get rewritten in
 * assembly once the terminal needs the speed.
 */
#include "kernel.h"
#include "hw.h"

#define CELL_W  6
#define CELL_H  10
#define ORIGIN_X 0      /* 85 * 6 = 510 of 512 pixels; at 0, four cells are three whole bytes */
#define ORIGIN_Y (BAR_H + 1)    /* below the bar: 32 * 10 = 320 of the 322 rows left */

extern const unsigned char font6x10[224][10];   /* codes 32-255 */

static u8 *fb;
static int cur_col, cur_row;
static int inverse;

/* Draw one character cell. A cell is 6 pixels wide and can start at any bit,
 * so it spans at most two bytes of each screen row. All access is bytewise:
 * the cell may start at an odd address. */
static void draw_cell_attr(int col, int row, unsigned char ch, u8 attr)
{
    const unsigned char *glyph;
    unsigned x = (unsigned)col * CELL_W + ORIGIN_X;
    unsigned shift = x & 7;
    u16 mask = (u16)(0xFC00u >> shift);
    u8 mask_hi = (u8)(mask >> 8), mask_lo = (u8)mask;
    u8 flip = (attr & CON_INVERSE) ? 0xFC : 0x00;
    u8 *p = fb + ((unsigned)row * CELL_H + ORIGIN_Y) * SCREEN_ROWBYTES + (x >> 3);
    u8 g;
    int i;

    if (ch < 32)
        ch = 127;                           /* no such character: a block */
    glyph = font6x10[ch - 32];

    for (i = 0; i < CELL_H; i++) {
        u16 bits;

        g = glyph[i];
        if (attr & CON_BOLD)
            g |= (u8)(g >> 1);              /* thicken by one pixel */
        if ((attr & CON_UNDERLINE) && i == CELL_H - 1)
            g = 0xFC;
        bits = (u16)(((u16)((g ^ flip) & 0xFC) << 8) >> shift);
        p[0] = (u8)((p[0] & ~mask_hi) | (bits >> 8));
        if (mask_lo)
            p[1] = (u8)((p[1] & ~mask_lo) | (u8)bits);
        p += SCREEN_ROWBYTES;
    }
}

static void draw_cell(int col, int row, unsigned char ch)
{
    draw_cell_attr(col, row, ch, inverse ? CON_INVERSE : 0);
}

static void clear_text_row(int row)
{
    memset(fb + ((unsigned)row * CELL_H + ORIGIN_Y) * SCREEN_ROWBYTES, 0,
           CELL_H * SCREEN_ROWBYTES);
}

static void scroll_up(void)
{
    u8 *top = fb + ORIGIN_Y * SCREEN_ROWBYTES;

    memmove(top, top + CELL_H * SCREEN_ROWBYTES,
            (CON_ROWS - 1) * CELL_H * SCREEN_ROWBYTES);
    clear_text_row(CON_ROWS - 1);
}

static void newline(void)
{
    cur_col = 0;
    if (++cur_row >= CON_ROWS) {
        cur_row = CON_ROWS - 1;
        scroll_up();
    }
}

void con_init(u8 *screen)
{
    fb = screen;
    inverse = 0;
    con_clear();
}

void con_clear(void)
{    gfx_pointer_park();

    memset(fb + BAR_H * SCREEN_ROWBYTES, 0, SCREEN_ROWBYTES * (SCREEN_H - BAR_H));
    cur_col = cur_row = 0;
}

void con_set_inverse(int on)
{
    inverse = on;
}

void con_goto(int col, int row)
{
    if (col >= 0 && col < CON_COLS)
        cur_col = col;
    if (row >= 0 && row < CON_ROWS)
        cur_row = row;
}

int con_row(void)
{
    return cur_row;
}

void con_putc(char c)
{    gfx_pointer_park();

    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        cur_col = 0;
        return;
    case '\b':
        if (cur_col > 0)
            cur_col--;
        return;
    case '\t':
        do {
            con_putc(' ');
        } while (cur_col & 7);
        return;
    }
    if (cur_col >= CON_COLS)
        newline();
    draw_cell(cur_col, cur_row, (unsigned char)c);
    cur_col++;
}

void con_draw(int col, int row, const u8 *text, int n, u8 attr)
{    gfx_pointer_park();

    if (row < 0 || row >= CON_ROWS || col < 0)
        return;
    for (; n > 0 && col < CON_COLS; n--, col++)
        draw_cell_attr(col, row, *text++, attr);
}

void con_fill(int col, int row, int n, u8 ch, u8 attr)
{    gfx_pointer_park();

    if (row < 0 || row >= CON_ROWS || col < 0)
        return;
    /* A whole row of plain blanks is the common case: clear it in one go. */
    if (col == 0 && n >= CON_COLS && ch == ' ' && !(attr & (CON_INVERSE | CON_UNDERLINE))) {
        clear_text_row(row);
        return;
    }
    for (; n > 0 && col < CON_COLS; n--, col++)
        draw_cell_attr(col, row, ch, attr);
}

/* One row of a cell's picture, as its top six bits. */
static u8 cell_bits(const unsigned char *glyph, u8 attr, int i)
{
    u8 g = glyph[i];

    if (attr & CON_BOLD)
        g |= (u8)(g >> 1);
    if ((attr & CON_UNDERLINE) && i == CELL_H - 1)
        g = 0xFC;
    if (attr & CON_INVERSE)
        g = (u8)~g;
    return g & 0xFC;
}

/* Draw cells from..to-1 of a row, given the whole row: its characters and
 * how each looks. This is the terminal's way to the screen, and the quick
 * one: four cells are 24 pixels, three whole bytes, so they are put
 * together and stored without reading the screen or shifting into it. The
 * span is widened to whole fours for that, which is why the whole row must
 * be given. Some eight times quicker than a cell at a time. */
void con_draw_cells(int row, int from, int to, const u8 *ch, const u8 *at)
{
    const unsigned char *g0, *g1, *g2, *g3;
    u8 a, b, c, d;
    u8 *p;
    int col, i;

    gfx_pointer_park();
    if (row < 0 || row >= CON_ROWS)
        return;
    if (from < 0)
        from = 0;
    if (to > CON_COLS)
        to = CON_COLS;
    for (col = from & ~3; col < to; col += 4) {
        if (col + 4 > CON_COLS) {           /* the odd cell at the end of the row */
            for (; col < CON_COLS; col++)
                draw_cell_attr(col, row, ch[col], at[col]);
            break;
        }
        p = fb + ((unsigned)row * CELL_H + ORIGIN_Y) * SCREEN_ROWBYTES + (unsigned)(col >> 2) * 3;
        if (!(at[col] | at[col + 1] | at[col + 2] | at[col + 3])) {
            if (ch[col] == ' ' && ch[col + 1] == ' ' && ch[col + 2] == ' ' && ch[col + 3] == ' ') {
                for (i = 0; i < CELL_H; i++, p += SCREEN_ROWBYTES)
                    p[0] = p[1] = p[2] = 0;
                continue;
            }
            g0 = font6x10[(ch[col] < 32 ? 127 : ch[col]) - 32];
            g1 = font6x10[(ch[col + 1] < 32 ? 127 : ch[col + 1]) - 32];
            g2 = font6x10[(ch[col + 2] < 32 ? 127 : ch[col + 2]) - 32];
            g3 = font6x10[(ch[col + 3] < 32 ? 127 : ch[col + 3]) - 32];
            for (i = 0; i < CELL_H; i++, p += SCREEN_ROWBYTES) {
                a = *g0++ & 0xFC;
                b = *g1++ & 0xFC;
                c = *g2++ & 0xFC;
                d = *g3++ & 0xFC;
                p[0] = (u8)(a | (b >> 6));
                p[1] = (u8)((b << 2) | (c >> 4));
                p[2] = (u8)((c << 4) | (d >> 2));
            }
            continue;
        }
        g0 = font6x10[(ch[col] < 32 ? 127 : ch[col]) - 32];
        g1 = font6x10[(ch[col + 1] < 32 ? 127 : ch[col + 1]) - 32];
        g2 = font6x10[(ch[col + 2] < 32 ? 127 : ch[col + 2]) - 32];
        g3 = font6x10[(ch[col + 3] < 32 ? 127 : ch[col + 3]) - 32];
        for (i = 0; i < CELL_H; i++, p += SCREEN_ROWBYTES) {
            a = cell_bits(g0, at[col], i);
            b = cell_bits(g1, at[col + 1], i);
            c = cell_bits(g2, at[col + 2], i);
            d = cell_bits(g3, at[col + 3], i);
            p[0] = (u8)(a | (b >> 6));
            p[1] = (u8)((b << 2) | (c >> 4));
            p[2] = (u8)((c << 4) | (d >> 2));
        }
    }
}

void con_scroll(int top, int bottom, int lines)
{    gfx_pointer_park();

    u8 *base;
    int height, n, i;
    u32 rowbytes = CELL_H * SCREEN_ROWBYTES;

    if (top < 0 || bottom >= CON_ROWS || top > bottom || lines == 0)
        return;
    height = bottom - top + 1;
    n = lines < 0 ? -lines : lines;
    if (n > height)
        n = height;
    base = fb + ((unsigned)top * CELL_H + ORIGIN_Y) * SCREEN_ROWBYTES;
    if (lines > 0) {
        memmove(base, base + (u32)n * rowbytes, (u32)(height - n) * rowbytes);
        for (i = bottom - n + 1; i <= bottom; i++)
            clear_text_row(i);
    } else {
        memmove(base + (u32)n * rowbytes, base, (u32)(height - n) * rowbytes);
        for (i = top; i < top + n; i++)
            clear_text_row(i);
    }
}

void con_invert(int col, int row)
{
    int save_col = cur_col, save_row = cur_row;

    if (col < 0 || col >= CON_COLS || row < 0 || row >= CON_ROWS)
        return;
    cur_col = col;
    cur_row = row;
    con_cursor_toggle();
    cur_col = save_col;
    cur_row = save_row;
}

/* Invert the cell under the cursor (call twice to restore it). */
void con_cursor_toggle(void)
{    gfx_pointer_park();

    unsigned x, shift;
    u16 mask;
    u8 *p;
    int i;

    if (cur_col >= CON_COLS)
        return;
    x = (unsigned)cur_col * CELL_W + ORIGIN_X;
    shift = x & 7;
    mask = (u16)(0xFC00u >> shift);
    p = fb + ((unsigned)cur_row * CELL_H + ORIGIN_Y) * SCREEN_ROWBYTES + (x >> 3);
    for (i = 0; i < CELL_H; i++) {
        p[0] ^= (u8)(mask >> 8);
        if ((u8)mask)
            p[1] ^= (u8)mask;
        p += SCREEN_ROWBYTES;
    }
}
