#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/net/net.h>
#include <kernel/net/net_internal.h>
#include <kernel/arch/x86/drivers/rng.h>
#include <kernel/crypt/random.h>

#include <kernel/malloc.h>
#include <kernel/mutex.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

//
// Interfaces, the receive queue and the "net" thread
//

#define NET_RXQ_MAX         64      // Frames waiting for the net thread, all interfaces
#define NET_RX_BATCH        32      // Frames handled per pass
#define NET_THREAD_MS       5

struct RxEntry {
    struct RxEntry* next;
    struct NetIf*   ifp;
    uint32_t        length;
    uint8_t         data[];
};

static struct NetIf* ifs[NETIF_MAX];

static mutex_t net_mutex;
static mutex_t rxq_mutex;               // Innermost: only around the list
static struct RxEntry* rxq_head = NULL;
static struct RxEntry* rxq_tail = NULL;
static uint32_t rxq_count = 0;

static bool net_ready = false;
static struct NetIf* loopback = NULL;

void net_lock(void)   { mutex_lock(&net_mutex); }
void net_unlock(void) { mutex_unlock(&net_mutex); }

//
// Helpers
//

uint64_t net_now_ms(void) {
    return timer_get_ms();
}

// SipHash-2-4 (Aumasson, Bernstein): a keyed hash whose output says
// nothing about the key. Used for the fallback generator below and for TCP
// SYN cookies.
#define SIP_ROTL(x, b)      (((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_ROUND(v0, v1, v2, v3) do { \
        v0 += v1; v1 = SIP_ROTL(v1, 13); v1 ^= v0; v0 = SIP_ROTL(v0, 32); \
        v2 += v3; v3 = SIP_ROTL(v3, 16); v3 ^= v2; \
        v0 += v3; v3 = SIP_ROTL(v3, 21); v3 ^= v0; \
        v2 += v1; v1 = SIP_ROTL(v1, 17); v1 ^= v2; v2 = SIP_ROTL(v2, 32); \
    } while (0)

uint64_t net_siphash(const uint64_t key[2], const uint8_t* data, uint32_t length) {
    uint64_t v0 = key[0] ^ 0x736f6d6570736575ULL;
    uint64_t v1 = key[1] ^ 0x646f72616e646f6dULL;
    uint64_t v2 = key[0] ^ 0x6c7967656e657261ULL;
    uint64_t v3 = key[1] ^ 0x7465646279746573ULL;
    uint32_t i = 0;
    for (; i + 8 <= length; i += 8) {
        uint64_t m = 0;
        for (int b = 7; b >= 0; b--)
            m = (m << 8) | data[i + (uint32_t)b];
        v3 ^= m;
        SIP_ROUND(v0, v1, v2, v3);
        SIP_ROUND(v0, v1, v2, v3);
        v0 ^= m;
    }
    uint64_t last = (uint64_t)(length & 0xFF) << 56;
    for (uint32_t b = 0; i + b < length; b++)
        last |= (uint64_t)data[i + b] << (8 * b);
    v3 ^= last;
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);
    v0 ^= last;
    v2 ^= 0xFF;
    for (int r = 0; r < 4; r++)
        SIP_ROUND(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

// Fallback generator, for CPUs without RDRAND: a secret 128-bit key that
// every call and every received frame stirs with the cycle counter (timing
// an attacker can't see to the cycle), and outputs that are SipHash of a
// counter under that key. An output gives away neither the key nor the
// next output, unlike the xorshift it replaces (one ISN gave the state).
// Net lock held, like every caller.
static uint64_t rng_key[2];
static uint64_t rng_counter;
static bool     rng_seeded = false;

static void net_rekey(uint64_t sample) {
    uint8_t block[24];
    uint64_t words[2] = { sample, rng_counter++ };
    memcpy(block, words, 16);
    memset(block + 16, 0, 8);
    uint64_t k0 = net_siphash(rng_key, block, sizeof(block));
    block[16] = 1;
    uint64_t k1 = net_siphash(rng_key, block, sizeof(block));
    rng_key[0] = k0;
    rng_key[1] = k1;
}

void net_entropy(uint64_t sample) {
    uint64_t cycles = rng_cycles();
    net_rekey(sample ^ cycles);
    // The pool programs draw on (SYS_RANDOM) gets the same timing
    uint64_t mix[2] = { sample, cycles };
    crypt_random_add(mix, sizeof(mix));
}

uint32_t net_random(void) {
    uint32_t value;
    if (rng_get_rand(&value))
        return value;

    if (!rng_seeded) {
        uint32_t seed = 0;
        uint64_t extra = rng_get_seed(&seed) ? seed : 0;
        net_rekey(extra ^ ((uint64_t)timer_get_ms() << 32) ^ (uint64_t)(uintptr_t)&value);
        rng_seeded = true;
    }
    net_rekey(rng_cycles() ^ timer_get_ms());
    uint64_t words[2] = { rng_counter++, 0x6e65745f726e67ULL };
    return (uint32_t)net_siphash(rng_key, (const uint8_t*)words, sizeof(words));
}

uint32_t net_sum(const uint8_t* data, uint32_t length, uint32_t initial) {
    uint32_t sum = initial;
    uint32_t i = 0;
    for (; i + 1 < length; i += 2)
        sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if (i < length)
        sum += (uint32_t)(data[i] << 8);
    return sum;
}

uint16_t net_fold(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

bool net_ip_martian(uint32_t ip) {
    uint32_t top = ip >> 24;
    return ip == 0 || ip == IP_BROADCAST || top == 127 || top >= 224;   // 224+: multicast, class E
}

bool net_parse_ip(const char* text, uint32_t* out) {
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        uint32_t value = 0;
        int digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (uint32_t)(*text++ - '0');
            if (++digits > 3 || value > 255)
                return false;
        }
        if (digits == 0)
            return false;
        ip = (ip << 8) | value;
        if (part < 3 && *text++ != '.')
            return false;
    }
    if (*text != '\0')
        return false;
    *out = ip;
    return true;
}

void net_format_ip(uint32_t ip, char* out) {
    char* p = out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        char digits[4];
        utos((ip >> shift) & 0xFF, digits);
        for (char* d = digits; *d != '\0'; d++)
            *p++ = *d;
        if (shift != 0)
            *p++ = '.';
    }
    *p = '\0';
}

void net_format_mac(const uint8_t* mac, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < NET_MAC_LEN; i++) {
        out[i * 3]     = hex[mac[i] >> 4];
        out[i * 3 + 1] = hex[mac[i] & 0x0F];
        out[i * 3 + 2] = (i + 1 < NET_MAC_LEN) ? ':' : '\0';
    }
}

//
// Interface table (net lock held)
//

struct NetIf* netif_find_locked(const char* name) {
    if (name == NULL)
        return NULL;
    for (uint32_t i = 0; i < NETIF_MAX; i++) {
        if (ifs[i] != NULL && strncmp(ifs[i]->name, name, NETIF_NAME_MAX) == 0)
            return ifs[i];
    }
    return NULL;
}

struct NetIf* netif_get_locked(uint32_t index) {
    for (uint32_t i = 0; i < NETIF_MAX; i++) {
        if (ifs[i] != NULL && index-- == 0)
            return ifs[i];
    }
    return NULL;
}

uint32_t netif_count_locked(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < NETIF_MAX; i++)
        if (ifs[i] != NULL)
            count++;
    return count;
}

bool netif_usable(const struct NetIf* ifp) {
    return ifp != NULL && (ifp->flags & (NETIF_UP | NETIF_LINK)) == (NETIF_UP | NETIF_LINK);
}

struct NetIf* net_owner_of(uint32_t ip) {
    for (uint32_t i = 0; i < NETIF_MAX; i++) {
        if (ifs[i] != NULL && ifs[i]->ip != 0 && ifs[i]->ip == ip)
            return ifs[i];
    }
    return NULL;
}

bool net_is_broadcast_for(const struct NetIf* ifp, uint32_t ip) {
    if (ip == IP_BROADCAST)
        return true;
    // A /31 or /32 has no broadcast address
    if (ifp->ip == 0 || ifp->netmask == 0 || (~ifp->netmask) <= 1)
        return false;
    return ip == (ifp->ip | ~ifp->netmask);
}

int netif_transmit(struct NetIf* ifp, const uint8_t* frame, uint32_t length) {
    if (!netif_usable(ifp)) {
        if (ifp != NULL) ifp->stats.tx_dropped++;
        return NET_ERR_DOWN;
    }
    if (length > NET_FRAME_MAX) {
        ifp->stats.tx_dropped++;
        return NET_ERR_TOOBIG;
    }
    int result = ifp->ops->transmit(ifp, frame, length);
    if (result == NET_OK) {
        ifp->stats.tx_packets++;
        ifp->stats.tx_bytes += length;
    } else {
        ifp->stats.tx_dropped++;
    }
    return result;
}

//
// Driver calls
//

static bool netif_name_ok(const char* name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= NETIF_NAME_MAX)
        return false;
    for (const char* c = name; *c != '\0'; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9');
        if (!ok)
            return false;
    }
    return true;
}

struct NetIf* netif_register(const char* name, const struct NetIfOps* ops, void* driver_data,
                             const uint8_t* mac, uint16_t mtu) {
    if (!net_ready || !netif_name_ok(name) || ops == NULL || ops->transmit == NULL)
        return NULL;
    if (mtu < 576 || mtu > NET_MTU_DEFAULT)
        mtu = NET_MTU_DEFAULT;

    struct NetIf* ifp = (struct NetIf*)malloc(sizeof(struct NetIf));
    if (ifp == NULL)
        return NULL;
    memset(ifp, 0, sizeof(*ifp));
    strncpy(ifp->name, name, NETIF_NAME_MAX - 1);
    if (mac != NULL)
        memcpy(ifp->mac, mac, NET_MAC_LEN);
    ifp->mtu         = mtu;
    ifp->flags       = NETIF_UP;
    ifp->ops         = ops;
    ifp->driver_data = driver_data;

    net_lock();
    int slot = -1;
    for (int i = 0; i < NETIF_MAX && slot < 0; i++)
        if (ifs[i] == NULL)
            slot = i;
    if (slot < 0 || netif_find_locked(name) != NULL) {
        net_unlock();
        free(ifp);
        return NULL;
    }
    ifs[slot] = ifp;
    net_unlock();
    return ifp;
}

void netif_unregister(struct NetIf* ifp) {
    if (ifp == NULL || ifp == loopback)
        return;

    net_lock();
    for (uint32_t i = 0; i < NETIF_MAX; i++)
        if (ifs[i] == ifp)
            ifs[i] = NULL;
    dhcp_stop(ifp);
    arp_flush_if(ifp);

    // Frames it queued: frames are only ever taken off the queue with the
    // net lock held, so none of them is being worked on right now
    mutex_lock(&rxq_mutex);
    struct RxEntry** link = &rxq_head;
    rxq_tail = NULL;
    while (*link != NULL) {
        struct RxEntry* e = *link;
        if (e->ifp == ifp) {
            *link = e->next;
            rxq_count--;
            free(e);
        } else {
            rxq_tail = e;
            link = &e->next;
        }
    }
    mutex_unlock(&rxq_mutex);
    net_unlock();

    free(ifp);
}

void netif_set_link(struct NetIf* ifp, bool link) {
    if (ifp == NULL)
        return;
    net_lock();
    bool was = (ifp->flags & NETIF_LINK) != 0;
    if (link && !was) {
        ifp->flags |= NETIF_LINK;
        // No static address: ask for one
        if (!(ifp->flags & NETIF_LOOPBACK) && ifp->ip == 0)
            dhcp_start(ifp);
    } else if (!link && was) {
        ifp->flags &= (uint16_t)~NETIF_LINK;
        arp_flush_if(ifp);
        if (ifp->flags & NETIF_DHCP)
            dhcp_stop(ifp);     // The lease means nothing on another network
    }
    net_unlock();
}

void netif_set_mac(struct NetIf* ifp, const uint8_t* mac) {
    if (ifp == NULL || mac == NULL)
        return;
    net_lock();
    memcpy(ifp->mac, mac, NET_MAC_LEN);
    net_unlock();
}

void netif_rx(struct NetIf* ifp, const void* frame, uint32_t length) {
    if (ifp == NULL || frame == NULL || length == 0 || length > NET_FRAME_MAX)
        return;

    struct RxEntry* e = (struct RxEntry*)malloc(sizeof(struct RxEntry) + length);
    if (e == NULL) {
        ifp->stats.rx_dropped++;
        return;
    }
    e->next   = NULL;
    e->ifp    = ifp;
    e->length = length;
    memcpy(e->data, frame, length);

    mutex_lock(&rxq_mutex);
    bool full = rxq_count >= NET_RXQ_MAX;
    if (!full) {
        if (rxq_tail != NULL)
            rxq_tail->next = e;
        else
            rxq_head = e;
        rxq_tail = e;
        rxq_count++;
    }
    mutex_unlock(&rxq_mutex);

    if (full) {
        ifp->stats.rx_dropped++;
        free(e);
    }
}

//
// Configuration
//

uint32_t netif_count(void) {
    net_lock();
    uint32_t count = netif_count_locked();
    net_unlock();
    return count;
}

bool netif_snapshot(uint32_t index, struct NetIf* out) {
    net_lock();
    struct NetIf* ifp = netif_get_locked(index);
    if (ifp != NULL) {
        *out = *ifp;
        out->ops = NULL;
        out->driver_data = NULL;
    }
    net_unlock();
    return ifp != NULL;
}

int netif_configure(const char* name, uint32_t ip, uint32_t netmask, uint32_t gateway) {
    // A mask must be contiguous ones, and a host address neither 0 nor
    // broadcast-looking garbage
    uint32_t inverted = ~netmask;
    if ((inverted & (inverted + 1)) != 0)
        return NET_ERR_PARAM;
    if (ip != 0 && (net_ip_martian(ip) || netmask == 0))
        return NET_ERR_PARAM;
    if (gateway != 0 && (net_ip_martian(gateway) || (gateway & netmask) != (ip & netmask)))
        return NET_ERR_PARAM;

    net_lock();
    struct NetIf* ifp = netif_find_locked(name);
    int result = NET_ERR_PARAM;
    if (ifp != NULL && !(ifp->flags & NETIF_LOOPBACK)) {
        dhcp_stop(ifp);
        arp_flush_if(ifp);
        ifp->ip      = ip;
        ifp->netmask = ip != 0 ? netmask : 0;
        ifp->gateway = ip != 0 ? gateway : 0;
        result = NET_OK;
    }
    net_unlock();
    return result;
}

int netif_dhcp(const char* name) {
    net_lock();
    struct NetIf* ifp = netif_find_locked(name);
    int result = NET_ERR_PARAM;
    if (ifp != NULL && !(ifp->flags & NETIF_LOOPBACK)) {
        dhcp_stop(ifp);
        ifp->ip = ifp->netmask = ifp->gateway = 0;
        if (netif_usable(ifp))
            dhcp_start(ifp);
        else
            ifp->flags |= NETIF_DHCP;   // Starts when the link comes up
        result = NET_OK;
    }
    net_unlock();
    return result;
}

int netif_set_up(const char* name, bool up) {
    net_lock();
    struct NetIf* ifp = netif_find_locked(name);
    if (ifp != NULL) {
        if (up)
            ifp->flags |= NETIF_UP;
        else
            ifp->flags &= (uint16_t)~NETIF_UP;
    }
    net_unlock();
    return ifp != NULL ? NET_OK : NET_ERR_PARAM;
}

//
// Loopback: IP packets go straight back into the receive queue
//

static int loopback_transmit(struct NetIf* ifp, const void* frame, uint32_t length) {
    netif_rx(ifp, frame, length);
    return NET_OK;
}

static const struct NetIfOps loopback_ops = {
    .transmit = loopback_transmit,
};

//
// The "net" thread
//

// One pass: frames first, then timers. Returns true if a frame was handled.
bool net_service(void) {
    bool worked = false;
    net_lock();
    for (int n = 0; n < NET_RX_BATCH; n++) {
        mutex_lock(&rxq_mutex);
        struct RxEntry* e = rxq_head;
        if (e != NULL) {
            rxq_head = e->next;
            if (rxq_head == NULL)
                rxq_tail = NULL;
            rxq_count--;
        }
        mutex_unlock(&rxq_mutex);
        if (e == NULL)
            break;

        // Arrival times feed the fallback random generator
        net_entropy(((uint64_t)e->length << 32) ^ net_now_ms());

        struct NetIf* ifp = e->ifp;
        if (ifp->flags & NETIF_UP) {
            ifp->stats.rx_packets++;
            ifp->stats.rx_bytes += e->length;
            if (ifp->flags & NETIF_LOOPBACK)
                ip_input(ifp, e->data, e->length);
            else
                eth_input(ifp, e->data, e->length);
        } else {
            ifp->stats.rx_dropped++;
        }
        free(e);
        worked = true;
    }

    arp_timer();
    dhcp_timer();
    tcp_timer();
    net_unlock();
    return worked;
}

static void net_thread(void* arg) {
    (void)arg;
    while (1) {
        if (!net_service())
            thread_sleep(NET_THREAD_MS);
    }
}

void net_init(void) {
    if (net_ready)
        return;
    mutex_init(&net_mutex);
    mutex_init(&rxq_mutex);
    net_ready = true;

    static const uint8_t no_mac[NET_MAC_LEN] = { 0 };
    loopback = netif_register("lo", &loopback_ops, NULL, no_mac, NET_MTU_DEFAULT);
    if (loopback != NULL) {
        net_lock();
        loopback->flags  |= NETIF_LOOPBACK | NETIF_LINK;
        loopback->ip      = IP_LOOPBACK;
        loopback->netmask = 0xFF000000u;
        net_unlock();
    }

    kthread_create(net_thread, NULL, PRIORITY_NORMAL, "net");
}
