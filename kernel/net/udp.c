#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/net/net.h>
#include <kernel/net/net_internal.h>

#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// UDP (RFC 768) for kernel code
//
// The length field must fit inside what IP delivered, a non-zero checksum
// must be right, and each socket queues at most UDP_QUEUE_MAX datagrams
// (the rest are dropped). Datagrams to a port nobody holds are dropped
// silently: no ICMP port unreachable, so scans learn nothing.
//

#define UDP_SOCKETS         16
#define UDP_EPHEMERAL_LOW   49152
#define DHCP_CLIENT_PORT    68

struct UdpDatagram {
    uint32_t ip;
    uint16_t port;
    uint16_t length;
    uint8_t* data;
};

struct UdpSocket {
    uint16_t port;
    uint8_t  head;                  // Oldest queued datagram
    uint8_t  count;
    struct UdpDatagram queue[UDP_QUEUE_MAX];
};

static struct UdpSocket* sockets[UDP_SOCKETS];

static struct UdpSocket* udp_find(uint16_t port) {
    for (uint32_t i = 0; i < UDP_SOCKETS; i++)
        if (sockets[i] != NULL && sockets[i]->port == port)
            return sockets[i];
    return NULL;
}

static uint32_t udp_pseudo_sum(uint32_t src, uint32_t dst, uint32_t length) {
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + IP_PROTO_UDP + length;
}

//
// In
//

void udp_input(struct NetIf* ifp, uint32_t src, uint32_t dst, const uint8_t* data, uint32_t length) {
    if (length < UDP_HEADER_LEN)
        return;
    uint16_t src_port = net_get16(data);
    uint16_t dst_port = net_get16(data + 2);
    uint32_t udp_length = net_get16(data + 4);
    uint16_t checksum = net_get16(data + 6);
    if (udp_length < UDP_HEADER_LEN || udp_length > length || src_port == 0 || dst_port == 0)
        return;
    length = udp_length;

    if (checksum != 0 && net_fold(net_sum(data, length, udp_pseudo_sum(src, dst, length))) != 0)
        return;

    const uint8_t* payload = data + UDP_HEADER_LEN;
    uint32_t payload_length = length - UDP_HEADER_LEN;
    bool loop = (ifp->flags & NETIF_LOOPBACK) != 0;

    if (dst_port == DHCP_CLIENT_PORT) {
        if (!loop)
            dhcp_input(ifp, src, payload, payload_length);
        return;
    }
    if (!loop && ifp->ip == 0)
        return;     // Leasing: only DHCP gets through

    struct UdpSocket* s = udp_find(dst_port);
    if (s == NULL || s->count >= UDP_QUEUE_MAX) {
        ifp->stats.rx_dropped++;
        return;
    }

    uint8_t* copy = NULL;
    if (payload_length != 0) {
        copy = (uint8_t*)malloc(payload_length);
        if (copy == NULL)
            return;
        memcpy(copy, payload, payload_length);
    }
    struct UdpDatagram* d = &s->queue[(s->head + s->count) % UDP_QUEUE_MAX];
    d->ip     = src;
    d->port   = src_port;
    d->length = (uint16_t)payload_length;
    d->data   = copy;
    s->count++;
}

//
// Out
//

int udp_send_raw(struct NetIf* ifp, uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port,
                 const void* data, uint32_t length) {
    if (length > UDP_PAYLOAD_MAX || src_port == 0 || dst_port == 0)
        return NET_ERR_PARAM;

    uint32_t total = UDP_HEADER_LEN + length;
    uint8_t* d = (uint8_t*)malloc(total);
    if (d == NULL)
        return NET_ERR_NOMEM;
    net_put16(d, src_port);
    net_put16(d + 2, dst_port);
    net_put16(d + 4, (uint16_t)total);
    net_put16(d + 6, 0);
    if (length != 0)
        memcpy(d + UDP_HEADER_LEN, data, length);

    uint16_t checksum = net_fold(net_sum(d, total, udp_pseudo_sum(src, dst, total)));
    net_put16(d + 6, checksum != 0 ? checksum : 0xFFFF);   // 0 means "none"

    int result = ip_output(src, dst, IP_PROTO_UDP, d, total, ifp);
    free(d);
    return result;
}

//
// Sockets
//

struct UdpSocket* udp_open(uint16_t port) {
    struct UdpSocket* s = (struct UdpSocket*)malloc(sizeof(struct UdpSocket));
    if (s == NULL)
        return NULL;
    memset(s, 0, sizeof(*s));

    net_lock();
    if (port == 0) {
        // A random free port, so the next one can't be guessed
        for (int tries = 0; tries < 64 && port == 0; tries++) {
            uint16_t candidate = (uint16_t)(UDP_EPHEMERAL_LOW + net_random() % (65536 - UDP_EPHEMERAL_LOW));
            if (udp_find(candidate) == NULL)
                port = candidate;
        }
    }
    int slot = -1;
    for (int i = 0; i < UDP_SOCKETS && slot < 0; i++)
        if (sockets[i] == NULL)
            slot = i;
    if (port == 0 || port == DHCP_CLIENT_PORT || slot < 0 || udp_find(port) != NULL) {
        net_unlock();
        free(s);
        return NULL;
    }
    s->port = port;
    sockets[slot] = s;
    net_unlock();
    return s;
}

uint16_t udp_local_port(const struct UdpSocket* s) {
    return s != NULL ? s->port : 0;
}

int udp_sendto(struct UdpSocket* s, uint32_t ip, uint16_t port, const void* data, uint32_t length) {
    if (s == NULL || (length != 0 && data == NULL))
        return NET_ERR_PARAM;
    if (ip == 0 || ip == IP_BROADCAST || (ip >> 28) >= 0xE)
        return NET_ERR_PARAM;       // Unicast only, for now

    net_lock();
    uint32_t src = ip_source_for(ip);
    int result = src != 0 ? udp_send_raw(NULL, src, s->port, ip, port, data, length) : NET_ERR_NOROUTE;
    net_unlock();
    return result < 0 ? result : (int)length;
}

int udp_recvfrom(struct UdpSocket* s, void* buffer, uint32_t size, uint32_t* ip, uint16_t* port) {
    if (s == NULL || (size != 0 && buffer == NULL))
        return NET_ERR_PARAM;

    net_lock();
    if (s->count == 0) {
        net_unlock();
        return NET_ERR_AGAIN;
    }
    struct UdpDatagram* d = &s->queue[s->head];
    uint32_t n = d->length < size ? d->length : size;
    if (n != 0)
        memcpy(buffer, d->data, n);
    if (ip != NULL)   *ip = d->ip;
    if (port != NULL) *port = d->port;
    free(d->data);
    memset(d, 0, sizeof(*d));
    s->head = (uint8_t)((s->head + 1) % UDP_QUEUE_MAX);
    s->count--;
    net_unlock();
    return (int)n;
}

void udp_close(struct UdpSocket* s) {
    if (s == NULL)
        return;
    net_lock();
    for (uint32_t i = 0; i < UDP_SOCKETS; i++)
        if (sockets[i] == s)
            sockets[i] = NULL;
    for (uint32_t i = 0; i < UDP_QUEUE_MAX; i++)
        free(s->queue[i].data);
    net_unlock();
    free(s);
}
