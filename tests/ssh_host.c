/* The SE's SSH client (kernel/ssh.c with the arithmetic it uses), built for
 * this Mac with real network sockets in place of the SSH OS network stack,
 * the screen on standard output and the keyboard on standard input. That
 * lets tests/test_ssh.py try it against a real OpenSSH server in seconds
 * and under the address and undefined-behaviour checkers. The Makefile
 * compiles it together with the kernel sources it needs and the generated
 * font file, for its character tables (test-ssh-host).
 *
 *     ssh_host SETTINGS-FILE user@host:port
 *
 * If SSHOS_DISK names a folder, that folder stands in for the SSH OS disk
 * (kernel/saved.c keeps HOSTS.TXT and RECENT.TXT there); otherwise there
 * is no disk.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "host_types.h"
#include "../kernel/kernel.h"
#include "../kernel/net.h"
#include "../kernel/shell.h"

/* ------------------------------------------------- kernel stand-ins -- */

const char config_text[1] = "";       /* settings come from the file named on the command line */
const u32 config_text_len = 0;

volatile u32 ticks;
volatile u8 kbd_escape;
int (*net_abort_check)(void);
static int sock = -1;
static int closed_by_peer;

static u32 now_ticks(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32)(ts.tv_sec * 60 + ts.tv_nsec / 16666667);
}

void timeout_start(struct timeout *t, u32 nticks)
{
    t->start = now_ticks();
    t->limit = nticks;
}

int timeout_expired(struct timeout *t)
{
    return now_ticks() - t->start >= t->limit;
}

void cpu_idle(void)
{
    usleep(2000);
    ticks = now_ticks();
}

void net_poll(void)
{
    ticks = now_ticks();
}

int net_wait_step(struct timeout *t)
{
    usleep(500);
    net_poll();
    if (net_abort_check && net_abort_check())
        return NET_ERR_ABORTED;
    return timeout_expired(t) ? NET_ERR_TIMEOUT : 0;
}

const char *net_strerror(int err)
{
    return err == NET_ERR_TIMEOUT ? "no answer in time" : err == NET_ERR_CLOSED ? "closed" :
           err == NET_ERR_REFUSED ? "nothing is listening on that port" : "network error";
}

int net_resolve(const char *name, u32 *ip)
{
    struct in_addr a;

    if (!inet_aton(name, &a))
        return NET_ERR_NAME;
    *ip = ntohl(a.s_addr);
    return 0;
}

int tcp_connect(u32 ip, u16 port, u32 timeout_ticks)
{
    struct sockaddr_in sa;
    int one = 1;

    (void)timeout_ticks;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(ip);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sock, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(sock);
        sock = -1;
        return NET_ERR_REFUSED;
    }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    return 3;                   /* any small number: there is one connection */
}

int tcp_recv(int c, void *buf, u16 max)
{
    ssize_t n;

    (void)c;
    if (closed_by_peer)
        return NET_ERR_CLOSED;
    n = recv(sock, buf, max, 0);
    if (n > 0)
        return (int)n;
    if (n == 0) {
        closed_by_peer = 1;
        return NET_ERR_CLOSED;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : NET_ERR_RESET;
}

int tcp_read(int c, void *buf, u16 max, u32 timeout_ticks)
{
    struct timeout t;
    int n;

    timeout_start(&t, timeout_ticks);
    for (;;) {
        n = tcp_recv(c, buf, max);
        if (n)
            return n;
        n = net_wait_step(&t);
        if (n)
            return n;
    }
}

int tcp_write(int c, const void *data, u32 len, u32 timeout_ticks)
{
    const u8 *p = data;
    ssize_t n;

    (void)c;
    (void)timeout_ticks;
    while (len) {
        n = send(sock, p, len, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(500);
                continue;
            }
            return NET_ERR_RESET;
        }
        p += n;
        len -= (u32)n;
    }
    return 0;
}

void tcp_close(int c)
{
    (void)c;
    close(sock);
}

void tcp_abort(int c)
{
    (void)c;
    close(sock);
}

int shell_need_network(void)
{
    return 0;
}

void shell_fail(int err)
{
    printf("Failed: %s.\n", net_strerror(err));
}

int shell_streq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

void rng_bytes(u8 *out, u32 n)
{
    static FILE *f;

    if (!f)
        f = fopen("/dev/urandom", "rb");
    if (!f || fread(out, 1, n, f) != n)
        abort();
}

/* The screen: our character codes become UTF-8 on standard output. */
void con_putc(char c)
{
    u8 b = (u8)c;
    u32 cp = b < 0x80 ? b : b >= 0xA0 ? b : charset_extra[b - 0x80];

    if (cp < 0x80) {
        putchar((int)cp);
    } else if (cp < 0x800) {
        putchar((int)(0xC0 | (cp >> 6)));
        putchar((int)(0x80 | (cp & 0x3F)));
    } else {
        putchar((int)(0xE0 | (cp >> 12)));
        putchar((int)(0x80 | ((cp >> 6) & 0x3F)));
        putchar((int)(0x80 | (cp & 0x3F)));
    }
    fflush(stdout);
}

void con_cursor_toggle(void)
{
}

void bar_tick(void)
{
}

int bar_poll(void)
{
    return BAR_NONE;
}

void snd_beep(u32 nticks)
{
    (void)nticks;
}

void bar_work(void)
{
}

void bar_idle(void)
{
    cpu_idle();
}

/* The session's screen is the terminal in vt.c, which draws cells; here its
 * copy for the serial port is what gets shown. */
void serial_putc(char c)
{
    con_putc(c);
}

void con_draw(int col, int row, const u8 *text, int n, u8 attr)
{
    (void)col; (void)row; (void)text; (void)n; (void)attr;
}

void con_fill(int col, int row, int n, u8 ch, u8 attr)
{
    (void)col; (void)row; (void)n; (void)ch; (void)attr;
}

void con_scroll(int top, int bottom, int lines)
{
    (void)top; (void)bottom; (void)lines;
}

void con_invert(int col, int row)
{
    (void)col; (void)row;
}

void con_clear(void)
{
}

void con_goto(int col, int row)
{
    (void)col; (void)row;
}

static void (*divert)(char);

void kputc_divert(void (*to)(char))
{
    divert = to;
}

void kputc(char c)
{
    if (divert)
        divert(c);
    else
        con_putc(c);
}

void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
}

void kprintf(const char *fmt, ...)
{
    char text[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    kputs(text);
}

/* The keyboard: bytes from standard input, one key each. Bytes from 0x80
 * are our character codes, as the real keyboard driver produces them; byte
 * 0x01 is the Left arrow, to have one key that is not a character. */
/* The kernel asks these in windows (kernel/ask.c); here they are lines. */
int ask_password(const char *user, const char *host, char *line, int max)
{
    kprintf("%s@%s's password: ", user, host);
    return kbd_ask_line(line, max, 0);
}

int ask_trust(const char *host, const char *print)
{
    char answer[8];

    kprintf("\nThis is the first connection to %s. Its key has the fingerprint\n  %s\n"
            "Continue? Type yes: ", host, print);
    return kbd_ask_line(answer, sizeof answer, 1) >= 0 && strcmp(answer, "yes") == 0;
}

int kbd_get_event(void)
{
    unsigned char b;

    if (read(0, &b, 1) != 1)
        return -1;
    if (b == 0x01)
        return 0x3B << 8;
    return b;
}

u8 kbd_modifiers(void)
{
    return 0;
}

/* ------------------------------------- a folder as the SSH OS disk -- */

static FILE *disk_file(const char *name, const char *mode)
{
    char path[1024];

    if (!getenv("SSHOS_DISK"))
        return NULL;
    snprintf(path, sizeof path, "%s/%s", getenv("SSHOS_DISK"), name);
    return fopen(path, mode);
}

int fs_mount(void)
{
    return getenv("SSHOS_DISK") ? 0 : FS_ERR_NO_DISK;
}

int fs_mounted(void)
{
    return getenv("SSHOS_DISK") != NULL;
}

int fs_read(const char *name, void *buf, u32 max, u32 *len)
{
    FILE *f = disk_file(name, "rb");

    *len = 0;
    if (!f)
        return fs_mounted() ? FS_ERR_NOT_FOUND : FS_ERR_NO_DISK;
    *len = (u32)fread(buf, 1, max, f);
    fclose(f);
    return 0;
}

int fs_write(const char *name, const void *buf, u32 len)
{
    FILE *f = disk_file(name, "wb");

    if (!f)
        return FS_ERR_NO_DISK;
    fwrite(buf, 1, len, f);
    fclose(f);
    return 0;
}

void serial_puts(const char *s)
{
    (void)s;
}

void snd_from_settings(void)
{
}

void mouse_from_settings(void)
{
}

int main(int argc, char **argv)
{
    char *args[2], line[600], name[32], value[400];
    FILE *f;

    ticks = now_ticks();
    if (argc != 3 || !(f = fopen(argv[1], "r")))
        return 2;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, " %31[^ =] = %399[^\n]", name, value) == 2)
            config_set(name, value);
    fclose(f);
    saved_start();
    fcntl(0, F_SETFL, O_NONBLOCK);
    args[0] = "ssh";
    args[1] = argv[2];
    config_set("termlog", "1");         /* so the terminal copies what it shows to us */
    cmd_ssh(2, args);
    return 0;
}
