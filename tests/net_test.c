/* Test of the SSH OS network stack on the build machine.
 *
 * kernel/net.c is compiled as it is, for this computer instead of the
 * 68000, and connected to a pretend network implemented below: a gateway
 * that answers ARP and ping, a DHCP server and a name server. The emulator
 * has no DHCP server, so this is the only place that code runs before it
 * meets a real network. It also lets the awkward cases happen on demand:
 * lost packets, silence, refusals and damaged frames.
 *
 * Checksums of everything the stack sends are verified with a separate
 * implementation, written differently from the one in net.c.
 *
 *   make test-net-host
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SSHOS_TYPES_H          /* use this machine's exact-width types instead */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

#include "../kernel/net.c"
#include "../kernel/tcp.c"

/* ------------------------------------------------- kernel stand-ins -- */

volatile u32 ticks;

void rng_stir(u32 value)
{
    (void)value;
}

void timeout_start(struct timeout *t, u32 nticks)
{
    t->start = ticks;
    t->limit = nticks + 1;
    t->spins = 0xFFFFFFFF;
}

int timeout_expired(struct timeout *t)
{
    return (u32)(ticks - t->start) >= t->limit;
}

/* ------------------------------------------------ the pretend network -- */

#define MY_MAC      "\x00\x80\x19\x12\x34\x56"
#define GW_MAC      "\x02\x00\x00\x00\x00\x01"
#define HOST_MAC    "\x02\x00\x00\x00\x00\x63"
#define GW_IP       IP4(192, 168, 1, 1)
#define HOST_IP     IP4(192, 168, 1, 99)        /* another machine on the LAN */
#define LEASE_IP    IP4(192, 168, 1, 50)
#define NETMASK     IP4(255, 255, 255, 0)
#define DNS_IP      IP4(9, 9, 9, 9)             /* reached through the gateway */
#define FAR_IP      IP4(93, 184, 216, 34)
#define WWW_IP      IP4(93, 184, 216, 34)

static struct {
    int dhcp_server;            /* 0 = no DHCP server on this network */
    int drop_discovers;         /* lose this many DISCOVERs first */
    int dhcp_nak;               /* refuse the REQUEST this many times */
    int dhcp_no_mask;           /* leave the mask out of the answers */
    int dhcp_second_server;     /* a late offer from another server arrives before the ACK */
    int arp_silent;             /* nobody answers ARP */
    int ping_silent;
    int dns_drop;               /* lose this many name queries first */
    int corrupt_replies;        /* damage the checksum of UDP answers */
} sim;

static struct {
    int frames, arp_requests, discovers, requests, dns_queries, pings, echo_replies;
    int bad_checksums;
    u8 last_dst_mac[6];
} seen;

#define QUEUE 16
static u8 queue[QUEUE][1600];
static u16 queue_len[QUEUE];
static int queue_head, queue_tail;

static int failures;

static void peer_rx(const u8 *ip, unsigned total);
static void peer_pump(void);

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("  FAILED line %d: %s\n", __LINE__, #cond); \
            failures++; \
        } \
    } while (0)

static void w16(u8 *p, unsigned v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void w32(u8 *p, u32 v) { w16(p, v >> 16); w16(p + 2, v & 0xFFFF); }
static unsigned r16(const u8 *p) { return ((unsigned)p[0] << 8) | p[1]; }
static u32 r32(const u8 *p) { return ((u32)r16(p) << 16) | r16(p + 2); }

/* Independent Internet checksum: 0 for a correctly checksummed block. */
static unsigned checksum(const u8 *p, size_t len, uint64_t sum)
{
    size_t i;

    for (i = 0; i < len; i++)
        sum += (i & 1) ? p[i] : (uint64_t)p[i] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned)(~sum & 0xFFFF);
}

static uint64_t pseudo(u32 src, u32 dst, unsigned proto, unsigned len)
{
    return (uint64_t)(src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
}

static void deliver(const u8 *frame, u16 len)
{
    int next = (queue_tail + 1) % QUEUE;

    if (next == queue_head) {
        printf("  simulator queue overflow\n");
        failures++;
        return;
    }
    memcpy(queue[queue_tail], frame, len);
    queue_len[queue_tail] = len;
    queue_tail = next;
}

/* Build Ethernet + IP headers around a payload already at out + 34. */
static u16 wrap_ip(u8 *out, const char *dst_mac, const char *src_mac, u32 src, u32 dst,
                   unsigned proto, unsigned payload_len)
{
    memcpy(out, dst_mac, 6);
    memcpy(out + 6, src_mac, 6);
    w16(out + 12, 0x0800);
    memset(out + 14, 0, 20);
    out[14] = 0x45;
    w16(out + 16, 20 + payload_len);
    out[22] = 64;
    out[23] = (u8)proto;
    w32(out + 26, src);
    w32(out + 30, dst);
    w16(out + 24, checksum(out + 14, 20, 0));
    return (u16)(34 + payload_len);
}

static u16 wrap_udp(u8 *out, const char *dst_mac, const char *src_mac, u32 src, u32 dst,
                    unsigned sport, unsigned dport, unsigned data_len)
{
    u8 *u = out + 34;
    unsigned sum;

    w16(u, sport);
    w16(u + 2, dport);
    w16(u + 4, 8 + data_len);
    w16(u + 6, 0);
    sum = checksum(u, 8 + data_len, pseudo(src, dst, 17, 8 + data_len));
    if (sim.corrupt_replies)
        sum ^= 0x0101;
    w16(u + 6, sum ? sum : 0xFFFF);
    return wrap_ip(out, dst_mac, src_mac, src, dst, 17, 8 + data_len);
}

static void sim_arp(const u8 *f)
{
    u8 out[60] = { 0 };
    u32 target = r32(f + 38);
    const char *mac;

    seen.arp_requests++;
    if (sim.arp_silent)
        return;
    if (target == GW_IP)
        mac = GW_MAC;
    else if (target == HOST_IP)
        mac = HOST_MAC;
    else
        return;
    memcpy(out, f + 6, 6);
    memcpy(out + 6, mac, 6);
    w16(out + 12, 0x0806);
    w16(out + 14, 1);
    w16(out + 16, 0x0800);
    out[18] = 6;
    out[19] = 4;
    w16(out + 20, 2);
    memcpy(out + 22, mac, 6);
    w32(out + 28, target);
    memcpy(out + 32, f + 22, 6);
    w32(out + 38, r32(f + 28));
    deliver(out, 60);
}

static const u8 *dhcp_option(const u8 *b, unsigned len, unsigned want)
{
    unsigned i = 240;

    while (i + 1 < len && b[i] != 255) {
        if (b[i] == 0) {
            i++;
            continue;
        }
        if (b[i] == want)
            return b + i + 2;
        i += 2 + b[i + 1];
    }
    return NULL;
}

/* Send DHCP message `reply` in answer to request b. */
static void dhcp_reply(const u8 *b, unsigned reply, u32 address, u32 server)
{
    u8 out[700] = { 0 }, *r = out + 42, *o;

    r[0] = 2;
    r[1] = 1;
    r[2] = 6;
    memcpy(r + 4, b + 4, 4);                    /* same transaction number */
    w16(r + 10, 0x8000);
    if (reply != 6)
        w32(r + 16, address);
    memcpy(r + 28, b + 28, 6);
    memcpy(r + 236, "\x63\x82\x53\x63", 4);
    o = r + 240;
    *o++ = 53; *o++ = 1; *o++ = (u8)reply;
    *o++ = 54; *o++ = 4; w32(o, server); o += 4;
    if (reply != 6) {
        *o++ = 0;                               /* padding in the middle, which is legal */
        *o++ = 51; *o++ = 4; w32(o, 86400); o += 4;
        if (!sim.dhcp_no_mask) {
            *o++ = 1; *o++ = 4; w32(o, NETMASK); o += 4;
        }
        *o++ = 3; *o++ = 8; w32(o, GW_IP); o += 4; w32(o, IP4(192, 168, 1, 2)); o += 4;
        *o++ = 6; *o++ = 8; w32(o, DNS_IP); o += 4; w32(o, IP4(8, 8, 8, 8)); o += 4;
        *o++ = 15; *o++ = 3; memcpy(o, "lan", 3); o += 3;   /* an option we did not ask for */
    }
    *o++ = 255;
    deliver(out, wrap_udp(out, "\xff\xff\xff\xff\xff\xff", GW_MAC, server, 0xFFFFFFFF, 67, 68,
                          (unsigned)(o - r)));
}

static void sim_dhcp(const u8 *f, const u8 *b, unsigned len)
{
    const u8 *type = dhcp_option(b, len, 53), *opt;
    unsigned reply;

    CHECK(len >= 300);
    CHECK(b[0] == 1 && b[1] == 1 && b[2] == 6);
    CHECK(memcmp(b + 28, MY_MAC, 6) == 0);
    CHECK(memcmp(b + 236, "\x63\x82\x53\x63", 4) == 0);
    CHECK(r16(b + 10) == 0x8000);
    CHECK(r32(f + 26) == 0 && r32(f + 30) == 0xFFFFFFFF);
    CHECK(memcmp(f, "\xff\xff\xff\xff\xff\xff", 6) == 0);
    CHECK(type != NULL);
    if (!type)
        return;

    if (*type == 1) {
        seen.discovers++;
        if (!sim.dhcp_server)
            return;
        if (sim.drop_discovers > 0) {
            sim.drop_discovers--;
            return;
        }
        reply = 2;
    } else if (*type == 3) {
        seen.requests++;
        opt = dhcp_option(b, len, 50);
        CHECK(opt && r32(opt) == LEASE_IP);
        opt = dhcp_option(b, len, 54);
        CHECK(opt && r32(opt) == GW_IP);
        if (!sim.dhcp_server)
            return;
        reply = 5;
        if (sim.dhcp_nak > 0) {
            sim.dhcp_nak--;
            reply = 6;
        }
    } else {
        CHECK(!"unexpected DHCP message type");
        return;
    }

    if (reply == 5 && sim.dhcp_second_server)
        dhcp_reply(b, 2, IP4(192, 168, 1, 200), IP4(192, 168, 1, 2));
    dhcp_reply(b, reply, LEASE_IP, GW_IP);
}

static void sim_dns(const u8 *f, const u8 *q, unsigned len)
{
    static const char www[] = "\x03www\x07" "example\x03" "com";    /* then a 0 byte */
    u8 out[400] = { 0 }, *r = out + 42, *o;
    unsigned qlen;
    int known;

    seen.dns_queries++;
    CHECK(memcmp(f, GW_MAC, 6) == 0);           /* off the LAN, so via the gateway */
    CHECK(r16(q + 2) == 0x0100 && r16(q + 4) == 1);
    if (sim.dns_drop > 0) {
        sim.dns_drop--;
        return;
    }
    qlen = len - 12;
    known = qlen == sizeof www + 4 && memcmp(q + 12, www, sizeof www) == 0;
    CHECK(r16(q + len - 4) == 1 && r16(q + len - 2) == 1);

    memcpy(r, q, len);                          /* id and question come back */
    w16(r + 2, known ? 0x8180 : 0x8183);        /* answer; 3 = no such name */
    o = r + len;
    if (known) {
        w16(r + 6, 2);
        /* First an alias record, to be skipped: www -> web.example.com */
        *o++ = 0xC0; *o++ = 12;                 /* name: pointer to the question */
        w16(o, 5); w16(o + 2, 1); w32(o + 4, 300); w16(o + 8, 6); o += 10;
        *o++ = 3; memcpy(o, "web", 3); o += 3; *o++ = 0xC0; *o++ = 16;
        /* Then the address. */
        *o++ = 0xC0; *o++ = 12;
        w16(o, 1); w16(o + 2, 1); w32(o + 4, 300); w16(o + 8, 4); o += 10;
        w32(o, WWW_IP); o += 4;
    }
    deliver(out, wrap_udp(out, MY_MAC, GW_MAC, DNS_IP, r32(f + 26), 53, r16(f + 34),
                          (unsigned)(o - r)));
}

static void sim_icmp(const u8 *f, const u8 *p, unsigned len)
{
    u8 out[1600];
    u32 dst = r32(f + 30);

    CHECK(checksum(p, len, 0) == 0);
    if (p[0] == 0) {
        seen.echo_replies++;
        return;
    }
    CHECK(p[0] == 8);
    seen.pings++;
    if (sim.ping_silent)
        return;
    memcpy(out + 34, p, len);
    out[34] = 0;
    w16(out + 36, 0);
    w16(out + 36, checksum(out + 34, len, 0));
    deliver(out, wrap_ip(out, MY_MAC, dst == HOST_IP ? HOST_MAC : GW_MAC, dst, r32(f + 26), 1, len));
}

int eth_send(const u8 *f, u16 len)
{
    const u8 *ip = f + 14;
    unsigned total, proto;

    seen.frames++;
    CHECK(len >= 60 && len <= 1514);
    CHECK(memcmp(f + 6, MY_MAC, 6) == 0);
    memcpy(seen.last_dst_mac, f, 6);

    if (r16(f + 12) == 0x0806) {
        if (r16(f + 20) == 1)
            sim_arp(f);
        return 0;
    }
    CHECK(r16(f + 12) == 0x0800);
    CHECK(ip[0] == 0x45);
    total = r16(ip + 2);
    proto = ip[9];
    CHECK(total + 14 <= len);
    if (checksum(ip, 20, 0) != 0) {
        seen.bad_checksums++;
        CHECK(!"bad IP header checksum");
    }
    if (proto == 17) {
        const u8 *u = ip + 20;
        unsigned ulen = r16(u + 4);

        CHECK(ulen == total - 20);
        CHECK(r16(u + 6) != 0);
        if (checksum(u, ulen, pseudo(r32(ip + 12), r32(ip + 16), 17, ulen)) != 0) {
            seen.bad_checksums++;
            CHECK(!"bad UDP checksum");
        }
        if (r16(u + 2) == 67)
            sim_dhcp(f, u + 8, ulen - 8);
        else if (r16(u + 2) == 53)
            sim_dns(f, u + 8, ulen - 8);
    } else if (proto == 1) {
        sim_icmp(f, ip + 20, total - 20);
    } else if (proto == 6) {
        peer_rx(ip, total);
    }
    return 0;
}

/* Each poll is one tick of pretend time, and hands over what has arrived. */
int eth_poll(void)
{
    int n = 0;

    ticks++;
    peer_pump();
    while (queue_head != queue_tail) {
        int i = queue_head;

        queue_head = (queue_head + 1) % QUEUE;
        net_input(queue[i], queue_len[i]);
        n++;
    }
    return n;
}

static void reset(void)
{
    memset(&sim, 0, sizeof sim);
    memset(&seen, 0, sizeof seen);
    queue_head = queue_tail = 0;
    sim.dhcp_server = 1;
    net_start((const u8 *)MY_MAC);
}

static void bring_up(void)
{
    reset();
    CHECK(net_dhcp() == 0);
    memset(&seen, 0, sizeof seen);
}

/* -------------------------------------------------------------- tests -- */

static void test_parse(void)
{
    u32 ip = 0;

    printf("address parsing\n");
    CHECK(net_parse_ip("192.168.1.20", &ip) && ip == 0xC0A80114);
    CHECK(net_parse_ip("0.0.0.0", &ip) && ip == 0);
    CHECK(net_parse_ip("255.255.255.255", &ip) && ip == 0xFFFFFFFF);
    CHECK(!net_parse_ip("256.1.1.1", &ip));
    CHECK(!net_parse_ip("1.2.3", &ip));
    CHECK(!net_parse_ip("1.2.3.4.5", &ip));
    CHECK(!net_parse_ip("1..3.4", &ip));
    CHECK(!net_parse_ip("example.com", &ip));
    CHECK(!net_parse_ip("", &ip));
    CHECK(!net_parse_ip("1.2.3.4 ", &ip));
}

static void test_dhcp(void)
{
    u32 before;
    int i, got;

    printf("DHCP: normal\n");
    reset();
    CHECK(net_dhcp() == 0);
    CHECK(net.ip == LEASE_IP && net.mask == NETMASK && net.gateway == GW_IP);
    CHECK(net.dns == DNS_IP && net.lease_secs == 86400 && net.from_dhcp);
    CHECK(seen.discovers == 1 && seen.requests == 1);

    printf("DHCP: first two requests lost\n");
    reset();
    sim.drop_discovers = 2;
    CHECK(net_dhcp() == 0);
    CHECK(net.ip == LEASE_IP && seen.discovers == 3);

    printf("DHCP: server refuses once\n");
    reset();
    sim.dhcp_nak = 1;
    CHECK(net_dhcp() == 0);
    CHECK(net.ip == LEASE_IP && seen.requests == 2);

    printf("DHCP: no mask in the answer\n");
    reset();
    sim.dhcp_no_mask = 1;
    CHECK(net_dhcp() == 0);
    CHECK(net.mask == NETMASK);                 /* the default for 192.x.x.x */

    printf("DHCP: a second server's late offer is not mistaken for the confirmation\n");
    reset();
    sim.dhcp_second_server = 1;
    CHECK(net_dhcp() == 0);
    CHECK(net.ip == LEASE_IP);

    printf("DHCP: no server\n");
    reset();
    sim.dhcp_server = 0;
    before = ticks;
    CHECK(net_dhcp() == NET_ERR_TIMEOUT);
    CHECK(net.ip == 0 && seen.discovers == 4);
    CHECK(ticks - before > 15 * 60 && ticks - before < 40 * 60);

    printf("DHCP: start-up asks once and gives up; asking on the side gets it later\n");
    reset();
    sim.dhcp_server = 0;                        /* the WiFi is not up yet */
    net_dhcp_tries = 1;
    before = ticks;
    CHECK(net_dhcp() == NET_ERR_TIMEOUT);
    net_dhcp_tries = 4;
    CHECK(ticks - before < 3 * 60 && net.ip == 0);
    CHECK(net_dhcp_step() == -1);               /* nothing asked yet */
    net_dhcp_begin();
    for (i = 0; i < 20 * 60 && net.ip == 0; i++) {
        if (i == 12 * 60)
            sim.dhcp_server = 1;                /* now it is */
        net_poll();
        got = net_dhcp_step();
        CHECK(got == 0 || (got == 1 && net.ip == LEASE_IP));
    }
    CHECK(net.ip == LEASE_IP && net.mask == NETMASK && net.from_dhcp);
    CHECK(net_dhcp_step() == -1);               /* and it has stopped asking */

    printf("DHCP: asking on the side, refused once\n");
    reset();
    sim.dhcp_nak = 1;
    net_dhcp_begin();
    for (i = 0; i < 20 * 60 && net.ip == 0; i++) {
        net_poll();
        net_dhcp_step();
    }
    CHECK(net.ip == LEASE_IP && seen.requests == 2);

    printf("DHCP: damaged answers\n");
    reset();
    sim.corrupt_replies = 1;
    CHECK(net_dhcp() == NET_ERR_TIMEOUT);
    CHECK(net.ip == 0 && net_stats.rx_bad >= 4);
}

static void test_ping(void)
{
    u32 rtt = 99;

    printf("ping: gateway\n");
    bring_up();
    CHECK(net_ping(GW_IP, 1, 120, &rtt) == 0);
    CHECK(rtt <= 2 && seen.arp_requests == 1 && seen.pings == 1);
    CHECK(net_ping(GW_IP, 2, 120, &rtt) == 0);
    CHECK(seen.arp_requests == 1);              /* remembered from the first time */

    printf("ping: another machine on the LAN goes direct\n");
    CHECK(net_ping(HOST_IP, 1, 120, &rtt) == 0);
    CHECK(memcmp(seen.last_dst_mac, HOST_MAC, 6) == 0);

    printf("ping: far address goes through the gateway\n");
    CHECK(net_ping(FAR_IP, 1, 120, &rtt) == 0);
    CHECK(memcmp(seen.last_dst_mac, GW_MAC, 6) == 0);

    printf("ping: no answer\n");
    sim.ping_silent = 1;
    CHECK(net_ping(GW_IP, 3, 120, &rtt) == NET_ERR_TIMEOUT);

    printf("ping: nobody has that address\n");
    bring_up();
    CHECK(net_ping(IP4(192, 168, 1, 77), 1, 120, &rtt) == NET_ERR_ARP);
    CHECK(seen.arp_requests == 4 && seen.pings == 0);

    printf("ping: before the network is up\n");
    net_stop();
    CHECK(net_ping(GW_IP, 1, 120, &rtt) == NET_ERR_DOWN);
    reset();
    CHECK(net_ping(GW_IP, 1, 120, &rtt) == NET_ERR_NO_ROUTE);
}

static void test_incoming(void)
{
    u8 f[1600];
    u16 len;
    int i;

    printf("incoming: we answer ping and ARP\n");
    bring_up();
    for (i = 0; i < 49; i++)                    /* odd length on purpose */
        f[34 + 8 + i] = (u8)(i * 3);
    f[34] = 8;
    f[35] = 0;
    w16(f + 36, 0);
    w16(f + 38, 0x1234);
    w16(f + 40, 7);
    w16(f + 36, checksum(f + 34, 8 + 49, 0));
    len = wrap_ip(f, MY_MAC, HOST_MAC, HOST_IP, LEASE_IP, 1, 8 + 49);
    net_input(f, len);
    CHECK(seen.echo_replies == 1);
    CHECK(seen.arp_requests == 0);              /* it learned the sender's address */
    CHECK(memcmp(seen.last_dst_mac, HOST_MAC, 6) == 0);

    memset(f, 0, 60);                           /* "who has LEASE_IP?" from the gateway */
    memset(f, 0xFF, 6);
    memcpy(f + 6, GW_MAC, 6);
    w16(f + 12, 0x0806);
    w16(f + 14, 1);
    w16(f + 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    w16(f + 20, 1);
    memcpy(f + 22, GW_MAC, 6);
    w32(f + 28, GW_IP);
    w32(f + 38, LEASE_IP);
    seen.frames = 0;
    net_input(f, 60);
    CHECK(seen.frames == 1 && memcmp(seen.last_dst_mac, GW_MAC, 6) == 0);
    w32(f + 38, HOST_IP);                       /* a question about somebody else */
    net_input(f, 60);
    CHECK(seen.frames == 1);

    printf("incoming: frames for other machines are ignored\n");
    f[34] = 8;
    w16(f + 36, 0);
    w16(f + 36, checksum(f + 34, 8, 0));
    len = wrap_ip(f, HOST_MAC, GW_MAC, GW_IP, HOST_IP, 1, 8);
    seen.echo_replies = 0;
    net_input(f, len);
    len = wrap_ip(f, MY_MAC, GW_MAC, GW_IP, HOST_IP, 1, 8);     /* our MAC, wrong IP */
    net_input(f, len);
    CHECK(seen.echo_replies == 0);
}

static void test_dns(void)
{
    u32 ip = 0;

    printf("DNS: lookup with an alias in the answer\n");
    bring_up();
    CHECK(net_dns("www.example.com", &ip) == 0 && ip == WWW_IP);
    CHECK(seen.dns_queries == 1 && seen.arp_requests == 1);
    ip = 0;
    CHECK(net_dns("www.example.com.", &ip) == 0 && ip == WWW_IP);

    printf("DNS: unknown name\n");
    CHECK(net_dns("nothing.example", &ip) == NET_ERR_NAME);

    printf("DNS: first query lost\n");
    sim.dns_drop = 1;
    seen.dns_queries = 0;
    CHECK(net_dns("www.example.com", &ip) == 0 && seen.dns_queries == 2);

    printf("DNS: no answer at all\n");
    sim.dns_drop = 99;
    CHECK(net_dns("www.example.com", &ip) == NET_ERR_TIMEOUT);

    printf("DNS: resolve takes addresses too\n");
    seen.dns_queries = 0;
    CHECK(net_resolve("10.1.2.3", &ip) == 0 && ip == IP4(10, 1, 2, 3) && seen.dns_queries == 0);

    printf("DNS: no name server configured\n");
    net_set_static(LEASE_IP, NETMASK, GW_IP, 0);
    CHECK(net_dns("www.example.com", &ip) == NET_ERR_NO_DNS);
}

static int aborts_left;

static int abort_soon(void)
{
    return --aborts_left <= 0;
}

static void test_abort(void)
{
    u32 before, rtt;

    printf("abort: Esc stops a wait\n");
    bring_up();
    sim.ping_silent = 1;
    aborts_left = 5;
    net_abort_check = abort_soon;
    before = ticks;
    CHECK(net_ping(GW_IP, 1, 600, &rtt) == NET_ERR_ABORTED);
    CHECK(ticks - before < 20);
    net_abort_check = NULL;
}

/* Throw damaged and random frames at the stack. Nothing to compare: the
 * point is that the address and undefined-behaviour checkers stay quiet. */
static void test_garbage(void)
{
    u8 f[1600], good[1600];
    u16 len, good_len;
    int i, j;

    printf("garbage: 200000 damaged frames\n");
    bring_up();
    udp_bind(68, dhcp_input);
    udp_bind(DNS_LOCAL_PORT, dns_input);
    dhcp.waiting_for = DHCP_OFFER;
    srand(1);
    for (i = 0; i < 200000; i++) {
        /* Start from something well formed so the damage reaches deep code. */
        memset(good, 0, sizeof good);
        switch (i % 4) {
        case 0:
            good[34] = (u8)(rand() % 2 ? 8 : 0);
            w16(good + 36, checksum(good + 34, 40, 0));
            good_len = wrap_ip(good, MY_MAC, GW_MAC, GW_IP, LEASE_IP, 1, 40);
            break;
        case 1:
            good[42] = 2;
            w32(good + 46, dhcp.xid);
            memcpy(good + 70, MY_MAC, 6);
            memcpy(good + 278, "\x63\x82\x53\x63", 4);
            for (j = 282; j < 342; j++)
                good[j] = (u8)rand();
            good_len = wrap_udp(good, MY_MAC, GW_MAC, GW_IP, LEASE_IP, 67, 68, 300);
            break;
        case 2:
            w16(good + 42, dns.id);
            good[44] = 0x81;
            good[45] = 0x80;
            w16(good + 46, rand() % 3);
            w16(good + 48, rand() % 4);
            for (j = 54; j < 120; j++)
                good[j] = (u8)rand();
            dns.done = 0;
            good_len = wrap_udp(good, MY_MAC, GW_MAC, DNS_IP, LEASE_IP, 53, DNS_LOCAL_PORT,
                                (unsigned)(12 + rand() % 80));
            break;
        default:
            memset(good, 0xFF, 6);
            memcpy(good + 6, GW_MAC, 6);
            w16(good + 12, 0x0806);
            w16(good + 14, 1);
            w16(good + 16, 0x0800);
            good[18] = 6;
            good[19] = 4;
            w16(good + 20, 1 + rand() % 2);
            w32(good + 28, GW_IP);
            w32(good + 38, LEASE_IP);
            good_len = 60;
        }
        memcpy(f, good, sizeof f);
        len = good_len;
        for (j = rand() % 4; j > 0; j--)
            f[rand() % (len + 8)] = (u8)rand();
        if (rand() % 3 == 0)
            len = (u16)(rand() % (len + 1));    /* cut short */
        dhcp.got = 0;
        /* Hand over an exact-size copy so reading past the end is caught. */
        {
            u8 *exact = malloc(len ? len : 1);

            memcpy(exact, f, len);
            net_input(exact, len);
            free(exact);
        }
        queue_head = queue_tail = 0;
    }
    CHECK(seen.bad_checksums == 0);
}

/* ------------------------------------------------------ the TCP peer -- */

/* A small TCP server living at SRV_IP:SRV_PORT, beyond the gateway. It can
 * send a prepared stream, collect what it is sent, echo, and misbehave on
 * request. Its sequence numbers start just below the 32-bit wrap-around,
 * so every longer transfer crosses it. */
#define SRV_IP      FAR_IP
#define SRV_PORT    7000
#define SRV_ISS     0xFFFFE000u

static struct peer {
    int state;                  /* 0 none, 1 SYN seen, 2 connected */
    int silent, refuse, drop_synack;
    int echo, close_after_send, reset_after;    /* reset_after: bytes sent before an RST */
    /* Faults, each hitting one segment in N at random. A strict "every Nth"
     * can fall into step with the retransmissions and hit the same segment
     * for ever, which no real network does. */
    int drop_rx_every, drop_tx_every, corrupt_every, dup_every, reorder_every;
    int deaf;                   /* stop reacting to anything (as if unplugged) */
    int overlap;                /* when sending again, start 300 bytes too early */
    int forgiving;              /* forged segments are about: do not judge the client by them */
    unsigned window;            /* what we advertise */
    int reopen_at;              /* tick at which a zero window opens again, silently */
    unsigned cport, client_window;
    u32 rcv_nxt, snd_una, snd_nxt;
    u32 last_progress;
    const u8 *txdata;
    u32 txlen, rxlen;
    int fin_sent, fin_acked, fin_rcvd, ack_due;
    u32 fin_seq;                /* where the client's FIN sits; it may never move */
    int fins;
    int syns, got_rst, strays, probes, out_of_order, tx_count, rx_count, resent;
    u8 held[1600];
    u16 held_len;
    u8 rxdata[600000];
} peer;

static int seq_gt(u32 a, u32 b) { return (int32_t)(a - b) > 0; }

static void peer_send(unsigned flags, u32 seq, const u8 *data, unsigned n)
{
    u8 out[1600], *t = out + 34;
    unsigned hlen = (flags & 0x02) ? 24 : 20, sum;
    u16 len;

    memset(t, 0, hlen);
    w16(t, SRV_PORT);
    w16(t + 2, peer.cport);
    w32(t + 4, seq);
    w32(t + 8, peer.rcv_nxt);
    t[12] = (u8)((hlen / 4) << 4);
    t[13] = (u8)flags;
    w16(t + 14, peer.window);
    if (flags & 0x02) {
        t[20] = 1;                              /* a no-op option first, to be skipped */
        t[21] = 1;
        t[22] = 2;
        t[23] = 4;                              /* runs past the header: must be ignored */
        hlen = 28;
        memset(t + 20, 0, 8);
        t[12] = (u8)((hlen / 4) << 4);
        t[20] = 1; t[21] = 1; t[22] = 1; t[23] = 1;
        t[24] = 2; t[25] = 4; w16(t + 26, 1400);    /* the peer's largest segment */
    }
    if (n)
        memcpy(t + hlen, data, n);
    sum = checksum(t, hlen + n, pseudo(SRV_IP, LEASE_IP, 6, hlen + n));
    peer.tx_count++;
    if (peer.corrupt_every && rand() % peer.corrupt_every == 0)
        sum ^= 0x5555;
    w16(t + 16, sum);
    len = wrap_ip(out, MY_MAC, GW_MAC, SRV_IP, LEASE_IP, 6, hlen + n);
    if (peer.drop_tx_every && rand() % peer.drop_tx_every == 0)
        return;
    if (peer.reorder_every && n && !peer.held_len && rand() % peer.reorder_every == 0) {
        memcpy(peer.held, out, len);            /* let the next one overtake it */
        peer.held_len = len;
        return;
    }
    deliver(out, len);
    if (peer.dup_every && rand() % peer.dup_every == 0)
        deliver(out, len);
    if (peer.held_len) {
        deliver(peer.held, peer.held_len);
        peer.held_len = 0;
    }
}

static void peer_rx(const u8 *ip, unsigned total)
{
    const u8 *t = ip + 20, *data;
    unsigned tlen = total - 20, hlen, n, flags;
    u32 seq, ack;

    CHECK(checksum(t, tlen, pseudo(r32(ip + 12), r32(ip + 16), 6, tlen)) == 0);
    CHECK(r32(ip + 16) == SRV_IP);
    flags = t[13];
    seq = r32(t + 4);
    ack = r32(t + 8);
    hlen = (unsigned)(t[12] >> 4) * 4;
    data = t + hlen;
    n = tlen - hlen;
    peer.rx_count++;
    if (peer.deaf)
        return;
    if (flags & 0x04) {
        peer.got_rst++;
        peer.state = 0;
        return;
    }
    if (r16(t + 2) != SRV_PORT || peer.refuse) {    /* nothing listens there */
        if (flags & 0x02) {
            peer.syns++;
            peer.cport = r16(t);
            peer.rcv_nxt = seq + 1;
            peer_send(0x14, 0, NULL, 0);
        }
        return;
    }
    if (peer.drop_rx_every && rand() % peer.drop_rx_every == 0)
        return;
    if (flags & 0x02) {
        peer.syns++;
        CHECK(hlen == 24 && t[20] == 2 && t[21] == 4 && r16(t + 22) == 1460);
        CHECK(!(flags & 0x10));
        if (peer.silent)
            return;
        peer.state = 1;
        peer.cport = r16(t);
        peer.rcv_nxt = seq + 1;
        peer.snd_una = SRV_ISS;
        peer.snd_nxt = SRV_ISS + 1;
        peer.client_window = r16(t + 14);
        if (peer.drop_synack > 0) {
            peer.drop_synack--;
            return;
        }
        peer_send(0x12, SRV_ISS, NULL, 0);
        return;
    }
    if (peer.state == 0 || r16(t) != peer.cport) {
        peer.strays++;
        return;
    }
    if (flags & 0x10) {
        if (peer.state == 1 && ack == SRV_ISS + 1) {
            peer.state = 2;
            peer.last_progress = ticks;
        }
        if (seq_gt(ack, peer.snd_una) && !seq_gt(ack, peer.snd_nxt)) {
            peer.snd_una = ack;
            peer.last_progress = ticks;
            if (peer.fin_sent && ack == SRV_ISS + 1 + peer.txlen + 1)
                peer.fin_acked = 1;
        }
        CHECK(peer.forgiving || !seq_gt(ack, SRV_ISS + 1 + peer.txlen + 1));
        peer.client_window = r16(t + 14);
    }
    if (flags & 0x01) {
        if (peer.fins++ == 0)
            peer.fin_seq = seq + n;
        CHECK(peer.forgiving || seq + n == peer.fin_seq);
    }
    if (n || (flags & 0x01)) {
        if (seq == peer.rcv_nxt) {
            if (n > peer.window) {
                CHECK(peer.forgiving || (peer.window == 0 && n == 1));  /* only a one-byte probe */
                peer.probes++;
            } else {
                CHECK(peer.rxlen + n <= sizeof peer.rxdata);
                memcpy(peer.rxdata + peer.rxlen, data, n);
                peer.rxlen += n;
                peer.rcv_nxt += n;
                if (flags & 0x01) {
                    peer.fin_rcvd = 1;
                    peer.rcv_nxt++;
                }
            }
        } else {
            peer.out_of_order++;
        }
        peer.ack_due = 1;
    }
}

/* Called once per tick: the peer sends what it may. */
static void peer_pump(void)
{
    u32 off, in_flight, n;
    int k;

    if (peer.state != 2 || peer.deaf)
        return;
    if (peer.reopen_at && (int)(ticks - (u32)peer.reopen_at) >= 0) {
        peer.window = 3000;                     /* without telling anybody */
        peer.reopen_at = 0;
    }
    if (peer.echo) {
        peer.txdata = peer.rxdata;
        peer.txlen = peer.rxlen;
    }
    if (peer.snd_una != peer.snd_nxt && ticks - peer.last_progress > 45) {
        peer.snd_nxt = peer.snd_una;            /* no acknowledgement: send again */
        if (peer.overlap && peer.snd_una - (SRV_ISS + 1) >= 300 && !peer.fin_sent)
            peer.snd_nxt -= 300;                /* ...including a part it already has */
        peer.last_progress = ticks;
        peer.resent++;
    }
    for (k = 0; k < 3; k++) {
        off = peer.snd_nxt - (SRV_ISS + 1);
        in_flight = seq_gt(peer.snd_nxt, peer.snd_una) ? peer.snd_nxt - peer.snd_una : 0;
        if (off >= peer.txlen || in_flight >= peer.client_window)
            break;
        n = peer.txlen - off;
        if (n > 1460)
            n = 1460;
        if (n > peer.client_window - in_flight)
            n = peer.client_window - in_flight;
        if (peer.reset_after && off >= (u32)peer.reset_after) {
            peer_send(0x14, peer.snd_nxt, NULL, 0);
            peer.state = 0;
            return;
        }
        peer_send(0x18, peer.snd_nxt, peer.txdata + off, n);
        peer.snd_nxt += n;
        peer.ack_due = 0;
    }
    if (peer.close_after_send && peer.snd_nxt - (SRV_ISS + 1) == peer.txlen) {
        peer_send(0x11, peer.snd_nxt, NULL, 0);
        peer.snd_nxt++;
        peer.fin_sent = 1;
        peer.ack_due = 0;
    }
    if (peer.ack_due) {
        peer_send(0x10, peer.snd_nxt, NULL, 0);
        peer.ack_due = 0;
    }
}

static u8 stream[400000];
static u8 chunk[5000];

static void peer_reset(void)
{
    u32 i;

    bring_up();
    srand(7);
    memset(&peer, 0, offsetof(struct peer, rxdata));
    peer.window = 20000;
    for (i = 0; i < sizeof stream; i++)
        stream[i] = (u8)(i * 131 + (i >> 8) * 7 + (i >> 16) + 3);
}

static void polls(int n)
{
    while (n--)
        net_poll();
}

static int all_free(void)
{
    int i;

    for (i = 0; i < CONNECTIONS; i++)
        if (conns[i].state != S_FREE)
            return 0;
    return 1;
}

/* Fetch `size` bytes the peer sends and then closes on; check every byte. */
static void download(u32 size, unsigned read_size, int lazy)
{
    u32 got = 0;
    int c, n = 0, bad = 0, turn = 0;

    peer.txdata = stream;
    peer.txlen = size;
    peer.close_after_send = 1;
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0);
    if (c < 0)
        return;
    for (;;) {
        if (lazy && ++turn % 40 == 0)
            polls(120);                         /* the program is busy; data piles up */
        n = tcp_read(c, chunk, (u16)read_size, 3000);
        if (n < 0)
            break;
        if (got + (u32)n > size || memcmp(chunk, stream + got, (size_t)n) != 0)
            bad++;
        got += (u32)n;
    }
    CHECK(n == NET_ERR_CLOSED);
    CHECK(got == size);
    CHECK(bad == 0);
    tcp_close(c);
    polls(400);
    CHECK(peer.fin_rcvd && peer.fin_acked);
    CHECK(all_free());
}

static void upload(u32 size)
{
    int c;

    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0);
    if (c < 0)
        return;
    CHECK(tcp_write(c, stream, size, 3000) == 0);
    CHECK(tcp_flush(c, 6000) == 0);
    CHECK(peer.rxlen == size && memcmp(peer.rxdata, stream, size) == 0);
    tcp_close(c);
    polls(100);
    CHECK(peer.fin_rcvd);
    peer.close_after_send = 1;                  /* now the peer finishes too */
    polls(400);
    CHECK(all_free());
}

static void test_tcp(void)
{
    u32 before;
    int c, n, i, held[CONNECTIONS + 1];

    printf("TCP: connect, refused, no answer\n");
    peer_reset();
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    polls(1);                                   /* our final handshake reply goes out */
    CHECK(c >= 0 && tcp_is_open(c) && peer.state == 2 && peer.syns == 1);
    CHECK(conns[c].mss == 1400);
    tcp_abort(c);
    CHECK(peer.got_rst == 1 && all_free());

    peer_reset();
    peer.refuse = 1;
    CHECK(tcp_connect(SRV_IP, SRV_PORT, 600) == NET_ERR_REFUSED);
    CHECK(all_free());

    peer_reset();
    peer.silent = 1;
    before = ticks;
    CHECK(tcp_connect(SRV_IP, SRV_PORT, 3000) == NET_ERR_TIMEOUT);
    CHECK(peer.syns == 5 && all_free());
    CHECK(ticks - before > 25 * 60 && ticks - before < 40 * 60);

    peer_reset();
    peer.silent = 1;
    CHECK(tcp_connect(SRV_IP, SRV_PORT, 90) == NET_ERR_TIMEOUT);    /* caller's own limit */
    CHECK(all_free());

    printf("TCP: first reply to the connection request lost\n");
    peer_reset();
    peer.drop_synack = 1;
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0 && peer.syns == 2);
    tcp_abort(c);

    printf("TCP: download 300 KB\n");
    peer_reset();
    download(300000, 1000, 0);
    CHECK(net_stats.tcp_resent == 0 && net_stats.tcp_dropped == 0);

    printf("TCP: download with a program that reads rarely\n");
    peer_reset();
    download(200000, 4096, 1);

    printf("TCP: download one byte at a time, and nothing at all\n");
    peer_reset();
    download(3000, 1, 0);
    peer_reset();
    download(0, 100, 0);

    printf("TCP: download with one segment in 9 to us lost\n");
    peer_reset();
    peer.drop_tx_every = 9;
    download(120000, 1500, 0);
    printf("TCP: download where repeats overlap what we already have\n");
    peer_reset();
    peer.drop_tx_every = 6;
    peer.overlap = 1;
    download(120000, 1500, 0);
    CHECK(peer.resent > 3);
    printf("TCP: download with one segment in 5 from us lost\n");
    peer_reset();
    peer.drop_rx_every = 5;
    download(120000, 1500, 0);
    printf("TCP: download with one segment in 7 damaged\n");
    peer_reset();
    peer.corrupt_every = 7;
    download(120000, 1500, 0);
    CHECK(net_stats.rx_bad > 0);
    printf("TCP: download with segments repeated and out of order\n");
    peer_reset();
    peer.dup_every = 3;
    peer.reorder_every = 4;
    download(120000, 1500, 0);
    CHECK(net_stats.tcp_dropped > 0);

    printf("TCP: upload 200 KB\n");
    peer_reset();
    upload(200000);
    CHECK(net_stats.tcp_resent == 0);

    printf("TCP: upload into a small window, and with segments lost\n");
    peer_reset();
    peer.window = 700;
    upload(40000);
    peer_reset();
    peer.drop_rx_every = 6;
    upload(60000);
    CHECK(net_stats.tcp_resent > 0);
    peer_reset();
    peer.drop_tx_every = 4;
    upload(60000);

    printf("TCP: upload while the other side's window is shut\n");
    peer_reset();
    peer.window = 0;
    peer.reopen_at = (int)ticks + 400;
    upload(20000);
    CHECK(peer.probes > 0);

    printf("TCP: window shut for a minute and a half, other side still answering\n");
    peer_reset();
    peer.window = 0;
    peer.reopen_at = (int)ticks + 100 * 60;
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0);
    CHECK(tcp_write(c, stream, 3000, 600) == 0);
    CHECK(tcp_flush(c, 200 * 60) == 0);
    CHECK(peer.probes >= 7 && peer.rxlen == 3000 && memcmp(peer.rxdata, stream, 3000) == 0);
    tcp_abort(c);

    printf("TCP: window shut and the other side gone\n");
    peer_reset();
    peer.window = 0;
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0);
    CHECK(tcp_write(c, stream, 1000, 600) == 0);
    polls(200);
    peer.deaf = 1;
    CHECK(tcp_flush(c, 200 * 60) == NET_ERR_TIMEOUT);
    tcp_close(c);
    CHECK(all_free());

    printf("TCP: echo\n");
    peer_reset();
    peer.echo = 1;
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0);
    for (i = 0; i < 50; i++) {
        CHECK(tcp_write(c, stream + i * 777, 777, 600) == 0);
        for (n = 0; n < 777;) {
            int r = tcp_read(c, chunk + n, (u16)(777 - n), 600);

            CHECK(r > 0);
            if (r <= 0)
                break;
            n += r;
        }
        CHECK(memcmp(chunk, stream + i * 777, 777) == 0);
    }
    tcp_abort(c);

    printf("TCP: the other side resets the connection\n");
    peer_reset();
    peer.txdata = stream;
    peer.txlen = 50000;
    peer.reset_after = 20000;
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0);
    while ((n = tcp_read(c, chunk, 4000, 600)) > 0)
        ;
    CHECK(n == NET_ERR_RESET);
    CHECK(tcp_send(c, "x", 1) == NET_ERR_RESET);
    tcp_close(c);
    CHECK(all_free());

    printf("TCP: the other side goes silent in mid-transfer\n");
    peer_reset();
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0);
    peer.deaf = 1;
    before = ticks;
    CHECK(tcp_write(c, stream, 3000, 600) == 0);
    CHECK(tcp_flush(c, 100 * 60) == NET_ERR_TIMEOUT);
    CHECK(tcp_error(c) == NET_ERR_TIMEOUT);
    CHECK(ticks - before > 50 * 60 && ticks - before < 80 * 60);
    tcp_close(c);
    CHECK(all_free());

    printf("TCP: closing when the other side never finishes\n");
    peer_reset();
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0);
    tcp_close(c);
    polls(100);
    CHECK(peer.fin_rcvd && !all_free());        /* our half is closed, theirs is not */
    polls(700);
    CHECK(all_free() && peer.got_rst == 1);     /* gave up waiting and said so */

    printf("TCP: connection limit\n");
    peer_reset();
    for (i = 0; i <= CONNECTIONS; i++)
        held[i] = tcp_open(IP4(192, 168, 1, 99), (u16)(9000 + i));
    for (i = 0; i < CONNECTIONS; i++)
        CHECK(held[i] >= 0);
    CHECK(held[CONNECTIONS] == NET_ERR_NO_SOCKET);
    for (i = 0; i < CONNECTIONS; i++)
        for (n = 0; n < i; n++)
            CHECK(conns[held[i]].local_port != conns[held[n]].local_port);
    for (i = 0; i < CONNECTIONS; i++)
        tcp_close(held[i]);
    CHECK(all_free());

    printf("TCP: a segment for no connection is answered with a reset\n");
    peer_reset();
    CHECK(net_ping(GW_IP, 1, 120, NULL) == 0);  /* so the gateway's address is known */
    peer.state = 2;
    peer.cport = 50000;
    peer.rcv_nxt = 1234;
    peer_send(0x18, 99, (const u8 *)"hello", 5);
    polls(3);
    CHECK(peer.got_rst == 1);

    printf("TCP: use after close is harmless\n");
    CHECK(tcp_send(2, "x", 1) == NET_ERR_CLOSED);
    CHECK(tcp_recv(2, chunk, 10) == NET_ERR_CLOSED);
    CHECK(tcp_recv(-1, chunk, 10) == NET_ERR_CLOSED && tcp_recv(99, chunk, 10) == NET_ERR_CLOSED);
    tcp_close(2);
    tcp_abort(99);
}

/* Damaged TCP segments aimed at a live connection. */
static void test_tcp_garbage(void)
{
    u8 f[1600];
    int c, i, j;
    u16 len;

    printf("TCP garbage: 300000 damaged segments at an open connection\n");
    peer_reset();
    peer.echo = 1;
    peer.forgiving = 1;
    c = tcp_connect(SRV_IP, SRV_PORT, 600);
    CHECK(c >= 0);
    srand(2);
    for (i = 0; i < 300000; i++) {
        u8 *t = f + 34, *exact;
        unsigned n = (unsigned)(rand() % 200), hlen = 20 + 4 * (unsigned)(rand() % 3);

        memset(f, 0, sizeof f);
        w16(t, SRV_PORT);
        w16(t + 2, conns[c].local_port);
        w32(t + 4, conns[c].rcv_nxt + (u32)(rand() % 5) - 2);
        w32(t + 8, conns[c].snd_una + (u32)(rand() % 5) - 2);
        t[12] = (u8)((hlen / 4) << 4);
        t[13] = (u8)(rand() % 3 ? 0x10 : rand());
        w16(t + 14, rand());
        for (j = 20; j < (int)(hlen + n); j++)
            t[j] = (u8)rand();
        if (rand() % 4)
            t[rand() % (hlen + n)] ^= (u8)rand();
        if (rand() % 3)
            w16(t + 16, 0), w16(t + 16, checksum(t, hlen + n, pseudo(SRV_IP, LEASE_IP, 6, hlen + n)));
        len = wrap_ip(f, MY_MAC, GW_MAC, SRV_IP, LEASE_IP, 6, hlen + n);
        if (rand() % 5 == 0)
            len = (u16)(34 + rand() % (len - 33));
        exact = malloc(len);
        memcpy(exact, f, len);
        net_input(exact, len);
        free(exact);
        if (i % 16 == 0) {
            tcp_recv(c, chunk, sizeof chunk);
            tcp_send(c, "abc", 3);
            tcp_poll();
        }
        queue_head = queue_tail = 0;
        if (!lookup(c) || conns[c].state != S_OPEN) {   /* a forged reset or FIN got through */
            tcp_abort(c);
            peer.state = 0;
            c = tcp_connect(SRV_IP, SRV_PORT, 600);
            if (c < 0) {
                CHECK(c >= 0);
                return;
            }
        }
    }
}

int main(void)
{
    test_parse();
    test_dhcp();
    test_ping();
    test_incoming();
    test_dns();
    test_abort();
    test_garbage();
    test_tcp();
    test_tcp_garbage();
    if (failures) {
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("All network tests passed.\n");
    return 0;
}
