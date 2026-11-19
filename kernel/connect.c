/* The screen shown while SSH connects from the home screen: the icon, who
 * to, a bar that fills as the steps go by, and under it what is happening
 * now. Nothing in ssh.c knows about it: while it is up, what would be
 * printed on the text screen comes here instead (kputc_divert), a line at
 * a time. The modem port still gets every character.
 *
 * If the connection fails, or ends with something to say, that is shown in
 * a window (ask_message) before going back to the home screen.
 */
#include "kernel.h"
#include "hw.h"
#include "shell.h"

#define BAR_W       240
#define BAR_X       ((SCREEN_W - BAR_W) / 2)
#define BAR_Y       (BAR_H + 104)
#define LABEL_Y     (BAR_Y + 22)
#define MORE_Y      (LABEL_Y + 22)
#define MORE_MAX    5
#define LINE_LEN    86
#define STAGES      6

/* A line that begins like one of these starts that step. */
static const char *const stages[STAGES] = {
    "Making a one-time key", "Connecting to", "Working out", "Checking the server",
    "Signing in", "Logged in",
};

static char label[LINE_LEN], more[MORE_MAX][LINE_LEN], cur[LINE_LEN];
static int nmore, cur_len, stage, dots;
static u8 quiet;                /* collect the lines but draw nothing */
static u8 logged_in;

static void centered(int y, const char *s, int bold)
{
    gfx_text((SCREEN_W - gfx_text_width(s, 1)) / 2, y, s, 0, 1, bold);
}

static void draw_bar(void)
{
    int fill = stage * (BAR_W - 4) / STAGES + dots * 3;
    int most = (stage + 1) * (BAR_W - 4) / STAGES - 4;

    if (fill > most)
        fill = most;
    gfx_begin();
    gfx_fill(BAR_X + 2, BAR_Y + 2, fill, 6, GFX_BLACK);
    gfx_end();
}

static void draw_text(void)
{
    int i;

    gfx_begin();
    gfx_fill(0, LABEL_Y, SCREEN_W, MORE_Y + MORE_MAX * 14 - LABEL_Y, GFX_WHITE);
    centered(LABEL_Y, label, 0);
    for (i = 0; i < nmore; i++)
        centered(MORE_Y + i * 14, more[i], 0);
    gfx_end();
}

static int begins(const char *s, const char *with)
{
    u32 n = strlen(with);

    return strlen(s) >= n && memcmp(s, with, n) == 0;
}

/* A whole line has arrived. */
static void line_done(void)
{
    int i, len = cur_len;

    cur[len] = 0;
    cur_len = 0;
    /* dots that only showed something was being worked out */
    while (len > 0 && cur[len - 1] == '.')
        cur[--len] = 0;
    if (len == 0)
        return;
    for (i = 0; i < STAGES; i++)
        if (begins(cur, stages[i]))
            break;
    if (i < STAGES) {
        /* the remark in brackets after the first step's name is for the text screen */
        if (i == 0)
            cur[strlen(stages[0])] = 0;
        memcpy(label, cur, strlen(cur) + 1);
        nmore = 0;
        stage = i;
    } else {
        if (nmore == MORE_MAX) {
            memmove(more[0], more[1], (MORE_MAX - 1) * LINE_LEN);
            nmore--;
        }
        memcpy(more[nmore++], cur, (u32)len + 1);
    }
    if (!quiet) {
        draw_bar();
        draw_text();
    }
}

static void take(char c)
{
    int i;

    if (c == '\n') {
        line_done();
        return;
    }
    if (c == '\b') {
        if (cur_len)
            cur_len--;
        return;
    }
    if ((u8)c < ' ' || cur_len >= LINE_LEN - 1)
        return;
    cur[cur_len++] = c;
    cur[cur_len] = 0;
    if (quiet)
        return;
    if (c == '.') {
        dots++;
        draw_bar();
        return;
    }
    /* A step's name shows as soon as it is there, before the work starts. */
    for (i = 0; i < STAGES; i++)
        if (strcmp(cur, stages[i]) == 0) {
            memcpy(label, cur, (u32)cur_len + 1);
            nmore = 0;
            stage = i;
            dots = 0;
            draw_bar();
            draw_text();
        }
}

/* Logged in: the text screen is the session's from here. */
static void at_login(void)
{
    logged_in = 1;
    kputc_divert(NULL);
    con_clear();
}

/* The session is over: what ssh says about it is kept, not drawn. */
static void at_end(void)
{
    nmore = 0;
    cur_len = 0;
    quiet = 1;
    kputc_divert(take);
}

/* Connect to target ("user@host[:port]") with the screen above, run the
 * session, and say how it ended if there is anything to say. Returns 1 if
 * it never logged in, so the caller can let the user try again. */
int connect_run(char *target)
{
    static const char *lines[MORE_MAX + 1];
    char title[LINE_LEN];
    char *argv[2];
    u32 n = strlen(target);
    int i, count = 0;

    argv[0] = "ssh";
    argv[1] = target;
    memcpy(title, "Connecting to ", 14);
    if (n > LINE_LEN - 15)
        n = LINE_LEN - 15;
    memcpy(title + 14, target, n);
    title[14 + n] = 0;

    label[0] = 0;
    nmore = cur_len = dots = 0;
    stage = 0;
    quiet = logged_in = 0;
    con_clear();
    gfx_begin();
    gfx_icon(SCREEN_W / 2 - icon_ssh.w / 2, BAR_H + 24, &icon_ssh, 0);
    centered(BAR_H + 74, title, 1);
    gfx_fill(BAR_X, BAR_Y, BAR_W, 10, GFX_BLACK);
    gfx_fill(BAR_X + 1, BAR_Y + 1, BAR_W - 2, 8, GFX_WHITE);
    gfx_end();

    serial_puts("DESK: connecting\n");
    ssh_login_hook = at_login;
    ssh_end_hook = at_end;
    kputc_divert(take);
    cmd_ssh(2, argv);
    if (cur_len)
        line_done();
    kputc_divert(NULL);
    ssh_login_hook = NULL;
    ssh_end_hook = NULL;

    if (logged_in && ssh_ended_well)
        return 0;               /* nothing to say */
    if (!nmore && label[0] && !logged_in)
        lines[count++] = label;
    for (i = 0; i < nmore; i++) {
        /* the window's title says it failed already */
        if (begins(more[i], "Failed: ")) {
            memmove(more[i], more[i] + 8, strlen(more[i] + 8) + 1);
            if (more[i][0] >= 'a' && more[i][0] <= 'z')
                more[i][0] -= 'a' - 'A';
        }
        lines[count++] = more[i];
    }
    if (!logged_in) {           /* the window says it now */
        gfx_begin();
        gfx_fill(0, LABEL_Y, SCREEN_W, MORE_Y + MORE_MAX * 14 - LABEL_Y, GFX_WHITE);
        gfx_end();
    }
    ask_message(logged_in ? "Connection ended" : "Could not connect", lines, count);
    return !logged_in;
}
