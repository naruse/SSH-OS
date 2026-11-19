/* Graphics for programs that draw their own screen: rectangles, patterns,
 * text at any position, small pictures, and the mouse pointer.
 *
 * The screen is 512 x 342, one bit per pixel, 1 = black, 64 bytes per row.
 * Everything is clipped to the screen.
 *
 * The pointer is drawn from the 60 Hz interrupt so it keeps moving while
 * the program is busy (waiting for the network, say). Anything else that
 * draws must bracket its work with gfx_begin() / gfx_end(): between them
 * the pointer stays where it is, and is taken off the screen only if
 * something is drawn where it lies; gfx_end() puts it back.
 */
#include "kernel.h"
#include "hw.h"

extern const unsigned char font6x10[224][10];

static u8 *fb;

static void ptr_prepare(void);

void gfx_init(u8 *screen)
{
    fb = screen;
    ptr_prepare();
}

/* The byte a pattern puts in screen row y. */
static u8 pattern_byte(int pattern, int y)
{
    switch (pattern) {
    case GFX_BLACK: return 0xFF;
    case GFX_GRAY:  return (y & 1) ? 0x55 : 0xAA;
    case GFX_LIGHT: return (y & 1) ? 0x00 : ((y & 2) ? 0x22 : 0x88);
    case GFX_DARK:  return (y & 1) ? 0xFF : ((y & 2) ? 0xDD : 0x77);
    }
    return 0x00;
}

static void ptr_clear_of(int x, int y, int w, int h);

void gfx_fill(int x, int y, int w, int h, int pattern)
{
    int x1 = x + w, y1 = y + h, first, last, n;
    u8 lmask, rmask, pat;
    u8 *p;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    if (x >= x1 || y >= y1)
        return;
    ptr_clear_of(x, y, x1 - x, y1 - y);
    first = x >> 3;
    last = (x1 - 1) >> 3;
    lmask = (u8)(0xFF >> (x & 7));
    rmask = (u8)(0xFF << (7 - ((x1 - 1) & 7)));
    if (first == last) {
        lmask &= rmask;
        rmask = 0;
    }
    n = last - first - 1;
    for (; y < y1; y++) {
        p = fb + (u32)y * SCREEN_ROWBYTES + first;
        if (pattern == GFX_INVERT) {
            *p++ ^= lmask;
            if (n > 0) {
                int i;

                for (i = 0; i < n; i++)
                    *p++ ^= 0xFF;
            }
            if (rmask)
                *p ^= rmask;
            continue;
        }
        pat = pattern_byte(pattern, y);
        *p = (u8)((*p & ~lmask) | (pat & lmask));
        p++;
        if (n > 0) {
            memset(p, pat, (size_t)n);
            p += n;
        }
        if (rmask)
            *p = (u8)((*p & ~rmask) | (pat & rmask));
    }
}

void gfx_frame(int x, int y, int w, int h, int thickness)
{
    gfx_fill(x, y, w, thickness, GFX_BLACK);
    gfx_fill(x, y + h - thickness, w, thickness, GFX_BLACK);
    gfx_fill(x, y, thickness, h, GFX_BLACK);
    gfx_fill(x + w - thickness, y, thickness, h, GFX_BLACK);
}

/* A rectangle with its corners cut round (radius 4), filled with a pattern
 * and outlined in black. */
void gfx_round_box(int x, int y, int w, int h, int pattern, int thickness)
{
    /* How far each of the first four rows is set in from the side. */
    static const u8 inset[4] = { 4, 2, 1, 1 };
    int i, t;

    if (w < 10 || h < 10) {
        gfx_fill(x, y, w, h, pattern);
        gfx_frame(x, y, w, h, 1);
        return;
    }
    for (i = 0; i < 4; i++) {
        gfx_fill(x + inset[i], y + i, w - 2 * inset[i], 1, pattern);
        gfx_fill(x + inset[i], y + h - 1 - i, w - 2 * inset[i], 1, pattern);
    }
    gfx_fill(x, y + 4, w, h - 8, pattern);
    for (t = 0; t < thickness; t++) {
        gfx_fill(x + 4, y + t, w - 8, 1, GFX_BLACK);
        gfx_fill(x + 4, y + h - 1 - t, w - 8, 1, GFX_BLACK);
        gfx_fill(x + t, y + 4, 1, h - 8, GFX_BLACK);
        gfx_fill(x + w - 1 - t, y + 4, 1, h - 8, GFX_BLACK);
        /* the four corners, as short diagonal steps */
        gfx_fill(x + 2 + t, y + 1 + t, 2, 1, GFX_BLACK);
        gfx_fill(x + 1 + t, y + 2 + t, 1, 2, GFX_BLACK);
        gfx_fill(x + w - 4 - t, y + 1 + t, 2, 1, GFX_BLACK);
        gfx_fill(x + w - 2 - t, y + 2 + t, 1, 2, GFX_BLACK);
        gfx_fill(x + 2 + t, y + h - 2 - t, 2, 1, GFX_BLACK);
        gfx_fill(x + 1 + t, y + h - 4 - t, 1, 2, GFX_BLACK);
        gfx_fill(x + w - 4 - t, y + h - 2 - t, 2, 1, GFX_BLACK);
        gfx_fill(x + w - 2 - t, y + h - 4 - t, 1, 2, GFX_BLACK);
    }
}

/* Put up to 16 pixels (the top bits of `bits`) at (x, y) in black ink, or
 * in white ink (erasing) when white is set. Pixels off the screen are lost. */
static void put_bits(int x, int y, u16 bits, int width, int white)
{
    u32 wide;
    u8 *p;
    int i, shift;

    if (y < 0 || y >= SCREEN_H || x <= -width || x >= SCREEN_W)
        return;
    if (x < 0) {
        bits = (u16)(bits << -x);
        x = 0;
    }
    ptr_clear_of(x, y, 16, 1);
    shift = x & 7;
    wide = ((u32)bits << 8) >> shift;       /* 24 bits across three bytes */
    p = fb + (u32)y * SCREEN_ROWBYTES + (x >> 3);
    for (i = 0; i < 3; i++) {
        u8 b = (u8)(wide >> (16 - 8 * i));

        if (b && (x >> 3) + i < SCREEN_ROWBYTES) {
            if (white)
                p[i] &= (u8)~b;
            else
                p[i] |= b;
        }
    }
}

/* Each bit doubled: abcdef.. becomes aabbccddeeff. */
static u16 widen(u8 g)
{
    u16 out = 0;
    int i;

    for (i = 0; i < 8; i++)
        if (g & (0x80 >> i))
            out |= (u16)(0xC000 >> (2 * i));
    return out;
}

int gfx_text_width(const char *s, int scale)
{
    return (int)strlen(s) * 6 * scale;
}

/* Text with a transparent background. scale 1 = 6 x 10 pixels per
 * character, 2 = 12 x 20. */
void gfx_text(int x, int y, const char *s, int white, int scale, int bold)
{
    const unsigned char *glyph;
    unsigned char ch;
    int row;
    u8 g;

    for (; *s; s++, x += 6 * scale) {
        ch = (unsigned char)*s;
        if (ch < 32 || ch == 127)
            ch = '?';
        glyph = font6x10[ch - 32];
        for (row = 0; row < 10; row++) {
            g = glyph[row] & 0xFC;
            if (bold)
                g |= (u8)(g >> 1);
            if (!g)
                continue;
            if (scale == 1) {
                put_bits(x, y + row, (u16)(g << 8), 8, white);
            } else {
                put_bits(x, y + 2 * row, widen(g), 16, white);
                put_bits(x, y + 2 * row + 1, widen(g), 16, white);
            }
        }
    }
}

/* A picture 16 pixels wide: h rows of 16 bits, drawn in black or white
 * ink, at its own size (scale 1) or doubled (scale 2). */
void gfx_picture(int x, int y, const u16 *rows, int h, int white, int scale)
{
    int i, k;

    for (i = 0; i < h; i++) {
        if (!rows[i])
            continue;
        if (scale == 1) {
            put_bits(x, y + i, rows[i], 16, white);
            continue;
        }
        for (k = 0; k < 2; k++) {
            put_bits(x, y + 2 * i + k, widen((u8)(rows[i] >> 8)), 16, white);
            put_bits(x + 16, y + 2 * i + k, widen((u8)rows[i]), 16, white);
        }
    }
}

/* ------------------------------------------------------------ pointer -- */

/* An icon from fonts/icons.txt: its black pixels are drawn black, or white
 * when white is set; the others are left as they are. */
void gfx_icon(int x, int y, const struct icon *icon, int white)
{
    int rowbytes = (icon->w + 7) / 8, row, b;
    const u8 *p = icon->bits;
    u16 bits;

    for (row = 0; row < icon->h; row++, p += rowbytes)
        for (b = 0; b < rowbytes; b += 2) {
            bits = (u16)(p[b] << 8);
            if (b + 1 < rowbytes)
                bits |= p[b + 1];
            if (bits)
                put_bits(x + 8 * b, y + row, bits, 16, white);
        }
}

#define PTR_H 16

/* The classic arrow: outline in black, inside in white. `shape` is every
 * pixel the arrow covers, `ink` the black ones. */
static const u16 ptr_shape[PTR_H] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780, 0x0380,
};
static const u16 ptr_ink[PTR_H] = {
    0x8000, 0xC000, 0xA000, 0x9000, 0x8800, 0x8400, 0x8200, 0x8100,
    0x8080, 0x83C0, 0x9200, 0xA900, 0xC900, 0x8480, 0x0480, 0x0380,
};

static struct {
    volatile u8 wanted;         /* the program wants a pointer */
    volatile u8 held;           /* >0 while the program is drawing */
    volatile u8 parked;         /* taken off the screen until the program is idle again */
    u8 drawn;
    s16 x, y;                   /* where it is drawn */
    u8 saved[PTR_H][3];         /* the screen underneath */
} ptr;

/* Both of these run in the 60 Hz interrupt, which comes as the screen's
 * blanking starts: taking the pointer off and putting it back must be over
 * before the picture is next sent out (about a thousandth of a second), or
 * a frame goes out without it and it flickers. Hence the plain code. */
static void ptr_erase(void)
{
    int rows, n = SCREEN_ROWBYTES - (ptr.x >> 3);
    const u8 *s = ptr.saved[0];
    u8 *p;

    if (!ptr.drawn)
        return;
    ptr.drawn = 0;
    rows = SCREEN_H - ptr.y;
    if (rows > PTR_H)
        rows = PTR_H;
    p = fb + (u32)ptr.y * SCREEN_ROWBYTES + (ptr.x >> 3);
    if (n >= 3) {
        for (; rows > 0; rows--, s += 3, p += SCREEN_ROWBYTES) {
            p[0] = s[0];
            p[1] = s[1];
            p[2] = s[2];
        }
    } else {
        for (; rows > 0; rows--, s += 3, p += SCREEN_ROWBYTES) {
            p[0] = s[0];
            if (n > 1)
                p[1] = s[1];
        }
    }
}

/* The arrow at each of the eight places it can have within a byte, worked
 * out once: for every row three bytes to keep (a mask) and three of ink. */
static u8 ptr_table[8][PTR_H][6];

static void ptr_prepare(void)
{
    int shift, row, i;
    u32 shape, ink;

    for (shift = 0; shift < 8; shift++)
        for (row = 0; row < PTR_H; row++) {
            shape = ((u32)ptr_shape[row] << 8) >> shift;
            ink = ((u32)ptr_ink[row] << 8) >> shift;
            for (i = 0; i < 3; i++) {
                ptr_table[shift][row][2 * i] = (u8)~(shape >> (16 - 8 * i));
                ptr_table[shift][row][2 * i + 1] = (u8)(ink >> (16 - 8 * i));
            }
        }
}

static void ptr_draw(void)
{
    int rows, n;
    const u8 *t;
    u8 *s = ptr.saved[0];
    u8 *p;
    u8 v;

    ptr.x = mouse.x;
    ptr.y = mouse.y;
    t = ptr_table[ptr.x & 7][0];
    n = SCREEN_ROWBYTES - (ptr.x >> 3);
    rows = SCREEN_H - ptr.y;
    if (rows > PTR_H)
        rows = PTR_H;
    p = fb + (u32)ptr.y * SCREEN_ROWBYTES + (ptr.x >> 3);
    if (n >= 3) {
        for (; rows > 0; rows--, p += SCREEN_ROWBYTES - 2) {
            v = *p; *s++ = v; *p++ = (u8)((v & t[0]) | t[1]);
            v = *p; *s++ = v; *p++ = (u8)((v & t[2]) | t[3]);
            v = *p; *s++ = v; *p = (u8)((v & t[4]) | t[5]);
            t += 6;
        }
    } else {
        for (; rows > 0; rows--, p += SCREEN_ROWBYTES, s += 3, t += 6) {
            s[0] = p[0];
            p[0] = (u8)((p[0] & t[0]) | t[1]);
            if (n > 1) {
                s[1] = p[1];
                p[1] = (u8)((p[1] & t[2]) | t[3]);
            }
        }
    }
    ptr.drawn = 1;
}

/* Something is about to be drawn in this rectangle (or read from it): if
 * the pointer lies there, take it off first. Only between gfx_begin() and
 * gfx_end(), when the interrupt leaves the pointer alone. */
static void ptr_clear_of(int x, int y, int w, int h)
{
    int px;

    if (!ptr.drawn || !ptr.held)
        return;
    px = ptr.x & ~7;            /* it is saved and restored in whole bytes */
    if (x < px + 24 && x + w > px && y < ptr.y + PTR_H && y + h > ptr.y)
        ptr_erase();
}

/* Called from the 60 Hz interrupt. */
void gfx_pointer_tick(void)
{
    if (!ptr.wanted || ptr.held || ptr.parked)
        return;
    if (ptr.drawn && ptr.x == mouse.x && ptr.y == mouse.y)
        return;
    ptr_erase();
    ptr_draw();
}

void gfx_pointer(int on)
{
    u32 was;

    was = cpu_irq_save();
    ptr.wanted = (u8)(on != 0);
    if (!on)
        ptr_erase();
    else if (!ptr.held && !ptr.drawn && !ptr.parked)    /* parked: the next tick after idle draws it */
        ptr_draw();
    cpu_irq_restore(was);
}

/* Around any drawing while the pointer is on. Calls may be nested. */
void gfx_begin(void)
{
    u32 was;

    was = cpu_irq_save();
    ptr.held++;
    cpu_irq_restore(was);
}

void gfx_end(void)
{
    u32 was;

    was = cpu_irq_save();
    if (ptr.held && --ptr.held == 0 && ptr.wanted && !ptr.parked &&
        !(ptr.drawn && ptr.x == mouse.x && ptr.y == mouse.y)) {
        ptr_erase();
        ptr_draw();
    }
    cpu_irq_restore(was);
}

/* For the text screen, which draws a character at a time: take the pointer
 * away at the first one (a flag test from then on) and let it come back
 * when the program next has nothing to do (bar_idle). */
void gfx_pointer_park(void)
{
    u32 was;

    if (ptr.parked)
        return;
    was = cpu_irq_save();
    ptr.parked = 1;
    ptr_erase();
    cpu_irq_restore(was);
}

void gfx_pointer_unpark(void)
{
    ptr.parked = 0;             /* the next tick draws it */
}

/* Keep a rectangle of the screen (widened to whole bytes) and put it back:
 * for menus and windows that open over whatever is there. One at a time. */
static u8 kept[12000];
static int kept_x, kept_y, kept_w, kept_h;      /* in bytes across, rows down */

void gfx_save(int x, int y, int w, int h)
{
    int row, x1 = (x + w + 7) >> 3;

    kept_x = x >> 3;
    if (x1 > SCREEN_ROWBYTES)
        x1 = SCREEN_ROWBYTES;
    kept_w = x1 - kept_x;
    kept_y = y;
    kept_h = y + h > SCREEN_H ? SCREEN_H - y : h;
    while (kept_w * kept_h > (int)sizeof kept)
        kept_h--;
    ptr_clear_of(kept_x * 8, kept_y, kept_w * 8, kept_h);
    for (row = 0; row < kept_h; row++)
        memcpy(kept + row * kept_w, fb + (u32)(kept_y + row) * SCREEN_ROWBYTES + kept_x,
               (size_t)kept_w);
}

void gfx_restore(void)
{
    int row;

    ptr_clear_of(kept_x * 8, kept_y, kept_w * 8, kept_h);
    for (row = 0; row < kept_h; row++)
        memcpy(fb + (u32)(kept_y + row) * SCREEN_ROWBYTES + kept_x, kept + row * kept_w,
               (size_t)kept_w);
    kept_h = 0;
}
