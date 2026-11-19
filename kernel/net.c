/* SSH OS network stack. See net.h for the outline.
 *
 * Everything on the wire is read and written a byte at a time through the
 * get/put helpers below. That keeps the code independent of the CPU's byte
 * order (it is also built and tested on a modern machine) and safe on the
 * 68000, which faults on 16- or 32-bit access to an odd address.
 *
 * One packet is built at a time, in txbuf, headers and all. A packet is
 * never kept waiting: if the hardware address of the next hop is not known
 * yet, an ARP request goes out instead and the caller tries again.
 */
#include "kernel.h"
#include "net.h"
#include "net_internal.h"

#define UDP_HLEN    8
#define ETH_MIN     60          /* shortest frame, without the checksum */

#define ETHERTYPE_IP    0x0800
#define ETHERTYPE_ARP   0x0806

#define ARP_ENTRIES     8
#define ARP_MAX_AGE     (10UL * 60 * TICKS_PER_SEC)
#define UDP_SOCKETS     4

struct net_config net;
struct net_stats net_stats;
int (*net_abort_check)(void);

static u8 txbuf[ETH_MAX + 2];
static u8 arpbuf[ETH_MIN];      /* ARP has its own buffer so it never disturbs txbuf */
static u16 ip_ident;

static const u8 mac_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static struct {
    u32 ip;
    u32 seen;                   /* tick count when learned */
    u8  mac[6];
    u8  valid;
} arp_cache[ARP_ENTRIES];

static struct {
    u16 port;
    udp_handler handler;
} udp_sockets[UDP_SOCKETS];

/* ------------------------------------------------------------ helpers -- */

u16 net_get16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

u32 net_get32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

void net_put16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

void net_put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static int same(const u8 *a, const u8 *b, u16 n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

/* Types for reading 32 bits straight out of a byte buffer: allowed to alias
 * anything, and only needing an even address (all the 68000 asks for). */
typedef u32 __attribute__((may_alias, aligned(2))) u32_in_buffer;

/* Internet checksum: add up 16-bit big-endian words. Chaining calls only
 * works when every part but the last has an even length. */
u32 net_sum(const u8 *p, u32 len, u32 sum)
{
    /* Fast path for the bulk of a large packet at an even address: add 32
     * bits at a time, carrying overflow back in at the bottom. This kind of
     * sum comes out the same whatever the byte order, apart from a final
     * swap, so it is done in the CPU's own order. About four times faster
     * on the 68000 than the byte loop below. */
    if (len >= 32 && ((size_t)p & 1) == 0) {
        const u32_in_buffer *l = (const u32_in_buffer *)p;
        u32 longs = len >> 2, part = 0, v;

        len &= 3;
        while (longs--) {
            v = *l++;
            part += v;
            if (part < v)
                part++;
        }
        p = (const u8 *)l;
        part = (part >> 16) + (part & 0xFFFF);
        part = (part >> 16) + (part & 0xFFFF);
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
        part = ((part & 0xFF) << 8) | (part >> 8);
#endif
        sum += part;
    }
    while (len > 1) {
        sum += ((u32)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)p[0] << 8;
    return sum;
}

u16 net_sum_finish(u32 sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static int is_local(u32 ip)
{
    return ((ip ^ net.ip) & net.mask) == 0;
}

static int is_broadcast(u32 ip)
{
    return ip == 0xFFFFFFFFUL || (net.ip && ip == (net.ip | ~net.mask));
}

/* One turn of a waiting loop: let the network run, then say whether to go
 * on waiting (0) or give up. */
int net_wait_step(struct timeout *t)
{
    net_poll();
    if (net_abort_check && net_abort_check())
        return NET_ERR_ABORTED;
    if (timeout_expired(t))
        return NET_ERR_TIMEOUT;
    return 0;
}

/* Poll the network until *flag becomes nonzero. */
static int net_wait_for(const volatile u8 *flag, u32 nticks)
{
    struct timeout t;
    int err;

    timeout_start(&t, nticks);
    for (;;) {
        err = net_wait_step(&t);
        if (*flag)
            return 0;
        if (err)
            return err;
    }
}

static int frame_send(u8 *frame, u16 len)
{
    while (len < ETH_MIN)
        frame[len++] = 0;
    if (eth_send(frame, len) != 0) {
        net_stats.tx_errors++;
        return NET_ERR_SEND;
    }
    net_stats.tx_frames++;
    return 0;
}

/* ---------------------------------------------------------------- ARP -- */

static void arp_learn(u32 ip, const u8 *mac)
{
    int i, slot = 0;
    u32 age, oldest = 0;

    for (i = 0; i < ARP_ENTRIES; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            slot = i;
            break;
        }
        age = arp_cache[i].valid ? (u32)(ticks - arp_cache[i].seen) + 1 : 0xFFFFFFFFUL;
        if (age > oldest) {     /* remember an empty slot, or else the oldest */
            oldest = age;
            slot = i;
        }
    }
    arp_cache[slot].ip = ip;
    arp_cache[slot].seen = ticks;
    arp_cache[slot].valid = 1;
    memcpy(arp_cache[slot].mac, mac, 6);
}

static int arp_lookup(u32 ip, u8 *mac)
{
    int i;

    for (i = 0; i < ARP_ENTRIES; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            if ((u32)(ticks - arp_cache[i].seen) > ARP_MAX_AGE) {
                arp_cache[i].valid = 0;     /* stale: ask again */
                return 0;
            }
            memcpy(mac, arp_cache[i].mac, 6);
            return 1;
        }
    return 0;
}

/* op 1 = "who has target_ip?", op 2 = "target_ip, here is my address". */
static void arp_send(u16 op, const u8 *eth_dst, const u8 *target_mac, u32 target_ip)
{
    u8 *a = arpbuf + ETH_HLEN;

    memcpy(arpbuf, eth_dst, 6);
    memcpy(arpbuf + 6, net.mac, 6);
    net_put16(arpbuf + 12, ETHERTYPE_ARP);
    net_put16(a, 1);                /* Ethernet */
    net_put16(a + 2, ETHERTYPE_IP);
    a[4] = 6;
    a[5] = 4;
    net_put16(a + 6, op);
    memcpy(a + 8, net.mac, 6);
    net_put32(a + 14, net.ip);
    memcpy(a + 18, target_mac, 6);
    net_put32(a + 24, target_ip);
    frame_send(arpbuf, ETH_HLEN + 28);
}

static void arp_input(const u8 *a, u16 len)
{
    u32 sender_ip, target_ip;
    u8 mac[6];

    if (len < 28 || net_get16(a) != 1 || net_get16(a + 2) != ETHERTYPE_IP || a[4] != 6 || a[5] != 4) {
        net_stats.rx_ignored++;
        return;
    }
    if (!net.ip)
        return;
    sender_ip = net_get32(a + 14);
    target_ip = net_get32(a + 24);

    /* Remember the sender if it is talking to us, or refresh what we had. */
    if (sender_ip && (target_ip == net.ip || arp_lookup(sender_ip, mac)))
        arp_learn(sender_ip, a + 8);
    if (net_get16(a + 6) == 1 && target_ip == net.ip)
        arp_send(2, a + 8, a + 8, sender_ip);
}

/* Which machine on the local network a packet for dst must be handed to. */
static int next_hop(u32 dst, u32 *hop)
{
    if (is_local(dst))
        *hop = dst;
    else if (net.gateway)
        *hop = net.gateway;
    else
        return NET_ERR_NO_ROUTE;
    return 0;
}

/* Find out the hardware address needed to reach dst, asking and waiting up
 * to about two seconds. Waiting calls use this before building a packet. */
int net_route_ready(u32 dst)
{
    struct timeout t;
    u32 hop;
    u8 mac[6];
    int err, tries;

    if (!net.up)
        return NET_ERR_DOWN;
    if (!net.ip)
        return NET_ERR_NO_ROUTE;
    if (is_broadcast(dst))
        return 0;
    err = next_hop(dst, &hop);
    if (err)
        return err;
    for (tries = 0; tries < 4; tries++) {
        if (arp_lookup(hop, mac))
            return 0;
        arp_send(1, mac_broadcast, mac_broadcast, hop);
        timeout_start(&t, TICKS_PER_SEC / 2);
        while (!timeout_expired(&t)) {
            net_poll();
            if (arp_lookup(hop, mac))
                return 0;
            if (net_abort_check && net_abort_check())
                return NET_ERR_ABORTED;
        }
    }
    return NET_ERR_ARP;
}

/* ----------------------------------------------------------------- IP -- */

u8 *net_ip_payload(void)
{
    return txbuf + ETH_HLEN + IP_HLEN;
}

/* Send the payload already placed at net_ip_payload(). */
int net_ip_send(u32 src, u32 dst, u8 proto, u16 payload_len)
{
    static u32 asked_ip, asked_at;
    u8 *ip = txbuf + ETH_HLEN;
    u8 mac[6];
    u32 hop;
    int err;

    if (!net.up)
        return NET_ERR_DOWN;
    if (payload_len > IP_MAX_DATA)
        return NET_ERR_TOO_BIG;
    if (is_broadcast(dst)) {
        memcpy(mac, mac_broadcast, 6);
    } else {
        err = next_hop(dst, &hop);
        if (err)
            return err;
        if (!arp_lookup(hop, mac)) {
            /* Ask, but not again on every retry while the answer is on its way. */
            if (hop != asked_ip || (u32)(ticks - asked_at) >= TICKS_PER_SEC / 2) {
                arp_send(1, mac_broadcast, mac_broadcast, hop);
                asked_ip = hop;
                asked_at = ticks;
            }
            return NET_ERR_ARP;
        }
    }
    memcpy(txbuf, mac, 6);
    memcpy(txbuf + 6, net.mac, 6);
    net_put16(txbuf + 12, ETHERTYPE_IP);

    ip[0] = 0x45;               /* version 4, 20-byte header */
    ip[1] = 0;
    net_put16(ip + 2, (u16)(IP_HLEN + payload_len));
    net_put16(ip + 4, ip_ident++);
    net_put16(ip + 6, 0x4000);      /* do not fragment */
    ip[8] = 64;                 /* time to live */
    ip[9] = proto;
    net_put16(ip + 10, 0);
    net_put32(ip + 12, src);
    net_put32(ip + 16, dst);
    net_put16(ip + 10, net_sum_finish(net_sum(ip, IP_HLEN, 0)));
    return frame_send(txbuf, (u16)(ETH_HLEN + IP_HLEN + payload_len));
}

/* --------------------------------------------------------------- ICMP -- */

static struct {
    u16 id, seq;
    u32 answered_at;
    volatile u8 answered;
} ping;

static void icmp_input(u32 src, const u8 *p, u16 len)
{
    u8 *out = txbuf + ETH_HLEN + IP_HLEN;

    if (len < 8 || net_sum_finish(net_sum(p, len, 0)) != 0) {
        net_stats.rx_bad++;
        return;
    }
    if (p[0] == 8 && net.ip) {              /* echo request: send it back */
        memcpy(out, p, len);
        out[0] = 0;
        net_put16(out + 2, 0);
        net_put16(out + 2, net_sum_finish(net_sum(out, len, 0)));
        net_ip_send(net.ip, src, PROTO_ICMP, len);
    } else if (p[0] == 0) {                 /* echo reply */
        if (!ping.answered && net_get16(p + 4) == ping.id && net_get16(p + 6) == ping.seq) {
            ping.answered_at = ticks;
            ping.answered = 1;
        }
    } else {
        net_stats.rx_ignored++;
    }
}

int net_ping(u32 ip, u16 seq, u32 timeout_ticks, u32 *rtt_ticks)
{
    enum { DATA = 32 };
    u8 *p = txbuf + ETH_HLEN + IP_HLEN;
    u32 sent_at;
    int err, i;

    err = net_route_ready(ip);
    if (err)
        return err;
    ping.id = (u16)(0x4A00 | net.mac[5]);
    ping.seq = seq;
    ping.answered = 0;

    p[0] = 8;
    p[1] = 0;
    net_put16(p + 2, 0);
    net_put16(p + 4, ping.id);
    net_put16(p + 6, seq);
    for (i = 0; i < DATA; i++)
        p[8 + i] = (u8)('a' + i % 26);
    net_put16(p + 2, net_sum_finish(net_sum(p, 8 + DATA, 0)));
    sent_at = ticks;
    err = net_ip_send(net.ip, ip, PROTO_ICMP, 8 + DATA);
    if (err)
        return err;
    err = net_wait_for(&ping.answered, timeout_ticks);
    if (err)
        return err;
    if (rtt_ticks)
        *rtt_ticks = ping.answered_at - sent_at;
    return 0;
}

/* ---------------------------------------------------------------- UDP -- */

int udp_bind(u16 port, udp_handler handler)
{
    int i;

    udp_unbind(port);
    for (i = 0; i < UDP_SOCKETS; i++)
        if (!udp_sockets[i].handler) {
            udp_sockets[i].port = port;
            udp_sockets[i].handler = handler;
            return 0;
        }
    return NET_ERR_TOO_BIG;
}

void udp_unbind(u16 port)
{
    int i;

    for (i = 0; i < UDP_SOCKETS; i++)
        if (udp_sockets[i].handler && udp_sockets[i].port == port)
            udp_sockets[i].handler = NULL;
}

u8 *udp_buffer(void)
{
    return txbuf + ETH_HLEN + IP_HLEN + UDP_HLEN;
}

u32 net_pseudo_sum(u32 src, u32 dst, u8 proto, u16 len)
{
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
}

static int udp_send_from(u32 src, u32 dst, u16 src_port, u16 dst_port, u16 len)
{
    u8 *u = txbuf + ETH_HLEN + IP_HLEN;
    u16 total = (u16)(UDP_HLEN + len), sum;

    if (len > UDP_MAX_DATA)
        return NET_ERR_TOO_BIG;
    net_put16(u, src_port);
    net_put16(u + 2, dst_port);
    net_put16(u + 4, total);
    net_put16(u + 6, 0);
    sum = net_sum_finish(net_sum(u, total, net_pseudo_sum(src, dst, PROTO_UDP, total)));
    net_put16(u + 6, sum ? sum : 0xFFFF);       /* 0 would mean "no checksum" */
    return net_ip_send(src, dst, PROTO_UDP, total);
}

int udp_send(u32 dst_ip, u16 src_port, u16 dst_port, u16 len)
{
    if (!net.ip)
        return NET_ERR_NO_ROUTE;
    return udp_send_from(net.ip, dst_ip, src_port, dst_port, len);
}

static void udp_input(u32 src, u32 dst, const u8 *u, u16 len)
{
    u16 total, port;
    int i;

    if (len < UDP_HLEN || (total = net_get16(u + 4)) < UDP_HLEN || total > len) {
        net_stats.rx_bad++;
        return;
    }
    if (net_get16(u + 6) != 0 &&
        net_sum_finish(net_sum(u, total, net_pseudo_sum(src, dst, PROTO_UDP, total))) != 0) {
        net_stats.rx_bad++;
        return;
    }
    port = net_get16(u + 2);
    for (i = 0; i < UDP_SOCKETS; i++)
        if (udp_sockets[i].handler && udp_sockets[i].port == port) {
            udp_sockets[i].handler(src, net_get16(u), u + UDP_HLEN, (u16)(total - UDP_HLEN));
            return;
        }
    net_stats.rx_ignored++;
}

/* -------------------------------------------------------------- input -- */

static void ip_input(const u8 *eth_src, const u8 *ip, u16 len)
{
    u16 hlen, total;
    u32 src, dst;

    if (len < IP_HLEN || (ip[0] >> 4) != 4) {
        net_stats.rx_bad++;
        return;
    }
    hlen = (u16)((ip[0] & 0x0F) * 4);
    total = net_get16(ip + 2);
    if (hlen < IP_HLEN || total < hlen || total > len ||
        net_sum_finish(net_sum(ip, hlen, 0)) != 0) {
        net_stats.rx_bad++;
        return;
    }
    if (net_get16(ip + 6) & 0x3FFF) {           /* a fragment: not supported */
        net_stats.rx_ignored++;
        return;
    }
    src = net_get32(ip + 12);
    dst = net_get32(ip + 16);
    /* Nothing genuine comes from "nobody", "everybody" or a group address,
     * and answering such a packet would be sent to all the network. */
    if (src == 0 || src >= 0xE0000000UL || is_broadcast(src)) {
        net_stats.rx_bad++;
        return;
    }
    /* With no address yet (DHCP still running) take anything sent to our
     * hardware address; otherwise only what is addressed to us. */
    if (net.ip && dst != net.ip && !is_broadcast(dst)) {
        net_stats.rx_ignored++;
        return;
    }
    /* A local sender just showed us its hardware address: no need to ask. */
    if (net.ip && is_local(src))
        arp_learn(src, eth_src);

    switch (ip[9]) {
    case PROTO_ICMP:
        icmp_input(src, ip + hlen, (u16)(total - hlen));
        break;
    case PROTO_UDP:
        udp_input(src, dst, ip + hlen, (u16)(total - hlen));
        break;
    case PROTO_TCP:
        if (net.ip && dst == net.ip)
            tcp_input(src, ip + hlen, (u16)(total - hlen));
        break;
    default:
        net_stats.rx_ignored++;
    }
}

void net_input(const u8 *frame, u16 len)
{
    if (!net.up)
        return;
    rng_stir(len);              /* when a frame arrives is hard to predict */
    net_stats.rx_frames++;
    if (len < ETH_HLEN) {
        net_stats.rx_bad++;
        return;
    }
    if (!same(frame, net.mac, 6) && !same(frame, mac_broadcast, 6)) {
        net_stats.rx_ignored++;
        return;
    }
    switch (net_get16(frame + 12)) {
    case ETHERTYPE_ARP:
        arp_input(frame + ETH_HLEN, (u16)(len - ETH_HLEN));
        break;
    case ETHERTYPE_IP:
        ip_input(frame + 6, frame + ETH_HLEN, (u16)(len - ETH_HLEN));
        break;
    default:
        net_stats.rx_ignored++;
    }
}

void net_poll(void)
{
    if (!net.up)
        return;
    /* TCP sends first, then frames are taken in. That way what TCP says
     * about data that arrived in the last poll (acknowledging it, and how
     * much room is left) is said after the program has had its turn to
     * take that data, in one segment instead of two. */
    tcp_poll();
    eth_poll();
}

void net_start(const u8 *mac)
{
    memset(&net, 0, sizeof net);
    memset(&net_stats, 0, sizeof net_stats);
    memset(arp_cache, 0, sizeof arp_cache);
    memset(udp_sockets, 0, sizeof udp_sockets);
    tcp_reset_all();
    memcpy(net.mac, mac, 6);
    ip_ident = (u16)(ticks ^ ((u16)mac[4] << 8) ^ mac[5]);
    net.up = 1;
}

void net_stop(void)
{
    tcp_reset_all();
    net.up = 0;
    net.ip = 0;
}

void net_set_static(u32 ip, u32 mask, u32 gateway, u32 dns)
{
    net.ip = ip;
    net.mask = mask;
    net.gateway = gateway;
    net.dns = dns;
    net.from_dhcp = 0;
    net.lease_secs = 0;
    memset(arp_cache, 0, sizeof arp_cache);
}

/* --------------------------------------------------------------- DHCP -- */

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_OPTIONS     240        /* fixed part plus the "magic cookie" */
#define DHCP_MIN_LEN     300        /* some servers drop anything shorter */

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

static const u8 dhcp_cookie[4] = { 0x63, 0x82, 0x53, 0x63 };

static struct {
    u32 xid;
    u32 ip, server, mask, gateway, dns, lease;
    u8 waiting_for;             /* message type we expect next */
    volatile u8 got;            /* set when it (or a refusal) arrived */
    u8 refused;
} dhcp;

static void dhcp_send(u8 type)
{
    u8 *b = udp_buffer(), *o;
    u16 len;

    memset(b, 0, DHCP_MIN_LEN);
    b[0] = 1;                   /* request */
    b[1] = 1;                   /* Ethernet */
    b[2] = 6;                   /* hardware address length */
    net_put32(b + 4, dhcp.xid);
    net_put16(b + 10, 0x8000);      /* answer by broadcast: we have no address yet */
    memcpy(b + 28, net.mac, 6);
    memcpy(b + 236, dhcp_cookie, 4);

    o = b + DHCP_OPTIONS;
    *o++ = 53; *o++ = 1; *o++ = type;                   /* message type */
    *o++ = 61; *o++ = 7; *o++ = 1;                      /* client identifier */
    memcpy(o, net.mac, 6);
    o += 6;
    if (type == DHCP_REQUEST) {
        *o++ = 50; *o++ = 4; net_put32(o, dhcp.ip); o += 4;         /* the address offered */
        *o++ = 54; *o++ = 4; net_put32(o, dhcp.server); o += 4;     /* by this server */
    }
    *o++ = 12; *o++ = 6; memcpy(o, "ssh-os", 6); o += 6;        /* host name */
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;   /* what we want back */
    *o++ = 255;
    len = (u16)(o - b);
    if (len < DHCP_MIN_LEN)
        len = DHCP_MIN_LEN;
    udp_send_from(0, 0xFFFFFFFFUL, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, len);
}

static void dhcp_input(u32 src_ip, u16 src_port, const u8 *b, u16 len)
{
    u32 mask = 0, gateway = 0, dns = 0, lease = 0, server = 0;
    u16 i = DHCP_OPTIONS;
    u8 type = 0, code, n;

    (void)src_ip;
    (void)src_port;
    if (dhcp.got || len < DHCP_OPTIONS || b[0] != 2 || net_get32(b + 4) != dhcp.xid ||
        !same(b + 28, net.mac, 6) || !same(b + 236, dhcp_cookie, 4))
        return;

    while (i < len) {
        code = b[i++];
        if (code == 0)
            continue;           /* padding */
        if (code == 255 || i >= len)
            break;
        n = b[i++];
        if (i + n > len)
            break;
        if (code == 53 && n >= 1)
            type = b[i];
        else if (code == 1 && n >= 4)
            mask = net_get32(b + i);
        else if (code == 3 && n >= 4)
            gateway = net_get32(b + i);
        else if (code == 6 && n >= 4)
            dns = net_get32(b + i);
        else if (code == 51 && n >= 4)
            lease = net_get32(b + i);
        else if (code == 54 && n >= 4)
            server = net_get32(b + i);
        i = (u16)(i + n);
    }

    if (type == DHCP_NAK && dhcp.waiting_for == DHCP_ACK) {
        dhcp.refused = 1;
        dhcp.got = 1;
        return;
    }
    if (type != dhcp.waiting_for || !net_get32(b + 16) || !server)
        return;
    dhcp.ip = net_get32(b + 16);    /* "your address" */
    dhcp.server = server;
    dhcp.mask = mask;
    dhcp.gateway = gateway;
    dhcp.dns = dns;
    dhcp.lease = lease;
    dhcp.got = 1;
}

/* The mask for an address whose server did not send one. */
static u32 default_mask(u32 ip)
{
    if ((ip >> 24) < 128)
        return 0xFF000000UL;
    if ((ip >> 24) < 192)
        return 0xFFFF0000UL;
    return 0xFFFFFF00UL;
}

/* How many times net_dhcp asks before giving up: waits of 2, 4, 8 and 8
 * seconds. Start-up asks less often, so a machine with no network is not
 * kept waiting. */
u8 net_dhcp_tries = 4;
static u8 asking;               /* asking on the side: 1 for an offer, 2 for its confirmation */
static u32 asked_at;

int net_dhcp(void)
{
    int attempt, err = NET_ERR_TIMEOUT;

    if (!net.up)
        return NET_ERR_DOWN;
    asking = 0;                 /* this takes over from asking on the side */
    net_set_static(0, 0, 0, 0);
    udp_bind(DHCP_CLIENT_PORT, dhcp_input);
    dhcp.xid = ((u32)net.mac[2] << 24) | ((u32)net.mac[3] << 16) |
               ((u32)net.mac[4] << 8) | net.mac[5];
    dhcp.xid ^= ticks << 8;

    for (attempt = 0; attempt < net_dhcp_tries; attempt++) {
        dhcp.xid++;
        dhcp.got = dhcp.refused = 0;
        dhcp.waiting_for = DHCP_OFFER;
        dhcp_send(DHCP_DISCOVER);
        err = net_wait_for(&dhcp.got, (u32)(2 * TICKS_PER_SEC) << (attempt < 2 ? attempt : 2));
        if (err == NET_ERR_ABORTED)
            break;
        if (err)
            continue;

        dhcp.got = 0;
        dhcp.waiting_for = DHCP_ACK;
        dhcp_send(DHCP_REQUEST);
        err = net_wait_for(&dhcp.got, 3 * TICKS_PER_SEC);
        if (err == NET_ERR_ABORTED)
            break;
        if (err || dhcp.refused) {
            err = NET_ERR_TIMEOUT;
            continue;
        }
        net_set_static(dhcp.ip, dhcp.mask ? dhcp.mask : default_mask(dhcp.ip),
                       dhcp.gateway, dhcp.dns);
        net.from_dhcp = 1;
        net.lease_secs = dhcp.lease;
        net.lease_start = ticks;
        err = 0;
        break;
    }
    udp_unbind(DHCP_CLIENT_PORT);
    return err;
}

/* The same without waiting, for a program that has other things to do (the
 * home screen, when start-up got no address): net_dhcp_begin() asks, and
 * net_dhcp_step(), called now and then from the program's loop along with
 * net_poll(), carries it on. It asks again every few seconds for as long as
 * nothing answers. */
void net_dhcp_begin(void)
{
    if (!net.up)
        return;
    net_set_static(0, 0, 0, 0);
    udp_bind(DHCP_CLIENT_PORT, dhcp_input);
    dhcp.xid++;
    dhcp.got = dhcp.refused = 0;
    dhcp.waiting_for = DHCP_OFFER;
    dhcp_send(DHCP_DISCOVER);
    asking = 1;
    asked_at = ticks;
}

/* Returns 1 when an address has just been set, 0 while still asking, -1 if
 * nothing is being asked. */
int net_dhcp_step(void)
{
    if (!asking)
        return -1;
    if (dhcp.got && asking == 1) {              /* an offer: take it */
        dhcp.got = 0;
        dhcp.waiting_for = DHCP_ACK;
        dhcp_send(DHCP_REQUEST);
        asking = 2;
        asked_at = ticks;
    } else if (dhcp.got && !dhcp.refused) {     /* confirmed */
        net_set_static(dhcp.ip, dhcp.mask ? dhcp.mask : default_mask(dhcp.ip),
                       dhcp.gateway, dhcp.dns);
        net.from_dhcp = 1;
        net.lease_secs = dhcp.lease;
        net.lease_start = ticks;
        udp_unbind(DHCP_CLIENT_PORT);
        asking = 0;
        return 1;
    } else if (dhcp.got || ticks - asked_at > (asking == 1 ? 5u : 3u) * TICKS_PER_SEC) {
        udp_unbind(DHCP_CLIENT_PORT);           /* refused or no answer: from the start */
        net_dhcp_begin();
    }
    return 0;
}

/* ---------------------------------------------------------------- DNS -- */

#define DNS_PORT        53
#define DNS_LOCAL_PORT  4053

static struct {
    u16 id;
    u32 ip;
    volatile u8 done;
    u8 found;
} dns;

/* Step over a (possibly compressed) name; returns the offset after it, or 0. */
static u16 dns_skip_name(const u8 *b, u16 len, u16 i)
{
    while (i < len) {
        u8 c = b[i];

        if (c == 0)
            return (u16)(i + 1);
        if ((c & 0xC0) == 0xC0)
            return (u16)(i + 2);
        i = (u16)(i + 1 + c);
    }
    return 0;
}

static void dns_input(u32 src_ip, u16 src_port, const u8 *b, u16 len)
{
    u16 i = 12, questions, answers, type, rdlen;

    (void)src_ip;
    (void)src_port;
    if (dns.done || len < 12 || net_get16(b) != dns.id || !(b[2] & 0x80))
        return;
    dns.done = 1;
    if (b[3] & 0x0F)            /* the server reports an error */
        return;
    questions = net_get16(b + 4);
    answers = net_get16(b + 6);
    while (questions--) {
        i = dns_skip_name(b, len, i);
        if (!i || i + 4 > len)
            return;
        i = (u16)(i + 4);
    }
    while (answers--) {
        i = dns_skip_name(b, len, i);
        if (!i || i + 10 > len)
            return;
        type = net_get16(b + i);
        rdlen = net_get16(b + i + 8);
        i = (u16)(i + 10);
        if (i + rdlen > len)
            return;
        if (type == 1 && rdlen == 4) {      /* an IPv4 address */
            dns.ip = net_get32(b + i);
            dns.found = 1;
            return;
        }
        i = (u16)(i + rdlen);
    }
}

int net_dns(const char *name, u32 *ip)
{
    u8 *b, *label;
    const char *s;
    int attempt, err = NET_ERR_TIMEOUT;
    u16 len;

    if (!net.up)
        return NET_ERR_DOWN;
    if (!net.dns)
        return NET_ERR_NO_DNS;
    if (strlen(name) > 253)
        return NET_ERR_NAME;
    udp_bind(DNS_LOCAL_PORT, dns_input);
    dns.id = (u16)(ticks * 31 + 0x4A4F);

    for (attempt = 0; attempt < 3; attempt++) {
        err = net_route_ready(net.dns);
        if (err)
            break;
        dns.id++;
        dns.done = dns.found = 0;

        b = udp_buffer();
        net_put16(b, dns.id);
        net_put16(b + 2, 0x0100);   /* a query; please look it up fully for us */
        net_put16(b + 4, 1);        /* one question */
        net_put16(b + 6, 0);
        net_put16(b + 8, 0);
        net_put16(b + 10, 0);
        /* The name as length-prefixed labels: 3 www 7 example 3 com 0 */
        label = b + 12;
        len = 13;
        *label = 0;
        for (s = name; *s; s++) {
            if (*s == '.') {
                label = b + len++;
                *label = 0;
            } else {
                if (*label == 63) {
                    err = NET_ERR_NAME;
                    goto out;
                }
                b[len++] = (u8)*s;
                (*label)++;
            }
        }
        if (*label)             /* no trailing dot: close the last label */
            b[len++] = 0;
        net_put16(b + len, 1);      /* type: IPv4 address */
        net_put16(b + len + 2, 1);  /* class: Internet */
        len = (u16)(len + 4);

        err = udp_send(net.dns, DNS_LOCAL_PORT, DNS_PORT, len);
        if (err && err != NET_ERR_ARP)
            break;
        err = net_wait_for(&dns.done, 3 * TICKS_PER_SEC);
        if (err == NET_ERR_ABORTED)
            break;
        if (err)
            continue;
        if (dns.found) {
            *ip = dns.ip;
            err = 0;
        } else {
            err = NET_ERR_NAME;
        }
        break;
    }
out:
    udp_unbind(DNS_LOCAL_PORT);
    return err;
}

/* ------------------------------------------------------------- naming -- */

int net_parse_ip(const char *s, u32 *ip)
{
    u32 v = 0, part;
    int parts, digits;

    for (parts = 0; parts < 4; parts++) {
        part = 0;
        for (digits = 0; *s >= '0' && *s <= '9'; s++, digits++) {
            part = part * 10 + (u32)(*s - '0');
            if (part > 255)
                return 0;
        }
        if (!digits)
            return 0;
        v = (v << 8) | part;
        if (parts < 3 && *s++ != '.')
            return 0;
    }
    if (*s)
        return 0;
    *ip = v;
    return 1;
}

int net_resolve(const char *name_or_address, u32 *ip)
{
    if (net_parse_ip(name_or_address, ip))
        return 0;
    return net_dns(name_or_address, ip);
}

const char *net_strerror(int err)
{
    switch (err) {
    case 0:                     return "no error";
    case NET_ERR_DOWN:          return "the network is not started (try: net up)";
    case NET_ERR_TIMEOUT:       return "no answer";
    case NET_ERR_ARP:           return "nobody on the local network answers for that address";
    case NET_ERR_SEND:          return "the adapter would not send";
    case NET_ERR_NO_ROUTE:      return "no address or no gateway configured";
    case NET_ERR_TOO_BIG:       return "too much data";
    case NET_ERR_NAME:          return "name not found";
    case NET_ERR_NO_DNS:        return "no name server configured";
    case NET_ERR_ABORTED:       return "stopped";
    case NET_ERR_NO_ADAPTER:    return "no network adapter on the SCSI bus";
    case NET_ERR_CLOSED:        return "connection closed";
    case NET_ERR_RESET:         return "connection reset by the other side";
    case NET_ERR_REFUSED:       return "connection refused";
    case NET_ERR_NO_SOCKET:     return "too many connections open";
    }
    return "unknown error";
}
