#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/net/net.h>
#include <kernel/net/net_internal.h>

#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// TCP (RFC 9293), kept small:
//   - in-order delivery only: a segment that skips ahead is dropped and
//     answered with a duplicate ACK, and the sender's retransmission fills
//     the gap (no out-of-order queue to exhaust)
//   - go-back-N retransmission on a timeout (RFC 6298 RTO, Karn's rule),
//     zero-window probes, no Nagle, no delayed ACKs
//   - only the MSS option, read on SYN; nothing else is offered, so peers
//     use no window scaling, SACK or timestamps
//
// Hardening:
//   - random initial sequence numbers and ephemeral ports (RFC 6528 / 6056)
//   - RFC 5961: a RST only takes effect when its sequence number is exactly
//     the next expected one (otherwise a challenge ACK), a SYN on a live
//     connection gets a challenge ACK, and ACKs for data never sent or long
//     acknowledged are dropped
//   - a fixed connection table, a per-listener cap on half-open plus
//     not-yet-accepted connections, and short SYN-RECEIVED retries, so a SYN
//     flood costs a few table slots, not memory
//   - SYN cookies once a listener's half-open slots (or the table) are
//     full: the SYN-ACK's sequence number is a keyed hash of the connection
//     and the time, nothing is kept, and the peer's ACK proves the
//     round trip. A spoofed flood can't lock real clients out, and half-open
//     connections can't take the slots outgoing connections need.
//   - fixed 8 KB buffers per connection; the window never advertises more
//     room than the receive buffer has
//   - resets for closed ports are rate limited and never sent in answer to
//     a reset
//   - every header length is checked against the segment, the checksum
//     always (no zero-checksum shortcut in TCP)
//

#define TCP_MAX             16      // Connections + listeners, all states
#define TCP_BUFFER          8192    // Each direction, per connection
#define TCP_HEADER_LEN      20
#define TCP_MSS_DEFAULT     536     // RFC 9293 3.7.1, when the peer says nothing
#define TCP_MSS_MIN         64      // Smaller "MSS" options are refused (tiny segment floods)

#define TCP_RTO_INITIAL_MS  1000
#define TCP_RTO_MIN_MS      200
#define TCP_RTO_MAX_MS      60000
#define TCP_RETRIES         8       // Timeouts in a row before the connection is dropped
#define TCP_SYN_RETRIES     4       // For SYN and SYN-ACK
#define TCP_TIME_WAIT_MS    30000   // 2 MSL, short
#define TCP_FIN_WAIT_2_MS   60000   // Closed by us, the peer never closes its side
#define TCP_RST_PER_SEC     10      // Resets for closed ports

#define TCP_EPHEMERAL_LOW   49152

// SYN cookies: the low 3 bits carry the peer's MSS (an index into
// cookie_mss), the rest a SipHash of the 4-tuple, the peer's ISN and a
// period of 2^16 ms; a cookie is good for this period and the one before
#define TCP_COOKIE_MSS_BITS 0x7u
#define TCP_COOKIE_SHIFT    16      // now_ms >> 16: ~65 s periods

// Header flags
#define TCP_FIN             0x01
#define TCP_SYN             0x02
#define TCP_RST             0x04
#define TCP_PSH             0x08
#define TCP_ACK             0x10

#define SEQ_LT(a, b)        ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b)       ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)        ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b)       ((int32_t)((a) - (b)) >= 0)

enum TcpState {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
};

struct Ring {
    uint8_t* data;
    uint32_t start;
    uint32_t length;
};

struct TcpSocket {
    enum TcpState state;
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;

    // Send side
    uint32_t iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2;
    uint32_t snd_max;               // Highest sequence sent (snd_nxt goes back on a retransmit)
    uint16_t mss;                   // Peer's, bounded
    struct Ring sndbuf;             // First byte is at snd_una
    bool     fin_queued;            // Closed by the user: FIN after the data
    bool     fin_sent;
    uint32_t fin_seq;

    // Receive side
    uint32_t irs, rcv_nxt;
    struct Ring rcvbuf;
    bool     peer_fin;

    // Timers
    uint32_t rto;
    uint32_t srtt, rttvar;          // 0: no sample yet
    bool     rtt_active;
    uint32_t rtt_seq;
    uint64_t rtt_start;
    uint64_t timer_at;              // Retransmit / probe / TIME-WAIT, 0: off
    uint32_t retries;
    bool     probe;

    // Listening
    struct TcpSocket* listener;     // Not yet accepted: the listener it came in on
    uint32_t backlog;
    bool     accepted;

    bool     user_closed;           // The owner let go: free it once CLOSED
    int      error;                 // NET_ERR_* once refused / reset / timed out
    bool     connected;             // Reached ESTABLISHED once
    uint64_t born;                  // Created (oldest half-open goes first)
};

static struct TcpSocket* tcbs[TCP_MAX];

static uint32_t rst_tokens = TCP_RST_PER_SEC;
static uint64_t rst_refill = 0;

//
// Buffers
//

static uint32_t ring_free(const struct Ring* r) {
    return TCP_BUFFER - r->length;
}

static uint32_t ring_write(struct Ring* r, const uint8_t* src, uint32_t n) {
    if (n > ring_free(r))
        n = ring_free(r);
    for (uint32_t i = 0; i < n; i++)
        r->data[(r->start + r->length + i) % TCP_BUFFER] = src[i];
    r->length += n;
    return n;
}

// Copy `n` bytes from `offset` on, without taking them out
static void ring_peek(const struct Ring* r, uint32_t offset, uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        dst[i] = r->data[(r->start + offset + i) % TCP_BUFFER];
}

static void ring_drop(struct Ring* r, uint32_t n) {
    if (n > r->length)
        n = r->length;
    r->start = (r->start + n) % TCP_BUFFER;
    r->length -= n;
}

//
// Table
//

// A half-open connection nobody has proved to be real yet: it came in on a
// listener and never finished the handshake
static bool tcp_half_open(const struct TcpSocket* t) {
    return t != NULL && t->state == TCP_SYN_RECEIVED && t->listener != NULL && !t->connected;
}

// `evict`: when the table is full, the oldest half-open connection gives
// its slot up (they may well be a spoofed flood); a SYN never evicts one
static struct TcpSocket* tcp_alloc(bool buffers, bool evict) {
    int slot = -1;
    for (int i = 0; i < TCP_MAX && slot < 0; i++)
        if (tcbs[i] == NULL)
            slot = i;
    // Full: a connection in TIME-WAIT gives its slot up first
    if (slot < 0) {
        for (int i = 0; i < TCP_MAX && slot < 0; i++) {
            if (tcbs[i]->state == TCP_TIME_WAIT && tcbs[i]->user_closed) {
                free(tcbs[i]->sndbuf.data);
                free(tcbs[i]->rcvbuf.data);
                free(tcbs[i]);
                tcbs[i] = NULL;
                slot = i;
            }
        }
    }
    if (slot < 0 && evict) {
        int oldest = -1;
        for (int i = 0; i < TCP_MAX; i++)
            if (tcp_half_open(tcbs[i]) && (oldest < 0 || tcbs[i]->born < tcbs[oldest]->born))
                oldest = i;
        if (oldest >= 0) {
            free(tcbs[oldest]->sndbuf.data);
            free(tcbs[oldest]->rcvbuf.data);
            free(tcbs[oldest]);
            tcbs[oldest] = NULL;
            slot = oldest;
        }
    }
    if (slot < 0)
        return NULL;

    struct TcpSocket* t = (struct TcpSocket*)malloc(sizeof(struct TcpSocket));
    if (t == NULL)
        return NULL;
    memset(t, 0, sizeof(*t));
    if (buffers) {
        t->sndbuf.data = (uint8_t*)malloc(TCP_BUFFER);
        t->rcvbuf.data = (uint8_t*)malloc(TCP_BUFFER);
        if (t->sndbuf.data == NULL || t->rcvbuf.data == NULL) {
            free(t->sndbuf.data);
            free(t->rcvbuf.data);
            free(t);
            return NULL;
        }
    }
    t->rto = TCP_RTO_INITIAL_MS;
    t->mss = TCP_MSS_DEFAULT;
    t->born = net_now_ms();
    tcbs[slot] = t;
    return t;
}

static void tcp_free(struct TcpSocket* t) {
    for (uint32_t i = 0; i < TCP_MAX; i++)
        if (tcbs[i] == t)
            tcbs[i] = NULL;
    free(t->sndbuf.data);
    free(t->rcvbuf.data);
    free(t);
}

static bool tcp_port_used(uint16_t port) {
    for (uint32_t i = 0; i < TCP_MAX; i++)
        if (tcbs[i] != NULL && tcbs[i]->local_port == port)
            return true;
    return false;
}

static struct TcpSocket* tcp_find(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    struct TcpSocket* listener = NULL;
    for (uint32_t i = 0; i < TCP_MAX; i++) {
        struct TcpSocket* t = tcbs[i];
        if (t == NULL || t->local_port != local_port)
            continue;
        if (t->state == TCP_LISTEN) {
            listener = t;
            continue;
        }
        if (t->state != TCP_CLOSED && t->local_ip == local_ip && t->remote_ip == remote_ip &&
            t->remote_port == remote_port)
            return t;
    }
    return listener;
}

//
// Sending segments
//

static uint32_t tcp_rcv_wnd(const struct TcpSocket* t) {
    return t->rcvbuf.data != NULL ? ring_free(&t->rcvbuf) : 0;
}

static uint32_t tcp_pseudo_sum(uint32_t src, uint32_t dst, uint32_t length) {
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + IP_PROTO_TCP + length;
}

// One segment. `data` bytes come from the send buffer at `offset`.
static int tcp_emit(uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port,
                    uint32_t seq, uint32_t ack, uint8_t flags, uint32_t window,
                    const struct Ring* ring, uint32_t offset, uint32_t length, bool mss_option) {
    uint32_t header = TCP_HEADER_LEN + (mss_option ? 4 : 0);
    uint32_t total = header + length;
    uint8_t* s = (uint8_t*)malloc(total);
    if (s == NULL)
        return NET_ERR_NOMEM;
    memset(s, 0, header);
    net_put16(s, src_port);
    net_put16(s + 2, dst_port);
    net_put32(s + 4, seq);
    net_put32(s + 8, (flags & TCP_ACK) ? ack : 0);
    s[12] = (uint8_t)((header / 4) << 4);
    s[13] = flags;
    net_put16(s + 14, (uint16_t)(window > 0xFFFF ? 0xFFFF : window));
    if (mss_option) {
        s[20] = 2;      // MSS
        s[21] = 4;
        net_put16(s + 22, (uint16_t)(NET_MTU_DEFAULT - IP_HEADER_LEN - TCP_HEADER_LEN));
    }
    if (length != 0)
        ring_peek(ring, offset, s + header, length);
    net_put16(s + 16, net_fold(net_sum(s, total, tcp_pseudo_sum(src, dst, total))));

    int result = ip_output(src, dst, IP_PROTO_TCP, s, total, NULL);
    free(s);
    return result;
}

static void tcp_send_ack(struct TcpSocket* t) {
    tcp_emit(t->local_ip, t->local_port, t->remote_ip, t->remote_port,
             t->snd_nxt, t->rcv_nxt, TCP_ACK, tcp_rcv_wnd(t), NULL, 0, 0, false);
}

static void tcp_send_syn(struct TcpSocket* t) {
    uint8_t flags = TCP_SYN | (t->state == TCP_SYN_RECEIVED ? TCP_ACK : 0);
    tcp_emit(t->local_ip, t->local_port, t->remote_ip, t->remote_port,
             t->iss, t->rcv_nxt, flags, tcp_rcv_wnd(t), NULL, 0, 0, true);
}

static void tcp_send_rst(struct TcpSocket* t) {
    tcp_emit(t->local_ip, t->local_port, t->remote_ip, t->remote_port,
             t->snd_nxt, t->rcv_nxt, TCP_RST | TCP_ACK, 0, NULL, 0, 0, false);
}

static void tcp_arm(struct TcpSocket* t) {
    if (t->timer_at == 0)
        t->timer_at = net_now_ms() + t->rto;
}

// Send what the window allows, then the FIN once the data is all out
static void tcp_output(struct TcpSocket* t) {
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT && t->state != TCP_FIN_WAIT_1 &&
        t->state != TCP_CLOSING && t->state != TCP_LAST_ACK)
        return;

    uint32_t data_end = t->snd_una + t->sndbuf.length;
    uint32_t window = t->snd_wnd;
    if (window == 0 && t->probe)
        window = 1;             // Zero-window probe: one byte past the window
    t->probe = false;

    while (SEQ_LT(t->snd_nxt, data_end)) {
        uint32_t window_end = t->snd_una + window;
        if (!SEQ_LT(t->snd_nxt, window_end))
            break;
        uint32_t n = data_end - t->snd_nxt;
        if (n > window_end - t->snd_nxt)
            n = window_end - t->snd_nxt;
        if (n > t->mss)
            n = t->mss;

        uint32_t offset = t->snd_nxt - t->snd_una;
        uint8_t flags = TCP_ACK | (t->snd_nxt + n == data_end ? TCP_PSH : 0);
        tcp_emit(t->local_ip, t->local_port, t->remote_ip, t->remote_port,
                 t->snd_nxt, t->rcv_nxt, flags, tcp_rcv_wnd(t), &t->sndbuf, offset, n, false);
        if (!t->rtt_active) {
            t->rtt_active = true;
            t->rtt_seq    = t->snd_nxt;
            t->rtt_start  = net_now_ms();
        }
        t->snd_nxt += n;
        if (SEQ_GT(t->snd_nxt, t->snd_max))
            t->snd_max = t->snd_nxt;
        tcp_arm(t);
    }

    if (t->fin_queued && !t->fin_sent && t->snd_nxt == data_end) {
        tcp_emit(t->local_ip, t->local_port, t->remote_ip, t->remote_port,
                 t->snd_nxt, t->rcv_nxt, TCP_FIN | TCP_ACK, tcp_rcv_wnd(t), NULL, 0, 0, false);
        t->fin_seq  = t->snd_nxt;
        t->fin_sent = true;
        t->snd_nxt++;
        if (SEQ_GT(t->snd_nxt, t->snd_max))
            t->snd_max = t->snd_nxt;
        tcp_arm(t);
    }

    // Data waiting behind a closed window: probe it on the timer
    if (SEQ_LT(t->snd_nxt, data_end) && t->snd_nxt == t->snd_una)
        tcp_arm(t);
}

// The connection is dead: report `error` (0: orderly) and free it if the
// owner already let go
static void tcp_finish(struct TcpSocket* t, int error) {
    if (error != 0 && t->error == 0)
        t->error = error;
    t->state = TCP_CLOSED;
    t->timer_at = 0;
    if (t->user_closed || (t->listener != NULL && !t->accepted))
        tcp_free(t);
}

// A reset for a segment nobody wants (RFC 9293 3.10.7.1), rate limited
static void tcp_reply_rst(uint32_t local_ip, uint32_t remote_ip, const uint8_t* seg, uint32_t data_length) {
    uint8_t flags = seg[13];
    if (flags & TCP_RST)
        return;

    uint64_t now = net_now_ms();
    if (now - rst_refill >= 1000) {
        rst_tokens = TCP_RST_PER_SEC;
        rst_refill = now;
    }
    if (rst_tokens == 0)
        return;
    rst_tokens--;

    uint16_t local_port = net_get16(seg + 2);
    uint16_t remote_port = net_get16(seg);
    if (flags & TCP_ACK) {
        tcp_emit(local_ip, local_port, remote_ip, remote_port, net_get32(seg + 8), 0, TCP_RST, 0, NULL, 0, 0, false);
    } else {
        uint32_t seg_len = data_length + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);
        tcp_emit(local_ip, local_port, remote_ip, remote_port, 0, net_get32(seg + 4) + seg_len,
                 TCP_RST | TCP_ACK, 0, NULL, 0, 0, false);
    }
}

//
// Receiving
//

// The MSS option of a SYN, bounded; every option length is checked
static uint16_t tcp_parse_mss(const uint8_t* options, uint32_t length) {
    uint16_t mss = TCP_MSS_DEFAULT;
    uint32_t i = 0;
    while (i < length) {
        uint8_t kind = options[i];
        if (kind == 0)
            break;              // End of options
        if (kind == 1) {
            i++;                // No-op
            continue;
        }
        if (i + 1 >= length)
            break;
        uint8_t olen = options[i + 1];
        if (olen < 2 || i + olen > length)
            break;              // Malformed: stop, keep the default
        if (kind == 2 && olen == 4)
            mss = net_get16(options + i + 2);
        i += olen;
    }
    uint16_t max = NET_MTU_DEFAULT - IP_HEADER_LEN - TCP_HEADER_LEN;
    if (mss < TCP_MSS_MIN)
        mss = TCP_MSS_DEFAULT;
    if (mss > max)
        mss = max;
    return mss;
}

static void tcp_rtt_sample(struct TcpSocket* t, uint32_t sample) {
    // RFC 6298 2.2 / 2.3, in ms
    if (t->srtt == 0) {
        t->srtt = sample ? sample : 1;
        t->rttvar = sample / 2;
    } else {
        uint32_t delta = t->srtt > sample ? t->srtt - sample : sample - t->srtt;
        t->rttvar = (3 * t->rttvar + delta) / 4;
        t->srtt = (7 * t->srtt + sample) / 8;
        if (t->srtt == 0)
            t->srtt = 1;
    }
    uint32_t rto = t->srtt + 4 * t->rttvar;
    t->rto = rto < TCP_RTO_MIN_MS ? TCP_RTO_MIN_MS : (rto > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : rto);
}

static uint32_t tcp_listener_load(const struct TcpSocket* l) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < TCP_MAX; i++)
        if (tcbs[i] != NULL && tcbs[i]->listener == l && !tcbs[i]->accepted)
            count++;
    return count;
}

// Connections done with the handshake that the owner hasn't taken yet
static uint32_t tcp_listener_ready(const struct TcpSocket* l) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < TCP_MAX; i++)
        if (tcbs[i] != NULL && tcbs[i]->listener == l && !tcbs[i]->accepted && tcbs[i]->connected)
            count++;
    return count;
}

//
// SYN cookies
//

static const uint16_t cookie_mss[] = { 536, 1220, 1440, 1460 };
static uint64_t cookie_key[2];
static bool     cookie_keyed = false;

static uint32_t tcp_cookie_hash(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port,
                                uint32_t irs, uint32_t period) {
    if (!cookie_keyed) {
        cookie_key[0] = ((uint64_t)net_random() << 32) | net_random();
        cookie_key[1] = ((uint64_t)net_random() << 32) | net_random();
        cookie_keyed = true;
    }
    uint8_t b[20];
    net_put32(b, local_ip);
    net_put32(b + 4, remote_ip);
    net_put16(b + 8, local_port);
    net_put16(b + 10, remote_port);
    net_put32(b + 12, irs);
    net_put32(b + 16, period);
    return (uint32_t)net_siphash(cookie_key, b, sizeof(b));
}

static uint32_t tcp_cookie_period(void) {
    return (uint32_t)(net_now_ms() >> TCP_COOKIE_SHIFT);
}

static uint32_t tcp_cookie_make(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port,
                                uint32_t irs, uint16_t mss) {
    uint32_t index = 0;
    for (uint32_t i = 0; i < sizeof(cookie_mss) / sizeof(cookie_mss[0]); i++)
        if (cookie_mss[i] <= mss)
            index = i;
    uint32_t hash = tcp_cookie_hash(local_ip, local_port, remote_ip, remote_port, irs, tcp_cookie_period());
    return (hash & ~TCP_COOKIE_MSS_BITS) | index;
}

// The MSS a cookie carries, 0 if it is not one we made in the last two periods
static uint16_t tcp_cookie_check(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port,
                                 uint32_t irs, uint32_t cookie) {
    uint32_t index = cookie & TCP_COOKIE_MSS_BITS;
    if (index >= sizeof(cookie_mss) / sizeof(cookie_mss[0]))
        return 0;
    uint32_t period = tcp_cookie_period();
    for (uint32_t back = 0; back < 2; back++) {
        uint32_t hash = tcp_cookie_hash(local_ip, local_port, remote_ip, remote_port, irs, period - back);
        if (((hash ^ cookie) & ~TCP_COOKIE_MSS_BITS) == 0)
            return cookie_mss[index];
    }
    return 0;
}

static void tcp_input_synced(struct TcpSocket* t, const uint8_t* seg, const uint8_t* data, uint32_t data_length);

// The ACK that ends a cookie handshake: a real connection, made only now.
// False if it is no cookie of ours (the caller resets).
static bool tcp_cookie_ack(struct TcpSocket* l, uint32_t local_ip, uint32_t remote_ip, const uint8_t* seg,
                           uint32_t header_length, uint32_t data_length) {
    uint32_t seq = net_get32(seg + 4);
    uint32_t ack = net_get32(seg + 8);
    uint16_t remote_port = net_get16(seg);
    uint16_t mss = tcp_cookie_check(local_ip, l->local_port, remote_ip, remote_port, seq - 1, ack - 1);
    if (mss == 0)
        return false;

    // Ours, but no room to queue it: drop it quietly, the peer resends
    if (tcp_listener_ready(l) >= l->backlog)
        return true;
    struct TcpSocket* t = tcp_alloc(true, true);
    if (t == NULL)
        return true;

    t->state       = TCP_ESTABLISHED;
    t->connected   = true;
    t->listener    = l;
    t->local_ip    = local_ip;
    t->local_port  = l->local_port;
    t->remote_ip   = remote_ip;
    t->remote_port = remote_port;
    t->irs         = seq - 1;
    t->rcv_nxt     = seq;
    t->iss         = ack - 1;
    t->snd_una     = ack;
    t->snd_nxt     = ack;
    t->snd_max     = ack;
    t->snd_wnd     = net_get16(seg + 14);
    t->snd_wl1     = seq;
    t->snd_wl2     = ack;
    t->mss         = mss;
    // Data or a FIN riding on the ACK
    tcp_input_synced(t, seg, seg + header_length, data_length);
    return true;
}

static void tcp_input_listen(struct TcpSocket* l, uint32_t local_ip, uint32_t remote_ip, const uint8_t* seg,
                             uint32_t header_length, uint32_t data_length) {
    uint8_t flags = seg[13];
    if (flags & TCP_RST)
        return;
    if (flags & TCP_ACK) {
        if ((flags & TCP_SYN) || !tcp_cookie_ack(l, local_ip, remote_ip, seg, header_length, data_length))
            tcp_reply_rst(local_ip, remote_ip, seg, data_length);
        return;
    }
    if (!(flags & TCP_SYN) || (flags & TCP_FIN))
        return;

    // Half-open slots (or the table) full: answer with a cookie and keep
    // nothing. If even the accept queue is full, drop: the peer tries again.
    struct TcpSocket* t = NULL;
    if (tcp_listener_load(l) < l->backlog)
        t = tcp_alloc(true, false);
    if (t == NULL) {
        if (tcp_listener_ready(l) >= l->backlog)
            return;
        uint32_t irs = net_get32(seg + 4);
        uint16_t remote_port = net_get16(seg);
        uint16_t mss = tcp_parse_mss(seg + TCP_HEADER_LEN, header_length - TCP_HEADER_LEN);
        uint32_t cookie = tcp_cookie_make(local_ip, l->local_port, remote_ip, remote_port, irs, mss);
        tcp_emit(local_ip, l->local_port, remote_ip, remote_port, cookie, irs + 1, TCP_SYN | TCP_ACK,
                 TCP_BUFFER, NULL, 0, 0, true);
        return;
    }

    t->state       = TCP_SYN_RECEIVED;
    t->listener    = l;
    t->local_ip    = local_ip;
    t->local_port  = l->local_port;
    t->remote_ip   = remote_ip;
    t->remote_port = net_get16(seg);
    t->irs         = net_get32(seg + 4);
    t->rcv_nxt     = t->irs + 1;        // Data on the SYN is not taken
    t->iss         = net_random();
    t->snd_una     = t->iss;
    t->snd_nxt     = t->iss + 1;
    t->snd_max     = t->snd_nxt;
    t->snd_wnd     = net_get16(seg + 14);
    t->snd_wl1     = t->irs;
    t->snd_wl2     = t->iss;
    t->mss         = tcp_parse_mss(seg + TCP_HEADER_LEN, header_length - TCP_HEADER_LEN);
    tcp_send_syn(t);
    tcp_arm(t);
}

static void tcp_input_syn_sent(struct TcpSocket* t, const uint8_t* seg, uint32_t header_length) {
    uint8_t  flags = seg[13];
    uint32_t seq   = net_get32(seg + 4);
    uint32_t ack   = net_get32(seg + 8);

    bool ack_ok = (flags & TCP_ACK) && ack == t->snd_nxt;
    if ((flags & TCP_ACK) && !ack_ok) {
        if (!(flags & TCP_RST))
            tcp_emit(t->local_ip, t->local_port, t->remote_ip, t->remote_port, ack, 0, TCP_RST, 0, NULL, 0, 0, false);
        return;
    }
    if (flags & TCP_RST) {
        if (ack_ok)
            tcp_finish(t, NET_ERR_REFUSED);
        return;
    }
    // Simultaneous open (SYN without ACK) is not supported: ignored
    if (!(flags & TCP_SYN) || !ack_ok)
        return;

    t->irs       = seq;
    t->rcv_nxt   = seq + 1;
    t->snd_una   = ack;
    t->snd_wnd   = net_get16(seg + 14);
    t->snd_wl1   = seq;
    t->snd_wl2   = ack;
    t->mss       = tcp_parse_mss(seg + TCP_HEADER_LEN, header_length - TCP_HEADER_LEN);
    t->state     = TCP_ESTABLISHED;
    t->connected = true;
    t->timer_at  = 0;
    t->retries   = 0;
    tcp_send_ack(t);
    tcp_output(t);
}

// Segment acceptability (RFC 9293 3.10.7.4, first check)
static bool tcp_acceptable(const struct TcpSocket* t, uint32_t seq, uint32_t seg_len) {
    uint32_t wnd = tcp_rcv_wnd(t);
    if (seg_len == 0) {
        if (wnd == 0)
            return seq == t->rcv_nxt;
        return SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + wnd);
    }
    if (wnd == 0)
        return false;
    uint32_t last = seq + seg_len - 1;
    return (SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + wnd)) ||
           (SEQ_GEQ(last, t->rcv_nxt) && SEQ_LT(last, t->rcv_nxt + wnd));
}

static void tcp_input_synced(struct TcpSocket* t, const uint8_t* seg, const uint8_t* data, uint32_t data_length) {
    uint8_t  flags  = seg[13];
    uint32_t seq    = net_get32(seg + 4);
    uint32_t ack    = net_get32(seg + 8);
    uint32_t window = net_get16(seg + 14);
    uint32_t seg_len = data_length + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);

    // 1. Sequence number
    if (!tcp_acceptable(t, seq, seg_len)) {
        if (!(flags & TCP_RST))
            tcp_send_ack(t);
        if (t->state == TCP_TIME_WAIT && (flags & TCP_FIN))
            t->timer_at = net_now_ms() + TCP_TIME_WAIT_MS;
        return;
    }

    // 2. RST: only exactly at rcv_nxt (RFC 5961 3.2); else a challenge ACK
    if (flags & TCP_RST) {
        if (seq != t->rcv_nxt) {
            tcp_send_ack(t);
            return;
        }
        tcp_finish(t, t->state == TCP_TIME_WAIT ? 0 : NET_ERR_RESET);
        return;
    }

    // 3. SYN on a synchronized connection: challenge ACK (RFC 5961 4.2)
    if (flags & TCP_SYN) {
        tcp_send_ack(t);
        return;
    }

    // 4. ACK
    if (!(flags & TCP_ACK))
        return;

    if (t->state == TCP_SYN_RECEIVED) {
        if (!SEQ_LT(t->snd_una, ack) || SEQ_GT(ack, t->snd_nxt)) {
            tcp_emit(t->local_ip, t->local_port, t->remote_ip, t->remote_port, ack, 0, TCP_RST, 0, NULL, 0, 0, false);
            return;
        }
        t->state     = TCP_ESTABLISHED;
        t->connected = true;
        t->snd_una   = ack;
        t->snd_wnd   = window;
        t->snd_wl1   = seq;
        t->snd_wl2   = ack;
        t->timer_at  = 0;
        t->retries   = 0;
    }

    if (SEQ_GT(ack, t->snd_max)) {
        tcp_send_ack(t);        // Acknowledges what we never sent
        return;
    }
    if (SEQ_LT(ack, t->snd_una - 65535))
        return;                 // Ancient: RFC 5961 5.2

    if (SEQ_GT(ack, t->snd_una)) {
        uint32_t acked = ack - t->snd_una;
        uint32_t data_acked = acked < t->sndbuf.length ? acked : t->sndbuf.length;
        ring_drop(&t->sndbuf, data_acked);
        t->snd_una = ack;
        if (SEQ_LT(t->snd_nxt, ack))
            t->snd_nxt = ack;   // Acked past a retransmission in progress

        if (t->rtt_active && SEQ_GT(ack, t->rtt_seq)) {
            uint64_t sample = net_now_ms() - t->rtt_start;
            tcp_rtt_sample(t, sample > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : (uint32_t)sample);
            t->rtt_active = false;
        }
        t->retries  = 0;
        t->timer_at = 0;
        if (t->snd_una != t->snd_max)
            tcp_arm(t);
    }
    // Window update, only from a newer segment (RFC 9293 3.10.7.4)
    if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LEQ(t->snd_wl2, ack))) {
        t->snd_wnd = window;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
    }

    bool fin_acked = t->fin_sent && SEQ_GT(t->snd_una, t->fin_seq);
    if (t->state == TCP_FIN_WAIT_1 && fin_acked) {
        t->state = TCP_FIN_WAIT_2;
        t->timer_at = net_now_ms() + TCP_FIN_WAIT_2_MS;
    }
    else if (t->state == TCP_CLOSING && fin_acked) {
        t->state = TCP_TIME_WAIT;
        t->timer_at = net_now_ms() + TCP_TIME_WAIT_MS;
    } else if (t->state == TCP_LAST_ACK && fin_acked) {
        tcp_finish(t, 0);
        return;
    }

    // 5. Data, in order only
    bool takes_data = t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2;
    bool all_taken = true;
    if (data_length != 0) {
        if (!takes_data) {
            tcp_send_ack(t);
            return;
        }
        if (SEQ_GT(seq, t->rcv_nxt)) {
            tcp_send_ack(t);    // A gap: the duplicate ACK asks for it
            return;
        }
        uint32_t skip = t->rcv_nxt - seq;   // Already have these
        if (skip > data_length)
            skip = data_length;
        uint32_t taken = ring_write(&t->rcvbuf, data + skip, data_length - skip);
        t->rcv_nxt += taken;
        all_taken = skip + taken == data_length;
    }

    // 6. FIN, once everything before it is in
    if ((flags & TCP_FIN) && all_taken && seq + data_length == t->rcv_nxt) {
        if (!t->peer_fin) {
            t->peer_fin = true;
            t->rcv_nxt++;
        }
        switch (t->state) {
            case TCP_ESTABLISHED:
                t->state = TCP_CLOSE_WAIT;
                break;
            case TCP_FIN_WAIT_1:
                t->state = fin_acked ? TCP_TIME_WAIT : TCP_CLOSING;
                if (t->state == TCP_TIME_WAIT)
                    t->timer_at = net_now_ms() + TCP_TIME_WAIT_MS;
                break;
            case TCP_FIN_WAIT_2:
                t->state = TCP_TIME_WAIT;
                t->timer_at = net_now_ms() + TCP_TIME_WAIT_MS;
                break;
            case TCP_TIME_WAIT:
                t->timer_at = net_now_ms() + TCP_TIME_WAIT_MS;
                break;
            default:
                break;
        }
        tcp_send_ack(t);
    } else if (data_length != 0) {
        tcp_send_ack(t);
    }

    tcp_output(t);
}

void tcp_input(struct NetIf* ifp, uint32_t src, uint32_t dst, const uint8_t* seg, uint32_t length) {
    (void)ifp;
    if (length < TCP_HEADER_LEN)
        return;
    uint32_t header_length = (uint32_t)(seg[12] >> 4) * 4;
    if (header_length < TCP_HEADER_LEN || header_length > length)
        return;
    if (net_fold(net_sum(seg, length, tcp_pseudo_sum(src, dst, length))) != 0)
        return;
    uint16_t src_port = net_get16(seg);
    uint16_t dst_port = net_get16(seg + 2);
    if (src_port == 0 || dst_port == 0)
        return;

    const uint8_t* data = seg + header_length;
    uint32_t data_length = length - header_length;

    struct TcpSocket* t = tcp_find(dst, dst_port, src, src_port);
    if (t == NULL) {
        tcp_reply_rst(dst, src, seg, data_length);
        return;
    }
    if (t->state == TCP_LISTEN) {
        tcp_input_listen(t, dst, src, seg, header_length, data_length);
        return;
    }
    if (t->state == TCP_SYN_SENT) {
        tcp_input_syn_sent(t, seg, header_length);
        return;
    }
    tcp_input_synced(t, seg, data, data_length);
}

//
// Timers
//

void tcp_timer(void) {
    uint64_t now = net_now_ms();
    for (uint32_t i = 0; i < TCP_MAX; i++) {
        struct TcpSocket* t = tcbs[i];
        if (t == NULL)
            continue;
        if (t->state == TCP_CLOSED) {
            if (t->user_closed)
                tcp_free(t);
            continue;
        }
        if (t->timer_at == 0 || now < t->timer_at)
            continue;
        t->timer_at = 0;

        if (t->state == TCP_TIME_WAIT || t->state == TCP_FIN_WAIT_2) {
            tcp_finish(t, 0);
            continue;
        }

        uint32_t limit = (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECEIVED) ? TCP_SYN_RETRIES : TCP_RETRIES;
        if (++t->retries > limit) {
            if (t->state != TCP_SYN_SENT && t->state != TCP_SYN_RECEIVED)
                tcp_send_rst(t);
            tcp_finish(t, NET_ERR_TIMEOUT);
            continue;
        }

        // Back off, then send again from the oldest unacknowledged byte
        t->rto = t->rto * 2 > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : t->rto * 2;
        t->rtt_active = false;      // Karn: no samples from retransmissions
        if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECEIVED) {
            tcp_send_syn(t);
            tcp_arm(t);
            continue;
        }
        t->snd_nxt = t->snd_una;
        if (t->fin_sent && SEQ_LEQ(t->snd_una, t->fin_seq))
            t->fin_sent = false;
        t->probe = t->snd_wnd == 0;
        tcp_output(t);
        tcp_arm(t);
    }
}

//
// Kernel API
//

struct TcpSocket* tcp_listen(uint16_t port, uint32_t backlog, int* error) {
    int dummy;
    if (error == NULL) error = &dummy;
    if (port == 0) {
        *error = NET_ERR_PARAM;
        return NULL;
    }
    if (backlog == 0) backlog = 1;
    if (backlog > TCP_BACKLOG_MAX) backlog = TCP_BACKLOG_MAX;

    net_lock();
    struct TcpSocket* t = NULL;
    if (tcp_port_used(port))
        *error = NET_ERR_INUSE;
    else if ((t = tcp_alloc(false, true)) == NULL)
        *error = NET_ERR_NOMEM;
    else {
        t->state = TCP_LISTEN;
        t->local_port = port;
        t->backlog = backlog;
        *error = NET_OK;
    }
    net_unlock();
    return t;
}

struct TcpSocket* tcp_accept(struct TcpSocket* l, uint32_t* ip, uint16_t* port) {
    if (l == NULL)
        return NULL;
    net_lock();
    struct TcpSocket* found = NULL;
    for (uint32_t i = 0; i < TCP_MAX && found == NULL; i++) {
        struct TcpSocket* t = tcbs[i];
        if (t != NULL && t->listener == l && !t->accepted && t->connected)
            found = t;
    }
    if (found != NULL) {
        found->accepted = true;
        found->listener = NULL;
        if (ip != NULL)   *ip = found->remote_ip;
        if (port != NULL) *port = found->remote_port;
    }
    net_unlock();
    return found;
}

struct TcpSocket* tcp_connect(uint32_t ip, uint16_t port, int* error) {
    int dummy;
    if (error == NULL) error = &dummy;
    if (port == 0 || ip == 0 || ip == IP_BROADCAST || (ip >> 28) >= 0xE) {
        *error = NET_ERR_PARAM;
        return NULL;
    }

    net_lock();
    uint32_t src = ip_source_for(ip);
    if (src == 0) {
        net_unlock();
        *error = NET_ERR_NOROUTE;
        return NULL;
    }
    uint16_t local_port = 0;
    for (int tries = 0; tries < 64 && local_port == 0; tries++) {
        uint16_t candidate = (uint16_t)(TCP_EPHEMERAL_LOW + net_random() % (65536 - TCP_EPHEMERAL_LOW));
        if (!tcp_port_used(candidate))
            local_port = candidate;
    }
    struct TcpSocket* t = local_port != 0 ? tcp_alloc(true, true) : NULL;
    if (t == NULL) {
        net_unlock();
        *error = local_port == 0 ? NET_ERR_INUSE : NET_ERR_NOMEM;
        return NULL;
    }
    t->state       = TCP_SYN_SENT;
    t->local_ip    = src;
    t->local_port  = local_port;
    t->remote_ip   = ip;
    t->remote_port = port;
    t->iss         = net_random();
    t->snd_una     = t->iss;
    t->snd_nxt     = t->iss + 1;
    t->snd_max     = t->snd_nxt;
    tcp_send_syn(t);
    tcp_arm(t);
    net_unlock();
    *error = NET_OK;
    return t;
}

int tcp_send(struct TcpSocket* s, const void* data, uint32_t length) {
    if (s == NULL || (length != 0 && data == NULL))
        return NET_ERR_PARAM;
    net_lock();
    int result;
    if (s->error != 0)
        result = s->error;
    else if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECEIVED)
        result = NET_ERR_AGAIN;
    else if ((s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) || s->fin_queued)
        result = NET_ERR_CLOSED;
    else if (length == 0)
        result = 0;
    else {
        uint32_t n = ring_write(&s->sndbuf, (const uint8_t*)data, length);
        result = n > 0 ? (int)n : NET_ERR_AGAIN;
        tcp_output(s);
    }
    net_unlock();
    return result;
}

int tcp_recv(struct TcpSocket* s, void* buffer, uint32_t size) {
    if (s == NULL || (size != 0 && buffer == NULL))
        return NET_ERR_PARAM;
    net_lock();
    int result;
    if (s->rcvbuf.data != NULL && s->rcvbuf.length > 0 && size > 0) {
        uint32_t before = ring_free(&s->rcvbuf);
        uint32_t n = s->rcvbuf.length < size ? s->rcvbuf.length : size;
        ring_peek(&s->rcvbuf, 0, (uint8_t*)buffer, n);
        ring_drop(&s->rcvbuf, n);
        // The window was (nearly) shut: tell the peer it opened
        if (before < s->mss && ring_free(&s->rcvbuf) >= s->mss && !s->peer_fin &&
            (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_1 || s->state == TCP_FIN_WAIT_2))
            tcp_send_ack(s);
        result = (int)n;
    } else if (s->peer_fin) {
        result = 0;
    } else if (s->error != 0) {
        result = s->error;
    } else if (s->state == TCP_CLOSED || s->state == TCP_LISTEN) {
        result = NET_ERR_CLOSED;
    } else {
        result = NET_ERR_AGAIN;
    }
    net_unlock();
    return result;
}

uint32_t tcp_poll(struct TcpSocket* s) {
    if (s == NULL)
        return 0;
    net_lock();
    uint32_t ev = 0;
    if (s->state == TCP_LISTEN) {
        for (uint32_t i = 0; i < TCP_MAX; i++)
            if (tcbs[i] != NULL && tcbs[i]->listener == s && !tcbs[i]->accepted && tcbs[i]->connected)
                ev |= TCP_EV_ACCEPT;
    } else {
        if (s->connected)
            ev |= TCP_EV_CONNECTED;
        if (s->error != 0)
            ev |= TCP_EV_ERROR | TCP_EV_READ | TCP_EV_WRITE;
        if ((s->rcvbuf.data != NULL && s->rcvbuf.length > 0) || s->peer_fin)
            ev |= TCP_EV_READ;
        if ((s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) && !s->fin_queued &&
            ring_free(&s->sndbuf) > 0)
            ev |= TCP_EV_WRITE;
    }
    net_unlock();
    return ev;
}

int tcp_error(struct TcpSocket* s) {
    if (s == NULL)
        return NET_ERR_PARAM;
    net_lock();
    int error = s->error;
    net_unlock();
    return error;
}

void tcp_endpoints(struct TcpSocket* s, uint32_t* local_ip, uint16_t* local_port,
                   uint32_t* remote_ip, uint16_t* remote_port) {
    if (s == NULL)
        return;
    net_lock();
    if (local_ip)    *local_ip = s->local_ip;
    if (local_port)  *local_port = s->local_port;
    if (remote_ip)   *remote_ip = s->remote_ip;
    if (remote_port) *remote_port = s->remote_port;
    net_unlock();
}

void tcp_close(struct TcpSocket* s) {
    if (s == NULL)
        return;
    net_lock();
    s->user_closed = true;

    switch (s->state) {
        case TCP_LISTEN:
            // Connections nobody accepted go with it
            for (uint32_t i = 0; i < TCP_MAX; i++) {
                struct TcpSocket* c = tcbs[i];
                if (c != NULL && c->listener == s && !c->accepted) {
                    tcp_send_rst(c);
                    c->user_closed = true;
                    tcp_finish(c, NET_ERR_RESET);
                }
            }
            tcp_free(s);
            break;
        case TCP_SYN_SENT:
        case TCP_CLOSED:
            tcp_free(s);
            break;
        case TCP_SYN_RECEIVED:
        case TCP_ESTABLISHED:
        case TCP_CLOSE_WAIT:
            // Unread data: a reset says it was lost (RFC 9293 3.6)
            if (s->rcvbuf.length > 0 || s->state == TCP_SYN_RECEIVED) {
                tcp_send_rst(s);
                tcp_free(s);
                break;
            }
            s->fin_queued = true;
            s->state = s->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
            tcp_output(s);
            break;
        default:
            break;      // Already closing: the timers finish it
    }
    net_unlock();
}
