#ifndef _KERNEL_NET_H_
#define _KERNEL_NET_H_

#include <stdint.h>
#include <stdbool.h>

//
// IPv4 network stack
//
//   netif.c    interfaces, the receive queue, the "net" thread, loopback
//   arp.c      Ethernet framing and ARP (address resolution)
//   ipv4.c     IPv4 in and out, routing, ICMP echo (ping)
//   udp.c      UDP sockets for kernel code
//   dhcp.c     DHCP client: addresses an interface once its link comes up
//   tcp.c      TCP connections for kernel code (and the socket calls)
//   dns.c      DNS stub resolver: host name -> IPv4 address
//
// No forwarding, no fragment reassembly (fragments are dropped), no IP
// options (skipped, never acted on).
//
// Threads and locks
//
// One "net" thread does all protocol work: it drains the receive queue,
// runs the ARP and DHCP timers, and sends what waits on ARP. One mutex (the
// net lock) covers every table here. Lock order:
//
//   driver lock -> net lock -> receive queue lock -> hardware / bus locks
//
// so a driver's transmit() runs with the net lock held and may take its bus
// lock, but a driver must never call into the stack with its bus lock held,
// except netif_rx(), which only queues.
//
// Everything that arrives is untrusted: every length is checked against
// what was actually received before anything is read, and every table
// (ARP, sockets, queues) has a fixed size.
//
// Addresses are uint32_t in host order: 192.168.1.2 is 0xC0A80102.
//

#define NET_MAC_LEN         6
#define NETIF_NAME_MAX      8       // Including the terminator
#define NETIF_MAX           4
#define NET_FRAME_MAX       1518    // Largest Ethernet frame taken in or sent
#define NET_MTU_DEFAULT     1500

// Results (negative)
#define NET_OK              0
#define NET_ERR_PARAM       (-1)
#define NET_ERR_NOMEM       (-2)
#define NET_ERR_DOWN        (-3)    // Interface down, no link, or not configured
#define NET_ERR_NOROUTE     (-4)    // No interface reaches that address
#define NET_ERR_TIMEOUT     (-5)
#define NET_ERR_AGAIN       (-6)    // Nothing yet / queue full: try again
#define NET_ERR_INUSE       (-7)    // UDP port taken
#define NET_ERR_TOOBIG      (-8)
#define NET_ERR_IO          (-9)    // The driver failed the send
#define NET_ERR_REFUSED     (-10)   // TCP: the peer answered with a reset
#define NET_ERR_RESET       (-11)   // TCP: the connection was reset
#define NET_ERR_CLOSED      (-12)   // TCP: not connected (any more)
#define NET_ERR_NOTFOUND    (-13)   // DNS: no such name, or it has no IPv4 address

//
// Interfaces (drivers)
//

struct NetIf;

struct NetIfOps {
    // Send one Ethernet frame (header included, length <= NET_FRAME_MAX).
    // Net lock held: may take the driver's bus lock, must not call back into
    // the stack (netif_rx is fine). Returns NET_OK or NET_ERR_*.
    int (*transmit)(struct NetIf* ifp, const void* frame, uint32_t length);
};

// NetIf.flags
#define NETIF_UP            0x0001  // Enabled (ifconfig up / down)
#define NETIF_LINK          0x0002  // Carrier: the driver says frames can move
#define NETIF_LOOPBACK      0x0004  // lo: IP packets with no Ethernet header
#define NETIF_DHCP          0x0008  // Addressed by DHCP (else static or none)

struct NetIfStats {
    uint32_t rx_packets, rx_bytes, rx_dropped;
    uint32_t tx_packets, tx_bytes, tx_dropped;
};

struct NetIf {
    char     name[NETIF_NAME_MAX];
    uint8_t  mac[NET_MAC_LEN];
    uint16_t mtu;
    uint16_t flags;                 // NETIF_*
    uint32_t ip;                    // 0: none
    uint32_t netmask;
    uint32_t gateway;               // 0: none
    uint32_t dns;                   // From DHCP, 0: none (dns.c asks the gateway then)
    struct NetIfStats stats;

    const struct NetIfOps* ops;
    void*    driver_data;
};

// Add an interface ("wlan0", "eth0"). It starts up, with no link and no
// address; DHCP starts by itself when the link comes up. NULL when the name
// is bad or taken, the table is full, or out of memory. Driver lock may be
// held (probe); net lock must not be.
struct NetIf* netif_register(const char* name, const struct NetIfOps* ops, void* driver_data,
                             const uint8_t* mac, uint16_t mtu);

// Take it away: waits for any transmit() in progress, drops its queued
// frames, ARP entries and DHCP state, and frees it
void netif_unregister(struct NetIf* ifp);

// Carrier on / off, and the hardware address (once the driver knows it)
void netif_set_link(struct NetIf* ifp, bool link);
void netif_set_mac(struct NetIf* ifp, const uint8_t* mac);

// A frame came in: copied into the receive queue (dropped when it is full
// or the frame is not a sane size). Callable from any thread, with bus locks
// held; never with the net lock held, except by loopback.
void netif_rx(struct NetIf* ifp, const void* frame, uint32_t length);

//
// Configuration (console, later system calls). Take the net lock.
//

uint32_t netif_count(void);
// A copy of interface `index` (0..netif_count()-1), false if there is none.
// Copies, so the caller needs no lock and the interface may go meanwhile.
bool netif_snapshot(uint32_t index, struct NetIf* out);

// Static address (ip 0: unconfigure). Stops DHCP on it.
int  netif_configure(const char* name, uint32_t ip, uint32_t netmask, uint32_t gateway);
// Start (again) DHCP on it
int  netif_dhcp(const char* name);
int  netif_set_up(const char* name, bool up);

//
// ARP table (console)
//

struct ArpView {
    uint32_t ip;
    uint8_t  mac[NET_MAC_LEN];
    bool     complete;
    char     ifname[NETIF_NAME_MAX];
};
// Copies up to `max` entries, returns how many
uint32_t arp_snapshot(struct ArpView* out, uint32_t max);

//
// ICMP echo
//

// Send one echo request and wait up to timeout_ms for the reply. Returns
// the round trip in ms (>= 0), or NET_ERR_*. Not with the net lock held.
int net_ping(uint32_t ip, uint16_t sequence, uint32_t timeout_ms);

//
// UDP (kernel)
//

#define UDP_PAYLOAD_MAX     1472    // 1500 - IP header - UDP header
#define UDP_QUEUE_MAX       8       // Datagrams waiting per socket

struct UdpSocket;

// Bind a local port (0: a random free one from 49152-65535). NULL when the
// port is taken, the table is full, or out of memory.
struct UdpSocket* udp_open(uint16_t port);
uint16_t udp_local_port(const struct UdpSocket* s);
int  udp_sendto(struct UdpSocket* s, uint32_t ip, uint16_t port, const void* data, uint32_t length);
// One datagram, cut to `size`. Returns its length, or NET_ERR_AGAIN when
// none is waiting. Never blocks.
int  udp_recvfrom(struct UdpSocket* s, void* buffer, uint32_t size, uint32_t* ip, uint16_t* port);
void udp_close(struct UdpSocket* s);

//
// TCP (kernel). Nothing blocks: calls return NET_ERR_AGAIN, and tcp_poll()
// says what is ready.
//

#define TCP_BACKLOG_MAX     8

struct TcpSocket;

// tcp_poll() bits
#define TCP_EV_READ         0x01    // Data waiting, or the peer closed (recv says 0), or an error
#define TCP_EV_WRITE        0x02    // Connected with room to send, or an error
#define TCP_EV_ACCEPT       0x04    // Listener: a connection is ready for tcp_accept
#define TCP_EV_ERROR        0x08    // Refused, reset or timed out (tcp_error says which)
#define TCP_EV_CONNECTED    0x10    // The handshake is done (stays set)

// Listen on `port` (any local address). backlog: connections, half-open
// ones included, waiting for tcp_accept (1..TCP_BACKLOG_MAX).
struct TcpSocket* tcp_listen(uint16_t port, uint32_t backlog, int* error);
// A connection that finished its handshake, or NULL. ip / port: the peer.
struct TcpSocket* tcp_accept(struct TcpSocket* listener, uint32_t* ip, uint16_t* port);
// Start connecting (from a random local port); poll for TCP_EV_CONNECTED
// or TCP_EV_ERROR
struct TcpSocket* tcp_connect(uint32_t ip, uint16_t port, int* error);
// Queue bytes to send. Returns how many were taken (> 0), NET_ERR_AGAIN
// when the send buffer is full, or an error.
int  tcp_send(struct TcpSocket* s, const void* data, uint32_t length);
// Bytes received (> 0), 0 once the peer closed and everything was read,
// NET_ERR_AGAIN when nothing is waiting, or an error.
int  tcp_recv(struct TcpSocket* s, void* buffer, uint32_t size);
uint32_t tcp_poll(struct TcpSocket* s);
int  tcp_error(struct TcpSocket* s);
// Peer and local ends (0 while unknown)
void tcp_endpoints(struct TcpSocket* s, uint32_t* local_ip, uint16_t* local_port,
                   uint32_t* remote_ip, uint16_t* remote_port);
// Let go of it: an orderly close (FIN) when everything was read, a reset
// otherwise. The stack finishes the close and frees it; never use `s` again.
void tcp_close(struct TcpSocket* s);

//
// DNS
//

#define DNS_NAME_MAX        254     // Bytes of a host name, terminator included

// The IPv4 address of host `name` ("www.example.com"), asked of the DNS
// server DHCP gave (else the gateway). A dotted address ("10.0.0.1") and
// "localhost" are answered at once. Blocks, sleeping, up to timeout_ms;
// `cancel` (may be NULL) is asked every slice and ends the wait early when
// it says true. Returns NET_OK, NET_ERR_PARAM (not a host name),
// NET_ERR_DOWN (no interface with an address), NET_ERR_NOTFOUND (no such
// name, or no IPv4 address for it), NET_ERR_TIMEOUT, NET_ERR_IO (the server
// failed the query) or NET_ERR_NOMEM. Not with the net lock held.
int dns_resolve(const char* name, uint32_t* out, uint32_t timeout_ms, bool (*cancel)(void));

//
// Setup and helpers
//

// Once at boot, before driver_init() (drivers register interfaces in probe)
void net_init(void);

// "a.b.c.d" -> address; false for anything else
bool net_parse_ip(const char* text, uint32_t* out);
// address -> "a.b.c.d" (out holds at least 16 bytes)
void net_format_ip(uint32_t ip, char* out);
// "aa:bb:cc:dd:ee:ff" (out holds at least 18 bytes)
void net_format_mac(const uint8_t* mac, char* out);

#endif
