/* SSH OS network stack: Ethernet, ARP, IPv4, ICMP, UDP, DHCP and DNS.
 *
 * net.c knows nothing about the hardware. It hands finished Ethernet frames
 * to eth_send() and is given received ones through net_input(); dayna.c
 * provides both for the BlueSCSI's network adapter. Nothing here runs from
 * an interrupt: frames only arrive while somebody calls net_poll().
 *
 * IP addresses are 32-bit numbers with the first part of the address in the
 * top byte: 192.168.1.20 is 0xC0A80114.
 */
#ifndef SSHOS_NET_H
#define SSHOS_NET_H

#include "types.h"

#define IP4(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

#define NET_ERR_DOWN      (-1)  /* the network has not been started */
#define NET_ERR_TIMEOUT   (-2)  /* no answer in time */
#define NET_ERR_ARP       (-3)  /* nobody on the local network has that address */
#define NET_ERR_SEND      (-4)  /* the adapter refused the frame */
#define NET_ERR_NO_ROUTE  (-5)  /* address is elsewhere and there is no gateway */
#define NET_ERR_TOO_BIG   (-6)
#define NET_ERR_NAME      (-7)  /* the name server does not know that name */
#define NET_ERR_NO_DNS    (-8)  /* no name server configured */
#define NET_ERR_ABORTED   (-9)  /* stopped by net_abort_check */
#define NET_ERR_NO_ADAPTER (-10)
#define NET_ERR_CLOSED    (-11) /* the other side finished, or we closed it */
#define NET_ERR_RESET     (-12) /* the other side dropped the connection */
#define NET_ERR_REFUSED   (-13) /* nothing is listening on that port */
#define NET_ERR_NO_SOCKET (-14) /* all connections are in use */

struct net_config {
    u8  up;                     /* adapter found and started */
    u8  mac[6];
    u32 ip, mask, gateway, dns; /* all 0 until configured */
    u8  from_dhcp;
    u32 lease_secs;             /* DHCP lease length */
    u32 lease_start;            /* tick count when it was granted */
};
extern struct net_config net;

struct net_stats {
    u32 rx_frames, tx_frames;
    u32 rx_bad;                 /* damaged or malformed */
    u32 rx_ignored;             /* fine, but not for us or not understood */
    u32 tx_errors;
    u32 tcp_resent;             /* segments sent again after no acknowledgement */
    u32 tcp_dropped;            /* segments that arrived out of order and were dropped */
};
extern struct net_stats net_stats;

/* Supplied by the adapter driver (dayna.c). */
int eth_send(const u8 *frame, u16 len);     /* 0 or negative */
int eth_poll(void);     /* fetch waiting frames, calling net_input for each */

/* Start-up and housekeeping. */
void net_start(const u8 *mac);              /* forget everything, begin with this address */
void net_stop(void);
void net_set_static(u32 ip, u32 mask, u32 gateway, u32 dns);
int  net_dhcp(void);                        /* ask the network for an address */
void net_dhcp_begin(void);                  /* the same without waiting: ask, */
int  net_dhcp_step(void);                   /* then call this now and then; 1 = address set */
extern u8 net_dhcp_tries;                   /* how often it asks before giving up (4) */
void net_input(const u8 *frame, u16 len);
void net_poll(void);

/* While one of the waiting calls below is waiting, this is called over and
 * over (if set); returning nonzero makes the call give up with
 * NET_ERR_ABORTED. The shell uses it for the Esc key. */
extern int (*net_abort_check)(void);

/* For a program's own waiting loop: lets the network run for a moment,
 * then returns 0 to keep waiting, or NET_ERR_ABORTED / NET_ERR_TIMEOUT. */
struct timeout;
int net_wait_step(struct timeout *t);

/* Waiting calls: they return when done, failed or timed out. */
int net_ping(u32 ip, u16 seq, u32 timeout_ticks, u32 *rtt_ticks);
int net_dns(const char *name, u32 *ip);
int net_resolve(const char *name_or_address, u32 *ip);

int net_parse_ip(const char *s, u32 *ip);   /* 1 if s is a.b.c.d */
const char *net_strerror(int err);

/* UDP. To send: write the data at udp_buffer(), then call udp_send() with
 * its length, without calling net_poll() in between. A handler runs inside
 * net_poll() for each datagram arriving on its port. */
#define UDP_MAX_DATA 1472
typedef void (*udp_handler)(u32 src_ip, u16 src_port, const u8 *data, u16 len);
int  udp_bind(u16 port, udp_handler handler);
void udp_unbind(u16 port);
u8  *udp_buffer(void);
int  udp_send(u32 dst_ip, u16 src_port, u16 dst_port, u16 len);

/* TCP, outgoing connections only. A connection is a small number handed
 * out by tcp_open() or tcp_connect(); give it back with tcp_close() or
 * tcp_abort() in every case, even after an error.
 *
 * Nothing is sent or received except inside net_poll(). The waiting calls
 * poll for you; with the others, keep calling net_poll(). */
int  tcp_open(u32 ip, u16 port);        /* start connecting; returns at once */
int  tcp_connecting(int c);             /* still waiting for the other side */
int  tcp_is_open(int c);                /* connected and we may still send */
int  tcp_error(int c);                  /* 0, or why the connection ended */
int  tcp_send(int c, const void *data, u16 len);    /* queue data; returns how much fitted */
u16  tcp_send_space(int c);
u16  tcp_unsent(int c);                 /* queued bytes the other side has not confirmed */
/* Received data, in order. tcp_recv copies; tcp_peek/tcp_consume let a
 * program use the bytes where they lie. Both return 0 when nothing has
 * arrived yet, and NET_ERR_CLOSED (or the error) once the other side is
 * done and everything has been read. */
int  tcp_recv(int c, void *buf, u16 max);
int  tcp_peek(int c, const u8 **data);
void tcp_consume(int c, u16 n);
void tcp_close(int c);                  /* send what is queued, then say goodbye */
void tcp_abort(int c);                  /* drop it now */

/* Waiting versions. */
int  tcp_connect(u32 ip, u16 port, u32 timeout_ticks);  /* connection, or NET_ERR_ */
int  tcp_write(int c, const void *data, u32 len, u32 timeout_ticks);    /* 0 once all is queued */
int  tcp_read(int c, void *buf, u16 max, u32 timeout_ticks);    /* at least one byte, or NET_ERR_ */
int  tcp_flush(int c, u32 timeout_ticks);   /* 0 once the other side confirmed everything */

#endif
