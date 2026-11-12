#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/net/net.h>
#include <kernel/net/net_internal.h>

#include <kernel/util/string.h>

//
// DHCP client (RFC 2131 / 2132)
//
//   SELECTING   DISCOVER broadcast, waiting for an OFFER
//   REQUESTING  REQUEST for the offer, waiting for ACK / NAK
//   BOUND       address in use until T1 (half the lease)
//   RENEWING    REQUEST broadcast again until ACK, NAK or the lease ends
//
// Replies are checked against what we asked: our transaction ID and
// hardware address, the server we picked, the address we asked for. The
// options are walked with every length checked against the packet; only
// the handful we use are read, option overload (52) is ignored, and every
// address taken from them must be sane (no martians, a contiguous mask, a
// router inside the subnet). DHCP itself has no authentication: anyone on
// the link can answer first. That is the protocol's limit, not this code's.
//

#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68

#define DHCP_FIXED_LEN      236
#define DHCP_COOKIE         0x63825363u
#define DHCP_MIN_MESSAGE    300     // BOOTP minimum, padded

#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_ACK            5
#define DHCP_NAK            6

#define OPT_PAD             0
#define OPT_MASK            1
#define OPT_ROUTER          3
#define OPT_DNS             6
#define OPT_REQUESTED_IP    50
#define OPT_LEASE           51
#define OPT_TYPE            53
#define OPT_SERVER_ID       54
#define OPT_PARAMS          55
#define OPT_END             255

#define DHCP_RETRY_FIRST_MS 2000
#define DHCP_RETRY_MAX_MS   32000
#define DHCP_LEASE_MIN_S    60
#define DHCP_LEASE_MAX_S    (7 * 24 * 3600)     // Longer leases are renewed weekly anyway

enum DhcpState { DHCP_IDLE, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING };

struct DhcpClient {
    struct NetIf* ifp;              // NULL: free slot
    enum DhcpState state;
    uint32_t xid;
    uint32_t retry_ms;
    uint64_t next_send;
    bool     owns_address;          // ifp's address came from here

    uint32_t offered_ip;
    uint32_t server;
    uint64_t renew_at;
    uint64_t expires_at;
};

static struct DhcpClient clients[NETIF_MAX];

struct DhcpReply {
    uint8_t  type;
    uint32_t yiaddr;
    uint32_t mask;
    uint32_t router;
    uint32_t dns;
    uint32_t lease;
    uint32_t server;
};

static struct DhcpClient* dhcp_client(const struct NetIf* ifp) {
    for (uint32_t i = 0; i < NETIF_MAX; i++)
        if (clients[i].ifp == ifp)
            return &clients[i];
    return NULL;
}

//
// Sending
//

static uint8_t* opt_add(uint8_t* p, uint8_t code, const void* data, uint8_t length) {
    p[0] = code;
    p[1] = length;
    memcpy(p + 2, data, length);
    return p + 2 + length;
}

static void dhcp_send(struct DhcpClient* c, uint8_t type) {
    struct NetIf* ifp = c->ifp;
    uint8_t m[DHCP_MIN_MESSAGE + 64];
    memset(m, 0, sizeof(m));

    bool renewing = c->state == DHCP_RENEWING;
    m[0] = 1;                       // BOOTREQUEST
    m[1] = 1;                       // Ethernet
    m[2] = NET_MAC_LEN;
    net_put32(m + 4, c->xid);
    net_put16(m + 10, renewing ? 0 : 0x8000);   // Broadcast replies until we have an address
    if (renewing)
        net_put32(m + 12, ifp->ip); // ciaddr
    memcpy(m + 28, ifp->mac, NET_MAC_LEN);
    net_put32(m + DHCP_FIXED_LEN, DHCP_COOKIE);

    uint8_t* p = m + DHCP_FIXED_LEN + 4;
    p = opt_add(p, OPT_TYPE, &type, 1);
    if (type == DHCP_REQUEST && !renewing) {
        uint8_t ip[4], server[4];
        net_put32(ip, c->offered_ip);
        net_put32(server, c->server);
        p = opt_add(p, OPT_REQUESTED_IP, ip, 4);
        p = opt_add(p, OPT_SERVER_ID, server, 4);
    }
    static const uint8_t params[] = { OPT_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE, OPT_SERVER_ID };
    p = opt_add(p, OPT_PARAMS, params, sizeof(params));
    *p++ = OPT_END;

    uint32_t length = (uint32_t)(p - m);
    if (length < DHCP_MIN_MESSAGE)
        length = DHCP_MIN_MESSAGE;
    udp_send_raw(ifp, renewing ? ifp->ip : 0, DHCP_CLIENT_PORT, IP_BROADCAST, DHCP_SERVER_PORT, m, length);
}

// Send now, and schedule the next try with exponential backoff and jitter
static void dhcp_send_scheduled(struct DhcpClient* c, uint8_t type) {
    dhcp_send(c, type);
    c->next_send = net_now_ms() + c->retry_ms + net_random() % 1000;
    if (c->retry_ms < DHCP_RETRY_MAX_MS)
        c->retry_ms *= 2;
}

static void dhcp_begin(struct DhcpClient* c) {
    c->state      = DHCP_SELECTING;
    c->xid        = net_random();
    c->retry_ms   = DHCP_RETRY_FIRST_MS;
    c->offered_ip = 0;
    c->server     = 0;
    if (netif_usable(c->ifp))
        dhcp_send_scheduled(c, DHCP_DISCOVER);
    else
        c->next_send = net_now_ms();
}

static void dhcp_drop_address(struct DhcpClient* c) {
    if (c->owns_address) {
        struct NetIf* ifp = c->ifp;
        arp_flush_if(ifp);
        ifp->ip = ifp->netmask = ifp->gateway = ifp->dns = 0;
        c->owns_address = false;
    }
}

//
// Receiving
//

static bool dhcp_mask_ok(uint32_t mask) {
    uint32_t inverted = ~mask;
    return mask != 0 && (inverted & (inverted + 1)) == 0 && inverted > 1;   // Room for hosts
}

// Walk the options. False if they run past the packet or lack a type.
static bool dhcp_parse(const uint8_t* p, uint32_t length, struct DhcpReply* r) {
    memset(r, 0, sizeof(*r));
    r->yiaddr = net_get32(p + 16);

    uint32_t i = DHCP_FIXED_LEN + 4;
    while (i < length) {
        uint8_t code = p[i];
        if (code == OPT_PAD) {
            i++;
            continue;
        }
        if (code == OPT_END)
            break;
        if (i + 2 > length)
            return false;
        uint8_t olen = p[i + 1];
        if (i + 2 + olen > length)
            return false;
        const uint8_t* v = p + i + 2;

        switch (code) {
            case OPT_TYPE:      if (olen == 1) r->type = v[0]; break;
            case OPT_MASK:      if (olen == 4) r->mask = net_get32(v); break;
            case OPT_ROUTER:    if (olen >= 4 && olen % 4 == 0) r->router = net_get32(v); break;
            case OPT_DNS:       if (olen >= 4 && olen % 4 == 0) r->dns = net_get32(v); break;
            case OPT_LEASE:     if (olen == 4) r->lease = net_get32(v); break;
            case OPT_SERVER_ID: if (olen == 4) r->server = net_get32(v); break;
            default:            break;      // Everything else is skipped, overload (52) included
        }
        i += 2 + olen;
    }
    return r->type != 0;
}

// Fill in what the server left out, and refuse what can't be used
static bool dhcp_sane(struct DhcpReply* r) {
    if (net_ip_martian(r->yiaddr) || net_owner_of(r->yiaddr) != NULL)
        return false;
    if (r->server == 0 || net_ip_martian(r->server))
        return false;
    if (!dhcp_mask_ok(r->mask))
        r->mask = 0xFFFFFF00u;      // No (usable) mask: assume a /24
    if ((r->yiaddr & ~r->mask) == ~r->mask)
        return false;               // The subnet's broadcast address
    if (r->router != 0 && (net_ip_martian(r->router) || (r->router & r->mask) != (r->yiaddr & r->mask) ||
                           r->router == r->yiaddr))
        r->router = 0;              // A router we can't reach directly is no router
    if (r->dns != 0 && net_ip_martian(r->dns))
        r->dns = 0;
    if (r->lease < DHCP_LEASE_MIN_S)
        r->lease = DHCP_LEASE_MIN_S;
    if (r->lease > DHCP_LEASE_MAX_S)
        r->lease = DHCP_LEASE_MAX_S;
    return true;
}

static void dhcp_bind(struct DhcpClient* c, const struct DhcpReply* r) {
    struct NetIf* ifp = c->ifp;
    if (ifp->ip != r->yiaddr)
        arp_flush_if(ifp);
    ifp->ip      = r->yiaddr;
    ifp->netmask = r->mask;
    ifp->gateway = r->router;
    ifp->dns     = r->dns;
    c->owns_address = true;

    uint64_t now = net_now_ms();
    c->state      = DHCP_BOUND;
    c->retry_ms   = DHCP_RETRY_FIRST_MS;
    c->renew_at   = now + (uint64_t)r->lease * 500;    // T1: half the lease
    c->expires_at = now + (uint64_t)r->lease * 1000;
}

void dhcp_input(struct NetIf* ifp, uint32_t src, const uint8_t* p, uint32_t length) {
    (void)src;
    struct DhcpClient* c = dhcp_client(ifp);
    if (c == NULL || c->state == DHCP_IDLE || c->state == DHCP_BOUND)
        return;
    if (length < DHCP_FIXED_LEN + 4)
        return;
    if (p[0] != 2 || p[1] != 1 || p[2] != NET_MAC_LEN)
        return;     // Not a BOOTREPLY for Ethernet
    if (net_get32(p + 4) != c->xid || memcmp(p + 28, ifp->mac, NET_MAC_LEN) != 0)
        return;     // Not an answer to us
    if (net_get32(p + DHCP_FIXED_LEN) != DHCP_COOKIE)
        return;

    struct DhcpReply r;
    if (!dhcp_parse(p, length, &r))
        return;

    if (c->state == DHCP_SELECTING && r.type == DHCP_OFFER) {
        if (!dhcp_sane(&r))
            return;
        c->offered_ip = r.yiaddr;
        c->server     = r.server;
        c->state      = DHCP_REQUESTING;
        c->retry_ms   = DHCP_RETRY_FIRST_MS;
        dhcp_send_scheduled(c, DHCP_REQUEST);
        return;
    }

    if (c->state != DHCP_REQUESTING && c->state != DHCP_RENEWING)
        return;
    // Only the server we chose (RENEWING: the one that leased it)
    if (r.server != c->server)
        return;

    if (r.type == DHCP_NAK) {
        dhcp_drop_address(c);
        dhcp_begin(c);
        return;
    }
    if (r.type == DHCP_ACK) {
        bool renewing = c->state == DHCP_RENEWING;
        uint32_t want = renewing ? ifp->ip : c->offered_ip;
        if (r.yiaddr != want) {
            // Renewing: the server moved us. Start over cleanly.
            if (renewing) {
                dhcp_drop_address(c);
                dhcp_begin(c);
            }
            return;
        }

        // While renewing, our own address must not count as taken
        uint32_t saved = ifp->ip;
        if (renewing)
            ifp->ip = 0;
        bool ok = dhcp_sane(&r);
        ifp->ip = saved;
        if (ok)
            dhcp_bind(c, &r);
    }
}

//
// Control (net lock held)
//

void dhcp_start(struct NetIf* ifp) {
    if (ifp == NULL || (ifp->flags & NETIF_LOOPBACK))
        return;
    struct DhcpClient* c = dhcp_client(ifp);
    if (c == NULL) {
        c = dhcp_client(NULL);
        if (c == NULL)
            return;
        memset(c, 0, sizeof(*c));
        c->ifp = ifp;
    }
    ifp->flags |= NETIF_DHCP;
    dhcp_begin(c);
}

void dhcp_stop(struct NetIf* ifp) {
    struct DhcpClient* c = dhcp_client(ifp);
    if (c != NULL) {
        dhcp_drop_address(c);
        memset(c, 0, sizeof(*c));
    }
    if (ifp != NULL)
        ifp->flags &= (uint16_t)~NETIF_DHCP;
}

void dhcp_timer(void) {
    uint64_t now = net_now_ms();
    for (uint32_t i = 0; i < NETIF_MAX; i++) {
        struct DhcpClient* c = &clients[i];
        if (c->ifp == NULL || c->state == DHCP_IDLE)
            continue;
        if (!netif_usable(c->ifp))
            continue;

        switch (c->state) {
            case DHCP_SELECTING:
                if (now >= c->next_send)
                    dhcp_send_scheduled(c, DHCP_DISCOVER);
                break;
            case DHCP_REQUESTING:
                // An offer that is never confirmed: back to the start
                if (now >= c->next_send) {
                    if (c->retry_ms > DHCP_RETRY_FIRST_MS * 8)
                        dhcp_begin(c);
                    else
                        dhcp_send_scheduled(c, DHCP_REQUEST);
                }
                break;
            case DHCP_BOUND:
                if (now >= c->renew_at) {
                    c->state    = DHCP_RENEWING;
                    c->xid      = net_random();
                    c->retry_ms = DHCP_RETRY_FIRST_MS;
                    dhcp_send_scheduled(c, DHCP_REQUEST);
                }
                break;
            case DHCP_RENEWING:
                if (now >= c->expires_at) {
                    dhcp_drop_address(c);
                    dhcp_begin(c);
                } else if (now >= c->next_send) {
                    dhcp_send_scheduled(c, DHCP_REQUEST);
                }
                break;
            default:
                break;
        }
    }
}
