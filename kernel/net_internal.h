/* Shared between net.c and tcp.c; other code uses net.h. */
#ifndef SSHOS_NET_INTERNAL_H
#define SSHOS_NET_INTERNAL_H

#include "kernel.h"
#include "net.h"

#define ETH_HLEN        14
#define IP_HLEN         20
#define ETH_MAX         1514
#define IP_MAX_DATA     (ETH_MAX - ETH_HLEN - IP_HLEN)
#define TICKS_PER_SEC   60

#define PROTO_ICMP      1
#define PROTO_TCP       6
#define PROTO_UDP       17

/* Big-endian fields in a byte buffer. */
u16  net_get16(const u8 *p);
u32  net_get32(const u8 *p);
void net_put16(u8 *p, u16 v);
void net_put32(u8 *p, u32 v);

/* Internet checksum, in two steps so a pseudo-header can be included. */
u32  net_sum(const u8 *p, u32 len, u32 sum);
u16  net_sum_finish(u32 sum);
u32  net_pseudo_sum(u32 src, u32 dst, u8 proto, u16 len);

/* Sending: build the data at net_ip_payload(), then net_ip_send(). Returns
 * NET_ERR_ARP (and sends the question) when the next hop's hardware address
 * is not known yet; try again later. */
u8  *net_ip_payload(void);
int  net_ip_send(u32 src, u32 dst, u8 proto, u16 payload_len);
int  net_route_ready(u32 dst);              /* wait until dst can be reached */

/* tcp.c */
void tcp_input(u32 src, const u8 *segment, u16 len);
void tcp_poll(void);                        /* timers and sending */
void tcp_reset_all(void);

#endif
