/* TCP for SSH OS: outgoing connections only, a handful at a time.
 *
 * Deliberately plain:
 *   - Data that arrives out of order is dropped and asked for again, rather
 *     than kept aside. On a home network that is rare.
 *   - A segment not acknowledged in time is sent again, along with all that
 *     followed it, waiting twice as long each time.
 *   - Acknowledgements are sent once per net_poll(), after all the frames
 *     that came in have been looked at, so a burst of segments gets one.
 *   - No window scaling, selective acknowledgement or timestamps.
 *
 * Each connection has two ring buffers. The send buffer holds everything
 * the program has queued and the other side has not yet confirmed; its
 * first byte is sequence number snd_una. The receive buffer holds data
 * that has arrived, in order, and the program has not yet taken; the free
 * space in it is the window we advertise.
 *
 * Closing: each side says "finished" with a FIN, which counts as one
 * sequence number after its last byte of data. A connection is over when
 * both FINs have been sent and confirmed.
 */
#include "net_internal.h"

#define CONNECTIONS 4
#define RX_SIZE     8192        /* both sizes must be powers of two */
#define TX_SIZE     4096
#define RX_MASK     (RX_SIZE - 1)
#define TX_MASK     (TX_SIZE - 1)

#define TCP_HLEN    20
#define OUR_MSS     (IP_MAX_DATA - TCP_HLEN)    /* 1460: the most data per segment */
#define DEFAULT_MSS 536         /* what to assume if the other side does not say */
#define BURST       6           /* most segments sent per connection per poll */

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define RTO_FIRST       (1 * TICKS_PER_SEC)     /* first wait for an acknowledgement */
#define RTO_MAX         (16 * TICKS_PER_SEC)
#define DATA_RETRIES    7       /* then give up: about a minute in all */
#define SYN_RETRIES     4       /* about 15 seconds */
#define LINGER_TICKS    (2 * TICKS_PER_SEC)     /* stay to re-acknowledge a repeated FIN */
#define RELEASE_TICKS   (10 * TICKS_PER_SEC)    /* longest a closed connection may take to finish */

/* Sequence numbers wrap around, so compare them by their difference. */
#define SEQ_LT(a, b)    ((s32)((u32)(a) - (u32)(b)) < 0)
#define SEQ_GT(a, b)    ((s32)((u32)(a) - (u32)(b)) > 0)

enum {
    S_FREE,
    S_CONNECTING,       /* SYN sent (or about to be), no answer yet */
    S_OPEN,             /* connected; either side may already have said FIN */
    S_DONE,             /* both FINs exchanged; lingering briefly */
    S_CLOSED            /* over; kept only until the program lets go of it */
};

struct connection {
    u8  state;
    u8  released;       /* the program has called tcp_close() */
    u8  syn_sent;
    u8  fin_wanted, fin_sent, fin_acked, fin_received;
    u8  ack_due;        /* we owe the other side an acknowledgement */
    u8  probing;        /* send one byte into a closed window */
    u8  heard;          /* something valid arrived since the timer last ran out */
    u8  timer_on;
    u8  retries;
    s8  error;
    u16 local_port, remote_port;
    u32 remote_ip;
    u16 mss;
    u16 rto;
    u32 timer_at;       /* when to send again */
    u32 deadline;       /* S_DONE: when to finish; released: when to give up */
    u32 snd_una;        /* oldest byte not yet confirmed */
    u32 snd_nxt;        /* next byte to send */
    u32 snd_max;        /* highest ever sent + 1 */
    u32 snd_wnd;        /* how much the other side will take beyond snd_una */
    u32 rcv_nxt;        /* next byte we expect */
    u32 rcv_adv;        /* end of the window we last advertised */
    u16 tx_head, tx_len;
    u16 rx_head, rx_len;
    u8  tx[TX_SIZE];
    u8  rx[RX_SIZE];
};

static struct connection conns[CONNECTIONS];
static u16 next_port;

static struct connection *lookup(int c)
{
    if (c < 0 || c >= CONNECTIONS || conns[c].state == S_FREE)
        return NULL;
    return &conns[c];
}

static void start_timer(struct connection *c)
{
    c->timer_at = ticks + c->rto;
    c->timer_on = 1;
}

/* The connection is over. Keep it until the program has seen that. */
static void finish(struct connection *c, int error)
{
    c->state = c->released ? S_FREE : S_CLOSED;
    c->error = (s8)error;
    c->timer_on = 0;
}

/* ------------------------------------------------------------ sending -- */

/* Build and send one segment. Data, if any, is `n` bytes of the send
 * buffer starting `offset` bytes after snd_una. */
static int send_segment(struct connection *c, u32 seq, u8 flags, u16 offset, u16 n)
{
    u8 *t = net_ip_payload();
    u16 hlen = TCP_HLEN, window = (u16)(RX_SIZE - c->rx_len), start, first;
    int err;

    net_put16(t, c->local_port);
    net_put16(t + 2, c->remote_port);
    net_put32(t + 4, seq);
    net_put32(t + 8, (flags & F_ACK) ? c->rcv_nxt : 0);
    t[13] = flags;
    net_put16(t + 14, window);
    net_put16(t + 16, 0);
    net_put16(t + 18, 0);
    if (flags & F_SYN) {        /* option: the largest segment we accept */
        t[20] = 2;
        t[21] = 4;
        net_put16(t + 22, OUR_MSS);
        hlen += 4;
    }
    t[12] = (u8)((hlen / 4) << 4);

    if (n) {                    /* copy out of the ring, in two parts if it wraps */
        start = (u16)((c->tx_head + offset) & TX_MASK);
        first = (u16)(TX_SIZE - start);
        if (first > n)
            first = n;
        memcpy(t + hlen, c->tx + start, first);
        memcpy(t + hlen + first, c->tx, (size_t)(n - first));
    }
    net_put16(t + 16, net_sum_finish(net_sum(t, (u32)hlen + n,
              net_pseudo_sum(net.ip, c->remote_ip, PROTO_TCP, (u16)(hlen + n)))));
    err = net_ip_send(net.ip, c->remote_ip, PROTO_TCP, (u16)(hlen + n));
    if (err == 0 && (flags & F_ACK)) {
        c->rcv_adv = c->rcv_nxt + window;
        c->ack_due = 0;
    }
    return err;
}

/* Answer a segment that belongs to no connection, so the sender gives up. */
static void send_reset(u32 dst, const u8 *seg, u16 data_len)
{
    u8 *t = net_ip_payload();
    u8 flags = seg[13];
    u32 seq = 0, ack = 0;

    if (flags & F_ACK) {
        seq = net_get32(seg + 8);
        flags = F_RST;
    } else {
        ack = net_get32(seg + 4) + data_len + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0);
        flags = F_RST | F_ACK;
    }
    net_put16(t, net_get16(seg + 2));
    net_put16(t + 2, net_get16(seg));
    net_put32(t + 4, seq);
    net_put32(t + 8, ack);
    t[12] = (TCP_HLEN / 4) << 4;
    t[13] = flags;
    net_put16(t + 14, 0);
    net_put16(t + 16, 0);
    net_put16(t + 18, 0);
    net_put16(t + 16, net_sum_finish(net_sum(t, TCP_HLEN,
              net_pseudo_sum(net.ip, dst, PROTO_TCP, TCP_HLEN))));
    net_ip_send(net.ip, dst, PROTO_TCP, TCP_HLEN);
}

/* Send whatever this connection can send now. */
static void output(struct connection *c)
{
    u32 offset, room, window;
    u16 unsent, n;
    u8 flags;
    int burst, fin;

    if (c->state == S_CONNECTING) {
        if (!c->syn_sent && send_segment(c, c->snd_una, F_SYN, 0, 0) == 0) {
            c->syn_sent = 1;
            start_timer(c);
        }
        return;
    }
    if (c->state != S_OPEN && c->state != S_DONE)
        return;

    for (burst = 0; burst < BURST; burst++) {
        offset = c->snd_nxt - c->snd_una;       /* bytes sent and not yet confirmed */
        if (offset > c->tx_len)
            break;                              /* everything is out, the FIN too */
        unsent = (u16)(c->tx_len - offset);
        window = c->snd_wnd;
        if (window == 0 && c->probing)
            window = 1;
        room = window > offset ? window - offset : 0;
        n = unsent;
        if (n > room)
            n = (u16)room;
        if (n > c->mss)
            n = c->mss;
        /* Our FIN goes with the last of the data, until it is confirmed. */
        fin = c->fin_wanted && !c->fin_acked && n == unsent;
        if (n == 0 && !fin) {
            /* Data waiting, nothing in flight, window shut: arrange to probe. */
            if (unsent && offset == 0 && !c->timer_on)
                start_timer(c);
            break;
        }
        flags = F_ACK;
        if (n && n == unsent)
            flags |= F_PSH;
        if (fin)
            flags |= F_FIN;
        if (send_segment(c, c->snd_nxt, flags, (u16)offset, n) != 0)
            break;                              /* try again on the next poll */
        if (SEQ_LT(c->snd_nxt, c->snd_max))
            net_stats.tcp_resent++;
        c->snd_nxt += n + (fin ? 1 : 0);
        if (SEQ_GT(c->snd_nxt, c->snd_max))
            c->snd_max = c->snd_nxt;
        if (fin)
            c->fin_sent = 1;
        c->probing = 0;
        if (!c->timer_on)
            start_timer(c);
    }
    if (c->ack_due)
        send_segment(c, c->snd_nxt, F_ACK, 0, 0);
}

/* The acknowledgement timer ran out. */
static void timed_out(struct connection *c)
{
    c->timer_on = 0;
    if (c->state == S_CONNECTING) {
        if (++c->retries > SYN_RETRIES) {
            finish(c, NET_ERR_TIMEOUT);
            return;
        }
        c->syn_sent = 0;
    } else if (c->tx_len && c->snd_wnd == 0) {
        /* The other side has no room. That can last as long as it likes,
         * provided it keeps answering: offer one byte now and then to learn
         * when room appears. Only silence counts towards giving up. */
        if (c->heard)
            c->retries = 0;
        else if (++c->retries > DATA_RETRIES) {
            finish(c, NET_ERR_TIMEOUT);
            return;
        }
        c->snd_nxt = c->snd_una;
        c->probing = 1;
    } else if (c->snd_una != c->snd_max) {      /* something sent was never confirmed */
        if (++c->retries > DATA_RETRIES) {
            finish(c, NET_ERR_TIMEOUT);
            return;
        }
        c->snd_nxt = c->snd_una;                /* go back and send it all again */
    } else {
        return;
    }
    c->heard = 0;
    c->rto = (u16)(c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2);
    output(c);
    if (c->state != S_FREE && c->state != S_CLOSED && !c->timer_on)
        start_timer(c);
}

void tcp_poll(void)
{
    struct connection *c;

    for (c = conns; c < conns + CONNECTIONS; c++) {
        if (c->state == S_FREE || c->state == S_CLOSED)
            continue;
        if (c->state == S_DONE && (s32)(ticks - c->deadline) >= 0) {
            finish(c, 0);
            continue;
        }
        if (c->released && c->state != S_DONE && (s32)(ticks - c->deadline) >= 0) {
            /* Closed by the program long ago and still not finished. */
            send_segment(c, c->snd_nxt, F_RST | F_ACK, 0, 0);
            c->state = S_FREE;
            continue;
        }
        if (c->timer_on && (s32)(ticks - c->timer_at) >= 0)
            timed_out(c);
        else
            output(c);
    }
}

void tcp_reset_all(void)
{
    memset(conns, 0, sizeof conns);
}

/* ---------------------------------------------------------- receiving -- */

/* The largest-segment option in a SYN, if there is one. */
static u16 mss_option(const u8 *seg, u16 hlen)
{
    u16 i = TCP_HLEN, n, mss;

    while (i < hlen) {
        if (seg[i] == 0)
            break;
        if (seg[i] == 1) {
            i++;
            continue;
        }
        if (i + 1 >= hlen)
            break;
        n = seg[i + 1];
        if (n < 2 || i + n > hlen)
            break;
        if (seg[i] == 2 && n == 4) {
            mss = net_get16(seg + i + 2);
            if (mss < 64)
                mss = 64;
            return mss < OUR_MSS ? mss : OUR_MSS;
        }
        i = (u16)(i + n);
    }
    return DEFAULT_MSS;
}

void tcp_input(u32 src, const u8 *seg, u16 len)
{
    struct connection *c;
    const u8 *data;
    u32 seq, ack, acked, skip;
    u16 hlen, data_len, take, space, at, first;
    u8 flags;

    if (len < TCP_HLEN || (hlen = (u16)((seg[12] >> 4) * 4)) < TCP_HLEN || hlen > len ||
        net_sum_finish(net_sum(seg, len, net_pseudo_sum(src, net.ip, PROTO_TCP, len))) != 0) {
        net_stats.rx_bad++;
        return;
    }
    seq = net_get32(seg + 4);
    ack = net_get32(seg + 8);
    flags = seg[13];
    data = seg + hlen;
    data_len = (u16)(len - hlen);

    for (c = conns; c < conns + CONNECTIONS; c++)
        if (c->state != S_FREE && c->state != S_CLOSED && c->remote_ip == src &&
            c->remote_port == net_get16(seg) && c->local_port == net_get16(seg + 2))
            break;
    if (c == conns + CONNECTIONS) {
        if (!(flags & F_RST))
            send_reset(src, seg, data_len);
        return;
    }

    if (c->state == S_CONNECTING) {
        if ((flags & F_ACK) && ack != c->snd_nxt) {
            if (!(flags & F_RST))
                send_reset(src, seg, data_len);     /* an answer to something we never sent */
            return;
        }
        if (flags & F_RST) {
            if (flags & F_ACK)
                finish(c, NET_ERR_REFUSED);
            return;
        }
        if ((flags & (F_SYN | F_ACK)) != (F_SYN | F_ACK))
            return;
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_wnd = net_get16(seg + 14);
        c->mss = mss_option(seg, hlen);
        c->state = S_OPEN;
        c->retries = 0;
        c->rto = RTO_FIRST;
        c->timer_on = 0;
        c->ack_due = 1;
        return;
    }

    /* Connected. */
    if (flags & F_RST) {
        if ((u32)(seq - c->rcv_nxt) < RX_SIZE)      /* only if it fits where we are */
            finish(c, NET_ERR_RESET);
        return;
    }
    if (flags & F_SYN) {        /* our reply to the SYN was lost: send it again */
        c->ack_due = 1;
        return;
    }
    if (!(flags & F_ACK))
        return;
    c->heard = 1;

    if (SEQ_GT(ack, c->snd_max)) {
        c->ack_due = 1;         /* confirms data we have not sent */
        return;
    }
    if (!SEQ_LT(ack, c->snd_una)) {
        acked = ack - c->snd_una;
        if (acked) {
            if (acked > c->tx_len) {            /* the extra one is our FIN */
                acked = c->tx_len;
                if (c->fin_sent)
                    c->fin_acked = 1;
            }
            c->tx_head = (u16)((c->tx_head + acked) & TX_MASK);
            c->tx_len = (u16)(c->tx_len - acked);
            c->snd_una = ack;
            if (SEQ_LT(c->snd_nxt, ack))
                c->snd_nxt = ack;
            c->retries = 0;
            c->rto = RTO_FIRST;
            c->timer_on = 0;
            if (c->snd_una != c->snd_max)
                start_timer(c);                 /* more is still in flight */
        }
        c->snd_wnd = net_get16(seg + 14);
    }

    if (data_len || (flags & F_FIN)) {
        if (c->fin_received) {
            c->ack_due = 1;                     /* a repeat of what we already have */
        } else {
            /* Cut off any part we already have (a segment sent again). */
            if (SEQ_LT(seq, c->rcv_nxt)) {
                skip = c->rcv_nxt - seq;
                if (skip > data_len) {
                    c->ack_due = 1;
                    goto closing;
                }
                data += skip;
                data_len = (u16)(data_len - skip);
                seq = c->rcv_nxt;
            }
            if (seq != c->rcv_nxt) {            /* a gap before it: ask again */
                net_stats.tcp_dropped++;
                c->ack_due = 1;
                goto closing;
            }
            space = (u16)(RX_SIZE - c->rx_len);
            take = data_len < space ? data_len : space;
            if (take) {
                at = (u16)((c->rx_head + c->rx_len) & RX_MASK);
                first = (u16)(RX_SIZE - at);
                if (first > take)
                    first = take;
                memcpy(c->rx + at, data, first);
                memcpy(c->rx, data + first, (size_t)(take - first));
                c->rx_len = (u16)(c->rx_len + take);
                c->rcv_nxt += take;
            }
            if ((flags & F_FIN) && take == data_len) {
                c->fin_received = 1;
                c->rcv_nxt++;
            }
            c->ack_due = 1;
        }
    }

closing:
    if (c->state == S_OPEN && c->fin_received && c->fin_acked) {
        c->state = S_DONE;
        c->timer_on = 0;
        c->deadline = ticks + LINGER_TICKS;
    }
}

/* --------------------------------------------------- program interface -- */

int tcp_open(u32 ip, u16 port)
{
    struct connection *c, *other;
    u32 iss;

    if (!net.up)
        return NET_ERR_DOWN;
    if (!net.ip)
        return NET_ERR_NO_ROUTE;
    for (c = conns; c < conns + CONNECTIONS; c++)
        if (c->state == S_FREE)
            break;
    if (c == conns + CONNECTIONS)
        return NET_ERR_NO_SOCKET;

    /* A local port from the range set aside for this, not already in use. */
    do {
        if (next_port < 49152)
            next_port = (u16)(49152 + (ticks & 0x1FFF));
        next_port++;
        for (other = conns; other < conns + CONNECTIONS; other++)
            if (other->state != S_FREE && other->local_port == next_port)
                break;
    } while (next_port < 49152 || other != conns + CONNECTIONS);

    /* Clear everything but the two big buffers. */
    memset(c, 0, (size_t)((u8 *)c->tx - (u8 *)c));
    iss = (ticks << 10) ^ ((u32)next_port * 0x9E3779B1UL) ^ ip;
    c->state = S_CONNECTING;
    c->local_port = next_port;
    c->remote_port = port;
    c->remote_ip = ip;
    c->mss = DEFAULT_MSS;
    c->rto = RTO_FIRST;
    c->snd_una = iss;
    c->snd_nxt = iss + 1;       /* the SYN counts as one */
    c->snd_max = iss + 1;
    output(c);
    return (int)(c - conns);
}

int tcp_connecting(int c)
{
    struct connection *k = lookup(c);

    return k && k->state == S_CONNECTING;
}

int tcp_is_open(int c)
{
    struct connection *k = lookup(c);

    return k && k->state == S_OPEN && !k->fin_wanted;
}

int tcp_error(int c)
{
    struct connection *k = lookup(c);

    return k ? k->error : NET_ERR_CLOSED;
}

u16 tcp_send_space(int c)
{
    struct connection *k = lookup(c);

    return (k && k->state == S_OPEN && !k->fin_wanted) ? (u16)(TX_SIZE - k->tx_len) : 0;
}

u16 tcp_unsent(int c)
{
    struct connection *k = lookup(c);

    return k ? k->tx_len : 0;
}

int tcp_send(int c, const void *data, u16 len)
{
    struct connection *k = lookup(c);
    u16 at, first;

    if (!k)
        return NET_ERR_CLOSED;
    if (k->state != S_OPEN || k->fin_wanted)
        return k->error ? k->error : NET_ERR_CLOSED;
    if (len > TX_SIZE - k->tx_len)
        len = (u16)(TX_SIZE - k->tx_len);
    at = (u16)((k->tx_head + k->tx_len) & TX_MASK);
    first = (u16)(TX_SIZE - at);
    if (first > len)
        first = len;
    memcpy(k->tx + at, data, first);
    memcpy(k->tx, (const u8 *)data + first, (size_t)(len - first));
    k->tx_len = (u16)(k->tx_len + len);
    return len;
}

int tcp_peek(int c, const u8 **data)
{
    struct connection *k = lookup(c);
    u16 n;

    if (!k)
        return NET_ERR_CLOSED;
    if (k->rx_len == 0) {
        if (k->state == S_CLOSED || k->state == S_DONE || k->fin_received)
            return k->error ? k->error : NET_ERR_CLOSED;
        return 0;
    }
    n = (u16)(RX_SIZE - k->rx_head);            /* up to the end of the ring */
    if (n > k->rx_len)
        n = k->rx_len;
    *data = k->rx + k->rx_head;
    return n;
}

void tcp_consume(int c, u16 n)
{
    struct connection *k = lookup(c);

    if (!k)
        return;
    if (n > k->rx_len)
        n = k->rx_len;
    k->rx_head = (u16)((k->rx_head + n) & RX_MASK);
    k->rx_len = (u16)(k->rx_len - n);
    /* Tell the other side once a worthwhile amount of room has opened up
     * beyond what it was last told; it may be waiting for exactly that. */
    if (k->state == S_OPEN && !k->fin_received &&
        (u32)(k->rcv_nxt + (RX_SIZE - k->rx_len) - k->rcv_adv) >= RX_SIZE / 4)
        k->ack_due = 1;
}

int tcp_recv(int c, void *buf, u16 max)
{
    const u8 *data = NULL;
    u8 *out = buf;
    int n, total = 0;

    while (max) {
        n = tcp_peek(c, &data);
        if (n <= 0)
            return total ? total : n;
        if (n > max)
            n = max;
        memcpy(out, data, (size_t)n);
        tcp_consume(c, (u16)n);
        out += n;
        max = (u16)(max - n);
        total += n;
    }
    return total;
}

void tcp_close(int c)
{
    struct connection *k = lookup(c);

    if (!k)
        return;
    k->released = 1;
    k->deadline = ticks + RELEASE_TICKS;
    if (k->state == S_OPEN) {
        k->fin_wanted = 1;
        k->rx_len = 0;          /* nobody will read what is left */
    } else if (k->state != S_DONE) {
        if (k->state == S_CONNECTING && k->syn_sent)
            send_segment(k, k->snd_nxt, F_RST, 0, 0);
        k->state = S_FREE;
    }
}

void tcp_abort(int c)
{
    struct connection *k = lookup(c);

    if (!k)
        return;
    if (k->state == S_OPEN || (k->state == S_CONNECTING && k->syn_sent))
        send_segment(k, k->snd_nxt, k->state == S_OPEN ? (F_RST | F_ACK) : F_RST, 0, 0);
    k->state = S_FREE;
}

/* ------------------------------------------------------ waiting calls -- */

int tcp_connect(u32 ip, u16 port, u32 timeout_ticks)
{
    struct timeout t;
    int c, err;

    err = net_route_ready(ip);
    if (err)
        return err;
    c = tcp_open(ip, port);
    if (c < 0)
        return c;
    timeout_start(&t, timeout_ticks);
    while (tcp_connecting(c)) {
        err = net_wait_step(&t);
        if (err && tcp_connecting(c)) {
            tcp_abort(c);
            return err;
        }
    }
    if (conns[c].state != S_OPEN) {
        err = tcp_error(c);
        tcp_abort(c);
        return err ? err : NET_ERR_CLOSED;
    }
    return c;
}

int tcp_write(int c, const void *data, u32 len, u32 timeout_ticks)
{
    struct timeout t;
    const u8 *p = data;
    int n, err;

    timeout_start(&t, timeout_ticks);
    while (len) {
        n = tcp_send(c, p, len > 0x4000 ? 0x4000 : (u16)len);
        if (n < 0)
            return n;
        p += n;
        len -= (u32)n;
        if (n)                  /* progress: start the clock again */
            timeout_start(&t, timeout_ticks);
        err = net_wait_step(&t);
        if (err && len)
            return err;
    }
    return 0;
}

int tcp_read(int c, void *buf, u16 max, u32 timeout_ticks)
{
    struct timeout t;
    int n, err;

    timeout_start(&t, timeout_ticks);
    for (;;) {
        n = tcp_recv(c, buf, max);
        if (n != 0)
            return n;
        err = net_wait_step(&t);
        if (err)
            return err;
    }
}

int tcp_flush(int c, u32 timeout_ticks)
{
    struct timeout t;
    struct connection *k = lookup(c);
    int err;

    if (!k)
        return NET_ERR_CLOSED;
    timeout_start(&t, timeout_ticks);
    while (k->tx_len) {
        if (k->state != S_OPEN)
            return k->error ? k->error : NET_ERR_CLOSED;
        err = net_wait_step(&t);
        if (err)
            return err;
    }
    return 0;
}
