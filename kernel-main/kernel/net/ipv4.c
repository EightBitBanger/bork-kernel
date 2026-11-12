#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/net/net.h>
#include <kernel/net/net_internal.h>

#include <kernel/malloc.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/util/string.h>

//
// IPv4 (RFC 791) and ICMP echo (RFC 792)
//
// A host, not a router: packets are taken only when addressed to the
// interface they came in on (or its broadcast), and never forwarded. What
// is dropped on the way in:
//   - anything whose header lengths disagree with each other or with the
//     bytes received, or whose checksum is wrong
//   - fragments (no reassembly, so no reassembly attacks)
//   - martian sources (0, broadcast, multicast, 127/8 off loopback) and
//     packets claiming to come from one of our own addresses
// Options are skipped over, never acted on (no source routing).
//
// ICMP: echo requests are answered (unicast only, rate limited, so this
// host is no smurf amplifier); nothing else is ever sent.
//

#define IP_TTL              64
#define IP_FLAG_DF          0x4000
#define IP_FRAG_MASK        0x3FFF  // MF + fragment offset

#define ICMP_ECHO_REPLY     0
#define ICMP_ECHO_REQUEST   8
#define ICMP_HEADER_LEN     8

#define ICMP_RATE_PER_SEC   10      // Echo replies sent per second, sustained
#define ICMP_BURST          20

#define PING_WAITERS        4
#define PING_PAYLOAD        32
#define PING_POLL_MS        5

//
// Out
//

// Interface and next hop for `dst`. NULL when nothing reaches it.
static struct NetIf* ip_route(uint32_t dst, uint32_t* next_hop) {
    *next_hop = dst;

    // Ours (any interface) or 127/8: loopback
    if ((dst >> 24) == 127 || net_owner_of(dst) != NULL)
        return netif_find_locked("lo");

    // On a directly attached subnet
    for (uint32_t i = 0; i < netif_count_locked(); i++) {
        struct NetIf* ifp = netif_get_locked(i);
        if ((ifp->flags & NETIF_LOOPBACK) || !netif_usable(ifp) || ifp->ip == 0)
            continue;
        if ((dst & ifp->netmask) == (ifp->ip & ifp->netmask))
            return ifp;
    }

    // Through the first gateway
    for (uint32_t i = 0; i < netif_count_locked(); i++) {
        struct NetIf* ifp = netif_get_locked(i);
        if ((ifp->flags & NETIF_LOOPBACK) || !netif_usable(ifp) || ifp->ip == 0 || ifp->gateway == 0)
            continue;
        *next_hop = ifp->gateway;
        return ifp;
    }
    return NULL;
}

// The address ip_output would send from for `dst`, 0 if there is no route
static uint32_t ip_pick_source(const struct NetIf* ifp, uint32_t dst) {
    return (ifp->flags & NETIF_LOOPBACK) && (dst >> 24) != 127 ? dst : ifp->ip;
}

uint32_t ip_source_for(uint32_t dst) {
    uint32_t next_hop;
    struct NetIf* ifp = ip_route(dst, &next_hop);
    return (ifp != NULL && netif_usable(ifp)) ? ip_pick_source(ifp, dst) : 0;
}

int ip_output(uint32_t src, uint32_t dst, uint8_t protocol, const uint8_t* payload, uint32_t length,
              struct NetIf* ifp_hint) {
    uint32_t next_hop = dst;
    struct NetIf* ifp = ifp_hint;
    if (ifp == NULL) {
        if (dst == IP_BROADCAST || dst == 0)
            return NET_ERR_NOROUTE;     // Limited broadcast needs an interface named
        ifp = ip_route(dst, &next_hop);
        if (ifp == NULL)
            return NET_ERR_NOROUTE;
    }
    if (!netif_usable(ifp))
        return NET_ERR_DOWN;
    if (length > (uint32_t)ifp->mtu - IP_HEADER_LEN)
        return NET_ERR_TOOBIG;          // No fragmentation: DF is always set

    if (src == 0 && ifp_hint == NULL)
        src = ip_pick_source(ifp, dst);

    uint32_t total = IP_HEADER_LEN + length;
    uint8_t* packet = (uint8_t*)malloc(total);
    if (packet == NULL)
        return NET_ERR_NOMEM;

    // Identification: random per packet (RFC 6864 allows any value with DF
    // set and no fragments). A counter, even from a random start, tells
    // anyone how much this host sends and makes it an idle-scan "zombie".
    uint8_t* h = packet;
    h[0] = 0x45;                        // Version 4, 5 words
    h[1] = 0;
    net_put16(h + 2, (uint16_t)total);
    net_put16(h + 4, (uint16_t)net_random());
    net_put16(h + 6, IP_FLAG_DF);
    h[8] = IP_TTL;
    h[9] = protocol;
    net_put16(h + 10, 0);
    net_put32(h + 12, src);
    net_put32(h + 16, dst);
    net_put16(h + 10, net_fold(net_sum(h, IP_HEADER_LEN, 0)));
    memcpy(packet + IP_HEADER_LEN, payload, length);

    int result = (ifp->flags & NETIF_LOOPBACK) ? netif_transmit(ifp, packet, total)
                                               : eth_output_ip(ifp, next_hop, packet, total);
    free(packet);
    return result;
}

//
// In
//

void ip_input(struct NetIf* ifp, const uint8_t* p, uint32_t length) {
    if (length < IP_HEADER_LEN || (p[0] >> 4) != 4)
        return;
    uint32_t header_length = (uint32_t)(p[0] & 0x0F) * 4;
    uint32_t total = net_get16(p + 2);
    if (header_length < IP_HEADER_LEN || header_length > length)
        return;
    if (total < header_length || total > length)
        return;                         // Shorter than it says: truncated or forged
    length = total;                     // Anything past it is link padding

    if (net_fold(net_sum(p, header_length, 0)) != 0)
        return;
    if (net_get16(p + 6) & IP_FRAG_MASK)
        return;

    uint8_t  protocol = p[9];
    uint32_t src = net_get32(p + 12);
    uint32_t dst = net_get32(p + 16);
    bool loop = (ifp->flags & NETIF_LOOPBACK) != 0;

    if (loop) {
        // Only what this host sent itself comes in here
        if (net_owner_of(dst) == NULL && (dst >> 24) != 127)
            return;
    } else {
        if (net_ip_martian(src) || net_owner_of(src) != NULL)
            return;
        bool to_us = ifp->ip != 0 && dst == ifp->ip;
        bool to_all = net_is_broadcast_for(ifp, dst);
        bool leasing = ifp->ip == 0 && (ifp->flags & NETIF_DHCP);   // udp_input lets DHCP only
        if (!to_us && !to_all && !leasing)
            return;
    }

    const uint8_t* payload = p + header_length;
    uint32_t payload_length = length - header_length;
    if (protocol == IP_PROTO_ICMP)
        icmp_input(ifp, src, dst, payload, payload_length);
    else if (protocol == IP_PROTO_UDP)
        udp_input(ifp, src, dst, payload, payload_length);
    else if (protocol == IP_PROTO_TCP && !net_is_broadcast_for(ifp, dst) && (loop || ifp->ip != 0))
        tcp_input(ifp, src, dst, payload, payload_length);     // Never broadcast, never while leasing
}

//
// ICMP
//

struct PingWaiter {
    bool     used;
    bool     answered;
    uint32_t ip;
    uint16_t id;
    uint16_t sequence;
};

static struct PingWaiter waiters[PING_WAITERS];

// Token bucket for echo replies
static uint32_t icmp_tokens = ICMP_BURST;
static uint64_t icmp_refill = 0;

static bool icmp_allow_reply(void) {
    uint64_t now = net_now_ms();
    if (now - icmp_refill >= 1000 / ICMP_RATE_PER_SEC) {
        uint64_t add = (now - icmp_refill) / (1000 / ICMP_RATE_PER_SEC);
        icmp_tokens = (uint32_t)((icmp_tokens + add > ICMP_BURST) ? ICMP_BURST : icmp_tokens + add);
        icmp_refill = now;
    }
    if (icmp_tokens == 0)
        return false;
    icmp_tokens--;
    return true;
}

void icmp_input(struct NetIf* ifp, uint32_t src, uint32_t dst, const uint8_t* data, uint32_t length) {
    if (length < ICMP_HEADER_LEN || net_fold(net_sum(data, length, 0)) != 0)
        return;
    if (!(ifp->flags & NETIF_LOOPBACK) && ifp->ip == 0)
        return;     // Still leasing: not on the network yet
    uint8_t type = data[0];

    if (type == ICMP_ECHO_REQUEST && data[1] == 0) {
        // Never to a broadcast (smurf), and only while the bucket allows
        if (net_is_broadcast_for(ifp, dst) || !icmp_allow_reply())
            return;
        uint8_t* reply = (uint8_t*)malloc(length);
        if (reply == NULL)
            return;
        memcpy(reply, data, length);
        reply[0] = ICMP_ECHO_REPLY;
        net_put16(reply + 2, 0);
        net_put16(reply + 2, net_fold(net_sum(reply, length, 0)));
        ip_output(dst, src, IP_PROTO_ICMP, reply, length, NULL);
        free(reply);
        return;
    }

    if (type == ICMP_ECHO_REPLY) {
        uint16_t id = net_get16(data + 4);
        uint16_t sequence = net_get16(data + 6);
        for (uint32_t i = 0; i < PING_WAITERS; i++) {
            struct PingWaiter* w = &waiters[i];
            if (w->used && !w->answered && w->ip == src && w->id == id && w->sequence == sequence)
                w->answered = true;
        }
    }
    // Everything else (unreachable, redirect, ...) is ignored
}

int net_ping(uint32_t ip, uint16_t sequence, uint32_t timeout_ms) {
    if (ip == 0 || ip == IP_BROADCAST || (ip >> 28) >= 0xE)
        return NET_ERR_PARAM;

    uint8_t request[ICMP_HEADER_LEN + PING_PAYLOAD];
    request[0] = ICMP_ECHO_REQUEST;
    request[1] = 0;
    net_put16(request + 2, 0);
    for (uint32_t i = 0; i < PING_PAYLOAD; i++)
        request[ICMP_HEADER_LEN + i] = (uint8_t)('a' + i % 23);

    net_lock();
    struct PingWaiter* w = NULL;
    for (uint32_t i = 0; i < PING_WAITERS && w == NULL; i++)
        if (!waiters[i].used)
            w = &waiters[i];
    if (w == NULL) {
        net_unlock();
        return NET_ERR_AGAIN;
    }
    w->used     = true;
    w->answered = false;
    w->ip       = ip;
    w->id       = (uint16_t)net_random();
    w->sequence = sequence;

    net_put16(request + 4, w->id);
    net_put16(request + 6, sequence);
    net_put16(request + 2, net_fold(net_sum(request, sizeof(request), 0)));

    uint64_t start = net_now_ms();
    int result = ip_output(0, ip, IP_PROTO_ICMP, request, sizeof(request), NULL);
    net_unlock();

    while (result == NET_OK) {
        thread_sleep(PING_POLL_MS);
        net_lock();
        bool answered = w->answered;
        net_unlock();
        uint64_t elapsed = net_now_ms() - start;
        if (answered) {
            result = (int)elapsed;
            break;
        }
        if (elapsed >= timeout_ms) {
            result = NET_ERR_TIMEOUT;
            break;
        }
    }

    net_lock();
    w->used = false;
    net_unlock();
    return result;
}
