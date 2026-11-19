/* Keyboard and mouse input.
 *
 * The ADB driver calls kbd_key() and mouse_packet() from the interrupt
 * handler. Key events go into a small ring buffer that the rest of the
 * kernel reads with kbd_get_event().
 *
 * Key codes are the ADB codes of Apple keyboards; the layout is US.
 *
 * Option works as on a Mac: Option-e and then a vowel gives it an acute
 * accent, Option-n then n gives an n with a tilde, Option-c a c with a
 * cedilla, and so on, for the characters the font has (the tables are
 * below). An Option combination without such a meaning is marked
 * KBD_EV_META, which "term" sends on as the Meta key.
 */
#include "kernel.h"

#define KEY_SHIFT       0x38
#define KEY_RSHIFT      0x7B
#define KEY_CAPS        0x39
#define KEY_CONTROL     0x36
#define KEY_RCONTROL    0x7D
#define KEY_ESCAPE      0x35
#define KEY_COMMAND     0x37
#define KEY_OPTION      0x3A
#define KEY_ROPTION     0x7C    /* extended keyboards; older ones send 0x3A for both */

static const char plain[128] = {
    [0x00] = 'a', [0x01] = 's', [0x02] = 'd', [0x03] = 'f', [0x04] = 'h',
    [0x05] = 'g', [0x06] = 'z', [0x07] = 'x', [0x08] = 'c', [0x09] = 'v',
    [0x0B] = 'b', [0x0C] = 'q', [0x0D] = 'w', [0x0E] = 'e', [0x0F] = 'r',
    [0x10] = 'y', [0x11] = 't', [0x12] = '1', [0x13] = '2', [0x14] = '3',
    [0x15] = '4', [0x16] = '6', [0x17] = '5', [0x18] = '=', [0x19] = '9',
    [0x1A] = '7', [0x1B] = '-', [0x1C] = '8', [0x1D] = '0', [0x1E] = ']',
    [0x1F] = 'o', [0x20] = 'u', [0x21] = '[', [0x22] = 'i', [0x23] = 'p',
    [0x24] = '\n', [0x25] = 'l', [0x26] = 'j', [0x27] = '\'', [0x28] = 'k',
    [0x29] = ';', [0x2A] = '\\', [0x2B] = ',', [0x2C] = '/', [0x2D] = 'n',
    [0x2E] = 'm', [0x2F] = '.', [0x30] = '\t', [0x31] = ' ', [0x32] = '`',
    [0x33] = '\b', [0x35] = 0x1B,
    /* numeric keypad */
    [0x41] = '.', [0x43] = '*', [0x45] = '+', [0x4B] = '/', [0x4C] = '\n',
    [0x4E] = '-', [0x51] = '=', [0x52] = '0', [0x53] = '1', [0x54] = '2',
    [0x55] = '3', [0x56] = '4', [0x57] = '5', [0x58] = '6', [0x59] = '7',
    [0x5B] = '8', [0x5C] = '9',
};

/* Each pair is a main-block character and what Shift turns it into. */
static const char shift_pairs[] = "1!2@3#4$5%6^7&8*9(0)-_=+[{]}\\|;:'\",<.>/?`~";

/* Option and one key. The values are SSH OS character codes: Latin-1, and
 * below 0xA0 the extra symbols listed in tools/juanterm/charset.py. 0 means
 * the combination has no character here. */
static const struct {
    u8 code, plain, shifted;
} option_keys[] = {
    {0x12, 0xA1, 0},            /* 1  inverted exclamation mark */
    {0x14, 0xA3, 0},            /* 3  pound */
    {0x15, 0xA2, 0},            /* 4  cent */
    {0x16, 0xA7, 0},            /* 6  section */
    {0x1A, 0xB6, 0},            /* 7  paragraph */
    {0x1C, 0x9B, 0xB0},         /* 8  bullet, degree */
    {0x19, 0xAA, 0xB7},         /* 9  feminine ordinal, middle dot */
    {0x1D, 0xBA, 0},            /* 0  masculine ordinal */
    {0x1B, 0, 0x9E},            /* -  (shifted) long dash */
    {0x18, 0, 0xB1},            /* =  (shifted) plus-minus */
    {0x0F, 0xAE, 0},            /* r  registered */
    {0x10, 0xA5, 0},            /* y  yen */
    {0x1F, 0xF8, 0xD8},         /* o  o with stroke */
    {0x2A, 0xAB, 0xBB},         /* \  angle quotes */
    {0x00, 0xE5, 0xC5},         /* a  a with ring */
    {0x01, 0xDF, 0},            /* s  sharp s */
    {0x05, 0xA9, 0},            /* g  copyright */
    {0x25, 0xAC, 0},            /* l  not sign */
    {0x29, 0x9C, 0},            /* ;  ellipsis */
    {0x27, 0xE6, 0xC6},         /* '  ae */
    {0x08, 0xE7, 0xC7},         /* c  c with cedilla */
    {0x2E, 0xB5, 0},            /* m  micro */
    {0x2B, 0, 0xAF},            /* ,  (shifted) macron */
    {0x2C, 0xF7, 0xBF},         /* /  division, inverted question mark */
};

/* Option and one of these keys types nothing yet: the accent goes onto the
 * letter typed next. Before a space, or with Shift, it is typed by itself. */
static const struct {
    u8 code, alone;
    const char *letters, *accented;
} dead_keys[] = {
    {0x0E, 0xB4, "aeiouyAEIOUY", "\xE1\xE9\xED\xF3\xFA\xFD\xC1\xC9\xCD\xD3\xDA\xDD"},  /* e: acute */
    {0x32, '`',  "aeiouAEIOU",   "\xE0\xE8\xEC\xF2\xF9\xC0\xC8\xCC\xD2\xD9"},          /* `: grave */
    {0x22, '^',  "aeiouAEIOU",   "\xE2\xEA\xEE\xF4\xFB\xC2\xCA\xCE\xD4\xDB"},          /* i: circumflex */
    {0x20, 0xA8, "aeiouyAEIOU",  "\xE4\xEB\xEF\xF6\xFC\xFF\xC4\xCB\xCF\xD6\xDC"},      /* u: diaeresis */
    {0x2D, '~',  "anoANO",       "\xE3\xF1\xF5\xC3\xD1\xD5"},                          /* n: tilde */
};
#define DEAD_KEYS ((int)(sizeof dead_keys / sizeof dead_keys[0]))

#define QUEUE_SIZE 64           /* must be a power of two */
static volatile u32 queue[QUEUE_SIZE];
static volatile u8 q_head, q_tail;

static u8 shift_down, control_down, caps_on, option_down, command_down;
static u8 pending;              /* 1 + index of the dead key waiting for its letter */

/* Set by "term" when the setting "option" is "meta": Option is then always
 * the Meta key and types no accents. */
u8 kbd_option_meta;

struct mouse_state mouse;

/* Set when Esc goes down, so a busy command can notice it without taking
 * other typed keys out of the queue. Whoever acts on it clears it. */
volatile u8 kbd_escape;

static char shifted(char c)
{
    const char *p;

    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    for (p = shift_pairs; *p; p += 2)
        if (*p == c)
            return p[1];
    return c;
}

static void push(u8 raw, u8 c, u8 meta)
{
    u8 next = (u8)((q_head + 1) & (QUEUE_SIZE - 1));

    if (next == q_tail)
        return;                 /* full: drop the event */
    queue[q_head] = (meta ? KBD_EV_META : 0) | ((u32)raw << 8) | c;
    q_head = next;
}

static u8 compose(int dead, u8 c)
{
    const char *p;

    for (p = dead_keys[dead].letters; *p; p++)
        if ((u8)*p == c)
            return (u8)dead_keys[dead].accented[p - dead_keys[dead].letters];
    return 0;
}

/* One key transition from the keyboard: bit 7 set = released. */
void kbd_key(u8 raw)
{
    u8 code = raw & 0x7F, released = raw & 0x80;
    u8 c = 0, meta = 0, upper, special = 0;
    int i, dead = -1, waiting;

    if (raw == 0xFF)
        return;
    rng_stir(raw);
    switch (code) {
    case KEY_SHIFT:
    case KEY_RSHIFT:
        shift_down = !released;
        break;
    case KEY_CONTROL:
    case KEY_RCONTROL:
        control_down = !released;
        break;
    case KEY_CAPS:
        caps_on = !released;    /* the key latches mechanically */
        break;
    case KEY_OPTION:
    case KEY_ROPTION:
        option_down = !released;
        break;
    case KEY_COMMAND:
        command_down = !released;
        break;
    default:
        if (released)
            break;
        if (code == KEY_ESCAPE)
            kbd_escape = 1;
        c = (u8)plain[code];
        upper = shift_down || (caps_on && c >= 'a' && c <= 'z');

        if (option_down && !control_down && !command_down && !kbd_option_meta) {
            for (i = 0; i < (int)(sizeof option_keys / sizeof option_keys[0]); i++)
                if (option_keys[i].code == code)
                    special = upper ? option_keys[i].shifted : option_keys[i].plain;
            for (i = 0; i < DEAD_KEYS; i++)
                if (dead_keys[i].code == code) {
                    if (upper)
                        special = dead_keys[i].alone;
                    else
                        dead = i;
                }
        }
        if (special) {
            c = special;
        } else if (dead >= 0) {
            c = 0;
        } else {
            meta = option_down;
            if (c >= 'a' && c <= 'z') {
                if (control_down)
                    c = (u8)(c & 0x1F);
                else if (upper)
                    c = (u8)shifted((char)c);
            } else if (control_down && (c == '[' || c == '\\' || c == ']')) {
                c = (u8)(c & 0x1F);         /* Control-[ is Esc, and so on */
            } else if (shift_down && code < 0x40) {
                c = (u8)shifted((char)c);
            }
        }

        /* An accent waiting for its letter. It joins a letter it fits, is
         * typed by itself before a space or any other character, and is
         * forgotten at Return, Delete, an arrow or a Meta key. */
        if (pending) {
            waiting = pending - 1;
            pending = 0;
            if (dead >= 0 || special)
                push(raw, dead_keys[waiting].alone, 0);
            else if (c == ' ' && !meta)
                c = dead_keys[waiting].alone;
            else if (c >= 32 && c != 127 && !meta && !command_down) {
                if (compose(waiting, c))
                    c = compose(waiting, c);
                else
                    push(raw, dead_keys[waiting].alone, 0);
            }
        }
        if (dead >= 0)
            pending = (u8)(dead + 1);
    }

    push(raw, c, meta);
}

u8 kbd_modifiers(void)
{
    return (u8)((shift_down ? KBD_SHIFT : 0) | (control_down ? KBD_CONTROL : 0) |
                (option_down ? KBD_OPTION : 0) | (command_down ? KBD_COMMAND : 0));
}

int kbd_get_event(void)
{
    u32 ev;

    if (q_tail == q_head)
        return -1;
    ev = queue[q_tail];
    q_tail = (u8)((q_tail + 1) & (QUEUE_SIZE - 1));
    return (int)ev;
}

/* How fast the pointer moves, 1 (half speed) to 7 (six times); 2 is one
 * pixel for each step the mouse reports. */
static const u8 speed_halves[7] = {1, 2, 3, 4, 6, 8, 12};
static u8 speed = 2;
static s8 carry_x, carry_y;

int mouse_speed(void)
{
    return speed;
}

void mouse_set_speed(int v)
{
    speed = (u8)(v < 1 ? 1 : v > 7 ? 7 : v);
}

/* Take the speed from the setting "mouse". */
void mouse_from_settings(void)
{
    const char *v = config_get("mouse");

    if (v && v[0] >= '1' && v[0] <= '7' && !v[1])
        speed = (u8)(v[0] - '0');
}

/* Keep it on the SSH OS disk. */
void mouse_keep_speed(void)
{
    char v[2] = {(char)('0' + speed), 0};

    config_store("mouse", v);
}

/* Mouse register 0: byte 0 = button (bit 7, 0 = down) and Y movement,
 * byte 1 = X movement; movements are 7-bit signed. */
void mouse_packet(u8 b0, u8 b1)
{
    s16 dy = (s16)(b0 & 0x7F), dx = (s16)(b1 & 0x7F);

    rng_stir(((u32)b0 << 8) | b1);
    if (dy & 0x40)
        dy -= 0x80;
    if (dx & 0x40)
        dx -= 0x80;
    /* The speed setting: so many halves of a pixel for each step the mouse
     * reports, the odd half carried over to the next move. */
    dx = (s16)(dx * speed_halves[speed - 1] + carry_x);
    dy = (s16)(dy * speed_halves[speed - 1] + carry_y);
    carry_x = (s8)(dx % 2);
    carry_y = (s8)(dy % 2);
    dx /= 2;
    dy /= 2;
    mouse.x += dx;
    mouse.y += dy;
    if (mouse.x < 0)
        mouse.x = 0;
    if (mouse.x > 511)
        mouse.x = 511;
    if (mouse.y < 0)
        mouse.y = 0;
    if (mouse.y > 341)
        mouse.y = 341;
    if (!(b0 & 0x80) && !mouse.button) {
        mouse.clicks++;
        mouse.click_x = mouse.x;
        mouse.click_y = mouse.y;
    }
    mouse.button = !(b0 & 0x80);
    mouse.moves++;
}
