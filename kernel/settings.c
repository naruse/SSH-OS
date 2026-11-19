/* Settings: a window on the home screen for the things one sets by feel.
 *
 *   Mouse speed   how far the pointer goes for the same move of the mouse
 *   Volume        the same as the bar's
 *
 * Up/Down choose a line, Left/Right (or - and +) change it, Return or Esc
 * closes; or click the - and + boxes and Done. What is chosen is kept on the
 * SSH OS disk.
 */
#include "kernel.h"
#include "hw.h"
#include "net.h"

#define LABEL_X     116
#define LESS_X      228
#define METER_X     252
#define SEG_W       14          /* a segment and the gap after it */
#define MORE_X      (METER_X + 7 * SEG_W + 6)
#define BOX         16
#define ROW0_Y      (BAR_H + 108)
#define ROW_STEP    30
#define DONE_W      84
#define DONE_H      22
#define DONE_X      (MORE_X + BOX - DONE_W)
#define DONE_Y      (BAR_H + 182)

static const char *const labels[2] = {"Mouse speed", "Volume"};

static int value(int row)
{
    return row == 0 ? mouse_speed() : snd_volume();
}

static void small_button(int x, int y, int plus)
{
    gfx_fill(x, y, BOX, BOX, GFX_BLACK);
    gfx_fill(x + 1, y + 1, BOX - 2, BOX - 2, GFX_WHITE);
    gfx_fill(x + 4, y + 7, 8, 2, GFX_BLACK);
    if (plus)
        gfx_fill(x + 7, y + 4, 2, 8, GFX_BLACK);
}

static void draw_row(int row, int chosen)
{
    int y = ROW0_Y + row * ROW_STEP, i, v = value(row);

    gfx_begin();
    gfx_fill(LABEL_X - 6, y - 2, LESS_X - LABEL_X - 4, BOX + 4, chosen ? GFX_BLACK : GFX_WHITE);
    gfx_text(LABEL_X, y + 3, labels[row], chosen, 1, 0);
    small_button(LESS_X, y, 0);
    small_button(MORE_X, y, 1);
    for (i = 0; i < 7; i++) {
        gfx_fill(METER_X + i * SEG_W, y + 2, SEG_W - 3, BOX - 4, GFX_BLACK);
        if (i >= v)
            gfx_fill(METER_X + i * SEG_W + 1, y + 3, SEG_W - 5, BOX - 6, GFX_WHITE);
    }
    gfx_end();
}

static void change(int row, int by)
{
    char note[] = "DESK: mouse speed 0\n";

    if (row == 0) {
        mouse_set_speed(mouse_speed() + by);
        note[18] = (char)('0' + mouse_speed());
        serial_puts(note);
    } else {
        bar_volume_change(by);
    }
}

/* Returns BAR_NONE, or BAR_HOME or BAR_CONSOLE when the menu on the bar was
 * used to leave. */
int settings_run(void)
{
    static const char title[] = "Settings";
    u32 clicks = mouse.clicks;
    int row = 0, ev, r, x, y, hit, changed = 0;
    u8 ch, raw;

    con_clear();
    gfx_begin();
    gfx_icon(SCREEN_W / 2 - icon_settings.w / 2, BAR_H + 22, &icon_settings, 0);
    gfx_text(SCREEN_W / 2 - gfx_text_width(title, 1) / 2, BAR_H + 74, title, 0, 1, 1);
    ask_button(DONE_X, DONE_Y, "Done", 1);
    gfx_end();
    draw_row(0, 1);
    draw_row(1, 0);
    serial_puts("DESK: settings\n");
    for (;;) {
        net_poll();
        bar_tick();
        r = bar_poll();
        if (r == BAR_HOME || r == BAR_CONSOLE)
            break;
        if (r != BAR_NONE) {
            clicks = mouse.clicks;
            draw_row(1, row == 1);              /* the bar's own volume buttons */
        }
        r = BAR_NONE;
        if (mouse.clicks != clicks) {
            clicks = mouse.clicks;
            x = mouse.click_x;
            y = mouse.click_y;
            if (x >= DONE_X && x < DONE_X + DONE_W && y >= DONE_Y && y < DONE_Y + DONE_H)
                break;
            hit = (y - ROW0_Y + 4) / ROW_STEP;
            if (y >= ROW0_Y - 4 && hit < 2 && y < ROW0_Y + hit * ROW_STEP + BOX + 4) {
                int old = row;

                row = hit;
                if (x >= LESS_X - 4 && x < LESS_X + BOX + 4) {
                    change(row, -1);
                    changed |= 1 << row;
                } else if (x >= MORE_X - 4 && x < MORE_X + BOX + 4) {
                    change(row, 1);
                    changed |= 1 << row;
                }
                if (old != row)
                    draw_row(old, 0);
                draw_row(row, 1);
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
        if (raw == 0x3D || raw == 0x3E || ch == '\t') {         /* Down, Up */
            draw_row(row, 0);
            row = !row;
            draw_row(row, 1);
        } else if (raw == 0x3B || ch == '-') {                  /* Left */
            change(row, -1);
            changed |= 1 << row;
            draw_row(row, 1);
        } else if (raw == 0x3C || ch == '+' || ch == '=') {     /* Right */
            change(row, 1);
            changed |= 1 << row;
            draw_row(row, 1);
        } else if (ch == '\n' || ch == 0x1B) {
            break;
        }
    }
    kbd_escape = 0;
    if (changed & 1)
        mouse_keep_speed();
    serial_puts("DESK: settings closed\n");
    return r;
}
