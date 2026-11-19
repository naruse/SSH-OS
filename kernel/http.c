/* A very small HTTP client: one request, one answer, connection closed.
 * Plain HTTP only; there is no encryption on this machine.
 */
#include "kernel.h"
#include "net.h"

#define HEAD_MAX 640

static char head[HEAD_MAX];

static char *put_str(char *p, char *end, const char *s)
{
    while (*s && p < end)
        *p++ = *s++;
    return p;
}

static char *put_num(char *p, char *end, u32 v)
{
    char digits[10];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n && p < end)
        *p++ = digits[--n];
    return p;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Is `needle` (lower case) somewhere in the first n bytes, ignoring case? */
static int contains(const char *text, u32 n, const char *needle)
{
    u32 i, len = strlen(needle), j;

    for (i = 0; i + len <= n; i++) {
        for (j = 0; j < len && lower(text[i + j]) == needle[j]; j++)
            ;
        if (j == len)
            return 1;
    }
    return 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = lower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* Undo "chunked" encoding in place: size in hex, CR LF, data, CR LF, ...
 * Returns the length of the plain data. */
static u32 unchunk(char *buf, u32 len)
{
    u32 in = 0, out = 0, size;
    int d;

    for (;;) {
        size = 0;
        while (in < len && (d = hex_digit(buf[in])) >= 0) {
            size = (size << 4) | (u32)d;
            in++;
        }
        while (in < len && buf[in] != '\n')     /* rest of the size line */
            in++;
        in++;
        if (size == 0 || in >= len)
            break;
        if (size > len - in)
            size = len - in;
        memmove(buf + out, buf + in, size);
        out += size;
        in += size + 2;                         /* the CR LF after the data */
    }
    return out;
}

/* Send a request and collect the answer's body in out (always ended with a
 * 0 byte; cut short if it does not fit). token and body may be NULL.
 * Returns 0 and sets *status to the HTTP status, or a NET_ERR_ value. */
int http_request(const char *method, u32 ip, u16 port, const char *host, const char *path,
                 const char *token, const char *body, char *out, u32 out_max, u32 *out_len,
                 int *status)
{
    char *p = head, *end = head + HEAD_MAX - 5;
    u32 body_len = body ? strlen(body) : 0, got = 0, i, start;
    int c, n, err;

    *status = 0;
    if (out_len)
        *out_len = 0;
    if (out_max)
        out[0] = 0;
    p = put_str(p, end, method);
    p = put_str(p, end, " ");
    p = put_str(p, end, path);
    p = put_str(p, end, " HTTP/1.1\r\nHost: ");
    p = put_str(p, end, host);
    p = put_str(p, end, "\r\nConnection: close\r\n");
    if (token && token[0]) {
        p = put_str(p, end, "Authorization: Bearer ");
        p = put_str(p, end, token);
        p = put_str(p, end, "\r\n");
    }
    if (body) {
        p = put_str(p, end, "Content-Type: application/json\r\nContent-Length: ");
        p = put_num(p, end, body_len);
        p = put_str(p, end, "\r\n");
    }
    if (p >= end)
        return NET_ERR_TOO_BIG;
    p = put_str(p, head + HEAD_MAX, "\r\n");

    c = tcp_connect(ip, port, 8 * 60);
    if (c < 0)
        return c;
    err = tcp_write(c, head, (u32)(p - head), 8 * 60);
    if (err == 0 && body_len)
        err = tcp_write(c, body, body_len, 8 * 60);
    /* Take everything until the server closes; drop what does not fit. */
    while (err == 0) {
        u32 room = out_max - 1 - got;

        if (room)
            n = tcp_read(c, out + got, (u16)(room > 4096 ? 4096 : room), 10 * 60);
        else
            n = tcp_read(c, head, HEAD_MAX, 10 * 60);   /* full: read and drop */
        if (n < 0) {
            if (n != NET_ERR_CLOSED)
                err = n;
            break;
        }
        if (room)
            got += (u32)n;
    }
    tcp_close(c);
    if (err)
        return err;

    /* "HTTP/1.1 200 OK", headers, an empty line, then the body. */
    if (got < 12 || out[0] != 'H' || out[4] != '/')
        return NET_ERR_RESET;
    *status = (out[9] - '0') * 100 + (out[10] - '0') * 10 + (out[11] - '0');
    for (start = 0; start + 3 < got; start++)
        if (out[start] == '\r' && out[start + 1] == '\n' && out[start + 2] == '\r' &&
            out[start + 3] == '\n')
            break;
    if (start + 3 >= got) {
        out[0] = 0;
        return 0;                               /* headers only */
    }
    i = contains(out, start, "transfer-encoding: chunked");
    start += 4;
    got -= start;
    memmove(out, out + start, got);
    if (i)
        got = unchunk(out, got);
    out[got] = 0;
    if (out_len)
        *out_len = got;
    return 0;
}
