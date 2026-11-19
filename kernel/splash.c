/* The start-up screen: the mark, the name, who made it, and a row of small
 * pictures that fills in from the left as each part of the system comes up.
 * While it shows, what the kernel prints goes to the serial port only.
 *
 * The loader (boot/boot.S) has already drawn the name on a white screen, in
 * the same place and from the same pixels (tools/mkbanner.py), so nothing
 * jumps when the kernel takes over.
 */
#include "kernel.h"
#include "hw.h"

extern const unsigned char banner_name_bytes;
extern const unsigned char banner_name[8][24];

/* These must agree with boot/boot.S. */
#define NAME_XB     20          /* in bytes: 160 pixels */
#define NAME_Y      164
#define NAME_VSCALE 4

#define LOGO_Y      50
#define BY_Y        206
#define ROW_Y       256
#define FOOT_Y      310
#define ROW_STEP    36
#define ROW_COUNT   5

static const char by[] = "by:", who[] = SSHOS_AUTHOR, where[] = SSHOS_SOURCE;
static int shown;
static u8 up;

static void swallow(char c)
{
    (void)c;
}

void splash_start(void)
{
    u8 *p;
    int r, k;

    gfx_fill(0, 0, SCREEN_W, SCREEN_H, GFX_WHITE);
    gfx_icon((SCREEN_W - 96) / 2, LOGO_Y, &icon_logo, 0);
    p = machine.screen + NAME_Y * SCREEN_ROWBYTES + NAME_XB;
    for (r = 0; r < 8; r++)
        for (k = 0; k < NAME_VSCALE; k++, p += SCREEN_ROWBYTES)
            memcpy(p, banner_name[r], banner_name_bytes);
    gfx_text((SCREEN_W - gfx_text_width(by, 1)) / 2, BY_Y, by, 0, 1, 0);
    gfx_text((SCREEN_W - gfx_text_width(who, 1)) / 2, BY_Y + 13, who, 0, 1, 0);
    /* where it comes from, under a thin line at the foot of the screen */
    gfx_fill(0, FOOT_Y, SCREEN_W, 1, GFX_BLACK);
    gfx_text((SCREEN_W - gfx_text_width(where, 1)) / 2, FOOT_Y + 11, where, 0, 1, 0);
    shown = 0;
    up = 1;
    kputc_divert(swallow);
}

void splash_step(const struct icon *icon)
{
    if (!up)
        return;
    gfx_icon((SCREEN_W - ((ROW_COUNT - 1) * ROW_STEP + 24)) / 2 + shown * ROW_STEP, ROW_Y, icon, 0);
    shown++;
}

void splash_end(void)
{
    if (!up)
        return;
    up = 0;
    kputc_divert(NULL);
}
