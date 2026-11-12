#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/net/net.h>
#include <kernel/net/net_internal.h>

#include <kernel/scheduler/scheduler.h>
#include <kernel/util/string.h>

//
// DNS stub resolver (RFC 1035): host name -> IPv4 address
//
// Asks the DNS server DHCP gave an interface (NetIf.dns), and that
// interface's gateway when there is none (most home routers answer DNS),
// one A query over UDP port 53, recursion desired. Retries every
// DNS_RETRY_MS, taking turns between the servers it found, until an answer
// or the timeout.
//
// What comes back is untrusted, like everything else on the wire. An
// answer is taken only when:
//   - it comes from one of the servers asked, from port 53, to the
//     random port we sent from, with our random 16-bit id
//   - it is a response to a standard query with exactly our question in it
//     (name compared without regard to case, type A, class IN)
//   - every length in it fits inside the datagram; compressed names are
//     followed at most DNS_POINTER_MAX times, and only backwards, so a
//     loop of pointers ends
// The first A record of class IN with 4 bytes of data in the answer section
// is the result (after any CNAMEs the server put first). Nothing is cached.
//

#define DNS_PORT            53
#define DNS_HEADER_LEN      12
#define DNS_QUERY_MAX       (DNS_HEADER_LEN + DNS_NAME_MAX + 2 + 4)
#define DNS_REPLY_MAX       512             // Plain UDP DNS (no EDNS asked for)
#define DNS_RETRY_MS        1000
#define DNS_POLL_MS         10
#define DNS_SERVERS_MAX     (NETIF_MAX * 2)
#define DNS_POINTER_MAX     16

#define DNS_TYPE_A          1
#define DNS_CLASS_IN        1

#define DNS_FLAG_QR         0x8000          // A response
#define DNS_FLAG_TC         0x0200          // Truncated
#define DNS_FLAG_RD         0x0100          // Recursion desired
#define DNS_OPCODE_MASK     0x7800
#define DNS_RCODE_MASK      0x000F
#define DNS_RCODE_NXDOMAIN  3

static char dns_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// "www.example.com" -> 3www7example3com0. Returns the encoded length, 0
// for a name that isn't one: empty, too long, an empty or over-long label,
// or a character other than letters, digits, '-' and '_'. One trailing dot
// is allowed ("example.com.").
static uint32_t dns_encode_name(const char* name, uint8_t* out) {
    uint32_t length = (uint32_t)strlen(name);
    if (length > 0 && name[length - 1] == '.')
        length--;
    if (length == 0 || length > DNS_NAME_MAX - 1)
        return 0;

    uint32_t o = 0;
    uint32_t i = 0;
    while (i < length) {
        uint32_t start = i;
        while (i < length && name[i] != '.') {
            char c = name[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_';
            if (!ok)
                return 0;
            i++;
        }
        uint32_t label = i - start;
        if (label == 0 || label > 63)
            return 0;
        out[o++] = (uint8_t)label;
        memcpy(out + o, name + start, label);
        o += label;
        if (i < length)
            i++;                    // The dot
        if (i == length && name[i - 1] == '.')
            return 0;               // "a..b" ended in an empty label
    }
    out[o++] = 0;
    return o;
}

// Step over the name at reply[*offset] (labels and/or a pointer), leaving
// *offset just past it. With `expect` (an encoded name), also compare the
// name against it without regard to case. False when the name runs off the
// end of the reply, loops, or doesn't match.
static bool dns_walk_name(const uint8_t* reply, uint32_t length, uint32_t* offset, const uint8_t* expect) {
    uint32_t at = *offset;
    uint32_t end = 0;               // Where the caller carries on: set at the first pointer
    bool jumped = false;
    uint32_t pointers = 0;
    uint32_t e = 0;

    while (1) {
        if (at >= length)
            return false;
        uint8_t b = reply[at];
        if ((b & 0xC0) == 0xC0) {
            if (at + 1 >= length || ++pointers > DNS_POINTER_MAX)
                return false;
            uint32_t target = ((uint32_t)(b & 0x3F) << 8) | reply[at + 1];
            if (target >= at)
                return false;       // Only backwards: no loops
            if (!jumped) {
                end = at + 2;
                jumped = true;
            }
            at = target;
            continue;
        }
        if ((b & 0xC0) != 0)
            return false;           // 0x40 / 0x80: extended label types, never used
        if (b == 0) {
            if (expect != NULL && expect[e] != 0)
                return false;
            *offset = jumped ? end : at + 1;
            return true;
        }
        if (at + 1 + b > length)
            return false;
        if (expect != NULL) {
            if (expect[e] != b)
                return false;
            for (uint32_t k = 0; k < b; k++)
                if (dns_lower((char)reply[at + 1 + k]) != dns_lower((char)expect[e + 1 + k]))
                    return false;
            e += 1u + b;
        }
        at += 1u + b;
    }
}

// The answer to query `id` for `qname`, or NET_ERR_* (NET_ERR_AGAIN: not an
// answer to this query at all, keep waiting)
static int dns_parse_reply(const uint8_t* reply, uint32_t length, uint16_t id, const uint8_t* qname,
                           uint32_t* out) {
    if (length < DNS_HEADER_LEN || net_get16(reply) != id)
        return NET_ERR_AGAIN;
    uint16_t flags = net_get16(reply + 2);
    uint16_t qdcount = net_get16(reply + 4);
    uint16_t ancount = net_get16(reply + 6);
    if (!(flags & DNS_FLAG_QR) || (flags & DNS_OPCODE_MASK) != 0 || qdcount != 1)
        return NET_ERR_AGAIN;

    // Our question, as asked
    uint32_t at = DNS_HEADER_LEN;
    if (!dns_walk_name(reply, length, &at, qname) || at + 4 > length ||
        net_get16(reply + at) != DNS_TYPE_A || net_get16(reply + at + 2) != DNS_CLASS_IN)
        return NET_ERR_AGAIN;
    at += 4;

    uint16_t rcode = flags & DNS_RCODE_MASK;
    if (rcode == DNS_RCODE_NXDOMAIN)
        return NET_ERR_NOTFOUND;
    if (rcode != 0)
        return NET_ERR_IO;          // SERVFAIL, REFUSED ...

    for (uint32_t i = 0; i < ancount; i++) {
        if (!dns_walk_name(reply, length, &at, NULL) || at + 10 > length)
            break;                  // Cut short (truncated reply): use what came before
        uint16_t type   = net_get16(reply + at);
        uint16_t klass  = net_get16(reply + at + 2);
        uint16_t rdlen  = net_get16(reply + at + 8);
        at += 10;
        if (at + rdlen > length)
            break;
        if (type == DNS_TYPE_A && klass == DNS_CLASS_IN && rdlen == 4) {
            uint32_t ip = net_get32(reply + at);
            if (ip == 0 || ip == 0xFFFFFFFFu || (ip >> 28) >= 0xE)
                return NET_ERR_NOTFOUND;    // Nothing we could connect to
            if ((ip >> 24) == 127 || (ip >> 24) == 0)
                return NET_ERR_NOTFOUND;    // This machine: a name off the
                                            // network never points back at
                                            // our own services (rebinding)
            *out = ip;
            return NET_OK;
        }
        at += rdlen;
    }
    // A name with no IPv4 address (only AAAA, or a CNAME to nowhere), or a
    // truncated reply that lost it
    return (flags & DNS_FLAG_TC) ? NET_ERR_IO : NET_ERR_NOTFOUND;
}

// The servers to ask: every interface's DNS server first, then gateways.
// Returns how many (0: no configured interface).
static uint32_t dns_servers(uint32_t* out) {
    uint32_t count = 0;
    struct NetIf ifp;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; netif_snapshot(i, &ifp); i++) {
            if ((ifp.flags & NETIF_LOOPBACK) || !(ifp.flags & NETIF_UP) || !(ifp.flags & NETIF_LINK) || ifp.ip == 0)
                continue;
            uint32_t server = pass == 0 ? ifp.dns : ifp.gateway;
            if (server == 0)
                continue;
            bool seen = false;
            for (uint32_t k = 0; k < count; k++)
                seen |= out[k] == server;
            if (!seen && count < DNS_SERVERS_MAX)
                out[count++] = server;
        }
    }
    return count;
}

int dns_resolve(const char* name, uint32_t* out, uint32_t timeout_ms, bool (*cancel)(void)) {
    if (name == NULL || out == NULL)
        return NET_ERR_PARAM;

    // Already an address, or ourselves
    if (net_parse_ip(name, out))
        return NET_OK;
    if (strcmp(name, "localhost") == 0 || strcmp(name, "localhost.") == 0) {
        *out = IP_LOOPBACK;
        return NET_OK;
    }

    uint8_t query[DNS_QUERY_MAX];
    uint32_t name_length = dns_encode_name(name, query + DNS_HEADER_LEN);
    if (name_length == 0)
        return NET_ERR_PARAM;

    uint32_t servers[DNS_SERVERS_MAX];
    uint32_t server_count = dns_servers(servers);
    if (server_count == 0)
        return NET_ERR_DOWN;

    struct UdpSocket* s = udp_open(0);
    if (s == NULL)
        return NET_ERR_NOMEM;

    net_lock();
    uint16_t id = (uint16_t)net_random();
    net_unlock();

    memset(query, 0, DNS_HEADER_LEN);
    net_put16(query, id);
    net_put16(query + 2, DNS_FLAG_RD);
    net_put16(query + 4, 1);        // One question
    uint32_t q = DNS_HEADER_LEN + name_length;
    net_put16(query + q, DNS_TYPE_A);
    net_put16(query + q + 2, DNS_CLASS_IN);
    uint32_t query_length = q + 4;
    const uint8_t* qname = query + DNS_HEADER_LEN;

    uint8_t reply[DNS_REPLY_MAX];
    int result = NET_ERR_TIMEOUT;
    int last_error = NET_OK;
    uint64_t start = net_now_ms();
    uint64_t next_send = start;
    uint32_t turn = 0;

    while (1) {
        uint64_t now = net_now_ms();
        if (now >= next_send) {
            uint32_t server = servers[turn++ % server_count];
            int sent = udp_sendto(s, server, DNS_PORT, query, query_length);
            if (sent < 0 && sent != NET_ERR_AGAIN)
                last_error = sent;
            else if (sent >= 0)
                last_error = NET_OK;
            next_send = now + DNS_RETRY_MS;
        }

        uint32_t from_ip;
        uint16_t from_port;
        int n;
        bool done = false;
        while ((n = udp_recvfrom(s, reply, DNS_REPLY_MAX, &from_ip, &from_port)) >= 0) {
            bool ours = false;
            for (uint32_t k = 0; k < server_count; k++)
                ours |= servers[k] == from_ip;
            if (!ours || from_port != DNS_PORT)
                continue;
            int r = dns_parse_reply(reply, (uint32_t)n, id, qname, out);
            if (r == NET_ERR_AGAIN)
                continue;
            result = r;
            done = true;
            break;
        }
        if (done)
            break;

        if (net_now_ms() - start >= timeout_ms) {
            // Nobody answered: say why if the last send failed
            result = last_error != NET_OK ? last_error : NET_ERR_TIMEOUT;
            break;
        }
        if (cancel != NULL && cancel()) {
            result = NET_ERR_TIMEOUT;
            break;
        }
        thread_sleep(DNS_POLL_MS);
    }

    udp_close(s);
    return result;
}
