#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/net/net.h>
#include <kernel/net/net_internal.h>

#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// Ethernet framing and ARP (RFC 826)
//
// The table is small and fixed. To keep a flood of forged ARP from filling
// it or redirecting traffic:
//   - a new entry is made only for an address we asked for, or for a host
//     that asks for us (RFC 826's "am I the target" rule); anything else
//     can only refresh an entry already there
//   - senders with a martian address, a multicast / broadcast / zero MAC,
//     or our own address are ignored
//   - every entry expires, unanswered requests quickly
//

#define ARP_ENTRIES         16
#define ARP_TTL_MS          (5 * 60 * 1000)     // Complete entries
#define ARP_RETRY_MS        1000                // Between requests
#define ARP_TRIES           3                   // Before giving up on a neighbour

#define ARP_PACKET_LEN      28
#define ARP_OP_REQUEST      1
#define ARP_OP_REPLY        2

struct ArpEntry {
    struct NetIf* ifp;              // NULL: free
    uint32_t ip;
    uint8_t  mac[NET_MAC_LEN];
    bool     complete;
    uint8_t  tries;
    uint64_t stamp;                 // Learned, or last request sent

    // One IP packet waiting for the answer (the latest wins)
    uint8_t* pending;
    uint32_t pending_length;
};

static struct ArpEntry table[ARP_ENTRIES];

static const uint8_t mac_broadcast[NET_MAC_LEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool mac_unicast(const uint8_t* mac) {
    bool zero = true;
    for (int i = 0; i < NET_MAC_LEN; i++)
        if (mac[i] != 0)
            zero = false;
    return !zero && !(mac[0] & 0x01);   // Group bit clear: not multicast / broadcast
}

static void arp_drop_pending(struct ArpEntry* e) {
    free(e->pending);
    e->pending = NULL;
    e->pending_length = 0;
}

static void arp_free_entry(struct ArpEntry* e) {
    arp_drop_pending(e);
    memset(e, 0, sizeof(*e));
}

static struct ArpEntry* arp_lookup(const struct NetIf* ifp, uint32_t ip) {
    for (uint32_t i = 0; i < ARP_ENTRIES; i++)
        if (table[i].ifp == ifp && table[i].ip == ip)
            return &table[i];
    return NULL;
}

// A slot for a new entry: a free one, else the oldest (incomplete first)
static struct ArpEntry* arp_new(struct NetIf* ifp, uint32_t ip) {
    struct ArpEntry* victim = NULL;
    for (uint32_t i = 0; i < ARP_ENTRIES; i++) {
        struct ArpEntry* e = &table[i];
        if (e->ifp == NULL) {
            victim = e;
            break;
        }
        if (victim == NULL || (!e->complete && victim->complete) ||
            (e->complete == victim->complete && e->stamp < victim->stamp))
            victim = e;
    }
    arp_free_entry(victim);
    victim->ifp = ifp;
    victim->ip  = ip;
    return victim;
}

//
// Frames out
//

// Ethernet header in front of `payload`, padded to the 60-byte minimum
static int eth_send(struct NetIf* ifp, const uint8_t* dst, uint16_t type, const uint8_t* payload, uint32_t length) {
    if (length > ifp->mtu)
        return NET_ERR_TOOBIG;

    uint32_t frame_length = ETH_HEADER_LEN + length;
    if (frame_length < ETH_MIN_FRAME)
        frame_length = ETH_MIN_FRAME;

    uint8_t* frame = (uint8_t*)malloc(frame_length);
    if (frame == NULL)
        return NET_ERR_NOMEM;
    memset(frame, 0, frame_length);     // Padding never carries old memory
    memcpy(frame, dst, NET_MAC_LEN);
    memcpy(frame + 6, ifp->mac, NET_MAC_LEN);
    net_put16(frame + 12, type);
    memcpy(frame + ETH_HEADER_LEN, payload, length);

    int result = netif_transmit(ifp, frame, frame_length);
    free(frame);
    return result;
}

static int arp_send(struct NetIf* ifp, uint16_t op, const uint8_t* target_mac, uint32_t target_ip) {
    uint8_t p[ARP_PACKET_LEN];
    net_put16(p + 0, 1);                // Ethernet
    net_put16(p + 2, ETH_TYPE_IPV4);
    p[4] = NET_MAC_LEN;
    p[5] = 4;
    net_put16(p + 6, op);
    memcpy(p + 8, ifp->mac, NET_MAC_LEN);
    net_put32(p + 14, ifp->ip);
    if (op == ARP_OP_REPLY)
        memcpy(p + 18, target_mac, NET_MAC_LEN);
    else
        memset(p + 18, 0, NET_MAC_LEN);
    net_put32(p + 24, target_ip);
    return eth_send(ifp, op == ARP_OP_REPLY ? target_mac : mac_broadcast, ETH_TYPE_ARP, p, sizeof(p));
}

int eth_output_ip(struct NetIf* ifp, uint32_t next_hop, const uint8_t* packet, uint32_t length) {
    if (next_hop == IP_BROADCAST || net_is_broadcast_for(ifp, next_hop))
        return eth_send(ifp, mac_broadcast, ETH_TYPE_IPV4, packet, length);
    if (ifp->ip == 0)
        return NET_ERR_DOWN;

    struct ArpEntry* e = arp_lookup(ifp, next_hop);
    if (e != NULL && e->complete)
        return eth_send(ifp, e->mac, ETH_TYPE_IPV4, packet, length);

    // Hold it for the answer
    if (e == NULL)
        e = arp_new(ifp, next_hop);
    uint8_t* copy = (uint8_t*)malloc(length);
    if (copy == NULL)
        return NET_ERR_NOMEM;
    memcpy(copy, packet, length);
    arp_drop_pending(e);
    e->pending = copy;
    e->pending_length = length;

    if (e->tries == 0) {
        e->tries = 1;
        e->stamp = net_now_ms();
        arp_send(ifp, ARP_OP_REQUEST, NULL, next_hop);
    }
    return NET_OK;
}

//
// Frames in
//

static void arp_input(struct NetIf* ifp, const uint8_t* p, uint32_t length) {
    if (length < ARP_PACKET_LEN)
        return;
    if (net_get16(p) != 1 || net_get16(p + 2) != ETH_TYPE_IPV4 || p[4] != NET_MAC_LEN || p[5] != 4)
        return;

    uint16_t op = net_get16(p + 6);
    const uint8_t* sender_mac = p + 8;
    uint32_t sender_ip = net_get32(p + 14);
    uint32_t target_ip = net_get32(p + 24);
    if (op != ARP_OP_REQUEST && op != ARP_OP_REPLY)
        return;
    if (!mac_unicast(sender_mac) || memcmp(sender_mac, ifp->mac, NET_MAC_LEN) == 0)
        return;
    if (net_ip_martian(sender_ip) || net_owner_of(sender_ip) != NULL || ifp->ip == 0)
        return;
    // Only neighbours on this interface's subnet
    if ((sender_ip & ifp->netmask) != (ifp->ip & ifp->netmask))
        return;

    bool for_us = target_ip == ifp->ip;
    struct ArpEntry* e = arp_lookup(ifp, sender_ip);
    if (e == NULL && for_us)
        e = arp_new(ifp, sender_ip);

    if (e != NULL) {
        memcpy(e->mac, sender_mac, NET_MAC_LEN);
        e->complete = true;
        e->tries = 0;
        e->stamp = net_now_ms();
        if (e->pending != NULL) {
            uint8_t* packet = e->pending;
            uint32_t packet_length = e->pending_length;
            e->pending = NULL;
            e->pending_length = 0;
            eth_send(ifp, e->mac, ETH_TYPE_IPV4, packet, packet_length);
            free(packet);
        }
    }

    if (op == ARP_OP_REQUEST && for_us)
        arp_send(ifp, ARP_OP_REPLY, sender_mac, sender_ip);
}

void eth_input(struct NetIf* ifp, const uint8_t* frame, uint32_t length) {
    if (length < ETH_HEADER_LEN)
        return;

    const uint8_t* dst = frame;
    const uint8_t* src = frame + 6;
    uint16_t type = net_get16(frame + 12);

    // Ours or broadcast; a frame claiming to come from a group address, or
    // from us, is forged
    bool to_us = memcmp(dst, ifp->mac, NET_MAC_LEN) == 0;
    bool to_all = memcmp(dst, mac_broadcast, NET_MAC_LEN) == 0;
    if (!to_us && !to_all)
        return;
    if (!mac_unicast(src) || memcmp(src, ifp->mac, NET_MAC_LEN) == 0)
        return;

    const uint8_t* payload = frame + ETH_HEADER_LEN;
    uint32_t payload_length = length - ETH_HEADER_LEN;
    if (type == ETH_TYPE_ARP)
        arp_input(ifp, payload, payload_length);
    else if (type == ETH_TYPE_IPV4)
        ip_input(ifp, payload, payload_length);
    // 802.1Q, IPv6 and the rest: dropped
}

//
// Upkeep
//

void arp_timer(void) {
    uint64_t now = net_now_ms();
    for (uint32_t i = 0; i < ARP_ENTRIES; i++) {
        struct ArpEntry* e = &table[i];
        if (e->ifp == NULL)
            continue;

        if (e->complete) {
            if (now - e->stamp >= ARP_TTL_MS)
                arp_free_entry(e);
            continue;
        }
        if (now - e->stamp < ARP_RETRY_MS)
            continue;
        if (e->tries >= ARP_TRIES || !netif_usable(e->ifp)) {
            arp_free_entry(e);  // No answer: the packet waiting is dropped
            continue;
        }
        e->tries++;
        e->stamp = now;
        arp_send(e->ifp, ARP_OP_REQUEST, NULL, e->ip);
    }
}

void arp_flush_if(const struct NetIf* ifp) {
    for (uint32_t i = 0; i < ARP_ENTRIES; i++)
        if (table[i].ifp == ifp)
            arp_free_entry(&table[i]);
}

uint32_t arp_snapshot(struct ArpView* out, uint32_t max) {
    uint32_t count = 0;
    net_lock();
    for (uint32_t i = 0; i < ARP_ENTRIES && count < max; i++) {
        const struct ArpEntry* e = &table[i];
        if (e->ifp == NULL)
            continue;
        struct ArpView* v = &out[count++];
        memset(v, 0, sizeof(*v));
        v->ip = e->ip;
        memcpy(v->mac, e->mac, NET_MAC_LEN);
        v->complete = e->complete;
        strncpy(v->ifname, e->ifp->name, NETIF_NAME_MAX - 1);
    }
    net_unlock();
    return count;
}
