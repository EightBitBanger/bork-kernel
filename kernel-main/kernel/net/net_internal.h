#ifndef _KERNEL_NET_INTERNAL_H_
#define _KERNEL_NET_INTERNAL_H_

// Shared by the files in net/ only. Net lock held for everything below
// unless it says otherwise.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/net/net.h>

#define ETH_HEADER_LEN      14
#define ETH_MIN_FRAME       60      // Without the FCS: shorter frames are padded
#define ETH_TYPE_IPV4       0x0800
#define ETH_TYPE_ARP        0x0806

#define IP_HEADER_LEN       20      // Without options (we never send any)
#define IP_PROTO_ICMP       1
#define IP_PROTO_TCP        6
#define IP_PROTO_UDP        17
#define UDP_HEADER_LEN      8

#define IP_BROADCAST        0xFFFFFFFFu
#define IP_LOOPBACK         0x7F000001u     // 127.0.0.1

void net_lock(void);
void net_unlock(void);

// One pass of the net thread (no lock held): the receive queue, then the
// timers. True if a frame was handled.
bool net_service(void);

// Big-endian fields in a byte buffer: no packed structs, no alignment traps
static inline uint16_t net_get16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t net_get32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void net_put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void net_put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// Internet checksum (RFC 1071): `initial` carries a pseudo header sum
uint32_t net_sum(const uint8_t* data, uint32_t length, uint32_t initial);
uint16_t net_fold(uint32_t sum);

// Unpredictable: RDRAND where the CPU has it, else a SipHash generator
// keyed from cycle-counter timing. Net lock held.
uint32_t net_random(void);
// Stir timing of an event into the fallback generator. Net lock held.
void     net_entropy(uint64_t sample);
// SipHash-2-4 of `data` under a 128-bit key
uint64_t net_siphash(const uint64_t key[2], const uint8_t* data, uint32_t length);
uint64_t net_now_ms(void);

// Interfaces (netif.c)
struct NetIf* netif_find_locked(const char* name);
struct NetIf* netif_get_locked(uint32_t index);
uint32_t      netif_count_locked(void);
// Hand a finished frame to the driver, counting it
int  netif_transmit(struct NetIf* ifp, const uint8_t* frame, uint32_t length);
bool netif_usable(const struct NetIf* ifp);     // Up, link, (an address or DHCP running)

// Is `ip` one of ours / the broadcast of one of our subnets
struct NetIf* net_owner_of(uint32_t ip);
bool net_is_broadcast_for(const struct NetIf* ifp, uint32_t ip);
// Never a valid source on the wire: 0, broadcast, multicast, 127/8, class E
bool net_ip_martian(uint32_t ip);

// Ethernet + ARP (arp.c)
void eth_input(struct NetIf* ifp, const uint8_t* frame, uint32_t length);
// Send an IP packet to `next_hop` on `ifp`: framed now if its MAC is known,
// else held (one per neighbour) until ARP answers
int  eth_output_ip(struct NetIf* ifp, uint32_t next_hop, const uint8_t* packet, uint32_t length);
void arp_timer(void);
void arp_flush_if(const struct NetIf* ifp);

// IPv4 (ipv4.c)
void ip_input(struct NetIf* ifp, const uint8_t* packet, uint32_t length);
// Build the header in front of `payload` and route it. `src` 0: the
// outgoing interface's address. `ifp_hint` forces an interface (DHCP).
int  ip_output(uint32_t src, uint32_t dst, uint8_t protocol, const uint8_t* payload, uint32_t length,
               struct NetIf* ifp_hint);
// Source address for a packet to `dst` (0: no route)
uint32_t ip_source_for(uint32_t dst);
void icmp_input(struct NetIf* ifp, uint32_t src, uint32_t dst, const uint8_t* data, uint32_t length);

// UDP (udp.c)
void udp_input(struct NetIf* ifp, uint32_t src, uint32_t dst, const uint8_t* data, uint32_t length);
// Port 68 is the DHCP client's: dhcp.c gets those directly
void dhcp_input(struct NetIf* ifp, uint32_t src, const uint8_t* data, uint32_t length);
int  udp_send_raw(struct NetIf* ifp, uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port,
                  const void* data, uint32_t length);

// TCP (tcp.c)
void tcp_input(struct NetIf* ifp, uint32_t src, uint32_t dst, const uint8_t* data, uint32_t length);
void tcp_timer(void);

// DHCP (dhcp.c)
void dhcp_start(struct NetIf* ifp);
void dhcp_stop(struct NetIf* ifp);
void dhcp_timer(void);

#endif
