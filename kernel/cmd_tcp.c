/* Shell commands that exercise TCP: a plain connection you can type into,
 * and a test that checks every byte of a transfer and times it. */
#include "kernel.h"
#include "net.h"
#include "shell.h"

/* The test servers (tools/testservers.py) and this file agree on what
 * byte number i of a test stream is: i * 131 + (i >> 8) * 7 + (i >> 16) + 3,
 * cut to 8 bits. It is worked out step by step here, because a multiply
 * per byte would take longer on the 68000 than receiving the byte does. */
struct pattern {
    u32 i;
    u8 value;
};

static void pattern_start(struct pattern *p)
{
    p->i = 0;
    p->value = 3;
}

static u8 pattern_next(struct pattern *p)
{
    u8 v = p->value;

    p->i++;
    p->value = (u8)(p->value + 131);
    if ((p->i & 0xFF) == 0) {
        p->value = (u8)(p->value + 7);
        if ((p->i & 0xFFFF) == 0)
            p->value++;
    }
    return v;
}

static int open_from_args(int argc, char **argv)
{
    u32 ip, port;
    int err, c;

    if (argc < 3 || !shell_parse_num(argv[2], &port) || port == 0 || port > 65535) {
        kprintf("Usage: %s HOST PORT\n", argv[0]);
        return -1000;
    }
    err = net_resolve(argv[1], &ip);
    if (err) {
        shell_fail(err);
        return -1000;
    }
    kputs("Connecting to ");
    shell_print_ip(ip);
    kprintf(" port %lu...\n", port);
    c = tcp_connect(ip, (u16)port, 20 * 60);
    if (c < 0) {
        shell_fail(c);
        return -1000;
    }
    return c;
}

/* tcp HOST PORT: show what arrives; send each typed line when Return is
 * pressed. Esc closes. Enough to talk to a web server by hand. */
void cmd_tcp(int argc, char **argv)
{
    char line[82];
    const u8 *data;
    int c, n, i, ev, len = 0;
    char ch;

    c = open_from_args(argc, argv);
    if (c < 0)
        return;
    kputs("Connected. Type a line and press Return to send it; Esc closes.\n");
    for (;;) {
        net_poll();
        n = tcp_peek(c, &data);
        if (n < 0) {
            kprintf("\n[%s]\n", net_strerror(n));
            break;
        }
        for (i = 0; i < n; i++) {
            ch = (char)data[i];
            if (ch == '\n' || ch == '\t' || (ch >= 32 && ch < 127))
                kputc(ch);
            else if (ch != '\r')
                kputc('.');
        }
        tcp_consume(c, (u16)n);

        if (kbd_escape) {
            kputs("\n[closed]\n");
            break;
        }
        while ((ev = kbd_get_event()) >= 0) {
            ch = (char)(ev & 0xFF);
            if (ch == '\n') {
                kputc('\n');
                line[len++] = '\r';
                line[len++] = '\n';
                n = tcp_write(c, line, (u32)len, 10 * 60);
                len = 0;
                if (n < 0) {
                    kprintf("[%s]\n", net_strerror(n));
                    goto out;
                }
            } else if (ch == '\b') {
                if (len) {
                    len--;
                    kputs("\b \b");
                }
            } else if (ch >= 32 && ch < 127 && len < (int)sizeof line - 2) {
                line[len++] = ch;
                kputc(ch);
            }
        }
        if (n == 0)
            cpu_idle();
    }
out:
    tcp_close(c);
}

static void report(const char *what, u32 bytes, u32 dt, u32 bad_at)
{
    if (dt == 0)
        dt = 1;
    kprintf("%s %lu bytes in %lu ticks: %lu KB per second. ", what, bytes, dt,
            (bytes / 1024) * 60 / dt + (bytes % 1024) * 60 / dt / 1024);
    if (bad_at == 0xFFFFFFFFUL)
        kputs("Every byte correct.\n");
    else
        kprintf("WRONG DATA at byte %lu.\n", bad_at);
}

/* tcptest HOST PORT down: read a test stream until the server closes.
 * tcptest HOST PORT echo: send 32 KB and check it comes back unchanged. */
void cmd_tcptest(int argc, char **argv)
{
    enum { ECHO_TOTAL = 32768, PIECE = 1000 };
    static u8 piece[PIECE];
    const u8 *data;
    u32 got = 0, sent = 0, bad_at = 0xFFFFFFFFUL, t0, i;
    int c, n, echo, err = 0;
    struct timeout t;
    struct pattern out, in;

    if (argc < 4 || (!shell_streq(argv[3], "down") && !shell_streq(argv[3], "echo"))) {
        kputs("Usage: tcptest HOST PORT down, or tcptest HOST PORT echo\n");
        return;
    }
    echo = shell_streq(argv[3], "echo");
    c = open_from_args(argc, argv);
    if (c < 0)
        return;
    pattern_start(&out);
    pattern_start(&in);
    t0 = ticks;
    timeout_start(&t, 20 * 60);
    for (;;) {
        if (echo && sent < ECHO_TOTAL && tcp_send_space(c) >= PIECE) {
            for (i = 0; i < PIECE && sent + i < ECHO_TOTAL; i++)
                piece[i] = pattern_next(&out);
            n = tcp_send(c, piece, (u16)i);
            if (n < 0) {
                err = n;
                break;
            }
            sent += (u32)n;
        }
        n = tcp_peek(c, &data);
        if (n < 0) {
            err = n;
            break;
        }
        for (i = 0; i < (u32)n; i++)
            if (data[i] != pattern_next(&in) && bad_at == 0xFFFFFFFFUL)
                bad_at = got + i;
        tcp_consume(c, (u16)n);
        got += (u32)n;
        if (n)
            timeout_start(&t, 20 * 60);
        if (echo && got >= ECHO_TOTAL)
            break;
        err = net_wait_step(&t);
        if (err)
            break;
    }
    if (err && err != NET_ERR_CLOSED)
        shell_fail(err);
    report(echo ? "Echoed" : "Received", got, ticks - t0, bad_at);
    kprintf("Segments sent again %lu, dropped as out of order %lu, damaged frames %lu.\n",
            net_stats.tcp_resent, net_stats.tcp_dropped, net_stats.rx_bad);
    tcp_close(c);
}
