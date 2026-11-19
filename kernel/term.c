/* term: the SE as a terminal for a shell running on a modern Mac.
 *
 * The other end is the helper program tools/juanterm. It runs the shell,
 * works out what the terminal screen should look like, and sends only the
 * parts that changed since the picture it knows we have. It waits for our
 * "drawn" reply before sending the next changes, so when output floods past
 * (a long listing, say) the SE skips straight to the newest screen instead
 * of drawing every line on the way.
 *
 * Connection start:
 *   helper -> SE   "JT1" and 16 random bytes
 *   SE -> helper   HMAC-SHA-256 of those bytes under the shared secret (32
 *                  bytes), then the screen size: columns, rows
 *   helper -> SE   1 = welcome, anything else = wrong secret
 * The secret itself never travels. Everything after that does travel
 * unencrypted, so this is for a home network only.
 *
 * Then messages in both directions: a type letter, a 16-bit length (high
 * byte first) and that many bytes.
 *   SE -> helper   'K' typed characters, in the SSH OS character set (the
 *                      helper turns accented letters into UTF-8)
 *                  'S' one special key (codes below)
 *                  'A' the last update has been drawn
 *   helper -> SE   'U' drawing operations    'X' the shell has ended
 *
 * Drawing operations, rows and columns counted from 0:
 *   01 row col n text...     text with the current attribute
 *   02 attr                  attribute for what follows: 1 inverse, 2 bold, 4 underline
 *   03 row col n ch          n copies of one character
 *   04 top bottom n          scroll rows top..bottom up by n; new rows are blank
 *   05 top bottom n          the same, downwards
 *   06 row col visible       cursor position
 *   07                       bell
 *   08                       clear the screen
 */
#include "kernel.h"
#include "net.h"
#include "shell.h"

#define MSG_MAX     8000        /* longest message the helper may send */
#define HELLO_LEN   19
#define NONCE_LEN   16

#define OP_TEXT         1
#define OP_ATTR         2
#define OP_REPEAT       3
#define OP_SCROLL_UP    4
#define OP_SCROLL_DOWN  5
#define OP_CURSOR       6
#define OP_BELL         7
#define OP_CLEAR        8

/* Special keys, as sent in an 'S' message. The helper turns them into
 * whatever the program on the other end currently expects. */
#define SK_UP 1
#define SK_DOWN 2
#define SK_RIGHT 3
#define SK_LEFT 4
#define SK_HOME 5
#define SK_END 6
#define SK_PAGE_UP 7
#define SK_PAGE_DOWN 8
#define SK_DELETE 9
#define SK_F1 11                /* F1..F12 are 11..22 */

static u8 msg[MSG_MAX];

static struct {
    int col, row;
    u8 visible, drawn;
} cursor;

static u8 log_text;             /* setting "termlog": copy drawn text to the serial port */

static void cursor_hide(void)
{
    if (cursor.drawn) {
        con_invert(cursor.col, cursor.row);
        cursor.drawn = 0;
    }
}

static void cursor_show(void)
{
    if (cursor.visible && !cursor.drawn) {
        con_invert(cursor.col, cursor.row);
        cursor.drawn = 1;
    }
}

static void log_run(const u8 *text, int n)
{
    while (n--)
        serial_putc((char)*text++);
    serial_putc('\n');
}

/* Carry out one update. A malformed one is abandoned where the trouble
 * starts; con_draw and friends ignore positions off the screen. */
static void apply(const u8 *p, u32 len)
{
    const u8 *end = p + len;
    u8 attr = 0;
    int n;

    cursor_hide();
    while (p < end) {
        switch (*p++) {
        case OP_TEXT:
            if (end - p < 3 || end - p - 3 < p[2])
                goto done;
            n = p[2];
            con_draw(p[1], p[0], p + 3, n, attr);
            if (log_text)
                log_run(p + 3, n);
            p += 3 + n;
            break;
        case OP_ATTR:
            if (end - p < 1)
                goto done;
            attr = *p++ & (CON_INVERSE | CON_BOLD | CON_UNDERLINE);
            break;
        case OP_REPEAT:
            if (end - p < 4)
                goto done;
            con_fill(p[1], p[0], p[2], p[3], attr);
            p += 4;
            break;
        case OP_SCROLL_UP:
        case OP_SCROLL_DOWN:
            if (end - p < 3)
                goto done;
            con_scroll(p[0], p[1], p[-1] == OP_SCROLL_UP ? p[2] : -(int)p[2]);
            p += 3;
            break;
        case OP_CURSOR:
            if (end - p < 3)
                goto done;
            cursor.row = p[0];
            cursor.col = p[1];
            cursor.visible = p[2] != 0;
            p += 3;
            break;
        case OP_BELL:
            break;              /* no sound yet */
        case OP_CLEAR:
            con_clear();
            break;
        default:
            goto done;
        }
    }
done:
    cursor_show();
}

static int send_msg(int c, char type, const u8 *payload, u16 len)
{
    u8 head[3];
    int err;

    head[0] = (u8)type;
    head[1] = (u8)(len >> 8);
    head[2] = (u8)len;
    err = tcp_write(c, head, 3, 5 * 60);
    if (err == 0 && len)
        err = tcp_write(c, payload, len, 5 * 60);
    return err;
}

/* Read exactly n bytes or fail. */
static int read_all(int c, u8 *buf, u16 n, u32 timeout_ticks)
{
    int got;

    while (n) {
        got = tcp_read(c, buf, n, timeout_ticks);
        if (got < 0)
            return got;
        buf += got;
        n = (u16)(n - got);
    }
    return 0;
}

static u8 special_key(u8 raw)
{
    static const u8 fkeys[12] = {
        0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F,
    };
    u8 i;

    switch (raw) {
    case 0x3E: return SK_UP;
    case 0x3D: return SK_DOWN;
    case 0x3C: return SK_RIGHT;
    case 0x3B: return SK_LEFT;
    case 0x73: return SK_HOME;
    case 0x77: return SK_END;
    case 0x74: return SK_PAGE_UP;
    case 0x79: return SK_PAGE_DOWN;
    case 0x75: return SK_DELETE;
    }
    for (i = 0; i < 12; i++)
        if (raw == fkeys[i])
            return (u8)(SK_F1 + i);
    return 0;
}

void cmd_term(int argc, char **argv)
{
    char host[64];
    const char *secret, *setting;
    u8 hello[HELLO_LEN], answer[34], head[3], keys[64], sk;
    u32 ip, body_len = 0, got = 0;
    u16 port;
    int c, n, err, ev, have = 0, nkeys, quit = 0, busy;
    const char *why = "closed";
    u8 ch;

    (void)argc;
    (void)argv;
    if (config_host_port("helper", host, sizeof host, &port, 2323) != 0) {
        kputs("Set the helper's address first, for example: set helper 192.168.1.10:2323\n");
        return;
    }
    secret = config_get("secret");
    if (!secret || !secret[0]) {
        kputs("Set the shared secret first: set secret WORD\n");
        return;
    }
    setting = config_get("termlog");
    log_text = setting && setting[0] == '1';
    setting = config_get("option");
    kbd_option_meta = setting && shell_streq(setting, "meta");

    err = net_resolve(host, &ip);
    if (err) {
        shell_fail(err);
        return;
    }
    kprintf("Connecting to the helper at %s port %d...\n", host, port);
    c = tcp_connect(ip, port, 10 * 60);
    if (c < 0) {
        shell_fail(c);
        return;
    }
    err = read_all(c, hello, HELLO_LEN, 10 * 60);
    if (err == 0 && (hello[0] != 'J' || hello[1] != 'T' || hello[2] != '1')) {
        kputs("That is not an SSH OS terminal helper.\n");
        tcp_abort(c);
        return;
    }
    if (err == 0) {
        hmac_sha256(secret, strlen(secret), hello + 3, NONCE_LEN, answer);
        answer[32] = CON_COLS;
        answer[33] = CON_ROWS;
        err = tcp_write(c, answer, sizeof answer, 10 * 60);
    }
    if (err == 0)
        err = read_all(c, head, 1, 10 * 60);
    if (err) {
        shell_fail(err);
        tcp_abort(c);
        return;
    }
    if (head[0] != 1) {
        kputs("The helper refused: the secret here and the helper's do not match.\n");
        tcp_close(c);
        return;
    }

    con_clear();
    cursor.drawn = cursor.visible = 0;
    while (!quit) {
        busy = 0;
        net_poll();

        /* Incoming messages: first the 3-byte head, then the body. */
        for (;;) {
            if (have < 3) {
                n = tcp_recv(c, head + have, (u16)(3 - have));
                if (n < 0) {
                    why = net_strerror(n);
                    quit = 1;
                    break;
                }
                have += n;
                if (have < 3)
                    break;
                body_len = ((u32)head[1] << 8) | head[2];
                got = 0;
                if (body_len > MSG_MAX) {
                    why = "the helper sent something this version does not understand";
                    quit = 1;
                    break;
                }
            }
            if (got < body_len) {
                n = tcp_recv(c, msg + got, (u16)(body_len - got));
                if (n < 0) {
                    why = net_strerror(n);
                    quit = 1;
                    break;
                }
                got += (u32)n;
                if (got < body_len)
                    break;
            }
            have = 0;
            busy = 1;
            if (head[0] == 'U') {
                apply(msg, body_len);
                if (send_msg(c, 'A', NULL, 0) != 0) {
                    why = "connection lost";
                    quit = 1;
                    break;
                }
            } else if (head[0] == 'X') {
                why = "the shell on the other side has ended";
                quit = 1;
                break;
            }
        }

        /* Keys. Command-Q or Control-] leaves the terminal. */
        nkeys = 0;
        while (!quit && (ev = kbd_get_event()) >= 0) {
            if (ev & 0x8000)
                continue;                   /* key released */
            ch = (u8)(ev & 0xFF);
            if (((kbd_modifiers() & KBD_COMMAND) && (ch == 'q' || ch == 'Q')) || ch == 0x1D) {
                why = "closed from the keyboard";
                quit = 1;
                break;
            }
            sk = special_key((u8)((ev >> 8) & 0x7F));
            if (sk) {
                if (nkeys && send_msg(c, 'K', keys, (u16)nkeys) != 0)
                    quit = 1;
                nkeys = 0;
                if (send_msg(c, 'S', &sk, 1) != 0)
                    quit = 1;
                continue;
            }
            if (!ch)
                continue;
            if (ch == '\n')
                ch = '\r';                  /* what terminals send for Return */
            else if (ch == '\b')
                ch = 0x7F;                  /* and for Delete */
            if (nkeys > (int)sizeof keys - 2) {
                if (send_msg(c, 'K', keys, (u16)nkeys) != 0)
                    quit = 1;
                nkeys = 0;
            }
            if (ev & KBD_EV_META)
                keys[nkeys++] = 0x1B;       /* Option without an accent: the Meta key */
            keys[nkeys++] = ch;
        }
        if (nkeys) {
            busy = 1;
            if (send_msg(c, 'K', keys, (u16)nkeys) != 0)
                quit = 1;
        }
        bar_tick();
        if (!busy && !quit)
            bar_idle();
        else
            bar_work();
    }

    kbd_option_meta = 0;
    tcp_close(c);
    cursor_hide();
    con_goto(0, CON_ROWS - 1);
    kprintf("\n[Terminal ended: %s.]\n", why);
}
