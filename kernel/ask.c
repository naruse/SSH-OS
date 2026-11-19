/* The questions SSH asks, each in a small window over whatever is on the
 * screen: the password, and whether to trust a server seen for the first
 * time. Return and Esc, or a click on a button, answer them.
 */
#include "kernel.h"
#include "hw.h"
#include "net.h"

#define BTN_W   84
#define BTN_H   22

static int box_x, box_y, box_w, box_h;
static u8 one_button;         /* no Cancel */

static void note(const char *s)
{
    serial_puts("SSH: ");
    serial_puts(s);
    serial_puts("\n");
}

void ask_button(int x, int y, const char *s, int main_one)
{
    gfx_round_box(x, y, BTN_W, BTN_H, GFX_WHITE, main_one ? 2 : 1);
    gfx_text(x + BTN_W / 2 - gfx_text_width(s, 1) / 2, y + 6, s, 0, 1, main_one);
}

/* Where the two buttons are, at the bottom right of the window. */
static int ok_x(void)
{
    return box_x + box_w - 16 - BTN_W;
}

static int no_x(void)
{
    return ok_x() - 12 - BTN_W;
}

static int btn_y(void)
{
    return box_y + box_h - 14 - BTN_H;
}

/* Draw the window with its title and buttons, keeping what was there. */
static void open(int w, int h, const char *title, const char *ok)
{
    box_w = w;
    box_h = h;
    box_x = (SCREEN_W - w) / 2;
    box_y = BAR_H + (SCREEN_H - BAR_H - h) / 2 - 16;
    gfx_begin();
    gfx_save(box_x, box_y, w + 3, h + 3);
    gfx_fill(box_x + 3, box_y + 3, w, h, GFX_BLACK);
    gfx_fill(box_x, box_y, w, h, GFX_BLACK);
    gfx_fill(box_x + 1, box_y + 1, w - 2, h - 2, GFX_WHITE);
    gfx_fill(box_x + 1, box_y + 1, w - 2, 16, GFX_BLACK);
    gfx_text(box_x + w / 2 - gfx_text_width(title, 1) / 2, box_y + 4, title, 1, 1, 1);
    if (!one_button)
        ask_button(no_x(), btn_y(), "Cancel", 0);
    ask_button(ok_x(), btn_y(), ok, 1);
    gfx_end();
}

static void close(void)
{
    gfx_begin();
    gfx_restore();
    gfx_end();
    while (kbd_get_event() >= 0)
        ;
    kbd_escape = 0;
}

/* One turn of waiting: 1 = the main button or Return, -1 = Cancel or Esc,
 * 2 = another key (in *key), 0 = nothing yet. */
static u8 editing;              /* the window has a text field: arrows and other clicks count */
static int turn_x, turn_y;      /* where such a click was */

static int turn(u32 *clicks, u8 *key)
{
    int ev, x, y;
    u8 c;

    net_poll();
    bar_tick();
    if (mouse.clicks != *clicks) {
        *clicks = mouse.clicks;
        x = mouse.click_x;
        y = mouse.click_y;
        if (y >= btn_y() && y < btn_y() + BTN_H) {
            if (x >= ok_x() && x < ok_x() + BTN_W)
                return 1;
            if (!one_button && x >= no_x() && x < no_x() + BTN_W)
                return -1;
        }
        if (editing) {          /* 4 = a click somewhere else: in the field, perhaps */
            turn_x = x;
            turn_y = y;
            return 4;
        }
    }
    ev = kbd_get_event();
    if (ev < 0) {
        bar_idle();
        return 0;
    }
    bar_work();
    c = (u8)(ev & 0xFF);
    if (editing && !(ev & 0x8000) && !c && (((ev >> 8) & 0x7F) == 0x3B || ((ev >> 8) & 0x7F) == 0x3C)) {
        *key = (u8)((ev >> 8) & 0x7F);  /* 3 = the Left or Right arrow, by its key number */
        return 3;
    }
    if ((ev & 0x8000) || !c)
        return 0;
    if (c == 0x1B)
        return -1;
    if (c == '\n')
        return 1;
    *key = c;
    return 2;
}

/* Text that may be too long for its place: the end is left out. */
static void fitted(int x, int y, const char *s, int chars, int bold)
{
    char shown[64];
    u32 n = strlen(s), most = (u32)chars < sizeof shown - 1 ? (u32)chars : sizeof shown - 1;

    if (n > most)
        n = most;
    memcpy(shown, s, n);
    shown[n] = 0;
    gfx_text(x, y, shown, 0, 1, bold);
}

/* ---------------------------------------------------------------- password -- */

#define PW_W        300
#define PW_H        124
#define PW_DOTS     33          /* how many fit in the field */

static int pw_first;            /* the first dot that shows, when there are more than fit */

/* letters dots, with the caret before dot number at. */
static void draw_dots(int letters, int at, int caret)
{
    int x = box_x + 16, y = box_y + 50, i;

    if (at < pw_first)
        pw_first = at;
    if (at > pw_first + PW_DOTS)
        pw_first = at - PW_DOTS;
    if (letters <= PW_DOTS)
        pw_first = 0;
    gfx_begin();
    gfx_fill(x + 2, y + 2, PW_W - 36, 18, GFX_WHITE);
    for (i = pw_first; i < letters && i < pw_first + PW_DOTS; i++) {
        gfx_fill(x + 9 + (i - pw_first) * 8, y + 8, 3, 5, GFX_BLACK);
        gfx_fill(x + 8 + (i - pw_first) * 8, y + 9, 5, 3, GFX_BLACK);
    }
    if (caret)
        gfx_fill(x + 7 + (at - pw_first) * 8, y + 4, 1, 14, GFX_BLACK);
    gfx_end();
}

/* How many letters (not bytes) are in the first n bytes of line. */
static int letters_in(const char *line, int n)
{
    int i, count = 0;

    for (i = 0; i < n; i++)
        if (((u8)line[i] & 0xC0) != 0x80)
            count++;
    return count;
}

/* The password of user at host, into line (max bytes), nothing of it shown
 * but a dot a letter. The caret moves by the arrows or a click, and typing
 * and Delete happen where it is. Returns the length, or -1 for Cancel. */
int ask_password(const char *user, const char *host, char *line, int max)
{
    u32 clicks = mouse.clicks, blink = ticks;
    int len = 0, at = 0, caret = 1, r, want;
    char who[100];
    u8 key = 0;

    open(PW_W, PW_H, "Password", "OK");
    who[0] = 0;
    if (strlen(user) + strlen(host) + 2 <= sizeof who) {
        memcpy(who, user, strlen(user));
        who[strlen(user)] = '@';
        memcpy(who + strlen(user) + 1, host, strlen(host) + 1);
    }
    gfx_begin();
    fitted(box_x + 16, box_y + 30, who, 44, 0);
    gfx_fill(box_x + 16, box_y + 50, PW_W - 32, 22, GFX_BLACK);
    gfx_end();
    line[0] = 0;
    pw_first = 0;
    draw_dots(0, 0, 1);
    note("password");
    editing = 1;
    for (;;) {
        r = turn(&clicks, &key);
        if (r == 0 && ticks - blink >= 30) {
            blink = ticks;
            caret = !caret;
            draw_dots(letters_in(line, len), letters_in(line, at), caret);
        }
        if (r == -1) {
            memset(line, 0, (size_t)max);
            editing = 0;
            close();
            return -1;
        }
        if (r == 1) {
            line[len] = 0;
            editing = 0;
            close();
            return len;
        }
        if (r == 4) {           /* a click in the field puts the caret between the dots there */
            if (turn_y < box_y + 50 || turn_y >= box_y + 72 || turn_x < box_x + 16 ||
                turn_x >= box_x + PW_W - 16)
                continue;
            want = pw_first + (turn_x - (box_x + 24) + 4) / 8;
            if (want < 0)
                want = 0;
            for (at = 0; at < len && want > 0; want--)
                for (at++; at < len && ((u8)line[at] & 0xC0) == 0x80; at++)
                    ;
        } else if (r == 3) {
            if (!kbd_line_edit(line, &len, &at, max, 0, key))
                continue;
        } else if (r != 2 || !kbd_line_edit(line, &len, &at, max, key, 0)) {
            continue;
        }
        caret = 1;
        blink = ticks;
        draw_dots(letters_in(line, len), letters_in(line, at), caret);
    }
}

/* ------------------------------------------------------------ a new server -- */

/* A server never seen before: show its key's fingerprint. 1 = trust it. */
int ask_trust(const char *host, const char *print)
{
    u32 clicks = mouse.clicks;
    int r, x, y;
    u8 key;

    open(372, 156, "New server", "Trust");
    x = box_x + 16;
    y = box_y;
    gfx_begin();
    gfx_text(x, y + 30, "First connection to", 0, 1, 0);
    fitted(x + gfx_text_width("First connection to ", 1), y + 30, host, 36, 1);
    gfx_text(x, y + 52, "Its key has this fingerprint:", 0, 1, 0);
    fitted(x, y + 68, print, 56, 0);
    gfx_text(x, y + 90, "Trust it only if the server shows the same.", 0, 1, 0);
    gfx_end();
    note("new server");
    while ((r = turn(&clicks, &key)) == 0 || r == 2)
        ;
    close();
    return r == 1;
}

/* ------------------------------------------------- no password next time? -- */

/* After a log-in by password: shall this Macintosh's own key be added to
 * the account, so that the next log-in needs none? make: the key has yet
 * to be made, which takes a while. 1 = yes. */
int ask_keyless(const char *user, const char *host, int make)
{
    u32 clicks = mouse.clicks;
    int r, x, y;
    u8 key;

    open(372, 156, "No password next time?", "Yes");
    x = box_x + 16;
    y = box_y;
    gfx_begin();
    gfx_text(x, y + 30, "Log in to", 0, 1, 0);
    fitted(x + gfx_text_width("Log in to ", 1), y + 30, host, 28, 1);
    gfx_text(x, y + 44, "as", 0, 1, 0);
    fitted(x + gfx_text_width("as ", 1), y + 44, user, 28, 1);
    gfx_text(x, y + 58, "without a password from now on?", 0, 1, 0);
    gfx_text(x, y + 80, make ? "This Macintosh makes a key of its own (about 15" :
             "This Macintosh's own key is added to your account", 0, 1, 0);
    gfx_text(x, y + 92, make ? "seconds, once) and adds it to your account there." :
             "there. The password itself is not kept anywhere.", 0, 1, 0);
    gfx_end();
    note("no password next time?");
    while ((r = turn(&clicks, &key)) == 0 || r == 2)
        ;
    close();
    return r == 1;
}

/* ---------------------------------------------------------------- a message -- */

#define MSG_CHARS   52          /* to a row */
#define MSG_ROWS    9

/* Say something, with an OK button; any key or a click on it closes. Long
 * lines are folded between words, and the square brackets the text screen
 * puts round a remark are left out. */
void ask_message(const char *title, const char *const *lines, int n)
{
    static char rows[MSG_ROWS][MSG_CHARS + 1];
    u32 clicks = mouse.clicks, len, take, i;
    int nrows = 0, k, r;
    const char *s;
    u8 key;

    for (k = 0; k < n && nrows < MSG_ROWS; k++) {
        s = lines[k];
        len = strlen(s);
        if (len >= 2 && s[0] == '[' && s[len - 1] == ']') {
            s++;
            len -= 2;
        }
        while (len && nrows < MSG_ROWS) {
            take = len;
            if (take > MSG_CHARS) {
                for (take = MSG_CHARS; take > MSG_CHARS / 2 && s[take] != ' '; take--)
                    ;
                if (s[take] != ' ')
                    take = MSG_CHARS;
            }
            memcpy(rows[nrows], s, take);
            rows[nrows++][take] = 0;
            s += take;
            len -= take;
            while (len && *s == ' ') {
                s++;
                len--;
            }
        }
    }
    while (kbd_get_event() >= 0)    /* keys typed before this showed are not an answer */
        ;
    clicks = mouse.clicks;
    one_button = 1;
    open(MSG_CHARS * 6 + 40, 30 + nrows * 14 + 46, title, "OK");
    one_button = 0;
    gfx_begin();
    for (i = 0; i < (u32)nrows; i++)
        gfx_text(box_x + 20, box_y + 30 + (int)i * 14, rows[i], 0, 1, 0);
    gfx_end();
    note("message");
    one_button = 1;
    while ((r = turn(&clicks, &key)) == 0)
        ;
    one_button = 0;
    close();
}
